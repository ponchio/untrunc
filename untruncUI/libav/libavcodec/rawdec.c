/*
 * Raw Video Decoder
 * Copyright (c) 2001 Fabrice Bellard
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Raw Video Decoder
 */

#include "avcodec.h"
#include "internal.h"
#include "raw.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/imgutils.h"

typedef struct RawVideoContext {
    AVBufferRef *palette;
    int frame_size;  /* size of the frame in bytes */
    int flip;
    int is_2_4_bpp; // 2 or 4 bpp raw in avi/mov
    int is_yuv2;
} RawVideoContext;

static const PixelFormatTag pix_fmt_bps_avi[] = {
    { AV_PIX_FMT_PAL8,    4 },
    { AV_PIX_FMT_PAL8,    8 },
    { AV_PIX_FMT_RGB444, 12 },
    { AV_PIX_FMT_RGB555, 15 },
    { AV_PIX_FMT_RGB555, 16 },
    { AV_PIX_FMT_BGR24,  24 },
    { AV_PIX_FMT_RGB32,  32 },
    { AV_PIX_FMT_NONE,    0 },
};

static const PixelFormatTag pix_fmt_bps_mov[] = {
    { AV_PIX_FMT_MONOWHITE, 1 },
    { AV_PIX_FMT_PAL8,      2 },
    { AV_PIX_FMT_PAL8,      4 },
    { AV_PIX_FMT_PAL8,      8 },
    // FIXME swscale does not support 16 bit in .mov, sample 16bit.mov
    // http://developer.apple.com/documentation/QuickTime/QTFF/QTFFChap3/qtff3.html
    { AV_PIX_FMT_RGB555BE, 16 },
    { AV_PIX_FMT_RGB24,    24 },
    { AV_PIX_FMT_ARGB,     32 },
    { AV_PIX_FMT_MONOWHITE,33 },
    { AV_PIX_FMT_NONE,      0 },
};

static enum AVPixelFormat find_pix_fmt(const PixelFormatTag *tags,
                                       unsigned int fourcc)
{
    while (tags->pix_fmt >= 0) {
        if (tags->fourcc == fourcc)
            return tags->pix_fmt;
        tags++;
    }
    return AV_PIX_FMT_YUV420P;
}

static av_cold int raw_init_decoder(AVCodecContext *avctx)
{
    RawVideoContext *context = avctx->priv_data;
    const AVPixFmtDescriptor *desc;

    if (avctx->codec_tag == MKTAG('r', 'a', 'w', ' '))
        avctx->pix_fmt = find_pix_fmt(pix_fmt_bps_mov,
                                      avctx->bits_per_coded_sample);
    else if (avctx->codec_tag == MKTAG('W', 'R', 'A', 'W'))
        avctx->pix_fmt = find_pix_fmt(pix_fmt_bps_avi,
                                      avctx->bits_per_coded_sample);
    else if (avctx->codec_tag)
        avctx->pix_fmt = find_pix_fmt(ff_raw_pix_fmt_tags, avctx->codec_tag);
    else if (avctx->pix_fmt == AV_PIX_FMT_NONE && avctx->bits_per_coded_sample)
        avctx->pix_fmt = find_pix_fmt(pix_fmt_bps_avi,
                                      avctx->bits_per_coded_sample);

    desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    if (!desc) {
        av_log(avctx, AV_LOG_ERROR, "Invalid pixel format.\n");
        return AVERROR(EINVAL);
    }

    if (desc->flags & (AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_PSEUDOPAL)) {
        context->palette = av_buffer_alloc(AVPALETTE_SIZE);
        if (!context->palette)
            return AVERROR(ENOMEM);
        if (desc->flags & AV_PIX_FMT_FLAG_PSEUDOPAL)
            avpriv_set_systematic_pal2((uint32_t*)context->palette->data, avctx->pix_fmt);
        else
            memset(context->palette->data, 0, AVPALETTE_SIZE);
    }

    context->frame_size = av_image_get_buffer_size(avctx->pix_fmt,
                                                   avctx->width,
                                                   avctx->height, 1);

    if ((avctx->bits_per_coded_sample == 4 || avctx->bits_per_coded_sample == 2) &&
        avctx->pix_fmt == AV_PIX_FMT_PAL8 &&
       (!avctx->codec_tag || avctx->codec_tag == MKTAG('r','a','w',' ')))
        context->is_2_4_bpp = 1;

    if ((avctx->extradata_size >= 9 &&
         !memcmp(avctx->extradata + avctx->extradata_size - 9, "BottomUp", 9)) ||
        avctx->codec_tag == MKTAG(3, 0, 0, 0) ||
        avctx->codec_tag == MKTAG('W','R','A','W'))
        context->flip = 1;

    if (avctx->codec_tag == AV_RL32("yuv2") &&
        avctx->pix_fmt   == AV_PIX_FMT_YUYV422)
        context->is_yuv2 = 1;

    return 0;
}

static void flip(AVCodecContext *avctx, AVFrame *frame)
{
    frame->data[0]     += frame->linesize[0] * (avctx->height - 1);
    frame->linesize[0] *= -1;
}

static int raw_decode(AVCodecContext *avctx, void *data, int *got_frame,
                      AVPacket *avpkt)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    RawVideoContext *context       = avctx->priv_data;
    const uint8_t *buf             = avpkt->data;
    int buf_size                   = avpkt->size;
    int need_copy                  = !avpkt->buf || context->is_2_4_bpp || context->is_yuv2;
    int res;

    AVFrame   *frame   = data;

    frame->pict_type        = AV_PICTURE_TYPE_I;
    frame->key_frame        = 1;

    res = ff_decode_frame_props(avctx, frame);
    if (res < 0)
        return res;

    if (buf_size < context->frame_size - (avctx->pix_fmt == AV_PIX_FMT_PAL8 ?
                                          AVPALETTE_SIZE : 0))
        return -1;

    if (need_copy)
        frame->buf[0] = av_buffer_alloc(context->frame_size);
    else
        frame->buf[0] = av_buffer_ref(avpkt->buf);
    if (!frame->buf[0])
        return AVERROR(ENOMEM);

    //2bpp and 4bpp raw in avi and mov (yes this is ugly ...)
    if (context->is_2_4_bpp) {
        int i;
        uint8_t *dst = frame->buf[0]->data;
        buf_size = context->frame_size - AVPALETTE_SIZE;
        if (avctx->bits_per_coded_sample == 4) {
            for (i = 0; 2 * i + 1 < buf_size; i++) {
                dst[2 * i + 0] = buf[i] >> 4;
                dst[2 * i + 1] = buf[i] & 15;
            }
        } else {
            for (i = 0; 4 * i + 3 < buf_size; i++) {
                dst[4 * i + 0] = buf[i] >> 6;
                dst[4 * i + 1] = buf[i] >> 4 & 3;
                dst[4 * i + 2] = buf[i] >> 2 & 3;
                dst[4 * i + 3] = buf[i]      & 3;
            }
        }
        buf = dst;
    } else if (need_copy) {
        memcpy(frame->buf[0]->data, buf, FFMIN(buf_size, context->frame_size));
        buf = frame->buf[0]->data;
    }

    if (avctx->codec_tag == MKTAG('A', 'V', '1', 'x') ||
        avctx->codec_tag == MKTAG('A', 'V', 'u', 'p'))
        buf += buf_size - context->frame_size;

    if ((res = av_image_fill_arrays(frame->data, frame->linesize,
                                    buf, avctx->pix_fmt,
                                    avctx->width, avctx->height, 1)) < 0)
        return res;

    if (avctx->pix_fmt == AV_PIX_FMT_PAL8) {
        const uint8_t *pal = av_packet_get_side_data(avpkt, AV_PKT_DATA_PALETTE,
                                                     NULL);

        if (pal) {
            av_buffer_unref(&context->palette);
            context->palette = av_buffer_alloc(AVPALETTE_SIZE);
            if (!context->palette)
                return AVERROR(ENOMEM);
            memcpy(context->palette->data, pal, AVPALETTE_SIZE);
            frame->palette_has_changed = 1;
        }
    }

    if ((avctx->pix_fmt == AV_PIX_FMT_PAL8 && buf_size < context->frame_size) ||
        (desc->flags & AV_PIX_FMT_FLAG_PSEUDOPAL)) {
        frame->buf[1]  = av_buffer_ref(context->palette);
        if (!frame->buf[1])
            return AVERROR(ENOMEM);
        frame->data[1] = frame->buf[1]->data;
    }
    if (avctx->pix_fmt == AV_PIX_FMT_BGR24 &&
        ((frame->linesize[0] + 3) & ~3) * avctx->height <= buf_size)
        frame->linesize[0] = (frame->linesize[0] + 3) & ~3;

    if (context->flip)
        flip(avctx, frame);

    if (avctx->codec_tag == MKTAG('Y', 'V', '1', '2') ||
        avctx->codec_tag == MKTAG('Y', 'V', '1', '6') ||
        avctx->codec_tag == MKTAG('Y', 'V', '2', '4') ||
        avctx->codec_tag == MKTAG('Y', 'V', 'U', '9'))
        FFSWAP(uint8_t *, frame->data[1], frame->data[2]);

    if (avctx->codec_tag == AV_RL32("yuv2") &&
        avctx->pix_fmt   == AV_PIX_FMT_YUYV422) {
        int x, y;
        uint8_t *line = frame->data[0];
        for (y = 0; y < avctx->height; y++) {
            for (x = 0; x < avctx->width; x++)
                line[2 * x + 1] ^= 0x80;
            line += frame->linesize[0];
        }
    }

    *got_frame = 1;
    return buf_size;
}

static av_cold int raw_close_decoder(AVCodecContext *avctx)
{
    RawVideoContext *context = avctx->priv_data;

    av_buffer_unref(&context->palette);
    return 0;
}

AVCodec ff_rawvideo_decoder = {
    .name           = "rawvideo",
    .long_name      = NULL_IF_CONFIG_SMALL("raw video"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_RAWVIDEO,
    .priv_data_size = sizeof(RawVideoContext),
    .init           = raw_init_decoder,
    .close          = raw_close_decoder,
    .decode         = raw_decode,
};

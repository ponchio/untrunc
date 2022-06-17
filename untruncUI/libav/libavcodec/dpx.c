/*
 * DPX (.dpx) image decoder
 * Copyright (c) 2009 Jimmy Christensen
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

#include "libavutil/intreadwrite.h"
#include "libavutil/imgutils.h"
#include "bytestream.h"
#include "avcodec.h"
#include "internal.h"

static unsigned int read32(const uint8_t **ptr, int is_big)
{
    unsigned int temp;
    if (is_big) {
        temp = AV_RB32(*ptr);
    } else {
        temp = AV_RL32(*ptr);
    }
    *ptr += 4;
    return temp;
}

static inline unsigned make_16bit(unsigned value)
{
    // mask away invalid bits
    value &= 0xFFC0;
    // correctly expand to 16 bits
    return value + (value >> 10);
}

static int decode_frame(AVCodecContext *avctx,
                        void *data,
                        int *got_frame,
                        AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    const uint8_t *buf_end = avpkt->data + avpkt->size;
    int buf_size       = avpkt->size;
    AVFrame *const p = data;
    uint8_t *ptr;

    unsigned int offset;
    int magic_num, endian;
    int x, y, ret;
    int w, h, stride, bits_per_color, descriptor, elements, target_packet_size, source_packet_size;

    unsigned int rgbBuffer;

    if (avpkt->size <= 1634) {
        av_log(avctx, AV_LOG_ERROR, "Packet too small for DPX header\n");
        return AVERROR_INVALIDDATA;
    }

    magic_num = AV_RB32(buf);
    buf += 4;

    /* Check if the files "magic number" is "SDPX" which means it uses
     * big-endian or XPDS which is for little-endian files */
    if (magic_num == AV_RL32("SDPX")) {
        endian = 0;
    } else if (magic_num == AV_RB32("SDPX")) {
        endian = 1;
    } else {
        av_log(avctx, AV_LOG_ERROR, "DPX marker not found\n");
        return AVERROR_INVALIDDATA;
    }

    offset = read32(&buf, endian);
    if (avpkt->size <= offset) {
        av_log(avctx, AV_LOG_ERROR, "Invalid data start offset\n");
        return AVERROR_INVALIDDATA;
    }
    // Need to end in 0x304 offset from start of file
    buf = avpkt->data + 0x304;
    w = read32(&buf, endian);
    h = read32(&buf, endian);

    // Need to end in 0x320 to read the descriptor
    buf += 20;
    descriptor = buf[0];

    // Need to end in 0x323 to read the bits per color
    buf += 3;
    avctx->bits_per_raw_sample =
    bits_per_color = buf[0];

    buf += 825;
    avctx->sample_aspect_ratio.num = read32(&buf, endian);
    avctx->sample_aspect_ratio.den = read32(&buf, endian);

    switch (descriptor) {
        case 51: // RGBA
            elements = 4;
            break;
        case 50: // RGB
            elements = 3;
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "Unsupported descriptor %d\n", descriptor);
            return AVERROR_INVALIDDATA;
    }

    switch (bits_per_color) {
        case 8:
            if (elements == 4) {
                avctx->pix_fmt = AV_PIX_FMT_RGBA;
            } else {
                avctx->pix_fmt = AV_PIX_FMT_RGB24;
            }
            source_packet_size = elements;
            target_packet_size = elements;
            break;
        case 10:
            avctx->pix_fmt = AV_PIX_FMT_RGB48;
            target_packet_size = 6;
            source_packet_size = 4;
            break;
        case 12:
        case 16:
            if (endian) {
                avctx->pix_fmt = AV_PIX_FMT_RGB48BE;
            } else {
                avctx->pix_fmt = AV_PIX_FMT_RGB48LE;
            }
            target_packet_size = 6;
            source_packet_size = elements * 2;
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "Unsupported color depth : %d\n", bits_per_color);
            return AVERROR_INVALIDDATA;
    }

    if ((ret = ff_set_dimensions(avctx, w, h)) < 0)
        return ret;

    ff_set_sar(avctx, avctx->sample_aspect_ratio);

    if ((ret = ff_get_buffer(avctx, p, 0)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
        return ret;
    }

    // Move pointer to offset from start of file
    buf =  avpkt->data + offset;

    ptr    = p->data[0];
    stride = p->linesize[0];

    if (source_packet_size*avctx->width*avctx->height > buf_end - buf) {
        av_log(avctx, AV_LOG_ERROR, "Overread buffer. Invalid header?\n");
        return AVERROR_INVALIDDATA;
    }
    switch (bits_per_color) {
        case 10:
            for (x = 0; x < avctx->height; x++) {
               uint16_t *dst = (uint16_t*)ptr;
               for (y = 0; y < avctx->width; y++) {
                   rgbBuffer = read32(&buf, endian);
                   // Read out the 10-bit colors and convert to 16-bit
                   *dst++ = make_16bit(rgbBuffer >> 16);
                   *dst++ = make_16bit(rgbBuffer >>  6);
                   *dst++ = make_16bit(rgbBuffer <<  4);
               }
               ptr += stride;
            }
            break;
        case 8:
        case 12: // Treat 12-bit as 16-bit
        case 16:
            if (source_packet_size == target_packet_size) {
                for (x = 0; x < avctx->height; x++) {
                    memcpy(ptr, buf, target_packet_size*avctx->width);
                    ptr += stride;
                    buf += source_packet_size*avctx->width;
                }
            } else {
                for (x = 0; x < avctx->height; x++) {
                    uint8_t *dst = ptr;
                    for (y = 0; y < avctx->width; y++) {
                        memcpy(dst, buf, target_packet_size);
                        dst += target_packet_size;
                        buf += source_packet_size;
                    }
                    ptr += stride;
                }
            }
            break;
    }

    *got_frame = 1;

    return buf_size;
}

AVCodec ff_dpx_decoder = {
    .name           = "dpx",
    .long_name      = NULL_IF_CONFIG_SMALL("DPX image"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_DPX,
    .decode         = decode_frame,
    .capabilities   = AV_CODEC_CAP_DR1,
};

/*
 * VC3/DNxHD decoder.
 * Copyright (c) 2007 SmartJog S.A., Baptiste Coudurier <baptiste dot coudurier at smartjog dot com>
 * Copyright (c) 2011 MirriAd Ltd
 *
 * 10 bit support added by MirriAd Ltd, Joseph Artsimovich <joseph@mirriad.com>
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

#include "libavutil/imgutils.h"
#include "libavutil/timer.h"
#include "avcodec.h"
#include "blockdsp.h"
#include "get_bits.h"
#include "dnxhddata.h"
#include "idctdsp.h"
#include "internal.h"
#include "thread.h"

typedef struct DNXHDContext {
    AVCodecContext *avctx;
    GetBitContext gb;
    BlockDSPContext bdsp;
    int cid;                            ///< compression id
    unsigned int width, height;
    unsigned int mb_width, mb_height;
    uint32_t mb_scan_index[68];         /* max for 1080p */
    int cur_field;                      ///< current interlaced field
    VLC ac_vlc, dc_vlc, run_vlc;
    int last_dc[3];
    IDCTDSPContext idsp;
    DECLARE_ALIGNED(16, int16_t, blocks)[12][64];
    ScanTable scantable;
    const CIDEntry *cid_table;
    int bit_depth; // 8, 10 or 0 if not initialized at all.
    int is_444;
    int mbaff;
    void (*decode_dct_block)(struct DNXHDContext *ctx, int16_t *block,
                             int n, int qscale);
} DNXHDContext;

#define DNXHD_VLC_BITS 9
#define DNXHD_DC_VLC_BITS 7

static void dnxhd_decode_dct_block_8(DNXHDContext *ctx, int16_t *block,
                                     int n, int qscale);
static void dnxhd_decode_dct_block_10(DNXHDContext *ctx, int16_t *block,
                                      int n, int qscale);
static void dnxhd_decode_dct_block_10_444(DNXHDContext *ctx, int16_t *block,
                                          int n, int qscale);

static av_cold int dnxhd_decode_init(AVCodecContext *avctx)
{
    DNXHDContext *ctx = avctx->priv_data;

    ctx->avctx = avctx;
    return 0;
}

static int dnxhd_init_vlc(DNXHDContext *ctx, int cid)
{
    if (cid != ctx->cid) {
        int index;

        if ((index = ff_dnxhd_get_cid_table(cid)) < 0) {
            av_log(ctx->avctx, AV_LOG_ERROR, "unsupported cid %d\n", cid);
            return AVERROR(ENOSYS);
        }
        ctx->cid_table = &ff_dnxhd_cid_table[index];
        av_log(ctx->avctx, AV_LOG_VERBOSE, "Profile cid %d.\n", cid);

        ff_free_vlc(&ctx->ac_vlc);
        ff_free_vlc(&ctx->dc_vlc);
        ff_free_vlc(&ctx->run_vlc);

        init_vlc(&ctx->ac_vlc, DNXHD_VLC_BITS, 257,
                 ctx->cid_table->ac_bits, 1, 1,
                 ctx->cid_table->ac_codes, 2, 2, 0);
        init_vlc(&ctx->dc_vlc, DNXHD_DC_VLC_BITS, ctx->bit_depth + 4,
                 ctx->cid_table->dc_bits, 1, 1,
                 ctx->cid_table->dc_codes, 1, 1, 0);
        init_vlc(&ctx->run_vlc, DNXHD_VLC_BITS, 62,
                 ctx->cid_table->run_bits, 1, 1,
                 ctx->cid_table->run_codes, 2, 2, 0);

        ff_init_scantable(ctx->idsp.idct_permutation, &ctx->scantable,
                          ff_zigzag_direct);
        ctx->cid = cid;
    }
    return 0;
}

static int dnxhd_decode_header(DNXHDContext *ctx, AVFrame *frame,
                               const uint8_t *buf, int buf_size,
                               int first_field)
{
    static const uint8_t header_prefix[]    = { 0x00, 0x00, 0x02, 0x80, 0x01 };
    static const uint8_t header_prefix444[] = { 0x00, 0x00, 0x02, 0x80, 0x02 };
    int i, cid, ret;
    int old_bit_depth = ctx->bit_depth;

    if (buf_size < 0x280) {
        av_log(ctx->avctx, AV_LOG_ERROR, "buffer too small (%d < 640).\n",
               buf_size);
        return AVERROR_INVALIDDATA;
    }

    if (memcmp(buf, header_prefix, 5) && memcmp(buf, header_prefix444, 5)) {
        av_log(ctx->avctx, AV_LOG_ERROR,
               "unknown header 0x%02"PRIX8" 0x%02"PRIX8" 0x%02"PRIX8" 0x%02"PRIX8" 0x%02"PRIX8"\n",
               buf[0], buf[1], buf[2], buf[3], buf[4]);
        return AVERROR_INVALIDDATA;
    }
    if (buf[5] & 2) { /* interlaced */
        ctx->cur_field = buf[5] & 1;
        frame->interlaced_frame = 1;
        frame->top_field_first  = first_field ^ ctx->cur_field;
        av_log(ctx->avctx, AV_LOG_DEBUG,
               "interlaced %"PRId8", cur field %d\n", buf[5] & 3, ctx->cur_field);
    }
    ctx->mbaff = buf[0x6] & 32;

    ctx->height = AV_RB16(buf + 0x18);
    ctx->width  = AV_RB16(buf + 0x1a);

    ff_dlog(ctx->avctx, "width %u, height %u\n", ctx->width, ctx->height);

    if (buf[0x21] == 0x58) { /* 10 bit */
        ctx->bit_depth = ctx->avctx->bits_per_raw_sample = 10;

        if (buf[0x4] == 0x2) {
            ctx->decode_dct_block = dnxhd_decode_dct_block_10_444;
            ctx->avctx->pix_fmt = AV_PIX_FMT_YUV444P10;
            ctx->is_444 = 1;
        } else {
            ctx->decode_dct_block = dnxhd_decode_dct_block_10;
            ctx->avctx->pix_fmt = AV_PIX_FMT_YUV422P10;
        }
    } else if (buf[0x21] == 0x38) { /* 8 bit */
        ctx->bit_depth = ctx->avctx->bits_per_raw_sample = 8;

        ctx->avctx->pix_fmt = AV_PIX_FMT_YUV422P;
        ctx->decode_dct_block = dnxhd_decode_dct_block_8;
    } else {
        av_log(ctx->avctx, AV_LOG_ERROR, "invalid bit depth value (%"PRId8").\n",
               buf[0x21]);
        return AVERROR_INVALIDDATA;
    }
    if (ctx->bit_depth != old_bit_depth) {
        ff_blockdsp_init(&ctx->bdsp);
        ff_idctdsp_init(&ctx->idsp, ctx->avctx);
    }

    cid = AV_RB32(buf + 0x28);
    ff_dlog(ctx->avctx, "compression id %d\n", cid);

    if ((ret = dnxhd_init_vlc(ctx, cid)) < 0)
        return ret;
    if (ctx->mbaff && ctx->cid_table->cid != 1260)
        av_log(ctx->avctx, AV_LOG_WARNING,
               "Adaptive MB interlace flag in an unsupported profile.\n");

    // make sure profile size constraints are respected
    // DNx100 allows 1920->1440 and 1280->960 subsampling
    if (ctx->width != ctx->cid_table->width) {
        av_reduce(&ctx->avctx->sample_aspect_ratio.num,
                  &ctx->avctx->sample_aspect_ratio.den,
                  ctx->width, ctx->cid_table->width, 255);
        ctx->width = ctx->cid_table->width;
    }

    if (buf_size < ctx->cid_table->coding_unit_size) {
        av_log(ctx->avctx, AV_LOG_ERROR, "incorrect frame size (%d < %u).\n",
               buf_size, ctx->cid_table->coding_unit_size);
        return AVERROR_INVALIDDATA;
    }

    ctx->mb_width  = ctx->width >> 4;
    ctx->mb_height = buf[0x16d];

    ff_dlog(ctx->avctx,
            "mb width %u, mb height %u\n", ctx->mb_width, ctx->mb_height);

    if ((ctx->height + 15) >> 4 == ctx->mb_height && frame->interlaced_frame)
        ctx->height <<= 1;

    if (ctx->mb_height > 68 ||
        (ctx->mb_height << frame->interlaced_frame) > (ctx->height + 15) >> 4) {
        av_log(ctx->avctx, AV_LOG_ERROR,
               "mb height too big: %u\n", ctx->mb_height);
        return AVERROR_INVALIDDATA;
    }

    for (i = 0; i < ctx->mb_height; i++) {
        ctx->mb_scan_index[i] = AV_RB32(buf + 0x170 + (i << 2));
        ff_dlog(ctx->avctx, "mb scan index %"PRIu32"\n", ctx->mb_scan_index[i]);
        if (buf_size < ctx->mb_scan_index[i] + 0x280) {
            av_log(ctx->avctx, AV_LOG_ERROR,
                   "invalid mb scan index (%d < %"PRIu32").\n",
                   buf_size, ctx->mb_scan_index[i] + 0x280);
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
}

static av_always_inline void dnxhd_decode_dct_block(DNXHDContext *ctx,
                                                    int16_t *block, int n,
                                                    int qscale,
                                                    int index_bits,
                                                    int level_bias,
                                                    int level_shift)
{
    int i, j, index1, index2, len;
    int level, component, sign;
    const uint8_t *weight_matrix;
    OPEN_READER(bs, &ctx->gb);

    if (!ctx->is_444) {
        if (n & 2) {
            component     = 1 + (n & 1);
            weight_matrix = ctx->cid_table->chroma_weight;
        } else {
            component     = 0;
            weight_matrix = ctx->cid_table->luma_weight;
        }
    } else {
        component = (n >> 1) % 3;
        if (component) {
            weight_matrix = ctx->cid_table->chroma_weight;
        } else {
            weight_matrix = ctx->cid_table->luma_weight;
        }
    }

    UPDATE_CACHE(bs, &ctx->gb);
    GET_VLC(len, bs, &ctx->gb, ctx->dc_vlc.table, DNXHD_DC_VLC_BITS, 1);
    if (len) {
        level = GET_CACHE(bs, &ctx->gb);
        LAST_SKIP_BITS(bs, &ctx->gb, len);
        sign  = ~level >> 31;
        level = (NEG_USR32(sign ^ level, len) ^ sign) - sign;
        ctx->last_dc[component] += level;
    }
    block[0] = ctx->last_dc[component];

    for (i = 1; ; i++) {
        UPDATE_CACHE(bs, &ctx->gb);
        GET_VLC(index1, bs, &ctx->gb, ctx->ac_vlc.table,
                DNXHD_VLC_BITS, 2);
        level = ctx->cid_table->ac_level[index1];
        if (!level) /* EOB */
            break;

        sign = SHOW_SBITS(bs, &ctx->gb, 1);
        SKIP_BITS(bs, &ctx->gb, 1);

        if (ctx->cid_table->ac_index_flag[index1]) {
            level += SHOW_UBITS(bs, &ctx->gb, index_bits) << 6;
            SKIP_BITS(bs, &ctx->gb, index_bits);
        }

        if (ctx->cid_table->ac_run_flag[index1]) {
            UPDATE_CACHE(bs, &ctx->gb);
            GET_VLC(index2, bs, &ctx->gb, ctx->run_vlc.table,
                    DNXHD_VLC_BITS, 2);
            i += ctx->cid_table->run[index2];
        }

        if (i > 63) {
            av_log(ctx->avctx, AV_LOG_ERROR, "ac tex damaged %d, %d\n", n, i);
            break;
        }

        j     = ctx->scantable.permutated[i];
        level = (2 * level + 1) * qscale * weight_matrix[i];
        if (level_bias < 32 || weight_matrix[i] != level_bias)
            level += level_bias;
        level >>= level_shift;

        block[j] = (level ^ sign) - sign;
    }

    CLOSE_READER(bs, &ctx->gb);
}

static void dnxhd_decode_dct_block_8(DNXHDContext *ctx, int16_t *block,
                                     int n, int qscale)
{
    dnxhd_decode_dct_block(ctx, block, n, qscale, 4, 32, 6);
}

static void dnxhd_decode_dct_block_10(DNXHDContext *ctx, int16_t *block,
                                      int n, int qscale)
{
    dnxhd_decode_dct_block(ctx, block, n, qscale, 6, 8, 4);
}

static void dnxhd_decode_dct_block_10_444(DNXHDContext *ctx, int16_t *block,
                                          int n, int qscale)
{
    dnxhd_decode_dct_block(ctx, block, n, qscale, 6, 32, 6);
}

static int dnxhd_decode_macroblock(DNXHDContext *ctx, AVFrame *frame,
                                   int x, int y)
{
    int shift1 = ctx->bit_depth == 10;
    int dct_linesize_luma   = frame->linesize[0];
    int dct_linesize_chroma = frame->linesize[1];
    uint8_t *dest_y, *dest_u, *dest_v;
    int dct_y_offset, dct_x_offset;
    int qscale, i;
    int interlaced_mb = 0;

    if (ctx->mbaff) {
        interlaced_mb = get_bits1(&ctx->gb);
        qscale = get_bits(&ctx->gb, 10);
    } else {
        qscale = get_bits(&ctx->gb, 11);
    }
    skip_bits1(&ctx->gb);

    for (i = 0; i < 8; i++) {
        ctx->bdsp.clear_block(ctx->blocks[i]);
        ctx->decode_dct_block(ctx, ctx->blocks[i], i, qscale);
    }
    if (ctx->is_444) {
        for (; i < 12; i++) {
            ctx->bdsp.clear_block(ctx->blocks[i]);
            ctx->decode_dct_block(ctx, ctx->blocks[i], i, qscale);
        }
    }

    if (frame->interlaced_frame) {
        dct_linesize_luma   <<= 1;
        dct_linesize_chroma <<= 1;
    }

    dest_y = frame->data[0] + ((y * dct_linesize_luma)   << 4) + (x << (4 + shift1));
    dest_u = frame->data[1] + ((y * dct_linesize_chroma) << 4) + (x << (3 + shift1 + ctx->is_444));
    dest_v = frame->data[2] + ((y * dct_linesize_chroma) << 4) + (x << (3 + shift1 + ctx->is_444));

    if (ctx->cur_field) {
        dest_y += frame->linesize[0];
        dest_u += frame->linesize[1];
        dest_v += frame->linesize[2];
    }
    if (interlaced_mb) {
        dct_linesize_luma   <<= 1;
        dct_linesize_chroma <<= 1;
    }

    dct_y_offset = interlaced_mb ? frame->linesize[0] : (dct_linesize_luma << 3);
    dct_x_offset = 8 << shift1;
    if (!ctx->is_444) {
        ctx->idsp.idct_put(dest_y,                               dct_linesize_luma, ctx->blocks[0]);
        ctx->idsp.idct_put(dest_y + dct_x_offset,                dct_linesize_luma, ctx->blocks[1]);
        ctx->idsp.idct_put(dest_y + dct_y_offset,                dct_linesize_luma, ctx->blocks[4]);
        ctx->idsp.idct_put(dest_y + dct_y_offset + dct_x_offset, dct_linesize_luma, ctx->blocks[5]);

        if (!(ctx->avctx->flags & AV_CODEC_FLAG_GRAY)) {
            dct_y_offset = interlaced_mb ? frame->linesize[1] : (dct_linesize_chroma << 3);
            ctx->idsp.idct_put(dest_u,                dct_linesize_chroma, ctx->blocks[2]);
            ctx->idsp.idct_put(dest_v,                dct_linesize_chroma, ctx->blocks[3]);
            ctx->idsp.idct_put(dest_u + dct_y_offset, dct_linesize_chroma, ctx->blocks[6]);
            ctx->idsp.idct_put(dest_v + dct_y_offset, dct_linesize_chroma, ctx->blocks[7]);
        }
    } else {
        ctx->idsp.idct_put(dest_y,                               dct_linesize_luma, ctx->blocks[0]);
        ctx->idsp.idct_put(dest_y + dct_x_offset,                dct_linesize_luma, ctx->blocks[1]);
        ctx->idsp.idct_put(dest_y + dct_y_offset,                dct_linesize_luma, ctx->blocks[6]);
        ctx->idsp.idct_put(dest_y + dct_y_offset + dct_x_offset, dct_linesize_luma, ctx->blocks[7]);

        if (!(ctx->avctx->flags & AV_CODEC_FLAG_GRAY)) {
            dct_y_offset = interlaced_mb ? frame->linesize[1] : (dct_linesize_chroma << 3);
            ctx->idsp.idct_put(dest_u,                               dct_linesize_chroma, ctx->blocks[2]);
            ctx->idsp.idct_put(dest_u + dct_x_offset,                dct_linesize_chroma, ctx->blocks[3]);
            ctx->idsp.idct_put(dest_u + dct_y_offset,                dct_linesize_chroma, ctx->blocks[8]);
            ctx->idsp.idct_put(dest_u + dct_y_offset + dct_x_offset, dct_linesize_chroma, ctx->blocks[9]);
            ctx->idsp.idct_put(dest_v,                               dct_linesize_chroma, ctx->blocks[4]);
            ctx->idsp.idct_put(dest_v + dct_x_offset,                dct_linesize_chroma, ctx->blocks[5]);
            ctx->idsp.idct_put(dest_v + dct_y_offset,                dct_linesize_chroma, ctx->blocks[10]);
            ctx->idsp.idct_put(dest_v + dct_y_offset + dct_x_offset, dct_linesize_chroma, ctx->blocks[11]);
        }
    }

    return 0;
}

static int dnxhd_decode_macroblocks(DNXHDContext *ctx, AVFrame *frame,
                                    const uint8_t *buf, int buf_size)
{
    int x, y;
    for (y = 0; y < ctx->mb_height; y++) {
        ctx->last_dc[0] =
        ctx->last_dc[1] =
        ctx->last_dc[2] = 1 << (ctx->bit_depth + 2); // for levels +2^(bitdepth-1)
        init_get_bits(&ctx->gb, buf + ctx->mb_scan_index[y], (buf_size - ctx->mb_scan_index[y]) << 3);
        for (x = 0; x < ctx->mb_width; x++) {
            //START_TIMER;
            dnxhd_decode_macroblock(ctx, frame, x, y);
            //STOP_TIMER("decode macroblock");
        }
    }
    return 0;
}

static int dnxhd_decode_frame(AVCodecContext *avctx, void *data,
                              int *got_frame, AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    DNXHDContext *ctx = avctx->priv_data;
    ThreadFrame tf;
    int first_field = 1;
    int ret;

    tf.f = data;

    ff_dlog(avctx, "frame size %d\n", buf_size);

decode_coding_unit:
    if ((ret = dnxhd_decode_header(ctx, tf.f, buf, buf_size, first_field)) < 0)
        return ret;

    if ((avctx->width || avctx->height) &&
        (ctx->width != avctx->width || ctx->height != avctx->height)) {
        av_log(avctx, AV_LOG_WARNING, "frame size changed: %dx%d -> %ux%u\n",
               avctx->width, avctx->height, ctx->width, ctx->height);
        first_field = 1;
    }

    ret = ff_set_dimensions(avctx, ctx->width, ctx->height);
    if (ret < 0)
        return ret;

    if (first_field) {
        if ((ret = ff_thread_get_buffer(avctx, &tf, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
            return ret;
        }
        tf.f->pict_type = AV_PICTURE_TYPE_I;
        tf.f->key_frame = 1;
    }

    dnxhd_decode_macroblocks(ctx, tf.f, buf + 0x280, buf_size - 0x280);

    if (first_field && tf.f->interlaced_frame) {
        buf      += ctx->cid_table->coding_unit_size;
        buf_size -= ctx->cid_table->coding_unit_size;
        first_field = 0;
        goto decode_coding_unit;
    }

    *got_frame = 1;
    return buf_size;
}

static av_cold int dnxhd_decode_close(AVCodecContext *avctx)
{
    DNXHDContext *ctx = avctx->priv_data;

    ff_free_vlc(&ctx->ac_vlc);
    ff_free_vlc(&ctx->dc_vlc);
    ff_free_vlc(&ctx->run_vlc);
    return 0;
}

AVCodec ff_dnxhd_decoder = {
    .name           = "dnxhd",
    .long_name      = NULL_IF_CONFIG_SMALL("VC3/DNxHD"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_DNXHD,
    .priv_data_size = sizeof(DNXHDContext),
    .init           = dnxhd_decode_init,
    .close          = dnxhd_decode_close,
    .decode         = dnxhd_decode_frame,
    .capabilities   = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_FRAME_THREADS,
};

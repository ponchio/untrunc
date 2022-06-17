/*
 * FFV1 encoder for libavcodec
 *
 * Copyright (c) 2003-2012 Michael Niedermayer <michaelni@gmx.at>
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
 * FF Video Codec 1 (a lossless codec) encoder
 */

#include "libavutil/attributes.h"
#include "libavutil/avassert.h"
#include "libavutil/pixdesc.h"
#include "libavutil/crc.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"

#include "avcodec.h"
#include "golomb.h"
#include "internal.h"
#include "put_bits.h"
#include "rangecoder.h"
#include "mathops.h"
#include "ffv1.h"

static void find_best_state(uint8_t best_state[256][256],
                            const uint8_t one_state[256])
{
    int i, j, k, m;
    double l2tab[256];

    for (i = 1; i < 256; i++)
        l2tab[i] = log2(i / 256.0);

    for (i = 0; i < 256; i++) {
        double best_len[256];
        double p = i / 256.0;

        for (j = 0; j < 256; j++)
            best_len[j] = 1 << 30;

        for (j = FFMAX(i - 10, 1); j < FFMIN(i + 11, 256); j++) {
            double occ[256] = { 0 };
            double len      = 0;
            occ[j] = 1.0;
            for (k = 0; k < 256; k++) {
                double newocc[256] = { 0 };
                for (m = 1; m < 256; m++)
                    if (occ[m]) {
                        len -= occ[m] *     (p  * l2tab[m] +
                                        (1 - p) * l2tab[256 - m]);
                    }
                if (len < best_len[k]) {
                    best_len[k]      = len;
                    best_state[i][k] = j;
                }
                for (m = 1; m < 256; m++)
                    if (occ[m]) {
                        newocc[one_state[m]]             += occ[m] * p;
                        newocc[256 - one_state[256 - m]] += occ[m] * (1 - p);
                    }
                memcpy(occ, newocc, sizeof(occ));
            }
        }
    }
}

static av_always_inline av_flatten void put_symbol_inline(RangeCoder *c,
                                                          uint8_t *state, int v,
                                                          int is_signed,
                                                          uint64_t rc_stat[256][2],
                                                          uint64_t rc_stat2[32][2])
{
    int i;

#define put_rac(C, S, B)                        \
    do {                                        \
        if (rc_stat) {                          \
            rc_stat[*(S)][B]++;                 \
            rc_stat2[(S) - state][B]++;         \
        }                                       \
        put_rac(C, S, B);                       \
    } while (0)

    if (v) {
        const int a = FFABS(v);
        const int e = av_log2(a);
        put_rac(c, state + 0, 0);
        if (e <= 9) {
            for (i = 0; i < e; i++)
                put_rac(c, state + 1 + i, 1);  // 1..10
            put_rac(c, state + 1 + i, 0);

            for (i = e - 1; i >= 0; i--)
                put_rac(c, state + 22 + i, (a >> i) & 1);  // 22..31

            if (is_signed)
                put_rac(c, state + 11 + e, v < 0);  // 11..21
        } else {
            for (i = 0; i < e; i++)
                put_rac(c, state + 1 + FFMIN(i, 9), 1);  // 1..10
            put_rac(c, state + 1 + 9, 0);

            for (i = e - 1; i >= 0; i--)
                put_rac(c, state + 22 + FFMIN(i, 9), (a >> i) & 1);  // 22..31

            if (is_signed)
                put_rac(c, state + 11 + 10, v < 0);  // 11..21
        }
    } else {
        put_rac(c, state + 0, 1);
    }
#undef put_rac
}

static av_noinline void put_symbol(RangeCoder *c, uint8_t *state,
                                   int v, int is_signed)
{
    put_symbol_inline(c, state, v, is_signed, NULL, NULL);
}

static inline void put_vlc_symbol(PutBitContext *pb, VlcState *const state,
                                  int v, int bits)
{
    int i, k, code;
    v = fold(v - state->bias, bits);

    i = state->count;
    k = 0;
    while (i < state->error_sum) { // FIXME: optimize
        k++;
        i += i;
    }

    assert(k <= 13);

    code = v ^ ((2 * state->drift + state->count) >> 31);

    ff_dlog(NULL, "v:%d/%d bias:%d error:%d drift:%d count:%d k:%d\n", v, code,
            state->bias, state->error_sum, state->drift, state->count, k);
    set_sr_golomb(pb, code, k, 12, bits);

    update_vlc_state(state, v);
}

static av_always_inline int encode_line(FFV1Context *s, int w,
                                        int16_t *sample[3],
                                        int plane_index, int bits)
{
    PlaneContext *const p = &s->plane[plane_index];
    RangeCoder *const c   = &s->c;
    int x;
    int run_index = s->run_index;
    int run_count = 0;
    int run_mode  = 0;

    if (s->ac != AC_GOLOMB_RICE) {
        if (c->bytestream_end - c->bytestream < w * 20) {
            av_log(s->avctx, AV_LOG_ERROR, "encoded frame too large\n");
            return AVERROR_INVALIDDATA;
        }
    } else {
        if (s->pb.buf_end - s->pb.buf - (put_bits_count(&s->pb) >> 3) < w * 4) {
            av_log(s->avctx, AV_LOG_ERROR, "encoded frame too large\n");
            return AVERROR_INVALIDDATA;
        }
    }

    for (x = 0; x < w; x++) {
        int diff, context;

        context = get_context(p, sample[0] + x, sample[1] + x, sample[2] + x);
        diff    = sample[0][x] - predict(sample[0] + x, sample[1] + x);

        if (context < 0) {
            context = -context;
            diff    = -diff;
        }

        diff = fold(diff, bits);

        if (s->ac != AC_GOLOMB_RICE) {
            if (s->flags & AV_CODEC_FLAG_PASS1) {
                put_symbol_inline(c, p->state[context], diff, 1, s->rc_stat,
                                  s->rc_stat2[p->quant_table_index][context]);
            } else {
                put_symbol_inline(c, p->state[context], diff, 1, NULL, NULL);
            }
        } else {
            if (context == 0)
                run_mode = 1;

            if (run_mode) {
                if (diff) {
                    while (run_count >= 1 << ff_log2_run[run_index]) {
                        run_count -= 1 << ff_log2_run[run_index];
                        run_index++;
                        put_bits(&s->pb, 1, 1);
                    }

                    put_bits(&s->pb, 1 + ff_log2_run[run_index], run_count);
                    if (run_index)
                        run_index--;
                    run_count = 0;
                    run_mode  = 0;
                    if (diff > 0)
                        diff--;
                } else {
                    run_count++;
                }
            }

            ff_dlog(s->avctx, "count:%d index:%d, mode:%d, x:%d pos:%d\n",
                    run_count, run_index, run_mode, x,
                    (int)put_bits_count(&s->pb));

            if (run_mode == 0)
                put_vlc_symbol(&s->pb, &p->vlc_state[context], diff, bits);
        }
    }
    if (run_mode) {
        while (run_count >= 1 << ff_log2_run[run_index]) {
            run_count -= 1 << ff_log2_run[run_index];
            run_index++;
            put_bits(&s->pb, 1, 1);
        }

        if (run_count)
            put_bits(&s->pb, 1, 1);
    }
    s->run_index = run_index;

    return 0;
}

static void encode_plane(FFV1Context *s, uint8_t *src, int w, int h,
                         int stride, int plane_index)
{
    int x, y, i;
    const int ring_size = s->context_model ? 3 : 2;
    int16_t *sample[3];
    s->run_index = 0;

    memset(s->sample_buffer, 0, ring_size * (w + 6) * sizeof(*s->sample_buffer));

    for (y = 0; y < h; y++) {
        for (i = 0; i < ring_size; i++)
            sample[i] = s->sample_buffer + (w + 6) * ((h + i - y) % ring_size) + 3;

        sample[0][-1] = sample[1][0];
        sample[1][w]  = sample[1][w - 1];
// { START_TIMER
        if (s->bits_per_raw_sample <= 8) {
            for (x = 0; x < w; x++)
                sample[0][x] = src[x + stride * y];
            encode_line(s, w, sample, plane_index, 8);
        } else {
            if (s->packed_at_lsb) {
                for (x = 0; x < w; x++)
                    sample[0][x] = ((uint16_t *)(src + stride * y))[x];
            } else {
                for (x = 0; x < w; x++)
                    sample[0][x] =
                        ((uint16_t *)(src + stride * y))[x] >> (16 - s->bits_per_raw_sample);
            }
            encode_line(s, w, sample, plane_index, s->bits_per_raw_sample);
        }
// STOP_TIMER("encode line") }
    }
}

static void encode_rgb_frame(FFV1Context *s, const uint8_t *src[3],
                             int w, int h, const int stride[3])
{
    int x, y, p, i;
    const int ring_size = s->context_model ? 3 : 2;
    int16_t *sample[MAX_PLANES][3];
    int lbd  = s->avctx->bits_per_raw_sample <= 8;
    int bits = s->avctx->bits_per_raw_sample > 0
               ? s->avctx->bits_per_raw_sample
               : 8;
    int offset = 1 << bits;

    s->run_index = 0;

    memset(s->sample_buffer, 0, ring_size * MAX_PLANES *
                                (w + 6) * sizeof(*s->sample_buffer));

    for (y = 0; y < h; y++) {
        for (i = 0; i < ring_size; i++)
            for (p = 0; p < MAX_PLANES; p++)
                sample[p][i] = s->sample_buffer + p * ring_size *
                               (w + 6) +
                               ((h + i - y) % ring_size) * (w + 6) + 3;

        for (x = 0; x < w; x++) {
            int b, g, r, av_uninit(a);
            if (lbd) {
                unsigned v = *((const uint32_t *)(src[0] + x * 4 + stride[0] * y));
                b = v & 0xFF;
                g = (v >> 8) & 0xFF;
                r = (v >> 16) & 0xFF;
                a = v >> 24;
            } else {
                b = *((const uint16_t *)(src[0] + x * 2 + stride[0] * y));
                g = *((const uint16_t *)(src[1] + x * 2 + stride[1] * y));
                r = *((const uint16_t *)(src[2] + x * 2 + stride[2] * y));
            }

            b -= g;
            r -= g;
            g += (b + r) >> 2;
            b += offset;
            r += offset;

            sample[0][0][x] = g;
            sample[1][0][x] = b;
            sample[2][0][x] = r;
            sample[3][0][x] = a;
        }
        for (p = 0; p < 3 + s->transparency; p++) {
            sample[p][0][-1] = sample[p][1][0];
            sample[p][1][w]  = sample[p][1][w - 1];
            if (lbd)
                encode_line(s, w, sample[p], (p + 1) / 2, 9);
            else
                encode_line(s, w, sample[p], (p + 1) / 2, bits + 1);
        }
    }
}


static void write_quant_table(RangeCoder *c, int16_t *quant_table)
{
    int last = 0;
    int i;
    uint8_t state[CONTEXT_SIZE];
    memset(state, 128, sizeof(state));

    for (i = 1; i < 128; i++)
        if (quant_table[i] != quant_table[i - 1]) {
            put_symbol(c, state, i - last - 1, 0);
            last = i;
        }
    put_symbol(c, state, i - last - 1, 0);
}

static void write_quant_tables(RangeCoder *c,
                               int16_t quant_table[MAX_CONTEXT_INPUTS][256])
{
    int i;
    for (i = 0; i < 5; i++)
        write_quant_table(c, quant_table[i]);
}

static void write_header(FFV1Context *f)
{
    uint8_t state[CONTEXT_SIZE];
    int i;
    RangeCoder *const c = &f->slice_context[0]->c;

    memset(state, 128, sizeof(state));

    if (f->version < 2) {
        put_symbol(c, state, f->version, 0);
        put_symbol(c, state, f->ac, 0);
        if (f->ac == AC_RANGE_CUSTOM_TAB) {
            for (i = 1; i < 256; i++)
                put_symbol(c, state,
                           f->state_transition[i] - c->one_state[i], 1);
        }
        put_symbol(c, state, f->colorspace, 0); // YUV cs type
        if (f->version > 0)
            put_symbol(c, state, f->bits_per_raw_sample, 0);
        put_rac(c, state, f->chroma_planes);
        put_symbol(c, state, f->chroma_h_shift, 0);
        put_symbol(c, state, f->chroma_v_shift, 0);
        put_rac(c, state, f->transparency);

        write_quant_tables(c, f->quant_table);
    }
}

static int write_extradata(FFV1Context *f)
{
    RangeCoder *const c = &f->c;
    uint8_t state[CONTEXT_SIZE];
    int i, j, k;
    uint8_t state2[32][CONTEXT_SIZE];
    unsigned v;

    memset(state2, 128, sizeof(state2));
    memset(state, 128, sizeof(state));

    f->avctx->extradata_size = 10000 + 4 +
                                    (11 * 11 * 5 * 5 * 5 + 11 * 11 * 11) * 32;
    f->avctx->extradata = av_malloc(f->avctx->extradata_size);
    ff_init_range_encoder(c, f->avctx->extradata, f->avctx->extradata_size);
    ff_build_rac_states(c, 0.05 * (1LL << 32), 256 - 8);

    put_symbol(c, state, f->version, 0);
    if (f->version > 1) {
        if (f->version == 3)
            f->minor_version = 2;
        put_symbol(c, state, f->minor_version, 0);
    }

    put_symbol(c, state, f->ac, 0);
    if (f->ac == AC_RANGE_CUSTOM_TAB)
        for (i = 1; i < 256; i++)
            put_symbol(c, state, f->state_transition[i] - c->one_state[i], 1);

    put_symbol(c, state, f->colorspace, 0); // YUV cs type
    put_symbol(c, state, f->bits_per_raw_sample, 0);
    put_rac(c, state, f->chroma_planes);
    put_symbol(c, state, f->chroma_h_shift, 0);
    put_symbol(c, state, f->chroma_v_shift, 0);
    put_rac(c, state, f->transparency);
    put_symbol(c, state, f->num_h_slices - 1, 0);
    put_symbol(c, state, f->num_v_slices - 1, 0);

    put_symbol(c, state, f->quant_table_count, 0);
    for (i = 0; i < f->quant_table_count; i++)
        write_quant_tables(c, f->quant_tables[i]);

    for (i = 0; i < f->quant_table_count; i++) {
        for (j = 0; j < f->context_count[i] * CONTEXT_SIZE; j++)
            if (f->initial_states[i] && f->initial_states[i][0][j] != 128)
                break;
        if (j < f->context_count[i] * CONTEXT_SIZE) {
            put_rac(c, state, 1);
            for (j = 0; j < f->context_count[i]; j++)
                for (k = 0; k < CONTEXT_SIZE; k++) {
                    int pred = j ? f->initial_states[i][j - 1][k] : 128;
                    put_symbol(c, state2[k],
                               (int8_t)(f->initial_states[i][j][k] - pred), 1);
                }
        } else {
            put_rac(c, state, 0);
        }
    }

    if (f->version > 2) {
        put_symbol(c, state, f->ec, 0);
    }

    f->avctx->extradata_size = ff_rac_terminate(c);

    v = av_crc(av_crc_get_table(AV_CRC_32_IEEE), 0,
               f->avctx->extradata, f->avctx->extradata_size);
    AV_WL32(f->avctx->extradata + f->avctx->extradata_size, v);
    f->avctx->extradata_size += 4;

    return 0;
}

static int sort_stt(FFV1Context *s, uint8_t stt[256])
{
    int i, i2, changed, print = 0;

    do {
        changed = 0;
        for (i = 12; i < 244; i++) {
            for (i2 = i + 1; i2 < 245 && i2 < i + 4; i2++) {

#define COST(old, new)                                      \
    s->rc_stat[old][0] * -log2((256 - (new)) / 256.0) +     \
    s->rc_stat[old][1] * -log2((new)         / 256.0)

#define COST2(old, new)                         \
    COST(old, new) + COST(256 - (old), 256 - (new))

                double size0 = COST2(i,  i) + COST2(i2, i2);
                double sizeX = COST2(i, i2) + COST2(i2, i);
                if (sizeX < size0 && i != 128 && i2 != 128) {
                    int j;
                    FFSWAP(int, stt[i], stt[i2]);
                    FFSWAP(int, s->rc_stat[i][0], s->rc_stat[i2][0]);
                    FFSWAP(int, s->rc_stat[i][1], s->rc_stat[i2][1]);
                    if (i != 256 - i2) {
                        FFSWAP(int, stt[256 - i], stt[256 - i2]);
                        FFSWAP(int, s->rc_stat[256 - i][0], s->rc_stat[256 - i2][0]);
                        FFSWAP(int, s->rc_stat[256 - i][1], s->rc_stat[256 - i2][1]);
                    }
                    for (j = 1; j < 256; j++) {
                        if (stt[j] == i)
                            stt[j] = i2;
                        else if (stt[j] == i2)
                            stt[j] = i;
                        if (i != 256 - i2) {
                            if (stt[256 - j] == 256 - i)
                                stt[256 - j] = 256 - i2;
                            else if (stt[256 - j] == 256 - i2)
                                stt[256 - j] = 256 - i;
                        }
                    }
                    print = changed = 1;
                }
            }
        }
    } while (changed);
    return print;
}

static av_cold int init_slices_state(FFV1Context *f)
{
    int i, ret;
    for (i = 0; i < f->slice_count; i++) {
        FFV1Context *fs = f->slice_context[i];
        if ((ret = ffv1_init_slice_state(f, fs)) < 0)
            return AVERROR(ENOMEM);
    }
    return 0;
}

static av_cold int ffv1_encode_init(AVCodecContext *avctx)
{
    FFV1Context *s = avctx->priv_data;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    int i, j, k, m, ret;

    ffv1_common_init(avctx);

    s->version = 0;

    switch (avctx->level) {
    case 3:
        break;
    case 2:
        av_log(avctx, AV_LOG_ERROR,
               "Version 2 had been deemed non-standard and deprecated "
               "the support for it had been removed\n");
        return AVERROR(ENOSYS);
    case 1:
    case 0:
        if (avctx->flags & (AV_CODEC_FLAG_PASS1 | AV_CODEC_FLAG_PASS2)) {
            av_log(avctx, AV_LOG_ERROR,
                   "Multiple pass encoding requires version 3.\n");
            return AVERROR(ENOSYS);
        }
        if (avctx->slices > 1) {
            av_log(avctx, AV_LOG_ERROR,
                   "Multiple slices support requires version 3.\n");
            return AVERROR(ENOSYS);
        }
        break;
    case FF_LEVEL_UNKNOWN:
        if ((avctx->flags & (AV_CODEC_FLAG_PASS1 | AV_CODEC_FLAG_PASS2)) ||
            avctx->slices > 1)
            s->version = 3;
        else
            s->version = 0;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Version %d not supported\n",
               avctx->level);
        return AVERROR(ENOSYS);
    }

    if (s->ec < 0) {
        s->ec = (s->version >= 3);
    }

#if FF_API_CODER_TYPE
FF_DISABLE_DEPRECATION_WARNINGS
    if (avctx->coder_type != -1)
        s->ac = avctx->coder_type > 0 ? AC_RANGE_CUSTOM_TAB : AC_GOLOMB_RICE;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    s->plane_count = 3;
    switch (avctx->pix_fmt) {
    case AV_PIX_FMT_YUV444P9:
    case AV_PIX_FMT_YUV422P9:
    case AV_PIX_FMT_YUV420P9:
        if (!avctx->bits_per_raw_sample)
            s->bits_per_raw_sample = 9;
    case AV_PIX_FMT_YUV444P10:
    case AV_PIX_FMT_YUV420P10:
    case AV_PIX_FMT_YUV422P10:
        s->packed_at_lsb = 1;
        if (!avctx->bits_per_raw_sample && !s->bits_per_raw_sample)
            s->bits_per_raw_sample = 10;
    case AV_PIX_FMT_GRAY16:
    case AV_PIX_FMT_YUV444P16:
    case AV_PIX_FMT_YUV422P16:
    case AV_PIX_FMT_YUV420P16:
        if (!avctx->bits_per_raw_sample && !s->bits_per_raw_sample) {
            s->bits_per_raw_sample = 16;
        } else if (!s->bits_per_raw_sample) {
            s->bits_per_raw_sample = avctx->bits_per_raw_sample;
        }
        if (s->bits_per_raw_sample <= 8) {
            av_log(avctx, AV_LOG_ERROR, "bits_per_raw_sample invalid\n");
            return AVERROR_INVALIDDATA;
        }
        if (s->ac == AC_GOLOMB_RICE) {
            av_log(avctx, AV_LOG_INFO,
                   "bits_per_raw_sample > 8, forcing range coder\n");
            s->ac = AC_RANGE_CUSTOM_TAB;
        }
        s->version = FFMAX(s->version, 1);
    case AV_PIX_FMT_GRAY8:
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUV440P:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV411P:
    case AV_PIX_FMT_YUV410P:
        s->chroma_planes = desc->nb_components < 3 ? 0 : 1;
        s->colorspace    = 0;
        break;
    case AV_PIX_FMT_YUVA444P:
    case AV_PIX_FMT_YUVA422P:
    case AV_PIX_FMT_YUVA420P:
        s->chroma_planes = 1;
        s->colorspace    = 0;
        s->transparency  = 1;
        break;
    case AV_PIX_FMT_RGB32:
        s->colorspace   = 1;
        s->transparency = 1;
        break;
    case AV_PIX_FMT_GBRP9:
        if (!avctx->bits_per_raw_sample)
            s->bits_per_raw_sample = 9;
    case AV_PIX_FMT_GBRP10:
        if (!avctx->bits_per_raw_sample && !s->bits_per_raw_sample)
            s->bits_per_raw_sample = 10;
    case AV_PIX_FMT_GBRP16:
        if (!avctx->bits_per_raw_sample && !s->bits_per_raw_sample)
            s->bits_per_raw_sample = 16;
        else if (!s->bits_per_raw_sample)
            s->bits_per_raw_sample = avctx->bits_per_raw_sample;
        s->colorspace    = 1;
        s->chroma_planes = 1;
        s->version       = FFMAX(s->version, 1);
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "format not supported\n");
        return AVERROR_INVALIDDATA;
    }
    if (s->transparency) {
        av_log(
            avctx, AV_LOG_WARNING,
            "Storing alpha plane, this will require a recent FFV1 decoder to playback!\n");
    }
#if FF_API_PRIVATE_OPT
FF_DISABLE_DEPRECATION_WARNINGS
    if (avctx->context_model)
        s->context_model = avctx->context_model;
    if (avctx->context_model > 1U) {
        av_log(avctx, AV_LOG_ERROR,
               "Invalid context model %d, valid values are 0 and 1\n",
               avctx->context_model);
        return AVERROR(EINVAL);
    }
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    if (s->ac == AC_RANGE_CUSTOM_TAB)
        for (i = 1; i < 256; i++)
            s->state_transition[i] = ffv1_ver2_state[i];

    for (i = 0; i < 256; i++) {
        s->quant_table_count = 2;
        if (s->bits_per_raw_sample <= 8) {
            s->quant_tables[0][0][i] = ffv1_quant11[i];
            s->quant_tables[0][1][i] = ffv1_quant11[i] * 11;
            s->quant_tables[0][2][i] = ffv1_quant11[i] * 11 * 11;
            s->quant_tables[1][0][i] = ffv1_quant11[i];
            s->quant_tables[1][1][i] = ffv1_quant11[i] * 11;
            s->quant_tables[1][2][i] = ffv1_quant5[i]  * 11 * 11;
            s->quant_tables[1][3][i] = ffv1_quant5[i]  *  5 * 11 * 11;
            s->quant_tables[1][4][i] = ffv1_quant5[i]  *  5 *  5 * 11 * 11;
        } else {
            s->quant_tables[0][0][i] = ffv1_quant9_10bit[i];
            s->quant_tables[0][1][i] = ffv1_quant9_10bit[i] * 11;
            s->quant_tables[0][2][i] = ffv1_quant9_10bit[i] * 11 * 11;
            s->quant_tables[1][0][i] = ffv1_quant9_10bit[i];
            s->quant_tables[1][1][i] = ffv1_quant9_10bit[i] * 11;
            s->quant_tables[1][2][i] = ffv1_quant5_10bit[i] * 11 * 11;
            s->quant_tables[1][3][i] = ffv1_quant5_10bit[i] *  5 * 11 * 11;
            s->quant_tables[1][4][i] = ffv1_quant5_10bit[i] *  5 *  5 * 11 * 11;
        }
    }
    s->context_count[0] = (11 * 11 * 11        + 1) / 2;
    s->context_count[1] = (11 * 11 * 5 * 5 * 5 + 1) / 2;
    memcpy(s->quant_table, s->quant_tables[s->context_model],
           sizeof(s->quant_table));

    for (i = 0; i < s->plane_count; i++) {
        PlaneContext *const p = &s->plane[i];

        memcpy(p->quant_table, s->quant_table, sizeof(p->quant_table));
        p->quant_table_index = s->context_model;
        p->context_count     = s->context_count[p->quant_table_index];
    }

    if ((ret = ffv1_allocate_initial_states(s)) < 0)
        return ret;

#if FF_API_CODED_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
    avctx->coded_frame->pict_type = AV_PICTURE_TYPE_I;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    if (!s->transparency)
        s->plane_count = 2;

    av_pix_fmt_get_chroma_sub_sample(avctx->pix_fmt, &s->chroma_h_shift,
                                     &s->chroma_v_shift);

    s->picture_number = 0;

    if (avctx->flags & (AV_CODEC_FLAG_PASS1 | AV_CODEC_FLAG_PASS2)) {
        for (i = 0; i < s->quant_table_count; i++) {
            s->rc_stat2[i] = av_mallocz(s->context_count[i] *
                                        sizeof(*s->rc_stat2[i]));
            if (!s->rc_stat2[i])
                return AVERROR(ENOMEM);
        }
    }
    if (avctx->stats_in) {
        char *p = avctx->stats_in;
        uint8_t best_state[256][256];
        int gob_count = 0;
        char *next;

        av_assert0(s->version > 2);

        for (;; ) {
            for (j = 0; j < 256; j++)
                for (i = 0; i < 2; i++) {
                    s->rc_stat[j][i] = strtol(p, &next, 0);
                    if (next == p) {
                        av_log(avctx, AV_LOG_ERROR,
                               "2Pass file invalid at %d %d [%s]\n", j, i, p);
                        return AVERROR_INVALIDDATA;
                    }
                    p = next;
                }
            for (i = 0; i < s->quant_table_count; i++)
                for (j = 0; j < s->context_count[i]; j++) {
                    for (k = 0; k < 32; k++)
                        for (m = 0; m < 2; m++) {
                            s->rc_stat2[i][j][k][m] = strtol(p, &next, 0);
                            if (next == p) {
                                av_log(avctx, AV_LOG_ERROR,
                                       "2Pass file invalid at %d %d %d %d [%s]\n",
                                       i, j, k, m, p);
                                return AVERROR_INVALIDDATA;
                            }
                            p = next;
                        }
                }
            gob_count = strtol(p, &next, 0);
            if (next == p || gob_count <= 0) {
                av_log(avctx, AV_LOG_ERROR, "2Pass file invalid\n");
                return AVERROR_INVALIDDATA;
            }
            p = next;
            while (*p == '\n' || *p == ' ')
                p++;
            if (p[0] == 0)
                break;
        }
        sort_stt(s, s->state_transition);

        find_best_state(best_state, s->state_transition);

        for (i = 0; i < s->quant_table_count; i++) {
            for (j = 0; j < s->context_count[i]; j++)
                for (k = 0; k < 32; k++) {
                    double p = 128;
                    if (s->rc_stat2[i][j][k][0] + s->rc_stat2[i][j][k][1]) {
                        p = 256.0 * s->rc_stat2[i][j][k][1] /
                            (s->rc_stat2[i][j][k][0] + s->rc_stat2[i][j][k][1]);
                    }
                    s->initial_states[i][j][k] =
                        best_state[av_clip(round(p), 1, 255)][av_clip((s->rc_stat2[i][j][k][0] +
                                                                       s->rc_stat2[i][j][k][1]) /
                                                                      gob_count, 0, 255)];
                }
        }
    }

    if (s->version > 1) {
        for (s->num_v_slices = 2; s->num_v_slices < 9; s->num_v_slices++)
            for (s->num_h_slices = s->num_v_slices;
                 s->num_h_slices < 2 * s->num_v_slices; s->num_h_slices++)
                if (avctx->slices == s->num_h_slices * s->num_v_slices &&
                    avctx->slices <= 64 || !avctx->slices)
                    goto slices_ok;
        av_log(avctx, AV_LOG_ERROR,
               "Unsupported number %d of slices requested, please specify a "
               "supported number with -slices (ex:4,6,9,12,16, ...)\n",
               avctx->slices);
        return AVERROR(ENOSYS);
slices_ok:
        write_extradata(s);
    }

    if ((ret = ffv1_init_slice_contexts(s)) < 0)
        return ret;
    if ((ret = init_slices_state(s)) < 0)
        return ret;

#define STATS_OUT_SIZE 1024 * 1024 * 6
    if (avctx->flags & AV_CODEC_FLAG_PASS1) {
        avctx->stats_out = av_mallocz(STATS_OUT_SIZE);
        for (i = 0; i < s->quant_table_count; i++)
            for (j = 0; j < s->slice_count; j++) {
                FFV1Context *sf = s->slice_context[j];
                av_assert0(!sf->rc_stat2[i]);
                sf->rc_stat2[i] = av_mallocz(s->context_count[i] *
                                             sizeof(*sf->rc_stat2[i]));
                if (!sf->rc_stat2[i])
                    return AVERROR(ENOMEM);
            }
    }

    return 0;
}

static void encode_slice_header(FFV1Context *f, FFV1Context *fs)
{
    RangeCoder *c = &fs->c;
    uint8_t state[CONTEXT_SIZE];
    int j;
    memset(state, 128, sizeof(state));

    put_symbol(c, state, (fs->slice_x + 1) * f->num_h_slices / f->width, 0);
    put_symbol(c, state, (fs->slice_y + 1) * f->num_v_slices / f->height, 0);
    put_symbol(c, state, (fs->slice_width + 1) * f->num_h_slices / f->width - 1,
               0);
    put_symbol(c, state,
               (fs->slice_height + 1) * f->num_v_slices / f->height - 1,
               0);
    for (j = 0; j < f->plane_count; j++) {
        put_symbol(c, state, f->plane[j].quant_table_index, 0);
        av_assert0(f->plane[j].quant_table_index == f->context_model);
    }
    if (!f->frame->interlaced_frame)
        put_symbol(c, state, 3, 0);
    else
        put_symbol(c, state, 1 + !f->frame->top_field_first, 0);
    put_symbol(c, state, f->frame->sample_aspect_ratio.num, 0);
    put_symbol(c, state, f->frame->sample_aspect_ratio.den, 0);
}

static int encode_slice(AVCodecContext *c, void *arg)
{
    FFV1Context *fs  = *(void **)arg;
    FFV1Context *f   = fs->avctx->priv_data;
    int width        = fs->slice_width;
    int height       = fs->slice_height;
    int x            = fs->slice_x;
    int y            = fs->slice_y;
    const AVFrame *const p = f->frame;
    const int ps     = (av_pix_fmt_desc_get(c->pix_fmt)->flags & AV_PIX_FMT_FLAG_PLANAR)
                       ? (f->bits_per_raw_sample > 8) + 1
                       : 4;

    if (f->key_frame)
        ffv1_clear_slice_state(f, fs);
    if (f->version > 2) {
        encode_slice_header(f, fs);
    }
    if (fs->ac == AC_GOLOMB_RICE) {
        if (f->version > 2)
            put_rac(&fs->c, (uint8_t[]) { 129 }, 0);
        fs->ac_byte_count = f->version > 2 || (!x && !y) ? ff_rac_terminate( &fs->c) : 0;
        init_put_bits(&fs->pb, fs->c.bytestream_start + fs->ac_byte_count,
                      fs->c.bytestream_end - fs->c.bytestream_start - fs->ac_byte_count);
    }

    if (f->colorspace == 0) {
        const int chroma_width  = AV_CEIL_RSHIFT(width,  f->chroma_h_shift);
        const int chroma_height = AV_CEIL_RSHIFT(height, f->chroma_v_shift);
        const int cx            = x >> f->chroma_h_shift;
        const int cy            = y >> f->chroma_v_shift;

        encode_plane(fs, p->data[0] + ps * x + y * p->linesize[0],
                     width, height, p->linesize[0], 0);

        if (f->chroma_planes) {
            encode_plane(fs, p->data[1] + ps * cx + cy * p->linesize[1],
                         chroma_width, chroma_height, p->linesize[1], 1);
            encode_plane(fs, p->data[2] + ps * cx + cy * p->linesize[2],
                         chroma_width, chroma_height, p->linesize[2], 1);
        }
        if (fs->transparency)
            encode_plane(fs, p->data[3] + ps * x + y * p->linesize[3], width,
                         height, p->linesize[3], 2);
    } else {
        const uint8_t *planes[3] = { p->data[0] + ps * x + y * p->linesize[0],
                                     p->data[1] + ps * x + y * p->linesize[1],
                                     p->data[2] + ps * x + y * p->linesize[2] };
        encode_rgb_frame(fs, planes, width, height, p->linesize);
    }
    emms_c();

    return 0;
}

static int ffv1_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                        const AVFrame *pict, int *got_packet)
{
    FFV1Context *f      = avctx->priv_data;
    RangeCoder *const c = &f->slice_context[0]->c;
    int used_count      = 0;
    uint8_t keystate    = 128;
    uint8_t *buf_p;
    int i, ret;

    f->frame = pict;

    if ((ret = ff_alloc_packet(pkt, avctx->width * avctx->height *
                             ((8 * 2 + 1 + 1) * 4) / 8 +
                             AV_INPUT_BUFFER_MIN_SIZE)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error getting output packet.\n");
        return ret;
    }

    ff_init_range_encoder(c, pkt->data, pkt->size);
    ff_build_rac_states(c, 0.05 * (1LL << 32), 256 - 8);

    if (avctx->gop_size == 0 || f->picture_number % avctx->gop_size == 0) {
        put_rac(c, &keystate, 1);
        f->key_frame = 1;
        f->gob_count++;
        write_header(f);
    } else {
        put_rac(c, &keystate, 0);
        f->key_frame = 0;
    }

    if (f->ac == AC_RANGE_CUSTOM_TAB) {
        int i;
        for (i = 1; i < 256; i++) {
            c->one_state[i]        = f->state_transition[i];
            c->zero_state[256 - i] = 256 - c->one_state[i];
        }
    }

    for (i = 1; i < f->slice_count; i++) {
        FFV1Context *fs = f->slice_context[i];
        uint8_t *start  = pkt->data +
                          (pkt->size - used_count) * (int64_t)i / f->slice_count;
        int len = pkt->size / f->slice_count;
        ff_init_range_encoder(&fs->c, start, len);
    }
    avctx->execute(avctx, encode_slice, &f->slice_context[0], NULL,
                   f->slice_count, sizeof(void *));

    buf_p = pkt->data;
    for (i = 0; i < f->slice_count; i++) {
        FFV1Context *fs = f->slice_context[i];
        int bytes;

        if (fs->ac != AC_GOLOMB_RICE) {
            uint8_t state = 129;
            put_rac(&fs->c, &state, 0);
            bytes = ff_rac_terminate(&fs->c);
        } else {
            flush_put_bits(&fs->pb); // FIXME: nicer padding
            bytes = fs->ac_byte_count + (put_bits_count(&fs->pb) + 7) / 8;
        }
        if (i > 0 || f->version > 2) {
            av_assert0(bytes < pkt->size / f->slice_count);
            memmove(buf_p, fs->c.bytestream_start, bytes);
            av_assert0(bytes < (1 << 24));
            AV_WB24(buf_p + bytes, bytes);
            bytes += 3;
        }
        if (f->ec) {
            unsigned v;
            buf_p[bytes++] = 0;
            v = av_crc(av_crc_get_table(AV_CRC_32_IEEE), 0, buf_p, bytes);
            AV_WL32(buf_p + bytes, v);
            bytes += 4;
        }
        buf_p += bytes;
    }

    if ((avctx->flags & AV_CODEC_FLAG_PASS1) && (f->picture_number & 31) == 0) {
        int j, k, m;
        char *p   = avctx->stats_out;
        char *end = p + STATS_OUT_SIZE;

        memset(f->rc_stat, 0, sizeof(f->rc_stat));
        for (i = 0; i < f->quant_table_count; i++)
            memset(f->rc_stat2[i], 0, f->context_count[i] * sizeof(*f->rc_stat2[i]));

        for (j = 0; j < f->slice_count; j++) {
            FFV1Context *fs = f->slice_context[j];
            for (i = 0; i < 256; i++) {
                f->rc_stat[i][0] += fs->rc_stat[i][0];
                f->rc_stat[i][1] += fs->rc_stat[i][1];
            }
            for (i = 0; i < f->quant_table_count; i++) {
                for (k = 0; k < f->context_count[i]; k++)
                    for (m = 0; m < 32; m++) {
                        f->rc_stat2[i][k][m][0] += fs->rc_stat2[i][k][m][0];
                        f->rc_stat2[i][k][m][1] += fs->rc_stat2[i][k][m][1];
                    }
            }
        }

        for (j = 0; j < 256; j++) {
            snprintf(p, end - p, "%" PRIu64 " %" PRIu64 " ",
                     f->rc_stat[j][0], f->rc_stat[j][1]);
            p += strlen(p);
        }
        snprintf(p, end - p, "\n");

        for (i = 0; i < f->quant_table_count; i++) {
            for (j = 0; j < f->context_count[i]; j++)
                for (m = 0; m < 32; m++) {
                    snprintf(p, end - p, "%" PRIu64 " %" PRIu64 " ",
                             f->rc_stat2[i][j][m][0], f->rc_stat2[i][j][m][1]);
                    p += strlen(p);
                }
        }
        snprintf(p, end - p, "%d\n", f->gob_count);
    } else if (avctx->flags & AV_CODEC_FLAG_PASS1)
        avctx->stats_out[0] = '\0';

#if FF_API_CODED_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
    avctx->coded_frame->key_frame = f->key_frame;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    f->picture_number++;
    pkt->size   = buf_p - pkt->data;
    pkt->flags |= AV_PKT_FLAG_KEY * f->key_frame;
    *got_packet = 1;

    return 0;
}

static av_cold int ffv1_encode_close(AVCodecContext *avctx)
{
    ffv1_close(avctx);
    return 0;
}

#define OFFSET(x) offsetof(FFV1Context, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "slicecrc", "Protect slices with CRCs", OFFSET(ec), AV_OPT_TYPE_INT,
             { .i64 = -1 }, -1, 1, VE },
    { "coder", "Coder type", OFFSET(ac), AV_OPT_TYPE_INT,
            { .i64 = AC_GOLOMB_RICE }, 0, 2, VE, "coder" },
        { "rice", "Golomb rice", 0, AV_OPT_TYPE_CONST,
            { .i64 = AC_GOLOMB_RICE }, INT_MIN, INT_MAX, VE, "coder" },
        { "range_def", "Range with default table", 0, AV_OPT_TYPE_CONST,
            { .i64 = AC_RANGE_DEFAULT_TAB }, INT_MIN, INT_MAX, VE, "coder" },
        { "range_tab", "Range with custom table", 0, AV_OPT_TYPE_CONST,
            { .i64 = AC_RANGE_CUSTOM_TAB }, INT_MIN, INT_MAX, VE, "coder" },
    { "context", "Context model", OFFSET(context_model), AV_OPT_TYPE_INT,
            { .i64 = 0 }, 0, 1, VE },

    { NULL }
};

static const AVClass class = {
    .class_name = "ffv1 encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

#if FF_API_CODER_TYPE
static const AVCodecDefault ffv1_defaults[] = {
    { "coder", "-1" },
    { NULL },
};
#endif

AVCodec ff_ffv1_encoder = {
    .name           = "ffv1",
    .long_name      = NULL_IF_CONFIG_SMALL("FFmpeg video codec #1"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_FFV1,
    .priv_data_size = sizeof(FFV1Context),
    .init           = ffv1_encode_init,
    .encode2        = ffv1_encode_frame,
    .close          = ffv1_encode_close,
    .capabilities   = AV_CODEC_CAP_SLICE_THREADS,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_YUV420P,   AV_PIX_FMT_YUV422P,   AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUV411P,   AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_YUV444P9,  AV_PIX_FMT_YUV422P9,  AV_PIX_FMT_YUV420P9,
        AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
        AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
        AV_PIX_FMT_RGB32,
        AV_PIX_FMT_GBRP9,     AV_PIX_FMT_GBRP10,
        AV_PIX_FMT_YUVA420P,  AV_PIX_FMT_YUVA422P,  AV_PIX_FMT_YUVA444P,
        AV_PIX_FMT_GRAY16,    AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_NONE

    },
#if FF_API_CODER_TYPE
    .defaults       = ffv1_defaults,
#endif
    .priv_class     = &class,
};

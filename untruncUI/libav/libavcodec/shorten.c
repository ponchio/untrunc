/*
 * Shorten decoder
 * Copyright (c) 2005 Jeff Muizelaar
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
 * Shorten decoder
 * @author Jeff Muizelaar
 */

#include <limits.h>

#include "avcodec.h"
#include "bitstream.h"
#include "bytestream.h"
#include "golomb.h"
#include "internal.h"

#define MAX_CHANNELS 8
#define MAX_BLOCKSIZE 65535

#define OUT_BUFFER_SIZE 16384

#define ULONGSIZE 2

#define WAVE_FORMAT_PCM 0x0001

#define DEFAULT_BLOCK_SIZE 256

#define TYPESIZE 4
#define CHANSIZE 0
#define LPCQSIZE 2
#define ENERGYSIZE 3
#define BITSHIFTSIZE 2

#define TYPE_S16HL 3
#define TYPE_S16LH 5

#define NWRAP 3
#define NSKIPSIZE 1

#define LPCQUANT 5
#define V2LPCQOFFSET (1 << LPCQUANT)

#define FNSIZE 2
#define FN_DIFF0        0
#define FN_DIFF1        1
#define FN_DIFF2        2
#define FN_DIFF3        3
#define FN_QUIT         4
#define FN_BLOCKSIZE    5
#define FN_BITSHIFT     6
#define FN_QLPC         7
#define FN_ZERO         8
#define FN_VERBATIM     9

/** indicates if the FN_* command is audio or non-audio */
static const uint8_t is_audio_command[10] = { 1, 1, 1, 1, 0, 0, 0, 1, 1, 0 };

#define VERBATIM_CKSIZE_SIZE 5
#define VERBATIM_BYTE_SIZE 8
#define CANONICAL_HEADER_SIZE 44

typedef struct ShortenContext {
    AVCodecContext *avctx;
    BitstreamContext bc;

    int min_framesize, max_framesize;
    unsigned channels;

    int32_t *decoded[MAX_CHANNELS];
    int32_t *decoded_base[MAX_CHANNELS];
    int32_t *offset[MAX_CHANNELS];
    int *coeffs;
    uint8_t *bitstream;
    int bitstream_size;
    int bitstream_index;
    unsigned int allocated_bitstream_size;
    int header_size;
    uint8_t header[OUT_BUFFER_SIZE];
    int version;
    int cur_chan;
    int bitshift;
    int nmean;
    int internal_ftype;
    int nwrap;
    int blocksize;
    int bitindex;
    int32_t lpcqoffset;
    int got_header;
    int got_quit_command;
} ShortenContext;

static av_cold int shorten_decode_init(AVCodecContext *avctx)
{
    ShortenContext *s = avctx->priv_data;
    s->avctx          = avctx;
    avctx->sample_fmt = AV_SAMPLE_FMT_S16P;

    return 0;
}

static int allocate_buffers(ShortenContext *s)
{
    int i, chan, err;

    for (chan = 0; chan < s->channels; chan++) {
        if (FFMAX(1, s->nmean) >= UINT_MAX / sizeof(int32_t)) {
            av_log(s->avctx, AV_LOG_ERROR, "nmean too large\n");
            return AVERROR_INVALIDDATA;
        }
        if (s->blocksize + s->nwrap >= UINT_MAX / sizeof(int32_t) ||
            s->blocksize + s->nwrap <= (unsigned)s->nwrap) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "s->blocksize + s->nwrap too large\n");
            return AVERROR_INVALIDDATA;
        }

        if ((err = av_reallocp(&s->offset[chan],
                               sizeof(int32_t) *
                               FFMAX(1, s->nmean))) < 0)
            return err;

        if ((err = av_reallocp(&s->decoded_base[chan], (s->blocksize + s->nwrap) *
                               sizeof(s->decoded_base[0][0]))) < 0)
            return err;
        for (i = 0; i < s->nwrap; i++)
            s->decoded_base[chan][i] = 0;
        s->decoded[chan] = s->decoded_base[chan] + s->nwrap;
    }

    if ((err = av_reallocp(&s->coeffs, s->nwrap * sizeof(*s->coeffs))) < 0)
        return err;

    return 0;
}

static inline unsigned int get_uint(ShortenContext *s, int k)
{
    if (s->version != 0)
        k = get_ur_golomb_shorten(&s->bc, ULONGSIZE);
    return get_ur_golomb_shorten(&s->bc, k);
}

static void fix_bitshift(ShortenContext *s, int32_t *buffer)
{
    int i;

    if (s->bitshift != 0)
        for (i = 0; i < s->blocksize; i++)
            buffer[i] <<= s->bitshift;
}

static int init_offset(ShortenContext *s)
{
    int32_t mean = 0;
    int chan, i;
    int nblock = FFMAX(1, s->nmean);
    /* initialise offset */
    switch (s->internal_ftype) {
    case TYPE_S16HL:
    case TYPE_S16LH:
        mean = 0;
        break;
    default:
        av_log(s->avctx, AV_LOG_ERROR, "unknown audio type");
        return AVERROR_INVALIDDATA;
    }

    for (chan = 0; chan < s->channels; chan++)
        for (i = 0; i < nblock; i++)
            s->offset[chan][i] = mean;
    return 0;
}

static int decode_wave_header(AVCodecContext *avctx, const uint8_t *header,
                              int header_size)
{
    int len;
    short wave_format;
    GetByteContext gb;

    bytestream2_init(&gb, header, header_size);

    if (bytestream2_get_le32(&gb) != MKTAG('R', 'I', 'F', 'F')) {
        av_log(avctx, AV_LOG_ERROR, "missing RIFF tag\n");
        return AVERROR_INVALIDDATA;
    }

    bytestream2_skip(&gb, 4); /* chunk size */

    if (bytestream2_get_le32(&gb) != MKTAG('W', 'A', 'V', 'E')) {
        av_log(avctx, AV_LOG_ERROR, "missing WAVE tag\n");
        return AVERROR_INVALIDDATA;
    }

    while (bytestream2_get_le32(&gb) != MKTAG('f', 'm', 't', ' ')) {
        len = bytestream2_get_le32(&gb);
        bytestream2_skip(&gb, len);
        if (bytestream2_get_bytes_left(&gb) < 16) {
            av_log(avctx, AV_LOG_ERROR, "no fmt chunk found\n");
            return AVERROR_INVALIDDATA;
        }
    }
    len = bytestream2_get_le32(&gb);

    if (len < 16) {
        av_log(avctx, AV_LOG_ERROR, "fmt chunk was too short\n");
        return AVERROR_INVALIDDATA;
    }

    wave_format = bytestream2_get_le16(&gb);

    switch (wave_format) {
    case WAVE_FORMAT_PCM:
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "unsupported wave format\n");
        return AVERROR(ENOSYS);
    }

    bytestream2_skip(&gb, 2); // skip channels    (already got from shorten header)
    avctx->sample_rate = bytestream2_get_le32(&gb);
    bytestream2_skip(&gb, 4); // skip bit rate    (represents original uncompressed bit rate)
    bytestream2_skip(&gb, 2); // skip block align (not needed)
    avctx->bits_per_coded_sample = bytestream2_get_le16(&gb);

    if (avctx->bits_per_coded_sample != 16) {
        av_log(avctx, AV_LOG_ERROR, "unsupported number of bits per sample\n");
        return AVERROR(ENOSYS);
    }

    len -= 16;
    if (len > 0)
        av_log(avctx, AV_LOG_INFO, "%d header bytes unparsed\n", len);

    return 0;
}

static void output_buffer(int16_t **samples, int nchan, int blocksize,
                          int32_t **buffer)
{
    int i, ch;
    for (ch = 0; ch < nchan; ch++) {
        int32_t *in  = buffer[ch];
        int16_t *out = samples[ch];
        for (i = 0; i < blocksize; i++)
            out[i] = av_clip_int16(in[i]);
    }
}

static const int fixed_coeffs[][3] = {
    { 0,  0,  0 },
    { 1,  0,  0 },
    { 2, -1,  0 },
    { 3, -3,  1 }
};

static int decode_subframe_lpc(ShortenContext *s, int command, int channel,
                               int residual_size, int32_t coffset)
{
    int pred_order, sum, qshift, init_sum, i, j;
    const int *coeffs;

    if (command == FN_QLPC) {
        /* read/validate prediction order */
        pred_order = get_ur_golomb_shorten(&s->bc, LPCQSIZE);
        if (pred_order > s->nwrap) {
            av_log(s->avctx, AV_LOG_ERROR, "invalid pred_order %d\n",
                   pred_order);
            return AVERROR(EINVAL);
        }
        /* read LPC coefficients */
        for (i = 0; i < pred_order; i++)
            s->coeffs[i] = get_sr_golomb_shorten(&s->bc, LPCQUANT);
        coeffs = s->coeffs;

        qshift = LPCQUANT;
    } else {
        /* fixed LPC coeffs */
        pred_order = command;
        if (pred_order >= FF_ARRAY_ELEMS(fixed_coeffs)) {
            av_log(s->avctx, AV_LOG_ERROR, "invalid pred_order %d\n",
                   pred_order);
            return AVERROR_INVALIDDATA;
        }
        coeffs     = fixed_coeffs[pred_order];
        qshift     = 0;
    }

    /* subtract offset from previous samples to use in prediction */
    if (command == FN_QLPC && coffset)
        for (i = -pred_order; i < 0; i++)
            s->decoded[channel][i] -= coffset;

    /* decode residual and do LPC prediction */
    init_sum = pred_order ? (command == FN_QLPC ? s->lpcqoffset : 0) : coffset;
    for (i = 0; i < s->blocksize; i++) {
        sum = init_sum;
        for (j = 0; j < pred_order; j++)
            sum += coeffs[j] * s->decoded[channel][i - j - 1];
        s->decoded[channel][i] = get_sr_golomb_shorten(&s->bc, residual_size) +
                                 (sum >> qshift);
    }

    /* add offset to current samples */
    if (command == FN_QLPC && coffset)
        for (i = 0; i < s->blocksize; i++)
            s->decoded[channel][i] += coffset;

    return 0;
}

static int read_header(ShortenContext *s)
{
    int i, ret;
    int maxnlpc = 0;
    /* shorten signature */
    if (bitstream_read(&s->bc, 32) != AV_RB32("ajkg")) {
        av_log(s->avctx, AV_LOG_ERROR, "missing shorten magic 'ajkg'\n");
        return AVERROR_INVALIDDATA;
    }

    s->lpcqoffset     = 0;
    s->blocksize      = DEFAULT_BLOCK_SIZE;
    s->nmean          = -1;
    s->version        = bitstream_read(&s->bc, 8);
    s->internal_ftype = get_uint(s, TYPESIZE);

    s->channels = get_uint(s, CHANSIZE);
    if (!s->channels) {
        av_log(s->avctx, AV_LOG_ERROR, "No channels reported\n");
        return AVERROR_INVALIDDATA;
    }
    if (s->channels > MAX_CHANNELS) {
        av_log(s->avctx, AV_LOG_ERROR, "too many channels: %d\n", s->channels);
        s->channels = 0;
        return AVERROR_INVALIDDATA;
    }
    s->avctx->channels = s->channels;

    /* get blocksize if version > 0 */
    if (s->version > 0) {
        int skip_bytes;
        unsigned blocksize;

        blocksize = get_uint(s, av_log2(DEFAULT_BLOCK_SIZE));
        if (!blocksize || blocksize > MAX_BLOCKSIZE) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "invalid or unsupported block size: %d\n",
                   blocksize);
            return AVERROR(EINVAL);
        }
        s->blocksize = blocksize;

        maxnlpc  = get_uint(s, LPCQSIZE);
        s->nmean = get_uint(s, 0);

        skip_bytes = get_uint(s, NSKIPSIZE);
        for (i = 0; i < skip_bytes; i++)
            bitstream_skip(&s->bc, 8);
    }
    s->nwrap = FFMAX(NWRAP, maxnlpc);

    if ((ret = allocate_buffers(s)) < 0)
        return ret;

    if ((ret = init_offset(s)) < 0)
        return ret;

    if (s->version > 1)
        s->lpcqoffset = V2LPCQOFFSET;

    if (get_ur_golomb_shorten(&s->bc, FNSIZE) != FN_VERBATIM) {
        av_log(s->avctx, AV_LOG_ERROR,
               "missing verbatim section at beginning of stream\n");
        return AVERROR_INVALIDDATA;
    }

    s->header_size = get_ur_golomb_shorten(&s->bc, VERBATIM_CKSIZE_SIZE);
    if (s->header_size >= OUT_BUFFER_SIZE ||
        s->header_size < CANONICAL_HEADER_SIZE) {
        av_log(s->avctx, AV_LOG_ERROR, "header is wrong size: %d\n",
               s->header_size);
        return AVERROR_INVALIDDATA;
    }

    for (i = 0; i < s->header_size; i++)
        s->header[i] = (char)get_ur_golomb_shorten(&s->bc, VERBATIM_BYTE_SIZE);

    if ((ret = decode_wave_header(s->avctx, s->header, s->header_size)) < 0)
        return ret;

    s->cur_chan = 0;
    s->bitshift = 0;

    s->got_header = 1;

    return 0;
}

static int shorten_decode_frame(AVCodecContext *avctx, void *data,
                                int *got_frame_ptr, AVPacket *avpkt)
{
    AVFrame *frame     = data;
    const uint8_t *buf = avpkt->data;
    int buf_size       = avpkt->size;
    ShortenContext *s  = avctx->priv_data;
    int i, input_buf_size = 0;
    int ret;

    /* allocate internal bitstream buffer */
    if (s->max_framesize == 0) {
        void *tmp_ptr;
        s->max_framesize = 1024; // should hopefully be enough for the first header
        tmp_ptr = av_fast_realloc(s->bitstream, &s->allocated_bitstream_size,
                                  s->max_framesize + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!tmp_ptr) {
            av_log(avctx, AV_LOG_ERROR, "error allocating bitstream buffer\n");
            return AVERROR(ENOMEM);
        }
        s->bitstream = tmp_ptr;
    }

    /* append current packet data to bitstream buffer */
    if (1 && s->max_framesize) { //FIXME truncated
        buf_size       = FFMIN(buf_size, s->max_framesize - s->bitstream_size);
        input_buf_size = buf_size;

        if (s->bitstream_index + s->bitstream_size + buf_size >
            s->allocated_bitstream_size) {
            memmove(s->bitstream, &s->bitstream[s->bitstream_index],
                    s->bitstream_size);
            s->bitstream_index = 0;
        }
        if (buf)
            memcpy(&s->bitstream[s->bitstream_index + s->bitstream_size], buf,
                   buf_size);
        buf               = &s->bitstream[s->bitstream_index];
        buf_size         += s->bitstream_size;
        s->bitstream_size = buf_size;

        /* do not decode until buffer has at least max_framesize bytes or
         * the end of the file has been reached */
        if (buf_size < s->max_framesize && avpkt->data) {
            *got_frame_ptr = 0;
            return input_buf_size;
        }
    }
    /* init and position bitstream reader */
    bitstream_init8(&s->bc, buf, buf_size);
    bitstream_skip(&s->bc, s->bitindex);

    /* process header or next subblock */
    if (!s->got_header) {
        if ((ret = read_header(s)) < 0)
            return ret;
        *got_frame_ptr = 0;
        goto finish_frame;
    }

    /* if quit command was read previously, don't decode anything */
    if (s->got_quit_command) {
        *got_frame_ptr = 0;
        return avpkt->size;
    }

    s->cur_chan = 0;
    while (s->cur_chan < s->channels) {
        unsigned cmd;
        int len;

        if (bitstream_bits_left(&s->bc) < 3 + FNSIZE) {
            *got_frame_ptr = 0;
            break;
        }

        cmd = get_ur_golomb_shorten(&s->bc, FNSIZE);

        if (cmd > FN_VERBATIM) {
            av_log(avctx, AV_LOG_ERROR, "unknown shorten function %d\n", cmd);
            *got_frame_ptr = 0;
            break;
        }

        if (!is_audio_command[cmd]) {
            /* process non-audio command */
            switch (cmd) {
            case FN_VERBATIM:
                len = get_ur_golomb_shorten(&s->bc, VERBATIM_CKSIZE_SIZE);
                while (len--)
                    get_ur_golomb_shorten(&s->bc, VERBATIM_BYTE_SIZE);
                break;
            case FN_BITSHIFT:
                s->bitshift = get_ur_golomb_shorten(&s->bc, BITSHIFTSIZE);
                if (s->bitshift < 0)
                    return AVERROR_INVALIDDATA;
                break;
            case FN_BLOCKSIZE: {
                unsigned blocksize = get_uint(s, av_log2(s->blocksize));
                if (blocksize > s->blocksize) {
                    avpriv_report_missing_feature(avctx,
                                                  "Increasing block size");
                    return AVERROR_PATCHWELCOME;
                }
                if (!blocksize || blocksize > MAX_BLOCKSIZE) {
                    av_log(avctx, AV_LOG_ERROR, "invalid or unsupported "
                                                "block size: %d\n", blocksize);
                    return AVERROR(EINVAL);
                }
                s->blocksize = blocksize;
                break;
            }
            case FN_QUIT:
                s->got_quit_command = 1;
                break;
            }
            if (cmd == FN_BLOCKSIZE || cmd == FN_QUIT) {
                *got_frame_ptr = 0;
                break;
            }
        } else {
            /* process audio command */
            int residual_size = 0;
            int channel = s->cur_chan;
            int32_t coffset;

            /* get Rice code for residual decoding */
            if (cmd != FN_ZERO) {
                residual_size = get_ur_golomb_shorten(&s->bc, ENERGYSIZE);
                /* This is a hack as version 0 differed in the definition
                 * of get_sr_golomb_shorten(). */
                if (s->version == 0)
                    residual_size--;
            }

            /* calculate sample offset using means from previous blocks */
            if (s->nmean == 0)
                coffset = s->offset[channel][0];
            else {
                int32_t sum = (s->version < 2) ? 0 : s->nmean / 2;
                for (i = 0; i < s->nmean; i++)
                    sum += s->offset[channel][i];
                coffset = sum / s->nmean;
                if (s->version >= 2)
                    coffset >>= FFMIN(1, s->bitshift);
            }

            /* decode samples for this channel */
            if (cmd == FN_ZERO) {
                for (i = 0; i < s->blocksize; i++)
                    s->decoded[channel][i] = 0;
            } else {
                if ((ret = decode_subframe_lpc(s, cmd, channel,
                                               residual_size, coffset)) < 0)
                    return ret;
            }

            /* update means with info from the current block */
            if (s->nmean > 0) {
                int32_t sum = (s->version < 2) ? 0 : s->blocksize / 2;
                for (i = 0; i < s->blocksize; i++)
                    sum += s->decoded[channel][i];

                for (i = 1; i < s->nmean; i++)
                    s->offset[channel][i - 1] = s->offset[channel][i];

                if (s->version < 2)
                    s->offset[channel][s->nmean - 1] = sum / s->blocksize;
                else
                    s->offset[channel][s->nmean - 1] = (sum / s->blocksize) << s->bitshift;
            }

            /* copy wrap samples for use with next block */
            for (i = -s->nwrap; i < 0; i++)
                s->decoded[channel][i] = s->decoded[channel][i + s->blocksize];

            /* shift samples to add in unused zero bits which were removed
             * during encoding */
            fix_bitshift(s, s->decoded[channel]);

            /* if this is the last channel in the block, output the samples */
            s->cur_chan++;
            if (s->cur_chan == s->channels) {
                /* get output buffer */
                frame->nb_samples = s->blocksize;
                if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
                    return ret;
                }
                /* interleave output */
                output_buffer((int16_t **)frame->extended_data, s->channels,
                              s->blocksize, s->decoded);

                *got_frame_ptr = 1;
            }
        }
    }
    if (s->cur_chan < s->channels)
        *got_frame_ptr = 0;

finish_frame:
    s->bitindex = bitstream_tell(&s->bc) - 8 * (bitstream_tell(&s->bc) / 8);
    i           = bitstream_tell(&s->bc) / 8;
    if (i > buf_size) {
        av_log(s->avctx, AV_LOG_ERROR, "overread: %d\n", i - buf_size);
        s->bitstream_size  = 0;
        s->bitstream_index = 0;
        return AVERROR_INVALIDDATA;
    }
    if (s->bitstream_size) {
        s->bitstream_index += i;
        s->bitstream_size  -= i;
        return input_buf_size;
    } else
        return i;
}

static av_cold int shorten_decode_close(AVCodecContext *avctx)
{
    ShortenContext *s = avctx->priv_data;
    int i;

    for (i = 0; i < s->channels; i++) {
        s->decoded[i] = NULL;
        av_freep(&s->decoded_base[i]);
        av_freep(&s->offset[i]);
    }
    av_freep(&s->bitstream);
    av_freep(&s->coeffs);

    return 0;
}

AVCodec ff_shorten_decoder = {
    .name           = "shorten",
    .long_name      = NULL_IF_CONFIG_SMALL("Shorten"),
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_SHORTEN,
    .priv_data_size = sizeof(ShortenContext),
    .init           = shorten_decode_init,
    .close          = shorten_decode_close,
    .decode         = shorten_decode_frame,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_DR1,
    .sample_fmts    = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16P,
                                                      AV_SAMPLE_FMT_NONE },
};

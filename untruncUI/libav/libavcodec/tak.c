/*
 * TAK common code
 * Copyright (c) 2012 Paul B Mahol
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

#include "libavutil/bswap.h"
#include "libavutil/crc.h"
#include "libavutil/intreadwrite.h"

#define BITSTREAM_READER_LE
#include "bitstream.h"
#include "tak.h"

static const uint16_t frame_duration_type_quants[] = {
    3, 4, 6, 8, 4096, 8192, 16384, 512, 1024, 2048,
};

static int tak_get_nb_samples(int sample_rate, enum TAKFrameSizeType type)
{
    int nb_samples, max_nb_samples;

    if (type <= TAK_FST_250ms) {
        nb_samples     = sample_rate * frame_duration_type_quants[type] >>
                         TAK_FRAME_DURATION_QUANT_SHIFT;
        max_nb_samples = 16384;
    } else if (type < FF_ARRAY_ELEMS(frame_duration_type_quants)) {
        nb_samples     = frame_duration_type_quants[type];
        max_nb_samples = sample_rate *
                         frame_duration_type_quants[TAK_FST_250ms] >>
                         TAK_FRAME_DURATION_QUANT_SHIFT;
    } else {
        return AVERROR_INVALIDDATA;
    }

    if (nb_samples <= 0 || nb_samples > max_nb_samples)
        return AVERROR_INVALIDDATA;

    return nb_samples;
}

static int crc_init = 0;
#if CONFIG_SMALL
#define CRC_TABLE_SIZE 257
#else
#define CRC_TABLE_SIZE 1024
#endif
static AVCRC crc_24[CRC_TABLE_SIZE];

av_cold void ff_tak_init_crc(void)
{
    if (!crc_init) {
        av_crc_init(crc_24, 0, 24, 0x864CFBU, sizeof(crc_24));
        crc_init = 1;
    }
}

int ff_tak_check_crc(const uint8_t *buf, unsigned int buf_size)
{
    uint32_t crc, CRC;

    if (buf_size < 4)
        return AVERROR_INVALIDDATA;
    buf_size -= 3;

    CRC = av_bswap32(AV_RL24(buf + buf_size)) >> 8;
    crc = av_crc(crc_24, 0xCE04B7U, buf, buf_size);
    if (CRC != crc)
        return AVERROR_INVALIDDATA;

    return 0;
}

void avpriv_tak_parse_streaminfo(BitstreamContext *bc, TAKStreamInfo *s)
{
    uint64_t channel_mask = 0;
    int frame_type, i;

    s->codec = bitstream_read(bc, TAK_ENCODER_CODEC_BITS);
    bitstream_skip(bc, TAK_ENCODER_PROFILE_BITS);

    frame_type = bitstream_read(bc, TAK_SIZE_FRAME_DURATION_BITS);
    s->samples = bitstream_read_63(bc, TAK_SIZE_SAMPLES_NUM_BITS);

    s->data_type   = bitstream_read(bc, TAK_FORMAT_DATA_TYPE_BITS);
    s->sample_rate = bitstream_read(bc, TAK_FORMAT_SAMPLE_RATE_BITS) +
                     TAK_SAMPLE_RATE_MIN;
    s->bps         = bitstream_read(bc, TAK_FORMAT_BPS_BITS) +
                     TAK_BPS_MIN;
    s->channels    = bitstream_read(bc, TAK_FORMAT_CHANNEL_BITS) +
                     TAK_CHANNELS_MIN;

    if (bitstream_read_bit(bc)) {
        bitstream_skip(bc, TAK_FORMAT_VALID_BITS);
        if (bitstream_read_bit(bc)) {
            for (i = 0; i < s->channels; i++) {
                int value = bitstream_read(bc, TAK_FORMAT_CH_LAYOUT_BITS);

                if (value > 0 && value <= 18)
                    channel_mask |= 1 << (value - 1);
            }
        }
    }

    s->ch_layout     = channel_mask;
    s->frame_samples = tak_get_nb_samples(s->sample_rate, frame_type);
}

int ff_tak_decode_frame_header(AVCodecContext *avctx, BitstreamContext *bc,
                               TAKStreamInfo *ti, int log_level_offset)
{
    if (bitstream_read(bc, TAK_FRAME_HEADER_SYNC_ID_BITS) != TAK_FRAME_HEADER_SYNC_ID) {
        av_log(avctx, AV_LOG_ERROR + log_level_offset, "missing sync id\n");
        return AVERROR_INVALIDDATA;
    }

    ti->flags     = bitstream_read(bc, TAK_FRAME_HEADER_FLAGS_BITS);
    ti->frame_num = bitstream_read(bc, TAK_FRAME_HEADER_NO_BITS);

    if (ti->flags & TAK_FRAME_FLAG_IS_LAST) {
        ti->last_frame_samples = bitstream_read(bc, TAK_FRAME_HEADER_SAMPLE_COUNT_BITS) + 1;
        bitstream_skip(bc, 2);
    } else {
        ti->last_frame_samples = 0;
    }

    if (ti->flags & TAK_FRAME_FLAG_HAS_INFO) {
        avpriv_tak_parse_streaminfo(bc, ti);

        if (bitstream_read(bc, 6))
            bitstream_skip(bc, 25);
        bitstream_align(bc);
    }

    bitstream_skip(bc, 24);

    return 0;
}

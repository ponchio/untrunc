/*
 *    Copyright (C) 2005  Matthieu CASTET
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

#include <stdlib.h>

#include "libavcodec/bitstream.h"
#include "libavcodec/flac.h"

#include "avformat.h"
#include "internal.h"
#include "oggdec.h"

#define OGG_FLAC_METADATA_TYPE_STREAMINFO 0x7F

static int
flac_header (AVFormatContext * s, int idx)
{
    struct ogg *ogg = s->priv_data;
    struct ogg_stream *os = ogg->streams + idx;
    AVStream *st = s->streams[idx];
    BitstreamContext bc;
    int mdt;

    if (os->buf[os->pstart] == 0xff)
        return 0;

    bitstream_init8(&bc, os->buf + os->pstart, os->psize);
    bitstream_skip(&bc, 1); /* metadata_last */
    mdt = bitstream_read(&bc, 7);

    if (mdt == OGG_FLAC_METADATA_TYPE_STREAMINFO) {
        uint8_t *streaminfo_start = os->buf + os->pstart + 5 + 4 + 4 + 4;
        uint32_t samplerate;

        bitstream_skip(&bc, 4 * 8); /* "FLAC" */
        if (bitstream_read(&bc, 8) != 1) /* unsupported major version */
            return -1;
        bitstream_skip(&bc, 8 + 16); /* minor version + header count */
        bitstream_skip(&bc, 4 * 8); /* "fLaC" */

        /* METADATA_BLOCK_HEADER */
        if (bitstream_read(&bc, 32) != FLAC_STREAMINFO_SIZE)
            return -1;

        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id = AV_CODEC_ID_FLAC;
        st->need_parsing = AVSTREAM_PARSE_HEADERS;

        st->codecpar->extradata =
            av_malloc(FLAC_STREAMINFO_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(st->codecpar->extradata, streaminfo_start, FLAC_STREAMINFO_SIZE);
        st->codecpar->extradata_size = FLAC_STREAMINFO_SIZE;

        samplerate = AV_RB24(st->codecpar->extradata + 10) >> 4;
        if (!samplerate)
            return AVERROR_INVALIDDATA;

        avpriv_set_pts_info(st, 64, 1, samplerate);
    } else if (mdt == FLAC_METADATA_TYPE_VORBIS_COMMENT) {
        ff_vorbis_stream_comment(s, st, os->buf + os->pstart + 4, os->psize - 4);
    }

    return 1;
}

static int
old_flac_header (AVFormatContext * s, int idx)
{
    AVStream *st = s->streams[idx];
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = AV_CODEC_ID_FLAC;

    return 0;
}

const struct ogg_codec ff_flac_codec = {
    .magic = "\177FLAC",
    .magicsize = 5,
    .header = flac_header,
    .nb_header = 2,
};

const struct ogg_codec ff_old_flac_codec = {
    .magic = "fLaC",
    .magicsize = 4,
    .header = old_flac_header,
    .nb_header = 0,
};

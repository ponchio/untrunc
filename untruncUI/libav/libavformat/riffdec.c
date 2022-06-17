/*
 * RIFF demuxing functions and data
 * Copyright (c) 2000 Fabrice Bellard
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

#include "libavutil/dict.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mathematics.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/bytestream.h"
#include "avformat.h"
#include "avio_internal.h"
#include "riff.h"

const AVCodecGuid ff_codec_wav_guids[] = {
    { AV_CODEC_ID_AC3,      { 0x2C, 0x80, 0x6D, 0xE0, 0x46, 0xDB, 0xCF, 0x11, 0xB4, 0xD1, 0x00, 0x80, 0x5F, 0x6C, 0xBB, 0xEA } },
    { AV_CODEC_ID_ATRAC3P,  { 0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44, 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62 } },
    { AV_CODEC_ID_EAC3,     { 0xAF, 0x87, 0xFB, 0xA7, 0x02, 0x2D, 0xFB, 0x42, 0xA4, 0xD4, 0x05, 0xCD, 0x93, 0x84, 0x3B, 0xDD } },
    { AV_CODEC_ID_MP2,      { 0x2B, 0x80, 0x6D, 0xE0, 0x46, 0xDB, 0xCF, 0x11, 0xB4, 0xD1, 0x00, 0x80, 0x5F, 0x6C, 0xBB, 0xEA } },
    { AV_CODEC_ID_NONE }
};

enum AVCodecID ff_codec_guid_get_id(const AVCodecGuid *guids, ff_asf_guid guid)
{
    int i;
    for (i = 0; guids[i].id != AV_CODEC_ID_NONE; i++)
        if (!ff_guidcmp(guids[i].guid, guid))
            return guids[i].id;
    return AV_CODEC_ID_NONE;
}

/* We could be given one of the three possible structures here:
 * WAVEFORMAT, PCMWAVEFORMAT or WAVEFORMATEX. Each structure
 * is an expansion of the previous one with the fields added
 * at the bottom. PCMWAVEFORMAT adds 'WORD wBitsPerSample' and
 * WAVEFORMATEX adds 'WORD  cbSize' and basically makes itself
 * an openended structure.
 */

static void parse_waveformatex(AVIOContext *pb, AVCodecParameters *par)
{
    ff_asf_guid subformat;
    int bps;

    bps = avio_rl16(pb);
    if (bps)
        par->bits_per_coded_sample = bps;
    par->channel_layout        = avio_rl32(pb); /* dwChannelMask */

    ff_get_guid(pb, &subformat);
    if (!memcmp(subformat + 4,
                (const uint8_t[]){ FF_MEDIASUBTYPE_BASE_GUID }, 12)) {
        par->codec_tag = AV_RL32(subformat);
        par->codec_id  = ff_wav_codec_get_id(par->codec_tag,
                                             par->bits_per_coded_sample);
    } else {
        par->codec_id = ff_codec_guid_get_id(ff_codec_wav_guids, subformat);
        if (!par->codec_id)
            av_log(pb, AV_LOG_WARNING,
                   "unknown subformat:"FF_PRI_GUID"\n",
                   FF_ARG_GUID(subformat));
    }
}

int ff_get_wav_header(AVFormatContext *s, AVIOContext *pb,
                      AVCodecParameters *par, int size)
{
    int id;
    uint64_t bitrate;

    if (size < 14)
        return AVERROR_INVALIDDATA;

    id                 = avio_rl16(pb);
    par->codec_type    = AVMEDIA_TYPE_AUDIO;
    par->channels      = avio_rl16(pb);
    par->sample_rate   = avio_rl32(pb);
    bitrate            = avio_rl32(pb) * 8;
    par->block_align   = avio_rl16(pb);
    if (size == 14) {  /* We're dealing with plain vanilla WAVEFORMAT */
        par->bits_per_coded_sample = 8;
    } else
        par->bits_per_coded_sample = avio_rl16(pb);
    if (id == 0xFFFE) {
        par->codec_tag = 0;
    } else {
        par->codec_tag = id;
        par->codec_id  = ff_wav_codec_get_id(id, par->bits_per_coded_sample);
    }
    if (size >= 18) {  /* We're obviously dealing with WAVEFORMATEX */
        int cbSize = avio_rl16(pb); /* cbSize */
        size  -= 18;
        cbSize = FFMIN(size, cbSize);
        if (cbSize >= 22 && id == 0xfffe) { /* WAVEFORMATEXTENSIBLE */
            parse_waveformatex(pb, par);
            cbSize -= 22;
            size   -= 22;
        }
        par->extradata_size = cbSize;
        if (cbSize > 0) {
            av_free(par->extradata);
            par->extradata = av_mallocz(par->extradata_size +
                                        AV_INPUT_BUFFER_PADDING_SIZE);
            if (!par->extradata)
                return AVERROR(ENOMEM);
            avio_read(pb, par->extradata, par->extradata_size);
            size -= cbSize;
        }

        /* It is possible for the chunk to contain garbage at the end */
        if (size > 0)
            avio_skip(pb, size);
    }

    if (bitrate > INT_MAX) {
        if (s->error_recognition & AV_EF_EXPLODE) {
            av_log(s, AV_LOG_ERROR,
                   "The bitrate %"PRIu64" is too large.\n",
                    bitrate);
            return AVERROR_INVALIDDATA;
        } else {
            av_log(s, AV_LOG_WARNING,
                   "The bitrate %"PRIu64" is too large, resetting to 0.",
                   bitrate);
            par->bit_rate = 0;
        }
    } else {
        par->bit_rate = bitrate;
    }

    if (par->sample_rate <= 0) {
        av_log(s, AV_LOG_ERROR,
               "Invalid sample rate: %d\n", par->sample_rate);
        return AVERROR_INVALIDDATA;
    }
    if (par->codec_id == AV_CODEC_ID_AAC_LATM) {
        /* Channels and sample_rate values are those prior to applying SBR
         * and/or PS. */
        par->channels    = 0;
        par->sample_rate = 0;
    }
    /* override bits_per_coded_sample for G.726 */
    if (par->codec_id == AV_CODEC_ID_ADPCM_G726)
        par->bits_per_coded_sample = par->bit_rate / par->sample_rate;

    return 0;
}

enum AVCodecID ff_wav_codec_get_id(unsigned int tag, int bps)
{
    enum AVCodecID id;
    id = ff_codec_get_id(ff_codec_wav_tags, tag);
    if (id <= 0)
        return id;

    if (id == AV_CODEC_ID_PCM_S16LE)
        id = ff_get_pcm_codec_id(bps, 0, 0, ~1);
    else if (id == AV_CODEC_ID_PCM_F32LE)
        id = ff_get_pcm_codec_id(bps, 1, 0,  0);

    if (id == AV_CODEC_ID_ADPCM_IMA_WAV && bps == 8)
        id = AV_CODEC_ID_PCM_ZORK;
    return id;
}

int ff_get_bmp_header(AVIOContext *pb, AVStream *st, uint32_t *size)
{
    int tag1;
    uint32_t size_ = avio_rl32(pb);
    if (size)
        *size = size_;
    st->codecpar->width  = avio_rl32(pb);
    st->codecpar->height = (int32_t)avio_rl32(pb);
    avio_rl16(pb); /* planes */
    st->codecpar->bits_per_coded_sample = avio_rl16(pb); /* depth */
    tag1                                = avio_rl32(pb);
    avio_rl32(pb); /* ImageSize */
    avio_rl32(pb); /* XPelsPerMeter */
    avio_rl32(pb); /* YPelsPerMeter */
    avio_rl32(pb); /* ClrUsed */
    avio_rl32(pb); /* ClrImportant */
    return tag1;
}

int ff_read_riff_info(AVFormatContext *s, int64_t size)
{
    int64_t start, end, cur;
    AVIOContext *pb = s->pb;

    start = avio_tell(pb);
    end   = start + size;

    while ((cur = avio_tell(pb)) >= 0 &&
           cur <= end - 8 /* = tag + size */) {
        uint32_t chunk_code;
        int64_t chunk_size;
        char key[5] = { 0 };
        char *value;

        chunk_code = avio_rl32(pb);
        chunk_size = avio_rl32(pb);

        if (chunk_size > end ||
            end - chunk_size < cur ||
            chunk_size == UINT_MAX) {
            av_log(s, AV_LOG_WARNING, "too big INFO subchunk\n");
            break;
        }

        chunk_size += (chunk_size & 1);

        if (!chunk_code) {
            if (chunk_size)
                avio_skip(pb, chunk_size);
            else if (pb->eof_reached) {
                av_log(s, AV_LOG_WARNING, "truncated file\n");
                return AVERROR_EOF;
            }
            continue;
        }

        value = av_malloc(chunk_size + 1);
        if (!value) {
            av_log(s, AV_LOG_ERROR,
                   "out of memory, unable to read INFO tag\n");
            return AVERROR(ENOMEM);
        }

        AV_WL32(key, chunk_code);

        if (avio_read(pb, value, chunk_size) != chunk_size) {
            av_free(value);
            av_log(s, AV_LOG_WARNING,
                   "premature end of file while reading INFO tag\n");
            break;
        }

        value[chunk_size] = 0;

        av_dict_set(&s->metadata, key, value, AV_DICT_DONT_STRDUP_VAL);
    }

    return 0;
}

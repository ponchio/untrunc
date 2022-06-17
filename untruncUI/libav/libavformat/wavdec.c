/*
 * WAV demuxer
 * Copyright (c) 2001, 2002 Fabrice Bellard
 *
 * Sony Wave64 demuxer
 * RF64 demuxer
 * Copyright (c) 2009 Daniel Verkamp
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

#include <stdint.h>

#include "libavutil/avassert.h"
#include "libavutil/dict.h"
#include "libavutil/log.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "avformat.h"
#include "avio.h"
#include "avio_internal.h"
#include "internal.h"
#include "metadata.h"
#include "pcm.h"
#include "riff.h"

typedef struct WAVDemuxContext {
    int64_t data_end;
    int w64;
} WAVDemuxContext;

#if CONFIG_WAV_DEMUXER

static int64_t next_tag(AVIOContext *pb, uint32_t *tag)
{
    *tag = avio_rl32(pb);
    return avio_rl32(pb);
}

/* RIFF chunks are always on a even offset. */
static int64_t wav_seek_tag(AVIOContext *s, int64_t offset, int whence)
{
    return avio_seek(s, offset + (offset & 1), whence);
}

/* return the size of the found tag */
static int64_t find_tag(AVIOContext *pb, uint32_t tag1)
{
    unsigned int tag;
    int64_t size;

    for (;;) {
        if (pb->eof_reached)
            return -1;
        size = next_tag(pb, &tag);
        if (tag == tag1)
            break;
        wav_seek_tag(pb, size, SEEK_CUR);
    }
    return size;
}

static int wav_probe(AVProbeData *p)
{
    /* check file header */
    if (p->buf_size <= 32)
        return 0;
    if (!memcmp(p->buf + 8, "WAVE", 4)) {
        if (!memcmp(p->buf, "RIFF", 4))
            /* Since the ACT demuxer has a standard WAV header at the top of
             * its own, the returned score is decreased to avoid a probe
             * conflict between ACT and WAV. */
            return AVPROBE_SCORE_MAX - 1;
        else if (!memcmp(p->buf,      "RF64", 4) &&
                 !memcmp(p->buf + 12, "ds64", 4))
            return AVPROBE_SCORE_MAX;
    }
    return 0;
}

static int wav_parse_fmt_tag(AVFormatContext *s, int64_t size, AVStream **st)
{
    AVIOContext *pb = s->pb;
    int ret;

    /* parse fmt header */
    *st = avformat_new_stream(s, NULL);
    if (!*st)
        return AVERROR(ENOMEM);

    ret = ff_get_wav_header(s, pb, (*st)->codecpar, size);
    if (ret < 0)
        return ret;
    (*st)->need_parsing = AVSTREAM_PARSE_FULL;

    avpriv_set_pts_info(*st, 64, 1, (*st)->codecpar->sample_rate);

    return 0;
}

static inline int wav_parse_bext_string(AVFormatContext *s, const char *key,
                                        int length)
{
    char temp[257];
    int ret;

    av_assert0(length <= sizeof(temp));
    if ((ret = avio_read(s->pb, temp, length)) < 0)
        return ret;

    temp[length] = 0;

    if (strlen(temp))
        return av_dict_set(&s->metadata, key, temp, 0);

    return 0;
}

static int wav_parse_bext_tag(AVFormatContext *s, int64_t size)
{
    char temp[131], *coding_history;
    int ret, x;
    uint64_t time_reference;
    int64_t umid_parts[8], umid_mask = 0;

    if ((ret = wav_parse_bext_string(s, "description", 256)) < 0 ||
        (ret = wav_parse_bext_string(s, "originator", 32)) < 0 ||
        (ret = wav_parse_bext_string(s, "originator_reference", 32)) < 0 ||
        (ret = wav_parse_bext_string(s, "origination_date", 10)) < 0 ||
        (ret = wav_parse_bext_string(s, "origination_time", 8)) < 0)
        return ret;

    time_reference = avio_rl64(s->pb);
    snprintf(temp, sizeof(temp), "%"PRIu64, time_reference);
    if ((ret = av_dict_set(&s->metadata, "time_reference", temp, 0)) < 0)
        return ret;

    /* check if version is >= 1, in which case an UMID may be present */
    if (avio_rl16(s->pb) >= 1) {
        for (x = 0; x < 8; x++)
            umid_mask |= umid_parts[x] = avio_rb64(s->pb);

        if (umid_mask) {
            /* the string formatting below is per SMPTE 330M-2004 Annex C */
            if (umid_parts[4] == 0 && umid_parts[5] == 0 &&
                umid_parts[6] == 0 && umid_parts[7] == 0) {
                /* basic UMID */
                snprintf(temp, sizeof(temp),
                         "0x%016"PRIX64"%016"PRIX64"%016"PRIX64"%016"PRIX64,
                         umid_parts[0], umid_parts[1],
                         umid_parts[2], umid_parts[3]);
            } else {
                /* extended UMID */
                snprintf(temp, sizeof(temp),
                         "0x%016"PRIX64"%016"PRIX64"%016"PRIX64"%016"PRIX64
                         "0x%016"PRIX64"%016"PRIX64"%016"PRIX64"%016"PRIX64,
                         umid_parts[0], umid_parts[1],
                         umid_parts[2], umid_parts[3],
                         umid_parts[4], umid_parts[5],
                         umid_parts[6], umid_parts[7]);
            }

            if ((ret = av_dict_set(&s->metadata, "umid", temp, 0)) < 0)
                return ret;
        }

        avio_skip(s->pb, 190);
    } else
        avio_skip(s->pb, 254);

    if (size > 602) {
        /* CodingHistory present */
        size -= 602;

        if (!(coding_history = av_malloc(size + 1)))
            return AVERROR(ENOMEM);

        if ((ret = avio_read(s->pb, coding_history, size)) < 0)
            return ret;

        coding_history[size] = 0;
        if ((ret = av_dict_set(&s->metadata, "coding_history", coding_history,
                               AV_DICT_DONT_STRDUP_VAL)) < 0)
            return ret;
    }

    return 0;
}

static const AVMetadataConv wav_metadata_conv[] = {
    { "description",      "comment"       },
    { "originator",       "encoded_by"    },
    { "origination_date", "date"          },
    { "origination_time", "creation_time" },
    { 0 },
};

/* wav input */
static int wav_read_header(AVFormatContext *s)
{
    int64_t size, av_uninit(data_size);
    int64_t sample_count = 0;
    int rf64;
    uint32_t tag;
    AVIOContext *pb      = s->pb;
    AVStream *st         = NULL;
    WAVDemuxContext *wav = s->priv_data;
    int ret, got_fmt = 0;
    int64_t next_tag_ofs, data_ofs = -1;

    /* check RIFF header */
    tag = avio_rl32(pb);

    rf64 = tag == MKTAG('R', 'F', '6', '4');
    if (!rf64 && tag != MKTAG('R', 'I', 'F', 'F'))
        return AVERROR_INVALIDDATA;
    avio_rl32(pb); /* file size */
    tag = avio_rl32(pb);
    if (tag != MKTAG('W', 'A', 'V', 'E'))
        return AVERROR_INVALIDDATA;

    if (rf64) {
        if (avio_rl32(pb) != MKTAG('d', 's', '6', '4'))
            return AVERROR_INVALIDDATA;
        size = avio_rl32(pb);
        if (size < 16)
            return AVERROR_INVALIDDATA;
        avio_rl64(pb); /* RIFF size */

        data_size    = avio_rl64(pb);
        sample_count = avio_rl64(pb);

        if (data_size < 0 || sample_count < 0) {
            av_log(s, AV_LOG_ERROR, "negative data_size and/or sample_count in "
                   "ds64: data_size = %"PRId64", sample_count = %"PRId64"\n",
                   data_size, sample_count);
            return AVERROR_INVALIDDATA;
        }
        avio_skip(pb, size - 16); /* skip rest of ds64 chunk */
    }

    for (;;) {
        size         = next_tag(pb, &tag);
        next_tag_ofs = avio_tell(pb) + size;

        if (pb->eof_reached)
            break;

        switch (tag) {
        case MKTAG('f', 'm', 't', ' '):
            /* only parse the first 'fmt ' tag found */
            if (!got_fmt && (ret = wav_parse_fmt_tag(s, size, &st) < 0)) {
                return ret;
            } else if (got_fmt)
                av_log(s, AV_LOG_WARNING, "found more than one 'fmt ' tag\n");

            got_fmt = 1;
            break;
        case MKTAG('d', 'a', 't', 'a'):
            if (!got_fmt) {
                av_log(s, AV_LOG_ERROR,
                       "found no 'fmt ' tag before the 'data' tag\n");
                return AVERROR_INVALIDDATA;
            }

            if (rf64) {
                next_tag_ofs = wav->data_end = avio_tell(pb) + data_size;
            } else {
                data_size    = size;
                next_tag_ofs = wav->data_end = size ? next_tag_ofs : INT64_MAX;
            }

            data_ofs = avio_tell(pb);

            /* don't look for footer metadata if we can't seek or if we don't
             * know where the data tag ends
             */
            if (!(pb->seekable & AVIO_SEEKABLE_NORMAL) || (!rf64 && !size))
                goto break_loop;
            break;
        case MKTAG('f', 'a', 'c', 't'):
            if (!sample_count)
                sample_count = avio_rl32(pb);
            break;
        case MKTAG('b', 'e', 'x', 't'):
            if ((ret = wav_parse_bext_tag(s, size)) < 0)
                return ret;
            break;
        case MKTAG('L', 'I', 'S', 'T'):
            if (size < 4) {
                av_log(s, AV_LOG_ERROR, "too short LIST");
                return AVERROR_INVALIDDATA;
            }
            switch (avio_rl32(pb)) {
            case MKTAG('I', 'N', 'F', 'O'):
                if ((ret = ff_read_riff_info(s, size - 4)) < 0)
                    return ret;
            }
            break;
        }

        /* seek to next tag unless we know that we'll run into EOF */
        if ((avio_size(pb) > 0 && next_tag_ofs >= avio_size(pb)) ||
            wav_seek_tag(pb, next_tag_ofs, SEEK_SET) < 0) {
            break;
        }
    }

break_loop:
    if (data_ofs < 0) {
        av_log(s, AV_LOG_ERROR, "no 'data' tag found\n");
        return AVERROR_INVALIDDATA;
    }

    avio_seek(pb, data_ofs, SEEK_SET);

    if (!sample_count && st->codecpar->channels &&
        av_get_bits_per_sample(st->codecpar->codec_id))
        sample_count = (data_size << 3) /
                       (st->codecpar->channels *
                        (uint64_t)av_get_bits_per_sample(st->codecpar->codec_id));
    if (sample_count)
        st->duration = sample_count;

    ff_metadata_conv_ctx(s, NULL, wav_metadata_conv);
    ff_metadata_conv_ctx(s, NULL, ff_riff_info_conv);

    return 0;
}

/**
 * Find chunk with w64 GUID by skipping over other chunks.
 * @return the size of the found chunk
 */
static int64_t find_guid(AVIOContext *pb, const uint8_t guid1[16])
{
    uint8_t guid[16];
    int64_t size;

    while (!pb->eof_reached) {
        avio_read(pb, guid, 16);
        size = avio_rl64(pb);
        if (size <= 24)
            return -1;
        if (!memcmp(guid, guid1, 16))
            return size;
        avio_skip(pb, FFALIGN(size, INT64_C(8)) - 24);
    }
    return -1;
}

static const uint8_t guid_data[16] = {
    'd',  'a',  't',  'a',
    0xF3, 0xAC, 0xD3, 0x11,0x8C,  0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A
};

#define MAX_SIZE 4096

static int wav_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret, size;
    int64_t left;
    AVStream *st;
    WAVDemuxContext *wav = s->priv_data;

    st = s->streams[0];

    left = wav->data_end - avio_tell(s->pb);
    if (left <= 0) {
        if (CONFIG_W64_DEMUXER && wav->w64)
            left = find_guid(s->pb, guid_data) - 24;
        else
            left = find_tag(s->pb, MKTAG('d', 'a', 't', 'a'));
        if (left < 0)
            return AVERROR_EOF;
        wav->data_end = avio_tell(s->pb) + left;
    }

    size = MAX_SIZE;
    if (st->codecpar->block_align > 1) {
        if (size < st->codecpar->block_align)
            size = st->codecpar->block_align;
        size = (size / st->codecpar->block_align) * st->codecpar->block_align;
    }
    size = FFMIN(size, left);
    ret  = av_get_packet(s->pb, pkt, size);
    if (ret < 0)
        return ret;
    pkt->stream_index = 0;

    return ret;
}

static int wav_read_seek(AVFormatContext *s,
                         int stream_index, int64_t timestamp, int flags)
{
    AVStream *st;

    st = s->streams[0];
    switch (st->codecpar->codec_id) {
    case AV_CODEC_ID_MP2:
    case AV_CODEC_ID_MP3:
    case AV_CODEC_ID_AC3:
    case AV_CODEC_ID_DTS:
        /* use generic seeking with dynamically generated indexes */
        return -1;
    default:
        break;
    }
    return ff_pcm_read_seek(s, stream_index, timestamp, flags);
}

AVInputFormat ff_wav_demuxer = {
    .name           = "wav",
    .long_name      = NULL_IF_CONFIG_SMALL("WAV / WAVE (Waveform Audio)"),
    .priv_data_size = sizeof(WAVDemuxContext),
    .read_probe     = wav_probe,
    .read_header    = wav_read_header,
    .read_packet    = wav_read_packet,
    .read_seek      = wav_read_seek,
    .flags          = AVFMT_GENERIC_INDEX,
    .codec_tag      = (const AVCodecTag * const []) { ff_codec_wav_tags,  0 },
};
#endif /* CONFIG_WAV_DEMUXER */

#if CONFIG_W64_DEMUXER
static const uint8_t guid_riff[16] = {
    'r',  'i',  'f',  'f',
    0x2E, 0x91, 0xCF, 0x11,0xA5,  0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00
};

static const uint8_t guid_wave[16] = {
    'w',  'a',  'v',  'e',
    0xF3, 0xAC, 0xD3, 0x11,0x8C,  0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A
};

static const uint8_t guid_fmt[16] = {
    'f',  'm',  't',  ' ',
    0xF3, 0xAC, 0xD3, 0x11,0x8C,  0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A
};

static int w64_probe(AVProbeData *p)
{
    if (p->buf_size <= 40)
        return 0;
    if (!memcmp(p->buf,      guid_riff, 16) &&
        !memcmp(p->buf + 24, guid_wave, 16))
        return AVPROBE_SCORE_MAX;
    else
        return 0;
}

static int w64_read_header(AVFormatContext *s)
{
    int64_t size;
    AVIOContext *pb      = s->pb;
    WAVDemuxContext *wav = s->priv_data;
    AVStream *st;
    uint8_t guid[16];
    int ret;

    avio_read(pb, guid, 16);
    if (memcmp(guid, guid_riff, 16))
        return AVERROR_INVALIDDATA;

    /* riff + wave + fmt + sizes */
    if (avio_rl64(pb) < 16 + 8 + 16 + 8 + 16 + 8)
        return AVERROR_INVALIDDATA;

    avio_read(pb, guid, 16);
    if (memcmp(guid, guid_wave, 16)) {
        av_log(s, AV_LOG_ERROR, "could not find wave guid\n");
        return AVERROR_INVALIDDATA;
    }

    size = find_guid(pb, guid_fmt);
    if (size < 0) {
        av_log(s, AV_LOG_ERROR, "could not find fmt guid\n");
        return AVERROR_INVALIDDATA;
    }

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    /* subtract chunk header size - normal wav file doesn't count it */
    ret = ff_get_wav_header(s, pb, st->codecpar, size - 24);
    if (ret < 0)
        return ret;
    avio_skip(pb, FFALIGN(size, INT64_C(8)) - size);

    st->need_parsing = AVSTREAM_PARSE_FULL;

    avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);

    size = find_guid(pb, guid_data);
    if (size < 0) {
        av_log(s, AV_LOG_ERROR, "could not find data guid\n");
        return AVERROR_INVALIDDATA;
    }
    wav->data_end = avio_tell(pb) + size - 24;
    wav->w64      = 1;

    return 0;
}

AVInputFormat ff_w64_demuxer = {
    .name           = "w64",
    .long_name      = NULL_IF_CONFIG_SMALL("Sony Wave64"),
    .priv_data_size = sizeof(WAVDemuxContext),
    .read_probe     = w64_probe,
    .read_header    = w64_read_header,
    .read_packet    = wav_read_packet,
    .read_seek      = wav_read_seek,
    .flags          = AVFMT_GENERIC_INDEX,
    .codec_tag      = (const AVCodecTag * const []) { ff_codec_wav_tags, 0 },
};
#endif /* CONFIG_W64_DEMUXER */

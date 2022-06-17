/*
 * AIFF/AIFF-C demuxer
 * Copyright (c) 2006  Patrick Guimond
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

#include "libavutil/mathematics.h"
#include "libavutil/dict.h"
#include "avformat.h"
#include "internal.h"
#include "pcm.h"
#include "aiff.h"

#define AIFF                    0
#define AIFF_C_VERSION1         0xA2805140

typedef struct AIFFInputContext {
    int64_t data_end;
    int block_duration;
} AIFFInputContext;

static enum AVCodecID aiff_codec_get_id(int bps)
{
    if (bps <= 8)
        return AV_CODEC_ID_PCM_S8;
    if (bps <= 16)
        return AV_CODEC_ID_PCM_S16BE;
    if (bps <= 24)
        return AV_CODEC_ID_PCM_S24BE;
    if (bps <= 32)
        return AV_CODEC_ID_PCM_S32BE;

    /* bigger than 32 isn't allowed  */
    return AV_CODEC_ID_NONE;
}

/* returns the size of the found tag */
static int get_tag(AVIOContext *pb, uint32_t * tag)
{
    int size;

    if (pb->eof_reached)
        return AVERROR(EIO);

    *tag = avio_rl32(pb);
    size = avio_rb32(pb);

    if (size < 0)
        size = 0x7fffffff;

    return size;
}

/* Metadata string read */
static void get_meta(AVFormatContext *s, const char *key, int size)
{
    uint8_t *str = av_malloc(size+1);
    int res;

    if (!str) {
        avio_skip(s->pb, size);
        return;
    }

    res = avio_read(s->pb, str, size);
    if (res < 0)
        return;

    str[res] = 0;
    av_dict_set(&s->metadata, key, str, AV_DICT_DONT_STRDUP_VAL);
}

/* Returns the number of sound data frames or negative on error */
static unsigned int get_aiff_header(AVFormatContext *s, int size,
                                    unsigned version)
{
    AVIOContext *pb        = s->pb;
    AVCodecParameters *par = s->streams[0]->codecpar;
    AIFFInputContext *aiff = s->priv_data;
    int exp;
    uint64_t val;
    double sample_rate;
    unsigned int num_frames;

    if (size & 1)
        size++;
    par->codec_type = AVMEDIA_TYPE_AUDIO;
    par->channels = avio_rb16(pb);
    num_frames = avio_rb32(pb);
    par->bits_per_coded_sample = avio_rb16(pb);

    exp = avio_rb16(pb);
    val = avio_rb64(pb);
    sample_rate = ldexp(val, exp - 16383 - 63);
    par->sample_rate = sample_rate;
    size -= 18;

    /* get codec id for AIFF-C */
    if (version == AIFF_C_VERSION1) {
        par->codec_tag = avio_rl32(pb);
        par->codec_id  = ff_codec_get_id(ff_codec_aiff_tags, par->codec_tag);
        size -= 4;
    }

    if (version != AIFF_C_VERSION1 || par->codec_id == AV_CODEC_ID_PCM_S16BE) {
        par->codec_id = aiff_codec_get_id(par->bits_per_coded_sample);
        par->bits_per_coded_sample = av_get_bits_per_sample(par->codec_id);
        aiff->block_duration = 1;
    } else {
        switch (par->codec_id) {
        case AV_CODEC_ID_PCM_F32BE:
        case AV_CODEC_ID_PCM_F64BE:
        case AV_CODEC_ID_PCM_S16LE:
        case AV_CODEC_ID_PCM_ALAW:
        case AV_CODEC_ID_PCM_MULAW:
            aiff->block_duration = 1;
            break;
        case AV_CODEC_ID_ADPCM_IMA_QT:
            par->block_align = 34 * par->channels;
            break;
        case AV_CODEC_ID_MACE3:
            par->block_align = 2 * par->channels;
            break;
        case AV_CODEC_ID_ADPCM_G722:
        case AV_CODEC_ID_MACE6:
            par->block_align = 1 * par->channels;
            break;
        case AV_CODEC_ID_GSM:
            par->block_align = 33;
            break;
        case AV_CODEC_ID_QCELP:
            par->block_align = 35;
            break;
        default:
            break;
        }
        if (par->block_align > 0)
            aiff->block_duration = av_get_audio_frame_duration2(par,
                                                                par->block_align);
    }

    /* Block align needs to be computed in all cases, as the definition
     * is specific to applications -> here we use the WAVE format definition */
    if (!par->block_align)
        par->block_align = (par->bits_per_coded_sample * par->channels) >> 3;

    if (aiff->block_duration) {
        par->bit_rate = par->sample_rate * (par->block_align << 3) /
                        aiff->block_duration;
    }

    /* Chunk is over */
    if (size)
        avio_skip(pb, size);

    return num_frames;
}

static int aiff_probe(AVProbeData *p)
{
    /* check file header */
    if (p->buf[0] == 'F' && p->buf[1] == 'O' &&
        p->buf[2] == 'R' && p->buf[3] == 'M' &&
        p->buf[8] == 'A' && p->buf[9] == 'I' &&
        p->buf[10] == 'F' && (p->buf[11] == 'F' || p->buf[11] == 'C'))
        return AVPROBE_SCORE_MAX;
    else
        return 0;
}

/* aiff input */
static int aiff_read_header(AVFormatContext *s)
{
    int size, filesize;
    int64_t offset = 0;
    uint32_t tag;
    unsigned version = AIFF_C_VERSION1;
    AVIOContext *pb = s->pb;
    AVStream * st;
    AIFFInputContext *aiff = s->priv_data;

    /* check FORM header */
    filesize = get_tag(pb, &tag);
    if (filesize < 0 || tag != MKTAG('F', 'O', 'R', 'M'))
        return AVERROR_INVALIDDATA;

    /* AIFF data type */
    tag = avio_rl32(pb);
    if (tag == MKTAG('A', 'I', 'F', 'F'))       /* Got an AIFF file */
        version = AIFF;
    else if (tag != MKTAG('A', 'I', 'F', 'C'))  /* An AIFF-C file then */
        return AVERROR_INVALIDDATA;

    filesize -= 4;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    while (filesize > 0) {
        /* parse different chunks */
        size = get_tag(pb, &tag);
        if (size < 0)
            return size;

        filesize -= size + 8;

        switch (tag) {
        case MKTAG('C', 'O', 'M', 'M'):     /* Common chunk */
            /* Then for the complete header info */
            st->nb_frames = get_aiff_header(s, size, version);
            if (st->nb_frames < 0)
                return st->nb_frames;
            if (offset > 0) // COMM is after SSND
                goto got_sound;
            break;
        case MKTAG('F', 'V', 'E', 'R'):     /* Version chunk */
            version = avio_rb32(pb);
            break;
        case MKTAG('N', 'A', 'M', 'E'):     /* Sample name chunk */
            get_meta(s, "title"    , size);
            break;
        case MKTAG('A', 'U', 'T', 'H'):     /* Author chunk */
            get_meta(s, "author"   , size);
            break;
        case MKTAG('(', 'c', ')', ' '):     /* Copyright chunk */
            get_meta(s, "copyright", size);
            break;
        case MKTAG('A', 'N', 'N', 'O'):     /* Annotation chunk */
            get_meta(s, "comment"  , size);
            break;
        case MKTAG('S', 'S', 'N', 'D'):     /* Sampled sound chunk */
            aiff->data_end = avio_tell(pb) + size;
            offset = avio_rb32(pb);      /* Offset of sound data */
            avio_rb32(pb);               /* BlockSize... don't care */
            offset += avio_tell(pb);    /* Compute absolute data offset */
            if (st->codecpar->block_align)    /* Assume COMM already parsed */
                goto got_sound;
            if (!(pb->seekable & AVIO_SEEKABLE_NORMAL)) {
                av_log(s, AV_LOG_ERROR, "file is not seekable\n");
                return -1;
            }
            avio_skip(pb, size - 8);
            break;
        case MKTAG('w', 'a', 'v', 'e'):
            if ((uint64_t)size > (1<<30))
                return -1;
            st->codecpar->extradata = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!st->codecpar->extradata)
                return AVERROR(ENOMEM);
            st->codecpar->extradata_size = size;
            avio_read(pb, st->codecpar->extradata, size);
            break;
        default: /* Jump */
            avio_skip(pb, size);
        }

        /* Skip required padding byte for odd-sized chunks. */
        if (size & 1) {
            filesize--;
            avio_skip(pb, 1);
        }
    }

got_sound:
    if (!st->codecpar->block_align) {
        av_log(s, AV_LOG_ERROR, "could not find COMM tag or invalid block_align value\n");
        return -1;
    }

    /* Now positioned, get the sound data start and end */
    avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);
    st->start_time = 0;
    st->duration = st->nb_frames * aiff->block_duration;

    /* Position the stream at the first block */
    avio_seek(pb, offset, SEEK_SET);

    return 0;
}

#define MAX_SIZE 4096

static int aiff_read_packet(AVFormatContext *s,
                            AVPacket *pkt)
{
    AVStream *st = s->streams[0];
    AIFFInputContext *aiff = s->priv_data;
    int64_t max_size;
    int res, size;

    /* calculate size of remaining data */
    max_size = aiff->data_end - avio_tell(s->pb);
    if (max_size <= 0)
        return AVERROR_EOF;

    /* Now for that packet */
    if (st->codecpar->block_align >= 33) // GSM, QCLP, IMA4
        size = st->codecpar->block_align;
    else
        size = (MAX_SIZE / st->codecpar->block_align) * st->codecpar->block_align;
    size = FFMIN(max_size, size);
    res = av_get_packet(s->pb, pkt, size);
    if (res < 0)
        return res;

    /* Only one stream in an AIFF file */
    pkt->stream_index = 0;
    pkt->duration     = (res / st->codecpar->block_align) * aiff->block_duration;
    return 0;
}

AVInputFormat ff_aiff_demuxer = {
    .name           = "aiff",
    .long_name      = NULL_IF_CONFIG_SMALL("Audio IFF"),
    .priv_data_size = sizeof(AIFFInputContext),
    .read_probe     = aiff_probe,
    .read_header    = aiff_read_header,
    .read_packet    = aiff_read_packet,
    .read_seek      = ff_pcm_read_seek,
    .codec_tag      = (const AVCodecTag* const []){ ff_codec_aiff_tags, 0 },
};

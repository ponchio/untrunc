/*
 * "NUT" Container Format demuxer
 * Copyright (c) 2004-2006 Michael Niedermayer
 * Copyright (c) 2003 Alex Beregszaszi
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

#include "libavutil/avstring.h"
#include "libavutil/bswap.h"
#include "libavutil/dict.h"
#include "libavutil/mathematics.h"
#include "libavutil/tree.h"
#include "avio_internal.h"
#include "nut.h"
#include "riff.h"

#undef NDEBUG
#include <assert.h>

#define NUT_MAX_STREAMS 256    /* arbitrary sanity check value */

static int get_str(AVIOContext *bc, char *string, unsigned int maxlen)
{
    unsigned int len = ffio_read_varlen(bc);

    if (len && maxlen)
        avio_read(bc, string, FFMIN(len, maxlen));
    while (len > maxlen) {
        avio_r8(bc);
        len--;
    }

    if (maxlen)
        string[FFMIN(len, maxlen - 1)] = 0;

    if (maxlen == len)
        return -1;
    else
        return 0;
}

static int64_t get_s(AVIOContext *bc)
{
    int64_t v = ffio_read_varlen(bc) + 1;

    if (v & 1)
        return -(v >> 1);
    else
        return  (v >> 1);
}

static uint64_t get_fourcc(AVIOContext *bc)
{
    unsigned int len = ffio_read_varlen(bc);

    if (len == 2)
        return avio_rl16(bc);
    else if (len == 4)
        return avio_rl32(bc);
    else
        return -1;
}

static int get_packetheader(NUTContext *nut, AVIOContext *bc,
                            int calculate_checksum, uint64_t startcode)
{
    int64_t size;

    startcode = av_be2ne64(startcode);
    startcode = ff_crc04C11DB7_update(0, (uint8_t*) &startcode, 8);

    ffio_init_checksum(bc, ff_crc04C11DB7_update, startcode);
    size = ffio_read_varlen(bc);
    if (size > 4096)
        avio_rb32(bc);
    if (ffio_get_checksum(bc) && size > 4096)
        return -1;

    ffio_init_checksum(bc, calculate_checksum ? ff_crc04C11DB7_update : NULL, 0);

    return size;
}

static uint64_t find_any_startcode(AVIOContext *bc, int64_t pos)
{
    uint64_t state = 0;

    if (pos >= 0)
        /* Note, this may fail if the stream is not seekable, but that should
         * not matter, as in this case we simply start where we currently are */
        avio_seek(bc, pos, SEEK_SET);
    while (!bc->eof_reached) {
        state = (state << 8) | avio_r8(bc);
        if ((state >> 56) != 'N')
            continue;
        switch (state) {
        case MAIN_STARTCODE:
        case STREAM_STARTCODE:
        case SYNCPOINT_STARTCODE:
        case INFO_STARTCODE:
        case INDEX_STARTCODE:
            return state;
        }
    }

    return 0;
}

/**
 * Find the given startcode.
 * @param code the startcode
 * @param pos the start position of the search, or -1 if the current position
 * @return the position of the startcode or -1 if not found
 */
static int64_t find_startcode(AVIOContext *bc, uint64_t code, int64_t pos)
{
    for (;;) {
        uint64_t startcode = find_any_startcode(bc, pos);
        if (startcode == code)
            return avio_tell(bc) - 8;
        else if (startcode == 0)
            return -1;
        pos = -1;
    }
}

static int nut_probe(AVProbeData *p)
{
    int i;
    uint64_t code = 0;

    for (i = 0; i < p->buf_size; i++) {
        code = (code << 8) | p->buf[i];
        if (code == MAIN_STARTCODE)
            return AVPROBE_SCORE_MAX;
    }
    return 0;
}

#define GET_V(dst, check)                                                     \
    do {                                                                      \
        tmp = ffio_read_varlen(bc);                                           \
        if (!(check)) {                                                       \
            av_log(s, AV_LOG_ERROR, "Error " #dst " is (%"PRId64")\n", tmp);  \
            return AVERROR_INVALIDDATA;                                       \
        }                                                                     \
        dst = tmp;                                                            \
    } while (0)

static int skip_reserved(AVIOContext *bc, int64_t pos)
{
    pos -= avio_tell(bc);
    if (pos < 0) {
        avio_seek(bc, pos, SEEK_CUR);
        return AVERROR_INVALIDDATA;
    } else {
        while (pos--)
            avio_r8(bc);
        return 0;
    }
}

static int decode_main_header(NUTContext *nut)
{
    AVFormatContext *s = nut->avf;
    AVIOContext *bc    = s->pb;
    uint64_t tmp, end;
    unsigned int stream_count;
    int i, j, count;
    int tmp_stream, tmp_mul, tmp_pts, tmp_size, tmp_res, tmp_head_idx;

    end  = get_packetheader(nut, bc, 1, MAIN_STARTCODE);
    end += avio_tell(bc);

    nut->version = ffio_read_varlen(bc);
    if (nut->version < NUT_MIN_VERSION &&
        nut->version > NUT_MAX_VERSION) {
        av_log(s, AV_LOG_ERROR, "Version %d not supported.\n",
               nut->version);
        return AVERROR(ENOSYS);
    }

    GET_V(stream_count, tmp > 0 && tmp <= NUT_MAX_STREAMS);

    nut->max_distance = ffio_read_varlen(bc);
    if (nut->max_distance > 65536) {
        av_log(s, AV_LOG_DEBUG, "max_distance %d\n", nut->max_distance);
        nut->max_distance = 65536;
    }

    GET_V(nut->time_base_count, tmp > 0 && tmp < INT_MAX / sizeof(AVRational));
    nut->time_base = av_malloc(nut->time_base_count * sizeof(AVRational));
    if (!nut->time_base)
        return AVERROR(ENOMEM);

    for (i = 0; i < nut->time_base_count; i++) {
        GET_V(nut->time_base[i].num, tmp > 0 && tmp < (1ULL << 31));
        GET_V(nut->time_base[i].den, tmp > 0 && tmp < (1ULL << 31));
        if (av_gcd(nut->time_base[i].num, nut->time_base[i].den) != 1) {
            av_log(s, AV_LOG_ERROR, "invalid time base %d/%d\n",
                   nut->time_base[i].num,
                   nut->time_base[i].den);
            return AVERROR_INVALIDDATA;
        }
    }
    tmp_pts      = 0;
    tmp_mul      = 1;
    tmp_stream   = 0;
    tmp_head_idx = 0;
    for (i = 0; i < 256;) {
        int tmp_flags  = ffio_read_varlen(bc);
        int tmp_fields = ffio_read_varlen(bc);

        if (tmp_fields > 0)
            tmp_pts = get_s(bc);
        if (tmp_fields > 1)
            tmp_mul = ffio_read_varlen(bc);
        if (tmp_fields > 2)
            tmp_stream = ffio_read_varlen(bc);
        if (tmp_fields > 3)
            tmp_size = ffio_read_varlen(bc);
        else
            tmp_size = 0;
        if (tmp_fields > 4)
            tmp_res = ffio_read_varlen(bc);
        else
            tmp_res = 0;
        if (tmp_fields > 5)
            count = ffio_read_varlen(bc);
        else
            count = tmp_mul - tmp_size;
        if (tmp_fields > 6)
            get_s(bc);
        if (tmp_fields > 7)
            tmp_head_idx = ffio_read_varlen(bc);

        while (tmp_fields-- > 8)
            ffio_read_varlen(bc);

        if (count == 0 || i + count > 256) {
            av_log(s, AV_LOG_ERROR, "illegal count %d at %d\n", count, i);
            return AVERROR_INVALIDDATA;
        }
        if (tmp_stream >= stream_count) {
            av_log(s, AV_LOG_ERROR, "illegal stream number %d >= %d\n",
                   tmp_stream, stream_count);
            return AVERROR_INVALIDDATA;
        }

        for (j = 0; j < count; j++, i++) {
            if (i == 'N') {
                nut->frame_code[i].flags = FLAG_INVALID;
                j--;
                continue;
            }
            nut->frame_code[i].flags          = tmp_flags;
            nut->frame_code[i].pts_delta      = tmp_pts;
            nut->frame_code[i].stream_id      = tmp_stream;
            nut->frame_code[i].size_mul       = tmp_mul;
            nut->frame_code[i].size_lsb       = tmp_size + j;
            nut->frame_code[i].reserved_count = tmp_res;
            nut->frame_code[i].header_idx     = tmp_head_idx;
        }
    }
    assert(nut->frame_code['N'].flags == FLAG_INVALID);

    if (end > avio_tell(bc) + 4) {
        int rem = 1024;
        GET_V(nut->header_count, tmp < 128U);
        nut->header_count++;
        for (i = 1; i < nut->header_count; i++) {
            uint8_t *hdr;
            GET_V(nut->header_len[i], tmp > 0 && tmp < 256);
            if (rem < nut->header_len[i]) {
                av_log(s, AV_LOG_ERROR,
                       "invalid elision header %d : %d > %d\n",
                       i, nut->header_len[i], rem);
                return AVERROR_INVALIDDATA;
            }
            rem -= nut->header_len[i];
            hdr = av_malloc(nut->header_len[i]);
            if (!hdr)
                return AVERROR(ENOMEM);
            avio_read(bc, hdr, nut->header_len[i]);
            nut->header[i] = hdr;
        }
        assert(nut->header_len[0] == 0);
    }

    // flags had been effectively introduced in version 4
    if (nut->version > NUT_STABLE_VERSION) {
        nut->flags = ffio_read_varlen(bc);
    }

    if (skip_reserved(bc, end) || ffio_get_checksum(bc)) {
        av_log(s, AV_LOG_ERROR, "main header checksum mismatch\n");
        return AVERROR_INVALIDDATA;
    }

    nut->stream = av_mallocz(sizeof(StreamContext) * stream_count);
    if (!nut->stream)
        return AVERROR(ENOMEM);
    for (i = 0; i < stream_count; i++)
        avformat_new_stream(s, NULL);

    return 0;
}

static int decode_stream_header(NUTContext *nut)
{
    AVFormatContext *s = nut->avf;
    AVIOContext *bc    = s->pb;
    StreamContext *stc;
    int class, stream_id;
    uint64_t tmp, end;
    AVStream *st;

    end  = get_packetheader(nut, bc, 1, STREAM_STARTCODE);
    end += avio_tell(bc);

    GET_V(stream_id, tmp < s->nb_streams && !nut->stream[tmp].time_base);
    stc = &nut->stream[stream_id];
    st  = s->streams[stream_id];
    if (!st)
        return AVERROR(ENOMEM);

    class                = ffio_read_varlen(bc);
    tmp                  = get_fourcc(bc);
    st->codecpar->codec_tag = tmp;
    switch (class) {
    case 0:
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id   = av_codec_get_id((const AVCodecTag * const []) {
                                                    ff_nut_video_tags,
                                                    ff_codec_bmp_tags,
                                                    0
                                                },
                                                tmp);
        break;
    case 1:
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id   = av_codec_get_id((const AVCodecTag * const []) {
                                                    ff_nut_audio_tags,
                                                    ff_codec_wav_tags,
                                                    0
                                                },
                                                tmp);
        break;
    case 2:
        st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
        st->codecpar->codec_id   = ff_codec_get_id(ff_nut_subtitle_tags, tmp);
        break;
    case 3:
        st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
        st->codecpar->codec_id   = ff_codec_get_id(ff_nut_data_tags, tmp);
        break;
    default:
        av_log(s, AV_LOG_ERROR, "unknown stream class (%d)\n", class);
        return AVERROR(ENOSYS);
    }
    if (class < 3 && st->codecpar->codec_id == AV_CODEC_ID_NONE)
        av_log(s, AV_LOG_ERROR,
               "Unknown codec tag '0x%04x' for stream number %d\n",
               (unsigned int) tmp, stream_id);

    GET_V(stc->time_base_id, tmp < nut->time_base_count);
    GET_V(stc->msb_pts_shift, tmp < 16);
    stc->max_pts_distance = ffio_read_varlen(bc);
    GET_V(stc->decode_delay, tmp < 1000); // sanity limit, raise this if Moore's law is true
    ffio_read_varlen(bc); // stream flags

    GET_V(st->codecpar->extradata_size, tmp < (1 << 30));
    if (st->codecpar->extradata_size) {
        st->codecpar->extradata = av_mallocz(st->codecpar->extradata_size +
                                             AV_INPUT_BUFFER_PADDING_SIZE);
        if (!st->codecpar->extradata)
            return AVERROR(ENOMEM);
        avio_read(bc, st->codecpar->extradata, st->codecpar->extradata_size);
    }

    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        GET_V(st->codecpar->width,  tmp > 0);
        GET_V(st->codecpar->height, tmp > 0);
        st->sample_aspect_ratio.num = ffio_read_varlen(bc);
        st->sample_aspect_ratio.den = ffio_read_varlen(bc);
        if ((!st->sample_aspect_ratio.num) != (!st->sample_aspect_ratio.den)) {
            av_log(s, AV_LOG_ERROR, "invalid aspect ratio %d/%d\n",
                   st->sample_aspect_ratio.num, st->sample_aspect_ratio.den);
            return AVERROR_INVALIDDATA;
        }
        ffio_read_varlen(bc); /* csp type */
    } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        GET_V(st->codecpar->sample_rate, tmp > 0);
        ffio_read_varlen(bc); // samplerate_den
        GET_V(st->codecpar->channels, tmp > 0);
    }
    if (skip_reserved(bc, end) || ffio_get_checksum(bc)) {
        av_log(s, AV_LOG_ERROR,
               "stream header %d checksum mismatch\n", stream_id);
        return AVERROR_INVALIDDATA;
    }
    stc->time_base = &nut->time_base[stc->time_base_id];
    avpriv_set_pts_info(s->streams[stream_id], 63, stc->time_base->num,
                        stc->time_base->den);
    return 0;
}

static void set_disposition_bits(AVFormatContext *avf, char *value,
                                 int stream_id)
{
    int flag = 0, i;

    for (i = 0; ff_nut_dispositions[i].flag; ++i)
        if (!strcmp(ff_nut_dispositions[i].str, value))
            flag = ff_nut_dispositions[i].flag;
    if (!flag)
        av_log(avf, AV_LOG_INFO, "unknown disposition type '%s'\n", value);
    for (i = 0; i < avf->nb_streams; ++i)
        if (stream_id == i || stream_id == -1)
            avf->streams[i]->disposition |= flag;
}

static int decode_info_header(NUTContext *nut)
{
    AVFormatContext *s = nut->avf;
    AVIOContext *bc    = s->pb;
    uint64_t tmp, chapter_start, chapter_len;
    unsigned int stream_id_plus1, count;
    int chapter_id, i;
    int64_t value, end;
    char name[256], str_value[1024], type_str[256];
    const char *type;
    int *event_flags        = NULL;
    AVChapter *chapter      = NULL;
    AVStream *st            = NULL;
    AVDictionary **metadata = NULL;
    int metadata_flag       = 0;

    end  = get_packetheader(nut, bc, 1, INFO_STARTCODE);
    end += avio_tell(bc);

    GET_V(stream_id_plus1, tmp <= s->nb_streams);
    chapter_id    = get_s(bc);
    chapter_start = ffio_read_varlen(bc);
    chapter_len   = ffio_read_varlen(bc);
    count         = ffio_read_varlen(bc);

    if (chapter_id && !stream_id_plus1) {
        int64_t start = chapter_start / nut->time_base_count;
        chapter = avpriv_new_chapter(s, chapter_id,
                                     nut->time_base[chapter_start %
                                                    nut->time_base_count],
                                     start, start + chapter_len, NULL);
        if (!chapter) {
            av_log(s, AV_LOG_ERROR, "Could not create chapter.\n");
            return AVERROR(ENOMEM);
        }
        metadata = &chapter->metadata;
    } else if (stream_id_plus1) {
        st       = s->streams[stream_id_plus1 - 1];
        metadata = &st->metadata;
        event_flags = &st->event_flags;
        metadata_flag = AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
    } else {
        metadata = &s->metadata;
        event_flags = &s->event_flags;
        metadata_flag = AVFMT_EVENT_FLAG_METADATA_UPDATED;
    }

    for (i = 0; i < count; i++) {
        get_str(bc, name, sizeof(name));
        value = get_s(bc);
        if (value == -1) {
            type = "UTF-8";
            get_str(bc, str_value, sizeof(str_value));
        } else if (value == -2) {
            get_str(bc, type_str, sizeof(type_str));
            type = type_str;
            get_str(bc, str_value, sizeof(str_value));
        } else if (value == -3) {
            type  = "s";
            value = get_s(bc);
        } else if (value == -4) {
            type  = "t";
            value = ffio_read_varlen(bc);
        } else if (value < -4) {
            type = "r";
            get_s(bc);
        } else {
            type = "v";
        }

        if (stream_id_plus1 > s->nb_streams) {
            av_log(s, AV_LOG_WARNING,
                   "invalid stream id %d for info packet\n",
                   stream_id_plus1);
            continue;
        }

        if (!strcmp(type, "UTF-8")) {
            if (chapter_id == 0 && !strcmp(name, "Disposition")) {
                set_disposition_bits(s, str_value, stream_id_plus1 - 1);
                continue;
            }
            if (metadata && av_strcasecmp(name, "Uses") &&
                av_strcasecmp(name, "Depends") && av_strcasecmp(name, "Replaces")) {
                if (event_flags)
                    *event_flags |= metadata_flag;
                av_dict_set(metadata, name, str_value, 0);
            }
        }
    }

    if (skip_reserved(bc, end) || ffio_get_checksum(bc)) {
        av_log(s, AV_LOG_ERROR, "info header checksum mismatch\n");
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

static int decode_syncpoint(NUTContext *nut, int64_t *ts, int64_t *back_ptr)
{
    AVFormatContext *s = nut->avf;
    AVIOContext *bc    = s->pb;
    int64_t end, tmp;
    int ret;

    nut->last_syncpoint_pos = avio_tell(bc) - 8;

    end  = get_packetheader(nut, bc, 1, SYNCPOINT_STARTCODE);
    end += avio_tell(bc);

    tmp       = ffio_read_varlen(bc);
    *back_ptr = nut->last_syncpoint_pos - 16 * ffio_read_varlen(bc);
    if (*back_ptr < 0)
        return -1;

    ff_nut_reset_ts(nut, nut->time_base[tmp % nut->time_base_count],
                    tmp / nut->time_base_count);

    if (nut->flags & NUT_BROADCAST) {
        tmp = ffio_read_varlen(bc);
        av_log(s, AV_LOG_VERBOSE, "Syncpoint wallclock %"PRId64"\n",
               av_rescale_q(tmp / nut->time_base_count,
                            nut->time_base[tmp % nut->time_base_count],
                            AV_TIME_BASE_Q));
    }

    if (skip_reserved(bc, end) || ffio_get_checksum(bc)) {
        av_log(s, AV_LOG_ERROR, "sync point checksum mismatch\n");
        return AVERROR_INVALIDDATA;
    }

    *ts = tmp / s->nb_streams *
          av_q2d(nut->time_base[tmp % s->nb_streams]) * AV_TIME_BASE;

    if ((ret = ff_nut_add_sp(nut, nut->last_syncpoint_pos, *back_ptr, *ts)) < 0)
        return ret;

    return 0;
}

static int find_and_decode_index(NUTContext *nut)
{
    AVFormatContext *s = nut->avf;
    AVIOContext *bc    = s->pb;
    uint64_t tmp, end;
    int i, j, syncpoint_count;
    int64_t filesize = avio_size(bc);
    int64_t *syncpoints;
    int8_t *has_keyframe;
    int ret = AVERROR_INVALIDDATA;

    avio_seek(bc, filesize - 12, SEEK_SET);
    avio_seek(bc, filesize - avio_rb64(bc), SEEK_SET);
    if (avio_rb64(bc) != INDEX_STARTCODE) {
        av_log(s, AV_LOG_WARNING, "no index at the end\n");
        return ret;
    }

    end  = get_packetheader(nut, bc, 1, INDEX_STARTCODE);
    end += avio_tell(bc);

    ffio_read_varlen(bc); // max_pts
    GET_V(syncpoint_count, tmp < INT_MAX / 8 && tmp > 0);
    syncpoints   = av_malloc(sizeof(int64_t) *  syncpoint_count);
    has_keyframe = av_malloc(sizeof(int8_t)  * (syncpoint_count + 1));
    if (!syncpoints || !has_keyframe) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    for (i = 0; i < syncpoint_count; i++) {
        syncpoints[i] = ffio_read_varlen(bc);
        if (syncpoints[i] <= 0)
            goto fail;
        if (i)
            syncpoints[i] += syncpoints[i - 1];
    }

    for (i = 0; i < s->nb_streams; i++) {
        int64_t last_pts = -1;
        for (j = 0; j < syncpoint_count;) {
            uint64_t x = ffio_read_varlen(bc);
            int type   = x & 1;
            int n      = j;
            x >>= 1;
            if (type) {
                int flag = x & 1;
                x >>= 1;
                if (n + x >= syncpoint_count + 1) {
                    av_log(s, AV_LOG_ERROR, "index overflow A\n");
                    goto fail;
                }
                while (x--)
                    has_keyframe[n++] = flag;
                has_keyframe[n++] = !flag;
            } else {
                while (x != 1) {
                    if (n >= syncpoint_count + 1) {
                        av_log(s, AV_LOG_ERROR, "index overflow B\n");
                        goto fail;
                    }
                    has_keyframe[n++] = x & 1;
                    x >>= 1;
                }
            }
            if (has_keyframe[0]) {
                av_log(s, AV_LOG_ERROR, "keyframe before first syncpoint in index\n");
                goto fail;
            }
            assert(n <= syncpoint_count + 1);
            for (; j < n && j < syncpoint_count; j++) {
                if (has_keyframe[j]) {
                    uint64_t B, A = ffio_read_varlen(bc);
                    if (!A) {
                        A = ffio_read_varlen(bc);
                        B = ffio_read_varlen(bc);
                        // eor_pts[j][i] = last_pts + A + B
                    } else
                        B = 0;
                    av_add_index_entry(s->streams[i], 16 * syncpoints[j - 1],
                                       last_pts + A, 0, 0, AVINDEX_KEYFRAME);
                    last_pts += A + B;
                }
            }
        }
    }

    if (skip_reserved(bc, end) || ffio_get_checksum(bc)) {
        av_log(s, AV_LOG_ERROR, "index checksum mismatch\n");
        goto fail;
    }
    ret = 0;

fail:
    av_free(syncpoints);
    av_free(has_keyframe);
    return ret;
}

static int nut_read_close(AVFormatContext *s)
{
    NUTContext *nut = s->priv_data;
    int i;

    av_freep(&nut->time_base);
    av_freep(&nut->stream);
    ff_nut_free_sp(nut);
    for (i = 1; i < nut->header_count; i++)
        av_freep(&nut->header[i]);

    return 0;
}

static int nut_read_header(AVFormatContext *s)
{
    NUTContext *nut = s->priv_data;
    AVIOContext *bc = s->pb;
    int64_t pos;
    int initialized_stream_count;

    nut->avf = s;

    /* main header */
    pos = 0;
    do {
        pos = find_startcode(bc, MAIN_STARTCODE, pos) + 1;
        if (pos < 0 + 1) {
            av_log(s, AV_LOG_ERROR, "No main startcode found.\n");
            goto fail;
        }
    } while (decode_main_header(nut) < 0);

    /* stream headers */
    pos = 0;
    for (initialized_stream_count = 0; initialized_stream_count < s->nb_streams;) {
        pos = find_startcode(bc, STREAM_STARTCODE, pos) + 1;
        if (pos < 0 + 1) {
            av_log(s, AV_LOG_ERROR, "Not all stream headers found.\n");
            goto fail;
        }
        if (decode_stream_header(nut) >= 0)
            initialized_stream_count++;
    }

    /* info headers */
    pos = 0;
    for (;;) {
        uint64_t startcode = find_any_startcode(bc, pos);
        pos = avio_tell(bc);

        if (startcode == 0) {
            av_log(s, AV_LOG_ERROR, "EOF before video frames\n");
            goto fail;
        } else if (startcode == SYNCPOINT_STARTCODE) {
            nut->next_startcode = startcode;
            break;
        } else if (startcode != INFO_STARTCODE) {
            continue;
        }

        decode_info_header(nut);
    }

    s->internal->data_offset = pos - 8;

    if (bc->seekable & AVIO_SEEKABLE_NORMAL) {
        int64_t orig_pos = avio_tell(bc);
        find_and_decode_index(nut);
        avio_seek(bc, orig_pos, SEEK_SET);
    }
    assert(nut->next_startcode == SYNCPOINT_STARTCODE);

    ff_metadata_conv_ctx(s, NULL, ff_nut_metadata_conv);

    return 0;

fail:
    nut_read_close(s);

    return AVERROR_INVALIDDATA;
}

static int decode_frame_header(NUTContext *nut, int64_t *pts, int *stream_id,
                               uint8_t *header_idx, int frame_code)
{
    AVFormatContext *s = nut->avf;
    AVIOContext *bc    = s->pb;
    StreamContext *stc;
    int size, flags, size_mul, pts_delta, i, reserved_count;
    uint64_t tmp;

    if (!(nut->flags & NUT_PIPE) &&
        avio_tell(bc) > nut->last_syncpoint_pos + nut->max_distance) {
        av_log(s, AV_LOG_ERROR,
               "Last frame must have been damaged %"PRId64" > %"PRId64" + %d\n",
               avio_tell(bc), nut->last_syncpoint_pos, nut->max_distance);
        return AVERROR_INVALIDDATA;
    }

    flags          = nut->frame_code[frame_code].flags;
    size_mul       = nut->frame_code[frame_code].size_mul;
    size           = nut->frame_code[frame_code].size_lsb;
    *stream_id     = nut->frame_code[frame_code].stream_id;
    pts_delta      = nut->frame_code[frame_code].pts_delta;
    reserved_count = nut->frame_code[frame_code].reserved_count;
    *header_idx    = nut->frame_code[frame_code].header_idx;

    if (flags & FLAG_INVALID)
        return AVERROR_INVALIDDATA;
    if (flags & FLAG_CODED)
        flags ^= ffio_read_varlen(bc);
    if (flags & FLAG_STREAM_ID) {
        GET_V(*stream_id, tmp < s->nb_streams);
    }
    stc = &nut->stream[*stream_id];
    if (flags & FLAG_CODED_PTS) {
        int coded_pts = ffio_read_varlen(bc);
        // FIXME check last_pts validity?
        if (coded_pts < (1 << stc->msb_pts_shift)) {
            *pts = ff_lsb2full(stc, coded_pts);
        } else
            *pts = coded_pts - (1 << stc->msb_pts_shift);
    } else
        *pts = stc->last_pts + pts_delta;
    if (flags & FLAG_SIZE_MSB)
        size += size_mul * ffio_read_varlen(bc);
    if (flags & FLAG_MATCH_TIME)
        get_s(bc);
    if (flags & FLAG_HEADER_IDX)
        *header_idx = ffio_read_varlen(bc);
    if (flags & FLAG_RESERVED)
        reserved_count = ffio_read_varlen(bc);
    for (i = 0; i < reserved_count; i++)
        ffio_read_varlen(bc);

    if (*header_idx >= (unsigned)nut->header_count) {
        av_log(s, AV_LOG_ERROR, "header_idx invalid\n");
        return AVERROR_INVALIDDATA;
    }
    if (size > 4096)
        *header_idx = 0;
    size -= nut->header_len[*header_idx];

    if (flags & FLAG_CHECKSUM) {
        avio_rb32(bc); // FIXME check this
    } else if (!(nut->flags & NUT_PIPE) &&
               size > 2 * nut->max_distance ||
               FFABS(stc->last_pts - *pts) > stc->max_pts_distance) {
        av_log(s, AV_LOG_ERROR, "frame size > 2max_distance and no checksum\n");
        return AVERROR_INVALIDDATA;
    }

    stc->last_pts   = *pts;
    stc->last_flags = flags;

    return size;
}

static int decode_frame(NUTContext *nut, AVPacket *pkt, int frame_code)
{
    AVFormatContext *s = nut->avf;
    AVIOContext *bc    = s->pb;
    int size, stream_id, discard, ret;
    int64_t pts, last_IP_pts;
    StreamContext *stc;
    uint8_t header_idx;

    size = decode_frame_header(nut, &pts, &stream_id, &header_idx, frame_code);
    if (size < 0)
        return size;

    stc = &nut->stream[stream_id];

    if (stc->last_flags & FLAG_KEY)
        stc->skip_until_key_frame = 0;

    discard     = s->streams[stream_id]->discard;
    last_IP_pts = s->streams[stream_id]->last_IP_pts;
    if ((discard >= AVDISCARD_NONKEY && !(stc->last_flags & FLAG_KEY)) ||
        (discard >= AVDISCARD_BIDIR  && last_IP_pts != AV_NOPTS_VALUE &&
         last_IP_pts > pts) ||
        discard >= AVDISCARD_ALL ||
        stc->skip_until_key_frame) {
        avio_skip(bc, size);
        return 1;
    }

    ret = av_new_packet(pkt, size + nut->header_len[header_idx]);
    if (ret < 0)
        return ret;
    if (nut->header[header_idx])
        memcpy(pkt->data, nut->header[header_idx], nut->header_len[header_idx]);
    pkt->pos = avio_tell(bc); // FIXME
    avio_read(bc, pkt->data + nut->header_len[header_idx], size);

    pkt->stream_index = stream_id;
    if (stc->last_flags & FLAG_KEY)
        pkt->flags |= AV_PKT_FLAG_KEY;
    pkt->pts = pts;

    return 0;
}

static int nut_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    NUTContext *nut = s->priv_data;
    AVIOContext *bc = s->pb;
    int i, frame_code = 0, ret, skip;
    int64_t ts, back_ptr;

    for (;;) {
        int64_t pos  = avio_tell(bc);
        uint64_t tmp = nut->next_startcode;
        nut->next_startcode = 0;

        if (tmp) {
            pos -= 8;
        } else {
            frame_code = avio_r8(bc);
            if (bc->eof_reached)
                return AVERROR_EOF;
            if (frame_code == 'N') {
                tmp = frame_code;
                for (i = 1; i < 8; i++)
                    tmp = (tmp << 8) + avio_r8(bc);
            }
        }
        switch (tmp) {
        case MAIN_STARTCODE:
        case STREAM_STARTCODE:
        case INDEX_STARTCODE:
            skip = get_packetheader(nut, bc, 0, tmp);
            avio_skip(bc, skip);
            break;
        case INFO_STARTCODE:
            if (decode_info_header(nut) < 0)
                goto resync;
            break;
        case SYNCPOINT_STARTCODE:
            if (decode_syncpoint(nut, &ts, &back_ptr) < 0)
                goto resync;
            frame_code = avio_r8(bc);
        case 0:
            ret = decode_frame(nut, pkt, frame_code);
            if (ret == 0)
                return 0;
            else if (ret == 1) // OK but discard packet
                break;
        default:
resync:
            av_log(s, AV_LOG_DEBUG, "syncing from %"PRId64"\n", pos);
            tmp = find_any_startcode(bc, nut->last_syncpoint_pos + 1);
            if (tmp == 0)
                return AVERROR_INVALIDDATA;
            av_log(s, AV_LOG_DEBUG, "sync\n");
            nut->next_startcode = tmp;
        }
    }
}

static int64_t nut_read_timestamp(AVFormatContext *s, int stream_index,
                                  int64_t *pos_arg, int64_t pos_limit)
{
    NUTContext *nut = s->priv_data;
    AVIOContext *bc = s->pb;
    int64_t pos, pts, back_ptr;
    av_log(s, AV_LOG_DEBUG, "read_timestamp(X,%d,%"PRId64",%"PRId64")\n",
           stream_index, *pos_arg, pos_limit);

    pos = *pos_arg;
    do {
        pos = find_startcode(bc, SYNCPOINT_STARTCODE, pos) + 1;
        if (pos < 1) {
            assert(nut->next_startcode == 0);
            av_log(s, AV_LOG_ERROR, "read_timestamp failed.\n");
            return AV_NOPTS_VALUE;
        }
    } while (decode_syncpoint(nut, &pts, &back_ptr) < 0);
    *pos_arg = pos - 1;
    assert(nut->last_syncpoint_pos == *pos_arg);

    av_log(s, AV_LOG_DEBUG, "return %"PRId64" %"PRId64"\n", pts, back_ptr);
    if (stream_index == -1)
        return pts;
    else if (stream_index == -2)
        return back_ptr;

    return AV_NOPTS_VALUE;
}

static int read_seek(AVFormatContext *s, int stream_index,
                     int64_t pts, int flags)
{
    NUTContext *nut    = s->priv_data;
    AVStream *st       = s->streams[stream_index];
    Syncpoint dummy    = { .ts = pts * av_q2d(st->time_base) * AV_TIME_BASE };
    Syncpoint nopts_sp = { .ts = AV_NOPTS_VALUE, .back_ptr = AV_NOPTS_VALUE };
    Syncpoint *sp, *next_node[2] = { &nopts_sp, &nopts_sp };
    int64_t pos, pos2, ts;
    int i;

    if (nut->flags & NUT_PIPE) {
        return AVERROR(ENOSYS);
    }

    if (st->index_entries) {
        int index = av_index_search_timestamp(st, pts, flags);
        if (index < 0)
            return -1;

        pos2 = st->index_entries[index].pos;
        ts   = st->index_entries[index].timestamp;
    } else {
        av_tree_find(nut->syncpoints, &dummy,
                     (int (*)(void *, const void *)) ff_nut_sp_pts_cmp,
                     (void **) next_node);
        av_log(s, AV_LOG_DEBUG, "%"PRIu64"-%"PRIu64" %"PRId64"-%"PRId64"\n",
               next_node[0]->pos, next_node[1]->pos, next_node[0]->ts,
               next_node[1]->ts);
        pos = ff_gen_search(s, -1, dummy.ts, next_node[0]->pos,
                            next_node[1]->pos, next_node[1]->pos,
                            next_node[0]->ts, next_node[1]->ts,
                            AVSEEK_FLAG_BACKWARD, &ts, nut_read_timestamp);

        if (!(flags & AVSEEK_FLAG_BACKWARD)) {
            dummy.pos    = pos + 16;
            next_node[1] = &nopts_sp;
            av_tree_find(nut->syncpoints, &dummy,
                         (int (*)(void *, const void *)) ff_nut_sp_pos_cmp,
                         (void **) next_node);
            pos2 = ff_gen_search(s, -2, dummy.pos, next_node[0]->pos,
                                 next_node[1]->pos, next_node[1]->pos,
                                 next_node[0]->back_ptr, next_node[1]->back_ptr,
                                 flags, &ts, nut_read_timestamp);
            if (pos2 >= 0)
                pos = pos2;
            // FIXME dir but I think it does not matter
        }
        dummy.pos = pos;
        sp = av_tree_find(nut->syncpoints, &dummy,
                          (int (*)(void *, const void *)) ff_nut_sp_pos_cmp,
                          NULL);

        assert(sp);
        pos2 = sp->back_ptr - 15;
    }
    av_log(NULL, AV_LOG_DEBUG, "SEEKTO: %"PRId64"\n", pos2);
    pos = find_startcode(s->pb, SYNCPOINT_STARTCODE, pos2);
    avio_seek(s->pb, pos, SEEK_SET);
    av_log(NULL, AV_LOG_DEBUG, "SP: %"PRId64"\n", pos);
    if (pos2 > pos || pos2 + 15 < pos)
        av_log(NULL, AV_LOG_ERROR, "no syncpoint at backptr pos\n");
    for (i = 0; i < s->nb_streams; i++)
        nut->stream[i].skip_until_key_frame = 1;

    return 0;
}

AVInputFormat ff_nut_demuxer = {
    .name           = "nut",
    .long_name      = NULL_IF_CONFIG_SMALL("NUT"),
    .priv_data_size = sizeof(NUTContext),
    .read_probe     = nut_probe,
    .read_header    = nut_read_header,
    .read_packet    = nut_read_packet,
    .read_close     = nut_read_close,
    .read_seek      = read_seek,
    .extensions     = "nut",
    .codec_tag      = ff_nut_codec_tags,
};

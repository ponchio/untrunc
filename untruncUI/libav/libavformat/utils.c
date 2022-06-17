/*
 * various utility functions for use within Libav
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
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

#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>

#include "config.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/dict.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

#include "libavcodec/bytestream.h"
#include "libavcodec/internal.h"

#include "audiointerleave.h"
#include "avformat.h"
#include "id3v2.h"
#include "internal.h"
#include "metadata.h"
#if CONFIG_NETWORK
#include "network.h"
#endif
#include "riff.h"
#include "url.h"

/**
 * @file
 * various utility functions for use within Libav
 */

unsigned avformat_version(void)
{
    return LIBAVFORMAT_VERSION_INT;
}

const char *avformat_configuration(void)
{
    return LIBAV_CONFIGURATION;
}

const char *avformat_license(void)
{
#define LICENSE_PREFIX "libavformat license: "
    return LICENSE_PREFIX LIBAV_LICENSE + sizeof(LICENSE_PREFIX) - 1;
}

/* an arbitrarily chosen "sane" max packet size -- 50M */
#define SANE_CHUNK_SIZE (50000000)

/* Read the data in sane-sized chunks and append to pkt.
 * Return the number of bytes read or an error. */
static int append_packet_chunked(AVIOContext *s, AVPacket *pkt, int size)
{
    int64_t chunk_size = size;
    int64_t orig_pos   = pkt->pos; // av_grow_packet might reset pos
    int orig_size      = pkt->size;
    int ret = 0;

    do {
        int prev_size = pkt->size;
        int read_size;

        /* When the caller requests a lot of data, limit it to the amount
         * left in file or SANE_CHUNK_SIZE when it is not known. */
        if (size > SANE_CHUNK_SIZE) {
            int64_t filesize = avio_size(s) - avio_tell(s);
            chunk_size = FFMAX(filesize, SANE_CHUNK_SIZE);
        }
        read_size = FFMIN(size, chunk_size);

        ret = av_grow_packet(pkt, read_size);
        if (ret < 0)
            break;

        ret = avio_read(s, pkt->data + prev_size, read_size);
        if (ret != read_size) {
            av_shrink_packet(pkt, prev_size + FFMAX(ret, 0));
            break;
        }

        size -= read_size;
    } while (size > 0);

    pkt->pos = orig_pos;
    if (!pkt->size)
        av_packet_unref(pkt);
    return pkt->size > orig_size ? pkt->size - orig_size : ret;
}

int av_get_packet(AVIOContext *s, AVPacket *pkt, int size)
{
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->pos  = avio_tell(s);

    return append_packet_chunked(s, pkt, size);
}

int av_append_packet(AVIOContext *s, AVPacket *pkt, int size)
{
    if (!pkt->size)
        return av_get_packet(s, pkt, size);
    return append_packet_chunked(s, pkt, size);
}

int av_filename_number_test(const char *filename)
{
    char buf[1024];
    return filename &&
           (av_get_frame_filename(buf, sizeof(buf), filename, 1) >= 0);
}

static int set_codec_from_probe_data(AVFormatContext *s, AVStream *st,
                                     AVProbeData *pd, int score)
{
    static const struct {
        const char *name;
        enum AVCodecID id;
        enum AVMediaType type;
    } fmt_id_type[] = {
        { "aac",       AV_CODEC_ID_AAC,        AVMEDIA_TYPE_AUDIO },
        { "ac3",       AV_CODEC_ID_AC3,        AVMEDIA_TYPE_AUDIO },
        { "dts",       AV_CODEC_ID_DTS,        AVMEDIA_TYPE_AUDIO },
        { "eac3",      AV_CODEC_ID_EAC3,       AVMEDIA_TYPE_AUDIO },
        { "h264",      AV_CODEC_ID_H264,       AVMEDIA_TYPE_VIDEO },
        { "latm",      AV_CODEC_ID_AAC_LATM,   AVMEDIA_TYPE_AUDIO },
        { "m4v",       AV_CODEC_ID_MPEG4,      AVMEDIA_TYPE_VIDEO },
        { "mp3",       AV_CODEC_ID_MP3,        AVMEDIA_TYPE_AUDIO },
        { "mpegvideo", AV_CODEC_ID_MPEG2VIDEO, AVMEDIA_TYPE_VIDEO },
        { 0 }
    };
    AVInputFormat *fmt = av_probe_input_format2(pd, 1, &score);

    if (fmt) {
        int i;
        av_log(s, AV_LOG_DEBUG,
               "Probe with size=%d, packets=%d detected %s with score=%d\n",
               pd->buf_size, MAX_PROBE_PACKETS - st->probe_packets,
               fmt->name, score);
        for (i = 0; fmt_id_type[i].name; i++) {
            if (!strcmp(fmt->name, fmt_id_type[i].name)) {
                st->codecpar->codec_id   = fmt_id_type[i].id;
                st->codecpar->codec_type = fmt_id_type[i].type;
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
                st->codec->codec_type = st->codecpar->codec_type;
                st->codec->codec_id   = st->codecpar->codec_id;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
                break;
            }
        }
    }
    return !!fmt;
}

/************************************************************/
/* input media file */

/* Open input file and probe the format if necessary. */
static int init_input(AVFormatContext *s, const char *filename,
                      AVDictionary **options)
{
    int ret;
    AVProbeData pd = { filename, NULL, 0 };

    if (s->pb) {
        s->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (!s->iformat)
            return av_probe_input_buffer(s->pb, &s->iformat, filename,
                                         s, 0, s->probesize);
        else if (s->iformat->flags & AVFMT_NOFILE)
            return AVERROR(EINVAL);
        return 0;
    }

    if ((s->iformat && s->iformat->flags & AVFMT_NOFILE) ||
        (!s->iformat && (s->iformat = av_probe_input_format(&pd, 0))))
        return 0;

    ret = s->io_open(s, &s->pb, filename, AVIO_FLAG_READ, options);
    if (ret < 0)
        return ret;
    if (s->iformat)
        return 0;
    return av_probe_input_buffer(s->pb, &s->iformat, filename,
                                 s, 0, s->probesize);
}

static int add_to_pktbuf(AVPacketList **packet_buffer, AVPacket *pkt,
                         AVPacketList **plast_pktl, int ref)
{
    AVPacketList *pktl = av_mallocz(sizeof(AVPacketList));
    int ret;

    if (!pktl)
        return AVERROR(ENOMEM);

    if (ref) {
        if ((ret = av_packet_ref(&pktl->pkt, pkt)) < 0) {
            av_free(pktl);
            return ret;
        }
    } else {
        pktl->pkt = *pkt;
    }

    if (*packet_buffer)
        (*plast_pktl)->next = pktl;
    else
        *packet_buffer = pktl;

    /* Add the packet in the buffered packet list. */
    *plast_pktl = pktl;
    return 0;
}

static int queue_attached_pictures(AVFormatContext *s)
{
    int i, ret;
    for (i = 0; i < s->nb_streams; i++)
        if (s->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC &&
            s->streams[i]->discard < AVDISCARD_ALL) {

            ret = add_to_pktbuf(&s->internal->raw_packet_buffer,
                                &s->streams[i]->attached_pic,
                                &s->internal->raw_packet_buffer_end, 1);
            if (ret < 0)
                return ret;
        }
    return 0;
}

#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
static int update_stream_avctx(AVFormatContext *s)
{
    int i, ret;
    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];

        if (!st->internal->need_codec_update)
            continue;

        ret = avcodec_parameters_to_context(st->codec, st->codecpar);
        if (ret < 0)
            return ret;

        st->internal->need_codec_update = 0;
    }
    return 0;
}
FF_ENABLE_DEPRECATION_WARNINGS
#endif

int avformat_open_input(AVFormatContext **ps, const char *filename,
                        AVInputFormat *fmt, AVDictionary **options)
{
    AVFormatContext *s = *ps;
    int i, ret = 0;
    AVDictionary *tmp = NULL;
    ID3v2ExtraMeta *id3v2_extra_meta = NULL;

    if (!s && !(s = avformat_alloc_context()))
        return AVERROR(ENOMEM);
    if (fmt)
        s->iformat = fmt;

    if (options)
        av_dict_copy(&tmp, *options, 0);

    if ((ret = av_opt_set_dict(s, &tmp)) < 0)
        goto fail;

    if ((ret = init_input(s, filename, &tmp)) < 0)
        goto fail;

    /* Check filename in case an image number is expected. */
    if (s->iformat->flags & AVFMT_NEEDNUMBER) {
        if (!av_filename_number_test(filename)) {
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }

    s->duration = s->start_time = AV_NOPTS_VALUE;
    av_strlcpy(s->filename, filename ? filename : "", sizeof(s->filename));

    /* Allocate private data. */
    if (s->iformat->priv_data_size > 0) {
        if (!(s->priv_data = av_mallocz(s->iformat->priv_data_size))) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        if (s->iformat->priv_class) {
            *(const AVClass **) s->priv_data = s->iformat->priv_class;
            av_opt_set_defaults(s->priv_data);
            if ((ret = av_opt_set_dict(s->priv_data, &tmp)) < 0)
                goto fail;
        }
    }

    /* e.g. AVFMT_NOFILE formats will not have a AVIOContext */
    if (s->pb)
        ff_id3v2_read(s, ID3v2_DEFAULT_MAGIC, &id3v2_extra_meta);

    if (s->iformat->read_header)
        if ((ret = s->iformat->read_header(s)) < 0)
            goto fail;

    if (id3v2_extra_meta &&
        (ret = ff_id3v2_parse_apic(s, &id3v2_extra_meta)) < 0)
        goto fail;
    ff_id3v2_free_extra_meta(&id3v2_extra_meta);

    if ((ret = queue_attached_pictures(s)) < 0)
        goto fail;

    if (s->pb && !s->internal->data_offset)
        s->internal->data_offset = avio_tell(s->pb);

    s->internal->raw_packet_buffer_remaining_size = RAW_PACKET_BUFFER_SIZE;

#if FF_API_LAVF_AVCTX
    update_stream_avctx(s);
#endif

    for (i = 0; i < s->nb_streams; i++)
        s->streams[i]->internal->orig_codec_id = s->streams[i]->codecpar->codec_id;

    if (options) {
        av_dict_free(options);
        *options = tmp;
    }
    *ps = s;
    return 0;

fail:
    ff_id3v2_free_extra_meta(&id3v2_extra_meta);
    av_dict_free(&tmp);
    if (s->pb && !(s->flags & AVFMT_FLAG_CUSTOM_IO))
        avio_close(s->pb);
    avformat_free_context(s);
    *ps = NULL;
    return ret;
}

/*******************************************************/

static int probe_codec(AVFormatContext *s, AVStream *st, const AVPacket *pkt)
{
    if (st->codecpar->codec_id == AV_CODEC_ID_PROBE) {
        AVProbeData *pd = &st->probe_data;
        av_log(s, AV_LOG_DEBUG, "probing stream %d\n", st->index);
        --st->probe_packets;

        if (pkt) {
            int err;
            if ((err = av_reallocp(&pd->buf, pd->buf_size + pkt->size +
                                   AVPROBE_PADDING_SIZE)) < 0)
                return err;
            memcpy(pd->buf + pd->buf_size, pkt->data, pkt->size);
            pd->buf_size += pkt->size;
            memset(pd->buf + pd->buf_size, 0, AVPROBE_PADDING_SIZE);
        } else {
            st->probe_packets = 0;
            if (!pd->buf_size) {
                av_log(s, AV_LOG_ERROR,
                       "nothing to probe for stream %d\n", st->index);
                return 0;
            }
        }

        if (!st->probe_packets ||
            av_log2(pd->buf_size) != av_log2(pd->buf_size - pkt->size)) {
            set_codec_from_probe_data(s, st, pd, st->probe_packets > 0
                                                 ? AVPROBE_SCORE_MAX / 4 : 0);
            if (st->codecpar->codec_id != AV_CODEC_ID_PROBE) {
                pd->buf_size = 0;
                av_freep(&pd->buf);
                av_log(s, AV_LOG_DEBUG, "probed stream %d\n", st->index);
            }
        }
    }
    return 0;
}

int ff_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret, i, err;
    AVStream *st;

    for (;;) {
        AVPacketList *pktl = s->internal->raw_packet_buffer;

        if (pktl) {
            *pkt = pktl->pkt;
            st   = s->streams[pkt->stream_index];
            if (st->codecpar->codec_id != AV_CODEC_ID_PROBE ||
                !st->probe_packets ||
                s->internal->raw_packet_buffer_remaining_size < pkt->size) {
                AVProbeData *pd;
                if (st->probe_packets)
                    if ((err = probe_codec(s, st, NULL)) < 0)
                        return err;
                pd = &st->probe_data;
                av_freep(&pd->buf);
                pd->buf_size = 0;
                s->internal->raw_packet_buffer                 = pktl->next;
                s->internal->raw_packet_buffer_remaining_size += pkt->size;
                av_free(pktl);
                return 0;
            }
        }

        pkt->data = NULL;
        pkt->size = 0;
        av_init_packet(pkt);
        ret = s->iformat->read_packet(s, pkt);
        if (ret < 0) {
            if (!pktl || ret == AVERROR(EAGAIN))
                return ret;
            for (i = 0; i < s->nb_streams; i++) {
                st = s->streams[i];
                if (st->probe_packets)
                    if ((err = probe_codec(s, st, NULL)) < 0)
                        return err;
            }
            continue;
        }

        if (!pkt->buf) {
            AVPacket tmp = { 0 };
            ret = av_packet_ref(&tmp, pkt);
            if (ret < 0)
                return ret;
            *pkt = tmp;
        }

        if ((s->flags & AVFMT_FLAG_DISCARD_CORRUPT) &&
            (pkt->flags & AV_PKT_FLAG_CORRUPT)) {
            av_log(s, AV_LOG_WARNING,
                   "Dropped corrupted packet (stream = %d)\n",
                   pkt->stream_index);
            av_packet_unref(pkt);
            continue;
        }

        st = s->streams[pkt->stream_index];

        switch (st->codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            if (s->video_codec_id)
                st->codecpar->codec_id = s->video_codec_id;
            break;
        case AVMEDIA_TYPE_AUDIO:
            if (s->audio_codec_id)
                st->codecpar->codec_id = s->audio_codec_id;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            if (s->subtitle_codec_id)
                st->codecpar->codec_id = s->subtitle_codec_id;
            break;
        }

        if (!pktl && (st->codecpar->codec_id != AV_CODEC_ID_PROBE ||
                      !st->probe_packets))
            return ret;

        err = add_to_pktbuf(&s->internal->raw_packet_buffer, pkt,
                            &s->internal->raw_packet_buffer_end, 0);
        if (err)
            return err;
        s->internal->raw_packet_buffer_remaining_size -= pkt->size;

        if ((err = probe_codec(s, st, pkt)) < 0)
            return err;
    }
}

/**********************************************************/

/**
 * Return the frame duration in seconds. Return 0 if not available.
 */
void ff_compute_frame_duration(AVFormatContext *s, int *pnum, int *pden, AVStream *st,
                               AVCodecParserContext *pc, AVPacket *pkt)
{
    AVRational codec_framerate = s->iformat ? st->internal->avctx->framerate :
                                              (AVRational){ 0, 1 };
    int frame_size;

    *pnum = 0;
    *pden = 0;
    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        if (st->avg_frame_rate.num) {
            *pnum = st->avg_frame_rate.den;
            *pden = st->avg_frame_rate.num;
        } else if (st->time_base.num * 1000LL > st->time_base.den) {
            *pnum = st->time_base.num;
            *pden = st->time_base.den;
        } else if (codec_framerate.den * 1000LL > codec_framerate.num) {
            *pnum = codec_framerate.den;
            *pden = codec_framerate.num;
            if (pc && pc->repeat_pict) {
                if (*pnum > INT_MAX / (1 + pc->repeat_pict))
                    *pden /= 1 + pc->repeat_pict;
                else
                    *pnum *= 1 + pc->repeat_pict;
            }
            /* If this codec can be interlaced or progressive then we need
             * a parser to compute duration of a packet. Thus if we have
             * no parser in such case leave duration undefined. */
            if (st->internal->avctx->ticks_per_frame > 1 && !pc)
                *pnum = *pden = 0;
        }
        break;
    case AVMEDIA_TYPE_AUDIO:
        frame_size = av_get_audio_frame_duration2(st->codecpar, pkt->size);
        if (frame_size <= 0 || st->codecpar->sample_rate <= 0)
            break;
        *pnum = frame_size;
        *pden = st->codecpar->sample_rate;
        break;
    default:
        break;
    }
}

static int is_intra_only(enum AVCodecID id)
{
    const AVCodecDescriptor *d = avcodec_descriptor_get(id);
    if (!d)
        return 0;
    if (d->type == AVMEDIA_TYPE_VIDEO && !(d->props & AV_CODEC_PROP_INTRA_ONLY))
        return 0;
    return 1;
}

static void update_initial_timestamps(AVFormatContext *s, int stream_index,
                                      int64_t dts, int64_t pts)
{
    AVStream *st       = s->streams[stream_index];
    AVPacketList *pktl = s->internal->packet_buffer;

    if (st->first_dts != AV_NOPTS_VALUE ||
        dts           == AV_NOPTS_VALUE ||
        st->cur_dts   == AV_NOPTS_VALUE)
        return;

    st->first_dts = dts - st->cur_dts;
    st->cur_dts   = dts;

    for (; pktl; pktl = pktl->next) {
        if (pktl->pkt.stream_index != stream_index)
            continue;
        // FIXME: think more about this check
        if (pktl->pkt.pts != AV_NOPTS_VALUE && pktl->pkt.pts == pktl->pkt.dts)
            pktl->pkt.pts += st->first_dts;

        if (pktl->pkt.dts != AV_NOPTS_VALUE)
            pktl->pkt.dts += st->first_dts;

        if (st->start_time == AV_NOPTS_VALUE && pktl->pkt.pts != AV_NOPTS_VALUE)
            st->start_time = pktl->pkt.pts;
    }
    if (st->start_time == AV_NOPTS_VALUE)
        st->start_time = pts;
}

static void update_initial_durations(AVFormatContext *s, AVStream *st,
                                     int stream_index, int duration)
{
    AVPacketList *pktl = s->internal->packet_buffer;
    int64_t cur_dts    = 0;

    if (st->first_dts != AV_NOPTS_VALUE) {
        cur_dts = st->first_dts;
        for (; pktl; pktl = pktl->next) {
            if (pktl->pkt.stream_index == stream_index) {
                if (pktl->pkt.pts != pktl->pkt.dts  ||
                    pktl->pkt.dts != AV_NOPTS_VALUE ||
                    pktl->pkt.duration)
                    break;
                cur_dts -= duration;
            }
        }
        pktl          = s->internal->packet_buffer;
        st->first_dts = cur_dts;
    } else if (st->cur_dts)
        return;

    for (; pktl; pktl = pktl->next) {
        if (pktl->pkt.stream_index != stream_index)
            continue;
        if (pktl->pkt.pts == pktl->pkt.dts  &&
            pktl->pkt.dts == AV_NOPTS_VALUE &&
            !pktl->pkt.duration) {
            pktl->pkt.dts = cur_dts;
            if (!st->internal->avctx->has_b_frames)
                pktl->pkt.pts = cur_dts;
            cur_dts += duration;
            if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
                pktl->pkt.duration = duration;
        } else
            break;
    }
    if (st->first_dts == AV_NOPTS_VALUE)
        st->cur_dts = cur_dts;
}

static void compute_pkt_fields(AVFormatContext *s, AVStream *st,
                               AVCodecParserContext *pc, AVPacket *pkt)
{
    int num, den, presentation_delayed, delay, i;
    int64_t offset;

    if (s->flags & AVFMT_FLAG_NOFILLIN)
        return;

    if ((s->flags & AVFMT_FLAG_IGNDTS) && pkt->pts != AV_NOPTS_VALUE)
        pkt->dts = AV_NOPTS_VALUE;

    /* do we have a video B-frame ? */
    delay = st->internal->avctx->has_b_frames;
    presentation_delayed = 0;

    /* XXX: need has_b_frame, but cannot get it if the codec is
     *  not initialized */
    if (delay &&
        pc && pc->pict_type != AV_PICTURE_TYPE_B)
        presentation_delayed = 1;

    if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE &&
        st->pts_wrap_bits < 63 &&
        pkt->dts - (1LL << (st->pts_wrap_bits - 1)) > pkt->pts) {
        pkt->dts -= 1LL << st->pts_wrap_bits;
    }

    /* Some MPEG-2 in MPEG-PS lack dts (issue #171 / input_file.mpg).
     * We take the conservative approach and discard both.
     * Note: If this is misbehaving for an H.264 file, then possibly
     * presentation_delayed is not set correctly. */
    if (delay == 1 && pkt->dts == pkt->pts &&
        pkt->dts != AV_NOPTS_VALUE && presentation_delayed) {
        av_log(s, AV_LOG_DEBUG, "invalid dts/pts combination\n");
        pkt->dts = AV_NOPTS_VALUE;
    }

    if (pkt->duration == 0 && st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        ff_compute_frame_duration(s, &num, &den, st, pc, pkt);
        if (den && num) {
            pkt->duration = av_rescale_rnd(1, num * (int64_t) st->time_base.den,
                                           den * (int64_t) st->time_base.num,
                                           AV_ROUND_DOWN);

            if (pkt->duration != 0 && s->internal->packet_buffer)
                update_initial_durations(s, st, pkt->stream_index,
                                         pkt->duration);
        }
    }

    /* Correct timestamps with byte offset if demuxers only have timestamps
     * on packet boundaries */
    if (pc && st->need_parsing == AVSTREAM_PARSE_TIMESTAMPS && pkt->size) {
        /* this will estimate bitrate based on this frame's duration and size */
        offset = av_rescale(pc->offset, pkt->duration, pkt->size);
        if (pkt->pts != AV_NOPTS_VALUE)
            pkt->pts += offset;
        if (pkt->dts != AV_NOPTS_VALUE)
            pkt->dts += offset;
    }

    /* This may be redundant, but it should not hurt. */
    if (pkt->dts != AV_NOPTS_VALUE &&
        pkt->pts != AV_NOPTS_VALUE &&
        pkt->pts > pkt->dts)
        presentation_delayed = 1;

    av_log(NULL, AV_LOG_TRACE,
            "IN delayed:%d pts:%"PRId64", dts:%"PRId64" "
            "cur_dts:%"PRId64" st:%d pc:%p\n",
            presentation_delayed, pkt->pts, pkt->dts, st->cur_dts,
            pkt->stream_index, pc);
    /* Interpolate PTS and DTS if they are not present. We skip H.264
     * currently because delay and has_b_frames are not reliably set. */
    if ((delay == 0 || (delay == 1 && pc)) &&
        st->codecpar->codec_id != AV_CODEC_ID_H264) {
        if (presentation_delayed) {
            /* DTS = decompression timestamp */
            /* PTS = presentation timestamp */
            if (pkt->dts == AV_NOPTS_VALUE)
                pkt->dts = st->last_IP_pts;
            update_initial_timestamps(s, pkt->stream_index, pkt->dts, pkt->pts);
            if (pkt->dts == AV_NOPTS_VALUE)
                pkt->dts = st->cur_dts;

            /* This is tricky: the dts must be incremented by the duration
             * of the frame we are displaying, i.e. the last I- or P-frame. */
            if (st->last_IP_duration == 0)
                st->last_IP_duration = pkt->duration;
            if (pkt->dts != AV_NOPTS_VALUE)
                st->cur_dts = pkt->dts + st->last_IP_duration;
            st->last_IP_duration = pkt->duration;
            st->last_IP_pts      = pkt->pts;
            /* Cannot compute PTS if not present (we can compute it only
             * by knowing the future. */
        } else if (pkt->pts != AV_NOPTS_VALUE ||
                   pkt->dts != AV_NOPTS_VALUE ||
                   pkt->duration              ||
                   st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            int duration = pkt->duration;
            if (!duration && st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                ff_compute_frame_duration(s, &num, &den, st, pc, pkt);
                if (den && num) {
                    duration = av_rescale_rnd(1,
                                              num * (int64_t) st->time_base.den,
                                              den * (int64_t) st->time_base.num,
                                              AV_ROUND_DOWN);
                    if (duration != 0 && s->internal->packet_buffer)
                        update_initial_durations(s, st, pkt->stream_index,
                                                 duration);
                }
            }

            if (pkt->pts != AV_NOPTS_VALUE || pkt->dts != AV_NOPTS_VALUE ||
                duration) {
                /* presentation is not delayed : PTS and DTS are the same */
                if (pkt->pts == AV_NOPTS_VALUE)
                    pkt->pts = pkt->dts;
                update_initial_timestamps(s, pkt->stream_index, pkt->pts,
                                          pkt->pts);
                if (pkt->pts == AV_NOPTS_VALUE)
                    pkt->pts = st->cur_dts;
                pkt->dts = pkt->pts;
                if (pkt->pts != AV_NOPTS_VALUE)
                    st->cur_dts = pkt->pts + duration;
            }
        }
    }

    if (pkt->pts != AV_NOPTS_VALUE && delay <= MAX_REORDER_DELAY) {
        st->pts_buffer[0] = pkt->pts;
        for (i = 0; i<delay && st->pts_buffer[i] > st->pts_buffer[i + 1]; i++)
            FFSWAP(int64_t, st->pts_buffer[i], st->pts_buffer[i + 1]);
        if (pkt->dts == AV_NOPTS_VALUE)
            pkt->dts = st->pts_buffer[0];
        // We skipped it above so we try here.
        if (st->codecpar->codec_id == AV_CODEC_ID_H264)
            // This should happen on the first packet
            update_initial_timestamps(s, pkt->stream_index, pkt->dts, pkt->pts);
        if (pkt->dts > st->cur_dts)
            st->cur_dts = pkt->dts;
    }

    av_log(NULL, AV_LOG_TRACE,
            "OUTdelayed:%d/%d pts:%"PRId64", dts:%"PRId64" cur_dts:%"PRId64"\n",
            presentation_delayed, delay, pkt->pts, pkt->dts, st->cur_dts);

    /* update flags */
    if (is_intra_only(st->codecpar->codec_id))
        pkt->flags |= AV_PKT_FLAG_KEY;
#if FF_API_CONVERGENCE_DURATION
FF_DISABLE_DEPRECATION_WARNINGS
    if (pc)
        pkt->convergence_duration = pc->convergence_duration;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
}

static void free_packet_buffer(AVPacketList **pkt_buf, AVPacketList **pkt_buf_end)
{
    while (*pkt_buf) {
        AVPacketList *pktl = *pkt_buf;
        *pkt_buf = pktl->next;
        av_packet_unref(&pktl->pkt);
        av_freep(&pktl);
    }
    *pkt_buf_end = NULL;
}

/**
 * Parse a packet, add all split parts to parse_queue.
 *
 * @param pkt Packet to parse, NULL when flushing the parser at end of stream.
 */
static int parse_packet(AVFormatContext *s, AVPacket *pkt, int stream_index)
{
    AVPacket out_pkt = { 0 }, flush_pkt = { 0 };
    AVStream *st = s->streams[stream_index];
    uint8_t *data = pkt ? pkt->data : NULL;
    int size      = pkt ? pkt->size : 0;
    int ret = 0, got_output = 0;

    if (!pkt) {
        av_init_packet(&flush_pkt);
        pkt        = &flush_pkt;
        got_output = 1;
    }

    while (size > 0 || (pkt == &flush_pkt && got_output)) {
        int len;

        av_init_packet(&out_pkt);
        len = av_parser_parse2(st->parser, st->internal->avctx,
                               &out_pkt.data, &out_pkt.size, data, size,
                               pkt->pts, pkt->dts, pkt->pos);

        pkt->pts = pkt->dts = AV_NOPTS_VALUE;
        /* increment read pointer */
        data += len;
        size -= len;

        got_output = !!out_pkt.size;

        if (!out_pkt.size)
            continue;

        if (pkt->side_data) {
            out_pkt.side_data       = pkt->side_data;
            out_pkt.side_data_elems = pkt->side_data_elems;
            pkt->side_data          = NULL;
            pkt->side_data_elems    = 0;
        }

        /* set the duration */
        out_pkt.duration = 0;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (st->internal->avctx->sample_rate > 0) {
                out_pkt.duration =
                    av_rescale_q_rnd(st->parser->duration,
                                     (AVRational) { 1, st->internal->avctx->sample_rate },
                                     st->time_base,
                                     AV_ROUND_DOWN);
            }
        }

        out_pkt.stream_index = st->index;
        out_pkt.pts          = st->parser->pts;
        out_pkt.dts          = st->parser->dts;
        out_pkt.pos          = st->parser->pos;

        if (st->parser->key_frame == 1 ||
            (st->parser->key_frame == -1 &&
             st->parser->pict_type == AV_PICTURE_TYPE_I))
            out_pkt.flags |= AV_PKT_FLAG_KEY;

        compute_pkt_fields(s, st, st->parser, &out_pkt);

        if ((s->iformat->flags & AVFMT_GENERIC_INDEX) &&
            out_pkt.flags & AV_PKT_FLAG_KEY) {
            ff_reduce_index(s, st->index);
            av_add_index_entry(st, st->parser->frame_offset, out_pkt.dts,
                               0, 0, AVINDEX_KEYFRAME);
        }

        if ((ret = add_to_pktbuf(&s->internal->parse_queue, &out_pkt,
                                 &s->internal->parse_queue_end,
                                 1))) {
            av_packet_unref(&out_pkt);
            goto fail;
        }
    }

    /* end of the stream => close and free the parser */
    if (pkt == &flush_pkt) {
        av_parser_close(st->parser);
        st->parser = NULL;
    }

fail:
    av_packet_unref(pkt);
    return ret;
}

static int read_from_packet_buffer(AVPacketList **pkt_buffer,
                                   AVPacketList **pkt_buffer_end,
                                   AVPacket      *pkt)
{
    AVPacketList *pktl;
    av_assert0(*pkt_buffer);
    pktl        = *pkt_buffer;
    *pkt        = pktl->pkt;
    *pkt_buffer = pktl->next;
    if (!pktl->next)
        *pkt_buffer_end = NULL;
    av_freep(&pktl);
    return 0;
}

static int read_frame_internal(AVFormatContext *s, AVPacket *pkt)
{
    int ret = 0, i, got_packet = 0;
    AVDictionary *metadata = NULL;

    av_init_packet(pkt);

    while (!got_packet && !s->internal->parse_queue) {
        AVStream *st;
        AVPacket cur_pkt;

        /* read next packet */
        ret = ff_read_packet(s, &cur_pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN))
                return ret;
            /* flush the parsers */
            for (i = 0; i < s->nb_streams; i++) {
                st = s->streams[i];
                if (st->parser && st->need_parsing)
                    parse_packet(s, NULL, st->index);
            }
            /* all remaining packets are now in parse_queue =>
             * really terminate parsing */
            break;
        }
        ret = 0;
        st  = s->streams[cur_pkt.stream_index];

        if (cur_pkt.pts != AV_NOPTS_VALUE &&
            cur_pkt.dts != AV_NOPTS_VALUE &&
            cur_pkt.pts < cur_pkt.dts) {
            av_log(s, AV_LOG_WARNING,
                   "Invalid timestamps stream=%d, pts=%"PRId64", "
                   "dts=%"PRId64", size=%d\n",
                   cur_pkt.stream_index, cur_pkt.pts,
                   cur_pkt.dts, cur_pkt.size);
        }
        if (s->debug & FF_FDEBUG_TS)
            av_log(s, AV_LOG_DEBUG,
                   "ff_read_packet stream=%d, pts=%"PRId64", dts=%"PRId64", "
                   "size=%d, duration=%"PRId64", flags=%d\n",
                   cur_pkt.stream_index, cur_pkt.pts, cur_pkt.dts,
                   cur_pkt.size, cur_pkt.duration, cur_pkt.flags);

        if (st->need_parsing && !st->parser && !(s->flags & AVFMT_FLAG_NOPARSE)) {
            st->parser = av_parser_init(st->codecpar->codec_id);
            if (!st->parser)
                /* no parser available: just output the raw packets */
                st->need_parsing = AVSTREAM_PARSE_NONE;
            else if (st->need_parsing == AVSTREAM_PARSE_HEADERS)
                st->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
            else if (st->need_parsing == AVSTREAM_PARSE_FULL_ONCE)
                st->parser->flags |= PARSER_FLAG_ONCE;
        }

        if (!st->need_parsing || !st->parser) {
            /* no parsing needed: we just output the packet as is */
            *pkt = cur_pkt;
            compute_pkt_fields(s, st, NULL, pkt);
            if ((s->iformat->flags & AVFMT_GENERIC_INDEX) &&
                (pkt->flags & AV_PKT_FLAG_KEY) && pkt->dts != AV_NOPTS_VALUE) {
                ff_reduce_index(s, st->index);
                av_add_index_entry(st, pkt->pos, pkt->dts,
                                   0, 0, AVINDEX_KEYFRAME);
            }
            got_packet = 1;
        } else if (st->discard < AVDISCARD_ALL) {
            if ((ret = parse_packet(s, &cur_pkt, cur_pkt.stream_index)) < 0)
                return ret;
        } else {
            /* free packet */
            av_packet_unref(&cur_pkt);
        }
    }

    if (!got_packet && s->internal->parse_queue)
        ret = read_from_packet_buffer(&s->internal->parse_queue, &s->internal->parse_queue_end, pkt);

    av_opt_get_dict_val(s, "metadata", AV_OPT_SEARCH_CHILDREN, &metadata);
    if (metadata) {
        s->event_flags |= AVFMT_EVENT_FLAG_METADATA_UPDATED;
        av_dict_copy(&s->metadata, metadata, 0);
        av_dict_free(&metadata);
        av_opt_set_dict_val(s, "metadata", NULL, AV_OPT_SEARCH_CHILDREN);
    }

#if FF_API_LAVF_AVCTX
    update_stream_avctx(s);
#endif

    if (s->debug & FF_FDEBUG_TS)
        av_log(s, AV_LOG_DEBUG,
               "read_frame_internal stream=%d, pts=%"PRId64", dts=%"PRId64", "
               "size=%d, duration=%"PRId64", flags=%d\n",
               pkt->stream_index, pkt->pts, pkt->dts,
               pkt->size, pkt->duration, pkt->flags);

    return ret;
}

int av_read_frame(AVFormatContext *s, AVPacket *pkt)
{
    const int genpts = s->flags & AVFMT_FLAG_GENPTS;
    int eof = 0;

    if (!genpts)
        return s->internal->packet_buffer
               ? read_from_packet_buffer(&s->internal->packet_buffer,
                                         &s->internal->packet_buffer_end, pkt)
               : read_frame_internal(s, pkt);

    for (;;) {
        int ret;
        AVPacketList *pktl = s->internal->packet_buffer;

        if (pktl) {
            AVPacket *next_pkt = &pktl->pkt;

            if (next_pkt->dts != AV_NOPTS_VALUE) {
                int wrap_bits = s->streams[next_pkt->stream_index]->pts_wrap_bits;
                while (pktl && next_pkt->pts == AV_NOPTS_VALUE) {
                    if (pktl->pkt.stream_index == next_pkt->stream_index &&
                        (av_compare_mod(next_pkt->dts, pktl->pkt.dts, 2LL << (wrap_bits - 1)) < 0) &&
                         av_compare_mod(pktl->pkt.pts, pktl->pkt.dts, 2LL << (wrap_bits - 1))) {
                        // not B-frame
                        next_pkt->pts = pktl->pkt.dts;
                    }
                    pktl = pktl->next;
                }
                pktl = s->internal->packet_buffer;
            }

            /* read packet from packet buffer, if there is data */
            if (!(next_pkt->pts == AV_NOPTS_VALUE &&
                  next_pkt->dts != AV_NOPTS_VALUE && !eof))
                return read_from_packet_buffer(&s->internal->packet_buffer,
                                               &s->internal->packet_buffer_end, pkt);
        }

        ret = read_frame_internal(s, pkt);
        if (ret < 0) {
            if (pktl && ret != AVERROR(EAGAIN)) {
                eof = 1;
                continue;
            } else
                return ret;
        }

        ret = add_to_pktbuf(&s->internal->packet_buffer, pkt,
                            &s->internal->packet_buffer_end, 1);
        if (ret < 0)
            return ret;
    }
}

/* XXX: suppress the packet queue */
static void flush_packet_queue(AVFormatContext *s)
{
    free_packet_buffer(&s->internal->parse_queue,       &s->internal->parse_queue_end);
    free_packet_buffer(&s->internal->packet_buffer,     &s->internal->packet_buffer_end);
    free_packet_buffer(&s->internal->raw_packet_buffer, &s->internal->raw_packet_buffer_end);

    s->internal->raw_packet_buffer_remaining_size = RAW_PACKET_BUFFER_SIZE;
}

/*******************************************************/
/* seek support */

int av_find_default_stream_index(AVFormatContext *s)
{
    int first_audio_index = -1;
    int i;
    AVStream *st;

    if (s->nb_streams <= 0)
        return -1;
    for (i = 0; i < s->nb_streams; i++) {
        st = s->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            return i;
        }
        if (first_audio_index < 0 &&
            st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            first_audio_index = i;
    }
    return first_audio_index >= 0 ? first_audio_index : 0;
}

/** Flush the frame reader. */
void ff_read_frame_flush(AVFormatContext *s)
{
    AVStream *st;
    int i, j;

    flush_packet_queue(s);

    /* Reset read state for each stream. */
    for (i = 0; i < s->nb_streams; i++) {
        st = s->streams[i];

        if (st->parser) {
            av_parser_close(st->parser);
            st->parser = NULL;
        }
        st->last_IP_pts = AV_NOPTS_VALUE;
        /* We set the current DTS to an unspecified origin. */
        st->cur_dts     = AV_NOPTS_VALUE;

        st->probe_packets = MAX_PROBE_PACKETS;

        for (j = 0; j < MAX_REORDER_DELAY + 1; j++)
            st->pts_buffer[j] = AV_NOPTS_VALUE;
    }
}

void ff_update_cur_dts(AVFormatContext *s, AVStream *ref_st, int64_t timestamp)
{
    int i;

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];

        st->cur_dts =
            av_rescale(timestamp,
                       st->time_base.den * (int64_t) ref_st->time_base.num,
                       st->time_base.num * (int64_t) ref_st->time_base.den);
    }
}

void ff_reduce_index(AVFormatContext *s, int stream_index)
{
    AVStream *st             = s->streams[stream_index];
    unsigned int max_entries = s->max_index_size / sizeof(AVIndexEntry);

    if ((unsigned) st->nb_index_entries >= max_entries) {
        int i;
        for (i = 0; 2 * i < st->nb_index_entries; i++)
            st->index_entries[i] = st->index_entries[2 * i];
        st->nb_index_entries = i;
    }
}

int ff_add_index_entry(AVIndexEntry **index_entries,
                       int *nb_index_entries,
                       unsigned int *index_entries_allocated_size,
                       int64_t pos, int64_t timestamp,
                       int size, int distance, int flags)
{
    AVIndexEntry *entries, *ie;
    int index;

    if ((unsigned) *nb_index_entries + 1 >= UINT_MAX / sizeof(AVIndexEntry))
        return -1;

    entries = av_fast_realloc(*index_entries,
                              index_entries_allocated_size,
                              (*nb_index_entries + 1) *
                              sizeof(AVIndexEntry));
    if (!entries)
        return -1;

    *index_entries = entries;

    index = ff_index_search_timestamp(*index_entries, *nb_index_entries,
                                      timestamp, AVSEEK_FLAG_ANY);

    if (index < 0) {
        index = (*nb_index_entries)++;
        ie    = &entries[index];
        assert(index == 0 || ie[-1].timestamp < timestamp);
    } else {
        ie = &entries[index];
        if (ie->timestamp != timestamp) {
            if (ie->timestamp <= timestamp)
                return -1;
            memmove(entries + index + 1, entries + index,
                    sizeof(AVIndexEntry) * (*nb_index_entries - index));
            (*nb_index_entries)++;
        } else if (ie->pos == pos && distance < ie->min_distance)
            // do not reduce the distance
            distance = ie->min_distance;
    }

    ie->pos          = pos;
    ie->timestamp    = timestamp;
    ie->min_distance = distance;
    ie->size         = size;
    ie->flags        = flags;

    return index;
}

int av_add_index_entry(AVStream *st, int64_t pos, int64_t timestamp,
                       int size, int distance, int flags)
{
    return ff_add_index_entry(&st->index_entries, &st->nb_index_entries,
                              &st->index_entries_allocated_size, pos,
                              timestamp, size, distance, flags);
}

int ff_index_search_timestamp(const AVIndexEntry *entries, int nb_entries,
                              int64_t wanted_timestamp, int flags)
{
    int a, b, m;
    int64_t timestamp;

    a = -1;
    b = nb_entries;

    // Optimize appending index entries at the end.
    if (b && entries[b - 1].timestamp < wanted_timestamp)
        a = b - 1;

    while (b - a > 1) {
        m         = (a + b) >> 1;
        timestamp = entries[m].timestamp;
        if (timestamp >= wanted_timestamp)
            b = m;
        if (timestamp <= wanted_timestamp)
            a = m;
    }
    m = (flags & AVSEEK_FLAG_BACKWARD) ? a : b;

    if (!(flags & AVSEEK_FLAG_ANY))
        while (m >= 0 && m < nb_entries &&
               !(entries[m].flags & AVINDEX_KEYFRAME))
            m += (flags & AVSEEK_FLAG_BACKWARD) ? -1 : 1;

    if (m == nb_entries)
        return -1;
    return m;
}

int av_index_search_timestamp(AVStream *st, int64_t wanted_timestamp, int flags)
{
    return ff_index_search_timestamp(st->index_entries, st->nb_index_entries,
                                     wanted_timestamp, flags);
}

int ff_seek_frame_binary(AVFormatContext *s, int stream_index,
                         int64_t target_ts, int flags)
{
    AVInputFormat *avif = s->iformat;
    int64_t av_uninit(pos_min), av_uninit(pos_max), pos, pos_limit;
    int64_t ts_min, ts_max, ts;
    int index;
    int64_t ret;
    AVStream *st;

    if (stream_index < 0)
        return -1;

    av_log(s, AV_LOG_TRACE, "read_seek: %d %"PRId64"\n", stream_index, target_ts);

    ts_max =
    ts_min = AV_NOPTS_VALUE;
    pos_limit = -1; // GCC falsely says it may be uninitialized.

    st = s->streams[stream_index];
    if (st->index_entries) {
        AVIndexEntry *e;

        /* FIXME: Whole function must be checked for non-keyframe entries in
         * index case, especially read_timestamp(). */
        index = av_index_search_timestamp(st, target_ts,
                                          flags | AVSEEK_FLAG_BACKWARD);
        index = FFMAX(index, 0);
        e     = &st->index_entries[index];

        if (e->timestamp <= target_ts || e->pos == e->min_distance) {
            pos_min = e->pos;
            ts_min  = e->timestamp;
            av_log(s, AV_LOG_TRACE, "using cached pos_min=0x%"PRIx64" dts_min=%"PRId64"\n",
                    pos_min, ts_min);
        } else {
            assert(index == 0);
        }

        index = av_index_search_timestamp(st, target_ts,
                                          flags & ~AVSEEK_FLAG_BACKWARD);
        assert(index < st->nb_index_entries);
        if (index >= 0) {
            e = &st->index_entries[index];
            assert(e->timestamp >= target_ts);
            pos_max   = e->pos;
            ts_max    = e->timestamp;
            pos_limit = pos_max - e->min_distance;
            av_log(s, AV_LOG_TRACE, "using cached pos_max=0x%"PRIx64" pos_limit=0x%"PRIx64
                    " dts_max=%"PRId64"\n", pos_max, pos_limit, ts_max);
        }
    }

    pos = ff_gen_search(s, stream_index, target_ts, pos_min, pos_max, pos_limit,
                        ts_min, ts_max, flags, &ts, avif->read_timestamp);
    if (pos < 0)
        return -1;

    /* do the seek */
    if ((ret = avio_seek(s->pb, pos, SEEK_SET)) < 0)
        return ret;

    ff_update_cur_dts(s, st, ts);

    return 0;
}

int64_t ff_gen_search(AVFormatContext *s, int stream_index, int64_t target_ts,
                      int64_t pos_min, int64_t pos_max, int64_t pos_limit,
                      int64_t ts_min, int64_t ts_max,
                      int flags, int64_t *ts_ret,
                      int64_t (*read_timestamp)(struct AVFormatContext *, int,
                                                int64_t *, int64_t))
{
    int64_t pos, ts;
    int64_t start_pos, filesize;
    int no_change;

    av_log(s, AV_LOG_TRACE, "gen_seek: %d %"PRId64"\n", stream_index, target_ts);

    if (ts_min == AV_NOPTS_VALUE) {
        pos_min = s->internal->data_offset;
        ts_min  = read_timestamp(s, stream_index, &pos_min, INT64_MAX);
        if (ts_min == AV_NOPTS_VALUE)
            return -1;
    }

    if (ts_max == AV_NOPTS_VALUE) {
        int step = 1024;
        filesize = avio_size(s->pb);
        pos_max  = filesize - 1;
        do {
            pos_max -= step;
            ts_max   = read_timestamp(s, stream_index, &pos_max,
                                      pos_max + step);
            step    += step;
        } while (ts_max == AV_NOPTS_VALUE && pos_max >= step);
        if (ts_max == AV_NOPTS_VALUE)
            return -1;

        for (;;) {
            int64_t tmp_pos = pos_max + 1;
            int64_t tmp_ts  = read_timestamp(s, stream_index,
                                             &tmp_pos, INT64_MAX);
            if (tmp_ts == AV_NOPTS_VALUE)
                break;
            ts_max  = tmp_ts;
            pos_max = tmp_pos;
            if (tmp_pos >= filesize)
                break;
        }
        pos_limit = pos_max;
    }

    if (ts_min > ts_max)
        return -1;
    else if (ts_min == ts_max)
        pos_limit = pos_min;

    no_change = 0;
    while (pos_min < pos_limit) {
        av_log(s, AV_LOG_TRACE, "pos_min=0x%"PRIx64" pos_max=0x%"PRIx64" dts_min=%"PRId64
                " dts_max=%"PRId64"\n", pos_min, pos_max, ts_min, ts_max);
        assert(pos_limit <= pos_max);

        if (no_change == 0) {
            int64_t approximate_keyframe_distance = pos_max - pos_limit;
            // interpolate position (better than dichotomy)
            pos = av_rescale(target_ts - ts_min, pos_max - pos_min,
                             ts_max - ts_min) +
                  pos_min - approximate_keyframe_distance;
        } else if (no_change == 1) {
            // bisection if interpolation did not change min / max pos last time
            pos = (pos_min + pos_limit) >> 1;
        } else {
            /* linear search if bisection failed, can only happen if there
             * are very few or no keyframes between min/max */
            pos = pos_min;
        }
        if (pos <= pos_min)
            pos = pos_min + 1;
        else if (pos > pos_limit)
            pos = pos_limit;
        start_pos = pos;

        // May pass pos_limit instead of -1.
        ts = read_timestamp(s, stream_index, &pos, INT64_MAX);
        if (pos == pos_max)
            no_change++;
        else
            no_change = 0;
        av_log(s, AV_LOG_TRACE, "%"PRId64" %"PRId64" %"PRId64" / %"PRId64" %"PRId64" %"PRId64
                " target:%"PRId64" limit:%"PRId64" start:%"PRId64" noc:%d\n",
                pos_min, pos, pos_max, ts_min, ts, ts_max, target_ts,
                pos_limit, start_pos, no_change);
        if (ts == AV_NOPTS_VALUE) {
            av_log(s, AV_LOG_ERROR, "read_timestamp() failed in the middle\n");
            return -1;
        }
        assert(ts != AV_NOPTS_VALUE);
        if (target_ts <= ts) {
            pos_limit = start_pos - 1;
            pos_max   = pos;
            ts_max    = ts;
        }
        if (target_ts >= ts) {
            pos_min = pos;
            ts_min  = ts;
        }
    }

    pos     = (flags & AVSEEK_FLAG_BACKWARD) ? pos_min : pos_max;
    ts      = (flags & AVSEEK_FLAG_BACKWARD) ? ts_min  : ts_max;
    pos_min = pos;
    ts_min  = read_timestamp(s, stream_index, &pos_min, INT64_MAX);
    pos_min++;
    ts_max = read_timestamp(s, stream_index, &pos_min, INT64_MAX);
    av_log(s, AV_LOG_TRACE, "pos=0x%"PRIx64" %"PRId64"<=%"PRId64"<=%"PRId64"\n",
            pos, ts_min, target_ts, ts_max);
    *ts_ret = ts;
    return pos;
}

static int seek_frame_byte(AVFormatContext *s, int stream_index,
                           int64_t pos, int flags)
{
    int64_t pos_min, pos_max;

    pos_min = s->internal->data_offset;
    pos_max = avio_size(s->pb) - 1;

    if (pos < pos_min)
        pos = pos_min;
    else if (pos > pos_max)
        pos = pos_max;

    avio_seek(s->pb, pos, SEEK_SET);

    return 0;
}

static int seek_frame_generic(AVFormatContext *s, int stream_index,
                              int64_t timestamp, int flags)
{
    int index;
    int64_t ret;
    AVStream *st;
    AVIndexEntry *ie;

    st = s->streams[stream_index];

    index = av_index_search_timestamp(st, timestamp, flags);

    if (index < 0 && st->nb_index_entries &&
        timestamp < st->index_entries[0].timestamp)
        return -1;

    if (index < 0 || index == st->nb_index_entries - 1) {
        AVPacket pkt;

        if (st->nb_index_entries) {
            assert(st->index_entries);
            ie = &st->index_entries[st->nb_index_entries - 1];
            if ((ret = avio_seek(s->pb, ie->pos, SEEK_SET)) < 0)
                return ret;
            ff_update_cur_dts(s, st, ie->timestamp);
        } else {
            if ((ret = avio_seek(s->pb, s->internal->data_offset, SEEK_SET)) < 0)
                return ret;
        }
        for (;;) {
            int read_status;
            do {
                read_status = av_read_frame(s, &pkt);
            } while (read_status == AVERROR(EAGAIN));
            if (read_status < 0)
                break;
            av_packet_unref(&pkt);
            if (stream_index == pkt.stream_index)
                if ((pkt.flags & AV_PKT_FLAG_KEY) && pkt.dts > timestamp)
                    break;
        }
        index = av_index_search_timestamp(st, timestamp, flags);
    }
    if (index < 0)
        return -1;

    ff_read_frame_flush(s);
    if (s->iformat->read_seek)
        if (s->iformat->read_seek(s, stream_index, timestamp, flags) >= 0)
            return 0;
    ie = &st->index_entries[index];
    if ((ret = avio_seek(s->pb, ie->pos, SEEK_SET)) < 0)
        return ret;
    ff_update_cur_dts(s, st, ie->timestamp);

    return 0;
}

static int seek_frame_internal(AVFormatContext *s, int stream_index,
                               int64_t timestamp, int flags)
{
    int ret;
    AVStream *st;

    if (flags & AVSEEK_FLAG_BYTE) {
        if (s->iformat->flags & AVFMT_NO_BYTE_SEEK)
            return -1;
        ff_read_frame_flush(s);
        return seek_frame_byte(s, stream_index, timestamp, flags);
    }

    if (stream_index < 0) {
        stream_index = av_find_default_stream_index(s);
        if (stream_index < 0)
            return -1;

        st = s->streams[stream_index];
        /* timestamp for default must be expressed in AV_TIME_BASE units */
        timestamp = av_rescale(timestamp, st->time_base.den,
                               AV_TIME_BASE * (int64_t) st->time_base.num);
    }

    /* first, we try the format specific seek */
    if (s->iformat->read_seek) {
        ff_read_frame_flush(s);
        ret = s->iformat->read_seek(s, stream_index, timestamp, flags);
    } else
        ret = -1;
    if (ret >= 0)
        return 0;

    if (s->iformat->read_timestamp &&
        !(s->iformat->flags & AVFMT_NOBINSEARCH)) {
        ff_read_frame_flush(s);
        return ff_seek_frame_binary(s, stream_index, timestamp, flags);
    } else if (!(s->iformat->flags & AVFMT_NOGENSEARCH)) {
        ff_read_frame_flush(s);
        return seek_frame_generic(s, stream_index, timestamp, flags);
    } else
        return -1;
}

int av_seek_frame(AVFormatContext *s, int stream_index,
                  int64_t timestamp, int flags)
{
    int ret = seek_frame_internal(s, stream_index, timestamp, flags);

    if (ret >= 0)
        ret = queue_attached_pictures(s);

    return ret;
}

int avformat_seek_file(AVFormatContext *s, int stream_index, int64_t min_ts,
                       int64_t ts, int64_t max_ts, int flags)
{
    if (min_ts > ts || max_ts < ts)
        return -1;

    if (s->iformat->read_seek2) {
        int ret;
        ff_read_frame_flush(s);
        ret = s->iformat->read_seek2(s, stream_index, min_ts,
                                     ts, max_ts, flags);

        if (ret >= 0)
            ret = queue_attached_pictures(s);
        return ret;
    }

    if (s->iformat->read_timestamp) {
        // try to seek via read_timestamp()
    }

    // Fall back on old API if new is not implemented but old is.
    // Note the old API has somewhat different semantics.
    if (s->iformat->read_seek || 1)
        return av_seek_frame(s, stream_index, ts,
                             flags | ((uint64_t) ts - min_ts >
                                      (uint64_t) max_ts - ts
                                      ? AVSEEK_FLAG_BACKWARD : 0));

    // try some generic seek like seek_frame_generic() but with new ts semantics
}

/*******************************************************/

/**
 * Return TRUE if the stream has accurate duration in any stream.
 *
 * @return TRUE if the stream has accurate duration for at least one component.
 */
static int has_duration(AVFormatContext *ic)
{
    int i;
    AVStream *st;

    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        if (st->duration != AV_NOPTS_VALUE)
            return 1;
    }
    if (ic->duration != AV_NOPTS_VALUE)
        return 1;
    return 0;
}

/**
 * Estimate the stream timings from the one of each components.
 *
 * Also computes the global bitrate if possible.
 */
static void update_stream_timings(AVFormatContext *ic)
{
    int64_t start_time, start_time1, end_time, end_time1;
    int64_t duration, duration1, filesize;
    int i;
    AVStream *st;

    start_time = INT64_MAX;
    end_time   = INT64_MIN;
    duration   = INT64_MIN;
    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        if (st->start_time != AV_NOPTS_VALUE && st->time_base.den) {
            start_time1 = av_rescale_q(st->start_time, st->time_base,
                                       AV_TIME_BASE_Q);
            start_time  = FFMIN(start_time, start_time1);
            if (st->duration != AV_NOPTS_VALUE) {
                end_time1 = start_time1 +
                            av_rescale_q(st->duration, st->time_base,
                                         AV_TIME_BASE_Q);
                end_time = FFMAX(end_time, end_time1);
            }
        }
        if (st->duration != AV_NOPTS_VALUE) {
            duration1 = av_rescale_q(st->duration, st->time_base,
                                     AV_TIME_BASE_Q);
            duration  = FFMAX(duration, duration1);
        }
    }
    if (start_time != INT64_MAX) {
        ic->start_time = start_time;
        if (end_time != INT64_MIN)
            duration = FFMAX(duration, end_time - start_time);
    }
    if (duration != INT64_MIN) {
        ic->duration = duration;
        if (ic->pb && (filesize = avio_size(ic->pb)) > 0)
            /* compute the bitrate */
            ic->bit_rate = (double) filesize * 8.0 * AV_TIME_BASE /
                           (double) ic->duration;
    }
}

static void fill_all_stream_timings(AVFormatContext *ic)
{
    int i;
    AVStream *st;

    update_stream_timings(ic);
    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        if (st->start_time == AV_NOPTS_VALUE) {
            if (ic->start_time != AV_NOPTS_VALUE)
                st->start_time = av_rescale_q(ic->start_time, AV_TIME_BASE_Q,
                                              st->time_base);
            if (ic->duration != AV_NOPTS_VALUE)
                st->duration = av_rescale_q(ic->duration, AV_TIME_BASE_Q,
                                            st->time_base);
        }
    }
}

static void estimate_timings_from_bit_rate(AVFormatContext *ic)
{
    int64_t filesize, duration;
    int i;
    AVStream *st;

    /* if bit_rate is already set, we believe it */
    if (ic->bit_rate <= 0) {
        int bit_rate = 0;
        for (i = 0; i < ic->nb_streams; i++) {
            st = ic->streams[i];
            if (st->codecpar->bit_rate > 0) {
                if (INT_MAX - st->codecpar->bit_rate < bit_rate) {
                    bit_rate = 0;
                    break;
                }
                bit_rate += st->codecpar->bit_rate;
            }
        }
        ic->bit_rate = bit_rate;
    }

    /* if duration is already set, we believe it */
    if (ic->duration == AV_NOPTS_VALUE &&
        ic->bit_rate != 0) {
        filesize = ic->pb ? avio_size(ic->pb) : 0;
        if (filesize > 0) {
            for (i = 0; i < ic->nb_streams; i++) {
                st       = ic->streams[i];
                duration = av_rescale(8 * filesize, st->time_base.den,
                                      ic->bit_rate *
                                      (int64_t) st->time_base.num);
                if (st->duration == AV_NOPTS_VALUE)
                    st->duration = duration;
            }
        }
    }
}

#define DURATION_MAX_READ_SIZE 250000
#define DURATION_MAX_RETRY 3

/* only usable for MPEG-PS streams */
static void estimate_timings_from_pts(AVFormatContext *ic, int64_t old_offset)
{
    AVPacket pkt1, *pkt = &pkt1;
    AVStream *st;
    int read_size, i, ret;
    int64_t end_time;
    int64_t filesize, offset, duration;
    int retry = 0;

    /* flush packet queue */
    flush_packet_queue(ic);

    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        if (st->start_time == AV_NOPTS_VALUE && st->first_dts == AV_NOPTS_VALUE)
            av_log(ic, AV_LOG_WARNING,
                   "start time is not set in estimate_timings_from_pts\n");

        if (st->parser) {
            av_parser_close(st->parser);
            st->parser = NULL;
        }
    }

    /* estimate the end time (duration) */
    /* XXX: may need to support wrapping */
    filesize = ic->pb ? avio_size(ic->pb) : 0;
    end_time = AV_NOPTS_VALUE;
    do {
        offset = filesize - (DURATION_MAX_READ_SIZE << retry);
        if (offset < 0)
            offset = 0;

        avio_seek(ic->pb, offset, SEEK_SET);
        read_size = 0;
        for (;;) {
            if (read_size >= DURATION_MAX_READ_SIZE << (FFMAX(retry - 1, 0)))
                break;

            do {
                ret = ff_read_packet(ic, pkt);
            } while (ret == AVERROR(EAGAIN));
            if (ret != 0)
                break;
            read_size += pkt->size;
            st         = ic->streams[pkt->stream_index];
            if (pkt->pts != AV_NOPTS_VALUE &&
                (st->start_time != AV_NOPTS_VALUE ||
                 st->first_dts  != AV_NOPTS_VALUE)) {
                duration = end_time = pkt->pts;
                if (st->start_time != AV_NOPTS_VALUE)
                    duration -= st->start_time;
                else
                    duration -= st->first_dts;
                if (duration < 0)
                    duration += 1LL << st->pts_wrap_bits;
                if (duration > 0) {
                    if (st->duration == AV_NOPTS_VALUE || st->duration < duration)
                        st->duration = duration;
                }
            }
            av_packet_unref(pkt);
        }
    } while (end_time == AV_NOPTS_VALUE &&
             filesize > (DURATION_MAX_READ_SIZE << retry) &&
             ++retry <= DURATION_MAX_RETRY);

    fill_all_stream_timings(ic);

    avio_seek(ic->pb, old_offset, SEEK_SET);
    for (i = 0; i < ic->nb_streams; i++) {
        st              = ic->streams[i];
        st->cur_dts     = st->first_dts;
        st->last_IP_pts = AV_NOPTS_VALUE;
    }
}

static void estimate_timings(AVFormatContext *ic, int64_t old_offset)
{
    int64_t file_size;

    /* get the file size, if possible */
    if (ic->iformat->flags & AVFMT_NOFILE) {
        file_size = 0;
    } else {
        file_size = avio_size(ic->pb);
        file_size = FFMAX(0, file_size);
    }

    if ((!strcmp(ic->iformat->name, "mpeg") ||
         !strcmp(ic->iformat->name, "mpegts")) &&
        file_size && (ic->pb->seekable & AVIO_SEEKABLE_NORMAL)) {
        /* get accurate estimate from the PTSes */
        estimate_timings_from_pts(ic, old_offset);
    } else if (has_duration(ic)) {
        /* at least one component has timings - we use them for all
         * the components */
        fill_all_stream_timings(ic);
    } else {
        av_log(ic, AV_LOG_WARNING,
               "Estimating duration from bitrate, this may be inaccurate\n");
        /* less precise: use bitrate info */
        estimate_timings_from_bit_rate(ic);
    }
    update_stream_timings(ic);

    {
        int i;
        AVStream av_unused *st;
        for (i = 0; i < ic->nb_streams; i++) {
            st = ic->streams[i];
            av_log(ic, AV_LOG_TRACE, "%d: start_time: %0.3f duration: %0.3f\n", i,
                    (double) st->start_time / AV_TIME_BASE,
                    (double) st->duration   / AV_TIME_BASE);
        }
        av_log(ic, AV_LOG_TRACE,
                "stream: start_time: %0.3f duration: %0.3f bitrate=%d kb/s\n",
                (double) ic->start_time / AV_TIME_BASE,
                (double) ic->duration   / AV_TIME_BASE,
                ic->bit_rate / 1000);
    }
}

static int has_codec_parameters(AVStream *st)
{
    AVCodecContext *avctx = st->internal->avctx;
    int val;

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        val = avctx->sample_rate && avctx->channels;
        if (st->info->found_decoder >= 0 &&
            avctx->sample_fmt == AV_SAMPLE_FMT_NONE)
            return 0;
        break;
    case AVMEDIA_TYPE_VIDEO:
        val = avctx->width;
        if (st->info->found_decoder >= 0 && avctx->pix_fmt == AV_PIX_FMT_NONE)
            return 0;
        break;
    default:
        val = 1;
        break;
    }
    return avctx->codec_id != AV_CODEC_ID_NONE && val != 0;
}

static int has_decode_delay_been_guessed(AVStream *st)
{
    return st->internal->avctx->codec_id != AV_CODEC_ID_H264 ||
           st->info->nb_decoded_frames >= 6;
}

/* returns 1 or 0 if or if not decoded data was returned, or a negative error */
static int try_decode_frame(AVFormatContext *s, AVStream *st, AVPacket *avpkt,
                            AVDictionary **options)
{
    AVCodecContext *avctx = st->internal->avctx;
    const AVCodec *codec;
    int got_picture = 1, ret = 0;
    AVFrame *frame = av_frame_alloc();
    AVPacket pkt = *avpkt;

    if (!frame)
        return AVERROR(ENOMEM);

    if (!avcodec_is_open(avctx) && !st->info->found_decoder) {
        AVDictionary *thread_opt = NULL;

#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
        codec = st->codec->codec ? st->codec->codec
                                 : avcodec_find_decoder(st->codecpar->codec_id);
FF_ENABLE_DEPRECATION_WARNINGS
#else
        codec = avcodec_find_decoder(st->codecpar->codec_id);
#endif

        if (!codec) {
            st->info->found_decoder = -1;
            ret                     = -1;
            goto fail;
        }

        /* Force thread count to 1 since the H.264 decoder will not extract
         * SPS and PPS to extradata during multi-threaded decoding. */
        av_dict_set(options ? options : &thread_opt, "threads", "1", 0);
        ret = avcodec_open2(avctx, codec, options ? options : &thread_opt);
        if (!options)
            av_dict_free(&thread_opt);
        if (ret < 0) {
            st->info->found_decoder = -1;
            goto fail;
        }
        st->info->found_decoder = 1;
    } else if (!st->info->found_decoder)
        st->info->found_decoder = 1;

    if (st->info->found_decoder < 0) {
        ret = -1;
        goto fail;
    }

    while ((pkt.size > 0 || (!pkt.data && got_picture)) &&
           ret >= 0 &&
           (!has_codec_parameters(st) || !has_decode_delay_been_guessed(st) ||
            (!st->codec_info_nb_frames &&
             (avctx->codec->capabilities & AV_CODEC_CAP_CHANNEL_CONF)))) {
        got_picture = 0;
        if (avctx->codec_type == AVMEDIA_TYPE_VIDEO ||
            avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            ret = avcodec_send_packet(avctx, &pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                break;
            if (ret >= 0)
                pkt.size = 0;
            ret = avcodec_receive_frame(avctx, frame);
            if (ret >= 0)
                got_picture = 1;
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                ret = 0;
        }
        if (ret >= 0) {
            if (got_picture)
                st->info->nb_decoded_frames++;
            ret       = got_picture;
        }
    }

fail:
    av_frame_free(&frame);
    return ret;
}

unsigned int ff_codec_get_tag(const AVCodecTag *tags, enum AVCodecID id)
{
    while (tags->id != AV_CODEC_ID_NONE) {
        if (tags->id == id)
            return tags->tag;
        tags++;
    }
    return 0;
}

enum AVCodecID ff_codec_get_id(const AVCodecTag *tags, unsigned int tag)
{
    int i;
    for (i = 0; tags[i].id != AV_CODEC_ID_NONE; i++)
        if (tag == tags[i].tag)
            return tags[i].id;
    for (i = 0; tags[i].id != AV_CODEC_ID_NONE; i++)
        if (avpriv_toupper4(tag) == avpriv_toupper4(tags[i].tag))
            return tags[i].id;
    return AV_CODEC_ID_NONE;
}

enum AVCodecID ff_get_pcm_codec_id(int bps, int flt, int be, int sflags)
{
    if (flt) {
        switch (bps) {
        case 32:
            return be ? AV_CODEC_ID_PCM_F32BE : AV_CODEC_ID_PCM_F32LE;
        case 64:
            return be ? AV_CODEC_ID_PCM_F64BE : AV_CODEC_ID_PCM_F64LE;
        default:
            return AV_CODEC_ID_NONE;
        }
    } else {
        bps >>= 3;
        if (sflags & (1 << (bps - 1))) {
            switch (bps) {
            case 1:
                return AV_CODEC_ID_PCM_S8;
            case 2:
                return be ? AV_CODEC_ID_PCM_S16BE : AV_CODEC_ID_PCM_S16LE;
            case 3:
                return be ? AV_CODEC_ID_PCM_S24BE : AV_CODEC_ID_PCM_S24LE;
            case 4:
                return be ? AV_CODEC_ID_PCM_S32BE : AV_CODEC_ID_PCM_S32LE;
            default:
                return AV_CODEC_ID_NONE;
            }
        } else {
            switch (bps) {
            case 1:
                return AV_CODEC_ID_PCM_U8;
            case 2:
                return be ? AV_CODEC_ID_PCM_U16BE : AV_CODEC_ID_PCM_U16LE;
            case 3:
                return be ? AV_CODEC_ID_PCM_U24BE : AV_CODEC_ID_PCM_U24LE;
            case 4:
                return be ? AV_CODEC_ID_PCM_U32BE : AV_CODEC_ID_PCM_U32LE;
            default:
                return AV_CODEC_ID_NONE;
            }
        }
    }
}

unsigned int av_codec_get_tag(const AVCodecTag *const *tags, enum AVCodecID id)
{
    int i;
    for (i = 0; tags && tags[i]; i++) {
        int tag = ff_codec_get_tag(tags[i], id);
        if (tag)
            return tag;
    }
    return 0;
}

enum AVCodecID av_codec_get_id(const AVCodecTag *const *tags, unsigned int tag)
{
    int i;
    for (i = 0; tags && tags[i]; i++) {
        enum AVCodecID id = ff_codec_get_id(tags[i], tag);
        if (id != AV_CODEC_ID_NONE)
            return id;
    }
    return AV_CODEC_ID_NONE;
}

static void compute_chapters_end(AVFormatContext *s)
{
    unsigned int i, j;
    int64_t max_time = s->duration +
                       ((s->start_time == AV_NOPTS_VALUE) ? 0 : s->start_time);

    for (i = 0; i < s->nb_chapters; i++)
        if (s->chapters[i]->end == AV_NOPTS_VALUE) {
            AVChapter *ch = s->chapters[i];
            int64_t end = max_time ? av_rescale_q(max_time, AV_TIME_BASE_Q,
                                                  ch->time_base)
                                   : INT64_MAX;

            for (j = 0; j < s->nb_chapters; j++) {
                AVChapter *ch1     = s->chapters[j];
                int64_t next_start = av_rescale_q(ch1->start, ch1->time_base,
                                                  ch->time_base);
                if (j != i && next_start > ch->start && next_start < end)
                    end = next_start;
            }
            ch->end = (end == INT64_MAX) ? ch->start : end;
        }
}

static int get_std_framerate(int i)
{
    if (i < 60 * 12)
        return (i + 1) * 1001;
    else
        return ((const int[]) { 24, 30, 60, 12, 15 })[i - 60 * 12] * 1000 * 12;
}

static int extract_extradata_init(AVStream *st)
{
    AVStreamInternal *i = st->internal;
    const AVBitStreamFilter *f;
    int ret;

    f = av_bsf_get_by_name("extract_extradata");
    if (!f)
        goto finish;

    /* check that the codec id is supported */
    if (f->codec_ids) {
        const enum AVCodecID *ids;
        for (ids = f->codec_ids; *ids != AV_CODEC_ID_NONE; ids++)
            if (*ids == st->codecpar->codec_id)
                break;
        if (*ids == AV_CODEC_ID_NONE)
            goto finish;
    }

    i->extract_extradata.pkt = av_packet_alloc();
    if (!i->extract_extradata.pkt)
        return AVERROR(ENOMEM);

    ret = av_bsf_alloc(f, &i->extract_extradata.bsf);
    if (ret < 0)
        goto fail;

    ret = avcodec_parameters_copy(i->extract_extradata.bsf->par_in,
                                  st->codecpar);
    if (ret < 0)
        goto fail;

    i->extract_extradata.bsf->time_base_in = st->time_base;

    /* if init fails here, we assume extracting extradata is just not
     * supported for this codec, so we return success */
    ret = av_bsf_init(i->extract_extradata.bsf);
    if (ret < 0) {
        av_bsf_free(&i->extract_extradata.bsf);
        ret = 0;
    }

finish:
    i->extract_extradata.inited = 1;

    return 0;
fail:
    av_bsf_free(&i->extract_extradata.bsf);
    av_packet_free(&i->extract_extradata.pkt);
    return ret;
}

static int extract_extradata(AVStream *st, AVPacket *pkt)
{
    AVStreamInternal *i = st->internal;
    AVPacket *pkt_ref;
    int ret;

    if (!i->extract_extradata.inited) {
        ret = extract_extradata_init(st);
        if (ret < 0)
            return ret;
    }

    if (i->extract_extradata.inited && !i->extract_extradata.bsf)
        return 0;

    pkt_ref = i->extract_extradata.pkt;
    ret = av_packet_ref(pkt_ref, pkt);
    if (ret < 0)
        return ret;

    ret = av_bsf_send_packet(i->extract_extradata.bsf, pkt_ref);
    if (ret < 0) {
        av_packet_unref(pkt_ref);
        return ret;
    }

    while (ret >= 0 && !i->avctx->extradata) {
        int extradata_size;
        uint8_t *extradata;

        ret = av_bsf_receive_packet(i->extract_extradata.bsf, pkt_ref);
        if (ret < 0) {
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                return ret;
            continue;
        }

        extradata = av_packet_get_side_data(pkt_ref, AV_PKT_DATA_NEW_EXTRADATA,
                                            &extradata_size);

        if (extradata) {
            i->avctx->extradata = av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!i->avctx->extradata) {
                av_packet_unref(pkt_ref);
                return AVERROR(ENOMEM);
            }
            memcpy(i->avctx->extradata, extradata, extradata_size);
            i->avctx->extradata_size = extradata_size;
        }
        av_packet_unref(pkt_ref);
    }

    return 0;
}

int avformat_find_stream_info(AVFormatContext *ic, AVDictionary **options)
{
    int i, count, ret, read_size, j;
    AVStream *st;
    AVCodecContext *avctx;
    AVPacket pkt1, *pkt;
    int64_t old_offset  = avio_tell(ic->pb);
    // new streams might appear, no options for those
    int orig_nb_streams = ic->nb_streams;

    for (i = 0; i < ic->nb_streams; i++) {
        const AVCodec *codec;
        AVDictionary *thread_opt = NULL;
        st = ic->streams[i];
        avctx = st->internal->avctx;

        // only for the split stuff
        if (!st->parser && !(ic->flags & AVFMT_FLAG_NOPARSE)) {
            st->parser = av_parser_init(st->codecpar->codec_id);
            if (st->need_parsing == AVSTREAM_PARSE_HEADERS && st->parser)
                st->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
        }

        /* check if the caller has overridden the codec id */
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
        if (st->codec->codec_id != st->internal->orig_codec_id) {
            st->codecpar->codec_id   = st->codec->codec_id;
            st->codecpar->codec_type = st->codec->codec_type;
            st->internal->orig_codec_id = st->codec->codec_id;
        }
FF_ENABLE_DEPRECATION_WARNINGS
#endif
        if (st->codecpar->codec_id != st->internal->orig_codec_id)
            st->internal->orig_codec_id = st->codecpar->codec_id;

        ret = avcodec_parameters_to_context(avctx, st->codecpar);
        if (ret < 0)
            goto find_stream_info_err;
        if (st->codecpar->codec_id != AV_CODEC_ID_PROBE &&
            st->codecpar->codec_id != AV_CODEC_ID_NONE)
            st->internal->avctx_inited = 1;

#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
        codec = st->codec->codec ? st->codec->codec
                                 : avcodec_find_decoder(st->codecpar->codec_id);
FF_ENABLE_DEPRECATION_WARNINGS
#else
        codec = avcodec_find_decoder(st->codecpar->codec_id);
#endif

        /* Force thread count to 1 since the H.264 decoder will not extract
         * SPS and PPS to extradata during multi-threaded decoding. */
        av_dict_set(options ? &options[i] : &thread_opt, "threads", "1", 0);

        /* Ensure that subtitle_header is properly set. */
        if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE
            && codec && !avctx->codec)
            avcodec_open2(avctx, codec,
                          options ? &options[i] : &thread_opt);

        // Try to just open decoders, in case this is enough to get parameters.
        if (!has_codec_parameters(st)) {
            if (codec && !avctx->codec)
                avcodec_open2(avctx, codec,
                              options ? &options[i] : &thread_opt);
        }
        if (!options)
            av_dict_free(&thread_opt);
    }

    for (i = 0; i < ic->nb_streams; i++) {
        ic->streams[i]->info->fps_first_dts = AV_NOPTS_VALUE;
        ic->streams[i]->info->fps_last_dts  = AV_NOPTS_VALUE;
    }

    count     = 0;
    read_size = 0;
    for (;;) {
        if (ff_check_interrupt(&ic->interrupt_callback)) {
            ret = AVERROR_EXIT;
            av_log(ic, AV_LOG_DEBUG, "interrupted\n");
            break;
        }

        /* check if one codec still needs to be handled */
        for (i = 0; i < ic->nb_streams; i++) {
            int fps_analyze_framecount = 20;

            st = ic->streams[i];
            if (!has_codec_parameters(st))
                break;
            /* If the timebase is coarse (like the usual millisecond precision
             * of mkv), we need to analyze more frames to reliably arrive at
             * the correct fps. */
            if (av_q2d(st->time_base) > 0.0005)
                fps_analyze_framecount *= 2;
            if (ic->fps_probe_size >= 0)
                fps_analyze_framecount = ic->fps_probe_size;
            /* variable fps and no guess at the real fps */
            if (!st->avg_frame_rate.num &&
                st->codec_info_nb_frames < fps_analyze_framecount &&
                st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                break;
            if (!st->codecpar->extradata &&
                !st->internal->avctx->extradata &&
                (!st->internal->extract_extradata.inited ||
                 st->internal->extract_extradata.bsf))
                break;
            if (st->first_dts == AV_NOPTS_VALUE &&
                st->codec_info_nb_frames < ic->max_ts_probe &&
                (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
                 st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO))
                break;
        }
        if (i == ic->nb_streams) {
            /* NOTE: If the format has no header, then we need to read some
             * packets to get most of the streams, so we cannot stop here. */
            if (!(ic->ctx_flags & AVFMTCTX_NOHEADER)) {
                /* If we found the info for all the codecs, we can stop. */
                ret = count;
                av_log(ic, AV_LOG_DEBUG, "All info found\n");
                break;
            }
        }
        /* We did not get all the codec info, but we read too much data. */
        if (read_size >= ic->probesize) {
            ret = count;
            av_log(ic, AV_LOG_DEBUG,
                   "Probe buffer size limit %d reached\n", ic->probesize);
            break;
        }

        /* NOTE: A new stream can be added there if no header in file
         * (AVFMTCTX_NOHEADER). */
        ret = read_frame_internal(ic, &pkt1);
        if (ret == AVERROR(EAGAIN))
            continue;

        if (ret < 0) {
            /* EOF or error*/
            AVPacket empty_pkt = { 0 };
            int err = 0;
            av_init_packet(&empty_pkt);

            /* We could not have all the codec parameters before EOF. */
            ret = -1;
            for (i = 0; i < ic->nb_streams; i++) {
                st = ic->streams[i];

                /* flush the decoders */
                if (st->info->found_decoder == 1) {
                    do {
                        err = try_decode_frame(ic, st, &empty_pkt,
                                               (options && i < orig_nb_streams)
                                               ? &options[i] : NULL);
                    } while (err > 0 && !has_codec_parameters(st));
                }

                if (err < 0) {
                    av_log(ic, AV_LOG_WARNING,
                           "decoding for stream %d failed\n", st->index);
                } else if (!has_codec_parameters(st)) {
                    char buf[256];
                    avcodec_string(buf, sizeof(buf), st->internal->avctx, 0);
                    av_log(ic, AV_LOG_WARNING,
                           "Could not find codec parameters (%s)\n", buf);
                } else {
                    ret = 0;
                }
            }
            break;
        }

        pkt = &pkt1;

        if (!(ic->flags & AVFMT_FLAG_NOBUFFER)) {
            ret = add_to_pktbuf(&ic->internal->packet_buffer, pkt,
                                &ic->internal->packet_buffer_end, 0);
            if (ret < 0)
                goto find_stream_info_err;
        }

        read_size += pkt->size;

        st = ic->streams[pkt->stream_index];
        avctx = st->internal->avctx;
        if (!st->internal->avctx_inited) {
            ret = avcodec_parameters_to_context(avctx, st->codecpar);
            if (ret < 0)
                goto find_stream_info_err;
            st->internal->avctx_inited = 1;
        }

        if (pkt->dts != AV_NOPTS_VALUE && st->codec_info_nb_frames > 1) {
            /* check for non-increasing dts */
            if (st->info->fps_last_dts != AV_NOPTS_VALUE &&
                st->info->fps_last_dts >= pkt->dts) {
                av_log(ic, AV_LOG_WARNING,
                       "Non-increasing DTS in stream %d: packet %d with DTS "
                       "%"PRId64", packet %d with DTS %"PRId64"\n",
                       st->index, st->info->fps_last_dts_idx,
                       st->info->fps_last_dts, st->codec_info_nb_frames,
                       pkt->dts);
                st->info->fps_first_dts =
                st->info->fps_last_dts  = AV_NOPTS_VALUE;
            }
            /* Check for a discontinuity in dts. If the difference in dts
             * is more than 1000 times the average packet duration in the
             * sequence, we treat it as a discontinuity. */
            if (st->info->fps_last_dts != AV_NOPTS_VALUE &&
                st->info->fps_last_dts_idx > st->info->fps_first_dts_idx &&
                (pkt->dts - st->info->fps_last_dts) / 1000 >
                (st->info->fps_last_dts     - st->info->fps_first_dts) /
                (st->info->fps_last_dts_idx - st->info->fps_first_dts_idx)) {
                av_log(ic, AV_LOG_WARNING,
                       "DTS discontinuity in stream %d: packet %d with DTS "
                       "%"PRId64", packet %d with DTS %"PRId64"\n",
                       st->index, st->info->fps_last_dts_idx,
                       st->info->fps_last_dts, st->codec_info_nb_frames,
                       pkt->dts);
                st->info->fps_first_dts =
                st->info->fps_last_dts  = AV_NOPTS_VALUE;
            }

            /* update stored dts values */
            if (st->info->fps_first_dts == AV_NOPTS_VALUE) {
                st->info->fps_first_dts     = pkt->dts;
                st->info->fps_first_dts_idx = st->codec_info_nb_frames;
            }
            st->info->fps_last_dts     = pkt->dts;
            st->info->fps_last_dts_idx = st->codec_info_nb_frames;

            /* check max_analyze_duration */
            if (av_rescale_q(pkt->dts - st->info->fps_first_dts, st->time_base,
                             AV_TIME_BASE_Q) >= ic->max_analyze_duration) {
                av_log(ic, AV_LOG_WARNING, "max_analyze_duration %d reached\n",
                       ic->max_analyze_duration);
                if (ic->flags & AVFMT_FLAG_NOBUFFER)
                    av_packet_unref(pkt);
                break;
            }
        }
        if (!st->internal->avctx->extradata) {
            ret = extract_extradata(st, pkt);
            if (ret < 0)
                goto find_stream_info_err;
        }

        /* If still no information, we try to open the codec and to
         * decompress the frame. We try to avoid that in most cases as
         * it takes longer and uses more memory. For MPEG-4, we need to
         * decompress for QuickTime.
         *
         * If AV_CODEC_CAP_CHANNEL_CONF is set this will force decoding of at
         * least one frame of codec data, this makes sure the codec initializes
         * the channel configuration and does not only trust the values from
         * the container. */
        try_decode_frame(ic, st, pkt,
                         (options && i < orig_nb_streams) ? &options[i] : NULL);

        if (ic->flags & AVFMT_FLAG_NOBUFFER)
            av_packet_unref(pkt);

        st->codec_info_nb_frames++;
        count++;
    }

    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        avctx = st->internal->avctx;
        if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            /* estimate average framerate if not set by demuxer */
            if (!st->avg_frame_rate.num &&
                st->info->fps_last_dts != st->info->fps_first_dts) {
                int64_t delta_dts = st->info->fps_last_dts -
                                    st->info->fps_first_dts;
                int delta_packets = st->info->fps_last_dts_idx -
                                    st->info->fps_first_dts_idx;
                int best_fps      = 0;
                double best_error = 0.01;

                if (delta_dts     >= INT64_MAX / st->time_base.num ||
                    delta_packets >= INT64_MAX / st->time_base.den ||
                    delta_dts < 0)
                    continue;
                av_reduce(&st->avg_frame_rate.num, &st->avg_frame_rate.den,
                          delta_packets * (int64_t) st->time_base.den,
                          delta_dts * (int64_t) st->time_base.num, 60000);

                /* Round guessed framerate to a "standard" framerate if it's
                 * within 1% of the original estimate. */
                for (j = 0; j < MAX_STD_TIMEBASES; j++) {
                    AVRational std_fps = { get_std_framerate(j), 12 * 1001 };
                    double error       = fabs(av_q2d(st->avg_frame_rate) /
                                              av_q2d(std_fps) - 1);

                    if (error < best_error) {
                        best_error = error;
                        best_fps   = std_fps.num;
                    }
                }
                if (best_fps)
                    av_reduce(&st->avg_frame_rate.num, &st->avg_frame_rate.den,
                              best_fps, 12 * 1001, INT_MAX);
            }
        } else if (avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (!avctx->bits_per_coded_sample)
                avctx->bits_per_coded_sample =
                    av_get_bits_per_sample(avctx->codec_id);
            // set stream disposition based on audio service type
            switch (avctx->audio_service_type) {
            case AV_AUDIO_SERVICE_TYPE_EFFECTS:
                st->disposition = AV_DISPOSITION_CLEAN_EFFECTS;
                break;
            case AV_AUDIO_SERVICE_TYPE_VISUALLY_IMPAIRED:
                st->disposition = AV_DISPOSITION_VISUAL_IMPAIRED;
                break;
            case AV_AUDIO_SERVICE_TYPE_HEARING_IMPAIRED:
                st->disposition = AV_DISPOSITION_HEARING_IMPAIRED;
                break;
            case AV_AUDIO_SERVICE_TYPE_COMMENTARY:
                st->disposition = AV_DISPOSITION_COMMENT;
                break;
            case AV_AUDIO_SERVICE_TYPE_KARAOKE:
                st->disposition = AV_DISPOSITION_KARAOKE;
                break;
            }
        }
    }

    compute_chapters_end(ic);

    /* update the stream parameters from the internal codec contexts */
    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        if (!st->internal->avctx_inited)
            continue;

        ret = avcodec_parameters_from_context(st->codecpar, st->internal->avctx);
        if (ret < 0)
            goto find_stream_info_err;

#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
        ret = avcodec_parameters_to_context(st->codec, st->codecpar);
        if (ret < 0)
            goto find_stream_info_err;

        if (st->internal->avctx->subtitle_header) {
            st->codec->subtitle_header = av_malloc(st->internal->avctx->subtitle_header_size);
            if (!st->codec->subtitle_header)
                goto find_stream_info_err;
            st->codec->subtitle_header_size = st->internal->avctx->subtitle_header_size;
            memcpy(st->codec->subtitle_header, st->internal->avctx->subtitle_header,
                   st->codec->subtitle_header_size);
        }
FF_ENABLE_DEPRECATION_WARNINGS
#endif

        st->internal->avctx_inited = 0;
    }

    estimate_timings(ic, old_offset);

find_stream_info_err:
    for (i = 0; i < ic->nb_streams; i++) {
        avcodec_close(ic->streams[i]->internal->avctx);
        av_freep(&ic->streams[i]->info);
        av_bsf_free(&ic->streams[i]->internal->extract_extradata.bsf);
        av_packet_free(&ic->streams[i]->internal->extract_extradata.pkt);
    }
    return ret;
}

static AVProgram *find_program_from_stream(AVFormatContext *ic, int s)
{
    int i, j;

    for (i = 0; i < ic->nb_programs; i++)
        for (j = 0; j < ic->programs[i]->nb_stream_indexes; j++)
            if (ic->programs[i]->stream_index[j] == s)
                return ic->programs[i];
    return NULL;
}

int av_find_best_stream(AVFormatContext *ic, enum AVMediaType type,
                        int wanted_stream_nb, int related_stream,
                        AVCodec **decoder_ret, int flags)
{
    int i, nb_streams = ic->nb_streams;
    int ret = AVERROR_STREAM_NOT_FOUND, best_count = -1;
    unsigned *program = NULL;
    AVCodec *decoder = NULL, *best_decoder = NULL;

    if (related_stream >= 0 && wanted_stream_nb < 0) {
        AVProgram *p = find_program_from_stream(ic, related_stream);
        if (p) {
            program    = p->stream_index;
            nb_streams = p->nb_stream_indexes;
        }
    }
    for (i = 0; i < nb_streams; i++) {
        int real_stream_index = program ? program[i] : i;
        AVStream *st          = ic->streams[real_stream_index];
        AVCodecParameters *par = st->codecpar;
        if (par->codec_type != type)
            continue;
        if (wanted_stream_nb >= 0 && real_stream_index != wanted_stream_nb)
            continue;
        if (st->disposition & (AV_DISPOSITION_HEARING_IMPAIRED |
                               AV_DISPOSITION_VISUAL_IMPAIRED))
            continue;
        if (decoder_ret) {
            decoder = avcodec_find_decoder(par->codec_id);
            if (!decoder) {
                if (ret < 0)
                    ret = AVERROR_DECODER_NOT_FOUND;
                continue;
            }
        }
        if (best_count >= st->codec_info_nb_frames)
            continue;
        best_count   = st->codec_info_nb_frames;
        ret          = real_stream_index;
        best_decoder = decoder;
        if (program && i == nb_streams - 1 && ret < 0) {
            program    = NULL;
            nb_streams = ic->nb_streams;
            /* no related stream found, try again with everything */
            i = 0;
        }
    }
    if (decoder_ret)
        *decoder_ret = best_decoder;
    return ret;
}

/*******************************************************/

int av_read_play(AVFormatContext *s)
{
    if (s->iformat->read_play)
        return s->iformat->read_play(s);
    if (s->pb)
        return avio_pause(s->pb, 0);
    return AVERROR(ENOSYS);
}

int av_read_pause(AVFormatContext *s)
{
    if (s->iformat->read_pause)
        return s->iformat->read_pause(s);
    if (s->pb)
        return avio_pause(s->pb, 1);
    return AVERROR(ENOSYS);
}

static void free_stream(AVStream **pst)
{
    AVStream *st = *pst;
    int i;

    if (!st)
        return;

    for (i = 0; i < st->nb_side_data; i++)
        av_freep(&st->side_data[i].data);
    av_freep(&st->side_data);

    if (st->parser)
        av_parser_close(st->parser);

    if (st->attached_pic.data)
        av_packet_unref(&st->attached_pic);

    if (st->internal) {
        avcodec_free_context(&st->internal->avctx);
        av_bsf_free(&st->internal->extract_extradata.bsf);
        av_packet_free(&st->internal->extract_extradata.pkt);
    }
    av_freep(&st->internal);

    av_dict_free(&st->metadata);
    avcodec_parameters_free(&st->codecpar);
    av_freep(&st->probe_data.buf);
    av_free(st->index_entries);
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
    av_free(st->codec->extradata);
    av_free(st->codec->subtitle_header);
    av_free(st->codec);
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    av_free(st->priv_data);
    av_free(st->info);

    av_freep(pst);
}

void avformat_free_context(AVFormatContext *s)
{
    int i;

    if (!s)
        return;

    av_opt_free(s);
    if (s->iformat && s->iformat->priv_class && s->priv_data)
        av_opt_free(s->priv_data);

    for (i = 0; i < s->nb_streams; i++)
        free_stream(&s->streams[i]);

    for (i = s->nb_programs - 1; i >= 0; i--) {
        av_dict_free(&s->programs[i]->metadata);
        av_freep(&s->programs[i]->stream_index);
        av_freep(&s->programs[i]);
    }
    av_freep(&s->programs);
    av_freep(&s->priv_data);
    while (s->nb_chapters--) {
        av_dict_free(&s->chapters[s->nb_chapters]->metadata);
        av_free(s->chapters[s->nb_chapters]);
    }
    av_freep(&s->chapters);
    av_dict_free(&s->metadata);
    av_freep(&s->streams);
    av_freep(&s->internal);
    av_free(s);
}

void avformat_close_input(AVFormatContext **ps)
{
    AVFormatContext *s = *ps;
    AVIOContext *pb;

    if (!*ps)
        return;

    pb = s->pb;

    if ((s->iformat && s->iformat->flags & AVFMT_NOFILE) ||
        (s->flags & AVFMT_FLAG_CUSTOM_IO))
        pb = NULL;

    flush_packet_queue(s);

    if (s->iformat)
        if (s->iformat->read_close)
            s->iformat->read_close(s);

    avformat_free_context(s);

    *ps = NULL;

    avio_close(pb);
}

AVStream *avformat_new_stream(AVFormatContext *s, const AVCodec *c)
{
    AVStream *st;
    int i;

    if (av_reallocp_array(&s->streams, s->nb_streams + 1,
                          sizeof(*s->streams)) < 0) {
        s->nb_streams = 0;
        return NULL;
    }

    st = av_mallocz(sizeof(AVStream));
    if (!st)
        return NULL;
    if (!(st->info = av_mallocz(sizeof(*st->info)))) {
        av_free(st);
        return NULL;
    }

#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
    st->codec = avcodec_alloc_context3(c);
    if (!st->codec) {
        av_free(st->info);
        av_free(st);
        return NULL;
    }
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    st->internal = av_mallocz(sizeof(*st->internal));
    if (!st->internal)
        goto fail;

    if (s->iformat) {
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
        /* no default bitrate if decoding */
        st->codec->bit_rate = 0;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

        /* default pts setting is MPEG-like */
        avpriv_set_pts_info(st, 33, 1, 90000);
        /* we set the current DTS to 0 so that formats without any timestamps
         * but durations get some timestamps, formats with some unknown
         * timestamps have their first few packets buffered and the
         * timestamps corrected before they are returned to the user */
        st->cur_dts = 0;
    } else {
        st->cur_dts = AV_NOPTS_VALUE;
    }

    st->codecpar = avcodec_parameters_alloc();
    if (!st->codecpar)
        goto fail;

    st->internal->avctx = avcodec_alloc_context3(NULL);
    if (!st->internal->avctx)
        goto fail;

    st->index      = s->nb_streams;
    st->start_time = AV_NOPTS_VALUE;
    st->duration   = AV_NOPTS_VALUE;
    st->first_dts     = AV_NOPTS_VALUE;
    st->probe_packets = MAX_PROBE_PACKETS;

    st->last_IP_pts = AV_NOPTS_VALUE;
    for (i = 0; i < MAX_REORDER_DELAY + 1; i++)
        st->pts_buffer[i] = AV_NOPTS_VALUE;

    st->sample_aspect_ratio = (AVRational) { 0, 1 };

    st->info->fps_first_dts = AV_NOPTS_VALUE;
    st->info->fps_last_dts  = AV_NOPTS_VALUE;

#if FF_API_LAVF_AVCTX
    st->internal->need_codec_update = 1;
#endif

    s->streams[s->nb_streams++] = st;
    return st;
fail:
    free_stream(&st);
    return NULL;
}

AVProgram *av_new_program(AVFormatContext *ac, int id)
{
    AVProgram *program = NULL;
    int i;

    av_log(ac, AV_LOG_TRACE, "new_program: id=0x%04x\n", id);

    for (i = 0; i < ac->nb_programs; i++)
        if (ac->programs[i]->id == id)
            program = ac->programs[i];

    if (!program) {
        program = av_mallocz(sizeof(AVProgram));
        if (!program)
            return NULL;
        dynarray_add(&ac->programs, &ac->nb_programs, program);
        program->discard = AVDISCARD_NONE;
    }
    program->id = id;

    return program;
}

AVChapter *avpriv_new_chapter(AVFormatContext *s, int id, AVRational time_base,
                              int64_t start, int64_t end, const char *title)
{
    AVChapter *chapter = NULL;
    int i;

    for (i = 0; i < s->nb_chapters; i++)
        if (s->chapters[i]->id == id)
            chapter = s->chapters[i];

    if (!chapter) {
        chapter = av_mallocz(sizeof(AVChapter));
        if (!chapter)
            return NULL;
        dynarray_add(&s->chapters, &s->nb_chapters, chapter);
    }
    av_dict_set(&chapter->metadata, "title", title, 0);
    chapter->id        = id;
    chapter->time_base = time_base;
    chapter->start     = start;
    chapter->end       = end;

    return chapter;
}

void ff_program_add_stream_index(AVFormatContext *ac, int progid, unsigned idx)
{
    int i, j;
    AVProgram *program = NULL;

    if (idx >= ac->nb_streams) {
        av_log(ac, AV_LOG_ERROR, "stream index %d is not valid\n", idx);
        return;
    }

    for (i = 0; i < ac->nb_programs; i++) {
        if (ac->programs[i]->id != progid)
            continue;
        program = ac->programs[i];
        for (j = 0; j < program->nb_stream_indexes; j++)
            if (program->stream_index[j] == idx)
                return;

        if (av_reallocp_array(&program->stream_index,
                              program->nb_stream_indexes + 1,
                              sizeof(*program->stream_index)) < 0) {
            program->nb_stream_indexes = 0;
            return;
        }
        program->stream_index[program->nb_stream_indexes++] = idx;
        return;
    }
}

uint64_t ff_ntp_time(void)
{
    return (av_gettime() / 1000) * 1000 + NTP_OFFSET_US;
}

int av_get_frame_filename(char *buf, int buf_size, const char *path, int number)
{
    const char *p;
    char *q, buf1[20], c;
    int nd, len, percentd_found;

    q = buf;
    p = path;
    percentd_found = 0;
    for (;;) {
        c = *p++;
        if (c == '\0')
            break;
        if (c == '%') {
            do {
                nd = 0;
                while (av_isdigit(*p))
                    nd = nd * 10 + *p++ - '0';
                c = *p++;
            } while (av_isdigit(c));

            switch (c) {
            case '%':
                goto addchar;
            case 'd':
                if (percentd_found)
                    goto fail;
                percentd_found = 1;
                snprintf(buf1, sizeof(buf1), "%0*d", nd, number);
                len = strlen(buf1);
                if ((q - buf + len) > buf_size - 1)
                    goto fail;
                memcpy(q, buf1, len);
                q += len;
                break;
            default:
                goto fail;
            }
        } else {
addchar:
            if ((q - buf) < buf_size - 1)
                *q++ = c;
        }
    }
    if (!percentd_found)
        goto fail;
    *q = '\0';
    return 0;
fail:
    *q = '\0';
    return -1;
}

void av_url_split(char *proto, int proto_size,
                  char *authorization, int authorization_size,
                  char *hostname, int hostname_size,
                  int *port_ptr, char *path, int path_size, const char *url)
{
    const char *p, *ls, *at, *col, *brk;

    if (port_ptr)
        *port_ptr = -1;
    if (proto_size > 0)
        proto[0] = 0;
    if (authorization_size > 0)
        authorization[0] = 0;
    if (hostname_size > 0)
        hostname[0] = 0;
    if (path_size > 0)
        path[0] = 0;

    /* parse protocol */
    if ((p = strchr(url, ':'))) {
        av_strlcpy(proto, url, FFMIN(proto_size, p + 1 - url));
        p++; /* skip ':' */
        if (*p == '/')
            p++;
        if (*p == '/')
            p++;
    } else {
        /* no protocol means plain filename */
        av_strlcpy(path, url, path_size);
        return;
    }

    /* separate path from hostname */
    ls = strchr(p, '/');
    if (!ls)
        ls = strchr(p, '?');
    if (ls)
        av_strlcpy(path, ls, path_size);
    else
        ls = &p[strlen(p)];  // XXX

    /* the rest is hostname, use that to parse auth/port */
    if (ls != p) {
        /* authorization (user[:pass]@hostname) */
        if ((at = strchr(p, '@')) && at < ls) {
            av_strlcpy(authorization, p,
                       FFMIN(authorization_size, at + 1 - p));
            p = at + 1; /* skip '@' */
        }

        if (*p == '[' && (brk = strchr(p, ']')) && brk < ls) {
            /* [host]:port */
            av_strlcpy(hostname, p + 1,
                       FFMIN(hostname_size, brk - p));
            if (brk[1] == ':' && port_ptr)
                *port_ptr = atoi(brk + 2);
        } else if ((col = strchr(p, ':')) && col < ls) {
            av_strlcpy(hostname, p,
                       FFMIN(col + 1 - p, hostname_size));
            if (port_ptr)
                *port_ptr = atoi(col + 1);
        } else
            av_strlcpy(hostname, p,
                       FFMIN(ls + 1 - p, hostname_size));
    }
}

char *ff_data_to_hex(char *buff, const uint8_t *src, int s, int lowercase)
{
    int i;
    static const char hex_table_uc[16] = { '0', '1', '2', '3',
                                           '4', '5', '6', '7',
                                           '8', '9', 'A', 'B',
                                           'C', 'D', 'E', 'F' };
    static const char hex_table_lc[16] = { '0', '1', '2', '3',
                                           '4', '5', '6', '7',
                                           '8', '9', 'a', 'b',
                                           'c', 'd', 'e', 'f' };
    const char *hex_table = lowercase ? hex_table_lc : hex_table_uc;

    for (i = 0; i < s; i++) {
        buff[i * 2]     = hex_table[src[i] >> 4];
        buff[i * 2 + 1] = hex_table[src[i] & 0xF];
    }

    return buff;
}

int ff_hex_to_data(uint8_t *data, const char *p)
{
    int c, len, v;

    len = 0;
    v   = 1;
    for (;;) {
        p += strspn(p, SPACE_CHARS);
        if (*p == '\0')
            break;
        c = av_toupper((unsigned char) *p++);
        if (c >= '0' && c <= '9')
            c = c - '0';
        else if (c >= 'A' && c <= 'F')
            c = c - 'A' + 10;
        else
            break;
        v = (v << 4) | c;
        if (v & 0x100) {
            if (data)
                data[len] = v;
            len++;
            v = 1;
        }
    }
    return len;
}

void avpriv_set_pts_info(AVStream *s, int pts_wrap_bits,
                         unsigned int pts_num, unsigned int pts_den)
{
    AVRational new_tb;
    if (av_reduce(&new_tb.num, &new_tb.den, pts_num, pts_den, INT_MAX)) {
        if (new_tb.num != pts_num)
            av_log(NULL, AV_LOG_DEBUG,
                   "st:%d removing common factor %d from timebase\n",
                   s->index, pts_num / new_tb.num);
    } else
        av_log(NULL, AV_LOG_WARNING,
               "st:%d has too large timebase, reducing\n", s->index);

    if (new_tb.num <= 0 || new_tb.den <= 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Ignoring attempt to set invalid timebase for st:%d\n",
               s->index);
        return;
    }
    s->time_base     = new_tb;
    s->pts_wrap_bits = pts_wrap_bits;
}

void ff_parse_key_value(const char *str, ff_parse_key_val_cb callback_get_buf,
                        void *context)
{
    const char *ptr = str;

    /* Parse key=value pairs. */
    for (;;) {
        const char *key;
        char *dest = NULL, *dest_end;
        int key_len, dest_len = 0;

        /* Skip whitespace and potential commas. */
        while (*ptr && (av_isspace(*ptr) || *ptr == ','))
            ptr++;
        if (!*ptr)
            break;

        key = ptr;

        if (!(ptr = strchr(key, '=')))
            break;
        ptr++;
        key_len = ptr - key;

        callback_get_buf(context, key, key_len, &dest, &dest_len);
        dest_end = dest + dest_len - 1;

        if (*ptr == '\"') {
            ptr++;
            while (*ptr && *ptr != '\"') {
                if (*ptr == '\\') {
                    if (!ptr[1])
                        break;
                    if (dest && dest < dest_end)
                        *dest++ = ptr[1];
                    ptr += 2;
                } else {
                    if (dest && dest < dest_end)
                        *dest++ = *ptr;
                    ptr++;
                }
            }
            if (*ptr == '\"')
                ptr++;
        } else {
            for (; *ptr && !(av_isspace(*ptr) || *ptr == ','); ptr++)
                if (dest && dest < dest_end)
                    *dest++ = *ptr;
        }
        if (dest)
            *dest = 0;
    }
}

int ff_find_stream_index(AVFormatContext *s, int id)
{
    int i;
    for (i = 0; i < s->nb_streams; i++)
        if (s->streams[i]->id == id)
            return i;
    return -1;
}

int64_t ff_iso8601_to_unix_time(const char *datestr)
{
    struct tm time1 = { 0 }, time2 = { 0 };
    const char *ret1, *ret2;
    ret1 = av_small_strptime(datestr, "%Y - %m - %d %T", &time1);
    ret2 = av_small_strptime(datestr, "%Y - %m - %dT%T", &time2);
    if (ret2 && !ret1)
        return av_timegm(&time2);
    else
        return av_timegm(&time1);
}

int avformat_query_codec(const AVOutputFormat *ofmt, enum AVCodecID codec_id,
                         int std_compliance)
{
    if (ofmt) {
        if (ofmt->query_codec)
            return ofmt->query_codec(codec_id, std_compliance);
        else if (ofmt->codec_tag)
            return !!av_codec_get_tag(ofmt->codec_tag, codec_id);
        else if (codec_id == ofmt->video_codec ||
                 codec_id == ofmt->audio_codec ||
                 codec_id == ofmt->subtitle_codec)
            return 1;
    }
    return AVERROR_PATCHWELCOME;
}

int avformat_network_init(void)
{
#if CONFIG_NETWORK
    int ret;
    ff_network_inited_globally = 1;
    if ((ret = ff_network_init()) < 0)
        return ret;
    ff_tls_init();
#endif
    return 0;
}

int avformat_network_deinit(void)
{
#if CONFIG_NETWORK
    ff_network_close();
    ff_tls_deinit();
#endif
    return 0;
}

int ff_add_param_change(AVPacket *pkt, int32_t channels,
                        uint64_t channel_layout, int32_t sample_rate,
                        int32_t width, int32_t height)
{
    uint32_t flags = 0;
    int size = 4;
    uint8_t *data;
    if (!pkt)
        return AVERROR(EINVAL);
    if (channels) {
        size  += 4;
        flags |= AV_SIDE_DATA_PARAM_CHANGE_CHANNEL_COUNT;
    }
    if (channel_layout) {
        size  += 8;
        flags |= AV_SIDE_DATA_PARAM_CHANGE_CHANNEL_LAYOUT;
    }
    if (sample_rate) {
        size  += 4;
        flags |= AV_SIDE_DATA_PARAM_CHANGE_SAMPLE_RATE;
    }
    if (width || height) {
        size  += 8;
        flags |= AV_SIDE_DATA_PARAM_CHANGE_DIMENSIONS;
    }
    data = av_packet_new_side_data(pkt, AV_PKT_DATA_PARAM_CHANGE, size);
    if (!data)
        return AVERROR(ENOMEM);
    bytestream_put_le32(&data, flags);
    if (channels)
        bytestream_put_le32(&data, channels);
    if (channel_layout)
        bytestream_put_le64(&data, channel_layout);
    if (sample_rate)
        bytestream_put_le32(&data, sample_rate);
    if (width || height) {
        bytestream_put_le32(&data, width);
        bytestream_put_le32(&data, height);
    }
    return 0;
}

int ff_generate_avci_extradata(AVStream *st)
{
    static const uint8_t avci100_1080p_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x7a, 0x10, 0x29,
        0xb6, 0xd4, 0x20, 0x22, 0x33, 0x19, 0xc6, 0x63,
        0x23, 0x21, 0x01, 0x11, 0x98, 0xce, 0x33, 0x19,
        0x18, 0x21, 0x02, 0x56, 0xb9, 0x3d, 0x7d, 0x7e,
        0x4f, 0xe3, 0x3f, 0x11, 0xf1, 0x9e, 0x08, 0xb8,
        0x8c, 0x54, 0x43, 0xc0, 0x78, 0x02, 0x27, 0xe2,
        0x70, 0x1e, 0x30, 0x10, 0x10, 0x14, 0x00, 0x00,
        0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xca,
        0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x33, 0x48,
        0xd0
    };
    static const uint8_t avci100_1080i_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x7a, 0x10, 0x29,
        0xb6, 0xd4, 0x20, 0x22, 0x33, 0x19, 0xc6, 0x63,
        0x23, 0x21, 0x01, 0x11, 0x98, 0xce, 0x33, 0x19,
        0x18, 0x21, 0x03, 0x3a, 0x46, 0x65, 0x6a, 0x65,
        0x24, 0xad, 0xe9, 0x12, 0x32, 0x14, 0x1a, 0x26,
        0x34, 0xad, 0xa4, 0x41, 0x82, 0x23, 0x01, 0x50,
        0x2b, 0x1a, 0x24, 0x69, 0x48, 0x30, 0x40, 0x2e,
        0x11, 0x12, 0x08, 0xc6, 0x8c, 0x04, 0x41, 0x28,
        0x4c, 0x34, 0xf0, 0x1e, 0x01, 0x13, 0xf2, 0xe0,
        0x3c, 0x60, 0x20, 0x20, 0x28, 0x00, 0x00, 0x03,
        0x00, 0x08, 0x00, 0x00, 0x03, 0x01, 0x94, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x33, 0x48,
        0xd0
    };
    static const uint8_t avci50_1080i_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x6e, 0x10, 0x28,
        0xa6, 0xd4, 0x20, 0x32, 0x33, 0x0c, 0x71, 0x18,
        0x88, 0x62, 0x10, 0x19, 0x19, 0x86, 0x38, 0x8c,
        0x44, 0x30, 0x21, 0x02, 0x56, 0x4e, 0x6e, 0x61,
        0x87, 0x3e, 0x73, 0x4d, 0x98, 0x0c, 0x03, 0x06,
        0x9c, 0x0b, 0x73, 0xe6, 0xc0, 0xb5, 0x18, 0x63,
        0x0d, 0x39, 0xe0, 0x5b, 0x02, 0xd4, 0xc6, 0x19,
        0x1a, 0x79, 0x8c, 0x32, 0x34, 0x24, 0xf0, 0x16,
        0x81, 0x13, 0xf7, 0xff, 0x80, 0x01, 0x80, 0x02,
        0x71, 0x80, 0x80, 0x80, 0xa0, 0x00, 0x00, 0x03,
        0x00, 0x20, 0x00, 0x00, 0x06, 0x50, 0x80, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x31, 0x12,
        0x11
    };
    static const uint8_t avci100_720p_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x7a, 0x10, 0x29,
        0xb6, 0xd4, 0x20, 0x2a, 0x33, 0x1d, 0xc7, 0x62,
        0xa1, 0x08, 0x40, 0x54, 0x66, 0x3b, 0x8e, 0xc5,
        0x42, 0x02, 0x10, 0x25, 0x64, 0x2c, 0x89, 0xe8,
        0x85, 0xe4, 0x21, 0x4b, 0x90, 0x83, 0x06, 0x95,
        0xd1, 0x06, 0x46, 0x97, 0x20, 0xc8, 0xd7, 0x43,
        0x08, 0x11, 0xc2, 0x1e, 0x4c, 0x91, 0x0f, 0x01,
        0x40, 0x16, 0xec, 0x07, 0x8c, 0x04, 0x04, 0x05,
        0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03,
        0x00, 0x64, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x31, 0x12,
        0x11
    };

    const uint8_t *data = NULL;
    int size            = 0;

    if (st->codecpar->width == 1920) {
        if (st->codecpar->field_order == AV_FIELD_PROGRESSIVE) {
            data = avci100_1080p_extradata;
            size = sizeof(avci100_1080p_extradata);
        } else {
            data = avci100_1080i_extradata;
            size = sizeof(avci100_1080i_extradata);
        }
    } else if (st->codecpar->width == 1440) {
        data = avci50_1080i_extradata;
        size = sizeof(avci50_1080i_extradata);
    } else if (st->codecpar->width == 1280) {
        data = avci100_720p_extradata;
        size = sizeof(avci100_720p_extradata);
    }

    if (!size)
        return 0;

    av_freep(&st->codecpar->extradata);
    st->codecpar->extradata_size = 0;
    st->codecpar->extradata      = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!st->codecpar->extradata)
        return AVERROR(ENOMEM);

    memcpy(st->codecpar->extradata, data, size);
    st->codecpar->extradata_size = size;

    return 0;
}

uint8_t *av_stream_get_side_data(AVStream *st, enum AVPacketSideDataType type,
                                 int *size)
{
    int i;

    for (i = 0; i < st->nb_side_data; i++) {
        if (st->side_data[i].type == type) {
            if (size)
                *size = st->side_data[i].size;
            return st->side_data[i].data;
        }
    }
    return NULL;
}

int av_stream_add_side_data(AVStream *st, enum AVPacketSideDataType type,
                            uint8_t *data, size_t size)
{
    AVPacketSideData *sd, *tmp;
    int i;

    for (i = 0; i < st->nb_side_data; i++) {
        sd = &st->side_data[i];

        if (sd->type == type) {
            av_freep(&sd->data);
            sd->data = data;
            sd->size = size;
            return 0;
        }
    }

    if ((unsigned) st->nb_side_data + 1 >= INT_MAX / sizeof(*st->side_data))
        return AVERROR(ERANGE);

    tmp = av_realloc(st->side_data, (st->nb_side_data + 1) * sizeof(*tmp));
    if (!tmp) {
        return AVERROR(ENOMEM);
    }

    st->side_data = tmp;
    st->nb_side_data++;

    sd = &st->side_data[st->nb_side_data - 1];
    sd->type = type;
    sd->data = data;
    sd->size = size;

    return 0;
}

uint8_t *av_stream_new_side_data(AVStream *st, enum AVPacketSideDataType type,
                                 int size)
{
    int ret;
    uint8_t *data = av_malloc(size);

    if (!data)
        return NULL;

    ret = av_stream_add_side_data(st, type, data, size);
    if (ret < 0) {
        av_freep(&data);
        return NULL;
    }

    return data;
}

void ff_format_io_close(AVFormatContext *s, AVIOContext **pb)
{
    if (*pb)
        s->io_close(s, *pb);
    *pb = NULL;
}

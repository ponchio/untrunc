/*
 * RSO demuxer
 * Copyright (c) 2001 Fabrice Bellard (original AU code)
 * Copyright (c) 2010 Rafael Carre
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

#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"
#include "avformat.h"
#include "internal.h"
#include "pcm.h"
#include "rso.h"

static int rso_read_header(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    int id, rate, bps;
    unsigned int size;
    enum AVCodecID codec;
    AVStream *st;

    id   = avio_rb16(pb);
    size = avio_rb16(pb);
    rate = avio_rb16(pb);
    avio_rb16(pb);   /* play mode ? (0x0000 = don't loop) */

    codec = ff_codec_get_id(ff_codec_rso_tags, id);

    if (codec == AV_CODEC_ID_ADPCM_IMA_WAV) {
        avpriv_report_missing_feature(s, "ADPCM in RSO");
        return AVERROR_PATCHWELCOME;
    }

    bps = av_get_bits_per_sample(codec);
    if (!bps) {
        avpriv_request_sample(s, "Unknown bits per sample");
        return AVERROR_PATCHWELCOME;
    }

    /* now we are ready: build format streams */
    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->duration            = (size * 8) / bps;
    st->codecpar->codec_type   = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_tag    = id;
    st->codecpar->codec_id     = codec;
    st->codecpar->channels     = 1;
    st->codecpar->channel_layout = AV_CH_LAYOUT_MONO;
    st->codecpar->sample_rate  = rate;

    avpriv_set_pts_info(st, 64, 1, rate);

    return 0;
}

#define BLOCK_SIZE 1024 /* in samples */

static int rso_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int bps = av_get_bits_per_sample(s->streams[0]->codecpar->codec_id);
    int ret = av_get_packet(s->pb, pkt, BLOCK_SIZE * bps >> 3);

    if (ret < 0)
        return ret;

    pkt->stream_index = 0;

    /* note: we need to modify the packet size here to handle the last packet */
    pkt->size = ret;

    return 0;
}

AVInputFormat ff_rso_demuxer = {
    .name           =   "rso",
    .long_name      =   NULL_IF_CONFIG_SMALL("Lego Mindstorms RSO"),
    .extensions     =   "rso",
    .read_header    =   rso_read_header,
    .read_packet    =   rso_read_packet,
    .read_seek      =   ff_pcm_read_seek,
    .codec_tag      =   (const AVCodecTag* const []){ff_codec_rso_tags, 0},
};

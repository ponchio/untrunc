/*
 * Matroska muxer
 * Copyright (c) 2007 David Conrad
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

#include "avc.h"
#include "hevc.h"
#include "avformat.h"
#include "avio_internal.h"
#include "avlanguage.h"
#include "flacenc.h"
#include "internal.h"
#include "isom.h"
#include "matroska.h"
#include "riff.h"
#include "vorbiscomment.h"
#include "wv.h"

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/dict.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/lfg.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/random_seed.h"
#include "libavutil/samplefmt.h"
#include "libavutil/stereo3d.h"
#include "libavutil/spherical.h"

#include "libavcodec/xiph.h"
#include "libavcodec/mpeg4audio.h"

typedef struct ebml_master {
    int64_t         pos;                ///< absolute offset in the file where the master's elements start
    int             sizebytes;          ///< how many bytes were reserved for the size
} ebml_master;

typedef struct mkv_seekhead_entry {
    unsigned int    elementid;
    uint64_t        segmentpos;
} mkv_seekhead_entry;

typedef struct mkv_seekhead {
    int64_t                 filepos;
    int64_t                 segment_offset;     ///< the file offset to the beginning of the segment
    int                     reserved_size;      ///< -1 if appending to file
    int                     max_entries;
    mkv_seekhead_entry      *entries;
    int                     num_entries;
} mkv_seekhead;

typedef struct mkv_cuepoint {
    uint64_t        pts;
    int             tracknum;
    int64_t         cluster_pos;        ///< file offset of the cluster containing the block
} mkv_cuepoint;

typedef struct mkv_cues {
    int64_t         segment_offset;
    mkv_cuepoint    *entries;
    int             num_entries;
} mkv_cues;

typedef struct mkv_track {
    int             write_dts;
    int             sample_rate;
    int64_t         sample_rate_offset;
    int64_t         codecpriv_offset;
    int64_t         ts_offset;
} mkv_track;

#define MODE_MATROSKAv2 0x01
#define MODE_WEBM       0x02

typedef struct MatroskaMuxContext {
    const AVClass  *class;
    int             mode;
    AVIOContext   *dyn_bc;
    ebml_master     segment;
    int64_t         segment_offset;
    ebml_master     cluster;
    int64_t         cluster_pos;        ///< file offset of the current cluster
    int64_t         cluster_pts;
    int64_t         duration_offset;
    int64_t         duration;
    mkv_seekhead    *main_seekhead;
    mkv_cues        *cues;
    mkv_track       *tracks;

    AVPacket        cur_audio_pkt;

    int have_attachments;
    int have_video;

    int reserve_cues_space;
    int cluster_size_limit;
    int64_t cues_pos;
    int64_t cluster_time_limit;
    int wrote_chapters;
} MatroskaMuxContext;


/** 2 bytes * 3 for EBML IDs, 3 1-byte EBML lengths, 8 bytes for 64 bit
 * offset, 4 bytes for target EBML ID */
#define MAX_SEEKENTRY_SIZE 21

/** per-cuepoint-track - 3 1-byte EBML IDs, 3 1-byte EBML sizes, 2
 * 8-byte uint max */
#define MAX_CUETRACKPOS_SIZE 22

/** per-cuepoint - 2 1-byte EBML IDs, 2 1-byte EBML sizes, 8-byte uint max */
#define MAX_CUEPOINT_SIZE(num_tracks) 12 + MAX_CUETRACKPOS_SIZE * num_tracks

static int ebml_id_size(unsigned int id)
{
    return (av_log2(id + 1) - 1) / 7 + 1;
}

static void put_ebml_id(AVIOContext *pb, unsigned int id)
{
    int i = ebml_id_size(id);
    while (i--)
        avio_w8(pb, id >> (i * 8));
}

/**
 * Write an EBML size meaning "unknown size".
 *
 * @param bytes The number of bytes the size should occupy (maximum: 8).
 */
static void put_ebml_size_unknown(AVIOContext *pb, int bytes)
{
    assert(bytes <= 8);
    avio_w8(pb, 0x1ff >> bytes);
    while (--bytes)
        avio_w8(pb, 0xff);
}

/**
 * Calculate how many bytes are needed to represent a given number in EBML.
 */
static int ebml_num_size(uint64_t num)
{
    int bytes = 1;
    while ((num + 1) >> bytes * 7)
        bytes++;
    return bytes;
}

/**
 * Write a number in EBML variable length format.
 *
 * @param bytes The number of bytes that need to be used to write the number.
 *              If zero, any number of bytes can be used.
 */
static void put_ebml_num(AVIOContext *pb, uint64_t num, int bytes)
{
    int i, needed_bytes = ebml_num_size(num);

    // sizes larger than this are currently undefined in EBML
    assert(num < (1ULL << 56) - 1);

    if (bytes == 0)
        // don't care how many bytes are used, so use the min
        bytes = needed_bytes;
    // the bytes needed to write the given size would exceed the bytes
    // that we need to use, so write unknown size. This shouldn't happen.
    assert(bytes >= needed_bytes);

    num |= 1ULL << bytes * 7;
    for (i = bytes - 1; i >= 0; i--)
        avio_w8(pb, num >> i * 8);
}

static void put_ebml_uint(AVIOContext *pb, unsigned int elementid, uint64_t val)
{
    int i, bytes = 1;
    uint64_t tmp = val;
    while (tmp >>= 8)
        bytes++;

    put_ebml_id(pb, elementid);
    put_ebml_num(pb, bytes, 0);
    for (i = bytes - 1; i >= 0; i--)
        avio_w8(pb, val >> i * 8);
}

static void put_ebml_float(AVIOContext *pb, unsigned int elementid, double val)
{
    put_ebml_id(pb, elementid);
    put_ebml_num(pb, 8, 0);
    avio_wb64(pb, av_double2int(val));
}

static void put_ebml_binary(AVIOContext *pb, unsigned int elementid,
                            const void *buf, int size)
{
    put_ebml_id(pb, elementid);
    put_ebml_num(pb, size, 0);
    avio_write(pb, buf, size);
}

static void put_ebml_string(AVIOContext *pb, unsigned int elementid,
                            const char *str)
{
    put_ebml_binary(pb, elementid, str, strlen(str));
}

/**
 * Write a void element of a given size. Useful for reserving space in
 * the file to be written to later.
 *
 * @param size The number of bytes to reserve, which must be at least 2.
 */
static void put_ebml_void(AVIOContext *pb, uint64_t size)
{
    int64_t currentpos = avio_tell(pb);

    assert(size >= 2);

    put_ebml_id(pb, EBML_ID_VOID);
    // we need to subtract the length needed to store the size from the
    // size we need to reserve so 2 cases, we use 8 bytes to store the
    // size if possible, 1 byte otherwise
    if (size < 10)
        put_ebml_num(pb, size - 1, 0);
    else
        put_ebml_num(pb, size - 9, 8);
    while (avio_tell(pb) < currentpos + size)
        avio_w8(pb, 0);
}

static ebml_master start_ebml_master(AVIOContext *pb, unsigned int elementid,
                                     uint64_t expectedsize)
{
    int bytes = expectedsize ? ebml_num_size(expectedsize) : 8;
    put_ebml_id(pb, elementid);
    put_ebml_size_unknown(pb, bytes);
    return (ebml_master) {avio_tell(pb), bytes };
}

static void end_ebml_master(AVIOContext *pb, ebml_master master)
{
    int64_t pos = avio_tell(pb);

    if (avio_seek(pb, master.pos - master.sizebytes, SEEK_SET) < 0)
        return;
    put_ebml_num(pb, pos - master.pos, master.sizebytes);
    avio_seek(pb, pos, SEEK_SET);
}

static void put_xiph_size(AVIOContext *pb, int size)
{
    int i;
    for (i = 0; i < size / 255; i++)
        avio_w8(pb, 255);
    avio_w8(pb, size % 255);
}

/**
 * Initialize a mkv_seekhead element to be ready to index level 1 Matroska
 * elements. If a maximum number of elements is specified, enough space
 * will be reserved at the current file location to write a seek head of
 * that size.
 *
 * @param segment_offset The absolute offset to the position in the file
 *                       where the segment begins.
 * @param numelements The maximum number of elements that will be indexed
 *                    by this seek head, 0 if unlimited.
 */
static mkv_seekhead *mkv_start_seekhead(AVIOContext *pb, int64_t segment_offset,
                                        int numelements)
{
    mkv_seekhead *new_seekhead = av_mallocz(sizeof(mkv_seekhead));
    if (!new_seekhead)
        return NULL;

    new_seekhead->segment_offset = segment_offset;

    if (numelements > 0) {
        new_seekhead->filepos = avio_tell(pb);
        // 21 bytes max for a seek entry, 10 bytes max for the SeekHead ID
        // and size, and 3 bytes to guarantee that an EBML void element
        // will fit afterwards
        new_seekhead->reserved_size = numelements * MAX_SEEKENTRY_SIZE + 13;
        new_seekhead->max_entries   = numelements;
        put_ebml_void(pb, new_seekhead->reserved_size);
    }
    return new_seekhead;
}

static int mkv_add_seekhead_entry(mkv_seekhead *seekhead, unsigned int elementid, uint64_t filepos)
{
    int err;

    // don't store more elements than we reserved space for
    if (seekhead->max_entries > 0 && seekhead->max_entries <= seekhead->num_entries)
        return -1;

    if ((err = av_reallocp_array(&seekhead->entries, seekhead->num_entries + 1,
                                 sizeof(*seekhead->entries))) < 0) {
        seekhead->num_entries = 0;
        return err;
    }

    seekhead->entries[seekhead->num_entries].elementid    = elementid;
    seekhead->entries[seekhead->num_entries++].segmentpos = filepos - seekhead->segment_offset;

    return 0;
}

/**
 * Write the seek head to the file and free it. If a maximum number of
 * elements was specified to mkv_start_seekhead(), the seek head will
 * be written at the location reserved for it. Otherwise, it is written
 * at the current location in the file.
 *
 * @return The file offset where the seekhead was written,
 * -1 if an error occurred.
 */
static int64_t mkv_write_seekhead(AVIOContext *pb, mkv_seekhead *seekhead)
{
    ebml_master metaseek, seekentry;
    int64_t currentpos;
    int i;

    currentpos = avio_tell(pb);

    if (seekhead->reserved_size > 0) {
        if (avio_seek(pb, seekhead->filepos, SEEK_SET) < 0) {
            currentpos = -1;
            goto fail;
        }
    }

    metaseek = start_ebml_master(pb, MATROSKA_ID_SEEKHEAD, seekhead->reserved_size);
    for (i = 0; i < seekhead->num_entries; i++) {
        mkv_seekhead_entry *entry = &seekhead->entries[i];

        seekentry = start_ebml_master(pb, MATROSKA_ID_SEEKENTRY, MAX_SEEKENTRY_SIZE);

        put_ebml_id(pb, MATROSKA_ID_SEEKID);
        put_ebml_num(pb, ebml_id_size(entry->elementid), 0);
        put_ebml_id(pb, entry->elementid);

        put_ebml_uint(pb, MATROSKA_ID_SEEKPOSITION, entry->segmentpos);
        end_ebml_master(pb, seekentry);
    }
    end_ebml_master(pb, metaseek);

    if (seekhead->reserved_size > 0) {
        uint64_t remaining = seekhead->filepos + seekhead->reserved_size - avio_tell(pb);
        put_ebml_void(pb, remaining);
        avio_seek(pb, currentpos, SEEK_SET);

        currentpos = seekhead->filepos;
    }
fail:
    av_free(seekhead->entries);
    av_free(seekhead);

    return currentpos;
}

static mkv_cues *mkv_start_cues(int64_t segment_offset)
{
    mkv_cues *cues = av_mallocz(sizeof(mkv_cues));
    if (!cues)
        return NULL;

    cues->segment_offset = segment_offset;
    return cues;
}

static int mkv_add_cuepoint(mkv_cues *cues, int stream, int64_t ts, int64_t cluster_pos)
{
    int err;

    if (ts < 0)
        return 0;

    if ((err = av_reallocp_array(&cues->entries, cues->num_entries + 1,
                                 sizeof(*cues->entries))) < 0) {
        cues->num_entries = 0;
        return err;
    }

    cues->entries[cues->num_entries].pts           = ts;
    cues->entries[cues->num_entries].tracknum      = stream + 1;
    cues->entries[cues->num_entries++].cluster_pos = cluster_pos - cues->segment_offset;

    return 0;
}

static int64_t mkv_write_cues(AVIOContext *pb, mkv_cues *cues, int num_tracks)
{
    ebml_master cues_element;
    int64_t currentpos;
    int i, j;

    currentpos = avio_tell(pb);
    cues_element = start_ebml_master(pb, MATROSKA_ID_CUES, 0);

    for (i = 0; i < cues->num_entries; i++) {
        ebml_master cuepoint, track_positions;
        mkv_cuepoint *entry = &cues->entries[i];
        uint64_t pts = entry->pts;

        cuepoint = start_ebml_master(pb, MATROSKA_ID_POINTENTRY, MAX_CUEPOINT_SIZE(num_tracks));
        put_ebml_uint(pb, MATROSKA_ID_CUETIME, pts);

        // put all the entries from different tracks that have the exact same
        // timestamp into the same CuePoint
        for (j = 0; j < cues->num_entries - i && entry[j].pts == pts; j++) {
            track_positions = start_ebml_master(pb, MATROSKA_ID_CUETRACKPOSITION, MAX_CUETRACKPOS_SIZE);
            put_ebml_uint(pb, MATROSKA_ID_CUETRACK          , entry[j].tracknum   );
            put_ebml_uint(pb, MATROSKA_ID_CUECLUSTERPOSITION, entry[j].cluster_pos);
            end_ebml_master(pb, track_positions);
        }
        i += j - 1;
        end_ebml_master(pb, cuepoint);
    }
    end_ebml_master(pb, cues_element);

    return currentpos;
}

static int put_xiph_codecpriv(AVFormatContext *s, AVIOContext *pb, AVCodecParameters *par)
{
    uint8_t *header_start[3];
    int header_len[3];
    int first_header_size;
    int j;

    if (par->codec_id == AV_CODEC_ID_VORBIS)
        first_header_size = 30;
    else
        first_header_size = 42;

    if (avpriv_split_xiph_headers(par->extradata, par->extradata_size,
                              first_header_size, header_start, header_len) < 0) {
        av_log(s, AV_LOG_ERROR, "Extradata corrupt.\n");
        return -1;
    }

    avio_w8(pb, 2);                    // number packets - 1
    for (j = 0; j < 2; j++) {
        put_xiph_size(pb, header_len[j]);
    }
    for (j = 0; j < 3; j++)
        avio_write(pb, header_start[j], header_len[j]);

    return 0;
}

static int put_wv_codecpriv(AVIOContext *pb, AVCodecParameters *par)
{
    if (par->extradata && par->extradata_size == 2)
        avio_write(pb, par->extradata, 2);
    else
        avio_wl16(pb, 0x403); // fallback to the version mentioned in matroska specs
    return 0;
}

static int put_flac_codecpriv(AVFormatContext *s,
                              AVIOContext *pb, AVCodecParameters *par)
{
    int write_comment = (par->channel_layout &&
                         !(par->channel_layout & ~0x3ffffULL) &&
                         !ff_flac_is_native_layout(par->channel_layout));
    int ret = ff_flac_write_header(pb, par->extradata, par->extradata_size,
                                   !write_comment);

    if (ret < 0)
        return ret;

    if (write_comment) {
        const char *vendor = (s->flags & AVFMT_FLAG_BITEXACT) ?
                             "Libav" : LIBAVFORMAT_IDENT;
        AVDictionary *dict = NULL;
        uint8_t buf[32], *data, *p;
        int len;

        snprintf(buf, sizeof(buf), "0x%"PRIx64, par->channel_layout);
        av_dict_set(&dict, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK", buf, 0);

        len = ff_vorbiscomment_length(dict, vendor);
        data = av_malloc(len + 4);
        if (!data) {
            av_dict_free(&dict);
            return AVERROR(ENOMEM);
        }

        data[0] = 0x84;
        AV_WB24(data + 1, len);

        p = data + 4;
        ff_vorbiscomment_write(&p, &dict, vendor);

        avio_write(pb, data, len + 4);

        av_freep(&data);
        av_dict_free(&dict);
    }

    return 0;
}

static int get_aac_sample_rates(AVFormatContext *s, uint8_t *extradata, int extradata_size,
                                int *sample_rate, int *output_sample_rate)
{
    MPEG4AudioConfig mp4ac;
    int ret;

    ret = avpriv_mpeg4audio_get_config(&mp4ac, extradata,
                                       extradata_size * 8, 1);
    /* Don't abort if the failure is because of missing extradata. Assume in that
     * case a bitstream filter will provide the muxer with the extradata in the
     * first packet.
     * Abort however if s->pb is not seekable, as we would not be able to seek back
     * to write the sample rate elements once the extradata shows up, anyway. */
    if (ret < 0 && (extradata_size || !(s->pb->seekable & AVIO_SEEKABLE_NORMAL))) {
        av_log(s, AV_LOG_ERROR,
               "Error parsing AAC extradata, unable to determine samplerate.\n");
        return AVERROR(EINVAL);
    }

    if (ret < 0) {
        /* This will only happen when this function is called while writing the
         * header and no extradata is available. The space for this element has
         * to be reserved for when this function is called again after the
         * extradata shows up in the first packet, as there's no way to know if
         * output_sample_rate will be different than sample_rate or not. */
        *output_sample_rate = *sample_rate;
    } else {
        *sample_rate        = mp4ac.sample_rate;
        *output_sample_rate = mp4ac.ext_sample_rate;
    }
    return 0;
}

static int mkv_write_native_codecprivate(AVFormatContext *s,
                                         AVCodecParameters *par,
                                         AVIOContext *dyn_cp)
{
    switch (par->codec_id) {
    case AV_CODEC_ID_VORBIS:
    case AV_CODEC_ID_THEORA:
        return put_xiph_codecpriv(s, dyn_cp, par);
    case AV_CODEC_ID_FLAC:
        return put_flac_codecpriv(s, dyn_cp, par);
    case AV_CODEC_ID_WAVPACK:
        return put_wv_codecpriv(dyn_cp, par);
    case AV_CODEC_ID_H264:
        return ff_isom_write_avcc(dyn_cp, par->extradata,
                                  par->extradata_size);
    case AV_CODEC_ID_HEVC:
        return ff_isom_write_hvcc(dyn_cp, par->extradata,
                                  par->extradata_size, 0);
    case AV_CODEC_ID_ALAC:
        if (par->extradata_size < 36) {
            av_log(s, AV_LOG_ERROR,
                   "Invalid extradata found, ALAC expects a 36-byte "
                   "QuickTime atom.");
            return AVERROR_INVALIDDATA;
        } else
            avio_write(dyn_cp, par->extradata + 12,
                       par->extradata_size - 12);
        break;
    default:
        if (par->extradata_size)
        avio_write(dyn_cp, par->extradata, par->extradata_size);
    }

    return 0;
}

static int mkv_write_codecprivate(AVFormatContext *s, AVIOContext *pb,
                                  AVCodecParameters *par,
                                  int native_id, int qt_id)
{
    AVIOContext *dyn_cp;
    uint8_t *codecpriv;
    int ret, codecpriv_size;

    ret = avio_open_dyn_buf(&dyn_cp);
    if (ret < 0)
        return ret;

    if (native_id) {
        ret = mkv_write_native_codecprivate(s, par, dyn_cp);
    } else if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (qt_id) {
            if (!par->codec_tag)
                par->codec_tag = ff_codec_get_tag(ff_codec_movvideo_tags,
                                                  par->codec_id);
            if (par->extradata_size)
                avio_write(dyn_cp, par->extradata, par->extradata_size);
        } else {
            if (!par->codec_tag)
                par->codec_tag = ff_codec_get_tag(ff_codec_bmp_tags,
                                                  par->codec_id);
            if (!par->codec_tag) {
                av_log(s, AV_LOG_ERROR, "No bmp codec ID found.\n");
                ret = -1;
            }

            ff_put_bmp_header(dyn_cp, par, ff_codec_bmp_tags, 0);
        }
    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
        unsigned int tag;
        tag = ff_codec_get_tag(ff_codec_wav_tags, par->codec_id);
        if (!tag) {
            av_log(s, AV_LOG_ERROR, "No wav codec ID found.\n");
            ret = -1;
        }
        if (!par->codec_tag)
            par->codec_tag = tag;

        ff_put_wav_header(s, dyn_cp, par);
    }

    codecpriv_size = avio_close_dyn_buf(dyn_cp, &codecpriv);
    if (codecpriv_size)
        put_ebml_binary(pb, MATROSKA_ID_CODECPRIVATE, codecpriv,
                        codecpriv_size);
    av_free(codecpriv);
    return ret;
}

static int mkv_write_video_projection(AVFormatContext *s, AVIOContext *pb,
                                      AVStream *st)
{
    AVIOContext b;
    AVIOContext *dyn_cp;
    int side_data_size = 0;
    int ret, projection_size;
    uint8_t *projection_ptr;
    uint8_t private[20];

    const AVSphericalMapping *spherical =
        (const AVSphericalMapping *)av_stream_get_side_data(st, AV_PKT_DATA_SPHERICAL,
                                                            &side_data_size);

    if (!side_data_size)
        return 0;

    ret = avio_open_dyn_buf(&dyn_cp);
    if (ret < 0)
        return ret;

    switch (spherical->projection) {
    case AV_SPHERICAL_EQUIRECTANGULAR:
        put_ebml_uint(dyn_cp, MATROSKA_ID_VIDEOPROJECTIONTYPE,
                      MATROSKA_VIDEO_PROJECTION_TYPE_EQUIRECTANGULAR);
        break;
    case AV_SPHERICAL_EQUIRECTANGULAR_TILE:
        ffio_init_context(&b, private, 20, 1, NULL, NULL, NULL, NULL);
        put_ebml_uint(dyn_cp, MATROSKA_ID_VIDEOPROJECTIONTYPE,
                      MATROSKA_VIDEO_PROJECTION_TYPE_EQUIRECTANGULAR);
        avio_wb32(&b, 0); // version + flags
        avio_wb32(&b, spherical->bound_top);
        avio_wb32(&b, spherical->bound_bottom);
        avio_wb32(&b, spherical->bound_left);
        avio_wb32(&b, spherical->bound_right);
        put_ebml_binary(dyn_cp, MATROSKA_ID_VIDEOPROJECTIONPRIVATE,
                        private, avio_tell(&b));
        break;
    case AV_SPHERICAL_CUBEMAP:
        ffio_init_context(&b, private, 12, 1, NULL, NULL, NULL, NULL);
        put_ebml_uint(dyn_cp, MATROSKA_ID_VIDEOPROJECTIONTYPE,
                      MATROSKA_VIDEO_PROJECTION_TYPE_CUBEMAP);
        avio_wb32(&b, 0); // version + flags
        avio_wb32(&b, 0); // layout
        avio_wb32(&b, spherical->padding);
        put_ebml_binary(dyn_cp, MATROSKA_ID_VIDEOPROJECTIONPRIVATE,
                        private, avio_tell(&b));
        break;
    default:
        av_log(s, AV_LOG_WARNING, "Unknown projection type\n");
        goto end;
    }

    if (spherical->yaw)
        put_ebml_float(dyn_cp, MATROSKA_ID_VIDEOPROJECTIONPOSEYAW,
                       (double) spherical->yaw   / (1 << 16));
    if (spherical->pitch)
        put_ebml_float(dyn_cp, MATROSKA_ID_VIDEOPROJECTIONPOSEPITCH,
                       (double) spherical->pitch / (1 << 16));
    if (spherical->roll)
        put_ebml_float(dyn_cp, MATROSKA_ID_VIDEOPROJECTIONPOSEROLL,
                       (double) spherical->roll  / (1 << 16));

end:
    projection_size = avio_close_dyn_buf(dyn_cp, &projection_ptr);
    if (projection_size) {
        ebml_master projection = start_ebml_master(pb,
                                                   MATROSKA_ID_VIDEOPROJECTION,
                                                   projection_size);
        avio_write(pb, projection_ptr, projection_size);
        end_ebml_master(pb, projection);
    }
    av_freep(&projection_ptr);

    return 0;
}

static void mkv_write_field_order(AVIOContext *pb,
                                  enum AVFieldOrder field_order)
{
    switch (field_order) {
    case AV_FIELD_UNKNOWN:
        put_ebml_uint(pb, MATROSKA_ID_VIDEOFLAGINTERLACED,
                      MATROSKA_VIDEO_INTERLACE_FLAG_UNDETERMINED);
        break;
    case AV_FIELD_PROGRESSIVE:
        put_ebml_uint(pb, MATROSKA_ID_VIDEOFLAGINTERLACED,
                      MATROSKA_VIDEO_INTERLACE_FLAG_PROGRESSIVE);
        break;
    case AV_FIELD_TT:
    case AV_FIELD_BB:
    case AV_FIELD_TB:
    case AV_FIELD_BT:
        put_ebml_uint(pb, MATROSKA_ID_VIDEOFLAGINTERLACED,
                      MATROSKA_VIDEO_INTERLACE_FLAG_INTERLACED);
        switch (field_order) {
        case AV_FIELD_TT:
            put_ebml_uint(pb, MATROSKA_ID_VIDEOFIELDORDER,
                          MATROSKA_VIDEO_FIELDORDER_TT);
            break;
        case AV_FIELD_BB:
             put_ebml_uint(pb, MATROSKA_ID_VIDEOFIELDORDER,
                          MATROSKA_VIDEO_FIELDORDER_BB);
            break;
        case AV_FIELD_TB:
            put_ebml_uint(pb, MATROSKA_ID_VIDEOFIELDORDER,
                          MATROSKA_VIDEO_FIELDORDER_TB);
            break;
        case AV_FIELD_BT:
            put_ebml_uint(pb, MATROSKA_ID_VIDEOFIELDORDER,
                          MATROSKA_VIDEO_FIELDORDER_BT);
            break;
        }
    }
}

static int mkv_write_stereo_mode(AVFormatContext *s, AVIOContext *pb,
                                 AVStream *st, int mode)
{
    int i;
    int display_width, display_height;
    int h_width = 1, h_height = 1;
    AVCodecParameters *par = st->codecpar;
    AVDictionaryEntry *tag;
    MatroskaVideoStereoModeType format = MATROSKA_VIDEO_STEREOMODE_TYPE_NB;

    // convert metadata into proper side data and add it to the stream
    if ((tag = av_dict_get(s->metadata, "stereo_mode", NULL, 0))) {
        int stereo_mode = atoi(tag->value);
        if (stereo_mode < MATROSKA_VIDEO_STEREOMODE_TYPE_NB &&
            stereo_mode != 10 && stereo_mode != 12) {
            int ret = ff_mkv_stereo3d_conv(st, stereo_mode);
            if (ret < 0)
                return ret;
        }
    }

    // iterate to find the stereo3d side data
    for (i = 0; i < st->nb_side_data; i++) {
        AVPacketSideData sd = st->side_data[i];
        if (sd.type == AV_PKT_DATA_STEREO3D) {
            AVStereo3D *stereo = (AVStereo3D *)sd.data;

            switch (stereo->type) {
            case AV_STEREO3D_2D:
                format = MATROSKA_VIDEO_STEREOMODE_TYPE_MONO;
                break;
            case AV_STEREO3D_SIDEBYSIDE:
                format = (stereo->flags & AV_STEREO3D_FLAG_INVERT)
                    ? MATROSKA_VIDEO_STEREOMODE_TYPE_RIGHT_LEFT
                    : MATROSKA_VIDEO_STEREOMODE_TYPE_LEFT_RIGHT;
                h_width = 2;
                break;
            case AV_STEREO3D_TOPBOTTOM:
                format = MATROSKA_VIDEO_STEREOMODE_TYPE_TOP_BOTTOM;
                if (stereo->flags & AV_STEREO3D_FLAG_INVERT)
                    format--;
                h_height = 2;
                break;
            case AV_STEREO3D_CHECKERBOARD:
                format = MATROSKA_VIDEO_STEREOMODE_TYPE_CHECKERBOARD_LR;
                if (stereo->flags & AV_STEREO3D_FLAG_INVERT)
                    format--;
                break;
            case AV_STEREO3D_LINES:
                format = MATROSKA_VIDEO_STEREOMODE_TYPE_ROW_INTERLEAVED_LR;
                if (stereo->flags & AV_STEREO3D_FLAG_INVERT)
                    format--;
                h_height = 2;
                break;
            case AV_STEREO3D_COLUMNS:
                format = MATROSKA_VIDEO_STEREOMODE_TYPE_COL_INTERLEAVED_LR;
                if (stereo->flags & AV_STEREO3D_FLAG_INVERT)
                    format--;
                h_width = 2;
                break;
            case AV_STEREO3D_FRAMESEQUENCE:
                format = MATROSKA_VIDEO_STEREOMODE_TYPE_BOTH_EYES_BLOCK_LR;
                if (stereo->flags & AV_STEREO3D_FLAG_INVERT)
                    format++;
                break;
            }

            break;
        }
    }

    // if webm, do not write unsupported modes
    if (mode == MODE_WEBM &&
        (format > MATROSKA_VIDEO_STEREOMODE_TYPE_TOP_BOTTOM &&
         format != MATROSKA_VIDEO_STEREOMODE_TYPE_RIGHT_LEFT))
        format = MATROSKA_VIDEO_STEREOMODE_TYPE_NB;

    // write StereoMode if format is valid
    if (format < MATROSKA_VIDEO_STEREOMODE_TYPE_NB)
        put_ebml_uint(pb, MATROSKA_ID_VIDEOSTEREOMODE, format);

    // write DisplayWidth and DisplayHeight, they contain the size of
    // a single source view and/or the display aspect ratio
    display_width  = par->width  / h_width;
    display_height = par->height / h_height;
    if (st->sample_aspect_ratio.num) {
        display_width *= av_q2d(st->sample_aspect_ratio);
        put_ebml_uint(pb, MATROSKA_ID_VIDEODISPLAYUNIT, 3); // DAR
    }
    if (st->sample_aspect_ratio.num ||
        format < MATROSKA_VIDEO_STEREOMODE_TYPE_NB) {
        put_ebml_uint(pb, MATROSKA_ID_VIDEODISPLAYWIDTH,  display_width);
        put_ebml_uint(pb, MATROSKA_ID_VIDEODISPLAYHEIGHT, display_height);
    }

    return 0;
}

static int mkv_write_track(AVFormatContext *s, MatroskaMuxContext *mkv,
                           int i, AVIOContext *pb)
{
    AVStream *st = s->streams[i];
    AVCodecParameters *par = st->codecpar;
    ebml_master subinfo, track;
    int native_id = 0;
    int qt_id = 0;
    int bit_depth = av_get_bits_per_sample(par->codec_id);
    int sample_rate = par->sample_rate;
    int output_sample_rate = 0;
    int j, ret;
    AVDictionaryEntry *tag;

    // ms precision is the de-facto standard timescale for mkv files
    avpriv_set_pts_info(st, 64, 1, 1000);

    if (par->codec_type == AVMEDIA_TYPE_ATTACHMENT) {
        mkv->have_attachments = 1;
        return 0;
    }

    if (!bit_depth)
        bit_depth = av_get_bytes_per_sample(par->format) << 3;

    if (par->codec_id == AV_CODEC_ID_AAC) {
        ret = get_aac_sample_rates(s, par->extradata, par->extradata_size, &sample_rate,
                                   &output_sample_rate);
        if (ret < 0)
            return ret;
    }

    track = start_ebml_master(pb, MATROSKA_ID_TRACKENTRY, 0);
    put_ebml_uint (pb, MATROSKA_ID_TRACKNUMBER     , i + 1);
    put_ebml_uint (pb, MATROSKA_ID_TRACKUID        , i + 1);
    put_ebml_uint (pb, MATROSKA_ID_TRACKFLAGLACING , 0);    // no lacing (yet)

    if ((tag = av_dict_get(st->metadata, "title", NULL, 0)))
        put_ebml_string(pb, MATROSKA_ID_TRACKNAME, tag->value);
    tag = av_dict_get(st->metadata, "language", NULL, 0);
    put_ebml_string(pb, MATROSKA_ID_TRACKLANGUAGE, tag ? tag->value:"und");

    // The default value for TRACKFLAGDEFAULT is 1, so add element
    // if we need to clear it.
    if (!(st->disposition & AV_DISPOSITION_DEFAULT))
        put_ebml_uint(pb, MATROSKA_ID_TRACKFLAGDEFAULT, !!(st->disposition & AV_DISPOSITION_DEFAULT));
    if (st->disposition & AV_DISPOSITION_FORCED)
        put_ebml_uint(pb, MATROSKA_ID_TRACKFLAGFORCED, !!(st->disposition & AV_DISPOSITION_FORCED));

    if (par->codec_type == AVMEDIA_TYPE_AUDIO && par->initial_padding) {
        mkv->tracks[i].ts_offset = av_rescale_q(par->initial_padding,
                                                (AVRational){ 1, par->sample_rate },
                                                st->time_base);

        put_ebml_uint(pb, MATROSKA_ID_CODECDELAY,
                      av_rescale_q(par->initial_padding,
                                   (AVRational){ 1, par->sample_rate },
                                   (AVRational){ 1, 1000000000 }));
    }

    // look for a codec ID string specific to mkv to use,
    // if none are found, use AVI codes
    for (j = 0; ff_mkv_codec_tags[j].id != AV_CODEC_ID_NONE; j++) {
        if (ff_mkv_codec_tags[j].id == par->codec_id) {
            put_ebml_string(pb, MATROSKA_ID_CODECID, ff_mkv_codec_tags[j].str);
            native_id = 1;
            break;
        }
    }

    if (mkv->mode == MODE_WEBM && !(par->codec_id == AV_CODEC_ID_VP8 ||
                                    par->codec_id == AV_CODEC_ID_VP9 ||
                                    par->codec_id == AV_CODEC_ID_OPUS ||
                                    par->codec_id == AV_CODEC_ID_VORBIS)) {
        av_log(s, AV_LOG_ERROR,
               "Only VP8 or VP9 video and Vorbis or Opus audio are supported for WebM.\n");
        return AVERROR(EINVAL);
    }

    switch (par->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        mkv->have_video = 1;
        put_ebml_uint(pb, MATROSKA_ID_TRACKTYPE, MATROSKA_TRACK_TYPE_VIDEO);
        if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
            put_ebml_uint(pb, MATROSKA_ID_TRACKDEFAULTDURATION, 1E9 / av_q2d(st->avg_frame_rate));

        if (!native_id &&
            ff_codec_get_tag(ff_codec_movvideo_tags, par->codec_id) &&
            (!ff_codec_get_tag(ff_codec_bmp_tags,    par->codec_id) ||
             par->codec_id == AV_CODEC_ID_SVQ1 ||
             par->codec_id == AV_CODEC_ID_SVQ3 ||
             par->codec_id == AV_CODEC_ID_CINEPAK))
            qt_id = 1;

        if (qt_id)
            put_ebml_string(pb, MATROSKA_ID_CODECID, "V_QUICKTIME");
        else if (!native_id) {
            // if there is no mkv-specific codec ID, use VFW mode
            put_ebml_string(pb, MATROSKA_ID_CODECID, "V_MS/VFW/FOURCC");
            mkv->tracks[i].write_dts = 1;
        }

        subinfo = start_ebml_master(pb, MATROSKA_ID_TRACKVIDEO, 0);

        put_ebml_uint (pb, MATROSKA_ID_VIDEOPIXELWIDTH , par->width);
        put_ebml_uint (pb, MATROSKA_ID_VIDEOPIXELHEIGHT, par->height);

        mkv_write_field_order(pb, par->field_order);

        // check both side data and metadata for stereo information,
        // write the result to the bitstream if any is found
        ret = mkv_write_stereo_mode(s, pb, st, mkv->mode);
        if (ret < 0)
            return ret;
        ret = mkv_write_video_projection(s, pb, st);
        if (ret < 0)
            return ret;

        end_ebml_master(pb, subinfo);
        break;

    case AVMEDIA_TYPE_AUDIO:
        put_ebml_uint(pb, MATROSKA_ID_TRACKTYPE, MATROSKA_TRACK_TYPE_AUDIO);

        if (!native_id)
            // no mkv-specific ID, use ACM mode
            put_ebml_string(pb, MATROSKA_ID_CODECID, "A_MS/ACM");

        subinfo = start_ebml_master(pb, MATROSKA_ID_TRACKAUDIO, 0);
        put_ebml_uint  (pb, MATROSKA_ID_AUDIOCHANNELS    , par->channels);

        mkv->tracks[i].sample_rate_offset = avio_tell(pb);
        put_ebml_float (pb, MATROSKA_ID_AUDIOSAMPLINGFREQ, sample_rate);
        if (output_sample_rate)
            put_ebml_float(pb, MATROSKA_ID_AUDIOOUTSAMPLINGFREQ, output_sample_rate);
        if (bit_depth)
            put_ebml_uint(pb, MATROSKA_ID_AUDIOBITDEPTH, bit_depth);
        end_ebml_master(pb, subinfo);
        break;

    case AVMEDIA_TYPE_SUBTITLE:
        put_ebml_uint(pb, MATROSKA_ID_TRACKTYPE, MATROSKA_TRACK_TYPE_SUBTITLE);
        if (!native_id) {
            av_log(s, AV_LOG_ERROR, "Subtitle codec %d is not supported.\n", par->codec_id);
            return AVERROR(ENOSYS);
        }
        break;
    default:
        av_log(s, AV_LOG_ERROR, "Only audio, video, and subtitles are supported for Matroska.\n");
        break;
    }

    mkv->tracks[i].codecpriv_offset = avio_tell(pb);
    ret = mkv_write_codecprivate(s, pb, par, native_id, qt_id);
    if (ret < 0)
        return ret;

    end_ebml_master(pb, track);

    return 0;
}

static int mkv_write_tracks(AVFormatContext *s)
{
    MatroskaMuxContext *mkv = s->priv_data;
    AVIOContext *pb = s->pb;
    ebml_master tracks;
    int i, ret;

    ret = mkv_add_seekhead_entry(mkv->main_seekhead, MATROSKA_ID_TRACKS, avio_tell(pb));
    if (ret < 0)
        return ret;

    tracks = start_ebml_master(pb, MATROSKA_ID_TRACKS, 0);
    for (i = 0; i < s->nb_streams; i++) {
        ret = mkv_write_track(s, mkv, i, pb);
        if (ret < 0)
            return ret;
    }
    end_ebml_master(pb, tracks);
    return 0;
}

static int mkv_write_chapters(AVFormatContext *s)
{
    MatroskaMuxContext *mkv = s->priv_data;
    AVIOContext *pb = s->pb;
    ebml_master chapters, editionentry;
    AVRational scale = {1, 1E9};
    int i, ret;

    if (!s->nb_chapters || mkv->wrote_chapters)
        return 0;

    ret = mkv_add_seekhead_entry(mkv->main_seekhead, MATROSKA_ID_CHAPTERS, avio_tell(pb));
    if (ret < 0) return ret;

    chapters     = start_ebml_master(pb, MATROSKA_ID_CHAPTERS    , 0);
    editionentry = start_ebml_master(pb, MATROSKA_ID_EDITIONENTRY, 0);
    put_ebml_uint(pb, MATROSKA_ID_EDITIONFLAGDEFAULT, 1);
    put_ebml_uint(pb, MATROSKA_ID_EDITIONFLAGHIDDEN , 0);
    for (i = 0; i < s->nb_chapters; i++) {
        ebml_master chapteratom, chapterdisplay;
        AVChapter *c     = s->chapters[i];
        int64_t chapterstart = av_rescale_q(c->start, c->time_base, scale);
        int64_t chapterend   = av_rescale_q(c->end,   c->time_base, scale);
        AVDictionaryEntry *t = NULL;
        if (chapterstart < 0 || chapterstart > chapterend || chapterend < 0) {
            av_log(s, AV_LOG_ERROR,
                   "Invalid chapter start (%"PRId64") or end (%"PRId64").\n",
                   chapterstart, chapterend);
            return AVERROR_INVALIDDATA;
        }

        chapteratom = start_ebml_master(pb, MATROSKA_ID_CHAPTERATOM, 0);
        put_ebml_uint(pb, MATROSKA_ID_CHAPTERUID, c->id);
        put_ebml_uint(pb, MATROSKA_ID_CHAPTERTIMESTART, chapterstart);
        put_ebml_uint(pb, MATROSKA_ID_CHAPTERTIMEEND, chapterend);
        put_ebml_uint(pb, MATROSKA_ID_CHAPTERFLAGHIDDEN , 0);
        put_ebml_uint(pb, MATROSKA_ID_CHAPTERFLAGENABLED, 1);
        if ((t = av_dict_get(c->metadata, "title", NULL, 0))) {
            chapterdisplay = start_ebml_master(pb, MATROSKA_ID_CHAPTERDISPLAY, 0);
            put_ebml_string(pb, MATROSKA_ID_CHAPSTRING, t->value);
            put_ebml_string(pb, MATROSKA_ID_CHAPLANG  , "und");
            end_ebml_master(pb, chapterdisplay);
        }
        end_ebml_master(pb, chapteratom);
    }
    end_ebml_master(pb, editionentry);
    end_ebml_master(pb, chapters);

    mkv->wrote_chapters = 1;
    return 0;
}

static int mkv_write_simpletag(AVIOContext *pb, AVDictionaryEntry *t)
{
    uint8_t *key = av_strdup(t->key);
    uint8_t *p   = key;
    const uint8_t *lang = NULL;
    ebml_master tag;

    if (!key)
        return AVERROR(ENOMEM);

    if ((p = strrchr(p, '-')) &&
        (lang = av_convert_lang_to(p + 1, AV_LANG_ISO639_2_BIBL)))
        *p = 0;

    p = key;
    while (*p) {
        if (*p == ' ')
            *p = '_';
        else if (*p >= 'a' && *p <= 'z')
            *p -= 'a' - 'A';
        p++;
    }

    tag = start_ebml_master(pb, MATROSKA_ID_SIMPLETAG, 0);
    put_ebml_string(pb, MATROSKA_ID_TAGNAME, key);
    if (lang)
        put_ebml_string(pb, MATROSKA_ID_TAGLANG, lang);
    put_ebml_string(pb, MATROSKA_ID_TAGSTRING, t->value);
    end_ebml_master(pb, tag);

    av_freep(&key);
    return 0;
}

static int mkv_write_tag(AVFormatContext *s, AVDictionary *m, unsigned int elementid,
                         unsigned int uid, ebml_master *tags)
{
    MatroskaMuxContext *mkv = s->priv_data;
    ebml_master tag, targets;
    AVDictionaryEntry *t = NULL;
    int ret;

    if (!tags->pos) {
        ret = mkv_add_seekhead_entry(mkv->main_seekhead, MATROSKA_ID_TAGS, avio_tell(s->pb));
        if (ret < 0) return ret;

        *tags = start_ebml_master(s->pb, MATROSKA_ID_TAGS, 0);
    }

    tag     = start_ebml_master(s->pb, MATROSKA_ID_TAG,        0);
    targets = start_ebml_master(s->pb, MATROSKA_ID_TAGTARGETS, 0);
    if (elementid)
        put_ebml_uint(s->pb, elementid, uid);
    end_ebml_master(s->pb, targets);

    while ((t = av_dict_get(m, "", t, AV_DICT_IGNORE_SUFFIX))) {
        if (av_strcasecmp(t->key, "title") &&
            av_strcasecmp(t->key, "encoding_tool") &&
            (elementid != MATROSKA_ID_TAGTARGETS_TRACKUID ||
             av_strcasecmp(t->key, "language"))) {
            ret = mkv_write_simpletag(s->pb, t);
            if (ret < 0)
                return ret;
        }
    }

    end_ebml_master(s->pb, tag);
    return 0;
}

static int mkv_write_tags(AVFormatContext *s)
{
    ebml_master tags = {0};
    int i, ret;

    ff_metadata_conv_ctx(s, ff_mkv_metadata_conv, NULL);

    if (av_dict_get(s->metadata, "", NULL, AV_DICT_IGNORE_SUFFIX)) {
        ret = mkv_write_tag(s, s->metadata, 0, 0, &tags);
        if (ret < 0) return ret;
    }

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];

        if (!av_dict_get(st->metadata, "", 0, AV_DICT_IGNORE_SUFFIX))
            continue;

        ret = mkv_write_tag(s, st->metadata, MATROSKA_ID_TAGTARGETS_TRACKUID, i + 1, &tags);
        if (ret < 0) return ret;
    }

    for (i = 0; i < s->nb_chapters; i++) {
        AVChapter *ch = s->chapters[i];

        if (!av_dict_get(ch->metadata, "", NULL, AV_DICT_IGNORE_SUFFIX))
            continue;

        ret = mkv_write_tag(s, ch->metadata, MATROSKA_ID_TAGTARGETS_CHAPTERUID, ch->id, &tags);
        if (ret < 0) return ret;
    }

    if (tags.pos)
        end_ebml_master(s->pb, tags);
    return 0;
}

static int mkv_write_attachments(AVFormatContext *s)
{
    MatroskaMuxContext *mkv = s->priv_data;
    AVIOContext *pb = s->pb;
    ebml_master attachments;
    AVLFG c;
    int i, ret;

    if (!mkv->have_attachments)
        return 0;

    av_lfg_init(&c, av_get_random_seed());

    ret = mkv_add_seekhead_entry(mkv->main_seekhead, MATROSKA_ID_ATTACHMENTS, avio_tell(pb));
    if (ret < 0) return ret;

    attachments = start_ebml_master(pb, MATROSKA_ID_ATTACHMENTS, 0);

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        ebml_master attached_file;
        AVDictionaryEntry *t;
        const char *mimetype = NULL;

        if (st->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT)
            continue;

        attached_file = start_ebml_master(pb, MATROSKA_ID_ATTACHEDFILE, 0);

        if (t = av_dict_get(st->metadata, "title", NULL, 0))
            put_ebml_string(pb, MATROSKA_ID_FILEDESC, t->value);
        if (!(t = av_dict_get(st->metadata, "filename", NULL, 0))) {
            av_log(s, AV_LOG_ERROR, "Attachment stream %d has no filename tag.\n", i);
            return AVERROR(EINVAL);
        }
        put_ebml_string(pb, MATROSKA_ID_FILENAME, t->value);
        if (t = av_dict_get(st->metadata, "mimetype", NULL, 0))
            mimetype = t->value;
        else if (st->codecpar->codec_id != AV_CODEC_ID_NONE ) {
            int i;
            for (i = 0; ff_mkv_mime_tags[i].id != AV_CODEC_ID_NONE; i++)
                if (ff_mkv_mime_tags[i].id == st->codecpar->codec_id) {
                    mimetype = ff_mkv_mime_tags[i].str;
                    break;
                }
            for (i = 0; ff_mkv_image_mime_tags[i].id != AV_CODEC_ID_NONE; i++)
                if (ff_mkv_image_mime_tags[i].id == st->codecpar->codec_id) {
                    mimetype = ff_mkv_image_mime_tags[i].str;
                    break;
                }
        }
        if (!mimetype) {
            av_log(s, AV_LOG_ERROR, "Attachment stream %d has no mimetype tag and "
                                    "it cannot be deduced from the codec id.\n", i);
            return AVERROR(EINVAL);
        }

        put_ebml_string(pb, MATROSKA_ID_FILEMIMETYPE, mimetype);
        put_ebml_binary(pb, MATROSKA_ID_FILEDATA, st->codecpar->extradata, st->codecpar->extradata_size);
        put_ebml_uint(pb, MATROSKA_ID_FILEUID, av_lfg_get(&c));
        end_ebml_master(pb, attached_file);
    }
    end_ebml_master(pb, attachments);

    return 0;
}

static int mkv_write_header(AVFormatContext *s)
{
    MatroskaMuxContext *mkv = s->priv_data;
    AVIOContext *pb = s->pb;
    ebml_master ebml_header, segment_info;
    AVDictionaryEntry *tag;
    int ret, i;

    if (!strcmp(s->oformat->name, "webm"))
        mkv->mode = MODE_WEBM;
    else
        mkv->mode = MODE_MATROSKAv2;

    mkv->tracks = av_mallocz(s->nb_streams * sizeof(*mkv->tracks));
    if (!mkv->tracks)
        return AVERROR(ENOMEM);

    ebml_header = start_ebml_master(pb, EBML_ID_HEADER, 0);
    put_ebml_uint   (pb, EBML_ID_EBMLVERSION        ,           1);
    put_ebml_uint   (pb, EBML_ID_EBMLREADVERSION    ,           1);
    put_ebml_uint   (pb, EBML_ID_EBMLMAXIDLENGTH    ,           4);
    put_ebml_uint   (pb, EBML_ID_EBMLMAXSIZELENGTH  ,           8);
    put_ebml_string (pb, EBML_ID_DOCTYPE            , s->oformat->name);
    put_ebml_uint   (pb, EBML_ID_DOCTYPEVERSION     ,           4);
    put_ebml_uint   (pb, EBML_ID_DOCTYPEREADVERSION ,           2);
    end_ebml_master(pb, ebml_header);

    mkv->segment = start_ebml_master(pb, MATROSKA_ID_SEGMENT, 0);
    mkv->segment_offset = avio_tell(pb);

    // we write 2 seek heads - one at the end of the file to point to each
    // cluster, and one at the beginning to point to all other level one
    // elements (including the seek head at the end of the file), which
    // isn't more than 10 elements if we only write one of each other
    // currently defined level 1 element
    mkv->main_seekhead    = mkv_start_seekhead(pb, mkv->segment_offset, 10);
    if (!mkv->main_seekhead)
        return AVERROR(ENOMEM);

    ret = mkv_add_seekhead_entry(mkv->main_seekhead, MATROSKA_ID_INFO, avio_tell(pb));
    if (ret < 0) return ret;

    segment_info = start_ebml_master(pb, MATROSKA_ID_INFO, 0);
    put_ebml_uint(pb, MATROSKA_ID_TIMECODESCALE, 1000000);
    if ((tag = av_dict_get(s->metadata, "title", NULL, 0)))
        put_ebml_string(pb, MATROSKA_ID_TITLE, tag->value);
    if (!(s->flags & AVFMT_FLAG_BITEXACT)) {
        uint32_t segment_uid[4];
        AVLFG lfg;

        av_lfg_init(&lfg, av_get_random_seed());

        for (i = 0; i < 4; i++)
            segment_uid[i] = av_lfg_get(&lfg);

        put_ebml_string(pb, MATROSKA_ID_MUXINGAPP, LIBAVFORMAT_IDENT);
        if ((tag = av_dict_get(s->metadata, "encoding_tool", NULL, 0)))
            put_ebml_string(pb, MATROSKA_ID_WRITINGAPP, tag->value);
        else
            put_ebml_string(pb, MATROSKA_ID_WRITINGAPP, LIBAVFORMAT_IDENT);
        put_ebml_binary(pb, MATROSKA_ID_SEGMENTUID, segment_uid, 16);
    }

    // reserve space for the duration
    mkv->duration = 0;
    mkv->duration_offset = avio_tell(pb);
    put_ebml_void(pb, 11);                  // assumes double-precision float to be written
    end_ebml_master(pb, segment_info);

    ret = mkv_write_tracks(s);
    if (ret < 0)
        return ret;

    if (mkv->mode != MODE_WEBM) {
        ret = mkv_write_chapters(s);
        if (ret < 0)
            return ret;

        ret = mkv_write_tags(s);
        if (ret < 0)
            return ret;

        ret = mkv_write_attachments(s);
        if (ret < 0)
            return ret;
    }

    if (!(s->pb->seekable & AVIO_SEEKABLE_NORMAL))
        mkv_write_seekhead(pb, mkv->main_seekhead);

    mkv->cues = mkv_start_cues(mkv->segment_offset);
    if (!mkv->cues)
        return AVERROR(ENOMEM);

    if ((pb->seekable & AVIO_SEEKABLE_NORMAL) && mkv->reserve_cues_space) {
        mkv->cues_pos = avio_tell(pb);
        put_ebml_void(pb, mkv->reserve_cues_space);
    }

    av_init_packet(&mkv->cur_audio_pkt);
    mkv->cur_audio_pkt.size = 0;

    avio_flush(pb);

    // start a new cluster every 5 MB or 5 sec, or 32k / 1 sec for streaming or
    // after 4k and on a keyframe
    if (pb->seekable & AVIO_SEEKABLE_NORMAL) {
        if (mkv->cluster_time_limit < 0)
            mkv->cluster_time_limit = 5000;
        if (mkv->cluster_size_limit < 0)
            mkv->cluster_size_limit = 5 * 1024 * 1024;
    } else {
        if (mkv->cluster_time_limit < 0)
            mkv->cluster_time_limit = 1000;
        if (mkv->cluster_size_limit < 0)
            mkv->cluster_size_limit = 32 * 1024;
    }

    return 0;
}

static int mkv_blockgroup_size(int pkt_size)
{
    int size = pkt_size + 4;
    size += ebml_num_size(size);
    size += 2;              // EBML ID for block and block duration
    size += 8;              // max size of block duration
    size += ebml_num_size(size);
    size += 1;              // blockgroup EBML ID
    return size;
}

static int ass_get_duration(AVFormatContext *s, const uint8_t *p)
{
    int sh, sm, ss, sc, eh, em, es, ec;
    uint64_t start, end;

    if (sscanf(p, "%*[^,],%d:%d:%d%*c%d,%d:%d:%d%*c%d",
               &sh, &sm, &ss, &sc, &eh, &em, &es, &ec) != 8)
        return 0;

    if (sh > 9 || sm > 59 || ss > 59 || sc > 99 ||
        eh > 9 || em > 59 || es > 59 || ec > 99) {
        av_log(s, AV_LOG_WARNING,
               "Non-standard time reference %d:%d:%d.%d,%d:%d:%d.%d\n",
               sh, sm, ss, sc, eh, em, es, ec);
        return 0;
    }

    start = 3600000 * sh + 60000 * sm + 1000 * ss + 10 * sc;
    end   = 3600000 * eh + 60000 * em + 1000 * es + 10 * ec;

    if (start > end) {
        av_log(s, AV_LOG_WARNING,
               "Unexpected time reference %d:%d:%d.%d,%d:%d:%d.%d\n",
               sh, sm, ss, sc, eh, em, es, ec);
        return 0;
    }

    return end - start;
}

static int mkv_write_ass_blocks(AVFormatContext *s, AVIOContext *pb,
                                AVPacket *pkt)
{
    MatroskaMuxContext *mkv = s->priv_data;
    int i, layer = 0, max_duration = 0, size, line_size, data_size = pkt->size;
    uint8_t *start, *end, *data = pkt->data;
    ebml_master blockgroup;
    char buffer[2048];

    while (data_size) {
        int duration = ass_get_duration(s, data);
        max_duration = FFMAX(duration, max_duration);
        end          = memchr(data, '\n', data_size);
        size         = line_size = end ? end - data + 1 : data_size;
        size        -= end ? (end[-1] == '\r') + 1 : 0;
        start        = data;
        for (i = 0; i < 3; i++, start++)
            if (!(start = memchr(start, ',', size - (start - data))))
                return max_duration;
        size -= start - data;
        sscanf(data, "Dialogue: %d,", &layer);
        i = snprintf(buffer, sizeof(buffer), "%" PRId64 ",%d,",
                     s->streams[pkt->stream_index]->nb_frames, layer);
        size = FFMIN(i + size, sizeof(buffer));
        memcpy(buffer + i, start, size - i);

        av_log(s, AV_LOG_DEBUG,
               "Writing block at offset %" PRIu64 ", size %d, "
               "pts %" PRId64 ", duration %d\n",
               avio_tell(pb), size, pkt->pts, duration);
        blockgroup = start_ebml_master(pb, MATROSKA_ID_BLOCKGROUP,
                                       mkv_blockgroup_size(size));
        put_ebml_id(pb, MATROSKA_ID_BLOCK);
        put_ebml_num(pb, size + 4, 0);
        // this assumes stream_index is less than 126
        avio_w8(pb, 0x80 | (pkt->stream_index + 1));
        avio_wb16(pb, pkt->pts - mkv->cluster_pts);
        avio_w8(pb, 0);
        avio_write(pb, buffer, size);
        put_ebml_uint(pb, MATROSKA_ID_BLOCKDURATION, duration);
        end_ebml_master(pb, blockgroup);

        data      += line_size;
        data_size -= line_size;
    }

    return max_duration;
}

static int mkv_strip_wavpack(const uint8_t *src, uint8_t **pdst, int *size)
{
    uint8_t *dst;
    int srclen = *size;
    int offset = 0;
    int ret;

    dst = av_malloc(srclen);
    if (!dst)
        return AVERROR(ENOMEM);

    while (srclen >= WV_HEADER_SIZE) {
        WvHeader header;

        ret = ff_wv_parse_header(&header, src);
        if (ret < 0)
            goto fail;
        src    += WV_HEADER_SIZE;
        srclen -= WV_HEADER_SIZE;

        if (srclen < header.blocksize) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        if (header.initial) {
            AV_WL32(dst + offset, header.samples);
            offset += 4;
        }
        AV_WL32(dst + offset,     header.flags);
        AV_WL32(dst + offset + 4, header.crc);
        offset += 8;

        if (!(header.initial && header.final)) {
            AV_WL32(dst + offset, header.blocksize);
            offset += 4;
        }

        memcpy(dst + offset, src, header.blocksize);
        src    += header.blocksize;
        srclen -= header.blocksize;
        offset += header.blocksize;
    }

    *pdst = dst;
    *size = offset;

    return 0;
fail:
    av_freep(&dst);
    return ret;
}

static void mkv_write_block(AVFormatContext *s, AVIOContext *pb,
                            unsigned int blockid, AVPacket *pkt, int flags)
{
    MatroskaMuxContext *mkv = s->priv_data;
    AVCodecParameters *par = s->streams[pkt->stream_index]->codecpar;
    uint8_t *data = NULL;
    int offset = 0, size = pkt->size;
    int64_t ts = mkv->tracks[pkt->stream_index].write_dts ? pkt->dts : pkt->pts;
    ts += mkv->tracks[pkt->stream_index].ts_offset;

    av_log(s, AV_LOG_DEBUG, "Writing block at offset %" PRIu64 ", size %d, "
           "pts %" PRId64 ", dts %" PRId64 ", duration %" PRId64 ", flags %d\n",
           avio_tell(pb), pkt->size, pkt->pts, pkt->dts, pkt->duration, flags);
    if (par->codec_id == AV_CODEC_ID_H264 && par->extradata_size > 0 &&
        (AV_RB24(par->extradata) == 1 || AV_RB32(par->extradata) == 1))
        ff_avc_parse_nal_units_buf(pkt->data, &data, &size);
    else if (par->codec_id == AV_CODEC_ID_HEVC && par->extradata_size > 6 &&
             (AV_RB24(par->extradata) == 1 || AV_RB32(par->extradata) == 1))
        /* extradata is Annex B, assume the bitstream is too and convert it */
        ff_hevc_annexb2mp4_buf(pkt->data, &data, &size, 0, NULL);
    else if (par->codec_id == AV_CODEC_ID_WAVPACK) {
        int ret = mkv_strip_wavpack(pkt->data, &data, &size);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Error stripping a WavPack packet.\n");
            return;
        }
    } else
        data = pkt->data;

    if (par->codec_id == AV_CODEC_ID_PRORES) {
        /* Matroska specification requires to remove the first QuickTime atom
         */
        size  -= 8;
        offset = 8;
    }

    put_ebml_id(pb, blockid);
    put_ebml_num(pb, size + 4, 0);
    // this assumes stream_index is less than 126
    avio_w8(pb, 0x80 | (pkt->stream_index + 1));
    avio_wb16(pb, ts - mkv->cluster_pts);
    avio_w8(pb, flags);
    avio_write(pb, data + offset, size);
    if (data != pkt->data)
        av_free(data);
}

static int srt_get_duration(uint8_t **buf)
{
    int i, duration = 0;

    for (i = 0; i < 2 && !duration; i++) {
        int s_hour, s_min, s_sec, s_hsec, e_hour, e_min, e_sec, e_hsec;
        if (sscanf(*buf, "%d:%2d:%2d%*1[,.]%3d --> %d:%2d:%2d%*1[,.]%3d",
                   &s_hour, &s_min, &s_sec, &s_hsec,
                   &e_hour, &e_min, &e_sec, &e_hsec) == 8) {
            s_min += 60 * s_hour;
            e_min += 60 * e_hour;
            s_sec += 60 * s_min;

            e_sec  += 60 * e_min;
            s_hsec += 1000 * s_sec;
            e_hsec += 1000 * e_sec;

            duration = e_hsec - s_hsec;
        }
        *buf += strcspn(*buf, "\n") + 1;
    }
    return duration;
}

static int mkv_write_srt_blocks(AVFormatContext *s, AVIOContext *pb,
                                AVPacket *pkt)
{
    ebml_master blockgroup;
    AVPacket pkt2 = *pkt;
    int64_t duration = srt_get_duration(&pkt2.data);
    pkt2.size -= pkt2.data - pkt->data;

    blockgroup = start_ebml_master(pb, MATROSKA_ID_BLOCKGROUP,
                                   mkv_blockgroup_size(pkt2.size));
    mkv_write_block(s, pb, MATROSKA_ID_BLOCK, &pkt2, 0);
    put_ebml_uint(pb, MATROSKA_ID_BLOCKDURATION, duration);
    end_ebml_master(pb, blockgroup);

    return duration;
}

static void mkv_flush_dynbuf(AVFormatContext *s)
{
    MatroskaMuxContext *mkv = s->priv_data;
    int bufsize;
    uint8_t *dyn_buf;

    if (!mkv->dyn_bc)
        return;

    bufsize = avio_close_dyn_buf(mkv->dyn_bc, &dyn_buf);
    avio_write(s->pb, dyn_buf, bufsize);
    av_free(dyn_buf);
    mkv->dyn_bc = NULL;
}

static int mkv_check_new_extra_data(AVFormatContext *s, AVPacket *pkt)
{
    MatroskaMuxContext *mkv = s->priv_data;
    AVCodecParameters *par  = s->streams[pkt->stream_index]->codecpar;
    mkv_track *track        = &mkv->tracks[pkt->stream_index];
    uint8_t *side_data;
    int side_data_size = 0, ret;

    side_data = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA,
                                        &side_data_size);

    switch (par->codec_id) {
    case AV_CODEC_ID_AAC:
        if (side_data_size && (s->pb->seekable & AVIO_SEEKABLE_NORMAL)) {
            int output_sample_rate = 0;
            int64_t curpos;
            ret = get_aac_sample_rates(s, side_data, side_data_size, &track->sample_rate,
                                       &output_sample_rate);
            if (ret < 0)
                return ret;
            if (!output_sample_rate)
                output_sample_rate = track->sample_rate; // Space is already reserved, so it's this or a void element.
            curpos = avio_tell(s->pb);
            avio_seek(s->pb, track->sample_rate_offset, SEEK_SET);
            put_ebml_float(s->pb, MATROSKA_ID_AUDIOSAMPLINGFREQ, track->sample_rate);
            put_ebml_float(s->pb, MATROSKA_ID_AUDIOOUTSAMPLINGFREQ, output_sample_rate);
            avio_seek(s->pb, curpos, SEEK_SET);
        } else if (!par->extradata_size && !track->sample_rate) {
            // No extradata (codecpar or packet side data).
            av_log(s, AV_LOG_ERROR, "Error parsing AAC extradata, unable to determine samplerate.\n");
            return AVERROR(EINVAL);
        }
        break;
    case AV_CODEC_ID_FLAC:
        if (side_data_size && (s->pb->seekable & AVIO_SEEKABLE_NORMAL)) {
            AVCodecParameters *codecpriv_par;
            int64_t curpos;
            if (side_data_size != par->extradata_size) {
                av_log(s, AV_LOG_ERROR, "Invalid FLAC STREAMINFO metadata for output stream %d\n",
                       pkt->stream_index);
                return AVERROR(EINVAL);
            }
            codecpriv_par = avcodec_parameters_alloc();
            if (!codecpriv_par)
                return AVERROR(ENOMEM);
            ret = avcodec_parameters_copy(codecpriv_par, par);
            if (ret < 0) {
                avcodec_parameters_free(&codecpriv_par);
                return ret;
            }
            memcpy(codecpriv_par->extradata, side_data, side_data_size);
            curpos = avio_tell(s->pb);
            avio_seek(s->pb, track->codecpriv_offset, SEEK_SET);
            mkv_write_codecprivate(s, s->pb, codecpriv_par, 1, 0);
            avio_seek(s->pb, curpos, SEEK_SET);
            avcodec_parameters_free(&codecpriv_par);
        }
        break;
    default:
        if (side_data_size)
            av_log(s, AV_LOG_DEBUG, "Ignoring new extradata in a packet for stream %d.\n", pkt->stream_index);
        break;
    }

    return 0;
}

static int mkv_write_packet_internal(AVFormatContext *s, AVPacket *pkt)
{
    MatroskaMuxContext *mkv = s->priv_data;
    AVIOContext *pb         = s->pb;
    AVCodecParameters *par  = s->streams[pkt->stream_index]->codecpar;
    int keyframe            = !!(pkt->flags & AV_PKT_FLAG_KEY);
    int duration            = pkt->duration;
    int ret;
    int64_t ts = mkv->tracks[pkt->stream_index].write_dts ? pkt->dts : pkt->pts;

    if (ts == AV_NOPTS_VALUE) {
        av_log(s, AV_LOG_ERROR, "Can't write packet with unknown timestamp\n");
        return AVERROR(EINVAL);
    }
    ts += mkv->tracks[pkt->stream_index].ts_offset;

    if (!(s->pb->seekable & AVIO_SEEKABLE_NORMAL)) {
        if (!mkv->dyn_bc) {
            ret = avio_open_dyn_buf(&mkv->dyn_bc);
            if (ret < 0)
                return ret;
        }
        pb = mkv->dyn_bc;
    }

    if (!mkv->cluster_pos) {
        mkv->cluster_pos = avio_tell(s->pb);
        mkv->cluster     = start_ebml_master(pb, MATROSKA_ID_CLUSTER, 0);
        put_ebml_uint(pb, MATROSKA_ID_CLUSTERTIMECODE, FFMAX(0, ts));
        mkv->cluster_pts = FFMAX(0, ts);
    }

    if (par->codec_type != AVMEDIA_TYPE_SUBTITLE) {
        mkv_write_block(s, pb, MATROSKA_ID_SIMPLEBLOCK, pkt, keyframe << 7);
    } else if (par->codec_id == AV_CODEC_ID_SSA) {
        duration = mkv_write_ass_blocks(s, pb, pkt);
    } else if (par->codec_id == AV_CODEC_ID_SRT) {
        duration = mkv_write_srt_blocks(s, pb, pkt);
    } else {
        ebml_master blockgroup = start_ebml_master(pb, MATROSKA_ID_BLOCKGROUP,
                                                   mkv_blockgroup_size(pkt->size));
        duration = pkt->duration;
#if FF_API_CONVERGENCE_DURATION
FF_DISABLE_DEPRECATION_WARNINGS
        if (pkt->convergence_duration)
            duration = pkt->convergence_duration;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
        mkv_write_block(s, pb, MATROSKA_ID_BLOCK, pkt, 0);
        put_ebml_uint(pb, MATROSKA_ID_BLOCKDURATION, duration);
        end_ebml_master(pb, blockgroup);
    }

    if (par->codec_type == AVMEDIA_TYPE_VIDEO && keyframe) {
        ret = mkv_add_cuepoint(mkv->cues, pkt->stream_index, ts,
                               mkv->cluster_pos);
        if (ret < 0)
            return ret;
    }

    mkv->duration = FFMAX(mkv->duration, ts + duration);
    return 0;
}

static int mkv_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    MatroskaMuxContext *mkv = s->priv_data;
    int codec_type          = s->streams[pkt->stream_index]->codecpar->codec_type;
    int keyframe            = !!(pkt->flags & AV_PKT_FLAG_KEY);
    int cluster_size;
    int64_t cluster_time;
    AVIOContext *pb;
    int ret;

    ret = mkv_check_new_extra_data(s, pkt);
    if (ret < 0)
        return ret;

    if (mkv->tracks[pkt->stream_index].write_dts)
        cluster_time = pkt->dts - mkv->cluster_pts;
    else
        cluster_time = pkt->pts - mkv->cluster_pts;
    cluster_time += mkv->tracks[pkt->stream_index].ts_offset;

    // start a new cluster every 5 MB or 5 sec, or 32k / 1 sec for streaming or
    // after 4k and on a keyframe
    if (s->pb->seekable & AVIO_SEEKABLE_NORMAL) {
        pb = s->pb;
        cluster_size = avio_tell(pb) - mkv->cluster_pos;
    } else {
        pb = mkv->dyn_bc;
        cluster_size = avio_tell(pb);
    }

    if (mkv->cluster_pos &&
        (cluster_size > mkv->cluster_size_limit ||
         cluster_time > mkv->cluster_time_limit ||
         (codec_type == AVMEDIA_TYPE_VIDEO && keyframe &&
          cluster_size > 4 * 1024))) {
        av_log(s, AV_LOG_DEBUG,
               "Starting new cluster at offset %" PRIu64 " bytes, "
               "pts %" PRIu64 "dts %" PRIu64 "\n",
               avio_tell(pb), pkt->pts, pkt->dts);
        end_ebml_master(pb, mkv->cluster);
        mkv->cluster_pos = 0;
        if (mkv->dyn_bc)
            mkv_flush_dynbuf(s);
        avio_flush(s->pb);
    }

    if (!mkv->cluster_pos)
        avio_write_marker(s->pb,
                          av_rescale_q(pkt->dts, s->streams[pkt->stream_index]->time_base, AV_TIME_BASE_Q),
                          keyframe && (mkv->have_video ? codec_type == AVMEDIA_TYPE_VIDEO : 1) ? AVIO_DATA_MARKER_SYNC_POINT : AVIO_DATA_MARKER_BOUNDARY_POINT);

    // check if we have an audio packet cached
    if (mkv->cur_audio_pkt.size > 0) {
        ret = mkv_write_packet_internal(s, &mkv->cur_audio_pkt);
        av_packet_unref(&mkv->cur_audio_pkt);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR,
                   "Could not write cached audio packet ret:%d\n", ret);
            return ret;
        }
    }

    // buffer an audio packet to ensure the packet containing the video
    // keyframe's timecode is contained in the same cluster for WebM
    if (codec_type == AVMEDIA_TYPE_AUDIO) {
        ret = av_packet_ref(&mkv->cur_audio_pkt, pkt);
    } else
        ret = mkv_write_packet_internal(s, pkt);
    return ret;
}

static int mkv_write_flush_packet(AVFormatContext *s, AVPacket *pkt)
{
    MatroskaMuxContext *mkv = s->priv_data;
    AVIOContext *pb;
    if (s->pb->seekable & AVIO_SEEKABLE_NORMAL)
        pb = s->pb;
    else
        pb = mkv->dyn_bc;
    if (!pkt) {
        if (mkv->cluster_pos) {
            av_log(s, AV_LOG_DEBUG,
                   "Flushing cluster at offset %" PRIu64 " bytes\n",
                   avio_tell(pb));
            end_ebml_master(pb, mkv->cluster);
            mkv->cluster_pos = 0;
            if (mkv->dyn_bc)
                mkv_flush_dynbuf(s);
            avio_flush(s->pb);
        }
        return 1;
    }
    return mkv_write_packet(s, pkt);
}

static int mkv_write_trailer(AVFormatContext *s)
{
    MatroskaMuxContext *mkv = s->priv_data;
    AVIOContext *pb = s->pb;
    int64_t currentpos, cuespos;
    int ret;

    // check if we have an audio packet cached
    if (mkv->cur_audio_pkt.size > 0) {
        ret = mkv_write_packet_internal(s, &mkv->cur_audio_pkt);
        av_packet_unref(&mkv->cur_audio_pkt);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR,
                   "Could not write cached audio packet ret:%d\n", ret);
            return ret;
        }
    }

    if (mkv->dyn_bc) {
        end_ebml_master(mkv->dyn_bc, mkv->cluster);
        mkv_flush_dynbuf(s);
    } else if (mkv->cluster_pos) {
        end_ebml_master(pb, mkv->cluster);
    }

    if (mkv->mode != MODE_WEBM) {
        ret = mkv_write_chapters(s);
        if (ret < 0)
            return ret;
    }

    if (pb->seekable & AVIO_SEEKABLE_NORMAL) {
        if (mkv->cues->num_entries) {
            if (mkv->reserve_cues_space) {
                int64_t cues_end;

                currentpos = avio_tell(pb);
                avio_seek(pb, mkv->cues_pos, SEEK_SET);

                cuespos  = mkv_write_cues(pb, mkv->cues, s->nb_streams);
                cues_end = avio_tell(pb);
                if (cues_end > cuespos + mkv->reserve_cues_space) {
                    av_log(s, AV_LOG_ERROR,
                           "Insufficient space reserved for cues: %d "
                           "(needed: %" PRId64 ").\n",
                           mkv->reserve_cues_space, cues_end - cuespos);
                    return AVERROR(EINVAL);
                }

                if (cues_end < cuespos + mkv->reserve_cues_space)
                    put_ebml_void(pb, mkv->reserve_cues_space -
                                  (cues_end - cuespos));

                avio_seek(pb, currentpos, SEEK_SET);
            } else {
                cuespos = mkv_write_cues(pb, mkv->cues, s->nb_streams);
            }

            ret = mkv_add_seekhead_entry(mkv->main_seekhead, MATROSKA_ID_CUES,
                                         cuespos);
            if (ret < 0)
                return ret;
        }

        mkv_write_seekhead(pb, mkv->main_seekhead);

        // update the duration
        av_log(s, AV_LOG_DEBUG, "end duration = %" PRIu64 "\n", mkv->duration);
        currentpos = avio_tell(pb);
        avio_seek(pb, mkv->duration_offset, SEEK_SET);
        put_ebml_float(pb, MATROSKA_ID_DURATION, mkv->duration);

        avio_seek(pb, currentpos, SEEK_SET);
    }

    end_ebml_master(pb, mkv->segment);
    av_free(mkv->tracks);
    av_freep(&mkv->cues->entries);
    av_freep(&mkv->cues);

    return 0;
}

static int mkv_query_codec(enum AVCodecID codec_id, int std_compliance)
{
    int i;
    for (i = 0; ff_mkv_codec_tags[i].id != AV_CODEC_ID_NONE; i++)
        if (ff_mkv_codec_tags[i].id == codec_id)
            return 1;

    if (std_compliance < FF_COMPLIANCE_NORMAL) {
        enum AVMediaType type = avcodec_get_type(codec_id);
        // mkv theoretically supports any video/audio through VFW/ACM
        if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO)
            return 1;
    }

    return 0;
}

#define OFFSET(x) offsetof(MatroskaMuxContext, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "reserve_index_space", "Reserve a given amount of space (in bytes) at the beginning of the file for the index (cues).", OFFSET(reserve_cues_space), AV_OPT_TYPE_INT,   { .i64 = 0 },   0, INT_MAX,   FLAGS },
    { "cluster_size_limit",  "Store at most the provided amount of bytes in a cluster. ",                                     OFFSET(cluster_size_limit), AV_OPT_TYPE_INT  , { .i64 = -1 }, -1, INT_MAX,   FLAGS },
    { "cluster_time_limit",  "Store at most the provided number of milliseconds in a cluster.",                               OFFSET(cluster_time_limit), AV_OPT_TYPE_INT64, { .i64 = -1 }, -1, INT64_MAX, FLAGS },
    { NULL },
};

#if CONFIG_MATROSKA_MUXER
static const AVClass matroska_class = {
    .class_name = "matroska muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVOutputFormat ff_matroska_muxer = {
    .name              = "matroska",
    .long_name         = NULL_IF_CONFIG_SMALL("Matroska"),
    .mime_type         = "video/x-matroska",
    .extensions        = "mkv",
    .priv_data_size    = sizeof(MatroskaMuxContext),
    .audio_codec       = CONFIG_LIBVORBIS_ENCODER ?
                         AV_CODEC_ID_VORBIS : AV_CODEC_ID_AC3,
    .video_codec       = CONFIG_LIBX264_ENCODER ?
                         AV_CODEC_ID_H264 : AV_CODEC_ID_MPEG4,
    .write_header      = mkv_write_header,
    .write_packet      = mkv_write_flush_packet,
    .write_trailer     = mkv_write_trailer,
    .flags             = AVFMT_GLOBALHEADER | AVFMT_VARIABLE_FPS |
                         AVFMT_TS_NONSTRICT | AVFMT_ALLOW_FLUSH,
    .codec_tag         = (const AVCodecTag* const []){
         ff_codec_bmp_tags, ff_codec_wav_tags, 0
    },
    .subtitle_codec    = AV_CODEC_ID_SSA,
    .query_codec       = mkv_query_codec,
    .priv_class        = &matroska_class,
};
#endif

#if CONFIG_WEBM_MUXER
static const AVClass webm_class = {
    .class_name = "webm muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVOutputFormat ff_webm_muxer = {
    .name              = "webm",
    .long_name         = NULL_IF_CONFIG_SMALL("WebM"),
    .mime_type         = "video/webm",
    .extensions        = "webm",
    .priv_data_size    = sizeof(MatroskaMuxContext),
    .audio_codec       = CONFIG_LIBOPUS_ENCODER ? AV_CODEC_ID_OPUS : AV_CODEC_ID_VORBIS,
    .video_codec       = CONFIG_LIBVPX_VP9_ENCODER? AV_CODEC_ID_VP9 : AV_CODEC_ID_VP8,
    .write_header      = mkv_write_header,
    .write_packet      = mkv_write_flush_packet,
    .write_trailer     = mkv_write_trailer,
    .flags             = AVFMT_GLOBALHEADER | AVFMT_VARIABLE_FPS |
                         AVFMT_TS_NONSTRICT | AVFMT_ALLOW_FLUSH,
    .priv_class        = &webm_class,
};
#endif

#if CONFIG_MATROSKA_AUDIO_MUXER
static const AVClass mka_class = {
    .class_name = "matroska audio muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVOutputFormat ff_matroska_audio_muxer = {
    .name              = "matroska",
    .long_name         = NULL_IF_CONFIG_SMALL("Matroska"),
    .mime_type         = "audio/x-matroska",
    .extensions        = "mka",
    .priv_data_size    = sizeof(MatroskaMuxContext),
    .audio_codec       = CONFIG_LIBVORBIS_ENCODER ?
                         AV_CODEC_ID_VORBIS : AV_CODEC_ID_AC3,
    .video_codec       = AV_CODEC_ID_NONE,
    .write_header      = mkv_write_header,
    .write_packet      = mkv_write_flush_packet,
    .write_trailer     = mkv_write_trailer,
    .flags             = AVFMT_GLOBALHEADER | AVFMT_TS_NONSTRICT |
                         AVFMT_ALLOW_FLUSH,
    .codec_tag         = (const AVCodecTag* const []){ ff_codec_wav_tags, 0 },
    .priv_class        = &mka_class,
};
#endif

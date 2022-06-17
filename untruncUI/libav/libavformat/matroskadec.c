/*
 * Matroska file demuxer
 * Copyright (c) 2003-2008 The Libav Project
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
 * Matroska file demuxer
 * @author Ronald Bultje <rbultje@ronald.bitfreak.net>
 * @author with a little help from Moritz Bunkus <moritz@bunkus.org>
 * @author totally reworked by Aurelien Jacobs <aurel@gnuage.org>
 * @see specs available on the Matroska project page: http://www.matroska.org/
 */

#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#if CONFIG_BZLIB
#include <bzlib.h>
#endif
#if CONFIG_ZLIB
#include <zlib.h>
#endif

#include "libavutil/avstring.h"
#include "libavutil/dict.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/lzo.h"
#include "libavutil/mathematics.h"
#include "libavutil/spherical.h"

#include "libavcodec/bytestream.h"
#include "libavcodec/flac.h"
#include "libavcodec/mpeg4audio.h"

#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"
#include "isom.h"
#include "matroska.h"
#include "oggdec.h"
/* For ff_codec_get_id(). */
#include "riff.h"
#include "rmsipr.h"

typedef enum {
    EBML_NONE,
    EBML_UINT,
    EBML_FLOAT,
    EBML_STR,
    EBML_UTF8,
    EBML_BIN,
    EBML_NEST,
    EBML_PASS,
    EBML_STOP,
    EBML_TYPE_COUNT
} EbmlType;

typedef const struct EbmlSyntax {
    uint32_t id;
    EbmlType type;
    int list_elem_size;
    int data_offset;
    union {
        uint64_t    u;
        double      f;
        const char *s;
        const struct EbmlSyntax *n;
    } def;
} EbmlSyntax;

typedef struct EbmlList {
    int nb_elem;
    void *elem;
} EbmlList;

typedef struct EbmlBin {
    int      size;
    uint8_t *data;
    int64_t  pos;
} EbmlBin;

typedef struct Ebml {
    uint64_t version;
    uint64_t max_size;
    uint64_t id_length;
    char    *doctype;
    uint64_t doctype_version;
} Ebml;

typedef struct MatroskaTrackCompression {
    uint64_t algo;
    EbmlBin  settings;
} MatroskaTrackCompression;

typedef struct MatroskaTrackEncoding {
    uint64_t scope;
    uint64_t type;
    MatroskaTrackCompression compression;
} MatroskaTrackEncoding;

typedef struct MatroskaTrackVideoProjection {
    uint64_t type;
    EbmlBin private;
    double yaw;
    double pitch;
    double roll;
} MatroskaTrackVideoProjection;

typedef struct MatroskaTrackVideo {
    double   frame_rate;
    uint64_t display_width;
    uint64_t display_height;
    uint64_t pixel_width;
    uint64_t pixel_height;
    uint64_t fourcc;
    uint64_t interlaced;
    uint64_t field_order;
    uint64_t stereo_mode;
    MatroskaTrackVideoProjection projection;
} MatroskaTrackVideo;

typedef struct MatroskaTrackAudio {
    double   samplerate;
    double   out_samplerate;
    uint64_t bitdepth;
    uint64_t channels;

    /* real audio header (extracted from extradata) */
    int      coded_framesize;
    int      sub_packet_h;
    int      frame_size;
    int      sub_packet_size;
    int      sub_packet_cnt;
    int      pkt_cnt;
    uint64_t buf_timecode;
    uint8_t *buf;
} MatroskaTrackAudio;

typedef struct MatroskaTrack {
    uint64_t num;
    uint64_t uid;
    uint64_t type;
    char    *name;
    char    *codec_id;
    EbmlBin  codec_priv;
    char    *language;
    double time_scale;
    uint64_t default_duration;
    uint64_t flag_default;
    uint64_t flag_forced;
    MatroskaTrackVideo video;
    MatroskaTrackAudio audio;
    EbmlList encodings;
    uint64_t codec_delay;

    AVStream *stream;
    int64_t end_timecode;
    int ms_compat;
} MatroskaTrack;

typedef struct MatroskaAttachment {
    uint64_t uid;
    char *filename;
    char *mime;
    EbmlBin bin;

    AVStream *stream;
} MatroskaAttachment;

typedef struct MatroskaChapter {
    uint64_t start;
    uint64_t end;
    uint64_t uid;
    char    *title;

    AVChapter *chapter;
} MatroskaChapter;

typedef struct MatroskaIndexPos {
    uint64_t track;
    uint64_t pos;
} MatroskaIndexPos;

typedef struct MatroskaIndex {
    uint64_t time;
    EbmlList pos;
} MatroskaIndex;

typedef struct MatroskaTag {
    char *name;
    char *string;
    char *lang;
    uint64_t def;
    EbmlList sub;
} MatroskaTag;

typedef struct MatroskaTagTarget {
    char    *type;
    uint64_t typevalue;
    uint64_t trackuid;
    uint64_t chapteruid;
    uint64_t attachuid;
} MatroskaTagTarget;

typedef struct MatroskaTags {
    MatroskaTagTarget target;
    EbmlList tag;
} MatroskaTags;

typedef struct MatroskaSeekhead {
    uint64_t id;
    uint64_t pos;
} MatroskaSeekhead;

typedef struct MatroskaLevel {
    uint64_t start;
    uint64_t length;
} MatroskaLevel;

typedef struct MatroskaCluster {
    uint64_t timecode;
    EbmlList blocks;
} MatroskaCluster;

typedef struct MatroskaDemuxContext {
    AVFormatContext *ctx;

    /* EBML stuff */
    int num_levels;
    MatroskaLevel levels[EBML_MAX_DEPTH];
    int level_up;
    uint32_t current_id;

    uint64_t time_scale;
    double   duration;
    char    *title;
    EbmlList tracks;
    EbmlList attachments;
    EbmlList chapters;
    EbmlList index;
    EbmlList tags;
    EbmlList seekhead;

    /* byte position of the segment inside the stream */
    int64_t segment_start;

    /* the packet queue */
    AVPacket **packets;
    int num_packets;
    AVPacket *prev_pkt;

    int done;

    /* What to skip before effectively reading a packet. */
    int skip_to_keyframe;
    uint64_t skip_to_timecode;

    /* File has a CUES element, but we defer parsing until it is needed. */
    int cues_parsing_deferred;

    int current_cluster_num_blocks;
    int64_t current_cluster_pos;
    MatroskaCluster current_cluster;

    /* File has SSA subtitles which prevent incremental cluster parsing. */
    int contains_ssa;
} MatroskaDemuxContext;

typedef struct MatroskaBlock {
    uint64_t duration;
    int64_t  reference;
    uint64_t non_simple;
    EbmlBin  bin;
} MatroskaBlock;

static EbmlSyntax ebml_header[] = {
    { EBML_ID_EBMLREADVERSION,    EBML_UINT, 0, offsetof(Ebml, version),         { .u = EBML_VERSION } },
    { EBML_ID_EBMLMAXSIZELENGTH,  EBML_UINT, 0, offsetof(Ebml, max_size),        { .u = 8 } },
    { EBML_ID_EBMLMAXIDLENGTH,    EBML_UINT, 0, offsetof(Ebml, id_length),       { .u = 4 } },
    { EBML_ID_DOCTYPE,            EBML_STR,  0, offsetof(Ebml, doctype),         { .s = "(none)" } },
    { EBML_ID_DOCTYPEREADVERSION, EBML_UINT, 0, offsetof(Ebml, doctype_version), { .u = 1 } },
    { EBML_ID_EBMLVERSION,        EBML_NONE },
    { EBML_ID_DOCTYPEVERSION,     EBML_NONE },
    { 0 }
};

static EbmlSyntax ebml_syntax[] = {
    { EBML_ID_HEADER, EBML_NEST, 0, 0, { .n = ebml_header } },
    { 0 }
};

static EbmlSyntax matroska_info[] = {
    { MATROSKA_ID_TIMECODESCALE, EBML_UINT,  0, offsetof(MatroskaDemuxContext, time_scale), { .u = 1000000 } },
    { MATROSKA_ID_DURATION,      EBML_FLOAT, 0, offsetof(MatroskaDemuxContext, duration) },
    { MATROSKA_ID_TITLE,         EBML_UTF8,  0, offsetof(MatroskaDemuxContext, title) },
    { MATROSKA_ID_WRITINGAPP,    EBML_NONE },
    { MATROSKA_ID_MUXINGAPP,     EBML_NONE },
    { MATROSKA_ID_DATEUTC,       EBML_NONE },
    { MATROSKA_ID_SEGMENTUID,    EBML_NONE },
    { 0 }
};

static const EbmlSyntax matroska_track_video_projection[] = {
    { MATROSKA_ID_VIDEOPROJECTIONTYPE,      EBML_UINT,  0, offsetof(MatroskaTrackVideoProjection, type), { .u = MATROSKA_VIDEO_PROJECTION_TYPE_RECTANGULAR } },
    { MATROSKA_ID_VIDEOPROJECTIONPRIVATE,   EBML_BIN,   0, offsetof(MatroskaTrackVideoProjection, private) },
    { MATROSKA_ID_VIDEOPROJECTIONPOSEYAW,   EBML_FLOAT, 0, offsetof(MatroskaTrackVideoProjection, yaw), { .f=0.0 } },
    { MATROSKA_ID_VIDEOPROJECTIONPOSEPITCH, EBML_FLOAT, 0, offsetof(MatroskaTrackVideoProjection, pitch), { .f=0.0 } },
    { MATROSKA_ID_VIDEOPROJECTIONPOSEROLL,  EBML_FLOAT, 0, offsetof(MatroskaTrackVideoProjection, roll), { .f=0.0 } },
    { 0 }
};

static EbmlSyntax matroska_track_video[] = {
    { MATROSKA_ID_VIDEOFRAMERATE,      EBML_FLOAT, 0, offsetof(MatroskaTrackVideo, frame_rate) },
    { MATROSKA_ID_VIDEODISPLAYWIDTH,   EBML_UINT,  0, offsetof(MatroskaTrackVideo, display_width) },
    { MATROSKA_ID_VIDEODISPLAYHEIGHT,  EBML_UINT,  0, offsetof(MatroskaTrackVideo, display_height) },
    { MATROSKA_ID_VIDEOPIXELWIDTH,     EBML_UINT,  0, offsetof(MatroskaTrackVideo, pixel_width) },
    { MATROSKA_ID_VIDEOPIXELHEIGHT,    EBML_UINT,  0, offsetof(MatroskaTrackVideo, pixel_height) },
    { MATROSKA_ID_VIDEOCOLORSPACE,     EBML_UINT,  0, offsetof(MatroskaTrackVideo, fourcc) },
    { MATROSKA_ID_VIDEOPROJECTION,     EBML_NEST,  0, offsetof(MatroskaTrackVideo, projection), { .n = matroska_track_video_projection } },
    { MATROSKA_ID_VIDEOPIXELCROPB,     EBML_NONE },
    { MATROSKA_ID_VIDEOPIXELCROPT,     EBML_NONE },
    { MATROSKA_ID_VIDEOPIXELCROPL,     EBML_NONE },
    { MATROSKA_ID_VIDEOPIXELCROPR,     EBML_NONE },
    { MATROSKA_ID_VIDEODISPLAYUNIT,    EBML_NONE },
    { MATROSKA_ID_VIDEOFLAGINTERLACED, EBML_UINT,  0, offsetof(MatroskaTrackVideo, interlaced),  { .u = MATROSKA_VIDEO_INTERLACE_FLAG_UNDETERMINED } },
    { MATROSKA_ID_VIDEOFIELDORDER,     EBML_UINT,  0, offsetof(MatroskaTrackVideo, field_order), { .u = MATROSKA_VIDEO_FIELDORDER_UNDETERMINED } },
    { MATROSKA_ID_VIDEOSTEREOMODE,     EBML_UINT,  0, offsetof(MatroskaTrackVideo, stereo_mode), { .u = MATROSKA_VIDEO_STEREOMODE_TYPE_NB } },
    { MATROSKA_ID_VIDEOASPECTRATIO,    EBML_NONE },
    { 0 }
};

static EbmlSyntax matroska_track_audio[] = {
    { MATROSKA_ID_AUDIOSAMPLINGFREQ,    EBML_FLOAT, 0, offsetof(MatroskaTrackAudio, samplerate), { .f = 8000.0 } },
    { MATROSKA_ID_AUDIOOUTSAMPLINGFREQ, EBML_FLOAT, 0, offsetof(MatroskaTrackAudio, out_samplerate) },
    { MATROSKA_ID_AUDIOBITDEPTH,        EBML_UINT,  0, offsetof(MatroskaTrackAudio, bitdepth) },
    { MATROSKA_ID_AUDIOCHANNELS,        EBML_UINT,  0, offsetof(MatroskaTrackAudio, channels),   { .u = 1 } },
    { 0 }
};

static EbmlSyntax matroska_track_encoding_compression[] = {
    { MATROSKA_ID_ENCODINGCOMPALGO,     EBML_UINT, 0, offsetof(MatroskaTrackCompression, algo), { .u = 0 } },
    { MATROSKA_ID_ENCODINGCOMPSETTINGS, EBML_BIN,  0, offsetof(MatroskaTrackCompression, settings) },
    { 0 }
};

static EbmlSyntax matroska_track_encoding[] = {
    { MATROSKA_ID_ENCODINGSCOPE,       EBML_UINT, 0, offsetof(MatroskaTrackEncoding, scope),       { .u = 1 } },
    { MATROSKA_ID_ENCODINGTYPE,        EBML_UINT, 0, offsetof(MatroskaTrackEncoding, type),        { .u = 0 } },
    { MATROSKA_ID_ENCODINGCOMPRESSION, EBML_NEST, 0, offsetof(MatroskaTrackEncoding, compression), { .n = matroska_track_encoding_compression } },
    { MATROSKA_ID_ENCODINGORDER,       EBML_NONE },
    { 0 }
};

static EbmlSyntax matroska_track_encodings[] = {
    { MATROSKA_ID_TRACKCONTENTENCODING, EBML_NEST, sizeof(MatroskaTrackEncoding), offsetof(MatroskaTrack, encodings), { .n = matroska_track_encoding } },
    { 0 }
};

static EbmlSyntax matroska_track[] = {
    { MATROSKA_ID_TRACKNUMBER,           EBML_UINT,  0, offsetof(MatroskaTrack, num) },
    { MATROSKA_ID_TRACKNAME,             EBML_UTF8,  0, offsetof(MatroskaTrack, name) },
    { MATROSKA_ID_TRACKUID,              EBML_UINT,  0, offsetof(MatroskaTrack, uid) },
    { MATROSKA_ID_TRACKTYPE,             EBML_UINT,  0, offsetof(MatroskaTrack, type) },
    { MATROSKA_ID_CODECID,               EBML_STR,   0, offsetof(MatroskaTrack, codec_id) },
    { MATROSKA_ID_CODECPRIVATE,          EBML_BIN,   0, offsetof(MatroskaTrack, codec_priv) },
    { MATROSKA_ID_CODECDELAY,            EBML_UINT,  0, offsetof(MatroskaTrack, codec_delay) },
    { MATROSKA_ID_TRACKLANGUAGE,         EBML_UTF8,  0, offsetof(MatroskaTrack, language),     { .s = "eng"  } },
    { MATROSKA_ID_TRACKDEFAULTDURATION,  EBML_UINT,  0, offsetof(MatroskaTrack, default_duration) },
    { MATROSKA_ID_TRACKTIMECODESCALE,    EBML_FLOAT, 0, offsetof(MatroskaTrack, time_scale),   { .f = 1.0  } },
    { MATROSKA_ID_TRACKFLAGDEFAULT,      EBML_UINT,  0, offsetof(MatroskaTrack, flag_default), { .u = 1  } },
    { MATROSKA_ID_TRACKFLAGFORCED,       EBML_UINT,  0, offsetof(MatroskaTrack, flag_forced),  { .u = 0   } },
    { MATROSKA_ID_TRACKVIDEO,            EBML_NEST,  0, offsetof(MatroskaTrack, video),        { .n = matroska_track_video } },
    { MATROSKA_ID_TRACKAUDIO,            EBML_NEST,  0, offsetof(MatroskaTrack, audio),        { .n = matroska_track_audio } },
    { MATROSKA_ID_TRACKCONTENTENCODINGS, EBML_NEST,  0, 0,                                     { .n = matroska_track_encodings } },
    { MATROSKA_ID_TRACKFLAGENABLED,      EBML_NONE },
    { MATROSKA_ID_TRACKFLAGLACING,       EBML_NONE },
    { MATROSKA_ID_CODECNAME,             EBML_NONE },
    { MATROSKA_ID_CODECDECODEALL,        EBML_NONE },
    { MATROSKA_ID_CODECINFOURL,          EBML_NONE },
    { MATROSKA_ID_CODECDOWNLOADURL,      EBML_NONE },
    { MATROSKA_ID_TRACKMINCACHE,         EBML_NONE },
    { MATROSKA_ID_TRACKMAXCACHE,         EBML_NONE },
    { MATROSKA_ID_TRACKMAXBLKADDID,      EBML_NONE },
    { 0 }
};

static EbmlSyntax matroska_tracks[] = {
    { MATROSKA_ID_TRACKENTRY, EBML_NEST, sizeof(MatroskaTrack), offsetof(MatroskaDemuxContext, tracks), { .n = matroska_track } },
    { 0 }
};

static EbmlSyntax matroska_attachment[] = {
    { MATROSKA_ID_FILEUID,      EBML_UINT, 0, offsetof(MatroskaAttachment, uid) },
    { MATROSKA_ID_FILENAME,     EBML_UTF8, 0, offsetof(MatroskaAttachment, filename) },
    { MATROSKA_ID_FILEMIMETYPE, EBML_STR,  0, offsetof(MatroskaAttachment, mime) },
    { MATROSKA_ID_FILEDATA,     EBML_BIN,  0, offsetof(MatroskaAttachment, bin) },
    { MATROSKA_ID_FILEDESC,     EBML_NONE },
    { 0 }
};

static EbmlSyntax matroska_attachments[] = {
    { MATROSKA_ID_ATTACHEDFILE, EBML_NEST, sizeof(MatroskaAttachment), offsetof(MatroskaDemuxContext, attachments), { .n = matroska_attachment } },
    { 0 }
};

static EbmlSyntax matroska_chapter_display[] = {
    { MATROSKA_ID_CHAPSTRING, EBML_UTF8, 0, offsetof(MatroskaChapter, title) },
    { MATROSKA_ID_CHAPLANG,   EBML_NONE },
    { 0 }
};

static EbmlSyntax matroska_chapter_entry[] = {
    { MATROSKA_ID_CHAPTERTIMESTART,   EBML_UINT, 0, offsetof(MatroskaChapter, start), { .u = AV_NOPTS_VALUE } },
    { MATROSKA_ID_CHAPTERTIMEEND,     EBML_UINT, 0, offsetof(MatroskaChapter, end),   { .u = AV_NOPTS_VALUE } },
    { MATROSKA_ID_CHAPTERUID,         EBML_UINT, 0, offsetof(MatroskaChapter, uid) },
    { MATROSKA_ID_CHAPTERDISPLAY,     EBML_NEST, 0,                        0,         { .n = matroska_chapter_display } },
    { MATROSKA_ID_CHAPTERFLAGHIDDEN,  EBML_NONE },
    { MATROSKA_ID_CHAPTERFLAGENABLED, EBML_NONE },
    { MATROSKA_ID_CHAPTERPHYSEQUIV,   EBML_NONE },
    { MATROSKA_ID_CHAPTERATOM,        EBML_NONE },
    { 0 }
};

static EbmlSyntax matroska_chapter[] = {
    { MATROSKA_ID_CHAPTERATOM,        EBML_NEST, sizeof(MatroskaChapter), offsetof(MatroskaDemuxContext, chapters), { .n = matroska_chapter_entry } },
    { MATROSKA_ID_EDITIONUID,         EBML_NONE },
    { MATROSKA_ID_EDITIONFLAGHIDDEN,  EBML_NONE },
    { MATROSKA_ID_EDITIONFLAGDEFAULT, EBML_NONE },
    { MATROSKA_ID_EDITIONFLAGORDERED, EBML_NONE },
    { 0 }
};

static EbmlSyntax matroska_chapters[] = {
    { MATROSKA_ID_EDITIONENTRY, EBML_NEST, 0, 0, { .n = matroska_chapter } },
    { 0 }
};

static EbmlSyntax matroska_index_pos[] = {
    { MATROSKA_ID_CUETRACK,           EBML_UINT, 0, offsetof(MatroskaIndexPos, track) },
    { MATROSKA_ID_CUECLUSTERPOSITION, EBML_UINT, 0, offsetof(MatroskaIndexPos, pos) },
    { MATROSKA_ID_CUEBLOCKNUMBER,     EBML_NONE },
    { 0 }
};

static EbmlSyntax matroska_index_entry[] = {
    { MATROSKA_ID_CUETIME,          EBML_UINT, 0,                        offsetof(MatroskaIndex, time) },
    { MATROSKA_ID_CUETRACKPOSITION, EBML_NEST, sizeof(MatroskaIndexPos), offsetof(MatroskaIndex, pos), { .n = matroska_index_pos } },
    { 0 }
};

static EbmlSyntax matroska_index[] = {
    { MATROSKA_ID_POINTENTRY, EBML_NEST, sizeof(MatroskaIndex), offsetof(MatroskaDemuxContext, index), { .n = matroska_index_entry } },
    { 0 }
};

static EbmlSyntax matroska_simpletag[] = {
    { MATROSKA_ID_TAGNAME,        EBML_UTF8, 0,                   offsetof(MatroskaTag, name) },
    { MATROSKA_ID_TAGSTRING,      EBML_UTF8, 0,                   offsetof(MatroskaTag, string) },
    { MATROSKA_ID_TAGLANG,        EBML_STR,  0,                   offsetof(MatroskaTag, lang), { .s = "und" } },
    { MATROSKA_ID_TAGDEFAULT,     EBML_UINT, 0,                   offsetof(MatroskaTag, def) },
    { MATROSKA_ID_TAGDEFAULT_BUG, EBML_UINT, 0,                   offsetof(MatroskaTag, def) },
    { MATROSKA_ID_SIMPLETAG,      EBML_NEST, sizeof(MatroskaTag), offsetof(MatroskaTag, sub),  { .n = matroska_simpletag } },
    { 0 }
};

static EbmlSyntax matroska_tagtargets[] = {
    { MATROSKA_ID_TAGTARGETS_TYPE,       EBML_STR,  0, offsetof(MatroskaTagTarget, type) },
    { MATROSKA_ID_TAGTARGETS_TYPEVALUE,  EBML_UINT, 0, offsetof(MatroskaTagTarget, typevalue), { .u = 50 } },
    { MATROSKA_ID_TAGTARGETS_TRACKUID,   EBML_UINT, 0, offsetof(MatroskaTagTarget, trackuid) },
    { MATROSKA_ID_TAGTARGETS_CHAPTERUID, EBML_UINT, 0, offsetof(MatroskaTagTarget, chapteruid) },
    { MATROSKA_ID_TAGTARGETS_ATTACHUID,  EBML_UINT, 0, offsetof(MatroskaTagTarget, attachuid) },
    { 0 }
};

static EbmlSyntax matroska_tag[] = {
    { MATROSKA_ID_SIMPLETAG,  EBML_NEST, sizeof(MatroskaTag), offsetof(MatroskaTags, tag),    { .n = matroska_simpletag } },
    { MATROSKA_ID_TAGTARGETS, EBML_NEST, 0,                   offsetof(MatroskaTags, target), { .n = matroska_tagtargets } },
    { 0 }
};

static EbmlSyntax matroska_tags[] = {
    { MATROSKA_ID_TAG, EBML_NEST, sizeof(MatroskaTags), offsetof(MatroskaDemuxContext, tags), { .n = matroska_tag } },
    { 0 }
};

static EbmlSyntax matroska_seekhead_entry[] = {
    { MATROSKA_ID_SEEKID,       EBML_UINT, 0, offsetof(MatroskaSeekhead, id) },
    { MATROSKA_ID_SEEKPOSITION, EBML_UINT, 0, offsetof(MatroskaSeekhead, pos), { .u = -1 } },
    { 0 }
};

static EbmlSyntax matroska_seekhead[] = {
    { MATROSKA_ID_SEEKENTRY, EBML_NEST, sizeof(MatroskaSeekhead), offsetof(MatroskaDemuxContext, seekhead), { .n = matroska_seekhead_entry } },
    { 0 }
};

static EbmlSyntax matroska_segment[] = {
    { MATROSKA_ID_INFO,        EBML_NEST, 0, 0, { .n = matroska_info } },
    { MATROSKA_ID_TRACKS,      EBML_NEST, 0, 0, { .n = matroska_tracks } },
    { MATROSKA_ID_ATTACHMENTS, EBML_NEST, 0, 0, { .n = matroska_attachments } },
    { MATROSKA_ID_CHAPTERS,    EBML_NEST, 0, 0, { .n = matroska_chapters } },
    { MATROSKA_ID_CUES,        EBML_NEST, 0, 0, { .n = matroska_index } },
    { MATROSKA_ID_TAGS,        EBML_NEST, 0, 0, { .n = matroska_tags } },
    { MATROSKA_ID_SEEKHEAD,    EBML_NEST, 0, 0, { .n = matroska_seekhead } },
    { MATROSKA_ID_CLUSTER,     EBML_STOP },
    { 0 }
};

static EbmlSyntax matroska_segments[] = {
    { MATROSKA_ID_SEGMENT, EBML_NEST, 0, 0, { .n = matroska_segment } },
    { 0 }
};

static EbmlSyntax matroska_blockgroup[] = {
    { MATROSKA_ID_BLOCK,          EBML_BIN,  0, offsetof(MatroskaBlock, bin) },
    { MATROSKA_ID_SIMPLEBLOCK,    EBML_BIN,  0, offsetof(MatroskaBlock, bin) },
    { MATROSKA_ID_BLOCKDURATION,  EBML_UINT, 0, offsetof(MatroskaBlock, duration), { .u = AV_NOPTS_VALUE } },
    { MATROSKA_ID_BLOCKREFERENCE, EBML_UINT, 0, offsetof(MatroskaBlock, reference) },
    { MATROSKA_ID_CODECSTATE,     EBML_NONE },
    {                          1, EBML_UINT, 0, offsetof(MatroskaBlock, non_simple), { .u = 1 } },
    { 0 }
};

static EbmlSyntax matroska_cluster[] = {
    { MATROSKA_ID_CLUSTERTIMECODE, EBML_UINT, 0,                     offsetof(MatroskaCluster, timecode) },
    { MATROSKA_ID_BLOCKGROUP,      EBML_NEST, sizeof(MatroskaBlock), offsetof(MatroskaCluster, blocks), { .n = matroska_blockgroup } },
    { MATROSKA_ID_SIMPLEBLOCK,     EBML_PASS, sizeof(MatroskaBlock), offsetof(MatroskaCluster, blocks), { .n = matroska_blockgroup } },
    { MATROSKA_ID_CLUSTERPOSITION, EBML_NONE },
    { MATROSKA_ID_CLUSTERPREVSIZE, EBML_NONE },
    { 0 }
};

static EbmlSyntax matroska_clusters[] = {
    { MATROSKA_ID_CLUSTER,  EBML_NEST, 0, 0, { .n = matroska_cluster } },
    { MATROSKA_ID_INFO,     EBML_NONE },
    { MATROSKA_ID_CUES,     EBML_NONE },
    { MATROSKA_ID_TAGS,     EBML_NONE },
    { MATROSKA_ID_SEEKHEAD, EBML_NONE },
    { 0 }
};

static EbmlSyntax matroska_cluster_incremental_parsing[] = {
    { MATROSKA_ID_CLUSTERTIMECODE, EBML_UINT, 0,                     offsetof(MatroskaCluster, timecode) },
    { MATROSKA_ID_BLOCKGROUP,      EBML_NEST, sizeof(MatroskaBlock), offsetof(MatroskaCluster, blocks), { .n = matroska_blockgroup } },
    { MATROSKA_ID_SIMPLEBLOCK,     EBML_PASS, sizeof(MatroskaBlock), offsetof(MatroskaCluster, blocks), { .n = matroska_blockgroup } },
    { MATROSKA_ID_CLUSTERPOSITION, EBML_NONE },
    { MATROSKA_ID_CLUSTERPREVSIZE, EBML_NONE },
    { MATROSKA_ID_INFO,            EBML_NONE },
    { MATROSKA_ID_CUES,            EBML_NONE },
    { MATROSKA_ID_TAGS,            EBML_NONE },
    { MATROSKA_ID_SEEKHEAD,        EBML_NONE },
    { MATROSKA_ID_CLUSTER,         EBML_STOP },
    { 0 }
};

static EbmlSyntax matroska_cluster_incremental[] = {
    { MATROSKA_ID_CLUSTERTIMECODE, EBML_UINT, 0, offsetof(MatroskaCluster, timecode) },
    { MATROSKA_ID_BLOCKGROUP,      EBML_STOP },
    { MATROSKA_ID_SIMPLEBLOCK,     EBML_STOP },
    { MATROSKA_ID_CLUSTERPOSITION, EBML_NONE },
    { MATROSKA_ID_CLUSTERPREVSIZE, EBML_NONE },
    { 0 }
};

static EbmlSyntax matroska_clusters_incremental[] = {
    { MATROSKA_ID_CLUSTER,  EBML_NEST, 0, 0, { .n = matroska_cluster_incremental } },
    { MATROSKA_ID_INFO,     EBML_NONE },
    { MATROSKA_ID_CUES,     EBML_NONE },
    { MATROSKA_ID_TAGS,     EBML_NONE },
    { MATROSKA_ID_SEEKHEAD, EBML_NONE },
    { 0 }
};

static const char *const matroska_doctypes[] = { "matroska", "webm" };

static int matroska_resync(MatroskaDemuxContext *matroska, int64_t last_pos)
{
    AVIOContext *pb = matroska->ctx->pb;
    uint32_t id;
    matroska->current_id = 0;
    matroska->num_levels = 0;

    /* seek to next position to resync from */
    if (avio_seek(pb, last_pos + 1, SEEK_SET) < 0)
        goto eof;

    id = avio_rb32(pb);

    // try to find a toplevel element
    while (!pb->eof_reached) {
        if (id == MATROSKA_ID_INFO     || id == MATROSKA_ID_TRACKS      ||
            id == MATROSKA_ID_CUES     || id == MATROSKA_ID_TAGS        ||
            id == MATROSKA_ID_SEEKHEAD || id == MATROSKA_ID_ATTACHMENTS ||
            id == MATROSKA_ID_CLUSTER  || id == MATROSKA_ID_CHAPTERS) {
            matroska->current_id = id;
            return 0;
        }
        id = (id << 8) | avio_r8(pb);
    }

eof:
    matroska->done = 1;
    return AVERROR_EOF;
}

/*
 * Return: Whether we reached the end of a level in the hierarchy or not.
 */
static int ebml_level_end(MatroskaDemuxContext *matroska)
{
    AVIOContext *pb = matroska->ctx->pb;
    int64_t pos = avio_tell(pb);

    if (matroska->num_levels > 0) {
        MatroskaLevel *level = &matroska->levels[matroska->num_levels - 1];
        if (pos - level->start >= level->length || matroska->current_id) {
            matroska->num_levels--;
            return 1;
        }
    }
    return 0;
}

/*
 * Read: an "EBML number", which is defined as a variable-length
 * array of bytes. The first byte indicates the length by giving a
 * number of 0-bits followed by a one. The position of the first
 * "one" bit inside the first byte indicates the length of this
 * number.
 * Returns: number of bytes read, < 0 on error
 */
static int ebml_read_num(MatroskaDemuxContext *matroska, AVIOContext *pb,
                         int max_size, uint64_t *number)
{
    int read = 1, n = 1;
    uint64_t total = 0;

    /* The first byte tells us the length in bytes - avio_r8() can normally
     * return 0, but since that's not a valid first ebmlID byte, we can
     * use it safely here to catch EOS. */
    if (!(total = avio_r8(pb))) {
        /* we might encounter EOS here */
        if (!pb->eof_reached) {
            int64_t pos = avio_tell(pb);
            av_log(matroska->ctx, AV_LOG_ERROR,
                   "Read error at pos. %"PRIu64" (0x%"PRIx64")\n",
                   pos, pos);
            return pb->error ? pb->error : AVERROR(EIO);
        }
        return AVERROR_EOF;
    }

    /* get the length of the EBML number */
    read = 8 - ff_log2_tab[total];
    if (read > max_size) {
        int64_t pos = avio_tell(pb) - 1;
        av_log(matroska->ctx, AV_LOG_ERROR,
               "Invalid EBML number size tag 0x%02x at pos %"PRIu64" (0x%"PRIx64")\n",
               (uint8_t) total, pos, pos);
        return AVERROR_INVALIDDATA;
    }

    /* read out length */
    total ^= 1 << ff_log2_tab[total];
    while (n++ < read)
        total = (total << 8) | avio_r8(pb);

    *number = total;

    return read;
}

/**
 * Read a EBML length value.
 * This needs special handling for the "unknown length" case which has multiple
 * encodings.
 */
static int ebml_read_length(MatroskaDemuxContext *matroska, AVIOContext *pb,
                            uint64_t *number)
{
    int res = ebml_read_num(matroska, pb, 8, number);
    if (res > 0 && *number + 1 == 1ULL << (7 * res))
        *number = 0xffffffffffffffULL;
    return res;
}

/*
 * Read the next element as an unsigned int.
 * 0 is success, < 0 is failure.
 */
static int ebml_read_uint(AVIOContext *pb, int size, uint64_t *num)
{
    int n = 0;

    if (size > 8)
        return AVERROR_INVALIDDATA;

    /* big-endian ordering; build up number */
    *num = 0;
    while (n++ < size)
        *num = (*num << 8) | avio_r8(pb);

    return 0;
}

/*
 * Read the next element as a float.
 * 0 is success, < 0 is failure.
 */
static int ebml_read_float(AVIOContext *pb, int size, double *num)
{
    if (size == 0)
        *num = 0;
    else if (size == 4)
        *num = av_int2float(avio_rb32(pb));
    else if (size == 8)
        *num = av_int2double(avio_rb64(pb));
    else
        return AVERROR_INVALIDDATA;

    return 0;
}

/*
 * Read the next element as an ASCII string.
 * 0 is success, < 0 is failure.
 */
static int ebml_read_ascii(AVIOContext *pb, int size, char **str)
{
    char *res;

    /* EBML strings are usually not 0-terminated, so we allocate one
     * byte more, read the string and NULL-terminate it ourselves. */
    if (!(res = av_malloc(size + 1)))
        return AVERROR(ENOMEM);
    if (avio_read(pb, (uint8_t *) res, size) != size) {
        av_free(res);
        return AVERROR(EIO);
    }
    (res)[size] = '\0';
    av_free(*str);
    *str = res;

    return 0;
}

/*
 * Read the next element as binary data.
 * 0 is success, < 0 is failure.
 */
static int ebml_read_binary(AVIOContext *pb, int length, EbmlBin *bin)
{
    av_free(bin->data);
    bin->size = 0;

    if (!(bin->data = av_mallocz(length + AV_INPUT_BUFFER_PADDING_SIZE)))
        return AVERROR(ENOMEM);

    bin->pos  = avio_tell(pb);
    if (avio_read(pb, bin->data, length) != length) {
        av_freep(&bin->data);
        return AVERROR(EIO);
    }

    bin->size = length;

    return 0;
}

/*
 * Read the next element, but only the header. The contents
 * are supposed to be sub-elements which can be read separately.
 * 0 is success, < 0 is failure.
 */
static int ebml_read_master(MatroskaDemuxContext *matroska, uint64_t length)
{
    AVIOContext *pb = matroska->ctx->pb;
    MatroskaLevel *level;

    if (matroska->num_levels >= EBML_MAX_DEPTH) {
        av_log(matroska->ctx, AV_LOG_ERROR,
               "File moves beyond max. allowed depth (%d)\n", EBML_MAX_DEPTH);
        return AVERROR(ENOSYS);
    }

    level         = &matroska->levels[matroska->num_levels++];
    level->start  = avio_tell(pb);
    level->length = length;

    return 0;
}

/*
 * Read signed/unsigned "EBML" numbers.
 * Return: number of bytes processed, < 0 on error
 */
static int matroska_ebmlnum_uint(MatroskaDemuxContext *matroska,
                                 uint8_t *data, uint32_t size, uint64_t *num)
{
    AVIOContext pb;
    ffio_init_context(&pb, data, size, 0, NULL, NULL, NULL, NULL);
    return ebml_read_num(matroska, &pb, FFMIN(size, 8), num);
}

/*
 * Same as above, but signed.
 */
static int matroska_ebmlnum_sint(MatroskaDemuxContext *matroska,
                                 uint8_t *data, uint32_t size, int64_t *num)
{
    uint64_t unum;
    int res;

    /* read as unsigned number first */
    if ((res = matroska_ebmlnum_uint(matroska, data, size, &unum)) < 0)
        return res;

    /* make signed (weird way) */
    *num = unum - ((1LL << (7 * res - 1)) - 1);

    return res;
}

static int ebml_parse_elem(MatroskaDemuxContext *matroska,
                           EbmlSyntax *syntax, void *data);

static int ebml_parse_id(MatroskaDemuxContext *matroska, EbmlSyntax *syntax,
                         uint32_t id, void *data)
{
    int i;
    for (i = 0; syntax[i].id; i++)
        if (id == syntax[i].id)
            break;
    if (!syntax[i].id && id == MATROSKA_ID_CLUSTER &&
        matroska->num_levels > 0                   &&
        matroska->levels[matroska->num_levels - 1].length == 0xffffffffffffff)
        return 0;  // we reached the end of an unknown size cluster
    if (!syntax[i].id && id != EBML_ID_VOID && id != EBML_ID_CRC32) {
        av_log(matroska->ctx, AV_LOG_INFO, "Unknown entry 0x%"PRIX32"\n", id);
        if (matroska->ctx->error_recognition & AV_EF_EXPLODE)
            return AVERROR_INVALIDDATA;
    }
    return ebml_parse_elem(matroska, &syntax[i], data);
}

static int ebml_parse(MatroskaDemuxContext *matroska, EbmlSyntax *syntax,
                      void *data)
{
    if (!matroska->current_id) {
        uint64_t id;
        int res = ebml_read_num(matroska, matroska->ctx->pb, 4, &id);
        if (res < 0)
            return res;
        matroska->current_id = id | 1 << 7 * res;
    }
    return ebml_parse_id(matroska, syntax, matroska->current_id, data);
}

static int ebml_parse_nest(MatroskaDemuxContext *matroska, EbmlSyntax *syntax,
                           void *data)
{
    int i, res = 0;

    for (i = 0; syntax[i].id; i++)
        switch (syntax[i].type) {
        case EBML_UINT:
            *(uint64_t *) ((char *) data + syntax[i].data_offset) = syntax[i].def.u;
            break;
        case EBML_FLOAT:
            *(double *) ((char *) data + syntax[i].data_offset) = syntax[i].def.f;
            break;
        case EBML_STR:
        case EBML_UTF8:
            // the default may be NULL
            if (syntax[i].def.s) {
                uint8_t **dst = (uint8_t **) ((uint8_t *) data + syntax[i].data_offset);
                *dst = av_strdup(syntax[i].def.s);
                if (!*dst)
                    return AVERROR(ENOMEM);
            }
            break;
        }

    while (!res && !ebml_level_end(matroska))
        res = ebml_parse(matroska, syntax, data);

    return res;
}

static int ebml_parse_elem(MatroskaDemuxContext *matroska,
                           EbmlSyntax *syntax, void *data)
{
    static const uint64_t max_lengths[EBML_TYPE_COUNT] = {
        [EBML_UINT]  = 8,
        [EBML_FLOAT] = 8,
        // max. 16 MB for strings
        [EBML_STR]   = 0x1000000,
        [EBML_UTF8]  = 0x1000000,
        // max. 256 MB for binary data
        [EBML_BIN]   = 0x10000000,
        // no limits for anything else
    };
    AVIOContext *pb = matroska->ctx->pb;
    uint32_t id = syntax->id;
    uint64_t length;
    int res;

    data = (char *) data + syntax->data_offset;
    if (syntax->list_elem_size) {
        EbmlList *list = data;
        if ((res = av_reallocp_array(&list->elem,
                                     list->nb_elem + 1,
                                     syntax->list_elem_size)) < 0) {
            list->nb_elem = 0;
            return res;
        }
        data = (char *) list->elem + list->nb_elem * syntax->list_elem_size;
        memset(data, 0, syntax->list_elem_size);
        list->nb_elem++;
    }

    if (syntax->type != EBML_PASS && syntax->type != EBML_STOP) {
        matroska->current_id = 0;
        if ((res = ebml_read_length(matroska, pb, &length)) < 0)
            return res;
        if (max_lengths[syntax->type] && length > max_lengths[syntax->type]) {
            av_log(matroska->ctx, AV_LOG_ERROR,
                   "Invalid length 0x%"PRIx64" > 0x%"PRIx64" for syntax element %i\n",
                   length, max_lengths[syntax->type], syntax->type);
            return AVERROR_INVALIDDATA;
        }
    }

    switch (syntax->type) {
    case EBML_UINT:
        res = ebml_read_uint(pb, length, data);
        break;
    case EBML_FLOAT:
        res = ebml_read_float(pb, length, data);
        break;
    case EBML_STR:
    case EBML_UTF8:
        res = ebml_read_ascii(pb, length, data);
        break;
    case EBML_BIN:
        res = ebml_read_binary(pb, length, data);
        break;
    case EBML_NEST:
        if ((res = ebml_read_master(matroska, length)) < 0)
            return res;
        if (id == MATROSKA_ID_SEGMENT)
            matroska->segment_start = avio_tell(matroska->ctx->pb);
        return ebml_parse_nest(matroska, syntax->def.n, data);
    case EBML_PASS:
        return ebml_parse_id(matroska, syntax->def.n, id, data);
    case EBML_STOP:
        return 1;
    default:
        return avio_skip(pb, length) < 0 ? AVERROR(EIO) : 0;
    }
    if (res == AVERROR_INVALIDDATA)
        av_log(matroska->ctx, AV_LOG_ERROR, "Invalid element\n");
    else if (res == AVERROR(EIO))
        av_log(matroska->ctx, AV_LOG_ERROR, "Read error\n");
    return res;
}

static void ebml_free(EbmlSyntax *syntax, void *data)
{
    int i, j;
    for (i = 0; syntax[i].id; i++) {
        void *data_off = (char *) data + syntax[i].data_offset;
        switch (syntax[i].type) {
        case EBML_STR:
        case EBML_UTF8:
            av_freep(data_off);
            break;
        case EBML_BIN:
            av_freep(&((EbmlBin *) data_off)->data);
            break;
        case EBML_NEST:
            if (syntax[i].list_elem_size) {
                EbmlList *list = data_off;
                char *ptr = list->elem;
                for (j = 0; j < list->nb_elem;
                     j++, ptr += syntax[i].list_elem_size)
                    ebml_free(syntax[i].def.n, ptr);
                av_free(list->elem);
            } else
                ebml_free(syntax[i].def.n, data_off);
        default:
            break;
        }
    }
}

/*
 * Autodetecting...
 */
static int matroska_probe(AVProbeData *p)
{
    uint64_t total = 0;
    int len_mask = 0x80, size = 1, n = 1, i;

    /* EBML header? */
    if (AV_RB32(p->buf) != EBML_ID_HEADER)
        return 0;

    /* length of header */
    total = p->buf[4];
    while (size <= 8 && !(total & len_mask)) {
        size++;
        len_mask >>= 1;
    }
    if (size > 8)
        return 0;
    total &= (len_mask - 1);
    while (n < size)
        total = (total << 8) | p->buf[4 + n++];

    /* Does the probe data contain the whole header? */
    if (p->buf_size < 4 + size + total)
        return 0;

    /* The header should contain a known document type. For now,
     * we don't parse the whole header but simply check for the
     * availability of that array of characters inside the header.
     * Not fully fool-proof, but good enough. */
    for (i = 0; i < FF_ARRAY_ELEMS(matroska_doctypes); i++) {
        int probelen = strlen(matroska_doctypes[i]);
        if (total < probelen)
            continue;
        for (n = 4 + size; n <= 4 + size + total - probelen; n++)
            if (!memcmp(p->buf + n, matroska_doctypes[i], probelen))
                return AVPROBE_SCORE_MAX;
    }

    // probably valid EBML header but no recognized doctype
    return AVPROBE_SCORE_EXTENSION;
}

static MatroskaTrack *matroska_find_track_by_num(MatroskaDemuxContext *matroska,
                                                 int num)
{
    MatroskaTrack *tracks = matroska->tracks.elem;
    int i;

    for (i = 0; i < matroska->tracks.nb_elem; i++)
        if (tracks[i].num == num)
            return &tracks[i];

    av_log(matroska->ctx, AV_LOG_ERROR, "Invalid track number %d\n", num);
    return NULL;
}

static int matroska_decode_buffer(uint8_t **buf, int *buf_size,
                                  MatroskaTrack *track)
{
    MatroskaTrackEncoding *encodings = track->encodings.elem;
    uint8_t *data = *buf;
    int isize = *buf_size;
    uint8_t *pkt_data = NULL;
    uint8_t av_unused *newpktdata;
    int pkt_size = isize;
    int result = 0;
    int olen;

    if (pkt_size >= 10000000)
        return AVERROR_INVALIDDATA;

    switch (encodings[0].compression.algo) {
    case MATROSKA_TRACK_ENCODING_COMP_HEADERSTRIP:
    {
        int header_size = encodings[0].compression.settings.size;
        uint8_t *header = encodings[0].compression.settings.data;

        if (!header_size)
            return 0;

        pkt_size = isize + header_size;
        pkt_data = av_malloc(pkt_size);
        if (!pkt_data)
            return AVERROR(ENOMEM);

        memcpy(pkt_data, header, header_size);
        memcpy(pkt_data + header_size, data, isize);
        break;
    }
#if CONFIG_LZO
    case MATROSKA_TRACK_ENCODING_COMP_LZO:
        do {
            olen       = pkt_size *= 3;
            newpktdata = av_realloc(pkt_data, pkt_size + AV_LZO_OUTPUT_PADDING);
            if (!newpktdata) {
                result = AVERROR(ENOMEM);
                goto failed;
            }
            pkt_data = newpktdata;
            result   = av_lzo1x_decode(pkt_data, &olen, data, &isize);
        } while (result == AV_LZO_OUTPUT_FULL && pkt_size < 10000000);
        if (result) {
            result = AVERROR_INVALIDDATA;
            goto failed;
        }
        pkt_size -= olen;
        break;
#endif
#if CONFIG_ZLIB
    case MATROSKA_TRACK_ENCODING_COMP_ZLIB:
    {
        z_stream zstream = { 0 };
        if (inflateInit(&zstream) != Z_OK)
            return -1;
        zstream.next_in  = data;
        zstream.avail_in = isize;
        do {
            pkt_size  *= 3;
            newpktdata = av_realloc(pkt_data, pkt_size);
            if (!newpktdata) {
                inflateEnd(&zstream);
                goto failed;
            }
            pkt_data          = newpktdata;
            zstream.avail_out = pkt_size - zstream.total_out;
            zstream.next_out  = pkt_data + zstream.total_out;
            result            = inflate(&zstream, Z_NO_FLUSH);
        } while (result == Z_OK && pkt_size < 10000000);
        pkt_size = zstream.total_out;
        inflateEnd(&zstream);
        if (result != Z_STREAM_END) {
            if (result == Z_MEM_ERROR)
                result = AVERROR(ENOMEM);
            else
                result = AVERROR_INVALIDDATA;
            goto failed;
        }
        break;
    }
#endif
#if CONFIG_BZLIB
    case MATROSKA_TRACK_ENCODING_COMP_BZLIB:
    {
        bz_stream bzstream = { 0 };
        if (BZ2_bzDecompressInit(&bzstream, 0, 0) != BZ_OK)
            return -1;
        bzstream.next_in  = data;
        bzstream.avail_in = isize;
        do {
            pkt_size  *= 3;
            newpktdata = av_realloc(pkt_data, pkt_size);
            if (!newpktdata) {
                BZ2_bzDecompressEnd(&bzstream);
                goto failed;
            }
            pkt_data           = newpktdata;
            bzstream.avail_out = pkt_size - bzstream.total_out_lo32;
            bzstream.next_out  = pkt_data + bzstream.total_out_lo32;
            result             = BZ2_bzDecompress(&bzstream);
        } while (result == BZ_OK && pkt_size < 10000000);
        pkt_size = bzstream.total_out_lo32;
        BZ2_bzDecompressEnd(&bzstream);
        if (result != BZ_STREAM_END) {
            if (result == BZ_MEM_ERROR)
                result = AVERROR(ENOMEM);
            else
                result = AVERROR_INVALIDDATA;
            goto failed;
        }
        break;
    }
#endif
    default:
        return AVERROR_INVALIDDATA;
    }

    *buf      = pkt_data;
    *buf_size = pkt_size;
    return 0;

failed:
    av_free(pkt_data);
    return result;
}

static void matroska_fix_ass_packet(MatroskaDemuxContext *matroska,
                                    AVPacket *pkt, uint64_t display_duration)
{
    AVBufferRef *line;
    char *layer, *ptr = pkt->data, *end = ptr + pkt->size;

    for (; *ptr != ',' && ptr < end - 1; ptr++)
        ;
    if (*ptr == ',')
        layer = ++ptr;
    for (; *ptr != ',' && ptr < end - 1; ptr++)
        ;
    if (*ptr == ',') {
        int64_t end_pts = pkt->pts + display_duration;
        int sc = matroska->time_scale * pkt->pts / 10000000;
        int ec = matroska->time_scale * end_pts  / 10000000;
        int sh, sm, ss, eh, em, es, len;
        sh     = sc / 360000;
        sc    -= 360000 * sh;
        sm     = sc / 6000;
        sc    -= 6000 * sm;
        ss     = sc / 100;
        sc    -= 100 * ss;
        eh     = ec / 360000;
        ec    -= 360000 * eh;
        em     = ec / 6000;
        ec    -= 6000 * em;
        es     = ec / 100;
        ec    -= 100 * es;
        *ptr++ = '\0';
        len    = 50 + end - ptr + AV_INPUT_BUFFER_PADDING_SIZE;
        if (!(line = av_buffer_alloc(len)))
            return;
        snprintf(line->data, len,
                 "Dialogue: %s,%d:%02d:%02d.%02d,%d:%02d:%02d.%02d,%s\r\n",
                 layer, sh, sm, ss, sc, eh, em, es, ec, ptr);
        av_buffer_unref(&pkt->buf);
        pkt->buf  = line;
        pkt->data = line->data;
        pkt->size = strlen(line->data);
    }
}

static int matroska_merge_packets(AVPacket *out, AVPacket *in)
{
    int old_size = out->size;
    int ret = av_grow_packet(out, in->size);
    if (ret < 0)
        return ret;

    memcpy(out->data + old_size, in->data, in->size);

    av_packet_unref(in);
    av_free(in);
    return 0;
}

static void matroska_convert_tag(AVFormatContext *s, EbmlList *list,
                                 AVDictionary **metadata, char *prefix)
{
    MatroskaTag *tags = list->elem;
    char key[1024];
    int i;

    for (i = 0; i < list->nb_elem; i++) {
        const char *lang = tags[i].lang &&
                           strcmp(tags[i].lang, "und") ? tags[i].lang : NULL;

        if (!tags[i].name) {
            av_log(s, AV_LOG_WARNING, "Skipping invalid tag with no TagName.\n");
            continue;
        }
        if (prefix)
            snprintf(key, sizeof(key), "%s/%s", prefix, tags[i].name);
        else
            av_strlcpy(key, tags[i].name, sizeof(key));
        if (tags[i].def || !lang) {
            av_dict_set(metadata, key, tags[i].string, 0);
            if (tags[i].sub.nb_elem)
                matroska_convert_tag(s, &tags[i].sub, metadata, key);
        }
        if (lang) {
            av_strlcat(key, "-", sizeof(key));
            av_strlcat(key, lang, sizeof(key));
            av_dict_set(metadata, key, tags[i].string, 0);
            if (tags[i].sub.nb_elem)
                matroska_convert_tag(s, &tags[i].sub, metadata, key);
        }
    }
    ff_metadata_conv(metadata, NULL, ff_mkv_metadata_conv);
}

static void matroska_convert_tags(AVFormatContext *s)
{
    MatroskaDemuxContext *matroska = s->priv_data;
    MatroskaTags *tags = matroska->tags.elem;
    int i, j;

    for (i = 0; i < matroska->tags.nb_elem; i++) {
        if (tags[i].target.attachuid) {
            MatroskaAttachment *attachment = matroska->attachments.elem;
            int found = 0;
            for (j = 0; j < matroska->attachments.nb_elem; j++) {
                if (attachment[j].uid == tags[i].target.attachuid &&
                    attachment[j].stream) {
                    matroska_convert_tag(s, &tags[i].tag,
                                         &attachment[j].stream->metadata, NULL);
                    found = 1;
                }
            }
            if (!found) {
                av_log(NULL, AV_LOG_WARNING,
                       "The tags at index %d refer to a "
                       "non-existent attachment %"PRId64".\n",
                       i, tags[i].target.attachuid);
            }
        } else if (tags[i].target.chapteruid) {
            MatroskaChapter *chapter = matroska->chapters.elem;
            int found = 0;
            for (j = 0; j < matroska->chapters.nb_elem; j++) {
                if (chapter[j].uid == tags[i].target.chapteruid &&
                    chapter[j].chapter) {
                    matroska_convert_tag(s, &tags[i].tag,
                                         &chapter[j].chapter->metadata, NULL);
                    found = 1;
                }
            }
            if (!found) {
                av_log(NULL, AV_LOG_WARNING,
                       "The tags at index %d refer to a non-existent chapter "
                       "%"PRId64".\n",
                       i, tags[i].target.chapteruid);
            }
        } else if (tags[i].target.trackuid) {
            MatroskaTrack *track = matroska->tracks.elem;
            int found = 0;
            for (j = 0; j < matroska->tracks.nb_elem; j++) {
                if (track[j].uid == tags[i].target.trackuid &&
                    track[j].stream) {
                    matroska_convert_tag(s, &tags[i].tag,
                                         &track[j].stream->metadata, NULL);
                    found = 1;
               }
            }
            if (!found) {
                av_log(NULL, AV_LOG_WARNING,
                       "The tags at index %d refer to a non-existent track "
                       "%"PRId64".\n",
                       i, tags[i].target.trackuid);
            }
        } else {
            matroska_convert_tag(s, &tags[i].tag, &s->metadata,
                                 tags[i].target.type);
        }
    }
}

static int matroska_parse_seekhead_entry(MatroskaDemuxContext *matroska,
                                         int idx)
{
    EbmlList *seekhead_list = &matroska->seekhead;
    uint32_t level_up       = matroska->level_up;
    uint32_t saved_id       = matroska->current_id;
    MatroskaSeekhead *seekhead = seekhead_list->elem;
    int64_t before_pos = avio_tell(matroska->ctx->pb);
    MatroskaLevel level;
    int64_t offset;
    int ret = 0;

    if (idx >= seekhead_list->nb_elem            ||
        seekhead[idx].id == MATROSKA_ID_SEEKHEAD ||
        seekhead[idx].id == MATROSKA_ID_CLUSTER)
        return 0;

    /* seek */
    offset = seekhead[idx].pos + matroska->segment_start;
    if (avio_seek(matroska->ctx->pb, offset, SEEK_SET) == offset) {
        /* We don't want to lose our seekhead level, so we add
         * a dummy. This is a crude hack. */
        if (matroska->num_levels == EBML_MAX_DEPTH) {
            av_log(matroska->ctx, AV_LOG_INFO,
                   "Max EBML element depth (%d) reached, "
                   "cannot parse further.\n", EBML_MAX_DEPTH);
            ret = AVERROR_INVALIDDATA;
        } else {
            level.start  = 0;
            level.length = (uint64_t) -1;
            matroska->levels[matroska->num_levels] = level;
            matroska->num_levels++;
            matroska->current_id                   = 0;

            ret = ebml_parse(matroska, matroska_segment, matroska);

            /* remove dummy level */
            while (matroska->num_levels) {
                uint64_t length = matroska->levels[--matroska->num_levels].length;
                if (length == (uint64_t) -1)
                    break;
            }
        }
    }
    /* seek back */
    avio_seek(matroska->ctx->pb, before_pos, SEEK_SET);
    matroska->level_up   = level_up;
    matroska->current_id = saved_id;

    return ret;
}

static void matroska_execute_seekhead(MatroskaDemuxContext *matroska)
{
    EbmlList *seekhead_list = &matroska->seekhead;
    int64_t before_pos = avio_tell(matroska->ctx->pb);
    int i;

    // we should not do any seeking in the streaming case
    if (!(matroska->ctx->pb->seekable & AVIO_SEEKABLE_NORMAL) ||
        (matroska->ctx->flags & AVFMT_FLAG_IGNIDX))
        return;

    for (i = 0; i < seekhead_list->nb_elem; i++) {
        MatroskaSeekhead *seekhead = seekhead_list->elem;
        if (seekhead[i].pos <= before_pos)
            continue;

        // defer cues parsing until we actually need cue data.
        if (seekhead[i].id == MATROSKA_ID_CUES) {
            matroska->cues_parsing_deferred = 1;
            continue;
        }

        if (matroska_parse_seekhead_entry(matroska, i) < 0)
            break;
    }
}

static void matroska_parse_cues(MatroskaDemuxContext *matroska)
{
    EbmlList *seekhead_list = &matroska->seekhead;
    MatroskaSeekhead *seekhead = seekhead_list->elem;
    EbmlList *index_list;
    MatroskaIndex *index;
    int index_scale = 1;
    int i, j;

    for (i = 0; i < seekhead_list->nb_elem; i++)
        if (seekhead[i].id == MATROSKA_ID_CUES)
            break;
    assert(i <= seekhead_list->nb_elem);

    matroska_parse_seekhead_entry(matroska, i);

    index_list = &matroska->index;
    index      = index_list->elem;
    if (index_list->nb_elem &&
        index[0].time > 1E14 / matroska->time_scale) {
        av_log(matroska->ctx, AV_LOG_WARNING, "Working around broken index.\n");
        index_scale = matroska->time_scale;
    }
    for (i = 0; i < index_list->nb_elem; i++) {
        EbmlList *pos_list    = &index[i].pos;
        MatroskaIndexPos *pos = pos_list->elem;
        for (j = 0; j < pos_list->nb_elem; j++) {
            MatroskaTrack *track = matroska_find_track_by_num(matroska,
                                                              pos[j].track);
            if (track && track->stream)
                av_add_index_entry(track->stream,
                                   pos[j].pos + matroska->segment_start,
                                   index[i].time / index_scale, 0, 0,
                                   AVINDEX_KEYFRAME);
        }
    }
}

static int matroska_aac_profile(char *codec_id)
{
    static const char *const aac_profiles[] = { "MAIN", "LC", "SSR" };
    int profile;

    for (profile = 0; profile < FF_ARRAY_ELEMS(aac_profiles); profile++)
        if (strstr(codec_id, aac_profiles[profile]))
            break;
    return profile + 1;
}

static int matroska_aac_sri(int samplerate)
{
    int sri;

    for (sri = 0; sri < FF_ARRAY_ELEMS(avpriv_mpeg4audio_sample_rates); sri++)
        if (avpriv_mpeg4audio_sample_rates[sri] == samplerate)
            break;
    return sri;
}

static int matroska_parse_flac(AVFormatContext *s,
                               MatroskaTrack *track,
                               int *offset)
{
    AVStream *st = track->stream;
    uint8_t *p = track->codec_priv.data;
    int size   = track->codec_priv.size;

    if (size < 8 + FLAC_STREAMINFO_SIZE || p[4] & 0x7f) {
        av_log(s, AV_LOG_WARNING, "Invalid FLAC private data\n");
        track->codec_priv.size = 0;
        return 0;
    }
    *offset = 8;
    track->codec_priv.size = 8 + FLAC_STREAMINFO_SIZE;

    p    += track->codec_priv.size;
    size -= track->codec_priv.size;

    /* parse the remaining metadata blocks if present */
    while (size >= 4) {
        int block_last, block_type, block_size;

        flac_parse_block_header(p, &block_last, &block_type, &block_size);

        p    += 4;
        size -= 4;
        if (block_size > size)
            return 0;

        /* check for the channel mask */
        if (block_type == FLAC_METADATA_TYPE_VORBIS_COMMENT) {
            AVDictionary *dict = NULL;
            AVDictionaryEntry *chmask;

            ff_vorbis_comment(s, &dict, p, block_size, 0);
            chmask = av_dict_get(dict, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK", NULL, 0);
            if (chmask) {
                uint64_t mask = strtol(chmask->value, NULL, 0);
                if (!mask || mask & ~0x3ffffULL) {
                    av_log(s, AV_LOG_WARNING,
                           "Invalid value of WAVEFORMATEXTENSIBLE_CHANNEL_MASK\n");
                } else
                    st->codecpar->channel_layout = mask;
            }
            av_dict_free(&dict);
        }

        p    += block_size;
        size -= block_size;
    }

    return 0;
}

static int mkv_field_order(int64_t field_order)
{
    switch (field_order) {
    case MATROSKA_VIDEO_FIELDORDER_PROGRESSIVE:
        return AV_FIELD_PROGRESSIVE;
    case MATROSKA_VIDEO_FIELDORDER_UNDETERMINED:
        return AV_FIELD_UNKNOWN;
    case MATROSKA_VIDEO_FIELDORDER_TT:
        return AV_FIELD_TT;
    case MATROSKA_VIDEO_FIELDORDER_BB:
        return AV_FIELD_BB;
    case MATROSKA_VIDEO_FIELDORDER_BT:
        return AV_FIELD_BT;
    case MATROSKA_VIDEO_FIELDORDER_TB:
        return AV_FIELD_TB;
    default:
        return AV_FIELD_UNKNOWN;
    }
}

static void mkv_stereo_mode_display_mul(int stereo_mode,
                                        int *h_width, int *h_height)
{
    switch (stereo_mode) {
    case MATROSKA_VIDEO_STEREOMODE_TYPE_MONO:
    case MATROSKA_VIDEO_STEREOMODE_TYPE_CHECKERBOARD_RL:
    case MATROSKA_VIDEO_STEREOMODE_TYPE_CHECKERBOARD_LR:
    case MATROSKA_VIDEO_STEREOMODE_TYPE_BOTH_EYES_BLOCK_RL:
    case MATROSKA_VIDEO_STEREOMODE_TYPE_BOTH_EYES_BLOCK_LR:
        break;
    case MATROSKA_VIDEO_STEREOMODE_TYPE_RIGHT_LEFT:
    case MATROSKA_VIDEO_STEREOMODE_TYPE_LEFT_RIGHT:
    case MATROSKA_VIDEO_STEREOMODE_TYPE_COL_INTERLEAVED_RL:
    case MATROSKA_VIDEO_STEREOMODE_TYPE_COL_INTERLEAVED_LR:
        *h_width = 2;
        break;
    case MATROSKA_VIDEO_STEREOMODE_TYPE_BOTTOM_TOP:
    case MATROSKA_VIDEO_STEREOMODE_TYPE_TOP_BOTTOM:
    case MATROSKA_VIDEO_STEREOMODE_TYPE_ROW_INTERLEAVED_RL:
    case MATROSKA_VIDEO_STEREOMODE_TYPE_ROW_INTERLEAVED_LR:
        *h_height = 2;
        break;
    }
}

static int mkv_parse_video_projection(AVStream *st, const MatroskaTrack *track)
{
    AVSphericalMapping *spherical;
    enum AVSphericalProjection projection;
    size_t spherical_size;
    uint32_t l = 0, t = 0, r = 0, b = 0;
    uint32_t padding = 0;
    int ret;
    GetByteContext gb;

    bytestream2_init(&gb, track->video.projection.private.data,
                     track->video.projection.private.size);

    if (bytestream2_get_byte(&gb) != 0) {
        av_log(NULL, AV_LOG_WARNING, "Unknown spherical metadata\n");
        return 0;
    }

    bytestream2_skip(&gb, 3); // flags

    switch (track->video.projection.type) {
    case MATROSKA_VIDEO_PROJECTION_TYPE_EQUIRECTANGULAR:
        if (track->video.projection.private.size == 20) {
            t = bytestream2_get_be32(&gb);
            b = bytestream2_get_be32(&gb);
            l = bytestream2_get_be32(&gb);
            r = bytestream2_get_be32(&gb);

            if (b >= UINT_MAX - t || r >= UINT_MAX - l) {
                av_log(NULL, AV_LOG_ERROR,
                       "Invalid bounding rectangle coordinates "
                       "%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32"\n", l, t, r, b);
                return AVERROR_INVALIDDATA;
            }
        } else if (track->video.projection.private.size != 0) {
            av_log(NULL, AV_LOG_ERROR, "Unknown spherical metadata\n");
            return AVERROR_INVALIDDATA;
        }

        if (l || t || r || b)
            projection = AV_SPHERICAL_EQUIRECTANGULAR_TILE;
        else
            projection = AV_SPHERICAL_EQUIRECTANGULAR;
        break;
    case MATROSKA_VIDEO_PROJECTION_TYPE_CUBEMAP:
        if (track->video.projection.private.size < 4) {
            av_log(NULL, AV_LOG_ERROR, "Missing projection private properties\n");
            return AVERROR_INVALIDDATA;
        } else if (track->video.projection.private.size == 12) {
            uint32_t layout = bytestream2_get_be32(&gb);
            if (layout) {
                av_log(NULL, AV_LOG_WARNING,
                       "Unknown spherical cubemap layout %"PRIu32"\n", layout);
                return 0;
            }
            projection = AV_SPHERICAL_CUBEMAP;
            padding = bytestream2_get_be32(&gb);
        } else {
            av_log(NULL, AV_LOG_ERROR, "Unknown spherical metadata\n");
            return AVERROR_INVALIDDATA;
        }
        break;
    case MATROSKA_VIDEO_PROJECTION_TYPE_RECTANGULAR:
        /* No Spherical metadata */
        return 0;
    default:
        av_log(NULL, AV_LOG_WARNING,
               "Unknown spherical metadata type %"PRIu64"\n",
               track->video.projection.type);
        return 0;
    }

    spherical = av_spherical_alloc(&spherical_size);
    if (!spherical)
        return AVERROR(ENOMEM);

    spherical->projection = projection;

    spherical->yaw   = (int32_t) (track->video.projection.yaw   * (1 << 16));
    spherical->pitch = (int32_t) (track->video.projection.pitch * (1 << 16));
    spherical->roll  = (int32_t) (track->video.projection.roll  * (1 << 16));

    spherical->padding = padding;

    spherical->bound_left   = l;
    spherical->bound_top    = t;
    spherical->bound_right  = r;
    spherical->bound_bottom = b;

    ret = av_stream_add_side_data(st, AV_PKT_DATA_SPHERICAL, (uint8_t *)spherical,
                                  spherical_size);
    if (ret < 0) {
        av_free(spherical);
        return ret;
    }

    return 0;
}

static int matroska_parse_tracks(AVFormatContext *s)
{
    MatroskaDemuxContext *matroska = s->priv_data;
    MatroskaTrack *tracks = matroska->tracks.elem;
    AVStream *st;
    int i, j, ret;

    for (i = 0; i < matroska->tracks.nb_elem; i++) {
        MatroskaTrack *track = &tracks[i];
        enum AVCodecID codec_id = AV_CODEC_ID_NONE;
        EbmlList *encodings_list = &track->encodings;
        MatroskaTrackEncoding *encodings = encodings_list->elem;
        uint8_t *extradata = NULL;
        int extradata_size = 0;
        int extradata_offset = 0;
        AVIOContext b;

        /* Apply some sanity checks. */
        if (track->type != MATROSKA_TRACK_TYPE_VIDEO &&
            track->type != MATROSKA_TRACK_TYPE_AUDIO &&
            track->type != MATROSKA_TRACK_TYPE_SUBTITLE) {
            av_log(matroska->ctx, AV_LOG_INFO,
                   "Unknown or unsupported track type %"PRIu64"\n",
                   track->type);
            continue;
        }
        if (!track->codec_id)
            continue;

        if (track->type == MATROSKA_TRACK_TYPE_VIDEO) {
            if (!track->default_duration && track->video.frame_rate > 0)
                track->default_duration = 1000000000 / track->video.frame_rate;
            if (!track->video.display_width)
                track->video.display_width = track->video.pixel_width;
            if (!track->video.display_height)
                track->video.display_height = track->video.pixel_height;
        } else if (track->type == MATROSKA_TRACK_TYPE_AUDIO) {
            if (!track->audio.out_samplerate)
                track->audio.out_samplerate = track->audio.samplerate;
        }
        if (encodings_list->nb_elem > 1) {
            av_log(matroska->ctx, AV_LOG_ERROR,
                   "Multiple combined encodings not supported");
        } else if (encodings_list->nb_elem == 1) {
            if (encodings[0].type ||
                (
#if CONFIG_ZLIB
                 encodings[0].compression.algo != MATROSKA_TRACK_ENCODING_COMP_ZLIB  &&
#endif
#if CONFIG_BZLIB
                 encodings[0].compression.algo != MATROSKA_TRACK_ENCODING_COMP_BZLIB &&
#endif
#if CONFIG_LZO
                 encodings[0].compression.algo != MATROSKA_TRACK_ENCODING_COMP_LZO   &&
#endif
                 encodings[0].compression.algo != MATROSKA_TRACK_ENCODING_COMP_HEADERSTRIP)) {
                encodings[0].scope = 0;
                av_log(matroska->ctx, AV_LOG_ERROR,
                       "Unsupported encoding type");
            } else if (track->codec_priv.size && encodings[0].scope & 2) {
                uint8_t *codec_priv = track->codec_priv.data;
                int ret = matroska_decode_buffer(&track->codec_priv.data,
                                                 &track->codec_priv.size,
                                                 track);
                if (ret < 0) {
                    track->codec_priv.data = NULL;
                    track->codec_priv.size = 0;
                    av_log(matroska->ctx, AV_LOG_ERROR,
                           "Failed to decode codec private data\n");
                }

                if (codec_priv != track->codec_priv.data)
                    av_free(codec_priv);
            }
        }

        for (j = 0; ff_mkv_codec_tags[j].id != AV_CODEC_ID_NONE; j++) {
            if (!strncmp(ff_mkv_codec_tags[j].str, track->codec_id,
                         strlen(ff_mkv_codec_tags[j].str))) {
                codec_id = ff_mkv_codec_tags[j].id;
                break;
            }
        }

        st = track->stream = avformat_new_stream(s, NULL);
        if (!st)
            return AVERROR(ENOMEM);

        if (!strcmp(track->codec_id, "V_MS/VFW/FOURCC") &&
            track->codec_priv.size >= 40                &&
            track->codec_priv.data) {
            track->ms_compat    = 1;
            track->video.fourcc = AV_RL32(track->codec_priv.data + 16);
            codec_id            = ff_codec_get_id(ff_codec_bmp_tags,
                                                  track->video.fourcc);
            extradata_offset    = 40;
        } else if (!strcmp(track->codec_id, "A_MS/ACM") &&
                   track->codec_priv.size >= 14         &&
                   track->codec_priv.data) {
            int ret;
            ffio_init_context(&b, track->codec_priv.data,
                              track->codec_priv.size,
                              0, NULL, NULL, NULL, NULL);
            ret = ff_get_wav_header(s, &b, st->codecpar, track->codec_priv.size);
            if (ret < 0)
                return ret;
            codec_id         = st->codecpar->codec_id;
            extradata_offset = FFMIN(track->codec_priv.size, 18);
        } else if (!strcmp(track->codec_id, "V_QUICKTIME") &&
                   (track->codec_priv.size >= 86)          &&
                   (track->codec_priv.data)) {
            if (track->codec_priv.size == AV_RB32(track->codec_priv.data)) {
                track->video.fourcc = AV_RL32(track->codec_priv.data + 4);
                codec_id            = ff_codec_get_id(ff_codec_movvideo_tags,
                                                      track->video.fourcc);
            }
            if (codec_id == AV_CODEC_ID_NONE) {
                track->video.fourcc = AV_RL32(track->codec_priv.data);
                codec_id            = ff_codec_get_id(ff_codec_movvideo_tags,
                                                      track->video.fourcc);
            }
            if (codec_id == AV_CODEC_ID_NONE) {
                char buf[32];
                av_get_codec_tag_string(buf, sizeof(buf), track->video.fourcc);
                av_log(matroska->ctx, AV_LOG_ERROR,
                       "mov FourCC not found %s.\n", buf);
            }
        } else if (codec_id == AV_CODEC_ID_PCM_S16BE) {
            switch (track->audio.bitdepth) {
            case  8:
                codec_id = AV_CODEC_ID_PCM_U8;
                break;
            case 24:
                codec_id = AV_CODEC_ID_PCM_S24BE;
                break;
            case 32:
                codec_id = AV_CODEC_ID_PCM_S32BE;
                break;
            }
        } else if (codec_id == AV_CODEC_ID_PCM_S16LE) {
            switch (track->audio.bitdepth) {
            case  8:
                codec_id = AV_CODEC_ID_PCM_U8;
                break;
            case 24:
                codec_id = AV_CODEC_ID_PCM_S24LE;
                break;
            case 32:
                codec_id = AV_CODEC_ID_PCM_S32LE;
                break;
            }
        } else if (codec_id == AV_CODEC_ID_PCM_F32LE &&
                   track->audio.bitdepth == 64) {
            codec_id = AV_CODEC_ID_PCM_F64LE;
        } else if (codec_id == AV_CODEC_ID_AAC && !track->codec_priv.size) {
            int profile = matroska_aac_profile(track->codec_id);
            int sri     = matroska_aac_sri(track->audio.samplerate);
            extradata   = av_mallocz(5 + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!extradata)
                return AVERROR(ENOMEM);
            extradata[0] = (profile << 3) | ((sri & 0x0E) >> 1);
            extradata[1] = ((sri & 0x01) << 7) | (track->audio.channels << 3);
            if (strstr(track->codec_id, "SBR")) {
                sri            = matroska_aac_sri(track->audio.out_samplerate);
                extradata[2]   = 0x56;
                extradata[3]   = 0xE5;
                extradata[4]   = 0x80 | (sri << 3);
                extradata_size = 5;
            } else
                extradata_size = 2;
        } else if (codec_id == AV_CODEC_ID_ALAC && track->codec_priv.size) {
            /* Only ALAC's magic cookie is stored in Matroska's track headers.
             * Create the "atom size", "tag", and "tag version" fields the
             * decoder expects manually. */
            extradata_size = 12 + track->codec_priv.size;
            extradata      = av_mallocz(extradata_size +
                                        AV_INPUT_BUFFER_PADDING_SIZE);
            if (!extradata)
                return AVERROR(ENOMEM);
            AV_WB32(extradata, extradata_size);
            memcpy(&extradata[4], "alac", 4);
            AV_WB32(&extradata[8], 0);
            memcpy(&extradata[12], track->codec_priv.data,
                   track->codec_priv.size);
        } else if (codec_id == AV_CODEC_ID_TTA) {
            extradata_size = 30;
            extradata      = av_mallocz(extradata_size);
            if (!extradata)
                return AVERROR(ENOMEM);
            ffio_init_context(&b, extradata, extradata_size, 1,
                              NULL, NULL, NULL, NULL);
            avio_write(&b, "TTA1", 4);
            avio_wl16(&b, 1);
            avio_wl16(&b, track->audio.channels);
            avio_wl16(&b, track->audio.bitdepth);
            avio_wl32(&b, track->audio.out_samplerate);
            avio_wl32(&b, matroska->ctx->duration *
                      track->audio.out_samplerate);
        } else if (codec_id == AV_CODEC_ID_RV10 ||
                   codec_id == AV_CODEC_ID_RV20 ||
                   codec_id == AV_CODEC_ID_RV30 ||
                   codec_id == AV_CODEC_ID_RV40) {
            extradata_offset = 26;
        } else if (codec_id == AV_CODEC_ID_RA_144) {
            track->audio.out_samplerate = 8000;
            track->audio.channels       = 1;
        } else if (codec_id == AV_CODEC_ID_RA_288 ||
                   codec_id == AV_CODEC_ID_COOK   ||
                   codec_id == AV_CODEC_ID_ATRAC3 ||
                   codec_id == AV_CODEC_ID_SIPR) {
            int flavor;
            ffio_init_context(&b, track->codec_priv.data,
                              track->codec_priv.size,
                              0, NULL, NULL, NULL, NULL);
            avio_skip(&b, 22);
            flavor                       = avio_rb16(&b);
            track->audio.coded_framesize = avio_rb32(&b);
            avio_skip(&b, 12);
            track->audio.sub_packet_h    = avio_rb16(&b);
            track->audio.frame_size      = avio_rb16(&b);
            track->audio.sub_packet_size = avio_rb16(&b);
            if (flavor                        < 0 ||
                track->audio.coded_framesize <= 0 ||
                track->audio.sub_packet_h    <= 0 ||
                track->audio.frame_size      <= 0 ||
                track->audio.sub_packet_size <= 0)
                return AVERROR_INVALIDDATA;
            track->audio.buf = av_malloc(track->audio.frame_size *
                                         track->audio.sub_packet_h);
            if (!track->audio.buf)
                return AVERROR(ENOMEM);
            if (codec_id == AV_CODEC_ID_RA_288) {
                st->codecpar->block_align = track->audio.coded_framesize;
                track->codec_priv.size = 0;
            } else {
                if (codec_id == AV_CODEC_ID_SIPR && flavor < 4) {
                    static const int sipr_bit_rate[4] = { 6504, 8496, 5000, 16000 };
                    track->audio.sub_packet_size = ff_sipr_subpk_size[flavor];
                    st->codecpar->bit_rate          = sipr_bit_rate[flavor];
                }
                st->codecpar->block_align = track->audio.sub_packet_size;
                extradata_offset       = 78;
            }
        } else if (codec_id == AV_CODEC_ID_FLAC && track->codec_priv.size) {
            ret = matroska_parse_flac(s, track, &extradata_offset);
            if (ret < 0)
                return ret;
        }
        track->codec_priv.size -= extradata_offset;

        if (codec_id == AV_CODEC_ID_NONE)
            av_log(matroska->ctx, AV_LOG_INFO,
                   "Unknown/unsupported AVCodecID %s.\n", track->codec_id);

        if (track->time_scale < 0.01)
            track->time_scale = 1.0;
        avpriv_set_pts_info(st, 64, matroska->time_scale * track->time_scale,
                            1000 * 1000 * 1000);    /* 64 bit pts in ns */

        if (track->type == MATROSKA_TRACK_TYPE_AUDIO &&
            track->audio.out_samplerate) {
            st->codecpar->initial_padding = av_rescale_q(track->codec_delay,
                                                         (AVRational){ 1, 1000000000 },
                                                         (AVRational){ 1, track->audio.out_samplerate });
        }

        /* convert the delay from ns to the track timebase */
        track->codec_delay = av_rescale_q(track->codec_delay,
                                          (AVRational){ 1, 1000000000 },
                                          st->time_base);

        st->codecpar->codec_id = codec_id;
        st->start_time      = 0;
        if (strcmp(track->language, "und"))
            av_dict_set(&st->metadata, "language", track->language, 0);
        av_dict_set(&st->metadata, "title", track->name, 0);

        if (track->flag_default)
            st->disposition |= AV_DISPOSITION_DEFAULT;
        if (track->flag_forced)
            st->disposition |= AV_DISPOSITION_FORCED;

        if (!st->codecpar->extradata) {
            if (extradata) {
                st->codecpar->extradata      = extradata;
                st->codecpar->extradata_size = extradata_size;
            } else if (track->codec_priv.data && track->codec_priv.size > 0) {
                st->codecpar->extradata = av_mallocz(track->codec_priv.size +
                                                     AV_INPUT_BUFFER_PADDING_SIZE);
                if (!st->codecpar->extradata)
                    return AVERROR(ENOMEM);
                st->codecpar->extradata_size = track->codec_priv.size;
                memcpy(st->codecpar->extradata,
                       track->codec_priv.data + extradata_offset,
                       track->codec_priv.size);
            }
        }

        if (track->type == MATROSKA_TRACK_TYPE_VIDEO) {
            int display_width_mul  = 1;
            int display_height_mul = 1;

            st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            st->codecpar->codec_tag  = track->video.fourcc;
            st->codecpar->width      = track->video.pixel_width;
            st->codecpar->height     = track->video.pixel_height;

            if (track->video.interlaced == MATROSKA_VIDEO_INTERLACE_FLAG_INTERLACED)
                st->codecpar->field_order = mkv_field_order(track->video.field_order);

            if (track->video.stereo_mode && track->video.stereo_mode < MATROSKA_VIDEO_STEREOMODE_TYPE_NB)
                mkv_stereo_mode_display_mul(track->video.stereo_mode, &display_width_mul, &display_height_mul);

            av_reduce(&st->sample_aspect_ratio.num,
                      &st->sample_aspect_ratio.den,
                      st->codecpar->height * track->video.display_width  * display_width_mul,
                      st->codecpar->width  * track->video.display_height * display_height_mul,
                      255);
            if (st->codecpar->codec_id != AV_CODEC_ID_H264 &&
                st->codecpar->codec_id != AV_CODEC_ID_HEVC)
                st->need_parsing = AVSTREAM_PARSE_HEADERS;
            if (track->default_duration) {
                av_reduce(&st->avg_frame_rate.num, &st->avg_frame_rate.den,
                          1000000000, track->default_duration, 30000);
            }
            // add stream level stereo3d side data if it is a supported format
            if (track->video.stereo_mode < MATROSKA_VIDEO_STEREOMODE_TYPE_NB &&
                track->video.stereo_mode != 10 && track->video.stereo_mode != 12) {
                int ret = ff_mkv_stereo3d_conv(st, track->video.stereo_mode);
                if (ret < 0)
                    return ret;
            }

            ret = mkv_parse_video_projection(st, track);
            if (ret < 0)
                return ret;
        } else if (track->type == MATROSKA_TRACK_TYPE_AUDIO) {
            st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
            st->codecpar->sample_rate = track->audio.out_samplerate;
            st->codecpar->channels    = track->audio.channels;
            if (st->codecpar->codec_id != AV_CODEC_ID_AAC)
                st->need_parsing = AVSTREAM_PARSE_HEADERS;
            if (st->codecpar->codec_id == AV_CODEC_ID_MP3)
                st->need_parsing = AVSTREAM_PARSE_FULL;
        } else if (track->type == MATROSKA_TRACK_TYPE_SUBTITLE) {
            st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
            if (st->codecpar->codec_id == AV_CODEC_ID_SSA)
                matroska->contains_ssa = 1;
        }
    }

    return 0;
}

static int matroska_read_header(AVFormatContext *s)
{
    MatroskaDemuxContext *matroska = s->priv_data;
    EbmlList *attachments_list = &matroska->attachments;
    EbmlList *chapters_list    = &matroska->chapters;
    MatroskaAttachment *attachments;
    MatroskaChapter *chapters;
    uint64_t max_start = 0;
    int64_t pos;
    Ebml ebml = { 0 };
    int i, j, res;

    matroska->ctx = s;

    /* First read the EBML header. */
    if (ebml_parse(matroska, ebml_syntax, &ebml) || !ebml.doctype) {
        av_log(matroska->ctx, AV_LOG_ERROR, "EBML header parsing failed\n");
        ebml_free(ebml_syntax, &ebml);
        return AVERROR_INVALIDDATA;
    }
    if (ebml.version         > EBML_VERSION      ||
        ebml.max_size        > sizeof(uint64_t)  ||
        ebml.id_length       > sizeof(uint32_t)  ||
        ebml.doctype_version > 3) {
        avpriv_report_missing_feature(matroska->ctx,
                                      "EBML version %"PRIu64", doctype %s, doc version %"PRIu64,
                                      ebml.version, ebml.doctype, ebml.doctype_version);
        ebml_free(ebml_syntax, &ebml);
        return AVERROR_PATCHWELCOME;
    }
    for (i = 0; i < FF_ARRAY_ELEMS(matroska_doctypes); i++)
        if (!strcmp(ebml.doctype, matroska_doctypes[i]))
            break;
    if (i >= FF_ARRAY_ELEMS(matroska_doctypes)) {
        av_log(s, AV_LOG_WARNING, "Unknown EBML doctype '%s'\n", ebml.doctype);
        if (matroska->ctx->error_recognition & AV_EF_EXPLODE) {
            ebml_free(ebml_syntax, &ebml);
            return AVERROR_INVALIDDATA;
        }
    }
    ebml_free(ebml_syntax, &ebml);

    /* The next thing is a segment. */
    pos = avio_tell(matroska->ctx->pb);
    res = ebml_parse(matroska, matroska_segments, matroska);
    // try resyncing until we find a EBML_STOP type element.
    while (res != 1) {
        res = matroska_resync(matroska, pos);
        if (res < 0)
            return res;
        pos = avio_tell(matroska->ctx->pb);
        res = ebml_parse(matroska, matroska_segment, matroska);
    }
    matroska_execute_seekhead(matroska);

    if (!matroska->time_scale)
        matroska->time_scale = 1000000;
    if (matroska->duration)
        matroska->ctx->duration = matroska->duration * matroska->time_scale *
                                  1000 / AV_TIME_BASE;
    av_dict_set(&s->metadata, "title", matroska->title, 0);

    res = matroska_parse_tracks(s);
    if (res < 0)
        return res;

    attachments = attachments_list->elem;
    for (j = 0; j < attachments_list->nb_elem; j++) {
        if (!(attachments[j].filename && attachments[j].mime &&
              attachments[j].bin.data && attachments[j].bin.size > 0)) {
            av_log(matroska->ctx, AV_LOG_ERROR, "incomplete attachment\n");
        } else {
            AVStream *st = avformat_new_stream(s, NULL);
            if (!st)
                break;
            av_dict_set(&st->metadata, "filename", attachments[j].filename, 0);
            av_dict_set(&st->metadata, "mimetype", attachments[j].mime, 0);
            st->codecpar->codec_id   = AV_CODEC_ID_NONE;

            for (i = 0; ff_mkv_image_mime_tags[i].id != AV_CODEC_ID_NONE; i++) {
                if (!strncmp(ff_mkv_image_mime_tags[i].str, attachments[j].mime,
                             strlen(ff_mkv_image_mime_tags[i].str))) {
                    st->codecpar->codec_id = ff_mkv_image_mime_tags[i].id;
                    break;
                }
            }

            attachments[j].stream = st;

            if (st->codecpar->codec_id != AV_CODEC_ID_NONE) {
                st->disposition         |= AV_DISPOSITION_ATTACHED_PIC;
                st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;

                av_init_packet(&st->attached_pic);
                if ((res = av_new_packet(&st->attached_pic, attachments[j].bin.size)) < 0)
                    return res;
                memcpy(st->attached_pic.data, attachments[j].bin.data, attachments[j].bin.size);
                st->attached_pic.stream_index = st->index;
                st->attached_pic.flags       |= AV_PKT_FLAG_KEY;
            } else {
                st->codecpar->codec_type = AVMEDIA_TYPE_ATTACHMENT;
                st->codecpar->extradata  = av_malloc(attachments[j].bin.size);
                if (!st->codecpar->extradata)
                    break;

                st->codecpar->extradata_size = attachments[j].bin.size;
                memcpy(st->codecpar->extradata, attachments[j].bin.data,
                       attachments[j].bin.size);

                for (i = 0; ff_mkv_mime_tags[i].id != AV_CODEC_ID_NONE; i++) {
                    if (!strncmp(ff_mkv_mime_tags[i].str, attachments[j].mime,
                                strlen(ff_mkv_mime_tags[i].str))) {
                        st->codecpar->codec_id = ff_mkv_mime_tags[i].id;
                        break;
                    }
                }
            }
        }
    }

    chapters = chapters_list->elem;
    for (i = 0; i < chapters_list->nb_elem; i++)
        if (chapters[i].start != AV_NOPTS_VALUE && chapters[i].uid &&
            (max_start == 0 || chapters[i].start > max_start)) {
            chapters[i].chapter =
                avpriv_new_chapter(s, chapters[i].uid,
                                   (AVRational) { 1, 1000000000 },
                                   chapters[i].start, chapters[i].end,
                                   chapters[i].title);
            av_dict_set(&chapters[i].chapter->metadata,
                        "title", chapters[i].title, 0);
            max_start = chapters[i].start;
        }

    matroska_convert_tags(s);

    return 0;
}

/*
 * Put one packet in an application-supplied AVPacket struct.
 * Returns 0 on success or -1 on failure.
 */
static int matroska_deliver_packet(MatroskaDemuxContext *matroska,
                                   AVPacket *pkt)
{
    if (matroska->num_packets > 0) {
        memcpy(pkt, matroska->packets[0], sizeof(AVPacket));
        av_free(matroska->packets[0]);
        if (matroska->num_packets > 1) {
            void *newpackets;
            memmove(&matroska->packets[0], &matroska->packets[1],
                    (matroska->num_packets - 1) * sizeof(AVPacket *));
            newpackets = av_realloc(matroska->packets,
                                    (matroska->num_packets - 1) *
                                    sizeof(AVPacket *));
            if (newpackets)
                matroska->packets = newpackets;
        } else {
            av_freep(&matroska->packets);
            matroska->prev_pkt = NULL;
        }
        matroska->num_packets--;
        return 0;
    }

    return -1;
}

/*
 * Free all packets in our internal queue.
 */
static void matroska_clear_queue(MatroskaDemuxContext *matroska)
{
    matroska->prev_pkt = NULL;
    if (matroska->packets) {
        int n;
        for (n = 0; n < matroska->num_packets; n++) {
            av_packet_unref(matroska->packets[n]);
            av_free(matroska->packets[n]);
        }
        av_freep(&matroska->packets);
        matroska->num_packets = 0;
    }
}

static int matroska_parse_laces(MatroskaDemuxContext *matroska, uint8_t **buf,
                                int *buf_size, int type,
                                uint32_t **lace_buf, int *laces)
{
    int res = 0, n, size = *buf_size;
    uint8_t *data = *buf;
    uint32_t *lace_size;

    if (!type) {
        *laces    = 1;
        *lace_buf = av_mallocz(sizeof(int));
        if (!*lace_buf)
            return AVERROR(ENOMEM);

        *lace_buf[0] = size;
        return 0;
    }

    assert(size > 0);
    *laces    = *data + 1;
    data     += 1;
    size     -= 1;
    lace_size = av_mallocz(*laces * sizeof(int));
    if (!lace_size)
        return AVERROR(ENOMEM);

    switch (type) {
    case 0x1: /* Xiph lacing */
    {
        uint8_t temp;
        uint32_t total = 0;
        for (n = 0; res == 0 && n < *laces - 1; n++) {
            while (1) {
                if (size == 0) {
                    res = AVERROR_EOF;
                    break;
                }
                temp          = *data;
                lace_size[n] += temp;
                data         += 1;
                size         -= 1;
                if (temp != 0xff)
                    break;
            }
            total += lace_size[n];
        }
        if (size <= total) {
            res = AVERROR_INVALIDDATA;
            break;
        }

        lace_size[n] = size - total;
        break;
    }

    case 0x2: /* fixed-size lacing */
        if (size % (*laces)) {
            res = AVERROR_INVALIDDATA;
            break;
        }
        for (n = 0; n < *laces; n++)
            lace_size[n] = size / *laces;
        break;

    case 0x3: /* EBML lacing */
    {
        uint64_t num;
        uint64_t total;
        n = matroska_ebmlnum_uint(matroska, data, size, &num);
        if (n < 0) {
            av_log(matroska->ctx, AV_LOG_INFO,
                   "EBML block data error\n");
            res = n;
            break;
        }
        data += n;
        size -= n;
        total = lace_size[0] = num;
        for (n = 1; res == 0 && n < *laces - 1; n++) {
            int64_t snum;
            int r;
            r = matroska_ebmlnum_sint(matroska, data, size, &snum);
            if (r < 0) {
                av_log(matroska->ctx, AV_LOG_INFO,
                       "EBML block data error\n");
                res = r;
                break;
            }
            data        += r;
            size        -= r;
            lace_size[n] = lace_size[n - 1] + snum;
            total       += lace_size[n];
        }
        if (size <= total) {
            res = AVERROR_INVALIDDATA;
            break;
        }
        lace_size[*laces - 1] = size - total;
        break;
    }
    }

    *buf      = data;
    *lace_buf = lace_size;
    *buf_size = size;

    return res;
}

static int matroska_parse_rm_audio(MatroskaDemuxContext *matroska,
                                   MatroskaTrack *track, AVStream *st,
                                   uint8_t *data, int size, uint64_t timecode,
                                   uint64_t duration, int64_t pos)
{
    int a = st->codecpar->block_align;
    int sps = track->audio.sub_packet_size;
    int cfs = track->audio.coded_framesize;
    int h   = track->audio.sub_packet_h;
    int y   = track->audio.sub_packet_cnt;
    int w   = track->audio.frame_size;
    int x;

    if (!track->audio.pkt_cnt) {
        if (track->audio.sub_packet_cnt == 0)
            track->audio.buf_timecode = timecode;
        if (st->codecpar->codec_id == AV_CODEC_ID_RA_288) {
            if (size < cfs * h / 2) {
                av_log(matroska->ctx, AV_LOG_ERROR,
                       "Corrupt int4 RM-style audio packet size\n");
                return AVERROR_INVALIDDATA;
            }
            for (x = 0; x < h / 2; x++)
                memcpy(track->audio.buf + x * 2 * w + y * cfs,
                       data + x * cfs, cfs);
        } else if (st->codecpar->codec_id == AV_CODEC_ID_SIPR) {
            if (size < w) {
                av_log(matroska->ctx, AV_LOG_ERROR,
                       "Corrupt sipr RM-style audio packet size\n");
                return AVERROR_INVALIDDATA;
            }
            memcpy(track->audio.buf + y * w, data, w);
        } else {
            if (size < sps * w / sps) {
                av_log(matroska->ctx, AV_LOG_ERROR,
                       "Corrupt generic RM-style audio packet size\n");
                return AVERROR_INVALIDDATA;
            }
            for (x = 0; x < w / sps; x++)
                memcpy(track->audio.buf +
                       sps * (h * x + ((h + 1) / 2) * (y & 1) + (y >> 1)),
                       data + x * sps, sps);
        }

        if (++track->audio.sub_packet_cnt >= h) {
            if (st->codecpar->codec_id == AV_CODEC_ID_SIPR)
                ff_rm_reorder_sipr_data(track->audio.buf, h, w);
            track->audio.sub_packet_cnt = 0;
            track->audio.pkt_cnt        = h * w / a;
        }
    }

    while (track->audio.pkt_cnt) {
        int ret;
        AVPacket *pkt = av_mallocz(sizeof(AVPacket));
        if (!pkt)
            return AVERROR(ENOMEM);

        ret = av_new_packet(pkt, a);
        if (ret < 0) {
            av_free(pkt);
            return ret;
        }
        memcpy(pkt->data,
               track->audio.buf + a * (h * w / a - track->audio.pkt_cnt--),
               a);
        pkt->pts                  = track->audio.buf_timecode;
        track->audio.buf_timecode = AV_NOPTS_VALUE;
        pkt->pos                  = pos;
        pkt->stream_index         = st->index;
        dynarray_add(&matroska->packets, &matroska->num_packets, pkt);
    }

    return 0;
}

/* reconstruct full wavpack blocks from mangled matroska ones */
static int matroska_parse_wavpack(MatroskaTrack *track, uint8_t *src,
                                  uint8_t **pdst, int *size)
{
    uint8_t *dst = NULL;
    int dstlen   = 0;
    int srclen   = *size;
    uint32_t samples;
    uint16_t ver;
    int ret, offset = 0;

    if (srclen < 12 || track->stream->codecpar->extradata_size < 2)
        return AVERROR_INVALIDDATA;

    ver = AV_RL16(track->stream->codecpar->extradata);

    samples = AV_RL32(src);
    src    += 4;
    srclen -= 4;

    while (srclen >= 8) {
        int multiblock;
        uint32_t blocksize;
        uint8_t *tmp;

        uint32_t flags = AV_RL32(src);
        uint32_t crc   = AV_RL32(src + 4);
        src    += 8;
        srclen -= 8;

        multiblock = (flags & 0x1800) != 0x1800;
        if (multiblock) {
            if (srclen < 4) {
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            blocksize = AV_RL32(src);
            src      += 4;
            srclen   -= 4;
        } else
            blocksize = srclen;

        if (blocksize > srclen) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        tmp = av_realloc(dst, dstlen + blocksize + 32);
        if (!tmp) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        dst     = tmp;
        dstlen += blocksize + 32;

        AV_WL32(dst + offset, MKTAG('w', 'v', 'p', 'k'));   // tag
        AV_WL32(dst + offset +  4, blocksize + 24);         // blocksize - 8
        AV_WL16(dst + offset +  8, ver);                    // version
        AV_WL16(dst + offset + 10, 0);                      // track/index_no
        AV_WL32(dst + offset + 12, 0);                      // total samples
        AV_WL32(dst + offset + 16, 0);                      // block index
        AV_WL32(dst + offset + 20, samples);                // number of samples
        AV_WL32(dst + offset + 24, flags);                  // flags
        AV_WL32(dst + offset + 28, crc);                    // crc
        memcpy(dst + offset + 32, src, blocksize);          // block data

        src    += blocksize;
        srclen -= blocksize;
        offset += blocksize + 32;
    }

    *pdst = dst;
    *size = dstlen;

    return 0;

fail:
    av_freep(&dst);
    return ret;
}

static int matroska_parse_frame(MatroskaDemuxContext *matroska,
                                MatroskaTrack *track, AVStream *st,
                                uint8_t *data, int pkt_size,
                                uint64_t timecode, uint64_t duration,
                                int64_t pos, int is_keyframe)
{
    MatroskaTrackEncoding *encodings = track->encodings.elem;
    uint8_t *pkt_data = data;
    int offset = 0, res;
    AVPacket *pkt;

    if (encodings && encodings->scope & 1) {
        res = matroska_decode_buffer(&pkt_data, &pkt_size, track);
        if (res < 0)
            return res;
    }

    if (st->codecpar->codec_id == AV_CODEC_ID_WAVPACK) {
        uint8_t *wv_data;
        res = matroska_parse_wavpack(track, pkt_data, &wv_data, &pkt_size);
        if (res < 0) {
            av_log(matroska->ctx, AV_LOG_ERROR,
                   "Error parsing a wavpack block.\n");
            goto fail;
        }
        if (pkt_data != data)
            av_freep(&pkt_data);
        pkt_data = wv_data;
    }

    if (st->codecpar->codec_id == AV_CODEC_ID_PRORES)
        offset = 8;

    pkt = av_mallocz(sizeof(AVPacket));
    if (!pkt) {
        av_freep(&pkt_data);
        return AVERROR(ENOMEM);
    }
    /* XXX: prevent data copy... */
    if (av_new_packet(pkt, pkt_size + offset) < 0) {
        av_free(pkt);
        av_freep(&pkt_data);
        return AVERROR(ENOMEM);
    }

    if (st->codecpar->codec_id == AV_CODEC_ID_PRORES) {
        uint8_t *buf = pkt->data;
        bytestream_put_be32(&buf, pkt_size);
        bytestream_put_be32(&buf, MKBETAG('i', 'c', 'p', 'f'));
    }

    memcpy(pkt->data + offset, pkt_data, pkt_size);

    if (pkt_data != data)
        av_free(pkt_data);

    pkt->flags        = is_keyframe;
    pkt->stream_index = st->index;

    if (track->ms_compat)
        pkt->dts = timecode;
    else
        pkt->pts = timecode;
    pkt->pos = pos;
    if (track->type != MATROSKA_TRACK_TYPE_SUBTITLE || st->codecpar->codec_id == AV_CODEC_ID_SRT)
        pkt->duration = duration;
#if FF_API_CONVERGENCE_DURATION
FF_DISABLE_DEPRECATION_WARNINGS
    if (st->codecpar->codec_id == AV_CODEC_ID_SRT)
        pkt->convergence_duration = duration;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    if (st->codecpar->codec_id == AV_CODEC_ID_SSA)
        matroska_fix_ass_packet(matroska, pkt, duration);

    if (matroska->prev_pkt                                 &&
        timecode                         != AV_NOPTS_VALUE &&
        matroska->prev_pkt->pts          == timecode       &&
        matroska->prev_pkt->stream_index == st->index      &&
        st->codecpar->codec_id == AV_CODEC_ID_SSA)
        matroska_merge_packets(matroska->prev_pkt, pkt);
    else {
        dynarray_add(&matroska->packets, &matroska->num_packets, pkt);
        matroska->prev_pkt = pkt;
    }

    return 0;

fail:
    if (pkt_data != data)
        av_freep(&pkt_data);
    return res;
}

static int matroska_parse_block(MatroskaDemuxContext *matroska, uint8_t *data,
                                int size, int64_t pos, uint64_t cluster_time,
                                uint64_t block_duration, int is_keyframe,
                                int64_t cluster_pos)
{
    uint64_t timecode = AV_NOPTS_VALUE;
    MatroskaTrack *track;
    int res = 0;
    AVStream *st;
    int16_t block_time;
    uint32_t *lace_size = NULL;
    int n, flags, laces = 0;
    uint64_t num, duration;

    if ((n = matroska_ebmlnum_uint(matroska, data, size, &num)) < 0) {
        av_log(matroska->ctx, AV_LOG_ERROR, "EBML block data error\n");
        return n;
    }
    data += n;
    size -= n;

    track = matroska_find_track_by_num(matroska, num);
    if (!track || !track->stream) {
        av_log(matroska->ctx, AV_LOG_INFO,
               "Invalid stream %"PRIu64" or size %u\n", num, size);
        return AVERROR_INVALIDDATA;
    } else if (size <= 3)
        return 0;
    st = track->stream;
    if (st->discard >= AVDISCARD_ALL)
        return res;

    block_time = AV_RB16(data);
    data      += 2;
    flags      = *data++;
    size      -= 3;
    if (is_keyframe == -1)
        is_keyframe = flags & 0x80 ? AV_PKT_FLAG_KEY : 0;

    if (cluster_time != (uint64_t) -1 &&
        (block_time >= 0 || cluster_time >= -block_time)) {
        timecode = cluster_time + block_time - track->codec_delay;
        if (track->type == MATROSKA_TRACK_TYPE_SUBTITLE &&
            timecode < track->end_timecode)
            is_keyframe = 0;  /* overlapping subtitles are not key frame */
        if (is_keyframe)
            av_add_index_entry(st, cluster_pos, timecode, 0, 0,
                               AVINDEX_KEYFRAME);
    }

    if (matroska->skip_to_keyframe &&
        track->type != MATROSKA_TRACK_TYPE_SUBTITLE) {
        if (!is_keyframe || timecode < matroska->skip_to_timecode)
            return res;
        matroska->skip_to_keyframe = 0;
    }

    res = matroska_parse_laces(matroska, &data, &size, (flags & 0x06) >> 1,
                               &lace_size, &laces);

    if (res)
        goto end;

    if (block_duration != AV_NOPTS_VALUE) {
        duration = block_duration / laces;
        if (block_duration != duration * laces) {
            av_log(matroska->ctx, AV_LOG_WARNING,
                   "Incorrect block_duration, possibly corrupted container");
        }
    } else {
        duration       = track->default_duration / matroska->time_scale;
        block_duration = duration * laces;
    }

    if (timecode != AV_NOPTS_VALUE)
        track->end_timecode =
            FFMAX(track->end_timecode, timecode + block_duration);

    for (n = 0; n < laces; n++) {
        if ((st->codecpar->codec_id == AV_CODEC_ID_RA_288 ||
             st->codecpar->codec_id == AV_CODEC_ID_COOK   ||
             st->codecpar->codec_id == AV_CODEC_ID_SIPR   ||
             st->codecpar->codec_id == AV_CODEC_ID_ATRAC3) &&
            st->codecpar->block_align && track->audio.sub_packet_size) {
            res = matroska_parse_rm_audio(matroska, track, st, data,
                                          lace_size[n],
                                          timecode, duration, pos);
            if (res)
                goto end;
        } else {
            res = matroska_parse_frame(matroska, track, st, data, lace_size[n],
                                       timecode, duration, pos,
                                       !n ? is_keyframe : 0);
            if (res)
                goto end;
        }

        if (timecode != AV_NOPTS_VALUE)
            timecode = duration ? timecode + duration : AV_NOPTS_VALUE;
        data += lace_size[n];
    }

end:
    av_free(lace_size);
    return res;
}

static int matroska_parse_cluster_incremental(MatroskaDemuxContext *matroska)
{
    EbmlList *blocks_list;
    MatroskaBlock *blocks;
    int i, res;
    res = ebml_parse(matroska,
                     matroska_cluster_incremental_parsing,
                     &matroska->current_cluster);
    if (res == 1) {
        /* New Cluster */
        if (matroska->current_cluster_pos)
            ebml_level_end(matroska);
        ebml_free(matroska_cluster, &matroska->current_cluster);
        memset(&matroska->current_cluster, 0, sizeof(MatroskaCluster));
        matroska->current_cluster_num_blocks = 0;
        matroska->current_cluster_pos        = avio_tell(matroska->ctx->pb);
        matroska->prev_pkt                   = NULL;
        /* sizeof the ID which was already read */
        if (matroska->current_id)
            matroska->current_cluster_pos -= 4;
        res = ebml_parse(matroska,
                         matroska_clusters_incremental,
                         &matroska->current_cluster);
        /* Try parsing the block again. */
        if (res == 1)
            res = ebml_parse(matroska,
                             matroska_cluster_incremental_parsing,
                             &matroska->current_cluster);
    }

    if (!res &&
        matroska->current_cluster_num_blocks <
        matroska->current_cluster.blocks.nb_elem) {
        blocks_list = &matroska->current_cluster.blocks;
        blocks      = blocks_list->elem;

        matroska->current_cluster_num_blocks = blocks_list->nb_elem;
        i                                    = blocks_list->nb_elem - 1;
        if (blocks[i].bin.size > 0 && blocks[i].bin.data) {
            int is_keyframe = blocks[i].non_simple ? !blocks[i].reference : -1;
            if (!blocks[i].non_simple)
                blocks[i].duration = AV_NOPTS_VALUE;
            res = matroska_parse_block(matroska, blocks[i].bin.data,
                                       blocks[i].bin.size, blocks[i].bin.pos,
                                       matroska->current_cluster.timecode,
                                       blocks[i].duration, is_keyframe,
                                       matroska->current_cluster_pos);
        }
    }

    if (res < 0)
        matroska->done = 1;
    return res;
}

static int matroska_parse_cluster(MatroskaDemuxContext *matroska)
{
    MatroskaCluster cluster = { 0 };
    EbmlList *blocks_list;
    MatroskaBlock *blocks;
    int i, res;
    int64_t pos;

    if (!matroska->contains_ssa)
        return matroska_parse_cluster_incremental(matroska);
    pos = avio_tell(matroska->ctx->pb);
    matroska->prev_pkt = NULL;
    if (matroska->current_id)
        pos -= 4;  /* sizeof the ID which was already read */
    res         = ebml_parse(matroska, matroska_clusters, &cluster);
    blocks_list = &cluster.blocks;
    blocks      = blocks_list->elem;
    for (i = 0; i < blocks_list->nb_elem && !res; i++)
        if (blocks[i].bin.size > 0 && blocks[i].bin.data) {
            int is_keyframe = blocks[i].non_simple ? !blocks[i].reference : -1;
            if (!blocks[i].non_simple)
                blocks[i].duration = AV_NOPTS_VALUE;
            res = matroska_parse_block(matroska, blocks[i].bin.data,
                                       blocks[i].bin.size, blocks[i].bin.pos,
                                       cluster.timecode, blocks[i].duration,
                                       is_keyframe, pos);
        }
    ebml_free(matroska_cluster, &cluster);
    return res;
}

static int matroska_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MatroskaDemuxContext *matroska = s->priv_data;
    int ret = 0;

    while (!ret && matroska_deliver_packet(matroska, pkt)) {
        int64_t pos = avio_tell(matroska->ctx->pb);
        if (matroska->done)
            return AVERROR_EOF;
        if (matroska_parse_cluster(matroska) < 0)
            ret = matroska_resync(matroska, pos);
    }

    if (ret == AVERROR_INVALIDDATA && pkt->data) {
        pkt->flags |= AV_PKT_FLAG_CORRUPT;
        return 0;
    }

    return ret;
}

static int matroska_read_seek(AVFormatContext *s, int stream_index,
                              int64_t timestamp, int flags)
{
    MatroskaDemuxContext *matroska = s->priv_data;
    MatroskaTrack *tracks = NULL;
    AVStream *st = s->streams[stream_index];
    int i, index, index_sub, index_min;

    /* Parse the CUES now since we need the index data to seek. */
    if (matroska->cues_parsing_deferred) {
        matroska_parse_cues(matroska);
        matroska->cues_parsing_deferred = 0;
    }

    if (!st->nb_index_entries)
        return 0;
    timestamp = FFMAX(timestamp, st->index_entries[0].timestamp);

    if ((index = av_index_search_timestamp(st, timestamp, flags)) < 0) {
        avio_seek(s->pb, st->index_entries[st->nb_index_entries - 1].pos,
                  SEEK_SET);
        matroska->current_id = 0;
        while ((index = av_index_search_timestamp(st, timestamp, flags)) < 0) {
            matroska_clear_queue(matroska);
            if (matroska_parse_cluster(matroska) < 0)
                break;
        }
    }

    matroska_clear_queue(matroska);
    if (index < 0)
        return 0;

    index_min = index;
    tracks = matroska->tracks.elem;
    for (i = 0; i < matroska->tracks.nb_elem; i++) {
        tracks[i].audio.pkt_cnt        = 0;
        tracks[i].audio.sub_packet_cnt = 0;
        tracks[i].audio.buf_timecode   = AV_NOPTS_VALUE;
        tracks[i].end_timecode         = 0;
        if (tracks[i].type == MATROSKA_TRACK_TYPE_SUBTITLE &&
            tracks[i].stream->discard != AVDISCARD_ALL) {
            index_sub = av_index_search_timestamp(
                tracks[i].stream, st->index_entries[index].timestamp,
                AVSEEK_FLAG_BACKWARD);
            if (index_sub >= 0 &&
                st->index_entries[index_sub].pos < st->index_entries[index_min].pos &&
                st->index_entries[index].timestamp -
                st->index_entries[index_sub].timestamp < 30000000000 / matroska->time_scale)
                index_min = index_sub;
        }
    }

    avio_seek(s->pb, st->index_entries[index_min].pos, SEEK_SET);
    matroska->current_id       = 0;
    matroska->skip_to_keyframe = !(flags & AVSEEK_FLAG_ANY);
    matroska->skip_to_timecode = st->index_entries[index].timestamp;
    matroska->done             = 0;
    ff_update_cur_dts(s, st, st->index_entries[index].timestamp);
    return 0;
}

static int matroska_read_close(AVFormatContext *s)
{
    MatroskaDemuxContext *matroska = s->priv_data;
    MatroskaTrack *tracks = matroska->tracks.elem;
    int n;

    matroska_clear_queue(matroska);

    for (n = 0; n < matroska->tracks.nb_elem; n++)
        if (tracks[n].type == MATROSKA_TRACK_TYPE_AUDIO)
            av_free(tracks[n].audio.buf);
    ebml_free(matroska_cluster, &matroska->current_cluster);
    ebml_free(matroska_segment, matroska);

    return 0;
}

AVInputFormat ff_matroska_demuxer = {
    .name           = "matroska,webm",
    .long_name      = NULL_IF_CONFIG_SMALL("Matroska / WebM"),
    .extensions     = "mkv,mk3d,mka,mks",
    .priv_data_size = sizeof(MatroskaDemuxContext),
    .read_probe     = matroska_probe,
    .read_header    = matroska_read_header,
    .read_packet    = matroska_read_packet,
    .read_close     = matroska_read_close,
    .read_seek      = matroska_read_seek,
    .mime_type      = "audio/webm,audio/x-matroska,video/webm,video/x-matroska"
};

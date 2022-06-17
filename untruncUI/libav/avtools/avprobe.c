/*
 * avprobe : Simple Media Prober based on the Libav libraries
 * Copyright (c) 2007-2010 Stefano Sabatini
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

#include "config.h"

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avstring.h"
#include "libavutil/display.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/spherical.h"
#include "libavutil/stereo3d.h"
#include "libavutil/dict.h"
#include "libavutil/libm.h"
#include "libavdevice/avdevice.h"
#include "cmdutils.h"

typedef struct InputStream {
    AVStream *st;

    AVCodecContext *dec_ctx;
} InputStream;

typedef struct InputFile {
    AVFormatContext *fmt_ctx;

    InputStream *streams;
    int       nb_streams;
} InputFile;

const char program_name[] = "avprobe";
const int program_birth_year = 2007;

static int do_show_format  = 0;
static AVDictionary *fmt_entries_to_show = NULL;
static int nb_fmt_entries_to_show;
static int do_show_packets = 0;
static int do_show_streams = 0;
static AVDictionary *stream_entries_to_show = NULL;
static int nb_stream_entries_to_show;

/* key used to print when probe_{int,str}(NULL, ..) is invoked */
static const char *header_key;

static int show_value_unit              = 0;
static int use_value_prefix             = 0;
static int use_byte_value_binary_prefix = 0;
static int use_value_sexagesimal_format = 0;

/* globals */
static const OptionDef *options;

/* avprobe context */
static const char *input_filename;
static AVInputFormat *iformat = NULL;

static const char *const binary_unit_prefixes [] = { "", "Ki", "Mi", "Gi", "Ti", "Pi" };
static const char *const decimal_unit_prefixes[] = { "", "K" , "M" , "G" , "T" , "P"  };

static const char unit_second_str[]         = "s"    ;
static const char unit_hertz_str[]          = "Hz"   ;
static const char unit_byte_str[]           = "byte" ;
static const char unit_bit_per_second_str[] = "bit/s";

static void avprobe_cleanup(int ret)
{
    av_dict_free(&fmt_entries_to_show);
    av_dict_free(&stream_entries_to_show);
}

/*
 * The output is structured in array and objects that might contain items
 * Array could require the objects within to not be named.
 * Object could require the items within to be named.
 *
 * For flat representation the name of each section is saved on prefix so it
 * can be rendered in order to represent nested structures (e.g. array of
 * objects for the packets list).
 *
 * Within an array each element can need an unique identifier or an index.
 *
 * Nesting level is accounted separately.
 */

typedef enum {
    ARRAY,
    OBJECT
} PrintElementType;

typedef struct PrintElement {
    const char *name;
    PrintElementType type;
    int64_t index;
    int64_t nb_elems;
} PrintElement;

typedef struct PrintContext {
    PrintElement *prefix;
    int level;
    void (*print_header)(void);
    void (*print_footer)(void);

    void (*print_array_header) (const char *name, int plain_values);
    void (*print_array_footer) (const char *name, int plain_values);
    void (*print_object_header)(const char *name);
    void (*print_object_footer)(const char *name);

    void (*print_integer) (const char *key, int64_t value);
    void (*print_string)  (const char *key, const char *value);
} PrintContext;

static AVIOContext *probe_out = NULL;
static PrintContext octx;
#define AVP_INDENT() avio_printf(probe_out, "%*c", octx.level * 2, ' ')
#define CONV_FP(x,fp) ((double) (x)) / (1 << fp)

/*
 * Default format, INI
 *
 * - all key and values are utf8
 * - '.' is the subgroup separator
 * - newlines and the following characters are escaped
 * - '\' is the escape character
 * - '#' is the comment
 * - '=' is the key/value separators
 * - ':' is not used but usually parsed as key/value separator
 */

static void ini_print_header(void)
{
    avio_printf(probe_out, "# avprobe output\n\n");
}
static void ini_print_footer(void)
{
    avio_w8(probe_out, '\n');
}

static void ini_escape_print(const char *s)
{
    int i = 0;
    char c = 0;

    while (c = s[i++]) {
        switch (c) {
        case '\r': avio_printf(probe_out, "%s", "\\r"); break;
        case '\n': avio_printf(probe_out, "%s", "\\n"); break;
        case '\f': avio_printf(probe_out, "%s", "\\f"); break;
        case '\b': avio_printf(probe_out, "%s", "\\b"); break;
        case '\t': avio_printf(probe_out, "%s", "\\t"); break;
        case '\\':
        case '#' :
        case '=' :
        case ':' : avio_w8(probe_out, '\\');
        default:
            if ((unsigned char)c < 32)
                avio_printf(probe_out, "\\x00%02x", c & 0xff);
            else
                avio_w8(probe_out, c);
        break;
        }
    }
}

static void ini_print_array_header(const char *name, int plain_values)
{
    if (!plain_values) {
        /* Add a new line if we create a new full group */
        if (octx.prefix[octx.level -1].nb_elems)
            avio_printf(probe_out, "\n");
    } else {
        ini_escape_print(name);
        avio_w8(probe_out, '=');
    }
}

static void ini_print_array_footer(const char *name, int plain_values)
{
    if (plain_values)
        avio_printf(probe_out, "\n");
}

static void ini_print_object_header(const char *name)
{
    int i;
    PrintElement *el = octx.prefix + octx.level -1;

    if (el->nb_elems)
        avio_printf(probe_out, "\n");

    avio_printf(probe_out, "[");

    for (i = 1; i < octx.level; i++) {
        el = octx.prefix + i;
        avio_printf(probe_out, "%s.", el->name);
        if (el->index >= 0)
            avio_printf(probe_out, "%"PRId64".", el->index);
    }

    avio_printf(probe_out, "%s", name);
    if (el->type == ARRAY)
        avio_printf(probe_out, ".%"PRId64"", el->nb_elems);
    avio_printf(probe_out, "]\n");
}

static void ini_print_integer(const char *key, int64_t value)
{
    if (key) {
        ini_escape_print(key);
        avio_printf(probe_out, "=%"PRId64"\n", value);
    } else {
        if (octx.prefix[octx.level -1].nb_elems)
            avio_printf(probe_out, ",");
        avio_printf(probe_out, "%"PRId64, value);
    }
}


static void ini_print_string(const char *key, const char *value)
{
    if (key) {
        ini_escape_print(key);
        avio_printf(probe_out, "=%s\n", value);
    } else {
        if (octx.prefix[octx.level -1].nb_elems)
            avio_printf(probe_out, ",");
        avio_printf(probe_out, "%s", value);
    }
}

/*
 * Alternate format, JSON
 */

static void json_print_header(void)
{
    avio_printf(probe_out, "{");
}
static void json_print_footer(void)
{
    avio_printf(probe_out, "}\n");
}

static void json_print_array_header(const char *name, int plain_values)
{
    if (octx.prefix[octx.level -1].nb_elems)
        avio_printf(probe_out, ",\n");
    AVP_INDENT();
    avio_printf(probe_out, "\"%s\" : ", name);
    avio_printf(probe_out, "[\n");
}

static void json_print_array_footer(const char *name, int plain_values)
{
    avio_printf(probe_out, "\n");
    AVP_INDENT();
    avio_printf(probe_out, "]");
}

static void json_print_object_header(const char *name)
{
    if (octx.prefix[octx.level -1].nb_elems)
        avio_printf(probe_out, ",\n");
    AVP_INDENT();
    if (octx.prefix[octx.level -1].type == OBJECT)
        avio_printf(probe_out, "\"%s\" : ", name);
    avio_printf(probe_out, "{\n");
}

static void json_print_object_footer(const char *name)
{
    avio_printf(probe_out, "\n");
    AVP_INDENT();
    avio_printf(probe_out, "}");
}

static void json_print_integer(const char *key, int64_t value)
{
    if (key) {
        if (octx.prefix[octx.level -1].nb_elems)
            avio_printf(probe_out, ",\n");
        AVP_INDENT();
        avio_printf(probe_out, "\"%s\" : ", key);
    } else {
        if (octx.prefix[octx.level -1].nb_elems)
            avio_printf(probe_out, ", ");
        else
            AVP_INDENT();
    }
    avio_printf(probe_out, "%"PRId64, value);
}

static void json_escape_print(const char *s)
{
    int i = 0;
    char c = 0;

    while (c = s[i++]) {
        switch (c) {
        case '\r': avio_printf(probe_out, "%s", "\\r"); break;
        case '\n': avio_printf(probe_out, "%s", "\\n"); break;
        case '\f': avio_printf(probe_out, "%s", "\\f"); break;
        case '\b': avio_printf(probe_out, "%s", "\\b"); break;
        case '\t': avio_printf(probe_out, "%s", "\\t"); break;
        case '\\':
        case '"' : avio_w8(probe_out, '\\');
        default:
            if ((unsigned char)c < 32)
                avio_printf(probe_out, "\\u00%02x", c & 0xff);
            else
                avio_w8(probe_out, c);
        break;
        }
    }
}

static void json_print_string(const char *key, const char *value)
{
    if (key) {
        if (octx.prefix[octx.level -1].nb_elems)
            avio_printf(probe_out, ",\n");
        AVP_INDENT();
        avio_w8(probe_out, '\"');
        json_escape_print(key);
        avio_printf(probe_out, "\" : \"");
        json_escape_print(value);
        avio_w8(probe_out, '\"');
    } else {
        if (octx.prefix[octx.level -1].nb_elems)
            avio_printf(probe_out, ", ");
        else
            AVP_INDENT();
        avio_w8(probe_out, '\"');
        json_escape_print(value);
        avio_w8(probe_out, '\"');
    }
}

/*
 * old-style pseudo-INI
 */
static void old_print_object_header(const char *name)
{
    char *str, *p;

    if (!strcmp(name, "tags"))
        return;

    str = p = av_strdup(name);
    if (!str)
        return;
    while (*p) {
        *p = av_toupper(*p);
        p++;
    }

    avio_printf(probe_out, "[%s]\n", str);
    av_freep(&str);
}

static void old_print_object_footer(const char *name)
{
    char *str, *p;

    if (!strcmp(name, "tags"))
        return;

    str = p = av_strdup(name);
    if (!str)
        return;
    while (*p) {
        *p = av_toupper(*p);
        p++;
    }

    avio_printf(probe_out, "[/%s]\n", str);
    av_freep(&str);
}

static void old_print_string(const char *key, const char *value)
{
    if (!strcmp(octx.prefix[octx.level - 1].name, "tags"))
        avio_printf(probe_out, "TAG:");
    ini_print_string(key, value);
}

/*
 * Simple Formatter for single entries.
 */

static void show_format_entry_integer(const char *key, int64_t value)
{
    if (key && av_dict_get(fmt_entries_to_show, key, NULL, 0)) {
        if (nb_fmt_entries_to_show > 1)
            avio_printf(probe_out, "%s=", key);
        avio_printf(probe_out, "%"PRId64"\n", value);
    }
}

static void show_format_entry_string(const char *key, const char *value)
{
    if (key && av_dict_get(fmt_entries_to_show, key, NULL, 0)) {
        if (nb_fmt_entries_to_show > 1)
            avio_printf(probe_out, "%s=", key);
        avio_printf(probe_out, "%s\n", value);
    }
}

static void show_stream_entry_header(const char *key, int value)
{
    header_key = key;
}

static void show_stream_entry_footer(const char *key, int value)
{
    header_key = NULL;
}

static void show_stream_entry_integer(const char *key, int64_t value)
{
    if (!key)
        key = header_key;

    if (key && av_dict_get(stream_entries_to_show, key, NULL, 0)) {
        if (nb_stream_entries_to_show > 1)
            avio_printf(probe_out, "%s=", key);
        avio_printf(probe_out, "%"PRId64"\n", value);
    }
}

static void show_stream_entry_string(const char *key, const char *value)
{
    if (key && av_dict_get(stream_entries_to_show, key, NULL, 0)) {
        if (nb_stream_entries_to_show > 1)
            avio_printf(probe_out, "%s=", key);
        avio_printf(probe_out, "%s\n", value);
    }
}

static void probe_group_enter(const char *name, int type)
{
    int64_t count = -1;

    octx.prefix =
        av_realloc(octx.prefix, sizeof(PrintElement) * (octx.level + 1));

    if (!octx.prefix || !name) {
        fprintf(stderr, "Out of memory\n");
        exit_program(1);
    }

    if (octx.level) {
        PrintElement *parent = octx.prefix + octx.level -1;
        if (parent->type == ARRAY)
            count = parent->nb_elems;
        parent->nb_elems++;
    }

    octx.prefix[octx.level++] = (PrintElement){name, type, count, 0};
}

static void probe_group_leave(void)
{
    --octx.level;
}

static void probe_header(void)
{
    if (octx.print_header)
        octx.print_header();
    probe_group_enter("root", OBJECT);
}

static void probe_footer(void)
{
    if (octx.print_footer)
        octx.print_footer();
    probe_group_leave();
}


static void probe_array_header(const char *name, int plain_values)
{
    if (octx.print_array_header)
        octx.print_array_header(name, plain_values);

    probe_group_enter(name, ARRAY);
}

static void probe_array_footer(const char *name, int plain_values)
{
    probe_group_leave();
    if (octx.print_array_footer)
        octx.print_array_footer(name, plain_values);
}

static void probe_object_header(const char *name)
{
    if (octx.print_object_header)
        octx.print_object_header(name);

    probe_group_enter(name, OBJECT);
}

static void probe_object_footer(const char *name)
{
    probe_group_leave();
    if (octx.print_object_footer)
        octx.print_object_footer(name);
}

static void probe_int(const char *key, int64_t value)
{
    octx.print_integer(key, value);
    octx.prefix[octx.level -1].nb_elems++;
}

static void probe_str(const char *key, const char *value)
{
    octx.print_string(key, value);
    octx.prefix[octx.level -1].nb_elems++;
}

static void probe_dict(AVDictionary *dict, const char *name)
{
    AVDictionaryEntry *entry = NULL;
    if (!dict)
        return;
    probe_object_header(name);
    while ((entry = av_dict_get(dict, "", entry, AV_DICT_IGNORE_SUFFIX))) {
        probe_str(entry->key, entry->value);
    }
    probe_object_footer(name);
}

static char *value_string(char *buf, int buf_size, double val, const char *unit)
{
    if (unit == unit_second_str && use_value_sexagesimal_format) {
        double secs;
        int hours, mins;
        secs  = val;
        mins  = (int)secs / 60;
        secs  = secs - mins * 60;
        hours = mins / 60;
        mins %= 60;
        snprintf(buf, buf_size, "%d:%02d:%09.6f", hours, mins, secs);
    } else if (use_value_prefix) {
        const char *prefix_string;
        int index;

        if (unit == unit_byte_str && use_byte_value_binary_prefix) {
            index = (int) log2(val) / 10;
            index = av_clip(index, 0, FF_ARRAY_ELEMS(binary_unit_prefixes) - 1);
            val  /= pow(2, index * 10);
            prefix_string = binary_unit_prefixes[index];
        } else {
            index = (int) (log10(val)) / 3;
            index = av_clip(index, 0, FF_ARRAY_ELEMS(decimal_unit_prefixes) - 1);
            val  /= pow(10, index * 3);
            prefix_string = decimal_unit_prefixes[index];
        }
        snprintf(buf, buf_size, "%.*f%s%s",
                 index ? 3 : 0, val,
                 prefix_string,
                 show_value_unit ? unit : "");
    } else {
        snprintf(buf, buf_size, "%f%s", val, show_value_unit ? unit : "");
    }

    return buf;
}

static char *time_value_string(char *buf, int buf_size, int64_t val,
                               const AVRational *time_base)
{
    if (val == AV_NOPTS_VALUE) {
        snprintf(buf, buf_size, "N/A");
    } else {
        value_string(buf, buf_size, val * av_q2d(*time_base), unit_second_str);
    }

    return buf;
}

static char *ts_value_string(char *buf, int buf_size, int64_t ts)
{
    if (ts == AV_NOPTS_VALUE) {
        snprintf(buf, buf_size, "N/A");
    } else {
        snprintf(buf, buf_size, "%"PRId64, ts);
    }

    return buf;
}

static char *rational_string(char *buf, int buf_size, const char *sep,
                             const AVRational *rat)
{
    snprintf(buf, buf_size, "%d%s%d", rat->num, sep, rat->den);
    return buf;
}

static char *tag_string(char *buf, int buf_size, int tag)
{
    snprintf(buf, buf_size, "0x%04x", tag);
    return buf;
}

static char *unknown_string(char *buf, int buf_size, int val)
{
    snprintf(buf, buf_size, "Unknown (%d)", val);
    return buf;
}

static void show_packet(AVFormatContext *fmt_ctx, AVPacket *pkt)
{
    char val_str[128];
    AVStream *st = fmt_ctx->streams[pkt->stream_index];

    probe_object_header("packet");
    probe_str("codec_type", media_type_string(st->codecpar->codec_type));
    probe_int("stream_index", pkt->stream_index);
    probe_str("pts", ts_value_string(val_str, sizeof(val_str), pkt->pts));
    probe_str("pts_time", time_value_string(val_str, sizeof(val_str),
                                               pkt->pts, &st->time_base));
    probe_str("dts", ts_value_string(val_str, sizeof(val_str), pkt->dts));
    probe_str("dts_time", time_value_string(val_str, sizeof(val_str),
                                               pkt->dts, &st->time_base));
    probe_str("duration", ts_value_string(val_str, sizeof(val_str),
                                             pkt->duration));
    probe_str("duration_time", time_value_string(val_str, sizeof(val_str),
                                                    pkt->duration,
                                                    &st->time_base));
    probe_str("size", value_string(val_str, sizeof(val_str),
                                      pkt->size, unit_byte_str));
    probe_int("pos", pkt->pos);
    probe_str("flags", pkt->flags & AV_PKT_FLAG_KEY ? "K" : "_");
    probe_object_footer("packet");
}

static void show_packets(InputFile *ifile)
{
    AVFormatContext *fmt_ctx = ifile->fmt_ctx;
    AVPacket pkt;

    av_init_packet(&pkt);
    probe_array_header("packets", 0);
    while (!av_read_frame(fmt_ctx, &pkt)) {
        show_packet(fmt_ctx, &pkt);
        av_packet_unref(&pkt);
    }
    probe_array_footer("packets", 0);
}

static void show_stream(InputFile *ifile, InputStream *ist)
{
    AVFormatContext *fmt_ctx = ifile->fmt_ctx;
    AVStream *stream = ist->st;
    AVCodecParameters *par;
    AVCodecContext *dec_ctx;
    const AVCodecDescriptor *codec_desc;
    const char *profile;
    char val_str[128];
    AVRational display_aspect_ratio, *sar = NULL;
    const AVPixFmtDescriptor *desc;
    const char *val;

    probe_object_header("stream");

    probe_int("index", stream->index);

    par     = stream->codecpar;
    dec_ctx = ist->dec_ctx;
    codec_desc = avcodec_descriptor_get(par->codec_id);
    if (codec_desc) {
        probe_str("codec_name", codec_desc->name);
        probe_str("codec_long_name", codec_desc->long_name);
    } else {
        probe_str("codec_name", "unknown");
    }

    probe_str("codec_type", media_type_string(par->codec_type));

    /* print AVI/FourCC tag */
    av_get_codec_tag_string(val_str, sizeof(val_str), par->codec_tag);
    probe_str("codec_tag_string", val_str);
    probe_str("codec_tag", tag_string(val_str, sizeof(val_str),
                                      par->codec_tag));

    /* print profile, if there is one */
    profile = avcodec_profile_name(par->codec_id, par->profile);
    if (profile)
        probe_str("profile", profile);

    switch (par->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        probe_int("width",  par->width);
        probe_int("height", par->height);
        if (dec_ctx) {
            probe_int("coded_width",  dec_ctx->coded_width);
            probe_int("coded_height", dec_ctx->coded_height);
            probe_int("has_b_frames", dec_ctx->has_b_frames);
        }
        if (dec_ctx && dec_ctx->sample_aspect_ratio.num)
            sar = &dec_ctx->sample_aspect_ratio;
        else if (par->sample_aspect_ratio.num)
            sar = &par->sample_aspect_ratio;
        else if (stream->sample_aspect_ratio.num)
            sar = &stream->sample_aspect_ratio;

        if (sar) {
            probe_str("sample_aspect_ratio",
                      rational_string(val_str, sizeof(val_str), ":", sar));
            av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
                      par->width  * sar->num, par->height * sar->den,
                      1024*1024);
            probe_str("display_aspect_ratio",
                      rational_string(val_str, sizeof(val_str), ":",
                      &display_aspect_ratio));
        }
        desc = av_pix_fmt_desc_get(par->format);
        probe_str("pix_fmt", desc ? desc->name : "unknown");
        probe_int("level", par->level);

        val = av_color_range_name(par->color_range);
        if (!val)
            val = unknown_string(val_str, sizeof(val_str), par->color_range);
        probe_str("color_range", val);

        val = av_color_space_name(par->color_space);
        if (!val)
            val = unknown_string(val_str, sizeof(val_str), par->color_space);
        probe_str("color_space", val);

        val = av_color_transfer_name(par->color_trc);
        if (!val)
            val = unknown_string(val_str, sizeof(val_str), par->color_trc);
        probe_str("color_trc", val);

        val = av_color_primaries_name(par->color_primaries);
        if (!val)
            val = unknown_string(val_str, sizeof(val_str), par->color_primaries);
        probe_str("color_pri", val);

        val = av_chroma_location_name(par->chroma_location);
        if (!val)
            val = unknown_string(val_str, sizeof(val_str), par->chroma_location);
        probe_str("chroma_loc", val);
        break;

    case AVMEDIA_TYPE_AUDIO:
        probe_str("sample_rate",
                  value_string(val_str, sizeof(val_str),
                               par->sample_rate,
                               unit_hertz_str));
        probe_int("channels", par->channels);
        probe_int("bits_per_sample",
                  av_get_bits_per_sample(par->codec_id));
        break;
    }

    if (fmt_ctx->iformat->flags & AVFMT_SHOW_IDS)
        probe_int("id", stream->id);
    probe_str("avg_frame_rate",
              rational_string(val_str, sizeof(val_str), "/",
              &stream->avg_frame_rate));

    if (par->bit_rate)
        probe_str("bit_rate",
                  value_string(val_str, sizeof(val_str),
                               par->bit_rate, unit_bit_per_second_str));
    probe_str("time_base",
              rational_string(val_str, sizeof(val_str), "/",
              &stream->time_base));
    probe_str("start_time",
              time_value_string(val_str, sizeof(val_str),
                                stream->start_time, &stream->time_base));
    probe_str("duration",
              time_value_string(val_str, sizeof(val_str),
                                stream->duration, &stream->time_base));
    if (stream->nb_frames)
        probe_int("nb_frames", stream->nb_frames);

    probe_dict(stream->metadata, "tags");

    if (stream->nb_side_data) {
        int i, j;

        probe_object_header("sidedata");
        for (i = 0; i < stream->nb_side_data; i++) {
            const AVPacketSideData* sd = &stream->side_data[i];
            AVStereo3D *stereo;
            AVSphericalMapping *spherical;

            switch (sd->type) {
            case AV_PKT_DATA_DISPLAYMATRIX:
                probe_object_header("displaymatrix");
                probe_array_header("matrix", 1);
                for (j = 0; j < 9; j++)
                    probe_int(NULL, ((int32_t *)sd->data)[j]);
                probe_array_footer("matrix", 1);
                probe_array_header("matrix_str", 1);
                for (j = 0; j < 9; j++) {
                    char buf[32];
                    int fp = (j == 2 || j == 5 || j == 8) ? 30 : 16;
                    int32_t val = ((int32_t *)sd->data)[j];
                    value_string(buf, sizeof(buf), CONV_FP(val, fp), "");
                    probe_str(NULL, buf);
                }
                probe_array_footer("matrix_str", 1);
                probe_int("rotation",
                          av_display_rotation_get((int32_t *)sd->data));
                probe_object_footer("displaymatrix");
                break;
            case AV_PKT_DATA_STEREO3D:
                stereo = (AVStereo3D *)sd->data;
                probe_object_header("stereo3d");
                probe_str("type", av_stereo3d_type_name(stereo->type));
                probe_int("inverted",
                          !!(stereo->flags & AV_STEREO3D_FLAG_INVERT));
                probe_object_footer("stereo3d");
                break;
            case AV_PKT_DATA_SPHERICAL:
                spherical = (AVSphericalMapping *)sd->data;
                probe_object_header("spherical");
                probe_str("projection", av_spherical_projection_name(spherical->projection));

                if (spherical->projection == AV_SPHERICAL_CUBEMAP) {
                    probe_int("padding", spherical->padding);
                } else if (spherical->projection == AV_SPHERICAL_EQUIRECTANGULAR_TILE) {
                    size_t l, t, r, b;
                    av_spherical_tile_bounds(spherical, par->width, par->height,
                                             &l, &t, &r, &b);
                    probe_object_header("bounding");
                    probe_int("left", l);
                    probe_int("top", t);
                    probe_int("right", r);
                    probe_int("bottom", b);
                    probe_object_footer("bounding");
                }

                probe_object_header("orientation");
                probe_int("yaw", (double) spherical->yaw / (1 << 16));
                probe_int("pitch", (double) spherical->pitch / (1 << 16));
                probe_int("roll", (double) spherical->roll / (1 << 16));
                probe_object_footer("orientation");

                probe_object_footer("spherical");
                break;
            }
        }
        probe_object_footer("sidedata");
    }

    probe_object_footer("stream");
}

static void show_format(InputFile *ifile)
{
    AVFormatContext *fmt_ctx = ifile->fmt_ctx;
    char val_str[128];
    int64_t size = fmt_ctx->pb ? avio_size(fmt_ctx->pb) : -1;

    probe_object_header("format");
    probe_str("filename",         fmt_ctx->filename);
    probe_int("nb_streams",       fmt_ctx->nb_streams);
    probe_str("format_name",      fmt_ctx->iformat->name);
    probe_str("format_long_name", fmt_ctx->iformat->long_name);
    probe_str("start_time",
                       time_value_string(val_str, sizeof(val_str),
                                         fmt_ctx->start_time, &AV_TIME_BASE_Q));
    probe_str("duration",
                       time_value_string(val_str, sizeof(val_str),
                                         fmt_ctx->duration, &AV_TIME_BASE_Q));
    probe_str("size",
                       size >= 0 ? value_string(val_str, sizeof(val_str),
                                                size, unit_byte_str)
                                  : "unknown");
    probe_str("bit_rate",
                       value_string(val_str, sizeof(val_str),
                                    fmt_ctx->bit_rate, unit_bit_per_second_str));

    probe_dict(fmt_ctx->metadata, "tags");

    probe_object_footer("format");
}

static int open_input_file(InputFile *ifile, const char *filename)
{
    int err, i;
    AVFormatContext *fmt_ctx = NULL;
    AVDictionaryEntry *t;

    if ((err = avformat_open_input(&fmt_ctx, filename,
                                   iformat, &format_opts)) < 0) {
        print_error(filename, err);
        return err;
    }
    if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        return AVERROR_OPTION_NOT_FOUND;
    }


    /* fill the streams in the format context */
    if ((err = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        print_error(filename, err);
        return err;
    }

    av_dump_format(fmt_ctx, 0, filename, 0);

    ifile->streams = av_mallocz_array(fmt_ctx->nb_streams,
                                      sizeof(*ifile->streams));
    if (!ifile->streams)
        exit(1);
    ifile->nb_streams = fmt_ctx->nb_streams;

    /* bind a decoder to each input stream */
    for (i = 0; i < fmt_ctx->nb_streams; i++) {
        InputStream *ist = &ifile->streams[i];
        AVStream *stream = fmt_ctx->streams[i];
        AVCodec *codec;

        ist->st = stream;

        if (stream->codecpar->codec_id == AV_CODEC_ID_PROBE) {
            fprintf(stderr, "Failed to probe codec for input stream %d\n",
                    stream->index);
            continue;
        }

        codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            fprintf(stderr,
                    "Unsupported codec with id %d for input stream %d\n",
                    stream->codecpar->codec_id, stream->index);
            continue;
        }

        ist->dec_ctx = avcodec_alloc_context3(codec);
        if (!ist->dec_ctx)
            exit(1);

        err = avcodec_parameters_to_context(ist->dec_ctx, stream->codecpar);
        if (err < 0)
            exit(1);

        err = avcodec_open2(ist->dec_ctx, NULL, NULL);
        if (err < 0) {
            fprintf(stderr, "Error while opening codec for input stream %d\n",
                    stream->index);
            exit(1);

        }
    }

    ifile->fmt_ctx = fmt_ctx;
    return 0;
}

static void close_input_file(InputFile *ifile)
{
    int i;

    /* close decoder for each stream */
    for (i = 0; i < ifile->nb_streams; i++) {
        InputStream *ist = &ifile->streams[i];

        avcodec_free_context(&ist->dec_ctx);
    }

    av_freep(&ifile->streams);
    ifile->nb_streams = 0;

    avformat_close_input(&ifile->fmt_ctx);
}

static int probe_file(const char *filename)
{
    InputFile ifile;
    int ret, i;

    ret = open_input_file(&ifile, filename);
    if (ret < 0)
        return ret;

    if (do_show_format)
        show_format(&ifile);

    if (do_show_streams) {
        probe_array_header("streams", 0);
        for (i = 0; i < ifile.nb_streams; i++)
            show_stream(&ifile, &ifile.streams[i]);
        probe_array_footer("streams", 0);
    }

    if (do_show_packets)
        show_packets(&ifile);

    close_input_file(&ifile);
    return 0;
}

static void show_usage(void)
{
    printf("Simple multimedia streams analyzer\n");
    printf("usage: %s [OPTIONS] [INPUT_FILE]\n", program_name);
    printf("\n");
}

static int opt_format(void *optctx, const char *opt, const char *arg)
{
    iformat = av_find_input_format(arg);
    if (!iformat) {
        fprintf(stderr, "Unknown input format: %s\n", arg);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int opt_output_format(void *optctx, const char *opt, const char *arg)
{

    if (!strcmp(arg, "json")) {
        octx.print_header        = json_print_header;
        octx.print_footer        = json_print_footer;
        octx.print_array_header  = json_print_array_header;
        octx.print_array_footer  = json_print_array_footer;
        octx.print_object_header = json_print_object_header;
        octx.print_object_footer = json_print_object_footer;

        octx.print_integer = json_print_integer;
        octx.print_string  = json_print_string;
    } else if (!strcmp(arg, "ini")) {
        octx.print_header        = ini_print_header;
        octx.print_footer        = ini_print_footer;
        octx.print_array_header  = ini_print_array_header;
        octx.print_array_footer  = ini_print_array_footer;
        octx.print_object_header = ini_print_object_header;

        octx.print_integer = ini_print_integer;
        octx.print_string  = ini_print_string;
    } else if (!strcmp(arg, "old")) {
        octx.print_header        = NULL;
        octx.print_object_header = old_print_object_header;
        octx.print_object_footer = old_print_object_footer;

        octx.print_string        = old_print_string;
    } else {
        av_log(NULL, AV_LOG_ERROR, "Unsupported formatter %s\n", arg);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int opt_show_format_entry(void *optctx, const char *opt, const char *arg)
{
    do_show_format = 1;
    nb_fmt_entries_to_show++;
    octx.print_header        = NULL;
    octx.print_footer        = NULL;
    octx.print_array_header  = NULL;
    octx.print_array_footer  = NULL;
    octx.print_object_header = NULL;
    octx.print_object_footer = NULL;

    octx.print_integer = show_format_entry_integer;
    octx.print_string  = show_format_entry_string;
    av_dict_set(&fmt_entries_to_show, arg, "", 0);
    return 0;
}

static int opt_show_stream_entry(void *optctx, const char *opt, const char *arg)
{
    const char *p = arg;

    do_show_streams = 1;
    nb_stream_entries_to_show++;
    octx.print_header        = NULL;
    octx.print_footer        = NULL;
    octx.print_array_header  = show_stream_entry_header;
    octx.print_array_footer  = show_stream_entry_footer;
    octx.print_object_header = NULL;
    octx.print_object_footer = NULL;

    octx.print_integer = show_stream_entry_integer;
    octx.print_string  = show_stream_entry_string;

    while (*p) {
        char *val = av_get_token(&p, ",");
        if (!val)
            return AVERROR(ENOMEM);

        av_dict_set(&stream_entries_to_show, val, "", 0);

        av_free(val);
        if (*p)
            p++;
    }

    return 0;
}

static void opt_input_file(void *optctx, const char *arg)
{
    if (input_filename) {
        fprintf(stderr,
                "Argument '%s' provided as input filename, but '%s' was already specified.\n",
                arg, input_filename);
        exit_program(1);
    }
    if (!strcmp(arg, "-"))
        arg = "pipe:";
    input_filename = arg;
}

void show_help_default(const char *opt, const char *arg)
{
    av_log_set_callback(log_callback_help);
    show_usage();
    show_help_options(options, "Main options:", 0, 0, 0);
    printf("\n");
    show_help_children(avformat_get_class(), AV_OPT_FLAG_DECODING_PARAM);
}

static int opt_pretty(void *optctx, const char *opt, const char *arg)
{
    show_value_unit              = 1;
    use_value_prefix             = 1;
    use_byte_value_binary_prefix = 1;
    use_value_sexagesimal_format = 1;
    return 0;
}

static const OptionDef real_options[] = {
    CMDUTILS_COMMON_OPTIONS
    { "f", HAS_ARG, {.func_arg = opt_format}, "force format", "format" },
    { "of", HAS_ARG, {.func_arg = opt_output_format}, "output the document either as ini or json", "output_format" },
    { "unit", OPT_BOOL, {&show_value_unit},
      "show unit of the displayed values" },
    { "prefix", OPT_BOOL, {&use_value_prefix},
      "use SI prefixes for the displayed values" },
    { "byte_binary_prefix", OPT_BOOL, {&use_byte_value_binary_prefix},
      "use binary prefixes for byte units" },
    { "sexagesimal", OPT_BOOL,  {&use_value_sexagesimal_format},
      "use sexagesimal format HOURS:MM:SS.MICROSECONDS for time units" },
    { "pretty", 0, {.func_arg = opt_pretty},
      "prettify the format of displayed values, make it more human readable" },
    { "show_format",  OPT_BOOL, {&do_show_format} , "show format/container info" },
    { "show_format_entry", HAS_ARG, {.func_arg = opt_show_format_entry},
      "show a particular entry from the format/container info", "entry" },
    { "show_packets", OPT_BOOL, {&do_show_packets}, "show packets info" },
    { "show_streams", OPT_BOOL, {&do_show_streams}, "show streams info" },
    { "show_stream_entry", HAS_ARG, {.func_arg = opt_show_stream_entry},
      "show a particular entry from all streams (comma separated)", "entry" },
    { "default", HAS_ARG | OPT_AUDIO | OPT_VIDEO | OPT_EXPERT, {.func_arg = opt_default},
      "generic catch all option", "" },
    { NULL, },
};

static int probe_buf_write(void *opaque, uint8_t *buf, int buf_size)
{
    printf("%.*s", buf_size, buf);
    return 0;
}

#define AVP_BUFFSIZE 4096

int main(int argc, char **argv)
{
    int ret;
    uint8_t *buffer = av_malloc(AVP_BUFFSIZE);

    if (!buffer)
        exit(1);

    register_exit(avprobe_cleanup);

    options = real_options;
    parse_loglevel(argc, argv, options);
    av_register_all();
    avformat_network_init();
    init_opts();
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif

    show_banner();

    octx.print_header = ini_print_header;
    octx.print_footer = ini_print_footer;

    octx.print_array_header = ini_print_array_header;
    octx.print_array_footer = ini_print_array_footer;
    octx.print_object_header = ini_print_object_header;

    octx.print_integer = ini_print_integer;
    octx.print_string = ini_print_string;

    parse_options(NULL, argc, argv, options, opt_input_file);

    if (!input_filename) {
        show_usage();
        fprintf(stderr, "You have to specify one input file.\n");
        fprintf(stderr,
                "Use -h to get full help or, even better, run 'man %s'.\n",
                program_name);
        exit_program(1);
    }

    probe_out = avio_alloc_context(buffer, AVP_BUFFSIZE, 1, NULL, NULL,
                                 probe_buf_write, NULL);
    if (!probe_out)
        exit_program(1);

    probe_header();
    ret = probe_file(input_filename);
    probe_footer();
    avio_flush(probe_out);
    avio_context_free(&probe_out);
    av_freep(&buffer);
    uninit_opts();
    avformat_network_deinit();

    return ret;
}

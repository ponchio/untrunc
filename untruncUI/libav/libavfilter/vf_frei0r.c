/*
 * Copyright (c) 2010 Stefano Sabatini
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
 * frei0r wrapper
 */

#include <dlfcn.h>
#include <frei0r.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "libavutil/avstring.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef f0r_instance_t (*f0r_construct_f)(unsigned int width, unsigned int height);
typedef void (*f0r_destruct_f)(f0r_instance_t instance);
typedef void (*f0r_deinit_f)(void);
typedef int (*f0r_init_f)(void);
typedef void (*f0r_get_plugin_info_f)(f0r_plugin_info_t *info);
typedef void (*f0r_get_param_info_f)(f0r_param_info_t *info, int param_index);
typedef void (*f0r_update_f)(f0r_instance_t instance, double time, const uint32_t *inframe, uint32_t *outframe);
typedef void (*f0r_update2_f)(f0r_instance_t instance, double time, const uint32_t *inframe1, const uint32_t *inframe2, const uint32_t *inframe3, uint32_t *outframe);
typedef void (*f0r_set_param_value_f)(f0r_instance_t instance, f0r_param_t param, int param_index);
typedef void (*f0r_get_param_value_f)(f0r_instance_t instance, f0r_param_t param, int param_index);

typedef struct Frei0rContext {
    const AVClass *class;
    f0r_update_f update;
    void *dl_handle;            /* dynamic library handle   */
    f0r_instance_t instance;
    f0r_plugin_info_t plugin_info;

    f0r_get_param_info_f  get_param_info;
    f0r_get_param_value_f get_param_value;
    f0r_set_param_value_f set_param_value;
    f0r_construct_f       construct;
    f0r_destruct_f        destruct;
    f0r_deinit_f          deinit;

    char *dl_name;
    char *params;
    char *size;
    char *framerate;

    /* only used by the source */
    int w, h;
    AVRational time_base;
    uint64_t pts;
} Frei0rContext;

static void *load_sym(AVFilterContext *ctx, const char *sym_name)
{
    Frei0rContext *s = ctx->priv;
    void *sym = dlsym(s->dl_handle, sym_name);
    if (!sym)
        av_log(ctx, AV_LOG_ERROR, "Could not find symbol '%s' in loaded module.\n", sym_name);
    return sym;
}

static int set_param(AVFilterContext *ctx, f0r_param_info_t info, int index, char *param)
{
    Frei0rContext *s = ctx->priv;
    union {
        double d;
        f0r_param_color_t col;
        f0r_param_position_t pos;
    } val;
    char *tail;
    uint8_t rgba[4];

    switch (info.type) {
    case F0R_PARAM_BOOL:
        if      (!strcmp(param, "y")) val.d = 1.0;
        else if (!strcmp(param, "n")) val.d = 0.0;
        else goto fail;
        break;

    case F0R_PARAM_DOUBLE:
        val.d = strtod(param, &tail);
        if (*tail || val.d == HUGE_VAL)
            goto fail;
        break;

    case F0R_PARAM_COLOR:
        if (sscanf(param, "%f/%f/%f", &val.col.r, &val.col.g, &val.col.b) != 3) {
            if (av_parse_color(rgba, param, -1, ctx) < 0)
                goto fail;
            val.col.r = rgba[0] / 255.0;
            val.col.g = rgba[1] / 255.0;
            val.col.b = rgba[2] / 255.0;
        }
        break;

    case F0R_PARAM_POSITION:
        if (sscanf(param, "%lf/%lf", &val.pos.x, &val.pos.y) != 2)
            goto fail;
        break;
    }

    s->set_param_value(s->instance, &val, index);
    return 0;

fail:
    av_log(ctx, AV_LOG_ERROR, "Invalid value '%s' for parameter '%s'.\n",
           param, info.name);
    return AVERROR(EINVAL);
}

static int set_params(AVFilterContext *ctx, const char *params)
{
    Frei0rContext *s = ctx->priv;
    int i;

    for (i = 0; i < s->plugin_info.num_params; i++) {
        f0r_param_info_t info;
        char *param;
        int ret;

        s->get_param_info(&info, i);

        if (*params) {
            if (!(param = av_get_token(&params, "|")))
                return AVERROR(ENOMEM);
            if (*params)
                params++;               /* skip ':' */
            ret = set_param(ctx, info, i, param);
            av_free(param);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static void *load_path(AVFilterContext *ctx, const char *prefix, const char *name)
{
    char path[1024];

    snprintf(path, sizeof(path), "%s%s%s", prefix, name, SLIBSUF);
    av_log(ctx, AV_LOG_DEBUG, "Looking for frei0r effect in '%s'.\n", path);
    return dlopen(path, RTLD_NOW|RTLD_LOCAL);
}

static av_cold int frei0r_init(AVFilterContext *ctx,
                               const char *dl_name, int type)
{
    Frei0rContext *s = ctx->priv;
    f0r_init_f            f0r_init;
    f0r_get_plugin_info_f f0r_get_plugin_info;
    f0r_plugin_info_t *pi;
    char *path;

    if (!dl_name) {
        av_log(ctx, AV_LOG_ERROR, "No filter name provided.\n");
        return AVERROR(EINVAL);
    }

    /* see: http://piksel.org/frei0r/1.2/spec/1.2/spec/group__pluglocations.html */
    if (path = getenv("FREI0R_PATH")) {
        while(*path) {
            char *ptr = av_get_token((const char **)&path, ":");
            if (!ptr)
                return AVERROR(ENOMEM);
            s->dl_handle = load_path(ctx, ptr, dl_name);
            av_freep(&ptr);
            if (s->dl_handle)
                break;              /* found */
            if (*path)
                path++;             /* skip ':' */
        }
    }
    if (!s->dl_handle && (path = getenv("HOME"))) {
        char prefix[1024];
        snprintf(prefix, sizeof(prefix), "%s/.frei0r-1/lib/", path);
        s->dl_handle = load_path(ctx, prefix, dl_name);
    }
    if (!s->dl_handle)
        s->dl_handle = load_path(ctx, "/usr/local/lib/frei0r-1/", dl_name);
    if (!s->dl_handle)
        s->dl_handle = load_path(ctx, "/usr/lib/frei0r-1/", dl_name);
    if (!s->dl_handle) {
        av_log(ctx, AV_LOG_ERROR, "Could not find module '%s'.\n", dl_name);
        return AVERROR(EINVAL);
    }

    if (!(f0r_init                = load_sym(ctx, "f0r_init"           )) ||
        !(f0r_get_plugin_info     = load_sym(ctx, "f0r_get_plugin_info")) ||
        !(s->get_param_info  = load_sym(ctx, "f0r_get_param_info" )) ||
        !(s->get_param_value = load_sym(ctx, "f0r_get_param_value")) ||
        !(s->set_param_value = load_sym(ctx, "f0r_set_param_value")) ||
        !(s->update          = load_sym(ctx, "f0r_update"         )) ||
        !(s->construct       = load_sym(ctx, "f0r_construct"      )) ||
        !(s->destruct        = load_sym(ctx, "f0r_destruct"       )) ||
        !(s->deinit          = load_sym(ctx, "f0r_deinit"         )))
        return AVERROR(EINVAL);

    if (f0r_init() < 0) {
        av_log(ctx, AV_LOG_ERROR, "Could not init the frei0r module.\n");
        return AVERROR(EINVAL);
    }

    f0r_get_plugin_info(&s->plugin_info);
    pi = &s->plugin_info;
    if (pi->plugin_type != type) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid type '%s' for this plugin\n",
               pi->plugin_type == F0R_PLUGIN_TYPE_FILTER ? "filter" :
               pi->plugin_type == F0R_PLUGIN_TYPE_SOURCE ? "source" :
               pi->plugin_type == F0R_PLUGIN_TYPE_MIXER2 ? "mixer2" :
               pi->plugin_type == F0R_PLUGIN_TYPE_MIXER3 ? "mixer3" : "unknown");
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_VERBOSE,
           "name:%s author:'%s' explanation:'%s' color_model:%s "
           "frei0r_version:%d version:%d.%d num_params:%d\n",
           pi->name, pi->author, pi->explanation,
           pi->color_model == F0R_COLOR_MODEL_BGRA8888 ? "bgra8888" :
           pi->color_model == F0R_COLOR_MODEL_RGBA8888 ? "rgba8888" :
           pi->color_model == F0R_COLOR_MODEL_PACKED32 ? "packed32" : "unknown",
           pi->frei0r_version, pi->major_version, pi->minor_version, pi->num_params);

    return 0;
}

static av_cold int filter_init(AVFilterContext *ctx)
{
    Frei0rContext *s = ctx->priv;

    return frei0r_init(ctx, s->dl_name, F0R_PLUGIN_TYPE_FILTER);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    Frei0rContext *s = ctx->priv;

    if (s->destruct && s->instance)
        s->destruct(s->instance);
    if (s->deinit)
        s->deinit();
    if (s->dl_handle)
        dlclose(s->dl_handle);
}

static int config_input_props(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    Frei0rContext *s = ctx->priv;

    if (s->destruct && s->instance)
        s->destruct(s->instance);
    if (!(s->instance = s->construct(inlink->w, inlink->h))) {
        av_log(ctx, AV_LOG_ERROR, "Impossible to load frei0r instance.\n");
        return AVERROR(EINVAL);
    }

    return set_params(ctx, s->params);
}

static int query_formats(AVFilterContext *ctx)
{
    Frei0rContext *s = ctx->priv;
    AVFilterFormats *formats = NULL;

    if        (s->plugin_info.color_model == F0R_COLOR_MODEL_BGRA8888) {
        ff_add_format(&formats, AV_PIX_FMT_BGRA);
    } else if (s->plugin_info.color_model == F0R_COLOR_MODEL_RGBA8888) {
        ff_add_format(&formats, AV_PIX_FMT_RGBA);
    } else {                                   /* F0R_COLOR_MODEL_PACKED32 */
        static const enum AVPixelFormat pix_fmts[] = {
            AV_PIX_FMT_BGRA, AV_PIX_FMT_ARGB, AV_PIX_FMT_ABGR, AV_PIX_FMT_ARGB, AV_PIX_FMT_NONE
        };
        formats = ff_make_format_list(pix_fmts);
    }

    if (!formats)
        return AVERROR(ENOMEM);

    ff_set_common_formats(ctx, formats);
    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    Frei0rContext *s = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    AVFrame *out;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    s->update(s->instance, in->pts * av_q2d(inlink->time_base) * 1000,
                   (const uint32_t *)in->data[0],
                   (uint32_t *)out->data[0]);

    av_frame_free(&in);

    return ff_filter_frame(outlink, out);
}

#define OFFSET(x) offsetof(Frei0rContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM
static const AVOption filter_options[] = {
    { "filter_name",   NULL, OFFSET(dl_name), AV_OPT_TYPE_STRING, .flags = FLAGS },
    { "filter_params", NULL, OFFSET(params),  AV_OPT_TYPE_STRING, .flags = FLAGS },
    { NULL },
};

static const AVClass filter_class = {
    .class_name = "frei0r",
    .item_name  = av_default_item_name,
    .option     = filter_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad avfilter_vf_frei0r_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_props,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_frei0r_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_frei0r = {
    .name      = "frei0r",
    .description = NULL_IF_CONFIG_SMALL("Apply a frei0r effect."),

    .query_formats = query_formats,
    .init = filter_init,
    .uninit = uninit,

    .priv_size = sizeof(Frei0rContext),
    .priv_class = &filter_class,

    .inputs    = avfilter_vf_frei0r_inputs,

    .outputs   = avfilter_vf_frei0r_outputs,
};

static av_cold int source_init(AVFilterContext *ctx)
{
    Frei0rContext *s = ctx->priv;
    AVRational frame_rate_q;

    if (av_parse_video_size(&s->w, &s->h, s->size) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Invalid frame size: '%s'.\n", s->size);
        return AVERROR(EINVAL);
    }

    if (av_parse_video_rate(&frame_rate_q, s->framerate) < 0 ||
        frame_rate_q.den <= 0 || frame_rate_q.num <= 0) {
        av_log(ctx, AV_LOG_ERROR, "Invalid frame rate: '%s'.\n", s->framerate);
        return AVERROR(EINVAL);
    }
    s->time_base.num = frame_rate_q.den;
    s->time_base.den = frame_rate_q.num;

    return frei0r_init(ctx, s->dl_name, F0R_PLUGIN_TYPE_SOURCE);
}

static int source_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    Frei0rContext *s = ctx->priv;

    if (av_image_check_size(s->w, s->h, 0, ctx) < 0)
        return AVERROR(EINVAL);
    outlink->w = s->w;
    outlink->h = s->h;
    outlink->time_base = s->time_base;
    outlink->frame_rate = av_inv_q(s->time_base);

    if (s->destruct && s->instance)
        s->destruct(s->instance);
    if (!(s->instance = s->construct(outlink->w, outlink->h))) {
        av_log(ctx, AV_LOG_ERROR, "Impossible to load frei0r instance.\n");
        return AVERROR(EINVAL);
    }
    if (!s->params) {
        av_log(ctx, AV_LOG_ERROR, "frei0r filter parameters not set.\n");
        return AVERROR(EINVAL);
    }

    return set_params(ctx, s->params);
}

static int source_request_frame(AVFilterLink *outlink)
{
    Frei0rContext *s = outlink->src->priv;
    AVFrame *frame = ff_get_video_buffer(outlink, outlink->w, outlink->h);

    if (!frame)
        return AVERROR(ENOMEM);

    frame->sample_aspect_ratio = (AVRational) {1, 1};
    frame->pts = s->pts++;

    s->update(s->instance, av_rescale_q(frame->pts, s->time_base, (AVRational){1,1000}),
                   NULL, (uint32_t *)frame->data[0]);

    return ff_filter_frame(outlink, frame);
}

static const AVOption src_options[] = {
    { "size",          "Dimensions of the generated video.", OFFSET(size),      AV_OPT_TYPE_STRING, { .str = "" },   .flags = FLAGS },
    { "framerate",     NULL,                                 OFFSET(framerate), AV_OPT_TYPE_STRING, { .str = "25" }, .flags = FLAGS },
    { "filter_name",   NULL,                                 OFFSET(dl_name),   AV_OPT_TYPE_STRING,                  .flags = FLAGS },
    { "filter_params", NULL,                                 OFFSET(params),    AV_OPT_TYPE_STRING,                  .flags = FLAGS },
    { NULL },
};

static const AVClass src_class = {
    .class_name = "frei0r_src",
    .item_name  = av_default_item_name,
    .option     = src_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad avfilter_vsrc_frei0r_src_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = source_request_frame,
        .config_props  = source_config_props
    },
    { NULL }
};

AVFilter ff_vsrc_frei0r_src = {
    .name        = "frei0r_src",
    .description = NULL_IF_CONFIG_SMALL("Generate a frei0r source."),

    .priv_size = sizeof(Frei0rContext),
    .priv_class = &src_class,
    .init      = source_init,
    .uninit    = uninit,

    .query_formats = query_formats,

    .inputs    = NULL,

    .outputs   = avfilter_vsrc_frei0r_src_outputs,
};

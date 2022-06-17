/*
 * Copyright (c) 2011 Stefano Sabatini
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
 * filter for showing textual video frame information
 */

#include <inttypes.h>

#include "libavutil/adler32.h"
#include "libavutil/display.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/pixdesc.h"
#include "libavutil/spherical.h"
#include "libavutil/stereo3d.h"

#include "avfilter.h"
#include "internal.h"
#include "video.h"

typedef struct ShowInfoContext {
    unsigned int frame;
} ShowInfoContext;

static void dump_spherical(AVFilterContext *ctx, AVFrame *frame, AVFrameSideData *sd)
{
    AVSphericalMapping *spherical = (AVSphericalMapping *)sd->data;
    double yaw, pitch, roll;

    av_log(ctx, AV_LOG_INFO, "spherical information: ");
    if (sd->size < sizeof(*spherical)) {
        av_log(ctx, AV_LOG_INFO, "invalid data");
        return;
    }

    if (spherical->projection == AV_SPHERICAL_EQUIRECTANGULAR)
        av_log(ctx, AV_LOG_INFO, "equirectangular ");
    else if (spherical->projection == AV_SPHERICAL_CUBEMAP)
        av_log(ctx, AV_LOG_INFO, "cubemap ");
    else if (spherical->projection == AV_SPHERICAL_EQUIRECTANGULAR_TILE)
        av_log(ctx, AV_LOG_INFO, "tiled equirectangular ");
    else {
        av_log(ctx, AV_LOG_WARNING, "unknown");
        return;
    }

    yaw = ((double)spherical->yaw) / (1 << 16);
    pitch = ((double)spherical->pitch) / (1 << 16);
    roll = ((double)spherical->roll) / (1 << 16);
    av_log(ctx, AV_LOG_INFO, "(%f/%f/%f) ", yaw, pitch, roll);

    if (spherical->projection == AV_SPHERICAL_EQUIRECTANGULAR_TILE) {
        size_t l, t, r, b;
        av_spherical_tile_bounds(spherical, frame->width, frame->height,
                                 &l, &t, &r, &b);
        av_log(ctx, AV_LOG_INFO, "[%zu, %zu, %zu, %zu] ", l, t, r, b);
    } else if (spherical->projection == AV_SPHERICAL_CUBEMAP) {
        av_log(ctx, AV_LOG_INFO, "[pad %"PRIu32"] ", spherical->padding);
    }
}

static void dump_stereo3d(AVFilterContext *ctx, AVFrameSideData *sd)
{
    AVStereo3D *stereo;

    av_log(ctx, AV_LOG_INFO, "stereoscopic information: ");
    if (sd->size < sizeof(*stereo)) {
        av_log(ctx, AV_LOG_INFO, "invalid data");
        return;
    }

    stereo = (AVStereo3D *)sd->data;

    av_log(ctx, AV_LOG_INFO, "type - %s", av_stereo3d_type_name(stereo->type));

    if (stereo->flags & AV_STEREO3D_FLAG_INVERT)
        av_log(ctx, AV_LOG_INFO, " (inverted)");
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    ShowInfoContext *showinfo = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    uint32_t plane_checksum[4] = {0}, checksum = 0;
    int i, plane, vsub = desc->log2_chroma_h;

    for (plane = 0; frame->data[plane] && plane < 4; plane++) {
        uint8_t *data = frame->data[plane];
        int h = plane == 1 || plane == 2 ? inlink->h >> vsub : inlink->h;
        int linesize = av_image_get_linesize(frame->format, frame->width, plane);
        if (linesize < 0)
            return linesize;

        for (i = 0; i < h; i++) {
            plane_checksum[plane] = av_adler32_update(plane_checksum[plane], data, linesize);
            checksum = av_adler32_update(checksum, data, linesize);
            data += frame->linesize[plane];
        }
    }

    av_log(ctx, AV_LOG_INFO,
           "n:%d pts:%"PRId64" pts_time:%f "
           "fmt:%s sar:%d/%d s:%dx%d i:%c iskey:%d type:%c "
           "checksum:%"PRIu32" plane_checksum:[%"PRIu32" %"PRIu32" %"PRIu32" %"PRIu32"]\n",
           showinfo->frame,
           frame->pts, frame->pts * av_q2d(inlink->time_base),
           desc->name,
           frame->sample_aspect_ratio.num, frame->sample_aspect_ratio.den,
           frame->width, frame->height,
           !frame->interlaced_frame ? 'P' :         /* Progressive  */
           frame->top_field_first   ? 'T' : 'B',    /* Top / Bottom */
           frame->key_frame,
           av_get_picture_type_char(frame->pict_type),
           checksum, plane_checksum[0], plane_checksum[1], plane_checksum[2], plane_checksum[3]);

    for (i = 0; i < frame->nb_side_data; i++) {
        AVFrameSideData *sd = frame->side_data[i];

        av_log(ctx, AV_LOG_INFO, "  side data - ");
        switch (sd->type) {
        case AV_FRAME_DATA_PANSCAN:
            av_log(ctx, AV_LOG_INFO, "pan/scan");
            break;
        case AV_FRAME_DATA_A53_CC:
            av_log(ctx, AV_LOG_INFO, "A/53 closed captions (%d bytes)", sd->size);
            break;
        case AV_FRAME_DATA_SPHERICAL:
            dump_spherical(ctx, frame, sd);
            break;
        case AV_FRAME_DATA_STEREO3D:
            dump_stereo3d(ctx, sd);
            break;
        case AV_FRAME_DATA_DISPLAYMATRIX:
            av_log(ctx, AV_LOG_INFO, "displaymatrix: rotation of %.2f degrees",
                   av_display_rotation_get((int32_t *)sd->data));
            break;
        case AV_FRAME_DATA_AFD:
            av_log(ctx, AV_LOG_INFO, "afd: value of %"PRIu8, sd->data[0]);
            break;
        default:
            av_log(ctx, AV_LOG_WARNING, "unknown side data type %d (%d bytes)",
                   sd->type, sd->size);
            break;
        }

        av_log(ctx, AV_LOG_INFO, "\n");
    }

    showinfo->frame++;
    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

static int config_props(AVFilterContext *ctx, AVFilterLink *link, int is_out)
{

    av_log(ctx, AV_LOG_INFO, "config %s time_base: %d/%d, frame_rate: %d/%d\n",
           is_out ? "out" : "in",
           link->time_base.num, link->time_base.den,
           link->frame_rate.num, link->frame_rate.den);

    return 0;
}

static int config_props_in(AVFilterLink *link)
{
    AVFilterContext *ctx = link->dst;
    return config_props(ctx, link, 0);
}

static int config_props_out(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    return config_props(ctx, link, 1);
}

static const AVFilterPad avfilter_vf_showinfo_inputs[] = {
    {
        .name             = "default",
        .type             = AVMEDIA_TYPE_VIDEO,
        .get_video_buffer = ff_null_get_video_buffer,
        .filter_frame     = filter_frame,
        .config_props     = config_props_in,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_showinfo_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_props_out,
    },
    { NULL }
};

AVFilter ff_vf_showinfo = {
    .name        = "showinfo",
    .description = NULL_IF_CONFIG_SMALL("Show textual information for each video frame."),

    .priv_size = sizeof(ShowInfoContext),

    .inputs    = avfilter_vf_showinfo_inputs,

    .outputs   = avfilter_vf_showinfo_outputs,
};

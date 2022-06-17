/*
 * Copyright (c) 2009 Stefano Sabatini
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

#include <stdio.h>

#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/formats.h"

int main(int argc, char **argv)
{
    const AVFilter *filter;
    AVFilterContext *filter_ctx;
    AVFilterGraph *graph_ctx;
    const char *filter_name;
    const char *filter_args = NULL;
    int i, j, ret = 0;

    av_log_set_level(AV_LOG_DEBUG);

    if (!argv[1]) {
        fprintf(stderr, "Missing filter name as argument\n");
        return 1;
    }

    filter_name = argv[1];
    if (argv[2])
        filter_args = argv[2];

    /* allocate graph */
    graph_ctx = avfilter_graph_alloc();
    if (!graph_ctx)
        return 1;

    avfilter_register_all();

    /* get a corresponding filter and open it */
    if (!(filter = avfilter_get_by_name(filter_name))) {
        fprintf(stderr, "Unrecognized filter with name '%s'\n", filter_name);
        return 1;
    }

    /* open filter and add it to the graph */
    if (!(filter_ctx = avfilter_graph_alloc_filter(graph_ctx, filter, filter_name))) {
        fprintf(stderr, "Impossible to open filter with name '%s'\n",
                filter_name);
        return 1;
    }
    if (avfilter_init_str(filter_ctx, filter_args) < 0) {
        fprintf(stderr, "Impossible to init filter '%s' with arguments '%s'\n",
                filter_name, filter_args);
        return 1;
    }

    /* create a link for each of the input pads */
    for (i = 0; i < filter_ctx->nb_inputs; i++) {
        AVFilterLink *link = av_mallocz(sizeof(AVFilterLink));
        if (!link) {
            fprintf(stderr, "Unable to allocate memory for filter input link\n");
            ret = 1;
            goto fail;
        }
        link->type = avfilter_pad_get_type(filter_ctx->filter->inputs, i);
        filter_ctx->inputs[i] = link;
    }
    for (i = 0; i < filter_ctx->nb_outputs; i++) {
        AVFilterLink *link = av_mallocz(sizeof(AVFilterLink));
        if (!link) {
            fprintf(stderr, "Unable to allocate memory for filter output link\n");
            ret = 1;
            goto fail;
        }
        link->type = avfilter_pad_get_type(filter_ctx->filter->outputs, i);
        filter_ctx->outputs[i] = link;
    }

    if (filter->query_formats)
        filter->query_formats(filter_ctx);
    else
        ff_default_query_formats(filter_ctx);

    /* print the supported formats in input */
    for (i = 0; i < filter_ctx->nb_inputs; i++) {
        AVFilterFormats *fmts = filter_ctx->inputs[i]->out_formats;
        for (j = 0; j < fmts->nb_formats; j++)
            printf("INPUT[%d] %s: %s\n",
                   i, avfilter_pad_get_name(filter_ctx->filter->inputs, i),
                   av_get_pix_fmt_name(fmts->formats[j]));
    }

    /* print the supported formats in output */
    for (i = 0; i < filter_ctx->nb_outputs; i++) {
        AVFilterFormats *fmts = filter_ctx->outputs[i]->in_formats;
        for (j = 0; j < fmts->nb_formats; j++)
            printf("OUTPUT[%d] %s: %s\n",
                   i, avfilter_pad_get_name(filter_ctx->filter->outputs, i),
                   av_get_pix_fmt_name(fmts->formats[j]));
    }

fail:
    avfilter_free(filter_ctx);
    avfilter_graph_free(&graph_ctx);
    fflush(stdout);
    return ret;
}

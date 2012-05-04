/*
 * filter graphs
 * Copyright (c) 2008 Vitor Sessak
 * Copyright (c) 2007 Bobby Bingham
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

#include <ctype.h>
#include <string.h>

#include "avfilter.h"
#include "avfiltergraph.h"
#include "internal.h"

#include "libavutil/log.h"

static const AVClass filtergraph_class = {
    .class_name = "AVFilterGraph",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVFilterGraph *avfilter_graph_alloc(void)
{
    AVFilterGraph *ret = av_mallocz(sizeof(AVFilterGraph));
    if (!ret)
        return NULL;
#if FF_API_GRAPH_AVCLASS
    ret->av_class = &filtergraph_class;
#endif
    return ret;
}

void avfilter_graph_free(AVFilterGraph **graph)
{
    if (!*graph)
        return;
    for (; (*graph)->filter_count > 0; (*graph)->filter_count--)
        avfilter_free((*graph)->filters[(*graph)->filter_count - 1]);
    av_freep(&(*graph)->scale_sws_opts);
    av_freep(&(*graph)->filters);
    av_freep(graph);
}

int avfilter_graph_add_filter(AVFilterGraph *graph, AVFilterContext *filter)
{
    AVFilterContext **filters = av_realloc(graph->filters,
                                           sizeof(AVFilterContext*) * (graph->filter_count+1));
    if (!filters)
        return AVERROR(ENOMEM);

    graph->filters = filters;
    graph->filters[graph->filter_count++] = filter;

    return 0;
}

int avfilter_graph_create_filter(AVFilterContext **filt_ctx, AVFilter *filt,
                                 const char *name, const char *args, void *opaque,
                                 AVFilterGraph *graph_ctx)
{
    int ret;

    if ((ret = avfilter_open(filt_ctx, filt, name)) < 0)
        goto fail;
    if ((ret = avfilter_init_filter(*filt_ctx, args, opaque)) < 0)
        goto fail;
    if ((ret = avfilter_graph_add_filter(graph_ctx, *filt_ctx)) < 0)
        goto fail;
    return 0;

fail:
    if (*filt_ctx)
        avfilter_free(*filt_ctx);
    *filt_ctx = NULL;
    return ret;
}

int ff_avfilter_graph_check_validity(AVFilterGraph *graph, AVClass *log_ctx)
{
    AVFilterContext *filt;
    int i, j;

    for (i = 0; i < graph->filter_count; i++) {
        filt = graph->filters[i];

        for (j = 0; j < filt->input_count; j++) {
            if (!filt->inputs[j] || !filt->inputs[j]->src) {
                av_log(log_ctx, AV_LOG_ERROR,
                       "Input pad \"%s\" for the filter \"%s\" of type \"%s\" not connected to any source\n",
                       filt->input_pads[j].name, filt->name, filt->filter->name);
                return AVERROR(EINVAL);
            }
        }

        for (j = 0; j < filt->output_count; j++) {
            if (!filt->outputs[j] || !filt->outputs[j]->dst) {
                av_log(log_ctx, AV_LOG_ERROR,
                       "Output pad \"%s\" for the filter \"%s\" of type \"%s\" not connected to any destination\n",
                       filt->output_pads[j].name, filt->name, filt->filter->name);
                return AVERROR(EINVAL);
            }
        }
    }

    return 0;
}

int ff_avfilter_graph_config_links(AVFilterGraph *graph, AVClass *log_ctx)
{
    AVFilterContext *filt;
    int i, ret;

    for (i=0; i < graph->filter_count; i++) {
        filt = graph->filters[i];

        if (!filt->output_count) {
            if ((ret = avfilter_config_links(filt)))
                return ret;
        }
    }

    return 0;
}

AVFilterContext *avfilter_graph_get_filter(AVFilterGraph *graph, char *name)
{
    int i;

    for (i = 0; i < graph->filter_count; i++)
        if (graph->filters[i]->name && !strcmp(name, graph->filters[i]->name))
            return graph->filters[i];

    return NULL;
}

static int query_formats(AVFilterGraph *graph, AVClass *log_ctx)
{
    int i, j, ret;
    int scaler_count = 0, resampler_count = 0;

    /* ask all the sub-filters for their supported media formats */
    for (i = 0; i < graph->filter_count; i++) {
        if (graph->filters[i]->filter->query_formats)
            graph->filters[i]->filter->query_formats(graph->filters[i]);
        else
            avfilter_default_query_formats(graph->filters[i]);
    }

    /* go through and merge as many format lists as possible */
    for (i = 0; i < graph->filter_count; i++) {
        AVFilterContext *filter = graph->filters[i];

        for (j = 0; j < filter->input_count; j++) {
            AVFilterLink *link = filter->inputs[j];
            if (link && link->in_formats != link->out_formats) {
                if (!avfilter_merge_formats(link->in_formats,
                                            link->out_formats)) {
                    AVFilterContext *convert;
                    AVFilter *filter;
                    AVFilterLink *inlink, *outlink;
                    char scale_args[256];
                    char inst_name[30];

                    /* couldn't merge format lists. auto-insert conversion filter */
                    switch (link->type) {
                    case AVMEDIA_TYPE_VIDEO:
                        snprintf(inst_name, sizeof(inst_name), "auto-inserted scaler %d",
                                 scaler_count++);
                        snprintf(scale_args, sizeof(scale_args), "0:0:%s", graph->scale_sws_opts);
                        if ((ret = avfilter_graph_create_filter(&convert,
                                                                avfilter_get_by_name("scale"),
                                                                inst_name, scale_args, NULL,
                                                                graph)) < 0)
                            return ret;
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        if (!(filter = avfilter_get_by_name("resample"))) {
                            av_log(log_ctx, AV_LOG_ERROR, "'resample' filter "
                                   "not present, cannot convert audio formats.\n");
                            return AVERROR(EINVAL);
                        }

                        snprintf(inst_name, sizeof(inst_name), "auto-inserted resampler %d",
                                 resampler_count++);
                        if ((ret = avfilter_graph_create_filter(&convert,
                                                                avfilter_get_by_name("resample"),
                                                                inst_name, NULL, NULL, graph)) < 0)
                            return ret;
                        break;
                    default:
                        return AVERROR(EINVAL);
                    }

                    if ((ret = avfilter_insert_filter(link, convert, 0, 0)) < 0)
                        return ret;

                    convert->filter->query_formats(convert);
                    inlink  = convert->inputs[0];
                    outlink = convert->outputs[0];
                    if (!avfilter_merge_formats( inlink->in_formats,  inlink->out_formats) ||
                        !avfilter_merge_formats(outlink->in_formats, outlink->out_formats)) {
                        av_log(log_ctx, AV_LOG_ERROR,
                               "Impossible to convert between the formats supported by the filter "
                               "'%s' and the filter '%s'\n", link->src->name, link->dst->name);
                        return AVERROR(EINVAL);
                    }
                }
            }
        }
    }

    return 0;
}

static void pick_format(AVFilterLink *link)
{
    if (!link || !link->in_formats)
        return;

    link->in_formats->format_count = 1;
    link->format = link->in_formats->formats[0];

    avfilter_formats_unref(&link->in_formats);
    avfilter_formats_unref(&link->out_formats);
}

static int reduce_formats_on_filter(AVFilterContext *filter)
{
    int i, j, k, ret = 0;

    for (i = 0; i < filter->input_count; i++) {
        AVFilterLink *link = filter->inputs[i];
        int         format = link->out_formats->formats[0];

        if (link->out_formats->format_count != 1)
            continue;

        for (j = 0; j < filter->output_count; j++) {
            AVFilterLink *out_link = filter->outputs[j];
            AVFilterFormats  *fmts = out_link->in_formats;

            if (link->type != out_link->type ||
                out_link->in_formats->format_count == 1)
                continue;

            for (k = 0; k < out_link->in_formats->format_count; k++)
                if (fmts->formats[k] == format) {
                    fmts->formats[0]   = format;
                    fmts->format_count = 1;
                    ret = 1;
                    break;
                }
        }
    }
    return ret;
}

static void reduce_formats(AVFilterGraph *graph)
{
    int i, reduced;

    do {
        reduced = 0;

        for (i = 0; i < graph->filter_count; i++)
            reduced |= reduce_formats_on_filter(graph->filters[i]);
    } while (reduced);
}

static void pick_formats(AVFilterGraph *graph)
{
    int i, j;

    for (i = 0; i < graph->filter_count; i++) {
        AVFilterContext *filter = graph->filters[i];

        for (j = 0; j < filter->input_count; j++)
            pick_format(filter->inputs[j]);
        for (j = 0; j < filter->output_count; j++)
            pick_format(filter->outputs[j]);
    }
}

int ff_avfilter_graph_config_formats(AVFilterGraph *graph, AVClass *log_ctx)
{
    int ret;

    /* find supported formats from sub-filters, and merge along links */
    if ((ret = query_formats(graph, log_ctx)) < 0)
        return ret;

    /* Once everything is merged, it's possible that we'll still have
     * multiple valid media format choices. We try to minimize the amount
     * of format conversion inside filters */
    reduce_formats(graph);

    pick_formats(graph);

    return 0;
}

int avfilter_graph_config(AVFilterGraph *graphctx, void *log_ctx)
{
    int ret;

    if ((ret = ff_avfilter_graph_check_validity(graphctx, log_ctx)))
        return ret;
    if ((ret = ff_avfilter_graph_config_formats(graphctx, log_ctx)))
        return ret;
    if ((ret = ff_avfilter_graph_config_links(graphctx, log_ctx)))
        return ret;

    return 0;
}

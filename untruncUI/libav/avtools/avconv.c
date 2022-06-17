/*
 * avconv main
 * Copyright (c) 2000-2011 The Libav developers
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
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <stdint.h>

#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavresample/avresample.h"
#include "libavutil/opt.h"
#include "libavutil/channel_layout.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/fifo.h"
#include "libavutil/hwcontext.h"
#include "libavutil/internal.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/dict.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avstring.h"
#include "libavutil/libm.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
#include "libavformat/os_support.h"

# include "libavfilter/avfilter.h"
# include "libavfilter/buffersrc.h"
# include "libavfilter/buffersink.h"

#if HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>
#elif HAVE_GETPROCESSTIMES
#include <windows.h>
#endif
#if HAVE_GETPROCESSMEMORYINFO
#include <windows.h>
#include <psapi.h>
#endif

#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#if HAVE_PTHREADS
#include <pthread.h>
#endif

#include <time.h>

#include "avconv.h"
#include "cmdutils.h"

#include "libavutil/avassert.h"

const char program_name[] = "avconv";
const int program_birth_year = 2000;

static FILE *vstats_file;

static int nb_frames_drop = 0;

static int want_sdp = 1;

#if HAVE_PTHREADS
/* signal to input threads that they should exit; set by the main thread */
static int transcoding_finished;
#endif

InputStream **input_streams = NULL;
int        nb_input_streams = 0;
InputFile   **input_files   = NULL;
int        nb_input_files   = 0;

OutputStream **output_streams = NULL;
int         nb_output_streams = 0;
OutputFile   **output_files   = NULL;
int         nb_output_files   = 0;

FilterGraph **filtergraphs;
int        nb_filtergraphs;

static void term_exit(void)
{
    av_log(NULL, AV_LOG_QUIET, "");
}

static volatile int received_sigterm = 0;
static volatile int received_nb_signals = 0;

static void
sigterm_handler(int sig)
{
    received_sigterm = sig;
    received_nb_signals++;
    term_exit();
}

static void term_init(void)
{
    signal(SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */
#ifdef SIGXCPU
    signal(SIGXCPU, sigterm_handler);
#endif
}

static int decode_interrupt_cb(void *ctx)
{
    return received_nb_signals > 1;
}

const AVIOInterruptCB int_cb = { decode_interrupt_cb, NULL };

static void avconv_cleanup(int ret)
{
    int i, j;

    for (i = 0; i < nb_filtergraphs; i++) {
        FilterGraph *fg = filtergraphs[i];
        avfilter_graph_free(&fg->graph);
        for (j = 0; j < fg->nb_inputs; j++) {
            while (av_fifo_size(fg->inputs[j]->frame_queue)) {
                AVFrame *frame;
                av_fifo_generic_read(fg->inputs[j]->frame_queue, &frame,
                                     sizeof(frame), NULL);
                av_frame_free(&frame);
            }
            av_fifo_free(fg->inputs[j]->frame_queue);
            av_buffer_unref(&fg->inputs[j]->hw_frames_ctx);
            av_freep(&fg->inputs[j]->name);
            av_freep(&fg->inputs[j]);
        }
        av_freep(&fg->inputs);
        for (j = 0; j < fg->nb_outputs; j++) {
            av_freep(&fg->outputs[j]->name);
            av_freep(&fg->outputs[j]->formats);
            av_freep(&fg->outputs[j]->channel_layouts);
            av_freep(&fg->outputs[j]->sample_rates);
            av_freep(&fg->outputs[j]);
        }
        av_freep(&fg->outputs);
        av_freep(&fg->graph_desc);

        av_freep(&filtergraphs[i]);
    }
    av_freep(&filtergraphs);

    /* close files */
    for (i = 0; i < nb_output_files; i++) {
        OutputFile *of = output_files[i];
        AVFormatContext *s = of->ctx;
        if (s && s->oformat && !(s->oformat->flags & AVFMT_NOFILE) && s->pb)
            avio_close(s->pb);
        avformat_free_context(s);
        av_dict_free(&of->opts);

        av_freep(&output_files[i]);
    }
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];

        for (j = 0; j < ost->nb_bitstream_filters; j++)
            av_bsf_free(&ost->bsf_ctx[j]);
        av_freep(&ost->bsf_ctx);

        av_frame_free(&ost->filtered_frame);

        av_parser_close(ost->parser);
        avcodec_free_context(&ost->parser_avctx);

        av_freep(&ost->forced_keyframes);
        av_freep(&ost->avfilter);
        av_freep(&ost->logfile_prefix);

        avcodec_free_context(&ost->enc_ctx);

        if (ost->muxing_queue) {
            while (av_fifo_size(ost->muxing_queue)) {
                AVPacket pkt;
                av_fifo_generic_read(ost->muxing_queue, &pkt, sizeof(pkt), NULL);
                av_packet_unref(&pkt);
            }
            av_fifo_free(ost->muxing_queue);
        }
        av_freep(&output_streams[i]);
    }
    for (i = 0; i < nb_input_files; i++) {
        avformat_close_input(&input_files[i]->ctx);
        av_freep(&input_files[i]);
    }
    for (i = 0; i < nb_input_streams; i++) {
        InputStream *ist = input_streams[i];

        av_frame_free(&ist->decoded_frame);
        av_frame_free(&ist->filter_frame);
        av_dict_free(&ist->decoder_opts);
        av_freep(&ist->filters);
        av_freep(&ist->hwaccel_device);

        avcodec_free_context(&ist->dec_ctx);

        av_freep(&input_streams[i]);
    }

    if (vstats_file)
        fclose(vstats_file);
    av_free(vstats_filename);

    av_freep(&input_streams);
    av_freep(&input_files);
    av_freep(&output_streams);
    av_freep(&output_files);

    uninit_opts();

    avformat_network_deinit();

    if (received_sigterm) {
        av_log(NULL, AV_LOG_INFO, "Received signal %d: terminating.\n",
               (int) received_sigterm);
        exit (255);
    }
}

void assert_avoptions(AVDictionary *m)
{
    AVDictionaryEntry *t;
    if ((t = av_dict_get(m, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_FATAL, "Option %s not found.\n", t->key);
        exit_program(1);
    }
}

static void abort_codec_experimental(AVCodec *c, int encoder)
{
    const char *codec_string = encoder ? "encoder" : "decoder";
    AVCodec *codec;
    av_log(NULL, AV_LOG_FATAL, "%s '%s' is experimental and might produce bad "
            "results.\nAdd '-strict experimental' if you want to use it.\n",
            codec_string, c->name);
    codec = encoder ? avcodec_find_encoder(c->id) : avcodec_find_decoder(c->id);
    if (!(codec->capabilities & AV_CODEC_CAP_EXPERIMENTAL))
        av_log(NULL, AV_LOG_FATAL, "Or use the non experimental %s '%s'.\n",
               codec_string, codec->name);
    exit_program(1);
}

static void write_packet(OutputFile *of, AVPacket *pkt, OutputStream *ost)
{
    AVFormatContext *s = of->ctx;
    AVStream *st = ost->st;
    int ret;

    if (!of->header_written) {
        AVPacket tmp_pkt = {0};
        /* the muxer is not initialized yet, buffer the packet */
        if (!av_fifo_space(ost->muxing_queue)) {
            int new_size = FFMIN(2 * av_fifo_size(ost->muxing_queue),
                                 ost->max_muxing_queue_size);
            if (new_size <= av_fifo_size(ost->muxing_queue)) {
                av_log(NULL, AV_LOG_ERROR,
                       "Too many packets buffered for output stream %d:%d.\n",
                       ost->file_index, ost->st->index);
                exit_program(1);
            }
            ret = av_fifo_realloc2(ost->muxing_queue, new_size);
            if (ret < 0)
                exit_program(1);
        }
        ret = av_packet_ref(&tmp_pkt, pkt);
        if (ret < 0)
            exit_program(1);
        av_fifo_generic_write(ost->muxing_queue, &tmp_pkt, sizeof(tmp_pkt), NULL);
        av_packet_unref(pkt);
        return;
    }

    /*
     * Audio encoders may split the packets --  #frames in != #packets out.
     * But there is no reordering, so we can limit the number of output packets
     * by simply dropping them here.
     * Counting encoded video frames needs to be done separately because of
     * reordering, see do_video_out()
     */
    if (!(st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && ost->encoding_needed)) {
        if (ost->frame_number >= ost->max_frames) {
            av_packet_unref(pkt);
            return;
        }
        ost->frame_number++;
    }
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        uint8_t *sd = av_packet_get_side_data(pkt, AV_PKT_DATA_QUALITY_FACTOR,
                                              NULL);
        ost->quality = sd ? *(int *)sd : -1;

        if (ost->frame_rate.num) {
            pkt->duration = av_rescale_q(1, av_inv_q(ost->frame_rate),
                                         ost->mux_timebase);
        }
    }

    av_packet_rescale_ts(pkt, ost->mux_timebase, ost->st->time_base);

    if (!(s->oformat->flags & AVFMT_NOTIMESTAMPS) &&
        ost->last_mux_dts != AV_NOPTS_VALUE &&
        pkt->dts < ost->last_mux_dts + !(s->oformat->flags & AVFMT_TS_NONSTRICT)) {
        av_log(NULL, AV_LOG_WARNING, "Non-monotonous DTS in output stream "
               "%d:%d; previous: %"PRId64", current: %"PRId64"; ",
               ost->file_index, ost->st->index, ost->last_mux_dts, pkt->dts);
        if (exit_on_error) {
            av_log(NULL, AV_LOG_FATAL, "aborting.\n");
            exit_program(1);
        }
        av_log(NULL, AV_LOG_WARNING, "changing to %"PRId64". This may result "
               "in incorrect timestamps in the output file.\n",
               ost->last_mux_dts + 1);
        pkt->dts = ost->last_mux_dts + 1;
        if (pkt->pts != AV_NOPTS_VALUE)
            pkt->pts = FFMAX(pkt->pts, pkt->dts);
    }
    ost->last_mux_dts = pkt->dts;

    ost->data_size += pkt->size;
    ost->packets_written++;

    pkt->stream_index = ost->index;

    ret = av_interleaved_write_frame(s, pkt);
    if (ret < 0) {
        print_error("av_interleaved_write_frame()", ret);
        exit_program(1);
    }
}

static void output_packet(OutputFile *of, AVPacket *pkt,
                          OutputStream *ost, int eof)
{
    int ret = 0;

    /* apply the output bitstream filters, if any */
    if (ost->nb_bitstream_filters) {
        int idx;

        ret = av_bsf_send_packet(ost->bsf_ctx[0], eof ? NULL : pkt);
        if (ret < 0)
            goto finish;

        eof = 0;
        idx = 1;
        while (idx) {
            /* get a packet from the previous filter up the chain */
            ret = av_bsf_receive_packet(ost->bsf_ctx[idx - 1], pkt);
            if (ret == AVERROR(EAGAIN)) {
                ret = 0;
                idx--;
                continue;
            } else if (ret == AVERROR_EOF) {
                eof = 1;
            } else if (ret < 0)
                goto finish;

            /* send it to the next filter down the chain or to the muxer */
            if (idx < ost->nb_bitstream_filters) {
                ret = av_bsf_send_packet(ost->bsf_ctx[idx], eof ? NULL : pkt);
                if (ret < 0)
                    goto finish;
                idx++;
                eof = 0;
            } else if (eof)
                goto finish;
            else
                write_packet(of, pkt, ost);
        }
    } else if (!eof)
        write_packet(of, pkt, ost);

finish:
    if (ret < 0 && ret != AVERROR_EOF) {
        av_log(NULL, AV_LOG_FATAL, "Error applying bitstream filters to an output "
               "packet for stream #%d:%d.\n", ost->file_index, ost->index);
        exit_program(1);
    }
}

static int check_recording_time(OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];

    if (of->recording_time != INT64_MAX &&
        av_compare_ts(ost->sync_opts - ost->first_pts, ost->enc_ctx->time_base, of->recording_time,
                      AV_TIME_BASE_Q) >= 0) {
        ost->finished = 1;
        return 0;
    }
    return 1;
}

static void do_audio_out(OutputFile *of, OutputStream *ost,
                         AVFrame *frame)
{
    AVCodecContext *enc = ost->enc_ctx;
    AVPacket pkt;
    int ret;

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    if (frame->pts == AV_NOPTS_VALUE || audio_sync_method < 0)
        frame->pts = ost->sync_opts;
    ost->sync_opts = frame->pts + frame->nb_samples;

    ost->samples_encoded += frame->nb_samples;
    ost->frames_encoded++;

    ret = avcodec_send_frame(enc, frame);
    if (ret < 0)
        goto error;

    while (1) {
        ret = avcodec_receive_packet(enc, &pkt);
        if (ret == AVERROR(EAGAIN))
            break;
        if (ret < 0)
            goto error;

        output_packet(of, &pkt, ost, 0);
    }

    return;
error:
    av_log(NULL, AV_LOG_FATAL, "Audio encoding failed\n");
    exit_program(1);
}

static void do_subtitle_out(OutputFile *of,
                            OutputStream *ost,
                            InputStream *ist,
                            AVSubtitle *sub,
                            int64_t pts)
{
    static uint8_t *subtitle_out = NULL;
    int subtitle_out_max_size = 1024 * 1024;
    int subtitle_out_size, nb, i;
    AVCodecContext *enc;
    AVPacket pkt;

    if (pts == AV_NOPTS_VALUE) {
        av_log(NULL, AV_LOG_ERROR, "Subtitle packets must have a pts\n");
        if (exit_on_error)
            exit_program(1);
        return;
    }

    enc = ost->enc_ctx;

    if (!subtitle_out) {
        subtitle_out = av_malloc(subtitle_out_max_size);
    }

    /* Note: DVB subtitle need one packet to draw them and one other
       packet to clear them */
    /* XXX: signal it in the codec context ? */
    if (enc->codec_id == AV_CODEC_ID_DVB_SUBTITLE)
        nb = 2;
    else
        nb = 1;

    for (i = 0; i < nb; i++) {
        ost->sync_opts = av_rescale_q(pts, ist->st->time_base, enc->time_base);
        if (!check_recording_time(ost))
            return;

        sub->pts = av_rescale_q(pts, ist->st->time_base, AV_TIME_BASE_Q);
        // start_display_time is required to be 0
        sub->pts               += av_rescale_q(sub->start_display_time, (AVRational){ 1, 1000 }, AV_TIME_BASE_Q);
        sub->end_display_time  -= sub->start_display_time;
        sub->start_display_time = 0;

        ost->frames_encoded++;

        subtitle_out_size = avcodec_encode_subtitle(enc, subtitle_out,
                                                    subtitle_out_max_size, sub);
        if (subtitle_out_size < 0) {
            av_log(NULL, AV_LOG_FATAL, "Subtitle encoding failed\n");
            exit_program(1);
        }

        av_init_packet(&pkt);
        pkt.data = subtitle_out;
        pkt.size = subtitle_out_size;
        pkt.pts  = av_rescale_q(sub->pts, AV_TIME_BASE_Q, ost->mux_timebase);
        if (enc->codec_id == AV_CODEC_ID_DVB_SUBTITLE) {
            /* XXX: the pts correction is handled here. Maybe handling
               it in the codec would be better */
            if (i == 0)
                pkt.pts += 90 * sub->start_display_time;
            else
                pkt.pts += 90 * sub->end_display_time;
        }
        output_packet(of, &pkt, ost, 0);
    }
}

static void do_video_out(OutputFile *of,
                         OutputStream *ost,
                         AVFrame *in_picture,
                         int *frame_size)
{
    int ret, format_video_sync;
    AVPacket pkt;
    AVCodecContext *enc = ost->enc_ctx;

    *frame_size = 0;

    format_video_sync = video_sync_method;
    if (format_video_sync == VSYNC_AUTO)
        format_video_sync = (of->ctx->oformat->flags & AVFMT_NOTIMESTAMPS) ? VSYNC_PASSTHROUGH :
                            (of->ctx->oformat->flags & AVFMT_VARIABLE_FPS) ? VSYNC_VFR : VSYNC_CFR;
    if (format_video_sync != VSYNC_PASSTHROUGH &&
        ost->frame_number &&
        in_picture->pts != AV_NOPTS_VALUE &&
        in_picture->pts < ost->sync_opts) {
        nb_frames_drop++;
        av_log(NULL, AV_LOG_WARNING,
               "*** dropping frame %d from stream %d at ts %"PRId64"\n",
               ost->frame_number, ost->st->index, in_picture->pts);
        return;
    }

    if (in_picture->pts == AV_NOPTS_VALUE)
        in_picture->pts = ost->sync_opts;
    ost->sync_opts = in_picture->pts;


    if (!ost->frame_number)
        ost->first_pts = in_picture->pts;

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    if (ost->frame_number >= ost->max_frames)
        return;

    if (enc->flags & (AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME) &&
        ost->top_field_first >= 0)
        in_picture->top_field_first = !!ost->top_field_first;

    in_picture->quality = enc->global_quality;
    in_picture->pict_type = 0;
    if (ost->forced_kf_index < ost->forced_kf_count &&
        in_picture->pts >= ost->forced_kf_pts[ost->forced_kf_index]) {
        in_picture->pict_type = AV_PICTURE_TYPE_I;
        ost->forced_kf_index++;
    }

    ost->frames_encoded++;

    ret = avcodec_send_frame(enc, in_picture);
    if (ret < 0)
        goto error;

    /*
     * For video, there may be reordering, so we can't throw away frames on
     * encoder flush, we need to limit them here, before they go into encoder.
     */
    ost->frame_number++;

    while (1) {
        ret = avcodec_receive_packet(enc, &pkt);
        if (ret == AVERROR(EAGAIN))
            break;
        if (ret < 0)
            goto error;

        output_packet(of, &pkt, ost, 0);
        *frame_size = pkt.size;

        /* if two pass, output log */
        if (ost->logfile && enc->stats_out) {
            fprintf(ost->logfile, "%s", enc->stats_out);
        }

        ost->sync_opts++;
    }

    return;
error:
    av_assert0(ret != AVERROR(EAGAIN) && ret != AVERROR_EOF);
    av_log(NULL, AV_LOG_FATAL, "Video encoding failed\n");
    exit_program(1);
}

#if FF_API_CODED_FRAME && FF_API_ERROR_FRAME
static double psnr(double d)
{
    return -10.0 * log(d) / log(10.0);
}
#endif

static void do_video_stats(OutputStream *ost, int frame_size)
{
    AVCodecContext *enc;
    int frame_number;
    double ti1, bitrate, avg_bitrate;

    /* this is executed just the first time do_video_stats is called */
    if (!vstats_file) {
        vstats_file = fopen(vstats_filename, "w");
        if (!vstats_file) {
            perror("fopen");
            exit_program(1);
        }
    }

    enc = ost->enc_ctx;
    if (enc->codec_type == AVMEDIA_TYPE_VIDEO) {
        frame_number = ost->frame_number;
        fprintf(vstats_file, "frame= %5d q= %2.1f ", frame_number,
                ost->quality / (float)FF_QP2LAMBDA);

#if FF_API_CODED_FRAME && FF_API_ERROR_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
        if (enc->flags & AV_CODEC_FLAG_PSNR)
            fprintf(vstats_file, "PSNR= %6.2f ", psnr(enc->coded_frame->error[0] / (enc->width * enc->height * 255.0 * 255.0)));
FF_ENABLE_DEPRECATION_WARNINGS
#endif

        fprintf(vstats_file,"f_size= %6d ", frame_size);
        /* compute pts value */
        ti1 = ost->sync_opts * av_q2d(enc->time_base);
        if (ti1 < 0.01)
            ti1 = 0.01;

        bitrate     = (frame_size * 8) / av_q2d(enc->time_base) / 1000.0;
        avg_bitrate = (double)(ost->data_size * 8) / ti1 / 1000.0;
        fprintf(vstats_file, "s_size= %8.0fkB time= %0.3f br= %7.1fkbits/s avg_br= %7.1fkbits/s ",
               (double)ost->data_size / 1024, ti1, bitrate, avg_bitrate);
#if FF_API_CODED_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
        fprintf(vstats_file, "type= %c\n", av_get_picture_type_char(enc->coded_frame->pict_type));
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    }
}

static int init_output_stream(OutputStream *ost, char *error, int error_len);

/*
 * Read one frame for lavfi output for ost and encode it.
 */
static int poll_filter(OutputStream *ost)
{
    OutputFile    *of = output_files[ost->file_index];
    AVFrame *filtered_frame = NULL;
    int frame_size, ret;

    if (!ost->filtered_frame && !(ost->filtered_frame = av_frame_alloc())) {
        return AVERROR(ENOMEM);
    }
    filtered_frame = ost->filtered_frame;

    if (!ost->initialized) {
        char error[1024];
        ret = init_output_stream(ost, error, sizeof(error));
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error initializing output stream %d:%d -- %s\n",
                   ost->file_index, ost->index, error);
            exit_program(1);
        }
    }

    if (ost->enc->type == AVMEDIA_TYPE_AUDIO &&
        !(ost->enc->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
        ret = av_buffersink_get_samples(ost->filter->filter, filtered_frame,
                                         ost->enc_ctx->frame_size);
    else
        ret = av_buffersink_get_frame(ost->filter->filter, filtered_frame);

    if (ret < 0)
        return ret;

    if (filtered_frame->pts != AV_NOPTS_VALUE) {
        int64_t start_time = (of->start_time == AV_NOPTS_VALUE) ? 0 : of->start_time;
        filtered_frame->pts = av_rescale_q(filtered_frame->pts,
                                           ost->filter->filter->inputs[0]->time_base,
                                           ost->enc_ctx->time_base) -
                              av_rescale_q(start_time,
                                           AV_TIME_BASE_Q,
                                           ost->enc_ctx->time_base);
    }

    switch (ost->filter->filter->inputs[0]->type) {
    case AVMEDIA_TYPE_VIDEO:
        if (!ost->frame_aspect_ratio)
            ost->enc_ctx->sample_aspect_ratio = filtered_frame->sample_aspect_ratio;

        do_video_out(of, ost, filtered_frame, &frame_size);
        if (vstats_filename && frame_size)
            do_video_stats(ost, frame_size);
        break;
    case AVMEDIA_TYPE_AUDIO:
        do_audio_out(of, ost, filtered_frame);
        break;
    default:
        // TODO support subtitle filters
        av_assert0(0);
    }

    av_frame_unref(filtered_frame);

    return 0;
}

static void finish_output_stream(OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];
    int i;

    ost->finished = 1;

    if (of->shortest) {
        for (i = 0; i < of->ctx->nb_streams; i++)
            output_streams[of->ost_index + i]->finished = 1;
    }
}

/*
 * Read as many frames from possible from lavfi and encode them.
 *
 * Always read from the active stream with the lowest timestamp. If no frames
 * are available for it then return EAGAIN and wait for more input. This way we
 * can use lavfi sources that generate unlimited amount of frames without memory
 * usage exploding.
 */
static int poll_filters(void)
{
    int i, ret = 0;

    while (ret >= 0 && !received_sigterm) {
        OutputStream *ost = NULL;
        int64_t min_pts = INT64_MAX;

        /* choose output stream with the lowest timestamp */
        for (i = 0; i < nb_output_streams; i++) {
            int64_t pts = output_streams[i]->sync_opts;

            if (output_streams[i]->filter && !output_streams[i]->filter->graph->graph &&
                !output_streams[i]->filter->graph->nb_inputs) {
                ret = configure_filtergraph(output_streams[i]->filter->graph);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error reinitializing filters!\n");
                    return ret;
                }
            }

            if (!output_streams[i]->filter || output_streams[i]->finished ||
                !output_streams[i]->filter->graph->graph)
                continue;

            pts = av_rescale_q(pts, output_streams[i]->enc_ctx->time_base,
                               AV_TIME_BASE_Q);
            if (pts < min_pts) {
                min_pts = pts;
                ost = output_streams[i];
            }
        }

        if (!ost)
            break;

        ret = poll_filter(ost);

        if (ret == AVERROR_EOF) {
            finish_output_stream(ost);
            ret = 0;
        } else if (ret == AVERROR(EAGAIN))
            return 0;
    }

    return ret;
}

static void print_final_stats(int64_t total_size)
{
    uint64_t video_size = 0, audio_size = 0, extra_size = 0, other_size = 0;
    uint64_t data_size = 0;
    float percent = -1.0;
    int i, j;

    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        switch (ost->enc_ctx->codec_type) {
            case AVMEDIA_TYPE_VIDEO: video_size += ost->data_size; break;
            case AVMEDIA_TYPE_AUDIO: audio_size += ost->data_size; break;
            default:                 other_size += ost->data_size; break;
        }
        extra_size += ost->enc_ctx->extradata_size;
        data_size  += ost->data_size;
    }

    if (data_size && total_size >= data_size)
        percent = 100.0 * (total_size - data_size) / data_size;

    av_log(NULL, AV_LOG_INFO, "\n");
    av_log(NULL, AV_LOG_INFO, "video:%1.0fkB audio:%1.0fkB other streams:%1.0fkB global headers:%1.0fkB muxing overhead: ",
           video_size / 1024.0,
           audio_size / 1024.0,
           other_size / 1024.0,
           extra_size / 1024.0);
    if (percent >= 0.0)
        av_log(NULL, AV_LOG_INFO, "%f%%", percent);
    else
        av_log(NULL, AV_LOG_INFO, "unknown");
    av_log(NULL, AV_LOG_INFO, "\n");

    /* print verbose per-stream stats */
    for (i = 0; i < nb_input_files; i++) {
        InputFile *f = input_files[i];
        uint64_t total_packets = 0, total_size = 0;

        av_log(NULL, AV_LOG_VERBOSE, "Input file #%d (%s):\n",
               i, f->ctx->filename);

        for (j = 0; j < f->nb_streams; j++) {
            InputStream *ist = input_streams[f->ist_index + j];
            enum AVMediaType type = ist->dec_ctx->codec_type;

            total_size    += ist->data_size;
            total_packets += ist->nb_packets;

            av_log(NULL, AV_LOG_VERBOSE, "  Input stream #%d:%d (%s): ",
                   i, j, media_type_string(type));
            av_log(NULL, AV_LOG_VERBOSE, "%"PRIu64" packets read (%"PRIu64" bytes); ",
                   ist->nb_packets, ist->data_size);

            if (ist->decoding_needed) {
                av_log(NULL, AV_LOG_VERBOSE, "%"PRIu64" frames decoded",
                       ist->frames_decoded);
                if (type == AVMEDIA_TYPE_AUDIO)
                    av_log(NULL, AV_LOG_VERBOSE, " (%"PRIu64" samples)", ist->samples_decoded);
                av_log(NULL, AV_LOG_VERBOSE, "; ");
            }

            av_log(NULL, AV_LOG_VERBOSE, "\n");
        }

        av_log(NULL, AV_LOG_VERBOSE, "  Total: %"PRIu64" packets (%"PRIu64" bytes) demuxed\n",
               total_packets, total_size);
    }

    for (i = 0; i < nb_output_files; i++) {
        OutputFile *of = output_files[i];
        uint64_t total_packets = 0, total_size = 0;

        av_log(NULL, AV_LOG_VERBOSE, "Output file #%d (%s):\n",
               i, of->ctx->filename);

        for (j = 0; j < of->ctx->nb_streams; j++) {
            OutputStream *ost = output_streams[of->ost_index + j];
            enum AVMediaType type = ost->enc_ctx->codec_type;

            total_size    += ost->data_size;
            total_packets += ost->packets_written;

            av_log(NULL, AV_LOG_VERBOSE, "  Output stream #%d:%d (%s): ",
                   i, j, media_type_string(type));
            if (ost->encoding_needed) {
                av_log(NULL, AV_LOG_VERBOSE, "%"PRIu64" frames encoded",
                       ost->frames_encoded);
                if (type == AVMEDIA_TYPE_AUDIO)
                    av_log(NULL, AV_LOG_VERBOSE, " (%"PRIu64" samples)", ost->samples_encoded);
                av_log(NULL, AV_LOG_VERBOSE, "; ");
            }

            av_log(NULL, AV_LOG_VERBOSE, "%"PRIu64" packets muxed (%"PRIu64" bytes); ",
                   ost->packets_written, ost->data_size);

            av_log(NULL, AV_LOG_VERBOSE, "\n");
        }

        av_log(NULL, AV_LOG_VERBOSE, "  Total: %"PRIu64" packets (%"PRIu64" bytes) muxed\n",
               total_packets, total_size);
    }
}

static void print_report(int is_last_report, int64_t timer_start)
{
    char buf[1024];
    OutputStream *ost;
    AVFormatContext *oc;
    int64_t total_size = 0;
    AVCodecContext *enc;
    int frame_number, vid, i;
    double bitrate, ti1, pts;
    static int64_t last_time = -1;
    static int qp_histogram[52];

    if (!print_stats && !is_last_report)
        return;

    if (!is_last_report) {
        int64_t cur_time;
        /* display the report every 0.5 seconds */
        cur_time = av_gettime_relative();
        if (last_time == -1) {
            last_time = cur_time;
            return;
        }
        if ((cur_time - last_time) < 500000)
            return;
        last_time = cur_time;
    }


    oc = output_files[0]->ctx;
    if (oc->pb) {
        total_size = avio_size(oc->pb);
        if (total_size <= 0) // FIXME improve avio_size() so it works with non seekable output too
            total_size = avio_tell(oc->pb);
        if (total_size < 0) {
            char errbuf[128];
            av_strerror(total_size, errbuf, sizeof(errbuf));
            av_log(NULL, AV_LOG_VERBOSE, "Bitrate not available, "
                   "avio_tell() failed: %s\n", errbuf);
            total_size = 0;
        }
    }

    buf[0] = '\0';
    ti1 = 1e10;
    vid = 0;
    for (i = 0; i < nb_output_streams; i++) {
        float q = -1;
        ost = output_streams[i];
        enc = ost->enc_ctx;
        if (!ost->stream_copy)
            q = ost->quality / (float) FF_QP2LAMBDA;

        if (vid && enc->codec_type == AVMEDIA_TYPE_VIDEO) {
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "q=%2.1f ", q);
        }
        if (!vid && enc->codec_type == AVMEDIA_TYPE_VIDEO) {
            float t = (av_gettime_relative() - timer_start) / 1000000.0;

            frame_number = ost->frame_number;
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "frame=%5d fps=%3d q=%3.1f ",
                     frame_number, (t > 1) ? (int)(frame_number / t + 0.5) : 0, q);
            if (is_last_report)
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "L");
            if (qp_hist) {
                int j;
                int qp = lrintf(q);
                if (qp >= 0 && qp < FF_ARRAY_ELEMS(qp_histogram))
                    qp_histogram[qp]++;
                for (j = 0; j < 32; j++)
                    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%X", (int)lrintf(log2(qp_histogram[j] + 1)));
            }

#if FF_API_CODED_FRAME && FF_API_ERROR_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
            if (enc->flags & AV_CODEC_FLAG_PSNR) {
                int j;
                double error, error_sum = 0;
                double scale, scale_sum = 0;
                char type[3] = { 'Y','U','V' };
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "PSNR=");
                for (j = 0; j < 3; j++) {
                    if (is_last_report) {
                        error = enc->error[j];
                        scale = enc->width * enc->height * 255.0 * 255.0 * frame_number;
                    } else {
                        error = enc->coded_frame->error[j];
                        scale = enc->width * enc->height * 255.0 * 255.0;
                    }
                    if (j)
                        scale /= 4;
                    error_sum += error;
                    scale_sum += scale;
                    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%c:%2.2f ", type[j], psnr(error / scale));
                }
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "*:%2.2f ", psnr(error_sum / scale_sum));
            }
FF_ENABLE_DEPRECATION_WARNINGS
#endif
            vid = 1;
        }
        /* compute min output value */
        pts = (double)ost->last_mux_dts * av_q2d(ost->st->time_base);
        if ((pts < ti1) && (pts > 0))
            ti1 = pts;
    }
    if (ti1 < 0.01)
        ti1 = 0.01;

    bitrate = (double)(total_size * 8) / ti1 / 1000.0;

    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
            "size=%8.0fkB time=%0.2f bitrate=%6.1fkbits/s",
            (double)total_size / 1024, ti1, bitrate);

    if (nb_frames_drop)
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " drop=%d",
                 nb_frames_drop);

    av_log(NULL, AV_LOG_INFO, "%s    \r", buf);

    fflush(stderr);

    if (is_last_report)
        print_final_stats(total_size);

}

static void flush_encoders(void)
{
    int i, ret;

    for (i = 0; i < nb_output_streams; i++) {
        OutputStream   *ost = output_streams[i];
        AVCodecContext *enc = ost->enc_ctx;
        OutputFile      *of = output_files[ost->file_index];
        int stop_encoding = 0;

        if (!ost->encoding_needed)
            continue;

        if (enc->codec_type == AVMEDIA_TYPE_AUDIO && enc->frame_size <= 1)
            continue;

        if (enc->codec_type != AVMEDIA_TYPE_VIDEO && enc->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;

        avcodec_send_frame(enc, NULL);

        for (;;) {
            const char *desc = NULL;

            switch (enc->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                desc   = "Audio";
                break;
            case AVMEDIA_TYPE_VIDEO:
                desc   = "Video";
                break;
            default:
                av_assert0(0);
            }

            if (1) {
                AVPacket pkt;
                av_init_packet(&pkt);
                pkt.data = NULL;
                pkt.size = 0;

                ret = avcodec_receive_packet(enc, &pkt);
                if (ret < 0 && ret != AVERROR_EOF) {
                    av_log(NULL, AV_LOG_FATAL, "%s encoding failed\n", desc);
                    exit_program(1);
                }
                if (ost->logfile && enc->stats_out) {
                    fprintf(ost->logfile, "%s", enc->stats_out);
                }
                output_packet(of, &pkt, ost, ret == AVERROR_EOF);
                if (ret == AVERROR_EOF) {
                    stop_encoding = 1;
                    break;
                }
            }

            if (stop_encoding)
                break;
        }
    }
}

/*
 * Check whether a packet from ist should be written into ost at this time
 */
static int check_output_constraints(InputStream *ist, OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];
    int ist_index  = input_files[ist->file_index]->ist_index + ist->st->index;

    if (ost->source_index != ist_index)
        return 0;

    if (of->start_time != AV_NOPTS_VALUE && ist->last_dts < of->start_time)
        return 0;

    return 1;
}

static void do_streamcopy(InputStream *ist, OutputStream *ost, const AVPacket *pkt)
{
    OutputFile *of = output_files[ost->file_index];
    InputFile   *f = input_files [ist->file_index];
    int64_t start_time = (of->start_time == AV_NOPTS_VALUE) ? 0 : of->start_time;
    int64_t ost_tb_start_time = av_rescale_q(start_time, AV_TIME_BASE_Q, ost->mux_timebase);
    AVPacket opkt = { 0 };

    av_init_packet(&opkt);

    // EOF: flush output bitstream filters.
    if (!pkt) {
        output_packet(of, &opkt, ost, 1);
        return;
    }

    if ((!ost->frame_number && !(pkt->flags & AV_PKT_FLAG_KEY)) &&
        !ost->copy_initial_nonkeyframes)
        return;

    if (of->recording_time != INT64_MAX &&
        ist->last_dts >= of->recording_time + start_time) {
        ost->finished = 1;
        return;
    }

    if (f->recording_time != INT64_MAX) {
        start_time = f->ctx->start_time;
        if (f->start_time != AV_NOPTS_VALUE)
            start_time += f->start_time;
        if (ist->last_dts >= f->recording_time + start_time) {
            ost->finished = 1;
            return;
        }
    }

    /* force the input stream PTS */
    if (ost->enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
        ost->sync_opts++;

    if (pkt->pts != AV_NOPTS_VALUE)
        opkt.pts = av_rescale_q(pkt->pts, ist->st->time_base, ost->mux_timebase) - ost_tb_start_time;
    else
        opkt.pts = AV_NOPTS_VALUE;

    if (pkt->dts == AV_NOPTS_VALUE)
        opkt.dts = av_rescale_q(ist->last_dts, AV_TIME_BASE_Q, ost->mux_timebase);
    else
        opkt.dts = av_rescale_q(pkt->dts, ist->st->time_base, ost->mux_timebase);
    opkt.dts -= ost_tb_start_time;

    opkt.duration = av_rescale_q(pkt->duration, ist->st->time_base, ost->mux_timebase);
    opkt.flags    = pkt->flags;

    // FIXME remove the following 2 lines they shall be replaced by the bitstream filters
    if (  ost->enc_ctx->codec_id != AV_CODEC_ID_H264
       && ost->enc_ctx->codec_id != AV_CODEC_ID_MPEG1VIDEO
       && ost->enc_ctx->codec_id != AV_CODEC_ID_MPEG2VIDEO
       && ost->enc_ctx->codec_id != AV_CODEC_ID_VC1
       ) {
        if (av_parser_change(ost->parser, ost->parser_avctx,
                             &opkt.data, &opkt.size,
                             pkt->data, pkt->size,
                             pkt->flags & AV_PKT_FLAG_KEY)) {
            opkt.buf = av_buffer_create(opkt.data, opkt.size, av_buffer_default_free, NULL, 0);
            if (!opkt.buf)
                exit_program(1);
        }
    } else {
        opkt.data = pkt->data;
        opkt.size = pkt->size;
    }

    output_packet(of, &opkt, ost, 0);
}

static int ifilter_send_frame(InputFilter *ifilter, AVFrame *frame)
{
    FilterGraph *fg = ifilter->graph;
    int need_reinit, ret, i;

    /* determine if the parameters for this input changed */
    need_reinit = ifilter->format != frame->format;
    if (!!ifilter->hw_frames_ctx != !!frame->hw_frames_ctx ||
        (ifilter->hw_frames_ctx && ifilter->hw_frames_ctx->data != frame->hw_frames_ctx->data))
        need_reinit = 1;

    switch (ifilter->ist->st->codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        need_reinit |= ifilter->sample_rate    != frame->sample_rate ||
                       ifilter->channel_layout != frame->channel_layout;
        break;
    case AVMEDIA_TYPE_VIDEO:
        need_reinit |= ifilter->width  != frame->width ||
                       ifilter->height != frame->height;
        break;
    }

    if (need_reinit) {
        ret = ifilter_parameters_from_frame(ifilter, frame);
        if (ret < 0)
            return ret;
    }

    /* (re)init the graph if possible, otherwise buffer the frame and return */
    if (need_reinit || !fg->graph) {
        for (i = 0; i < fg->nb_inputs; i++) {
            if (fg->inputs[i]->format < 0) {
                AVFrame *tmp = av_frame_clone(frame);
                if (!tmp)
                    return AVERROR(ENOMEM);
                av_frame_unref(frame);

                if (!av_fifo_space(ifilter->frame_queue)) {
                    ret = av_fifo_realloc2(ifilter->frame_queue, 2 * av_fifo_size(ifilter->frame_queue));
                    if (ret < 0)
                        return ret;
                }
                av_fifo_generic_write(ifilter->frame_queue, &tmp, sizeof(tmp), NULL);
                return 0;
            }
        }

        ret = poll_filters();
        if (ret < 0 && ret != AVERROR_EOF) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));

            av_log(NULL, AV_LOG_ERROR, "Error while filtering: %s\n", errbuf);
            return ret;
        }

        ret = configure_filtergraph(fg);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error reinitializing filters!\n");
            return ret;
        }
    }

    ret = av_buffersrc_add_frame(ifilter->filter, frame);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while filtering\n");
        return ret;
    }

    return 0;
}

static int ifilter_send_eof(InputFilter *ifilter)
{
    int i, j, ret;

    ifilter->eof = 1;

    if (ifilter->filter) {
        ret = av_buffersrc_add_frame(ifilter->filter, NULL);
        if (ret < 0)
            return ret;
    } else {
        // the filtergraph was never configured
        FilterGraph *fg = ifilter->graph;
        for (i = 0; i < fg->nb_inputs; i++)
            if (!fg->inputs[i]->eof)
                break;
        if (i == fg->nb_inputs) {
            // All the input streams have finished without the filtergraph
            // ever being configured.
            // Mark the output streams as finished.
            for (j = 0; j < fg->nb_outputs; j++)
                finish_output_stream(fg->outputs[j]->ost);
        }
    }

    return 0;
}

// This does not quite work like avcodec_decode_audio4/avcodec_decode_video2.
// There is the following difference: if you got a frame, you must call
// it again with pkt=NULL. pkt==NULL is treated differently from pkt.size==0
// (pkt==NULL means get more output, pkt.size==0 is a flush/drain packet)
static int decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt)
{
    int ret;

    *got_frame = 0;

    if (pkt) {
        ret = avcodec_send_packet(avctx, pkt);
        // In particular, we don't expect AVERROR(EAGAIN), because we read all
        // decoded frames with avcodec_receive_frame() until done.
        if (ret < 0)
            return ret == AVERROR_EOF ? 0 : ret;
    }

    ret = avcodec_receive_frame(avctx, frame);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        return ret;
    if (ret >= 0)
        *got_frame = 1;

    return 0;
}

int guess_input_channel_layout(InputStream *ist)
{
    AVCodecContext *dec = ist->dec_ctx;

    if (!dec->channel_layout) {
        char layout_name[256];

        dec->channel_layout = av_get_default_channel_layout(dec->channels);
        if (!dec->channel_layout)
            return 0;
        av_get_channel_layout_string(layout_name, sizeof(layout_name),
                                     dec->channels, dec->channel_layout);
        av_log(NULL, AV_LOG_WARNING, "Guessed Channel Layout for Input Stream "
               "#%d.%d : %s\n", ist->file_index, ist->st->index, layout_name);
    }
    return 1;
}

static int decode_audio(InputStream *ist, AVPacket *pkt, int *got_output,
                        int *decode_failed)
{
    AVFrame *decoded_frame, *f;
    AVCodecContext *avctx = ist->dec_ctx;
    int i, ret, err = 0;

    if (!ist->decoded_frame && !(ist->decoded_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    if (!ist->filter_frame && !(ist->filter_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    decoded_frame = ist->decoded_frame;

    ret = decode(avctx, decoded_frame, got_output, pkt);
    if (ret < 0)
        *decode_failed = 1;
    if (!*got_output || ret < 0)
        return ret;

    ist->samples_decoded += decoded_frame->nb_samples;
    ist->frames_decoded++;

    /* if the decoder provides a pts, use it instead of the last packet pts.
       the decoder could be delaying output by a packet or more. */
    if (decoded_frame->pts != AV_NOPTS_VALUE)
        ist->next_dts = av_rescale_q(decoded_frame->pts, ist->st->time_base, AV_TIME_BASE_Q);
    else if (pkt && pkt->pts != AV_NOPTS_VALUE) {
        decoded_frame->pts = pkt->pts;
    }

    if (decoded_frame->pts != AV_NOPTS_VALUE)
        decoded_frame->pts = av_rescale_q(decoded_frame->pts,
                                          ist->st->time_base,
                                          (AVRational){1, avctx->sample_rate});
    ist->nb_samples = decoded_frame->nb_samples;
    for (i = 0; i < ist->nb_filters; i++) {
        if (i < ist->nb_filters - 1) {
            f = ist->filter_frame;
            err = av_frame_ref(f, decoded_frame);
            if (err < 0)
                break;
        } else
            f = decoded_frame;

        err = ifilter_send_frame(ist->filters[i], f);
        if (err < 0)
            break;
    }

    av_frame_unref(ist->filter_frame);
    av_frame_unref(decoded_frame);
    return err < 0 ? err : ret;
}

static int decode_video(InputStream *ist, AVPacket *pkt, int *got_output,
                        int *decode_failed)
{
    AVFrame *decoded_frame, *f;
    int i, ret = 0, err = 0;

    if (!ist->decoded_frame && !(ist->decoded_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    if (!ist->filter_frame && !(ist->filter_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    decoded_frame = ist->decoded_frame;

    ret = decode(ist->dec_ctx, decoded_frame, got_output, pkt);
    if (ret < 0)
        *decode_failed = 1;
    if (!*got_output || ret < 0)
        return ret;

    ist->frames_decoded++;

    if (ist->hwaccel_retrieve_data && decoded_frame->format == ist->hwaccel_pix_fmt) {
        err = ist->hwaccel_retrieve_data(ist->dec_ctx, decoded_frame);
        if (err < 0)
            goto fail;
    }
    ist->hwaccel_retrieved_pix_fmt = decoded_frame->format;

    decoded_frame->pts = guess_correct_pts(&ist->pts_ctx, decoded_frame->pts,
                                           decoded_frame->pkt_dts);
    if (ist->framerate.num)
        decoded_frame->pts = ist->cfr_next_pts++;

    if (ist->st->sample_aspect_ratio.num)
        decoded_frame->sample_aspect_ratio = ist->st->sample_aspect_ratio;

    for (i = 0; i < ist->nb_filters; i++) {
        if (i < ist->nb_filters - 1) {
            f = ist->filter_frame;
            err = av_frame_ref(f, decoded_frame);
            if (err < 0)
                break;
        } else
            f = decoded_frame;

        err = ifilter_send_frame(ist->filters[i], f);
        if (err < 0)
            break;
    }

fail:
    av_frame_unref(ist->filter_frame);
    av_frame_unref(decoded_frame);
    return err < 0 ? err : ret;
}

static int transcode_subtitles(InputStream *ist, AVPacket *pkt, int *got_output,
                               int *decode_failed)
{
    AVSubtitle subtitle;
    int i, ret = avcodec_decode_subtitle2(ist->dec_ctx,
                                          &subtitle, got_output, pkt);
    if (ret < 0) {
        *decode_failed = 1;
        return ret;
    }
    if (!*got_output)
        return ret;

    ist->frames_decoded++;

    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];

        if (!check_output_constraints(ist, ost) || !ost->encoding_needed)
            continue;

        do_subtitle_out(output_files[ost->file_index], ost, ist, &subtitle, pkt->pts);
    }

    avsubtitle_free(&subtitle);
    return ret;
}

static int send_filter_eof(InputStream *ist)
{
    int i, ret;
    for (i = 0; i < ist->nb_filters; i++) {
        ret = ifilter_send_eof(ist->filters[i]);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/* pkt = NULL means EOF (needed to flush decoder buffers) */
static void process_input_packet(InputStream *ist, const AVPacket *pkt, int no_eof)
{
    int i;
    int repeating = 0;
    AVPacket avpkt;

    if (ist->next_dts == AV_NOPTS_VALUE)
        ist->next_dts = ist->last_dts;

    if (!pkt) {
        /* EOF handling */
        av_init_packet(&avpkt);
        avpkt.data = NULL;
        avpkt.size = 0;
    } else {
        avpkt = *pkt;
    }

    if (pkt && pkt->dts != AV_NOPTS_VALUE)
        ist->next_dts = ist->last_dts = av_rescale_q(pkt->dts, ist->st->time_base, AV_TIME_BASE_Q);

    // while we have more to decode or while the decoder did output something on EOF
    while (ist->decoding_needed && (!pkt || avpkt.size > 0)) {
        int ret = 0;
        int got_output = 0;
        int decode_failed = 0;

        if (!repeating)
            ist->last_dts = ist->next_dts;

        switch (ist->dec_ctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            ret = decode_audio    (ist, repeating ? NULL : &avpkt, &got_output,
                                   &decode_failed);
            break;
        case AVMEDIA_TYPE_VIDEO:
            ret = decode_video    (ist, repeating ? NULL : &avpkt, &got_output,
                                   &decode_failed);
            if (repeating && !got_output)
                ;
            else if (pkt && pkt->duration)
                ist->next_dts += av_rescale_q(pkt->duration, ist->st->time_base, AV_TIME_BASE_Q);
            else if (ist->st->avg_frame_rate.num)
                ist->next_dts += av_rescale_q(1, av_inv_q(ist->st->avg_frame_rate),
                                              AV_TIME_BASE_Q);
            else if (ist->dec_ctx->framerate.num != 0) {
                int ticks      = ist->st->parser ? ist->st->parser->repeat_pict + 1 :
                                                   ist->dec_ctx->ticks_per_frame;
                ist->next_dts += av_rescale_q(ticks, ist->dec_ctx->framerate, AV_TIME_BASE_Q);
            }
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            if (repeating)
                break;
            ret = transcode_subtitles(ist, &avpkt, &got_output, &decode_failed);
            break;
        default:
            return;
        }

        if (ret < 0) {
            if (decode_failed) {
                av_log(NULL, AV_LOG_ERROR, "Error while decoding stream #%d:%d\n",
                       ist->file_index, ist->st->index);
            } else {
                av_log(NULL, AV_LOG_FATAL, "Error while processing the decoded "
                       "data for stream #%d:%d\n", ist->file_index, ist->st->index);
            }
            if (!decode_failed || exit_on_error)
                exit_program(1);
            break;
        }

        if (!got_output)
            break;

        repeating = 1;
    }

    /* after flushing, send an EOF on all the filter inputs attached to the stream */
    /* except when looping we need to flush but not to send an EOF */
    if (!pkt && ist->decoding_needed && !no_eof) {
        int ret = send_filter_eof(ist);
        if (ret < 0) {
            av_log(NULL, AV_LOG_FATAL, "Error marking filters as finished\n");
            exit_program(1);
        }
    }

    /* handle stream copy */
    if (!ist->decoding_needed) {
        ist->last_dts = ist->next_dts;
        switch (ist->dec_ctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            ist->next_dts += ((int64_t)AV_TIME_BASE * ist->dec_ctx->frame_size) /
                             ist->dec_ctx->sample_rate;
            break;
        case AVMEDIA_TYPE_VIDEO:
            if (ist->dec_ctx->framerate.num != 0) {
                int ticks = ist->st->parser ? ist->st->parser->repeat_pict + 1 : ist->dec_ctx->ticks_per_frame;
                ist->next_dts += ((int64_t)AV_TIME_BASE *
                                  ist->dec_ctx->framerate.den * ticks) /
                                  ist->dec_ctx->framerate.num;
            }
            break;
        }
    }
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];

        if (!check_output_constraints(ist, ost) || ost->encoding_needed)
            continue;

        do_streamcopy(ist, ost, pkt);
    }

    return;
}

static void print_sdp(void)
{
    char sdp[16384];
    int i;
    AVFormatContext **avc;

    for (i = 0; i < nb_output_files; i++) {
        if (!output_files[i]->header_written)
            return;
    }

    avc = av_malloc(sizeof(*avc) * nb_output_files);
    if (!avc)
        exit_program(1);
    for (i = 0; i < nb_output_files; i++)
        avc[i] = output_files[i]->ctx;

    av_sdp_create(avc, nb_output_files, sdp, sizeof(sdp));
    printf("SDP:\n%s\n", sdp);
    fflush(stdout);
    av_freep(&avc);
}

static enum AVPixelFormat get_format(AVCodecContext *s, const enum AVPixelFormat *pix_fmts)
{
    InputStream *ist = s->opaque;
    const enum AVPixelFormat *p;
    int ret;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        const AVCodecHWConfig  *config = NULL;
        int i;

        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            break;

        if (ist->hwaccel_id == HWACCEL_GENERIC ||
            ist->hwaccel_id == HWACCEL_AUTO) {
            for (i = 0;; i++) {
                config = avcodec_get_hw_config(s->codec, i);
                if (!config)
                    break;
                if (!(config->methods &
                      AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                    continue;
                if (config->pix_fmt == *p)
                    break;
            }
        }
        if (config) {
            if (config->device_type != ist->hwaccel_device_type) {
                // Different hwaccel offered, ignore.
                continue;
            }

            ret = hwaccel_decode_init(s);
            if (ret < 0) {
                if (ist->hwaccel_id == HWACCEL_GENERIC) {
                    av_log(NULL, AV_LOG_FATAL,
                           "%s hwaccel requested for input stream #%d:%d, "
                           "but cannot be initialized.\n",
                           av_hwdevice_get_type_name(config->device_type),
                           ist->file_index, ist->st->index);
                    return AV_PIX_FMT_NONE;
                }
                continue;
            }
        } else {
            const HWAccel *hwaccel = NULL;
            int i;
            for (i = 0; hwaccels[i].name; i++) {
                if (hwaccels[i].pix_fmt == *p) {
                    hwaccel = &hwaccels[i];
                    break;
                }
            }
            if (!hwaccel) {
                // No hwaccel supporting this pixfmt.
                continue;
            }
            if (hwaccel->id != ist->hwaccel_id) {
                // Does not match requested hwaccel.
                continue;
            }

            ret = hwaccel->init(s);
            if (ret < 0) {
                av_log(NULL, AV_LOG_FATAL,
                       "%s hwaccel requested for input stream #%d:%d, "
                       "but cannot be initialized.\n", hwaccel->name,
                       ist->file_index, ist->st->index);
                return AV_PIX_FMT_NONE;
            }
        }

        if (ist->hw_frames_ctx) {
            s->hw_frames_ctx = av_buffer_ref(ist->hw_frames_ctx);
            if (!s->hw_frames_ctx)
                return AV_PIX_FMT_NONE;
        }

        ist->hwaccel_pix_fmt = *p;
        break;
    }

    return *p;
}

static int get_buffer(AVCodecContext *s, AVFrame *frame, int flags)
{
    InputStream *ist = s->opaque;

    if (ist->hwaccel_get_buffer && frame->format == ist->hwaccel_pix_fmt)
        return ist->hwaccel_get_buffer(s, frame, flags);

    return avcodec_default_get_buffer2(s, frame, flags);
}

static int init_input_stream(int ist_index, char *error, int error_len)
{
    int ret;
    InputStream *ist = input_streams[ist_index];

    if (ist->decoding_needed) {
        AVCodec *codec = ist->dec;
        if (!codec) {
            snprintf(error, error_len, "Decoder (codec id %d) not found for input stream #%d:%d",
                    ist->dec_ctx->codec_id, ist->file_index, ist->st->index);
            return AVERROR(EINVAL);
        }

        ist->dec_ctx->opaque                = ist;
        ist->dec_ctx->get_format            = get_format;
        ist->dec_ctx->get_buffer2           = get_buffer;
        ist->dec_ctx->thread_safe_callbacks = 1;

        av_opt_set_int(ist->dec_ctx, "refcounted_frames", 1, 0);

        if (!av_dict_get(ist->decoder_opts, "threads", NULL, 0))
            av_dict_set(&ist->decoder_opts, "threads", "auto", 0);

        ret = hw_device_setup_for_decode(ist);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            snprintf(error, error_len, "Device setup failed for "
                     "decoder on input stream #%d:%d : %s",
                     ist->file_index, ist->st->index, errbuf);
            return ret;
        }

        if ((ret = avcodec_open2(ist->dec_ctx, codec, &ist->decoder_opts)) < 0) {
            char errbuf[128];
            if (ret == AVERROR_EXPERIMENTAL)
                abort_codec_experimental(codec, 0);

            av_strerror(ret, errbuf, sizeof(errbuf));

            snprintf(error, error_len,
                     "Error while opening decoder for input stream "
                     "#%d:%d : %s",
                     ist->file_index, ist->st->index, errbuf);
            return ret;
        }
        assert_avoptions(ist->decoder_opts);
    }

    ist->last_dts = ist->st->avg_frame_rate.num ? - ist->dec_ctx->has_b_frames * AV_TIME_BASE / av_q2d(ist->st->avg_frame_rate) : 0;
    ist->next_dts = AV_NOPTS_VALUE;
    init_pts_correction(&ist->pts_ctx);

    return 0;
}

static InputStream *get_input_stream(OutputStream *ost)
{
    if (ost->source_index >= 0)
        return input_streams[ost->source_index];

    if (ost->filter) {
        FilterGraph *fg = ost->filter->graph;
        int i;

        for (i = 0; i < fg->nb_inputs; i++)
            if (fg->inputs[i]->ist->dec_ctx->codec_type == ost->enc_ctx->codec_type)
                return fg->inputs[i]->ist;
    }

    return NULL;
}

/* open the muxer when all the streams are initialized */
static int check_init_output_file(OutputFile *of, int file_index)
{
    int ret, i;

    for (i = 0; i < of->ctx->nb_streams; i++) {
        OutputStream *ost = output_streams[of->ost_index + i];
        if (!ost->initialized)
            return 0;
    }

    of->ctx->interrupt_callback = int_cb;

    ret = avformat_write_header(of->ctx, &of->opts);
    if (ret < 0) {
        char errbuf[128];

        av_strerror(ret, errbuf, sizeof(errbuf));

        av_log(NULL, AV_LOG_ERROR,
               "Could not write header for output file #%d "
               "(incorrect codec parameters ?): %s",
               file_index, errbuf);
        return ret;
    }
    assert_avoptions(of->opts);
    of->header_written = 1;

    av_dump_format(of->ctx, file_index, of->ctx->filename, 1);

    if (want_sdp)
        print_sdp();

    /* flush the muxing queues */
    for (i = 0; i < of->ctx->nb_streams; i++) {
        OutputStream *ost = output_streams[of->ost_index + i];

        while (av_fifo_size(ost->muxing_queue)) {
            AVPacket pkt;
            av_fifo_generic_read(ost->muxing_queue, &pkt, sizeof(pkt), NULL);
            write_packet(of, &pkt, ost);
        }
    }

    return 0;
}

static int init_output_bsfs(OutputStream *ost)
{
    AVBSFContext *ctx;
    int i, ret;

    if (!ost->nb_bitstream_filters)
        return 0;

    for (i = 0; i < ost->nb_bitstream_filters; i++) {
        ctx = ost->bsf_ctx[i];

        ret = avcodec_parameters_copy(ctx->par_in,
                                      i ? ost->bsf_ctx[i - 1]->par_out : ost->st->codecpar);
        if (ret < 0)
            return ret;

        ctx->time_base_in = i ? ost->bsf_ctx[i - 1]->time_base_out : ost->st->time_base;

        ret = av_bsf_init(ctx);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error initializing bitstream filter: %s\n",
                   ctx->filter->name);
            return ret;
        }
    }

    ret = avcodec_parameters_copy(ost->st->codecpar, ctx->par_out);
    if (ret < 0)
        return ret;

    ost->st->time_base = ctx->time_base_out;

    return 0;
}

static int init_output_stream_streamcopy(OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];
    InputStream *ist = get_input_stream(ost);
    AVCodecParameters *par_dst = ost->st->codecpar;
    AVCodecParameters *par_src = ist->st->codecpar;
    AVRational sar;
    uint32_t codec_tag = par_dst->codec_tag;
    int i, ret;

    if (!codec_tag) {
        if (!of->ctx->oformat->codec_tag ||
             av_codec_get_id (of->ctx->oformat->codec_tag, par_src->codec_tag) == par_src->codec_id ||
             av_codec_get_tag(of->ctx->oformat->codec_tag, par_src->codec_id) <= 0)
            codec_tag = par_src->codec_tag;
    }

    ret = avcodec_parameters_copy(par_dst, par_src);
    if (ret < 0)
        return ret;

    par_dst->codec_tag = codec_tag;

    ost->st->disposition = ist->st->disposition;

    ost->st->time_base = ist->st->time_base;

    if (ost->bitrate_override)
        par_dst->bit_rate = ost->bitrate_override;

    if (ist->st->nb_side_data) {
        ost->st->side_data = av_realloc_array(NULL, ist->st->nb_side_data,
                                              sizeof(*ist->st->side_data));
        if (!ost->st->side_data)
            return AVERROR(ENOMEM);

        for (i = 0; i < ist->st->nb_side_data; i++) {
            const AVPacketSideData *sd_src = &ist->st->side_data[i];
            AVPacketSideData *sd_dst = &ost->st->side_data[i];

            sd_dst->data = av_malloc(sd_src->size);
            if (!sd_dst->data)
                return AVERROR(ENOMEM);
            memcpy(sd_dst->data, sd_src->data, sd_src->size);
            sd_dst->size = sd_src->size;
            sd_dst->type = sd_src->type;
            ost->st->nb_side_data++;
        }
    }

    ost->parser = av_parser_init(par_dst->codec_id);
    ost->parser_avctx = avcodec_alloc_context3(NULL);
    if (!ost->parser_avctx)
        return AVERROR(ENOMEM);

    if (par_dst->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (ost->frame_aspect_ratio)
            sar = av_d2q(ost->frame_aspect_ratio * par_dst->height / par_dst->width, 255);
        else if (ist->st->sample_aspect_ratio.num)
            sar = ist->st->sample_aspect_ratio;
        else
            sar = par_src->sample_aspect_ratio;
        ost->st->sample_aspect_ratio = par_dst->sample_aspect_ratio = sar;
    }

    return 0;
}

static void set_encoder_id(OutputFile *of, OutputStream *ost)
{
    AVDictionaryEntry *e;

    uint8_t *encoder_string;
    int encoder_string_len;
    int format_flags = 0;

    e = av_dict_get(of->opts, "fflags", NULL, 0);
    if (e) {
        const AVOption *o = av_opt_find(of->ctx, "fflags", NULL, 0, 0);
        if (!o)
            return;
        av_opt_eval_flags(of->ctx, o, e->value, &format_flags);
    }

    encoder_string_len = sizeof(LIBAVCODEC_IDENT) + strlen(ost->enc->name) + 2;
    encoder_string     = av_mallocz(encoder_string_len);
    if (!encoder_string)
        exit_program(1);

    if (!(format_flags & AVFMT_FLAG_BITEXACT))
        av_strlcpy(encoder_string, LIBAVCODEC_IDENT " ", encoder_string_len);
    av_strlcat(encoder_string, ost->enc->name, encoder_string_len);
    av_dict_set(&ost->st->metadata, "encoder",  encoder_string,
                AV_DICT_DONT_STRDUP_VAL | AV_DICT_DONT_OVERWRITE);
}

static void parse_forced_key_frames(char *kf, OutputStream *ost,
                                    AVCodecContext *avctx)
{
    char *p;
    int n = 1, i;
    int64_t t;

    for (p = kf; *p; p++)
        if (*p == ',')
            n++;
    ost->forced_kf_count = n;
    ost->forced_kf_pts   = av_malloc(sizeof(*ost->forced_kf_pts) * n);
    if (!ost->forced_kf_pts) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate forced key frames array.\n");
        exit_program(1);
    }

    p = kf;
    for (i = 0; i < n; i++) {
        char *next = strchr(p, ',');

        if (next)
            *next++ = 0;

        t = parse_time_or_die("force_key_frames", p, 1);
        ost->forced_kf_pts[i] = av_rescale_q(t, AV_TIME_BASE_Q, avctx->time_base);

        p = next;
    }
}

static int init_output_stream_encode(OutputStream *ost)
{
    InputStream *ist = get_input_stream(ost);
    AVCodecContext *enc_ctx = ost->enc_ctx;
    AVCodecContext *dec_ctx = NULL;

    set_encoder_id(output_files[ost->file_index], ost);

    if (ist) {
        ost->st->disposition = ist->st->disposition;

        dec_ctx = ist->dec_ctx;

        enc_ctx->bits_per_raw_sample    = dec_ctx->bits_per_raw_sample;
        enc_ctx->chroma_sample_location = dec_ctx->chroma_sample_location;
    }

    switch (enc_ctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        enc_ctx->sample_fmt     = ost->filter->filter->inputs[0]->format;
        enc_ctx->sample_rate    = ost->filter->filter->inputs[0]->sample_rate;
        enc_ctx->channel_layout = ost->filter->filter->inputs[0]->channel_layout;
        enc_ctx->channels       = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
        enc_ctx->time_base      = (AVRational){ 1, enc_ctx->sample_rate };
        break;
    case AVMEDIA_TYPE_VIDEO:
        enc_ctx->time_base = ost->filter->filter->inputs[0]->time_base;

        enc_ctx->width  = ost->filter->filter->inputs[0]->w;
        enc_ctx->height = ost->filter->filter->inputs[0]->h;
        enc_ctx->sample_aspect_ratio = ost->st->sample_aspect_ratio =
            ost->frame_aspect_ratio ? // overridden by the -aspect cli option
            av_d2q(ost->frame_aspect_ratio * enc_ctx->height/enc_ctx->width, 255) :
            ost->filter->filter->inputs[0]->sample_aspect_ratio;
        enc_ctx->pix_fmt = ost->filter->filter->inputs[0]->format;

        enc_ctx->framerate = ost->frame_rate;

        ost->st->avg_frame_rate = ost->frame_rate;

        if (dec_ctx &&
            (enc_ctx->width   != dec_ctx->width  ||
             enc_ctx->height  != dec_ctx->height ||
             enc_ctx->pix_fmt != dec_ctx->pix_fmt)) {
            enc_ctx->bits_per_raw_sample = 0;
        }

        if (ost->forced_keyframes)
            parse_forced_key_frames(ost->forced_keyframes, ost,
                                    ost->enc_ctx);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        enc_ctx->time_base = (AVRational){1, 1000};
        break;
    default:
        abort();
        break;
    }

    return 0;
}

static int init_output_stream(OutputStream *ost, char *error, int error_len)
{
    int ret = 0;

    if (ost->encoding_needed) {
        AVCodec      *codec = ost->enc;
        AVCodecContext *dec = NULL;
        InputStream *ist;

        ret = init_output_stream_encode(ost);
        if (ret < 0)
            return ret;

        if ((ist = get_input_stream(ost)))
            dec = ist->dec_ctx;
        if (dec && dec->subtitle_header) {
            ost->enc_ctx->subtitle_header = av_malloc(dec->subtitle_header_size);
            if (!ost->enc_ctx->subtitle_header)
                return AVERROR(ENOMEM);
            memcpy(ost->enc_ctx->subtitle_header, dec->subtitle_header, dec->subtitle_header_size);
            ost->enc_ctx->subtitle_header_size = dec->subtitle_header_size;
        }
        if (!av_dict_get(ost->encoder_opts, "threads", NULL, 0))
            av_dict_set(&ost->encoder_opts, "threads", "auto", 0);

        if (ost->filter && ost->filter->filter->inputs[0]->hw_frames_ctx &&
            ((AVHWFramesContext*)ost->filter->filter->inputs[0]->hw_frames_ctx->data)->format ==
            ost->filter->filter->inputs[0]->format) {
            ost->enc_ctx->hw_frames_ctx = av_buffer_ref(ost->filter->filter->inputs[0]->hw_frames_ctx);
            if (!ost->enc_ctx->hw_frames_ctx)
                return AVERROR(ENOMEM);
        } else {
            ret = hw_device_setup_for_encode(ost);
            if (ret < 0) {
                char errbuf[128];
                av_strerror(ret, errbuf, sizeof(errbuf));
                snprintf(error, error_len, "Device setup failed for "
                         "encoder on output stream #%d:%d : %s",
                     ost->file_index, ost->index, errbuf);
                return ret;
            }
        }

        if ((ret = avcodec_open2(ost->enc_ctx, codec, &ost->encoder_opts)) < 0) {
            if (ret == AVERROR_EXPERIMENTAL)
                abort_codec_experimental(codec, 1);
            snprintf(error, error_len,
                     "Error while opening encoder for output stream #%d:%d - "
                     "maybe incorrect parameters such as bit_rate, rate, width or height",
                    ost->file_index, ost->index);
            return ret;
        }
        assert_avoptions(ost->encoder_opts);
        if (ost->enc_ctx->bit_rate && ost->enc_ctx->bit_rate < 1000)
            av_log(NULL, AV_LOG_WARNING, "The bitrate parameter is set too low."
                                         "It takes bits/s as argument, not kbits/s\n");

        ret = avcodec_parameters_from_context(ost->st->codecpar, ost->enc_ctx);
        if (ret < 0) {
            av_log(NULL, AV_LOG_FATAL,
                   "Error initializing the output stream codec context.\n");
            exit_program(1);
        }

        if (ost->enc_ctx->nb_coded_side_data) {
            int i;

            ost->st->side_data = av_realloc_array(NULL, ost->enc_ctx->nb_coded_side_data,
                                                  sizeof(*ost->st->side_data));
            if (!ost->st->side_data)
                return AVERROR(ENOMEM);

            for (i = 0; i < ost->enc_ctx->nb_coded_side_data; i++) {
                const AVPacketSideData *sd_src = &ost->enc_ctx->coded_side_data[i];
                AVPacketSideData *sd_dst = &ost->st->side_data[i];

                sd_dst->data = av_malloc(sd_src->size);
                if (!sd_dst->data)
                    return AVERROR(ENOMEM);
                memcpy(sd_dst->data, sd_src->data, sd_src->size);
                sd_dst->size = sd_src->size;
                sd_dst->type = sd_src->type;
                ost->st->nb_side_data++;
            }
        }

        ost->st->time_base = ost->enc_ctx->time_base;
    } else if (ost->stream_copy) {
        ret = init_output_stream_streamcopy(ost);
        if (ret < 0)
            return ret;

        /*
         * FIXME: will the codec context used by the parser during streamcopy
         * This should go away with the new parser API.
         */
        ret = avcodec_parameters_to_context(ost->parser_avctx, ost->st->codecpar);
        if (ret < 0)
            return ret;
    }

    /* initialize bitstream filters for the output stream
     * needs to be done here, because the codec id for streamcopy is not
     * known until now */
    ret = init_output_bsfs(ost);
    if (ret < 0)
        return ret;

    ost->mux_timebase = ost->st->time_base;

    ost->initialized = 1;

    ret = check_init_output_file(output_files[ost->file_index], ost->file_index);
    if (ret < 0)
        return ret;

    return ret;
}

static int transcode_init(void)
{
    int ret = 0, i, j, k;
    OutputStream *ost;
    InputStream *ist;
    char error[1024];

    /* init framerate emulation */
    for (i = 0; i < nb_input_files; i++) {
        InputFile *ifile = input_files[i];
        if (ifile->rate_emu)
            for (j = 0; j < ifile->nb_streams; j++)
                input_streams[j + ifile->ist_index]->start = av_gettime_relative();
    }

    /* init input streams */
    for (i = 0; i < nb_input_streams; i++)
        if ((ret = init_input_stream(i, error, sizeof(error))) < 0)
            goto dump_format;

    /* open each encoder */
    for (i = 0; i < nb_output_streams; i++) {
        // skip streams fed from filtergraphs until we have a frame for them
        if (output_streams[i]->filter)
            continue;

        ret = init_output_stream(output_streams[i], error, sizeof(error));
        if (ret < 0)
            goto dump_format;
    }


    /* discard unused programs */
    for (i = 0; i < nb_input_files; i++) {
        InputFile *ifile = input_files[i];
        for (j = 0; j < ifile->ctx->nb_programs; j++) {
            AVProgram *p = ifile->ctx->programs[j];
            int discard  = AVDISCARD_ALL;

            for (k = 0; k < p->nb_stream_indexes; k++)
                if (!input_streams[ifile->ist_index + p->stream_index[k]]->discard) {
                    discard = AVDISCARD_DEFAULT;
                    break;
                }
            p->discard = discard;
        }
    }

 dump_format:
    /* dump the stream mapping */
    av_log(NULL, AV_LOG_INFO, "Stream mapping:\n");
    for (i = 0; i < nb_input_streams; i++) {
        ist = input_streams[i];

        for (j = 0; j < ist->nb_filters; j++) {
            if (!filtergraph_is_simple(ist->filters[j]->graph)) {
                av_log(NULL, AV_LOG_INFO, "  Stream #%d:%d (%s) -> %s",
                       ist->file_index, ist->st->index, ist->dec ? ist->dec->name : "?",
                       ist->filters[j]->name);
                if (nb_filtergraphs > 1)
                    av_log(NULL, AV_LOG_INFO, " (graph %d)", ist->filters[j]->graph->index);
                av_log(NULL, AV_LOG_INFO, "\n");
            }
        }
    }

    for (i = 0; i < nb_output_streams; i++) {
        ost = output_streams[i];

        if (ost->attachment_filename) {
            /* an attached file */
            av_log(NULL, AV_LOG_INFO, "  File %s -> Stream #%d:%d\n",
                   ost->attachment_filename, ost->file_index, ost->index);
            continue;
        }

        if (ost->filter && !filtergraph_is_simple(ost->filter->graph)) {
            /* output from a complex graph */
            av_log(NULL, AV_LOG_INFO, "  %s", ost->filter->name);
            if (nb_filtergraphs > 1)
                av_log(NULL, AV_LOG_INFO, " (graph %d)", ost->filter->graph->index);

            av_log(NULL, AV_LOG_INFO, " -> Stream #%d:%d (%s)\n", ost->file_index,
                   ost->index, ost->enc ? ost->enc->name : "?");
            continue;
        }

        av_log(NULL, AV_LOG_INFO, "  Stream #%d:%d -> #%d:%d",
               input_streams[ost->source_index]->file_index,
               input_streams[ost->source_index]->st->index,
               ost->file_index,
               ost->index);
        if (ost->sync_ist != input_streams[ost->source_index])
            av_log(NULL, AV_LOG_INFO, " [sync #%d:%d]",
                   ost->sync_ist->file_index,
                   ost->sync_ist->st->index);
        if (ost->stream_copy)
            av_log(NULL, AV_LOG_INFO, " (copy)");
        else {
            const AVCodec *in_codec    = input_streams[ost->source_index]->dec;
            const AVCodec *out_codec   = ost->enc;
            const char *decoder_name   = "?";
            const char *in_codec_name  = "?";
            const char *encoder_name   = "?";
            const char *out_codec_name = "?";
            const AVCodecDescriptor *desc;

            if (in_codec) {
                decoder_name  = in_codec->name;
                desc = avcodec_descriptor_get(in_codec->id);
                if (desc)
                    in_codec_name = desc->name;
                if (!strcmp(decoder_name, in_codec_name))
                    decoder_name = "native";
            }

            if (out_codec) {
                encoder_name   = out_codec->name;
                desc = avcodec_descriptor_get(out_codec->id);
                if (desc)
                    out_codec_name = desc->name;
                if (!strcmp(encoder_name, out_codec_name))
                    encoder_name = "native";
            }

            av_log(NULL, AV_LOG_INFO, " (%s (%s) -> %s (%s))",
                   in_codec_name, decoder_name,
                   out_codec_name, encoder_name);
        }
        av_log(NULL, AV_LOG_INFO, "\n");
    }

    if (ret) {
        av_log(NULL, AV_LOG_ERROR, "%s\n", error);
        return ret;
    }

    return 0;
}

/* Return 1 if there remain streams where more output is wanted, 0 otherwise. */
static int need_output(void)
{
    int i;

    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost    = output_streams[i];
        OutputFile *of       = output_files[ost->file_index];
        AVFormatContext *os  = output_files[ost->file_index]->ctx;

        if (ost->finished ||
            (os->pb && avio_tell(os->pb) >= of->limit_filesize))
            continue;
        if (ost->frame_number >= ost->max_frames) {
            int j;
            for (j = 0; j < of->ctx->nb_streams; j++)
                output_streams[of->ost_index + j]->finished = 1;
            continue;
        }

        return 1;
    }

    return 0;
}

static InputFile *select_input_file(void)
{
    InputFile *ifile = NULL;
    int64_t ipts_min = INT64_MAX;
    int i;

    for (i = 0; i < nb_input_streams; i++) {
        InputStream *ist = input_streams[i];
        int64_t ipts     = ist->last_dts;

        if (ist->discard || input_files[ist->file_index]->eagain)
            continue;
        if (!input_files[ist->file_index]->eof_reached) {
            if (ipts < ipts_min) {
                ipts_min = ipts;
                ifile    = input_files[ist->file_index];
            }
        }
    }

    return ifile;
}

#if HAVE_PTHREADS
static void *input_thread(void *arg)
{
    InputFile *f = arg;
    int ret = 0;

    while (!transcoding_finished && ret >= 0) {
        AVPacket pkt;
        ret = av_read_frame(f->ctx, &pkt);

        if (ret == AVERROR(EAGAIN)) {
            av_usleep(10000);
            ret = 0;
            continue;
        } else if (ret < 0)
            break;

        pthread_mutex_lock(&f->fifo_lock);
        while (!av_fifo_space(f->fifo))
            pthread_cond_wait(&f->fifo_cond, &f->fifo_lock);

        av_fifo_generic_write(f->fifo, &pkt, sizeof(pkt), NULL);

        pthread_mutex_unlock(&f->fifo_lock);
    }

    f->finished = 1;
    return NULL;
}

static void free_input_threads(void)
{
    int i;

    if (nb_input_files == 1)
        return;

    transcoding_finished = 1;

    for (i = 0; i < nb_input_files; i++) {
        InputFile *f = input_files[i];
        AVPacket pkt;

        if (!f->fifo || f->joined)
            continue;

        pthread_mutex_lock(&f->fifo_lock);
        while (av_fifo_size(f->fifo)) {
            av_fifo_generic_read(f->fifo, &pkt, sizeof(pkt), NULL);
            av_packet_unref(&pkt);
        }
        pthread_cond_signal(&f->fifo_cond);
        pthread_mutex_unlock(&f->fifo_lock);

        pthread_join(f->thread, NULL);
        f->joined = 1;

        while (av_fifo_size(f->fifo)) {
            av_fifo_generic_read(f->fifo, &pkt, sizeof(pkt), NULL);
            av_packet_unref(&pkt);
        }
        av_fifo_free(f->fifo);
    }
}

static int init_input_threads(void)
{
    int i, ret;

    if (nb_input_files == 1)
        return 0;

    for (i = 0; i < nb_input_files; i++) {
        InputFile *f = input_files[i];

        if (!(f->fifo = av_fifo_alloc(8*sizeof(AVPacket))))
            return AVERROR(ENOMEM);

        pthread_mutex_init(&f->fifo_lock, NULL);
        pthread_cond_init (&f->fifo_cond, NULL);

        if ((ret = pthread_create(&f->thread, NULL, input_thread, f)))
            return AVERROR(ret);
    }
    return 0;
}

static int get_input_packet_mt(InputFile *f, AVPacket *pkt)
{
    int ret = 0;

    pthread_mutex_lock(&f->fifo_lock);

    if (av_fifo_size(f->fifo)) {
        av_fifo_generic_read(f->fifo, pkt, sizeof(*pkt), NULL);
        pthread_cond_signal(&f->fifo_cond);
    } else {
        if (f->finished)
            ret = AVERROR_EOF;
        else
            ret = AVERROR(EAGAIN);
    }

    pthread_mutex_unlock(&f->fifo_lock);

    return ret;
}
#endif

static int get_input_packet(InputFile *f, AVPacket *pkt)
{
    if (f->rate_emu) {
        int i;
        for (i = 0; i < f->nb_streams; i++) {
            InputStream *ist = input_streams[f->ist_index + i];
            int64_t pts = av_rescale(ist->last_dts, 1000000, AV_TIME_BASE);
            int64_t now = av_gettime_relative() - ist->start;
            if (pts > now)
                return AVERROR(EAGAIN);
        }
    }

#if HAVE_PTHREADS
    if (nb_input_files > 1)
        return get_input_packet_mt(f, pkt);
#endif
    return av_read_frame(f->ctx, pkt);
}

static int got_eagain(void)
{
    int i;
    for (i = 0; i < nb_input_files; i++)
        if (input_files[i]->eagain)
            return 1;
    return 0;
}

static void reset_eagain(void)
{
    int i;
    for (i = 0; i < nb_input_files; i++)
        input_files[i]->eagain = 0;
}

// set duration to max(tmp, duration) in a proper time base and return duration's time_base
static AVRational duration_max(int64_t tmp, int64_t *duration, AVRational tmp_time_base,
                                AVRational time_base)
{
    int ret;

    if (!*duration) {
        *duration = tmp;
        return tmp_time_base;
    }

    ret = av_compare_ts(*duration, time_base, tmp, tmp_time_base);
    if (ret < 0) {
        *duration = tmp;
        return tmp_time_base;
    }

    return time_base;
}

static int seek_to_start(InputFile *ifile, AVFormatContext *is)
{
    InputStream *ist;
    AVCodecContext *avctx;
    int i, ret, has_audio = 0;
    int64_t duration = 0;

    ret = av_seek_frame(is, -1, is->start_time, 0);
    if (ret < 0)
        return ret;

    for (i = 0; i < ifile->nb_streams; i++) {
        ist   = input_streams[ifile->ist_index + i];
        avctx = ist->dec_ctx;

        // flush decoders
        if (ist->decoding_needed) {
            process_input_packet(ist, NULL, 1);
            avcodec_flush_buffers(avctx);
        }

        /* duration is the length of the last frame in a stream
         * when audio stream is present we don't care about
         * last video frame length because it's not defined exactly */
        if (avctx->codec_type == AVMEDIA_TYPE_AUDIO && ist->nb_samples)
            has_audio = 1;
    }

    for (i = 0; i < ifile->nb_streams; i++) {
        ist   = input_streams[ifile->ist_index + i];
        avctx = ist->dec_ctx;

        if (has_audio) {
            if (avctx->codec_type == AVMEDIA_TYPE_AUDIO && ist->nb_samples) {
                AVRational sample_rate = {1, avctx->sample_rate};

                duration = av_rescale_q(ist->nb_samples, sample_rate, ist->st->time_base);
            } else
                continue;
        } else {
            if (ist->framerate.num) {
                duration = FFMAX(av_rescale_q(1, av_inv_q(ist->framerate), ist->st->time_base), 1);
            } else if (ist->st->avg_frame_rate.num) {
                duration = FFMAX(av_rescale_q(1, av_inv_q(ist->st->avg_frame_rate), ist->st->time_base), 1);
            } else duration = 1;
        }
        if (!ifile->duration)
            ifile->time_base = ist->st->time_base;
        /* the total duration of the stream, max_pts - min_pts is
         * the duration of the stream without the last frame */
        duration += ist->max_pts - ist->min_pts;
        ifile->time_base = duration_max(duration, &ifile->duration, ist->st->time_base,
                                        ifile->time_base);
    }

    if (ifile->loop > 0)
        ifile->loop--;

    return ret;
}

/*
 * Read one packet from an input file and send it for
 * - decoding -> lavfi (audio/video)
 * - decoding -> encoding -> muxing (subtitles)
 * - muxing (streamcopy)
 *
 * Return
 * - 0 -- one packet was read and processed
 * - AVERROR(EAGAIN) -- no packets were available for selected file,
 *   this function should be called again
 * - AVERROR_EOF -- this function should not be called again
 */
static int process_input(void)
{
    InputFile *ifile;
    AVFormatContext *is;
    InputStream *ist;
    AVPacket pkt;
    int ret, i, j;
    int64_t duration;

    /* select the stream that we must read now */
    ifile = select_input_file();
    /* if none, if is finished */
    if (!ifile) {
        if (got_eagain()) {
            reset_eagain();
            av_usleep(10000);
            return AVERROR(EAGAIN);
        }
        av_log(NULL, AV_LOG_VERBOSE, "No more inputs to read from.\n");
        return AVERROR_EOF;
    }

    is  = ifile->ctx;
    ret = get_input_packet(ifile, &pkt);

    if (ret == AVERROR(EAGAIN)) {
        ifile->eagain = 1;
        return ret;
    }
    if (ret < 0 && ifile->loop) {
        ret = seek_to_start(ifile, is);
        if(ret < 0)
            av_log(NULL, AV_LOG_WARNING, "Seek to start failed.\n");
        else
            ret = get_input_packet(ifile, &pkt);
    }
    if (ret < 0) {
        if (ret != AVERROR_EOF) {
            print_error(is->filename, ret);
            if (exit_on_error)
                exit_program(1);
        }
        ifile->eof_reached = 1;

        for (i = 0; i < ifile->nb_streams; i++) {
            ist = input_streams[ifile->ist_index + i];
            if (ist->decoding_needed)
                process_input_packet(ist, NULL, 0);

            /* mark all outputs that don't go through lavfi as finished */
            for (j = 0; j < nb_output_streams; j++) {
                OutputStream *ost = output_streams[j];

                if (ost->source_index == ifile->ist_index + i &&
                    (ost->stream_copy || ost->enc->type == AVMEDIA_TYPE_SUBTITLE))
                    finish_output_stream(ost);
            }
        }

        return AVERROR(EAGAIN);
    }

    reset_eagain();

    if (do_pkt_dump) {
        av_pkt_dump_log2(NULL, AV_LOG_DEBUG, &pkt, do_hex_dump,
                         is->streams[pkt.stream_index]);
    }
    /* the following test is needed in case new streams appear
       dynamically in stream : we ignore them */
    if (pkt.stream_index >= ifile->nb_streams)
        goto discard_packet;

    ist = input_streams[ifile->ist_index + pkt.stream_index];

    ist->data_size += pkt.size;
    ist->nb_packets++;

    if (ist->discard)
        goto discard_packet;

    /* add the stream-global side data to the first packet */
    if (ist->nb_packets == 1)
        for (i = 0; i < ist->st->nb_side_data; i++) {
            AVPacketSideData *src_sd = &ist->st->side_data[i];
            uint8_t *dst_data;

            if (av_packet_get_side_data(&pkt, src_sd->type, NULL))
                continue;
            if (ist->autorotate && src_sd->type == AV_PKT_DATA_DISPLAYMATRIX)
                continue;

            dst_data = av_packet_new_side_data(&pkt, src_sd->type, src_sd->size);
            if (!dst_data)
                exit_program(1);

            memcpy(dst_data, src_sd->data, src_sd->size);
        }

    if (pkt.dts != AV_NOPTS_VALUE)
        pkt.dts += av_rescale_q(ifile->ts_offset, AV_TIME_BASE_Q, ist->st->time_base);
    if (pkt.pts != AV_NOPTS_VALUE)
        pkt.pts += av_rescale_q(ifile->ts_offset, AV_TIME_BASE_Q, ist->st->time_base);

    if (pkt.pts != AV_NOPTS_VALUE)
        pkt.pts *= ist->ts_scale;
    if (pkt.dts != AV_NOPTS_VALUE)
        pkt.dts *= ist->ts_scale;

    if ((ist->dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO ||
         ist->dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) &&
        pkt.dts != AV_NOPTS_VALUE && ist->next_dts != AV_NOPTS_VALUE &&
        (is->iformat->flags & AVFMT_TS_DISCONT)) {
        int64_t pkt_dts = av_rescale_q(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q);
        int64_t delta   = pkt_dts - ist->next_dts;

        if ((FFABS(delta) > 1LL * dts_delta_threshold * AV_TIME_BASE || pkt_dts + 1 < ist->last_dts) && !copy_ts) {
            ifile->ts_offset -= delta;
            av_log(NULL, AV_LOG_DEBUG,
                   "timestamp discontinuity %"PRId64", new offset= %"PRId64"\n",
                   delta, ifile->ts_offset);
            pkt.dts -= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
            if (pkt.pts != AV_NOPTS_VALUE)
                pkt.pts -= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
        }
    }
    duration = av_rescale_q(ifile->duration, ifile->time_base, ist->st->time_base);
    if (pkt.pts != AV_NOPTS_VALUE) {
        pkt.pts += duration;
        ist->max_pts = FFMAX(pkt.pts, ist->max_pts);
        ist->min_pts = FFMIN(pkt.pts, ist->min_pts);
    }

    if (pkt.dts != AV_NOPTS_VALUE)
        pkt.dts += duration;

    process_input_packet(ist, &pkt, 0);

discard_packet:
    av_packet_unref(&pkt);

    return 0;
}

/*
 * The following code is the main loop of the file converter
 */
static int transcode(void)
{
    int ret, i, need_input = 1;
    AVFormatContext *os;
    OutputStream *ost;
    InputStream *ist;
    int64_t timer_start;

    ret = transcode_init();
    if (ret < 0)
        goto fail;

    av_log(NULL, AV_LOG_INFO, "Press ctrl-c to stop encoding\n");
    term_init();

    timer_start = av_gettime_relative();

#if HAVE_PTHREADS
    if ((ret = init_input_threads()) < 0)
        goto fail;
#endif

    while (!received_sigterm) {
        /* check if there's any stream where output is still needed */
        if (!need_output()) {
            av_log(NULL, AV_LOG_VERBOSE, "No more output streams to write to, finishing.\n");
            break;
        }

        /* read and process one input packet if needed */
        if (need_input) {
            ret = process_input();
            if (ret == AVERROR_EOF)
                need_input = 0;
        }

        ret = poll_filters();
        if (ret < 0 && ret != AVERROR_EOF) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));

            av_log(NULL, AV_LOG_ERROR, "Error while filtering: %s\n", errbuf);
            break;
        }

        /* dump report by using the output first video and audio streams */
        print_report(0, timer_start);
    }
#if HAVE_PTHREADS
    free_input_threads();
#endif

    /* at the end of stream, we must flush the decoder buffers */
    for (i = 0; i < nb_input_streams; i++) {
        ist = input_streams[i];
        if (!input_files[ist->file_index]->eof_reached) {
            process_input_packet(ist, NULL, 0);
        }
    }
    poll_filters();
    flush_encoders();

    term_exit();

    /* write the trailer if needed and close file */
    for (i = 0; i < nb_output_files; i++) {
        os = output_files[i]->ctx;
        if (!output_files[i]->header_written) {
            av_log(NULL, AV_LOG_ERROR,
                   "Nothing was written into output file %d (%s), because "
                   "at least one of its streams received no packets.\n",
                   i, os->filename);
            continue;
        }
        av_write_trailer(os);
    }

    /* dump report by using the first video and audio streams */
    print_report(1, timer_start);

    /* close each encoder */
    for (i = 0; i < nb_output_streams; i++) {
        ost = output_streams[i];
        if (ost->encoding_needed) {
            av_freep(&ost->enc_ctx->stats_in);
        }
    }

    /* close each decoder */
    for (i = 0; i < nb_input_streams; i++) {
        ist = input_streams[i];
        if (ist->decoding_needed) {
            avcodec_close(ist->dec_ctx);
            if (ist->hwaccel_uninit)
                ist->hwaccel_uninit(ist->dec_ctx);
        }
    }

    av_buffer_unref(&hw_device_ctx);
    hw_device_free_all();

    /* finished ! */
    ret = 0;

 fail:
#if HAVE_PTHREADS
    free_input_threads();
#endif

    if (output_streams) {
        for (i = 0; i < nb_output_streams; i++) {
            ost = output_streams[i];
            if (ost) {
                if (ost->logfile) {
                    fclose(ost->logfile);
                    ost->logfile = NULL;
                }
                av_free(ost->forced_kf_pts);
                av_dict_free(&ost->encoder_opts);
                av_dict_free(&ost->resample_opts);
            }
        }
    }
    return ret;
}

static int64_t getutime(void)
{
#if HAVE_GETRUSAGE
    struct rusage rusage;

    getrusage(RUSAGE_SELF, &rusage);
    return (rusage.ru_utime.tv_sec * 1000000LL) + rusage.ru_utime.tv_usec;
#elif HAVE_GETPROCESSTIMES
    HANDLE proc;
    FILETIME c, e, k, u;
    proc = GetCurrentProcess();
    GetProcessTimes(proc, &c, &e, &k, &u);
    return ((int64_t) u.dwHighDateTime << 32 | u.dwLowDateTime) / 10;
#else
    return av_gettime_relative();
#endif
}

static int64_t getmaxrss(void)
{
#if HAVE_GETRUSAGE && HAVE_STRUCT_RUSAGE_RU_MAXRSS
    struct rusage rusage;
    getrusage(RUSAGE_SELF, &rusage);
    return (int64_t)rusage.ru_maxrss * 1024;
#elif HAVE_GETPROCESSMEMORYINFO
    HANDLE proc;
    PROCESS_MEMORY_COUNTERS memcounters;
    proc = GetCurrentProcess();
    memcounters.cb = sizeof(memcounters);
    GetProcessMemoryInfo(proc, &memcounters, sizeof(memcounters));
    return memcounters.PeakPagefileUsage;
#else
    return 0;
#endif
}

int main(int argc, char **argv)
{
    int i, ret;
    int64_t ti;

    register_exit(avconv_cleanup);

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);

    avcodec_register_all();
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avfilter_register_all();
    av_register_all();
    avformat_network_init();

    show_banner();

    /* parse options and open all input/output files */
    ret = avconv_parse_options(argc, argv);
    if (ret < 0)
        exit_program(1);

    if (nb_output_files <= 0 && nb_input_files == 0) {
        show_usage();
        av_log(NULL, AV_LOG_WARNING, "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        exit_program(1);
    }

    /* file converter / grab */
    if (nb_output_files <= 0) {
        fprintf(stderr, "At least one output file must be specified\n");
        exit_program(1);
    }

    for (i = 0; i < nb_output_files; i++) {
        if (strcmp(output_files[i]->ctx->oformat->name, "rtp"))
            want_sdp = 0;
    }

    ti = getutime();
    if (transcode() < 0)
        exit_program(1);
    ti = getutime() - ti;
    if (do_benchmark) {
        int maxrss = getmaxrss() / 1024;
        printf("bench: utime=%0.3fs maxrss=%ikB\n", ti / 1000000.0, maxrss);
    }

    exit_program(0);
    return 0;
}

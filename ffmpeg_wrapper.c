#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <android/log.h>
#include <libavutil/error.h>

#define LOG_TAG "FFMPEG_WRAPPER"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/*  Helper: send one (or NULL) frame to encoder, write all packets    */
/* ------------------------------------------------------------------ */
static int encode_and_write(AVCodecContext *enc_ctx,
                            AVFormatContext *ofmt_ctx,
                            AVStream       *out_stream,
                            AVPacket       *out_pkt,
                            AVFrame        *frame)          /* NULL = flush */
{
    int ret = avcodec_send_frame(enc_ctx, frame);
    if (ret < 0)
        return ret;

    while (1) {
        ret = avcodec_receive_packet(enc_ctx, out_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;

        av_packet_rescale_ts(out_pkt, enc_ctx->time_base, out_stream->time_base);
        out_pkt->stream_index = out_stream->index;

        ret = av_interleaved_write_frame(ofmt_ctx, out_pkt);
        if (ret < 0)
            return ret;
    }
}

/* ------------------------------------------------------------------ */
/*  Main public function                                              */
/* ------------------------------------------------------------------ */
int extract_audio(int         start_time_sec,
                  int         end_time_sec,
                  const char *source_path,
                  int         sample_rate,
                  const char *output_format,
                  const char *output_path)
{
    int ret = 0;

    /* Contexts / objects that must be freed at the end */
    AVFormatContext *ifmt_ctx  = NULL;
    AVFormatContext *ofmt_ctx  = NULL;
    AVCodecContext  *dec_ctx   = NULL;
    AVCodecContext  *enc_ctx   = NULL;
    SwrContext      *swr_ctx   = NULL;
    AVPacket        *pkt       = NULL;
    AVPacket        *out_pkt   = NULL;
    AVFrame         *frame     = NULL;
    AVFrame         *rsmp_frame = NULL;
    int              header_written = 0;

    AVChannelLayout mono_layout = AV_CHANNEL_LAYOUT_MONO;

    LOGD("source_path = %s", source_path);

    LOGD("Step 1 started. Open input, find best audio stream, open decoder");

    /* ============================================================== */
    /*  1. Open input, find best audio stream, open decoder           */
    /* ============================================================== */
    if ((ret = avformat_open_input(&ifmt_ctx, source_path, NULL, NULL)) < 0)
        goto cleanup;
    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0)
        goto cleanup;

    int audio_idx = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO,
                                        -1, -1, NULL, 0);
    if (audio_idx < 0) { ret = audio_idx; goto cleanup; }

    AVStream *in_stream = ifmt_ctx->streams[audio_idx];

    const AVCodec *decoder = avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!decoder) { ret = AVERROR_DECODER_NOT_FOUND; goto cleanup; }

    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) { ret = AVERROR(ENOMEM); goto cleanup; }

    if ((ret = avcodec_parameters_to_context(dec_ctx, in_stream->codecpar)) < 0)
        goto cleanup;

    dec_ctx->pkt_timebase = in_stream->time_base;

    if ((ret = avcodec_open2(dec_ctx, decoder, NULL)) < 0)
        goto cleanup;

    LOGI("Step 1 completed.");

    /* ============================================================== */
    /*  2. Seek to start time (input-level seek, like -ss before -i)  */
    /* ============================================================== */
    if (start_time_sec > 0) {
        int64_t seek_target = (int64_t)start_time_sec * AV_TIME_BASE;
        ret = avformat_seek_file(ifmt_ctx, -1,
                                 INT64_MIN, seek_target, seek_target, 0);
        if (ret < 0)
            goto cleanup;
        avcodec_flush_buffers(dec_ctx);
    }

    /* Timestamps expressed in the audio stream's time_base */
    int64_t start_pts = av_rescale_q((int64_t)start_time_sec * AV_TIME_BASE,
                                     AV_TIME_BASE_Q, in_stream->time_base);
    int64_t end_pts   = av_rescale_q((int64_t)end_time_sec   * AV_TIME_BASE,
                                     AV_TIME_BASE_Q, in_stream->time_base);

    LOGI("Step 2 completed. Seek to start time (input-level seek, like -ss before -i)");

    /* ============================================================== */
    /*  3. Set up output format, encoder, stream                      */
    /* ============================================================== */
    if ((ret = avformat_alloc_output_context2(&ofmt_ctx, NULL,
                                              output_format, output_path)) < 0)
        goto cleanup;

    /* Pick the codec the mux prefers (PCM_S16LE for WAV) */
    enum AVCodecID out_codec_id =
        av_guess_codec(ofmt_ctx->oformat, NULL, output_path, NULL,
                       AVMEDIA_TYPE_AUDIO);
    if (out_codec_id == AV_CODEC_ID_NONE)
        out_codec_id = AV_CODEC_ID_PCM_S16LE;          /* safe fallback */

    const AVCodec *encoder = avcodec_find_encoder(out_codec_id);
    if (!encoder) { ret = AVERROR_ENCODER_NOT_FOUND; goto cleanup; }

    AVStream *out_stream = avformat_new_stream(ofmt_ctx, NULL);
    if (!out_stream) { ret = AVERROR(ENOMEM); goto cleanup; }

    enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) { ret = AVERROR(ENOMEM); goto cleanup; }

    /* Choose the sample format the encoder supports (prefer S16) */
    enum AVSampleFormat enc_sample_fmt = AV_SAMPLE_FMT_S16;
    if (encoder->sample_fmts) {
        enc_sample_fmt = encoder->sample_fmts[0];      /* first supported */
        for (int i = 0; encoder->sample_fmts[i] != AV_SAMPLE_FMT_NONE; i++) {
            if (encoder->sample_fmts[i] == AV_SAMPLE_FMT_S16) {
                enc_sample_fmt = AV_SAMPLE_FMT_S16;
                break;
            }
        }
    }

    enc_ctx->sample_rate = sample_rate;
    enc_ctx->sample_fmt  = enc_sample_fmt;
    enc_ctx->time_base   = (AVRational){ 1, sample_rate };
    if ((ret = av_channel_layout_copy(&enc_ctx->ch_layout, &mono_layout)) < 0)
        goto cleanup;

    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if ((ret = avcodec_open2(enc_ctx, encoder, NULL)) < 0)
        goto cleanup;

    if ((ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx)) < 0)
        goto cleanup;
    out_stream->time_base = enc_ctx->time_base;

    LOGI("Step 3 completed. Set up output format, encoder, stream");

    /* ============================================================== */
    /*  4. Open output I/O (handles file paths AND unix:// sockets)   */
    /* ============================================================== */
    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&ofmt_ctx->pb, output_path, AVIO_FLAG_WRITE)) < 0)
            goto cleanup;
    }

    if ((ret = avformat_write_header(ofmt_ctx, NULL)) < 0)
        goto cleanup;
    header_written = 1;

    LOGI("Step 4 completed. Open output I/O (handles file paths AND unix:// sockets)");

    /* ============================================================== */
    /*  5. Set up resampler  (any input fmt/rate/layout → mono S16)   */
    /* ============================================================== */
    if ((ret = swr_alloc_set_opts2(&swr_ctx,
                                   &mono_layout,        enc_sample_fmt, sample_rate,
                                   &dec_ctx->ch_layout, dec_ctx->sample_fmt,
                                   dec_ctx->sample_rate,
                                   0, NULL)) < 0)
        goto cleanup;
    if ((ret = swr_init(swr_ctx)) < 0)
        goto cleanup;

    LOGI("Step 5 completed. Set up resampler  (any input fmt/rate/layout → mono S16)");

    /* ============================================================== */
    /*  6. Allocate working packets / frames                          */
    /* ============================================================== */
    pkt        = av_packet_alloc();
    out_pkt    = av_packet_alloc();
    frame      = av_frame_alloc();
    rsmp_frame = av_frame_alloc();
    if (!pkt || !out_pkt || !frame || !rsmp_frame) {
        ret = AVERROR(ENOMEM); goto cleanup;
    }

    LOGI("Step 6 completed. Allocate working packets / frames");

    /* ============================================================== */
    /*  7. Main decode → resample → encode loop                       */
    /* ============================================================== */
    int64_t output_pts = 0;
    int     flushing   = 0;       /* set when we sent NULL to decoder */

    while (1) {

        /* ---------- get next decoded frame ---------- */
        ret = avcodec_receive_frame(dec_ctx, frame);

        if (ret == AVERROR(EAGAIN) && !flushing) {
            /* Decoder needs more data – read packets */
            while (1) {
                ret = av_read_frame(ifmt_ctx, pkt);
                if (ret == AVERROR_EOF) {
                    /* End of file – flush decoder */
                    avcodec_send_packet(dec_ctx, NULL);
                    flushing = 1;
                    break;
                }
                if (ret < 0)
                    goto cleanup;

                if (pkt->stream_index != audio_idx) {
                    av_packet_unref(pkt);
                    continue;                           /* skip non-audio */
                }

                /* If this packet is entirely past the end, flush instead */
                if (pkt->pts != AV_NOPTS_VALUE && pkt->pts >= end_pts) {
                    av_packet_unref(pkt);
                    avcodec_send_packet(dec_ctx, NULL);
                    flushing = 1;
                    break;
                }

                ret = avcodec_send_packet(dec_ctx, pkt);
                av_packet_unref(pkt);
                if (ret < 0 && ret != AVERROR(EAGAIN))
                    goto cleanup;
                break;                                  /* try receive again */
            }
            continue;                                   /* back to receive_frame */
        }

        if (ret == AVERROR(EAGAIN) && flushing) {
            /* Shouldn't normally happen; treat as EOF */
            break;
        }
        if (ret == AVERROR_EOF) {
            /* Decoder fully drained */
            break;
        }
        if (ret < 0)
            goto cleanup;

        /* ---------- timestamp filtering ---------- */
        int64_t frame_pts = frame->pts;
        if (frame_pts == AV_NOPTS_VALUE)
            frame_pts = frame->best_effort_timestamp;

        /* Frames before start: discard */
        if (frame_pts != AV_NOPTS_VALUE && frame_pts < start_pts) {
            av_frame_unref(frame);
            continue;
        }
        /* Frames at or beyond end: stop */
        if (frame_pts != AV_NOPTS_VALUE && frame_pts >= end_pts) {
            av_frame_unref(frame);
            if (!flushing) {
                avcodec_send_packet(dec_ctx, NULL);     /* flush decoder */
                flushing = 1;
            }
            break;
        }

        /* ---------- resample ---------- */
        av_frame_unref(rsmp_frame);
        rsmp_frame->sample_rate = sample_rate;
        rsmp_frame->format      = enc_sample_fmt;
        av_channel_layout_copy(&rsmp_frame->ch_layout, &mono_layout);

        if ((ret = swr_convert_frame(swr_ctx, rsmp_frame, frame)) < 0) {
            av_frame_unref(frame);
            goto cleanup;
        }
        av_frame_unref(frame);

        if (rsmp_frame->nb_samples == 0)
            continue;

        rsmp_frame->pts = output_pts;
        output_pts     += rsmp_frame->nb_samples;

        /* ---------- encode & write ---------- */
        if ((ret = encode_and_write(enc_ctx, ofmt_ctx,
                                    out_stream, out_pkt, rsmp_frame)) < 0)
            goto cleanup;
    }

    LOGI("Step 7 completed. Main decode → resample → encode loop");

    /* ============================================================== */
    /*  8. Flush resampler (internal buffered samples)                */
    /* ============================================================== */
    while (swr_get_delay(swr_ctx, sample_rate) > 0) {
        av_frame_unref(rsmp_frame);
        rsmp_frame->sample_rate = sample_rate;
        rsmp_frame->format      = enc_sample_fmt;
        av_channel_layout_copy(&rsmp_frame->ch_layout, &mono_layout);

        if (swr_convert_frame(swr_ctx, rsmp_frame, NULL) < 0)
            break;
        if (rsmp_frame->nb_samples == 0)
            break;

        rsmp_frame->pts = output_pts;
        output_pts     += rsmp_frame->nb_samples;

        if ((ret = encode_and_write(enc_ctx, ofmt_ctx,
                                    out_stream, out_pkt, rsmp_frame)) < 0)
            goto cleanup;
    }

    LOGI("Step 8 completed. Flush resampler (internal buffered samples)");

    /* ============================================================== */
    /*  9. Flush encoder                                              */
    /* ============================================================== */
    if ((ret = encode_and_write(enc_ctx, ofmt_ctx,
                                out_stream, out_pkt, NULL)) < 0)
        goto cleanup;

    LOGI("Step 9 completed. Flush encoder");

    /* ============================================================== */
    /* 10. Write trailer (updates WAV header sizes, etc.)             */
    /* ============================================================== */
    ret = av_write_trailer(ofmt_ctx);

    LOGI("Step 10 completed. Write trailer (updates WAV header sizes, etc.)");

    /* ============================================================== */
    /*  Cleanup                                                       */
    /* ============================================================== */
cleanup:
    av_frame_free(&rsmp_frame);
    av_frame_free(&frame);
    av_packet_free(&out_pkt);
    av_packet_free(&pkt);
    swr_free(&swr_ctx);
    avcodec_free_context(&enc_ctx);
    avcodec_free_context(&dec_ctx);
    if (ofmt_ctx) {
        if (header_written)
            /* If header was written but write_trailer wasn't reached,
               the trailer is lost – this is an error path anyway.    */
            ;
        if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE) && ofmt_ctx->pb)
            avio_closep(&ofmt_ctx->pb);
        avformat_free_context(ofmt_ctx);
    }
    avformat_close_input(&ifmt_ctx);

    if (ret < 0)
    {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOGE("FFmpeg error: %s", errbuf);

        return ret;
    }

    return 0;
}
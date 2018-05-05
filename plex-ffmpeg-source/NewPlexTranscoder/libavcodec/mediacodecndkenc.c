/*
 * Android MediaCodec NDK encoder
 *
 * Copyright (c) 2015-2016, NVIDIA CORPORATION. All rights reserved.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <assert.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>

#include "libavutil/internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "avcodec.h"
#include "internal.h"
#include "mediacodecndk.h"

typedef struct MediaCodecNDKEncoderContext
{
    AVClass *avclass;
    AMediaCodec *encoder;
    AVFrame  frame;
    int saw_output_eos;
    int64_t last_dts;
    int rc_mode;
    int width;
    int height;
    uint8_t *new_extradata;
    int new_extradata_size;
    int is_rtk;
    ssize_t waiting_buffer;
} MediaCodecNDKEncoderContext;

#define LOCAL_BUFFER_FLAG_SYNCFRAME 1
#define LOCAL_BUFFER_FLAG_CODECCONFIG 2

#define OFFSET(x) offsetof(MediaCodecNDKEncoderContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

#define RC_MODE_CQ  0 // Unimplemented
#define RC_MODE_VBR 1
#define RC_MODE_CBR 2

static const AVOption options[] = {
    { "rc-mode", "The bitrate mode to use", OFFSET(rc_mode), AV_OPT_TYPE_INT, { .i64 = RC_MODE_VBR }, RC_MODE_VBR, RC_MODE_CBR, VE, "rc_mode"},
//    { "cq", "Constant quality", 0, AV_OPT_TYPE_CONST, {.i64 = RC_MODE_CQ}, INT_MIN, INT_MAX, VE, "rc_mode" },
    { "vbr", "Variable bitrate", 0, AV_OPT_TYPE_CONST, {.i64 = RC_MODE_VBR}, INT_MIN, INT_MAX, VE, "rc_mode" },
    { "cbr", "Constant bitrate", 0, AV_OPT_TYPE_CONST, {.i64 = RC_MODE_CBR}, INT_MIN, INT_MAX, VE, "rc_mode" },
    { "mediacodec_output_size", "Temporary hack to support scaling on output", OFFSET(width), AV_OPT_TYPE_IMAGE_SIZE, {.i64 = 0} , 48, 3840, VE },
    { "is-rtk", "Whether the encoder is Realtek's", OFFSET(is_rtk), AV_OPT_TYPE_BOOL, { .i64 = -1 }, -1, 1, VE },
    { NULL },
};

static av_cold int mediacodecndk_encode_init(AVCodecContext *avctx)
{
    MediaCodecNDKEncoderContext *ctx = avctx->priv_data;
    AMediaFormat* format = NULL;
    int pixelFormat;
    const char* mime = ff_mediacodecndk_get_mime(avctx->codec_id);
    int encoderStatus;
    AMediaCodecBufferInfo bufferInfo;
    int ret = ff_mediacodecndk_init_binder();
    AVRational sar = avctx->sample_aspect_ratio;

    if (ret < 0)
        return ret;

    if (ctx->is_rtk < 0)
        ctx->is_rtk = !!getenv("PLEX_MEDIA_SERVER_IS_KAMINO");

    pixelFormat = ff_mediacodecndk_get_color_format(avctx->pix_fmt);

    if (!(format = AMediaFormat_new()))
        return AVERROR(ENOMEM);

    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, avctx->height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, avctx->width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_WIDTH, avctx->width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_HEIGHT, avctx->height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, pixelFormat);

    AMediaFormat_setInt32(format, "bitrate-mode", ctx->rc_mode);

    if (avctx->rc_max_rate && avctx->rc_buffer_size) {
        AMediaFormat_setInt32(format, "max-bitrate", avctx->rc_max_rate);
        AMediaFormat_setInt32(format, "virtualbuffersize", avctx->rc_buffer_size);
    }
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, avctx->bit_rate);

    if (avctx->framerate.num && avctx->framerate.den)
        AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, av_q2d(avctx->framerate));
    else
        AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, av_q2d(av_inv_q(avctx->time_base)));
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);//FIXME
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_STRIDE, avctx->width);
    AMediaFormat_setInt32(format, "priority", 1);

    AMediaFormat_setInt32(format, "profile", 0x08);//High
    AMediaFormat_setInt32(format, "level", 0x200);//Level31

    if (!sar.num || !sar.den)
        sar.num = sar.den = 1;

    if (ctx->width && ctx->height) {
        AMediaFormat_setInt32(format, "output_width", ctx->width);
        AMediaFormat_setInt32(format, "output_height", ctx->height);

        sar = av_mul_q((AVRational){ctx->height * avctx->width, ctx->width * avctx->height}, sar);
    }

    av_reduce(&sar.num, &sar.den, sar.num, sar.den, 4096);

    AMediaFormat_setInt32(format, "aspect-width", sar.num);
    AMediaFormat_setInt32(format, "aspect-height", sar.den);

    if (!(ctx->encoder = AMediaCodec_createEncoderByType(mime))) {
        av_log(avctx, AV_LOG_ERROR, "Encoder could not be created\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (AMediaCodec_configure(ctx->encoder, format, NULL, 0,
                              AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to configure encoder; check parameters\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    AMediaCodec_start(ctx->encoder);

    AMediaFormat_delete(format);
    format = NULL;

    while ((encoderStatus = AMediaCodec_dequeueOutputBuffer(ctx->encoder, &bufferInfo, 0)) >= 0) {
        size_t outSize;
        uint8_t *outBuffer = NULL;
        outBuffer = AMediaCodec_getOutputBuffer(ctx->encoder, encoderStatus, &outSize);

        av_assert0(outBuffer);
        av_log(avctx, AV_LOG_DEBUG, "Got codec specific data of size %d\n", bufferInfo.size);
        if ((ret = av_reallocp(&avctx->extradata,
                               avctx->extradata_size + bufferInfo.size +
                               AV_INPUT_BUFFER_PADDING_SIZE)) < 0) {
            goto fail;
        }
        memcpy(avctx->extradata + avctx->extradata_size, outBuffer, bufferInfo.size);
        avctx->extradata_size += bufferInfo.size;
        memset(avctx->extradata + avctx->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        AMediaCodec_releaseOutputBuffer(ctx->encoder, encoderStatus, false);
    }

    av_log(avctx, AV_LOG_DEBUG, "Finished init: %i\n", encoderStatus);

    return 0;

fail:
    if (format)
        AMediaFormat_delete(format);
    if (ctx->encoder)
        AMediaCodec_delete(ctx->encoder);
    return ret;
}

static int mediacodecndk_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
    MediaCodecNDKEncoderContext *ctx = avctx->priv_data;
    ssize_t bufferIndex = ctx->waiting_buffer;

    ctx->waiting_buffer = -1;
    if (bufferIndex < 0)
        bufferIndex = AMediaCodec_dequeueInputBuffer(ctx->encoder, 1000000);

    if (bufferIndex >= 0) {
        int ret = 0;
        if (!frame) {
            AMediaCodec_queueInputBuffer(ctx->encoder, bufferIndex, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            av_log(avctx, AV_LOG_DEBUG, "Queued EOS buffer %zi\n", bufferIndex);
        } else {
            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
            int i, nb_planes = 0, linesize[4], out_linesize[4];
            int align = ctx->is_rtk ? 16 : 1;
            uint32_t flags = 0;
            size_t bufferSize = 0;
            uint8_t *buffer = AMediaCodec_getInputBuffer(ctx->encoder, bufferIndex, &bufferSize);
            uint8_t *bufferEnd = buffer + bufferSize;

            if (!desc) {
                av_log(avctx, AV_LOG_ERROR, "Cannot get input pixdesc!\n");
                ret = AVERROR(EINVAL);
                goto fail;
            }

            if (!buffer) {
                av_log(avctx, AV_LOG_ERROR, "Cannot get input buffer!\n");
                return AVERROR_EXTERNAL;
            }

            for (i = 0; i < desc->nb_components; i++)
                nb_planes = FFMAX(desc->comp[i].plane + 1, nb_planes);

            ret = av_image_fill_linesizes(linesize, frame->format, frame->width);
            av_assert0(ret >= 0);
            ret = av_image_fill_linesizes(out_linesize, frame->format, FFALIGN(frame->width, align));
            av_assert0(ret >= 0);

            for (i = 0; i < nb_planes; i++) {
                int j;
                int shift_h = (i == 1 || i == 2) ? desc->log2_chroma_h : 0;
                const uint8_t *src = frame->data[i];
                int h = AV_CEIL_RSHIFT(frame->height, shift_h);

                if (buffer + (out_linesize[i] * (h - 1)) + linesize[i] > bufferEnd) {
                    av_log(avctx, AV_LOG_ERROR, "Buffer not large enough for input (%ix%i %s %zu)\n",
                           frame->width, frame->height, desc->name, bufferSize);
                    ret = AVERROR(EINVAL);
                    break;
                }

                for (j = 0; j < h; j++) {
                    memcpy(buffer, src, linesize[i]);
                    buffer += out_linesize[i];
                    src += frame->linesize[i];
                }

                buffer += out_linesize[i] * (FFALIGN(h, FFMAX(align >> shift_h, 1)) - h);
            }

            if (frame->pict_type == AV_PICTURE_TYPE_I)
                flags |= LOCAL_BUFFER_FLAG_SYNCFRAME;

fail:
            AMediaCodec_queueInputBuffer(ctx->encoder, bufferIndex, 0, bufferSize, av_rescale_q(frame->pts, avctx->time_base, AV_TIME_BASE_Q), flags);
            av_log(avctx, AV_LOG_DEBUG, "Queued input buffer %zi (flags=%i)\n", bufferIndex, flags);
        }
        return 0;
    } else if (bufferIndex == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        av_log(avctx, AV_LOG_WARNING, "No input buffers available\n");
        return AVERROR(EAGAIN);
    } else {
        av_log(avctx, AV_LOG_ERROR, "Unknown error in dequeueInputBuffer: %zi\n", bufferIndex);
        return AVERROR_EXTERNAL;
    }
}

static int mediacodecndk_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    int64_t timeout = avctx->internal->draining ? 1000000 : 0;
    MediaCodecNDKEncoderContext *ctx = avctx->priv_data;
    while (!ctx->saw_output_eos) {
        AMediaCodecBufferInfo bufferInfo;
        int encoderStatus = AMediaCodec_dequeueOutputBuffer(ctx->encoder, &bufferInfo, timeout);
        if (encoderStatus == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            // no output available yet
            av_log(avctx, AV_LOG_DEBUG, "No packets available yet\n");
            // This can mean either that the codec is starved and we need to send more
            // packets (EAGAIN), or that it's still working and we need to wait on it.
            // We can't tell which case it is, but if there are no input buffers
            // available, we at least know it shouldn't be starved, so try again
            // with a larger timeout in that case.
            if (ctx->waiting_buffer < 0 && !timeout) {
                ctx->waiting_buffer = AMediaCodec_dequeueInputBuffer(ctx->encoder, 0);
                if (ctx->waiting_buffer < 0) {
                    av_log(avctx, AV_LOG_VERBOSE, "Out of input buffers; waiting for output\n");
                    timeout = 1000000;
                    continue;
                }
            }
            return AVERROR(EAGAIN);
        } else if (encoderStatus == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            // should happen before receiving buffers, and should only happen once
            AMediaFormat *format = AMediaCodec_getOutputFormat(ctx->encoder);
            av_assert0(format);
            av_log(avctx, AV_LOG_DEBUG, "MediaCodec output format changed: %s\n",
                   AMediaFormat_toString(format));
            AMediaFormat_delete(format);
        } else if (encoderStatus == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            av_log(avctx, AV_LOG_DEBUG, "MediaCodec output buffers changed\n");
        } else if (encoderStatus < 0) {
            av_log(avctx, AV_LOG_WARNING, "Unknown MediaCodec status: %i\n", encoderStatus);
        } else {
            int ret;
            size_t outSize;
            uint8_t *outBuffer = AMediaCodec_getOutputBuffer(ctx->encoder, encoderStatus, &outSize);
            if (bufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                av_log(avctx, AV_LOG_DEBUG, "Got EOS at output\n");
                AMediaCodec_releaseOutputBuffer(ctx->encoder, encoderStatus, false);
                ctx->saw_output_eos = true;
                break;
            }

            av_assert0(outBuffer);
            if (bufferInfo.flags & LOCAL_BUFFER_FLAG_CODECCONFIG) {
                av_log(avctx, AV_LOG_DEBUG, "Got extradata of size %d\n", bufferInfo.size);
                if (ctx->new_extradata)
                    av_free(ctx->new_extradata);
                ctx->new_extradata = av_mallocz(bufferInfo.size + AV_INPUT_BUFFER_PADDING_SIZE);
                ctx->new_extradata_size = bufferInfo.size;
                if (!ctx->new_extradata) {
                    AMediaCodec_releaseOutputBuffer(ctx->encoder, encoderStatus, false);
                    av_log(avctx, AV_LOG_ERROR, "Failed to allocate extradata");
                    return AVERROR(ENOMEM);
                }
                memcpy(ctx->new_extradata, outBuffer, bufferInfo.size);
                AMediaCodec_releaseOutputBuffer(ctx->encoder, encoderStatus, false);
                continue;
            }

            if ((ret = ff_alloc_packet2(avctx, pkt, bufferInfo.size, bufferInfo.size) < 0)) {
                AMediaCodec_releaseOutputBuffer(ctx->encoder, encoderStatus, false);
                av_log(avctx, AV_LOG_ERROR, "Failed to allocate packet: %i\n", ret);
                return ret;
            }
            memcpy(pkt->data, outBuffer, bufferInfo.size);
            pkt->pts = av_rescale_q(bufferInfo.presentationTimeUs, AV_TIME_BASE_Q, avctx->time_base);
            pkt->dts = AV_NOPTS_VALUE;
            if (bufferInfo.flags & LOCAL_BUFFER_FLAG_SYNCFRAME)
                pkt->flags |= AV_PKT_FLAG_KEY;

            AMediaCodec_releaseOutputBuffer(ctx->encoder, encoderStatus, false);

            if (ctx->new_extradata) {
                ret = av_packet_add_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA,
                                              ctx->new_extradata,
                                              ctx->new_extradata_size);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to add extradata: %i\n", ret);
                    return ret;
                }
                ctx->new_extradata = NULL;
            }

            return 0;
        }
    }
    return AVERROR_EOF;
}

static av_cold int mediacodecndk_encode_close(AVCodecContext *avctx)
{
    MediaCodecNDKEncoderContext *ctx = avctx->priv_data;

    if (ctx->encoder) {
        AMediaCodec_stop(ctx->encoder);
        AMediaCodec_flush(ctx->encoder);
        AMediaCodec_delete(ctx->encoder);
    }

    return 0;
}

static const AVClass mediacodecndk_class = {
    .class_name = "h264_mediacodecndk_class",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_h264_mediacodecndk_encoder = {
    .name = "h264_mediacodecndk",
    .long_name = NULL_IF_CONFIG_SMALL("h264 (MediaCodec NDK)"),
    .type = AVMEDIA_TYPE_VIDEO,
    .id = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(MediaCodecNDKEncoderContext),
    .init = mediacodecndk_encode_init,
    .send_frame     = mediacodecndk_send_frame,
    .receive_packet = mediacodecndk_receive_packet,
    .close = mediacodecndk_encode_close,
    .capabilities = AV_CODEC_CAP_DELAY,
    .priv_class = &mediacodecndk_class,
    .pix_fmts = (const enum AVPixelFormat[]){
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
    },
};

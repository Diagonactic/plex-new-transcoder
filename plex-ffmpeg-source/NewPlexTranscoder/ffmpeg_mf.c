/*
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
#include <stdlib.h>

#include "libavutil/dict.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_mf.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "ffmpeg.h"

typedef struct MFContext {
    AVBufferRef *device_ref; // really AVHWDeviceContext*
    AVFrame *tmp_frame;
} MFContext;

static void mf_uninit(AVCodecContext *s)
{
    InputStream *ist = s->opaque;
    MFContext   *ctx = ist->hwaccel_ctx;

    av_frame_free(&ctx->tmp_frame);

    s->hwaccel_context = NULL;
    av_buffer_unref(&ctx->device_ref);
    av_freep(&ctx);
}

static int mf_retrieve_data(AVCodecContext *s, AVFrame *frame)
{
    InputStream        *ist = s->opaque;
    MFContext          *ctx = ist->hwaccel_ctx;
    int ret;

    ret = av_hwframe_transfer_data(ctx->tmp_frame, frame, 0);
    if (ret < 0)
        return ret;

    ret = av_frame_copy_props(ctx->tmp_frame, frame);
    if (ret < 0) {
        av_frame_unref(ctx->tmp_frame);
        return ret;
    }

    av_frame_unref(frame);
    av_frame_move_ref(frame, ctx->tmp_frame);

    return 0;
}

int mf_init(AVCodecContext *s)
{
    InputStream *ist = s->opaque;
    MFContext *ctx;
    AVHWDeviceContext *hwctx;
    AVMFDeviceContext *mf_hwctx;
    int ret;

    ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return AVERROR(ENOMEM);

    ctx->tmp_frame = av_frame_alloc();
    if (!ctx->tmp_frame) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    ctx->device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MF);
    if (!ctx->device_ref) {
        ret = AVERROR(ENOMEM);
        goto error;
    }
    if ((ret = av_hwdevice_ctx_init(ctx->device_ref)) < 0)
        goto error;

    hwctx = (void *)ctx->device_ref->data;
    mf_hwctx = hwctx->hwctx;
    if (mf_hwctx->d3d9_manager) {
        av_log(NULL, AV_LOG_INFO, "Using D3D9.\n");
    } else if (mf_hwctx->d3d11_manager) {
        av_log(NULL, AV_LOG_INFO, "Using D3D11.\n");
    } else {
        av_log(NULL, AV_LOG_WARNING, "Not actually using hardware decoding.\n");
    }

    s->hwaccel_context = ctx->device_ref;

    ist->hwaccel_ctx = ctx;
    ist->hwaccel_retrieve_data = mf_retrieve_data;
    ist->hwaccel_uninit = mf_uninit;

    return 0;

error:
    av_frame_free(&ctx->tmp_frame);
    av_buffer_unref(&ctx->device_ref);
    av_free(ctx);
    return ret;
}

/*
 * A generic frame drop bitstream filter
 * Copyright (c) 2017 Graham Booker <gbooker@plex.tv>
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

#include <string.h>

#include "avcodec.h"
#include "bsf.h"

#include "libavutil/opt.h"

typedef struct FrameDropBSFContext {
    const AVClass *class;
    int droppedFrameCount;
    int frameDropCount;
} FrameDropBSFContext;

static int framedrop_filter(AVBSFContext *ctx, AVPacket *out)
{
    FrameDropBSFContext *s = ctx->priv_data;
    AVPacket *in;
    int ret = 0;

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    if (s->droppedFrameCount < s->frameDropCount) {
        s->droppedFrameCount++;
        ret = AVERROR(EAGAIN);
        goto done;
    } else {
        av_packet_move_ref(out, in);
    }
    
done:
    av_packet_free(&in);
    
    return ret;
}

#define OFFSET(x) offsetof(FrameDropBSFContext, x)
static const AVOption options[] = {
    { "count", "How many leading frames to drop", OFFSET(frameDropCount), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM},
    { NULL }
};

static const AVClass framedrop_class = {
    .class_name = "framedrop bsf",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_MAJOR,
};

const AVBitStreamFilter ff_framedrop_bsf = {
    .name           = "framedrop",
    .priv_data_size = sizeof(FrameDropBSFContext),
    .priv_class     = &framedrop_class,
    .filter         = framedrop_filter,
};

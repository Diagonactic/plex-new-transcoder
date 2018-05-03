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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/internal.h"
#include "avcodec.h"
#include "internal.h"

static int decode_frame(AVCodecContext *avctx, void *data,
                        int *got_frame, AVPacket *avpkt)
{
    return -1;
}

static av_cold int decode_init(AVCodecContext *avctx)
{
    return -1;
}

AVCodec ff_fake_avi_decoder = {
    .name           = "fake_avi",
    .long_name      = NULL_IF_CONFIG_SMALL("AVI demuxer"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_BIN_DATA,
    .init           = decode_init,
    .decode         = decode_frame,
    .capabilities   = AV_CODEC_CAP_DR1,
};

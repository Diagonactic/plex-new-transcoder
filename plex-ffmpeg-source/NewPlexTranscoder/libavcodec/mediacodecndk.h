/*
 * Android MediaCodec NDK encoder/decoder
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

#ifndef AVCODEC_MEDIACODECNDK_H
#define AVCODEC_MEDIACODECNDK_H

#include "avcodec.h"

enum FFMediaCodecNDKColorFormat ff_mediacodecndk_get_color_format(enum AVPixelFormat lav);
enum AVPixelFormat ff_mediacodecndk_get_pix_fmt(enum FFMediaCodecNDKColorFormat ndk);
const char* ff_mediacodecndk_get_mime(enum AVCodecID codec_id);

int ff_mediacodecndk_init_binder(void);

#define BUFFER_FLAG_SYNCFRAME   1
#define BUFFER_FLAG_CODECCONFIG 2
#define BUFFER_FLAG_EOS         4

enum FFMediaCodecNDKColorFormat {
    COLOR_FormatYUV420Planar = 0x00000013,
    COLOR_FormatYUV420PackedPlanar = 0x00000014,
    COLOR_FormatYUV420SemiPlanar = 0x00000015,
    COLOR_FormatYUV420PackedSemiPlanar = 0x00000027,
    COLOR_FormatYUV422Flexible = 0x7f422888,
    COLOR_FormatYUV444Flexible = 0x7f444888,
    COLOR_FormatRGBFlexible = 0x7f36b888,
    COLOR_Format24bitBGR888 = 0x0000000c,
    COLOR_Format32bitABGR8888 = 0x7f00a000,
    COLOR_FormatRGBAFlexible = 0x7f36a888,
    COLOR_Format16bitRGB565 = 0x00000006,
    COLOR_FormatSurface = 0x7f000789,
};

#endif

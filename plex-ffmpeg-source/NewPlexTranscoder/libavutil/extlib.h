/*
 * copyright (c) 2016 Rodger Combs <rodger.combs@gmail.com>
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

#ifndef AVUTIL_EXTLIB_H
#define AVUTIL_EXTLIB_H

#include "libavcodec/avcodec.h"
#include "libavutil/avstring.h"

typedef struct FFLibrary {
    int is_master; // 0 for a component lib, 1 for e.g. libavcodec proper
    char *loaded_dso_list;
    void (*av_vlog)(void *avcl, int level, const char *fmt, va_list vl);
    char const *(*av_version_info)(void);

    // libavcodec (NULL for other libs)
    unsigned (*avcodec_version)(void);
    void (*av_register_codec_parser)(struct AVCodecParser *parser);
    void (*avcodec_register)(struct AVCodec *codec);
    void (*av_register_hwaccel)(struct AVHWAccel *hwaccel);
    struct AVHWAccel *(*av_hwaccel_next)(const struct AVHWAccel *hwaccel);
} FFLibrary;

typedef int (*AVInitLibrary)(FFLibrary* lib, int level);

int av_init_library(FFLibrary* lib, int level);

void avpriv_load_new_libs(FFLibrary* lib);

// laziness
static inline int ff_strcaseendswith(const char *s1, const char *s2)
{
    if (strlen(s1) < strlen(s2))
        return 0;
    return av_strcasecmp(s1 + strlen(s1) - strlen(s2), s2) == 0;
}

#endif /* AVUTIL_EXTLIB_H */

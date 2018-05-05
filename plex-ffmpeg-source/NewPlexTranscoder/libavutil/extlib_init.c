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

#include "avstring.h"
#include "log.h"
#include "extlib.h"
#include "version.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/internal.h"

extern int ff_is_master;

static void register_component(FFLibrary *lib)
{
#if defined(BUILDING_EXTERNAL_DECODER) || defined(BUILDING_EXTERNAL_ENCODER)
    extern AVCodec EXTLIBNAME;
    lib->avcodec_register(&EXTLIBNAME);
#else
#error "Unknown extlib type"
#endif
}

int av_init_library(FFLibrary* lib, int level)
{
    static int already_loaded = 0;
    unsigned our_version, lib_version;

    av_log_set_callback(lib->av_vlog);

    if (already_loaded) {
        av_log(NULL, AV_LOG_VERBOSE, "Library already loaded.\n");
        return -1;
    }

    already_loaded = 1;

    ff_is_master = 0;

#ifdef FFLIB_avcodec
    if (lib->avcodec_version) {
        our_version = avcodec_version();
        lib_version = lib->avcodec_version();
#else
#error "Unknown fflib type"
#endif
    } else {
        // Not the same library kind (e.g. libavcodec and libavformat)
        av_log(NULL, AV_LOG_VERBOSE, "Wrong library type.\n");
        return -1;
    }
    if (our_version != lib_version) {
        av_log(NULL, level, "Incompatible library versions.\n");
        return -1;
    }
    if (strcmp(av_version_info(), lib->av_version_info()) != 0) {
        av_log(NULL, level, "Incompatible FFmpeg versions: '%s' vs. '%s'.\n",
               av_version_info(), lib->av_version_info());
        return -1;
    }
    register_component(lib);
    return 0;
}

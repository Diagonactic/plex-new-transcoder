/*
 * Copyright (c) 2016 Plex, Inc.
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

#ifndef FFTOOLS_PLEX_H
#define FFTOOLS_PLEX_H

#include "config.h"
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <unistd.h>
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/colorspace.h"
#include "libavutil/fifo.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/dict.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avstring.h"
#include "libavutil/libm.h"
#include "libavformat/os_support.h"

#include "libavfilter/avfilter.h"

#if HAVE_SYS_RESOURCE_H
#include <sys/types.h>
#include <sys/time.h>
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

#if HAVE_TERMIOS_H
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#elif HAVE_KBHIT
#include <conio.h>
#endif
#include <time.h>

#include "cmdutils.h"
#include "ffmpeg.h"

#include <stdio.h>

typedef struct
{
    int file_index;
    int stream_index;
    AVFilterContext *ctx;
} InlineAssContext;

typedef struct
{
    int64_t input_start_time;           //[+]

    int64_t output_duration;            //[+]
    char* progress_url;                 //[-]
    int throttle_delay;

    int nb_inlineass_ctxs;
    InlineAssContext *inlineass_ctxs;

    int packets_in;
    int hwaccel_failed;
    int hwaccel_succeeded;
    int sw_failed;
    int sw_succeeded;
} PlexContext;

extern PlexContext plexContext;

typedef enum LogLevel
{
   LOG_LEVEL_ERROR,
   LOG_LEVEL_WARNING,
   LOG_LEVEL_INFO,
   LOG_LEVEL_DEBUG,
   LOG_LEVEL_VERBOSE,
} LogLevel;

char* PMS_IssueHttpRequest(const char* url, const char* verb);
void PMS_Log(LogLevel level, const char* format, ...);

void plex_init(void);
int av_log_get_level_plex(void);
void av_log_set_level_plex(int);

void plex_report_stream(const AVStream *st);
void plex_report_stream_detail(const AVStream *st);

int plex_opt_subtitle_stream(void *optctx, const char *opt, const char *arg);

int plex_opt_progress_url(void *optctx, const char *opt, const char *arg);
int plex_opt_loglevel(void *o, const char *opt, const char *arg);

void plex_feedback(const AVFormatContext *ic);

void plex_prepare_setup_streams_for_input_stream(InputStream* ist);
void plex_link_subtitles_to_graph(AVFilterGraph* graph);
int plex_process_subtitles(const InputStream *ist, AVSubtitle *sub);
void plex_link_input_stream(const InputStream *ist);

#endif

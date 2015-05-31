/*
 *  vf_inlinepgs.h
 *  ffmpeg
 *
 *  Created by Frank Bauer on 2010/11/29.
 *  Copyright 2010 Plex Inc. All rights reserved.
 *
 */

#pragma once
#include "libavfilter/avfilter.h"
#include "libavformat/avformat.h"

typedef struct InlinePGSContext{
    AVStream  *subtitleStream;
    AVCodec   *subtitleCodec;
    AVFrame   *subtitleFrame;
    AVFormatContext* formatContext;
    
    int current_pts;
} InlinePGSContext;

void vf_inlinepgs_set_subtitle_stream(AVFormatContext *fctx, AVFilterContext *ctx, AVStream *stream);


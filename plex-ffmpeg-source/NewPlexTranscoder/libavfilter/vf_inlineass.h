/*
 * inlineass filter
 * Copyright (c) 2016 Rodger Combs
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

#ifndef AVFILTER_INLINEASS_H
#define AVFILTER_INLINEASS_H

#include "avfilter.h"
#include "libavcodec/avcodec.h"

void avfilter_inlineass_process_header(AVFilterContext *link,
                                       AVCodecContext *dec_ctx);
void avfilter_inlineass_append_data(AVFilterContext *link, AVCodecContext *dec_ctx,
                                    AVSubtitle *sub);
void avfilter_inlineass_set_aspect_ratio(AVFilterContext *context, double dar);
void avfilter_inlineass_add_attachment(AVFilterContext *context, AVStream *st);
void avfilter_inlineass_set_fonts(AVFilterContext *context);
void avfilter_inlineass_set_storage_size(AVFilterContext *context, int w, int h);

#endif // AVFILTER_INLINEASS_H

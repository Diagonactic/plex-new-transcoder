/*
 * HTTP protocol for ffmpeg client
 * Copyright (c) 2014 Rodger Combs
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

#ifndef AVFORMAT_PLEX_HTTP_H
#define AVFORMAT_PLEX_HTTP_H

typedef enum LogLevel
{
   LOG_LEVEL_ERROR,
   LOG_LEVEL_WARNING,
   LOG_LEVEL_INFO,
   LOG_LEVEL_DEBUG,
   LOG_LEVEL_VERBOSE,
} LogLevel;

void ff_http_set_userAgent(const char* myAgent);
void ff_http_set_cookie(const char* myCookie);
char* PMS_IssueHttpRequest(const char* url, const char* verb);
void PMS_Log(LogLevel level, const char* format, ...);

#endif

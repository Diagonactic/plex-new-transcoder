/*
 * HTTP protocol for ffmpeg client
 * Copyright (c) 2009 Elan Feingold
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

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "plex_http.h"

#include "libavformat/avio.h"
#include "libavutil/dict.h"
#include "libavutil/avstring.h"

//#define DEBUG 1

char* HTTP_USER_AGENT = 0;
char* HTTP_COOKIES = 0;

void ff_http_set_userAgent(const char* myAgent)
{
  HTTP_USER_AGENT = (char* )myAgent;
}

void ff_http_set_cookie(const char* myCookie)
{
  HTTP_COOKIES = (char* )myCookie;
}

char* PMS_IssueHttpRequest(const char* url, const char* verb)
{
    char* reply = NULL;
    AVIOContext *ioctx = NULL;
    AVDictionary *settings = NULL;
    int size = 0;
    int ret;

    av_dict_set(&settings, "user_agent", HTTP_USER_AGENT, 0);
    av_dict_set(&settings, "cookies", HTTP_COOKIES, 0);
    av_dict_set(&settings, "method", verb, 0);
    av_dict_set(&settings, "timeout", "1000000", 0);

    ret = avio_open2(&ioctx, url, AVIO_FLAG_READ,
                     NULL,
                     &settings);

    if (ret < 0)
        goto fail;

    size = avio_size(ioctx);
    if (size < 0)
        size = 4096;
    else if (!size)
        goto fail;

    reply = av_malloc(size);

    ret = avio_read(ioctx, reply, size);

    if (ret < 0)
        *reply = 0;

    avio_close(ioctx);
    av_dict_free(&settings);
    return reply;

fail:
    avio_close(ioctx);
    av_dict_free(&settings);
    reply = av_malloc(1);
    *reply = 0;
    return reply;
}

void PMS_Log(LogLevel level, const char* format, ...)
{
    // Format the mesage.
    char msg[2048];
    char tb[256];
    char url[4096];
    va_list va;
    va_start(va, format);
    vsnprintf(msg, sizeof(msg), format, va);
    va_end(va);

    // Build the URL.
    snprintf(url, sizeof(url), "http://127.0.0.1:32400/log?level=%d&source=Transcoder&message=", level < 0 ? 0 : level);

    for (int i = 0; i < 256; i++) {
        tb[i] = isalnum(i)||i == '*'||i == '-'||i == '.'||i == '_'
            ? i : (i == ' ') ? '+' : 0;
    }
    for (unsigned i = 0; msg[i] && i < sizeof(msg); i++) {
        if (msg[i] > 0 && tb[(int)msg[i]])
            av_strlcatf(url, sizeof(url), "%c", tb[(int)msg[i]]);
        else
            av_strlcatf(url, sizeof(url), "%%%02X", msg[i]);
    }

    // Issue the request.
    char* reply = PMS_IssueHttpRequest(url, "GET");
    av_free(reply);
}

/*
 * Copyright (c) 2013 Matthew Heaney
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

/**
 * @file
 * WebVTT subtitle muxer
 * @see http://dev.w3.org/html5/webvtt/
 */

#include "avformat.h"
#include "internal.h"
//PLEX
#include "libavutil/opt.h"
#include <float.h>
//PLEX

//PLEX
typedef struct {
    const AVClass  *class;
    float           sync_vtt;
    int64_t         sync_mpeg;
} WebVTTMuxContext;
//PLEX

static void webvtt_write_time(AVIOContext *pb, int64_t millisec)
{
    int64_t sec, min, hour;
    sec = millisec / 1000;
    millisec -= 1000 * sec;
    min = sec / 60;
    sec -= 60 * min;
    hour = min / 60;
    min -= 60 * hour;

    if (hour > 0)
        avio_printf(pb, "%"PRId64":", hour);

    avio_printf(pb, "%02"PRId64":%02"PRId64".%03"PRId64"", min, sec, millisec);
}

static int webvtt_write_header(AVFormatContext *ctx)
{
    AVStream     *s = ctx->streams[0];
    AVIOContext *pb = ctx->pb;
    WebVTTMuxContext *priv = (WebVTTMuxContext*)ctx->priv_data; //PLEX

    avpriv_set_pts_info(s, 64, 1, 1000);

    avio_printf(pb, "WEBVTT\n");
    //PLEX
    avio_printf(pb, "X-TIMESTAMP-MAP=LOCAL:");
    webvtt_write_time(pb, priv->sync_vtt * 1000);
    avio_printf(pb, ",MPEGTS:%02"PRId64"\n", priv->sync_mpeg);
    //PLEX
    avio_flush(pb);

    return 0;
}

static int webvtt_write_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    AVIOContext  *pb = ctx->pb;
    int id_size, settings_size;
    uint8_t *id, *settings;

    avio_printf(pb, "\n");

    id = av_packet_get_side_data(pkt, AV_PKT_DATA_WEBVTT_IDENTIFIER,
                                 &id_size);

    if (id && id_size > 0)
        avio_printf(pb, "%.*s\n", id_size, id);

    webvtt_write_time(pb, pkt->pts);
    avio_printf(pb, " --> ");
    webvtt_write_time(pb, pkt->pts + pkt->duration);

    settings = av_packet_get_side_data(pkt, AV_PKT_DATA_WEBVTT_SETTINGS,
                                       &settings_size);

    if (settings && settings_size > 0)
        avio_printf(pb, " %.*s", settings_size, settings);

    avio_printf(pb, "\n");

    avio_write(pb, pkt->data, pkt->size);
    avio_printf(pb, "\n");

    return 0;
}

//PLEX
#define OFFSET(x) offsetof(WebVTTMuxContext, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "sync_vtt",  "Specifies a particular WebVTT timestamp for the sync header.", OFFSET(sync_vtt),  AV_OPT_TYPE_FLOAT, { .dbl = 0      }, 0, FLT_MAX,   FLAGS },
    { "sync_mpeg", "Specifies a particular MPEGTS timestamp for the sync header.", OFFSET(sync_mpeg), AV_OPT_TYPE_INT64, { .i64 = 900000 }, 0, INT64_MAX, FLAGS },
    { NULL },
};

static const AVClass webvtt_class = {
    .class_name = "WebVTT muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
//PLEX

AVOutputFormat ff_webvtt_muxer = {
    .name              = "webvtt",
    .long_name         = NULL_IF_CONFIG_SMALL("WebVTT subtitle"),
    .extensions        = "vtt",
    .mime_type         = "text/vtt",
    .priv_data_size    = sizeof(WebVTTMuxContext), //PLEX
    .priv_class        = &webvtt_class, //PLEX
    .flags             = AVFMT_VARIABLE_FPS | AVFMT_TS_NONSTRICT,
    .subtitle_codec    = AV_CODEC_ID_WEBVTT,
    .write_header      = webvtt_write_header,
    .write_packet      = webvtt_write_packet,
};

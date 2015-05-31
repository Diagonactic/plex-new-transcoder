/*
 * Plex Media Server muxer
 * Copyright (c) 2011 Elan Feingold
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

#include "avformat.h"
#include "avio_internal.h"
#include "riff.h"
#include "avio.h"
#include "isom.h"
#include "avc.h"
#include "internal.h"
#include "libavutil/avstring.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/dict.h"
#include "movenc.h"

#undef NDEBUG
#include <assert.h>

#define MOV_CLASS(flavor)\
static const AVClass flavor ## _muxer_class = {\
    .class_name = #flavor " muxer",\
    .item_name  = av_default_item_name,\
    .option     = options,\
    .version    = LIBAVUTIL_VERSION_INT,\
};

typedef struct PMSMuxContext
{
  const AVClass *av_class;
  MOVTrack *tracks;
  int       have_written_header;
  int       timebase;
} PMSMuxContext;

static const AVOption options[] = {
    {"timebase", "set timebase", offsetof(PMSMuxContext, timebase), FF_OPT_TYPE_INT, { 0 }, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM},
    { NULL },
};

static char* hex2str(char* dst, unsigned char const* start, unsigned char const* end)
{
  static const char* hexDigits = "0123456789ABCDEF";

  for (; start != end; start++)
  {
    *dst++ = hexDigits[(*start >> 4) & 0xF];
    *dst++ = hexDigits[(*start >> 0) & 0xF];
  }

  return dst;
}

static int pms_write_header(AVFormatContext *s)
{
  AVIOContext* pb = s->pb;
  PMSMuxContext *mov = s->priv_data;
  
  // Allocate tracks.
  mov->tracks = av_mallocz(s->nb_streams*sizeof(*mov->tracks));

  AVDictionaryEntry* pDuration = av_dict_get(s->metadata, "plex.total_duration", NULL, 0);
  if (pDuration)
  {
    double duration = atof(pDuration->value);
    avio_printf(pb, "Duration: %g\n", duration);
  }

  pDuration = av_dict_get(s->metadata, "plex.duration", NULL, 0);
  if (pDuration)
  {
    double duration = atof(pDuration->value);
    avio_printf(pb, "Segment-Duration: %g\n", duration);
  }

  // Adjust timescales.
  for(int i=0; i<s->nb_streams; i++)
  {
    AVStream *st = s->streams[i];
    MOVTrack *track = &mov->tracks[i];
    int timescale = 0;
    
    track->enc = st->codec;

    // Set the timebase if we have a specific requirement.
    if (mov->timebase != 0)
    {
      avpriv_set_pts_info(st, 64, 1, mov->timebase);
    }
    else
    {
      if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO)
        timescale = st->codec->time_base.den;
      else if(st->codec->codec_type == AVMEDIA_TYPE_AUDIO)
        timescale = st->codec->sample_rate;
      
      avpriv_set_pts_info(st, 64, 1, timescale);
    }
    
    if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO)
      avio_printf(pb, "Stream-%d-Resolution: %d %d\n", i, st->codec->width, st->codec->height);
    else if (st->codec->codec_type == AVMEDIA_TYPE_AUDIO)
      avio_printf(pb, "Stream-%d-Channels: %d\n", i, st->codec->channels);
    
    int g = av_gcd(st->time_base.num, st->time_base.den);
    avio_printf(pb, "Stream-%d-Timebase: %d %d\n", i, st->time_base.num/g, st->time_base.den/g);
 
    // Extra data.
    if (st->codec->extradata != 0x0)
    {
      // See if we need to convert to Annex B format.
      if (st->codec->codec_id == CODEC_ID_H264 && st->codec->extradata_size > 8 &&
         (st->codec->extradata[0] == 0x00 && st->codec->extradata[1] == 0x00 && st->codec->extradata[2] == 0x00 && st->codec->extradata[3] == 0x01) == 0)
      {
        char str[2048];
        char sizePPS = 0;

        // Must convert to Annex B.
        avio_printf(pb, "Stream-%d-ExtraData: 00000001", i);

        // SPS.
        size_t sizeSPS = (st->codec->extradata[6] << 8 | st->codec->extradata[7]);
        char* p = hex2str(str, st->codec->extradata + 8, st->codec->extradata + 8 + sizeSPS);
        *p = '\0';
        avio_printf(pb, "%s00000001", str);

        // PPS.
        sizePPS = st->codec->extradata[8 + sizeSPS + 1] << 8 | st->codec->extradata[8 + sizeSPS + 2];
        p = hex2str(str, st->codec->extradata + 8 + sizeSPS + 3, st->codec->extradata + 8 + sizeSPS + 3 + sizePPS);
        *p = '\0';
        avio_printf(pb, "%s\n", str);
      }
      else
      {
        // Annex B format or raw.
        char str[2048];
        char* p = hex2str(str, st->codec->extradata, st->codec->extradata + st->codec->extradata_size);
        *p = '\0';
        avio_printf(pb, "Stream-%d-ExtraData: %s\n", i, str);
      }
    }
  }
 
  avio_flush(pb);
  return 0;
}

static int pms_write_packet(AVFormatContext *s, AVPacket *pkt)
{
  PMSMuxContext *mov = s->priv_data;
  AVIOContext *pb = s->pb;
  MOVTrack *trk =  &mov->tracks[pkt->stream_index];
  AVCodecContext *enc = trk->enc;
  int size = pkt->size;
 
  if (!size) return 0; /* Discard 0 sized packets */

  /* Write the sample aspect data if we haven't already (need a packet for it to work) */
  if (mov->have_written_header == 0)
  {
    mov->have_written_header = 1;
    
    for(int i=0; i<s->nb_streams; i++)
    {
      AVStream *st = s->streams[i];
      if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO)
      {
        if (st->codec->sample_aspect_ratio.den && st->codec->sample_aspect_ratio.num &&
            st->codec->sample_aspect_ratio.den != st->codec->sample_aspect_ratio.num)
        {
          avio_printf(pb, "Stream-%d-SampleAspectRatio: %d %d\n", i, st->codec->sample_aspect_ratio.num, st->codec->sample_aspect_ratio.den);
        }
      }
    }
    
    avio_printf(pb, "\n");
  }

  /* copy extradata if it exists */
  if (trk->vos_len == 0 && enc->extradata_size > 0) 
  {
    trk->vos_len = enc->extradata_size;
    trk->vos_data = av_malloc(trk->vos_len);
    memcpy(trk->vos_data, enc->extradata, trk->vos_len);
  }
  
  avio_printf(pb, "Stream: %d\n", pkt->stream_index);
  avio_printf(pb, "Duration: %d\n", pkt->duration);

  if (enc->codec_id == CODEC_ID_H264 && trk->vos_len > 0 && *(uint8_t *)trk->vos_data != 1) 
  {
    /* from x264 or from bytestream h264 */
    /* nal reformating needed */
    AVIOContext* buf = 0;
    uint8_t *pbuffer = 0;
    
    avio_open_dyn_buf(&buf);
    size = ff_avc_parse_nal_units(buf, pkt->data, pkt->size);
    avio_close_dyn_buf(buf, &pbuffer);

    avio_printf(pb, "Content-Length: %d\n\n", size);
    avio_write(pb, pbuffer, size);
    av_free(pbuffer);
  } 
  else 
  {
    avio_printf(pb, "Content-Length: %d\n\n", size);
    avio_write(pb, pkt->data, size);
  }
  
  avio_flush(pb);
  return 0;
}

static int pms_write_trailer(AVFormatContext *s)
{
    PMSMuxContext *mov = s->priv_data;
    AVIOContext *pb = s->pb;

    avio_printf(pb, "Content-Length: 0\n\n");
    avio_flush(pb);
    
    av_freep(&mov->tracks);

    return 0;
}

MOV_CLASS(pms)
AVOutputFormat ff_pms_muxer = {
    .name              = "pms",
    .long_name         = NULL_IF_CONFIG_SMALL("PMS format"),
    .extensions        = "pms",
    .priv_data_size    = sizeof(PMSMuxContext),
    .audio_codec       = CODEC_ID_AAC,
    .video_codec       = CONFIG_LIBX264_ENCODER ?
                         AV_CODEC_ID_H264 : AV_CODEC_ID_MPEG4,
    .write_header      = pms_write_header,
    .write_packet      = pms_write_packet,
    .write_trailer     = pms_write_trailer,
    .flags             = AVFMT_GLOBALHEADER,
    .codec_tag = (const AVCodecTag* const []){ff_codec_movvideo_tags, ff_codec_movaudio_tags, 0},
    .priv_class = &pms_muxer_class,
};

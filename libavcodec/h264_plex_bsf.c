/*
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

#include "avcodec.h"
#include "h264.h"

typedef struct H264PlexContext 
{
  uint8_t bs_type;
} H264PlexContext;

static int find_next_nal_annexb(const uint8_t *src, int *p, int size)
{
    int i = 0;
    int j = *p;
    for (;j<size;j++) {
        uint8_t t = src[j];
        if (i == 2) {
            if (t == 1) {
                *p = j + 1;
                return src[j+1] & 0x1f;
            } else if (t)
                i = 0;
        } else if (t == 0)
            i++;
        else
            i = 0;
    }
    return 0;
}

static int ff_h264_plex_filter(AVBitStreamFilterContext *bsfc,
                                 AVCodecContext *avctx, const char *args,
                                 uint8_t **poutbuf, int *poutbuf_size,
                                 const uint8_t *buf, int      buf_size,
                                 int keyframe)
{
    H264PlexContext *ctx = bsfc->priv_data;

    *poutbuf = buf;
    *poutbuf_size = buf_size;

    if (avctx->codec_id != CODEC_ID_H264)
        return 0;

    // check bitstream type
    if (!ctx->bs_type) {
        if (buf && buf_size > 5) {
            uint32_t t = AV_RB32(buf);
            ctx->bs_type = 2;
            if (t == 1)
                ctx->bs_type = 1;
            else if ((t & 0xFFFFFF00) == 0x0100) {
                int i = 4;
                if (find_next_nal_annexb(buf, &i, buf_size) > 0)
                    ctx->bs_type = 1;
            }
        }
        else
            return 0;
    }

    int needmove = 0;
    uint32_t i = 0, b = 0;
    if (ctx->bs_type == 1) 
    {
        uint32_t t, j = 0;
        int skip = 0;
        
        while ((t = find_next_nal_annexb(buf, &i, buf_size))) 
        {
          if (!skip)
          {
            if (needmove)
                memmove(buf + b, buf + j, i - j);
            b += i - j;
          }
          
          if (t == NAL_SEI && buf[b+1] == SEI_TYPE_USER_DATA_UNREGISTERED)
          {
            skip = 1;
            needmove = 1;
          } 
          else
          {
            skip = 0;
          }
          
          j = i;
        }
        i = j;
    } 

    if (needmove) 
    {
        memmove(buf + b, buf + i, buf_size - i);
        buf_size -= i - b;
        *poutbuf_size = buf_size;
    }

    return 0;
}

AVBitStreamFilter ff_h264_plex_bsf = {
    "h264_plex",
    sizeof(H264PlexContext),
    ff_h264_plex_filter,
};

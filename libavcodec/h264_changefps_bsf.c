/*
 * H.264 change fps filter
 *
 * Simplified/adapted version of:
 * H.264 change sps filter
 * Copyright (c) 2010 Zongyi Zhou <zhouzy@os.pku.edu.cn>
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

#ifdef __linux__
#include <alloca.h>
#endif

#ifdef __WIN32__
#include <malloc.h>
#define alloca _alloca
#endif

#include "avcodec.h"
#include "golomb.h"

// NAL unit types, copied from h264.h
enum {
    NAL_SLICE=1,
    NAL_DPA,
    NAL_DPB,
    NAL_DPC,
    NAL_IDR_SLICE,
    NAL_SEI,
    NAL_SPS,
    NAL_PPS,
    NAL_AUD,
    NAL_END_SEQUENCE,
    NAL_END_STREAM,
    NAL_FILLER_DATA,
    NAL_SPS_EXT,
    NAL_AUXILIARY_SLICE=19
};

typedef struct H264FPSContext {
    uint8_t state;
    uint8_t bs_type; // 1=annexb 2=mp4
    int8_t fps_mode;
    int32_t fps_den, fps_num;
    int32_t level;
} H264FPSContext;

static int parse_args(struct H264FPSContext *c, const char *args)
{
    int r = 0;
    int32_t level = -1;
    if (!args) return 0;
    c->fps_den = c->fps_num = -1;
    c->fps_mode = 1;
    while (*args) {
        if (sscanf(args, "fps=%u:%u", &c->fps_num, &c->fps_den) == 2 ||
            sscanf(args, "level=%u", &level) == 1)
                r++;
        else if (!strncmp(args, "vfr", 3)) {
            r++;
            c->fps_mode = 0;
        } else if (!strncmp(args, "cfr", 3)) {
            r++;
            c->fps_mode = 1;
        }
        if (!(args = strchr(args, '/'))) break;
        args++;
    }
    c->level = level;

    return r;
}

static void skip_hrd_param(GetBitContext *pgb)
{
    int t = get_ue_golomb_31(pgb);
    skip_bits(pgb, 8);
    for (t++;t;t--) {
        get_ue_golomb(pgb);
        get_ue_golomb(pgb);
        skip_bits1(pgb);
    }
    skip_bits(pgb, 20);
}

static int nal_enc(uint8_t *dst, const uint8_t *src, int size)
{
    int i = 0;
    uint8_t *dst_start = dst;
    for(;size;size--) {
        if( i == 2 && *src <= 3 ) {
            *dst++ = 3;
            i = 0;
        }
        if (*src == 0)
            i++;
        else
            i = 0;
        *dst++ = *src++;
    }
    return dst - dst_start;
}

static int nal_dec(uint8_t *dst, const uint8_t *src, int size)
{
    int i = 0;
    uint8_t *dst_start = dst;
    for(;size;size--) {
        uint8_t t = *src++;
        if (i == 2) {
            i = 0;
            if (t == 3)
                continue;
        }
        if (t == 0)
            i++;
        else
            i = 0;
        *dst++ = t;
    }
    return dst - dst_start;
}

static void sl_copy(GetBitContext *pgb, PutBitContext *ppb, int size)
{
    int delta, next = 8, j;
    for(j = 0;j < size && next;j++) {
        delta = get_se_golomb(pgb);
        set_se_golomb(ppb, delta);
        next = (next + delta + 256) & 255;
    }
}

static int h264_modify(uint8_t *outbuf, const uint8_t *inbuf, H264FPSContext *ctx, int insize)
{
    GetBitContext gb;
    PutBitContext pb;
#define COPYUE set_ue_golomb(&pb, get_ue_golomb(&gb))
#define COPYUE31 set_ue_golomb(&pb, get_ue_golomb_31(&gb))
#define COPYSE set_se_golomb(&pb, get_se_golomb(&gb))
#define COPYBITS1 put_bits(&pb, 1, get_bits1(&gb))
    int p, t, i;

    init_get_bits(&gb, inbuf, insize * 8);
    init_put_bits(&pb, outbuf, (insize + 10) * 8);
    p = get_bits(&gb, 8); //profile_idc
    put_bits(&pb, 8, p);
    put_bits(&pb, 8, get_bits(&gb, 8)); //constraint_set
    t = get_bits(&gb, 8); //level_idc
    if (ctx->level != -1)
        t = ctx->level;
    put_bits(&pb, 8, t);
    COPYUE31;
    if (p >= 100) {
        t = get_ue_golomb(&gb); //chroma_format_idc
        set_ue_golomb(&pb, t);
        if (t == 3)
            COPYBITS1; //residue_transform_flag
        COPYUE; //bit_depth_luma_minus8
        COPYUE; //bit_depth_chroma_minus8
        COPYBITS1; //qpprime_y_zero_transform_bypass_flag
        t = get_bits1(&gb); //seq_scaling_matrix_present_flag
        put_bits(&pb, 1, t);
        if (t) {
            //copy scaling list
            for (i = 0;i < 8;i++) {
                t = get_bits1(&gb);
                put_bits(&pb, 1, t);
                if (t)
                    sl_copy(&gb, &pb, i < 6? 16 : 64);
            }
        }
    }
    COPYUE; //log2_max_frame_num-4
    t = get_ue_golomb_31(&gb); //poc_type
    set_ue_golomb(&pb, t);
    if (t == 0) COPYUE; //log2_max_poc_lsb
    else if (t == 1) {
        COPYBITS1; //delta_pic_order_always_zero
        COPYSE; //offset_for_non_ref_pic
        COPYSE; //offset_for_top_to_bottom_field
        t = get_ue_golomb(&gb); //num_ref_frames_in_poc_cycle
        set_ue_golomb(&pb, t);
        for (;t;t--) COPYUE;
    }
    //num_ref_frames
    t = get_ue_golomb_31(&gb);
    set_ue_golomb(&pb,t);
    COPYBITS1;
    COPYUE;
    COPYUE;
    i = get_bits1(&gb); //frame_mbs_only
    put_bits(&pb, 1, i);
    if (!i) COPYBITS1;
    COPYBITS1;

    t = get_bits1(&gb);
    put_bits(&pb, 1, t);
    if (t) {
        int t1, t2, t3, t4;
        t1 = get_ue_golomb(&gb),
        t2 = get_ue_golomb(&gb),
        t3 = get_ue_golomb(&gb),
        t4 = get_ue_golomb(&gb);
        set_ue_golomb(&pb, t1);
        set_ue_golomb(&pb, t2);
        set_ue_golomb(&pb, t3);
        set_ue_golomb(&pb, t4);
    }
    t = get_bits1(&gb);
    put_bits(&pb, 1, t);
    if (t) {
        int ch = 0;
        t = get_bits1(&gb);
        put_bits(&pb, 1, t);
        if (t) {
            t = get_bits(&gb, 8);
            put_bits(&pb, 8, t);
            if (t == 255) {
                t = get_bits(&gb, 16);
                i = get_bits(&gb, 16);
                put_bits(&pb, 16, t);
                put_bits(&pb, 16, i);
            }
        }

        t = get_bits1(&gb); //overscan_info_present_flag
        put_bits(&pb, 1, t);
        if (t) COPYBITS1;

        p = ch = -1; i = 0;
        t = get_bits1(&gb); //video_signal_type_present_flag
        if (t) {
            p = get_bits(&gb, 4); //video_format + video_full_range_flag
            i = get_bits1(&gb); //colour_description_present_flag
            if (i)
                ch = get_bits(&gb, 24);
        }
        if (p != -1 || ch != -1)
            t = 1;
        put_bits(&pb, 1, t);
        if (t) {
            if (p == -1)
                p = 5; //undef
            put_bits(&pb, 4 ,p);
            if (ch != -1)
                i = 1;
            put_bits(&pb, 1, i);
            if (i)
                put_bits(&pb, 24, ch);
        }

        t = get_bits1(&gb); //chroma_location_info_present_flag
        put_bits(&pb, 1, t);
        if (t) {
            COPYUE;
            COPYUE;
        }

        t = get_bits1(&gb); //timing_info_present_flag
        ch = ctx->fps_den != -1;
        if (t) {
            t = get_bits_long(&gb, 32),
            i = get_bits_long(&gb, 32);
            p = get_bits1(&gb);
        }
        if (ctx->fps_mode >= 0)
            p = ctx->fps_mode;
        if (ch) {
            t = ctx->fps_den;
            i = ctx->fps_num * 2;
        }
        if (t || p >= 0) {
            put_bits(&pb, 1, 1);
            put_bits(&pb, 32, t);
            put_bits(&pb, 32, i);
            put_bits(&pb, 1, p);
        } else put_bits(&pb, 1, 0);
        p = get_bits_count(&gb);
        i = get_bits1(&gb); //nal_hrd_parameters_present_flag
        if (i) skip_hrd_param(&gb);
        t = get_bits1(&gb); //vcl_hrd_parameters_present_flag
        if (t) skip_hrd_param(&gb);
        if (i || t) skip_bits1(&gb); //low_delay_hrd_flag
        p = get_bits_count(&gb) - p;
        skip_bits_long(&gb, -p);
        for (;p > 24;p -= 24)
            put_bits(&pb, 24, get_bits(&gb, 24));
        put_bits(&pb, p, get_bits(&gb, p));
        COPYBITS1; //pic_struct_present_flag
        t = get_bits1(&gb); //bitstream_restriction_flag
        put_bits(&pb, 1, t);
        if (t) {
            COPYBITS1;
            COPYUE;
            COPYUE;
            COPYUE;
            COPYUE;
            COPYUE;
            COPYUE;
        }
    }
    COPYBITS1; //rbsp trailing
    t = get_bits_count(&gb);
    flush_put_bits(&pb);
        t = (t + 7) >> 3;
    i = put_bits_count(&pb) >> 3;
    if (insize > t)
        memcpy(outbuf + i, inbuf + t, insize - t);
    return insize + i - t;
}

int find_next_nal_annexb(const uint8_t *src, int *p, int size);
int find_next_nal_annexb(const uint8_t *src, int *p, int size)
{
    int i = 0;
    int j = *p;
    for(;j<size;j++) {
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

static int h264_changefps_filter(AVBitStreamFilterContext *bsfc,
                                 AVCodecContext *avctx, const char *args,
                                 uint8_t  **poutbuf, int *poutbuf_size,
                                 const uint8_t *buf, int      buf_size,
                                 int keyframe)
{
    H264FPSContext *ctx = bsfc->priv_data;
    static const uint8_t profile_level[256] = {
        [66]  = 1, [77]  = 1, [88]  = 1, [100] = 1,
        [110] = 1, [122] = 1, [144] = 1, [244] = 1,
        [10] = 2, [11] = 2, [12] = 2, [13] = 2, [16] = 2, [20] = 2,
        [21] = 2, [22] = 2, [30] = 2, [31] = 2, [32] = 2, [40] = 2,
        [41] = 2, [42] = 2, [50] = 2, [51] = 2
    };

    uint32_t spslen = 0;

    *poutbuf = buf;
    *poutbuf_size = buf_size;

    if (avctx->codec_id != CODEC_ID_H264)
        return 0;

    if (!ctx->state) {
        if (parse_args(ctx, args)) ctx->state = 1;
        else {
            ctx->state = 16;
            return 0;
        }
    }

    /* filter extradata */
    if ((ctx->state & 1) && avctx->extradata && avctx->extradata_size > 5) {
        uint8_t *data = avctx->extradata, *ndata;
        int i = (data[5] & 0x1f)? 8 : 9;
        int l = avctx->extradata_size;

        if (AV_RB16(data)) {
            for (;i < l-1;i++) {
                if ((data[i] & 0x1f) == 7) {
                    int t = data[i + 1];
                    if (profile_level[t] > 0) break;
                }
            }
            if (i < l-1) {
                int r, m, d;
                ndata = av_mallocz(l + 16);
                i++;
                memcpy(ndata, data, i);
                d = l;
                l = nal_dec(data + i, data + i, l - i) + i;
                r = h264_modify(ndata + 8 + i, data + i, ctx, l - i);
                r = nal_enc(ndata + i, ndata + 8 + i, r);
                d = r + i - d;
                avctx->extradata_size = r + i;
                av_free(avctx->extradata);
                avctx->extradata = ndata;
                if (ctx->level != -1)
                    ndata[3] = ctx->level;
                m = (ndata[5] & 0x1f)? 6 : 7;
                AV_WB16(ndata + m, AV_RB16(ndata + m) + d);
                r = ctx->fps_num;
                if (r > 0) {
                    avctx->time_base.den = r;
                    avctx->time_base.num = ctx->fps_den;
                }
                ctx->state = 6;
            }
            else ctx->state = 2;
        }

    }

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

    if (keyframe) {
        int i = 0;
        uint32_t t, r, b;
        if (ctx->bs_type == 1) {
            while (t = find_next_nal_annexb(buf, &i, buf_size)) {
                if (t == NAL_SPS) {
                    b = i + 1;
                    if (!find_next_nal_annexb(buf, &i, buf_size))
                        break;
                    for(i-=2;!buf[i];i--);
                    spslen = i - b + 1;
                    break;
                } else if (t == NAL_SLICE || t == NAL_IDR_SLICE)
                    break;
            }
        } else {
            do {
                r = AV_RB32(buf + i) - 1;
                t = buf[i+4] & 0x1f;
                if (t == NAL_SPS) {
                    spslen = r;
                    b = i + 5;
                    break;
                }
                else if (t == NAL_SLICE || t == NAL_IDR_SLICE)
                    break;
                i += r + 5;
            } while(i < buf_size);
        }

        if (spslen) {
            uint8_t* spsbuf0 = alloca(spslen);
            uint8_t*  spsbuf1 = alloca(spslen);
          
            r = nal_dec(spsbuf0, buf + b, spslen);
            r = h264_modify(spsbuf1, spsbuf0, ctx, r);
            r = nal_enc(spsbuf0, spsbuf1, r);
            ctx->state |= 8;
            if (spslen == r)
                memcpy(buf + b, spsbuf0, spslen);
            else if (r < spslen) {
                *poutbuf_size = buf_size + r - spslen;
                if (ctx->bs_type == 2)
                    AV_WB32(buf + b - 5, r + 1);
                memcpy(buf + b, spsbuf0, r);
                memmove(buf + b + r - 1, buf + b + spslen - 1, buf_size - b - spslen);
            } else {
                *poutbuf = av_mallocz(buf_size + r - spslen);
                *poutbuf_size = buf_size + r - spslen;
                memcpy(*poutbuf, buf, b);
                if (ctx->bs_type == 2)
                    AV_WB32(*poutbuf + b - 5, r + 1);
                memcpy(*poutbuf + b, spsbuf0, r);
                memcpy(*poutbuf + b + r - 1, buf + b + spslen - 1, buf_size - b - spslen);
                return 1;
            }
        }
    }

    return 0;
}

AVBitStreamFilter ff_h264_changefps_bsf = {
    "h264_changefps",
    sizeof(H264FPSContext),
    h264_changefps_filter,
};

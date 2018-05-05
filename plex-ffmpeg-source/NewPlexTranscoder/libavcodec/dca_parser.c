/*
 * DCA parser
 * Copyright (C) 2004 Gildas Bazin
 * Copyright (C) 2004 Benjamin Zores
 * Copyright (C) 2006 Benjamin Larsson
 * Copyright (C) 2007 Konstantin Shishkov
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

#include "dca.h"
#include "dcadata.h"
#include "dca_core.h"
#include "dca_exss.h"
#include "dca_xll.h"
#include "dca_lbr.h"
#include "dca_syncwords.h"
#include "get_bits.h"
#include "parser.h"
#include "libavutil/crc.h"

enum ExtAudioType {
    EXT_AUDIO_XCH   = 0,
    EXT_AUDIO_X96   = 2,
    EXT_AUDIO_XXCH  = 6
};

typedef struct DCAParseContext {
    ParseContext pc;
    uint32_t lastmarker;
    int size;
    int framesize;
    unsigned int startpos;
    DCAExssParser exss;
    unsigned int sr_code;
} DCAParseContext;

#define IS_CORE_MARKER(state) \
    (((state & 0xFFFFFFFFF0FF) == (((uint64_t)DCA_SYNCWORD_CORE_14B_LE << 16) | 0xF007)) || \
     ((state & 0xFFFFFFFFFFF0) == (((uint64_t)DCA_SYNCWORD_CORE_14B_BE << 16) | 0x07F0)) || \
     ((state & 0xFFFFFFFF00FC) == (((uint64_t)DCA_SYNCWORD_CORE_LE     << 16) | 0x00FC)) || \
     ((state & 0xFFFFFFFFFC00) == (((uint64_t)DCA_SYNCWORD_CORE_BE     << 16) | 0xFC00)))

#define IS_EXSS_MARKER(state)   ((state & 0xFFFFFFFF) == DCA_SYNCWORD_SUBSTREAM)

#define IS_MARKER(state)        (IS_CORE_MARKER(state) || IS_EXSS_MARKER(state))

#define CORE_MARKER(state)      ((state >> 16) & 0xFFFFFFFF)
#define EXSS_MARKER(state)      (state & 0xFFFFFFFF)

#define STATE_LE(state)     (((state & 0xFF00FF00) >> 8) | ((state & 0x00FF00FF) << 8))
#define STATE_14(state)     (((state & 0x3FFF0000) >> 8) | ((state & 0x00003FFF) >> 6))

#define CORE_FRAMESIZE(state)   (((state >> 4) & 0x3FFF) + 1)
#define EXSS_FRAMESIZE(state)   ((state & 0x2000000000) ? \
                                 ((state >>  5) & 0xFFFFF) + 1 : \
                                 ((state >> 13) & 0x0FFFF) + 1)

/**
 * Find the end of the current frame in the bitstream.
 * @return the position of the first byte of the next frame, or -1
 */
static int dca_find_frame_end(DCAParseContext *pc1, const uint8_t *buf,
                              int buf_size)
{
    int start_found, size, i;
    uint64_t state;
    ParseContext *pc = &pc1->pc;

    start_found = pc->frame_start_found;
    state       = pc->state64;
    size        = pc1->size;

    i = 0;
    if (!start_found) {
        for (; i < buf_size; i++) {
            size++;
            state = (state << 8) | buf[i];

            if (IS_MARKER(state) &&
                (!pc1->lastmarker ||
                  pc1->lastmarker == CORE_MARKER(state) ||
                  pc1->lastmarker == DCA_SYNCWORD_SUBSTREAM)) {
                if (!pc1->lastmarker)
                    pc1->startpos = IS_EXSS_MARKER(state) ? size - 4 : size - 6;

                if (IS_EXSS_MARKER(state))
                    pc1->lastmarker = EXSS_MARKER(state);
                else
                    pc1->lastmarker = CORE_MARKER(state);

                start_found = 1;
                size        = 0;

                i++;
                break;
            }
        }
    }

    if (start_found) {
        for (; i < buf_size; i++) {
            size++;
            state = (state << 8) | buf[i];

            if (start_found == 1) {
                switch (pc1->lastmarker) {
                case DCA_SYNCWORD_CORE_BE:
                    if (size == 2) {
                        pc1->framesize = CORE_FRAMESIZE(state);
                        start_found    = 2;
                    }
                    break;
                case DCA_SYNCWORD_CORE_LE:
                    if (size == 2) {
                        pc1->framesize = CORE_FRAMESIZE(STATE_LE(state));
                        start_found    = 4;
                    }
                    break;
                case DCA_SYNCWORD_CORE_14B_BE:
                    if (size == 4) {
                        pc1->framesize = CORE_FRAMESIZE(STATE_14(state));
                        start_found    = 4;
                    }
                    break;
                case DCA_SYNCWORD_CORE_14B_LE:
                    if (size == 4) {
                        pc1->framesize = CORE_FRAMESIZE(STATE_14(STATE_LE(state)));
                        start_found    = 4;
                    }
                    break;
                case DCA_SYNCWORD_SUBSTREAM:
                    if (size == 6) {
                        pc1->framesize = EXSS_FRAMESIZE(state);
                        start_found    = 4;
                    }
                    break;
                default:
                    av_assert0(0);
                }
                continue;
            }

            if (start_found == 2 && IS_EXSS_MARKER(state) &&
                pc1->framesize <= size + 2) {
                pc1->framesize  = size + 2;
                start_found     = 3;
                continue;
            }

            if (start_found == 3) {
                if (size == pc1->framesize + 4) {
                    pc1->framesize += EXSS_FRAMESIZE(state);
                    start_found     = 4;
                }
                continue;
            }

            if (pc1->framesize > size)
                continue;

            if (IS_MARKER(state) &&
                (pc1->lastmarker == CORE_MARKER(state) ||
                 pc1->lastmarker == DCA_SYNCWORD_SUBSTREAM)) {
                pc->frame_start_found = 0;
                pc->state64           = -1;
                pc1->size             = 0;
                return IS_EXSS_MARKER(state) ? i - 3 : i - 5;
            }
        }
    }

    pc->frame_start_found = start_found;
    pc->state64           = state;
    pc1->size             = size;
    return END_NOT_FOUND;
}

static av_cold int dca_parse_init(AVCodecParserContext *s)
{
    DCAParseContext *pc1 = s->priv_data;

    s->flags |= PARSER_FLAG_ONCE; //PLEX

    pc1->lastmarker = 0;
    pc1->sr_code = -1;
    return 0;
}

static int64_t get_channel_layout(int dca_mask)
{
    static const uint8_t dca2wav_norm[28] = {
         2,  0, 1, 9, 10,  3,  8,  4,  5,  9, 10, 6, 7, 12,
        13, 14, 3, 6,  7, 11, 12, 14, 16, 15, 17, 8, 4,  5,
    };

    static const uint8_t dca2wav_wide[28] = {
         2,  0, 1, 4,  5,  3,  8,  4,  5,  9, 10, 6, 7, 12,
        13, 14, 3, 9, 10, 11, 12, 14, 16, 15, 17, 8, 4,  5,
    };

    int dca_ch;
    int wav_mask = 0;
    const uint8_t *dca2wav;
    if (dca_mask == DCA_SPEAKER_LAYOUT_7POINT0_WIDE ||
        dca_mask == DCA_SPEAKER_LAYOUT_7POINT1_WIDE)
        dca2wav = dca2wav_wide;
    else
        dca2wav = dca2wav_norm;
    for (dca_ch = 0; dca_ch < 28; dca_ch++) {
        if (dca_mask & (1 << dca_ch)) {
            int wav_ch = dca2wav[dca_ch];
            wav_mask |= 1 << wav_ch;
        }
    }
    return wav_mask;
}

static void parse_xxch_frame(GetBitContext *gb, int *mask)
{
    int xxch_mask_nbits, nchannels, header_size;

    // XXCH sync word
    if (get_bits_long(gb, 32) != DCA_SYNCWORD_XXCH)
        return;

    header_size = get_bits(gb, 6) + 1;
    if (header_size <= 6)
        return;

    // CRC presence flag for channel set header
    skip_bits1(gb);

    // Number of bits for loudspeaker mask
    xxch_mask_nbits = get_bits(gb, 5) + 1;
    if (xxch_mask_nbits <= DCA_SPEAKER_Cs)
        return;

    // XXCH frame header length
    skip_bits_long(gb, header_size * 8 - (32 + 6 + 1 + 5));

    // Channel set header length
    skip_bits(gb, 7);

    // Number of channels in a channel set
    nchannels = get_bits(gb, 3) + 1;
    if (nchannels > DCA_XXCH_CHANNELS_MAX)
        return;

    // Loudspeaker layout mask
    *mask |= get_bits_long(gb, xxch_mask_nbits - DCA_SPEAKER_Cs) << DCA_SPEAKER_Cs;
}

static int dca_parse_params(DCAParseContext *pc1, AVCodecContext *avctx, const uint8_t *buf,
                            int buf_size, int *duration, int *sample_rate,
                            int *profile)
{
    DCAExssAsset *asset = &pc1->exss.assets[0];
    GetBitContext gb;
    DCACoreFrameHeader h;
    uint8_t hdr[DCA_CORE_FRAME_HEADER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE] = { 0 };
    int ret, sample_blocks, audio_mode, ch_mask, frame_size;
    int ext_audio_type, ext_audio_present;

    if (buf_size < DCA_CORE_FRAME_HEADER_SIZE)
        return AVERROR_INVALIDDATA;

    if (AV_RB32(buf) == DCA_SYNCWORD_SUBSTREAM) {
        if ((ret = ff_dca_exss_parse(&pc1->exss, buf, buf_size)) < 0)
            return ret;

        if (asset->extension_mask & DCA_EXSS_LBR) {
            if ((ret = init_get_bits8(&gb, buf + asset->lbr_offset, asset->lbr_size)) < 0)
                return ret;

            if (get_bits_long(&gb, 32) != DCA_SYNCWORD_LBR)
                return AVERROR_INVALIDDATA;

            switch (get_bits(&gb, 8)) {
            case DCA_LBR_HEADER_DECODER_INIT:
                pc1->sr_code = get_bits(&gb, 8);
            case DCA_LBR_HEADER_SYNC_ONLY:
                break;
            default:
                return AVERROR_INVALIDDATA;
            }

            if (pc1->sr_code >= FF_ARRAY_ELEMS(ff_dca_sampling_freqs))
                return AVERROR_INVALIDDATA;

            *sample_rate = ff_dca_sampling_freqs[pc1->sr_code];
            *duration = 1024 << ff_dca_freq_ranges[pc1->sr_code];
            *profile = FF_PROFILE_DTS_EXPRESS;
            goto parse_full;
        }

        if (asset->extension_mask & DCA_EXSS_XLL) {
            int nsamples_log2;

            if ((ret = init_get_bits8(&gb, buf + asset->xll_offset, asset->xll_size)) < 0)
                return ret;

            if (get_bits_long(&gb, 32) != DCA_SYNCWORD_XLL)
                return AVERROR_INVALIDDATA;

            if (get_bits(&gb, 4))
                return AVERROR_INVALIDDATA;

            skip_bits(&gb, 8);
            skip_bits_long(&gb, get_bits(&gb, 5) + 1);
            skip_bits(&gb, 4);
            nsamples_log2 = get_bits(&gb, 4) + get_bits(&gb, 4);
            if (nsamples_log2 > 24)
                return AVERROR_INVALIDDATA;

            *sample_rate = asset->max_sample_rate;
            *duration = (1 + (*sample_rate > 96000)) << nsamples_log2;
            *profile = FF_PROFILE_DTS_HD_MA;
            goto parse_full;
        }

        return AVERROR_INVALIDDATA;
    }

    if ((ret = avpriv_dca_convert_bitstream(buf, DCA_CORE_FRAME_HEADER_SIZE,
                                            hdr, DCA_CORE_FRAME_HEADER_SIZE)) < 0)
        return ret;
    if (avpriv_dca_parse_core_frame_header(&h, hdr, ret) < 0)
        return AVERROR_INVALIDDATA;

    init_get_bits(&gb, hdr, 96);
    *duration = h.npcmblocks * DCA_PCMBLOCK_SAMPLES;
    *sample_rate = avpriv_dca_sample_rates[h.sr_code];
#if 0
    if (*profile != FF_PROFILE_UNKNOWN)
        return 0;
#endif

    *profile = FF_PROFILE_DTS;
    if (h.ext_audio_present) {
        switch (h.ext_audio_type) {
        case DCA_EXT_AUDIO_XCH:
        case DCA_EXT_AUDIO_XXCH:
            *profile = FF_PROFILE_DTS_ES;
            break;
        case DCA_EXT_AUDIO_X96:
            *profile = FF_PROFILE_DTS_96_24;
            break;
        }
    }

    frame_size = FFALIGN(h.frame_size, 4);
    if (buf_size - 4 < frame_size)
        return 0;

    buf      += frame_size;
    buf_size -= frame_size;
    if (AV_RB32(buf) != DCA_SYNCWORD_SUBSTREAM)
        return 0;
    if (ff_dca_exss_parse(&pc1->exss, buf, buf_size) < 0)
        return 0;

    if (asset->extension_mask & DCA_EXSS_XLL)
        *profile = FF_PROFILE_DTS_HD_MA;
    else if (asset->extension_mask & (DCA_EXSS_XBR | DCA_EXSS_XXCH | DCA_EXSS_X96))
        *profile = FF_PROFILE_DTS_HD_HRA;

    // Transmission bit rate
    avctx->bit_rate = ff_dca_bit_rates[get_bits(&gb, 5)];

    // Additional flags
    skip_bits(&gb, 5);

    // Extension audio
    ext_audio_type = get_bits(&gb, 3);
    ext_audio_present = get_bits1(&gb);

    // Audio sync word insertion flag
    skip_bits(&gb, 1);

    ch_mask = ff_dca_audio_mode_ch_mask[audio_mode];
    // Low frequency effects flag
    if (get_bits(&gb, 2))
        ch_mask |= DCA_SPEAKER_MASK_LFE1;

    frame_size = FFALIGN(pc1->framesize, 4);

parse_full:
    if ((!avctx->channel_layout || !avctx->channels || !avctx->sample_rate)) {
        const AVCRC *crctab = av_crc_get_table(AV_CRC_16_CCITT);
        int xch_pos = 0, x96_pos = 0, xxch_pos = 0, i;
        uint32_t mrk = AV_RB32(buf);
        uint8_t *bebuf = NULL;
        const uint8_t *input = buf;
        int input_size = buf_size;
        DCAExssAsset *asset = &pc1->exss.assets[0];
        if (mrk != DCA_SYNCWORD_CORE_BE && mrk != DCA_SYNCWORD_SUBSTREAM) {
            if (!(bebuf = av_malloc(buf_size)))
                goto fail;
            if ((ret = avpriv_dca_convert_bitstream(buf, buf_size, bebuf, buf_size)) < 0)
                goto fail;
            input = bebuf;
        }

        if ((ret = init_get_bits8(&gb, input, buf_size)) < 0)
            goto fail;

        if (AV_RB32(input) == DCA_SYNCWORD_CORE_BE) {
            if (ext_audio_present) {
                int sync_pos = FFMIN(frame_size / 4, gb.size_in_bits / 32) - 1;
                int last_pos = get_bits_count(&gb) / 32;
                int size, dist;
                switch (ext_audio_type) {
                case EXT_AUDIO_XCH:
                    if (avctx->request_channel_layout)
                        break;

                    // The distance between XCH sync word and end of the core frame
                    // must be equal to XCH frame size. Off by one error is allowed for
                    // compatibility with legacy bitstreams. Minimum XCH frame size is
                    // 96 bytes. AMODE and PCHS are further checked to reduce
                    // probability of alias sync detection.
                    for (; sync_pos >= last_pos; sync_pos--) {
                        if (AV_RB32(gb.buffer + sync_pos * 4) == DCA_SYNCWORD_XCH) {
                            gb.index = (sync_pos + 1) * 32;
                            size = get_bits(&gb, 10) + 1;
                            dist = frame_size - sync_pos * 4;
                            if (size >= 96
                                && (size == dist || size - 1 == dist)
                                && get_bits(&gb, 7) == 0x08) {
                                xch_pos = get_bits_count(&gb);
                                break;
                            }
                        }
                    }
                    break;

                case EXT_AUDIO_X96:
                    // The distance between X96 sync word and end of the core frame
                    // must be equal to X96 frame size. Minimum X96 frame size is 96
                    // bytes.
                    for (; sync_pos >= last_pos; sync_pos--) {
                        if (AV_RB32(gb.buffer + sync_pos * 4) == DCA_SYNCWORD_X96) {
                            gb.index = (sync_pos + 1) * 32;
                            size = get_bits(&gb, 12) + 1;
                            dist = frame_size - sync_pos * 4;
                            if (size >= 96 && size == dist) {
                                x96_pos = get_bits_count(&gb);
                                break;
                            }
                        }
                    }
                    break;

                case EXT_AUDIO_XXCH:
                    if (avctx->request_channel_layout)
                        break;

                    // XXCH frame header CRC must be valid. Minimum XXCH frame header
                    // size is 11 bytes.
                    for (; sync_pos >= last_pos; sync_pos--) {
                        if (AV_RB32(gb.buffer + sync_pos * 4) == DCA_SYNCWORD_XXCH) {
                            gb.index = (sync_pos + 1) * 32;
                            size = get_bits(&gb, 6) + 1;
                            dist = gb.size_in_bits / 8 - sync_pos * 4;
                            if (size >= 11 && size <= dist &&
                                !av_crc(crctab, 0xffff, gb.buffer +
                                        (sync_pos + 1) * 4, size - 4)) {
                                xxch_pos = sync_pos * 32;
                                break;
                            }
                        }
                    }
                    break;
                }
            }
            if (input_size - 4 > frame_size) {
                input      += frame_size;
                input_size -= frame_size;
            }
        }
        if (AV_RB32(input) == DCA_SYNCWORD_SUBSTREAM) {
            if ((ret = ff_dca_exss_parse(&pc1->exss, (uint8_t*)input, input_size)) < 0)
                goto fail;
        }

        avctx->sample_fmt = AV_SAMPLE_FMT_S16P;

        if (asset && asset->extension_mask & DCA_EXSS_XLL) {
            DCAXllChSet *c;
            DCAXllDecoder xll = {0};
            if ((ret = ff_dca_xll_parse(&xll, (uint8_t*)input, asset)) < 0)
                goto fail;
            avctx->profile = FF_PROFILE_DTS_HD_MA;
            *sample_rate = xll.chset[0].freq << (xll.nfreqbands - 1);
            *duration = xll.nframesamples << (xll.nfreqbands - 1);
            avctx->bits_per_raw_sample = xll.chset[0].storage_bit_res;
            avctx->bit_rate = 0;
            if (avctx->bits_per_raw_sample == 24)
                avctx->sample_fmt = AV_SAMPLE_FMT_S32P;
            ch_mask = 0;
            for (i = 0, c = xll.chset; i < xll.nactivechsets; i++, c++)
                ch_mask |= c->ch_mask;
        } else if (asset && (asset->extension_mask & DCA_EXSS_LBR)) {
            // TODO: DTS-EXPRESS
/*            DCAXllDecoder xll;
            if ((ret = ff_dca_lbr_parse(&lbr, input, asset)) < 0)
                goto fail;*/
            avctx->profile = FF_PROFILE_DTS_EXPRESS;
        } else if (asset && asset->extension_mask) {
            avctx->profile = FF_PROFILE_DTS_HD_HRA;
            if (asset->extension_mask & DCA_EXSS_X96 || x96_pos) {
                *sample_rate <<= 1;
                *duration <<= 1;
            }
            if (xch_pos)
                ch_mask |= DCA_SPEAKER_MASK_Cs;
            if (asset->extension_mask & DCA_EXSS_XXCH) {
                if ((ret = init_get_bits8(&gb, input + asset->xxch_offset, asset->xxch_size)) < 0)
                    return ret;
                parse_xxch_frame(&gb, &ch_mask);
            }
        } else if (xch_pos || xxch_pos) {
            if (xch_pos)
                ch_mask |= DCA_SPEAKER_MASK_Cs;
            if (xxch_pos) {
                if ((ret = init_get_bits8(&gb, bebuf, buf_size)) < 0)
                    goto fail;
                gb.index = xxch_pos;
                parse_xxch_frame(&gb, &ch_mask);
            }
            avctx->profile = FF_PROFILE_DTS_ES;
            if (x96_pos) {
                *sample_rate <<= 1;
                *duration <<= 1;
            }
        } else if (x96_pos) {
            avctx->profile = FF_PROFILE_DTS_96_24;
            *sample_rate <<= 1;
            *duration <<= 1;
        } else {
            avctx->profile = FF_PROFILE_DTS;
        }

fail:
        av_freep(&bebuf);
/*        if (ret < 0)
            return ret; */
    }

    if (!avctx->channel_layout) {
        avctx->channel_layout = get_channel_layout(ch_mask);
        avctx->channels = av_get_channel_layout_nb_channels(avctx->channel_layout);
    }

    return 0;
}

static int dca_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                     const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    DCAParseContext *pc1 = s->priv_data;
    ParseContext *pc = &pc1->pc;
    int next, duration, sample_rate;

    if (s->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        next = buf_size;
    } else {
        next = dca_find_frame_end(pc1, buf, buf_size);

        if (ff_combine_frame(pc, next, &buf, &buf_size) < 0) {
            *poutbuf      = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }

        /* skip initial padding */
        if (buf_size  > pc1->startpos) {
            buf      += pc1->startpos;
            buf_size -= pc1->startpos;
        }
        pc1->startpos = 0;
    }

    /* read the duration and sample rate from the frame header */
    if (!dca_parse_params(pc1, avctx, buf, buf_size, &duration, &sample_rate, &avctx->profile)) {
        if (!avctx->sample_rate)
            avctx->sample_rate = sample_rate;
        s->duration = av_rescale(duration, avctx->sample_rate, sample_rate);
    } else
        s->duration = 0;

    *poutbuf      = buf;
    *poutbuf_size = buf_size;
    return next;
}

AVCodecParser ff_dca_parser = {
    .codec_ids      = { AV_CODEC_ID_DTS },
    .priv_data_size = sizeof(DCAParseContext),
    .parser_init    = dca_parse_init,
    .parser_parse   = dca_parse,
    .parser_close   = ff_parse_close,
};

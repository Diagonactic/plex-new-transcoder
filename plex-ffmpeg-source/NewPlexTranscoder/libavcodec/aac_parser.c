/*
 * Audio and Video frame extraction
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2003 Michael Niedermayer
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

#include "parser.h"
#include "aac.h"
#include "aacdectab.h"
#include "aactab.h"
#include "aac_ac3_parser.h"
#include "adts_header.h"
#include "adts_parser.h"
#include "get_bits.h"
#include "mpeg4audio.h"

static int aac_sync(uint64_t state, AACAC3ParseContext *hdr_info,
        int *need_next_header, int *new_frame_start)
{
    GetBitContext bits;
    AACADTSHeaderInfo hdr;
    int size;
    union {
        uint64_t u64;
        uint8_t  u8[8 + AV_INPUT_BUFFER_PADDING_SIZE];
    } tmp;

    tmp.u64 = av_be2ne64(state);
    init_get_bits(&bits, tmp.u8 + 8 - AV_AAC_ADTS_HEADER_SIZE,
                  AV_AAC_ADTS_HEADER_SIZE * 8);

    if ((size = ff_adts_header_parse(&bits, &hdr)) < 0)
        return 0;
    *need_next_header = 0;
    *new_frame_start  = 1;
    hdr_info->sample_rate = hdr.sample_rate;
    hdr_info->channels    = ff_mpeg4audio_channels[hdr.chan_config];
    hdr_info->samples     = hdr.samples;
    hdr_info->bit_rate    = hdr.bit_rate;
    return size;
}

/**
 * Individual Channel Stream
 */
typedef struct PIndividualChannelStream {
    uint8_t max_sfb;            ///< number of scalefactor bands per group
    enum WindowSequence window_sequence[2];
    uint8_t use_kb_window[2];   ///< If set, use Kaiser-Bessel window, otherwise use a sine window.
    int num_window_groups;
    uint8_t group_len[8];
    const uint16_t *swb_offset; ///< table of offsets to the lowest spectral coefficient of a scalefactor band, sfb, for a particular window
    int num_swb;                ///< number of scalefactor window bands
    int num_windows;
    int tns_max_bands;
    int predictor_present;
    int predictor_reset_group;
} PIndividualChannelStream;

/**
 * Single Channel Element - used for both SCE and LFE elements.
 */
typedef struct PSingleChannelElement {
    PIndividualChannelStream ics;
    enum BandType band_type[128];                   ///< band types
    int band_type_run_end[120];                     ///< band type run end points
} PSingleChannelElement;

/**
 * channel element - generic struct for SCE/CPE/CCE/LFE
 */
typedef struct PChannelElement {
    PSingleChannelElement ch[2];
} PChannelElement;

static int decode_pce(AVCodecParserContext *s1, GetBitContext *gb)
{
    AACAC3ParseContext *s = s1->priv_data;
    int num_front, num_side, num_back, num_lfe, num_assoc_data, num_cc, orig_num, i;
    int comment_len;
    uint64_t layout = 0;

    skip_bits(gb, 2);  // object_type
    skip_bits(gb, 4); // sampling_index

    num_front       = get_bits(gb, 4);
    num_side        = get_bits(gb, 4);
    num_back        = get_bits(gb, 4);
    num_lfe         = get_bits(gb, 2);
    num_assoc_data  = get_bits(gb, 3);
    num_cc          = get_bits(gb, 4);

    if (get_bits1(gb))
        skip_bits(gb, 4); // mono_mixdown_tag
    if (get_bits1(gb))
        skip_bits(gb, 4); // stereo_mixdown_tag

    if (get_bits1(gb))
        skip_bits(gb, 3); // mixdown_coeff_index and pseudo_surround

#define GET_PAIRS(type) \
    orig_num = num_ ## type; \
    for (i = 0; i < orig_num; i++) { \
        num_ ## type += get_bits1(gb); \
        skip_bits(gb, 4); \
    }

    GET_PAIRS(front)
    GET_PAIRS(side)
    GET_PAIRS(back)

    if (num_side == 0 && num_back >= 4) {
        num_side = 2;
        num_back -= 2;
    }

    if (num_front & 1) {
        layout |= AV_CH_FRONT_CENTER;
        num_front--;
    }
    if (num_front >= 4) {
        layout |= AV_CH_FRONT_LEFT_OF_CENTER | AV_CH_FRONT_RIGHT_OF_CENTER;
        num_front -= 2;
    }
    if (num_front >= 2)
        layout |= AV_CH_FRONT_LEFT | AV_CH_FRONT_RIGHT;

    if (num_side >= 2)
        layout |= AV_CH_SIDE_LEFT | AV_CH_SIDE_RIGHT;

    if (num_back & 1) {
        layout |= AV_CH_BACK_CENTER;
        num_back--;
    }
    if (num_back >= 2)
        layout |= AV_CH_BACK_LEFT | AV_CH_BACK_RIGHT;

    if (num_lfe)
        layout |= AV_CH_LOW_FREQUENCY;

    skip_bits_long(gb, 4 * (num_lfe + num_assoc_data + num_cc));
    skip_bits_long(gb, (num_cc));

    align_get_bits(gb);

    /* comment field, first byte is length */
    comment_len = get_bits(gb, 8) * 8;
    if (get_bits_left(gb) < comment_len)
        return AVERROR_INVALIDDATA;
    skip_bits_long(gb, comment_len);

    s->channel_layout = layout;
    s->channels = av_get_channel_layout_nb_channels(s->channel_layout);

    return 0;
}

static int set_default_channels(AVCodecParserContext *s1, int channel_config)
{
    AACAC3ParseContext *s = s1->priv_data;
    if (channel_config < 1 || (channel_config > 7 && channel_config < 11) ||
        channel_config > 12)
        return AVERROR_INVALIDDATA;
    s->channel_layout = aac_channel_layout[channel_config - 1];
    if (channel_config == 7)
        s->channel_layout = AV_CH_LAYOUT_7POINT1;
    s->channels = av_get_channel_layout_nb_channels(s->channel_layout);
    return 0;
}

static int skip_data_stream_element(GetBitContext *gb)
{
    int byte_align = get_bits1(gb);
    int count = get_bits(gb, 8);
    if (count == 255)
        count += get_bits(gb, 8);
    if (byte_align)
        align_get_bits(gb);

    if (get_bits_left(gb) < 8 * count)
        return AVERROR_INVALIDDATA;

    skip_bits_long(gb, 8 * count);
    return 0;
}

static void skip_prediction(const MPEG4AudioConfig *m4ac, PIndividualChannelStream *ics,
                            GetBitContext *gb)
{
    if (get_bits1(gb))
        skip_bits(gb, 5);
    skip_bits_long(gb, FFMIN(ics->max_sfb, ff_aac_pred_sfb_max[m4ac->sampling_index]));
}

static void skip_ltp(GetBitContext *gb, uint8_t max_sfb)
{
    skip_bits(gb, 11);
    skip_bits(gb, 3);
    skip_bits_long(gb, FFMIN(max_sfb, MAX_LTP_LONG_SFB));
}

static int decode_band_types(GetBitContext *gb, enum BandType band_type[120],
                             int band_type_run_end[120], PIndividualChannelStream *ics)
{
    int g, idx = 0;
    const int bits = (ics->window_sequence[0] == EIGHT_SHORT_SEQUENCE) ? 3 : 5;
    for (g = 0; g < ics->num_window_groups; g++) {
        int k = 0;
        while (k < ics->max_sfb) {
            uint8_t sect_end = k;
            int sect_len_incr;
            int sect_band_type = get_bits(gb, 4);
            if (sect_band_type == 12) {
                return AVERROR_INVALIDDATA;
            }
            do {
                sect_len_incr = get_bits(gb, bits);
                sect_end += sect_len_incr;
                if (get_bits_left(gb) < 0) {
                    return AVERROR_INVALIDDATA;
                }
                if (sect_end > ics->max_sfb) {
                    return AVERROR_INVALIDDATA;
                }
            } while (sect_len_incr == (1 << bits) - 1);
            for (; k < sect_end; k++) {
                band_type        [idx]   = sect_band_type;
                band_type_run_end[idx++] = sect_end;
            }
        }
    }
    return 0;
}

static void skip_scalefactors(GetBitContext *gb,
                              PIndividualChannelStream *ics,
                              enum BandType band_type[120],
                              int band_type_run_end[120])
{
    int g, i, idx = 0;
    int noise_flag = 1;
    for (g = 0; g < ics->num_window_groups; g++) {
        for (i = 0; i < ics->max_sfb;) {
            int run_end = band_type_run_end[idx];
            if (band_type[idx] == ZERO_BT) {
                for (; i < run_end; i++, idx++);
            } else if ((band_type[idx] == INTENSITY_BT) ||
                       (band_type[idx] == INTENSITY_BT2)) {
                for (; i < run_end; i++, idx++) {
                    get_vlc2(gb, ff_vlc_scalefactors.table, 7, 3);
                }
            } else if (band_type[idx] == NOISE_BT) {
                for (; i < run_end; i++, idx++) {
                    if (noise_flag-- > 0)
                        skip_bits(gb, NOISE_PRE_BITS);
                    else
                        get_vlc2(gb, ff_vlc_scalefactors.table, 7, 3);
                }
            } else {
                for (; i < run_end; i++, idx++) {
                    get_vlc2(gb, ff_vlc_scalefactors.table, 7, 3);
                }
            }
        }
    }
}

static void skip_pulses(GetBitContext *gb, int num_swb)
{
    int i;
    int num_pulse = get_bits(gb, 2) + 1;
    skip_bits(gb, 6);
    skip_bits(gb, 5);
    skip_bits(gb, 4);
    for (i = 1; i < num_pulse; i++) {
        skip_bits(gb, 5);
        skip_bits(gb, 4);
    }
}

static void skip_tns(GetBitContext *gb, const PIndividualChannelStream *ics,
                     const MPEG4AudioConfig *m4ac)
{
    int w, filt, i, coef_len, coef_res, coef_compress;
    const int is8 = ics->window_sequence[0] == EIGHT_SHORT_SEQUENCE;
    for (w = 0; w < ics->num_windows; w++) {
        int n_filt;
        if ((n_filt = get_bits(gb, 2 - is8))) {
            coef_res = get_bits1(gb);

            for (filt = 0; filt < n_filt; filt++) {
                int order;
                skip_bits(gb, 6 - 2 * is8);

                order = get_bits(gb, 5 - 2 * is8);
                if (order) {
                    skip_bits1(gb);
                    coef_compress = get_bits1(gb);
                    coef_len = coef_res + 3 - coef_compress;

                    for (i = 0; i < order; i++)
                        get_bits(gb, coef_len);
                }
            }
        }
    }
}

static void skip_mid_side_stereo(PIndividualChannelStream *ics, GetBitContext *gb,
                                 int ms_present)
{
    if (ms_present == 1) {
        int max_idx = ics->num_window_groups * ics->max_sfb;
        skip_bits_long(gb, max_idx);
    }
}

static int skip_spectrum_and_dequant(GetBitContext *gb, const PIndividualChannelStream *ics,
                                     enum BandType band_type[120])
{
    int i, g, idx = 0;
    const uint16_t *offsets = ics->swb_offset;

    for (g = 0; g < ics->num_window_groups; g++) {
        unsigned g_len = ics->group_len[g];

        for (i = 0; i < ics->max_sfb; i++, idx++) {
            const unsigned cbt_m1 = band_type[idx] - 1;
            int off_len = offsets[i + 1] - offsets[i];
            int group;

            if (cbt_m1 < NOISE_BT - 1) {
                VLC_TYPE (*vlc_tab)[2] = ff_vlc_spectral[cbt_m1].table;
                const uint16_t *cb_vector_idx = ff_aac_codebook_vector_idx[cbt_m1];
                OPEN_READER(re, gb);

                switch (cbt_m1 >> 1) {
                case 0:
                    for (group = 0; group < (int)g_len; group++) {
                        int len = off_len;

                        do {
                            int code;

                            UPDATE_CACHE(re, gb);
                            GET_VLC(code, re, gb, vlc_tab, 8, 2);
                        } while (len -= 4);
                    }
                    break;

                case 1:
                    for (group = 0; group < (int)g_len; group++) {
                        int len = off_len;

                        do {
                            int code;
                            unsigned nnz;
                            unsigned cb_idx;

                            UPDATE_CACHE(re, gb);
                            GET_VLC(code, re, gb, vlc_tab, 8, 2);
                            cb_idx = cb_vector_idx[code];
                            nnz = cb_idx >> 8 & 15;
                            if (nnz)
                                GET_CACHE(re, gb);
                            LAST_SKIP_BITS(re, gb, nnz);
                        } while (len -= 4);
                    }
                    break;

                case 2:
                    for (group = 0; group < (int)g_len; group++) {
                        int len = off_len;

                        do {
                            int code;

                            UPDATE_CACHE(re, gb);
                            GET_VLC(code, re, gb, vlc_tab, 8, 2);
                        } while (len -= 2);
                    }
                    break;

                case 3:
                case 4:
                    for (group = 0; group < (int)g_len; group++) {
                        int len = off_len;

                        do {
                            int code;
                            unsigned nnz;
                            unsigned cb_idx;

                            UPDATE_CACHE(re, gb);
                            GET_VLC(code, re, gb, vlc_tab, 8, 2);
                            cb_idx = cb_vector_idx[code];
                            nnz = cb_idx >> 8 & 15;
                            if (nnz)
                                SHOW_UBITS(re, gb, nnz);
                            LAST_SKIP_BITS(re, gb, nnz);
                        } while (len -= 2);
                    }
                    break;

                default:
                    for (group = 0; group < (int)g_len; group++) {
                        int len = off_len;

                        do {
                            int code;
                            unsigned nzt, nnz;
                            unsigned cb_idx;
                            uint32_t bits;
                            int j;

                            UPDATE_CACHE(re, gb);
                            GET_VLC(code, re, gb, vlc_tab, 8, 2);

                            if (!code) {
                                continue;
                            }

                            cb_idx = cb_vector_idx[code];
                            nnz = cb_idx >> 12;
                            nzt = cb_idx >> 8;
                            bits = SHOW_UBITS(re, gb, nnz) << (32-nnz);
                            LAST_SKIP_BITS(re, gb, nnz);

                            for (j = 0; j < 2; j++) {
                                if (nzt & 1<<j) {
                                    uint32_t b;
                                    /* The total length of escape_sequence must be < 22 bits according
                                       to the specification (i.e. max is 111111110xxxxxxxxxxxx). */
                                    UPDATE_CACHE(re, gb);
                                    b = GET_CACHE(re, gb);
                                    b = 31 - av_log2(~b);

                                    if (b > 8) {
                                        return AVERROR_INVALIDDATA;
                                    }

                                    SKIP_BITS(re, gb, b + 1);
                                    b += 4;
                                    LAST_SKIP_BITS(re, gb, b);
                                    bits <<= 1;
                                } else {
                                    int v = cb_idx & 15;
                                    if (bits & 1U<<31)
                                        v = -v;
                                    bits <<= !!v;
                                }
                                cb_idx >>= 4;
                            }
                        } while (len -= 2);
                    }
                }

                CLOSE_READER(re, gb);
            }
        }
    }
    return 0;
}

static int decode_ics_info(const MPEG4AudioConfig *m4ac, PIndividualChannelStream *ics,
                           GetBitContext *gb)
{
    const int aot = m4ac->object_type;
    const int sampling_index = m4ac->sampling_index;
    if (aot != AOT_ER_AAC_ELD) {
        skip_bits1(gb);
        ics->window_sequence[0] = get_bits(gb, 2);
        if (aot == AOT_ER_AAC_LD && ics->window_sequence[0] != ONLY_LONG_SEQUENCE)
            return AVERROR_INVALIDDATA;
        ics->use_kb_window[0]   = get_bits1(gb);
    }
    ics->num_window_groups  = 1;
    ics->group_len[0]       = 1;
    if (ics->window_sequence[0] == EIGHT_SHORT_SEQUENCE) {
        int i;
        ics->max_sfb = get_bits(gb, 4);
        for (i = 0; i < 7; i++) {
            if (get_bits1(gb)) {
                ics->group_len[ics->num_window_groups - 1]++;
            } else {
                ics->num_window_groups++;
                ics->group_len[ics->num_window_groups - 1] = 1;
            }
        }
        ics->num_windows       = 8;
        ics->swb_offset        =    ff_swb_offset_128[sampling_index];
        ics->num_swb           =   ff_aac_num_swb_128[sampling_index];
        ics->tns_max_bands     = ff_tns_max_bands_128[sampling_index];
        ics->predictor_present = 0;
    } else {
        ics->max_sfb           = get_bits(gb, 6);
        ics->num_windows       = 1;
        if (aot == AOT_ER_AAC_LD || aot == AOT_ER_AAC_ELD) {
            if (m4ac->frame_length_short) {
                ics->swb_offset    =     ff_swb_offset_480[sampling_index];
                ics->num_swb       =    ff_aac_num_swb_480[sampling_index];
                ics->tns_max_bands =  ff_tns_max_bands_480[sampling_index];
            } else {
                ics->swb_offset    =     ff_swb_offset_512[sampling_index];
                ics->num_swb       =    ff_aac_num_swb_512[sampling_index];
                ics->tns_max_bands =  ff_tns_max_bands_512[sampling_index];
            }
            if (!ics->num_swb || !ics->swb_offset)
                return AVERROR_BUG;
        } else {
            ics->swb_offset    =    ff_swb_offset_1024[sampling_index];
            ics->num_swb       =   ff_aac_num_swb_1024[sampling_index];
            ics->tns_max_bands = ff_tns_max_bands_1024[sampling_index];
        }
        if (aot != AOT_ER_AAC_ELD) {
            ics->predictor_present     = get_bits1(gb);
            ics->predictor_reset_group = 0;
        }
        if (ics->predictor_present) {
            if (aot == AOT_AAC_MAIN) {
                skip_prediction(m4ac, ics, gb);
            } else if (aot == AOT_AAC_LC ||
                       aot == AOT_ER_AAC_LC) {
                goto fail;
            } else {
                if (aot == AOT_ER_AAC_LD) {
                    return AVERROR_PATCHWELCOME;
                }
                if (get_bits(gb, 1))
                    skip_ltp(gb, ics->max_sfb);
            }
        }
    }

    if (ics->max_sfb > ics->num_swb)
        goto fail;

    return 0;
fail:
    ics->max_sfb = 0;
    return AVERROR_INVALIDDATA;
}

static int decode_ics(MPEG4AudioConfig *m4ac, PSingleChannelElement *sce,
                      GetBitContext *gb, int common_window, int scale_flag)
{
    PIndividualChannelStream *ics;
    enum BandType *band_type;
    int *band_type_run_end;
    int eld_syntax, er_syntax, pulse_present = 0;
    int aot = m4ac->object_type;
    PIndividualChannelStream ics_local;
    enum BandType band_type_local[120];
    int band_type_run_end_local[120];
    int ret;

    if (sce) {
        ics = &sce->ics;
        band_type = sce->band_type;
        band_type_run_end = sce->band_type_run_end;
    } else {
        ics = &ics_local;
        band_type = band_type_local;
        band_type_run_end = band_type_run_end_local;
    }


    eld_syntax = aot == AOT_ER_AAC_ELD;
    er_syntax  = aot == AOT_ER_AAC_LC  ||
                 aot == AOT_ER_AAC_LTP ||
                 aot == AOT_ER_AAC_LD  ||
                 aot == AOT_ER_AAC_ELD;

    skip_bits(gb, 8);

    if (!common_window && !scale_flag) {
        if (decode_ics_info(m4ac, ics, gb) < 0)
            return AVERROR_INVALIDDATA;
    }

    if ((ret = decode_band_types(gb, band_type, band_type_run_end, ics)) < 0)
        return ret;
    skip_scalefactors(gb, ics, band_type, band_type_run_end);

    if (!scale_flag) {
        int tns_present;
        if (!eld_syntax && (pulse_present = get_bits1(gb))) {
            skip_pulses(gb, ics->num_swb);
        }
        tns_present = get_bits1(gb);
        if (tns_present && !er_syntax)
            skip_tns(gb, ics, m4ac);
        if (!eld_syntax && get_bits1(gb))
            return AVERROR_PATCHWELCOME;
        // I see no textual basis in the spec for this occurring after SSR gain
        // control, but this is what both reference and real implmentations do
        if (tns_present && er_syntax)
            skip_tns(gb, ics, m4ac);
    }

    if (skip_spectrum_and_dequant(gb, ics, band_type) < 0)
        return AVERROR_INVALIDDATA;

    return 0;
}

static int skip_cpe(MPEG4AudioConfig *m4ac, GetBitContext *gb)
{
    int ret, common_window, ms_present = 0;
    int eld_syntax = m4ac->object_type == AOT_ER_AAC_ELD;
    PChannelElement cpe;

    common_window = eld_syntax || get_bits1(gb);
    if (common_window) {
        if (decode_ics_info(m4ac, &cpe.ch[0].ics, gb))
            return AVERROR_INVALIDDATA;
        //i = cpe.ch[1].ics.use_kb_window[0];
        cpe.ch[1].ics = cpe.ch[0].ics;
        //cpe.ch[1].ics.use_kb_window[1] = i;
        if (cpe.ch[1].ics.predictor_present &&
            (m4ac->object_type != AOT_AAC_MAIN))
            if (get_bits(gb, 1))
                skip_ltp(gb, cpe.ch[1].ics.max_sfb);
        ms_present = get_bits(gb, 2);
        if (ms_present == 3)
            return AVERROR_INVALIDDATA;
        else if (ms_present)
            skip_mid_side_stereo(&cpe.ch[1].ics, gb, ms_present);
    }
    if ((ret = decode_ics(m4ac, &cpe.ch[0], gb, common_window, 0)))
        return ret;
    if ((ret = decode_ics(m4ac, &cpe.ch[1], gb, common_window, 0)))
        return ret;

    return 0;
}

static int skip_cce(MPEG4AudioConfig *m4ac, GetBitContext *gb)
{
    int num_gain = 0;
    int c, g, sfb, ret, num_coupled, coupling_point;
    PSingleChannelElement sce;
    PIndividualChannelStream *ics = &sce.ics;

    coupling_point = get_bits1(gb);
    num_coupled = get_bits(gb, 3);
    for (c = 0; c <= num_coupled; c++) {
        int type = get_bits1(gb) ? TYPE_CPE : TYPE_SCE;
        skip_bits(gb, 4);
        num_gain++;
        if (type == TYPE_CPE) {
            int ch_select = get_bits(gb, 2);
            if (ch_select == 3)
                num_gain++;
        }
    }
    coupling_point += get_bits1(gb) || (coupling_point >> 1);

    skip_bits1(gb);
    skip_bits(gb, 2);

    if ((ret = decode_ics(m4ac, &sce, gb, 0, 0)))
        return ret;

    for (c = 0; c < num_gain; c++) {
        static VLC_TYPE table[352][2];
        int idx  = 0;
        int cge  = 1;
        if (c) {
            cge = coupling_point == AFTER_IMDCT ? 1 : get_bits1(gb);
            if (cge)
                get_vlc2(gb, table, 7, 3);
        }
        if (coupling_point != AFTER_IMDCT) {
            for (g = 0; g < ics->num_window_groups; g++) {
                for (sfb = 0; sfb < ics->max_sfb; sfb++, idx++) {
                    if (sce.band_type[idx] != ZERO_BT) {
                        if (!cge) {
                            get_vlc2(gb, table, 7, 3);
                        }
                    }
                }
            }
        }
    }
    return 0;
}

static int decode_extension_payload(AVCodecParserContext *s1, MPEG4AudioConfig *m4ac,
                                    GetBitContext *gb, int cnt)
{
    AACAC3ParseContext *s = s1->priv_data;
    int res = cnt;
    int type = get_bits(gb, 4);

    if (type == EXT_SBR_DATA || type == EXT_SBR_DATA_CRC) { // extension type
        if (m4ac->ps && s->channels == 1) {
            m4ac->sbr = 1;
            m4ac->ps = 1;
            s->channels = 2;
            s->channel_layout = AV_CH_LAYOUT_STEREO;
            m4ac->sample_rate *= 2;
            s->profile = FF_PROFILE_AAC_HE_V2;
        } else {
            m4ac->sbr = 1;
            m4ac->sample_rate *= 2;
            s->profile = FF_PROFILE_AAC_HE;
        }
    };
    skip_bits_long(gb, 8 * cnt - 4);

    return res;
}

static int parse_frame(AVCodecParserContext *s1, MPEG4AudioConfig *m4ac, GetBitContext *gb)
{
    AACAC3ParseContext *s = s1->priv_data;
    enum RawDataBlockType elem_type;
    int elem_id, err = 0, sample_rate = m4ac->sample_rate;
    while ((elem_type = get_bits(gb, 3)) != TYPE_END) {
        elem_id = get_bits(gb, 4);

        switch (elem_type) {
        case TYPE_SCE:
        case TYPE_LFE:
            err = decode_ics(m4ac, NULL, gb, 0, 0);
            break;

        case TYPE_CPE:
            err = skip_cpe(m4ac, gb);
            break;

        case TYPE_CCE:
            err = skip_cce(m4ac, gb);
            break;

        case TYPE_DSE:
            err = skip_data_stream_element(gb);
            break;

        case TYPE_PCE:
            err = decode_pce(s1, gb);
            break;

        case TYPE_FIL:
            if (elem_id == 15)
                elem_id += get_bits(gb, 8) - 1;
            if (get_bits_left(gb) < 8 * elem_id) {
                return AVERROR_INVALIDDATA;
            }
            while (elem_id > 0)
                elem_id -= decode_extension_payload(s1, m4ac, gb, elem_id);
            break;

        default:
            err = AVERROR_BUG; /* should not happen, but keeps compiler happy */
            break;
        }

        if (err)
            return AVERROR_INVALIDDATA;

        if (get_bits_left(gb) < 3)
            return AVERROR_INVALIDDATA;
    }

    if (m4ac->sample_rate > sample_rate)
        s->samples *= 2;

    return 0;
}

static int aac_parse_full(AVCodecParserContext *s1, AVCodecContext *avctx,
                          const uint8_t *buf, int buf_size)
{
    AACAC3ParseContext *s = s1->priv_data;
    int aot, ret, frame_length_short = 0;
    GetBitContext gb;
    MPEG4AudioConfig m4ac = {0};
    init_get_bits8(&gb, buf, buf_size);
    if (avctx->extradata) {
        GetBitContext gb2;
        int len;
        if (avctx->extradata_size > INT_MAX / 8)
            return AVERROR_INVALIDDATA;
        if ((len = avpriv_mpeg4audio_get_config(&m4ac, avctx->extradata,
                                                avctx->extradata_size * 8, 1)) < 0)
            return AVERROR_INVALIDDATA;
        if (m4ac.sampling_index > 12) {
            av_log(avctx, AV_LOG_ERROR,
                   "invalid sampling rate index %d\n",
                   m4ac.sampling_index);
            return AVERROR_INVALIDDATA;
        }
        if (m4ac.object_type == AOT_ER_AAC_LD &&
            (m4ac.sampling_index < 3 || m4ac.sampling_index > 7)) {
            av_log(avctx, AV_LOG_ERROR,
                   "invalid low delay sampling rate index %d\n",
                   m4ac.sampling_index);
            return AVERROR_INVALIDDATA;
        }
        init_get_bits8(&gb2, avctx->extradata, avctx->extradata_size);
        skip_bits_long(&gb2, len);
        m4ac.frame_length_short = get_bits1(&gb2);
        if (m4ac.chan_config == 0 &&
            (m4ac.object_type == AOT_AAC_MAIN ||
             m4ac.object_type == AOT_AAC_LC ||
             m4ac.object_type == AOT_AAC_LTP ||
             m4ac.object_type == AOT_ER_AAC_LC ||
             m4ac.object_type == AOT_ER_AAC_LD)) {
            if (get_bits1(&gb2))     // dependsOnCoreCoder
                skip_bits(&gb2, 14); // coreCoderDelay
            skip_bits1(&gb2);
            if (m4ac.object_type == AOT_AAC_SCALABLE ||
                m4ac.object_type == AOT_ER_AAC_SCALABLE)
                skip_bits(&gb2, 3);  // layerNr
            skip_bits(&gb2, 4);      // element_instance_tag
            if ((ret = decode_pce(s1, &gb2)) < 0)
                return ret;
        } else {
            if ((ret = set_default_channels(s1, m4ac.chan_config)) < 0)
                return ret;
        }
    } else if (show_bits(&gb, 12) == 0xfff) {
        AACADTSHeaderInfo hdr_info;
        if ((ret = ff_adts_header_parse(&gb, &hdr_info)) < 0)
            return ret;

        m4ac.chan_config = hdr_info.chan_config;
        if (hdr_info.chan_config) {
            if ((ret = set_default_channels(s1, hdr_info.chan_config)) < 0)
                return ret;
        }
        m4ac.sample_rate     = hdr_info.sample_rate;
        m4ac.sampling_index  = hdr_info.sampling_index;
        m4ac.object_type     = hdr_info.object_type;
        m4ac.frame_length_short = 0;
        m4ac.sbr = -1;
        m4ac.ps  = -1;
        if (!hdr_info.crc_absent)
            skip_bits(&gb, 16);
    }

    aot = m4ac.object_type;
    if (aot > 0)
        s->profile = aot - 1;

    if (aot == AOT_ER_AAC_LC ||
        aot == AOT_ER_AAC_LTP ||
        aot == AOT_ER_AAC_LD ||
        aot == AOT_ER_AAC_ELD) {
        s->samples = frame_length_short ? 960 : 1024;
        if (aot == AOT_ER_AAC_LD || aot == AOT_ER_AAC_ELD)
            s->samples >>= 1;
    } else {
        s->samples = 1024;
        if (ff_thread_once(&ff_aac_table_init_common, &ff_aac_static_table_init_common))
            return AVERROR_UNKNOWN;
        parse_frame(s1, &m4ac, &gb);
    }

    s1->duration = s->samples;

    if (avctx->profile == FF_PROFILE_UNKNOWN)
        avctx->profile = s->profile;
    if (!avctx->channels) {
        avctx->channels = s->channels;
        avctx->channel_layout = s->channel_layout;
    }
    if (!avctx->sample_rate)
        avctx->sample_rate = m4ac.sample_rate;

    return 0;
}


static av_cold int aac_parse_init(AVCodecParserContext *s1)
{
    AACAC3ParseContext *s = s1->priv_data;
    s->header_size = AV_AAC_ADTS_HEADER_SIZE;
    s->sync = aac_sync;
    s->parse_full = aac_parse_full;
    s1->flags |= PARSER_FLAG_ONCE;
    return 0;
}


AVCodecParser ff_aac_parser = {
    .codec_ids      = { AV_CODEC_ID_AAC },
    .priv_data_size = sizeof(AACAC3ParseContext),
    .parser_init    = aac_parse_init,
    .parser_parse   = ff_aac_ac3_parse,
    .parser_close   = ff_parse_close,
};

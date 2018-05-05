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
#define COBJMACROS
#define _WIN32_WINNT 0x0601

#include "mf_utils.h"

// Include after mf_utils.h due to Windows include mess.
#include "mpeg4audio.h"

// Used to destroy the decoder once the last frame reference has been
// released when using opaque decoding mode.
typedef struct MFDecoder {
    IMFTransform *mft;
    AVBufferRef *device_ref;
} MFDecoder;

typedef struct MFContext {
    AVClass *av_class;
    int is_dec, is_enc, is_video, is_audio;
    GUID main_subtype;
    IMFTransform *mft;
    IMFMediaEventGenerator *async_events;
    DWORD in_stream_id, out_stream_id;
    MFT_INPUT_STREAM_INFO in_info;
    MFT_OUTPUT_STREAM_INFO out_info;
    int out_stream_provides_samples;
    int draining, draining_done;
    int sample_sent;
    int async_need_input, async_have_output, async_marker;
    int lavc_init_done;
    uint8_t *send_extradata;
    int send_extradata_size;
    ICodecAPI *codec_api;
    AVBSFContext *bsfc;
    int sw_format;
    int use_opaque; // whether AV_PIX_FMT_MF is returned to the user
    AVBufferRef *device_ref; // really AVHWDeviceContext*
    AVBufferRef *frames_ref; // really AVHWFramesContext*
    AVBufferRef *decoder_ref; // really MFDecoder*
    AVFrame *tmp_frame;
    // Important parameters which might be overwritten by decoding.
    int original_channels;
    // set by AVOption
    int opt_enc_rc;
    int opt_enc_quality;
    int opt_use_d3d;
    int opt_require_d3d;
    int opt_out_samples;
    int opt_d3d_bind_flags;
    int opt_enc_d3d;
} MFContext;

static int mf_choose_output_type(AVCodecContext *avctx);
static int mf_setup_context(AVCodecContext *avctx);

#define MF_TIMEBASE (AVRational){1, 10000000}
// Sentinel value only used by us.
#define MF_INVALID_TIME AV_NOPTS_VALUE

static int mf_wait_events(AVCodecContext *avctx)
{
    MFContext *c = avctx->priv_data;

    if (!c->async_events)
        return 0;

    while (!(c->async_need_input || c->async_have_output || c->draining_done || c->async_marker)) {
        IMFMediaEvent *ev = NULL;
        MediaEventType ev_id = 0;
        HRESULT hr = IMFMediaEventGenerator_GetEvent(c->async_events, 0, &ev);
        if (FAILED(hr)) {
            av_log(avctx, AV_LOG_ERROR, "IMFMediaEventGenerator_GetEvent() failed: %s\n",
                   ff_hr_str(hr));
            return AVERROR_EXTERNAL;
        }
        IMFMediaEvent_GetType(ev, &ev_id);
        switch (ev_id) {
        case ff_METransformNeedInput:
            if (!c->draining)
                c->async_need_input = 1;
            break;
        case ff_METransformHaveOutput:
            c->async_have_output = 1;
            break;
        case ff_METransformDrainComplete:
            c->draining_done = 1;
            break;
        case ff_METransformMarker:
            c->async_marker = 1;
            break;
        default: ;
        }
        IMFMediaEvent_Release(ev);
    }

    return 0;
}

static AVRational mf_get_tb(AVCodecContext *avctx)
{
    if (avctx->pkt_timebase.num > 0 && avctx->pkt_timebase.den > 0)
        return avctx->pkt_timebase;
    if (avctx->time_base.num > 0 && avctx->time_base.den > 0)
        return avctx->time_base;
    return MF_TIMEBASE;
}

static LONGLONG mf_to_mf_time(AVCodecContext *avctx, int64_t av_pts)
{
    if (av_pts == AV_NOPTS_VALUE)
        return MF_INVALID_TIME;
    return av_rescale_q(av_pts, mf_get_tb(avctx), MF_TIMEBASE);
}

static void mf_sample_set_pts(AVCodecContext *avctx, IMFSample *sample, int64_t av_pts)
{
    LONGLONG stime = mf_to_mf_time(avctx, av_pts);
    if (stime != MF_INVALID_TIME)
        IMFSample_SetSampleTime(sample, stime);
}

static int64_t mf_from_mf_time(AVCodecContext *avctx, LONGLONG stime)
{
    return av_rescale_q(stime, MF_TIMEBASE, mf_get_tb(avctx));
}

static int64_t mf_sample_get_pts(AVCodecContext *avctx, IMFSample *sample)
{
    LONGLONG pts;
    HRESULT hr = IMFSample_GetSampleTime(sample, &pts);
    if (FAILED(hr))
        return AV_NOPTS_VALUE;
    return mf_from_mf_time(avctx, pts);
}

static IMFSample *mf_avpacket_to_sample(AVCodecContext *avctx, const AVPacket *avpkt)
{
    MFContext *c = avctx->priv_data;
    IMFSample *sample = NULL;
    AVPacket tmp = {0};
    int ret;

    if ((ret = av_packet_ref(&tmp, avpkt)) < 0)
        goto done;

    if (c->bsfc) {
        AVPacket tmp2 = {0};
        if ((ret = av_bsf_send_packet(c->bsfc, &tmp)) < 0)
            goto done;
        if ((ret = av_bsf_receive_packet(c->bsfc, &tmp)) < 0)
            goto done;
        // We don't support any 1:m BSF filtering - but at least don't get stuck.
        while ((ret = av_bsf_receive_packet(c->bsfc, &tmp2)) >= 0)
            av_log(avctx, AV_LOG_ERROR, "Discarding unsupported sub-packet.\n");
        av_packet_unref(&tmp2);
    }

    sample = ff_create_memory_sample(tmp.data, tmp.size, c->in_info.cbAlignment);
    if (sample) {
        int64_t pts = avpkt->pts;
        if (pts == AV_NOPTS_VALUE)
            pts = avpkt->dts;
        mf_sample_set_pts(avctx, sample, pts);
        if (avpkt->flags & AV_PKT_FLAG_KEY)
            IMFAttributes_SetUINT32(sample, &MFSampleExtension_CleanPoint, TRUE);
    }

done:
    av_packet_unref(&tmp);
    return sample;
}

static int mf_deca_output_type_get(AVCodecContext *avctx, IMFMediaType *type)
{
    UINT32 t;
    HRESULT hr;

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &t);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;
    avctx->channels = t;
    avctx->channel_layout = av_get_default_channel_layout(t);

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_CHANNEL_MASK, &t);
    if (!FAILED(hr))
        avctx->channel_layout = t;

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &t);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;
    avctx->sample_rate = t;

    avctx->sample_fmt = ff_media_type_to_sample_fmt((IMFAttributes *)type);

    if (avctx->sample_fmt == AV_SAMPLE_FMT_NONE || !avctx->channels)
        return AVERROR_EXTERNAL;

    return 0;
}

static int mf_decv_output_type_get(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    AVHWFramesContext *frames_context;
    HRESULT hr;
    UINT32 w, h, cw, ch, t, t2;
    MFVideoArea area = {0};
    int ret;

    c->sw_format = ff_media_type_to_pix_fmt((IMFAttributes *)type);
//    avctx->pix_fmt = c->use_opaque ? AV_PIX_FMT_MF : c->sw_format;

    hr = ff_MFGetAttributeSize((IMFAttributes *)type, &MF_MT_FRAME_SIZE, &cw, &ch);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;

    // Cropping rectangle. Ignore the fractional offset, because nobody uses that anyway.
    // (libavcodec native decoders still try to crop away mod-2 offset pixels by
    // adjusting the pixel plane pointers.)
    hr = IMFAttributes_GetBlob(type, &MF_MT_MINIMUM_DISPLAY_APERTURE, (void *)&area, sizeof(area), NULL);
    if (FAILED(hr)) {
        w = cw;
        h = ch;
    } else {
        w = area.OffsetX.value + area.Area.cx;
        h = area.OffsetY.value + area.Area.cy;
    }

    if (w > cw || h > ch)
        return AVERROR_EXTERNAL;

    hr = ff_MFGetAttributeRatio((IMFAttributes *)type, &MF_MT_PIXEL_ASPECT_RATIO, &t, &t2);
    if (!FAILED(hr)) {
        avctx->sample_aspect_ratio.num = t;
        avctx->sample_aspect_ratio.den = t2;
    }

    hr = IMFAttributes_GetUINT32(type, &MF_MT_YUV_MATRIX, &t);
    if (!FAILED(hr)) {
        switch (t) {
        case MFVideoTransferMatrix_BT709:       avctx->colorspace = AVCOL_SPC_BT709; break;
        case MFVideoTransferMatrix_BT601:       avctx->colorspace = AVCOL_SPC_BT470BG; break;
        case MFVideoTransferMatrix_SMPTE240M:   avctx->colorspace = AVCOL_SPC_SMPTE240M; break;
        }
    }

    hr = IMFAttributes_GetUINT32(type, &MF_MT_VIDEO_PRIMARIES, &t);
    if (!FAILED(hr)) {
        switch (t) {
        case MFVideoPrimaries_BT709:            avctx->color_primaries = AVCOL_PRI_BT709; break;
        case MFVideoPrimaries_BT470_2_SysM:     avctx->color_primaries = AVCOL_PRI_BT470M; break;
        case MFVideoPrimaries_BT470_2_SysBG:    avctx->color_primaries = AVCOL_PRI_BT470BG; break;
        case MFVideoPrimaries_SMPTE170M:        avctx->color_primaries = AVCOL_PRI_SMPTE170M; break;
        case MFVideoPrimaries_SMPTE240M:        avctx->color_primaries = AVCOL_PRI_SMPTE240M; break;
        }
    }

    hr = IMFAttributes_GetUINT32(type, &MF_MT_TRANSFER_FUNCTION, &t);
    if (!FAILED(hr)) {
        switch (t) {
        case MFVideoTransFunc_10:               avctx->color_trc = AVCOL_TRC_LINEAR; break;
        case MFVideoTransFunc_22:               avctx->color_trc = AVCOL_TRC_GAMMA22; break;
        case MFVideoTransFunc_709:              avctx->color_trc = AVCOL_TRC_BT709; break;
        case MFVideoTransFunc_240M:             avctx->color_trc = AVCOL_TRC_SMPTE240M; break;
        case MFVideoTransFunc_sRGB:             avctx->color_trc = AVCOL_TRC_IEC61966_2_1; break;
        case MFVideoTransFunc_28:               avctx->color_trc = AVCOL_TRC_GAMMA28; break;
        // mingw doesn't define these yet
        //case MFVideoTransFunc_Log_100:          avctx->color_trc = AVCOL_TRC_LOG; break;
        //case MFVideoTransFunc_Log_316:          avctx->color_trc = AVCOL_TRC_LOG_SQRT; break;
        }
    }

    hr = IMFAttributes_GetUINT32(type, &MF_MT_VIDEO_CHROMA_SITING, &t);
    if (!FAILED(hr)) {
        switch (t) {
        case MFVideoChromaSubsampling_MPEG2:    avctx->chroma_sample_location = AVCHROMA_LOC_LEFT; break;
        case MFVideoChromaSubsampling_MPEG1:    avctx->chroma_sample_location = AVCHROMA_LOC_CENTER; break;
        }
    }

    hr = IMFAttributes_GetUINT32(type, &MF_MT_VIDEO_NOMINAL_RANGE, &t);
    if (!FAILED(hr)) {
        switch (t) {
        case MFNominalRange_0_255:              avctx->color_range = AVCOL_RANGE_JPEG; break;
        case MFNominalRange_16_235:             avctx->color_range = AVCOL_RANGE_MPEG; break;
        }
    }

    if ((ret = ff_set_dimensions(avctx, cw, ch)) < 0)
        return ret;

    avctx->width = w;
    avctx->height = h;

    av_buffer_unref(&c->frames_ref);
    c->frames_ref = av_hwframe_ctx_alloc(c->device_ref);
    if (!c->frames_ref)
        return AVERROR(ENOMEM);
    frames_context = (void *)c->frames_ref->data;
//    frames_context->format = AV_PIX_FMT_MF;
    frames_context->width = cw;
    frames_context->height = ch;
    frames_context->sw_format = c->sw_format;
    if ((ret = av_hwframe_ctx_init(c->frames_ref)) < 0) {
        av_buffer_unref(&c->frames_ref);
        return ret;
    }

    return ret;
}

static int mf_enca_output_type_get(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    UINT32 sz;

    if (avctx->codec_id != AV_CODEC_ID_MP3 && avctx->codec_id != AV_CODEC_ID_AC3) {
        hr = IMFAttributes_GetBlobSize(type, &MF_MT_USER_DATA, &sz);
        if (!FAILED(hr) && sz > 0) {
            avctx->extradata = av_mallocz(sz + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!avctx->extradata)
                return AVERROR(ENOMEM);
            avctx->extradata_size = sz;
            hr = IMFAttributes_GetBlob(type, &MF_MT_USER_DATA, avctx->extradata, sz, NULL);
            if (FAILED(hr))
                return AVERROR_EXTERNAL;

            if (avctx->codec_id == AV_CODEC_ID_AAC && avctx->extradata_size >= 12) {
                // Get rid of HEAACWAVEINFO (after wfx field, 12 bytes).
                avctx->extradata_size = avctx->extradata_size - 12;
                memmove(avctx->extradata, avctx->extradata + 12, avctx->extradata_size);
            }
        }
    }

    // I don't know where it's documented that we need this. It happens with the
    // MS mp3 encoder MFT. The idea for the workaround is taken from NAudio.
    // (Certainly any lossy codec will have frames much smaller than 1 second.)
    if (!c->out_info.cbSize && !c->out_stream_provides_samples) {
        hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &sz);
        if (!FAILED(hr)) {
            av_log(avctx, AV_LOG_VERBOSE, "MFT_OUTPUT_STREAM_INFO.cbSize set to 0, "
                   "assuming %d bytes instead.\n", (int)sz);
            c->out_info.cbSize = sz;
        }
    }

    return 0;
}

static int mf_encv_output_type_get(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    UINT32 sz;

    hr = IMFAttributes_GetBlobSize(type, &MF_MT_MPEG_SEQUENCE_HEADER, &sz);
    if (!FAILED(hr) && sz > 0) {
        uint8_t *extradata = av_mallocz(sz + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!extradata)
            return AVERROR(ENOMEM);
        hr = IMFAttributes_GetBlob(type, &MF_MT_MPEG_SEQUENCE_HEADER, extradata, sz, NULL);
        if (FAILED(hr)) {
            av_free(extradata);
            return AVERROR_EXTERNAL;
        }
        if (c->lavc_init_done) {
            // At least the Intel QSV h264 MFT sets up extradata when the first
            // frame is encoded, and after the AVCodecContext was opened.
            // Send it as side-data with the next packet.
            av_freep(&c->send_extradata);
            c->send_extradata = extradata;
            c->send_extradata_size = sz;
        } else {
            av_freep(&avctx->extradata);
            avctx->extradata = extradata;
            avctx->extradata_size = sz;
        }
    }

    return 0;
}

static int mf_output_type_get(AVCodecContext *avctx)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    IMFMediaType *type;
    int ret;

    hr = IMFTransform_GetOutputCurrentType(c->mft, c->out_stream_id, &type);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not get output type\n");
        return AVERROR_EXTERNAL;
    }

    av_log(avctx, AV_LOG_VERBOSE, "final output type:\n");
    ff_media_type_dump(avctx, type);

    ret = 0;
    if (c->is_dec && c->is_video) {
        ret = mf_decv_output_type_get(avctx, type);
    } else if (c->is_dec && c->is_audio) {
        ret = mf_deca_output_type_get(avctx, type);
    } else if (c->is_enc && c->is_video) {
        ret = mf_encv_output_type_get(avctx, type);
    } else if (c->is_enc && c->is_audio) {
        ret = mf_enca_output_type_get(avctx, type);
    }

    if (ret < 0)
        av_log(avctx, AV_LOG_ERROR, "output type not supported\n");

    IMFMediaType_Release(type);
    return ret;
}

static int mf_sample_to_a_avframe(AVCodecContext *avctx, IMFSample *sample, AVFrame *frame)
{
    HRESULT hr;
    int ret;
    DWORD len;
    IMFMediaBuffer *buffer;
    BYTE *data;
    size_t bps;

    hr = IMFSample_GetTotalLength(sample, &len);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;

    bps = av_get_bytes_per_sample(avctx->sample_fmt) * avctx->channels;

    frame->nb_samples = len / bps;
    if (frame->nb_samples * bps != len)
        return AVERROR_EXTERNAL; // unaligned crap -> assume not possible

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    IMFSample_ConvertToContiguousBuffer(sample, &buffer);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;

    hr = IMFMediaBuffer_Lock(buffer, &data, NULL, NULL);
    if (FAILED(hr)) {
        IMFMediaBuffer_Release(buffer);
        return AVERROR_EXTERNAL;
    }

    memcpy(frame->data[0], data, len);

    IMFMediaBuffer_Unlock(buffer);
    IMFMediaBuffer_Release(buffer);

    return 0;
}

struct frame_ref {
    IMFSample *sample;
    AVBufferRef *decoder_ref; // really MFDecoder*
};

static void mf_buffer_ref_free(void *opaque, uint8_t *data)
{
    struct frame_ref *ref = (void *)data;
    IMFSample_Release(ref->sample);
    av_buffer_unref(&ref->decoder_ref);
    av_free(ref);
}

static int mf_sample_to_v_avframe(AVCodecContext *avctx, IMFSample *sample, AVFrame *frame)
{
    MFContext *c = avctx->priv_data;
    AVFrame *mf_frame = c->tmp_frame;
    int ret = 0;

    if (!c->frames_ref)
        return AVERROR(EINVAL);

    av_frame_unref(mf_frame);
    av_frame_unref(frame);

    mf_frame->width = avctx->width;
    mf_frame->height = avctx->height;
//    mf_frame->format = AV_PIX_FMT_MF;
    mf_frame->data[3] = (void *)sample;

    if ((ret = ff_decode_frame_props(avctx, mf_frame)) < 0)
        return ret;

    // ff_decode_frame_props() overwites this
//    mf_frame->format = AV_PIX_FMT_MF;

    mf_frame->hw_frames_ctx = av_buffer_ref(c->frames_ref);
    if (!mf_frame->hw_frames_ctx)
        return AVERROR(ENOMEM);

    if (c->use_opaque) {
        struct frame_ref *ref = av_mallocz(sizeof(*ref));
        if (!ref)
            return AVERROR(ENOMEM);
        ref->sample = sample;
        ref->decoder_ref = av_buffer_ref(c->decoder_ref);
        if (!ref->decoder_ref) {
            av_free(ref);
            return AVERROR(ENOMEM);
        }
        mf_frame->buf[0] = av_buffer_create((void *)ref, sizeof(*ref),
                                            mf_buffer_ref_free, NULL,
                                            AV_BUFFER_FLAG_READONLY);
        if (!mf_frame->buf[0]) {
            av_buffer_unref(&ref->decoder_ref);
            av_free(ref);
            return AVERROR(ENOMEM);
        }
        IMFSample_AddRef(sample);
        av_frame_move_ref(frame, mf_frame);
    } else {
        frame->width = mf_frame->width;
        frame->height = mf_frame->height;
        frame->format = c->sw_format;

        if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
            return ret;

        if ((ret = av_hwframe_transfer_data(frame, mf_frame, 0)) < 0)
            return ret;
    }

    // Strictly optional - release the IMFSample a little bit earlier.
    av_frame_unref(mf_frame);

    return 0;
}

// Allocate the given frame and copy the sample to it.
// Format must have been set on the avctx.
static int mf_sample_to_avframe(AVCodecContext *avctx, IMFSample *sample, AVFrame *frame)
{
    MFContext *c = avctx->priv_data;
    int ret;

    if (c->is_audio) {
        ret = mf_sample_to_a_avframe(avctx, sample, frame);
    } else {
        ret = mf_sample_to_v_avframe(avctx, sample, frame);
    }

    frame->pts = mf_sample_get_pts(avctx, sample);
    frame->pkt_dts = AV_NOPTS_VALUE;

    return ret;
}

static int mf_sample_to_avpacket(AVCodecContext *avctx, IMFSample *sample, AVPacket *avpkt)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    int ret;
    DWORD len;
    IMFMediaBuffer *buffer;
    BYTE *data;
    UINT64 t;
    UINT32 t32;

    hr = IMFSample_GetTotalLength(sample, &len);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;

    if ((ret = av_new_packet(avpkt, len)) < 0)
        return ret;

    IMFSample_ConvertToContiguousBuffer(sample, &buffer);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;

    hr = IMFMediaBuffer_Lock(buffer, &data, NULL, NULL);
    if (FAILED(hr)) {
        IMFMediaBuffer_Release(buffer);
        return AVERROR_EXTERNAL;
    }

    memcpy(avpkt->data, data, len);

    IMFMediaBuffer_Unlock(buffer);
    IMFMediaBuffer_Release(buffer);

    avpkt->pts = avpkt->dts = mf_sample_get_pts(avctx, sample);

    hr = IMFAttributes_GetUINT32(sample, &MFSampleExtension_CleanPoint, &t32);
    if (c->is_audio || (!FAILED(hr) && t32 != 0))
        avpkt->flags |= AV_PKT_FLAG_KEY;

    hr = IMFAttributes_GetUINT64(sample, &MFSampleExtension_DecodeTimestamp, &t);
    if (!FAILED(hr))
        avpkt->dts = mf_from_mf_time(avctx, t);

    return 0;
}

static IMFSample *mf_a_avframe_to_sample(AVCodecContext *avctx, const AVFrame *frame)
{
    MFContext *c = avctx->priv_data;
    size_t len;
    size_t bps;
    IMFSample *sample;

    bps = av_get_bytes_per_sample(avctx->sample_fmt) * avctx->channels;
    len = frame->nb_samples * bps;

    sample = ff_create_memory_sample(frame->data[0], len, c->in_info.cbAlignment);
    if (sample)
        IMFSample_SetSampleDuration(sample, mf_to_mf_time(avctx, frame->nb_samples));
    return sample;
}

static IMFSample *mf_v_avframe_to_sample(AVCodecContext *avctx, const AVFrame *frame)
{
    MFContext *c = avctx->priv_data;
    IMFSample *sample;
    IMFMediaBuffer *buffer;
    BYTE *data;
    HRESULT hr;
    int ret;
    int size;

    size = av_image_get_buffer_size(avctx->pix_fmt, avctx->width, avctx->height, 1);
    if (size < 0)
        return NULL;

    sample = ff_create_memory_sample(NULL, size, c->in_info.cbAlignment);
    if (!sample)
        return NULL;

    hr = IMFSample_GetBufferByIndex(sample, 0, &buffer);
    if (FAILED(hr)) {
        IMFSample_Release(sample);
        return NULL;
    }

    hr = IMFMediaBuffer_Lock(buffer, &data, NULL, NULL);
    if (FAILED(hr)) {
        IMFMediaBuffer_Release(buffer);
        IMFSample_Release(sample);
        return NULL;
    }

    ret = av_image_copy_to_buffer((uint8_t *)data, size, (void *)frame->data, frame->linesize,
                                  avctx->pix_fmt, avctx->width, avctx->height, 1);
    IMFMediaBuffer_SetCurrentLength(buffer, size);
    IMFMediaBuffer_Unlock(buffer);
    IMFMediaBuffer_Release(buffer);
    if (ret < 0) {
        IMFSample_Release(sample);
        return NULL;
    }

    IMFSample_SetSampleDuration(sample, mf_to_mf_time(avctx, frame->pkt_duration));

    return sample;
}

static IMFSample *mf_avframe_to_sample(AVCodecContext *avctx, const AVFrame *frame)
{
    MFContext *c = avctx->priv_data;
    IMFSample *sample;

    if (c->is_audio) {
        sample = mf_a_avframe_to_sample(avctx, frame);
    } else {
        sample = mf_v_avframe_to_sample(avctx, frame);
    }

    if (sample)
        mf_sample_set_pts(avctx, sample, frame->pts);

    return sample;
}

static int mf_send_sample(AVCodecContext *avctx, IMFSample *sample)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    int ret;

    if (sample) {
        if (c->async_events) {
            if ((ret = mf_wait_events(avctx)) < 0)
                return ret;
            if (!c->async_need_input)
                return AVERROR(EAGAIN);
        }
        if (!c->sample_sent)
            IMFSample_SetUINT32(sample, &MFSampleExtension_Discontinuity, TRUE);
        c->sample_sent = 1;
        hr = IMFTransform_ProcessInput(c->mft, c->in_stream_id, sample, 0);
        if (hr == MF_E_NOTACCEPTING) {
            return AVERROR(EAGAIN);
        } else if (FAILED(hr)) {
            av_log(avctx, AV_LOG_ERROR, "failed processing input: %s\n", ff_hr_str(hr));
            return AVERROR_EXTERNAL;
        }
        c->async_need_input = 0;
    } else if (!c->draining) {
        hr = IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (FAILED(hr))
            av_log(avctx, AV_LOG_ERROR, "failed draining: %s\n", ff_hr_str(hr));
        // Some MFTs (AC3) will send a frame after each drain command (???), so
        // this is required to make draining actually terminate.
        c->draining = 1;
        c->async_need_input = 0;
    } else {
        return AVERROR_EOF;
    }
    return 0;
}

static int mf_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
    MFContext *c = avctx->priv_data;
    int ret;
    IMFSample *sample = NULL;
    if (frame) {
        sample = mf_avframe_to_sample(avctx, frame);
        if (!sample)
            return AVERROR(ENOMEM);
        if (c->is_enc && c->is_video && c->codec_api) {
            if (frame->pict_type == AV_PICTURE_TYPE_I || !c->sample_sent)
                ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVEncVideoForceKeyFrame, FF_VAL_VT_UI4(1));
        }
    }
    ret = mf_send_sample(avctx, sample);
    if (sample)
        IMFSample_Release(sample);
    return ret;
}

static int mf_send_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    int ret;
    IMFSample *sample = NULL;
    if (avpkt) {
        sample = mf_avpacket_to_sample(avctx, avpkt);
        if (!sample)
            return AVERROR(ENOMEM);
    }
    ret = mf_send_sample(avctx, sample);
    if (sample)
        IMFSample_Release(sample);
    return ret;
}

static int mf_receive_sample(AVCodecContext *avctx, IMFSample **out_sample)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    DWORD st;
    MFT_OUTPUT_DATA_BUFFER out_buffers;
    IMFSample *sample;
    int ret = 0;

    while (1) {
        *out_sample = NULL;
        sample = NULL;

        if (c->async_events) {
            if ((ret = mf_wait_events(avctx)) < 0)
                return ret;
            if (!c->async_have_output || c->draining_done) {
                ret = 0;
                break;
            }
        }

        if (!c->out_stream_provides_samples) {
            sample = ff_create_memory_sample(NULL, c->out_info.cbSize, c->out_info.cbAlignment);
            if (!sample)
                return AVERROR(ENOMEM);
        }

        out_buffers = (MFT_OUTPUT_DATA_BUFFER) {
            .dwStreamID = c->out_stream_id,
            .pSample = sample,
        };

        st = 0;
        hr = IMFTransform_ProcessOutput(c->mft, 0, 1, &out_buffers, &st);

        if (out_buffers.pEvents)
            IMFCollection_Release(out_buffers.pEvents);

        if (!FAILED(hr)) {
            *out_sample = out_buffers.pSample;
            ret = 0;
            break;
        }

        if (out_buffers.pSample)
            IMFSample_Release(out_buffers.pSample);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (c->draining)
                c->draining_done = 1;
            ret = 0;
        } else if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            av_log(avctx,  AV_LOG_WARNING, "stream format change\n");
            ret = mf_choose_output_type(avctx);
            if (ret == 0) // we don't expect renegotiating the input type
                ret = AVERROR_EXTERNAL;
            if (ret > 0) {
                ret = mf_setup_context(avctx);
                if (ret >= 0) {
                    c->async_have_output = 0;
                    continue;
                }
            }
        } else {
            av_log(avctx, AV_LOG_ERROR, "failed processing output: %s\n", ff_hr_str(hr));
            ret = AVERROR_EXTERNAL;
        }

        break;
    }

    c->async_have_output = 0;

    if (ret >= 0 && !*out_sample)
        ret = c->draining_done ? AVERROR_EOF : AVERROR(EAGAIN);

    return ret;
}

static int mf_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    IMFSample *sample;
    int ret;

    ret = mf_receive_sample(avctx, &sample);
    if (ret < 0)
        return ret;

    ret = mf_sample_to_avframe(avctx, sample, frame);
    IMFSample_Release(sample);
    return ret;
}

static int mf_receive_packet(AVCodecContext *avctx, AVPacket *avpkt)
{
    MFContext *c = avctx->priv_data;
    IMFSample *sample;
    int ret;

    ret = mf_receive_sample(avctx, &sample);
    if (ret < 0)
        return ret;

    ret = mf_sample_to_avpacket(avctx, sample, avpkt);
    IMFSample_Release(sample);

    if (c->send_extradata) {
        ret = av_packet_add_side_data(avpkt, AV_PKT_DATA_NEW_EXTRADATA,
                                      c->send_extradata,
                                      c->send_extradata_size);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to add extradata: %i\n", ret);
            return ret;
        }
        c->send_extradata = NULL;
        c->send_extradata_size = 0;
    }

    return ret;
}

static void mf_flush(AVCodecContext *avctx)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr = IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (FAILED(hr))
        av_log(avctx, AV_LOG_ERROR, "flushing failed\n");

    hr = IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    if (FAILED(hr))
        av_log(avctx, AV_LOG_ERROR, "could not end streaming (%s)\n", ff_hr_str(hr));

    // In async mode, we have to wait until previous events have been flushed.
    if (c->async_events) {
        hr = IMFMediaEventGenerator_QueueEvent(c->async_events, ff_METransformMarker,
                                               &GUID_NULL, S_OK, NULL);
        if (FAILED(hr)) {
            av_log(avctx, AV_LOG_ERROR, "sending marker failed\n");
        } else {
            while (!c->async_marker) {
                if (mf_wait_events(avctx) < 0)
                    break; // just don't lock up
                c->async_need_input = c->async_have_output = c->draining_done = 0;
            }
            c->async_marker = 0;
        }
    }

    c->draining = 0;
    c->sample_sent = 0;
    c->draining_done = 0;
    c->async_need_input = c->async_have_output = 0;
    hr = IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr))
        av_log(avctx, AV_LOG_ERROR, "stream restart failed\n");
}

// Most encoders seem to enumerate supported audio formats on the output types,
// at least as far as channel configuration and sample rate is concerned. Pick
// the one which seems to match best.
static int64_t mf_enca_output_score(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    UINT32 t;
    GUID tg;
    int64_t score = 0;

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &t);
    if (!FAILED(hr) && t == avctx->sample_rate)
        score |= 1LL << 32;

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &t);
    if (!FAILED(hr) && t == avctx->channels)
        score |= 2LL << 32;

    hr = IMFAttributes_GetGUID(type, &MF_MT_SUBTYPE, &tg);
    if (!FAILED(hr)) {
        if (IsEqualGUID(&c->main_subtype, &tg))
            score |= 4LL << 32;
    }

    // Select the bitrate (lowest priority).
    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &t);
    if (!FAILED(hr)) {
        int diff = (int)t - avctx->bit_rate / 8;
        if (diff >= 0) {
            score |= (1LL << 31) - diff; // prefer lower bitrate
        } else {
            score |= (1LL << 30) + diff; // prefer higher bitrate
        }
    }

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AAC_PAYLOAD_TYPE, &t);
    if (!FAILED(hr) && t != 0)
        return -1;

    return score;
}

static int mf_enca_output_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    // (some decoders allow adjusting this freely, but it can also cause failure
    //  to set the output type - so it's commented for being too fragile)
    //IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avctx->bit_rate / 8);

    return 0;
}

static int64_t mf_enca_input_score(AVCodecContext *avctx, IMFMediaType *type)
{
    HRESULT hr;
    UINT32 t;
    int64_t score = 0;

    enum AVSampleFormat sformat = ff_media_type_to_sample_fmt((IMFAttributes *)type);
    if (sformat == AV_SAMPLE_FMT_NONE)
        return -1; // can not use

    if (sformat == avctx->sample_fmt)
        score |= 1;

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &t);
    if (!FAILED(hr) && t == avctx->sample_rate)
        score |= 2;

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &t);
    if (!FAILED(hr) && t == avctx->channels)
        score |= 4;

    return score;
}

static int mf_enca_input_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    HRESULT hr;
    UINT32 t;

    enum AVSampleFormat sformat = ff_media_type_to_sample_fmt((IMFAttributes *)type);
    if (sformat != avctx->sample_fmt) {
        av_log(avctx, AV_LOG_ERROR, "unsupported input sample format set\n");
        return AVERROR(EINVAL);
    }

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &t);
    if (FAILED(hr) || t != avctx->sample_rate) {
        av_log(avctx, AV_LOG_ERROR, "unsupported input sample rate set\n");
        return AVERROR(EINVAL);
    }

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &t);
    if (FAILED(hr) || t != avctx->channels) {
        av_log(avctx, AV_LOG_ERROR, "unsupported input channel number set\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static int64_t mf_encv_output_score(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    GUID tg;
    HRESULT hr;
    int score = -1;

    hr = IMFAttributes_GetGUID(type, &MF_MT_SUBTYPE, &tg);
    if (!FAILED(hr)) {
        if (IsEqualGUID(&c->main_subtype, &tg))
            score = 1;
    }

    return score;
}

static int mf_encv_output_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    AVRational frame_rate = av_inv_q(avctx->time_base);
    frame_rate.den *= avctx->ticks_per_frame;

    ff_MFSetAttributeSize((IMFAttributes *)type, &MF_MT_FRAME_SIZE, avctx->width, avctx->height);
    IMFAttributes_SetUINT32(type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    ff_MFSetAttributeRatio((IMFAttributes *)type, &MF_MT_FRAME_RATE, frame_rate.num, frame_rate.den);

    // (MS HEVC supports eAVEncH265VProfile_Main_420_8 only.)
    if (avctx->codec_id == AV_CODEC_ID_H264) {
        UINT32 profile = eAVEncH264VProfile_Base;
        switch (avctx->profile) {
        case FF_PROFILE_H264_MAIN:
            profile = eAVEncH264VProfile_Main;
            break;
        case FF_PROFILE_H264_HIGH:
            profile = eAVEncH264VProfile_High;
            break;
        }
        IMFAttributes_SetUINT32(type, &MF_MT_MPEG2_PROFILE, profile);
    }

    IMFAttributes_SetUINT32(type, &MF_MT_AVG_BITRATE, avctx->bit_rate);

    // Note that some of the ICodecAPI options must be set before SetOutputType.
    if (c->codec_api) {
        if (avctx->bit_rate)
            ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVEncCommonMeanBitRate, FF_VAL_VT_UI4(avctx->bit_rate));

        if (c->opt_enc_rc >= 0)
            ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVEncCommonRateControlMode, FF_VAL_VT_UI4(c->opt_enc_rc));

        if (c->opt_enc_quality >= 0)
            ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVEncCommonQuality, FF_VAL_VT_UI4(c->opt_enc_quality));

        if (avctx->max_b_frames > 0)
            ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVEncMPVDefaultBPictureCount, FF_VAL_VT_UI4(avctx->max_b_frames));

        ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVEncH264CABACEnable, FF_VAL_VT_BOOL(1));
    }

    return 0;
}

static int64_t mf_encv_input_score(AVCodecContext *avctx, IMFMediaType *type)
{
    enum AVPixelFormat pix_fmt = ff_media_type_to_pix_fmt((IMFAttributes *)type);
    if (pix_fmt != avctx->pix_fmt)
        return -1; // can not use

    return 0;
}

static int mf_encv_input_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    enum AVPixelFormat pix_fmt = ff_media_type_to_pix_fmt((IMFAttributes *)type);
    if (pix_fmt != avctx->pix_fmt) {
        av_log(avctx, AV_LOG_ERROR, "unsupported input pixel format set\n");
        return AVERROR(EINVAL);
    }

    //ff_MFSetAttributeSize((IMFAttributes *)type, &MF_MT_FRAME_SIZE, avctx->width, avctx->height);

    return 0;
}

static int mf_deca_input_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;

    int sample_rate = avctx->sample_rate;
    int channels = avctx->channels;

    IMFAttributes_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFAttributes_SetGUID(type, &MF_MT_SUBTYPE, &c->main_subtype);

    if (avctx->codec_id == AV_CODEC_ID_AAC) {
        int assume_adts = avctx->extradata_size == 0;
        // The first 12 bytes are the remainder of HEAACWAVEINFO.
        // Fortunately all fields can be left 0.
        size_t ed_size = 12 + (size_t)avctx->extradata_size;
        uint8_t *ed = av_mallocz(ed_size);
        if (!ed)
            return AVERROR(ENOMEM);
        if (assume_adts)
            ed[0] = 1; // wPayloadType=1 (ADTS)
        if (avctx->extradata_size) {
            MPEG4AudioConfig c = {0};
            memcpy(ed + 12, avctx->extradata, avctx->extradata_size);
            if (avpriv_mpeg4audio_get_config(&c, avctx->extradata, avctx->extradata_size * 8, 0) >= 0) {
                if (c.channels > 0)
                    channels = c.channels;
                sample_rate = c.sample_rate;
            }
        }
        IMFAttributes_SetBlob(type, &MF_MT_USER_DATA, ed, ed_size);
        av_free(ed);
        IMFAttributes_SetUINT32(type, &MF_MT_AAC_PAYLOAD_TYPE, assume_adts ? 1 : 0);
    } else if (avctx->extradata_size) {
        IMFAttributes_SetBlob(type, &MF_MT_USER_DATA, avctx->extradata, avctx->extradata_size);
    }

    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, channels);

    // WAVEFORMATEX stuff; might be required by some codecs.
    if (avctx->block_align)
        IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, avctx->block_align);
    if (avctx->bit_rate)
        IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avctx->bit_rate / 8);
    if (avctx->bits_per_coded_sample)
        IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, avctx->bits_per_coded_sample);

    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_PREFER_WAVEFORMATEX, 1);

    return 0;
}

static int64_t mf_decv_input_score(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    uint32_t fourcc;
    GUID tg;
    HRESULT hr;
    int score = -1;

    hr = IMFAttributes_GetGUID(type, &MF_MT_SUBTYPE, &tg);
    if (!FAILED(hr)) {
        if (IsEqualGUID(&c->main_subtype, &tg))
            score = 1;

        // For the MPEG-4 decoder (selects MPEG-4 variant via FourCC).
        if (ff_fourcc_from_guid(&tg, &fourcc) >= 0 && fourcc == avctx->codec_tag)
            score = 2;
    }

    return score;
}

static int mf_decv_input_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    int use_extradata = avctx->extradata_size && !c->bsfc;

    IMFAttributes_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);

    hr = IMFAttributes_GetItem(type, &MF_MT_SUBTYPE, NULL);
    if (FAILED(hr))
        IMFAttributes_SetGUID(type, &MF_MT_SUBTYPE, &c->main_subtype);

    ff_MFSetAttributeSize((IMFAttributes *)type, &MF_MT_FRAME_SIZE, avctx->width, avctx->height);

    IMFAttributes_SetUINT32(type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);

    if (avctx->sample_aspect_ratio.num)
        ff_MFSetAttributeRatio((IMFAttributes *)type, &MF_MT_PIXEL_ASPECT_RATIO,
                               avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den);

    if (avctx->bit_rate)
        IMFAttributes_SetUINT32(type, &MF_MT_AVG_BITRATE, avctx->bit_rate);

    if (IsEqualGUID(&c->main_subtype, &MFVideoFormat_MP4V) ||
        IsEqualGUID(&c->main_subtype, &MFVideoFormat_MP43) ||
        IsEqualGUID(&c->main_subtype, &ff_MFVideoFormat_MP42)) {
        if (avctx->extradata_size < 3 ||
            avctx->extradata[0] || avctx->extradata[1] ||
            avctx->extradata[2] != 1)
            use_extradata = 0;
    }

    if (use_extradata)
        IMFAttributes_SetBlob(type, &MF_MT_USER_DATA, avctx->extradata, avctx->extradata_size);

    return 0;
}

static int64_t mf_deca_input_score(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    GUID tg;
    int score = -1;

    hr = IMFAttributes_GetGUID(type, &MF_MT_SUBTYPE, &tg);
    if (!FAILED(hr)) {
        if (IsEqualGUID(&c->main_subtype, &tg))
            score = 1;
    }

    return score;
}

// Sort the types by preference:
// - float sample format (highest)
// - sample depth
// - channel count
// - sample rate (lowest)
// Assume missing information means any is allowed.
static int64_t mf_deca_output_score(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    UINT32 t;
    int sample_fmt;
    int64_t score = 0;

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &t);
    if (!FAILED(hr))
        score |= t;

    // MF doesn't seem to tell us the native channel count. Try to get the
    // same number of channels by looking at the input codec parameters.
    // (With some luck they are correct, or even come from a parser.)
    // Prefer equal or larger channel count.
    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &t);
    if (!FAILED(hr)) {
        int channels = av_get_channel_layout_nb_channels(avctx->request_channel_layout);
        int64_t ch_score = 0;
        int diff;
        if (channels < 1)
            channels = c->original_channels;
        diff = (int)t - channels;
        if (diff >= 0) {
            ch_score |= (1LL << 7) - diff;
        } else {
            ch_score |= (1LL << 6) + diff;
        }
        score |= ch_score << 20;
    }

    sample_fmt = ff_media_type_to_sample_fmt((IMFAttributes *)type);
    if (sample_fmt == AV_SAMPLE_FMT_NONE) {
        score = -1;
    } else {
        score |= av_get_bytes_per_sample(sample_fmt) << 28;
        if (sample_fmt == AV_SAMPLE_FMT_FLT)
            score |= 1LL << 32;
    }

    return score;
}

static int mf_deca_output_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    int block_align;
    HRESULT hr;

    // Some decoders (wmapro) do not list any output types. I have no clue
    // what we're supposed to do, and this is surely a MFT bug. Setting an
    // arbitrary output type helps.
    hr = IMFAttributes_GetItem(type, &MF_MT_MAJOR_TYPE, NULL);
    if (!FAILED(hr))
        return 0;

    IMFAttributes_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);

    block_align = 4;
    IMFAttributes_SetGUID(type, &MF_MT_SUBTYPE, &MFAudioFormat_Float);
    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 32);

    block_align *= avctx->channels;
    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, avctx->channels);

    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align);

    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, avctx->sample_rate);

    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, block_align * avctx->sample_rate);

    return 0;
}

static int64_t mf_decv_output_score(AVCodecContext *avctx, IMFMediaType *type)
{
    enum AVPixelFormat pix_fmt = ff_media_type_to_pix_fmt((IMFAttributes *)type);
    if (pix_fmt == AV_PIX_FMT_NONE)
        return -1;
    if (pix_fmt == AV_PIX_FMT_P010)
        return 2;
    if (pix_fmt == AV_PIX_FMT_NV12)
        return 1;
    return 0;
}

static int mf_choose_output_type(AVCodecContext *avctx)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    int ret;
    IMFMediaType *out_type = NULL;
    int64_t out_type_score = -1;
    int out_type_index = -1;
    int n;

    av_log(avctx, AV_LOG_VERBOSE, "output types:\n");
    for (n = 0; ; n++) {
        IMFMediaType *type;
        int64_t score = -1;

        hr = IMFTransform_GetOutputAvailableType(c->mft, c->out_stream_id, n, &type);
        if (hr == MF_E_NO_MORE_TYPES || hr == E_NOTIMPL)
            break;
        if (hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            av_log(avctx, AV_LOG_VERBOSE, "(need to set input type)\n");
            ret = 0;
            goto done;
        }
        if (FAILED(hr)) {
            av_log(avctx, AV_LOG_ERROR, "error getting output type: %s\n", ff_hr_str(hr));
            ret = AVERROR_EXTERNAL;
            goto done;
        }

        av_log(avctx, AV_LOG_VERBOSE, "output type %d:\n", n);
        ff_media_type_dump(avctx, type);

        if (c->is_dec && c->is_video) {
            score = mf_decv_output_score(avctx, type);
        } else if (c->is_dec && c->is_audio) {
            score = mf_deca_output_score(avctx, type);
        } else if (c->is_enc && c->is_video) {
            score = mf_encv_output_score(avctx, type);
        } else if (c->is_enc && c->is_audio) {
            score = mf_enca_output_score(avctx, type);
        }

        if (score > out_type_score) {
            if (out_type)
                IMFMediaType_Release(out_type);
            out_type = type;
            out_type_score = score;
            out_type_index = n;
            IMFMediaType_AddRef(out_type);
        }

        IMFMediaType_Release(type);
    }

    if (out_type) {
        av_log(avctx, AV_LOG_VERBOSE, "picking output type %d.\n", out_type_index);
    } else {
        hr = MFCreateMediaType(&out_type);
        if (FAILED(hr)) {
            ret = AVERROR(ENOMEM);
            goto done;
        }
    }

    ret = 0;
    if (c->is_dec && c->is_video) {
        //ret = mf_decv_output_adjust(avctx, out_type);
    } else if (c->is_dec && c->is_audio) {
        ret = mf_deca_output_adjust(avctx, out_type);
    } else if (c->is_enc && c->is_video) {
        ret = mf_encv_output_adjust(avctx, out_type);
    } else if (c->is_enc && c->is_audio) {
        ret = mf_enca_output_adjust(avctx, out_type);
    }

    if (ret >= 0) {
        av_log(avctx, AV_LOG_VERBOSE, "setting output type:\n");
        ff_media_type_dump(avctx, out_type);

        hr = IMFTransform_SetOutputType(c->mft, c->out_stream_id, out_type, 0);
        if (!FAILED(hr)) {
            ret = 1;
        } else if (hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            av_log(avctx, AV_LOG_VERBOSE, "rejected - need to set input type\n");
            ret = 0;
        } else {
            av_log(avctx, AV_LOG_ERROR, "could not set output type (%s)\n", ff_hr_str(hr));
            ret = AVERROR_EXTERNAL;
        }
    }

done:
    if (out_type)
        IMFMediaType_Release(out_type);
    return ret;
}

static int mf_choose_input_type(AVCodecContext *avctx)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    int ret;
    IMFMediaType *in_type = NULL;
    int64_t in_type_score = -1;
    int in_type_index = -1;
    int n;

    av_log(avctx, AV_LOG_VERBOSE, "input types:\n");
    for (n = 0; ; n++) {
        IMFMediaType *type = NULL;
        int64_t score = -1;

        hr = IMFTransform_GetInputAvailableType(c->mft, c->in_stream_id, n, &type);
        if (hr == MF_E_NO_MORE_TYPES || hr == E_NOTIMPL)
            break;
        if (hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            av_log(avctx, AV_LOG_VERBOSE, "(need to set output type 1)\n");
            ret = 0;
            goto done;
        }
        if (FAILED(hr)) {
            av_log(avctx, AV_LOG_ERROR, "error getting input type: %s\n", ff_hr_str(hr));
            ret = AVERROR_EXTERNAL;
            goto done;
        }

        av_log(avctx, AV_LOG_VERBOSE, "input type %d:\n", n);
        ff_media_type_dump(avctx, type);

        if (c->is_dec && c->is_video) {
            score = mf_decv_input_score(avctx, type);
        } else if (c->is_dec && c->is_audio) {
            score = mf_deca_input_score(avctx, type);
        } else if (c->is_enc && c->is_video) {
            score = mf_encv_input_score(avctx, type);
        } else if (c->is_enc && c->is_audio) {
            score = mf_enca_input_score(avctx, type);
        }

        if (score > in_type_score) {
            if (in_type)
                IMFMediaType_Release(in_type);
            in_type = type;
            in_type_score = score;
            in_type_index = n;
            IMFMediaType_AddRef(in_type);
        }

        IMFMediaType_Release(type);
    }

    if (in_type) {
        av_log(avctx, AV_LOG_VERBOSE, "picking input type %d.\n", in_type_index);
    } else {
        // Some buggy MFTs (WMA encoder) fail to return MF_E_TRANSFORM_TYPE_NOT_SET.
        if (c->is_enc) {
            av_log(avctx, AV_LOG_VERBOSE, "(need to set output type 2)\n");
            ret = 0;
            goto done;
        }
        hr = MFCreateMediaType(&in_type);
        if (FAILED(hr)) {
            ret = AVERROR(ENOMEM);
            goto done;
        }
    }

    ret = 0;
    if (c->is_dec && c->is_video) {
        ret = mf_decv_input_adjust(avctx, in_type);
    } else if (c->is_dec && c->is_audio) {
        ret = mf_deca_input_adjust(avctx, in_type);
    } else if (c->is_enc && c->is_video) {
        ret = mf_encv_input_adjust(avctx, in_type);
    } else if (c->is_enc && c->is_audio) {
        ret = mf_enca_input_adjust(avctx, in_type);
    }

    if (ret >= 0) {
        av_log(avctx, AV_LOG_VERBOSE, "setting input type:\n");
        ff_media_type_dump(avctx, in_type);

        hr = IMFTransform_SetInputType(c->mft, c->in_stream_id, in_type, 0);
        if (!FAILED(hr)) {
            ret = 1;
        } else if (hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            av_log(avctx, AV_LOG_VERBOSE, "rejected - need to set output type\n");
            ret = 0;
        } else {
            av_log(avctx, AV_LOG_ERROR, "could not set input type (%s)\n", ff_hr_str(hr));
            ret = AVERROR_EXTERNAL;
        }
    }

done:
    if (in_type)
        IMFMediaType_Release(in_type);
    return ret;
}

static int mf_negotiate_types(AVCodecContext *avctx)
{
    // This follows steps 1-5 on:
    //  https://msdn.microsoft.com/en-us/library/windows/desktop/aa965264(v=vs.85).aspx
    // If every MFT implementer does this correctly, this loop should at worst
    // be repeated once.
    int need_input = 1, need_output = 1;
    int n;
    for (n = 0; n < 2 && (need_input || need_output); n++) {
        int ret;
        ret = mf_choose_input_type(avctx);
        if (ret < 0)
            return ret;
        need_input = ret < 1;
        ret = mf_choose_output_type(avctx);
        if (ret < 0)
            return ret;
        need_output = ret < 1;
    }
    if (need_input || need_output) {
        av_log(avctx, AV_LOG_ERROR, "format negotiation failed (%d/%d)\n",
               need_input, need_output);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

static int mf_setup_context(AVCodecContext *avctx)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    int ret;

    hr = IMFTransform_GetInputStreamInfo(c->mft, c->in_stream_id, &c->in_info);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;
    av_log(avctx, AV_LOG_VERBOSE, "in_info: size=%d, align=%d\n",
           (int)c->in_info.cbSize, (int)c->in_info.cbAlignment);

    hr = IMFTransform_GetOutputStreamInfo(c->mft, c->out_stream_id, &c->out_info);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;
    c->out_stream_provides_samples =
        (c->out_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) ||
        (c->out_info.dwFlags & MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
    av_log(avctx, AV_LOG_VERBOSE, "out_info: size=%d, align=%d%s\n",
           (int)c->out_info.cbSize, (int)c->out_info.cbAlignment,
           c->out_stream_provides_samples ? " (provides samples)" : "");

    if ((ret = mf_output_type_get(avctx)) < 0)
        return ret;

    return 0;
}

/*
static int mf_init_hwaccel(AVCodecContext *avctx)
{
    MFContext *c = avctx->priv_data;
    AVMFDeviceContext *mf_device_ctx;
    void *manager = NULL;
    HRESULT hr;
    IMFAttributes *attrs;
    UINT32 d3d_aware = 0, d3d11_aware = 0;
    int ret;
    MFDecoder *dec = (void *)c->decoder_ref->data;
    enum AVPixelFormat pixfmts[] = { AV_PIX_FMT_MF,
                                     AV_PIX_FMT_NV12,
                                     AV_PIX_FMT_NONE };

    if (c->is_enc)
        return 0;

    if (c->is_dec) {
        // Ask the user whether to use hwaccel mode. This is the _only_ purpose of this
        // get_format call, and we don't negotiate the actual pixfmt with it. The
        // user can also signal to get IMFSamples even if no D3D decoding is used.
        if ((ret = ff_get_format(avctx, pixfmts)) < 0)
            return ret;
    } else {
        ret = AV_PIX_FMT_NONE;
    }
    if (ret == AV_PIX_FMT_MF) {
        AVBufferRef *device_ref = avctx->hwaccel_context;
        if (device_ref)
            c->device_ref = av_buffer_ref(device_ref);
        c->use_opaque = 1;
    }

    hr = IMFTransform_GetAttributes(c->mft, &attrs);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_VERBOSE, "error retrieving MFT attributes: %s\n", ff_hr_str(hr));
    } else {
        hr = IMFAttributes_GetUINT32(attrs, &MF_SA_D3D_AWARE, &d3d_aware);
        if (FAILED(hr))
            d3d_aware = 0;

        hr = IMFAttributes_GetUINT32(attrs, &ff_MF_SA_D3D11_AWARE, &d3d11_aware);
        if (FAILED(hr))
            d3d11_aware = 0;

        if (c->is_dec && c->use_opaque && c->opt_out_samples >= 0) {
            hr = IMFAttributes_SetUINT32(attrs, &ff_MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT, c->opt_out_samples);
            if (FAILED(hr))
                av_log(avctx, AV_LOG_ERROR, "could not set samplecount(%s)\n", ff_hr_str(hr));
        }

        IMFAttributes_Release(attrs);
    }

    if (c->device_ref) {
        AVHWDeviceContext *device_ctx = (void *)c->device_ref->data;
        mf_device_ctx = (void *)device_ctx->hwctx;
        av_log(avctx, AV_LOG_VERBOSE, "Using user-provided AVHWDeviceContext.\n");
    } else {
        // Even for opt_use_d3d==AV_MF_NONE, a a dummy MF AVHWDeviceContext is
        // needed to copy frame data from IMFSamples to AVFrames.
        AVHWDeviceContext *device_ctx;
        c->device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MF);
        if (!c->device_ref)
            return AVERROR(ENOMEM);
        device_ctx = (void *)c->device_ref->data;
        mf_device_ctx = device_ctx->hwctx;
        mf_device_ctx->device_type = c->opt_use_d3d;
        if ((ret = av_hwdevice_ctx_init(c->device_ref)) < 0)
            return ret;
    }

    dec->device_ref = c->device_ref; // dec has ownership

    if (mf_device_ctx->d3d11_manager && d3d11_aware) {
        manager = mf_device_ctx->d3d11_manager;
    } else if (mf_device_ctx->d3d9_manager && d3d_aware) {
        manager = mf_device_ctx->d3d9_manager;
    }

    if ((mf_device_ctx->d3d11_manager || mf_device_ctx->d3d9_manager) && !manager && c->opt_require_d3d) {
        av_log(avctx, AV_LOG_INFO, "MFT does not support hardware decoding.\n");
        return AVERROR_DECODER_NOT_FOUND;
    }

    if (manager) {
        av_log(avctx, AV_LOG_VERBOSE, "Setting D3D manager: %p\n", manager);

        hr = IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)manager);
        if (FAILED(hr)) {
            av_log(avctx, AV_LOG_ERROR, "failed to set D3D manager: %s\n", ff_hr_str(hr));
            return AVERROR_EXTERNAL;
        }
    }
    if (manager && c->is_dec) {
        hr = IMFTransform_GetOutputStreamAttributes(c->mft, c->out_stream_id, &attrs);
        if (FAILED(hr)) {
            av_log(avctx, AV_LOG_ERROR, "could not get output stream attributes\n");
            return AVERROR_EXTERNAL;
        }

        if (c->opt_d3d_bind_flags >= 0) {
            hr = IMFAttributes_SetUINT32(attrs, &ff_MF_SA_D3D11_BINDFLAGS, c->opt_d3d_bind_flags);
            if (FAILED(hr))
                av_log(avctx, AV_LOG_ERROR, "could not set bindflags (%s)\n", ff_hr_str(hr));
        }

        IMFAttributes_Release(attrs);
    }

    return 0;
}
*/

static LONG mf_codecapi_get_int(ICodecAPI *capi, const GUID *guid, LONG def)
{
    LONG ret = def;
    VARIANT v;
    HRESULT hr = ICodecAPI_GetValue(capi, &ff_CODECAPI_AVDecVideoMaxCodedWidth, &v);
    if (FAILED(hr))
        return ret;
    if (v.vt == VT_I4)
        ret = v.lVal;
    if (v.vt == VT_UI4)
        ret = v.ulVal;
    VariantClear(&v);
    return ret;
}

static int mf_check_codec_requirements(AVCodecContext *avctx)
{
    MFContext *c = avctx->priv_data;

    if (c->is_dec && c->is_video && c->codec_api) {
        LONG w = mf_codecapi_get_int(c->codec_api, &ff_CODECAPI_AVDecVideoMaxCodedWidth, 0);
        LONG h = mf_codecapi_get_int(c->codec_api, &ff_CODECAPI_AVDecVideoMaxCodedHeight, 0);

        if (w <= 0 || h <= 0)
            return 0;

        av_log(avctx, AV_LOG_VERBOSE, "Max. supported video size: %dx%d\n", (int)w, (int)h);

        // avctx generally has only the cropped size. Assume the coded size is
        // the same size, rounded up to the next macroblock boundary.
        if (avctx->width > w || avctx->height > h) {
            av_log(avctx, AV_LOG_ERROR, "Video size %dx%d larger than supported size.\n",
                   avctx->width, avctx->height);
            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static int mf_unlock_async(AVCodecContext *avctx)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    IMFAttributes *attrs;
    UINT32 v;
    int res = AVERROR_EXTERNAL;

    // For hw encoding we unfortunately need it, otherwise don't risk it.
    if (!(c->is_enc && c->is_video && c->opt_enc_d3d))
        return 0;

    hr = IMFTransform_GetAttributes(c->mft, &attrs);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "error retrieving MFT attributes: %s\n", ff_hr_str(hr));
        goto err;
    }

    hr = IMFAttributes_GetUINT32(attrs, &MF_TRANSFORM_ASYNC, &v);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "error querying async: %s\n", ff_hr_str(hr));
        goto err;
    }

    if (!v) {
        av_log(avctx, AV_LOG_ERROR, "hardware MFT is not async\n");
        goto err;
    }

    hr = IMFAttributes_SetUINT32(attrs, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not set async unlock: %s\n", ff_hr_str(hr));
        goto err;
    }

    hr = IMFTransform_QueryInterface(c->mft, &IID_IMFMediaEventGenerator, (void **)&c->async_events);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not get async interface\n");
        goto err;
    }

    res = 0;

err:
    IMFAttributes_Release(attrs);
    return res;
}

static int mf_create(void *log, IMFTransform **mft, const AVCodec *codec, int use_hw)
{
    int is_audio = codec->type == AVMEDIA_TYPE_AUDIO;
    int is_dec = av_codec_is_decoder(codec);
    const CLSID *subtype = ff_codec_to_mf_subtype(codec->id);
    MFT_REGISTER_TYPE_INFO reg = {0};
    GUID category;
    int ret;

    *mft = NULL;

    if (!subtype)
        return AVERROR(ENOSYS);

    reg.guidSubtype = *subtype;

    if (is_dec) {
        if (is_audio) {
            reg.guidMajorType = MFMediaType_Audio;
            category = MFT_CATEGORY_AUDIO_DECODER;
        } else {
            reg.guidMajorType = MFMediaType_Video;
            category = MFT_CATEGORY_VIDEO_DECODER;
        }

        if ((ret = ff_instantiate_mf(log, category, &reg, NULL, use_hw, mft)) < 0)
            return ret;
    } else {
        if (is_audio) {
            reg.guidMajorType = MFMediaType_Audio;
            category = MFT_CATEGORY_AUDIO_ENCODER;
        } else {
            reg.guidMajorType = MFMediaType_Video;
            category = MFT_CATEGORY_VIDEO_ENCODER;
        }

        if ((ret = ff_instantiate_mf(log, category, NULL, &reg, use_hw, mft)) < 0)
            return ret;
    }

    return 0;
}

static void mf_release_decoder(void *opaque, uint8_t *data)
{
    MFDecoder *dec = (void *)data;

    // At least async MFTs require this to be called to truly terminate it.
    // Of course, mingw is missing both the import lib stub for
    // MFShutdownObject, as well as the entire IMFShutdown interface.
    HANDLE lib = LoadLibraryW(L"mf.dll");
    if (lib) {
        HRESULT (WINAPI *MFShutdownObject_ptr)(IUnknown *pUnk)
            = (void *)GetProcAddress(lib, "MFShutdownObject");
        if (MFShutdownObject_ptr)
            MFShutdownObject_ptr((IUnknown *)dec->mft);
        FreeLibrary(lib);
    }

    IMFTransform_Release(dec->mft);

    av_buffer_unref(&dec->device_ref);
}

static int mf_init(AVCodecContext *avctx)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    int ret;
    MFDecoder *dec;
    const CLSID *subtype = ff_codec_to_mf_subtype(avctx->codec_id);
    int use_hw = 0;

    c->tmp_frame = av_frame_alloc();
    if (!c->tmp_frame)
        return AVERROR(ENOMEM);

    c->original_channels = avctx->channels;

    c->is_dec = av_codec_is_decoder(avctx->codec);
    c->is_enc = !c->is_dec;
    c->is_audio = avctx->codec_type == AVMEDIA_TYPE_AUDIO;
    c->is_video = !c->is_audio;

    if (c->is_video && c->is_enc && c->opt_enc_d3d)
        use_hw = 1;

    if (!subtype)
        return AVERROR(ENOSYS);

    c->main_subtype = *subtype;

    if ((ret = mf_create(avctx, &c->mft, avctx->codec, use_hw)) < 0)
        return ret;

    dec = av_mallocz(sizeof(*dec));
    if (!dec) {
        ff_free_mf(&c->mft);
        return AVERROR(ENOMEM);
    }
    dec->mft = c->mft;

    c->decoder_ref = av_buffer_create((void *)dec, sizeof(*dec),
                                      mf_release_decoder, NULL,
                                      AV_BUFFER_FLAG_READONLY);
    if (!c->decoder_ref) {
        ff_free_mf(&c->mft);
        return AVERROR(ENOMEM);
    }

    if ((ret = mf_unlock_async(avctx)) < 0)
        return ret;

    hr = IMFTransform_QueryInterface(c->mft, &IID_ICodecAPI, (void **)&c->codec_api);
    if (!FAILED(hr))
        av_log(avctx, AV_LOG_VERBOSE, "MFT supports ICodecAPI.\n");

    if (c->is_dec) {
        const char *bsf = NULL;

        if (avctx->codec->id == AV_CODEC_ID_H264 && avctx->extradata && avctx->extradata[0] == 1)
            bsf = "h264_mp4toannexb";

        if (avctx->codec->id == AV_CODEC_ID_HEVC && avctx->extradata && avctx->extradata[0] == 1)
            bsf = "hevc_mp4toannexb";

        if (bsf) {
            const AVBitStreamFilter *bsfc = av_bsf_get_by_name(bsf);
            if (!bsfc) {
                ret = AVERROR(ENOSYS);
                goto bsf_done;
            }
            if ((ret = av_bsf_alloc(bsfc, &c->bsfc)) < 0)
                goto bsf_done;
            if ((ret = avcodec_parameters_from_context(c->bsfc->par_in, avctx)) < 0)
                goto bsf_done;
            if ((ret = av_bsf_init(c->bsfc)) < 0)
                goto bsf_done;
            ret = 0;
        bsf_done:
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Cannot open the %s BSF!\n", bsf);
                return ret;
            }
        }
    }

//    if (c->is_video && ((ret = mf_init_hwaccel(avctx)) < 0))
//        return ret;

    if ((ret = mf_check_codec_requirements(avctx)) < 0)
        return ret;

    hr = IMFTransform_GetStreamIDs(c->mft, 1, &c->in_stream_id, 1, &c->out_stream_id);
    if (hr == E_NOTIMPL) {
        c->in_stream_id = c->out_stream_id = 0;
    } else if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not get stream IDs (%s)\n", ff_hr_str(hr));
        return AVERROR_EXTERNAL;
    }

    if ((ret = mf_negotiate_types(avctx)) < 0)
        return ret;

    if ((ret = mf_setup_context(avctx)) < 0)
        return ret;

    hr = IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not start streaming (%s)\n", ff_hr_str(hr));
        return AVERROR_EXTERNAL;
    }

    hr = IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not start stream (%s)\n", ff_hr_str(hr));
        return AVERROR_EXTERNAL;
    }

    c->lavc_init_done = 1;

    return 0;
}

static int mf_close(AVCodecContext *avctx)
{
    MFContext *c = avctx->priv_data;
    int uninit_com = c->mft != NULL;

    if (c->codec_api)
        ICodecAPI_Release(c->codec_api);

    if (c->async_events)
        IMFMediaEventGenerator_Release(c->async_events);

    av_bsf_free(&c->bsfc);

    av_buffer_unref(&c->frames_ref);
    av_frame_free(&c->tmp_frame);
    av_buffer_unref(&c->decoder_ref);

    if (uninit_com)
        CoUninitialize();

    if (c->is_enc) {
        av_freep(&avctx->extradata);
        avctx->extradata_size = 0;

        av_freep(&c->send_extradata);
        c->send_extradata_size = 0;
    }

    return 0;
}

static int mf_probe(struct AVCodec *codec)
{
    IMFTransform *mft;
    int ret;

    if ((ret = mf_create(NULL, &mft, codec, 0)) < 0)
        return ret;

    ff_free_mf(&mft);
    return 0;
}

#define OFFSET(x) offsetof(MFContext, x)

#define MF_DECODER(MEDIATYPE, NAME, ID, OPTS) \
    static const AVClass ff_ ## NAME ## _mf_decoder_class = {                  \
        .class_name = #NAME "_mf",                                             \
        .item_name  = av_default_item_name,                                    \
        .option     = OPTS,                                                    \
        .version    = LIBAVUTIL_VERSION_INT,                                   \
    };                                                                         \
    AVCodec ff_ ## NAME ## _mf_decoder = {                                     \
        .priv_class     = &ff_ ## NAME ## _mf_decoder_class,                   \
        .name           = #NAME "_mf",                                         \
        .long_name      = NULL_IF_CONFIG_SMALL(#ID " via MediaFoundation"),    \
        .type           = AVMEDIA_TYPE_ ## MEDIATYPE,                          \
        .id             = AV_CODEC_ID_ ## ID,                                  \
        .priv_data_size = sizeof(MFContext),                                   \
        .probe          = mf_probe,                                            \
        .init           = mf_init,                                             \
        .close          = mf_close,                                            \
        .send_packet    = mf_send_packet,                                      \
        .receive_frame  = mf_receive_frame,                                    \
        .flush          = mf_flush,                                            \
        .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,     \
        .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS |                          \
                          FF_CODEC_CAP_INIT_THREADSAFE |                       \
                          FF_CODEC_CAP_INIT_CLEANUP,                           \
    };

MF_DECODER(AUDIO, ac3,         AC3,             NULL);
MF_DECODER(AUDIO, eac3,        EAC3,            NULL);
MF_DECODER(AUDIO, aac,         AAC,             NULL);
MF_DECODER(AUDIO, mp1,         MP1,             NULL);
MF_DECODER(AUDIO, mp2,         MP2,             NULL);
MF_DECODER(AUDIO, mp3,         MP3,             NULL);
MF_DECODER(AUDIO, wmav1,       WMAV1,           NULL);
MF_DECODER(AUDIO, wmav2,       WMAV2,           NULL);
MF_DECODER(AUDIO, wmalossless, WMALOSSLESS,     NULL);
MF_DECODER(AUDIO, wmapro,      WMAPRO,          NULL);
MF_DECODER(AUDIO, wmavoice,    WMAVOICE,        NULL);

#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption vdec_opts[] = {
    // Only used for non-opaque output (otherwise, the AVHWDeviceContext matters)
/*    {"use_d3d",       "D3D decoding mode", OFFSET(opt_use_d3d), AV_OPT_TYPE_INT, {.i64 = AV_MF_NONE}, 0, INT_MAX, VD, "use_d3d"},
    { "auto",         "Any (or none) D3D mode", 0, AV_OPT_TYPE_CONST, {.i64 = AV_MF_AUTO}, 0, 0, VD, "use_d3d"},
    { "none",         "Disable D3D mode", 0, AV_OPT_TYPE_CONST, {.i64 = AV_MF_NONE}, 0, 0, VD, "use_d3d"},
    { "d3d9",         "D3D9 decoding", 0, AV_OPT_TYPE_CONST, {.i64 = AV_MF_D3D9}, 0, 0, VD, "use_d3d"},
    { "d3d11",        "D3D11 decoding", 0, AV_OPT_TYPE_CONST, {.i64 = AV_MF_D3D11}, 0, 0, VD, "use_d3d"},*/
    // Can be used to fail early if no hwaccel is available
    {"require_d3d",   "Fail init if D3D cannot be used", OFFSET(opt_require_d3d), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VD},
    // Experimenting with h264/d3d11 shows: allocated_textures = MIN(out_samples, 5) + 18
    // (not set if -1)
    {"out_samples",   "Minimum output sample count", OFFSET(opt_out_samples), AV_OPT_TYPE_INT, {.i64 = -1}, -1, 100, VD},
    // D3D11_BIND_FLAG used for texture allocations; must include D3D11_BIND_DECODER
    // (not set if -1)
    {"d3d_bind_flags","Texture D3D_BIND_FLAG", OFFSET(opt_d3d_bind_flags), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, VD},
    {NULL}
};

#define MF_VIDEO_DECODER(NAME, ID) \
    MF_DECODER(VIDEO, NAME, ID, vdec_opts);

/*
    AVHWAccel ff_ ## NAME ## _mf_hwaccel = {                                   \
        .name       = #NAME "_mf",                                             \
        .type       = AVMEDIA_TYPE_VIDEO,                                      \
        .id         = AV_CODEC_ID_ ## ID,                                      \
        .pix_fmt    = AV_PIX_FMT_MF,                                           \
    };                                                                         \
*/

MF_VIDEO_DECODER(h264,         H264);
MF_VIDEO_DECODER(hevc,         HEVC);
MF_VIDEO_DECODER(vc1,          VC1);
MF_VIDEO_DECODER(wmv1,         WMV1);
MF_VIDEO_DECODER(wmv2,         WMV2);
MF_VIDEO_DECODER(wmv3,         WMV3);
MF_VIDEO_DECODER(mpeg2,        MPEG2VIDEO);
MF_VIDEO_DECODER(mpeg4,        MPEG4);
MF_VIDEO_DECODER(msmpeg4v1,    MSMPEG4V1);
MF_VIDEO_DECODER(msmpeg4v2,    MSMPEG4V2);
MF_VIDEO_DECODER(msmpeg4v3,    MSMPEG4V3);
MF_VIDEO_DECODER(mjpeg,        MJPEG);

#define MF_ENCODER(MEDIATYPE, NAME, ID, OPTS, EXTRA) \
    static const AVClass ff_ ## NAME ## _mf_encoder_class = {                  \
        .class_name = #NAME "_mf",                                             \
        .item_name  = av_default_item_name,                                    \
        .option     = OPTS,                                                    \
        .version    = LIBAVUTIL_VERSION_INT,                                   \
    };                                                                         \
    AVCodec ff_ ## NAME ## _mf_encoder = {                                     \
        .priv_class     = &ff_ ## NAME ## _mf_encoder_class,                   \
        .name           = #NAME "_mf",                                         \
        .long_name      = NULL_IF_CONFIG_SMALL(#ID " via MediaFoundation"),    \
        .type           = AVMEDIA_TYPE_ ## MEDIATYPE,                          \
        .id             = AV_CODEC_ID_ ## ID,                                  \
        .priv_data_size = sizeof(MFContext),                                   \
        .probe          = mf_probe,                                            \
        .init           = mf_init,                                             \
        .close          = mf_close,                                            \
        .send_frame     = mf_send_frame,                                       \
        .receive_packet = mf_receive_packet,                                   \
        EXTRA                                                                  \
        .capabilities   = AV_CODEC_CAP_DELAY,                                  \
        .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |                       \
                          FF_CODEC_CAP_INIT_CLEANUP,                           \
    };

#define AFMTS \
        .sample_fmts    = (const enum AVSampleFormat[]){ AV_SAMPLE_FMT_S16,    \
                                                         AV_SAMPLE_FMT_NONE },

MF_ENCODER(AUDIO, aac,         AAC, NULL, AFMTS);
MF_ENCODER(AUDIO, ac3,         AC3, NULL, AFMTS);
MF_ENCODER(AUDIO, mp3,         MP3, NULL, AFMTS);

#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption venc_opts[] = {
    {"rate_control",  "Select rate control mode", OFFSET(opt_enc_rc), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, VE, "rate_control"},
    { "default",      "Default mode", 0, AV_OPT_TYPE_CONST, {.i64 = -1}, 0, 0, VE, "rate_control"},
    { "cbr",          "CBR mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_CBR}, 0, 0, VE, "rate_control"},
    { "pc_vbr",       "Peak constrained VBR mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_PeakConstrainedVBR}, 0, 0, VE, "rate_control"},
    { "u_vbr",        "Unconstrained VBR mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_UnconstrainedVBR}, 0, 0, VE, "rate_control"},
    { "quality",      "Quality mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_Quality}, 0, 0, VE, "rate_control" },
    // The following rate_control modes require Windows 8.
    { "ld_vbr",       "Low delay VBR mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_LowDelayVBR}, 0, 0, VE, "rate_control"},
    { "g_vbr",        "Global VBR mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_GlobalVBR}, 0, 0, VE, "rate_control" },
    { "gld_vbr",      "Global low delay VBR mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_GlobalLowDelayVBR}, 0, 0, VE, "rate_control"},
    {"quality",       "Quality", OFFSET(opt_enc_quality), AV_OPT_TYPE_INT, {.i64 = -1}, -1, 100, VE},
    {"hw_encoding",   "Force hardware encoding", OFFSET(opt_enc_d3d), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VE, "hw_encoding"},
    {NULL}
};

#define VFMTS \
        .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_NV12,       \
                                                        AV_PIX_FMT_YUV420P,    \
                                                        AV_PIX_FMT_NONE },

MF_ENCODER(VIDEO, h264,        H264, venc_opts, VFMTS);
MF_ENCODER(VIDEO, hevc,        HEVC, venc_opts, VFMTS);

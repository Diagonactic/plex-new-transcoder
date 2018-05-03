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

#include <windows.h>
#include <initguid.h>
#include <wmcodecdsp.h>
#include <mftransform.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <ksmedia.h>
#include <uuids.h>
#include <codecapi.h>
#include <d3d11.h>
#include <d3d9.h>
#include <dxva2api.h>

#include <stdint.h>
#include <string.h>

#include "avassert.h"
#include "buffer.h"
#include "common.h"
#include "imgutils.h"
#include "hwcontext.h"
#include "hwcontext_mf.h"
#include "hwcontext_internal.h"
#include "mem.h"
#include "pixfmt.h"
#include "pixdesc.h"

typedef struct MFDeviceContext {
    HANDLE d3d11_dll;
    HANDLE d3d9_dll;
    HANDLE dxva2_dll;
    IDirect3D9 *d3d9;
} MFDeviceContext;

typedef IDirect3D9* WINAPI pDirect3DCreate9(UINT);
typedef HRESULT WINAPI pCreateDeviceManager9(UINT *, IDirect3DDeviceManager9 **);

static void mf_uninit_d3d(AVHWDeviceContext *ctx)
{
    AVMFDeviceContext *hwctx = ctx->hwctx;
    MFDeviceContext *priv = ctx->internal->priv;

    if (hwctx->d3d11_manager)
        IMFDXGIDeviceManager_Release(hwctx->d3d11_manager);
    hwctx->d3d11_manager = NULL;

    if (priv->d3d11_dll)
        FreeLibrary(priv->d3d11_dll);
    priv->d3d11_dll = NULL;

    if (hwctx->d3d9_manager)
        IDirect3DDeviceManager9_Release(hwctx->d3d9_manager);
    hwctx->d3d9_manager = NULL;

    if (priv->d3d9)
        IDirect3D9_Release(priv->d3d9);
    priv->d3d9 = NULL;

    if (priv->d3d9_dll)
        FreeLibrary(priv->d3d9_dll);
    priv->d3d9_dll = NULL;

    if (priv->dxva2_dll)
        FreeLibrary(priv->dxva2_dll);
    priv->dxva2_dll = NULL;
}

static int mf_create_d3d11_device(AVHWDeviceContext *ctx, int loglevel)
{
    AVMFDeviceContext *hwctx = ctx->hwctx;
    MFDeviceContext *priv = ctx->internal->priv;
    HRESULT hr;
    UINT token;
    ID3D11Device *d3d11_device = NULL;
    ID3D10Multithread *multithread;
    HRESULT (WINAPI *pD3D11CreateDevice)(
    _In_opt_        IDXGIAdapter        *pAdapter,
                    D3D_DRIVER_TYPE     DriverType,
                    HMODULE             Software,
                    UINT                Flags,
    _In_opt_  const D3D_FEATURE_LEVEL   *pFeatureLevels,
                    UINT                FeatureLevels,
                    UINT                SDKVersion,
    _Out_opt_       ID3D11Device        **ppDevice,
    _Out_opt_       D3D_FEATURE_LEVEL   *pFeatureLevel,
    _Out_opt_       ID3D11DeviceContext **ppImmediateContext
    );
    HRESULT (WINAPI *pMFCreateDXGIDeviceManager)(
    _Out_ UINT                 *pResetToken,
    _Out_ IMFDXGIDeviceManager **ppDXVAManager
    );
    HANDLE mfplat_dll;

    if (hwctx->init_d3d11_device) {
        d3d11_device = hwctx->init_d3d11_device;
        ID3D11Device_AddRef(d3d11_device);
    } else {
        priv->d3d11_dll = LoadLibraryW(L"D3D11.dll");
        if (!priv->d3d11_dll)
            return AVERROR_EXTERNAL;

        pD3D11CreateDevice = (void *)GetProcAddress(priv->d3d11_dll, "D3D11CreateDevice");
        if (!pD3D11CreateDevice)
            return AVERROR_EXTERNAL;

        hr = pD3D11CreateDevice(0,
                                D3D_DRIVER_TYPE_HARDWARE,
                                NULL,
                                D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                NULL,
                                0,
                                D3D11_SDK_VERSION,
                                &d3d11_device,
                                NULL,
                                NULL);
        if (FAILED(hr)) {
            av_log(ctx, loglevel, "failed to create D3D device\n");
            goto error;
        }

        hr = IMFMediaBuffer_QueryInterface(d3d11_device, &IID_ID3D10Multithread, (void **)&multithread);
        if (FAILED(hr)) {
            av_log(ctx, loglevel, "could not get ID3D10Multithread\n");
            goto error;
        }

        hr = ID3D10Multithread_SetMultithreadProtected(multithread, TRUE);
        if (FAILED(hr)) {
            av_log(ctx, loglevel, "failed to call ID3D10Multithread::SetMultithreadProtected\n");
            ID3D10Multithread_Release(multithread);
            goto error;
        }

        ID3D10Multithread_Release(multithread);
    }

    // If this code is enabled, we already link against this DLL.
    // MFCreateDXGIDeviceManager is just not available on Windows 7.
    mfplat_dll = GetModuleHandleW(L"mfplat.dll");
    if (!mfplat_dll) {
        av_log(ctx, loglevel, "mfplat.dll not present\n");
        goto error;
    }
    pMFCreateDXGIDeviceManager = (void *)GetProcAddress(mfplat_dll, "MFCreateDXGIDeviceManager");
    if (!pMFCreateDXGIDeviceManager) {
        av_log(ctx, loglevel, "MFCreateDXGIDeviceManager not found\n");
        goto error;
    }

    hr = pMFCreateDXGIDeviceManager(&token, &hwctx->d3d11_manager);
    if (FAILED(hr)) {
        av_log(ctx, loglevel, "failed to create IMFDXGIDeviceManager\n");
        goto error;
    }

    hr = IMFDXGIDeviceManager_ResetDevice(hwctx->d3d11_manager, (IUnknown *)d3d11_device, token);
    if (FAILED(hr)) {
        av_log(ctx, loglevel, "failed to init IMFDXGIDeviceManager\n");
        goto error;
    }

    ID3D11Device_Release(d3d11_device);
    return 0;

error:
    if (d3d11_device)
        ID3D11Device_Release(d3d11_device);
    mf_uninit_d3d(ctx);
    return AVERROR_EXTERNAL;
}

static int mf_create_d3d9_device(AVHWDeviceContext *ctx, int loglevel)
{
    AVMFDeviceContext *hwctx = ctx->hwctx;
    MFDeviceContext *priv = ctx->internal->priv;
    pDirect3DCreate9      *createD3D = NULL;
    pCreateDeviceManager9 *createDeviceManager = NULL;
    IDirect3DDevice9      *d3d9device = NULL;
    HRESULT hr;
    D3DPRESENT_PARAMETERS d3dpp = {0};
    D3DDISPLAYMODE        d3ddm;
    unsigned resetToken = 0;

    if (hwctx->init_d3d9_device) {
        d3d9device = hwctx->init_d3d9_device;
        IDirect3DDevice9_AddRef(d3d9device);
    } else {
        priv->d3d9_dll = LoadLibraryW(L"d3d9.dll");
        if (!priv->d3d9_dll) {
            av_log(ctx, loglevel, "Failed to load D3D9 library\n");
            goto fail;
        }

        createD3D = (pDirect3DCreate9 *)GetProcAddress(priv->d3d9_dll, "Direct3DCreate9");
        if (!createD3D) {
            av_log(ctx, loglevel, "Failed to locate Direct3DCreate9\n");
            goto fail;
        }

        priv->d3d9 = createD3D(D3D_SDK_VERSION);
        if (!priv->d3d9) {
            av_log(ctx, loglevel, "Failed to create IDirect3D object\n");
            goto fail;
        }

        IDirect3D9_GetAdapterDisplayMode(priv->d3d9, hwctx->init_d3d9_adapter, &d3ddm);
        d3dpp.Windowed         = TRUE;
        d3dpp.BackBufferWidth  = 640;
        d3dpp.BackBufferHeight = 480;
        d3dpp.BackBufferCount  = 0;
        d3dpp.BackBufferFormat = d3ddm.Format;
        d3dpp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
        d3dpp.Flags            = D3DPRESENTFLAG_VIDEO;

        hr = IDirect3D9_CreateDevice(priv->d3d9, hwctx->init_d3d9_adapter, D3DDEVTYPE_HAL, GetDesktopWindow(),
                                    D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
                                    &d3dpp, &d3d9device);
        if (FAILED(hr)) {
            av_log(ctx, loglevel, "Failed to create Direct3D device\n");
            goto fail;
        }
    }

    priv->dxva2_dll = LoadLibraryW(L"dxva2.dll");
    if (!priv->dxva2_dll) {
        av_log(ctx, loglevel, "Failed to load DXVA2 library\n");
        goto fail;
    }
        createDeviceManager = (pCreateDeviceManager9 *)GetProcAddress(priv->dxva2_dll, "DXVA2CreateDirect3DDeviceManager9");
    if (!createDeviceManager) {
        av_log(ctx, loglevel, "Failed to locate DXVA2CreateDirect3DDeviceManager9\n");
        goto fail;
    }

    hr = createDeviceManager(&resetToken, &hwctx->d3d9_manager);
    if (FAILED(hr)) {
        av_log(ctx, loglevel, "Failed to create Direct3D device manager\n");
        goto fail;
    }

    hr = IDirect3DDeviceManager9_ResetDevice(hwctx->d3d9_manager, d3d9device, resetToken);
    if (FAILED(hr)) {
        av_log(ctx, loglevel, "Failed to bind Direct3D device to device manager\n");
        goto fail;
    }

    IDirect3DDevice9_Release(d3d9device);
    return 0;
fail:
    if (d3d9device)
        IDirect3DDevice9_Release(d3d9device);
    mf_uninit_d3d(ctx);
    return AVERROR_EXTERNAL;
}

static int mf_device_init(AVHWDeviceContext *ctx)
{
    AVMFDeviceContext *hwctx = ctx->hwctx;
    int ret;

    if (hwctx->device_type == AV_MF_NONE) {
        if (hwctx->d3d11_manager || hwctx->d3d9_manager)
            return AVERROR(EINVAL);
    } else if (hwctx->device_type == AV_MF_D3D11) {
        if (hwctx->d3d9_manager)
            return AVERROR(EINVAL);
        if (!hwctx->d3d11_manager && ((ret = mf_create_d3d11_device(ctx, AV_LOG_ERROR)) < 0))
            return ret;
    } else if (hwctx->device_type == AV_MF_D3D9) {
        if (hwctx->d3d11_manager)
            return AVERROR(EINVAL);
        if (!hwctx->d3d9_manager && ((ret = mf_create_d3d9_device(ctx, AV_LOG_ERROR)) < 0))
            return ret;
    } else if (hwctx->device_type == AV_MF_AUTO) {
        if (mf_create_d3d11_device(ctx, AV_LOG_VERBOSE) >= 0) {
            hwctx->device_type = AV_MF_D3D11;
        } else if (mf_create_d3d9_device(ctx, AV_LOG_VERBOSE) >= 0) {
            hwctx->device_type = AV_MF_D3D9;
        } else {
            hwctx->device_type = AV_MF_NONE;
        }
    } else {
        return AVERROR(EINVAL);
    }

    return 0;
}

static void mf_device_uninit(AVHWDeviceContext *ctx)
{
    AVMFDeviceContext *hwctx = ctx->hwctx;

    mf_uninit_d3d(ctx);

    if (hwctx->init_d3d9_device)
        IDirect3DDevice9_Release(hwctx->init_d3d9_device);

    if (hwctx->init_d3d11_device)
        ID3D11Device_Release(hwctx->init_d3d11_device);
}

static int mf_transfer_get_formats(AVHWFramesContext *ctx,
                                   enum AVHWFrameTransferDirection dir,
                                   enum AVPixelFormat **formats)
{
    *formats = av_malloc_array(2, sizeof(*formats));
    if (!*formats)
        return AVERROR(ENOMEM);

    (*formats)[0] = ctx->sw_format;
    (*formats)[1] = AV_PIX_FMT_NONE;

    return 0;
}

static int mf_transfer_data_from(AVHWFramesContext *ctx, AVFrame *dst,
                                    const AVFrame *src)
{
    IMFSample *sample = (void *)src->data[3];
    HRESULT hr;
    DWORD num_buffers;
    IMFMediaBuffer *buffer;
    IMF2DBuffer *buffer_2d = NULL;
    IMF2DBuffer2 *buffer_2d2 = NULL;
    uint8_t *src_data[4] = {0};
    int src_linesizes[4] = {0};
    int locked_1d = 0;
    int locked_2d = 0;
    int copy_w = FFMIN(dst->width, ctx->width);
    int copy_h = FFMIN(dst->height, ctx->height);
    int ret = 0;

    av_assert0(dst->format == ctx->sw_format);

    hr = IMFSample_GetBufferCount(sample, &num_buffers);
    if (FAILED(hr) || num_buffers != 1)
        return AVERROR_EXTERNAL;

    hr = IMFSample_GetBufferByIndex(sample, 0, &buffer);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;

    // Prefer IMF2DBuffer(2) if supported - it's faster, but usually only
    // present if hwaccel is used.
    hr = IMFMediaBuffer_QueryInterface(buffer, &IID_IMF2DBuffer, (void **)&buffer_2d);
    if (!FAILED(hr) && (ctx->sw_format == AV_PIX_FMT_NV12 ||
                        ctx->sw_format == AV_PIX_FMT_P010)) {
        BYTE *sc = NULL;
        LONG pitch = 0;

        // Prefer IMF2DBuffer2 if supported.
        IMFMediaBuffer_QueryInterface(buffer, &IID_IMF2DBuffer2, (void **)&buffer_2d2);
        if (buffer_2d2) {
            BYTE *start = NULL;
            DWORD length = 0;
            hr = IMF2DBuffer2_Lock2DSize(buffer_2d2, MF2DBuffer_LockFlags_Read, &sc, &pitch, &start, &length);
        } else {
            hr = IMF2DBuffer_Lock2D(buffer_2d, &sc, &pitch);
        }
        if (FAILED(hr)) {
            ret = AVERROR_EXTERNAL;
            goto done;
        }
        locked_2d = 1; // (always uses IMF2DBuffer_Unlock2D)

        src_data[0] = (uint8_t *)sc;
        src_linesizes[0] = pitch;
        src_data[1] = (uint8_t *)sc + pitch * ctx->height;
        src_linesizes[1] = pitch;
    } else {
        BYTE *data;
        DWORD length;

        hr = IMFMediaBuffer_Lock(buffer, &data, NULL, &length);
        if (FAILED(hr)) {
            ret = AVERROR_EXTERNAL;
            goto done;
        }
        locked_1d = 1;

        av_image_fill_arrays(src_data, src_linesizes, data, dst->format,
                             ctx->width, ctx->height, 1);
    }

    av_image_copy(dst->data, dst->linesize, (void *)src_data, src_linesizes,
                  ctx->sw_format, copy_w, copy_h);

done:

    if (locked_1d)
        IMFMediaBuffer_Unlock(buffer);
    if (locked_2d)
        IMF2DBuffer_Unlock2D(buffer_2d);
    if (buffer_2d)
        IMF2DBuffer_Release(buffer_2d);
    if (buffer_2d2)
        IMF2DBuffer2_Release(buffer_2d2);

    IMFMediaBuffer_Release(buffer);
    return ret;
}

const HWContextType ff_hwcontext_type_mf = {
    .type                 = AV_HWDEVICE_TYPE_MF,
    .name                 = "MF",

    .device_hwctx_size    = sizeof(AVMFDeviceContext),
    .device_priv_size     = sizeof(MFDeviceContext),
    .frames_priv_size     = 0,

    .device_init          = mf_device_init,
    .device_uninit        = mf_device_uninit,
    .frames_init          = NULL,
    .frames_get_buffer    = NULL,
    .transfer_get_formats = mf_transfer_get_formats,
    .transfer_data_to     = NULL,
    .transfer_data_from   = mf_transfer_data_from,

    .pix_fmts = (const enum AVPixelFormat[]){ AV_PIX_FMT_MF, AV_PIX_FMT_NONE },
};

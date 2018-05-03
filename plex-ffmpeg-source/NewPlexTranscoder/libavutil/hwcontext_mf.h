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

#ifndef AVUTIL_HWCONTEXT_MF_H
#define AVUTIL_HWCONTEXT_MF_H

/**
 * @file
 * An API-specific header for AV_HWDEVICE_TYPE_MF.
 */

#include <windows.h>
#include <d3d11.h>
#include <d3d9.h>
#include <mfidl.h>
#include <dxva2api.h>

enum AVMFDeviceType {
    AV_MF_AUTO,
    AV_MF_NONE,
    AV_MF_D3D9,
    AV_MF_D3D11,
};

/**
 * This struct is allocated as AVHWDeviceContext.hwctx
 *
 * All fields are considered immutable after av_hwdevice_ctx_init().
 */
typedef struct AVMFDeviceContext {
    /**
     * This can be set before av_hwdevice_ctx_init() to request creation of
     * a certain device.
     *
     * If set to AV_MF_AUTO, try to create a D3D11 device, and if that fails,
     * a D3D9 device, and if that fails, go with no device.
     *
     * If set to AV_MF_D3D9/D3D11, create a D3D9/D3D11 device.
     *
     * If set to AV_MF_NONE, do not use D3D at all. (This mode makes sense
     * when using MF in software mode; mostly by the libavcodec wrapper
     * itself.)
     *
     * If the fields have been set by the user, the init function will skip
     * creating devices, or return an error if there is an inconsistency.
     * The following conditions must be true for consistent settings:
     * - if set to AV_MF_AUTO, d3d11_manager!=NULL switches to AV_MF_D3D11,
     *   and d3d9_manager!=NULL switches to AV_MF_D3D9.
     * - if set to AV_MF_D3D11, d3d9_manager must be unset
     * - if set to AV_MF_D3D9, d3d11_manager must be unset
     * - if set to AV_MF_NONE, both d3d9_manager and d3d11_manager must be unset
     *
     * Note that the missing objects will be automatically created and set as
     * required.
     *
     * If the user sets any of the fields during init, the reference count
     * must have been incremented, and the AVMFDeviceContext will own the
     * references.
     *
     * Be aware that AVMFDeviceContext may load/unload DLLs related to the
     * objects on init/destruction. Trying to keep COM references may not
     * work if AVMFDeviceContext is destroyed.
     *
     * After init, this is set to the actual device that was created/found.
     */
    enum AVMFDeviceType device_type;

    /**
     * The following field is set for device_type==AV_MF_D3D11.
     * Will be released on AVHWDeviceContext termination.
     */
    IMFDXGIDeviceManager *d3d11_manager;

    /**
     * The following field is set for device_type==AV_MF_D3D9.
     * Will be released on AVHWDeviceContext termination.
     */
    IDirect3DDeviceManager9 *d3d9_manager;

    /**
     * Used if the d3d9_manager is created by this code; ignored otherwise. The
     * default is D3DADAPTER_DEFAULT (0).
     */
    UINT init_d3d9_adapter;

    /**
     * If d3d11_manager is not set, and device_type indicates d3d11 should be
     * used, this will be used to create a d3d11_manager.
     * This _must_ have been initialized with SetMultithreadProtected(TRUE).
     * Will be released on AVHWDeviceContext termination.
     */
    ID3D11Device *init_d3d11_device;

    /**
     * If d3d9_manager is not set, and device_type indicates d3d9 should be
     * used, this will be used to create a d3d9_manager.
     * Will be released on AVHWDeviceContext termination.
     */
    IDirect3DDevice9 *init_d3d9_device;
} AVMFDeviceContext;


/**
 * AVHWFramesContext.hwctx is currently not used
 */

#endif /* AVUTIL_HWCONTEXT_MF_H */

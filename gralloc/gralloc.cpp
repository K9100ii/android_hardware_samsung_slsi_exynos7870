/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <hardware/exynos/ion.h>
#include <linux/ion.h>
#include <exynos_ion.h>
#include <log/log.h>
#include <cutils/atomic.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include "gralloc_priv.h"
#include "exynos_format.h"
#include "gr.h"

#define PRIV_SIZE 64

#define MSCL_EXT_SIZE 512
#define MSCL_ALIGN 128

#if MALI_AFBC_GRALLOC == 1 /* It's for AFBC support on GPU DDK*/
#include "format_chooser.h"
#define GRALLOC_ARM_INTFMT_EXTENSION_BIT_START     32
/* This format will be use AFBC */
#define GRALLOC_ARM_INTFMT_AFBC                 (1ULL << (GRALLOC_ARM_INTFMT_EXTENSION_BIT_START+0))

#define AFBC_PIXELS_PER_BLOCK                    16
#define AFBC_BODY_BUFFER_BYTE_ALIGNMENT          1024
#define AFBC_HEADER_BUFFER_BYTES_PER_BLOCKENTRY  16
#endif

/*****************************************************************************/

struct gralloc_context_t {
    alloc_device_t  device;
    /* our private data here */
};

/*****************************************************************************/

int fb_device_open(const hw_module_t* module, const char* name,
                   hw_device_t** device);

static int gralloc_device_open(const hw_module_t* module, const char* name,
                               hw_device_t** device);

extern int gralloc_lock(gralloc_module_t const* module,
                        buffer_handle_t handle, int usage,
                        int l, int t, int w, int h,
                        void** vaddr);

extern int gralloc_unlock(gralloc_module_t const* module,
                          buffer_handle_t handle);

extern int gralloc_lock_ycbcr(gralloc_module_t const* module,
                        buffer_handle_t handle, int usage,
                        int l, int t, int w, int h,
                        android_ycbcr *ycbcr);

extern int gralloc_register_buffer(gralloc_module_t const* module,
                                   buffer_handle_t handle);

extern int gralloc_unregister_buffer(gralloc_module_t const* module,
                                     buffer_handle_t handle);

/*****************************************************************************/

static struct hw_module_methods_t gralloc_module_methods = {
    gralloc_device_open
};

/* version_major is for module_api_verison
 * lock_ycbcr is for MODULE_API_VERSION_0_2
 */
struct private_module_t HAL_MODULE_INFO_SYM = {
.base = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = GRALLOC_MODULE_API_VERSION_0_2,
        .hal_api_version = 0,
        .id = GRALLOC_HARDWARE_MODULE_ID,
        .name = "Graphics Memory Allocator Module",
        .author = "The Android Open Source Project",
        .methods = &gralloc_module_methods
    },
    .registerBuffer = gralloc_register_buffer,
    .unregisterBuffer = gralloc_unregister_buffer,
    .lock = gralloc_lock,
    .unlock = gralloc_unlock,
    .perform = NULL,
    .lock_ycbcr = gralloc_lock_ycbcr,
    .validateBufferSize = NULL,
    .getTransportSize = NULL,
},
.framebuffer = 0,
.flags = 0,
.numBuffers = 0,
.bufferMask = 0,
.lock = PTHREAD_MUTEX_INITIALIZER,
.currentBuffer = 0,
.ionfd = -1,
};

/*****************************************************************************/

static unsigned int _select_heap(int usage)
{
    unsigned int heap_mask;
#ifdef USES_EXYNOS_COMMON_GRALLOC
    if (usage & GRALLOC_USAGE_PROTECTED) {
        if (usage & GRALLOC_USAGE_PRIVATE_NONSECURE && !(usage & GRALLOC_USAGE_PHYSICALLY_LINEAR))
            heap_mask = ION_HEAP_SYSTEM_MASK;
        else
            heap_mask = ION_HEAP_EXYNOS_CONTIG_MASK;
    } else if (usage & GRALLOC_USAGE_CAMERA_RESERVED) {
        heap_mask = EXYNOS_ION_HEAP_CAMERA;
    } else if ((usage & GRALLOC_USAGE_PRIVATE_NONSECURE) && (usage & GRALLOC_USAGE_PHYSICALLY_LINEAR)) {
        heap_mask = EXYNOS_ION_HEAP_CRYPTO_MASK;
    } else if (usage & GRALLOC_USAGE_SECURE_CAMERA_RESERVED) {
        heap_mask = EXYNOS_ION_HEAP_SECURE_CAMERA;
    } else {
        heap_mask = ION_HEAP_SYSTEM_MASK;
    }
#else
	if (usage & GRALLOC_USAGE_PROTECTED)
		heap_mask = ION_HEAP_EXYNOS_CONTIG_MASK;
	else
		heap_mask = ION_HEAP_SYSTEM_MASK;
#endif

    return heap_mask;
}

/*
 * Define GRALLOC_ARM_FORMAT_SELECTION_DISABLE to disable the format selection completely
 */
static int gralloc_alloc_rgb(int ionfd, int w, int h, int format, int usage,
                             unsigned int ion_flags, private_handle_t **hnd, int *stride)
{
    size_t bpr = 0, ext_size=256, size = 0;
    int bpp = 0, vstride = 0;
    int fd = -1;
    uint32_t nblocks = 0;

    unsigned int heap_mask = _select_heap(usage);
    int is_compressible = check_for_compression(w, h, format, usage);

    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_FP16:
            bpp = 8;
            break;
        case HAL_PIXEL_FORMAT_EXYNOS_ARGB_8888:
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
        case HAL_PIXEL_FORMAT_RGBA_1010102:
            bpp = 4;
            break;
        case HAL_PIXEL_FORMAT_RGB_888:
            bpp = 3;
            break;
        case HAL_PIXEL_FORMAT_RGB_565:
        case HAL_PIXEL_FORMAT_RAW16:
        case HAL_PIXEL_FORMAT_RAW12:
        case HAL_PIXEL_FORMAT_RAW10:
        case HAL_PIXEL_FORMAT_RAW_OPAQUE:
            bpp = 2;
            break;
        case HAL_PIXEL_FORMAT_BLOB:
            *stride = w;
            vstride = h;
            size = w * h;
            break;
        default:
            return -EINVAL;
    }

    if (format != HAL_PIXEL_FORMAT_BLOB) {
        bpr = ALIGN(w, 16)* bpp;
        vstride = ALIGN(h, 16);

        if (vstride < h + 2)
            size = bpr * (h + 2);
        else
            size = bpr * vstride;

        *stride = bpr / bpp;
        size = size + ext_size;
        if (is_compressible)
        {
            /* if is_compressible = 1, width is alread 16 align so we can use width instead of w_aligned*/
            int h_aligned = ALIGN( h, AFBC_PIXELS_PER_BLOCK );
            nblocks = *stride / AFBC_PIXELS_PER_BLOCK * h_aligned / AFBC_PIXELS_PER_BLOCK;

            size = *stride * h_aligned * bpp +
                ALIGN( nblocks * AFBC_HEADER_BUFFER_BYTES_PER_BLOCKENTRY, AFBC_BODY_BUFFER_BYTE_ALIGNMENT ) + ext_size;

            /* Memory must be zeroed for using mmap during afbc header initialization */
            ion_flags &= ~ION_FLAG_NOZEROED;
        }
#ifdef GRALLOC_MSCL_ALIGN_RESTRICTION
        if (w % MSCL_ALIGN)
            size += MSCL_EXT_SIZE;
#endif
    }

    if (usage & GRALLOC_USAGE_PROTECTED) {
        if ((usage & GRALLOC_USAGE_PRIVATE_NONSECURE) && (usage & GRALLOC_USAGE_PHYSICALLY_LINEAR))
            ion_flags |= ION_EXYNOS_G2D_WFD_MASK;
        else if (usage & GRALLOC_USAGE_VIDEO_EXT)
            ion_flags |= (ION_EXYNOS_VIDEO_EXT_MASK | ION_FLAG_PROTECTED);
        else if ((usage & GRALLOC_USAGE_HW_COMPOSER) &&
            !(usage & GRALLOC_USAGE_HW_TEXTURE) && !(usage & GRALLOC_USAGE_HW_RENDER)) {
            // For DRM Playback
            ion_flags |= (ION_EXYNOS_FIMD_VIDEO_MASK | ION_FLAG_PROTECTED);
        }
        else
            ion_flags |= (ION_EXYNOS_MFC_OUTPUT_MASK | ION_FLAG_PROTECTED);
    } else if ((usage & GRALLOC_USAGE_PRIVATE_NONSECURE) && (usage & GRALLOC_USAGE_PHYSICALLY_LINEAR)) {
        ion_flags |= ION_FLAG_PROTECTED;
    }

    fd = exynos_ion_alloc(ionfd, size, heap_mask, ion_flags);
    if (fd < 0)
    {
        ALOGE("failed to get fd from exynos_ion_alloc, %s, %d\n", __func__, __LINE__);
        return -EINVAL;
    }

    *hnd = new private_handle_t(fd, size, usage, w, h, format, format,
                    format, *stride, vstride, is_compressible);

    return 0;
}

static int gralloc_alloc_framework_yuv(int ionfd, int w, int h, int format, int frameworkFormat,
                                       int usage, unsigned int ion_flags,
                                       private_handle_t **hnd, int *stride)
{
    size_t size=0, ext_size=256;
    int fd = -1;
    unsigned int heap_mask = _select_heap(usage);
    int is_compressible = 0;

    switch (format) {
        case HAL_PIXEL_FORMAT_YV12:
        case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P:
            *stride = ALIGN(w, 16);
            size = (*stride * h) + (ALIGN(*stride / 2, 16) * h) + ext_size;
            break;
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            *stride = w;
            size = *stride * h * 3 / 2 + ext_size;
            break;
        case HAL_PIXEL_FORMAT_Y8:
            *stride = ALIGN(w, 16);
            size = *stride * h;
            break;
        case HAL_PIXEL_FORMAT_Y16:
            *stride = ALIGN(w, 16);
            size = (*stride * h) * 2;
            break;
        default:
            ALOGE("invalid yuv format %d\n", format);
            return -EINVAL;
    }

#ifdef GRALLOC_MSCL_ALIGN_RESTRICTION
    if (w % MSCL_ALIGN)
        size += MSCL_EXT_SIZE;
#endif

    if (frameworkFormat == HAL_PIXEL_FORMAT_YCbCr_420_888)
        *stride = 0;

    fd = exynos_ion_alloc(ionfd, size, heap_mask, ion_flags);
    if (fd < 0)
    {
        ALOGE("failed to get fd from exynos_ion_alloc, %s, %d\n", __func__, __LINE__);
        return -EINVAL;
    }

    *hnd = new private_handle_t(fd, size,
                    usage, w, h, format, format, frameworkFormat, *stride, h, is_compressible);
    return 0;
}

static int gralloc_alloc_yuv(int ionfd, int w, int h, int format,
                             int usage, unsigned int ion_flags,
                             private_handle_t **hnd, int *stride)
{
    size_t luma_size=0, chroma_size=0, ext_size=256;
    int planes = 0, fd = -1, fd1 = -1, fd2 = -1;
    size_t luma_vstride = 0;
    unsigned int heap_mask = _select_heap(usage);
    // Keep around original requested format for later validation
    int frameworkFormat = format;
    int is_compressible = 0;
    uint64_t internal_format = 0;
    int size = 0, size1 = 0, size2 = 0;

    *stride = ALIGN(w, 16);
    luma_vstride = ALIGN(h, 16);

    if (format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
        ALOGV("HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED : usage(%x), flags(%x)\n", usage, ion_flags);
        if ((usage & GRALLOC_USAGE_HW_CAMERA_ZSL) == GRALLOC_USAGE_HW_CAMERA_ZSL) {
            format = HAL_PIXEL_FORMAT_YCbCr_422_I; // YUYV
        } else if ((usage & GRALLOC_USAGE_HW_TEXTURE) || (usage & GRALLOC_USAGE_HW_COMPOSER)) {
            if (usage & GRALLOC_USAGE_YUV_RANGE_FULL)
                format = HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_FULL; // NV21M Full
            else
                format = HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M;	//NV21M narrow
        } else if (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) {
            format = HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M;	//NV21M narrow
        } else {
            format = HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M;	//NV21M narrow
        }
    }
    else if (format == HAL_PIXEL_FORMAT_YCbCr_420_888) {
        if (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) {
            format = HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M;
        } else {
            // Flexible framework-accessible YUV format; map to NV21 for now
            format = HAL_PIXEL_FORMAT_YCrCb_420_SP;
        }
    }

    if (usage & GRALLOC_USAGE_PROTECTED) {
        ion_flags |= ION_FLAG_PROTECTED;
        if (usage & GRALLOC_USAGE_VIDEO_EXT)
            ion_flags |= ION_EXYNOS_VIDEO_EXT_MASK;
        else if (usage & GRALLOC_USAGE_PROTECTED_DPB)
            ion_flags |= ION_EXYNOS_VIDEO_EXT2_MASK;
        else if ((usage & GRALLOC_USAGE_HW_COMPOSER) &&
            !(usage & GRALLOC_USAGE_HW_TEXTURE) && !(usage & GRALLOC_USAGE_HW_RENDER)) {
            // For DRM Playback
            ion_flags |= ION_EXYNOS_FIMD_VIDEO_MASK;
        }
        else
            ion_flags |= ION_EXYNOS_MFC_OUTPUT_MASK;
    } else if (usage & GRALLOC_USAGE_CAMERA_RESERVED)
        ion_flags |= ION_EXYNOS_MFC_OUTPUT_MASK;

    // Sync iinternal_format with format
    internal_format = format;

    switch (format) {
        case HAL_PIXEL_FORMAT_EXYNOS_YV12_M:
        case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P_M:
            {
                *stride = ALIGN(w, 32);
                luma_size = luma_vstride * *stride + ext_size;
#ifdef EXYNOS_CHROMA_VSTRIDE_ALIGN
                chroma_size = ALIGN(luma_vstride / 2, CHROMA_VALIGN) * ALIGN(*stride / 2, 16) + ext_size;
#else
                chroma_size = (luma_vstride / 2) * ALIGN(*stride / 2, 16) + ext_size;
#endif
                planes = 3;
                break;
            }
        case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_TILED:
            {
                size_t chroma_vstride = ALIGN(h / 2, 32);
                luma_vstride = ALIGN(h, 32);
                luma_size = luma_vstride * *stride + ext_size;
                chroma_size = chroma_vstride * *stride + ext_size;
                planes = 2;
                break;
            }
        case HAL_PIXEL_FORMAT_YV12:
        case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P:
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
        case HAL_PIXEL_FORMAT_Y8:
        case HAL_PIXEL_FORMAT_Y16:
            return gralloc_alloc_framework_yuv(ionfd, w, h, format, frameworkFormat, usage,
                                               ion_flags, hnd, stride);
        case HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M:
        case HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_FULL:
        case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M:
            {
                luma_size = *stride * luma_vstride+ext_size;
#ifdef EXYNOS_CHROMA_VSTRIDE_ALIGN
                chroma_size = *stride * ALIGN(luma_vstride / 2, CHROMA_VALIGN)+ext_size;
#else
                chroma_size = *stride * ALIGN(luma_vstride / 2, 8)+ext_size;
#endif
                planes = 2;
                break;
            }
        case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_PRIV:
            {
                luma_vstride = ALIGN(h, 32);
                luma_size = *stride * luma_vstride+ext_size;
                chroma_size = *stride * ALIGN(luma_vstride / 2, 8)+ext_size;
                planes = 3;
                break;
            }
        case HAL_PIXEL_FORMAT_YCbCr_422_I:
            {
                luma_vstride = h;
                luma_size = luma_vstride * *stride * 2+ext_size;
                planes = 1;
                break;
            }
        case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN:
            {
#ifdef EXYNOS_CHROMA_VSTRIDE_ALIGN
                chroma_size = ALIGN((*stride * ALIGN(luma_vstride / 2, CHROMA_VALIGN)) + ext_size, 16);
#else
                chroma_size = ALIGN((*stride * luma_vstride / 2) + ext_size, 16);
#endif
                luma_size = (*stride * luma_vstride) + ext_size + chroma_size;
                planes = 1;
                break;
            }
        case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B:
            {
#ifdef GRALLOC_10B_ALIGN_RESTRICTION
                luma_size = (*stride * luma_vstride + ext_size) + ((ALIGN(w / 4, 16) * luma_vstride) + ext_size);
                chroma_size = (*stride * luma_vstride / 2 + ext_size) + ((ALIGN(w / 4, 16) * (luma_vstride / 2)) + ext_size);
#else
                luma_size = (*stride * luma_vstride + ext_size) + ((ALIGN(w / 4, 16) * h) + ext_size);
                chroma_size = (*stride * luma_vstride / 2 + ext_size) + ((ALIGN(w / 4, 16) * (h / 2)) + ext_size);
#endif
                planes = 3;
                break;
            }
        case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B:
            {
                chroma_size = ALIGN((*stride * luma_vstride / 2) + ext_size, 16) + (ALIGN(w / 4, 16) * (luma_vstride / 2)) + 64;
                luma_size = (*stride * luma_vstride) + ext_size + (ALIGN(w / 4, 16) * luma_vstride) + 64 + chroma_size;
                planes = 1;
                break;
            }
        case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_P010_M:
            {
                chroma_size = ((*stride * 2) * luma_vstride / 2) + ext_size;
                luma_size = ((*stride * 2) * luma_vstride) + ext_size;
                planes = 3;
                break;
            }
        default:
            ALOGE("invalid yuv format %d\n", format);
            return -EINVAL;
    }

#ifdef GRALLOC_MSCL_ALIGN_RESTRICTION
    if (w % MSCL_ALIGN) {
        luma_size += MSCL_EXT_SIZE;
        chroma_size += MSCL_EXT_SIZE/2;
    }
#endif

    size = luma_size;
    fd = exynos_ion_alloc(ionfd, size, heap_mask, ion_flags);
    if (fd < 0) {
        if (usage & GRALLOC_USAGE_PROTECTED_DPB) {
            ion_flags &= ~ION_EXYNOS_VIDEO_EXT2_MASK;
            ion_flags |= ION_EXYNOS_MFC_OUTPUT_MASK;
            fd = exynos_ion_alloc(ionfd, size, heap_mask, ion_flags);
            if (fd < 0)
            {
                ALOGE("failed to get fd from exynos_ion_alloc, %s, %d\n", __func__, __LINE__);
                return -EINVAL;
            }
        } else {
            ALOGE("failed to get fd from exynos_ion_alloc, %s, %d\n", __func__, __LINE__);
            return -EINVAL;
        }
    }

    if (planes == 1) {
        *hnd = new private_handle_t(fd, size, usage, w, h,
                                    format, internal_format, frameworkFormat, *stride, luma_vstride, is_compressible);
    } else {
        size1 = chroma_size;
        fd1 = exynos_ion_alloc(ionfd, size1, heap_mask, ion_flags);
        if (fd1 < 0)
        {
            ALOGE("failed to get fd from exynos_ion_alloc, %s, %d\n", __func__, __LINE__);
            close(fd);
            return -EINVAL;
        }

        if (planes == 3) {
            if ((format == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_PRIV) ||
                (format == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B)) {
                size2 = PRIV_SIZE;
                fd2 = exynos_ion_alloc(ionfd, size2, ION_HEAP_SYSTEM_MASK, 0);
            } else {
                size2 = chroma_size;
                fd2 = exynos_ion_alloc(ionfd, size2, heap_mask, ion_flags);
            }
            if (fd2 < 0)
            {
                ALOGE("failed to get fd2 from exynos_ion_alloc, %s, %d\n", __func__, __LINE__);
                close(fd);
                close(fd1);
                return -EINVAL;
            }

            *hnd = new private_handle_t(fd, fd1, fd2, size, size1, size2, usage, w, h,
                                        format, internal_format, frameworkFormat, *stride, luma_vstride, is_compressible);
        } else {
            *hnd = new private_handle_t(fd, fd1, size, size1, usage, w, h,
                                        format, internal_format, frameworkFormat, *stride, luma_vstride, is_compressible);
        }
    }

    return 0;
}

static int gralloc_alloc(alloc_device_t* dev,
                         int w, int h, int format, int usage,
                         buffer_handle_t* pHandle, int* pStride)
{
    int stride;
    int err;
    unsigned int ion_flags = 0;
    private_handle_t *hnd = NULL;

    if (!pHandle || !pStride || w <= 0 || h <= 0)
        return -EINVAL;

    if ((usage & GRALLOC_USAGE_SW_READ_MASK) == GRALLOC_USAGE_SW_READ_OFTEN) {
        ion_flags = ION_FLAG_CACHED | ION_FLAG_CACHED_NEEDS_SYNC;
        if (usage & GRALLOC_USAGE_HW_RENDER)
            ion_flags |= ION_FLAG_SYNC_FORCE;
    }

    if (usage & GRALLOC_USAGE_HW_RENDER)
        ion_flags |= ION_FLAG_MAY_HWRENDER;

    if (usage & GRALLOC_USAGE_NOZEROED)
        ion_flags |= ION_FLAG_NOZEROED;

    private_module_t* m = reinterpret_cast<private_module_t*>
        (dev->common.module);

    err = gralloc_alloc_rgb(m->ionfd, w, h, format, usage, ion_flags, &hnd,
                            &stride);
    if (err)
        err = gralloc_alloc_yuv(m->ionfd, w, h, format, usage, ion_flags,
                                &hnd, &stride);
    if (err)
        goto err;

    *pHandle = hnd;
    *pStride = stride;
    return 0;
err:
    if (!hnd)
        return err;
    close(hnd->fd);
    if (hnd->fd1 >= 0)
        close(hnd->fd1);
    if (hnd->fd2 >= 0)
        close(hnd->fd2);
    delete hnd;
    return err;
}

static int gralloc_free(alloc_device_t* dev,
                        buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t const* hnd = reinterpret_cast<private_handle_t const*>(handle);
    gralloc_module_t* module = reinterpret_cast<gralloc_module_t*>(
                                                                   dev->common.module);
    if (hnd->base)
        grallocUnmap(const_cast<private_handle_t*>(hnd));

    if (hnd->handle)
        exynos_ion_free_handle(getIonFd(module), hnd->handle);
    if (hnd->handle1)
        exynos_ion_free_handle(getIonFd(module), hnd->handle1);
    if (hnd->handle2)
        exynos_ion_free_handle(getIonFd(module), hnd->handle2);

    close(hnd->fd);
    if (hnd->fd1 >= 0)
        close(hnd->fd1);
    if (hnd->fd2 >= 0)
        close(hnd->fd2);

    delete hnd;
    return 0;
}

/*****************************************************************************/

static int gralloc_close(struct hw_device_t *dev)
{
    gralloc_context_t* ctx = reinterpret_cast<gralloc_context_t*>(dev);
    if (ctx) {
        /* TODO: keep a list of all buffer_handle_t created, and free them
         * all here.
         */
        free(ctx);
    }
    return 0;
}

int gralloc_device_open(const hw_module_t* module, const char* name,
                        hw_device_t** device)
{
    int status = -EINVAL;
    if (!strcmp(name, GRALLOC_HARDWARE_GPU0)) {
        gralloc_context_t *dev;
        dev = (gralloc_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = gralloc_close;

        dev->device.alloc = gralloc_alloc;
        dev->device.free = gralloc_free;

        private_module_t *p = reinterpret_cast<private_module_t*>(dev->device.common.module);
        if (p->ionfd == -1)
            p->ionfd = exynos_ion_open();

        *device = &dev->device.common;
        status = 0;
    } else {
        status = fb_device_open(module, name, device);
    }
    return status;
}

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
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <log/log.h>
#include <cutils/atomic.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>
#include <inttypes.h>
#include <sync/sync.h>

#include <hardware/exynos/ion.h>
#include <linux/ion.h>
#include <exynos_ion.h>
#include "gralloc_priv.h"
#include "exynos_format.h"

#define INT_TO_PTR(var) ((void *)(unsigned long)var)
#define MSCL_EXT_SIZE 512
#define MSCL_ALIGN 128

#define PRIV_SIZE 64

#include "format_chooser.h"

/*****************************************************************************/
int getIonFd(gralloc_module_t const *module)
{
    private_module_t* m = const_cast<private_module_t*>(reinterpret_cast<const private_module_t*>(module));
    if (m->ionfd == -1)
        m->ionfd = exynos_ion_open();
    return m->ionfd;
}

static int gralloc_map(gralloc_module_t const* module, buffer_handle_t handle)
{
    void *privAddress;

    private_handle_t *hnd = (private_handle_t*)handle;
    hnd->base = hnd->base1 = hnd->base2 = 0;

    switch (hnd->format) {
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B:
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_PRIV:
        privAddress = mmap(0, hnd->size2, PROT_READ|PROT_WRITE, MAP_SHARED, hnd->fd2, 0);
        if (privAddress == MAP_FAILED) {
            ALOGE("%s: could not mmap %s", __func__, strerror(errno));
        } else {
            hnd->base2 = (uint64_t)privAddress;
            exynos_ion_sync_fd(getIonFd(module), hnd->fd2);
        }
        break;
    default:
        break;
    }

    if ((hnd->flags & GRALLOC_USAGE_PROTECTED) &&
            !(hnd->flags & GRALLOC_USAGE_PRIVATE_NONSECURE)) {
        return 0;
    }

    if (!(hnd->flags & GRALLOC_USAGE_PROTECTED) && !(hnd->flags & GRALLOC_USAGE_NOZEROED)) {
        void* mappedAddress = mmap(0, hnd->size, PROT_READ|PROT_WRITE, MAP_SHARED,
                                   hnd->fd, 0);
        if (mappedAddress == MAP_FAILED) {
            ALOGE("%s: could not mmap %s", __func__, strerror(errno));
            return -errno;
        }
        hnd->base = (uint64_t)mappedAddress;
        exynos_ion_sync_fd(getIonFd(module), hnd->fd);

        if (hnd->fd1 >= 0) {
            void *mappedAddress1 = (void*)mmap(0, hnd->size1, PROT_READ|PROT_WRITE,
                                                MAP_SHARED, hnd->fd1, 0);
            if (mappedAddress1 == MAP_FAILED) {
                ALOGE("%s: could not mmap %s", __func__, strerror(errno));
                return -errno;
            }
            hnd->base1 = (uint64_t)mappedAddress1;
            exynos_ion_sync_fd(getIonFd(module), hnd->fd1);
        }
        if (hnd->fd2 >= 0) {
            if ((hnd->format != HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_PRIV) &&
                (hnd->format != HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B)) {
                void *mappedAddress2 = (void*)mmap(0, hnd->size2, PROT_READ|PROT_WRITE, MAP_SHARED, hnd->fd2, 0);
                if (mappedAddress2 == MAP_FAILED) {
                    ALOGE("%s: could not mmap %s", __func__, strerror(errno));
                    return -errno;
                }
                hnd->base2 = (uint64_t)mappedAddress2;
                exynos_ion_sync_fd(getIonFd(module), hnd->fd2);
            }
        }
    }

    return 0;
}

static int gralloc_unmap(buffer_handle_t handle)
{
    private_handle_t* hnd = (private_handle_t*)handle;

    switch (hnd->format) {
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B:
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_PRIV:
        if (munmap(INT_TO_PTR(hnd->base2), hnd->size2) < 0) {
            ALOGE("%s :could not unmap %s %#" PRIx64 " %d", __func__, strerror(errno), hnd->base2, hnd->size2);
        }
        hnd->base2 = 0;
        break;
    default:
        break;
    }

    if (!hnd->base)
        return 0;

    if (munmap(INT_TO_PTR(hnd->base), hnd->size) < 0) {
        ALOGE("%s :could not unmap %s %#" PRIx64 " %d", __func__, strerror(errno), hnd->base, hnd->size);
    }
    hnd->base = 0;

    if (hnd->fd1 >= 0) {
        if (!hnd->base1)
            return 0;
        if (munmap(INT_TO_PTR(hnd->base1), hnd->size1) < 0) {
            ALOGE("%s :could not unmap %s %#" PRIx64 " %d", __func__, strerror(errno), hnd->base1, hnd->size1);
        }
        hnd->base1 = 0;
    }
    if (hnd->fd2 >= 0) {
        if (!hnd->base2)
            return 0;
        if (munmap(INT_TO_PTR(hnd->base2), hnd->size2) < 0) {
            ALOGE("%s :could not unmap %s %#" PRIx64 " %d", __func__, strerror(errno), hnd->base2, hnd->size2);
        }
        hnd->base2 = 0;
    }
    return 0;
}

/*****************************************************************************/

int grallocMap(gralloc_module_t const* module, private_handle_t *hnd)
{
    return gralloc_map(module, hnd);
}

int grallocUnmap(private_handle_t *hnd)
{
    return gralloc_unmap(hnd);
}

/*****************************************************************************/

int gralloc_register_buffer(gralloc_module_t const* module,
                            buffer_handle_t handle)
{
    int err;
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    err = gralloc_map(module, handle);

    private_handle_t* hnd = (private_handle_t*)handle;
    ALOGV("%s: base %#" PRIx64 " %d %d %d %d\n", __func__, hnd->base, hnd->size,
          hnd->width, hnd->height, hnd->stride);

    int ret;
    ret = exynos_ion_import_handle(getIonFd(module), hnd->fd, &hnd->handle);
    if (ret)
        ALOGE("error importing handle %d %x\n", hnd->fd, hnd->format);
    if (hnd->fd1 >= 0) {
        ret = exynos_ion_import_handle(getIonFd(module), hnd->fd1, &hnd->handle1);
        if (ret)
            ALOGE("error importing handle1 %d %x\n", hnd->fd1, hnd->format);
    }
    if (hnd->fd2 >= 0) {
        ret = exynos_ion_import_handle(getIonFd(module), hnd->fd2, &hnd->handle2);
        if (ret)
            ALOGE("error importing handle2 %d %x\n", hnd->fd2, hnd->format);
    }

    return err;
}

int gralloc_unregister_buffer(gralloc_module_t const* module,
                              buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t* hnd = (private_handle_t*)handle;
    ALOGV("%s: base %#" PRIx64 " %d %d %d %d\n", __func__, hnd->base, hnd->size,
          hnd->width, hnd->height, hnd->stride);

    gralloc_unmap(handle);

    if (hnd->handle)
        exynos_ion_free_handle(getIonFd(module), hnd->handle);
    if (hnd->handle1)
        exynos_ion_free_handle(getIonFd(module), hnd->handle1);
    if (hnd->handle2)
        exynos_ion_free_handle(getIonFd(module), hnd->handle2);

    return 0;
}

int gralloc_lock(gralloc_module_t const* module,
                 buffer_handle_t handle, int usage,
                 int l, int t, int w, int h,
                 void** vaddr)
{
    // this is called when a buffer is being locked for software
    // access. in thin implementation we have nothing to do since
    // not synchronization with the h/w is needed.
    // typically this is used to wait for the h/w to finish with
    // this buffer if relevant. the data cache may need to be
    // flushed or invalidated depending on the usage bits and the
    // hardware.

    if (private_handle_t::validate(handle) < 0)
    {
        ALOGE("handle is not valid. usage(%x), l,t,w,h(%d, %d, %d, %d)\n", usage, l, t, w, h);
        return -EINVAL;
    }

    private_handle_t* hnd = (private_handle_t*)handle;

    if (hnd->frameworkFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) {
        ALOGE("gralloc_lock can't be used with YCbCr_420_888 format");
        return -EINVAL;
    }

    switch(hnd->format)
    {
        case HAL_PIXEL_FORMAT_EXYNOS_ARGB_8888:
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
        case HAL_PIXEL_FORMAT_RGB_888:
        case HAL_PIXEL_FORMAT_RGB_565:
        case HAL_PIXEL_FORMAT_RAW16:
        case HAL_PIXEL_FORMAT_RAW_OPAQUE:
        case HAL_PIXEL_FORMAT_BLOB:
        case HAL_PIXEL_FORMAT_YCbCr_422_I:
        case HAL_PIXEL_FORMAT_Y8:
        case HAL_PIXEL_FORMAT_Y16:
        case HAL_PIXEL_FORMAT_YV12:
        case HAL_PIXEL_FORMAT_RGBA_1010102:
        case HAL_PIXEL_FORMAT_RGBA_FP16:
            break;
        default:
            ALOGE("gralloc_lock doesn't support YUV formats. Please use gralloc_lock_ycbcr()");
            return -EINVAL;
    }

#ifdef GRALLOC_RANGE_FLUSH
    if(usage & GRALLOC_USAGE_SW_WRITE_MASK)
    {
        hnd->lock_usage = GRALLOC_USAGE_SW_WRITE_RARELY;
        hnd->lock_offset = t * hnd->stride;
        hnd->lock_len = h * hnd->stride;
    }
    else
    {
        hnd->lock_usage = 0;
        hnd->lock_offset = 0;
        hnd->lock_len = 0;
    }
#endif

    if (!hnd->base)
        gralloc_map(module, hnd);

    *vaddr = INT_TO_PTR(hnd->base);

    return 0;
}

int gralloc_unlock(gralloc_module_t const* module,
                   buffer_handle_t handle)
{
    // we're done with a software buffer. nothing to do in this
    // implementation. typically this is used to flush the data cache.
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t* hnd = (private_handle_t*)handle;

    if (!((hnd->flags & GRALLOC_USAGE_SW_READ_MASK) == GRALLOC_USAGE_SW_READ_OFTEN))
        return 0;

#ifdef GRALLOC_RANGE_FLUSH
    if(((hnd->format == HAL_PIXEL_FORMAT_RGBA_8888)
         || (hnd->format == HAL_PIXEL_FORMAT_RGBX_8888)) && (hnd->lock_offset != 0))
         exynos_ion_sync_fd_partial(getIonFd(module), hnd->fd, hnd->lock_offset * 4, hnd->lock_len * 4);
    else
        exynos_ion_sync_fd(getIonFd(module), hnd->fd);

    if (hnd->fd1 >= 0)
        exynos_ion_sync_fd(getIonFd(module), hnd->fd1);
    if (hnd->fd2 >= 0)
        exynos_ion_sync_fd(getIonFd(module), hnd->fd2);
#else
    exynos_ion_sync_fd(getIonFd(module), hnd->fd);

    if (hnd->fd1 >= 0)
        exynos_ion_sync_fd(getIonFd(module), hnd->fd1);
    if (hnd->fd2 >= 0)
        exynos_ion_sync_fd(getIonFd(module), hnd->fd2);
#endif

    return 0;
}

int gralloc_lock_ycbcr(gralloc_module_t const* module,
                        buffer_handle_t handle, int usage,
                        int l, int t, int w, int h,
                        android_ycbcr *ycbcr)
{
    if (private_handle_t::validate(handle) < 0)
    {
        ALOGE("handle is not valid. usage(%x), l,t,w,h(%d, %d, %d, %d)\n", usage, l, t, w, h);
        return -EINVAL;
    }

    if (!ycbcr) {
        ALOGE("gralloc_lock_ycbcr got NULL ycbcr struct");
        return -EINVAL;
    }

    int ext_size = 256;

    private_handle_t* hnd = (private_handle_t*)handle;

    if (!hnd->base)
        gralloc_map(module, hnd);

    // If all CPU addresses are still NULL, do not anything.
    if (!hnd->base && !hnd->base1 && !hnd->base2)
        return 0;

    // Calculate offsets to underlying YUV data
    size_t yStride;
    size_t cStride;
    size_t uOffset;
    size_t vOffset;
    size_t cStep;
    switch (hnd->format) {
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
        yStride = cStride = hnd->width;
        vOffset = yStride * hnd->height;
        uOffset = vOffset + 1;
        cStep = 2;
        ycbcr->y  = (void *)((unsigned long)hnd->base);
        ycbcr->cb = (void *)(((unsigned long)hnd->base) + uOffset);
        ycbcr->cr = (void *)(((unsigned long)hnd->base) + vOffset);
        break;
    case HAL_PIXEL_FORMAT_YV12:
        yStride = ALIGN(hnd->width, 16);
        cStride = ALIGN(yStride/2, 16);
        vOffset = yStride * hnd->height;
        uOffset = vOffset + (cStride * (hnd->height / 2));
        cStep = 1;
        ycbcr->y  = (void*)((unsigned long)hnd->base);
        ycbcr->cr = (void*)(((unsigned long)hnd->base) + vOffset);
        ycbcr->cb = (void*)(((unsigned long)hnd->base) + uOffset);
        cStep = 1;
        break;
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M:
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_PRIV:
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B:
        yStride = cStride = hnd->stride;
        vOffset = 1;
        cStep = 2;
        ycbcr->y  = (void *)((unsigned long)hnd->base);
        ycbcr->cb = (void *)((unsigned long)hnd->base1);
        ycbcr->cr = (void *)(((unsigned long)hnd->base1) + vOffset);

        if ((hnd->format != HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M) && (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER))  /* usage name will be changed as GRALLOC_USAGE_HW_VIDEO since v2.0 */
            ycbcr->cr = (void *)((unsigned long)hnd->base2);
        break;
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P:
        yStride = hnd->stride;
        cStride = ALIGN(yStride/2, 16);
        uOffset = yStride * hnd->height;
        vOffset = uOffset + (cStride * (hnd->height / 2));
        cStep = 1;
        ycbcr->y  = (void *)((unsigned long)hnd->base);
        ycbcr->cb = (void*)(((unsigned long)hnd->base) + uOffset);
        ycbcr->cr = (void*)(((unsigned long)hnd->base) + vOffset);
        break;
    /* separated color plane */
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_TILED:  /* can't describe tiled format for user application */
        if (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) {  /* usage name will be changed as GRALLOC_USAGE_HW_VIDEO since v2.0 */
            yStride = hnd->stride;
            cStride = hnd->stride;
            cStep   = 1;
            ycbcr->y  = (void *)((unsigned long)hnd->base);
            ycbcr->cb = (void *)((unsigned long)hnd->base1);
            ycbcr->cr = NULL;
        } else {
            ALOGE("gralloc_lock_ycbcr unexpected internal format %x",
                    hnd->format);
            return -EINVAL;
        }
        break;
    case HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M:
    case HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_FULL:
        yStride = cStride = hnd->stride;
        uOffset = 1;
        cStep = 2;
        ycbcr->y  = (void *)((unsigned long)hnd->base);
        ycbcr->cr = (void *)((unsigned long)hnd->base1);
        ycbcr->cb = (void *)(((unsigned long)hnd->base1) + uOffset);
        break;
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P_M:
        yStride = hnd->stride;
        cStride = ALIGN(yStride/2, 16);
        cStep = 1;
        ycbcr->y  = (void *)((unsigned long)hnd->base);
        ycbcr->cb = (void *)((unsigned long)hnd->base1);
        ycbcr->cr = (void *)((unsigned long)hnd->base2);
        break;
    case HAL_PIXEL_FORMAT_EXYNOS_YV12_M:
        yStride = hnd->stride;
        cStride = ALIGN(yStride/2, 16);
        cStep = 1;
        ycbcr->y  = (void *)((unsigned long)hnd->base);
        ycbcr->cr = (void *)((unsigned long)hnd->base1);
        ycbcr->cb = (void *)((unsigned long)hnd->base2);
        break;
    case HAL_PIXEL_FORMAT_YCbCr_422_I:
        yStride = cStride = hnd->stride;
        cStep = 1;
        ycbcr->y = (void *)((unsigned long)hnd->base);
        ycbcr->cb = 0;
        ycbcr->cr = 0;
        break;
    /* included h/w restrictions */
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN:
        yStride = cStride = hnd->stride;
        uOffset = (yStride * hnd->vstride) + ext_size;
        vOffset = uOffset + 1;
        cStep = 2;
        ycbcr->y  = (void *)((unsigned long)hnd->base);
        ycbcr->cb = (void *)(((unsigned long)hnd->base) + uOffset);
        ycbcr->cr = (void *)(((unsigned long)hnd->base) + vOffset);
        break;
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B:
        yStride = cStride = hnd->stride;
        uOffset = ((yStride * hnd->vstride) + ext_size) + ((ALIGN(hnd->width / 4, 16) * hnd->vstride) + 64);
        vOffset = uOffset + 1;
        cStep = 2;
        ycbcr->y  = (void *)((unsigned long)hnd->base);
        ycbcr->cb = (void *)(((unsigned long)hnd->base) + uOffset);
        ycbcr->cr = (void *)(((unsigned long)hnd->base) + vOffset);
        break;
    case HAL_PIXEL_FORMAT_Y8:
    case HAL_PIXEL_FORMAT_Y16:
        yStride = cStride = hnd->stride;
        uOffset = 0;
        vOffset = 0;
        cStep = 1;
        ycbcr->y  = (void *)((unsigned long)hnd->base);
        ycbcr->cb = 0;
        ycbcr->cr = 0;
        break;
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_P010_M:
        yStride = cStride = hnd->stride;
        vOffset = 2;
        cStep = 2;
        ycbcr->y  = (void *)((unsigned long)hnd->base);
        ycbcr->cb = (void *)((unsigned long)hnd->base1);
        ycbcr->cr = (void *)((unsigned long)hnd->base2);
        break;
    default:
        ALOGE("gralloc_lock_ycbcr unexpected internal format %x",
                hnd->format);
        return -EINVAL;
    }

    ycbcr->ystride = yStride;
    ycbcr->cstride = cStride;
    ycbcr->chroma_step = cStep;

    // Zero out reserved fields
    memset(ycbcr->reserved, 0, sizeof(ycbcr->reserved));

/*
    ALOGD("gralloc_lock_ycbcr success. format : %x, usage: %x, ycbcr.y: %p, .cb: %p, .cr: %p, "
            ".ystride: %d , .cstride: %d, .chroma_step: %d", hnd->format, usage,
            ycbcr->y, ycbcr->cb, ycbcr->cr, ycbcr->ystride, ycbcr->cstride,
            ycbcr->chroma_step);
*/

    return 0;
}

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

#ifndef GRALLOC_PRIV_H_
#define GRALLOC_PRIV_H_

#include <stdint.h>
#include <limits.h>
#include <sys/cdefs.h>
#include <hardware/gralloc.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <cutils/native_handle.h>
#include <linux/fb.h>
#include <linux/ion.h>

/* SLSI specific usages */
#define GRALLOC_USAGE_PROTECTED_DPB                 0x00800000U
#define GRALLOC_USAGE_PHYSICALLY_LINEAR             0x01000000U
#define GRALLOC_USAGE_PRIVATE_NONSECURE             0x02000000U
#define GRALLOC_USAGE_CAMERA_RESERVED               0x04000000U
#define GRALLOC_USAGE_NOZEROED                      0x08000000U
#define GRALLOC_USAGE_VIDEO_EXT                     GRALLOC_USAGE_PRIVATE_0
#define GRALLOC_USAGE_YUV_RANGE_FULL                GRALLOC_USAGE_PRIVATE_1
#define GRALLOC_USAGE_DAYDREAM_SINGLE_BUFFER_MODE   GRALLOC_USAGE_PRIVATE_2
#define GRALLOC_USAGE_SECURE_CAMERA_RESERVED        GRALLOC_USAGE_PRIVATE_3

#define AFBC_INFO_SIZE                              (sizeof(int))
#define AFBC_ENABLE                                 (0xafbc)

enum {
	PREFER_COMPRESSION_NO_CHANGE = 0x00,
	PREFER_COMPRESSION_ENABLE    = 0x01,
	PREFER_COMPRESSION_DISABLE   = 0x02,
};

/*****************************************************************************/

struct private_module_t;
struct private_handle_t;

struct private_module_t {
    gralloc_module_t base;

    private_handle_t* framebuffer;
    uint32_t flags;
    uint32_t numBuffers;
    uint32_t bufferMask;
    pthread_mutex_t lock;
    buffer_handle_t currentBuffer;
    int ionfd;

    struct fb_var_screeninfo info;
    struct fb_fix_screeninfo finfo;
    int xres;
    int yres;
    int line_length;
    float xdpi;
    float ydpi;
    float fps;
    void *queue;
    pthread_mutex_t queue_lock;

};

/*****************************************************************************/

#ifdef __cplusplus
struct private_handle_t : public native_handle {
#else
struct private_handle_t {
    struct native_handle nativeHandle;
#endif

    enum {
        PRIV_FLAGS_FRAMEBUFFER = 0x00000001,
        PRIV_FLAGS_USES_UMP    = 0x00000002,
        PRIV_FLAGS_USES_ION    = 0x00000020
    };

    // file-descriptors
    int     fd;
    int     fd1;
    int     fd2;
    // ints
    int     magic; // 18
    int     flags; // 1c
    int     size; // 20
    int     size1; // 24
    int     size2; // 28

    int     offset; // 2c
    int     format; // 30
    int     __unknown2; // 34
    int     internal_format; // 38

    int     format_top_bit; // 3c

    int     frameworkFormat; // 40
    int     width; // 44
    int     height; // 48
    int     stride; // 4c
    int     vstride; // 50
    int     is_compressible; // 54

    // FIXME: the attributes below should be out-of-line
    int __unknown3; // 58
    int __unknown4; // 5c
    int __unknown5; // 60
    /* not sure about these three */
    int __unknown6; // 64
    int compressed_out; // 68
    int prefer_compression; // 6c

#ifdef __cplusplus
    static inline int sNumInts() {
        return (((sizeof(private_handle_t) - sizeof(native_handle_t))/sizeof(int)) - sNumFds);
    }
    static const int sNumFds = 3;
    static const int sMagic = 0x3141592;

    private_handle_t(int fd, int size, int flags) :
        fd(fd), fd1(-1), fd2(-1), magic(sMagic), flags(flags), size(size), size1(0), size2(0),
        offset(0), format(0), internal_format(0), frameworkFormat(0), width(0), height(0), stride(0), vstride(0),
        is_compressible(0), compressed_out(0), prefer_compression(PREFER_COMPRESSION_NO_CHANGE),
        dssRatio(0), handle(0), handle1(0), handle2(0), base(0), base1(0), base2(0)

    {
        version = sizeof(native_handle);
        numInts = sNumInts() + 2;
        numFds = sNumFds -2 ;
    }

    private_handle_t(int fd, int size, int flags, int w,
                    int h, int format, uint64_t internal_format, int frameworkFormat, int stride, int vstride, int is_compressible) :
        fd(fd), fd1(-1), fd2(-1), magic(sMagic), flags(flags), size(size), size1(0), size2(0),
        offset(0), format(format), internal_format(internal_format), frameworkFormat(frameworkFormat), width(w), height(h), stride(stride), vstride(vstride),
        is_compressible(is_compressible), compressed_out(0), prefer_compression(PREFER_COMPRESSION_NO_CHANGE),
        dssRatio(0), handle(0), handle1(0), handle2(0), base(0), base1(0), base2(0)

    {
        version = sizeof(native_handle);
        numInts = sNumInts() + 2;
        numFds = sNumFds - 2;
    }

    private_handle_t(int fd, int fd1, int size, int size1, int flags, int w,
                    int h, int format, uint64_t internal_format, int frameworkFormat, int stride, int vstride, int is_compressible) :
        fd(fd), fd1(fd1), fd2(-1), magic(sMagic), flags(flags), size(size), size1(size1), size2(0),
        offset(0), format(format), internal_format(internal_format), frameworkFormat(frameworkFormat), width(w), height(h), stride(stride), vstride(vstride),
        is_compressible(is_compressible), compressed_out(0), prefer_compression(PREFER_COMPRESSION_NO_CHANGE),
        dssRatio(0), handle(0), handle1(0), handle2(0), base(0), base1(0), base2(0)

    {
        version = sizeof(native_handle);
        numInts = sNumInts() + 1;
        numFds = sNumFds - 1;
    }

    private_handle_t(int fd, int fd1, int fd2, int size, int size1, int size2, int flags, int w,
                    int h, int format, uint64_t internal_format, int frameworkFormat, int stride, int vstride, int is_compressible) :
        fd(fd), fd1(fd1), fd2(fd2), magic(sMagic), flags(flags), size(size), size1(size1), size2(size2),
        offset(0), format(format), internal_format(internal_format), frameworkFormat(frameworkFormat), width(w), height(h), stride(stride), vstride(vstride),
        is_compressible(is_compressible), compressed_out(0), prefer_compression(PREFER_COMPRESSION_NO_CHANGE),
        dssRatio(0), handle(0), handle1(0), handle2(0), base(0), base1(0), base2(0)

    {
        version = sizeof(native_handle);
        numInts = sNumInts();
        numFds = sNumFds;
    }
    ~private_handle_t() {
        magic = 0;
    }

    static int validate(const native_handle* h) {
        const private_handle_t* hnd = (const private_handle_t*)h;
        if (!h || h->version != sizeof(native_handle) ||
                hnd->numInts + hnd->numFds != sNumInts() + sNumFds ||
                hnd->magic != sMagic)
        {
            ALOGE("invalid gralloc handle (at %p)", reinterpret_cast<void *>(const_cast<native_handle *>(h)));
            return -EINVAL;
        }
        return 0;
    }

    static private_handle_t* dynamicCast(const native_handle* in)
    {
        if (validate(in) == 0)
            return const_cast<private_handle_t*>(static_cast<const private_handle_t*>(in));

        return NULL;
    }

    /* not sure about these four */
    int     lock_usage; // 70
    int     lock_offset; // 74
    int     lock_len; // 78
    int     dssRatio; // 7c

    ion_user_handle_t handle; // 80
    ion_user_handle_t handle1; // 84
    ion_user_handle_t handle2; // 88
/* 8c padding */
    uint64_t base __attribute__((aligned(8))); // 90
    uint64_t base1 __attribute__((aligned(8))); // 98
    uint64_t base2 __attribute__((aligned(8))); // a0
#endif
};
#endif /* GRALLOC_PRIV_H_ */

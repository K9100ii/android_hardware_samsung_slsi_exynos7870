/*
 * Copyright (C) 2014 ARM Limited. All rights reserved.
 *
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdlib.h>
#include <hardware/hardware.h>
#include <log/log.h>
#include <cutils/properties.h>
#include <hardware/gralloc.h>
#include "format_chooser.h"

#define FBT (GRALLOC_USAGE_HW_FB | GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER)
#define GENERAL_UI (GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER)

#ifdef USES_EXYNOS_AFBC_FEATURE
/* It's for compression check format, width, usage*/
int check_for_compression(int w, int h, int format, int usage)
{
	switch(format)
	{
		/* AFBC disabled for 2 and 3 byte formats because
		 * 2,3 byte formats + AFBC + 64byte align not yet compatible with DPU.
		 */
		case HAL_PIXEL_FORMAT_RGB_565:
		case HAL_PIXEL_FORMAT_RGB_888:
			return 0;
		case HAL_PIXEL_FORMAT_RGBA_8888:
		case HAL_PIXEL_FORMAT_BGRA_8888:
		case HAL_PIXEL_FORMAT_RGBX_8888:
		case HAL_PIXEL_FORMAT_YV12:
		{
			if (usage & GRALLOC_USAGE_PROTECTED)
				return 0;
			if ((w <= 192) || (h <= 192)) /* min restriction for performance */
				return 0;
			if( (usage & (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK)) != 0 || usage == 0 )
				return 0;
			if (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER)
				return 0;
			if ((usage & FBT) || (usage & GENERAL_UI)) /*only support FBT and General UI */
				return 1;
			else
				return 0;

			break;
		}
	}

	return 0;
}
#else
int check_for_compression(__unused int w, __unused int h, __unused int format, __unused int usage)
{
	return 0;
}
#endif

uint64_t gralloc_select_format(int req_format, int usage, int is_compressible)
{
	uint64_t new_format = req_format;

	if( req_format == 0 )
	{
		return 0;
	}

	if( (usage & (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK)) != 0 ||
             usage == 0 )
	{
		return new_format;
	}

	if( is_compressible == 0)
	{
		return new_format;
	}
#if 0
	/* This is currently a limitation with the display and will be removed eventually
	 *  We can't allocate fbdev framebuffer buffers in AFBC format */
	if( usage & GRALLOC_USAGE_HW_FB )
	{
		return new_format;
	}
#endif
	new_format |= GRALLOC_ARM_INTFMT_AFBC;

	return new_format;
}

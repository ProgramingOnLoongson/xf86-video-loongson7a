/*
 * Copyright © 2013 ARM Limited.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include "drmmode_driver.h"
#include <stddef.h>
#include <xf86drmMode.h>
#include <xf86drm.h>

/* taken from libdrm */

union omap_gem_size {
	uint32_t bytes;		/* (for non-tiled formats) */
	struct {
		uint16_t width;
		uint16_t height;
	} tiled;		/* (for tiled formats) */
};

struct drm_omap_gem_new {
	union omap_gem_size size;	/* in */
	uint32_t flags;			/* in */
	uint32_t handle;		/* out */
	uint32_t __pad;
};


/* Cursor dimensions
 * Technically we probably don't have any size limit, since we are just 
 * using an overlay. But xserver will always create cursor images in the
 * max size, so don't use width/height values that are too big
 */

#define CURSORW   (64)
#define CURSORH   (64)
/* Padding added down each side of cursor image */
#define CURSORPAD (0)


static int LS7A_InitPlaneForCursor(int drm_fd, uint32_t plane_id)
{
	int res = -1;
	drmModeObjectPropertiesPtr props;
	props = drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
	if (props)
	{
		int i;

		for (i = 0; i < props->count_props; ++i)
		{
			drmModePropertyPtr this_prop;
			this_prop = drmModeGetProperty(drm_fd, props->props[i]);

			if (this_prop)
			{
				if (!strncmp(this_prop->name, "zorder", DRM_PROP_NAME_LEN))
				{
					res = drmModeObjectSetProperty(drm_fd,
					          plane_id, DRM_MODE_OBJECT_PLANE,
					          this_prop->prop_id, 1);
					drmModeFreeProperty(this_prop);
					break;
				}
				drmModeFreeProperty(this_prop);
			}
		}
		drmModeFreeObjectProperties(props);
	}

	return res;
}


struct drmmode_interface loongson7a_interface = {
	/* Must match name used in the kernel driver */
	.driver_name = "loongson-drm",
	/* DRM page flip events should be requested and waited for 
	 * during DRM_IOCTL_MODE_PAGE_FLIP. */
	.use_page_flip_events = 1,
	/* allows the next back buffer to be obtained while 
	 * the previous is being flipped. */
	.use_early_display = 1,
	.cursor_width = CURSORW,
	.cursor_height = CURSORH,
	.cursor_padding = CURSORPAD,
	/* No hardware cursor - use a software cursor */
	.cursor_api = HWCURSOR_API_NONE,
	.init_plane_for_cursor = LS7A_InitPlaneForCursor,
	.vblank_query_supported = 0,
};

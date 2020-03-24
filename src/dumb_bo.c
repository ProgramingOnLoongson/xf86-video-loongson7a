/*
 * Copyright © 2007 Red Hat, Inc.
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
 * Authors:
 *    Dave Airlie <airlied@redhat.com>
 *    Sui Jingfeng <suijingfeng@loongson.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "dumb_bo.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
// libdrm, drmPrimeFDToHandle
#include <xf86drm.h>
// drmModeRmFB, drmModeAddFB
#include <xf86drmMode.h>


#include "loongson_debug.h"

#ifndef ALIGN
#define ALIGN(val, align)	(((val) + (align) - 1) & ~((align) - 1))
#endif

struct dumb_bo * dumb_bo_create(int fd,
               const unsigned width, const unsigned height, const unsigned bpp)
{
    struct drm_mode_create_dumb arg;
    struct dumb_bo *bo;
    int ret;

    bo = calloc(1, sizeof(*bo));
    if (!bo)
        return NULL;

    memset(&arg, 0, sizeof(arg));
    arg.width = width;
    arg.height = height;
    arg.bpp = bpp;

    ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg);
    if (ret)
    {
        xf86DrvMsg(-1, X_ERROR,
               "CREATE_DUMB({fd: %d, %dx%d, bpp: %d}) failed. ret:%d, errno:%d, %s\n",
               fd, width, height, bpp, ret, errno, strerror(errno));
        // suijingfeng: dislike goto.
        free(bo);
        return NULL;
    }

    bo->handle = arg.handle;
    bo->size = arg.size;
    bo->pitch = arg.pitch;

    return bo;
}


int dumb_bo_map(int fd, struct dumb_bo *bo)
{
    struct drm_mode_map_dumb arg;
    int ret;
    void *map;
    // suijingfeng: trace get commented, as overwhelming
    // TRACE_ENTER();

    if (bo->ptr)
    {
        // suijingfeng: already mapped ???
        // TRACE_EXIT();
        return 0;
    }

    memset(&arg, 0, sizeof(arg));
    arg.handle = bo->handle;

    ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &arg);
    if (ret)
    {
        TRACE_EXIT();
        return ret;
    }
    map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, arg.offset);
    if (map == MAP_FAILED)
    {
        TRACE_EXIT();
        return -errno;
    }
    bo->ptr = map;
    return 0;
}



int dumb_bo_destroy(int fd, struct dumb_bo *bo)
{
    struct drm_mode_destroy_dumb arg;
    int ret;

    if (bo->ptr) {
        munmap(bo->ptr, bo->size);
        bo->ptr = NULL;
    }

    memset(&arg, 0, sizeof(arg));
    arg.handle = bo->handle;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &arg);
    if (ret)
        return -errno;

    free(bo);
    return 0;
}



/* OUTPUT SLAVE SUPPORT */
// using prime_fd instead handle is more clear
struct dumb_bo * dumb_get_bo_from_fd(int fd, int prime_fd, int pitch, int size)
{
    struct dumb_bo *bo;
    int ret;

    bo = calloc(1, sizeof(*bo));
    if (!bo)
        return NULL;

    ret = drmPrimeFDToHandle(fd, prime_fd, &bo->handle);
    if (ret) {
        free(bo);
        return NULL;
    }
    bo->pitch = pitch;
    bo->size = size;
    return bo;
}

/////////////////////////////////////////////////////////////////


/* buffer-object related functions:
 */

void armsoc_bo_clear_dmabuf(struct dumb_bo *bo)
{
    assert(bo->refcnt > 0);
    assert(armsoc_bo_has_dmabuf(bo));

    close(bo->dmabuf);
    bo->dmabuf = -1;
}

int armsoc_bo_has_dmabuf(struct dumb_bo *bo)
{
    return bo->dmabuf > 0;
}



int armsoc_bo_to_dmabuf(struct dumb_bo *bo, int * pPrimeFD)
{
    assert(bo->refcnt > 0);
    assert(!armsoc_bo_has_dmabuf(bo));

    return drmPrimeHandleToFD(bo->fd, bo->handle, 0, pPrimeFD);
}



struct dumb_bo * armsoc_bo_new_with_dim( int fd,
            uint32_t width, uint32_t height, uint8_t depth, uint8_t bpp)
{
    struct dumb_bo * new_buf = NULL;

    TRACE_ENTER();

    new_buf = dumb_bo_create( fd,  width, height, bpp );
    if (new_buf == NULL)
    {
        return NULL;
    }


    // those three's value will be filled by create dumb ioctl
    // new_buf->handle = arg.handle;
    // new_buf->size = arg.size;
    // new_buf->pitch = arg.pitch;

    new_buf->fd = fd;
    new_buf->width = width;
    new_buf->height = height;
    new_buf->bpp = bpp;
    new_buf->original_size = new_buf->size;
    new_buf->ptr = NULL;

    // suijingfeng:
    // How does thoes guy get inlvoved ?
    // I want remove them all, but I can't ...
    new_buf->fb_id = 0;
    new_buf->depth = depth;

    new_buf->refcnt = 1;
    new_buf->dmabuf = -1;
    new_buf->name = 0;

    TRACE_EXIT();

    return new_buf;
}


void armsoc_bo_unreference(struct dumb_bo *bo)
{
    TRACE_ENTER();

    if (bo == NULL)
    {
        DEBUG_MSG("Unreference NULL BO.");
        TRACE_EXIT();
        return;
    }

    if (--bo->refcnt == 0)
    {
        armsoc_bo_rm_fb(bo);
        dumb_bo_destroy(bo->fd, bo);
    }

    TRACE_EXIT();
}


void armsoc_bo_reference(struct dumb_bo *bo)
{
    TRACE_ENTER();

    if (bo == NULL)
    {
        xf86DrvMsg(-1, X_INFO, "Reference: BO = NULL ???.\n");

        TRACE_EXIT();
        return;
    }

    ++bo->refcnt;

    TRACE_EXIT();
}


int armsoc_bo_get_name(struct dumb_bo *bo, uint32_t *name)
{
	if (bo->name == 0) {
		int ret;
		struct drm_gem_flink flink;

		assert(bo->refcnt > 0);
		flink.handle = bo->handle;

		ret = drmIoctl(bo->fd, DRM_IOCTL_GEM_FLINK, &flink);
		if (ret) {
			xf86DrvMsg(-1, X_ERROR,
			           "_GEM_FLINK(handle:0x%X)failed. errno:0x%X\n",
			           flink.handle, errno);
			return ret;
		}

		bo->name = flink.name;
	}

	*name = bo->name;

	return 0;
}


uint32_t armsoc_bo_handle(struct dumb_bo *bo)
{
	assert(bo->refcnt > 0);
	return bo->handle;
}

uint32_t armsoc_bo_size(struct dumb_bo *bo)
{
	assert(bo->refcnt > 0);
	return bo->size;
}

uint32_t armsoc_bo_width(struct dumb_bo *bo)
{
	assert(bo->refcnt > 0);
	return bo->width;
}

uint32_t armsoc_bo_height(struct dumb_bo *bo)
{
	assert(bo->refcnt > 0);
	return bo->height;
}

uint8_t armsoc_bo_depth(struct dumb_bo *bo)
{
	assert(bo->refcnt > 0);
	return bo->depth;
}

uint32_t armsoc_bo_bpp(struct dumb_bo *bo)
{
    assert(bo->refcnt > 0);
    return bo->bpp;
}

uint32_t armsoc_bo_pitch(struct dumb_bo *bo)
{
    assert(bo->refcnt > 0);
    return bo->pitch;
}


int armsoc_bo_cpu_prep(struct dumb_bo *bo)
{
    int ret = 0;

    assert(bo->refcnt > 0);
    if (armsoc_bo_has_dmabuf(bo))
    {
        fd_set fds;
        /* 10s before printing a msg */
        const struct timeval timeout = {10, 0};
        struct timeval t;

        FD_ZERO(&fds);
        FD_SET(bo->dmabuf, &fds);

        do {
            t = timeout;
            ret = select(bo->dmabuf + 1, &fds, NULL, NULL, &t);
            if (ret == 0)
                xf86DrvMsg(-1, X_ERROR,
                        "select() on dma_buf fd has timed-out\n");
        } while ((ret == -1 && errno == EINTR) || ret == 0);

        if (ret > 0)
            ret = 0;
    }
    return ret;
}

int armsoc_bo_cpu_fini(struct dumb_bo *bo)
{
    assert(bo->refcnt > 0);
    return msync(bo->ptr, bo->size, MS_SYNC | MS_INVALIDATE);
}


int armsoc_bo_add_fb(struct dumb_bo *bo)
{
    int ret;

    assert(bo->refcnt > 0);
    assert(bo->fb_id == 0);

    ret = drmModeAddFB(bo->fd, bo->width, bo->height, bo->depth,
            bo->bpp, bo->pitch, bo->handle, &bo->fb_id);

    if (ret < 0)
    {
        bo->fb_id = 0;
        return ret;
    }

    return 0;
}

int armsoc_bo_rm_fb(struct dumb_bo *bo)
{
    int ret;

    if ( bo->fb_id )
    {
        ret = drmModeRmFB(bo->fd, bo->fb_id);
        if (ret < 0)
        {
            xf86DrvMsg(-1, X_ERROR,
                    "Could not remove fb from bo %d\n", ret);
            return ret;
        }
    }

    bo->fb_id = 0;

    return 0;
}


uint32_t armsoc_bo_get_fb(struct dumb_bo *bo)
{
    assert(bo->refcnt > 0);
    return bo->fb_id;
}


int armsoc_bo_clear(struct dumb_bo *bo)
{

    assert(bo->refcnt > 0);

    dumb_bo_map(bo->fd, bo);

    if (NULL == bo->ptr)
    {
        xf86DrvMsg(-1, X_ERROR,
                "Couldn't map scanout bo\n");
        return -1;
    }

    if (armsoc_bo_cpu_prep(bo))
    {
        xf86DrvMsg(-1, X_ERROR,
                " %s: bo_cpu_prep failed - unable to synchronise access.\n",
                __func__);
        return -1;
    }

    memset(bo->ptr, 0x0, bo->size);
    armsoc_bo_cpu_fini(bo);
    return 0;
}


int armsoc_bo_resize(struct dumb_bo *bo, uint32_t new_width, uint32_t new_height)
{
    uint32_t new_size;
    uint32_t new_pitch;

    assert(bo != NULL);
    assert(new_width > 0);
    assert(new_height > 0);
    /* The caller must remove the fb object before
     * attempting to resize.
     */
    assert(bo->fb_id == 0);
    assert(bo->refcnt > 0);

    xf86DrvMsg(-1, X_INFO, "Resizing bo from %dx%d to %dx%d\n",
            bo->width, bo->height, new_width, new_height);

    /* TODO: MIDEGL-1563: Get pitch from DRM as
     * only DRM knows the ideal pitch and alignment
     * requirements
     * */
    new_pitch  = new_width * ((armsoc_bo_bpp(bo) + 7) / 8);
    /* Align pitch to 64 byte */
    new_pitch  = ALIGN(new_pitch, 256);
    new_size   = (((new_height - 1) * new_pitch) +
            (new_width * ((armsoc_bo_bpp(bo) + 7) / 8)));

    if (new_size <= bo->original_size) {
        bo->width  = new_width;
        bo->height = new_height;
        bo->pitch  = new_pitch;
        bo->size   = new_size;
        return 0;
    }
    xf86DrvMsg(-1, X_ERROR, "Failed to resize buffer\n");
    return -1;
}

/*
 * Copyright © 2007 Red Hat, Inc.
 * Copyright © 2020 Loongson Corporation
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
 *     Dave Airlie <airlied@redhat.com>
 *     Sui Jingfeng <suijingfeng@loongson.cn>
 */
#ifndef DUMB_BO_H
#define DUMB_BO_H

#include <stdint.h>


struct dumb_bo {

    uint32_t handle;
    uint32_t size;
    void *ptr;
    uint32_t pitch;

    int fd;
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint8_t depth;
    uint8_t bpp;

    int refcnt;
    int dmabuf;
    /* initial size of backing memory. Used on resize to
     * check if the new size will fit
     */
    uint32_t original_size;
    uint32_t name;
};


///////////////////////////////////////////////////////////////////////////////

struct dumb_bo *dumb_bo_create(int fd, const unsigned width,
                               const unsigned height, const unsigned bpp);
int dumb_bo_map(int fd, struct dumb_bo *bo);
int dumb_bo_destroy(int fd, struct dumb_bo *bo);
struct dumb_bo *dumb_get_bo_from_fd(int fd, int handle, int pitch, int size);

///////////////////////////////////////////////////////////////////////////////

int armsoc_bo_get_name(struct dumb_bo *bo, uint32_t *name);
uint32_t armsoc_bo_handle(struct dumb_bo *bo);

int armsoc_bo_add_fb(struct dumb_bo *bo);
uint32_t armsoc_bo_get_fb(struct dumb_bo *bo);

uint32_t armsoc_bo_size(struct dumb_bo *bo);

struct dumb_bo *armsoc_bo_new_with_dim( int fd,
uint32_t width, uint32_t height, uint8_t depth, uint8_t bpp);

uint32_t armsoc_bo_width(struct dumb_bo *bo);
uint32_t armsoc_bo_height(struct dumb_bo *bo);
uint8_t armsoc_bo_depth(struct dumb_bo *bo);
uint32_t armsoc_bo_bpp(struct dumb_bo *bo);
uint32_t armsoc_bo_pitch(struct dumb_bo *bo);

void armsoc_bo_reference(struct dumb_bo *bo);
void armsoc_bo_unreference(struct dumb_bo *bo);

/* When dmabuf is set on a bo, armsoc_bo_cpu_prep()
 *  waits for KDS shared access
 */

void armsoc_bo_clear_dmabuf(struct dumb_bo *bo);
int armsoc_bo_has_dmabuf(struct dumb_bo *bo);
int armsoc_bo_clear(struct dumb_bo *bo);
int armsoc_bo_rm_fb(struct dumb_bo *bo);
int armsoc_bo_resize(struct dumb_bo *bo, uint32_t new_width,
						uint32_t new_height);

int armsoc_bo_to_dmabuf(struct dumb_bo *bo, int * pPrimeFD);

// ?
int armsoc_bo_cpu_prep(struct dumb_bo *bo);
int armsoc_bo_cpu_fini(struct dumb_bo *bo);


#endif

/*
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
 *    Sui Jingfeng <suijingfeng@loongson.cn>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/poll.h>

#include <xf86drm.h>
#include <xf86Crtc.h>
#include "loongson_driver.h"
#include "loongson_present.h"

int drmmode_page_flip(ScreenPtr pScreen, DrawablePtr draw, uint32_t fb_id,
        Bool async, void *priv)
{
    // ScreenPtr pScreen = draw->pScreen;
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    struct ARMSOCRec *pARMSOC = loongsonPTR(pScrn);
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);

    struct drmmode_crtc_private_rec *crtc = config->crtc[0]->driver_private;
    struct drmmode_rec *mode = crtc->drmmode;

    int ret, i, failed = 0, num_flipped = 0;

    unsigned int flags = 0;

    if (pARMSOC->drmmode.pageflip)
        flags |= DRM_MODE_PAGE_FLIP_EVENT;
    if (async)
        flags |= DRM_MODE_PAGE_FLIP_ASYNC;

    /* if we can flip, we must be fullscreen.. so flip all CRTC's.. */

    for (i = 0; i < config->num_crtc; i++)
    {
        crtc = config->crtc[i]->driver_private;

        xf86CrtcPtr pCrtc = config->crtc[i];

        if (!ms_crtc_on(pCrtc))
            continue;

        ret = drmModePageFlip(mode->fd, crtc->crtc_id,
                fb_id, flags, priv);
        if (ret)
        {
            xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                    "flip queue failed: %s\n",
                    strerror(errno));
            failed = 1;
        }
        else
        {
            num_flipped += 1;
        }
    }

    if (failed)
        return -(num_flipped + 1);
    else
        return num_flipped;
}



///////////////////////////////////////////////////////////////////////////

/*

typedef void (*ls_pageflip_handler_proc)(struct loongsonRec * ls,
                                         uint64_t frame,
                                         uint64_t usec,
                                         void *data);

typedef void (*ls_pageflip_abort_proc)(struct loongsonRec * ls, void *data);

Bool ms_do_pageflip(ScreenPtr screen,
               PixmapPtr new_front,
               void *event,
               int ref_crtc_vblank_pipe,
               Bool async,
               ls_pageflip_handler_proc pageflip_handler,
               ls_pageflip_abort_proc pageflip_abort,
               const char *log_prefix)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct ARMSOCRec * ms = loongsonPTR(scrn);
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    drmmode_bo new_front_bo;
    uint32_t flags;
    int i;
    struct ms_flipdata *flipdata;
    ms->glamor.block_handler(screen);

    new_front_bo.gbm = ms->glamor.gbm_bo_from_pixmap(screen, new_front);
    new_front_bo.dumb = NULL;

    if (!new_front_bo.gbm) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: Failed to get GBM BO for flip to new front.\n",
                   log_prefix);
        return FALSE;
    }

    flipdata = calloc(1, sizeof(struct ms_flipdata));
    if (!flipdata) {
        drmmode_bo_destroy(&ms->drmmode, &new_front_bo);
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: Failed to allocate flipdata.\n", log_prefix);
        return FALSE;
    }

    flipdata->event = event;
    flipdata->screen = screen;
    flipdata->event_handler = pageflip_handler;
    flipdata->abort_handler = pageflip_abort;

    //
    // Take a local reference on flipdata.
    // if the first flip fails, the sequence abort
    // code will free the crtc flip data, and drop
    // it's reference which would cause this to be
    // freed when we still required it.
    //
    flipdata->flip_count++;

    // Create a new handle for the back buffer
    flipdata->old_fb_id = ms->drmmode.fb_id;

    new_front_bo.width = new_front->drawable.width;
    new_front_bo.height = new_front->drawable.height;
    if (drmmode_bo_import(&ms->drmmode, &new_front_bo,
                          &ms->drmmode.fb_id)) {
        if (!ms->drmmode.flip_bo_import_failed) {
            xf86DrvMsg(scrn->scrnIndex, X_WARNING, "%s: Import BO failed: %s\n",
                       log_prefix, strerror(errno));
            ms->drmmode.flip_bo_import_failed = TRUE;
        }
        goto error_out;
    } else {
        if (ms->drmmode.flip_bo_import_failed &&
            new_front != screen->GetScreenPixmap(screen))
            ms->drmmode.flip_bo_import_failed = FALSE;
    }

    flags = DRM_MODE_PAGE_FLIP_EVENT;
    if (async)
        flags |= DRM_MODE_PAGE_FLIP_ASYNC;

    // Queue flips on all enabled CRTCs.
    //
    // Note that if/when we get per-CRTC buffers, we'll have to update this.
    // Right now it assumes a single shared fb across all CRTCs, with the
    // kernel fixing up the offset of each CRTC as necessary.
    //
    // Also, flips queued on disabled or incorrectly configured displays
    // may never complete; this is a configuration error.
    //
    for (i = 0; i < config->num_crtc; ++i)
    {
        xf86CrtcPtr crtc = config->crtc[i];

        if (!ms_crtc_on(crtc))
            continue;

        if (!queue_flip_on_crtc(screen, crtc, flipdata,
                                ref_crtc_vblank_pipe, flags))
        {
            xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                       "%s: Queue flip on CRTC %d failed: %s\n",
                       log_prefix, i, strerror(errno));
            goto error_undo;
        }
    }

    drmmode_bo_destroy(&ms->drmmode, &new_front_bo);

    //
    // Do we have more than our local reference,
    // if so and no errors, then drop our local
    // reference and return now.
    //
    if (flipdata->flip_count > 1)
    {
        --flipdata->flip_count;
        return TRUE;
    }

error_undo:

    //
    // Have we just got the local reference?
    // free the framebuffer if so since nobody successfully
    // submitted anything
    //
    if (flipdata->flip_count == 1)
    {
        drmModeRmFB(ms->fd, ms->drmmode.fb_id);
        ms->drmmode.fb_id = flipdata->old_fb_id;
    }

error_out:
    xf86DrvMsg(scrn->scrnIndex, X_WARNING, "Page flip failed: %s\n",
               strerror(errno));
    drmmode_bo_destroy(&ms->drmmode, &new_front_bo);
    // if only the local reference - free the structure,
    // else drop the local reference and return
    if (flipdata->flip_count == 1)
        free(flipdata);
    else
        --flipdata->flip_count;

    return FALSE;
}

*/

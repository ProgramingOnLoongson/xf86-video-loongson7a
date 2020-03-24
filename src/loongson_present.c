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


#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include <xf86.h>
#include <xf86Crtc.h>
#include <xf86drm.h>
#include <xf86str.h>
#include <present.h>

#include "loongson_driver.h"
#include "drmmode_display.h"
#include "driver.h"
#include "loongson_debug.h"
#include "loongson_present.h"

// TODO: add a option to conf to allow
// user enable and disable this
#define LOONGSON_PRESENT_FLIP 1


#ifdef DEBUG

#define ARMSOC_PRESENT_DBG_MSG(fmt, ...)        \
            do { xf86Msg(X_INFO, fmt "\n",      \
                ##__VA_ARGS__);                 \
            } while (0)


#else

#define ARMSOC_PRESENT_DBG_MSG(fmt, ...)

#endif

extern drmEventContext event_context;


static void ms_box_intersect(BoxPtr dest, BoxPtr a, BoxPtr b)
{
    dest->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    dest->x2 = a->x2 < b->x2 ? a->x2 : b->x2;
    if (dest->x1 >= dest->x2) {
        dest->x1 = dest->x2 = dest->y1 = dest->y2 = 0;
        return;
    }

    dest->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    dest->y2 = a->y2 < b->y2 ? a->y2 : b->y2;
    if (dest->y1 >= dest->y2)
        dest->x1 = dest->x2 = dest->y1 = dest->y2 = 0;
}


static void armsoc_crtc_box(xf86CrtcPtr crtc, BoxPtr crtc_box)
{
    if (crtc->enabled)
    {
        crtc_box->x1 = crtc->x;
        crtc_box->y1 = crtc->y;

        crtc_box->x2 = crtc->x + xf86ModeWidth(&crtc->mode, crtc->rotation);
        crtc_box->y2 = crtc->y + xf86ModeHeight(&crtc->mode, crtc->rotation);
    }
    else
    {
        crtc_box->x1 = crtc_box->x2 = crtc_box->y1 = crtc_box->y2 = 0;
    }
}


static int ms_box_area(BoxPtr box)
{
    return (int)(box->x2 - box->x1) * (int)(box->y2 - box->y1);
}

Bool ms_crtc_on(xf86CrtcPtr crtc)
{
    struct drmmode_crtc_private_rec * drmmode_crtc = crtc->driver_private;
    return crtc->enabled && (drmmode_crtc->dpms_mode == DPMSModeOn);
}

/*
 * Return the crtc covering 'box'. If two crtcs cover a portion of
 * 'box', then prefer 'desired'. If 'desired' is NULL, then prefer the crtc
 * with greater coverage
 */

/////////////////////////////////////////////////////////////////////

static void ms_randr_crtc_box(RRCrtcPtr crtc, BoxPtr crtc_box)
{
    if (crtc->mode) {
        crtc_box->x1 = crtc->x;
        crtc_box->y1 = crtc->y;
        switch (crtc->rotation) {
            case RR_Rotate_0:
            case RR_Rotate_180:
            default:
                crtc_box->x2 = crtc->x + crtc->mode->mode.width;
                crtc_box->y2 = crtc->y + crtc->mode->mode.height;
                break;
            case RR_Rotate_90:
            case RR_Rotate_270:
                crtc_box->x2 = crtc->x + crtc->mode->mode.height;
                crtc_box->y2 = crtc->y + crtc->mode->mode.width;
                break;
        }
    } else
        crtc_box->x1 = crtc_box->x2 = crtc_box->y1 = crtc_box->y2 = 0;
}



/*
 * Return the first output which is connected to an active CRTC on this screen.
 *
 * RRFirstOutput() will return an output from a slave screen if it is primary,
 * which is not the behavior that ms_covering_crtc() wants.
 */

static RROutputPtr ms_first_output(ScreenPtr pScreen)
{
    rrScrPriv(pScreen);
    RROutputPtr output;
    int i, j;

    if (!pScrPriv)
        return NULL;

    if (pScrPriv->primaryOutput && pScrPriv->primaryOutput->crtc &&
        (pScrPriv->primaryOutput->pScreen == pScreen)) {
        return pScrPriv->primaryOutput;
    }

    for (i = 0; i < pScrPriv->numCrtcs; i++) {
        RRCrtcPtr crtc = pScrPriv->crtcs[i];

        for (j = 0; j < pScrPriv->numOutputs; j++) {
            output = pScrPriv->outputs[j];
            if (output->crtc == crtc)
                return output;
        }
    }
    return NULL;
}


static RRCrtcPtr ms_covering_randr_crtc(ScreenPtr pScreen,
            BoxPtr box, Bool screen_is_ms)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(pScreen);
    rrScrPrivPtr pScrPriv;
    RRCrtcPtr crtc, best_crtc;
    int coverage, best_coverage;
    int c;
    BoxRec crtc_box, cover_box;
    Bool crtc_on;

    best_crtc = NULL;
    best_coverage = 0;

    if (!dixPrivateKeyRegistered(rrPrivKey))
        return NULL;

    pScrPriv = rrGetScrPriv(pScreen);

    if (!pScrPriv)
        return NULL;

    for (c = 0; c < pScrPriv->numCrtcs; c++) {
        crtc = pScrPriv->crtcs[c];

        if (screen_is_ms) {
            crtc_on = ms_crtc_on((xf86CrtcPtr) crtc->devPrivate);
        } else {
            crtc_on = !!crtc->mode;
        }

        /* If the CRTC is off, treat it as not covering */
        if (!crtc_on)
            continue;

        ms_randr_crtc_box(crtc, &crtc_box);
        ms_box_intersect(&cover_box, &crtc_box, box);
        coverage = ms_box_area(&cover_box);
        if (coverage > best_coverage) {
            best_crtc = crtc;
            best_coverage = coverage;
        }
    }


    /* Fallback to primary crtc for drawable's on slave outputs */
    if (best_crtc == NULL && !pScreen->isGPU) {
        DEBUG_MSG("Fallback to primary crtc");
    }

    return best_crtc;
}

RRCrtcPtr ms_randr_crtc_covering_drawable(DrawablePtr pDraw)
{
    ScreenPtr pScreen = pDraw->pScreen;
    BoxRec box;

    box.x1 = pDraw->x;
    box.y1 = pDraw->y;
    box.x2 = box.x1 + pDraw->width;
    box.y2 = box.y1 + pDraw->height;

    return ms_covering_randr_crtc(pScreen, &box, TRUE);
}

/////
// Return the current CRTC for 'window'.
static RRCrtcPtr ms_present_get_crtc(WindowPtr window)
{
    return ms_randr_crtc_covering_drawable(&window->drawable);
}

static int ms_present_get_ust_msc(RRCrtcPtr crtc, CARD64 *ust, CARD64 *msc)
{
    xf86CrtcPtr xf86_crtc = crtc->devPrivate;

    return armsoc_get_crtc_ust_msc(xf86_crtc, ust, msc);
}

/*
 * Called when the queued vblank event has occurred
 */
static void armsoc_present_vblank_handler(uint64_t msc, uint64_t usec, void *data)
{
	struct armsoc_present_vblank_event *event = data;

	ARMSOC_PRESENT_DBG_MSG("present_vblank_handler event_id:%llu msc:%llu",
	                       (long long) event->event_id, (long long) msc);

	present_event_notify(event->event_id, usec, msc);
	free(event);
}


/*
 * Called when the queued vblank is aborted
 */
static void armsoc_present_vblank_abort(void *data)
{
    struct armsoc_present_vblank_event *event = data;
    ARMSOC_PRESENT_DBG_MSG("present_vblank_abort: \t\tma %lld\n",
        (long long) event->event_id);

    free(event);
}

/*
 * Queue an event to report back to the Present extension when the specified
 * MSC has past
 */
static int ms_present_queue_vblank(RRCrtcPtr crtc, uint64_t event_id, uint64_t msc)
{
    xf86CrtcPtr xf86_crtc = crtc->devPrivate;
    struct armsoc_present_vblank_event *event;
    uint32_t seq;


    ARMSOC_PRESENT_DBG_MSG("queue_vblank: event_id:%llu msc:%llu",
            event_id, msc);

    event = calloc(sizeof(struct armsoc_present_vblank_event), 1);
    if (!event)
        return BadAlloc;
    event->event_id = event_id;
    seq = armsoc_drm_queue_alloc(xf86_crtc, event,
            armsoc_present_vblank_handler,
            armsoc_present_vblank_abort);
    if (0 == seq)
    {
        free(event);
        return BadAlloc;
    }

    if (!ls_queue_vblank(xf86_crtc, MS_QUEUE_ABSOLUTE , msc, NULL, seq, event))
        return BadAlloc;

    ARMSOC_PRESENT_DBG_MSG("queue_vblank: event_id:%llu seq:%u msc:%llu",
            (long long) event_id, seq, (long long) msc );
    return Success;
}

static Bool
armsoc_present_event_match(void *data, void *match_data)
{
	struct armsoc_present_vblank_event *event = data;
	uint64_t *match = match_data;
	ARMSOC_PRESENT_DBG_MSG("armsoc_present_event_match");

	return *match == event->event_id;
}

/*
 * Remove a pending vblank event from the DRM queue so that it is not reported
 * to the extension
 */
static void ms_present_abort_vblank(RRCrtcPtr crtc, uint64_t event_id, uint64_t msc)
{
    ScreenPtr screen = crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);

    armsoc_drm_abort_event(scrn, event_id);
}



#if LOONGSON_PRESENT_FLIP
/*
 * Flush our batch buffer when requested by the Present extension.
 * Flush pending drawing on 'window' to the hardware.
 */
static void ms_present_flush(WindowPtr window)
{
	ScreenPtr screen = window->drawable.pScreen;
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	struct ARMSOCRec * pLS = loongsonPTR(scrn);

	ARMSOC_PRESENT_DBG_MSG("ms_present_flush");
}


/**
 * Callback for the DRM event queue when a flip has completed on all pipes
 *
 * Notify the extension code
 */
static void armsoc_present_flip_handler(struct ARMSOCRec * pARMSOC, uint64_t msc,
        uint64_t ust, void *data)
{
    struct armsoc_present_vblank_event *event = data;

    ARMSOC_PRESENT_DBG_MSG("present_flip_handler event_id:%llu msc:%llu ust:%llu\n",
            (long long) event->event_id,  (long long) msc, (long long) ust);

    //    if (event->unflip)
    //      ms->drmmode.present_flipping = FALSE;

    armsoc_present_vblank_handler(msc, ust, event);
}

/*
 * Callback for the DRM queue abort code.  A flip has been aborted.
 */
static void armsoc_present_flip_abort(struct ARMSOCRec * pARMSOC, void *data)
{
	struct armsoc_present_vblank_event *event = data;

	ARMSOC_PRESENT_DBG_MSG("armsoc_present_flip_abort ms:fa %lld\n",
         (long long) event->event_id);

	free(event);
}


/*
 * Check if 'pixmap' is suitable for flipping to 'window'.
 * can return a 'reason' why the flip would fail.
 *
 */
static Bool ms_present_check_flip(RRCrtcPtr crtc,
        WindowPtr window, PixmapPtr pixmap, Bool sync_flip,
        PresentFlipReason *reason)
{
    ScreenPtr screen = window->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct ARMSOCRec * pARMSOC = ARMSOCPTR(scrn);


    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    int num_crtcs_on = 0;
    int i;

    ARMSOC_PRESENT_DBG_MSG("ms_present_check_flip");

    if (!scrn->vtSema)
        return FALSE;

    for (i = 0; i < config->num_crtc; i++)
    {
        struct drmmode_crtc_private_rec * drmmode_crtc = config->crtc[i]->driver_private;

        if (ms_crtc_on(config->crtc[i]))
            num_crtcs_on++;
    }

    /* We can't do pageflipping if all the CRTCs are off. */
    if (num_crtcs_on == 0)
        return FALSE;

    /* Check stride, can't change that on flip */
/*
    if (pixmap->devKind != armsoc_bo_pitch(&pARMSOC->drmmode.front_bo))
    {
        DEBUG_MSG("pixmap->devKind=%d", pixmap->devKind );
        DEBUG_MSG("pixmap->devKind=%d", pixmap->devKind );

        // return FALSE;
    }
*/

    DEBUG_MSG("pixmap->devKind=%d", pixmap->devKind );

    /* Make sure there's a bo we can get to */

    return TRUE;
}

/*
 * Queue a flip on 'crtc' to 'pixmap' at 'target_msc'. If 'sync_flip' is true,
 * then wait for vblank. Otherwise, flip immediately
 *
 * Flip pixmap, return false if it didn't happen.
 *
 * 'crtc' is to be used for any necessary synchronization.
 *
 * 'sync_flip' requests that the flip be performed at the next
 *     vertical blank interval to avoid tearing artifacts.
 *     If false, the flip should be performed as soon as possible.
 *
 * present_event_notify should be called with 'event_id' when the flip
 * occurs
 */
static Bool ms_present_flip(RRCrtcPtr crtc,
                            uint64_t event_id,
                            uint64_t target_msc,
                            PixmapPtr pixmap,
                            Bool sync_flip)
{
    ScreenPtr screen = crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct ARMSOCRec * pLS = loongsonPTR(scrn);
    xf86CrtcPtr xf86_crtc = crtc->devPrivate;
    struct drmmode_crtc_private_rec * drmmode_crtc = xf86_crtc->driver_private;
    Bool ret;
    struct armsoc_present_vblank_event *event;

    ARMSOC_PRESENT_DBG_MSG("present_flip");

    if (!ms_present_check_flip(crtc, screen->root, pixmap, sync_flip, NULL))
        return FALSE;

    event = calloc(1, sizeof(struct armsoc_present_vblank_event));
    if (!event)
        return FALSE;

    ARMSOC_PRESENT_DBG_MSG("present_flip ms:pf %lld msc %llu\n",
            (long long) event_id, (long long) target_msc);

    event->event_id = event_id;
    event->unflip = FALSE;

    ret = drmmode_page_flip(screen, &pixmap->drawable,
        drmmode_crtc->drmmode->fb_id, sync_flip, NULL);

    if (!ret)
        xf86DrvMsg(scrn->scrnIndex, X_ERROR, "present flip failed\n");
    else
        pLS->drmmode.present_flipping = TRUE;

    return ret;
}

/*
 * Queue a flip back to the normal frame buffer
 * "unflip" back to the regular screen scanout buffer
 */
static void ms_present_unflip(ScreenPtr screen, uint64_t event_id)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	struct ARMSOCRec * pLs = loongsonPTR(scrn);
	PixmapPtr pixmap = screen->GetScreenPixmap(screen);
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
	int i;
	struct armsoc_present_vblank_event *event;


    ARMSOC_PRESENT_DBG_MSG("present_unflip");

	event = calloc(1, sizeof(struct armsoc_present_vblank_event));
	if (!event)
		return;

	event->event_id = event_id;
	event->unflip = TRUE;

/*
	if (ms_present_check_flip(NULL, screen->root, pixmap, TRUE) &&
	        drmmode_page_flip(screen, pixmap, event)
	   ) {
		return;
	}
*/
	for (i = 0; i < config->num_crtc; ++i)
	{
		xf86CrtcPtr crtc = config->crtc[i];
		struct drmmode_crtc_private_rec * drmmode_crtc = crtc->driver_private;

		if (!crtc->enabled)
			continue;

		/* info->drmmode.fb_id still points to the FB for the last flipped BO.
		 * Clear it, drmmode_set_mode_major will re-create it
		 */
		if (drmmode_crtc->drmmode->fb_id) {
			drmModeRmFB(drmmode_crtc->drmmode->fd,
			            drmmode_crtc->drmmode->fb_id);
			drmmode_crtc->drmmode->fb_id = 0;
		}

		if (drmmode_crtc->dpms_mode == DPMSModeOn)
			crtc->funcs->set_mode_major(crtc, &crtc->mode, crtc->rotation,
			                            crtc->x, crtc->y);
		else
			drmmode_crtc->need_modeset = TRUE;
	}

	present_event_notify(event_id, 0, 0);
	pLs->drmmode.present_flipping = FALSE;
}
#endif

static present_screen_info_rec loongson_present_screen_info = {
    .version = PRESENT_SCREEN_INFO_VERSION,

    .get_crtc = ms_present_get_crtc,
    .get_ust_msc = ms_present_get_ust_msc,
    .queue_vblank = ms_present_queue_vblank,
    .abort_vblank = ms_present_abort_vblank,
    .flush = ms_present_flush,
    .capabilities = PresentCapabilityNone,
#if LOONGSON_PRESENT_FLIP
    .check_flip = NULL,
    .check_flip2 = ms_present_check_flip,
    .flip = ms_present_flip,
    .unflip = ms_present_unflip,
#endif
};



Bool LS_PresentScreenInit(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct ARMSOCRec * pLs = loongsonPTR(scrn);
    uint64_t value;
    int ret;

    ls_vblank_screen_init(screen);

    ret = drmGetCap(pLs->drmFD, DRM_CAP_ASYNC_PAGE_FLIP, &value);
    if (ret == 0 && value == 1)
    {
        xf86Msg(X_INFO, "PresentCapabilityAsync supported.\n");
        loongson_present_screen_info.capabilities |= PresentCapabilityAsync;
    }

    xf86Msg(X_INFO, "Present screen initialized.\n");

    return present_screen_init(screen, &loongson_present_screen_info);
}

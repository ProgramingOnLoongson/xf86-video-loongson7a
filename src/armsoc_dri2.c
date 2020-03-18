/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright © 2011 Texas Instruments, Inc
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
 *    Rob Clark <rob@ti.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "armsoc_driver.h"
#include "armsoc_exa.h"

#include "dri2.h"

/* any point to support earlier? */
#if DRI2INFOREC_VERSION < 4
#	error "Requires newer DRI2"
#endif

#include "drmmode_driver.h"

struct ARMSOCDRI2BufferRec {
	DRI2BufferRec base;

	/**
	 * Pixmap(s) that are backing the buffer
	 *
	 * NOTE: don't track the pixmap ptr for the front buffer if it is
	 * a window.. this could get reallocated from beneath us, so we should
	 * always use draw2pix to be sure to have the correct one
	 */
	PixmapPtr *pPixmaps;

	/**
	 * Hold an explicit reference to the bo. You would hope that having
	 * the pixmap pointer above would allow us to do this, but things get
	 * complex when working with scanout. For that, we use the main drawable
	 * itself, and swap it's backing pixmap as appropriate. */
	struct armsoc_bo *bo;

	/**
	 * Pixmap that corresponds to the DRI2BufferRec.name, so wraps
	 * the buffer that will be used for DRI2GetBuffers calls and the
	 * next DRI2SwapBuffers call.
	 *
	 * When using more than double buffering this (and the name) are updated
	 * after a swap, before the next DRI2GetBuffers call.
	 */
	unsigned currentPixmap;

	/**
	 * Number of Pixmaps to use.
	 *
	 * This allows the number of back buffers used to be reduced, for
	 * example when allocation fails. It cannot be changed to increase the
	 * number of buffers as we would overflow the pPixmaps array.
	 */
	unsigned numPixmaps;

	/**
	 * The DRI2 buffers are reference counted to avoid crashyness when the
	 * client detaches a dri2 drawable while we are still waiting for a
	 * page_flip event.
	 */
	int refcnt;

	/**
         * We don't want to overdo attempting fb allocation for mapped
         * scanout buffers, to behave nice under low memory conditions.
         * Instead we use this flag to attempt the allocation just once
         * every time the window is mapped.
         */
	int attempted_fb_alloc;

};

#define ARMSOCBUF(p)	((struct ARMSOCDRI2BufferRec *)(p))
#define DRIBUF(p)	((DRI2BufferPtr)(&(p)->base))


static inline DrawablePtr
dri2draw(DrawablePtr pDraw, DRI2BufferPtr buf)
{
	if (buf->attachment == DRI2BufferFrontLeft)
		return pDraw;
	else {
		const unsigned curPix = ARMSOCBUF(buf)->currentPixmap;
		return &(ARMSOCBUF(buf)->pPixmaps[curPix]->drawable);
	}
}

static Bool
canflip(DrawablePtr pDraw)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);

	if (pARMSOC->NoFlip) {
		/* flipping is disabled by user option */
		return FALSE;
	} else {
		return (pDraw->type == DRAWABLE_WINDOW) &&
				DRI2CanFlip(pDraw);
	}
}

static inline Bool
exchangebufs(DrawablePtr pDraw, DRI2BufferPtr a, DRI2BufferPtr b)
{
	PixmapPtr aPix = draw2pix(dri2draw(pDraw, a));
	PixmapPtr bPix = draw2pix(dri2draw(pDraw, b));

	ARMSOCPixmapExchange(aPix, bPix);
	exchange(a->name, b->name);
	return TRUE;
}

static PixmapPtr
createpix(DrawablePtr pDraw)
{
	ScreenPtr pScreen = pDraw->pScreen;
	int flags = canflip(pDraw) ? ARMSOC_CREATE_PIXMAP_SCANOUT : CREATE_PIXMAP_USAGE_BACKING_PIXMAP;
	return pScreen->CreatePixmap(pScreen,
			pDraw->width, pDraw->height, pDraw->depth, flags);
}

static inline Bool
canexchange(DrawablePtr pDraw, struct armsoc_bo *src_bo, struct armsoc_bo *dst_bo)
{
	Bool ret = FALSE;
	ScreenPtr pScreen = pDraw->pScreen;
	PixmapPtr pRootPixmap, pWindowPixmap;
	int src_fb_id, dst_fb_id;

	pRootPixmap = pScreen->GetWindowPixmap(pScreen->root);
	pWindowPixmap = pDraw->type == DRAWABLE_PIXMAP ? (PixmapPtr)pDraw : pScreen->GetWindowPixmap((WindowPtr)pDraw);

	src_fb_id = armsoc_bo_get_fb(src_bo);
	dst_fb_id = armsoc_bo_get_fb(dst_bo);

	if (pRootPixmap != pWindowPixmap &&
	    armsoc_bo_width(src_bo) == armsoc_bo_width(dst_bo) &&
	    armsoc_bo_height(src_bo) == armsoc_bo_height(dst_bo) &&
	    armsoc_bo_bpp(src_bo) == armsoc_bo_bpp(dst_bo) &&
	    armsoc_bo_width(src_bo) == pDraw->width &&
	    armsoc_bo_height(src_bo) == pDraw->height &&
	    armsoc_bo_bpp(src_bo) == pDraw->bitsPerPixel &&
	    src_fb_id == 0 && dst_fb_id == 0) {
		ret = TRUE;
	}

	/*
	 * Don't exchange for windows which do not own their complete backing storage,
	 * e.g. are not redirected for composition, are child windows of window manager
	 * frame/decoration windows, or have child windows of their own.
	 *
	 * A special case is made for shaped windows which are not occluded by children
	 * or siblings (clipList == borderClip), do not share storage with a parent who could
	 * render into it (while pWin->parent), and for whom the extents of the clip region
	 * cover the entire pixmap (non-degenerate shaped windows).
	 *
	 * In these cases, fall back to CopyArea which respects the clip.
	 */
	if (pDraw->type == DRAWABLE_WINDOW) {
		ScreenPtr pScreen = pDraw->pScreen;
		WindowPtr pWin = (WindowPtr) pDraw;
		WindowPtr pWinParent = pWin->parent;
		PixmapPtr pPixmap = pScreen->GetWindowPixmap(pWin);
		BoxPtr extents = RegionExtents(&pWin->clipList);

		if (RegionNumRects(&pWin->clipList) != 1) {
			if (!RegionEqual(&pWin->clipList, &pWin->borderClip))
				return FALSE;

			while (pWinParent) {
				PixmapPtr pPixmapParent = pScreen->GetWindowPixmap(pWinParent);

				if (pPixmapParent != pPixmap)
					break;
				if (RegionNotEmpty(&pWinParent->clipList))
					return FALSE;

				pWinParent = pWinParent->parent;
			}
		}

		if (extents->x1 != pPixmap->screen_x ||
		    extents->y1 != pPixmap->screen_y ||
		    extents->x2 != pDraw->width + pPixmap->screen_x ||
		    extents->y2 != pDraw->height + pPixmap->screen_y)
			return FALSE;
	}

	return ret;
}

static Bool CreateBufferResources(DrawablePtr pDraw, DRI2BufferPtr buffer)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCDRI2BufferRec *buf = ARMSOCBUF(buffer);
	PixmapPtr pPixmap = NULL;
	struct armsoc_bo *bo;
	int ret;

	if (buffer->attachment == DRI2BufferFrontLeft) {
		pPixmap = draw2pix(pDraw);
		pPixmap->refcnt++;
	} else {
		pPixmap = createpix(pDraw);
	}

	if (!pPixmap) {
		assert(buffer->attachment != DRI2BufferFrontLeft);
		ERROR_MSG("Failed to create back buffer for window");
		return FALSE;
	}

	buf->pPixmaps[0] = pPixmap;
	assert(buf->currentPixmap == 0);

	bo = ARMSOCPixmapBo(pPixmap);
	if (!bo) {
		ERROR_MSG(
			"Attempting to DRI2 wrap a pixmap with no DRM buffer object backing");
		goto fail;
	}

	DRIBUF(buf)->pitch = exaGetPixmapPitch(pPixmap);
	DRIBUF(buf)->cpp = pPixmap->drawable.bitsPerPixel / 8;
	DRIBUF(buf)->flags = 0;

	ret = armsoc_bo_get_name(bo, &DRIBUF(buf)->name);
	if (ret) {
		ERROR_MSG("could not get buffer name: %d", ret);
		goto fail;
	}

	if (canflip(pDraw) && buffer->attachment != DRI2BufferFrontLeft) {
		/* Create an fb around this buffer. This will fail and we will
		 * fall back to blitting if the display controller hardware
		 * cannot scan out this buffer (for example, if it doesn't
		 * support the format or there was insufficient scanout memory
		 * at buffer creation time).
		 *
		 * If the window is not mapped at this time, we will not hit
		 * this codepath, but ARMSOCDRI2ReuseBufferNotify will create
		 * a framebuffer if it gets mapped later on. */
		int ret = armsoc_bo_add_fb(bo);
	        buf->attempted_fb_alloc = TRUE;
		if (ret) {
			WARNING_MSG("Falling back to blitting a flippable window");
		}
	}

	/* Register Pixmap as having a buffer that can be accessed externally,
	 * so needs synchronised access */
	ARMSOCRegisterExternalAccess(pPixmap);

	/* At this point we would expect the texture to be used by the GPU.
	 * However there is no need to make the corresponding call into UMP,
	 * because libMali will do that before using it. */

	/* Take a direct reference from DRI2Buffer to the corresponding bo. It
	 * is not enough to do this through the pixmap, because for the scanout
	 * pixmap, we can change it's backing bo to something else. */
	buf->bo = bo;
	armsoc_bo_reference(bo);
	return TRUE;

fail:
	if (buffer->attachment != DRI2BufferFrontLeft)
		pScreen->DestroyPixmap(pPixmap);
	else
		pPixmap->refcnt--;
	return FALSE;
}

static void DestroyBufferResources(DrawablePtr pDraw, DRI2BufferPtr buffer)
{
	struct ARMSOCDRI2BufferRec *buf = ARMSOCBUF(buffer);
	/* Note: pDraw may already be deleted, so use the pPixmap here
	 * instead (since it is at least refcntd)
	 */
	ScreenPtr pScreen = buf->pPixmaps[0]->drawable.pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
	int numBuffers, i;

	if (buffer->attachment == DRI2BufferBackLeft) {
		assert(pARMSOC->driNumBufs > 1);
		numBuffers = pARMSOC->driNumBufs-1;
	} else
		numBuffers = 1;

	for (i = 0; i < numBuffers && buf->pPixmaps[i] != NULL; ++i) {
		ARMSOCDeregisterExternalAccess(buf->pPixmaps[i]);
		pScreen->DestroyPixmap(buf->pPixmaps[i]);
	}

	armsoc_bo_unreference(buf->bo);
}

/**
 * Create Buffer.
 *
 * Note that 'format' is used from the client side to specify the DRI buffer
 * format, which could differ from the drawable format.  For example, the
 * drawable could be 32b RGB, but the DRI buffer some YUV format (video) or
 * perhaps lower bit depth RGB (GL).  The color conversion is handled when
 * blitting to front buffer, and page-flipping (overlay or flipchain) can
 * only be used if the display supports.
 */
static DRI2BufferPtr
ARMSOCDRI2CreateBuffer(DrawablePtr pDraw, unsigned int attachment,
		unsigned int format)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCDRI2BufferRec *buf = calloc(1, sizeof(*buf));
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);

	DEBUG_MSG("pDraw=%p, attachment=%d, format=%08x",
			pDraw, attachment, format);

	if (!buf) {
		ERROR_MSG("Couldn't allocate internal buffer structure");
		return NULL;
	}

	if (attachment == DRI2BufferBackLeft && pARMSOC->driNumBufs > 2) {
		buf->pPixmaps = calloc(pARMSOC->driNumBufs-1, sizeof(PixmapPtr));
		buf->numPixmaps = pARMSOC->driNumBufs-1;
	} else {
		buf->pPixmaps = malloc(sizeof(PixmapPtr));
		buf->numPixmaps = 1;
	}

	if (!buf->pPixmaps) {
		ERROR_MSG("Failed to allocate PixmapPtr array for DRI2Buffer");
		goto fail;
	}

	DRIBUF(buf)->attachment = attachment;
	DRIBUF(buf)->format = format;
	buf->refcnt = 1;
	if (!CreateBufferResources(pDraw, DRIBUF(buf)))
		goto fail;

	return DRIBUF(buf);

fail:
	free(buf->pPixmaps);
	free(buf);

	return NULL;
}

/* Called when DRI2 is handling a GetBuffers request and is going to
 * reuse a buffer that we created earlier.
 * Our interest in this situation is that we might have omitted creating
 * a framebuffer for a backbuffer due to it not being flippable at creation
 * time (e.g. because the window wasn't mapped yet).
 * But if GetBuffers has been called because the window is now mapped,
 * we are going to need a framebuffer so that we can page flip it later.
 * We avoid creating a framebuffer when it is not necessary in order to save
 * on scanout memory which is potentially scarce.
 *
 * Mali r4p0 is generally light on calling GetBuffers (e.g. it doesn't do it
 * in response to an InvalidateBuffers event) but we have determined
 * experimentally that it does always seem to call GetBuffers upon a
 * unmapped-to-mapped transition.
 */
static void
ARMSOCDRI2ReuseBufferNotify(DrawablePtr pDraw, DRI2BufferPtr buffer)
{
	struct ARMSOCDRI2BufferRec *buf = ARMSOCBUF(buffer);
	struct armsoc_bo *bo;
	Bool flippable;
	int fb_id;

	if (buffer->attachment == DRI2BufferFrontLeft)
		return;

	bo = ARMSOCPixmapBo(buf->pPixmaps[0]);
	fb_id = armsoc_bo_get_fb(bo);
	flippable = canflip(pDraw);

	/* Detect unflippable-to-flippable transition:
	 * Window is flippable, but we haven't yet tried to allocate a
	 * framebuffer for it, and it doesn't already have a framebuffer.
         * Also it was allocated as a pixmap, but now we need it as scanout.
	 * This can happen when CreateBuffer was called before the window
	 * was mapped, and we have now been mapped. */
	if (flippable && !buf->attempted_fb_alloc && fb_id == 0) {
		DestroyBufferResources(pDraw, buffer);
		CreateBufferResources(pDraw, buffer);
	}

	/* Detect flippable-to-unflippable transition:
	 * Window is now unflippable, but we have a framebuffer allocated for
	 * it, and it is using scanout memory. We can now free the framebuffer
	 * and switch it back to a backing pixmap allocation to save on
	 * scanout memory. */
	if (!flippable && fb_id != 0) {
	        buf->attempted_fb_alloc = FALSE;
		armsoc_bo_rm_fb(bo);
		DestroyBufferResources(pDraw, buffer);
		CreateBufferResources(pDraw, buffer);
	}
}

/**
 * Destroy Buffer
 */
static void
ARMSOCDRI2DestroyBuffer(DrawablePtr pDraw, DRI2BufferPtr buffer)
{
	struct ARMSOCDRI2BufferRec *buf = ARMSOCBUF(buffer);
	/* Note: pDraw may already be deleted, so use the pPixmap here
	 * instead (since it is at least refcntd)
	 */
	ScreenPtr pScreen = buf->pPixmaps[0]->drawable.pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);

	if (--buf->refcnt > 0)
		return;

	DEBUG_MSG("pDraw=%p, buffer=%p", pDraw, buffer);

	DestroyBufferResources(pDraw, buffer);
	free(buf->pPixmaps);
	free(buf);
}

static void
ARMSOCDRI2ReferenceBuffer(DRI2BufferPtr buffer)
{
	struct ARMSOCDRI2BufferRec *buf = ARMSOCBUF(buffer);
	buf->refcnt++;
}

/**
 *
 */
static void
ARMSOCDRI2CopyRegion(DrawablePtr pDraw, RegionPtr pRegion,
		DRI2BufferPtr pDstBuffer, DRI2BufferPtr pSrcBuffer)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	DrawablePtr pSrcDraw = dri2draw(pDraw, pSrcBuffer);
	DrawablePtr pDstDraw = dri2draw(pDraw, pDstBuffer);
	RegionPtr pCopyClip;
	GCPtr pGC;

	DEBUG_MSG("pDraw=%p, pDstBuffer=%p (%p), pSrcBuffer=%p (%p)",
			pDraw, pDstBuffer, pSrcDraw, pSrcBuffer, pDstDraw);
	pGC = GetScratchGC(pDstDraw->depth, pScreen);
	if (!pGC)
		return;

	/* No need to worry about UMP/caching here, this will trigger
	 * PrepareAccess and FinishAccess which do the right thing. */

	pCopyClip = REGION_CREATE(pScreen, NULL, 0);
	RegionCopy(pCopyClip, pRegion);
	(*pGC->funcs->ChangeClip) (pGC, CT_REGION, pCopyClip, 0);
	ValidateGC(pDstDraw, pGC);

	/* If the dst is the framebuffer, and we had a way to
	 * schedule a deferred blit synchronized w/ vsync, that
	 * would be a nice thing to do utilize here to avoid
	 * tearing..  when we have sync object support for GEM
	 * buffers, I think we could do something more clever
	 * here.
	 */

	pGC->ops->CopyArea(pSrcDraw, pDstDraw, pGC,
			0, 0, pDraw->width, pDraw->height, 0, 0);

	FreeScratchGC(pGC);
}

/**
 * Get current frame count and frame count timestamp, based on drawable's
 * crtc.
 */
static int
ARMSOCDRI2GetMSC(DrawablePtr pDraw, CARD64 *ust, CARD64 *msc)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
	drmVBlank vbl = { .request = {
		.type = DRM_VBLANK_RELATIVE,
		.sequence = 0,
	} };
	int ret;

	if (!pARMSOC->drmmode_interface->vblank_query_supported)
		return FALSE;

	ret = drmWaitVBlank(pARMSOC->drmFD, &vbl);
	if (ret) {
		ERROR_MSG("get vblank counter failed: %s", strerror(errno));
		return FALSE;
	}

	if (ust)
		*ust = ((CARD64)vbl.reply.tval_sec * 1000000)
			+ vbl.reply.tval_usec;

	if (msc)
		*msc = vbl.reply.sequence;

	return TRUE;
}

#define ARMSOC_SWAP_FAKE_FLIP (1 << 0)
#define ARMSOC_SWAP_FAIL      (1 << 1)

struct ARMSOCDRISwapCmd {
	int type;
	ClientPtr client;
	ScreenPtr pScreen;
	/* Note: store drawable ID, rather than drawable.  It's possible that
	 * the drawable can be destroyed while we wait for page flip event:
	 */
	XID draw_id;
	DRI2BufferPtr pDstBuffer;
	DRI2BufferPtr pSrcBuffer;
	DRI2SwapEventPtr func;
	int swapCount;
	int flags;
	void *data;

	struct armsoc_bo *old_src_bo;
	struct armsoc_bo *old_dst_bo;
};

static const char * const swap_names[] = {
		[DRI2_EXCHANGE_COMPLETE] = "exchange",
		[DRI2_BLIT_COMPLETE] = "blit",
		[DRI2_FLIP_COMPLETE] = "flip,"
};

static Bool allocNextBuffer(DrawablePtr pDraw, PixmapPtr *ppPixmap,
		uint32_t *name) {
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct armsoc_bo *bo;
	PixmapPtr pPixmap;
	int ret;
	uint32_t new_name;
	Bool extRegistered = FALSE;

	pPixmap = createpix(pDraw);

	if (!pPixmap)
		goto error;

	bo = ARMSOCPixmapBo(pPixmap);
	if (!bo) {
		WARNING_MSG(
			"Attempting to DRI2 wrap a pixmap with no DRM buffer object backing");
		goto error;
	}

	ARMSOCRegisterExternalAccess(pPixmap);
	extRegistered = TRUE;

	ret = armsoc_bo_get_name(bo, &new_name);
	if (ret) {
		ERROR_MSG("Could not get buffer name: %d", ret);
		goto error;
	}

	if (!armsoc_bo_get_fb(bo)) {
		ret = armsoc_bo_add_fb(bo);
		/* Should always be able to add fb, as we only add more buffers
		 * when flipping*/
		if (ret) {
			ERROR_MSG(
				"Could not add framebuffer to additional back buffer");
			goto error;
		}
	}

	/* No errors, update pixmap and name */
	*ppPixmap = pPixmap;
	*name = new_name;

	return TRUE;

error:
	/* revert to existing pixmap */
	if (pPixmap) {
		if (extRegistered)
			ARMSOCDeregisterExternalAccess(pPixmap);
		pScreen->DestroyPixmap(pPixmap);
	}

	return FALSE;
}

static void nextBuffer(DrawablePtr pDraw, struct ARMSOCDRI2BufferRec *backBuf)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);

	if (pARMSOC->driNumBufs <= 2) {
		/*Only using double buffering, leave the pixmap as-is */
		return;
	}

	backBuf->currentPixmap++;
	backBuf->currentPixmap %= backBuf->numPixmaps;

	if (backBuf->pPixmaps[backBuf->currentPixmap]) {
		/* Already allocated the next buffer - get the name and
		 * early-out */
		struct armsoc_bo *bo;
		int ret;

		bo = ARMSOCPixmapBo(backBuf->pPixmaps[backBuf->currentPixmap]);
		assert(bo);
		ret = armsoc_bo_get_name(bo, &DRIBUF(backBuf)->name);
		assert(!ret);
	} else {
		Bool ret;
		PixmapPtr * const curBackPix =
			&backBuf->pPixmaps[backBuf->currentPixmap];
		ret = allocNextBuffer(pDraw, curBackPix,
			&DRIBUF(backBuf)->name);
		if (!ret) {
			/* can't have failed on the first buffer */
			assert(backBuf->currentPixmap > 0);
			/* Fall back to last buffer */
			backBuf->currentPixmap--;
			WARNING_MSG(
				"Failed to use the requested %d-buffering due to an allocation failure.\n"
				"Falling back to %d-buffering for this DRI2Drawable",
				backBuf->numPixmaps+1,
				backBuf->currentPixmap+2);
			backBuf->numPixmaps = backBuf->currentPixmap+1;
		}
	}
}

static struct armsoc_bo *boFromBuffer(DRI2BufferPtr buf)
{
	PixmapPtr pPixmap;
	struct ARMSOCPixmapPrivRec *priv;

	pPixmap = ARMSOCBUF(buf)->pPixmaps[ARMSOCBUF(buf)->currentPixmap];
	priv = exaGetPixmapDriverPrivate(pPixmap);
	return priv->bo;
}

static
void updateResizedBuffer(ScrnInfoPtr pScrn, void *buffer,
		struct armsoc_bo *old_bo, struct armsoc_bo *resized_bo) {
	DRI2BufferPtr dri2buf = (DRI2BufferPtr)buffer;
	struct ARMSOCDRI2BufferRec *buf = ARMSOCBUF(dri2buf);
	int i;

	for (i = 0; i < buf->numPixmaps; i++) {
		if (buf->pPixmaps[i] != NULL) {
			struct ARMSOCPixmapPrivRec *priv = exaGetPixmapDriverPrivate(buf->pPixmaps[i]);

			if (old_bo == priv->bo) {
				int ret;

				/* Update the buffer name if this pixmap is current */
				if (i == buf->currentPixmap) {
					ret = armsoc_bo_get_name(resized_bo, &dri2buf->name);
					assert(!ret);
				}

				/* pixmap takes ref on resized bo */
				armsoc_bo_reference(resized_bo);
				/* replace the old_bo with the resized_bo */
				priv->bo = resized_bo;
				/* pixmap drops ref on old bo */
				armsoc_bo_unreference(old_bo);
			}
		}
	}
}

void
ARMSOCDRI2ResizeSwapChain(ScrnInfoPtr pScrn, struct armsoc_bo *old_bo,
		struct armsoc_bo *resized_bo)
{
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
	struct ARMSOCDRISwapCmd *cmd = NULL;
	int i;
	int back = pARMSOC->swap_chain_count - 1; /* The last swap scheduled */

	/* Update the bos for each scheduled swap in the swap chain */
	for (i = 0; i < pARMSOC->swap_chain_size && back >= 0; i++) {
		unsigned int idx = back % pARMSOC->swap_chain_size;

		cmd = pARMSOC->swap_chain[idx];
		back--;
		if (!cmd)
			continue;
		updateResizedBuffer(pScrn, cmd->pSrcBuffer, old_bo, resized_bo);
		updateResizedBuffer(pScrn, cmd->pDstBuffer, old_bo, resized_bo);
	}
}

void
ARMSOCDRI2SwapComplete(struct ARMSOCDRISwapCmd *cmd)
{
	ScreenPtr pScreen = cmd->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
	DrawablePtr pDraw = NULL;
	int status;

	if (--cmd->swapCount > 0)
		return;

	if ((cmd->flags & ARMSOC_SWAP_FAIL) == 0) {
		DEBUG_MSG("%s complete: %d -> %d", swap_names[cmd->type],
			cmd->pSrcBuffer->attachment,
			cmd->pDstBuffer->attachment);

		status = dixLookupDrawable(&pDraw, cmd->draw_id, serverClient,
				M_ANY, DixWriteAccess);

		if (status == Success) {
			if (cmd->type != DRI2_BLIT_COMPLETE &&
			    cmd->type != DRI2_EXCHANGE_COMPLETE &&
			   (cmd->flags & ARMSOC_SWAP_FAKE_FLIP) == 0) {
				assert(cmd->type == DRI2_FLIP_COMPLETE);
				exchangebufs(pDraw, cmd->pSrcBuffer,
							cmd->pDstBuffer);

				if (cmd->pSrcBuffer->attachment ==
						DRI2BufferBackLeft)
					nextBuffer(pDraw,
						ARMSOCBUF(cmd->pSrcBuffer));
			}

			DRI2SwapComplete(cmd->client, pDraw, 0, 0, 0, cmd->type,
					cmd->func, cmd->data);

			if (cmd->type != DRI2_BLIT_COMPLETE &&
			    cmd->type != DRI2_EXCHANGE_COMPLETE &&
			   (cmd->flags & ARMSOC_SWAP_FAKE_FLIP) == 0) {
				assert(cmd->type == DRI2_FLIP_COMPLETE);
				set_scanout_bo(pScrn,
					boFromBuffer(cmd->pDstBuffer));
			}
		}
	}

	/* drop extra refcnt we obtained prior to swap:
	 */
	ARMSOCDRI2DestroyBuffer(pDraw, cmd->pSrcBuffer);
	ARMSOCDRI2DestroyBuffer(pDraw, cmd->pDstBuffer);
	armsoc_bo_unreference(cmd->old_src_bo);
	armsoc_bo_unreference(cmd->old_dst_bo);
	pARMSOC->pending_flips--;

	free(cmd);
}

static void ARMSOCDRI2ExecuteSwap(struct ARMSOCDRISwapCmd *cmd)
{
	int status;
	DrawablePtr pDraw = NULL;

	status = dixLookupDrawable(&pDraw, cmd->draw_id, serverClient,
				   M_ANY, DixWriteAccess);
	if (status != Success) {
		ARMSOCDRI2SwapComplete(cmd);
		return;
	}

	if (canexchange(pDraw, cmd->old_src_bo, cmd->old_dst_bo)) {
		PixmapPtr pDstPixmap = draw2pix(dri2draw(pDraw, cmd->pDstBuffer));
		RegionRec region;

		exchangebufs(pDraw, cmd->pSrcBuffer, cmd->pDstBuffer);
		if (cmd->pSrcBuffer->attachment == DRI2BufferBackLeft)
			nextBuffer(pDraw, ARMSOCBUF(cmd->pSrcBuffer));

		region.extents.x1 = region.extents.y1 = 0;
		region.extents.x2 = pDstPixmap->drawable.width;
		region.extents.y2 = pDstPixmap->drawable.height;
		region.data = NULL;
		DamageRegionAppend(&pDstPixmap->drawable, &region);
		DamageRegionProcessPending(&pDstPixmap->drawable);

		cmd->type = DRI2_EXCHANGE_COMPLETE;
		ARMSOCDRI2SwapComplete(cmd);
	} else {
		/* fallback to blit: */
		BoxRec box = {
				.x1 = 0,
				.y1 = 0,
				.x2 = pDraw->width,
				.y2 = pDraw->height,
		};
		RegionRec region;
		RegionInit(&region, &box, 0);
		ARMSOCDRI2CopyRegion(pDraw, &region, cmd->pDstBuffer, cmd->pSrcBuffer);
		cmd->type = DRI2_BLIT_COMPLETE;
		ARMSOCDRI2SwapComplete(cmd);
	}
}

void ARMSOCDRI2VBlankHandler(unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec, void *user_data)
{
	struct ARMSOCDRISwapCmd *cmd = user_data;
	ARMSOCDRI2ExecuteSwap(cmd);
}

/**
 * ScheduleSwap is responsible for requesting a DRM vblank event for the
 * appropriate frame.
 *
 * In the case of a blit (e.g. for a windowed swap) or buffer exchange,
 * the vblank requested can simply be the last queued swap frame + the swap
 * interval for the drawable.
 *
 * In the case of a page flip, we request an event for the last queued swap
 * frame + swap interval - 1, since we'll need to queue the flip for the frame
 * immediately following the received event.
 */
static int
ARMSOCDRI2ScheduleSwap(ClientPtr client, DrawablePtr pDraw,
		DRI2BufferPtr pDstBuffer, DRI2BufferPtr pSrcBuffer,
		CARD64 *target_msc, CARD64 divisor, CARD64 remainder,
		DRI2SwapEventPtr func, void *data)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
	struct ARMSOCDRISwapCmd *cmd = calloc(1, sizeof(*cmd));
	struct armsoc_bo *src_bo, *dst_bo;
	int src_fb_id, dst_fb_id;
	int ret, do_flip;

	if (!cmd)
		return FALSE;

	cmd->client = client;
	cmd->pScreen = pScreen;
	cmd->draw_id = pDraw->id;
	cmd->pSrcBuffer = pSrcBuffer;
	cmd->pDstBuffer = pDstBuffer;
	cmd->swapCount = 0;
	cmd->flags = 0;
	cmd->func = func;
	cmd->data = data;


	DEBUG_MSG("%d -> %d", pSrcBuffer->attachment, pDstBuffer->attachment);

	/* obtain extra ref on buffers to avoid them going away while we await
	 * the page flip event:
	 */
	ARMSOCDRI2ReferenceBuffer(pSrcBuffer);
	ARMSOCDRI2ReferenceBuffer(pDstBuffer);
	pARMSOC->pending_flips++;

	src_bo = boFromBuffer(pSrcBuffer);
	dst_bo = boFromBuffer(pDstBuffer);

	/* Save these such that ARMSOCDRI2SwapComplete can deref the right buffers */
	cmd->old_src_bo = src_bo;
	cmd->old_dst_bo = dst_bo;


	src_fb_id = armsoc_bo_get_fb(src_bo);
	dst_fb_id = armsoc_bo_get_fb(dst_bo);

	armsoc_bo_reference(src_bo);
	armsoc_bo_reference(dst_bo);

	do_flip = src_fb_id && dst_fb_id && canflip(pDraw);

	/* After a resolution change the back buffer (src) will still be
	 * of the original size. We can't sensibly flip to a framebuffer of
	 * a different size to the current resolution (it will look corrupted)
	 * so we must do a copy for this frame (which will clip the contents
	 * as expected).
	 *
	 * Once the client calls DRI2GetBuffers again, it will receive a new
	 * back buffer of the same size as the new resolution, and subsequent
	 * DRI2SwapBuffers will result in a flip.
	 */
	do_flip = do_flip &&
			(armsoc_bo_width(src_bo) == armsoc_bo_width(dst_bo));
	do_flip = do_flip &&
			(armsoc_bo_height(src_bo) == armsoc_bo_height(dst_bo));

	if (do_flip) {
		DEBUG_MSG("can flip:  %d -> %d", src_fb_id, dst_fb_id);
		cmd->type = DRI2_FLIP_COMPLETE;

		/* TODO: MIDEGL-1461: Handle rollback if multiple CRTC flip is
		 * only partially successful
		 */
		ret = drmmode_page_flip(pDraw, src_fb_id, cmd);

		/* If using page flip events, we'll trigger an immediate
		 * completion in the case that no CRTCs were enabled to be
		 * flipped. If not using page flip events, trigger immediate
		 * completion unconditionally.
		 */
		if (ret < 0) {
			/*
			 * Error while flipping; bail.
			 */
			cmd->flags |= ARMSOC_SWAP_FAIL;

			if (pARMSOC->drmmode_interface->use_page_flip_events)
				cmd->swapCount = -(ret + 1);
			else
				cmd->swapCount = 0;

			if (cmd->swapCount == 0)
				ARMSOCDRI2SwapComplete(cmd);

			return FALSE;
		} else {
			if (ret == 0)
				cmd->flags |= ARMSOC_SWAP_FAKE_FLIP;

			if (pARMSOC->drmmode_interface->use_page_flip_events)
				cmd->swapCount = ret;
			else
				cmd->swapCount = 0;

			if (cmd->swapCount == 0)
				ARMSOCDRI2SwapComplete(cmd);
		}
	} else {
		drmVBlank vbl = { };

		/* If we're not page flipping, delay the swap until
		 * vblank time ourselves. */
		vbl.request.type = (DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT);
		vbl.request.sequence = *target_msc;
		vbl.request.signal = (unsigned long) cmd;

		ret = drmWaitVBlank(pARMSOC->drmFD, &vbl);
		if (ret) {
			/* Oops, we couldn't schedule the swap for vblank.
			 * Just do it immediately. */
			ARMSOCDRI2ExecuteSwap(cmd);
		}
	}

	return TRUE;
}

/**
 * Request a DRM event when the requested conditions will be satisfied.
 *
 * We need to handle the event and ask the server to wake up the client when
 * we receive it.
 */
static int
ARMSOCDRI2ScheduleWaitMSC(ClientPtr client, DrawablePtr pDraw,
	CARD64 target_msc, CARD64 divisor, CARD64 remainder)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);

	ERROR_MSG("not implemented");
	return FALSE;
}

/**
 * The DRI2 ScreenInit() function.. register our handler fxns w/ DRI2 core
 */
Bool ARMSOCDRI2ScreenInit(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
	drmVBlank vbl = { 
		.request = {
		.type = DRM_VBLANK_RELATIVE,
		.sequence = 0,}
	};
	int ret;

	DRI2InfoRec info = {
		.version         = 6,
		.fd              = pARMSOC->drmFD,
		.driverName      = "etnaviv",
		.deviceName      = pARMSOC->deviceName,
		.CreateBuffer    = ARMSOCDRI2CreateBuffer,
		.DestroyBuffer   = ARMSOCDRI2DestroyBuffer,
		.ReuseBufferNotify = ARMSOCDRI2ReuseBufferNotify,
		.CopyRegion      = ARMSOCDRI2CopyRegion,
		.ScheduleSwap    = ARMSOCDRI2ScheduleSwap,
		.ScheduleWaitMSC = ARMSOCDRI2ScheduleWaitMSC,
		.GetMSC          = ARMSOCDRI2GetMSC,
		.AuthMagic       = drmAuthMagic,
		.SwapLimitValidate = NULL,
	};
	int minor = 1, major = 0;

	if (xf86LoaderCheckSymbol("DRI2Version"))
		DRI2Version(&major, &minor);

	if (minor < 1) {
		WARNING_MSG("DRI2 requires DRI2 module version 1.1.0 or later");
		return FALSE;
	}

	ret = drmWaitVBlank(pARMSOC->drmFD, &vbl);
	if (ret)
		pARMSOC->drmmode_interface->vblank_query_supported = 0;
	else
		pARMSOC->drmmode_interface->vblank_query_supported = 1;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, " DRI2 initialzed. \n");

	return DRI2ScreenInit(pScreen, &info);
}

/**
 * The DRI2 CloseScreen() function.. unregister ourself w/ DRI2 core.
 */
void ARMSOCDRI2CloseScreen(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
	while (pARMSOC->pending_flips > 0) {
		DEBUG_MSG("waiting..");
		drmmode_wait_for_event(pScrn);
	}
	DRI2CloseScreen(pScreen);
}

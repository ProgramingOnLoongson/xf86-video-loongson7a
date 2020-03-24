/*
 * Copyright © 2011 Texas Instruments, Inc
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
 *    Ian Elliott <ianelliottus@yahoo.com>
 *    Rob Clark <rob@ti.com>
 *    Sui Jingfeng <suijingfeng@loongson.cn>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <exa.h>
#include <unistd.h>

#include "loongson_exa.h"
#include "loongson_driver.h"
#include "loongson_debug.h"
#include "loongson_driver.h"
#include "loongson_pixmap.h"


/*
 * This file has a trivial EXA implementation which accelerates nothing.
 * It is used as the fall-back in case the EXA implementation for the
 * current chipset is not available.
 *
 * For example, on chipsets which used the closed source IMG PowerVR plus
 * vivante EXA implementation, if the closed-source submodule is not installed.
 */

#define NULL_DBG_MSG(fmt, ...)
// #define NULL_DBG_MSG(fmt, ...)		\
//	do { xf86Msg(X_INFO, fmt "\n", ##__VA_ARGS__); } while (0)


// suijingfeng: this is not defined in original code, debuging it
#define ARMSOC_EXA_MAP_USERPTR 1

// #define ARMSOC_EXA_DEBUG 1

struct FakeExa
{
    struct ARMSOCEXARec base;
    ExaDriverPtr exa;
    /* add any other driver private data here.. */
};


/////////////////////////////////////////////////////////////////////////

static Bool PrepareSolidFail(PixmapPtr pPixmap, int alu, Pixel planemask,
        Pixel fill_colour)
{
    return FALSE;
}

static Bool PrepareCopyFail(PixmapPtr pSrc, PixmapPtr pDst, int xdir, int ydir,
        int alu, Pixel planemask)
{
    return FALSE;
}

static Bool CheckCompositeFail(int op, PicturePtr pSrcPicture, PicturePtr pMaskPicture,
        PicturePtr pDstPicture)
{
    return FALSE;
}

static Bool PrepareCompositeFail(int op, PicturePtr pSrcPicture, PicturePtr pMaskPicture,
        PicturePtr pDstPicture, PixmapPtr pSrc, PixmapPtr pMask, PixmapPtr pDst)
{
    return FALSE;
}

/////////////////////////////////////////////////////////////////////////


static void AllocBuf(struct ARMSOCEXARec *exa, int width, int height,
        int depth, int bpp, int usage_hint, struct ARMSOCEXABuf * pBuf)
{
    unsigned int pitch = ((width * bpp + FB_MASK) >> FB_SHIFT) * sizeof(FbBits);
    // suijingfeng: testing this  align value is correct ???
    pitch = (pitch + 255) & ~(255);
    size_t size = pitch * height;
    pBuf->buf = malloc(size);
    pBuf->pitch = pitch;
    pBuf->size = size;
    //	xf86Msg(X_INFO, " AllocBuffer:%p, pitch:%d\n", pBuf->buf, pitch);
}


static void FreeBuf(struct ARMSOCEXARec *exa, struct ARMSOCEXABuf * pBuf)
{
    //	NULL_DBG_MSG("FreeBuf buf:%p", pBuf->buf);
    free( pBuf->buf );
    pBuf->buf = NULL;
    pBuf->pitch = 0;
    pBuf->size = 0;
}


//////////////////////////////////////////////////////////////////////////

static void Reattach(PixmapPtr pPixmap, int width, int height, int pitch)
{
    xf86Msg(X_INFO, "Reattach pixmap:%p, %dx%d, pitch=%d\n",
            pPixmap, width, height, pitch);
}




/**
 * CloseScreen() is called at the end of each server generation and
 * cleans up everything initialised in InitSoftEXA()
 */
static Bool CloseScreen(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);

    exaDriverFini(pScreen);
    free(((struct FakeExa *)pARMSOC->pARMSOCEXA)->exa);
    free(pARMSOC->pARMSOCEXA);
    pARMSOC->pARMSOCEXA = NULL;

    return TRUE;
}

/* FreeScreen() is called on an error during PreInit and
 * should clean up anything initialised before InitSoftEXA()
 * (which currently is nothing)
 *
 */
static void FreeScreen(ScrnInfoPtr arg)
{

}

/**
 * WaitMarker is a required EXA callback but synchronization is
 * performed during PrepareAccess so this function does not
 * have anything to do at present
 */
static void WaitMarker(ScreenPtr pScreen, int marker)
{
    /* no-op */
}



static void * CreatePixmap2(ScreenPtr pScreen,
        int width, int height, int depth,
        int usage_hint, int bitsPerPixel, int *new_fb_pitch)
{

    // ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);

    // loongsonRecPtr pARMSOC = loongsonPTR(pScrn);

    if ( usage_hint == CREATE_PIXMAP_USAGE_BACKING_PIXMAP)
    {
        return LS_CreateDumbPixmap( pScreen, width, height, depth,
            usage_hint, bitsPerPixel, new_fb_pitch );
    }
    else
    {
        return LS_CreateExaPixmap( pScreen, width, height, depth,
            usage_hint, bitsPerPixel, new_fb_pitch );
    }
}


static void DestroyPixmap(ScreenPtr pScreen, void *driverPriv)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
    struct ARMSOCPixmapPrivRec *priv = driverPriv;

    assert(!priv->ext_access_cnt);

    /* If ModifyPixmapHeader failed, it's possible we don't have a bo
     * backing this pixmap. */
    if (priv->bo)
    {
        assert(!armsoc_bo_has_dmabuf(priv->bo));
#ifdef ARMSOC_EXA_DEBUG
        INFO_MSG("DestroyPixmap bo:%p", priv->bo);
#endif
        /* pixmap drops ref on its bo */
        armsoc_bo_unreference(priv->bo);
    }

    if (priv->buf.buf)
    {
#ifdef ARMSOC_EXA_DEBUG
        INFO_MSG("DestroyPixmap buf:%p", priv->buf);
#endif
        FreeBuf(pARMSOC->pARMSOCEXA, &priv->buf);
    }

    free(priv);
}


//  For pixmaps that are scanout or backing for windows,
//  we "accelerate" them by allocating them via GEM.
//
//  For all other pixmaps (where we never expect DRI2
//  CreateBuffer to be called), we just malloc them,
//  which turns out to be much faster.
//
/*
if(priv->usage_hint == ARMSOC_CREATE_PIXMAP_SCANOUT)
{
    xf86Msg(X_INFO,
            "usage hint : ARMSOC_CREATE_PIXMAP_SCANOUT \n");
}
else if(priv->usage_hint == CREATE_PIXMAP_USAGE_BACKING_PIXMAP)
{
    xf86Msg(X_INFO,
            "usage hint : CREATE_PIXMAP_USAGE_BACKING_PIXMAP \n");
}
else if(priv->usage_hint == CREATE_PIXMAP_USAGE_SHARED)
{
    xf86Msg(X_INFO,
            "usage hint : CREATE_PIXMAP_USAGE_SHARED \n");
}
else if(priv->usage_hint == CREATE_PIXMAP_USAGE_SCRATCH)
{
    xf86Msg(X_INFO,
            "usage hint : CREATE_PIXMAP_USAGE_SCRATCH \n");
}
else if(priv->usage_hint == CREATE_PIXMAP_USAGE_GLYPH_PICTURE)
{
    xf86Msg(X_INFO,
            "usage hint : CREATE_PIXMAP_USAGE_GLYPH_PICTURE \n");
}
*/

static Bool IsDumbPixmap(struct ARMSOCPixmapPrivRec *priv)
{
//		(priv->usage_hint == CREATE_PIXMAP_USAGE_BACKING_PIXMAP) ||
//		(priv->usage_hint == CREATE_PIXMAP_USAGE_SCRATCH)
	// suijingfeng: what about CREATE_PIXMAP_USAGE_SHARED ?
	return ((priv->usage_hint == ARMSOC_CREATE_PIXMAP_SCANOUT) ||
            (priv->usage_hint == CREATE_PIXMAP_USAGE_BACKING_PIXMAP));
}




/**
 * PrepareAccess() is called before CPU access to an offscreen pixmap.
 *
 * @param pPix the pixmap being accessed
 * @param index the index of the pixmap being accessed.
 *
 * PrepareAccess() will be called before CPU access to an offscreen pixmap.
 * This can be used to set up hardware surfaces for byteswapping or
 * untiling, or to adjust the pixmap's devPrivate.ptr for the purpose of
 * making CPU access use a different aperture.
 *
 * The index is one of #EXA_PREPARE_DEST, #EXA_PREPARE_SRC,
 * #EXA_PREPARE_MASK, #EXA_PREPARE_AUX_DEST, #EXA_PREPARE_AUX_SRC, or
 * #EXA_PREPARE_AUX_MASK. Since only up to #EXA_NUM_PREPARE_INDICES pixmaps
 * will have PrepareAccess() called on them per operation, drivers can have
 * a small, statically-allocated space to maintain state for PrepareAccess()
 * and FinishAccess() in.  Note that PrepareAccess() is only called once per
 * pixmap and operation, regardless of whether the pixmap is used as a
 * destination and/or source, and the index may not reflect the usage.
 *
 * PrepareAccess() may fail.  An example might be the case of hardware that
 * can set up 1 or 2 surfaces for CPU access, but not 3.  If PrepareAccess()
 * fails, EXA will migrate the pixmap to system memory.
 * DownloadFromScreen() must be implemented and must not fail if a driver
 * wishes to fail in PrepareAccess().  PrepareAccess() must not fail when
 * pPix is the visible screen, because the visible screen can not be
 * migrated.
 *
 * @return TRUE if PrepareAccess() successfully prepared the pixmap for CPU
 * drawing.
 * @return FALSE if PrepareAccess() is unsuccessful and EXA should use
 * DownloadFromScreen() to migate the pixmap out.
 */
static Bool PrepareAccess(PixmapPtr pPixmap, int index)
{
    struct ARMSOCPixmapPrivRec *priv = exaGetPixmapDriverPrivate(pPixmap);
    ScreenPtr pScreen = pPixmap->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    loongsonRecPtr pARMSOC = loongsonPTR(pScrn);

//    TRACE_ENTER();

    if (NULL == priv)
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
            "%s: Failed to map buffer.\n", __func__);
        TRACE_EXIT();
        return FALSE;
    }

/*
    if (pPix->devPrivate.ptr)
    {
        // why ?
//        TRACE_EXIT();
        return TRUE;
    }
*/

    if (FALSE == IsDumbPixmap(priv))
    {
        pPixmap->devPrivate.ptr = priv->buf.buf;
//        TRACE_EXIT();
        return TRUE;
    }

    dumb_bo_map(priv->bo->fd, priv->bo);

    pPixmap->devPrivate.ptr = priv->bo->ptr;

    if (!pPixmap->devPrivate.ptr)
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
            "%s: Failed to map buffer.\n", __func__);
        TRACE_EXIT();
        return FALSE;
    }

    /*
    * Attach dmabuf fd to bo to synchronise access if pixmap wrapped by DRI2
    */
    if (priv->ext_access_cnt && !armsoc_bo_has_dmabuf(priv->bo))
    {
        int prime_fd;
        if (armsoc_bo_to_dmabuf(priv->bo, &prime_fd))
        {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                    "%s: Unable to get dma_buf fd for bo.\n",
                    __func__);
            return FALSE;
        }
    }

    if ( (0 == priv->ext_access_cnt) ||
         (priv->usage_hint == ARMSOC_CREATE_PIXMAP_SCANOUT) )
    {
        return TRUE;
    }


    if (armsoc_bo_cpu_prep(priv->bo))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                "%s: bo_cpu_prep failed - unable to synchronise access.\n",
                __func__);
        return FALSE;
    }

//    TRACE_EXIT();
    return TRUE;
}


/**
 * FinishAccess() is called after CPU access to an offscreen pixmap.
 *
 * @param pPix the pixmap being accessed
 * @param index the index of the pixmap being accessed.
 *
 * FinishAccess() will be called after finishing CPU access of an offscreen
 * pixmap set up by PrepareAccess().  Note that the FinishAccess() will not be
 * called if PrepareAccess() failed and the pixmap was migrated out.
 */
static void FinishAccess(PixmapPtr pPixmap, int index)
{
    struct ARMSOCPixmapPrivRec *priv = exaGetPixmapDriverPrivate(pPixmap);

    pPixmap->devPrivate.ptr = NULL;

    /* NOTE: can we use EXA migration module to track which parts of the
     * buffer was accessed by sw, and pass that info down to kernel to
     * do a more precise cache flush..
     */
    if( IsDumbPixmap(priv) )
    {
        armsoc_bo_cpu_fini(priv->bo);
    }
}


static Bool PixmapIsOffscreen(PixmapPtr pPixmap)
{
    // offscreen means in 'gpu accessible memory', not that it's off
    // the visible screen. We currently have no special constraints,
    // since compatible ARM CPUS have a flat memory model (no separate
    // GPU memory).
    //
    // If an individual EXA implementation has additional constraints,
    // like buffer size or mapping in GPU MMU, it should wrap this function.
    struct ARMSOCPixmapPrivRec *priv = exaGetPixmapDriverPrivate(pPixmap);
    return priv && (priv->bo || priv->buf.buf);
}


static Bool ModifyPixmapHeader(PixmapPtr pPixmap, int width, int height,
        int depth, int bitsPerPixel, int devKind, pointer pPixData)
{
    struct ARMSOCPixmapPrivRec *priv = exaGetPixmapDriverPrivate(pPixmap);

    if ( IsDumbPixmap(priv) )
    {
        return LS_ModifyDumbPixmapHeader(pPixmap, width, height,
                depth, bitsPerPixel, devKind, pPixData);
    }
    else
    {
        return LS_ModifyExaPixmapHeader(pPixmap, width, height,
                depth, bitsPerPixel, devKind, pPixData);
    }
}


struct ARMSOCEXARec * LS_InitSoftwareEXA(ScreenPtr pScreen, ScrnInfoPtr pScrn, int fd)
{
    struct FakeExa *pSoftExa;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Soft EXA mode enable.\n");

    pSoftExa = calloc(1, sizeof(*pSoftExa));
    if ( NULL == pSoftExa )
    {
        return NULL;
    }

    ExaDriverPtr pExaDrv = exaDriverAlloc();
    if ( NULL == pExaDrv )
    {
        free( pSoftExa );
        return NULL;
    }

    pExaDrv->exa_major = EXA_VERSION_MAJOR;
    pExaDrv->exa_minor = EXA_VERSION_MINOR;

    pExaDrv->pixmapOffsetAlign = 16;
    pExaDrv->pixmapPitchAlign = 256;
    pExaDrv->flags =
        EXA_OFFSCREEN_PIXMAPS | EXA_HANDLES_PIXMAPS | EXA_SUPPORTS_PREPARE_AUX;
    pExaDrv->maxX = 4096;
    pExaDrv->maxY = 4096;

    /* Required EXA functions: */
    pExaDrv->WaitMarker = WaitMarker;
    pExaDrv->DestroyPixmap = DestroyPixmap;
    pExaDrv->CreatePixmap2 = CreatePixmap2;

    pExaDrv->ModifyPixmapHeader = ModifyPixmapHeader;

    pExaDrv->PrepareAccess = PrepareAccess;
    pExaDrv->FinishAccess = FinishAccess;

    // PixmapIsOffscreen() is an optional driver replacement to exaPixmapHasGpuCopy().
    // Set to NULL if you want the standard behaviour of exaPixmapHasGpuCopy().
    // return TRUE if the given drawable is in framebuffer memory.
    //
    // exaPixmapHasGpuCopy() is used to determine if a pixmap is in offscreen memory,
    // meaning that acceleration could probably be done to it, and that it will need
    // to be wrapped by PrepareAccess()/FinishAccess() when accessing it with the CPU.
    pExaDrv->PixmapIsOffscreen = PixmapIsOffscreen;

    /* Always fallback for software operations */
    pExaDrv->PrepareCopy = PrepareCopyFail;
    pExaDrv->PrepareSolid = PrepareSolidFail;
    pExaDrv->CheckComposite = CheckCompositeFail;
    pExaDrv->PrepareComposite = PrepareCompositeFail;

    if (!exaDriverInit(pScreen, pExaDrv))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "exaDriverInit failed\n");
        free(pExaDrv);
        free(pSoftExa);
        return NULL;
    }


    pSoftExa->exa = pExaDrv;
    struct ARMSOCEXARec *pBase = &pSoftExa->base;
    pBase->Reattach = Reattach;
    pBase->AllocBuf = AllocBuf;
    pBase->FreeBuf = FreeBuf;
    pBase->CloseScreen = CloseScreen;
    pBase->FreeScreen = FreeScreen;

    return pBase;
}

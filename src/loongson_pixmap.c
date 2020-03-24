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


#include <unistd.h>

#include "loongson_exa.h"
#include "loongson_driver.h"
#include "loongson_debug.h"


void * LS_CreateExaPixmap(ScreenPtr pScreen,
        int width, int height, int depth,
        int usage_hint, int bitsPerPixel,
        int *new_fb_pitch)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    loongsonRecPtr pLs = loongsonPTR(pScrn);

    // TRACE_ENTER();

    struct ARMSOCPixmapPrivRec *priv = calloc(1, sizeof( *priv ));
    if ( NULL == priv )
    {
        return NULL;
    }

    priv->usage_hint = usage_hint;


    if (width > 0 && height > 0 && depth > 0 && bitsPerPixel > 0)
    {
        // DEBUG_MSG ( "Create Exa Pixmap %dx%d %d %d",
        //        width, height, depth, bitsPerPixel);

        pLs->pARMSOCEXA->AllocBuf( pLs->pARMSOCEXA,
            width, height, depth, bitsPerPixel, usage_hint, &priv->buf);

        if ( NULL == priv->buf.buf)
        {
            ERROR_MSG("failed to allocate %dx%d mem", width, height);
            free(priv);
            return NULL;
        }

        *new_fb_pitch = priv->buf.pitch;
    }

    // TRACE_EXIT();

    return priv;
}


void * LS_CreateDumbPixmap(ScreenPtr pScreen,
        int width, int height, int depth,
        int usage_hint, int bitsPerPixel,
        int *new_fb_pitch )
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    loongsonRecPtr pLs = loongsonPTR(pScrn);

    TRACE_ENTER();

    struct ARMSOCPixmapPrivRec *priv = calloc(1, sizeof( *priv ));

    if ( NULL == priv )
    {
        return NULL;
    }

    priv->usage_hint = usage_hint;

    if ( (width > 0) && (height > 0) && (depth > 0) && (bitsPerPixel > 0) )
    {
        /* Pixmap creates and takes a ref on its bo */
        DEBUG_MSG ( "Create Dumb Pixmap %dx%d %d %d",
                width, height, depth, bitsPerPixel);

        priv->bo = armsoc_bo_new_with_dim(pLs->drmFD,
                width, height, depth, bitsPerPixel);

        if (NULL == priv->bo)
        {
            ERROR_MSG("failed to allocate %dx%d bo", width, height);
            free(priv);
            return NULL;
        }

        *new_fb_pitch = armsoc_bo_pitch(priv->bo);

        // *new_fb_pitch = (*new_fb_pitch + 255) & ~255;
        // DEBUG_MSG ( "Create Dumb Pixmap: new pitch %d",
        //    *new_fb_pitch );
    }

    TRACE_EXIT();

    return priv;
}



//////////////////////////////////////////////////////////////////////////
//
//    Modify pixmap header
//
//////////////////////////////////////////////////////////////////////////


Bool LS_ModifyDumbPixmapHeader( PixmapPtr pPixmap,
        int width, int height, int depth, int bitsPerPixel,
        int devKind, pointer pPixData )
{
    ScreenPtr pScreen = pPixmap->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    loongsonRecPtr pARMSOC = loongsonPTR(pScrn);

    struct ARMSOCPixmapPrivRec *priv = exaGetPixmapDriverPrivate(pPixmap);

    Bool ret = FALSE;
    TRACE_ENTER();

    ret = miModifyPixmapHeader(pPixmap, width, height, depth, bitsPerPixel,
                                devKind, pPixData);
    if (FALSE == ret)
    {
         TRACE_EXIT();
         return FALSE;
    }

    dumb_bo_map(pARMSOC->scanout->fd, pARMSOC->scanout);

    //  We can't accelerate this pixmap, and don't ever want to see it again..
    if (pPixData && pPixData != pARMSOC->scanout->ptr)
    {
        // scratch-pixmap (see GetScratchPixmapHeader()) gets recycled,
        // so could have a previous bo! Pixmap drops ref on its old bo
        armsoc_bo_unreference(priv->bo);
        priv->bo = NULL;

        TRACE_EXIT();
        // Returning FALSE calls miModifyPixmapHeader
        return FALSE;
    }
/*
	//  Replacing the pixmap's current bo with the scanout bo
	if ((pPixData == pARMSOC->scanout->ptr) && (priv->bo != pARMSOC->scanout) )
	{
		struct dumb_bo *old_bo = priv->bo;

		priv->bo = pARMSOC->scanout;

//		INFO_MSG("ModifyDumbPixmapHeader scanout pix:%p bo:%p, %dx%d %d %d",
//		priv, priv->bo, width, height, depth, bitsPerPixel);

		// pixmap takes a ref on its new bo
		armsoc_bo_reference(priv->bo);

		if (old_bo)
		{
			// We are detaching the old_bo so clear it now.
			if (armsoc_bo_has_dmabuf(old_bo))
			{
				armsoc_bo_clear_dmabuf(old_bo);
			// pixmap drops ref on previous bo
			}
			armsoc_bo_unreference(old_bo);
		}
	}
*/

    // suijingfeng: replace
    if ( pPixData == pARMSOC->scanout->ptr )
    {
        armsoc_bo_unreference(priv->bo);
        priv->bo = pARMSOC->scanout;
        armsoc_bo_reference(priv->bo);
    }

     // X will sometimes create an empty pixmap (width = 0 or height == 0)
     // and then use ModifyPixmapHeader to point it at PixData.

     // We'll hit  this path during the CreatePixmap call.
     // Just return true and skip the allocate in this case.

    if ((0 == pPixmap->drawable.width) || (0 == pPixmap->drawable.height))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                "Dumb: create an empty pixmap.\n");

        TRACE_EXIT();
        return TRUE;
    }

    assert(priv->bo);

	if ( (armsoc_bo_width(priv->bo) != pPixmap->drawable.width) ||
		 (armsoc_bo_height(priv->bo) != pPixmap->drawable.height) ||
		 (armsoc_bo_bpp(priv->bo) != pPixmap->drawable.bitsPerPixel) )
	{
		/* pixmap drops ref on its old bo */
		armsoc_bo_unreference(priv->bo);


        DEBUG_MSG( "ModifyDumbPixmapHeader: pPixmap: %dx%d, bpp: %d\n",
                pPixmap->drawable.width,
                pPixmap->drawable.height,
                pPixmap->drawable.bitsPerPixel );

        DEBUG_MSG( "ModifyDumbPixmapHeader: priv->bo: %dx%d, bpp: %d\n",
                armsoc_bo_width(priv->bo),
                armsoc_bo_height(priv->bo),
                armsoc_bo_bpp(priv->bo) );

/*
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"bo : %dx%d, depth: %d, bpp: %d\n",
				armsoc_bo_width(priv->bo), armsoc_bo_height(priv->bo),
				armsoc_bo_depth(priv->bo), armsoc_bo_bpp);
*/

		/* pixmap creates new bo and takes ref on it */
		priv->bo = armsoc_bo_new_with_dim(pARMSOC->drmFD,
			pPixmap->drawable.width, pPixmap->drawable.height,
			pPixmap->drawable.depth, pPixmap->drawable.bitsPerPixel);


		if (priv->bo == NULL)
		{
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Dumb: failed to allocate %dx%d bo\n",
				pPixmap->drawable.width, pPixmap->drawable.height);

            TRACE_EXIT();
			return FALSE;
		}

        // DEBUG_MSG("copy mode %s (%p %p)", kmode->name, mode->name, mode);
        // priv->pitch = armsoc_bo_pitch(priv->bo);
        pPixmap->devKind = armsoc_bo_pitch(priv->bo);
    }

    TRACE_EXIT();

    return TRUE;
}


Bool LS_ModifyExaPixmapHeader( PixmapPtr pPixmap,
        int width, int height, int depth,
        int bitsPerPixel, int devKind, pointer pPixData )
{

	ScreenPtr pScreen = pPixmap->drawable.pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	loongsonRecPtr pARMSOC = loongsonPTR(pScrn);

	struct ARMSOCPixmapPrivRec *priv = exaGetPixmapDriverPrivate(pPixmap);
	size_t size;

	/* Only modify specified fields, keeping all others intact. */
	if (pPixData)
	{
		pPixmap->devPrivate.ptr = pPixData;
	}

	if (devKind > 0)
	{
		pPixmap->devKind = devKind;
	}

#ifndef ARMSOC_EXA_MAP_USERPTR

	// Someone is messing with the memory allocation. Let's step out of the picture.
	if (pPixData && pPixData != priv->buf.buf)
	{
#ifdef ARMSOC_EXA_DEBUG
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			" %p pPixData(%p) != priv->buf.buf(%p) %dx%d %d %d/%d\n",
			pPixmap, pPixData, priv->buf.buf, width, height, devKind,
			bitsPerPixel, depth);
#endif
		if (priv->buf.buf)
		{
			pARMSOC->pARMSOCEXA->FreeBuf(pARMSOC->pARMSOCEXA, &priv->buf);
		}

		priv->buf.buf = NULL;
		priv->buf.size = 0;
		priv->buf.pitch = 0;

		/* Returning FALSE calls miModifyPixmapHeader */
		return FALSE;
	}
#endif

	if (depth > 0)
		pPixmap->drawable.depth = depth;

	if (bitsPerPixel > 0)
		pPixmap->drawable.bitsPerPixel = bitsPerPixel;

	if (width > 0)
		pPixmap->drawable.width = width;

	if (height > 0)
		pPixmap->drawable.height = height;

	/*
	 * X will sometimes create an empty pixmap (width/height == 0) and then
	 * use ModifyPixmapHeader to point it at PixData. We'll hit this path
	 * during the CreatePixmap call. Just return true and skip the allocate
	 * in this case.
	 */
	if (!pPixmap->drawable.width || !pPixmap->drawable.height)
		return TRUE;

	size = devKind * height;

	// Someone is messing with the memory allocation. Let's step out of  the picture.
#ifdef ARMSOC_EXA_MAP_USERPTR
	if (pPixData)
	{
#ifdef ARMSOC_EXA_DEBUG
		INFO_MSG("Modify Exa Pixmap Header pPixmap=%p, pPixData=%p",
			pPixmap, pPixData);
		INFO_MSG("%dx%d, devKind=%d, depth=%d, bpp=%d/%d",
			width, height, devKind, depth, bitsPerPixel);
#endif
		if (pPixData != priv->buf.buf || priv->buf.size != size)
		{

#ifdef ARMSOC_EXA_DEBUG
			if(pPixData != priv->buf.buf)
				INFO_MSG("pPixData(%p) != priv->buf.buf(%p)", pPixData, priv->buf.buf);

			if(priv->buf.size != size)
				INFO_MSG("priv->buf.size(%d) != size(%d)", priv->buf.size, size);
#endif

			if ( priv->buf.buf && pARMSOC->pARMSOCEXA->UnmapUsermemBuf )
			{
				pARMSOC->pARMSOCEXA->UnmapUsermemBuf(pARMSOC->pARMSOCEXA, &priv->buf);
			}

			if ( pARMSOC->pARMSOCEXA->MapUsermemBuf &&
				pARMSOC->pARMSOCEXA->MapUsermemBuf( pARMSOC->pARMSOCEXA,
						width, height, devKind, pPixData, &priv->buf) )
			{
				return TRUE;
			}
			else
			{
				priv->buf.buf = NULL;
				priv->buf.size = 0;
				priv->buf.pitch = 0;
				/* Returning FALSE calls miModifyPixmapHeader */
				return FALSE;
			}
		}
		else
		{
			return TRUE;
		}
	}
#endif

	if (!priv->buf.buf || priv->buf.size != size)
	{
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                "priv->buf.size=%d, size=%d\n",
                priv->buf.size, size);

		/* re-allocate buffer! */
		if (priv->buf.buf)
		{
			pARMSOC->pARMSOCEXA->FreeBuf(pARMSOC->pARMSOCEXA, &priv->buf);
		}

		pARMSOC->pARMSOCEXA->AllocBuf(pARMSOC->pARMSOCEXA,
			pPixmap->drawable.width, pPixmap->drawable.height,
			pPixmap->drawable.depth, pPixmap->drawable.bitsPerPixel, 0, &priv->buf);

		if (NULL == priv->buf.buf)
		{
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                    "ModifyExaPixmapHeader failed to allocate %d bytes mem.\n",
                    size);
			priv->buf.size = 0;
			priv->buf.pitch = 0;
			return FALSE;
		}

		pPixmap->devKind = priv->buf.pitch;
	}

	return TRUE;
}

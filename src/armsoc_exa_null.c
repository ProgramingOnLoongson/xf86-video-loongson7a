/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright: GPL
 *
 * Authors:
 *    Rob Clark <rob@ti.com>
 *    suijingfeng@loognson.com
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "armsoc_driver.h"
#include "armsoc_exa.h"

#include <exa.h>

/* This file has a trivial EXA implementation which accelerates nothing. 
 * It is used as the fall-back in case the EXA implementation for the 
 * current chipset is not available. For example, on chipsets which used
 * the closed source IMG PowerVR/vivante EXA implementation, if the closed-source
 * submodule is not installed.
 */

#define NULL_DBG_MSG(fmt, ...)
// #define NULL_DBG_MSG(fmt, ...)		\
//	do { xf86Msg(X_INFO, fmt "\n", ##__VA_ARGS__); } while (0)

struct ARMSOCNullEXARec {
	struct ARMSOCEXARec base;
	ExaDriverPtr exa;
	/* add any other driver private data here.. */
};

static void AllocBuf( struct ARMSOCEXARec *exa, int width, int height, 
		int depth, int bpp, int usage_hint, struct ARMSOCEXABuf * pBuf)
{
	unsigned int pitch = ((width * bpp + FB_MASK) >> FB_SHIFT) * sizeof(FbBits);
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

static void Reattach(PixmapPtr pPixmap, int width, int height, int pitch)
{
	NULL_DBG_MSG("Reattach pixmap:%p", pPixmap);
}


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

/**
 * CloseScreen() is called at the end of each server generation and
 * cleans up everything initialised in InitNullEXA()
 */
static Bool CloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);

	exaDriverFini(pScreen);
	free(((struct ARMSOCNullEXARec *)pARMSOC->pARMSOCEXA)->exa);
	free(pARMSOC->pARMSOCEXA);
	pARMSOC->pARMSOCEXA = NULL;

	return TRUE;
}

/* FreeScreen() is called on an error during PreInit and
 * should clean up anything initialised before InitNullEXA()
 * (which currently is nothing)
 *
 */
static void FreeScreen(FREE_SCREEN_ARGS_DECL)
{
}

struct ARMSOCEXARec * InitNullEXA(ScreenPtr pScreen, ScrnInfoPtr pScrn, int fd)
{
	struct ARMSOCNullEXARec *pSoftExa;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Soft EXA mode enable \n");

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

	pExaDrv->pixmapOffsetAlign = 0;
	pExaDrv->pixmapPitchAlign = 32;
	pExaDrv->flags = EXA_OFFSCREEN_PIXMAPS |
	             EXA_HANDLES_PIXMAPS | EXA_SUPPORTS_PREPARE_AUX;
	pExaDrv->maxX = 2048;
	pExaDrv->maxY = 2048;

	/* Required EXA functions: */
	pExaDrv->WaitMarker = ARMSOCWaitMarker;
	pExaDrv->CreatePixmap2 = ARMSOCCreatePixmap2;
	pExaDrv->DestroyPixmap = ARMSOCDestroyPixmap;
	pExaDrv->ModifyPixmapHeader = ARMSOCModifyPixmapHeader;

	pExaDrv->PrepareAccess = ARMSOCPrepareAccess;
	pExaDrv->FinishAccess = ARMSOCFinishAccess;
	pExaDrv->PixmapIsOffscreen = ARMSOCPixmapIsOffscreen;

	/* Always fallback for software operations */
	pExaDrv->PrepareCopy = PrepareCopyFail;
	pExaDrv->PrepareSolid = PrepareSolidFail;
	pExaDrv->CheckComposite = CheckCompositeFail;
	pExaDrv->PrepareComposite = PrepareCompositeFail;

	if (!exaDriverInit(pScreen, pExaDrv))
	{
		ERROR_MSG("exaDriverInit failed");
		free(pExaDrv);
		free(pSoftExa);
		return NULL;
	}

	struct ARMSOCEXARec *pBase = &pSoftExa->base;
	pBase->Reattach = Reattach;
	pBase->AllocBuf = AllocBuf;
	pBase->FreeBuf = FreeBuf;
	pBase->CloseScreen = CloseScreen;
	pBase->FreeScreen = FreeScreen;
	pSoftExa->exa = pExaDrv;
	
	return pBase;
}


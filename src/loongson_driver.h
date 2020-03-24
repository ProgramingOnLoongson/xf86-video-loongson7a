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

#ifndef __ARMSOC_DRV_H__
#define __ARMSOC_DRV_H__


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif


#include <xf86.h>
#include <xf86xv.h>
#include <xf86drm.h>
#include <errno.h>
#include "loongson_exa.h"
#include "drmmode_display.h"

/* Driver name as used in config file */
#define LOONGSON_DRIVER_NAME		"loongson7a"


/* Various logging/debug macros for use in the X driver and the external
 * sub-modules:
 */


/** The driver's Screen-specific, "private" data structure. */
struct ARMSOCRec {
	/**
	 * Pointer to a structure used to communicate and coordinate with an
	 * external EXA library (if loaded).
	 */
	struct ARMSOCEXARec * pARMSOCEXA;
    EntityInfoPtr pEnt;
	/** record if ARMSOCDRI2ScreenInit() was successful */
	Bool				dri2;
	Bool				dri3;

	/** user-configurable option: */
	Bool 				SoftExa;
	unsigned			driNumBufs;

	/** File descriptor of the connection with the DRM. */
	int				drmFD;

	char * deviceName;
	/** interface to hardware specific functionality */


////////////////////////////////////////////////////////////////////////////////
	// struct drmmode_interface *drmmode_interface;
	/* Must match name used in the kernel driver */
	const char *driver_name;

	/* The cursor width */
	uint32_t cursor_width;

	/* The cursor height */
	uint32_t cursor_height;

	/* Boolean value indicating whether the DRM supports vblank timestamp query */
	Bool vblank_query_supported;

/////////////////////////////////////////////////////////////////////////////
    // suijingfeng: where to initialize it ?
    struct drmmode_rec drmmode;

	/** Scan-out buffer. */
	struct dumb_bo * scanout;

	/** Pointer to the options for this screen. */
	OptionInfoPtr		pOptionInfo;

	/** Save (wrap) the original pScreen functions. */
	CloseScreenProcPtr				SavedCloseScreen;
	CreateScreenResourcesProcPtr	SavedCreateScreenResources;
	ScreenBlockHandlerProcPtr		SavedBlockHandler;

	/** Flips we are waiting for: */
	int					pending_flips;

	/* Identify which CRTC to use. -1 uses all CRTCs */
	int					crtcNum;

	/* The Swap Chain stores the pending swap operations */
	struct ARMSOCDRISwapCmd            **swap_chain;

	/* Count of swaps scheduled since startup.
	 * Used as swap_id of the next swap cmd */
	unsigned int                       swap_chain_count;

	/* Size of the swap chain. Set to 1 if DRI2SwapLimit unsupported,
	 * driNumBufs if early display enabled, otherwise driNumBufs-1 */
	unsigned int                       swap_chain_size;

	XF86VideoAdaptorPtr textureAdaptor;
};


typedef struct ARMSOCRec loongsonRec;
typedef struct ARMSOCRec* loongsonRecPtr;

/*
 * Misc utility macros:
 */

/** Return a pointer to the driver's private structure. */
#define ARMSOCPTR(p) ((struct ARMSOCRec *)((p)->driverPrivate))

#define loongsonPTR(p) ((loongsonRecPtr)((p)->driverPrivate))


#define ARMSOCPTR_FROM_SCREEN(pScreen) \
	((struct ARMSOCRec *)(xf86ScreenToScrn(pScreen))->driverPrivate)




#ifndef ARRAY_SIZE
#  define ARRAY_SIZE(a)  (sizeof(a) / sizeof(a[0]))
#endif

/**
 * drmmode functions..
 */
Bool drmmode_pre_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode, int cpp);
void drmmode_uevent_init(ScrnInfoPtr pScrn, struct drmmode_rec *drmmode);
void drmmode_uevent_fini(ScrnInfoPtr pScrn);

void drmmode_adjust_frame(ScrnInfoPtr pScrn, int x, int y);
void drmmode_wait_for_event(ScrnInfoPtr pScrn);
void drmmode_cursor_fini(ScreenPtr pScreen);
void drmmode_init_wakeup_handler(struct ARMSOCRec *pARMSOC);
void drmmode_fini_wakeup_handler(struct ARMSOCRec *pARMSOC);
uint32_t drmmode_get_crtc_id(ScrnInfoPtr pScrn);



void LS_SetupScrnHooks(ScrnInfoPtr scrn, Bool (* pFnProbe)(DriverPtr, int));



// XV
Bool ARMSOCVideoScreenInit(ScreenPtr pScreen);
void ARMSOCVideoCloseScreen(ScreenPtr pScreen);


// EXA
struct ARMSOCEXARec *InitViv2DEXA(ScreenPtr pScreen, ScrnInfoPtr pScrn, int fd);


#endif /* __ARMSOC_DRV_H__ */

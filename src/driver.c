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

#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include <xf86drmMode.h>
#include <xf86drm.h>
#include <xf86Crtc.h>
#include <xf86cmap.h>

#include <micmap.h>

#ifdef XSERVER_PLATFORM_BUS
#include <xf86platformBus.h>
#endif


#include "loongson_driver.h"
#include "loongson_probe.h"
#include "loongson_debug.h"
#include "loongson_options.h"
#include "loongson_entity.h"
#include "loongson_present.h"
#include "loongson_helpers.h"
#include "loongson_dri2.h"
#include "loongson_dri3.h"

#include "drmmode_display.h"
#include "drmmode_cursor.h"

// those things defined in the xf86.h and xf86str.h files
// are visible to video drivers

/*
 * Forward declarations:
 */
static Bool PreInit(ScrnInfoPtr pScrn, int flags);
// An initialisation function is called from the DIX layer
// for each screen at the start of each server generation.
static Bool ScreenInit(ScreenPtr pScreen, int argc, char **argv);


static void FreeScreen(ScrnInfoPtr arg);
static Bool CloseScreen(ScreenPtr pScreen);

// The server takes control of the console.
static Bool EnterVT(ScrnInfoPtr pScrn);
// The server releases control of the console.
static void LeaveVT(ScrnInfoPtr pScrn);


static ModeStatus ValidMode(ScrnInfoPtr arg, DisplayModePtr mode,
        Bool verbose, int flags);

// Change video mode.
static Bool SwitchMode(ScrnInfoPtr arg, DisplayModePtr mode);

static void AdjustFrame(ScrnInfoPtr arg, int x, int y);


//////////////////////////////////////////////////////////////////////////

static void drmmode_load_palette(ScrnInfoPtr pScrn, int numColors,
                     int *indices, LOCO * colors, VisualPtr pVisual);


static void LS_LoadPalette(ScrnInfoPtr pScrn, int numColors, int *indices,
                              LOCO *colors, VisualPtr pVisual);

static Bool CreateScreenResources(ScreenPtr pScreen);
static void lsBlockHandler(ScreenPtr pScreen, void *timeout);



static int SetMaster(ScrnInfoPtr pScrn)
{
    int ret = 0;

    loongsonRecPtr pLS = loongsonPTR(pScrn);

#ifdef XF86_PDEV_SERVER_FD
    if (pLS->pEnt->location.type == BUS_PLATFORM &&
        (pLS->pEnt->location.id.plat->flags & XF86_PDEV_SERVER_FD))
    {
        return TRUE;
    }
#endif

    ret = drmSetMaster(pLS->drmFD);
    if (ret)
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "drmSetMaster failed: %s\n",
                   strerror(errno));
    }

    return ret == 0;
}


static Bool LS_OpenDRMReal(ScrnInfoPtr pScrn)
{
    int cached_fd = -1;

    loongsonRecPtr pLs = loongsonPTR(pScrn);

    EntityInfoPtr pEnt = pLs->pEnt;

    int err;

    cached_fd = LS_EntityGetCachedFd(pScrn);
    if ( cached_fd != 0)
    {
        pLs->drmFD = cached_fd;
        LS_EntityIncreaseFdReference(pScrn);

        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "Reusing fd %d for second head\n", cached_fd);

        return TRUE;
    }


#ifdef XSERVER_PLATFORM_BUS
    if (pEnt->location.type == BUS_PLATFORM)
    {
#ifdef XF86_PDEV_SERVER_FD
        if (pEnt->location.id.plat->flags & XF86_PDEV_SERVER_FD)
        {
            cached_fd =
                xf86_platform_device_odev_attributes(pEnt->location.id.plat)->fd;
            // suijingfeng : server manage fd is not working on our platform
            // now. we don't know what's the reason and how to enable that.
            xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                    "Get the fd from server managed fd.\n");
        }
        else
#endif
        {
            char *path = xf86_platform_device_odev_attributes(
                        pEnt->location.id.plat)->path;
            if (NULL != path)
            {
                xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                        "path = %s, got from PLATFORM.\n", path);
            }

            cached_fd = LS_OpenHW(path);
        }
    }
    else
#endif
#ifdef XSERVER_LIBPCIACCESS
    if (pEnt->location.type == BUS_PCI)
    {
        char *BusID = NULL;
        struct pci_device *PciInfo;
        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "BUS: PCI\n");

        PciInfo = xf86GetPciInfoForEntity(pEnt->index);
        if (PciInfo)
        {
            if ((BusID = LS_DRICreatePCIBusID(PciInfo)) != NULL)
            {
                cached_fd = drmOpen(NULL, BusID);

                xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                        "BusID = %s, got from pci bus.\n", BusID);

                free(BusID);
            }
        }
    }
    else
#endif
    {
        const char *kDrvName;

        // get Kernel driver name from conf,
        // if null, we just open loongson-drm
        kDrvName = xf86FindOptionValue(pEnt->device->options, "KernelDriverName");

        if( kDrvName == NULL )
        {
            kDrvName = "loongson-drm";
        }

        cached_fd = LS_OpenDRM( kDrvName );
    }

    if (cached_fd < 0)
        return FALSE;

    LS_ShowDriverInfo(cached_fd);

    LS_EntityInitFd(pScrn, cached_fd);

    pLs->drmFD = cached_fd;

    return TRUE;
}



static ModeStatus ValidMode(ScrnInfoPtr arg, DisplayModePtr mode,
        Bool verbose, int flags)
{
    TRACE_ENTER();
    TRACE_EXIT();
    return MODE_OK;
}


void LS_SetupScrnHooks(ScrnInfoPtr pScrn, Bool (* pFnProbe)(DriverPtr, int))
{
    /* Apparently not used by X server */
    pScrn->driverVersion = 1;
    /* Driver name as used in config file */
    pScrn->driverName    = LOONGSON_DRIVER_NAME;
    /* log prefix */
    pScrn->name          = "ls7a";

    pScrn->Probe         = pFnProbe;
    pScrn->PreInit       = PreInit;
    pScrn->ScreenInit    = ScreenInit;
    pScrn->SwitchMode    = SwitchMode;
    pScrn->AdjustFrame   = AdjustFrame;
    pScrn->EnterVT       = EnterVT;
    pScrn->LeaveVT       = LeaveVT;
    pScrn->FreeScreen    = FreeScreen;
    pScrn->ValidMode     = ValidMode;
}


static Bool LS_AllocDriverPrivate(ScrnInfoPtr pScrn)
{
    // Allocate the driver's Screen-specific, "private"
    // data structure and hook it into the ScrnInfoRec's
    // driverPrivate field.
    TRACE_ENTER();

    if (NULL == pScrn->driverPrivate)
    {
        pScrn->driverPrivate = xnfcalloc(1, sizeof(loongsonRec));

        if (NULL == pScrn->driverPrivate)
        {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "GetRec: Failed allocate for driver private.\n");
            return FALSE;
        }
        /* Allocate driverPrivate */
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "Allocate for driver private.\n");
    }

    TRACE_EXIT();

    return TRUE;
}



/* This is called by PreInit to set up the default visual */
static Bool InitDefaultVisual(ScrnInfoPtr pScrn)
{
    loongsonRecPtr pLS = loongsonPTR(pScrn);

    int defaultdepth, defaultbpp;

    int bppflags = PreferConvert24to32 | SupportConvert24to32 | Support32bppFb;


    TRACE_ENTER();


    /* Get the current color depth & bpp, and set it for XFree86: */
    drmmode_get_default_bpp(pScrn, pLS->drmFD, &defaultdepth, &defaultbpp);


    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                "defaultdepth: %d, defaultbpp: %d.\n",
                defaultdepth, defaultbpp);

    if ( (defaultdepth == 24) && (defaultbpp == 24) )
    {
        pLS->drmmode.force_24_32 = TRUE;
        pLS->drmmode.kbpp = 24;
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                "Using 24bpp hw front buffer with 32bpp shadow\n");
        defaultbpp = 32;
    }
    else
    {
        pLS->drmmode.kbpp = 0;
    }

    // 0 is not used, dont warry
    if (!xf86SetDepthBpp(pScrn, defaultdepth, 0, defaultbpp, bppflags))
    {
        /* The above function prints an error message. */
        return FALSE;
    }


    xf86PrintDepthBpp(pScrn);

    switch (pScrn->depth)
    {
        case 15:
        case 16:
        case 24:
        case 30:
            break;
        default:
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Given depth (%d) is not supported by the driver\n",
                   pScrn->depth);
            return FALSE;
    }


    {
        rgb defaultMask = { 0, 0, 0 };
        rgb defaultWeight = { 0, 0, 0 };
        /* Set the color weight: */
        if (!xf86SetWeight(pScrn, defaultWeight, defaultMask))
        {
            /* The above function prints an error message. */
            return FALSE;
        }
    }


    if (!xf86SetDefaultVisual(pScrn, -1))
    {
        return FALSE;
    }


    if (0 == pLS->drmmode.kbpp)
    {
        pLS->drmmode.kbpp = pScrn->bitsPerPixel;
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "drmmode.kbpp = %d\n", pLS->drmmode.kbpp);
    }

    TRACE_EXIT();

    return TRUE;
}


/**
 * Additional hardware probing is allowed now, including display configuration.
 *
 * This is done at the start of the first server generation only.
 *
 * For each ScrnInfoRec, enable access to the screens entities and call the
 * ChipPreInit() function.
 * The purpose of this function is to find out all the information required
 * to determine if the configuration is usable, and to initialise those parts
 * of the ScrnInfoRec that can be set once at the beginning of the first server
 * generation.
 *
 * The number of entities registered for the screen should be checked against
 * the expected number (most drivers expect only one). The entity information
 * for each of them should be retrieved (with xf86GetEntityInfo()) and checked
 * for the correct bus type and that none of the sharable resources registered
 * during the Probe phase was rejected.
 *
 * Access to resources for the entities that can be controlled in a
 * device-independent way are enabled before this function is called.
 *
 * If the driver needs to access any resources that it has disabled in an
 * EntityInit() function that it registered, then it may enable them here
 * providing that it disables them before this function returns.
 *
 * This includes probing for video memory, clocks, ramdac, and all other HW
 * info that is needed. It includes determining the depth/bpp/visual and
 * related info. It includes validating and determining the set of video
 * modes that will be used (and anything that is required to determine that).
 *
 * This information should be determined in the least intrusive way possible.
 * The state of the HW must remain unchanged by this function. Although video
 * memory (including MMIO) may be mapped within this function, it must be
 * unmapped before returning.
 *
 * The bulk of the ScrnInfoRec fields should be filled out in this function.
 */
static Bool PreInit(ScrnInfoPtr pScrn, int flags)
{
    loongsonRecPtr pLS;

    int connector_count;

    TRACE_ENTER();

    /* Check the number of entities, and fail if it isn't one. */
    if (pScrn->numEntities != 1)
    {
        // suijingfeng: print this to see when could this happen
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "PreInit: pScrn->numEntities = %d \n", pScrn->numEntities );
        return FALSE;
    }


    if (flags & PROBE_DETECT)
    {
        // support the \"-configure\" or \"-probe\" command line arguments.
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
            "does not support \"-configure\" or \"-probe\" command line arguments.");
        return FALSE;
    }


    //  Create DRM device instance:
    //
    // Driver specific information should be stored in a structure hooked into
    // the ScrnInfoRec's driverPrivate field. Any other modules which require
    // persistent data (ie data that persists across server generations) should
    // be initialised in this function, and they should allocate a “privates”
    // index to hook their data into by calling xf86AllocateScrnInfoPrivateIndex().
    //
    // The “privates” data is persistent.
    //
    if ( FALSE == LS_AllocDriverPrivate(pScrn) )
    {
        return FALSE;
    }

    pLS = loongsonPTR(pScrn);

    //
    // Pointer to the entity structure for this screen.
    //
    // xf86GetEntityInfo() -- This function hands information from the
    // EntityRec struct to the drivers. The EntityRec structure itself
    // remains invisible to the driver.
    //
    pLS->pEnt = xf86GetEntityInfo(pScrn->entityList[0]);

    pLS->drmmode.is_secondary = FALSE;
    /* This is actually  memory pitch */
    pScrn->displayWidth = 640;  /* default it */

    if (xf86IsEntityShared(pScrn->entityList[0]))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "PreInit: Entity is shared.\n");
        if (xf86IsPrimInitDone(pScrn->entityList[0]))
        {
            pLS->drmmode.is_secondary = TRUE;
            xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "PreInit: Primary init is done.\n");
        }
        else
        {
            xf86SetPrimInitDone(pScrn->entityList[0]);

            xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "PreInit: Primary init is NOT done, set it.\n");
        }
    }

    pScrn->monitor = pScrn->confScreen->monitor;
    // Using a programmable clock:
    pScrn->progClock = TRUE;
    pScrn->rgbBits = 8;


    // Open a DRM, so we can communicate with the KMS code
    if (FALSE == LS_OpenDRMReal(pScrn))
    {
        return FALSE;
    }


    if (!LS_CheckOutputs(pLS->drmFD, &connector_count))
    {
        return FALSE;
    }

    // initially mark to use all DRM crtcs
    pLS->crtcNum = -1;

    // pLS->drmmode.fd = pLS->drmFD;

    InitDefaultVisual(pScrn);

    LS_ProcessOptions(pScrn, &pLS->pOptionInfo);

    LS_GetCursorDimK(pScrn);

    /* Must match name used in the kernel driver */
    pLS->driver_name = "loongson-drm";


    LS_PrepareDebug(pScrn);

    /* Determine if user wants to disable buffer flipping:
     * Boolean value indicating whether DRM page flip events should
     * be requested and waited for during DRM_IOCTL_MODE_PAGE_FLIP.
     */
    pLS->drmmode.pageflip =
        xf86ReturnOptValBool(pLS->pOptionInfo, OPTION_PAGEFLIP, FALSE);

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Buffer Flipping is %s\n",
        pLS->drmmode.pageflip ? "Enabled" : "Disabled");


    pLS->SoftExa = xf86ReturnOptValBool(pLS->pOptionInfo, OPTION_SOFT_EXA, FALSE);
    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
            "Hardware EXA is %s\n", pLS->SoftExa ? "Disabled" : "Enabled");

    {
        int ret;
        uint64_t value;

        pScrn->capabilities = 0;

        ret = drmGetCap(pLS->drmFD, DRM_CAP_PRIME, &value);
        if (ret == 0)
        {
            if (connector_count && (value & DRM_PRIME_CAP_IMPORT))
            {
                xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                        "DRM PRIME IMPORT support.\n");
                pScrn->capabilities |= RR_Capability_SinkOutput;
            }
        }
    }

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Setting the video modes ...\n");

    /* Don't call drmCheckModesettingSupported() as its written only for PCI devices.
    */
    // this is prepare for drmmode_pre_init
    pLS->drmmode.fd = pLS->drmFD;

    if ( FALSE == drmmode_pre_init(pScrn, &pLS->drmmode, (pScrn->bitsPerPixel >> 3)) )
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "KMS setup failed.\n");
        return FALSE;
    }
    else
    {
        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Initial KMS successful.\n");
    }

    /* Ensure we have a supported bitsPerPixel: */
    switch (pScrn->bitsPerPixel)
    {
        case 16:
        case 24:
        case 32:
            break;
        default:
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                    "Requested bits per pixel (%d) is unsupported.\n",
                    pScrn->bitsPerPixel);
            return FALSE;
    }


    {
        Gamma defaultGamma = { 0.0, 0.0, 0.0 };
        /* Set the gamma: */
        if (!xf86SetGamma(pScrn, defaultGamma))
        {
            /* The above function prints an error message. */
            goto fail;
        }
    }

    if (!(pScrn->is_gpu && connector_count == 0) && (pScrn->modes == NULL))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No modes.\n");
        return FALSE;
    }


    pScrn->currentMode = pScrn->modes;

    /* Let XFree86 calculate or get (from command line) the display DPI: */
    xf86SetDpi(pScrn, 0, 0);


    // Modules may be loaded at any point in this function, and all modules
    // that the driver will need must be loaded before the end of this function.
    // Either the xf86LoadSubModule() or the xf86LoadDrvSubModule() function
    // should be used to load modules depending on whether a ScrnInfoRec has
    // been set up.
    //
    // A driver may unload a module within this function if it was only
    // needed temporarily, and the xf86UnloadSubModule() function should be
    // used to do that. Otherwise there is no need to explicitly unload modules
    // because the loader takes care of module dependencies and will unload
    // submodules automatically if/when the driver module is unloaded.

    /* Get ScreenInit function */
    if (!xf86LoadSubModule(pScrn, "fb"))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                "Loading dr2 submodule failed.\n");
        return FALSE;
    }

    /* Load external sub-modules now: */
    if (!xf86LoadSubModule(pScrn, "dri2") )
    {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                "Loading dr2 submodule failed.\n");
        return FALSE;
    }

    if (!xf86LoadSubModule(pScrn, "dri3"))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                "Loading dr3 submodule failed.\n");
        return FALSE;
    }

    // suijingfeng
    if (!xf86LoadSubModule(pScrn, "exa"))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                "Loading exa submodule failed.\n");
        return FALSE;
    }

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "PreInit Successed.\n");

    TRACE_EXIT();

    return TRUE;


fail:
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "PreInit failed...\n");

    // PreInit() returns FALSE when the configuration is unusable
    // in some way (unsupported depth, no valid modes, not enough
    // video memory, etc), and TRUE if it is usable.
    //
    // It is expected that if the ChipPreInit() function returns TRUE,
    // then the only reasons that subsequent stages in the driver
    // might fail are lack or resources (like xalloc failures).
    //
    // All other possible reasons for failure should be determined
    // by the ChipPreInit() function.
    return FALSE;
}


// At this point it is known which screens will be in use,
// and which drivers are being used. Unreferenced drivers
// (and modules they may have loaded) are unloaded here.

/**
 * Initialize EXA and DRI2
 */
static void LS7A_AccelInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    struct ARMSOCRec *pLs = ARMSOCPTR(pScrn);

    TRACE_ENTER();

    if ((0 == pLs->SoftExa) && (NULL == pLs->pARMSOCEXA))
    {
        pLs->pARMSOCEXA = InitViv2DEXA(pScreen, pScrn, pLs->drmFD);
    }

    // Fall back to soft EXA
    if ( (1 == pLs->SoftExa) && (NULL == pLs->pARMSOCEXA) )
    {
        pLs->pARMSOCEXA = LS_InitSoftwareEXA(pScreen, pScrn, pLs->drmFD);
    }

    if (pLs->pARMSOCEXA)
    {
        pLs->dri2 = ARMSOCDRI2ScreenInit(pScreen); // DRI2
        pLs->dri3 = LS_DRI3ScreenInit(pScreen); // DRI3
        LS_PresentScreenInit(pScreen); // Present
        ARMSOCVideoScreenInit(pScreen); // XV
    }
    else
    {
        pLs->dri2 = FALSE;
        pLs->dri3 = FALSE;
    }

    TRACE_EXIT();
}


Bool drmmode_setup_colormap(ScreenPtr pScreen, ScrnInfoPtr pScrn)
{
    TRACE_ENTER();
    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, 0,
                   "Initializing kms color map for depth %d, %d bpc.\n",
                   pScrn->depth, pScrn->rgbBits);
    if (!miCreateDefColormap(pScreen))
    {
        TRACE_EXIT();
        return FALSE;
    }

    /* Adapt color map size and depth to color depth of screen. */
    if (!xf86HandleColormaps(pScreen, 1 << pScrn->rgbBits, 10,
                             drmmode_load_palette, NULL,
                             CMAP_PALETTED_TRUECOLOR | CMAP_RELOAD_ON_MODE_SWITCH))
    {
        TRACE_EXIT();
        return FALSE;
    }

    TRACE_EXIT();

    return TRUE;
}

/*
	if (!xf86HandleColormaps(pScreen, 1 << pScrn->rgbBits, pScrn->rgbBits,
		LS_LoadPalette, NULL, CMAP_PALETTED_TRUECOLOR))
	{
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "xf86HandleColormaps() failed!\n");
		goto fail8;
	}
*/

/* create front and cursor BOs */
Bool drmmode_create_initial_bos(ScrnInfoPtr pScrn, drmmode_ptr drmmode)
{
    // ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    loongsonRecPtr pLs = loongsonPTR(pScrn);
    int bpp = pLs->drmmode.kbpp;
    int cpp = (bpp + 7) / 8;

    TRACE_ENTER();

/*
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    int width;
    int height;
    int bpp = ms->drmmode.kbpp;
    int i;
    int cpp = (bpp + 7) / 8;

    width = pScrn->virtualX;
    height = pScrn->virtualY;

    if (!drmmode_create_bo(drmmode, &drmmode->front_bo, width, height, bpp))
        return FALSE;
    pScrn->displayWidth = drmmode_bo_get_pitch(&drmmode->front_bo) / cpp;

    width = ms->cursor_width;
    height = ms->cursor_height;
    bpp = 32;
    for (i = 0; i < xf86_config->num_crtc; i++) {
        xf86CrtcPtr crtc = xf86_config->crtc[i];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

        drmmode_crtc->cursor_bo =
            dumb_bo_create(drmmode->fd, width, height, bpp);
    }
*/

    /* We create a single visual with the depth set to the
     * screen's bpp as otherwise XComposite will add an alternate
     * visual and ARGB8888 windows will be implicitly redirected.
     * The initial scanout buffer is created with the same depth
     * to match the visual.
     */

    /* Allocate initial scanout buffer.*/
    DEBUG_MSG("allocating new scanout buffer: %dx%d %d %d",
            pScrn->virtualX, pScrn->virtualY, pScrn->depth, pScrn->bitsPerPixel);

    /* Screen creates and takes a ref on the scanout bo */

    pLs->scanout = armsoc_bo_new_with_dim(pLs->drmFD,
            pScrn->virtualX, pScrn->virtualY,
            pScrn->depth, pScrn->bitsPerPixel);

    if (!pLs->scanout)
    {
        ERROR_MSG("Cannot allocate scanout buffer\n");
        return FALSE;
    }

    dumb_bo_map(pLs->drmFD, pLs->scanout);


    /* memory pitch */
    // pScrn->displayWidth = drmmode_bo_get_pitch(&drmmode->front_bo) / cpp;
    pScrn->displayWidth = armsoc_bo_pitch(pLs->scanout) / cpp;


    DEBUG_MSG("calculated displayWidth: %d", pScrn->displayWidth);


    DEBUG_MSG("calculated pitch: %d", armsoc_bo_pitch(pLs->scanout) );

    TRACE_EXIT();

    return TRUE;
}

/**
 * The driver's ScreenInit() function, called at the start of each server
 * generation. Fill in pScreen, map the frame buffer, save state,
 * initialize the mode, etc.
 *
 * AddScreen (ScreenInit)
 *
 * At this point, the valid screens are known. AddScreen() is called for
 * each of them, passing ScreenInit() as the argument. AddScreen() is a
 * DIX function that allocates a new screenInfo.screen[] entry (aka pScreen),
 * and does some basic initialisation of it.
 *
 * It then calls the ChipScreenInit() function, with pScreen as one of
 * its arguments. If ChipScreenInit() returns FALSE, AddScreen() returns
 * -1. Otherwise it returns the index of the screen.
 *
 * AddScreen() should only fail because of programming errors or failure
 * to allocate resources (like memory).
 *
 * All configuration problems should be detected BEFORE this point.
 *
 */
static Bool ScreenInit(ScreenPtr pScreen, int argc, char **argv)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    struct ARMSOCRec *pLs = ARMSOCPTR(pScrn);
    struct ARMSOCRec * ms = pLs;
    VisualPtr visual;

    int j;

    TRACE_ENTER();

    pScrn->pScreen = pScreen;

    // vtSema field should be set to TRUE just prior to changing the
    // video hardware's state.
    pScrn->vtSema = TRUE;
    /* set drm master before allocating scanout buffer */
    if ( !SetMaster(pScrn) )
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                "Cannot get DRM master: %s", strerror(errno));
        goto fail;
    }


    /* HW dependent - FIXME */
    // suijingfeng : drmmode_create_initial_bos will set
    // pScrn->displayWidth,
    //
    // suijingfeng: is this necessary ? YES !
    //

    pScrn->displayWidth = pScrn->virtualX;
    DEBUG_MSG("pScrn->displayWidth: %d", pScrn->displayWidth);


    if (!drmmode_create_initial_bos(pScrn, &pLs->drmmode))
    {
        return FALSE;
    }


    {
        xf86CrtcConfigPtr xf86_config;
        /* need to point to new screen on server regeneration */
        xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
        for (j = 0; j < xf86_config->num_crtc; j++)
            xf86_config->crtc[j]->scrn = pScrn;
        for (j = 0; j < xf86_config->num_output; j++)
            xf86_config->output[j]->scrn = pScrn;
    }


    /*
     * The next step is to setup the screen's visuals, and initialize the
     * framebuffer code.  In cases where the framebuffer's default
     * choices for things like visual layouts and bits per RGB are OK,
     * this may be as simple as calling the framebuffer's ScreenInit()
     * function.  If not, the visuals will need to be setup before calling
     * a fb ScreenInit() function and fixed up after.
     *
     * For most PC hardware at depths >= 8, the defaults that fb uses
     * are not appropriate.  In this driver, we fixup the visuals after.
     */

    /* Reset the visual list. */
    miClearVisualTypes();

    if (!miSetVisualTypes(pScrn->depth,
                    miGetDefaultVisualMask(pScrn->depth),
                    pScrn->rgbBits, pScrn->defaultVisual))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                "Cannot initialize the visual type for %d depth, %d bits per pixel!\n",
                pScrn->depth, pScrn->bitsPerPixel);
        goto fail2;
    }

    if (!miSetPixmapDepths())
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                "Cannot initialize the pixmap depth!\n");
        goto fail3;
    }

    pScrn->memPhysBase = 0;
    pScrn->fbOffset = 0;


    /* Initialize some generic 2D drawing functions: */
    if (!fbScreenInit(pScreen, pLs->scanout->ptr,
                pScrn->virtualX, pScrn->virtualY,
                pScrn->xDpi, pScrn->yDpi,
                pScrn->displayWidth, pScrn->bitsPerPixel))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "fbScreenInit() failed!");
        goto fail3;
    }


    if (pScrn->bitsPerPixel > 8)
    {
        /* Fixup RGB ordering: */
        visual = pScreen->visuals + pScreen->numVisuals;
        while (--visual >= pScreen->visuals)
        {
            if ((visual->class | DynamicClass) == DirectColor)
            {
                visual->offsetRed = pScrn->offset.red;
                visual->offsetGreen = pScrn->offset.green;
                visual->offsetBlue = pScrn->offset.blue;
                visual->redMask = pScrn->mask.red;
                visual->greenMask = pScrn->mask.green;
                visual->blueMask = pScrn->mask.blue;
                visual->bitsPerRGBValue = pScrn->rgbBits;

                visual->ColormapEntries = 1 << pScrn->rgbBits;
            }
        }
    }

    /* Continue initializing the generic 2D drawing functions after
     * fixing the RGB ordering:
     */
    if (!fbPictureInit(pScreen, NULL, 0))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "fbPictureInit() failed!");
        goto fail4;
    }


//    pLs->SavedCreateScreenResources = pScreen->CreateScreenResources;
//    pScreen->CreateScreenResources = CreateScreenResources;

    /* Set the initial black & white colormap indices: */
    xf86SetBlackWhitePixels(pScreen);
    /* Initialize backing store: */
    xf86SetBackingStore(pScreen);
    /* Enable cursor position updates by mouse signal handler: */
    xf86SetSilkenMouse(pScreen);


    /* Initialize external sub-modules for EXA now, this has to be before
     * miDCInitialize() otherwise stacking order for wrapped ScreenPtr fxns
     * ends up in the wrong order.
     */

    /* Initialize the cursor: */
    if (!miDCInitialize(pScreen, xf86GetPointerScreenFuncs()))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
            "miDCInitialize() failed!");
        goto fail5;
    }

    /* Must force it before EnterVT, so we are in control of VT and
     * later memory should be bound when allocating, e.g rotate_mem */
    pScrn->vtSema = TRUE;


    LS7A_AccelInit(pScreen);


    // suijingfeng: why those are necessary ?
    pScreen->SaveScreen = xf86SaveScreen;
    ms->SavedCloseScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = CloseScreen;

    ms->SavedBlockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = lsBlockHandler;


    pLs->SavedCreateScreenResources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = CreateScreenResources;

    drmmode_uevent_init(pScrn, &pLs->drmmode);
    drmmode_init_wakeup_handler(pLs);

    /* Do some XRandR initialization. */
    if(!xf86CrtcScreenInit(pScreen))
    {
        return FALSE;
    }

    if (!drmmode_setup_colormap(pScreen, pScrn))
    {
        return FALSE;
    }
//    if (ms->atomic_modeset)
//        xf86DPMSInit(pScreen, drmmode_set_dpms, 0);
//    else
    /* Setup power management: */
    xf86DPMSInit(pScreen, xf86DPMSSet, 0);

    ////
    if (serverGeneration == 1)
    {
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                "serverGeneration = 1!\n");

        xf86ShowUnusedOptions(pScrn->scrnIndex, pScrn->options);
    }

    // Take over the virtual terminal from the console, set the desired mode, etc.
    if (!EnterVT(pScrn))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "EnterVT() failed!\n");
        goto fail6;
    }


    pScrn->vtSema = TRUE;

    TRACE_EXIT();

    return TRUE;

	/* cleanup on failures */
fail8:
	/* uninstall the default colormap */
	miUninstallColormap(GetInstalledmiColormap(pScreen));

fail7:
	LeaveVT(pScrn);
	pScrn->vtSema = FALSE;

fail6:
	drmmode_cursor_fini(pScreen);

fail5:
	if (pLs->dri2)
		ARMSOCDRI2CloseScreen(pScreen);

	if (pLs->pARMSOCEXA)
		if (pLs->pARMSOCEXA->CloseScreen)
			pLs->pARMSOCEXA->CloseScreen(pScreen);
fail4:
	/* Call the CloseScreen functions for fbInitScreen,  miDCInitialize,
	 * exaDriverInit & xf86CrtcScreenInit as appropriate via their
	 * wrapped pointers.
	 * exaDDXCloseScreen uses the XF86SCRNINFO macro so we must
	 * set up the key for this before it gets called.
	 */
	dixSetPrivate(&pScreen->devPrivates, xf86ScreenKey, pScrn);
	(*pScreen->CloseScreen)(pScreen);

fail3:
	/* reset the visual list */
	miClearVisualTypes();

fail2:
    /* Screen drops its ref on scanout bo on failure exit */
    armsoc_bo_unreference(pLs->scanout);
    pLs->scanout = NULL;
    pScrn->displayWidth = 0;

fail1:
    /* drop drm master */
    (void)drmDropMaster(pLs->drmFD);

fail:
    TRACE_EXIT();
    return FALSE;
}


static void LS_LoadPalette(ScrnInfoPtr pScrn, int num, int *indices,
	LOCO *colors, VisualPtr pVisual)
{
	TRACE_ENTER();

	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	uint16_t lut_r[256], lut_g[256], lut_b[256];
	int i, p;

	for (i = 0; i < num; ++i)
	{
		int index = indices[i];
		lut_r[index] = colors[index].red << 8;
		lut_g[index] = colors[index].green << 8;
		lut_b[index] = colors[index].blue << 8;
	}

	for (p = 0; p < xf86_config->num_crtc; ++p)
	{
		xf86CrtcPtr crtc = xf86_config->crtc[p];

		/* Make the change through RandR */
		if (crtc->randr_crtc)
			RRCrtcGammaSet(crtc->randr_crtc, lut_r, lut_g, lut_b);
		else
			crtc->funcs->gamma_set(crtc, lut_r, lut_g, lut_b, 256);
	}
	TRACE_EXIT();
}


static void drmmode_load_palette(ScrnInfoPtr pScrn, int numColors,
                     int *indices, LOCO * colors, VisualPtr pVisual)
{
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    uint16_t lut_r[256], lut_g[256], lut_b[256];
    int index, j, i;
    int c;

    for (c = 0; c < xf86_config->num_crtc; c++) {
        xf86CrtcPtr crtc = xf86_config->crtc[c];
        struct drmmode_crtc_private_rec * drmmode_crtc = crtc->driver_private;

        for (i = 0; i < 256; i++) {
            lut_r[i] = drmmode_crtc->lut_r[i] << 6;
            lut_g[i] = drmmode_crtc->lut_g[i] << 6;
            lut_b[i] = drmmode_crtc->lut_b[i] << 6;
        }

        switch (pScrn->depth) {
        case 15:
            for (i = 0; i < numColors; i++) {
                index = indices[i];
                for (j = 0; j < 8; j++) {
                    lut_r[index * 8 + j] = colors[index].red << 6;
                    lut_g[index * 8 + j] = colors[index].green << 6;
                    lut_b[index * 8 + j] = colors[index].blue << 6;
                }
            }
            break;
        case 16:
            for (i = 0; i < numColors; i++) {
                index = indices[i];

                if (i <= 31) {
                    for (j = 0; j < 8; j++) {
                        lut_r[index * 8 + j] = colors[index].red << 6;
                        lut_b[index * 8 + j] = colors[index].blue << 6;
                    }
                }

                for (j = 0; j < 4; j++) {
                    lut_g[index * 4 + j] = colors[index].green << 6;
                }
            }
            break;
        default:
            for (i = 0; i < numColors; i++) {
                index = indices[i];
                lut_r[index] = colors[index].red << 6;
                lut_g[index] = colors[index].green << 6;
                lut_b[index] = colors[index].blue << 6;
            }
            break;
        }

        /* Make the change through RandR */
        if (crtc->randr_crtc)
            RRCrtcGammaSet(crtc->randr_crtc, lut_r, lut_g, lut_b);
        else
            crtc->funcs->gamma_set(crtc, lut_r, lut_g, lut_b, 256);
    }
}

/**
 * The driver's CloseScreen() function.  This is called at the end of each
 * server generation.  Restore state, unmap the frame buffer (and any other
 * mapped memory regions), and free per-Screen data structures (except those
 * held by pScrn).
 */
static Bool CloseScreen(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCRec *pLs = loongsonPTR(pScrn);
	Bool ret;

    TRACE_ENTER();

    drmmode_uevent_fini(pScrn);
    drmmode_fini_wakeup_handler(pLs);

    drmmode_cursor_fini(pScreen);

    /*
     * pScreen->devPrivate holds the root pixmap created around our bo
     * by miCreateResources which is installed by fbScreenInit() when
     * called from ScreenInit().
     *
     * This pixmap should be destroyed in miScreenClose() but this isn't
     * wrapped by fbScreenInit() so to prevent a leak we do it here, 
     * before calling the CloseScreen chain which would just free 
     * pScreen->devPrivate in fbCloseScreen()
     */
	if (pScreen->devPrivate)
    {
		// NOT: this make X segfault
		// Xorg: ../../../../include/privates.h:122: dixGetPrivateAddr: Assertion `key->initialized' failed.
//		(void) (*pScreen->DestroyPixmap)(pScreen->devPrivate);
//		pScreen->devPrivate = NULL;
	}


	// (driNumBufsunwrap(pLs, pScreen, CloseScreen);
	// unwrap(pLs, pScreen, BlockHandler);
	// unwrap(pLs, pScreen, CreateScreenResources);

    pScreen->BlockHandler = pLs->SavedBlockHandler;
    pScreen->CreateScreenResources = pLs->SavedCreateScreenResources;



	if (pLs->dri2)
		ARMSOCDRI2CloseScreen(pScreen);

	if (pLs->pARMSOCEXA)
		if (pLs->pARMSOCEXA->CloseScreen)
			pLs->pARMSOCEXA->CloseScreen(pScreen);

	assert(pLs->scanout);
	/* Screen drops its ref on the scanout buffer */
	armsoc_bo_unreference(pLs->scanout);
	pLs->scanout = NULL;

	pScrn->displayWidth = 0;

	if (pScrn->vtSema == TRUE)
		LeaveVT(pScrn);

	pScrn->vtSema = FALSE;

	TRACE_EXIT();


    pScreen->CloseScreen = pLs->SavedCloseScreen;
    ret = (*pScreen->CloseScreen)(pScreen);

	return ret;
}

/**
 * Adjust the screen pixmap for the current location of the front buffer.
 * This is done at EnterVT when buffers are bound as long as the resources
 * have already been created, but the first EnterVT happens before
 * CreateScreenResources.
 */

static Bool CreateScreenResources(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    struct ARMSOCRec *pLs = loongsonPTR(pScrn);

    TRACE_ENTER();

    // swap(pLs, pScreen, CreateScreenResources);
    {
        void *tmp = pLs->SavedCreateScreenResources;
        pLs->SavedCreateScreenResources = pScreen->CreateScreenResources;
        pScreen->CreateScreenResources = tmp;
    }

    if (!(*pScreen->CreateScreenResources) (pScreen))
    {
        TRACE_EXIT();
        return FALSE;
    }

    // swap(pLs, pScreen, CreateScreenResources);
    {
        void *tmp = pLs->SavedCreateScreenResources;
        pLs->SavedCreateScreenResources = pScreen->CreateScreenResources;
        pScreen->CreateScreenResources = tmp;
    }

    TRACE_EXIT();
    return TRUE;
}


//
// This is a runtime function ...
// ls prefix is used to avoid name collision
static void lsBlockHandler(ScreenPtr pScreen, void *timeout)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    loongsonRecPtr pLs = loongsonPTR(pScrn);

//    TRACE_ENTER();

    if (pLs->pARMSOCEXA && pLs->pARMSOCEXA->Flush)
    {
        pLs->pARMSOCEXA->Flush(pLs->pARMSOCEXA);
    }

    // swap(pLs, pScreen, BlockHandler);
    {
        void *tmp = pLs->SavedBlockHandler;
        pLs->SavedBlockHandler = pScreen->BlockHandler;
        pScreen->BlockHandler = tmp;
    }

    (*pScreen->BlockHandler) (pScreen, timeout);

    // swap(pLs, pScreen, BlockHandler);
    {
        void *tmp = pLs->SavedBlockHandler;
        pLs->SavedBlockHandler = pScreen->BlockHandler;
        pScreen->BlockHandler = tmp;
    }

//    TRACE_EXIT();
}



/**
 * Initialize the new mode for the Screen.
 */
static Bool SwitchMode( ScrnInfoPtr pScrn, DisplayModePtr mode )
{
    return xf86SetSingleMode(pScrn, mode, RR_Rotate_0);
}



/**
 * For cases where the frame buffer is larger than the monitor
 * resolution, this function can pan around the frame buffer
 * within the "viewport" of the monitor.
 */
static void AdjustFrame(ScrnInfoPtr pScrn, int x, int y)
{
	drmmode_adjust_frame(pScrn, x, y);
}



/**
 * This is called at server startup time, and when the X server
 * takes over the virtual terminal from the console.  As such,
 * it may need to save the current (i.e. console) HW state,
 * and set the HW state as needed by the X server.
 */
static Bool EnterVT(ScrnInfoPtr pScrn)
{
    int i, ret;

    TRACE_ENTER();

    // Allow screens to be enabled/disabled individually
    pScrn->vtSema = TRUE;

    ret = SetMaster(pScrn);

    for (i = 1; i < currentMaxClients; ++i)
    {
        if (clients[i] && !clients[i]->clientGone)
            AttendClient(clients[i]);
    }

    if (!xf86SetDesiredModes(pScrn)) {
        ERROR_MSG("xf86SetDesiredModes() failed!");
        return FALSE;
    }

    TRACE_EXIT();
    return TRUE;
}



/**
 * The driver's LeaveVT() function.  This is called when the X server
 * temporarily gives up the virtual terminal to the console.  As such, it may
 * need to restore the console's HW state.
 */
static void LeaveVT(ScrnInfoPtr pScrn)
{
    int i, ret;

    TRACE_ENTER();

    struct ARMSOCRec * pLS = loongsonPTR(pScrn);

    xf86_hide_cursors(pScrn);

    pScrn->vtSema = FALSE;

    drmDropMaster(pLS->drmFD);

    TRACE_EXIT();
}

static void FreeRec(ScrnInfoPtr pScrn)
{
    struct ARMSOCRec * ls;

    if (!pScrn)
        return;

    ls = loongsonPTR(pScrn);
    if (!ls)
        return;

    TRACE_ENTER();

    if (ls->drmFD > 0) {
        int ret;
        if ( 0 == LS_EntityDecreaseFdReference(pScrn) )
        {
            if (ls->pEnt->location.type == BUS_PCI)
            {
                ret = drmClose(ls->drmFD);
            }
            else
            {

#ifdef XF86_PDEV_SERVER_FD
                if (!(ls->pEnt->location.type == BUS_PLATFORM &&
                      (ls->pEnt->location.id.plat->flags & XF86_PDEV_SERVER_FD)))
#endif

                 ret = close(ls->drmFD);
            }

            (void) ret;
        }
    }

    pScrn->driverPrivate = NULL;
    free(ls->drmmode.Options);
    free(ls);

    TRACE_EXIT();
}

/**
 * The driver's FreeScreen() function.
 * Frees the ScrnInfoRec driverPrivate field when a screen is
 * deleted by the common layer.
 * This function is not used in normal (error free) operation.
 */
static void FreeScreen(ScrnInfoPtr pScrn)
{
	struct ARMSOCRec *pLs = ARMSOCPTR(pScrn);

	TRACE_ENTER();

    if (!pLs) {
        /* This can happen if a Screen is deleted after Probe(): */
        return;
    }

    FreeRec(pScrn);

	if (pLs->pARMSOCEXA) {
		if (pLs->pARMSOCEXA->FreeScreen)
			pLs->pARMSOCEXA->FreeScreen( pScrn );
	}

	/* Free the driver's Screen-specific, "private" data structure and
	 * NULL-out the ScrnInfoRec's driverPrivate field.
	 */
	if (pScrn->driverPrivate) {
		free(pScrn->driverPrivate);
		pScrn->driverPrivate = NULL;
	}

	TRACE_EXIT();
}

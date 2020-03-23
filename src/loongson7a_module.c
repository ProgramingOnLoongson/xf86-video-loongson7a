#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <sys/fcntl.h>
#include <unistd.h>

#include <xf86.h>
#ifdef XSERVER_PLATFORM_BUS
#include "xf86platformBus.h"
#endif
#include <xf86drmMode.h>
#include <xf86drm.h>

#include "armsoc_driver.h"

#include "micmap.h"

#include "xf86cmap.h"
#include "xf86RandR12.h"
//suijingfeng
#include "xf86Priv.h"
#include "compat-api.h"

#include "drmmode_driver.h"

/* Driver name as used in config file */
#define LOONGSON7A_DRIVER_NAME		"loongson7a"
/* Apparently not used by X server */
#define LOONGSON7A_DRIVER_VERSION	1000
/**
 * A structure used by the XFree86 code when loading this driver,
 * so that it can access the Probe() function, and other functions
 * that it uses before it calls the Probe() function.  The name of
 * this structure must be the all-upper-case version of the driver
 * name.
 */

/**
 * The mandatory Identify() function.  It is run before Probe(),
 * and prints out an identifying message.
 */
static SymTabRec Chipsets[] = {
    {0, "LS7A1000"},
    {1, "LS7A2000"},
    {1, "LS2K"},
    {-1, NULL}
};

static void LS7A_Identify(int flags)
{
    xf86PrintChipsets(LOONGSON7A_DRIVER_NAME, "Device Dependent X Driver for",
            Chipsets);
}

/** Supported options, as enum values. */
enum {
    OPTION_DEBUG,
    OPTION_NO_FLIP,
    OPTION_CARD_NUM,
    OPTION_DEV_PATH,
    OPTION_BUSID,
    OPTION_DRIVERNAME,
    OPTION_DRI_NUM_BUF,
    OPTION_INIT_FROM_FBDEV,
    OPTION_SOFT_EXA,
};


/** Supported options. */
static const OptionInfoRec Options[] = {
    { OPTION_DEBUG,      "Debug",      OPTV_BOOLEAN, {0}, FALSE },
    { OPTION_NO_FLIP,    "NoFlip",     OPTV_BOOLEAN, {0}, FALSE },
    { OPTION_CARD_NUM,   "DRICard",    OPTV_INTEGER, {0}, FALSE },
    { OPTION_DEV_PATH,   "kmsdev",     OPTV_STRING, {0}, FALSE },
    { OPTION_BUSID,      "BusID",      OPTV_STRING,  {0}, FALSE },
    { OPTION_DRIVERNAME, "DriverName", OPTV_STRING,  {0}, FALSE },
    { OPTION_DRI_NUM_BUF, "DRI2MaxBuffers", OPTV_INTEGER, { -1}, FALSE },
    { OPTION_INIT_FROM_FBDEV, "InitFromFBDev", OPTV_STRING, {0}, FALSE },
    { OPTION_SOFT_EXA,   "SoftEXA",   OPTV_BOOLEAN, {0}, FALSE },
    { -1,                NULL,         OPTV_NONE,    {0}, FALSE }
};

/**
 * It returns the available driver options to the "-configure" option, 
 * so that an xorg.conf file can be built and the user can see which
 * options are available for them to use.
 */
static const OptionInfoRec * LS7A_AvailableOptions(int chipid, int busid)
{
    return Options;
}


//  Process the "xorg.conf" file options:
void ProcessXorgConfOptions(ScrnInfoPtr pScrn, struct ARMSOCRec *pARMSOC)
{
    int driNumBufs;
    xf86CollectOptions(pScrn, NULL);
    pARMSOC->pOptionInfo = malloc(sizeof(Options));

    memcpy(pARMSOC->pOptionInfo, Options, sizeof(Options));
    xf86ProcessOptions(pScrn->scrnIndex,
            pARMSOC->pEntityInfo->device->options, pARMSOC->pOptionInfo);

    /* Determine if the user wants debug messages turned on: */
    armsocDebug = xf86ReturnOptValBool(pARMSOC->pOptionInfo, OPTION_DEBUG, FALSE);

    if (!xf86GetOptValInteger(pARMSOC->pOptionInfo, OPTION_DRI_NUM_BUF,
                &driNumBufs)) {
        /* Default to double buffering */
        driNumBufs = 2;
    }

    if (driNumBufs < 2)
    {
        ERROR_MSG( "Invalid option for %s: %d. Must be greater than or equal to 2",
                xf86TokenToOptName(pARMSOC->pOptionInfo,
                    OPTION_DRI_NUM_BUF), driNumBufs);
        return ;
    }
    pARMSOC->driNumBufs = driNumBufs;
    /* Determine if user wants to disable buffer flipping: */
    pARMSOC->NoFlip = xf86ReturnOptValBool(pARMSOC->pOptionInfo,
            OPTION_NO_FLIP, FALSE);
    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
            "Buffer Flipping is %s\n", pARMSOC->NoFlip ? "Disabled" : "Enabled");

    pARMSOC->SoftExa = xf86ReturnOptValBool(pARMSOC->pOptionInfo,
            OPTION_SOFT_EXA, FALSE);
    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
            "Hardware EXA is %s\n", pARMSOC->SoftExa ? "Disabled" : "Enabled");
}

static Bool LS7A_DriverFunc(ScrnInfoPtr scrn, xorgDriverFuncOp op, void *data)
{
    xorgHWFlags *flag;

    switch (op) {
    case GET_REQUIRED_HW_INTERFACES:
        flag = (CARD32 *) data;
        (*flag) = 0;
        return TRUE;
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,15,99,902,0)
    case SUPPORTS_SERVER_FDS:
        return TRUE;
#endif
    default:
        return FALSE;
    }
}


#ifdef XSERVER_LIBPCIACCESS
static int get_passed_fd(void)
{
    if (xf86DRMMasterFd >= 0)
    {
        xf86DrvMsg(-1, X_INFO, "Using passed DRM master file descriptor %d\n", 
                xf86DRMMasterFd);
        return dup(xf86DRMMasterFd);
    }
    return -1;
}

static int LS_OpenHW(const char *dev)
{
    int fd;

    if ((fd = get_passed_fd()) != -1)
        return fd;

    if (dev)
        fd = open(dev, O_RDWR | O_CLOEXEC, 0);
    else {
        dev = getenv("KMSDEVICE");
        if ((NULL == dev) || ((fd = open(dev, O_RDWR | O_CLOEXEC, 0)) == -1)) {
            dev = "/dev/dri/card0";
            fd = open(dev, O_RDWR | O_CLOEXEC, 0);
        }
    }
    if (fd == -1)
        xf86DrvMsg(-1, X_ERROR, "open %s: %s\n", dev, strerror(errno));

    return fd;
}

static char * LS_DRICreatePCIBusID(const struct pci_device *dev)
{
    char *busID;

    if (asprintf(&busID, "pci:%04x:%02x:%02x.%d",
                 dev->domain, dev->bus, dev->dev, dev->func) == -1)
        return NULL;

    return busID;
}

static int LS_CheckOutputs(int fd, int *count)
{
    drmModeResPtr res = drmModeGetResources(fd);
    int ret;

    if (!res)
        return FALSE;

    if (count)
        *count = res->count_connectors;

    ret = res->count_connectors > 0;

    if (ret == FALSE) {
        uint64_t value = 0;
        if (drmGetCap(fd, DRM_CAP_PRIME, &value) == 0 &&
                (value & DRM_PRIME_CAP_EXPORT))
            ret = TRUE;
    }

    drmModeFreeResources(res);
    return ret;
}

static Bool probe_hw_pci(const char *dev, struct pci_device *pdev)
{
    int ret = FALSE, fd = LS_OpenHW(dev);
    char *id, *devid;
    drmSetVersion sv;

    if (fd == -1)
        return FALSE;

    sv.drm_di_major = 1;
    sv.drm_di_minor = 4;
    sv.drm_dd_major = -1;
    sv.drm_dd_minor = -1;
    if (drmSetInterfaceVersion(fd, &sv)) {
        close(fd);
        return FALSE;
    }

    id = drmGetBusid(fd);
    devid = LS_DRICreatePCIBusID(pdev);

    if (id && devid && !strcmp(id, devid))
        ret = LS_CheckOutputs(fd, NULL);

    close(fd);
    free(id);
    free(devid);
    return ret;
}



int ms_entity_index=-1;



static void ls_setup_entity(ScrnInfoPtr scrn, int entity_num)
{

    DevUnion *pPriv;

    xf86SetEntitySharable(entity_num);

    if (ms_entity_index == -1)
        ms_entity_index = xf86AllocateEntityPrivateIndex();

    pPriv = xf86GetEntityPrivate(entity_num, ms_entity_index);

    xf86SetEntityInstanceForScreen(scrn, entity_num,
    xf86GetNumEntityInstances(entity_num) - 1);

//    if (!pPriv->ptr)
//        pPriv->ptr = xnfcalloc(sizeof(struct ARMSOCConnection), 1);
}

// If a resource conflict is found between exclusive resources the driver will fail
// immediately. This is usually best done with the xf86ConfigPciEntity() helper
// function for PCI.

static Bool LS7A_PciProbe(DriverPtr driver,
        int entity_num, struct pci_device *dev, intptr_t match_data)
{
    ScrnInfoPtr scrn = NULL;

    scrn = xf86ConfigPciEntity(scrn, 0, entity_num, NULL, NULL, NULL, NULL, NULL, NULL);

    if (scrn)
    {
        const char *devpath;
        GDevPtr devSection = xf86GetDevFromEntity(scrn->entityList[0],
                scrn->entityInstanceList[0]);

        devpath = xf86FindOptionValue(devSection->options, "kmsdev");

        xf86DrvMsg(scrn->scrnIndex, X_INFO,
                "kmsdev using %s\n", devpath ? devpath : "default device");

        /*
           if (probe_hw_pci(devpath, dev))
           {
        // suijingfeng
        // LS7A_SetupScrnHooks(scrn);

        xf86DrvMsg(scrn->scrnIndex, X_CONFIG,
        "claimed PCI slot %d@%d:%d:%d\n",
        dev->bus, dev->domain, dev->dev, dev->func);

        // ls_setup_entity(scrn, entity_num);
        }
        else
        scrn = NULL;
        */
    }

    return 0;
}
#endif



#ifdef XSERVER_LIBPCIACCESS
static const struct pci_id_match LOONGSON7A_DEVICE_MATCH[] =
{
    { PCI_MATCH_ANY, PCI_MATCH_ANY, PCI_MATCH_ANY, PCI_MATCH_ANY,
     0x00030000, 0x00ff0000, 0},
    {0, 0, 0},
};
#endif



#ifdef XSERVER_PLATFORM_BUS
static Bool probe_hw(const char *dev, struct xf86_platform_device *platform_dev)
{
    int fd;

#ifdef XF86_PDEV_SERVER_FD
    if (platform_dev && (platform_dev->flags & XF86_PDEV_SERVER_FD)) {
        fd = xf86_platform_device_odev_attributes(platform_dev)->fd;
        if (fd == -1)
            return FALSE;
        return LS_CheckOutputs(fd, NULL);
    }
#endif

    fd = LS_OpenHW(dev);
    if (fd != -1) {
        int ret = LS_CheckOutputs(fd, NULL);

        close(fd);
        return ret;
    }
    return FALSE;
}

static Bool LS7A_PlatformProbe(DriverPtr driver,
                  int entity_num, int flags, struct xf86_platform_device *dev,
                  intptr_t match_data)
{
    ScrnInfoPtr scrn = NULL;
    const char *path = xf86_platform_device_odev_attributes(dev)->path;
    int scr_flags = 0;

    if (flags & PLATFORM_PROBE_GPU_SCREEN)
        scr_flags = XF86_ALLOCATE_GPU_SCREEN;

    if (probe_hw(path, dev))
    {
        scrn = xf86AllocateScreen(driver, scr_flags);
        if (xf86IsEntitySharable(entity_num))
            xf86SetEntityShared(entity_num);
        xf86AddEntityToScreen(scrn, entity_num);

        // ms_setup_scrn_hooks(scrn);

        // LS7A_SetupScrnHooks(scrn);
        xf86DrvMsg(scrn->scrnIndex, X_INFO, "platform probe, using drv %s\n", path ? path : "default device");

        // ms_setup_entity(scrn, entity_num);
    }

    return scrn != NULL;
}
#endif



/** Provide basic version information to the XFree86 code. */
static XF86ModuleVersionInfo VersRec = {
        LOONGSON7A_DRIVER_NAME,
        MODULEVENDORSTRING,
        MODINFOSTRING1,
        MODINFOSTRING2,
        XORG_VERSION_CURRENT,
        PACKAGE_VERSION_MAJOR,
        PACKAGE_VERSION_MINOR,
        PACKAGE_VERSION_PATCHLEVEL,
        ABI_CLASS_VIDEODRV,
        ABI_VIDEODRV_VERSION,
        MOD_CLASS_VIDEODRV,
        {0, 0, 0, 0}
};



/** Let the XFree86 code know the Setup() function. */
static MODULESETUPPROTO(Setup);



_X_EXPORT DriverRec Loongson7a = {
    .driverVersion = LOONGSON7A_DRIVER_VERSION,
    .driverName = LOONGSON7A_DRIVER_NAME,
    .Identify = LS7A_Identify,
    .Probe = LS7A_Probe,
    .AvailableOptions = LS7A_AvailableOptions,
    .module = NULL,
    .refCount = 0,
    .driverFunc = LS7A_DriverFunc,
#ifdef XSERVER_LIBPCIACCESS
    .supported_devices = LOONGSON7A_DEVICE_MATCH,
    .PciProbe = LS7A_PciProbe,
#endif
#ifdef XSERVER_PLATFORM_BUS
    .platformProbe = LS7A_PlatformProbe,
#endif
};


/**
 * The first function that the XFree86 code calls, after loading this .
 */
static void * Setup(pointer module, pointer opts, int *errmaj, int *errmin)
{
    static Bool setupDone = FALSE;

    /* This module should be loaded only once, but check to be sure: */
    if (!setupDone)
    {
        setupDone = TRUE;
        xf86AddDriver(&Loongson7a, module, 0);

        // The return value must be non-NULL on success
        // even though there is no TearDownProc.
        return (void *) 1;
    }
    else
    {
        if (errmaj)
            *errmaj = LDR_ONCEONLY;
        return NULL;
    }
}

/** Let the XFree86 code know about the VersRec and Setup() function. */
_X_EXPORT XF86ModuleData loongson7aModuleData = { &VersRec, Setup, NULL };

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

#include <xf86.h>

#ifdef XSERVER_PLATFORM_BUS
#include <xf86platformBus.h>
#endif

#include "loongson_driver.h"
#include "loongson_probe.h"
#include "loongson_helpers.h"
#include "loongson_entity.h"

#include "drmmode_display.h"


/*
                         Theory of Operation

The XFree86 common layer has knowledge of generic access control mechanisms
for devices on certain bus systems (currently the PCI bus) as well as of
methods to enable or disable access to the buses itself. Furthermore it
can access information on resources decoded by these devices and if
necessary modify it.


When first starting the Xserver collects all this information,
saves it for restoration, checks it for consistency, and if necessary,
corrects it. Finally it disables all resources on a generic level prior
to calling any driver function.


When the Probe() function of each driver is called the device sections
are matched against the devices found in the system. The driver may probe
devices at this stage that cannot be identified by using device independent
methods.

Access to all resources that can be controlled in a device independent way
is disabled. The Probe() function should register all non-relocatable
resources at this stage. If a resource conflict is found between exclusive
resources the driver will fail immediately.

*/

#ifdef XSERVER_LIBPCIACCESS


char * LS_DRICreatePCIBusID(const struct pci_device *dev)
{
    char *busID = NULL;

    if (asprintf(&busID, "pci:%04x:%02x:%02x.%d",
                 dev->domain, dev->bus, dev->dev, dev->func) == -1)
    {
        return NULL;
    }

    return busID;
}


static Bool probe_pci_hw(const char *dev, struct pci_device *pdev)
{
    char *id, *devid;
    int ret = FALSE;
    int fd = LS_OpenHW(dev);

    if (fd == -1)
        return FALSE;

    // Set the drm interface version to 1.4 so we get the bus id correctly.
    if ( !LS_IsMasterFd(fd) )
    {
        close(fd);
        return FALSE;
    }

    id = drmGetBusid(fd);
    devid = LS_DRICreatePCIBusID(pdev);

    if (id)
    {
        xf86Msg(X_INFO, "PCI probe: id : %s\n", id);
    }

    if (devid)
    {
        xf86Msg(X_INFO, "PCI probe: devid : %s\n", devid);
    }

    // make sure the pci device corresponds to the drm device
    // If we get asked to pci open a device with a kms path override,
    // make sure they match, otherwise this driver can steal the primary
    // device binding for a usb adaptor.
    //
    // The driver should fallback to the old probe entry point in this case.

    if (id && devid && !strcmp(id, devid))
    {
        ret = LS_CheckOutputs(fd, NULL);
    }

    close(fd);
    free(id);
    free(devid);
    return ret;
}



Bool LS_PciProbe(DriverPtr driver,
             int entity_num, struct pci_device *pci_dev,
             intptr_t match_data)
{
    ScrnInfoPtr scrn = NULL;

    scrn = xf86ConfigPciEntity(scrn, 0, entity_num, NULL,
                               NULL, NULL, NULL, NULL, NULL);

    if (scrn)
    {
        const char *devpath;
        GDevPtr devSection = xf86GetDevFromEntity(
                scrn->entityList[0], scrn->entityInstanceList[0]);
        //TODO: pci bus id, replace this BusID
        devpath = xf86FindOptionValue(devSection->options, "kmsdev");

        xf86DrvMsg(scrn->scrnIndex, X_CONFIG, "PCI probe: kmsdev=%s\n",
                (NULL != devpath) ? devpath : "NULL");

        if (probe_pci_hw(devpath, pci_dev))
        {
            // suijingfeng: why here we don't pass ls_pci_probe and
            // hook it with probe ?
            LS_SetupScrnHooks(scrn, NULL);

            xf86DrvMsg(scrn->scrnIndex, X_CONFIG,
                   "claimed PCI slot %d@%d:%d:%d\n",
                   pci_dev->bus, pci_dev->domain,
                   pci_dev->dev, pci_dev->func);

            xf86DrvMsg(scrn->scrnIndex, X_INFO,
                       "using %s\n", devpath ? devpath : "default device");

            LS_SetupEntity(scrn, entity_num);
        }
        else
        {
            scrn = NULL;
        }
    }

    return scrn != NULL;
}
#endif


/**
 * The driver's Probe() function.  This function finds all instances of
 * ARM hardware that the driver supports (from within the "xorg.conf"
 * device sections), and for instances not already claimed by another driver,
 * claim the instances, and allocate a ScrnInfoRec.  Only minimal hardware
 * probing is allowed here.
 */
Bool LS_Probe(DriverPtr drv, int flags)
{
    int i;
    GDevPtr *devSections = NULL;
    int numDevSections;
    Bool foundScreen = FALSE;

    //  Get the "xorg.conf" file device sections that match this driver,
    //  and return (error out) if there are none:

    /* For now, just bail out for PROBE_DETECT. */
    if (flags & PROBE_DETECT)
    {
        xf86Msg(X_INFO, "Probe: PROBE_DETECT.\n");
        return FALSE;
    }

    // The probe must find the active device sections that match the driver
    // by calling xf86MatchDevice().
    // If no matches are found, the function should return FALSE immediately.

    // This function takes the name of the driver and returns via driversectlist
    // a list of device sections that match the driver name. The function return
    // value is the number of matches found. If a fatal error is encountered the
    // return value is -1.
    // The caller should use xfree() to free *driversectlist when it is no longer
    // needed.
    numDevSections = xf86MatchDevice(LOONGSON_DRIVER_NAME, &devSections);
    if (numDevSections <= 0)
    {
        xf86Msg(X_WARNING, "Cant not find matched device.\n");
        return FALSE;
    }
    else
    {
        xf86Msg(X_INFO, "Probe: %d matched device (loongson) found.\n",
                numDevSections);
    }

    // Devices that cannot be identified by using device-independent methods
    // should be probed at this stage (keeping in mind that access to all
    // resources that can be disabled in a device-independent way are disabled
    // during this phase).

    for (i = 0; i < numDevSections; ++i)
    {
        int entity_num;
        int fd;
        Bool res = FALSE;
        ScrnInfoPtr pScrn = NULL;

        // Get Kernel driver name from conf,
        // if null, we just open loongson-drm
        const char * dev = xf86FindOptionValue(devSections[i]->options,
            "KernelDriverName");

        if( dev == NULL )
        {
            dev = "loongson-drm";
        }

        fd = LS_OpenDRM( dev );

        if (fd != -1)
        {
            int num_connector = 0;

            res = LS_CheckOutputs(fd, &num_connector);

            xf86Msg(X_INFO, "LS_Probe: Open %s successful, %d connector(s)\n",
                            dev, num_connector);

            drmClose(fd);
        }
        else
        {
            xf86Msg(X_ERROR, "LS_Probe(%s) failed.\n", dev);
            return FALSE;
        }

        if (res)
        {
            // TODO: figure out the difference between
            // xf86ClaimNoSlot and xf86ClaimFbSlot
            // suijingfeng: why I am need to claim this ?

            entity_num = xf86ClaimFbSlot(drv, 0, devSections[i], TRUE);
            // int entity = xf86ClaimNoSlot(drv, 0, devSections[i], TRUE);

            pScrn = xf86ConfigFbEntity(NULL, 0, entity_num, NULL, NULL, NULL, NULL);
            xf86Msg(X_INFO, "Probe: ClaimFbSlot: entity_num=%d.\n", entity_num);
            /* Allocate the ScrnInfoRec */
            // pScrn = xf86AllocateScreen(drv, 0);
        }

        if (pScrn)
        {
            foundScreen = TRUE;

            LS_SetupScrnHooks(pScrn, LS_Probe);

            LS_SetupEntity(pScrn, entity_num);
        }
    }

    xf86Msg(X_INFO, "LS_Probe() Successed.\n");

    free(devSections);

    return foundScreen;
}


#ifdef XSERVER_PLATFORM_BUS

static Bool probe_hw(struct xf86_platform_device *platform_dev)
{
    int fd;
    const char *path = xf86_get_platform_device_attrib(platform_dev, ODEV_ATTRIB_PATH);

//
// Since xf86platformBus.h is only included when XSERVER_PLATFORM_BUS is
// defined, and configure.ac only defines that on systems with udev,
// remove XF86_PDEV_SERVER_FD will breaks the build on non-udev systems
// like Solaris.
//
// XF86_PDEV_SERVER_FD is defined since 2014
// With systemd-logind support, the xserver, rather than the drivers will
// be responsible for opening/closing the fd for drm nodes.
//
// This commit adds a fd member to OdevAttributes to store the fd to pass
// it along to the driver.
//
// systemd-logind tracks devices by their chardev major + minor numbers,
// so also add OdevAttributes to store the major and minor.

#ifdef XF86_PDEV_SERVER_FD
    if (platform_dev && (platform_dev->flags & XF86_PDEV_SERVER_FD))
    {
        // suijingfeng: hre we print to make sure that
        // it is server maneged case ...
        xf86Msg(X_INFO, "XF86: SERVER MANAGED FD\n");

        fd = xf86_platform_device_odev_attributes(platform_dev)->fd;
        if (fd == -1)
        {
            // here why we dont give another try to
            // call LS_OpenHW with path ?
            xf86Msg(X_INFO, "Platform probe: get fd from platform failed.\n");

            return FALSE;
        }

        return LS_CheckOutputs(fd, NULL);
    }
#endif

    fd = LS_OpenHW(path);

    if (fd != -1)
    {
        int ret = LS_CheckOutputs(fd, NULL);
        close(fd);

        xf86Msg(X_INFO, "probe_hw: using drv %s\n",
                path ? path : "default device");

        return ret;
    }

    return FALSE;
}



Bool LS_PlatformProbe(DriverPtr driver,
        int entity_num, int flags, struct xf86_platform_device *platform_dev,
        intptr_t match_data)
{
    ScrnInfoPtr pScrn = NULL;

    int scr_flags = 0;

    if ( probe_hw(platform_dev) )
    {
        xf86Msg( X_INFO, "Platform probe: entity number: %d\n", entity_num);

        if (flags & PLATFORM_PROBE_GPU_SCREEN)
        {
            scr_flags = XF86_ALLOCATE_GPU_SCREEN;
            xf86Msg( X_INFO, "XF86_ALLOCATE_GPU_SCREEN\n");
        }

        pScrn = xf86AllocateScreen(driver, scr_flags);

        if (xf86IsEntitySharable(entity_num))
        {
            xf86SetEntityShared(entity_num);
            xf86Msg(X_INFO, "Entity %d is sharable\n", entity_num);
        }
        else
        {
            xf86Msg(X_INFO, "Entity %d is NOT sharable\n", entity_num);
        }

        xf86AddEntityToScreen(pScrn, entity_num);

        LS_SetupScrnHooks(pScrn, NULL);

        LS_SetupEntity(pScrn, entity_num);
    }
    return (NULL != pScrn);
}

#endif

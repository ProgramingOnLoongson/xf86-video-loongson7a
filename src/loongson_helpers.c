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
#include <fcntl.h>
#include <xf86drm.h>
#include "loongson_driver.h"
#include "dumb_bo.h"

// isolate it to another file, avoid misleading

static void ShowBusId(int fd)
{
    char *bus_id = NULL;

    bus_id = drmGetBusid(fd);

    if (bus_id)
    {
        xf86Msg(X_INFO, "bus_id is [%s]\n", bus_id );

        drmFreeBusid(bus_id);
    }
}


static void ShowDeviceName(int fd)
{
    char *deviceName = NULL;

    deviceName = drmGetDeviceNameFromFd(fd);

    if (deviceName)
    {
        xf86Msg(X_INFO, "DeviceName is [%s]\n", deviceName );

        drmFree(deviceName);
    }
}


static void ShowVersionInfo(int fd)
{
    drmVersionPtr version;

    version = drmGetVersion(fd);

    if (version)
    {
        xf86Msg(X_INFO, "name: [%s]\n", version->name);
        xf86Msg(X_INFO, "version: [%d.%d.%d]\n",
            version->version_major,
            version->version_minor,
            version->version_patchlevel);

        xf86Msg(X_INFO, "description: [%s]\n", version->desc);

        drmFreeVersion(version);
    }
    else
    {
        xf86Msg(X_INFO, "version is [NULL]\n");
    }
}


/*
 * Check that what we opened was a master or a master-capable FD
 * by setting the version of the interface we'll use to talk to it.
 * If successful this leaves us as master. (see DRIOpenDRMMaster() in DRI1)
 */
Bool LS_IsMasterFd(int fd)
{
    drmSetVersion sv;

    sv.drm_di_major = 1;
    sv.drm_di_minor = 4;
    sv.drm_dd_major = -1;
    sv.drm_dd_minor = -1;

    return drmSetInterfaceVersion(fd, &sv) == 0;
}

void LS_ShowDriverInfo(int fd)
{
    xf86Msg(X_INFO, " -----------------------------------------\n");
    if( LS_IsMasterFd(fd) )
    {
         xf86Msg(X_INFO, "Set the DRM interface version to 1.4.\n");
    }
    else
    {
         xf86Msg(X_INFO, "Failed Set the DRM interface version.\n");
    }

    ShowBusId(fd);
    ShowDeviceName(fd);
    ShowVersionInfo(fd);
    xf86Msg(X_INFO, " -----------------------------------------\n");
}


Bool LS_CheckOutputs(int fd, int *count)
{
    drmModeResPtr res = drmModeGetResources(fd);
    Bool ret;

    if (!res)
        return FALSE;

    if (count)
    {
        *count = res->count_connectors;
    }

    ret = res->count_connectors > 0;

    drmModeFreeResources(res);
    return ret;
}

int LS_OpenHW(const char *dev)
{
    int fd;

/*
    if ((fd = LS_GetPassedFD()) != -1)
        return fd;
*/
    if (dev)
    {
        fd = open(dev, O_RDWR | O_CLOEXEC, 0);
    }
    else
    {
        dev = getenv("KMSDEVICE");
        if ((NULL == dev) || ((fd = open(dev, O_RDWR | O_CLOEXEC, 0)) == -1))
        {
            dev = "/dev/dri/card0";
            fd = open(dev, O_RDWR | O_CLOEXEC, 0);
        }
    }

    if (fd == -1)
    {
        xf86DrvMsg(-1, X_ERROR, "open %s: %s\n", dev, strerror(errno));
    }

    return fd;
}


int LS_OpenDRM(const char *devName)
{
    int fd = -1;

    if (devName)
    {
        /* user specified bus ID or driver name - pass to drmOpen */
        xf86Msg(X_INFO, "Opening [%s]\n", devName);

        fd = drmOpen(devName, NULL);
        if (fd < 0)
        {
            xf86Msg(X_ERROR, "Cannot open a connection with the DRM - %s",
                strerror(errno));
            return -1;
        }
    }

    return fd;
}


/* ugly workaround to see if we can create 32bpp */
void drmmode_get_default_bpp(ScrnInfoPtr pScrn, int fd, int *depth, int *bpp)
{
    drmModeResPtr mode_res;
    uint64_t value;
    struct dumb_bo *bo;

    int ret;

    /* 16 is fine */
    ret = drmGetCap(fd, DRM_CAP_DUMB_PREFERRED_DEPTH, &value);
    if (!ret && (value == 16 || value == 8))
    {
        *depth = value;
        *bpp = value;
        return;
    }

    *depth = 24;
    mode_res = drmModeGetResources(fd);
    if (NULL == mode_res)
    {
        return;
    }

    // workaround kernel bug reporting 0x0 as valid mins
    // It reports these but then you can't create a 0 sized bo.
    if (mode_res->min_width == 0)
        mode_res->min_width = 1;
    if (mode_res->min_height == 0)
        mode_res->min_height = 1;

    /*create a bo */
    bo = dumb_bo_create(fd, mode_res->min_width, mode_res->min_height, 32);
    if (!bo)
    {
        *bpp = 24;
        goto out;
    }


    {
        uint32_t fb_id;
        ret = drmModeAddFB(fd, mode_res->min_width, mode_res->min_height,
                24, 32, bo->pitch, bo->handle, &fb_id);

        if (ret)
        {
            *bpp = 24;
            dumb_bo_destroy(fd, bo);
            goto out;
        }

        drmModeRmFB(fd, fb_id);
    }

    *bpp = 32;

    dumb_bo_destroy(fd, bo);
 out:
    drmModeFreeResources(mode_res);
    return;
}

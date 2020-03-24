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

#include <xf86.h>
#ifdef XSERVER_PLATFORM_BUS
#include <xf86platformBus.h>
#endif

#include "loongson_probe.h"
#include "loongson_driver.h"
#include "loongson_options.h"

// The mandatory Identify() function. It is run before Probe(),
// and prints out an identifying message.

static SymTabRec Chipsets[] =
{
    {
        .token = 0,
        .name = "LS7A1000"
    },
    {
        .token = -1,
        .name = NULL
    }
};


static void Identify(int flags)
{
    xf86PrintChipsets(LOONGSON_DRIVER_NAME, "Device Dependent X Driver for",
            Chipsets);
}


static Bool DriverFunc(ScrnInfoPtr scrn, xorgDriverFuncOp op, void *data)
{
    xorgHWFlags *flag;

    switch (op)
    {
        case GET_REQUIRED_HW_INTERFACES:
        {
            flag = (CARD32 *) data;
            *flag = 0;

            xf86Msg( X_INFO, "loongson: require hw interface.\n");
            return TRUE;
        }
        case SUPPORTS_SERVER_FDS:
        {
            xf86Msg( X_INFO, "loongson: server managed fd is NOT support.\n");
            return TRUE;
        }
        default:
            return FALSE;
    }
}


static const struct pci_id_match loongson_device_match[] =
{
    LOONGSON_DEVICE_MATCH_V1,
    LOONGSON_DEVICE_MATCH_V2,
    {0, 0, 0},
};


// Let the XFree86 code know the Setup() function.
static MODULESETUPPROTO(fnSetup);

// Provide basic version information to the XFree86 code.
static XF86ModuleVersionInfo VersRec =
{
    .modname = LOONGSON_DRIVER_NAME,
    .vendor = MODULEVENDORSTRING,
    ._modinfo1_ = MODINFOSTRING1,
    ._modinfo2_ = MODINFOSTRING2,
    .xf86version = XORG_VERSION_CURRENT,
    .majorversion = PACKAGE_VERSION_MAJOR,
    .minorversion = PACKAGE_VERSION_MINOR,
    .patchlevel = PACKAGE_VERSION_PATCHLEVEL,
    .abiclass = ABI_CLASS_VIDEODRV,
    .abiversion = ABI_VIDEODRV_VERSION,
    .moduleclass = MOD_CLASS_VIDEODRV,
    .checksum = {0, 0, 0, 0}
};


//
// suijingfeng: I_ means Interface
//
_X_EXPORT DriverRec I_Loongson7aDrv =
{
    .driverVersion = 1,
    .driverName = LOONGSON_DRIVER_NAME,
    .Identify = Identify,
    .Probe = LS_Probe,
    .AvailableOptions = LS_AvailableOptions,
    .module = NULL,
    .refCount = 0,
    .driverFunc = DriverFunc,
#ifdef XSERVER_LIBPCIACCESS
    .supported_devices = loongson_device_match,
    .PciProbe = LS_PciProbe,
#endif
#ifdef XSERVER_PLATFORM_BUS
    .platformProbe = LS_PlatformProbe,
#endif
};


static void * fnSetup(pointer module, pointer opts, int *errmaj, int *errmin)
{
    static Bool setupDone = FALSE;

    /* This module should be loaded only once, but check to be sure: */
    if (!setupDone)
    {
        setupDone = TRUE;
        xf86AddDriver(&I_Loongson7aDrv, module, HaveDriverFuncs);

        // The return value must be non-NULL on success
        // even though there is no TearDownProc.
        return (void *) 1;
    }
    else
    {
        if (errmaj)
        {
            *errmaj = LDR_ONCEONLY;
        }
        return NULL;
    }
}


_X_EXPORT XF86ModuleData loongson7aModuleData =
{
    .vers = &VersRec,
    .setup = fnSetup,
    .teardown = NULL
};

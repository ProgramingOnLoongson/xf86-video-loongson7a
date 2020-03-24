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

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#include <sys/fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86.h>


#include <misyncshm.h>

#include "loongson_driver.h"

#include "loongson_dri3.h"

#include "loongson_debug.h"

static int IsRenderNode(int fd, struct stat *st)
{
    if (fstat(fd, st))
        return 0;

    if (!S_ISCHR(st->st_mode))
        return 0;

    return st->st_rdev & 0x80;
}


static int DRI3_Open(ScreenPtr pScreen, RRProviderPtr provider, int *fdp)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    loongsonRecPtr pLs = loongsonPTR(pScrn);
    drm_magic_t magic;
    int fd;
    int ret;

    struct stat master;
    if (IsRenderNode(pLs->drmFD, &master))
    {
        return TRUE;
    }


    fd = open(pLs->deviceName, O_RDWR | O_CLOEXEC, 0);
    // why can't using the following open method
    // fd = drmOpen(pLs->deviceName, 0);
    // int drmOpenWithType(const char *name, const char *busid, int type);
    // fd = drmOpenWithType("etnaviv", 0, DRM_NODE_RENDER);

    if (fd < 0)
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                "DRI3Open: cannot open %s.\n", pLs->deviceName);
        return BadAlloc;
    }
    else
    {
        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "%s opened in %d\n",
                pLs->deviceName, fd);
    }

    /* Before FD passing in the X protocol with DRI3 (and increased security of
     * rendering with per-process address spaces on the GPU), the kernel had to
     * come up with a way to have the server decide which clients got to access
     * the GPU, which was done by each client getting a unique (magic) number
     * from the kernel, passing it to the server, and the server then telling
     * the kernel which clients were authenticated for using the device.
     *
     * Now that we have FD passing, the server can just set up the authentication
     * on its own and hand the prepared FD off to the client.
     */
    ret = drmGetMagic(fd, &magic);
    if (ret < 0)
    {
        if (errno == EACCES)
        {
            // Assume that we're on a render node, and the fd is already as
            // authenticated as it should be.
            *fdp = fd;
            return Success;
        }
        else
        {
            close(fd);
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                    "DRI3Open: cannot get magic : ret %d\n", ret);
            return BadMatch;
        }
        return FALSE;
    }

    // suijingfeng: what it the difference between fd and pLs->drmFD ?
    // why we need two fd ?
    ret = drmAuthMagic(pLs->drmFD, magic);
    if (ret < 0)
    {
        close(fd);
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                "DRI3Open: cannot auth magic: %s, ret %d\n",
                pLs->deviceName, ret);
        return BadMatch;
    }

    *fdp = fd;
    return Success;
}


Bool LS_ExaSetPixmapBo(ScreenPtr pScreen, PixmapPtr pPixmap,
                     struct dumb_bo *bo)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    struct ARMSOCPixmapPrivRec *priv = exaGetPixmapDriverPrivate(pPixmap);
    // loongsonRecPtr pLs = loongsonPTR(pScrn);

    TRACE_ENTER();

    if ( NULL == priv )
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "DRI3: no privPix.\n");
        pScreen->DestroyPixmap(pPixmap);
        TRACE_EXIT();
        return FALSE;
    }

    if (priv->bo)
    {
        // armsoc_bo_rm_fb(priv->bo);
        // dumb_bo_destroy(priv->bo->fd, priv->bo);
        armsoc_bo_unreference(priv->bo);
    }

    priv->bo = bo;

    pPixmap->devPrivate.ptr = NULL;
    pPixmap->devKind = bo->pitch;

//    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
//        "DRI3: PixmapFromFD: pixmap:%p pix:%p bo:%p %dx%d %d/%d %d->%d\n",
//        pPixmap, priv, priv->bo, width, height, depth, bpp, stride, pPixmap->devKind);

    TRACE_EXIT();

    return TRUE;
}


static PixmapPtr DRI3_PixmapFromFD(ScreenPtr pScreen, int prime_fd,
                    CARD16 width, CARD16 height, CARD16 stride, CARD8 depth, CARD8 bpp)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    loongsonRecPtr pLs = loongsonPTR(pScrn);

    PixmapPtr pixmap;

    struct dumb_bo * new_bo = NULL;

    TRACE_ENTER();

    // width, height,
    pixmap = pScreen->CreatePixmap(pScreen, width, height, depth,
                                CREATE_PIXMAP_USAGE_BACKING_PIXMAP);
    if (pixmap == NullPixmap)
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "DRI3: cannot create pixmap\n");
        TRACE_EXIT();
        return NullPixmap;
    }


    // suijingfeng: pixmap->devKind vs stride ???
    if (!pScreen->ModifyPixmapHeader(pixmap, width, height, depth, bpp, stride, NULL))
    {
        pScreen->DestroyPixmap(pixmap);
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "DRI3: ModifyPixmapHeader failed.\n");
        TRACE_EXIT();
        return NULL;
    }


    {
        // drmPrimeFDToHandle(priv->bo->fd, prime_fd, &priv->bo->handle);

        new_bo = dumb_get_bo_from_fd( pLs->drmFD,
            prime_fd, stride, stride * height);

        // stand for device node
        new_bo->fd = pLs->drmFD;
        new_bo->fb_id = 0;
        // bo is just bo, shouldn't store the following member ...
        new_bo->width = width;
        new_bo->height = height;
        new_bo->pitch = stride;

        new_bo->depth = depth;
        new_bo->bpp = bpp;
        new_bo->refcnt = 1;
    }


    LS_ExaSetPixmapBo(pScreen, pixmap, new_bo);

    TRACE_EXIT();

    return pixmap;
}


static int DRI3_FDFromPixmap(ScreenPtr pScreen, PixmapPtr pixmap,
        CARD16 *stride, CARD32 *size)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    struct ARMSOCPixmapPrivRec *priv = exaGetPixmapDriverPrivate(pixmap);
    int prime_fd;
    // suijingfeng: doesn't get called ...
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "DRI3: FDFromPixmap.\n");

    /* Only support pixmaps backed by an etnadrm bo */
    if (!priv || !priv->bo)
        return BadMatch;

    *stride = pixmap->devKind;
    *size = armsoc_bo_size(priv->bo);

    armsoc_bo_to_dmabuf(priv->bo, &prime_fd);
    return prime_fd;
}


static dri3_screen_info_rec loongson_dri3_info = {
    .version = 0,
    .open = DRI3_Open,
    .pixmap_from_fd = DRI3_PixmapFromFD,
    .fd_from_pixmap = DRI3_FDFromPixmap,
};


void PrintRenderInfo(ScreenPtr pScreen, const char * pRenderName)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);

    int fd = drmOpenWithType(pRenderName, NULL, DRM_NODE_RENDER);

    if( fd != -1 )
    {
        drmVersionPtr version = drmGetVersion(fd);
        if (version)
        {
            xf86DrvMsg(pScrn->scrnIndex, X_INFO, " ----------------------------- \n");
            xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Version: %d.%d.%d\n",
                    version->version_major, version->version_minor,
                    version->version_patchlevel);
            xf86DrvMsg(pScrn->scrnIndex, X_INFO, "  Name: %s\n", version->name);
            xf86DrvMsg(pScrn->scrnIndex, X_INFO, "  Date: %s\n", version->date);
            xf86DrvMsg(pScrn->scrnIndex, X_INFO, "  Description: %s\n", version->desc);
            xf86DrvMsg(pScrn->scrnIndex, X_INFO, " ----------------------------- \n");

            drmFreeVersion(version);
        }
        drmClose(fd);
    }
}


Bool LS_DRI3ScreenInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    loongsonRecPtr pLs = loongsonPTR(pScrn);
    struct stat st;

    if (!pLs)
        return FALSE;

    if (fstat(pLs->drmFD, &st) || !S_ISCHR(st.st_mode))
        return FALSE;

    if (!miSyncShmScreenInit(pScreen))
    {
        return FALSE;
    }


    // dri3 using this
    pLs->deviceName = drmGetDeviceNameFromFd2(pLs->drmFD);

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
        "DRI3 Screen init: device name: %s.\n",
        pLs->deviceName);

    return dri3_screen_init(pScreen, &loongson_dri3_info);
}

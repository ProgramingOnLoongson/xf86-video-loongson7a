/*
 * Copyright © 2007 Red Hat, Inc.
 * Copyright © 2008 Maarten Maathuis
 * Copyright © 2011 Texas Instruments, Inc
 * Copyright © 2020 Loongson Corporation
 *
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
 *    Dave Airlie <airlied@redhat.com>
 *    Ian Elliott <ianelliottus@yahoo.com>
 *    Sui Jingfeng <suijingfeng@loongson.cn>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <xf86DDC.h>
#include <xf86RandR12.h>
#include <xf86Crtc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifdef HAVE_XEXTPROTO_71
#include <X11/extensions/dpmsconst.h>
#else
#define DPMS_SERVER
#include <X11/extensions/dpms.h>
#endif

#include <X11/Xatom.h>

#include <libudev.h>

#include "loongson_driver.h"
#include "loongson_debug.h"
#include "loongson_entity.h"
#include "loongson_dri2.h"
#include "dumb_bo.h"
#include "drmmode_display.h"

static Bool drmmode_xf86crtc_resize(ScrnInfoPtr scrn, int width, int height);
static void drmmode_output_dpms(xf86OutputPtr output, int mode);
static Bool resize_scanout_bo(ScrnInfoPtr pScrn, int width, int height);
static Bool drmmode_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode, Rotation rotation, int x, int y);


static struct drmmode_rec * drmmode_from_scrn(ScrnInfoPtr pScrn)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	struct drmmode_crtc_private_rec *drmmode_crtc;

	drmmode_crtc = xf86_config->crtc[0]->driver_private;
	return drmmode_crtc->drmmode;
}

static void
drmmode_ConvertFromKMode(ScrnInfoPtr pScrn, drmModeModeInfo *kmode, DisplayModePtr mode)
{
	memset(mode, 0, sizeof(DisplayModeRec));
	mode->status = MODE_OK;

	mode->Clock = kmode->clock;

	mode->HDisplay = kmode->hdisplay;
	mode->HSyncStart = kmode->hsync_start;
	mode->HSyncEnd = kmode->hsync_end;
	mode->HTotal = kmode->htotal;
	mode->HSkew = kmode->hskew;

	mode->VDisplay = kmode->vdisplay;
	mode->VSyncStart = kmode->vsync_start;
	mode->VSyncEnd = kmode->vsync_end;
	mode->VTotal = kmode->vtotal;
	mode->VScan = kmode->vscan;

	mode->Flags = kmode->flags;
	mode->name = strdup(kmode->name);

	DEBUG_MSG("copy mode %s (%p %p)", kmode->name, mode->name, mode);

	if (kmode->type & DRM_MODE_TYPE_DRIVER)
		mode->type = M_T_DRIVER;

	if (kmode->type & DRM_MODE_TYPE_PREFERRED)
		mode->type |= M_T_PREFERRED;

	xf86SetModeCrtc(mode, pScrn->adjustFlags);
}

static void
drmmode_ConvertToKMode(ScrnInfoPtr pScrn, drmModeModeInfo *kmode,
		DisplayModePtr mode)
{
	memset(kmode, 0, sizeof(*kmode));

	kmode->clock = mode->Clock;
	kmode->hdisplay = mode->HDisplay;
	kmode->hsync_start = mode->HSyncStart;
	kmode->hsync_end = mode->HSyncEnd;
	kmode->htotal = mode->HTotal;
	kmode->hskew = mode->HSkew;

	kmode->vdisplay = mode->VDisplay;
	kmode->vsync_start = mode->VSyncStart;
	kmode->vsync_end = mode->VSyncEnd;
	kmode->vtotal = mode->VTotal;
	kmode->vscan = mode->VScan;

	kmode->flags = mode->Flags;
	if (mode->name)
		strncpy(kmode->name, mode->name, DRM_DISPLAY_MODE_LEN);
	kmode->name[DRM_DISPLAY_MODE_LEN-1] = 0;
}

static void
drmmode_crtc_dpms(xf86CrtcPtr crtc, int mode)
{
	struct drmmode_crtc_private_rec *drmmode_crtc = crtc->driver_private;
	struct drmmode_rec *drmmode = drmmode_crtc->drmmode;
	ScrnInfoPtr pScrn = crtc->scrn;

	DEBUG_MSG("Setting dpms mode %d on crtc %d", mode, drmmode_crtc->crtc_id);
	drmmode_crtc->dpms_mode = mode;

	switch (mode) {
	case DPMSModeOn:
		drmmode_set_mode_major(crtc, &crtc->mode, crtc->rotation, crtc->x, crtc->y);
		break;

	/* unimplemented modes fall through to the next lowest mode */
	case DPMSModeStandby:
	case DPMSModeSuspend:
	case DPMSModeOff:
		if (drmModeSetCrtc(drmmode->fd, drmmode_crtc->crtc_id, 0, 0, 0, 0, 0, NULL)) {
			ERROR_MSG("drm failed to disable crtc %d", drmmode_crtc->crtc_id);
		} else {
			int i;
			xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);

			/* set dpms off for all outputs for this crtc */
			for (i = 0; i < xf86_config->num_output; i++) {
				xf86OutputPtr output = xf86_config->output[i];
				if (output->crtc != crtc)
					continue;
				drmmode_output_dpms(output, mode);
			}
		}
		break;
	default:
		ERROR_MSG("bad dpms mode %d for crtc %d", mode, drmmode_crtc->crtc_id);
		return;
	}
}

static int
drmmode_revert_mode(xf86CrtcPtr crtc, uint32_t *output_ids, int output_count)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	struct drmmode_crtc_private_rec *drmmode_crtc = crtc->driver_private;
	uint32_t fb_id;
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
	drmModeModeInfo kmode;

	if (!drmmode_crtc->last_good_mode) {
		DEBUG_MSG("No last good values to use");
		return FALSE;
	}

	/* revert to last good settings */
	DEBUG_MSG("Reverting to last_good values");
	if (!resize_scanout_bo(pScrn,
			drmmode_crtc->last_good_mode->HDisplay,
			drmmode_crtc->last_good_mode->VDisplay)) {
		ERROR_MSG("Could not revert to last good mode");
		return FALSE;
	}

	fb_id = armsoc_bo_get_fb(pARMSOC->scanout);
	drmmode_ConvertToKMode(crtc->scrn, &kmode,
			drmmode_crtc->last_good_mode);
	drmModeSetCrtc(drmmode_crtc->drmmode->fd,
			drmmode_crtc->crtc_id,
			fb_id,
			drmmode_crtc->last_good_x,
			drmmode_crtc->last_good_y,
			output_ids, output_count, &kmode);

	/* let RandR know we changed things */
	xf86RandR12TellChanged(pScrn->pScreen);

	return TRUE;
}

static Bool
drmmode_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
        Rotation rotation, int x, int y)
{
    ScrnInfoPtr pScrn = crtc->scrn;
    struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
    struct drmmode_crtc_private_rec *drmmode_crtc = crtc->driver_private;
    struct drmmode_rec *drmmode = drmmode_crtc->drmmode;
    uint32_t *output_ids = NULL;
    int output_count = 0;
    int ret = TRUE;
    int err;
    int i;
    uint32_t fb_id;
    drmModeModeInfo kmode;
    drmModeCrtcPtr newcrtc = NULL;

    TRACE_ENTER();

    fb_id = armsoc_bo_get_fb(pARMSOC->scanout);

    if (fb_id == 0)
    {
        err = armsoc_bo_add_fb(pARMSOC->scanout);
        if (err) {
            ERROR_MSG(
                    "Failed to add framebuffer to the scanout buffer %d", err);
            return FALSE;
        }

        fb_id = armsoc_bo_get_fb(pARMSOC->scanout);
        if (0 == fb_id)
        {
            DEBUG_MSG("Failed create framebuffer: %dx%d",
                pScrn->virtualX, pScrn->virtualY);
            return FALSE;
        }
        else
        {
            struct dumb_bo * pBO = pARMSOC->scanout;

            DEBUG_MSG("Create framebuffer: %dx%d, depth=%d, bpp=%d, pitch=%d",
                armsoc_bo_width(pBO), armsoc_bo_height(pBO), armsoc_bo_depth(pBO),
                armsoc_bo_bpp(pBO), armsoc_bo_pitch(pBO) );
        }
    }

    drmmode->fb_id=fb_id;

    /* Set the new mode: */
    if (mode) {

        crtc->mode = *mode;
        crtc->x = x;
        crtc->y = y;
        crtc->rotation = rotation;

        output_ids = calloc(xf86_config->num_output, sizeof *output_ids);
        if (!output_ids)
        {
            ERROR_MSG( "memory allocation failed in drmmode_set_mode_major()" );
            ret = FALSE;
            goto cleanup;
        }

        for (i = 0; i < xf86_config->num_output; ++i)
        {
            xf86OutputPtr output = xf86_config->output[i];
            struct drmmode_output_priv *drmmode_output;

            if (output->crtc != crtc)
                continue;

            drmmode_output = output->driver_private;
            output_ids[output_count] = drmmode_output->connector->connector_id;
            output_count++;
        }

        if (!xf86CrtcRotate(crtc)) {
            ERROR_MSG(
                    "failed to assign rotation in drmmode_set_mode_major()");
            ret = FALSE;
            goto cleanup;
        }

        if (crtc->funcs->gamma_set)
            crtc->funcs->gamma_set(crtc, crtc->gamma_red, crtc->gamma_green,
                    crtc->gamma_blue, crtc->gamma_size);

        drmmode_ConvertToKMode(crtc->scrn, &kmode, mode);

        err = drmModeSetCrtc(drmmode->fd, drmmode_crtc->crtc_id,
                fb_id, x, y, output_ids, output_count, &kmode);
        if (err) {
            ERROR_MSG(
                    "drm failed to set mode: %s", strerror(-err));

            ret = FALSE;
            if (!drmmode_revert_mode(crtc, output_ids, output_count))
                goto cleanup;
            else
                goto done_setting;
        }

        /* get the actual crtc info */
        newcrtc = drmModeGetCrtc(drmmode->fd, drmmode_crtc->crtc_id);
        if (!newcrtc) {
            ERROR_MSG("couldn't get actual mode back");

            ret = FALSE;
            if (!drmmode_revert_mode(crtc, output_ids, output_count))
                goto cleanup;
            else
                goto done_setting;
        }

        if (kmode.hdisplay != newcrtc->mode.hdisplay ||
                kmode.vdisplay != newcrtc->mode.vdisplay) {

            ERROR_MSG(
                    "drm did not set requested mode! (requested %dx%d, actual %dx%d)",
                    kmode.hdisplay, kmode.vdisplay,
                    newcrtc->mode.hdisplay,
                    newcrtc->mode.vdisplay);

            ret = FALSE;
            if (!drmmode_revert_mode(crtc, output_ids, output_count))
                goto cleanup;
            else
                goto done_setting;
        }

        /* When called on a resize, crtc->mode already contains the
         * resized values so we can't use this for recovery.
         * We can't read it out of the crtc either as mode_valid is 0.
         * Instead we save the last good mode set here & fallback to
         * that on failure.
         */
        DEBUG_MSG("Saving last good values");
        drmmode_crtc->last_good_x = crtc->x;
        drmmode_crtc->last_good_y = crtc->y;
        drmmode_crtc->last_good_rotation = crtc->rotation;
        if (drmmode_crtc->last_good_mode) {
            if (drmmode_crtc->last_good_mode->name)
                free((void *)drmmode_crtc->last_good_mode->name);
            free(drmmode_crtc->last_good_mode);
        }
        drmmode_crtc->last_good_mode = xf86DuplicateMode(&crtc->mode);

        ret = TRUE;

    }
done_setting:
    /* Turn on any outputs on this crtc that may have been disabled: */
    for (i = 0; i < xf86_config->num_output; i++) {
        xf86OutputPtr output = xf86_config->output[i];

        if (output->crtc != crtc)
            continue;

        drmmode_output_dpms(output, DPMSModeOn);
    }


cleanup:
    if (newcrtc)
        drmModeFreeCrtc(newcrtc);

    if (output_ids)
        free(output_ids);

    if (!ret && drmmode_crtc->last_good_mode) {
        /* If there was a problem, restore the last good mode: */
        crtc->x = drmmode_crtc->last_good_x;
        crtc->y = drmmode_crtc->last_good_y;
        crtc->rotation = drmmode_crtc->last_good_rotation;
        crtc->mode = *drmmode_crtc->last_good_mode;
    }

    TRACE_EXIT();
    return ret;
}

static void
drmmode_hide_cursor(xf86CrtcPtr crtc)
{
	struct drmmode_crtc_private_rec *drmmode_crtc = crtc->driver_private;
	struct drmmode_rec *drmmode = drmmode_crtc->drmmode;
	struct drmmode_cursor_rec *cursor = drmmode->cursor;
	ScrnInfoPtr pScrn = crtc->scrn;
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);

	if (!cursor)
		return;

	drmmode_crtc->cursor_visible = FALSE;

    { /* HWCURSOR_API_STANDARD */
		/* set handle to 0 to disable the cursor */
		drmModeSetCursor(drmmode->fd, drmmode_crtc->crtc_id, 0, 0, 0);
	}
}

/*
 * The argument "update_image" controls whether the cursor image needs
 * to be updated by the HW or not. This is ignored by HWCURSOR_API_PLANE
 * which doesn't allow changing the cursor position without updating
 * the image too.
 */
static void
drmmode_show_cursor_image(xf86CrtcPtr crtc, Bool update_image)
{
	struct drmmode_crtc_private_rec *drmmode_crtc = crtc->driver_private;
	struct drmmode_rec *drmmode = drmmode_crtc->drmmode;
	struct drmmode_cursor_rec *cursor = drmmode->cursor;
	int crtc_x, crtc_y, src_x, src_y;
	int w, h;
	ScrnInfoPtr pScrn = crtc->scrn;
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);

	if (!cursor)
		return;

	drmmode_crtc->cursor_visible = TRUE;

	w = pARMSOC->cursor_width;
	h = pARMSOC->cursor_height;

	/* get x of padded cursor */
	crtc_x = cursor->x;
	crtc_y = cursor->y;

    {
		if (update_image)
			drmModeSetCursor(drmmode->fd,
					 drmmode_crtc->crtc_id,
					 cursor->handle, w, h);
		drmModeMoveCursor(drmmode->fd,
				  drmmode_crtc->crtc_id,
				  crtc_x, crtc_y);
	}
}

static void
drmmode_show_cursor(xf86CrtcPtr crtc)
{
	drmmode_show_cursor_image(crtc, TRUE);
}

static void
drmmode_set_cursor_position(xf86CrtcPtr crtc, int x, int y)
{
	struct drmmode_crtc_private_rec *drmmode_crtc = crtc->driver_private;
	struct drmmode_rec *drmmode = drmmode_crtc->drmmode;
	struct drmmode_cursor_rec *cursor = drmmode->cursor;

	if (!cursor)
		return;

	cursor->x = x;
	cursor->y = y;

	/*
	 * Show the cursor at a different position without updating the image
	 * when possible (HWCURSOR_API_PLANE doesn't have a way to update
	 * cursor position without updating the image too).
	 */
	drmmode_show_cursor_image(crtc, FALSE);
}

/*
 * The cursor format is ARGB so the image can be copied straight over.
 * Columns of CURSORPAD blank pixels are maintained down either side
 * of the destination image. This is a workaround for a bug causing
 * corruption when the cursor reaches the screen edges in some DRM
 * drivers.
 */
static void set_cursor_image(xf86CrtcPtr crtc, uint32_t *d, CARD32 *s)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
	int row;
	void *dst;
	const char *src_row;
	char *dst_row;
	uint32_t cursorh = pARMSOC->cursor_height;
	uint32_t cursorw = pARMSOC->cursor_width;
	uint32_t cursorpad = 0;

	dst = d;
	for (row = 0; row < cursorh; row += 1) {
		/* we're operating with ARGB data (4 bytes per pixel) */
		src_row = (const char *)s + row * 4 * cursorw;
		dst_row = (char *)dst + row * 4 * (cursorw + 2 * cursorpad);

		/* set first CURSORPAD pixels in row to 0 */
		memset(dst_row, 0, (4 * cursorpad));
		/* copy cursor image pixel row across */
		memcpy(dst_row + (4 * cursorpad), src_row, 4 * cursorw);
		/* set last CURSORPAD pixels in row to 0 */
		memset(dst_row + 4 * (cursorpad + cursorw),
				0, (4 * cursorpad));
	}
}

static void drmmode_load_cursor_argb(xf86CrtcPtr crtc, CARD32 *image)
{
    uint32_t *d;
    struct drmmode_crtc_private_rec *drmmode_crtc = crtc->driver_private;
    struct drmmode_rec *drmmode = drmmode_crtc->drmmode;
    struct drmmode_cursor_rec *cursor = drmmode->cursor;
    int visible;

    TRACE_ENTER();
    if (!cursor)
    {
        return;
        TRACE_EXIT();
    }

    visible = drmmode_crtc->cursor_visible;

    if (visible)
        drmmode_hide_cursor(crtc);


    dumb_bo_map(cursor->bo->fd, cursor->bo);

    d = cursor->bo->ptr;

    if (!d)
    {
        xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
                "load_cursor_argb map failure\n");
        if (visible)
            drmmode_show_cursor_image(crtc, TRUE);

        TRACE_EXIT();
        return;
    }

    set_cursor_image(crtc, d, image);

    if (visible)
    {
        drmmode_show_cursor_image(crtc, TRUE);
    }

    TRACE_EXIT();
}




void drmmode_cursor_fini(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct drmmode_rec *drmmode = drmmode_from_scrn(pScrn);
	struct drmmode_cursor_rec *cursor = drmmode->cursor;
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);

	if (!cursor)
		return;

	drmmode->cursor = NULL;
	xf86_cursors_fini(pScreen);

	armsoc_bo_unreference(cursor->bo);

	free(cursor);
}



static void
drmmode_gamma_set(xf86CrtcPtr crtc, CARD16 *red, CARD16 *green, CARD16 *blue, int size)
{
	struct drmmode_crtc_private_rec *drmmode_crtc = crtc->driver_private;
	struct drmmode_rec *drmmode = drmmode_crtc->drmmode;
	int ret;

	ret = drmModeCrtcSetGamma(drmmode->fd, drmmode_crtc->crtc_id,
			size, red, green, blue);
	if (ret != 0) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
				"failed to set gamma: %s\n", strerror(-ret));
	}
}


static const xf86CrtcFuncsRec drmmode_crtc_funcs = {
		.dpms = drmmode_crtc_dpms,
		.set_mode_major = drmmode_set_mode_major,
		.set_cursor_position = drmmode_set_cursor_position,
		.show_cursor = drmmode_show_cursor,
		.hide_cursor = drmmode_hide_cursor,
		.load_cursor_argb = drmmode_load_cursor_argb,
		.gamma_set = drmmode_gamma_set,
};


static uint32_t drmmode_crtc_vblank_pipe(int crtc_id)
{
    if (crtc_id > 1)
        return crtc_id << DRM_VBLANK_HIGH_CRTC_SHIFT;
    else if (crtc_id > 0)
        return DRM_VBLANK_SECONDARY;
    else
        return 0;
}


static Bool drmmode_crtc_init(ScrnInfoPtr pScrn, struct drmmode_rec *drmmode,
        drmModeResPtr mode_res, int num)
{
    xf86CrtcPtr crtc;
    struct drmmode_crtc_private_rec *drmmode_crtc;

    TRACE_ENTER();

    crtc = xf86CrtcCreate(pScrn, &drmmode_crtc_funcs);
    if (crtc == NULL)
    {
        TRACE_EXIT();
        return FALSE;
    }

    drmmode_crtc = xnfcalloc(1, sizeof *drmmode_crtc);
    drmmode_crtc->crtc_id = mode_res->crtcs[num];
    drmmode_crtc->drmmode = drmmode;
    drmmode_crtc->last_good_mode = NULL;
    drmmode_crtc->vblank_pipe = drmmode_crtc_vblank_pipe(num);
    crtc->driver_private = drmmode_crtc;

    INFO_MSG("Got CRTC: %d (id: %d)", num, drmmode_crtc->crtc_id);

    // xorg_list_init(&drmmode_crtc->mode_list);

    /* Mark num'th crtc as in use on this device. */
    LS_MarkCrtcInUse(pScrn, num);

    TRACE_EXIT();

    return TRUE;
}


static xf86OutputStatus drmmode_output_detect(xf86OutputPtr output)
{
	/* go to the hw and retrieve a new output struct */
	struct drmmode_output_priv *drmmode_output = output->driver_private;
	struct drmmode_rec *drmmode = drmmode_output->drmmode;
	xf86OutputStatus status;
	drmModeFreeConnector(drmmode_output->connector);

	drmmode_output->connector =
			drmModeGetConnector(drmmode->fd, drmmode_output->output_id);

	switch (drmmode_output->connector->connection) {
	case DRM_MODE_CONNECTED:
		status = XF86OutputStatusConnected;
		break;
	case DRM_MODE_DISCONNECTED:
		status = XF86OutputStatusDisconnected;
		break;
	default:
	case DRM_MODE_UNKNOWNCONNECTION:
		status = XF86OutputStatusUnknown;
		break;
	}
	return status;
}

static Bool drmmode_output_mode_valid(xf86OutputPtr output, DisplayModePtr mode)
{
	if (mode->type & M_T_DEFAULT)
		/* Default modes are harmful here. */
		return MODE_BAD;

	return MODE_OK;
}

static DisplayModePtr drmmode_output_get_modes(xf86OutputPtr output)
{
	ScrnInfoPtr pScrn = output->scrn;
	struct drmmode_output_priv *drmmode_output = output->driver_private;
	drmModeConnectorPtr connector = drmmode_output->connector;
	struct drmmode_rec *drmmode = drmmode_output->drmmode;
	DisplayModePtr modes = NULL;
	drmModePropertyPtr prop;
	xf86MonPtr ddc_mon = NULL;
	int i;

	/* look for an EDID property */
	for (i = 0; i < connector->count_props; ++i)
	{
		prop = drmModeGetProperty(drmmode->fd, connector->props[i]);
		if (!prop)
			continue;

		if ((prop->flags & DRM_MODE_PROP_BLOB) && !strcmp(prop->name, "EDID"))
		{
			if (drmmode_output->edid_blob)
				drmModeFreePropertyBlob(
						drmmode_output->edid_blob);

			drmmode_output->edid_blob =
					drmModeGetPropertyBlob(drmmode->fd, connector->prop_values[i]);
		}
		drmModeFreeProperty(prop);
	}

	if (drmmode_output->edid_blob)
	{
		ddc_mon = xf86InterpretEDID(pScrn->scrnIndex, drmmode_output->edid_blob->data);
	}

	if (ddc_mon)
	{
		if (drmmode_output->edid_blob->length > 128)
			ddc_mon->flags |= MONITOR_EDID_COMPLETE_RAWDATA;

		xf86OutputSetEDID(output, ddc_mon);
		xf86SetDDCproperties(pScrn, ddc_mon);
	}

	DEBUG_MSG("count_modes: %d", connector->count_modes);

	/* modes should already be available */
	for (i = 0; i < connector->count_modes; ++i)
	{
		DisplayModePtr mode = xnfalloc(sizeof(DisplayModeRec));

		drmmode_ConvertFromKMode(pScrn, &connector->modes[i], mode);
		modes = xf86ModesAdd(modes, mode);
	}
	return modes;
}


static void drmmode_output_destroy(xf86OutputPtr output)
{
	struct drmmode_output_priv *drmmode_output = output->driver_private;
	int i;

	if (drmmode_output->edid_blob)
	{
		drmModeFreePropertyBlob(drmmode_output->edid_blob);
	}

	for (i = 0; i < drmmode_output->num_props; ++i)
	{
		drmModeFreeProperty(drmmode_output->props[i].mode_prop);
		free(drmmode_output->props[i].atoms);
	}
	free(drmmode_output->props);

	for (i = 0; i < drmmode_output->connector->count_encoders; ++i)
	{
		drmModeFreeEncoder(drmmode_output->encoders[i]);
	}

	free(drmmode_output->encoders);

	drmModeFreeConnector(drmmode_output->connector);
	free(drmmode_output);
	output->driver_private = NULL;
}


static void drmmode_output_dpms(xf86OutputPtr output, int mode)
{
	struct drmmode_output_priv *drmmode_output = output->driver_private;
	drmModeConnectorPtr connector = drmmode_output->connector;
	drmModePropertyPtr prop;
	struct drmmode_rec *drmmode = drmmode_output->drmmode;
	int mode_id = -1, i;

	for (i = 0; i < connector->count_props; i++) {
		prop = drmModeGetProperty(drmmode->fd, connector->props[i]);
		if (!prop)
			continue;
		if ((prop->flags & DRM_MODE_PROP_ENUM) &&
		    !strcmp(prop->name, "DPMS")) {
			mode_id = connector->props[i];
			drmModeFreeProperty(prop);
			break;
		}
		drmModeFreeProperty(prop);
	}

	if (mode_id < 0)
		return;

	drmModeConnectorSetProperty(drmmode->fd, connector->connector_id,
			mode_id, mode);
}

static Bool drmmode_property_ignore(drmModePropertyPtr prop)
{
	if (!prop)
		return TRUE;
	/* ignore blob prop */
	if (prop->flags & DRM_MODE_PROP_BLOB)
		return TRUE;
	/* ignore standard property */
	if (!strcmp(prop->name, "EDID") ||
			!strcmp(prop->name, "DPMS"))
		return TRUE;

	return FALSE;
}

static void drmmode_output_create_resources(xf86OutputPtr output)
{
	struct drmmode_output_priv *drmmode_output = output->driver_private;
	drmModeConnectorPtr connector = drmmode_output->connector;
	struct drmmode_rec *drmmode = drmmode_output->drmmode;
	drmModePropertyPtr drmmode_prop;
	uint32_t value;
	int i, j, err;

	drmmode_output->props = calloc(connector->count_props,
					sizeof(*drmmode_output->props));
	if (!drmmode_output->props)
		return;

	drmmode_output->num_props = 0;
	for (i = 0; i < connector->count_props; i++) {
		drmmode_prop = drmModeGetProperty(drmmode->fd,
			connector->props[i]);
		if (drmmode_property_ignore(drmmode_prop)) {
			drmModeFreeProperty(drmmode_prop);
			continue;
		}
		drmmode_output->props[drmmode_output->num_props].mode_prop =
				drmmode_prop;
		drmmode_output->props[drmmode_output->num_props].index = i;
		drmmode_output->num_props++;
	}

	for (i = 0; i < drmmode_output->num_props; i++) {
		struct drmmode_prop_rec *p = &drmmode_output->props[i];
		drmmode_prop = p->mode_prop;

		value = drmmode_output->connector->prop_values[p->index];

		if (drmmode_prop->flags & DRM_MODE_PROP_RANGE) {
			INT32 range[2];

			p->num_atoms = 1;
			p->atoms = calloc(p->num_atoms, sizeof *p->atoms);
			if (!p->atoms)
				continue;
			p->atoms[0] = MakeAtom(drmmode_prop->name,
						strlen(drmmode_prop->name),
						TRUE);
			range[0] = drmmode_prop->values[0];
			range[1] = drmmode_prop->values[1];
			err = RRConfigureOutputProperty(output->randr_output,
					p->atoms[0],
					FALSE, TRUE,
					drmmode_prop->flags &
						DRM_MODE_PROP_IMMUTABLE ?
							TRUE : FALSE,
					2, range);

			if (err != 0)
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
						"RRConfigureOutputProperty error, %d\n",
						err);

			err = RRChangeOutputProperty(output->randr_output,
					p->atoms[0],
					XA_INTEGER, 32, PropModeReplace, 1,
					&value, FALSE, FALSE);
			if (err != 0)
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
						"RRChangeOutputProperty error, %d\n",
						err);

		} else if (drmmode_prop->flags & DRM_MODE_PROP_ENUM) {
			p->num_atoms = drmmode_prop->count_enums + 1;
			p->atoms = calloc(p->num_atoms, sizeof *p->atoms);
			if (!p->atoms)
				continue;
			p->atoms[0] = MakeAtom(drmmode_prop->name,
						strlen(drmmode_prop->name),
						TRUE);
			for (j = 1; j <= drmmode_prop->count_enums; j++) {
				struct drm_mode_property_enum *e =
						&drmmode_prop->enums[j-1];
				p->atoms[j] = MakeAtom(e->name,
							strlen(e->name), TRUE);
			}
			err = RRConfigureOutputProperty(output->randr_output,
					p->atoms[0],
					FALSE, FALSE,
					drmmode_prop->flags &
						DRM_MODE_PROP_IMMUTABLE ?
							TRUE : FALSE,
					p->num_atoms - 1,
					(INT32 *)&p->atoms[1]);

			if (err != 0)
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
						"RRConfigureOutputProperty error, %d\n",
						err);

			for (j = 0; j < drmmode_prop->count_enums; j++)
				if (drmmode_prop->enums[j].value == value)
					break;
			/* there's always a matching value */
			err = RRChangeOutputProperty(output->randr_output,
					p->atoms[0],
					XA_ATOM, 32, PropModeReplace, 1,
					&p->atoms[j+1], FALSE, FALSE);
			if (err != 0)
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
						"RRChangeOutputProperty error, %d\n",
						err);
		}
	}
}

static Bool
drmmode_output_set_property(xf86OutputPtr output, Atom property,
		RRPropertyValuePtr value)
{
	struct drmmode_output_priv *drmmode_output = output->driver_private;
	struct drmmode_rec *drmmode = drmmode_output->drmmode;
	int i, ret;

	for (i = 0; i < drmmode_output->num_props; i++) {
		struct drmmode_prop_rec *p = &drmmode_output->props[i];

		if (p->atoms[0] != property)
			continue;

		if (p->mode_prop->flags & DRM_MODE_PROP_RANGE) {
			uint32_t val;

			if (value->type != XA_INTEGER || value->format != 32 ||
					value->size != 1)
				return FALSE;
			val = *(uint32_t *)value->data;

			ret = drmModeConnectorSetProperty(drmmode->fd,
					drmmode_output->output_id,
					p->mode_prop->prop_id, (uint64_t)val);

			if (ret)
				return FALSE;

			return TRUE;

		} else if (p->mode_prop->flags & DRM_MODE_PROP_ENUM) {
			Atom	atom;
			const char	*name;
			int		j;

			if (value->type != XA_ATOM ||
					value->format != 32 ||
					value->size != 1)
				return FALSE;

			memcpy(&atom, value->data, 4);
			name = NameForAtom(atom);
			if (name == NULL)
				return FALSE;

			/* search for matching name string, then
			 * set its value down
			 */
			for (j = 0; j < p->mode_prop->count_enums; j++) {
				if (!strcmp(p->mode_prop->enums[j].name,
						name)) {
					ret = drmModeConnectorSetProperty(
						drmmode->fd,
						drmmode_output->output_id,
						p->mode_prop->prop_id,
						p->mode_prop->enums[j].value);

					if (ret)
						return FALSE;

					return TRUE;
				}
			}
			return FALSE;
		}
	}
	return TRUE;
}

static Bool
drmmode_output_get_property(xf86OutputPtr output, Atom property)
{

	struct drmmode_output_priv *drmmode_output = output->driver_private;
	struct drmmode_rec *drmmode = drmmode_output->drmmode;
	uint32_t value;
	int err, i;

	if (output->scrn->vtSema) {
		drmModeFreeConnector(drmmode_output->connector);
		drmmode_output->connector =
				drmModeGetConnector(drmmode->fd,
						drmmode_output->output_id);
	}

	for (i = 0; i < drmmode_output->num_props; i++) {
		struct drmmode_prop_rec *p = &drmmode_output->props[i];
		if (p->atoms[0] != property)
			continue;

		value = drmmode_output->connector->prop_values[p->index];

		if (p->mode_prop->flags & DRM_MODE_PROP_RANGE) {
			err = RRChangeOutputProperty(output->randr_output,
					property, XA_INTEGER, 32,
					PropModeReplace, 1, &value,
					FALSE, FALSE);

			return !err;
		} else if (p->mode_prop->flags & DRM_MODE_PROP_ENUM) {
			int		j;

			/* search for matching name string, then set
			 * its value down
			 */
			for (j = 0; j < p->mode_prop->count_enums; j++) {
				if (p->mode_prop->enums[j].value == value)
					break;
			}

			err = RRChangeOutputProperty(output->randr_output,
						property,
						XA_ATOM, 32, PropModeReplace, 1,
						&p->atoms[j+1], FALSE, FALSE);

			return !err;
		}
	}

	return FALSE;
}

static const xf86OutputFuncsRec drmmode_output_funcs = {
    .create_resources = drmmode_output_create_resources,
    .dpms = drmmode_output_dpms,
    .mode_valid = drmmode_output_mode_valid,
    .detect = drmmode_output_detect,
    .get_modes = drmmode_output_get_modes,
    .set_property = drmmode_output_set_property,
    .get_property = drmmode_output_get_property,
    .destroy = drmmode_output_destroy
};


/* Convert libdrm's subpixel order to Xorg subpixel */
static const int subpixel_conv_table[] = {
    [DRM_MODE_SUBPIXEL_UNKNOWN]        = SubPixelUnknown,
    [DRM_MODE_SUBPIXEL_HORIZONTAL_RGB] = SubPixelHorizontalRGB,
    [DRM_MODE_SUBPIXEL_HORIZONTAL_BGR] = SubPixelHorizontalBGR,
    [DRM_MODE_SUBPIXEL_VERTICAL_RGB]   = SubPixelVerticalRGB,
    [DRM_MODE_SUBPIXEL_VERTICAL_BGR]   = SubPixelVerticalBGR,
    [DRM_MODE_SUBPIXEL_NONE]           = SubPixelNone,
};

const char *output_names[] = {
    "None",
    "VGA",
    "DVI-I",
    "DVI-D",
    "DVI-A",
    "Composite",
    "SVIDEO",
    "LVDS",
    "CTV",
    "Component",
    "DIN",
    "DP",
    "HDMI",
    "HDMI-B",
    "TV",
    "eDP",
    "Virtual",
    "DSI",
    "DPI",
};

#define NUM_OUTPUT_NAMES (sizeof(output_names) / sizeof(output_names[0]))


static int drmmode_output_init(ScrnInfoPtr pScrn, struct drmmode_rec *drmmode,
        drmModeResPtr mode_res, int num)
{
    xf86OutputPtr output;
    drmModeConnectorPtr connector;
    drmModeEncoderPtr *kencoders = NULL;
    struct drmmode_output_priv *drmmode_output;
    char name[32];
    int i;

    TRACE_ENTER();

    connector = drmModeGetConnector(drmmode->fd, mode_res->connectors[num]);

    if (NULL == connector)
    {
        TRACE_EXIT();
        return 0;
    }

    kencoders = calloc(sizeof(drmModeEncoderPtr), connector->count_encoders);
    if (NULL == kencoders)
    {
        TRACE_EXIT();
        drmModeFreeConnector(connector);
        return 0;
    }

    for (i = 0; i < connector->count_encoders; ++i)
    {
        kencoders[i] = drmModeGetEncoder(drmmode->fd, connector->encoders[i]);
        if (!kencoders[i])
        {
            goto free_encoders_exit;
        }
    }

    if (connector->connector_type >= NUM_OUTPUT_NAMES)
    {
        snprintf(name, 32, "Unknown%d-%d",
            connector->connector_type, connector->connector_type_id);
    }
    else
    {
        snprintf(name, 32, "%s-%d",
            output_names[connector->connector_type], connector->connector_type_id);
    }

    output = xf86OutputCreate(pScrn, &drmmode_output_funcs, name);
    if (!output)
        goto free_encoders_exit;

    drmmode_output = calloc(1, sizeof *drmmode_output);
    if (!drmmode_output)
    {
        xf86OutputDestroy(output);
        goto free_encoders_exit;
    }

    drmmode_output->output_id = mode_res->connectors[num];
    drmmode_output->connector = connector;
    drmmode_output->encoders = kencoders;
    drmmode_output->drmmode = drmmode;

    output->mm_width = connector->mmWidth;
    output->mm_height = connector->mmHeight;
    output->subpixel_order = subpixel_conv_table[connector->subpixel];
    // Whether this output can support interlaced modes
    output->interlaceAllowed = TRUE;
    // Whether this output can support double scan modes
    output->doubleScanAllowed = TRUE;

    output->driver_private = drmmode_output;

    /*
     * Determine which crtcs are supported by all the encoders which
     * are valid for the connector of this output.
     */
    output->possible_crtcs = 0xffffffff;
    for (i = 0; i < connector->count_encoders; i++)
    {
        output->possible_crtcs &= kencoders[i]->possible_crtcs;
    }

    /*
     * output->possible_crtcs is a bitmask arranged by index of crtcs
     * for this screen while encoders->possible_crtcs covers all crtcs
     * supported by the drm. If we have selected one crtc per screen,
     * it must be at index 0.
     */

    loongsonRecPtr pLs = loongsonPTR(pScrn);

    if (pLs->crtcNum >= 0)
    {
        output->possible_crtcs = (output->possible_crtcs >> (pLs->crtcNum)) & 1;
    }

    output->possible_clones = 0; /* set after all outputs initialized */

    TRACE_EXIT();
    return 1;

free_encoders_exit:
    for (i = 0; i < connector->count_encoders; i++)
        drmModeFreeEncoder(kencoders[i]);

free_connector_exit:
    drmModeFreeConnector(connector);
    return 0;
}


static uint32_t find_clones(ScrnInfoPtr scrn, xf86OutputPtr output)
{
    struct drmmode_output_priv *drmmode_output = output->driver_private;
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
    int index_mask = 0;

    int i;

    TRACE_ENTER();

    // output->possible_clones = 0;
    if (drmmode_output->enc_clones == 0)
    {
        TRACE_EXIT();
        return 0;
    }

    for (i = 0; i < xf86_config->num_output; ++i)
    {
        xf86OutputPtr clone_output = xf86_config->output[i];
        struct drmmode_output_priv *clone = clone_output->driver_private;

        if (clone_output == output)
        {
            continue;
        }

        if (clone->enc_mask == 0)
        {
            continue;
        }

        if (drmmode_output->enc_clones == clone->enc_mask)
        {
            index_mask |= (1 << i);
        }
    }

    TRACE_EXIT();
    return index_mask;
}


static void drmmode_clones_init(ScrnInfoPtr pScrn,
                  struct drmmode_rec *drmmode, drmModeResPtr mode_res)
{
    int i;
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);

    /* For each output generate enc_mask, a mask of encoders present,
     * and enc_clones, a mask of possible clone encoders
     */
    for (i = 0; i < xf86_config->num_output; ++i)
    {
        xf86OutputPtr output = xf86_config->output[i];
        struct drmmode_output_priv *drmmode_output = output->driver_private;
        int j;

        drmmode_output->enc_clones = 0xffffffff;
        drmmode_output->enc_mask = 0;

        for (j = 0; j < drmmode_output->connector->count_encoders; ++j)
        {
            int k;

            /* set index ordered mask of encoders on this output */
            for (k = 0; k < mode_res->count_encoders; ++k)
            {
                if ( mode_res->encoders[k] == drmmode_output->encoders[j]->encoder_id)
                    drmmode_output->enc_mask |= (1 << k);
            }
            /* set mask for encoder clones possible with all encoders on this output */
            drmmode_output->enc_clones &= drmmode_output->encoders[j]->possible_clones;
        }
    }

    // Output j is a possible clone of output i
    // if the enc_mask for j matches the enc_clones for i

    for (i = 0; i < xf86_config->num_output; i++)
    {
        xf86OutputPtr output = xf86_config->output[i];
        output->possible_clones = find_clones(pScrn, output);

        ////////////////////////////////////////////////////////////////////
        /*
        struct drmmode_output_priv *drmmode_output = output->driver_private;
        int j;

        output->possible_clones = 0;
        if (drmmode_output->enc_clones == 0)
            continue;

        for (j = 0; j < xf86_config->num_output; ++j)
        {
            struct drmmode_output_priv *clone = xf86_config->output[j]->driver_private;

            if ((i != j) && (clone->enc_mask != 0) &&
                    (drmmode_output->enc_clones == clone->enc_mask))
            {
                output->possible_clones |= (1 << j);
            }
        }
        */
    }
}


void set_scanout_bo(ScrnInfoPtr pScrn, struct dumb_bo *bo)
{
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
	struct dumb_bo *old_scanout = pARMSOC->scanout;
	/* It had better have a framebuffer if we're scanning it out */
	assert(armsoc_bo_get_fb(bo));

	armsoc_bo_reference(bo); /* Screen takes ref on new scanout bo */
	pARMSOC->scanout = bo;
	if (old_scanout)
		armsoc_bo_unreference(old_scanout); /* Screen drops ref on old scanout bo */
}



static Bool resize_scanout_bo(ScrnInfoPtr pScrn, int width, int height)
{
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);

	ScreenPtr pScreen = pScrn->pScreen;
	uint32_t pitch;
	int cpp = (pScrn->bitsPerPixel + 7) / 8;
	uint8_t depth = armsoc_bo_depth(pARMSOC->scanout);
	uint8_t bpp = armsoc_bo_bpp(pARMSOC->scanout);
	
	// suijingfeng: debuging here
	// struct drmmode_rec drmmode = &pARMSOC->drmmode;


	TRACE_ENTER();

	if (pScrn->virtualX == width && pScrn->virtualY == height)
		return TRUE;


	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"Resize: %dx%d %d,%d", width, height, depth, bpp);

	/* We don't expect the depth and bpp to change for the screen
	 * assert this here as a check */
	assert(depth == pScrn->depth);
	assert(bpp == pScrn->bitsPerPixel);


	pScrn->virtualX = width;
	pScrn->virtualY = height;

	if ((width != armsoc_bo_width(pARMSOC->scanout)) ||
		(height != armsoc_bo_height(pARMSOC->scanout)) )
	{
		/* creates and takes ref on new scanout bo */
		struct dumb_bo * new_scanout = armsoc_bo_new_with_dim(
			pARMSOC->drmFD, width, height, depth, bpp);
		if (NULL == new_scanout)
		{
			/* Try to use the previous buffer if the new resolution
			 * is smaller than the one on buffer creation
			 */
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				"Allocate new scanout buffer failed - resizing existing bo\n");
			/* Remove the old fb from the bo */
			if (armsoc_bo_rm_fb(pARMSOC->scanout))
				return FALSE;

			/* Resize the bo */
			if (armsoc_bo_resize(pARMSOC->scanout, width, height))
			{
				armsoc_bo_clear(pARMSOC->scanout);
				if (armsoc_bo_add_fb(pARMSOC->scanout))
					xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
						"Failed to add framebuffer to the existing scanout buffer.\n");
				return FALSE;
			}

			/* Add new fb to the bo */
			if (armsoc_bo_clear(pARMSOC->scanout))
				return FALSE;

			if (armsoc_bo_add_fb(pARMSOC->scanout))
			{
				xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
					"Failed to add framebuffer to the existing scanout buffer.\n");
				return FALSE;
			}

			pitch = armsoc_bo_pitch(pARMSOC->scanout);
		}
		else
		{
			struct dumb_bo *old_scanout = pARMSOC->scanout;

			xf86DrvMsg(pScrn->scrnIndex, X_INFO, "allocated new scanout buffer ok.\n");
			pitch = armsoc_bo_pitch(new_scanout);
			/* clear new BO and add FB */
			if (armsoc_bo_clear(new_scanout)) {
				/* drops ref on new scanout on failure exit */
				armsoc_bo_unreference(new_scanout);
				return FALSE;
			}

			if (armsoc_bo_add_fb(new_scanout))
			{
				xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
						"Failed to add framebuffer to the new scanout buffer.\n");
				/* drops ref on new scanout on failure exit */
				armsoc_bo_unreference(new_scanout);
				return FALSE;
			}

			/* Handle dma_buf fd that may be attached to old bo */
			if (armsoc_bo_has_dmabuf(old_scanout))
			{
				int res;
                int prime_fd;
				armsoc_bo_clear_dmabuf(old_scanout);
				res = armsoc_bo_to_dmabuf(new_scanout, &prime_fd);
				if (res != 0)
				{
					xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
						"Unable to attach dma_buf fd to new scanout buffer - %d (%s)\n",
							res, strerror(res));
					/* drops ref on new scanout on failure exit */
					armsoc_bo_unreference(new_scanout);
					return FALSE;
				}
			}
			/* use new scanout buffer */
			set_scanout_bo(pScrn, new_scanout);
			/* Screen has now taken ref on new_scanout so  drops it */
			armsoc_bo_unreference(new_scanout);
			ARMSOCDRI2ResizeSwapChain(pScrn, old_scanout, new_scanout);
		}
		pScrn->displayWidth = pitch / ((pScrn->bitsPerPixel + 7) / 8);
	}
	else
	{
		pitch = armsoc_bo_pitch(pARMSOC->scanout);
	}

	if (pScreen && pScreen->ModifyPixmapHeader)
	{
		PixmapPtr rootPixmap = pScreen->GetScreenPixmap(pScreen);

        dumb_bo_map(pARMSOC->scanout->fd, pARMSOC->scanout);

		/* Wrap the screen pixmap around the new scanout bo.
		 * If we are n-buffering and the scanout bo is behind the
		 * screen pixmap by a few flips, the bo which is being resized
		 * may not belong to the screen pixmap. However we need to
		 * resize the screen pixmap in order to continue flipping.
		 * For now we let this happen and the swap chain reference
		 * on the screen pixmap's existing bo will prevent it being
		 * deleted here. Things may look odd until the swap chain
		 * works through. This needs improvement.
		 */
		pScreen->ModifyPixmapHeader(rootPixmap,
			pScrn->virtualX, pScrn->virtualY, depth, bpp, pitch,
			pARMSOC->scanout->ptr);

		/* Bump the serial number to ensure that all existing DRI2
		 * buffers are invalidated.
		 *
		 * This is particularly required for when the resolution is
		 * changed and then reverted to the original size without a
		 * DRI2 client/s getting a new buffer. Without this, the
		 * drawable is the same size and serial number so the old
		 * DRI2Buffer will be returned, even though the backing buffer
		 * has been deleted.
		 */
		rootPixmap->drawable.serialNumber = NEXT_SERIAL_NUMBER;
	}
	TRACE_EXIT();
	return TRUE;
}

Bool drmmode_xf86crtc_resize(ScrnInfoPtr pScrn, int width, int height)
{
    int i;

    struct ARMSOCRec * ls = loongsonPTR(pScrn);

    drmmode_ptr drmmode = &ls->drmmode;

    uint32_t old_fb_id = drmmode->fb_id;

    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);

    TRACE_ENTER();

    if (!resize_scanout_bo(pScrn, width, height))
        goto fail;

    /* Framebuffer needs to be reset on all CRTCs, not just
     * those that have repositioned */

    for (i = 0; i < xf86_config->num_crtc; i++)
    {
        xf86CrtcPtr crtc = xf86_config->crtc[i];

        if (!crtc->enabled)
            continue;

        drmmode_set_mode_major(crtc, &crtc->mode,
                                crtc->rotation, crtc->x, crtc->y);
    }

/*
    if (old_fb_id)
    {
        drmModeRmFB(drmmode->fd, old_fb_id);
        drmmode_bo_destroy(drmmode, &old_front);
    }
*/

    TRACE_EXIT();
    return TRUE;

fail:
    TRACE_EXIT();
    return FALSE;
}

static const xf86CrtcConfigFuncsRec drmmode_xf86crtc_config_funcs = {
		.resize = drmmode_xf86crtc_resize
};


Bool drmmode_pre_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode, int cpp)
{
    int i;
    int ret;
    uint64_t value = 0;
    drmModeResPtr mode_res;

    struct ARMSOCRec *pLS;

    TRACE_ENTER();

    /* check for dumb capability */
    ret = drmGetCap(drmmode->fd, DRM_CAP_DUMB_BUFFER, &value);
    if (ret > 0 || value != 1)
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "KMS doesn't support dumb interface.\n");
        return FALSE;
    }

    // Allocate an xf86CrtcConfig
    xf86CrtcConfigInit(pScrn, &drmmode_xf86crtc_config_funcs);

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "PreInit: byte per pixel = %d.\n", cpp);

    // suijingfeng: why we need cache pScrn
    drmmode->scrn = pScrn;
    drmmode->cpp = cpp;
    mode_res = drmModeGetResources(drmmode->fd);

    if (NULL == mode_res)
    {
        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "drmModeGetResources failed.\n");
        return FALSE;
    }
    else
    {
        xf86DrvMsg(pScrn->scrnIndex, X_INFO, " ----------------------------\n");

        xf86DrvMsg(pScrn->scrnIndex, X_INFO, " Got KMS resources.\n");
        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "  %d Connectors, %d Encoders.\n",
                mode_res->count_connectors, mode_res->count_encoders);

        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "  %d CRTCs, %d FBs.\n",
                mode_res->count_crtcs, mode_res->count_fbs);

        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "  %dx%d minimum resolution.\n",
                mode_res->min_width, mode_res->min_height);

        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "  %dx%d maximum resolution.\n",
                mode_res->max_width, mode_res->max_height);

        xf86DrvMsg(pScrn->scrnIndex, X_INFO, " ----------------------------\n");
    }

    xf86CrtcSetSizeRange(pScrn, 320, 200, mode_res->max_width, mode_res->max_height);


    pLS = loongsonPTR(pScrn);

    if (pLS->crtcNum == -1)
    {
        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Adding all CRTCs\n");
        for (i = 0; i < mode_res->count_crtcs; i++)
            drmmode_crtc_init(pScrn, drmmode, mode_res, i);
    }
    else if (pLS->crtcNum < mode_res->count_crtcs)
    {
        drmmode_crtc_init(pScrn, drmmode, mode_res, pLS->crtcNum);
    }
    else
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                "Specified Screens in xorg.conf is more than DRM CRTCs\n");
        return FALSE;
    }


    for (i = 0; i < mode_res->count_connectors; ++i)
    {
        drmmode_output_init(pScrn, drmmode, mode_res, i);
    }

    /* workout clones */
    drmmode_clones_init(pScrn, drmmode, mode_res);

    drmModeFreeResources(mode_res);

    /* XF86_CRTC_VERSION >= 5 */
    xf86ProviderSetup(pScrn, NULL, "loongson");

    xf86InitialConfiguration(pScrn, TRUE);

    TRACE_EXIT();

    return TRUE;
}


void drmmode_adjust_frame(ScrnInfoPtr pScrn, int x, int y)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
    xf86OutputPtr output = config->output[config->compat_output];
    xf86CrtcPtr crtc = output->crtc;

    if (crtc && crtc->enabled) {
        drmmode_set_mode_major(crtc, &crtc->mode, crtc->rotation, x, y);
    }
}


/*
 * Page Flipping
 */

static void
page_flip_handler(int fd, unsigned int sequence, unsigned int tv_sec,
		unsigned int tv_usec, void *user_data)
{
	ARMSOCDRI2SwapComplete(user_data);
}

static void
vblank_handler(int fd, unsigned int sequence, unsigned int tv_sec,
		unsigned int tv_usec, void *user_data)
{
	ARMSOCDRI2VBlankHandler(sequence, tv_sec, tv_usec, user_data);
}

drmEventContext event_context = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = page_flip_handler,
		.vblank_handler = vblank_handler,
};



/*
 * Hot Plug Event handling:
 * TODO: MIDEGL-1441: Do we need to keep this handler, which
 * Rob originally wrote?
 */
static void
drmmode_handle_uevents(int fd, void *closure)
{
	ScrnInfoPtr pScrn = closure;
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
	struct drmmode_rec *drmmode = drmmode_from_scrn(pScrn);
	struct udev_device *dev;
	const char *hotplug;
	struct stat s;
	dev_t udev_devnum;

	dev = udev_monitor_receive_device(drmmode->uevent_monitor);
	if (!dev)
		return;

	/*
	 * Check to make sure this event is directed at our
	 * device (by comparing dev_t values), then make
	 * sure it's a hotplug event (HOTPLUG=1)
	 */
	udev_devnum = udev_device_get_devnum(dev);
	if (fstat(pARMSOC->drmFD, &s)) {
		ERROR_MSG("fstat failed: %s", strerror(errno));
		udev_device_unref(dev);
		return;
	}

	hotplug = udev_device_get_property_value(dev, "HOTPLUG");

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "hotplug=%s, match=%d\n", hotplug,
			!memcmp(&s.st_rdev, &udev_devnum, sizeof(dev_t)));

	if (memcmp(&s.st_rdev, &udev_devnum, sizeof(dev_t)) == 0 &&
			hotplug && atoi(hotplug) == 1) {
		RRGetInfo(xf86ScrnToScreen(pScrn), TRUE);
	}
	udev_device_unref(dev);
}


void drmmode_uevent_init(ScrnInfoPtr pScrn, struct drmmode_rec *drmmode)
{
    //  = drmmode_from_scrn(pScrn);
    struct udev *u;
    struct udev_monitor *mon;

    TRACE_ENTER();

    u = udev_new();
    if (!u)
    {
        return;
    }

    mon = udev_monitor_new_from_netlink(u, "udev");
    if (!mon)
    {
        udev_unref(u);
        return;
    }

    if (udev_monitor_filter_add_match_subsystem_devtype(mon, "drm",  "drm_minor") < 0
         || ( udev_monitor_enable_receiving(mon) < 0) )
    {
        udev_monitor_unref(mon);
        udev_unref(u);
        return;
    }

    drmmode->uevent_handler =
        xf86AddGeneralHandler(udev_monitor_get_fd(mon),
                drmmode_handle_uevents, drmmode);

    drmmode->uevent_monitor = mon;

    TRACE_EXIT();
}


void drmmode_uevent_fini(ScrnInfoPtr pScrn)
{
    struct drmmode_rec *drmmode = drmmode_from_scrn(pScrn);

    TRACE_ENTER();

    if (drmmode->uevent_handler) {
        struct udev *u = udev_monitor_get_udev(drmmode->uevent_monitor);
        xf86RemoveGeneralHandler(drmmode->uevent_handler);

        udev_monitor_unref(drmmode->uevent_monitor);
        udev_unref(u);
    }

    TRACE_EXIT();
}


static void
drmmode_notify_fd(int fd, int notify, void *data)
{
	drmHandleEvent(fd, &event_context);
}


void drmmode_init_wakeup_handler(struct ARMSOCRec *pARMSOC)
{
	SetNotifyFd(pARMSOC->drmFD, drmmode_notify_fd, X_NOTIFY_READ, pARMSOC);
}

void drmmode_fini_wakeup_handler(struct ARMSOCRec *pARMSOC)
{
	RemoveNotifyFd(pARMSOC->drmFD);
}

void
drmmode_wait_for_event(ScrnInfoPtr pScrn)
{
	struct drmmode_rec *drmmode = drmmode_from_scrn(pScrn);
	drmHandleEvent(drmmode->fd, &event_context);
}

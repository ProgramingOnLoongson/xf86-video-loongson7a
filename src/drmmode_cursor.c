#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <stdint.h>
#include <xf86drmMode.h>
#include <xf86str.h>
#include <xf86drm.h>
#include <xf86Crtc.h>
#include <drm_fourcc.h>

#include "loongson_driver.h"
#include "drmmode_display.h"

//
// we use software cursor, those funcs are not used, 
// isolate them. but still leaving them here. for reference
//

/* (Optional) Initialize the hardware cursor plane.
 *
 * When cursor_api is HWCURSOR_API_PLANE, this function should do any
 * plane initialization necessary, for example setting the z-order on the
 * plane to appear above all other layers. If this function fails the driver
 * falls back to using a software cursor.
 *
 * If cursor_api is not HWCURSOR_API_PLANE this function should be omitted.
 *
 * @param drm_fd   The DRM device file
 * @param plane_id The plane to initialize
 * @return 0 on success, non-zero on failure
 */
static int LS7A_InitPlaneForCursor(int drm_fd, uint32_t plane_id)
{
	int res = -1;
	drmModeObjectPropertiesPtr props;
	props = drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
	if (props)
	{
		uint32_t i;

		for (i = 0; i < props->count_props; ++i)
		{
			drmModePropertyPtr this_prop;
			this_prop = drmModeGetProperty(drm_fd, props->props[i]);

			if (this_prop)
			{
				if (!strncmp(this_prop->name, "zorder", DRM_PROP_NAME_LEN))
				{
					res = drmModeObjectSetProperty(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, this_prop->prop_id, 1);
					drmModeFreeProperty(this_prop);
					break;
				}
				drmModeFreeProperty(this_prop);
			}
		}
		drmModeFreeObjectProperties(props);
	}
	return res;
}

static struct drmmode_rec * drmmode_from_scrn(ScrnInfoPtr pScrn)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	struct drmmode_crtc_private_rec *drmmode_crtc;

	drmmode_crtc = xf86_config->crtc[0]->driver_private;
	return drmmode_crtc->drmmode;
}

static Bool cursor_init_standard(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
	struct drmmode_rec *drmmode = drmmode_from_scrn(pScrn);
	struct drmmode_cursor_rec *cursor;
	int w, h;

    if (drmmode->cursor) {
        xf86DrvMsg(pScrn->scrnIndex, X_INFO, 
                "cursor already initialized\n");
        return TRUE;
    }

	if (!xf86LoaderCheckSymbol("drmModeSetCursor") ||
	    !xf86LoaderCheckSymbol("drmModeMoveCursor"))
	{
		 xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
            "Standard HW cursor needs libdrm 2.4.3 or higher)");
		return FALSE;
	}

	cursor = calloc(1, sizeof(struct drmmode_cursor_rec));
	if (!cursor) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
            "HW cursor (standard): calloc failed");
		return FALSE;
	}

	w = pARMSOC->cursor_width;
	h = pARMSOC->cursor_height;

	/* allow for cursor padding in the bo */
	cursor->bo  = armsoc_bo_new_with_dim(pARMSOC->drmFD,
		w, h, 0, 32);

	if (!cursor->bo) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
            "HW cursor (standard): buffer allocation failed");
		free(cursor);
		return FALSE;
	}

	cursor->handle = armsoc_bo_handle(cursor->bo);

	if (!xf86_cursors_init(pScreen, w, h, HARDWARE_CURSOR_ARGB)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                "xf86_cursors_init() failed");
		if (drmModeRmFB(drmmode->fd, cursor->fb_id))
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                    "drmModeRmFB() failed");

		armsoc_bo_unreference(cursor->bo);
		free(cursor);
		return FALSE;
	}

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "HW cursor initialized.\n");
	drmmode->cursor = cursor;
	return TRUE;
}



static Bool cursor_init_plane(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);
	struct drmmode_rec *drmmode = drmmode_from_scrn(pScrn);
	struct drmmode_cursor_rec *cursor;
	drmModePlaneRes *plane_resources;
	drmModePlane *ovr;
	int w, h;
	uint32_t handles[4], pitches[4], offsets[4]; /* we only use [0] */

	if (drmmode->cursor) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "cursor already initialized");
		return TRUE;
	}

	if (!xf86LoaderCheckSymbol("drmModeGetPlaneResources")) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"HW cursor not supported (needs libdrm 2.4.30 or higher)");
		return FALSE;
	}

	/* find an unused plane which can be used as a mouse cursor.  Note
	 * that we cheat a bit, in order to not burn one overlay per crtc,
	 * and only show the mouse cursor on one crtc at a time
	 */
	plane_resources = drmModeGetPlaneResources(drmmode->fd);
	if (!plane_resources) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"HW cursor: drmModeGetPlaneResources failed: %s",
						strerror(errno));
		return FALSE;
	}

	if (plane_resources->count_planes < 1) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"not enough planes for HW cursor");
		drmModeFreePlaneResources(plane_resources);
		return FALSE;
	}

	ovr = drmModeGetPlane(drmmode->fd, plane_resources->planes[0]);
	if (!ovr) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                "HW cursor: drmModeGetPlane failed: %s\n",
                strerror(errno));
		drmModeFreePlaneResources(plane_resources);
		return FALSE;
	}
/*
	if (pARMSOC->init_plane_for_cursor &&
		pARMSOC->init_plane_for_cursor(drmmode->fd, ovr->plane_id))
	{
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"Failed driver-specific cursor initialization");
		drmModeFreePlaneResources(plane_resources);
		return FALSE;
	}
*/
	cursor = calloc(1, sizeof(struct drmmode_cursor_rec));
	if (!cursor) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"HW cursor: calloc failed");
		drmModeFreePlane(ovr);
		drmModeFreePlaneResources(plane_resources);
		return FALSE;
	}

	cursor->ovr = ovr;

	w = pARMSOC->cursor_width;
	h = pARMSOC->cursor_height;

	/* allow for cursor padding in the bo */
	cursor->bo = armsoc_bo_new_with_dim(pARMSOC->drmFD, w, h, 0, 32);

	if (NULL == cursor->bo)
	{
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"HW cursor: buffer allocation failed");
		free(cursor);
		drmModeFreePlane(ovr);
		drmModeFreePlaneResources(plane_resources);
		return FALSE;
	}

	handles[0] = armsoc_bo_handle(cursor->bo);
	pitches[0] = armsoc_bo_pitch(cursor->bo);
	offsets[0] = 0;

	/* allow for cursor padding in the fb */
	if (drmModeAddFB2(drmmode->fd, w , h, DRM_FORMAT_ARGB8888,
			handles, pitches, offsets, &cursor->fb_id, 0)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"HW cursor: drmModeAddFB2 failed: %s",
					strerror(errno));
		armsoc_bo_unreference(cursor->bo);
		free(cursor);
		drmModeFreePlane(ovr);
		drmModeFreePlaneResources(plane_resources);
		return FALSE;
	}

	if (!xf86_cursors_init(pScreen, w, h, HARDWARE_CURSOR_ARGB))
	{
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"xf86_cursors_init() failed");
		if (drmModeRmFB(drmmode->fd, cursor->fb_id))
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"drmModeRmFB() failed");

		armsoc_bo_unreference(cursor->bo);
		free(cursor);
		drmModeFreePlane(ovr);
		drmModeFreePlaneResources(plane_resources);
		return FALSE;
	}

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "HW cursor initialized");
	drmmode->cursor = cursor;
	drmModeFreePlaneResources(plane_resources);
	return TRUE;
}




/* Specifies the hardware cursor api used by the DRM :
 *   HWCURSOR_API_PLANE    - Use planes.
 *   HWCURSOR_API_STANDARD - Use the standard API : drmModeSetCursor() & drmModeMoveCursor().
 *   HWCURSOR_API_NONE     - No hardware cursor - use a software cursor.
 */

/*
enum hwcursor_api {
	HWCURSOR_API_PLANE = 0,
	HWCURSOR_API_STANDARD = 1,
	HWCURSOR_API_NONE = 2
};


enum hwcursor_api cursor_api;


Bool drmmode_cursor_init(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct ARMSOCRec *pARMSOC = ARMSOCPTR(pScrn);

	switch (pARMSOC->cursor_api)
	{
		case HWCURSOR_API_PLANE:
			return cursor_init_plane(pScreen);
		case HWCURSOR_API_STANDARD:
			return cursor_init_standard(pScreen);
		case HWCURSOR_API_NONE:
			return FALSE;
		default:
			assert(0);
	}
}

*/


// the suffix K stand for Kernel
void LS_GetCursorDimK(ScrnInfoPtr pScrn)
{
    // cusor related
    // if (xf86ReturnOptValBool(pLS->drmmode.Options, OPTION_SW_CURSOR, FALSE)) {
    //    pLS->drmmode.sw_cursor = TRUE;
    // }

    int ret;
    uint64_t value;

    loongsonRecPtr pLS = loongsonPTR(pScrn);

    /* Cursor dimensions
     * Technically we probably don't have any size limit,
     * since we are just using an overlay. But xserver will
     * always create cursor images in the max size,
     * so don't use width/height values that are too big
     */
    pLS->cursor_width = 64;
    pLS->cursor_height = 64;

    ret = drmGetCap(pLS->drmFD, DRM_CAP_CURSOR_WIDTH, &value);
    if (!ret)
    {
        pLS->cursor_width = value;
    }

    ret = drmGetCap(pLS->drmFD, DRM_CAP_CURSOR_HEIGHT, &value);
    if (!ret)
    {
        pLS->cursor_height = value;
    }

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
            "Cursor: width x height = %dx%d\n",
            pLS->cursor_width, pLS->cursor_height );
}

#ifndef DRMMODE_DISPLAY_H
#define DRMMODE_DISPLAY_H
#include <xf86drmMode.h>

struct drmmode_cursor_rec {
	/* hardware cursor: */
	struct dumb_bo *bo;
	int x, y;
	 /* These are used for HWCURSOR_API_PLANE */
	drmModePlane *ovr;
	uint32_t fb_id;
	/* This is used for HWCURSOR_API_STANDARD */
	uint32_t handle;
};

typedef struct {
    uint32_t width;
    uint32_t height;
    struct dumb_bo *dumb;
} drmmode_bo;


struct drmmode_rec {
        int fd;
        uint32_t fb_id;
        drmModeFBPtr mode_fb;
        int cpp;
        int kbpp;
        ScrnInfoPtr scrn;
        struct udev_monitor *uevent_monitor;
        InputHandlerProc uevent_handler;
        struct drmmode_cursor_rec *cursor;
        drmEventContext event_context;
        drmmode_bo front_bo;
        Bool sw_cursor;

        /* Broken-out options. */
        OptionInfoPtr Options;

        Bool glamor;
        Bool shadow_enable;
        Bool shadow_enable2;
        /* DRM page flip events should be requested and
         * waited for during DRM_IOCTL_MODE_PAGE_FLIP. */
        Bool pageflip;
        Bool force_24_32;
        void *shadow_fb;
        void *shadow_fb2;

        DevPrivateKeyRec pixmapPrivateKeyRec;
        DevScreenPrivateKeyRec spritePrivateKeyRec;
        /* Number of SW cursors currently visible on this screen */
        int sprites_visible;

        Bool reverse_prime_offload_mode;

        Bool is_secondary;

        PixmapPtr fbcon_pixmap;

        Bool dri2_flipping;
        Bool present_flipping;
        Bool flip_bo_import_failed;

        Bool dri2_enable;
        Bool present_enable;
};


typedef struct drmmode_rec * drmmode_ptr;

struct drmmode_crtc_private_rec {
	struct drmmode_rec *drmmode;
	uint32_t crtc_id;
	int cursor_visible;
	/* settings retained on last good modeset */
	int last_good_x;
	int last_good_y;
	Rotation last_good_rotation;
	DisplayModePtr last_good_mode;

	int dpms_mode;
	uint32_t vblank_pipe;

	uint16_t lut_r[256], lut_g[256], lut_b[256];
    /**
     * @{ MSC (vblank count) handling for the PRESENT extension.
     *
     * The kernel's vblank counters are 32 bits and apparently full of
     * lies, and we need to give a reliable 64-bit msc for GL, so we
     * have to track and convert to a userland-tracked 64-bit msc.
     */
    int32_t vblank_offset;
    uint32_t msc_prev;
    uint64_t msc_high;
/** @} */
    Bool need_modeset;
    struct xorg_list mode_list;
	/* properties that we care about: */
	uint32_t prop_rotation;
};

struct drmmode_prop_rec {
	drmModePropertyPtr mode_prop;
	/* Index within the kernel-side property arrays for this connector. */
	int index;
	/* if range prop, num_atoms == 1;
	 * if enum prop, num_atoms == num_enums + 1
	 */
	int num_atoms;
	Atom *atoms;
};

struct drmmode_output_priv {
    struct drmmode_rec *drmmode;
    int output_id;
    drmModeConnectorPtr connector;
    drmModeEncoderPtr *encoders;
    drmModePropertyBlobPtr edid_blob;
    int num_props;
    struct drmmode_prop_rec *props;
    int enc_mask;   /* encoders present (mask of encoder indices) */
    int enc_clones; /* encoder clones possible (mask of encoder indices) */
};

typedef struct drmmode_output_priv drmmode_output_private_rec;
typedef struct drmmode_output_priv * drmmode_output_private_ptr;


#endif

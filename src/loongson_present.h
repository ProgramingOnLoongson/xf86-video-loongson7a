#ifndef LOONGSON_PRESENT_H_
#define LOONGSON_PRESENT_H_

#include <xf86Crtc.h>

// pageflip
Bool drmmode_page_flip(ScreenPtr screen, DrawablePtr draw,
        uint32_t fb_id, Bool sync_flip, void *priv);
Bool ms_crtc_on(xf86CrtcPtr crtc);

// Present
Bool LS_PresentScreenInit(ScreenPtr screen);
#endif

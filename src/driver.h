#ifndef LS_DRIVER_H_
#define LS_DRIVER_H_


#include <drm.h>
#include <xf86drm.h>
#include <xf86Crtc.h>

typedef enum ms_queue_flag {
    MS_QUEUE_ABSOLUTE = 0,
    MS_QUEUE_RELATIVE = 1,
    MS_QUEUE_NEXT_ON_MISS = 2
} ms_queue_flag;

struct armsoc_present_vblank_event
{
    uint64_t event_id;
    Bool unflip;
};

typedef void (*armsoc_drm_handler_proc)(uint64_t frame,
                                        uint64_t usec,
                                        void *data);

typedef void (*armsoc_drm_abort_proc)(void *data);

uint64_t armsoc_kernel_msc_to_crtc_msc(xf86CrtcPtr crtc, 
uint32_t sequence, Bool is64bit);

int armsoc_get_crtc_ust_msc(xf86CrtcPtr crtc, CARD64 *ust, CARD64 *msc);

void armsoc_drm_abort_event(ScrnInfoPtr scrn, uint64_t event_id);

Bool ls_queue_vblank(xf86CrtcPtr crtc, ms_queue_flag flags,
                uint64_t msc, uint64_t *msc_queued, uint32_t seq,
                struct armsoc_present_vblank_event *event);

uint32_t armsoc_drm_queue_alloc(xf86CrtcPtr crtc,
                       void *data,
                       armsoc_drm_handler_proc handler,
                       armsoc_drm_abort_proc abort);

Bool ls_vblank_screen_init(ScreenPtr screen);
#endif

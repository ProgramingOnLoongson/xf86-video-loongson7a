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


#include <errno.h>
#include <xorg-server.h>
#include <xf86.h>
#include <xf86drm.h>
#include <xf86str.h>

#include <sys/poll.h>

#include "loongson_driver.h"
#include "drmmode_display.h"
#include "driver.h"


#define VBLANK_DBG_MSG(fmt, ...)
/*#define VBLANK_DBG_MSG(fmt, ...) \
		do { xf86Msg(X_INFO, fmt "\n",\
				##__VA_ARGS__); } while (0)
*/

extern drmEventContext event_context;

struct armsoc_drm_queue {
	struct xorg_list list;
	xf86CrtcPtr crtc;
	uint32_t seq;
	void *data;
	ScrnInfoPtr scrn;
	armsoc_drm_handler_proc handler;
	armsoc_drm_abort_proc abort;
};

static struct xorg_list armsoc_drm_queue;

static uint32_t armsoc_drm_seq;


/*
 * Enqueue a potential drm response; when the associated response
 * appears, we've got data to pass to the handler from here
 */
uint32_t armsoc_drm_queue_alloc(xf86CrtcPtr crtc,
                       void *data,
                       armsoc_drm_handler_proc handler,
                       armsoc_drm_abort_proc abort)
{
	ScreenPtr screen = crtc->randr_crtc->pScreen;
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	struct armsoc_drm_queue *q;

	q = calloc(1, sizeof(struct armsoc_drm_queue));

	if (!q)
		return 0;
	if (!armsoc_drm_seq)
		++armsoc_drm_seq;

	q->seq = armsoc_drm_seq++;
	q->scrn = scrn;
	q->crtc = crtc;
	q->data = data;
	q->handler = handler;
	q->abort = abort;

	xorg_list_add(&q->list, &armsoc_drm_queue);

	return q->seq;
}

/*
 * Flush the DRM event queue when full; makes space for new events.
 *
 * Returns a negative value on error, 0 if there was nothing to process,
 * or 1 if we handled any events.
 */
static int ls_flush_drm_events(ScreenPtr screen)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	struct ARMSOCRec * pARMSOC = ARMSOCPTR(scrn);

	struct pollfd p = { .fd = pARMSOC->drmFD, .events = POLLIN };
	int r;

	VBLANK_DBG_MSG("flush_drm_events");

	do {
		r = poll(&p, 1, 0);
	} while (r == -1 && (errno == EINTR || errno == EAGAIN));

	/* If there was an error, r will be < 0.  Return that.  If there was
	 * nothing to process, r == 0.  Return that.
	 */
	if (r <= 0)
		return r;

	/* Try to handle the event.  If there was an error, return it. */
	r = drmHandleEvent(pARMSOC->drmFD, &event_context);
	if (r < 0)
		return r;

	/* Otherwise return 1 to indicate that we handled an event. */
	return 1;
}


static Bool get_kernel_ust_msc(xf86CrtcPtr crtc, uint32_t *msc, uint64_t *ust)
{
	ScreenPtr screen = crtc->randr_crtc->pScreen;
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	struct ARMSOCRec * pARMSOC = ARMSOCPTR(scrn);
	struct drmmode_crtc_private_rec * drmmode_crtc = crtc->driver_private;
	drmVBlank vbl;
	int ret;

	/* Get current count */
	vbl.request.type = DRM_VBLANK_RELATIVE | drmmode_crtc->vblank_pipe;
	vbl.request.sequence = 0;
	vbl.request.signal = 0;

	ret = drmWaitVBlank(pARMSOC->drmFD, &vbl);

	if (ret)
	{
		*msc = 0;
		*ust = 0;
		return FALSE;
	}
	else
	{
		*msc = vbl.reply.sequence;
		*ust = (CARD64) vbl.reply.tval_sec * 1000000 + vbl.reply.tval_usec;
		VBLANK_DBG_MSG(" get_kernel_ust_msc msc:%d ust:%d", *msc, *ust);
		return TRUE;
	}
}


/**
 * Convert a 32-bit kernel MSC sequence number to a 64-bit local sequence
 * number, adding in the vblank_offset and high 32 bits, and dealing
 * with 64-bit wrapping
 */
uint64_t armsoc_kernel_msc_to_crtc_msc(xf86CrtcPtr crtc, uint32_t sequence, Bool is64bit)
{
	struct drmmode_crtc_private_rec *drmmode_crtc = crtc->driver_private;
	sequence += drmmode_crtc->vblank_offset;

	if ((int32_t) (sequence - drmmode_crtc->msc_prev) < -0x40000000)
		drmmode_crtc->msc_high += 0x100000000L;
	drmmode_crtc->msc_prev = sequence;
	return drmmode_crtc->msc_high + sequence;
}


int armsoc_get_crtc_ust_msc(xf86CrtcPtr crtc, CARD64 *ust, CARD64 *msc)
{
	uint32_t kernel_msc;

	if (!get_kernel_ust_msc(crtc, &kernel_msc, ust))
		return BadMatch;
	*msc = armsoc_kernel_msc_to_crtc_msc(crtc, kernel_msc, 0);

	return Success;
}


void armsoc_drm_abort_event(ScrnInfoPtr scrn, uint64_t event_id)
{
	struct armsoc_drm_queue *q, *tmp;

	xorg_list_for_each_entry_safe(q, tmp, &armsoc_drm_queue, list)
	{
		struct armsoc_present_vblank_event *event = q->data;
		if (event->event_id == event_id)
		{
			xorg_list_del(&q->list);
			q->abort(q->data);
			free(q);
			break;
		}
	}
}



/**
 * Abort one queued DRM entry, removing it from the list, 
 * calling the abort function and freeing the memory
 */
static void ms_drm_abort_one(struct armsoc_drm_queue *q)
{
        xorg_list_del(&q->list);
        q->abort(q->data);
        free(q);
}

/**
 * Abort by drm queue sequence number.
 */
static void ls_drm_abort_seq(ScrnInfoPtr scrn, uint32_t seq)
{
    struct armsoc_drm_queue *q, *tmp;

    xorg_list_for_each_entry_safe(q, tmp, &armsoc_drm_queue, list)
    {
        if (q->seq == seq) {
            ms_drm_abort_one(q);
            break;
        }
    }

/*
	struct armsoc_drm_queue *q, *tmp;

	xorg_list_for_each_entry_safe(q, tmp, &armsoc_drm_queue, list)
	{
		struct armsoc_present_vblank_event *event = q->data;
		if (event->event_id == event_id)
		{
			xorg_list_del(&q->list);
			q->abort(q->data);
			free(q);
			break;
		}
	}
}
*/
}


#define MAX_VBLANK_OFFSET 1000
/**
 * Convert a 64-bit adjusted MSC value into a 32-bit kernel sequence number,
 * removing the high 32 bits and subtracting out the vblank_offset term.
 *
 * This also updates the vblank_offset when it notices that the value should
 * change.
 */
static uint32_t armsoc_crtc_msc_to_kernel_msc(xf86CrtcPtr crtc, uint64_t expect)
{
	struct drmmode_crtc_private_rec *drmmode_crtc = crtc->driver_private;
	uint64_t msc;
	uint64_t ust;
	int64_t diff;

	if (armsoc_get_crtc_ust_msc(crtc, &ust, &msc) == Success)
	{
		diff = expect - msc;
		/* We're way off here, assume that the kernel has lost its mind
		 * and smack the vblank back to something sensible
		 */
		if (diff < -MAX_VBLANK_OFFSET || MAX_VBLANK_OFFSET < diff)
		{
			drmmode_crtc->vblank_offset += (int32_t) diff;
			if (drmmode_crtc->vblank_offset > -MAX_VBLANK_OFFSET &&
			        drmmode_crtc->vblank_offset < MAX_VBLANK_OFFSET)
				drmmode_crtc->vblank_offset = 0;
		}
	}
	return (uint32_t) (expect - drmmode_crtc->vblank_offset);
}


Bool ls_queue_vblank(xf86CrtcPtr crtc, ms_queue_flag flags,
                uint64_t msc, uint64_t *msc_queued, uint32_t seq, 
                struct armsoc_present_vblank_event *event)
{

    ScreenPtr screen = crtc->randr_crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct ARMSOCRec * pARMSOC = ARMSOCPTR(scrn);

    struct drmmode_crtc_private_rec * drmmode_crtc = crtc->driver_private;
    drmVBlank vbl;
    int ret;

/*
    for (;;) {
        // Queue an event at the specified sequence 
        if (ms->has_queue_sequence || !ms->tried_queue_sequence) {
            uint32_t drm_flags = 0;
            uint64_t kernel_queued;

            ms->tried_queue_sequence = TRUE;

            if (flags & MS_QUEUE_RELATIVE)
                drm_flags |= DRM_CRTC_SEQUENCE_RELATIVE;
            if (flags & MS_QUEUE_NEXT_ON_MISS)
                drm_flags |= DRM_CRTC_SEQUENCE_NEXT_ON_MISS;

            ret = drmCrtcQueueSequence(ms->fd, drmmode_crtc->mode_crtc->crtc_id,
                                       drm_flags, msc, &kernel_queued, seq);
            if (ret == 0) {
                if (msc_queued)
                    *msc_queued = ms_kernel_msc_to_crtc_msc(crtc, kernel_queued, TRUE);
                ms->has_queue_sequence = TRUE;
                return TRUE;
            }

            if (ret != -1 || (errno != ENOTTY && errno != EINVAL)) {
                ms->has_queue_sequence = TRUE;
                goto check;
            }
        }
        vbl.request.type = DRM_VBLANK_EVENT | drmmode_crtc->vblank_pipe;
        if (flags & MS_QUEUE_RELATIVE)
            vbl.request.type |= DRM_VBLANK_RELATIVE;
        else
            vbl.request.type |= DRM_VBLANK_ABSOLUTE;
        if (flags & MS_QUEUE_NEXT_ON_MISS)
            vbl.request.type |= DRM_VBLANK_NEXTONMISS;

        vbl.request.sequence = msc;
        vbl.request.signal = seq;
        ret = drmWaitVBlank(ms->fd, &vbl);
        if (ret == 0) {
            if (msc_queued)
                *msc_queued = ms_kernel_msc_to_crtc_msc(crtc, vbl.reply.sequence, FALSE);
            return TRUE;
        }
    check:
        if (errno != EBUSY) {
            ms_drm_abort_seq(scrn, seq);
            return FALSE;
        }
        ms_flush_drm_events(screen);
    }
*/


	for (;;)
	{
		vbl.request.type = DRM_VBLANK_EVENT | drmmode_crtc->vblank_pipe;
		if (flags & MS_QUEUE_RELATIVE)
			vbl.request.type |= DRM_VBLANK_RELATIVE;
		else
			vbl.request.type |= DRM_VBLANK_ABSOLUTE;

		vbl.request.sequence = armsoc_crtc_msc_to_kernel_msc(crtc, msc);
		// warnning: this is original
		vbl.request.signal = (unsigned long)event;
		// suijingfeng: changed here
		// vbl.request.signal = seq;

		ret = drmWaitVBlank(pARMSOC->drmFD, &vbl);
		if (0 == ret)
		{
			if (msc_queued)
				*msc_queued = armsoc_kernel_msc_to_crtc_msc(crtc, vbl.reply.sequence, TRUE);
			return TRUE;
		}

		VBLANK_DBG_MSG("present_queue_vblank drmWaitVBlank %d", ret);

		/* If we hit EBUSY, then try to flush events.  If we can't, then
		 * this is an error.
		 */

		if (errno != EBUSY )
		{
			VBLANK_DBG_MSG("abort %d", errno);
			// warnning: original
			// armsoc_drm_abort_event(scrn, event_id);
			// suijingfeng: replaced with following
			ls_drm_abort_seq(scrn, seq);
			return FALSE;
		}
		ls_flush_drm_events(screen);
	}
    return TRUE;
}

/*
 * General DRM kernel handler. Looks for the matching sequence number in the
 * drm event queue and calls the handler for it.
 */

static void DrmHandler(int fd, uint32_t frame, uint32_t sec, uint32_t usec,
                   void *user_ptr)
{
	struct armsoc_drm_queue *q, *tmp;

	VBLANK_DBG_MSG("drm_handler fd:%d frame:%d sec:%d usec:%d", 
                fd, frame, sec, usec);

	xorg_list_for_each_entry_safe(q, tmp, &armsoc_drm_queue, list)
	{
		if (q->data == user_ptr)
		{
			uint64_t msc;

			msc = armsoc_kernel_msc_to_crtc_msc(q->crtc, frame, 0);
			xorg_list_del(&q->list);
			q->handler(msc, (uint64_t) sec * 1000000 + usec, q->data);
			free(q);
			break;
		}
	}
}



Bool ls_vblank_screen_init(ScreenPtr screen)
{
	xorg_list_init(&armsoc_drm_queue);

	event_context.version = DRM_EVENT_CONTEXT_VERSION;
	event_context.vblank_handler = DrmHandler;
	event_context.page_flip_handler = DrmHandler;

	return TRUE;
}


/*
Bool ls_vblank_screen_init(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    modesettingEntPtr ms_ent = ms_ent_priv(scrn);
    xorg_list_init(&ms_drm_queue);

    ms->event_context.version = 4;
    ms->event_context.vblank_handler = ms_drm_handler;
    ms->event_context.page_flip_handler = ms_drm_handler;
    ms->event_context.sequence_handler = ms_drm_sequence_handler_64bit;

    // We need to re-register the DRM fd for the synchronisation
    // feedback on every server generation, so perform the
    // registration within ScreenInit and not PreInit.

    if (ms_ent->fd_wakeup_registered != serverGeneration) {
        SetNotifyFd(ms->fd, ms_drm_socket_handler, X_NOTIFY_READ, screen);
        ms_ent->fd_wakeup_registered = serverGeneration;
        ms_ent->fd_wakeup_ref = 1;
    } else
        ms_ent->fd_wakeup_ref++;

    return TRUE;
}
*/

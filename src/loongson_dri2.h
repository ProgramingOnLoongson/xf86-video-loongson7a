#ifndef LOONGSON_DRI2_H_
#define LOONGSON_DRI2_H_


#define DRI2_BUFFER_FB_MASK         0x02 /* FB: 1, non-FB: 0 */
#define DRI2_BUFFER_MAPPED_MASK     0x04 /* mapped: 1, not-mapped: 0 */
#define DRI2_BUFFER_REUSED_MASK     0x08 /* re-used: 1, re-created: 0 */
#define DRI2_BUFFER_AGE_MASK        0x70 /* buffer age */
#define DRI2_BUFFER_FLAG_MASK       0x7f /* dri2 buffer flag mask */


#define DRI2_BUFFER_GET_FB(flag)                \
                    (((flag) & DRI2_BUFFER_FB_MASK) ? 1 : 0 )


#define DRI2_BUFFER_SET_FB(flag, fb)            \
                    (flag) |= (((fb) << 1) & DRI2_BUFFER_FB_MASK);


#define DRI2_BUFFER_GET_MAPPED(flag)            \
                    (((flag) & DRI2_BUFFER_MAPPED_MASK) ? 1 : 0)

#define DRI2_BUFFER_SET_MAPPED(flag, mapped)    \
                    (flag) |= (((mapped) << 2) & DRI2_BUFFER_MAPPED_MASK);


#define DRI2_BUFFER_GET_REUSED(flag)            \
                    (((flag) & DRI2_BUFFER_REUSED_MASK) ? 1 : 0)

#define DRI2_BUFFER_SET_REUSED(flag, reused)    \
                    (flag) |= (((reused) << 3) & DRI2_BUFFER_REUSED_MASK);


#define DRI2_BUFFER_GET_AGE(flag)               \
                    (((flag) & DRI2_BUFFER_AGE_MASK) >> 4 )

#define DRI2_BUFFER_SET_AGE(flag, age)          \
                    (flag) |= (((age) << 4) & DRI2_BUFFER_AGE_MASK);


/**
 * DRI2 functions..
 */
struct ARMSOCDRISwapCmd;
Bool ARMSOCDRI2ScreenInit(ScreenPtr pScreen);
void ARMSOCDRI2CloseScreen(ScreenPtr pScreen);
void ARMSOCDRI2SwapComplete(struct ARMSOCDRISwapCmd *cmd);
void ARMSOCDRI2ResizeSwapChain(ScrnInfoPtr pScrn,
    struct dumb_bo *old_bo, struct dumb_bo *resized_bo);

void ARMSOCDRI2VBlankHandler(unsigned int sequence,
    unsigned int tv_sec, unsigned int tv_usec, void *user_data);

void set_scanout_bo(ScrnInfoPtr pScrn, struct dumb_bo *bo);

#endif

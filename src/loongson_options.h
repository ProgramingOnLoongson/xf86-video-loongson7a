#ifndef LOONGSON_OPTIONS_H_
#define LOONGSON_OPTIONS_H_

#include <xf86str.h>
#include <xf86Opt.h>

/** Supported options, as enum values. */
enum {
        OPTION_DEBUG,
        OPTION_PAGEFLIP,
        OPTION_DRI_NUM_BUF,
        OPTION_DRIVERNAME,
        OPTION_SOFT_EXA,
} loongsonOpts;


// Options
Bool LS_ProcessOptions(ScrnInfoPtr pScrn, OptionInfoPtr * ppOptions );
const OptionInfoRec * LS_AvailableOptions(int chipid, int busid);

#endif

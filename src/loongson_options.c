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
 *    Sui Jingfeng <suijingfeng@loongson.com>
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "loongson_driver.h"
#include "loongson_options.h"


/** Supported options. */
static const OptionInfoRec Options[] = {
    { OPTION_DEBUG,       "Debug",            OPTV_BOOLEAN, {0},   FALSE },
    { OPTION_PAGEFLIP,    "PageFlip",         OPTV_BOOLEAN, {0},   FALSE },
    { OPTION_DRI_NUM_BUF, "DRI2MaxBuffers",   OPTV_INTEGER, {-1},  FALSE },
    { OPTION_DRIVERNAME,  "KernelDriverName", OPTV_STRING,  {0},   FALSE },
    { OPTION_SOFT_EXA,    "SoftEXA",          OPTV_BOOLEAN, {0},   FALSE },
    { -1,                 NULL,               OPTV_NONE,    {0},   FALSE }
};


/**
 * It returns the available driver options to the "-configure" option,
 * so that an xorg.conf file can be built and the user can see which
 * options are available for them to use.
 */
const OptionInfoRec * LS_AvailableOptions(int chipid, int busid)
{
    return Options;
}


Bool LS_ProcessOptions(ScrnInfoPtr pScrn, OptionInfoPtr * ppOptions )
{
    /*
     * Process the "xorg.conf" file options:
     */
    OptionInfoPtr pTmpOps;

    xf86CollectOptions(pScrn, NULL);

    // pARMSOC->pOptionInfo = malloc(sizeof(Options));
    pTmpOps = malloc(sizeof(Options));

    if (pTmpOps == NULL )
    {
        return FALSE;
    }

    memcpy(pTmpOps, Options, sizeof(Options));

    *ppOptions = pTmpOps;

    xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, pTmpOps);

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Options Processed.\n");

    return TRUE;
}

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

#ifndef VIV2D_EXA_H
#define VIV2D_EXA_H


#include "loongson_exa.h"

typedef struct {
	struct ARMSOCEXARec base;
	ExaDriverPtr exa;
	/* add any other driver private data here.. */
	Viv2DPtr v2d;
} Viv2DEXARec, *Viv2DEXAPtr;

static inline Viv2DRec*
Viv2DPrivFromPixmap(PixmapPtr pPixmap)
{
	ScreenPtr pScreen = pPixmap->drawable.pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	loongsonRecPtr pARMSOC = loongsonPTR(pScrn);
	Viv2DEXAPtr exa = (Viv2DEXAPtr)(pARMSOC->pARMSOCEXA);
	Viv2DRec *v2d = exa->v2d;
	return v2d;
}

static inline Viv2DRec* Viv2DPrivFromARMSOC(struct ARMSOCRec *pARMSOC)
{
	Viv2DEXAPtr exa = (Viv2DEXAPtr)(pARMSOC->pARMSOCEXA);
	Viv2DRec *v2d = exa->v2d;
	return v2d;
}

static inline Viv2DRec*
Viv2DPrivFromScreen(ScreenPtr pScreen)
{
	Viv2DRec *v2d;
	Viv2DEXAPtr exa;
	struct ARMSOCRec *pARMSOC = ARMSOCPTR_FROM_SCREEN(pScreen);
	exa = (Viv2DEXAPtr)(pARMSOC->pARMSOCEXA);
	v2d = exa->v2d;
	return v2d;
}


struct ARMSOCEXARec *InitViv2DEXA(ScreenPtr pScreen, ScrnInfoPtr pScrn, int fd);

#endif

#  Copyright © 2020 Loongson Corporation
#
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  on the rights to use, copy, modify, merge, publish, distribute, sub
#  license, and/or sell copies of the Software, and to permit persons to whom
#  the Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice (including the next
#  paragraph) shall be included in all copies or substantial portions of the
#  Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
#  ADAM JACKSON BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
#  IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
#  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# Authors:
#    Ian Elliott <ianelliottus@yahoo.com>
#    Rob Clark <rob@ti.com>
#    suijingfeng <suijingfeng@loongson.cn>

# this is obnoxious:
# -module lets us name the module exactly how we want
# -avoid-version prevents gratuitous .0.0.0 version numbers on the end
# _ladir passes a dummy rpath to libtool so the thing will actually link
# TODO: -nostdlib/-Bstatic/-lgcc platform magic, not installing the .a, etc.


LOONGSON_CFLAGS = -O2 -march=mips64r2 -mtune=loongson3a -mel -mabi=64

AM_ASFLAGS = $(LOONGSON_CFLAGS)

AM_CFLAGS = @XORG_CFLAGS@ $(LOONGSON_CFLAGS)
loongson7a_drv_la_LTLIBRARIES = loongson7a_drv.la
loongson7a_drv_la_LDFLAGS = -module -avoid-version -no-undefined
loongson7a_drv_la_LIBADD = @XORG_LIBS@
loongson7a_drv_ladir = @moduledir@/drivers

loongson7a_drv_la_SOURCES = \
	driver.c \
	dumb_bo.c \
	drmmode_display.c \
	drmmode_cursor.c \
	fake_exa.c \
	loongson_dri2.c \
	loongson_dri3.c \
	loongson_present.c \
	vblank.c \
	pageflip.c \
	xv.c \
	viv2d/queue.c \
	viv2d/etnaviv_extra.c \
	viv2d/viv2d_exa.c \
	loongson_module.c \
	loongson_probe.c \
	loongson_options.c \
	loongson_entity.c \
	loongson_helpers.c \
	loongson_debug.c \
	loongson_pixmap.c

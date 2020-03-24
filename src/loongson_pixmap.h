#ifndef LOONGSON_PIXMAP_H_
#define LOONGSON_PIXMAP_H_


void * LS_CreateExaPixmap(ScreenPtr pScreen,
        int width, int height, int depth,
        int usage_hint, int bitsPerPixel,
        int *new_fb_pitch);


void * LS_CreateDumbPixmap(ScreenPtr pScreen,
        int width, int height, int depth,
        int usage_hint, int bitsPerPixel,
        int *new_fb_pitch );


Bool LS_ModifyDumbPixmapHeader( PixmapPtr pPixmap,
        int width, int height, int depth, int bitsPerPixel,
        int devKind, pointer pPixData );

Bool LS_ModifyExaPixmapHeader( PixmapPtr pPixmap,
        int width, int height, int depth,
        int bitsPerPixel, int devKind, pointer pPixData );


#endif

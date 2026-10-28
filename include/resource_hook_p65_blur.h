#ifndef RESOURCE_HOOK_P65_BLUR_H
#define RESOURCE_HOOK_P65_BLUR_H
#include <stdint.h>
#include <stddef.h>
#include "resource_hook_blur.h"

/* Exact P65 3.100.043. Private identities, restricted/PENDING/NOT_PROBED.
 * Evidence: targets/redmi-watch-6-3.100.043/blur-evidence.json. */
#define P65_BLUR_SCREEN_WIDTH 432u
#define P65_BLUR_SCREEN_HEIGHT 514u
#define P65_BLUR_WIDTH 72u
#define P65_BLUR_HEIGHT 86u
#define P65_BLUR_SCALE 1536u
#define P65_BLUR_PIXELS (P65_BLUR_WIDTH * P65_BLUR_HEIGHT)
#define P65_BLUR_DRAWBUF_CREATE 0x0c719551u
#define P65_BLUR_DRAWBUF_DESTROY 0x0c719799u
#define P65_BLUR_SNAPSHOT_TO_BUFFER 0x0c7417c1u
#define P65_BLUR_GPU_FINISH 0x0c8bc081u
#define P65_BLUR_CACHE_INVALIDATE 0x0c72f021u
#define P65_BLUR_CACHE_CLEAN 0x0c72f02du
#define P65_BLUR_HANDLER_TABLE 0x3c450108u
#define P65_BLUR_DRAW_UNITS_SLOT 0x3c450134u
#define P65_BLUR_VG_DISPATCH 0x0c7287f9u
#define P65_BLUR_SW_DISPATCH 0x0c7222bdu

struct p65_blur_pixel_view {
    unsigned char *pixels;
    size_t size, stride;
    unsigned width, height, format;
};
/* Format15 is 24-bit BGR, mapped to the native VG RGB888 format0x428.
 * Six-by-six averaging clamps the last two rows to the actual panel bottom.
 * The published72x86 image is scaled6x and clipped by the screen draw owner. */
static inline int p65_blur_downsample_bgr888(const struct p65_blur_pixel_view *v,
                                           uint16_t *out, size_t count) {
    unsigned x,y,dx,dy;
    if (!v || !v->pixels || !out || count < P65_BLUR_PIXELS || v->format != 15u ||
        v->width != P65_BLUR_SCREEN_WIDTH || v->height != P65_BLUR_SCREEN_HEIGHT ||
        v->stride < P65_BLUR_SCREEN_WIDTH * 3u || v->stride > SIZE_MAX / v->height ||
        v->size < v->stride * v->height) return 0;
    for (y=0;y<P65_BLUR_HEIGHT;++y) for (x=0;x<P65_BLUR_WIDTH;++x) {
        unsigned r=0,g=0,b=0;
        for (dy=0;dy<6u;++dy) for (dx=0;dx<6u;++dx) {
            unsigned sy=y*6u+dy;
            if (sy >= P65_BLUR_SCREEN_HEIGHT) sy=P65_BLUR_SCREEN_HEIGHT-1u;
            const unsigned char *p=v->pixels+sy*v->stride+(x*6u+dx)*3u;
            b+=p[0];g+=p[1];r+=p[2];
        }
        out[y*P65_BLUR_WIDTH+x]=(uint16_t)(((r/36u>>3)<<11)|((g/36u>>2)<<5)|(b/36u>>3));
    }
    return 1;
}
static inline int p65_blur_store_rgb565(const uint16_t *input,
                                       const struct p65_blur_pixel_view *v) {
    unsigned x,y;
    if (!input || !v || !v->pixels || v->format != 18u || v->width != P65_BLUR_WIDTH ||
        v->height != P65_BLUR_HEIGHT || v->stride < P65_BLUR_WIDTH*2u ||
        (v->stride&3u) || v->stride > SIZE_MAX/v->height || v->size < v->stride*v->height)
        return 0;
    for(y=0;y<P65_BLUR_HEIGHT;++y) for(x=0;x<P65_BLUR_WIDTH;++x) {
        uint16_t pixel=input[y*P65_BLUR_WIDTH+x];
        v->pixels[y*v->stride+x*2u]=(unsigned char)pixel;
        v->pixels[y*v->stride+x*2u+1u]=(unsigned char)(pixel>>8);
    }
    return 1;
}
#endif

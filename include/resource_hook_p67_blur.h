#ifndef RESOURCE_HOOK_P67_BLUR_H
#define RESOURCE_HOOK_P67_BLUR_H
#include <stdint.h>
#include <stddef.h>

#define P67_BLUR_WIDTH 84u
#define P67_BLUR_HEIGHT 120u
#define P67_BLUR_PIXELS (P67_BLUR_WIDTH * P67_BLUR_HEIGHT)
/* This view contains an already validated, coherent CPU alias. No native
 * descriptor/pixel pointer is retained by these helpers. Format15 is BGR888
 * only when the caller's exact-target channel-order gate has passed. */
struct p67_blur_pixel_view {
    const unsigned char *pixels;
    size_t size, stride;
    unsigned width, height, format;
};
static inline int p67_blur_view_valid(const struct p67_blur_pixel_view *v) {
    size_t row;
    if(!v || !v->pixels || v->format!=15 || v->width!=336 || v->height!=480) return 0;
    row=(size_t)v->width*3;
    if(v->stride<row || v->stride>SIZE_MAX/v->height) return 0;
    /* Require full stride storage: never trust a malformed native size. */
    return v->size>=v->stride*v->height;
}
static inline int p67_blur_downsample_bgr888(const struct p67_blur_pixel_view *v,
                                            uint16_t *out, size_t count) {
    unsigned x,y,dx,dy;
    if(!out || count<P67_BLUR_PIXELS || !p67_blur_view_valid(v)) return 0;
    for(y=0;y<P67_BLUR_HEIGHT;++y) for(x=0;x<P67_BLUR_WIDTH;++x) {
        unsigned r=0,g=0,b=0;
        for(dy=0;dy<4;++dy) for(dx=0;dx<4;++dx) {
            const unsigned char *p=v->pixels+(y*4+dy)*v->stride+(x*4+dx)*3;
            b+=p[0]; g+=p[1]; r+=p[2];
        }
        out[y*P67_BLUR_WIDTH+x]=(uint16_t)(((r/16>>3)<<11)|((g/16>>2)<<5)|(b/16>>3));
    }
    return 1;
}
/* Parameterized Band11 box-pass arithmetic: edge windows shrink rather than
 * replicate pixels, and each axis truncates channels separately. The accepted
 * Band11 implementation remains untouched. Bounded arguments prevent overflow.
 * Optional inherited darkening is applied once before filtering. */
static inline int p67_blur_filter_rgb565(uint16_t *src, uint16_t *tmp,
                                        unsigned width, unsigned height,
                                        unsigned radius, unsigned passes,
                                        unsigned darken_percent) {
    unsigned x,y,axis,pass,i;
    if(!src || !tmp || src==tmp || !width || !height || width>P67_BLUR_WIDTH ||
       height>P67_BLUR_HEIGHT || radius>8 || passes>8 || darken_percent>100) return 0;
    if(darken_percent!=100) for(i=0;i<width*height;++i) {
        unsigned v=src[i],r=(v>>11)*darken_percent/100;
        unsigned g=((v>>5)&63)*darken_percent/100,b=(v&31)*darken_percent/100;
        src[i]=(uint16_t)((r<<11)|(g<<5)|b);
    }
    for(pass=0;pass<passes;++pass) for(axis=0;axis<2;++axis) {
        uint16_t *in=axis?tmp:src,*out=axis?src:tmp;
        for(y=0;y<height;++y) for(x=0;x<width;++x) {
            unsigned r=0,g=0,b=0,count=0; int d;
            for(d=-(int)radius;d<=(int)radius;++d) {
                int xx=(int)x+(axis?0:d),yy=(int)y+(axis?d:0);
                if(xx>=0 && xx<(int)width && yy>=0 && yy<(int)height) {
                    unsigned v=in[(unsigned)yy*width+(unsigned)xx];
                    r+=v>>11; g+=(v>>5)&63; b+=v&31; ++count;
                }
            }
            out[y*width+x]=(uint16_t)(((r/count)<<11)|((g/count)<<5)|(b/count));
        }
    }
    return 1;
}
/* Publish a separate immutable 84x120 format18 image. Native image drawing
 * performs the 4x transform; CPU work never expands this to screen resolution.
 * Little-endian pixels support unaligned padded rows without touching padding. */
static inline int p67_blur_store_rgb565(const uint16_t *in, void *pixels,
                                       size_t stride, size_t size) {
    unsigned x,y;
    unsigned char *out=pixels;
    if(!in || !out || stride<P67_BLUR_WIDTH*2u ||
       stride>SIZE_MAX/P67_BLUR_HEIGHT || size<stride*P67_BLUR_HEIGHT) return 0;
    /* Publication storage must not alias scratch, even with overlapping rows. */
    const uintptr_t source=(uintptr_t)in,target=(uintptr_t)out;
    const size_t input_bytes=P67_BLUR_PIXELS*sizeof(*in),output_bytes=stride*P67_BLUR_HEIGHT;
    if(source>UINTPTR_MAX-input_bytes || target>UINTPTR_MAX-output_bytes ||
       (target>=source ? target-source<input_bytes : source-target<output_bytes)) return 0;
    for(y=0;y<P67_BLUR_HEIGHT;++y) for(x=0;x<P67_BLUR_WIDTH;++x) {
        unsigned v=in[y*P67_BLUR_WIDTH+x];
        out[y*stride+x*2u]=(unsigned char)v;
        out[y*stride+x*2u+1u]=(unsigned char)(v>>8);
    }
    return 1;
}
#endif

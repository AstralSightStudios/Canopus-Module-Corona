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
/* Inherited 4x bilinear arithmetic on individual RGB565 channels, vertical
 * then horizontal truncation. Output is little-endian native format18;
 * unaligned padded strides are supported, and padding is untouched. */
static inline int p67_blur_upscale_rgb565(const uint16_t *in, void *pixels,
                                         size_t stride, size_t size) {
    unsigned x,y;
    unsigned char *out=pixels;
    if(!in || !out || stride<336u*2 || stride>SIZE_MAX/480 || size<stride*480) return 0;
    for(y=0;y<480;++y) {
        unsigned yy=y/4, fy=y%4, yn=yy<119?yy+1:yy;
        for(x=0;x<336;++x) {
            unsigned xx=x/4, fx=x%4, xn=xx<83?xx+1:xx;
            unsigned a=in[yy*84+xx],b=in[yy*84+xn],c=in[yn*84+xx],d=in[yn*84+xn];
            unsigned r0=((a>>11)*(4-fy)+(c>>11)*fy)>>2;
            unsigned r1=((b>>11)*(4-fy)+(d>>11)*fy)>>2;
            unsigned g0=(((a>>5)&63)*(4-fy)+((c>>5)&63)*fy)>>2;
            unsigned g1=(((b>>5)&63)*(4-fy)+((d>>5)&63)*fy)>>2;
            unsigned b0=((a&31)*(4-fy)+(c&31)*fy)>>2;
            unsigned b1=((b&31)*(4-fy)+(d&31)*fy)>>2;
            unsigned r=(r0*(4-fx)+r1*fx)>>2,g=(g0*(4-fx)+g1*fx)>>2;
            unsigned bl=(b0*(4-fx)+b1*fx)>>2,v=(r<<11)|(g<<5)|bl;
            out[y*stride+x*2]=(unsigned char)v;
            out[y*stride+x*2+1]=(unsigned char)(v>>8);
        }
    }
    return 1;
}
#endif

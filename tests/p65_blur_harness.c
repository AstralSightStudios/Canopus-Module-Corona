#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resource_hook_background.h"
#include "resource_hook_target.h"
#include "resource_hook_p65_blur.h"

#define BASE 0x3c500000u
#define MEMORY_SIZE (3u*1024u*1024u)
static unsigned char memory[MEMORY_SIZE];
static uint32_t display=BASE+0x1000u,watchface=BASE+0x2000u,layer=BASE+0x3000u,unit=BASE+0x4000u;
static uint32_t active_root,draw_units,clean_hook,invalidate_hook;
static unsigned creates,destroys,captures,fences,cleans,invalidates,live_allocs,max_live;
static unsigned fail_alloc_at,fail_capture,fail_fence_at,bad_descriptor,change_root,change_epoch,reenter;
static int gpu_failed;
struct allocation { uint32_t descriptor,pixels; unsigned live; };
static struct allocation allocations[32];
static uint32_t next_descriptor,next_pixels;
static unsigned char *at(uint32_t address,unsigned size) {
    assert(address>=BASE && address-BASE<=MEMORY_SIZE && size<=MEMORY_SIZE-(address-BASE));
    return memory+(address-BASE);
}
static uint32_t rd(uint32_t address) {
    uint32_t value;
    if(address==RH_FW_DISPLAY_SLOT) return active_root;
    if(address==P65_BLUR_DRAW_UNITS_SLOT) return draw_units;
    if(address==P65_BLUR_HANDLER_TABLE+12u) return invalidate_hook;
    if(address==P65_BLUR_HANDLER_TABLE+16u) return clean_hook;
    memcpy(&value,at(address,4),4); return value;
}
static void wr(uint32_t address,uint32_t value) { memcpy(at(address,4),&value,4); }
static uint32_t create(uint32_t w,uint32_t h,uint32_t cf,uint32_t stride);
static void destroy(uint32_t descriptor);
static int snapshot(uint32_t root,uint32_t cf,uint32_t descriptor);
static int finish(void);
static void clean(uint32_t descriptor,const int32_t *area);
static void invalidate(uint32_t pixels,uint32_t size,uint32_t cf,uint32_t unused);
static uintptr_t native(uint32_t address) {
    switch(address) {
    case P65_BLUR_DRAWBUF_CREATE:return (uintptr_t)create;
    case P65_BLUR_DRAWBUF_DESTROY:return (uintptr_t)destroy;
    case P65_BLUR_SNAPSHOT_TO_BUFFER:return (uintptr_t)snapshot;
    case P65_BLUR_GPU_FINISH:return (uintptr_t)finish;
    case P65_BLUR_CACHE_CLEAN:return (uintptr_t)clean;
    case P65_BLUR_CACHE_INVALIDATE:return (uintptr_t)invalidate;
    default:assert(!"unexpected native callable"); return 0;
    }
}
#define P65_BLUR_READ32(a) rd(a)
#define P65_BLUR_READ8(a) (*at((a),1))
#define P65_BLUR_PIXELS_AT(a) at((a),1)
#define P65_BLUR_NATIVE(a) native(a)
#define P65_BLUR_WATCHFACE() watchface
#define P65_BLUR_BARRIER() ((void)0)
#include "../src/p65_blur_native.inc"

static uint32_t create(uint32_t w,uint32_t h,uint32_t cf,uint32_t stride) {
    unsigned index=creates++;
    uint32_t descriptor,pixels,size;
    assert(w==P65_BLUR_SCREEN_WIDTH || w==P65_BLUR_WIDTH);
    assert((w==P65_BLUR_SCREEN_WIDTH && h==P65_BLUR_SCREEN_HEIGHT && cf==15u) ||
           (w==P65_BLUR_WIDTH && h==P65_BLUR_HEIGHT && cf==18u));
    if(creates==fail_alloc_at) return 0;
    assert(index<32u);
    if(!stride) stride=((w+15u)&~15u)*(cf==15u?3u:2u);
    size=stride*h;descriptor=next_descriptor;next_descriptor+=32u;
    pixels=next_pixels;next_pixels=(next_pixels+size+63u)&~63u;
    memset(at(pixels,size),0,size);
    wr(descriptor,0x120019u|(cf<<8));wr(descriptor+4u,w|(h<<16));
    wr(descriptor+8u,stride);wr(descriptor+12u,size);wr(descriptor+16u,pixels);wr(descriptor+20u,pixels);
    if(bad_descriptor) {wr(descriptor+16u,pixels+1u);bad_descriptor=0;}
    allocations[index]=(struct allocation){descriptor,pixels,1};live_allocs++;
    if(live_allocs>max_live)max_live=live_allocs;
    return descriptor;
}
static void destroy(uint32_t descriptor) {
    unsigned i;
    assert(!gpu_failed);assert(p65_blur_idle());
    for(i=0;i<32u;++i)if(allocations[i].descriptor==descriptor && allocations[i].live) {
        allocations[i].live=0;live_allocs--;destroys++;return;
    }
    assert(!"double or foreign native destroy");
}
static int snapshot(uint32_t root,uint32_t cf,uint32_t descriptor) {
    unsigned x,y;uint32_t pixels=rd(descriptor+16u),stride=rd(descriptor+8u)&65535u;
    assert(root==watchface && cf==15u);captures++;
    for(y=0;y<P65_BLUR_SCREEN_HEIGHT;++y)for(x=0;x<P65_BLUR_SCREEN_WIDTH;++x) {
        unsigned char *p=at(pixels+y*stride+x*3u,3);
        p[0]=32;p[1]=64;p[2]=200;
    }
    if(change_root) {watchface+=0x100u;change_root=0;}
    if(change_epoch) {p65_blur_reset();change_epoch=0;}
    if(reenter) {unsigned before=creates;reenter=0;p65_blur_watch(1);assert(creates==before);}
    return fail_capture?0:1;
}
static int finish(void) {fences++;return gpu_failed || fences==fail_fence_at ? 12:0;}
static void clean(uint32_t descriptor,const int32_t *area) {assert(descriptor && !area);cleans++;}
static void invalidate(uint32_t pixels,uint32_t size,uint32_t cf,uint32_t unused) {
    assert(cf==15u && !unused && size>=P65_BLUR_SCREEN_WIDTH*P65_BLUR_SCREEN_HEIGHT*3u);
    (void)at(pixels,size);invalidates++;
}
static void setup(void) {
    memset(memory,0,sizeof(memory));memset(&p65_blur_state,0,sizeof(p65_blur_state));
    memset(allocations,0,sizeof(allocations));
    display=BASE+0x1000u;watchface=BASE+0x2000u;layer=BASE+0x3000u;unit=BASE+0x4000u;
    active_root=display;draw_units=unit;clean_hook=P65_BLUR_CACHE_CLEAN;invalidate_hook=P65_BLUR_CACHE_INVALIDATE;
    wr(display+672u,layer);wr(display+688u,watchface);wr(unit+12u,P65_BLUR_VG_DISPATCH);
    creates=destroys=captures=fences=cleans=invalidates=live_allocs=max_live=0;
    fail_alloc_at=fail_capture=fail_fence_at=bad_descriptor=change_root=change_epoch=reenter=0;gpu_failed=0;
    next_descriptor=BASE+0x6000u;next_pixels=BASE+0x10000u;
}
static void finish_all(void) {
    gpu_failed=0;fail_fence_at=0;wr(layer+100u,0);wr(unit+24u,0);*at(display+58u,1)=0;
    p65_blur_watch(0);assert(!live_allocs && !p65_blur_source(":blur"));
}
static void test_success_and_restore(void) {
    uintptr_t source;unsigned before;
    setup();assert(!p65_blur_source(0) && !p65_blur_source(""));
    const char *path="/theme/a.bin";assert(p65_blur_source(path)==(uintptr_t)path);
    reenter=1;p65_blur_watch(1);source=p65_blur_source(":blur");
    assert(source && captures==1 && creates==2 && invalidates==1 && cleans==2);
    assert(rd((uint32_t)source+4u)==(P65_BLUR_WIDTH|(P65_BLUR_HEIGHT<<16)));
    assert(rd((uint32_t)source+8u)==160u);
    uint16_t pixel;memcpy(&pixel,at(rd((uint32_t)source+16u),2),2);
    assert(pixel==0x8962u); /* BGR input -> RGB565,70% darkening,constant boxfilter */
    before=creates;p65_blur_watch(1);assert(creates==before && captures==1 && live_allocs==1);
    p65_blur_reset();p65_blur_watch(1);assert(p65_blur_source(":blur")!=source && captures==2);
    p65_blur_watch(1);assert(live_allocs==1 && max_live<=3u);
    finish_all();
}
static void test_fence_and_allocation_failures(void) {
    unsigned before;
    setup();fail_fence_at=2;p65_blur_watch(1);
    assert(captures==1 && p65_blur_state.input && live_allocs==1 && !invalidates);
    assert(!p65_blur_source(":blur"));before=creates;gpu_failed=1;
    p65_blur_watch(1);assert(creates==before && !destroys && live_allocs==1);
    gpu_failed=0;fail_fence_at=0;p65_blur_watch(1);assert(p65_blur_source(":blur") && captures==2);
    finish_all();
    setup();fail_alloc_at=1;p65_blur_watch(1);assert(!captures && !live_allocs);finish_all();
    setup();fail_alloc_at=2;p65_blur_watch(1);assert(captures==1 && !p65_blur_source(":blur"));finish_all();
    setup();fail_capture=1;p65_blur_watch(1);assert(!p65_blur_source(":blur") && !invalidates);finish_all();
    setup();bad_descriptor=1;p65_blur_watch(1);assert(!captures && live_allocs==1);finish_all();
}
static void test_stale_identity_and_busy_guards(void) {
    setup();change_root=1;p65_blur_watch(1);assert(!p65_blur_source(":blur"));finish_all();
    setup();change_epoch=1;p65_blur_watch(1);assert(!p65_blur_source(":blur"));finish_all();
    setup();wr(layer+100u,1);p65_blur_watch(1);assert(!creates);wr(layer+100u,0);
    wr(unit+24u,1);p65_blur_watch(1);assert(!creates);wr(unit+24u,0);
    *at(display+58u,1)=2;p65_blur_watch(1);assert(!creates);*at(display+58u,1)=0;
    invalidate_hook=0x123u;p65_blur_watch(1);assert(!creates);invalidate_hook=P65_BLUR_CACHE_INVALIDATE;
    p65_blur_watch(1);assert(p65_blur_source(":blur"));
    p65_blur_drop();gpu_failed=1;p65_blur_watch(0);assert(live_allocs && !p65_blur_source(":blur"));
    finish_all();
}
static void test_downsample_bottom_and_padding(void) {
    uint16_t plane[P65_BLUR_PIXELS];unsigned char *p=calloc(1,1300u*514u);assert(p);
    struct p65_blur_pixel_view v={p,1300u*514u,1300u,432u,514u,15u};
    for(unsigned x=0;x<432u;++x) {p[513u*1300u+x*3u+2u]=255;}
    assert(p65_blur_downsample_bgr888(&v,plane,P65_BLUR_PIXELS));
    assert(plane[(P65_BLUR_HEIGHT-1u)*P65_BLUR_WIDTH]==0x7800u);
    v.size--;assert(!p65_blur_downsample_bgr888(&v,plane,P65_BLUR_PIXELS));free(p);
}
int main(void) {
    test_success_and_restore();test_fence_and_allocation_failures();
    test_stale_identity_and_busy_guards();test_downsample_bottom_and_padding();
    puts("P65 blur: native allocation, BGR/stride/filter, fence retention, identity and restore passed");
    return 0;
}

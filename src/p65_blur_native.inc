/* P65 module-owned snapshot provider. Included by p65_backgrounds.inc.
 * P65_BLUR_WATCHFACE() must read the exact native watchface root identity.
 * All watch/maintenance operations belong to the serialized UI owner; source()
 * only reads immutable publication. DELETE/config paths only retire logically. */
#include "resource_hook_p65_blur.h"
#ifndef P65_BLUR_WATCHFACE
#error "P65 blur requires the exact native watchface root provider"
#endif
#ifndef P65_BLUR_READ32
#define P65_BLUR_READ32(a) (*(volatile const uint32_t *)(uintptr_t)(a))
#define P65_BLUR_READ8(a) (*(volatile const unsigned char *)(uintptr_t)(a))
#endif
#ifndef P65_BLUR_NATIVE
#define P65_BLUR_NATIVE(a) (a)
#endif
#ifndef P65_BLUR_PIXELS_AT
#define P65_BLUR_PIXELS_AT(a) ((unsigned char *)(uintptr_t)(a))
#endif
#ifndef P65_BLUR_BARRIER
#if defined(__arm__) || defined(__thumb__)
#define P65_BLUR_BARRIER() __asm__ volatile("dsb sy\nisb sy" ::: "memory")
#else
#define P65_BLUR_BARRIER() ((void)0)
#endif
#endif

struct p65_blur_output { uint32_t descriptor; unsigned retired; };
static struct {
    struct p65_blur_output output[2];
    uint32_t input, root, epoch, published;
    unsigned busy, demand, pending;
} p65_blur_state;
static uint16_t p65_blur_plane[P65_BLUR_PIXELS],p65_blur_work[P65_BLUR_PIXELS];

static int p65_blur_ram(uint32_t p,uint32_t n) {
    return n && ((p>=0x3c000000u && p<0x3e000000u && n<=0x3e000000u-p) ||
                 (p>=0x20000000u && p<0x20200000u && n<=0x20200000u-p));
}
static int p65_blur_idle(void) {
    uint32_t display=P65_BLUR_READ32(RH_FW_DISPLAY_SLOT),layer,unit;
    unsigned guard=0;
    if (!p65_blur_ram(display,784u) || (P65_BLUR_READ8(display+58u)&2u)) return 0;
    for (layer=P65_BLUR_READ32(display+672u);layer;layer=P65_BLUR_READ32(layer+108u)) {
        if (++guard>16u || !p65_blur_ram(layer,120u) || P65_BLUR_READ32(layer+100u)) return 0;
    }
    guard=0;
    for (unit=P65_BLUR_READ32(P65_BLUR_DRAW_UNITS_SLOT);unit;unit=P65_BLUR_READ32(unit)) {
        uint32_t dispatch;
        if (++guard>16u || !p65_blur_ram(unit,28u) || P65_BLUR_READ32(unit+24u)) return 0;
        dispatch=P65_BLUR_READ32(unit+12u);
        if (dispatch!=P65_BLUR_VG_DISPATCH && dispatch!=P65_BLUR_SW_DISPATCH) return 0;
    }
    return display==P65_BLUR_READ32(RH_FW_DISPLAY_SLOT) && !(P65_BLUR_READ8(display+58u)&2u);
}
static int p65_blur_fence(void) {
    int status=((int (*)(void))(uintptr_t)P65_BLUR_NATIVE(P65_BLUR_GPU_FINISH))();
    P65_BLUR_BARRIER();
    return status==0;
}
static int p65_blur_view(uint32_t descriptor,unsigned format,unsigned width,unsigned height,
                         struct p65_blur_pixel_view *view) {
    uint32_t header,packed,size,pixels,allocation,stride;
    if (!p65_blur_ram(descriptor,24u)) return 0;
    header=P65_BLUR_READ32(descriptor);packed=P65_BLUR_READ32(descriptor+4u);
    stride=P65_BLUR_READ32(descriptor+8u)&0xffffu;
    size=P65_BLUR_READ32(descriptor+12u);pixels=P65_BLUR_READ32(descriptor+16u);
    allocation=P65_BLUR_READ32(descriptor+20u);
    if ((header&0xffu)!=25u || ((header>>8)&0xffu)!=format || !(header&0x100000u) ||
        packed!=(width|(height<<16)) || !stride || stride>2048u ||
        stride<width*(format==15u?3u:2u) || size<stride*height || size>1024u*1024u ||
        (pixels&63u) || pixels<allocation || pixels-allocation>63u ||
        !p65_blur_ram(allocation,size+pixels-allocation)) return 0;
    view->pixels=P65_BLUR_PIXELS_AT(pixels);view->size=size;view->stride=stride;
    view->width=width;view->height=height;view->format=format;
    return 1;
}
/* Exact platform draw-buffer cache hooks are no-op bodies in this firmware.
 * Still require the known identities, and call through the native contract:
 * CPU aliases must not silently inherit another target's cache instructions. */
static int p65_blur_cache_ready(void) {
    return P65_BLUR_READ32(P65_BLUR_HANDLER_TABLE+12u)==P65_BLUR_CACHE_INVALIDATE &&
           P65_BLUR_READ32(P65_BLUR_HANDLER_TABLE+16u)==P65_BLUR_CACHE_CLEAN;
}
static void p65_blur_clean(uint32_t descriptor) {
    ((void (*)(uint32_t,const int32_t *))(uintptr_t)
        P65_BLUR_NATIVE(P65_BLUR_CACHE_CLEAN))(descriptor,0);
    P65_BLUR_BARRIER();
}
static void p65_blur_invalidate(uint32_t descriptor,const struct p65_blur_pixel_view *view) {
    ((void (*)(uint32_t,uint32_t,uint32_t,uint32_t))(uintptr_t)
        P65_BLUR_NATIVE(P65_BLUR_CACHE_INVALIDATE))(
            P65_BLUR_READ32(descriptor+16u),(uint32_t)view->size,view->format,0);
    P65_BLUR_BARRIER();
}
static void p65_blur_destroy(uint32_t descriptor) {
    ((void (*)(uint32_t))(uintptr_t)P65_BLUR_NATIVE(P65_BLUR_DRAWBUF_DESTROY))(descriptor);
}
static void p65_blur_drop(void) {
    unsigned i;
    ++p65_blur_state.epoch;
    for(i=0;i<2u;++i) if(p65_blur_state.published && p65_blur_state.output[i].descriptor==p65_blur_state.published)
        p65_blur_state.output[i].retired=1;
    p65_blur_state.published=0;p65_blur_state.pending=p65_blur_state.demand;
}
static void p65_blur_reset(void) {
    ++p65_blur_state.epoch;p65_blur_state.pending=1;
}
/* A failed fence retains exactly one input and at most two output descriptors.
 * The next idle UI watch retries retirement before making another allocation. */
static int p65_blur_maintain(void) {
    unsigned i,needed=p65_blur_state.input!=0;
    for(i=0;i<2u;++i) needed|=p65_blur_state.output[i].retired;
    if(!needed) return 1;
    if(!p65_blur_idle() || !p65_blur_fence() || !p65_blur_idle()) return 0;
    if(p65_blur_state.input) {
        uint32_t input=p65_blur_state.input;p65_blur_state.input=0;p65_blur_destroy(input);
    }
    for(i=0;i<2u;++i) if(p65_blur_state.output[i].retired) {
        uint32_t d=p65_blur_state.output[i].descriptor;
        p65_blur_state.output[i].descriptor=0;p65_blur_state.output[i].retired=0;
        if(d) p65_blur_destroy(d);
    }
    return 1;
}
static uintptr_t p65_blur_source(const char *path) {
    return !path || !path[0]?0:rh_background_is_blur(path)?p65_blur_state.published:(uintptr_t)path;
}
/* One bounded capture per caller's serialized UI poll. Keep pending on failure;
 * callers must not spin/retry this function inside the same refresh callback. */
static void p65_blur_watch(unsigned demand) {
    struct p65_blur_pixel_view input_view,output_view;
    uint32_t root,epoch,input,output;
    unsigned i,slot=2u;
    int captured;
    if(p65_blur_state.busy) return;
    p65_blur_state.busy=1;
    root=P65_BLUR_WATCHFACE();
    if(root!=p65_blur_state.root) {
        p65_blur_drop();p65_blur_state.root=root;p65_blur_state.pending=demand!=0;
    }
    if((demand!=0)!=p65_blur_state.demand) {
        p65_blur_state.demand=demand!=0;
        if(!demand) p65_blur_drop();else p65_blur_state.pending=1;
    }
    if(!p65_blur_maintain() || !demand || !p65_blur_state.pending ||
       !p65_blur_ram(root,52u) || (P65_BLUR_READ8(root+51u)&16u) ||
       !p65_blur_cache_ready() || !p65_blur_idle() || !p65_blur_fence()) goto done;
    for(i=0;i<2u;++i) if(!p65_blur_state.output[i].descriptor) { slot=i;break; }
    if(slot==2u) goto done;
    epoch=p65_blur_state.epoch;
    input=((uint32_t (*)(uint32_t,uint32_t,uint32_t,uint32_t))(uintptr_t)
        P65_BLUR_NATIVE(P65_BLUR_DRAWBUF_CREATE))(P65_BLUR_SCREEN_WIDTH,P65_BLUR_SCREEN_HEIGHT,15u,0);
    if(!input) goto done;
    p65_blur_state.input=input;
    if(!p65_blur_view(input,15u,P65_BLUR_SCREEN_WIDTH,P65_BLUR_SCREEN_HEIGHT,&input_view)) goto done;
    p65_blur_clean(input);
    captured=((int (*)(uint32_t,uint32_t,uint32_t))(uintptr_t)
        P65_BLUR_NATIVE(P65_BLUR_SNAPSHOT_TO_BUFFER))(root,15u,input);
    if(!p65_blur_fence() || !p65_blur_idle() || captured!=1 ||
       root!=P65_BLUR_WATCHFACE() || epoch!=p65_blur_state.epoch) goto done;
    if(!p65_blur_view(input,15u,P65_BLUR_SCREEN_WIDTH,P65_BLUR_SCREEN_HEIGHT,&input_view)) goto done;
    p65_blur_invalidate(input,&input_view);
    if(!p65_blur_downsample_bgr888(&input_view,p65_blur_plane,P65_BLUR_PIXELS) ||
       !rh_blur_filter_rgb565(p65_blur_plane,p65_blur_work,P65_BLUR_WIDTH,P65_BLUR_HEIGHT,3u,3u,70u)) goto done;
    output=((uint32_t (*)(uint32_t,uint32_t,uint32_t,uint32_t))(uintptr_t)
        P65_BLUR_NATIVE(P65_BLUR_DRAWBUF_CREATE))(P65_BLUR_WIDTH,P65_BLUR_HEIGHT,18u,0);
    if(!output) goto done;
    p65_blur_state.output[slot].descriptor=output;
    p65_blur_state.output[slot].retired=1;
    if(!p65_blur_view(output,18u,P65_BLUR_WIDTH,P65_BLUR_HEIGHT,&output_view) ||
       !p65_blur_store_rgb565(p65_blur_plane,&output_view)) goto done;
    p65_blur_clean(output);
    if(root!=P65_BLUR_WATCHFACE() || epoch!=p65_blur_state.epoch) goto done;
    for(i=0;i<2u;++i) if(p65_blur_state.published && p65_blur_state.output[i].descriptor==p65_blur_state.published)
        p65_blur_state.output[i].retired=1;
    p65_blur_state.output[slot].retired=0;
    p65_blur_state.published=output;p65_blur_state.pending=0;
done:
    /* No asynchronous work is begun after this point. A failed capture fence
     * leaves the input pinned until a future call, rather than immediately
     * treating a second fence in the same call as an implicit success. */
    p65_blur_state.busy=0;
}

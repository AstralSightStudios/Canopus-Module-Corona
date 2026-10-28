/* Exact P65 live background ownership; included by platform.c. Native snapshot
 * allocation/deletion remains native-owned. Module events draw before children
 * and temporarily show live foreground during native widget/notify swipes. */
#include "resource_hook_p65_background.h"
#ifndef P65_BG_NATIVE
#define P65_BG_NATIVE(address) (address)
#endif
#ifndef P65_BG_U32
#define P65_BG_U32(address) (*(volatile uint32_t *)(uintptr_t)(address))
#endif
#ifndef P65_BG_U8
#define P65_BG_U8(address) (*(volatile unsigned char *)(uintptr_t)(address))
#endif
#ifndef P65_BG_CALLBACK
#define P65_BG_CALLBACK(callback) ((uint32_t)(uintptr_t)(callback))
#endif
struct p65_bg_surface {
    uint32_t root_slot, image_slot, native_scroll;
    uint32_t root, owner, page, generation;
    unsigned swapped, busy;
    char path[2][RH_PATH];
    unsigned path_index;
};
static struct p65_bg_surface p65_bg_surfaces[4] = {
    {.root_slot=RH_P65_BG_LAUNCHER_ROOT},
    {.root_slot=RH_P65_BG_CONTROL_ROOT},
    {.root_slot=RH_P65_BG_NOTIFY_ROOT,.image_slot=RH_P65_BG_NOTIFY_IMAGE,.native_scroll=RH_P65_BG_NOTIFY_SCROLL},
    {.root_slot=RH_P65_BG_WIDGET_ROOT,.image_slot=RH_P65_BG_WIDGET_IMAGE,.native_scroll=RH_P65_BG_WIDGET_SCROLL}
};
static unsigned p65_bg_busy,p65_bg_serial,p65_bg_wake;
static void p65_bg_event(void *event);
static void p65_bg_notify_scroll(void *event);
static void p65_bg_widget_scroll(void *event);
struct p65_bg_membership { uint32_t wanted,found,count; };
static int p65_bg_find(uint32_t object,void *cookie) {
    struct p65_bg_membership *m=cookie;
    if(++m->count>RH_RELOAD_LIMIT) return 2;
    if(object==m->wanted) {m->found=1;return 2;} return 0;
}
static int p65_bg_live(uint32_t object) {
    struct p65_bg_membership m={object,0,0};
    if(!object) return 0;
    ((void (*)(uint32_t,int (*)(uint32_t,void *),void *))(uintptr_t)
        P65_BG_NATIVE(RH_FW_OBJECT_TREE_WALK))(0,p65_bg_find,&m);
    return m.found!=0;
}
static uint32_t p65_bg_parent(uint32_t object) {
    return ((uint32_t (*)(uint32_t))(uintptr_t)P65_BG_NATIVE(RH_P65_BG_PARENT))(object);
}
static unsigned p65_bg_count(uint32_t object) {
    return ((uint32_t (*)(uint32_t))(uintptr_t)P65_BG_NATIVE(RH_P65_BG_EVENT_COUNT))(object);
}
static uint32_t *p65_bg_descriptor(uint32_t object,unsigned index) {
    return ((uint32_t *(*)(uint32_t,unsigned))(uintptr_t)P65_BG_NATIVE(RH_P65_BG_EVENT_GET))(object,index);
}
static uint32_t p65_bg_scroll_callback(struct p65_bg_surface *s) {
    return P65_BG_CALLBACK(s==&p65_bg_surfaces[2]?p65_bg_notify_scroll:p65_bg_widget_scroll);
}
static uint32_t *p65_bg_add(uint32_t object,uint32_t callback,unsigned filter,uint32_t generation) {
    return ((uint32_t *(*)(uint32_t,uint32_t,unsigned,uint32_t))(uintptr_t)
        P65_BG_NATIVE(RH_P65_BG_EVENT_ADD))(object,callback,filter,generation);
}
static void p65_bg_remove(uint32_t object,uint32_t callback) {
    unsigned n=0;
    while(n++<64 && ((int (*)(uint32_t,uint32_t))(uintptr_t)
        P65_BG_NATIVE(RH_P65_BG_EVENT_REMOVE))(object,callback)) {}
}
static int p65_bg_hidden(uint32_t object) {
    return ((int (*)(uint32_t,uint32_t))(uintptr_t)P65_BG_NATIVE(RH_P65_BG_HAS_FLAG))(object,1);
}
static void p65_bg_hide(uint32_t object,unsigned hide) {
    if(p65_bg_live(object) && !!p65_bg_hidden(object)!=!!hide)
        ((void (*)(uint32_t,uint32_t))(uintptr_t)
            P65_BG_NATIVE(hide?RH_P65_BG_ADD_FLAG:RH_P65_BG_CLEAR_FLAG))(object,1);
}
/* A module-owned blur provider uses native drawing allocations (not ctx->mem).
 * It is supplied before the surface functions and never accesses native pools. */
#ifdef P65_BG_TEST_BLUR
#include P65_BG_TEST_BLUR
#else
static uint32_t p65_bg_watchface(void) {
    uint32_t root=P65_BG_U32(RH_P65_BG_WATCHFACE_ROOT);
    return p65_bg_live(root)?root:0;
}
#define P65_BLUR_WATCHFACE() p65_bg_watchface()
#include "p65_blur_native.inc"
#endif
static const char *p65_bg_path(struct p65_bg_surface *s) {return s->path[s->path_index];}
static int p65_bg_pair(struct p65_bg_surface *s) {
    uint32_t image=P65_BG_U32(s->image_slot);
    return s->root && s->page && P65_BG_U32(s->root_slot)==s->root &&
        p65_bg_live(s->root) && p65_bg_live(s->page) && p65_bg_live(image) &&
        P65_BG_U32(image)==RH_FW_IMAGE_OBJECT_CLASS &&
        p65_bg_parent(image)==s->page && p65_bg_parent(s->root)==s->page;
}
static void p65_bg_native_foreground(struct p65_bg_surface *s) {
    uint32_t image;
    if(!s->swapped) return;
    if(p65_bg_pair(s)) {
        image=P65_BG_U32(s->image_slot);
        p65_bg_hide(image,0);
        if(p65_bg_pair(s) && image==P65_BG_U32(s->image_slot)) p65_bg_hide(s->root,1);
    }
    s->swapped=0;
}
static void p65_bg_live_foreground(struct p65_bg_surface *s) {
    uint32_t image;
    if(!p65_bg_pair(s)) {s->swapped=0;return;}
    image=P65_BG_U32(s->image_slot);
    if(p65_bg_hidden(s->root)) {
        p65_bg_hide(image,1);
        if(p65_bg_pair(s) && image==P65_BG_U32(s->image_slot)) {
            p65_bg_hide(s->root,0);
            if(p65_bg_pair(s)) s->swapped=1;
        }
    }
}
static int p65_bg_scroll_descriptors(struct p65_bg_surface *s,uint32_t from,uint32_t to) {
    unsigned n=p65_bg_count(s->owner),i,found=0;
    if(n>64) return 0;
    for(i=0;i<n;++i) {
        uint32_t *d=p65_bg_descriptor(s->owner,i);
        if(d && d[0]==from) {
            /* Original native callback has one descriptor per event filter. */
            if(d[1]!=s->page || (d[2]!=9 && d[2]!=11 && d[2]!=12 && d[2]!=19)) return 0;
            ++found;
        }
    }
    if(!found) return 0;
    for(i=0;i<n;++i) {uint32_t *d=p65_bg_descriptor(s->owner,i);if(d && d[0]==from) d[0]=to;}
    return 1;
}
static void p65_bg_detach(struct p65_bg_surface *s) {
    uint32_t root=s->root,owner=s->owner;
    p65_bg_native_foreground(s);
    if(owner && p65_bg_live(owner)) {
        p65_bg_scroll_descriptors(s,p65_bg_scroll_callback(s),s->native_scroll);
        p65_bg_remove(owner,P65_BG_CALLBACK(p65_bg_event));
    }
    if(root && p65_bg_live(root)) p65_bg_remove(root,P65_BG_CALLBACK(p65_bg_event));
    s->root=s->owner=s->page=0;s->swapped=0;s->generation=++p65_bg_serial;
}
static void p65_bg_draw(struct p65_bg_surface *s,uint32_t layer) {
    uint32_t source,descriptor[23],header[3]={0,0,0};
    int32_t area[4],saved[4],clip[4];unsigned i,w,h;
    const char *path=p65_bg_path(s);
    source=(uint32_t)p65_blur_source(path);
    if(!source || !layer || !s->root || P65_BG_U32(s->root_slot)!=s->root) return;
    if(((int (*)(uint32_t,void *))(uintptr_t)P65_BG_NATIVE(RH_FW_IMAGE_GET_INFO))(source,header)!=1) return;
    w=header[1]&0xffffu;h=header[1]>>16;
    if(!w || !h || w>4096 || h>4096) return;
    for(i=0;i<4;++i) {
        int32_t bound=(int32_t)P65_BG_U32(s->root+28u+4u*i);
        int32_t screen=i==2?431:i==3?513:0;
        saved[i]=(int32_t)P65_BG_U32(layer+24u+4u*i);
        clip[i]=i<2?(saved[i]>bound?saved[i]:bound):(saved[i]<bound?saved[i]:bound);
        if(i<2 && clip[i]<screen) clip[i]=screen;
        if(i>=2 && clip[i]>screen) clip[i]=screen;
    }
    if(clip[0]>clip[2] || clip[1]>clip[3]) return;
    ((void (*)(void *))(uintptr_t)P65_BG_NATIVE(RH_P65_BG_IMAGE_DSC_INIT))(descriptor);
    descriptor[7]=source;
    descriptor[12]=432u*256u/w;descriptor[13]=514u*256u/h;
    descriptor[16]=descriptor[17]=0;
    area[0]=area[1]=0;area[2]=(int32_t)w-1;area[3]=(int32_t)h-1;
    for(i=0;i<4;++i) P65_BG_U32(layer+24u+4u*i)=(uint32_t)clip[i];
    ((void (*)(uint32_t,const void *,const int32_t *))(uintptr_t)
        P65_BG_NATIVE(RH_P65_BG_DRAW_IMAGE))(layer,descriptor,area);
    for(i=0;i<4;++i) P65_BG_U32(layer+24u+4u*i)=(uint32_t)saved[i];
}
static void p65_bg_event(void *event) {
    uint32_t object=*(uint32_t *)event,target=((uint32_t *)event)[1],generation=((uint32_t *)event)[3];
    unsigned code=*(uint16_t *)((char *)event+8)&0x7fffu,i;
    if(object!=target) return;
    for(i=0;i<4;++i) {
        struct p65_bg_surface *s=&p65_bg_surfaces[i];
        if(generation!=s->generation) continue;
        if(code==RH_P65_BG_DELETE && (object==s->root || object==s->owner)) {
            /* DELETE owns object memory; don't unregister the current event. */
            if(object==s->root) s->root=0;
            if(object==s->owner) s->owner=0;
            s->swapped=0;s->generation=++p65_bg_serial;
            return;
        }
        if(object==s->root && code==RH_P65_BG_DRAW_MAIN_END && p65_bg_path(s)[0] && !s->busy)
            p65_bg_draw(s,((uint32_t *)event)[4]);
    }
}
static void p65_bg_scroll(struct p65_bg_surface *s,void *event) {
    unsigned code=*(uint16_t *)((char *)event+8)&0x7fffu;
    uint32_t generation=s->generation;
    if(s->busy || !s->owner || *(uint32_t *)event!=s->owner || ((uint32_t *)event)[3]!=s->page) return;
    s->busy=1;
    if(code==9 || code==11) p65_bg_native_foreground(s);
    ((void (*)(void *))(uintptr_t)P65_BG_NATIVE(s->native_scroll))(event);
    if(generation==s->generation && s->root && P65_BG_U32(s->root_slot)==s->root && p65_bg_path(s)[0])
        p65_bg_live_foreground(s);
    if(code==9 && rh_background_is_blur(p65_bg_path(s))) p65_blur_reset();
    s->busy=0;
}
static void p65_bg_notify_scroll(void *event) {p65_bg_scroll(&p65_bg_surfaces[2],event);}
static void p65_bg_widget_scroll(void *event) {p65_bg_scroll(&p65_bg_surfaces[3],event);}
static int p65_bg_registered(struct p65_bg_surface *s) {
    unsigned i,n=p65_bg_count(s->root),found=0;
    if(n>64) return 0;
    for(i=0;i<n;++i) {
        uint32_t *d=p65_bg_descriptor(s->root,i);
        if(d && d[0]==P65_BG_CALLBACK(p65_bg_event) && d[1]==s->generation && d[2]==0) ++found;
    }
    return found==1;
}
static void p65_bg_bind(struct p65_bg_surface *s) {
    uint32_t root=P65_BG_U32(s->root_slot),page,owner,generation;uint32_t *e;
    if(!p65_bg_path(s)[0]) {if(s->root||s->owner)p65_bg_detach(s);return;}
    if(root==s->root && s->root && p65_bg_live(root) && p65_bg_registered(s)) return;
    if(s->root||s->owner) p65_bg_detach(s);
    if(!p65_bg_live(root)) return;
    s->root=root;s->generation=++p65_bg_serial;generation=s->generation;
    e=p65_bg_add(root,P65_BG_CALLBACK(p65_bg_event),0,s->generation);
    if(!e || s->root!=root || s->generation!=generation || P65_BG_U32(s->root_slot)!=root || !p65_bg_live(root)) {p65_bg_detach(s);return;}
    if(!s->native_scroll) return;
    page=p65_bg_parent(root);if(!p65_bg_live(page)) {p65_bg_detach(s);return;}
    owner=p65_bg_parent(page);if(!p65_bg_live(owner)) {p65_bg_detach(s);return;}
    s->page=page;s->owner=owner;
    if(!p65_bg_add(owner,P65_BG_CALLBACK(p65_bg_event),RH_P65_BG_DELETE,s->generation) ||
       !p65_bg_scroll_descriptors(s,s->native_scroll,p65_bg_scroll_callback(s))) p65_bg_detach(s);
}
void rh_platform_backgrounds_poll(void) {
    unsigned i,demand=0;
    if(p65_bg_busy) return;
    p65_bg_busy=1;
    for(i=0;i<4;++i) demand|=rh_background_is_blur(p65_bg_path(&p65_bg_surfaces[i]));
    p65_blur_watch(demand);
    for(i=0;i<4;++i) p65_bg_bind(&p65_bg_surfaces[i]);
    if(p65_bg_wake && !rh_platform_request_full_redraw()) p65_bg_wake=0;
    p65_bg_busy=0;
}
int rh_platform_backgrounds_stage(const struct rh_backgrounds *config) {
    const char *paths[4];unsigned i;uint32_t header[3];
    if(!config) return -2142;
    paths[0]=config->launcher;paths[1]=config->control_center;paths[2]=config->notifications;paths[3]=config->widgets;
    for(i=0;i<4;++i) {
        uint32_t cache;int fd;
        if(!paths[i][0]||rh_background_is_blur(paths[i])) continue;
        fd=rh_platform_open(paths[i],RH_FILE_READ);if(fd<0)return -2143;rh_platform_close(fd);
        cache=P65_BG_U32(RH_FW_HEADER_CACHE_SLOT);
        if(cache && (!cache_valid(cache,RH_FW_HEADER_CACHE_CLASS)||retire_cache(cache,0,0,paths[i]))) return -2145;
        cache=P65_BG_U32(RH_FW_IMAGE_CACHE_SLOT);
        if(cache && (!cache_valid(cache,RH_FW_IMAGE_CACHE_CLASS)||retire_cache(cache,4,0,paths[i]))) return -2145;
        header[0]=header[1]=header[2]=0;
        if(((int (*)(const char *,void *))(uintptr_t)P65_BG_NATIVE(RH_FW_IMAGE_GET_INFO))(paths[i],header)!=1) return -2143;
    }
    return 0;
}
int rh_platform_backgrounds_commit(const struct rh_backgrounds *config) {
    const char *paths[4];unsigned i,j;
    if(!config) return -2142;
    if(p65_bg_busy) return -2145;
    for(i=0;i<4;++i) if(p65_bg_surfaces[i].busy ||
        (p65_bg_surfaces[i].image_slot && P65_BG_U32(p65_bg_surfaces[i].image_slot))) return -2145;
    paths[0]=config->launcher;paths[1]=config->control_center;paths[2]=config->notifications;paths[3]=config->widgets;
    for(i=0;i<4;++i) {
        struct p65_bg_surface *s=&p65_bg_surfaces[i];
        p65_bg_detach(s);s->path_index^=1u;
        for(j=0;j<RH_PATH;++j) {s->path[s->path_index][j]=paths[i][j];if(!paths[i][j])break;}
    }
    p65_blur_reset();p65_bg_wake=1;rh_platform_backgrounds_poll();return 0;
}

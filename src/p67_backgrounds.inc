/* Four-surface opt-in. Keep the accepted launcher adapter unchanged. */
#define rh_platform_backgrounds_stage p67_launcher_stage
#define rh_platform_backgrounds_commit p67_launcher_commit
#define rh_platform_backgrounds_poll p67_launcher_poll
#include "p67_launcher.inc"
#undef rh_platform_backgrounds_stage
#undef rh_platform_backgrounds_commit
#undef rh_platform_backgrounds_poll

struct p67_surface {
    uint32_t root_slot, image_slot, callback;
    uint32_t owner, page, root, generation, list;
    unsigned moving, swapped, busy, dirty, epoch;
    char path[RH_PATH], alternate[RH_PATH];
    unsigned path_index;
};
static struct p67_surface p67_surfaces[3] = {
    { .root_slot=0x2010fe50u, .image_slot=0x2010fe64u, .callback=0x0c5036c5u },
    { .root_slot=0x20110ef4u, .image_slot=0x20110ed4u, .callback=0x0c527bbdu },
    { .root_slot=0x201100e0u, .image_slot=0x201100ccu, .callback=0x0c50abb5u }
};
static const char *p67_surface_path(struct p67_surface *s) { return s->path_index ? s->alternate : s->path; }
static void p67_surface_event(void *event);
static void p67_surface_scroll_for(struct p67_surface *s,void *event);
static void p67_surface_refresh_list(struct p67_surface *s);
static uint32_t p67_surface_serial;
static void p67_scroll_cc(void *e) { p67_surface_scroll_for(&p67_surfaces[0],e); }
static void p67_scroll_notify(void *e) { p67_surface_scroll_for(&p67_surfaces[1],e); }
static void p67_scroll_widget(void *e) { p67_surface_scroll_for(&p67_surfaces[2],e); }
static void p67_event_cc(void *e) { p67_surface_event(e); }
static void p67_event_notify(void *e) { p67_surface_event(e); }
static void p67_event_widget(void *e) { p67_surface_event(e); }
static uint32_t p67_scroll_fn(struct p67_surface *s) {
    void (*f[3])(void *)={p67_scroll_cc,p67_scroll_notify,p67_scroll_widget};
    return (uint32_t)(uintptr_t)f[s-p67_surfaces];
}
static uint32_t p67_event_fn(struct p67_surface *s) {
    void (*f[3])(void *)={p67_event_cc,p67_event_notify,p67_event_widget};
    return (uint32_t)(uintptr_t)f[s-p67_surfaces];
}
static int p67_terminal(struct p67_surface *s,uint32_t page) {
    int32_t pos=(int32_t)P67_U32(page+(s==&p67_surfaces[1]?32u:28u));
    return pos==0 || pos==(s==&p67_surfaces[0]?-336:s==&p67_surfaces[1]?-480:336);
}
static void p67_hidden(uint32_t obj, int hide) {
    if (p67_live(obj) &&
        !!((int (*)(uint32_t,uint32_t))(uintptr_t)P67_NATIVE(0x0c5881c1u))(obj,1)!=!!hide)
        ((void (*)(uint32_t,uint32_t))(uintptr_t)
        P67_NATIVE(hide ? 0x0c588961u : 0x0c5891b1u))(obj, 1);
}
/* Restore precisely the native swap before forwarding: widget/CC end handlers
 * test these flags before releasing their native buffers. Never touch buffers. */
static int p67_surface_pair(struct p67_surface *s) {
    uint32_t image=load32(s->image_slot);
    if(!p67_live(s->root) || load32(s->root_slot)!=s->root ||
       load32(s->root)!=0x2cdba8b4u || !p67_live(s->page) ||
       !p67_live(image) || load32(image)!=0x2cce61ecu) return 0;
    return ((uint32_t (*)(uint32_t))(uintptr_t)P67_NATIVE(0x0c589579u))(image)==s->page &&
           ((uint32_t (*)(uint32_t))(uintptr_t)P67_NATIVE(0x0c589579u))(s->root)==s->page;
}
static void p67_surface_native(struct p67_surface *s) {
    uint32_t generation=s->generation, root=s->root, image=load32(s->image_slot);
    unsigned epoch=s->epoch;
    if (s->swapped && p67_surface_pair(s)) {
        p67_hidden(image, 0);
        if(epoch!=s->epoch || generation!=s->generation || root!=s->root ||
           image!=load32(s->image_slot) || !p67_surface_pair(s)) return;
        p67_hidden(root, 1);
        if(epoch!=s->epoch || generation!=s->generation || root!=s->root ||
           image!=load32(s->image_slot) || !p67_surface_pair(s)) return;
        s->swapped=0;
    } else if(!s->swapped) s->swapped=0;
}
static void p67_surface_detach(struct p67_surface *s) {
    uint32_t *d;
    p67_surface_native(s);
    if (p67_live(s->owner)) {
        d=p67_descriptor(s->owner,p67_scroll_fn(s));
        if (d) d[0]=s->callback;
        ((int (*)(uint32_t,uint32_t))(uintptr_t)P67_NATIVE(0x002bf811u))
            (s->owner,p67_event_fn(s));
    }
    if (p67_live(s->root)) ((int (*)(uint32_t,uint32_t))(uintptr_t)
        P67_NATIVE(0x002bf811u))(s->root,p67_event_fn(s));
    if(p67_live(s->list)) ((int (*)(uint32_t,uint32_t))(uintptr_t)
        P67_NATIVE(0x002bf811u))(s->list,p67_event_fn(s));
    s->list=0;s->dirty=0;s->swapped=0;
    s->owner=s->root=s->page=0; s->generation=++p67_surface_serial;
}
static void p67_surface_scroll_for(struct p67_surface *s,void *event) {
    uint32_t page=*((uint32_t *)event+3);
    unsigned code=*(uint16_t *)((char *)event+8)&0x7fffu;
    {
        if (!s->owner || s->page!=page || *(uint32_t *)event!=s->owner) return;
        if(s->busy) return;
        /* ALL also receives layout and render notifications. Only the audited
         * gesture callbacks may restore/swap visibility or refresh listeners. */
        /* Notification SCROLL (12) only updates the native direction icon;
         * it needs no snapshot visibility restoration or listener refresh. */
        if((code!=9 && code!=11 && code!=12) ||
           (code==12 && s==&p67_surfaces[1])) {
            ((void (*)(void *))(uintptr_t)P67_NATIVE(s->callback))(event);
            return;
        }
        uint32_t generation=s->generation;
        s->busy=1;
        p67_surface_native(s);
        if(generation!=s->generation) { s->busy=0; return; }
        /* Forward ALL begin bookkeeping, including widget first-entry cache,
         * timer cancellation and notification list repositioning. Capture is
         * native-owned and our drawing is suppressed during native callbacks. */
        ((void (*)(void *))(uintptr_t)P67_NATIVE(s->callback))(event);
        if(generation!=s->generation || !p67_live(s->owner) ||
           load32(s->root_slot)!=s->root || !p67_live(s->root)) { s->busy=0; return; }
        p67_surface_refresh_list(s);
        if(code==9) s->moving=1;
        if(code==11 && p67_terminal(s,page)) s->moving=0;
        if(s->moving && p67_surface_pair(s)) {
            int hidden=((int (*)(uint32_t,uint32_t))(uintptr_t)
                P67_NATIVE(0x0c5881c1u))(s->root,1);
            if(hidden) {
                p67_hidden(load32(s->image_slot),1);
                if(generation==s->generation && p67_surface_pair(s)) {
                    p67_hidden(s->root,0);
                    if(generation==s->generation && p67_surface_pair(s)) s->swapped=1;
                }
            }
        }
        s->busy=0;
        return;
    }
}
static void p67_surface_event(void *event) {
    uint32_t obj=*(uint32_t *)event, target=*((uint32_t *)event+1);
    unsigned code=*(uint16_t *)((char *)event+8)&0x7fffu,i,j;
    for(i=0;i<3;++i) {
        struct p67_surface *s=&p67_surfaces[i];
        uint32_t layer, descriptor[47];
        int32_t saved[4],clip[4],area[4]={0,0,335,479};
        if(obj!=target || (obj!=s->owner && obj!=s->root && obj!=s->list) ||
           *((uint32_t *)event+3)!=s->generation) continue;
        if(code==36) {
            ++s->epoch;
            if(obj==s->owner) {
                uint32_t *d=p67_descriptor(obj,p67_scroll_fn(s));
                if(d) d[0]=s->callback;
                s->owner=0;
            }
            if(obj==s->root) s->root=0;
            int list_delete=obj==s->list;
            if(list_delete) s->list=0;
            if(!list_delete) s->moving=0;
            s->dirty=1; if(!list_delete) s->generation=++p67_surface_serial;
            return;
        }
        if(code!=25 || (obj!=s->root && obj!=s->list) || s->busy || !p67_surface_path(s)[0] ||
           !p67_live(s->page) || load32(s->root_slot)!=s->root) return;
        layer=*((uint32_t *)event+4); if(!layer) return;
        for(j=0;j<4;++j) {
            int32_t bound=(int32_t)P67_U32(s->page+28+4*j);
            saved[j]=(int32_t)P67_U32(layer+24+4*j);
            clip[j]=j<2 ? (saved[j]>bound?saved[j]:bound) : (saved[j]<bound?saved[j]:bound);
            if(j<2 && clip[j]<area[j]) clip[j]=area[j];
            if(j>=2 && clip[j]>area[j]) clip[j]=area[j];
        }
        if(clip[0]>clip[2] || clip[1]>clip[3]) return;
        ((void (*)(void *))(uintptr_t)P67_NATIVE(0x0c588269u))(descriptor);
        ((unsigned char *)descriptor)[32]=0; descriptor[30]=(uint32_t)(uintptr_t)p67_surface_path(s);
        for(j=0;j<4;++j) P67_U32(layer+24+4*j)=(uint32_t)clip[j];
        ((void (*)(uint32_t,const void *,const int32_t *))(uintptr_t)
            P67_NATIVE(0x0c588351u))(layer,descriptor,area);
        for(j=0;j<4;++j) P67_U32(layer+24+4*j)=(uint32_t)saved[j];
        return;
    }
}
static int p67_surface_idle(struct p67_surface *s) {
    uint32_t root=load32(s->root_slot), page, image=load32(s->image_slot);
    if(s->busy || s->moving) return 0;
    if(!p67_live(root)) return 1;
    page=((uint32_t (*)(uint32_t))(uintptr_t)P67_NATIVE(0x0c589579u))(root);
    if(!p67_live(page) || !p67_terminal(s,page)) return 0;
    if(((int (*)(uint32_t,uint32_t))(uintptr_t)P67_NATIVE(0x0c5881c1u))(root,1)) return 0;
    if(p67_live(image) && !((int (*)(uint32_t,uint32_t))(uintptr_t)
       P67_NATIVE(0x0c5881c1u))(image,1)) return 0;
    return 1;
}
static int p67_surface_descendant(uint32_t obj,uint32_t root) {
    unsigned n;
    for(n=0;n<16 && p67_live(obj);++n) {
        if(obj==root) return 1;
        obj=((uint32_t (*)(uint32_t))(uintptr_t)P67_NATIVE(0x0c589579u))(obj);
    }
    return 0;
}
static void p67_surface_refresh_list(struct p67_surface *s) {
    uint32_t list=0;
    uint32_t *added;
    if(!s->owner || !p67_live(s->root)) return;
    if(s==&p67_surfaces[1]) list=load32(0x20110ee8u);
    if(s==&p67_surfaces[2]) list=load32(0x201100dcu);
    if(list==s->list) return;
    if(list && (!p67_live(list) || load32(list)!=0x2cdba28cu ||
       !p67_surface_descendant(list,s->root))) return;
    if(p67_live(s->list)) ((int (*)(uint32_t,uint32_t))(uintptr_t)
        P67_NATIVE(0x002bf811u))(s->list,p67_event_fn(s));
    s->list=0;
    if(!list) return;
    added=((uint32_t *(*)(uint32_t,uint32_t,uint32_t,uint32_t))(uintptr_t)
       P67_NATIVE(0x0c5883f1u))(list,p67_event_fn(s),0,s->generation);
    if(added) s->list=list;
}
static void p67_surface_poll(struct p67_surface *s) {
    uint32_t root,page,owner,*d,*a,*b,*c=0,g,list=0;
    if(!s->busy && s->owner && s->moving) p67_surface_refresh_list(s);
    if(!p67_surface_idle(s) || !p67_surface_path(s)[0]) return;
    root=load32(s->root_slot);
    if(s->owner && !s->dirty && s->root==root && p67_live(root) && p67_live(s->owner) &&
       (s!=&p67_surfaces[1] || s->list==load32(0x20110ee8u)) &&
       (s!=&p67_surfaces[2] || s->list==load32(0x201100dcu))) return;
    if(s->owner || s->root || s->swapped || s->list) p67_surface_detach(s);
    if(!p67_live(root) || load32(root)!=0x2cdba8b4u) return;
    page=((uint32_t (*)(uint32_t))(uintptr_t)P67_NATIVE(0x0c589579u))(root);
    if(!p67_live(page)) return;
    owner=((uint32_t (*)(uint32_t))(uintptr_t)P67_NATIVE(0x0c589579u))(page);
    if(!p67_live(owner)) return;
    d=p67_descriptor(owner,s->callback);
    if(!d || d[1]!=page || d[2]) return;
    if(s==&p67_surfaces[1]) list=load32(0x20110ee8u);
    if(s==&p67_surfaces[2]) list=load32(0x201100dcu);
    if(list && (!p67_live(list) || load32(list)!=0x2cdba28cu ||
                !p67_surface_descendant(list,root))) return;
    g=++p67_surface_serial;
    a=((uint32_t *(*)(uint32_t,uint32_t,uint32_t,uint32_t))(uintptr_t)
       P67_NATIVE(0x0c5883f1u))(owner,p67_event_fn(s),36,g);
    b=a?((uint32_t *(*)(uint32_t,uint32_t,uint32_t,uint32_t))(uintptr_t)
       P67_NATIVE(0x0c5883f1u))(root,p67_event_fn(s),0,g):0;
    if(b && list) c=((uint32_t *(*)(uint32_t,uint32_t,uint32_t,uint32_t))(uintptr_t)
       P67_NATIVE(0x0c5883f1u))(list,p67_event_fn(s),0,g);
    d=p67_descriptor(owner,s->callback);
    if((list && !c) || !a || !b || !d || d[1]!=page || d[2]) {
        if(a) ((int (*)(uint32_t,uint32_t))(uintptr_t)P67_NATIVE(0x002bf811u))(owner,p67_event_fn(s));
        if(c) ((int (*)(uint32_t,uint32_t))(uintptr_t)P67_NATIVE(0x002bf811u))(list,p67_event_fn(s));
        if(b) ((int (*)(uint32_t,uint32_t))(uintptr_t)P67_NATIVE(0x002bf811u))(root,p67_event_fn(s));
        return;
    }
    s->owner=owner;s->page=page;s->root=root;s->generation=g;s->list=list;
    d[0]=p67_scroll_fn(s);
}
void rh_platform_backgrounds_poll(void) {
    unsigned i; p67_launcher_poll();
    for(i=0;i<3;++i) p67_surface_poll(&p67_surfaces[i]);
}
int rh_platform_backgrounds_stage(const struct rh_backgrounds *bg) {
    struct rh_backgrounds one; const char *paths[4]; unsigned i,j; int rc;
    if(!bg) return -2142;
    paths[0]=bg->launcher;paths[1]=bg->control_center;paths[2]=bg->notifications;paths[3]=bg->widgets;
    for(i=0;i<4;++i) {
        one.control_center[0]=one.notifications[0]=one.widgets[0]=0;
        for(j=0;j<RH_PATH;++j) { one.launcher[j]=paths[i][j]; if(!paths[i][j]) break; }
        rc=p67_launcher_stage(&one); if(rc) return rc;
    }
    return 0;
}
int rh_platform_backgrounds_commit(const struct rh_backgrounds *bg) {
    struct rh_backgrounds one; const char *paths[3]; unsigned i,j; int rc;
    if(!bg) return -2142;
    if(rh_background_is_blur(bg->control_center) || rh_background_is_blur(bg->notifications) || rh_background_is_blur(bg->widgets)) return -2144;
    for(i=0;i<3;++i) if(!p67_surface_idle(&p67_surfaces[i])) return -2145;
    one.control_center[0]=one.notifications[0]=one.widgets[0]=0;
    for(j=0;j<RH_PATH;++j) { one.launcher[j]=bg->launcher[j]; if(!bg->launcher[j]) break; }
    rc=p67_launcher_commit(&one);if(rc) return rc;
    paths[0]=bg->control_center;paths[1]=bg->notifications;paths[2]=bg->widgets;
    for(i=0;i<3;++i) {
        p67_surface_detach(&p67_surfaces[i]);
        p67_surfaces[i].path_index^=1u;
        for(j=0;j<RH_PATH;++j) {
            char *dest=p67_surfaces[i].path_index?p67_surfaces[i].alternate:p67_surfaces[i].path;
            dest[j]=paths[i][j]; if(!paths[i][j]) break;
        }
    }
    rh_platform_backgrounds_poll(); p67_wake_pending=1;
    return 0;
}

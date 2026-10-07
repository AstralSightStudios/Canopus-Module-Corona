#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "resource_hook_background.h"
#define RH_TARGET_1043 1
#define RH_RELOAD_LIMIT 1024u
#define RH_FW_OBJECT_TREE_WALK 1u
#define RH_FW_IMAGE_GET_INFO 2u
#define RH_FW_IMAGE_CLASS 0x2cce61ecu
#define RH_FW_HEADER_CACHE_SLOT 3u
#define RH_FW_IMAGE_CACHE_SLOT 4u
#define RH_FW_HEADER_CACHE_CLASS 5u
#define RH_FW_IMAGE_CACHE_CLASS 6u
#define OWNER 100u
#define PAGE 200u
#define FOREGROUND 300u
#define LIST 400u
#define NEW_LIST 500u
static uintptr_t native(uint32_t address);
static unsigned char *byte_at(uint32_t address);
static uint32_t *word_at(uint32_t address);
#define P67_NATIVE(address) native(address)
#define P67_U8(address) (*byte_at(address))
#define P67_U32(address) (*word_at(address))
struct object { uint32_t id, parent, clz, y, bounds[4], count, d[8][3]; int live; };
static struct object objects[5];
static unsigned char moving;
static uint32_t surface_root, surface_image;
static int hidden_root, hidden_image, capture_fail;
static unsigned cleanup, selected, first_enter, cache_reuses, captures;
static uint32_t populated;
static int delete_on_flag, delete_on_remove, shared_mode;
static uint32_t root_slot, image_slot, foreground_slot;
static unsigned add_calls, fail_add, mutate_binding, forwards[64], preparations, draws;
static int forbid_mutation;
static unsigned tree_calls;
static uint32_t layer_clip[4];
static int32_t expected_clip[4];
static uint32_t last_prepared;
static struct object *object(uint32_t id) {
    for (unsigned i = 0; i < 5; ++i) if (objects[i].id == id) return &objects[i];
    assert(!"unknown object"); return 0;
}
static unsigned char *byte_at(uint32_t a) { assert(a == 0x201102d9u); return &moving; }
static uint32_t *word_at(uint32_t a) {
    if (a >= 77u + 24u && a <= 77u + 36u) return &layer_clip[(a - 77u - 24u) / 4u];
    for (unsigned i = 0; i < 5; ++i) {
        struct object *s = &objects[i];
        if (a >= s->id + 28u && a <= s->id + 40u) {
            unsigned index = (a - s->id - 28u) / 4u;
            return index == 1 ? &s->y : &s->bounds[index];
        }
    }
    assert(!"unknown word"); return 0;
}
static uint32_t load32(uint32_t a) {
    if(shared_mode) {
      if(a==0x2010fe50u) return LIST;
      if(a==0x20110ef4u) return FOREGROUND;
      if(a==0x201100e0u || a==0x2010fe64u || a==0x20110ed4u || a==0x201100ccu) return 0;
    }
    if(a==0x2010fe50u || a==0x20110ef4u || a==0x201100e0u) return surface_root;
    if(a==0x2010fe64u || a==0x20110ed4u || a==0x201100ccu) return surface_image;
    if(a==0x20110ee8u || a==0x201100dcu) return populated;
    if (a == 0x201102ecu) return root_slot;
    if (a == 0x201102dcu) return image_slot;
    if (a == 0x201102f4u) return foreground_slot;
    if (a == RH_FW_HEADER_CACHE_SLOT || a == RH_FW_IMAGE_CACHE_SLOT) return 0;
    return object(a)->clz;
}
static int cache_valid(uint32_t a, uint32_t b) { (void)a; (void)b; return 1; }
static int retire_cache(uint32_t a, uint32_t b, const struct rh_mapping_view *v, const char *p) {
    (void)a; (void)b; (void)v; (void)p; return 0;
}
static int rh_platform_open(const char *p, int flags) { (void)p; assert(flags == 0); return 1; }
static void rh_platform_close(int fd) { assert(fd == 1); }
static unsigned redraw_requests;
static int redraw_result;
static int rh_platform_request_full_redraw(void) { ++redraw_requests; return redraw_result; }
#include "../src/p67_backgrounds.inc"
static void tree(uint32_t root, int (*cb)(uint32_t, void *), void *cookie) {
    assert(root == 0); ++tree_calls;
    for (unsigned i = 0; i < 5; ++i) if (objects[i].live && cb(objects[i].id, cookie) == 2) break;
}
static uint32_t count(uint32_t o) { return object(o)->count; }
static uint32_t *get(uint32_t o, uint32_t index) {
    struct object *s = object(o); assert(s->live && index < s->count); return s->d[index];
}
static uint32_t parent(uint32_t o) { assert(object(o)->live); return object(o)->parent; }
static uint32_t *add(uint32_t o, uint32_t cb, uint32_t filter, uint32_t user) {
    struct object *s = object(o); assert(s->live && !forbid_mutation);
    if (++add_calls == fail_add) return 0;
    if (mutate_binding && add_calls == 2) object(OWNER)->d[0][1] = FOREGROUND;
    assert(s->count < 8); uint32_t *d = s->d[s->count++];
    d[0] = cb; d[1] = user; d[2] = filter; return d;
}
static int remove_event(uint32_t o, uint32_t cb) {
    struct object *s = object(o); assert(s->live && !forbid_mutation);
    for (unsigned i = 0; i < s->count; ++i) if (s->d[i][0] == cb) {
        memmove(&s->d[i], &s->d[i + 1], (s->count - i - 1) * sizeof(s->d[0]));
        --s->count; return 1;
    }
    return 0;
}
static void original(void *e) {
    unsigned code = *(uint16_t *)((unsigned char *)e + 8) & 0x7fffu; assert(code < 64);
    ++forwards[code]; if (code == 11) moving = 0;
}
static void prepare(uint32_t list, int a, int b) {
    assert(!a && !b); ++preparations; last_prepared = list;
}
static void rect_init(void *d) { memset(d, 0, 188); }
static void rect_draw(uint32_t layer, const void *d, const int32_t *area) {
    assert(layer == 77 && ((const uint32_t *)d)[30]);
    assert(!area[0] && !area[1] && area[2] == 335 && area[3] == 479);
    for (unsigned i = 0; i < 4; ++i) assert((int32_t)layer_clip[i] == expected_clip[i]);
    ++draws;
}
static int image_info(const char *p, void *out) { (void)out; assert(p[0]); return 1; }
static void flag_add(uint32_t o,uint32_t f) { assert(f==1 && !forbid_mutation);
 if(delete_on_flag && o==NEW_LIST) {
  delete_on_flag=0;
  uint32_t e[5]={LIST,LIST,36,p67_surfaces[selected].generation,77};
  p67_surface_event(e);object(LIST)->live=0;surface_root=0;
 }
 if(o==LIST) hidden_root=1; else {assert(o==NEW_LIST);hidden_image=1;} }
static void flag_remove(uint32_t o,uint32_t f) { assert(f==1 && !forbid_mutation);
 if(delete_on_remove && o==NEW_LIST) {
  delete_on_remove=0;uint32_t e[5]={LIST,LIST,36,p67_surfaces[selected].generation,77};
  p67_surface_event(e);object(LIST)->live=0;surface_root=0;
 }
 if(o==LIST) hidden_root=0; else {assert(o==NEW_LIST);hidden_image=0;} }
static int flag_has(uint32_t o,uint32_t f) { assert(f==1);if(shared_mode && o==FOREGROUND)return 0;if(o==LIST)return hidden_root;assert(o==NEW_LIST);return hidden_image; }
static void surface_original(void *e) {
 unsigned code=*(uint16_t *)((char *)e+8)&0x7fff; assert(code<64); ++forwards[code];
 if(code==9) {
  if(selected==2 && !first_enter++) {
   if(surface_image) {++cache_reuses;hidden_root=1;hidden_image=0;}
  } else if(!capture_fail) {++captures;surface_image=NEW_LIST;hidden_root=1;hidden_image=0;}
 }
 if(code==11 && p67_terminal(&p67_surfaces[selected],PAGE)) { if(surface_image) {assert(hidden_root && !hidden_image); ++cleanup;} hidden_root=0;hidden_image=1; }
}
static unsigned native_cc_calls,native_notify_calls,native_widget_calls;
static void original_cc(void *e) { ++native_cc_calls;surface_original(e); }
static void original_notify(void *e) { ++native_notify_calls;surface_original(e); }
static void original_widget(void *e) { ++native_widget_calls;surface_original(e); }
static uintptr_t native(uint32_t a) {
    switch (a) {
    case 0x0c5036c5u:return (uintptr_t)original_cc;
    case 0x0c527bbdu:return (uintptr_t)original_notify;
    case 0x0c50abb5u:return (uintptr_t)original_widget;
    case 0x0c588961u:return (uintptr_t)flag_add;
    case 0x0c5891b1u:return (uintptr_t)flag_remove;
    case 0x0c5881c1u:return (uintptr_t)flag_has;
    case RH_FW_OBJECT_TREE_WALK: return (uintptr_t)tree;
    case RH_FW_IMAGE_GET_INFO: return (uintptr_t)image_info;
    case 0x002bf7d5u: return (uintptr_t)count;
    case 0x002bf7e9u: return (uintptr_t)get;
    case 0x002bf811u: return (uintptr_t)remove_event;
    case 0x0c589579u: return (uintptr_t)parent;
    case 0x0c5883f1u: return (uintptr_t)add;
    case P67_SCROLL_CB: return (uintptr_t)original;
    case 0x0c5166f1u: return (uintptr_t)prepare;
    case 0x0c588269u: return (uintptr_t)rect_init;
    case 0x0c588351u: return (uintptr_t)rect_draw;
    default: assert(!"unknown native call"); return 0;
    }
}
static void event(uint32_t e[5], uint32_t current, uint32_t target, unsigned code, uint32_t user) {
    e[0] = current; e[1] = target; e[2] = code; e[3] = user; e[4] = 77;
}
/* Model descriptor ALL delivery: each callback receives its own userdata,
 * including the native page binding (not the adapter generation) on owner. */
static void dispatch(struct p67_surface *s, uint32_t obj, unsigned code) {
    struct object *o=object(obj);
    for(unsigned n=0;n<o->count;++n) {
        uint32_t *d=o->d[n], e[5];
        if(d[2] && d[2]!=code) continue;
        event(e,obj,obj,code,d[1]);
        if(d[0]==p67_scroll_fn(s)) p67_surface_scroll_for(s,e);
        else { assert(d[0]==p67_event_fn(s)); p67_surface_event(e); }
    }
}
static void check_non_gestures(struct p67_surface *s) {
    const unsigned codes[]={1,20,21,23,24,25,26,27,28,32,33,47};
    struct p67_surface before=*s;
    struct object saved_objects[5];
    memcpy(saved_objects,objects,sizeof(objects));
    unsigned adds=add_calls;
    /* A newly populated list must not register from an ordinary/render ALL. */
    populated=FOREGROUND;
    object(FOREGROUND)->clz=0x2cdba28cu;object(FOREGROUND)->parent=LIST;
    memcpy(saved_objects,objects,sizeof(objects));
    /* Stronger than a render-only guard: all unaudited ALL events, including
     * SIZE_CHANGED/STYLE_CHANGED/GET_SELF_SIZE, must be mutation-free. */
    forbid_mutation=1;
    for(unsigned n=0;n<sizeof(codes)/sizeof(codes[0]);++n) {
        unsigned calls=forwards[codes[n]];
        dispatch(s,OWNER,codes[n]);
        assert(forwards[codes[n]]==calls+1);
        assert(!memcmp(&before,s,sizeof(before)));
        assert(!memcmp(saved_objects,objects,sizeof(objects)));
        assert(add_calls==adds && !hidden_root && hidden_image);
    }
    /* Render phases traverse owner ALL before the custom root draw. */
    unsigned rendered=draws;
    for(unsigned code=23;code<=28;++code) {
        dispatch(s,OWNER,code);dispatch(s,LIST,code);
        assert(!memcmp(&before,s,sizeof(before)));
    }
    assert(draws==rendered+1);
    forbid_mutation=0;populated=0;
}
static void check_notification_scroll(struct p67_surface *s) {
    struct p67_surface before=*s;
    struct object saved_objects[5];
    memcpy(saved_objects,objects,sizeof(objects));
    unsigned walks=tree_calls, calls=forwards[12], adds=add_calls;
    unsigned snapshots=captures, released=cleanup;
    assert(selected==1 && s->moving && s->swapped);
    forbid_mutation=1;
    for(unsigned n=0;n<128;++n) {
        dispatch(s,OWNER,12);
        assert(forwards[12]==calls+n+1);
        assert(tree_calls==walks && add_calls==adds);
        assert(!memcmp(&before,s,sizeof(before)));
        assert(!memcmp(saved_objects,objects,sizeof(objects)));
        assert(!hidden_root && hidden_image);
        assert(captures==snapshots && cleanup==released);
    }
    forbid_mutation=0;
}
static void reset(void) {
    forbid_mutation=0; tree_calls=0;
    memset(objects, 0, sizeof(objects));
    uint32_t ids[5] = {OWNER, PAGE, FOREGROUND, LIST, NEW_LIST};
    for (unsigned i = 0; i < 5; ++i) { objects[i].id = ids[i]; objects[i].live = 1; }
    object(PAGE)->parent = OWNER; object(FOREGROUND)->parent = PAGE;
    object(LIST)->clz = object(NEW_LIST)->clz = 0x2cd2024cu;
    object(OWNER)->d[0][0] = P67_SCROLL_CB;
    object(OWNER)->d[0][1] = PAGE;
    object(OWNER)->count = 1;
    root_slot = LIST; foreground_slot = FOREGROUND; image_slot = moving = 0;
    p67_owner = p67_page = p67_draw_owner = p67_generation = p67_busy = p67_wake_pending = 0;
    redraw_requests = 0; redraw_result = 0;
    p67_path_index = 0; memset(p67_path, 0, sizeof(p67_path));
    strcpy(p67_path[0], "/data/static.bin");
    add_calls = fail_add = mutate_binding = preparations = draws = 0; memset(forwards, 0, sizeof(forwards));
    object(PAGE)->bounds[0] = 0; object(PAGE)->bounds[2] = 335; object(PAGE)->bounds[3] = 479;
    for (unsigned i = 0; i < 4; ++i) layer_clip[i] = (uint32_t)(expected_clip[i] = (int32_t)object(PAGE)->bounds[i]);
}
int main(void) {
 uint32_t e[5];
 for(unsigned i=0;i<3;++i) {
  reset();selected=i;first_enter=cache_reuses=captures=0;populated=0;delete_on_flag=0;p67_path[0][0]=0;
  struct p67_surface *s=&p67_surfaces[i];
  s->owner=s->page=s->root=s->list=s->moving=s->swapped=s->busy=0;
  surface_root=LIST;surface_image=NEW_LIST;hidden_root=0;hidden_image=1;capture_fail=0;cleanup=0;
  object(LIST)->clz=0x2cdba8b4u;object(NEW_LIST)->clz=RH_FW_IMAGE_CLASS;
  object(LIST)->parent=object(NEW_LIST)->parent=PAGE;
  object(OWNER)->d[0][0]=s->callback;strcpy(s->path,"/data/static.bin");
  fail_add=2;p67_surface_poll(s);assert(!s->owner && object(OWNER)->count==1);
  fail_add=0;p67_surface_poll(s);assert(s->owner==OWNER);
  event(e,OWNER,LIST,9,PAGE);p67_surface_scroll_for(s,e);
  assert(forwards[9]==1 && s->moving && s->swapped && !hidden_root && hidden_image);
  if(i==2) assert(first_enter==1 && cache_reuses==1 && !captures);
  assert(object(OWNER)->d[0][1]==PAGE && !object(OWNER)->d[0][2]);
  check_non_gestures(s);draws=0;
  /* Intermediate native end is not terminal and must stay live/busy. */
  if(i==1) object(PAGE)->y=(uint32_t)-100;
  else object(PAGE)->bounds[0]=i==0?(uint32_t)-100:100;
  event(e,OWNER,LIST,11,PAGE);p67_surface_scroll_for(s,e);
  assert(!cleanup && s->moving && s->swapped && !hidden_root && hidden_image);
  struct rh_backgrounds bg;memset(&bg,0,sizeof(bg));
  assert(rh_platform_backgrounds_commit(&bg)==-2145);
  /* Reversal begin preserves native bookkeeping then terminal end restores. */
  if(i==1) object(PAGE)->y=0;else object(PAGE)->bounds[0]=0;
  event(e,OWNER,LIST,9,PAGE);p67_surface_scroll_for(s,e);assert(s->swapped);
  if(i>0) {
   populated=FOREGROUND;object(FOREGROUND)->clz=0x2cdba28cu;object(FOREGROUND)->parent=LIST;
   p67_surface_poll(s);assert(s->list==FOREGROUND && object(FOREGROUND)->count==1);
   event(e,FOREGROUND,FOREGROUND,36,s->generation);p67_surface_event(e);
   object(FOREGROUND)->live=0;populated=0;
   assert(s->moving && !s->list);
   object(FOREGROUND)->live=1;object(FOREGROUND)->count=0;populated=FOREGROUND;
   p67_surface_poll(s);assert(s->moving && s->list==FOREGROUND);
  }
  if(i==1) check_notification_scroll(s);
  else {
   unsigned walks=tree_calls;
   event(e,OWNER,LIST,12,PAGE);p67_surface_scroll_for(s,e);
   assert(forwards[12]==1 && s->swapped && tree_calls>walks);
  }
  object(PAGE)->y=120;object(PAGE)->bounds[3]=599;
  layer_clip[1]=100;layer_clip[3]=600;expected_clip[1]=120;expected_clip[3]=479;
  uint32_t saved[4];memcpy(saved,layer_clip,sizeof(saved));
  event(e,LIST,LIST,25,s->generation);p67_surface_event(e);assert(draws==1);
  assert(!memcmp(saved,layer_clip,sizeof(saved)));
  object(PAGE)->y=480;p67_surface_event(e);assert(draws==1);
  object(PAGE)->y=0;
  event(e,OWNER,LIST,11,PAGE);p67_surface_scroll_for(s,e);
  assert(cleanup==1 && !s->moving && !s->swapped && !hidden_root);
  surface_image=0;capture_fail=1;
  event(e,OWNER,LIST,9,PAGE);p67_surface_scroll_for(s,e);assert(!s->swapped);
  event(e,OWNER,LIST,11,PAGE);p67_surface_scroll_for(s,e);
  /* Direct owner DELETE restores native wrapper immediately; survivor cleanup
   * is deferred without walking the deleting subtree. */
  event(e,OWNER,OWNER,36,s->generation);p67_surface_event(e);
  assert(!s->owner && s->dirty && object(OWNER)->d[0][0]==s->callback);
  object(OWNER)->live=0;p67_surface_detach(s);
  assert(!s->owner && !object(LIST)->count);object(OWNER)->live=1;
  assert(!object(FOREGROUND)->count);
  assert(object(OWNER)->d[0][0]==s->callback);
  p67_surface_poll(s);assert(s->owner);
  capture_fail=0;surface_image=NEW_LIST;hidden_image=1;
  delete_on_flag=1;
  event(e,OWNER,LIST,9,PAGE);p67_surface_scroll_for(s,e);
  assert(!s->root && !object(LIST)->live && !s->swapped);
  p67_surface_detach(s);
  /* Native-visibility restoration itself may delete the live root. */
  object(LIST)->live=1;object(LIST)->count=0;surface_root=LIST;hidden_root=0;hidden_image=1;
  s->root=LIST;s->page=PAGE;s->swapped=1;delete_on_remove=1;
  p67_surface_native(s);assert(!s->root && !hidden_root && s->swapped);
  p67_surface_detach(s);
  s->path[0]=0;
 }
 /* Two page bindings on one owner have unique wrapper/event identities. */
 reset();p67_path[0][0]=0;shared_mode=1;populated=0;hidden_root=0;
 object(LIST)->clz=object(FOREGROUND)->clz=0x2cdba8b4u;
 object(LIST)->parent=PAGE;object(FOREGROUND)->parent=NEW_LIST;object(NEW_LIST)->parent=OWNER;
 object(OWNER)->count=2;
 object(OWNER)->d[0][0]=p67_surfaces[0].callback;object(OWNER)->d[0][1]=PAGE;
 object(OWNER)->d[1][0]=p67_surfaces[1].callback;object(OWNER)->d[1][1]=NEW_LIST;
 for(unsigned i=0;i<2;++i) {strcpy(p67_surfaces[i].path,"/data/static.bin");p67_surfaces[i].path_index=0;p67_surface_poll(&p67_surfaces[i]);assert(p67_surfaces[i].owner==OWNER);}
 assert(p67_surfaces[0].generation!=p67_surfaces[1].generation);
 assert(object(OWNER)->count==4);
 p67_surface_detach(&p67_surfaces[0]);assert(object(OWNER)->count==3);
 assert(p67_descriptor(OWNER,p67_scroll_fn(&p67_surfaces[1])));
 p67_surface_detach(&p67_surfaces[1]);assert(object(OWNER)->count==2);
 assert(p67_descriptor(OWNER,p67_surfaces[0].callback) && p67_descriptor(OWNER,p67_surfaces[1].callback));
 /* Same owner AND same page: wrapper identity, not a userdata loop,
  * determines which native callback is forwarded. */
 p67_surfaces[0].owner=p67_surfaces[2].owner=OWNER;
 p67_surfaces[0].page=p67_surfaces[2].page=PAGE;
 p67_surfaces[0].root=p67_surfaces[2].root=LIST;
 p67_surfaces[0].busy=p67_surfaces[2].busy=0;
 native_cc_calls=native_notify_calls=native_widget_calls=0;
 event(e,OWNER,LIST,20,PAGE);p67_scroll_widget(e);
 assert(native_widget_calls==1 && !native_cc_calls && !native_notify_calls);
 p67_scroll_cc(e);assert(native_cc_calls==1 && native_widget_calls==1);
 event(e,LIST,LIST,20,PAGE);p67_scroll_widget(e);assert(native_widget_calls==1);
 puts("p67 three native-owned live surfaces passed");return 0;
}

/* Actual production owner adapter with native leaf/memory injection. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "resource_hook_platform.h"
#include "resource_hook_background.h"
#include "resource_hook_target_p65.h"
#include "resource_hook_p65_background.h"
#define RH_RELOAD_LIMIT 1024u
struct cell {uint32_t address,value;};
static struct cell cells[4096];static unsigned cell_count;
static uint32_t *memory(uint32_t address) {
    for(unsigned i=0;i<cell_count;++i)if(cells[i].address==address)return &cells[i].value;
    assert(cell_count<4096);cells[cell_count]=(struct cell){address,0};return &cells[cell_count++].value;
}
#define P65_BG_U32(address) (*memory(address))
#define P65_BG_U8(address) (*(unsigned char *)memory(address))
struct object {uint32_t id,parent,flags,events[32][3];unsigned event_count,live;};
static struct object objects[20];static unsigned object_count,draws,opens,closes,retirements,native_calls,blur_polls,blur_reset_calls;
static uint32_t native_code,last_source,last_scale[2];static int fail_add,fail_info;
static uintptr_t ids[64];static unsigned id_count;
static uint32_t callback_id(uintptr_t callback) {
    for(unsigned i=0;i<id_count;++i)if(ids[i]==callback)return 0xa000u+4u*i;
    assert(id_count<64);ids[id_count]=callback;return 0xa000u+4u*id_count++;
}
#define P65_BG_CALLBACK(callback) callback_id((uintptr_t)(callback))
static struct object *object(uint32_t id) {for(unsigned i=0;i<object_count;++i)if(objects[i].id==id)return &objects[i];assert(0);return 0;}
static void walk(uint32_t root,int (*cb)(uint32_t,void *),void *cookie) {(void)root;for(unsigned i=0;i<object_count;++i)if(objects[i].live&&cb(objects[i].id,cookie)==2)return;}
static uint32_t parent(uint32_t id) {return object(id)->parent;}
static uint32_t count(uint32_t id) {return object(id)->event_count;}
static uint32_t *get(uint32_t id,unsigned i) {struct object *o=object(id);return i<o->event_count?o->events[i]:0;}
static uint32_t *add(uint32_t id,uint32_t cb,unsigned filter,uint32_t user) {struct object *o=object(id);if(fail_add)return 0;assert(o->event_count<32);uint32_t *d=o->events[o->event_count++];d[0]=cb;d[1]=user;d[2]=filter;return d;}
static int remove_event(uint32_t id,uint32_t cb) {struct object *o=object(id);for(unsigned i=0;i<o->event_count;++i)if(o->events[i][0]==cb){memmove(o->events[i],o->events[i+1],(o->event_count-i-1)*sizeof(o->events[0]));--o->event_count;return 1;}return 0;}
static int has_flag(uint32_t id,uint32_t flag){return !!(object(id)->flags&flag);}
static void add_flag(uint32_t id,uint32_t flag){object(id)->flags|=flag;}
static void clear_flag(uint32_t id,uint32_t flag){object(id)->flags&=~flag;}
static void image_init(void *d){memset(d,0,92);((uint32_t *)d)[5]=92;}
static int info(uintptr_t source,void *out){(void)source;if(fail_info)return 0;((uint32_t *)out)[1]=514u<<16|432u;return 1;}
static void draw(uint32_t layer,const void *d,const int32_t *a){const uint32_t *p=d;assert(layer==0x5000);assert(a[0]==0&&a[1]==0&&a[2]==431&&a[3]==513);assert(P65_BG_U32(layer+24)==0&&P65_BG_U32(layer+28)==0&&P65_BG_U32(layer+32)==431&&P65_BG_U32(layer+36)==513);last_source=p[7];last_scale[0]=p[12];last_scale[1]=p[13];++draws;}
static void native_scroll(void *event) {
    uint32_t owner=*(uint32_t *)event,page=((uint32_t *)event)[3];
    uint32_t root=owner==0x3000?0x3200:0x4200,image=owner==0x3000?0x3300:0x4300;
    uint32_t slot=owner==0x3000?RH_P65_BG_NOTIFY_IMAGE:RH_P65_BG_WIDGET_IMAGE;
    native_code=*(uint16_t *)((char *)event+8);++native_calls;
    assert(page==owner+0x100);
    if(native_code==9){P65_BG_U32(slot)=image;object(root)->flags|=1;object(image)->flags&=~1u;}
    if(native_code==11){assert(object(root)->flags&1u);assert(!(object(image)->flags&1u));P65_BG_U32(slot)=0;object(root)->flags&=~1u;}
}
static uintptr_t native(uint32_t address) {
    switch(address){
    case RH_FW_OBJECT_TREE_WALK:return (uintptr_t)walk;
    case RH_P65_BG_PARENT:return (uintptr_t)parent;
    case RH_P65_BG_EVENT_COUNT:return (uintptr_t)count;
    case RH_P65_BG_EVENT_GET:return (uintptr_t)get;
    case RH_P65_BG_EVENT_ADD:return (uintptr_t)add;
    case RH_P65_BG_EVENT_REMOVE:return (uintptr_t)remove_event;
    case RH_P65_BG_HAS_FLAG:return (uintptr_t)has_flag;
    case RH_P65_BG_ADD_FLAG:return (uintptr_t)add_flag;
    case RH_P65_BG_CLEAR_FLAG:return (uintptr_t)clear_flag;
    case RH_P65_BG_IMAGE_DSC_INIT:return (uintptr_t)image_init;
    case RH_P65_BG_DRAW_IMAGE:return (uintptr_t)draw;
    case RH_FW_IMAGE_GET_INFO:return (uintptr_t)info;
    case RH_P65_BG_NOTIFY_SCROLL:case RH_P65_BG_WIDGET_SCROLL:return (uintptr_t)native_scroll;
    default:assert(0);return 0;
    }
}
#define P65_BG_NATIVE(address) native(address)
int rh_platform_open(const char *path,int mode){assert(path&&path[0]&&mode==RH_FILE_READ);++opens;return 7;}
void rh_platform_close(int fd){assert(fd==7);++closes;}
int rh_platform_request_full_redraw(void){return 0;}
static int cache_valid(uint32_t cache,uint32_t clz){(void)cache;(void)clz;return 1;}
static int retire_cache(uint32_t cache,uint32_t offset,const struct rh_mapping_view *view,const char *path){(void)cache;(void)offset;assert(!view&&path&&path[0]);++retirements;return 0;}
#include "../src/p65_backgrounds.inc"
static void make_object(uint32_t id,uint32_t p){assert(object_count<20);objects[object_count++]=(struct object){.id=id,.parent=p,.live=1};P65_BG_U32(id+28)=P65_BG_U32(id+32)=0;P65_BG_U32(id+36)=431;P65_BG_U32(id+40)=513;}
static void event(uint32_t obj,uint32_t user,unsigned code,uint32_t layer){uint32_t e[7]={obj,obj,code,user,layer,0,0};p65_bg_event(e);}
int main(void){
    struct rh_backgrounds bg={0},system={0};
    make_object(0x1000,0);make_object(0x2000,0);
    make_object(0x3000,0);make_object(0x3100,0x3000);make_object(0x3200,0x3100);make_object(0x3300,0x3100);
    make_object(0x4000,0);make_object(0x4100,0x4000);make_object(0x4200,0x4100);make_object(0x4300,0x4100);
    P65_BG_U32(RH_P65_BG_LAUNCHER_ROOT)=0x1000;P65_BG_U32(RH_P65_BG_CONTROL_ROOT)=0x2000;
    P65_BG_U32(RH_P65_BG_NOTIFY_ROOT)=0x3200;P65_BG_U32(RH_P65_BG_WIDGET_ROOT)=0x4200;
    P65_BG_U32(0x3300)=P65_BG_U32(0x4300)=RH_FW_IMAGE_OBJECT_CLASS;
    for(unsigned i=0;i<3;++i){unsigned filter=i==0?9:i==1?11:12;add(0x3000,RH_P65_BG_NOTIFY_SCROLL,filter,0x3100);add(0x4000,RH_P65_BG_WIDGET_SCROLL,filter,0x4100);}
    add(0x4000,RH_P65_BG_WIDGET_SCROLL,19,0x4100);
    strcpy(bg.launcher,"/themes/demo/launcher.bin");strcpy(bg.control_center,"/themes/demo/cc.bin");strcpy(bg.notifications,"/themes/demo/notify.bin");strcpy(bg.widgets,"/themes/demo/widgets.bin");
    assert(!rh_platform_backgrounds_stage(&bg));assert(opens==4&&closes==4);
    assert(!rh_platform_backgrounds_commit(&bg));
    for(unsigned i=0;i<4;++i){struct p65_bg_surface *s=&p65_bg_surfaces[i];assert(s->root);assert(object(s->root)->event_count);P65_BG_U32(0x5000+24)=P65_BG_U32(0x5000+28)=0;P65_BG_U32(0x5000+32)=700;P65_BG_U32(0x5000+36)=800;event(s->root,s->generation,24,0x5000);assert(last_scale[0]==256&&last_scale[1]==256);assert(P65_BG_U32(0x5000+32)==700&&P65_BG_U32(0x5000+36)==800);}
    assert(draws==4);event(0x1000,0,24,0x5000);assert(draws==4);event(0x1000,p65_bg_surfaces[0].generation,25,0x5000);assert(draws==4);
    for(unsigned i=2;i<4;++i){struct p65_bg_surface *s=&p65_bg_surfaces[i];uint32_t e[7]={s->owner,s->owner,9,s->page,0,0,0};p65_bg_scroll(s,e);assert(s->swapped);assert(!p65_bg_hidden(s->root));assert(p65_bg_hidden(P65_BG_U32(s->image_slot)));assert(rh_platform_backgrounds_commit(&system)==-2145);e[2]=11;p65_bg_scroll(s,e);assert(!s->swapped&&!P65_BG_U32(s->image_slot));assert(!p65_bg_hidden(s->root));}
    assert(native_calls==4);
    assert(!rh_platform_backgrounds_commit(&system));for(unsigned i=0;i<4;++i)assert(!p65_bg_surfaces[i].root);
    assert(!object(0x1000)->event_count&&!object(0x2000)->event_count);for(unsigned i=0;i<object(0x3000)->event_count;++i)assert(object(0x3000)->events[i][0]==RH_P65_BG_NOTIFY_SCROLL);
    fail_add=1;assert(!rh_platform_backgrounds_commit(&bg));for(unsigned i=0;i<4;++i)assert(!p65_bg_surfaces[i].root);fail_add=0;rh_platform_backgrounds_poll();assert(p65_bg_surfaces[0].root);
    uint32_t oldgen=p65_bg_surfaces[0].generation;event(0x1000,oldgen,35,0);object(0x1000)->live=0;assert(!p65_bg_surfaces[0].root);event(0x1000,oldgen,24,0x5000);assert(draws==4);
    assert(!rh_platform_backgrounds_commit(&system));strcpy(bg.launcher,":blur");bg.control_center[0]=bg.notifications[0]=bg.widgets[0]=0;assert(!rh_platform_backgrounds_stage(&bg));assert(!rh_platform_backgrounds_commit(&bg));assert(blur_polls&&blur_reset_calls);
    fail_info=1;strcpy(bg.launcher,"/bad.bin");assert(rh_platform_backgrounds_stage(&bg)==-2143);
    puts("P65 all4 backgrounds: stages, draw/clip, native swipe restoration, delete/recreate, defaults/failures passed");return 0;
}

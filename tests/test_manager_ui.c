/* Native UI event integration against the real SDK tree validator. Data and
 * assets are adapters here; their independent suites exercise actual I/O. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "manager_ui.h"
#include "manager_state.h"
#include "manager_assets.h"
#include "canopus_ui.h"

static struct canopus_context_v1 context;
static struct canopus_ui_context_v1 ui;
static const struct canopus_ui_entry_v1 *entry;
static canopus_ui_task_fn pending;
static struct rh_module_status_snapshot status={.state=RH_MGR_STATE_RUNNING,.active_rules=3};
static struct rh_transfer_ui_state transfer;
static rh_manager_state_listener listener;
static void *listener_cookie;
static uint32_t width=212,height=520,move_from,move_to,moves,selected,chooses,deletes,reloads,captured,released,largest_y,prepared,completed,failed_move;
static int close_count,fail_delete,fail_complete,reject_apply,data_pending,dense_metadata,fail_create,fail_refresh,delete_protected;
/* Reload notice coverage: the card is the only surface for a reload result, so
 * the mock has to be able to fail each stage and to time a transaction out. */
static int fail_prepare,fail_start,failed_complete,reject_reload;
/* Set for the multi-page pass: the modelled host then answers ctx->ui->route. */
static int multi_page;
static uint32_t refresh_count,applies;
static uint32_t subscription_count;
/* The module may hold several timers at once: the drag edge timer, the hold
 * timer, the reload watchdog and a notice dwell. Slots are addressed by id, so
 * a destroyed timer cannot be fired by accident. */
#define TIMERS 8
static canopus_timer_callback_t timer_cb[TIMERS];
static uint32_t timer_period[TIMERS];
static int timer_live_ms(uint32_t ms) {int i;for(i=0;i<TIMERS;i++) if(timer_cb[i] && timer_period[i]==ms) return 1;return 0;}
static int timer_live(void) {int i;for(i=0;i<TIMERS;i++) if(timer_cb[i]) return 1;return 0;}
static void timer_fire(uint32_t ms) {
    int i;
    for(i=0;i<TIMERS;i++) if(timer_cb[i] && timer_period[i]==ms) {
        canopus_timer_callback_t cb=timer_cb[i];cb(&context,(int32_t)i+1,0);return;
    }
    fprintf(stderr,"No live %ums timer\n",(unsigned)ms);assert(0);
}
static char applied_dir[64];
#ifdef RH_MANAGER_UI_TEST
static char probe_snapshot[257];static uint32_t probe_used;
static int32_t probe_open(const struct canopus_context_v1 *c,const char *path,uint32_t flags) {
    (void)c;assert(!strcmp(path,"ui-probe.txt") && flags==CANOPUS_FS_WRITE);probe_used=0;probe_snapshot[0]=0;return 99;
}
static int32_t probe_write(const struct canopus_context_v1 *c,int32_t fd,const void *data,uint32_t n) {
    (void)c;assert(fd==99 && probe_used+n<sizeof(probe_snapshot));memcpy(probe_snapshot+probe_used,data,n);probe_used+=n;probe_snapshot[probe_used]=0;return (int32_t)n;
}
static int32_t probe_close(const struct canopus_context_v1 *c,int32_t fd) {(void)c;assert(fd==99);return 0;}
static uint32_t probe_value(const char *name) {
    char key[40];const char *p;snprintf(key,sizeof(key),"%s=",name);p=strstr(probe_snapshot,key);assert(p);return (uint32_t)strtoul(p+strlen(key),0,10);
}
#endif
static int32_t backend_apply(void *cookie,const struct canopus_ui_snapshot_v1 *s) {
    uint32_t i;(void)cookie;assert(s->node_count<=32);assert(s->string_used<=1536);
    for(i=0;i<s->node_count;i++) {
        uint16_t type=s->nodes[i].type;const struct canopus_ui_layout_v1 *l=&s->layouts[i];
        /* Exact constraints in canopus_ui_host.c:host_validate, shared by
           .043/.155. Runtime capabilities alone do not validate these. All three
           shipped targets bind FW_UI_STYLE_OPA (.043 through
           EVID-UI-1043-OPACITY-001), so opacity is legal; the range and the
           explicit-flag rule still have to hold. */
        assert(l->grow==0 && l->shrink==1);
        assert(s->styles[i].opacity<=1000u);
        assert(s->styles[i].border_width<=0);
        if(type==CANOPUS_UI_NODE_STATUS_ROW || type==CANOPUS_UI_NODE_ACTION_ROW || type==CANOPUS_UI_NODE_BUTTON || type==CANOPUS_UI_NODE_SWITCH_ROW)
            assert(!l->padding_left && !l->padding_right && !l->padding_top && !l->padding_bottom);
        if(type==CANOPUS_UI_NODE_IMAGE) assert(l->width>0 && l->height>0);
        if(type==CANOPUS_UI_NODE_NAVIGATION_HEADER)
            assert(!(s->visuals[i].flags&CANOPUS_UI_VISUAL_FLAG_VIRTUAL_CONTENT));
    }
    if(reject_apply) {reject_apply=0;return CANOPUS_UI_ERR_BACKEND;}
    applies++;
    return 0;
}
static int32_t backend_control(void *cookie,uint32_t gen,uint32_t key,uint32_t op,int32_t a,int32_t b,void *out) {
    (void)cookie;(void)gen;(void)key;(void)b;
    if(op==CANOPUS_UI_CONTROL_CAPTURE) captured++;
    if(op==CANOPUS_UI_CONTROL_RELEASE_CAPTURE) released++;
    if(op==CANOPUS_UI_CONTROL_TRANSLATE && a==0) {uint32_t v=b<0?0:(uint32_t)b;if(v>largest_y)largest_y=v;}
    if(op==CANOPUS_UI_CONTROL_SCROLL_QUERY) {struct canopus_ui_scroll_metrics_v1 *m=out;m->y=0;m->viewport_height=height==480?424:444;m->content_height=41632;}
    return 0;
}
static const struct canopus_ui_backend_v1 backend={sizeof(backend),2,0,(1u<<18)-1,backend_apply,backend_control};
static int32_t set_entry(const struct canopus_context_v1 *c,const struct canopus_ui_entry_v1 *e) {(void)c;entry=e;return 0;}
static int32_t post(const struct canopus_context_v1 *c,canopus_ui_task_fn cb,void *cookie) {(void)c;(void)cookie;assert(!pending||pending==cb);pending=cb;return 0;}
static int32_t close_page(const struct canopus_context_v1 *c) {(void)c;close_count++;entry->on_close(&context,0);return 0;}
/* Multi-page entries: the modelled host moves to the destination page and
 * drops the old view, exactly like the target backend after page_goto. */
static uint32_t routed_routes;
static int32_t route_page(const struct canopus_context_v1 *c,uint32_t r) {(void)c;assert(multi_page);assert(r<5);routed_routes++;return 0;}
static int32_t publish_launcher(const struct canopus_context_v1 *c,const struct canopus_app_entry_v1 *e) {(void)c;assert(!strcmp(e->label,"Corona"));assert(e->icon);assert(!e->pages&&!e->page_count);return 0xCC;}
static void resolution(const struct canopus_context_v1 *c,uint32_t *w,uint32_t *h) {(void)c;*w=width;*h=height;}
static uint32_t clock_ms(const struct canopus_context_v1 *c) {(void)c;return 1000;}
static int32_t timer_create(const struct canopus_context_v1 *c,uint32_t ms,int periodic,canopus_timer_callback_t cb,void *cookie) {
    int i;(void)c;(void)cookie;
    /* 40ms is the drag edge timer, 450ms the long-press hold; 5000ms is the
       reload watchdog and 2000/3500ms are the success and failure dwells. */
    assert(periodic?(ms==40):(ms==450||ms==2000||ms==3500||ms==5000));
    if(fail_start && ms>=2000) return -1;
    for(i=0;i<TIMERS;i++) if(!timer_cb[i]) {timer_cb[i]=cb;timer_period[i]=ms;return (int32_t)i+1;}
    assert(0);return -1;
}
static int32_t timer_start(const struct canopus_context_v1 *c,int32_t id) {(void)c;assert(id>0 && id<=TIMERS && timer_cb[id-1]);return 0;}
static int32_t timer_destroy(const struct canopus_context_v1 *c,int32_t id) {(void)c;assert(id>0 && id<=TIMERS && timer_cb[id-1]);timer_cb[id-1]=0;timer_period[id-1]=0;return 0;}
static const struct canopus_ui_obj ui_api={
    .capabilities=canopus_ui_context_capabilities,.tree_begin=canopus_ui_tree_begin,.tree_commit=canopus_ui_tree_commit,
    .tree_abort=canopus_ui_tree_abort,.end=canopus_ui_end,.navigation_page=canopus_ui_navigation_page,.section=canopus_ui_section,
    .text=canopus_ui_text,.status_row=canopus_ui_status_row,.button=canopus_ui_button,.action_row=canopus_ui_action_row,
    .switch_row=canopus_ui_switch_row,.navigation_header=canopus_ui_navigation_header,.component=canopus_ui_component,
    .node_set_style=canopus_ui_node_set_style,.node_set_layout=canopus_ui_node_set_layout,.set_entry=set_entry,.close_page=close_page,
    .node_set_visual=canopus_ui_node_set_visual,.control=canopus_ui_control,.post=post
};
/* The multi-page backend differs only by offering ctx->ui->route. */
static const struct canopus_ui_obj ui_api_multi={
    .capabilities=canopus_ui_context_capabilities,.tree_begin=canopus_ui_tree_begin,.tree_commit=canopus_ui_tree_commit,
    .tree_abort=canopus_ui_tree_abort,.end=canopus_ui_end,.navigation_page=canopus_ui_navigation_page,.section=canopus_ui_section,
    .text=canopus_ui_text,.status_row=canopus_ui_status_row,.button=canopus_ui_button,.action_row=canopus_ui_action_row,
    .switch_row=canopus_ui_switch_row,.navigation_header=canopus_ui_navigation_header,.component=canopus_ui_component,
    .node_set_style=canopus_ui_node_set_style,.node_set_layout=canopus_ui_node_set_layout,.set_entry=set_entry,.close_page=close_page,
    .route=route_page,
    .node_set_visual=canopus_ui_node_set_visual,.control=canopus_ui_control,.post=post
};
static int32_t config_stat(const struct canopus_context_v1 *c,const char *path,struct canopus_fs_stat *out) {
    (void)c;memset(out,0,sizeof(*out));
    /* A previous process published generation 1. The new UI must skip it. */
    if(!strcmp(path,"active/1")) {out->is_dir=1;return 0;}return CANOPUS_SVC_ERR_NOTFOUND;
}
static const struct canopus_fs_obj fs_api={.stat=config_stat,
#ifdef RH_MANAGER_UI_TEST
    .open=probe_open,.write=probe_write,.close=probe_close,
#endif
};
static const struct canopus_launcher_obj launcher_api={.register_app=publish_launcher};
static const struct canopus_display_obj display_api={.get_resolution=resolution};
static const struct canopus_time_obj time_api={.monotonic_ms=clock_ms,.timer_create=timer_create,.timer_start=timer_start,.timer_destroy=timer_destroy};

void rh_module_get_status_snapshot(struct rh_module_status_snapshot *out) {*out=status;}
void rh_manager_get_transfer_state(struct rh_transfer_ui_state *out) {*out=transfer;}
int rh_manager_subscribe(rh_manager_state_listener cb,void *cookie) {subscription_count++;listener=cb;listener_cookie=cookie;return 0;}
void rh_manager_unsubscribe(rh_manager_state_listener cb,void *cookie) {(void)cb;(void)cookie;}
int rh_module_request_reload_from(uint32_t generation,const char *dir) {
    reloads++;if(reject_reload) return -9;
    status.reload_generation=generation;status.reload_result=1;
    strcpy(applied_dir,dir);listener(listener_cookie,RH_MANAGER_CHANGED_STATUS);return 0;
}
int rh_module_request_reload(uint32_t generation) {return rh_module_request_reload_from(generation,"unused");}
int rh_manager_set_transfer_state(const struct rh_transfer_ui_state *s) {transfer=*s;listener(listener_cookie,RH_MANAGER_CHANGED_TRANSFER);return 0;}
int rh_manager_assets_install(const struct canopus_context_v1 *c) {(void)c;return 0;}
int rh_manager_asset_ensure(enum rh_manager_asset a,const char **path) {(void)a;*path="/data/canopus/m.corona/assets/image.bin";return 0;}
const char *rh_manager_asset_path(enum rh_manager_asset a) {(void)a;return "/data/canopus/m.corona/assets/image.bin";}
int rh_manager_data_create(const struct canopus_context_v1 *c,const struct rh_manager_data_hooks *h,struct rh_manager_data **out) {(void)c;(void)h;if(fail_create){*out=0;return -5;}*out=(struct rh_manager_data *)&context;return 0;}
void rh_manager_data_destroy(struct rh_manager_data *d) {(void)d;}
int rh_manager_data_refresh(struct rh_manager_data *d) {(void)d;refresh_count++;return fail_refresh?-5:data_pending?CANOPUS_SVC_ERR_BUSY:0;}
uint32_t rh_manager_data_order_count(const struct rh_manager_data *d) {(void)d;return 400;}
int rh_manager_data_metadata(struct rh_manager_data *d,const char *id,struct rh_manager_metadata *out) {
    (void)d;memset(out,0,sizeof(*out));strcpy(out->id,id);strcpy(out->name,"测试主题");strcpy(out->author,"作者");strcpy(out->version,"1.2.0");strcpy(out->description,"这是一份原始主题简介");strcpy(out->targets,"测试设备");out->available=1;
    if(!strcmp(id,"theme1")) {memset(out->description,'d',1024);out->description[1024]=0;memset(out->targets,'t',2064);out->targets[2064]=0;}
    if(!strcmp(id,"theme2")) {uint32_t n;for(n=0;n<20;n++)memcpy(out->name+n*3,"汉",3);out->name[60]=0;}
    if(dense_metadata) {memset(out->name,'N',128);out->name[128]=0;memset(out->description,'D',1024);out->description[1024]=0;}
    return 0;
}
int rh_manager_data_order_get(struct rh_manager_data *d,uint32_t i,char id[65],struct rh_manager_metadata *out) {assert(i<400);if(!i)strcpy(id,"@system");else snprintf(id,65,"theme%u",i);return rh_manager_data_metadata(d,id,out);}
int rh_manager_data_move(struct rh_manager_data *d,uint32_t from,uint32_t to) {(void)d;move_from=from;move_to=to;moves++;return failed_move?-5:0;}
int rh_manager_data_mix_count(struct rh_manager_data *d,uint32_t *n) {(void)d;*n=400;return 0;}
int rh_manager_data_mix_get(struct rh_manager_data *d,uint32_t i,struct rh_manager_mix_item *item) {(void)d;memset(item,0,sizeof(*item));if(!i){strcpy(item->key,"feature:launcher");strcpy(item->label,"启动器背景");item->feature=1;}else {snprintf(item->key,256,"/system/icons/icon%u.bin",i);strcpy(item->label,item->key);}strcpy(item->choice,selected?"theme1":"@default");strcpy(item->choice_name,selected?"测试主题":"默认");return 0;}
int rh_manager_data_option_count(struct rh_manager_data *d,const char *key,uint32_t *n) {(void)d;(void)key;*n=20;return 0;}
int rh_manager_data_option_get(struct rh_manager_data *d,const char *key,uint32_t i,struct rh_manager_option *o) {
    (void)d;(void)key;memset(o,0,sizeof(*o));assert(i<20);
    if(!i) {strcpy(o->id,"@default");strcpy(o->name,"默认");}
    else if(i==1) {strcpy(o->id,"@system");strcpy(o->name,"系统");}
    else {snprintf(o->id,65,"theme%u",i);strcpy(o->name,"测试主题");}
    /* Index 2 keeps the compact shape: no preview file means no image row. */
    if(i!=2) strcpy(o->preview,i==1?"/.rh_sys/system/icon.bin":"/data/canopus/m.corona/themes/theme1/icon.bin");
    o->selected=selected==i;return 0;
}
int rh_manager_data_choose(struct rh_manager_data *d,const char *key,const char *choice) {(void)d;(void)key;chooses++;selected=!strcmp(choice,"@system")?1:0;return 0;}
int rh_manager_data_prepare_apply(struct rh_manager_data *d,const char *dir) {(void)d;prepared++;if(fail_prepare) return -7;data_pending=1;assert(!strncmp(dir,"active/",7));return 0;}
int rh_manager_data_complete_apply(struct rh_manager_data *d,int success) {
    (void)d;data_pending=0;
    /* A watchdog rollback passes success=0 and must be accepted: it is the
       staged-transaction abort, not a second completion. */
    if(!success) {failed_complete++;return 0;}
    completed++;return fail_complete?-5:0;
}
int rh_manager_data_delete(struct rh_manager_data *d,const char *id) {(void)d;(void)id;deletes++;return fail_delete?-5:0;}
int rh_manager_data_delete_check(struct rh_manager_data *d,const char *id) {(void)d;(void)id;return delete_protected?RH_MANAGER_ERR_ACTIVE:0;}
const char *rh_manager_data_error(const struct rh_manager_data *d) {(void)d;return "";}
int rh_manager_data_last_installed(const struct rh_manager_data *d,char id[65]) {(void)d;strcpy(id,"theme3");return 0;}

static int32_t dispatch(void *cookie,const struct canopus_ui_event_v1 *e) {(void)cookie;return entry->on_event(&context,&ui,e,entry->user_data);}
static void pump(void) {canopus_ui_task_fn cb=pending;pending=0;if(cb) cb(&context,0);}
static uint32_t find_type(uint16_t type,uint32_t start) {uint32_t i;for(i=start;i<ui.committed.node_count;i++)if(ui.committed.nodes[i].type==type)return i;assert(0);return 0;}
static uint32_t find_text(const char *s) {
    uint32_t i;
    for(i=0;i<ui.committed.node_count;i++) if(!strcmp(ui.committed.strings+ui.committed.nodes[i].primary_off,s)) return i;
    fprintf(stderr,"Missing UI text '%s' (page key %u, phase %u, nodes %u)\n",s,ui.committed.nodes[0].key,transfer.phase,ui.committed.node_count);
    for(i=0;i<ui.committed.node_count;i++) fprintf(stderr,"  key %u type %u: %s\n",ui.committed.nodes[i].key,ui.committed.nodes[i].type,ui.committed.strings+ui.committed.nodes[i].primary_off);
    assert(0);return 0;
}
static uint32_t find_key(uint32_t key) {uint32_t i;for(i=0;i<ui.committed.node_count;i++)if(ui.committed.nodes[i].key==key)return i;assert(0);return 0;}
static void assert_no_text(const char *s) {
    uint32_t i;
    for(i=0;i<ui.committed.node_count;i++)
        if(!strcmp(ui.committed.strings+ui.committed.nodes[i].primary_off,s)) {
            fprintf(stderr,"Unexpected UI text '%s' (page key %u, phase %u)\n",s,ui.committed.nodes[0].key,transfer.phase);
            assert(0);
        }
}
static void visible_copy(const char *s) {
    char joined[1536];uint32_t i,n=0;joined[0]=0;
    for(i=0;i<ui.committed.node_count;i++) {
        const struct canopus_ui_node_v1 *node=&ui.committed.nodes[i];
        if(node->type==CANOPUS_UI_NODE_TEXT) {assert(n+node->primary_len<sizeof(joined));memcpy(joined+n,ui.committed.strings+node->primary_off,node->primary_len);n+=node->primary_len;joined[n]=0;}
    }
    if(!strstr(joined,s)) fprintf(stderr,"Missing visible text '%s' in '%s'\n",s,joined);
    assert(strstr(joined,s));
}
static int quickapp_hook(void *cookie,const char *package,char out[256]) {(void)cookie;(void)package;strcpy(out,"/data/icon.bin");return 0;}
static int event_result(uint32_t key,uint32_t type,uint32_t ms,int32_t x,int32_t y,int32_t scroll) {
    const struct canopus_ui_node_v1 *n=&ui.committed.nodes[find_key(key)];
    struct canopus_ui_event_v1 e={sizeof(e),ui.committed.generation,key,n->event_id,type,ms,x,y,0,0,scroll,444,41632};
    return canopus_ui_dispatch_event(&ui,&e);
}
static void event(uint32_t key,uint32_t type,uint32_t ms,int32_t x,int32_t y,int32_t scroll) {
    assert(!event_result(key,type,ms,x,y,scroll));
}
static void activate_text(const char *s) {uint32_t i=find_text(s);event(ui.committed.nodes[i].key,CANOPUS_UI_EVENT_ACTIVATE,0,0,0,0);}
static void back(void) {event(2,CANOPUS_UI_EVENT_ACTIVATE,0,0,0,0);}
/* Match the real host: any unconsumed callback finishes the module page. */
static void host_back(void) {if(entry->on_back(&context,&ui,0)) assert(!close_page(&context));}
static void tap(uint32_t key,uint32_t time) {event(key,CANOPUS_UI_EVENT_PRESS,time,10,10,0);event(key,CANOPUS_UI_EVENT_RELEASE,time+5,10,10,0);}
static void assert_route(uint32_t route) {assert(ui.committed.nodes[0].key==1+route*10000);}
int main(int argc,char **argv) {
    uint32_t i,bytes=0;struct rh_transfer_ui_state s={0};
    struct rh_manager_data_hooks hooks={.quickapp_icon=quickapp_hook,.cookie=&context};(void)argv;
    if(argc>1){width=336;height=480;}
    context.struct_size=sizeof(context);context.ui=&ui_api;context.fs=&fs_api;context.launcher=&launcher_api;context.display=&display_api;context.time=&time_api;
    assert(!canopus_ui_context_init(&ui,&backend,0,dispatch,0));
    assert(!rh_manager_ui_set_data_hooks(&hooks));
    assert(!rh_manager_ui_register(&context));assert(!rh_manager_launcher_register(&context));
    assert(!entry->on_open(&context,&ui,0));assert_route(0);
    /* The root page carries no back affordance: the stock apps exit through
       the system back gesture so the firmware draws the page-out transition,
       and only sub-routes draw a header back. */
    assert(!(ui.committed.nodes[find_key(2)].flags&CANOPUS_UI_NODE_FLAG_HEADER_BACK));
    i=find_text("正在工作");assert(ui.committed.visuals[0].flags==0);
    (void)i;
    assert(ui.committed.visuals[find_key(20)].background_rgb==0x1da362);
    assert(ui.committed.styles[find_key(20)].corner_radius==32);
    assert(ui.committed.visuals[find_key(24)].x==(height==480?186:133));
    assert(ui.committed.visuals[find_key(24)].y==(height==480?-7:1));
    /* Initial import errors must leave both return paths usable without a
       data model. A failed home render must still consume the host's back. */
    fail_create=1;
    activate_text("资源管理");assert_route(1);find_text("读取资源包失败，请返回后重试。");
    back();assert_route(0);assert(!close_count);
    activate_text("资源管理");assert_route(1);host_back();assert_route(0);assert(!close_count);
    activate_text("资源管理");assert_route(1);reject_apply=1;host_back();assert_route(1);assert(!close_count);
    host_back();assert_route(0);assert(!close_count);
    fail_create=0;
    activate_text("资源管理");assert_route(1);
    fail_refresh=1;back();activate_text("资源管理");assert_route(1);find_text("读取资源包失败，请返回后重试。");
    host_back();assert_route(0);assert(!close_count);fail_refresh=0;
    /* CONTAIN images are native clickable viewports: use the child key,
       whose event_id remains zero, exactly as the firmware hit test does. */
    tap(31,100);assert(reloads==1 && prepared==1);assert(!strcmp(applied_dir,"active/2"));
    /* The busy card: the status card is the reload action's only surface, so it
       reports the transaction while the bottom button is disabled. */
    find_text("正在重载");find_text("重新读取映射");
    assert(ui.committed.visuals[find_key(20)].background_rgb==0x1da362);
    assert(timer_live_ms(5000));
    tap(31,120);assert(reloads==1);pump();
    {uint32_t reads=refresh_count;
        activate_text("资源管理");assert_route(1);find_text("系统样式");assert(refresh_count==reads);
        back();activate_text("混搭微调");assert_route(3);
find_text("启动器背景");assert(refresh_count==reads);
        status.last_changed=3;status.reload_result=0;listener(listener_cookie,1);pump();assert(completed==1 && refresh_count==reads+1);
        find_text("启动器背景");back();
    }
    /* The outcome takes the card for one dwell and then yields to the module
       state again; the reload watchdog is gone the moment it reports. */
    find_text("重载完成");find_text("更新3项资源");
    assert(ui.committed.visuals[find_key(20)].background_rgb==0x1da362);
    assert(!timer_live_ms(5000) && timer_live_ms(2000));
    timer_fire(2000);find_text("正在工作");find_text("已加载3条规则");
    assert(!timer_live());
    fail_complete=1;tap(30,200);pump();status.reload_result=0;listener(listener_cookie,1);pump();assert(completed==2);
    /* A refresh whose protection record failed did work: it must not read as a
       reload failure, and it keeps its own dwell. */
    find_text("重载完成");find_text("保护记录未存");
    assert(ui.committed.visuals[find_key(20)].background_rgb==0x1da362);
    assert(timer_live_ms(2000));timer_fire(2000);
    find_text("资源管理");fail_complete=0;
    /* Every failure the action can hit is a card, never a page error: a blocking
       page would also drop the button the user needs to retry. The watchdog is
       the only thing that keeps a lost generation from greying the action out
       for the rest of the session. */
    fail_refresh=1;tap(31,300);find_text("重载失败");find_text("读取资源失败");
    assert(ui.committed.visuals[find_key(20)].background_rgb==0xdf9725);
    assert(!timer_live_ms(5000) && timer_live_ms(3500));timer_fire(3500);fail_refresh=0;
    fail_prepare=1;tap(31,320);find_text("重载失败");find_text("映射生成失败");
    timer_fire(3500);fail_prepare=0;
    reject_reload=1;{int failures=failed_complete;tap(31,340);assert(failed_complete==failures+1);}
    find_text("重载失败");find_text("错误码 -9");timer_fire(3500);reject_reload=0;
    {int failures=failed_complete;tap(31,360);assert(timer_live_ms(5000));
     timer_fire(5000);assert(failed_complete==failures+1);assert(!timer_live_ms(5000));}
    find_text("重载失败");find_text("模块无响应");
    /* The action is usable again, and a late negative result keeps its code. */
    {uint32_t before=reloads;tap(31,380);assert(reloads==before+1);assert(!timer_live_ms(3500));
     status.reload_result=-7;listener(listener_cookie,1);pump();}
    find_text("重载失败");find_text("错误码 -7");timer_fire(3500);
    {uint32_t before=reloads;tap(31,400);assert(reloads==before+1);
     status.last_changed=1;status.reload_result=0;listener(listener_cookie,1);pump();}
    find_text("重载完成");find_text("更新1项资源");timer_fire(2000);find_text("正在工作");
    assert(!timer_live());
    /* No heartbeat timer means no dwell and no watchdog: the result stays until
       the next tap replaces it, which is the documented degradation. */
    fail_start=1;{uint32_t before=reloads;tap(31,420);assert(reloads==before+1);assert(!timer_live());}
    find_text("正在重载");
    status.last_changed=2;status.reload_result=0;listener(listener_cookie,1);pump();
    find_text("更新2项资源");assert(!timer_live());fail_start=0;
    tap(31,440);find_text("正在重载");status.reload_result=0;listener(listener_cookie,1);pump();
    find_text("重载完成");timer_fire(2000);assert(!timer_live());
    activate_text("资源管理");assert_route(1);
    assert(ui.committed.nodes[find_text("系统样式")].event_id==0);
    event(1000,CANOPUS_UI_EVENT_PRESS,100,50,50,0);event(1000,CANOPUS_UI_EVENT_LONG_PRESS,600,50,50,0);
    assert(captured==1 && timer_live_ms(40));
    event(1000,CANOPUS_UI_EVENT_MOVE,650,50,260,0);event(1000,CANOPUS_UI_EVENT_RELEASE,700,50,260,0);
    assert(moves==1 && move_from==0 && move_to==2 && released==1 && !timer_live_ms(40));
    event(10,CANOPUS_UI_EVENT_SCROLL,800,0,0,38000);assert(largest_y>32767);
    event(10,CANOPUS_UI_EVENT_SCROLL,801,0,0,0);
    /* Overscroll must not rebuild. The firmware drives the bounce itself; a
       replacement per sample re-issues a scroll position and cuts the spring,
       and only the bottom edge is visible to the module because a negative
       sample is rejected before dispatch. Both edges stay inside the window. */
    {uint32_t before;
        /* Reach the bottom, then feed samples from inside the bounce. The
           firmware drives that spring; a replacement per sample re-issues a
           scroll position and cuts it. Only the bottom edge is observable here
           because a negative sample is rejected before dispatch. */
        event(10,CANOPUS_UI_EVENT_SCROLL,802,0,0,41188);
        before=applies;
        event(10,CANOPUS_UI_EVENT_SCROLL,803,0,0,41300);
        event(10,CANOPUS_UI_EVENT_SCROLL,804,0,0,41500);
        event(10,CANOPUS_UI_EVENT_SCROLL,805,0,0,41632);
        event(10,CANOPUS_UI_EVENT_SCROLL,806,0,0,41188);
        assert(applies==before);
        /* A sample that leaves the built window still rebuilds, so the list
           keeps filling as the user scrolls away from the boundary. */
        event(10,CANOPUS_UI_EVENT_SCROLL,807,0,0,0);
        event(10,CANOPUS_UI_EVENT_SCROLL,808,0,0,40000);
        assert(applies>before);
        event(10,CANOPUS_UI_EVENT_SCROLL,809,0,0,0);
    }
    /* Maximum-sized names/previews fill the arena. A captured row outside the
       window must remain present, and failed commits must never strand it. */
    dense_metadata=1;
    for(i=0;i<3;i++) {
        uint32_t captures=captured,releases=released;
        event(10,CANOPUS_UI_EVENT_SCROLL,900,0,0,0);
        event(1001,CANOPUS_UI_EVENT_PRESS,1000,20,120,0);event(1001,CANOPUS_UI_EVENT_LONG_PRESS,1500,20,120,0);
        assert(captured==captures+1 && timer_live_ms(40));
        event(10,CANOPUS_UI_EVENT_SCROLL,1510,0,0,38000);find_key(1001);assert(ui.committed.string_used<=1536);
        reject_apply=1;assert(event_result(10,CANOPUS_UI_EVENT_SCROLL,1520,0,0,20000)==CANOPUS_UI_ERR_BACKEND);
        if(i==0) event(1001,CANOPUS_UI_EVENT_RELEASE,1600,20,120,0);
        else if(i==1) event(1001,CANOPUS_UI_EVENT_CANCEL,1600,20,120,0);
        else {back();assert_route(0);activate_text("资源管理");}
        assert(released==releases+1 && !timer_live_ms(40) && moves==1);
    }
    dense_metadata=0;event(10,CANOPUS_UI_EVENT_SCROLL,1700,0,0,0);
    event(1001,CANOPUS_UI_EVENT_PRESS,1900,20,120,0);event(1001,CANOPUS_UI_EVENT_RELEASE,1910,20,120,0);
    event(1001,CANOPUS_UI_EVENT_ACTIVATE,1911,20,120,0);assert_route(2);
    host_back();assert_route(1);assert(!close_count);
    event(1001,CANOPUS_UI_EVENT_ACTIVATE,1912,20,120,0);assert_route(2);
    /* Every byte in both values remains reachable; chunks are not shortened
       to fit the arena. The scroll walk also exercises variable-height fields. */
    for(i=0;i<12000;i+=50) {
        uint32_t j;event(10,CANOPUS_UI_EVENT_SCROLL,1000+i,0,0,(int32_t)i);
        for(j=0;j<ui.committed.node_count;j++) {
            const struct canopus_ui_node_v1 *n=&ui.committed.nodes[j];
            if(n->type==CANOPUS_UI_NODE_TEXT && n->primary_len && ui.committed.strings[n->primary_off]=='t') bytes+=n->primary_len;
        }
    }
    assert(bytes>=2064);
    /* The delete action opens a confirmation page instead of arming a second
       tap: a referenced package states why it cannot go and dims the action,
       and only the confirmation page ever runs the delete. */
    delete_protected=1;tap(31,20000);assert(deletes==0);assert_route(5);
    visible_copy("正在使用中，无法删除");visible_copy("「混搭微调」中改回「默认」");
    /* A referenced package keeps its action but disables it: the host draws no
       disabled appearance on its own, so the page asks for the grey semantic
       colour (opacity is unavailable on .043) and the runtime refuses the
       dispatch through the cleared ENABLED flag. */
    {uint32_t action=find_text("删除");
     assert(!(ui.committed.nodes[action].flags&CANOPUS_UI_NODE_FLAG_ENABLED));
     assert(ui.committed.styles[action].foreground==CANOPUS_UI_COLOR_DISABLED);
     assert(ui.committed.styles[action].opacity==500);
     assert(ui.committed.styles[action].variant!=CANOPUS_UI_VARIANT_DESTRUCTIVE);}
    back();assert_route(2);
    delete_protected=0;tap(31,21000);assert(deletes==0);assert_route(5);
    visible_copy("删除后，资源包文件与索引将一并移除");
    {uint32_t action=find_text("删除");
     assert(ui.committed.nodes[action].flags&CANOPUS_UI_NODE_FLAG_ENABLED);
     assert(ui.committed.styles[action].opacity==0);
     assert(ui.committed.styles[action].variant==CANOPUS_UI_VARIANT_DESTRUCTIVE);}
    /* A refused transaction keeps the page and replaces the promise with the
       reason, so the action can be retried without leaving and coming back. */
    fail_delete=1;activate_text("删除");assert(deletes==1);assert_route(5);
    visible_copy("删除失败");
    fail_delete=0;activate_text("删除");assert(deletes==2);assert_route(1);
    /* 60 UTF-8 bytes are twenty CJK glyphs, which need multiple visual lines
       even though the previous byte threshold classified them as short. */
    event(1002,CANOPUS_UI_EVENT_ACTIVATE,22500,0,0,0);assert_route(2);
    {char cjk[61];uint32_t j,node;for(j=0;j<20;j++)memcpy(cjk+j*3,"汉",3);cjk[60]=0;
        node=find_text(cjk);assert(ui.committed.nodes[node].type==CANOPUS_UI_NODE_TEXT);assert(ui.committed.layouts[node].height>32);
    }
    back();
    back();activate_text("混搭微调");assert_route(3);
event(10,CANOPUS_UI_EVENT_SCROLL,0,0,0,208);
    event(1002,CANOPUS_UI_EVENT_ACTIVATE,0,0,0,0);assert_route(4);
    i=find_text("系统");event(ui.committed.nodes[i].key,CANOPUS_UI_EVENT_ACTIVATE,0,0,0,0);assert(selected==1);
    i=find_text("系统");assert(ui.committed.nodes[i].flags&CANOPUS_UI_NODE_FLAG_SELECTED);
    /* Every option is a real card again. The compact rows carry the same
       #262626/24px surface as the preview rows, and the page suppresses the
       host's own (●) because it reserves that column for its own icon. */
    {uint32_t compact=0;
        /* The stub names every theme the same; take the third option's row. */
        for(i=0;i<ui.committed.node_count;i++)
            if(ui.committed.nodes[i].type==CANOPUS_UI_NODE_RADIO_ROW &&
               ui.committed.layouts[i].height==76) {compact=i;break;}
        assert(ui.committed.visuals[compact].background_rgb==0x262626);
        assert(ui.committed.styles[compact].corner_radius==24);
        assert(ui.committed.styles[compact].variant==CANOPUS_UI_VARIANT_PLAIN);
        assert(ui.committed.layouts[compact].padding_left==14);
        assert(ui.committed.layouts[compact].padding_right==13);
        assert(ui.committed.layouts[compact].height==76);
        /* The selected state is a 32px icon at the card's right edge, not the
           host's synthesized glyph. */
        {uint32_t mark=0,j;for(j=0;j<ui.committed.node_count;j++)
            if(ui.committed.nodes[j].type==CANOPUS_UI_NODE_IMAGE &&
               ui.committed.layouts[j].width==32) {mark=j;break;}
         assert(mark!=0);
         /* Vertical placement is the post-commit TRANSLATE path, which the
            scroll assertions above already cover. */
         {int row=(int)(width>=300?336:204);
          assert(ui.committed.visuals[mark].x+32==(int)(width-row)/2+row-13);}}
    }
    /* The tall variant keeps one card: SECTION at 160 owns the preview image
       and the label row, and the label row only carries the shared inset. */
    {uint32_t preview=0;
        for(i=0;i<ui.committed.node_count;i++)
            if(ui.committed.nodes[i].type==CANOPUS_UI_NODE_RADIO_ROW &&
               ui.committed.layouts[i].height==48) {preview=i;break;}
        assert(preview!=0);
        uint32_t card=ui.committed.nodes[preview].parent;
        assert(card!=CANOPUS_UI_NO_NODE);
        assert(ui.committed.nodes[card].type==CANOPUS_UI_NODE_SECTION);
        assert(ui.committed.layouts[card].height==160);
        assert(ui.committed.visuals[card].background_rgb==0x262626);
        assert(ui.committed.styles[card].corner_radius==24);
        assert(ui.committed.layouts[preview].padding_left==14);
        assert(ui.committed.layouts[preview].padding_right==13);
        assert(ui.committed.visuals[preview].background_rgb==0);
    }
    /* The preview is wrapped in a clickable LIST (a bare image is not one),
       and the whole card picks: the wrapper carries the event while its bare
       image child and the check mark report only their keys, exactly as the
       firmware hit test does. Each tap is one pick, never two. */
    {uint32_t wrap=find_type(CANOPUS_UI_NODE_LIST,0),base=chooses;
     uint32_t wkey=ui.committed.nodes[wrap].key;
     assert(ui.committed.nodes[wrap].event_id!=0);
     assert(ui.committed.nodes[wrap].parent==find_key(wkey-1));
     tap(wkey,23000);assert(chooses==base+1 && selected==0);
     tap(wkey+6,23100);assert(chooses==base+2 && selected==1);
     tap(wkey+3,23200);assert(chooses==base+3 && selected==0);
     assert(ui.committed.nodes[find_key(wkey+2)].flags&CANOPUS_UI_NODE_FLAG_SELECTED);
    }
    host_back();assert_route(3);assert(!close_count);assert(ui.committed.visuals[find_key(10)].scroll_y==208);back();
    activate_text("资源管理");assert_route(1);
    s.remote_connected=1;s.phase=RH_XFER_PHASE_RECEIVING;s.percent=68;assert(!rh_manager_set_transfer_state(&s));pump();
    find_text("正在接收 · 68%");
    i=find_type(CANOPUS_UI_NODE_PROGRESS,0);assert(ui.committed.values[i].value==68);
    s.phase=RH_XFER_PHASE_VERIFYING;assert(!rh_manager_set_transfer_state(&s));pump();find_text("正在校验完整性");
    s.phase=RH_XFER_PHASE_INSTALLING;assert(!rh_manager_set_transfer_state(&s));pump();find_text("正在解压并登记");
    {uint32_t reads=refresh_count;
        s.phase=RH_XFER_PHASE_INSTALLED;assert(!rh_manager_set_transfer_state(&s));pump();
        assert(refresh_count==reads+1);
    }
    s.phase=RH_XFER_PHASE_ERROR;s.detail=6;assert(!rh_manager_set_transfer_state(&s));pump();
    find_text("传输中断");visible_copy("主题包完整性校验失败，请重新发送");
    s.phase=RH_XFER_PHASE_CANCELED;assert(!rh_manager_set_transfer_state(&s));pump();find_text("任务已取消");
    /* Errors persist in the banner until the user dismisses the card; any
       later phase transition brings the banner back. */
    tap(60,30000);assert_no_text("传输中断");
    s.phase=RH_XFER_PHASE_RECEIVING;s.percent=20;assert(!rh_manager_set_transfer_state(&s));pump();find_text("正在接收 · 20%");
    {struct rh_transfer_ui_state idle={0};assert(!rh_manager_set_transfer_state(&idle));pump();assert_no_text("正在接收 · 20%");}
    host_back();assert_route(0);assert(!close_count);
    entry->on_close(&context,0);
    /* A closed app has no UI to announce the finished install to. */
    {struct rh_transfer_ui_state done={0};done.phase=RH_XFER_PHASE_INSTALLED;
     assert(!rh_manager_set_transfer_state(&done));pump();assert_no_text("已安装");}
    assert(!rh_manager_ui_set_data_hooks(&hooks));assert(!rh_manager_ui_register(&context));assert(subscription_count==1);
    {struct rh_manager_data_hooks changed_hooks=hooks;changed_hooks.cookie=0;assert(rh_manager_ui_set_data_hooks(&changed_hooks)==CANOPUS_SVC_ERR_STATE);}
    assert(!entry->on_open(&context,&ui,0));activate_text("资源管理");assert_route(1);
    host_back();assert_route(0);assert(!close_count);
    /* Sub-routes still draw it, so an internal pop keeps its title affordance. */
    activate_text("资源管理");assert_route(1);
    assert(ui.committed.nodes[find_key(2)].flags&CANOPUS_UI_NODE_FLAG_HEADER_BACK);
    back();assert_route(0);assert(!close_count);
    host_back();assert(close_count==1);
#ifdef RH_MANAGER_UI_TEST
    assert(!entry->on_open(&context,&ui,0));
    activate_text("资源管理");assert_route(1);
    /* Long holds cycle the banner states; the header is not a list row, so the
       drag-to-reorder long press keeps its meaning while the states turn. */
    for(i=0;i<6;i++) {
        event(2,CANOPUS_UI_EVENT_LONG_PRESS,51000+i*1000,50,60,0);pump();
        if(i==0) {assert(transfer.phase==RH_XFER_PHASE_RECEIVING && transfer.percent==68);}
        if(i==4) {assert(transfer.phase==RH_XFER_PHASE_ERROR && transfer.detail==6);}
        if(i==5) {assert(transfer.phase==RH_XFER_PHASE_CANCELED);}
    }
    assert(probe_value("routes_mask")==63 && probe_value("phases_mask")==127);
    /* The four intentional backend rejections above are latched even though
       later renders recover. A clean emulator scenario expects zero. */
    assert(probe_value("render_failures")==4);
    assert(probe_value("received_68_seen")==1);
    assert(probe_value("render_error")==0 && probe_used<=256);
    entry->on_close(&context,0);
#endif
    /* Multi-page pass: the entry declares one firmware page per route and every
       navigate() goes through ctx->ui->route, which is what gives a real
       firmware page push/pop its stock transition. */
    multi_page=1;routed_routes=0;context.ui=&ui_api_multi;
    assert(entry->page_count==6);
    assert(!entry->on_open(&context,&ui,0));assert_route(0);
    assert(!(ui.committed.nodes[find_key(2)].flags&CANOPUS_UI_NODE_FLAG_HEADER_BACK));
    activate_text("资源管理");assert_route(1);
    assert(routed_routes==1);
    assert(ui.committed.nodes[find_key(2)].flags&CANOPUS_UI_NODE_FLAG_HEADER_BACK);
    back();assert_route(0);assert(routed_routes==2);
    /* A header back on the root declines too: close_page from inside the
       callback would consume the gesture and skip the page-out transition.
       There is no header node on the root, so call the entry directly. */
    {int before=close_count;assert(entry->on_back(&context,&ui,0)==1);
     assert(close_count==before);assert_route(0);}
    activate_text("混搭微调");assert_route(3);assert(routed_routes==3);
    /* The root page declines back so the firmware plays its page-out. */
    host_back();assert(close_count==1);
    entry->on_close(&context,0);
    puts(height==480?"native UI .043: passed":"native UI .155: passed");return 0;
}

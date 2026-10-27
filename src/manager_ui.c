#include "manager_ui.h"
#include "manager_state.h"
#include "manager_assets.h"
#include "canopus_ui.h"
#include "canopus_memory.h"

#define ON (CANOPUS_UI_NODE_FLAG_VISIBLE | CANOPUS_UI_NODE_FLAG_ENABLED)
#define TRY(call) do { int _r = (call); if (_r) return _r; } while (0)
enum { INDEX, RESOURCES, DETAIL, MIX, SELECT, DELETE, ROUTES };
enum { EV_BACK=1, EV_RESOURCES, EV_MIX, EV_RELOAD, EV_DELETE, EV_SCROLL, EV_DISMISS,
       EV_ORDER=1000, EV_MIX_ITEM=1000000, EV_OPTION=2000000, EV_CHOOSE=3000000 };
enum { KEY_SCROLL=10, KEY_ACTION=30, KEY_BANNER=60, KEY_ROW=1000, OPTION_SLOTS=5,
       ROW_STEP=104, BANNER_H=96, WINDOW=7, EDGE=42, SLOP=8 };
/* Reload feedback lives in the home status card: the removal of the TOAST API
 * left the bottom action without any other surface. A result is transient, so
 * it yields to the real module state after a short dwell; a failure keeps its
 * error code on screen longer because that code is the only place it appears. */
enum { RH_NOTICE_NONE=0, RH_NOTICE_DONE, RH_NOTICE_PROTECT_FAILED,
       RH_NOTICE_LOAD_FAILED, RH_NOTICE_PREPARE_FAILED, RH_NOTICE_REJECTED,
       RH_NOTICE_MODULE_FAILED, RH_NOTICE_TIMEOUT };
enum { RELOAD_WATCHDOG_MS=5000, NOTICE_OK_MS=2000, NOTICE_FAIL_MS=3500 };
/* UI state is shared across routes. Catalog and metadata are never duplicated
 * into page models, and only a visible subset enters the bounded UI tree. */
static struct {
    const struct canopus_context_v1 *ctx;
    struct canopus_ui_context_v1 *ui;
    struct rh_manager_data *data;
    struct rh_manager_data_hooks hooks;
    struct rh_module_status_snapshot status;
    struct rh_transfer_ui_state transfer;
    struct rh_manager_metadata metadata;
    uint32_t route, width, height, content_height, row_width;
    int32_t scroll[ROUTES];
    uint32_t generation, reload_generation, subscribed, opened, wide, busy, render_failed;
    uint32_t mix_index, count, shown;
    /* Reload result shown in the home status card, plus its dwell timer and the
       watchdog that gives the action back when a generation never reports. */
    uint32_t notice, notice_changed;
    int32_t notice_code, notice_timer, reload_timer;
    /* Content span the committed tree actually builds. While the whole viewport
       stays inside it the tree is reusable, so a scroll sample costs no rebuild. */
    int32_t window_from, window_to, window_total, viewport;
    char detail_id[65], mix_key[256], mix_title[256], error[256];
    int32_t edge_timer, hold_timer, list_top;
    uint32_t pressed, press_id, press_key, press_time, moved, drag, drag_from, drag_to;
    int32_t start_x, start_y, last_y, start_scroll, drag_delta;
    uint32_t suppressed, banner_hidden, position_count, position_keys[2*WINDOW+4];
    int32_t position_y[2*WINDOW+4];
    /* delete_blocked mirrors rh_manager_data_delete_check for the confirmation
       page; confirm_error carries the reason a confirm attempt still failed. */
    int32_t delete_blocked;
    char confirm_error[96];
} U = { .notice_timer=-1, .reload_timer=-1, .edge_timer=-1, .hold_timer=-1 };
#ifdef RH_MANAGER_UI_TEST
/* Compiled only into the emulator acceptance artifact. A long hold enters
 * receive and steps its copied state; no Connect listener is registered. */
static struct {
    uint32_t reload,drag,delete_taps,pick,scroll,cycle;
    uint32_t routes_mask,phases_mask,render_failures,received_68_seen;
    int32_t render_rc;
} probe_state;
#endif

static uint32_t len(const char *s) { uint32_t n=0; if (s) while(s[n]) n++; return n; }
static void copy(char *out, uint32_t cap, const char *s) {
    uint32_t n=0; if (!cap) return; if (s) while (s[n] && n+1u<cap) { out[n]=s[n]; n++; }
    /* Never leave a partial UTF-8 sequence in a diagnostic. Product fields are
       passed at their complete validated length, rather than using this helper. */
    if (s && s[n]) while(n && ((unsigned char)s[n]&0xc0u)==0x80u) n--;
    out[n]=0;
}
static int equal(const char *a,const char *b) {
    uint32_t i=0; while(a[i] && a[i]==b[i]) i++; return a[i]==b[i];
}
static uint32_t slice_bytes(const char *s,uint32_t maximum) {
    uint32_t n=len(s);if(n<=maximum) return n;n=maximum;
    while(n && ((unsigned char)s[n]&0xc0u)==0x80u) n--;
    return n;
}
/* Incumbent list names and descriptions are previews; use an explicit ellipsis
 * within their visible width. The detail view keeps the complete source text. */
static void preview(char *out,uint32_t cap,const char *s) {
    uint32_t n;if(len(s)<cap) {copy(out,cap,s);return;}
    n=slice_bytes(s,cap-4u);canopus_memcpy(out,s,n);canopus_memcpy(out+n,"…",4);
}
static uint32_t number(char *out,uint32_t v) {
    char rev[10]; uint32_t n=0,i;
    do {rev[n++]=(char)('0'+v%10u);v/=10u;} while(v);
    for(i=0;i<n;i++) out[i]=rev[n-i-1]; out[n]=0; return n;
}
static int32_t clamp(int32_t x,int32_t lo,int32_t hi) { return x<lo?lo:x>hi?hi:x; }
#ifdef RH_MANAGER_UI_TEST
static char *probe_field(char *out,const char *name,uint32_t value) {
    uint32_t n=len(name);canopus_memcpy(out,name,n);out+=n;*out++='=';
    out+=number(out,value);*out++='\n';return out;
}
static void write_probe(int rc) {
    char data[256],*p=data;int fd;uint32_t done=0,total;
    probe_state.render_rc=rc;
    if(rc) {if(probe_state.render_failures!=0xffffffffu) probe_state.render_failures++;}
    else {
        probe_state.routes_mask|=1u<<U.route;
        probe_state.phases_mask|=1u<<U.transfer.phase;
        if(U.route==RESOURCES && U.transfer.phase==RH_XFER_PHASE_RECEIVING && U.transfer.percent==68) probe_state.received_68_seen=1;
    }
    p=probe_field(p,"route",U.route);p=probe_field(p,"phase",U.transfer.phase);
    p=probe_field(p,"connected",U.transfer.remote_connected);p=probe_field(p,"percent",U.transfer.percent);
    p=probe_field(p,"render_error",rc<0?0u-(uint32_t)rc:(uint32_t)rc);
    p=probe_field(p,"reload",probe_state.reload);p=probe_field(p,"drag",probe_state.drag);
    p=probe_field(p,"delete",probe_state.delete_taps);p=probe_field(p,"pick",probe_state.pick);
    p=probe_field(p,"scroll",probe_state.scroll);
    p=probe_field(p,"routes_mask",probe_state.routes_mask);p=probe_field(p,"phases_mask",probe_state.phases_mask);
    p=probe_field(p,"render_failures",probe_state.render_failures);
    p=probe_field(p,"notice",U.notice);
    p=probe_field(p,"received_68_seen",probe_state.received_68_seen);total=(uint32_t)(p-data);
    if(!U.ctx->fs || !U.ctx->fs->open || !U.ctx->fs->write || !U.ctx->fs->close) return;
    fd=U.ctx->fs->open(U.ctx,"ui-probe.txt",CANOPUS_FS_WRITE);if(fd<0) return;
    while(done<total) {int got=U.ctx->fs->write(U.ctx,fd,data+done,total-done);if(got<=0 || (uint32_t)got>total-done) break;done+=(uint32_t)got;}
    (void)U.ctx->fs->close(U.ctx,fd);
}
#endif
static int render(void);
static void set_error(const char *fallback) {
    const char *s=U.data?rh_manager_data_error(U.data):0;
    copy(U.error,sizeof(U.error),s && *s?s:fallback);
}
/* A failure is anything at or past the first failure kind; the two success
 * kinds come first so one comparison separates the dwell times. */
static int notice_failed(uint32_t kind) { return kind>=RH_NOTICE_LOAD_FAILED; }
static void notice_timer_stop(void) {
    if(U.notice_timer>=0) {(void)U.ctx->time->timer_destroy(U.ctx,U.notice_timer);U.notice_timer=-1;}
}
static void notice_clear(void) {
    notice_timer_stop();U.notice=RH_NOTICE_NONE;U.notice_changed=0;U.notice_code=0;
}
static void notice_expire(const struct canopus_context_v1 *ctx,int32_t timer,void *cookie) {
    (void)ctx;(void)cookie;if(timer!=U.notice_timer) return;
    notice_clear();(void)render();
}
static void notice_set(uint32_t kind,uint32_t changed,int32_t code) {
    notice_clear();U.notice=kind;U.notice_changed=changed;U.notice_code=code;
    if(!U.ctx || !U.ctx->time || !U.ctx->time->timer_create) return;
    /* No heartbeat timer means no expiry: the result then stays until the next
       reload replaces it. A stuck notice is a smaller lie than a missing one. */
    U.notice_timer=U.ctx->time->timer_create(U.ctx,notice_failed(kind)?NOTICE_FAIL_MS:NOTICE_OK_MS,0,notice_expire,0);
    if(U.notice_timer>=0 && U.ctx->time->timer_start(U.ctx,U.notice_timer)<0) notice_timer_stop();
}
static void reload_timer_stop(void) {
    if(U.reload_timer>=0) {(void)U.ctx->time->timer_destroy(U.ctx,U.reload_timer);U.reload_timer=-1;}
}
static int build_confirm(struct canopus_ui_tree_v1 *t);
static void cancel_gesture(void);
static int build_banner(struct canopus_ui_tree_v1 *t);
static int banner_active(void);
static void changed(void *,uint32_t);
static void edge_tick(const struct canopus_context_v1 *,int32_t,void *);
static void clear_hold(void) {
    if(U.hold_timer>=0) {(void)U.ctx->time->timer_destroy(U.ctx,U.hold_timer);U.hold_timer=-1;}
}
static int component(struct canopus_ui_tree_v1 *t,uint32_t key,uint16_t type,
                     const char *a,const char *b,uint32_t event,uint32_t flags) {
    struct canopus_ui_component_props_v1 p={sizeof(p),a,len(a),b,len(b),event,flags,0,0,0,0,0};
    return U.ctx->ui->component(t,key,type,&p);
}
static int text(struct canopus_ui_tree_v1 *t,uint32_t key,const char *s,uint32_t style) {
    struct canopus_ui_text_props_v1 p={sizeof(p),s,len(s),style};
    return U.ctx->ui->text(t,key,&p);
}
static int row(struct canopus_ui_tree_v1 *t,uint32_t key,const char *title,const char *detail,uint32_t event) {
    if(event) {
        struct canopus_ui_action_row_props_v1 p={sizeof(p),title,len(title),detail,len(detail),event,1};
        return U.ctx->ui->action_row(t,key,&p);
    } else {
        struct canopus_ui_status_row_props_v1 p={sizeof(p),title,len(title),detail,len(detail)};
        return U.ctx->ui->status_row(t,key,&p);
    }
}
static int section(struct canopus_ui_tree_v1 *t,uint32_t key) {
    struct canopus_ui_section_props_v1 p={sizeof(p),"",0};return U.ctx->ui->section(t,key,&p);
}
static int box(struct canopus_ui_tree_v1 *t,uint32_t key,int width,int height,int x,int y,
               uint32_t rgb,int color,int radius,int clip) {
    struct canopus_ui_layout_v1 l=CANOPUS_UI_LAYOUT_DEFAULT;
    struct canopus_ui_style_v1 s=CANOPUS_UI_STYLE_DEFAULT;
    struct canopus_ui_visual_v1 v=CANOPUS_UI_VISUAL_DEFAULT;
    l.width=(int16_t)width;l.height=(int16_t)height;
    v.flags=CANOPUS_UI_VISUAL_FLAG_ABSOLUTE;v.x=(int16_t)x;v.y=(int16_t)y;
    if(color) {v.flags|=CANOPUS_UI_VISUAL_FLAG_BACKGROUND_RGB;v.background_rgb=rgb;}
    if(clip) v.flags|=CANOPUS_UI_VISUAL_FLAG_CLIP;
    if(radius>=0) {s.corner_radius=(int16_t)radius;TRY(U.ctx->ui->node_set_style(t,key,&s));}
    TRY(U.ctx->ui->node_set_layout(t,key,&l));return U.ctx->ui->node_set_visual(t,key,&v);
}
static int label_box(struct canopus_ui_tree_v1 *t,uint32_t key,const char *s,int width,int height,
                     int x,int y,uint16_t font,uint32_t rgb) {
    struct canopus_ui_visual_v1 v=CANOPUS_UI_VISUAL_DEFAULT;
    TRY(text(t,key,s,CANOPUS_UI_TEXT_BODY));TRY(box(t,key,width,height,x,y,0,0,-1,0));
    v.flags=CANOPUS_UI_VISUAL_FLAG_ABSOLUTE|CANOPUS_UI_VISUAL_FLAG_FOREGROUND_RGB;
    v.x=(int16_t)x;v.y=(int16_t)y;v.font_px=font;v.foreground_rgb=rgb;
    return U.ctx->ui->node_set_visual(t,key,&v);
}
static int image_path(struct canopus_ui_tree_v1 *t,uint32_t key,const char *path,int width,int height,int x,int y) {
    struct canopus_ui_component_props_v1 p={sizeof(p),path,len(path),"",0,0,ON,0,0,0,0,1};
    struct canopus_ui_visual_v1 v=CANOPUS_UI_VISUAL_DEFAULT;
    TRY(U.ctx->ui->component(t,key,CANOPUS_UI_NODE_IMAGE,&p));
    TRY(box(t,key,width,height,x,y,0,0,-1,0));
    v.flags=CANOPUS_UI_VISUAL_FLAG_ABSOLUTE;v.x=(int16_t)x;v.y=(int16_t)y;v.image_fit=CANOPUS_UI_IMAGE_FIT_CONTAIN;
    return U.ctx->ui->node_set_visual(t,key,&v);
}
static int image_asset(struct canopus_ui_tree_v1 *t,uint32_t key,enum rh_manager_asset a,
                      int w,int h,int x,int y) {
    const char *path=0;TRY(rh_manager_asset_ensure(a,&path));return image_path(t,key,path,w,h,x,y);
}
static int image_button(struct canopus_ui_tree_v1 *t,enum rh_manager_asset asset,uint32_t event,
                        int width,int height,int y,int enabled) {
    TRY(component(t,KEY_ACTION,CANOPUS_UI_NODE_LIST,"","",event,
                  CANOPUS_UI_NODE_FLAG_VISIBLE|(enabled?CANOPUS_UI_NODE_FLAG_ENABLED:0)));
    TRY(box(t,KEY_ACTION,width,height,((int)U.width-width)/2,y,0,0,-1,1));
    TRY(image_asset(t,KEY_ACTION+1,asset,width,height,0,0));
    return U.ctx->ui->end(t);
}
static int ensure_data(void) {
    if(!U.data) return rh_manager_data_create(U.ctx,&U.hooks,&U.data);
    return 0;
}
int rh_manager_ui_get_data(const struct canopus_context_v1 *ctx,struct rh_manager_data **out) {
    int rc;
    if(!ctx || !out || (U.ctx && U.ctx!=ctx)) return CANOPUS_SVC_ERR_STATE;
    U.ctx=ctx;rc=ensure_data();
    if(!rc) *out=U.data;
    return rc;
}
void rh_manager_ui_reset_data(void) {
    if(U.data)rh_manager_data_destroy(U.data);
    U.data=0;U.count=0;
}
static int load_route(uint32_t route,int refresh) {
    int rc=0; U.error[0]=0;
    if(route==INDEX) return 0;
    rc=ensure_data();
    if(!rc && refresh && !U.busy) rc=rh_manager_data_refresh(U.data);
    if(!rc && route==DETAIL) rc=rh_manager_data_metadata(U.data,U.detail_id,&U.metadata);
    if(!rc && route==DELETE) {
        /* The confirmation page states the outcome up front instead of letting
         * the user discover it on the one action it offers. */
        rc=rh_manager_data_metadata(U.data,U.detail_id,&U.metadata);
        if(!rc) {U.delete_blocked=rh_manager_data_delete_check(U.data,U.detail_id);U.confirm_error[0]=0;}
    }
    if(!rc && route==RESOURCES) U.count=rh_manager_data_order_count(U.data);
    if(!rc && route==MIX) rc=rh_manager_data_mix_count(U.data,&U.count);
    if(!rc && route==SELECT) rc=rh_manager_data_option_count(U.data,U.mix_key,&U.count);
    if(rc) set_error((route==DETAIL||route==DELETE)?"无法读取资源包信息":"读取资源包失败，请返回后重试。");
    return rc;
}
/* Each route is its own firmware page so the move is a stock push/pop and the
 * firmware draws the transition; the host re-creates the view on the
 * destination page, so render() rebuilds the whole tree from U afterwards.
 * A backend without multi-page entries keeps the single-page replacement. */
static int navigate(uint32_t route,int refresh) {
    uint32_t previous_route=U.route,previous_count=U.count;char previous_error[256];int rc;
    copy(previous_error,sizeof(previous_error),U.error);
    cancel_gesture();U.route=route;
    (void)load_route(route,refresh);
    if(U.ctx->ui->route && !U.ctx->ui->route(U.ctx,route)) {
        /* The host replaced its view; the destination page still owes a tree. */
        U.generation=U.ui->committed.generation;
        return render();
    }
    rc=render();
    /* A rejected replacement leaves the old native page visible. Its route
     * must still own the next back event instead of accidentally exiting. */
    if(rc && U.ui->committed.nodes[0].key!=1u+route*10000u) {
        U.route=previous_route;U.count=previous_count;
        copy(U.error,sizeof(U.error),previous_error);
    }
    return rc;
}
static int build_home(struct canopus_ui_tree_v1 *t) {
    const char *title,*subtitle; char count[48]; uint32_t color,fg;
    int state=U.status.state, cardw=U.wide?324:204;
    /* The decoration answers "which module state". A transient reload notice
       keeps its own state's illustration instead of claiming a new one, so a
       failed reload never looks like a crashed module. */
    enum rh_manager_asset asset=state==RH_MGR_STATE_CONFIG_ERROR?RH_MANAGER_ASSET_STATUS_CONFIG_ERROR:
        state==RH_MGR_STATE_UNRESPONSIVE?RH_MANAGER_ASSET_STATUS_UNRESPONSIVE:
        RH_MANAGER_ASSET_STATUS_RUNNING;
    uint16_t subfont;
    if(U.busy || U.notice!=RH_NOTICE_NONE) {
        /* 7 glyphs at font 17 is all the 134px subtitle column holds, so every
           result string below is budgeted to that width on the narrow target. */
        subfont=U.wide?24:17;
        if(U.busy) {title="正在重载";subtitle="重新读取映射";color=0x1da362;fg=0xc6e8d8;}
        else if(notice_failed(U.notice)) {
            color=0xdf9725;fg=0xf7e5c8;title="重载失败";
            if(U.notice==RH_NOTICE_TIMEOUT) subtitle="模块无响应";
            else if(U.notice==RH_NOTICE_PREPARE_FAILED) subtitle="映射生成失败";
            else if(U.notice==RH_NOTICE_LOAD_FAILED) subtitle="读取资源失败";
            else {
                uint32_t n;int32_t code=U.notice_code<0?(int32_t)(0u-(uint32_t)U.notice_code):U.notice_code;
                copy(count,sizeof(count),"错误码 -");n=len(count);n+=number(count+n,(uint32_t)code);subtitle=count;
            }
        } else {
            color=0x1da362;fg=0xc6e8d8;title="重载完成";
            if(U.notice==RH_NOTICE_PROTECT_FAILED) subtitle="保护记录未存";
            else if(!U.notice_changed) subtitle="模块已响应";
            else {
                uint32_t n;copy(count,sizeof(count),"更新");n=len(count);
                n+=number(count+n,U.notice_changed);copy(count+n,sizeof(count)-n,"项资源");subtitle=count;
            }
        }
    } else if(state==RH_MGR_STATE_RUNNING) {
        uint32_t n;copy(count,sizeof(count),"已加载");n=len(count);n+=number(count+n,U.status.active_rules);copy(count+n,sizeof(count)-n,"条规则");
        title="正在工作";subtitle=count;color=0x1da362;fg=0xc6e8d8;subfont=U.wide?24:17;
    } else if(state==RH_MGR_STATE_CONFIG_ERROR) {
        title="配置错误";subtitle="模块已安装";color=0xdf9725;fg=0xf7e5c8;subfont=U.wide?26:24;
    } else if(state==RH_MGR_STATE_UNRESPONSIVE) {
        title="模块异常";subtitle="未检测到模块";color=0xda593c;fg=0xf6d5ce;subfont=U.wide?26:21;
    } else {title="检测失败";subtitle="请稍后重载";color=0x262626;fg=0xbfbfbf;subfont=U.wide?26:24;}
    TRY(section(t,20));TRY(box(t,20,cardw,100,((int)U.width-cardw)/2,U.wide?12:18,color,1,32,1));
    if(state!=RH_MGR_STATE_UNAVAILABLE) {
        TRY(image_asset(t,24,asset,U.wide?138:92,U.wide?145:97,U.wide?186:133,U.wide?-7:1));
    }
    TRY(label_box(t,21,title,U.wide?183:134,U.wide?42:40,14,U.wide?12:14,U.wide?32:30,0xffffff));
    TRY(label_box(t,22,subtitle,U.wide?183:134,U.wide?34:32,14,54,subfont,fg));
    TRY(U.ctx->ui->end(t));
    TRY(row(t,40,"资源管理","排序与管理资源包",EV_RESOURCES));TRY(box(t,40,(int)U.row_width,96,((int)U.width-(int)U.row_width)/2,U.wide?122:128,0,0,-1,0));
    TRY(row(t,41,"混搭微调","自由搭配资源样式",EV_MIX));TRY(box(t,41,(int)U.row_width,96,((int)U.width-(int)U.row_width)/2,U.wide?226:232,0,0,-1,0));
    /* Bottom actions float 10px off the screen edge (the Band 11 content
       viewport stops 8px short of it), mirroring the Vela page placement. */
    return image_button(t,U.wide?RH_MANAGER_ASSET_RELOAD_WIDE:RH_MANAGER_ASSET_RELOAD,EV_RELOAD,U.wide?324:150,U.wide?80:78,
                        (int)U.content_height-(U.wide?90:80),!U.busy);
}
static int scroll_begin(struct canopus_ui_tree_v1 *t,uint32_t total,int height,int top) {
    struct canopus_ui_visual_v1 v=CANOPUS_UI_VISUAL_DEFAULT;
    int32_t maximum;
    if(total>0x7fffffffu) return CANOPUS_UI_ERR_CAPACITY;
    /* The firmware owns the overscroll bounce. Echoing a sample from outside the
       valid range back into the visual would make the host issue a position
       write per sample and cut the spring short, so the stored offset stays
       inside the range and only the built window decides whether to rebuild. */
    maximum=(int32_t)total-height;if(maximum<0) maximum=0;
    if(U.scroll[U.route]<0) U.scroll[U.route]=0;
    else if(U.scroll[U.route]>maximum) U.scroll[U.route]=maximum;
    U.viewport=height;U.list_top=top;U.window_total=(int32_t)total;U.window_from=U.window_to=0;
    TRY(component(t,KEY_SCROLL,CANOPUS_UI_NODE_SCROLL,"","",EV_SCROLL,ON));
    TRY(box(t,KEY_SCROLL,(int)U.width,height,0,top,0,0,-1,1));
    v.flags=CANOPUS_UI_VISUAL_FLAG_ABSOLUTE|CANOPUS_UI_VISUAL_FLAG_CLIP|CANOPUS_UI_VISUAL_FLAG_VIRTUAL_CONTENT;
    v.content_height=total;v.scroll_y=U.scroll[U.route];
    return U.ctx->ui->node_set_visual(t,KEY_SCROLL,&v);
}
/* True while the committed window already spans the visible part of the
 * viewport, so a scroll sample needs no tree replacement. */
static int window_covers(int32_t y) {
    int32_t end=y+U.viewport;
    if(end>U.window_total) end=U.window_total;
    return y>=U.window_from && end<=U.window_to;
}
static int positioned(struct canopus_ui_tree_v1 *t,uint32_t key,int height,int32_t y) {
    /* The ABI stores absolute y in int16. Immediate TRANSLATE accepts int32;
       use it after commit, before returning to the display, for long catalogs. */
    if(U.position_count>=2*WINDOW+4) return CANOPUS_UI_ERR_CAPACITY;
    U.position_keys[U.position_count]=key;U.position_y[U.position_count++]=y;
    return box(t,key,(int)U.row_width,height,((int)U.width-(int)U.row_width)/2,0,0,0,-1,0);
}
static uint32_t string_room(struct canopus_ui_tree_v1 *t) {
    /* Public bounded storage; reading its accounting avoids speculatively
       appending a row that would leave a half-built staging tree. */
    uint32_t used=t->_snapshot.string_used,reserve=8u;
    return used+reserve>=CANOPUS_UI_STRING_CAPACITY?0:CANOPUS_UI_STRING_CAPACITY-used-reserve;
}
static int build_resource_row(struct canopus_ui_tree_v1 *t,uint32_t i) {
    char id[65],detail[1300],detail_preview[129];uint32_t n=0;int rc;
    rc=rh_manager_data_order_get(U.data,i,id,&U.metadata);if(rc) return rc;
    if(equal(id,RH_MANAGER_SYSTEM)) {
        TRY(row(t,KEY_ROW+i,"系统样式","设备内置样式 · 长按拖动排序",0));
        /* STATUS_ROW's zero event id still receives native pointer events.
           Match its stable key for drag; short activation remains disabled. */
    } else {
        if(!U.metadata.available) copy(detail,sizeof(detail),"corona.json 不可用 · 包信息不可用");
        else {
            if(U.metadata.author[0]) {copy(detail,sizeof(detail),U.metadata.author);n=len(detail);}
            if(U.metadata.version[0]) {
                if(n) {copy(detail+n,sizeof(detail)-n," · ");n=len(detail);}
                detail[n++]='v';copy(detail+n,sizeof(detail)-n,U.metadata.version);n=len(detail);
            }
            if(!n) {copy(detail,sizeof(detail),"资源包详情");n=len(detail);}
            copy(detail+n,sizeof(detail)-n," · ");n=len(detail);
            copy(detail+n,sizeof(detail)-n,U.metadata.description[0]?U.metadata.description:"点按查看资源包详情");
        }
        const char *title=U.metadata.available?U.metadata.name:id;
        preview(detail_preview,sizeof(detail_preview),detail);
        if(len(title)+len(detail_preview)+2u>string_room(t)) return CANOPUS_UI_ERR_CAPACITY;
        TRY(row(t,KEY_ROW+i,title,detail_preview,EV_ORDER+i));
    }
    TRY(positioned(t,KEY_ROW+i,96,(int32_t)(i*ROW_STEP)));
    if(U.drag && i==U.drag_from) {
        struct canopus_ui_visual_v1 v=CANOPUS_UI_VISUAL_DEFAULT;
        v.flags=CANOPUS_UI_VISUAL_FLAG_ABSOLUTE|CANOPUS_UI_VISUAL_FLAG_BACKGROUND_RGB;
        v.x=(int16_t)((U.width-U.row_width)/2);v.background_rgb=0x444444;
        TRY(U.ctx->ui->node_set_visual(t,KEY_ROW+i,&v));
    }
    return 0;
}
static int build_mix_row(struct canopus_ui_tree_v1 *t,uint32_t i) {
    struct rh_manager_mix_item item;char choice[34];TRY(rh_manager_data_mix_get(U.data,i,&item));
    preview(choice,sizeof(choice),item.choice_name);
    if(len(item.label)+len(choice)+2>string_room(t)) return CANOPUS_UI_ERR_CAPACITY;
    TRY(row(t,KEY_ROW+i,item.label,choice,EV_MIX_ITEM+i));return positioned(t,KEY_ROW+i,96,(int32_t)(i*ROW_STEP));
}
static int build_list(struct canopus_ui_tree_v1 *t) {
    uint32_t i,first;int rc,banner=banner_active();
    if(banner) TRY(build_banner(t));
    if(!U.count) return image_asset(t,50,RH_MANAGER_ASSET_NONE,205,152,((int)U.width-205)/2,130);
    if(U.count>(0x7fffffffu-32u)/ROW_STEP) return CANOPUS_UI_ERR_CAPACITY;
    TRY(scroll_begin(t,U.count*ROW_STEP+32u,(int)U.content_height-(banner?BANNER_H+8:0),banner?BANNER_H+8:0));
    first=(uint32_t)U.scroll[U.route]/ROW_STEP;
    if(first>=U.count) first=U.count-1;
    U.shown=0;
    /* Commit the captured row before filling the ordinary window. Its exact
       strings and node are therefore reserved even after it leaves the view. */
    if(U.drag) TRY(build_resource_row(t,U.drag_from));
    for(i=first;i<U.count && U.shown<WINDOW;i++) {
        if(U.drag && i==U.drag_from) {U.shown++;continue;}
        rc=U.route==RESOURCES?build_resource_row(t,i):build_mix_row(t,i);
        if(rc==CANOPUS_UI_ERR_CAPACITY && U.shown) break;
        if(rc) return rc;
        U.shown++;
    }
    /* Rows sit on a fixed ROW_STEP grid, so the window covers exactly the
       slots that were emitted; one extra step of slack absorbs a sample that
       lands between two grid points without forcing a rebuild. */
    U.window_from=(int32_t)first*ROW_STEP;
    U.window_to=(int32_t)(first+U.shown)*ROW_STEP+ROW_STEP;
    if(U.window_to>U.window_total) U.window_to=U.window_total;
    return U.ctx->ui->end(t);
}
static const char *detail_value(uint32_t field,char version[68]) {
    switch(field) {
    case 0:return U.metadata.name;
    case 1:return U.metadata.author[0]?U.metadata.author:"未声明作者";
    case 2:if(!U.metadata.version[0]) return "未声明版本";
        version[0]='v';copy(version+1,67,U.metadata.version);return version;
    case 3:return U.metadata.description[0]?U.metadata.description:"暂无简介";
    case 4:return U.metadata.targets[0]?U.metadata.targets:"未声明目标设备";
    default:return U.detail_id;
    }
}
static uint32_t text_height(const char *value,uint32_t width) {
    uint32_t lines=1,column=0,columns=width/12;const unsigned char *p=(const unsigned char *)value;
    if(!columns) columns=1;
    while(*p) {
        if(*p=='\n') {lines++;column=0;}
        else if((*p&0xc0u)!=0x80u) {column+=*p<0x80u?1u:2u;if(column>columns) {lines++;column=*p<0x80u?1u:2u;}}
        p++;
    }
    return lines*32u;
}
static int detail_segments(struct canopus_ui_tree_v1 *t,uint32_t *height) {
    static const char *const labels[]={"主题包","作者","版本","简介","适用设备","资源包 ID"};
    uint32_t field,segment=0,y=0;char version[68],chunk[181];
    /* Segments are emitted in ascending y, so the first and last emitted ones
       bound the committed window; a scroll inside that span needs no rebuild. */
    int32_t window_from=0,window_to=0;int started=0;
    for(field=0;field<6;field++) {
        const char *value=detail_value(field,version);uint32_t offset=0,n=len(value),part=0;
        do {
            uint32_t bytes=slice_bytes(value+offset,180),h,key=KEY_ROW+segment*3;
            int short_field=text_height(value,U.row_width-32)==32u;
            canopus_memcpy(chunk,value+offset,bytes);chunk[bytes]=0;
            h=short_field?96u:text_height(chunk,U.row_width-24)+(part?24u:60u);
            if(t && y+h>(uint32_t)U.scroll[DETAIL] && U.shown<WINDOW &&
               y<(uint32_t)U.scroll[DETAIL]+U.content_height) {
                if(len(chunk)+len(labels[field])+4>string_room(t)) break;
                if(short_field) {
                    TRY(row(t,key,labels[field],chunk,0));TRY(positioned(t,key,(int)h,(int32_t)y));
                } else {
                    struct canopus_ui_section_props_v1 sp={sizeof(sp),part?"":labels[field],part?0:len(labels[field])};
                    struct canopus_ui_style_v1 style=CANOPUS_UI_STYLE_DEFAULT;
                    TRY(U.ctx->ui->section(t,key,&sp));TRY(positioned(t,key,(int)h,(int32_t)y));
                    style.corner_radius=24;style.background=CANOPUS_UI_COLOR_SURFACE_ALT;
                    TRY(U.ctx->ui->node_set_style(t,key,&style));
                    TRY(label_box(t,key+1,chunk,(int)U.row_width-24,(int)h-(part?24:60),12,part?12:48,24,0xbbbbbb));
                    TRY(U.ctx->ui->end(t));
                }
                U.shown++;
                if(!started) {started=1;window_from=(int32_t)y;}
                window_to=(int32_t)y+(int32_t)h;
            }
            y+=h+8;offset+=bytes;segment++;part++;
        } while(offset<n);
    }
    if(t) {U.window_from=window_from;U.window_to=window_to;}
    /* Trailing spacer mirrors the Vela detail page: at full scroll the last
       card stops 10px above the floating delete action. */
    *height=y+(U.wide?100:90);return 0;
}
static int build_detail(struct canopus_ui_tree_v1 *t) {
    uint32_t height;
    /* Long values exceed both a stock row and sometimes the whole arena.
       Chunk only at UTF-8 boundaries, preserving every byte across scrolling. */
    TRY(detail_segments(0,&height));
    /* The delete action floats over a full-height scroll like the Vela page:
       cards pass beneath it and the trailing spacer clears the last card. */
    TRY(scroll_begin(t,height,(int)U.content_height,0));U.shown=0;
    TRY(detail_segments(t,&height));TRY(U.ctx->ui->end(t));
    return image_button(t,U.wide?RH_MANAGER_ASSET_DELETE_WIDE:RH_MANAGER_ASSET_DELETE,EV_DELETE,U.wide?324:150,U.wide?80:78,
                        (int)U.content_height-(U.wide?90:80),!U.busy);
}
static int option_height(struct rh_manager_option *o) {return o->preview[0]?168:84;}
/* The choice list mirrors the previous Vela page: every option is a #262626
 * card with a 24px radius, a 14/13 side inset and an 8px gap. The tall variant
 * stacks a centred preview above the labelled row; the compact variant is the
 * row card itself. Rows are positioned through the post-commit TRANSLATE path
 * (positioned) so catalogs beyond the int16 visual limit still work. */
/* The label row inside a tall card: it sits under the 96px preview (top offset
   8) and keeps the card's 14/13 inset, so name and mark line up with the Vela
   footer. Its own placement is local to the card and needs no translation. */
static int choice_style(struct canopus_ui_tree_v1 *t,uint32_t key) {
    struct canopus_ui_style_v1 s=CANOPUS_UI_STYLE_DEFAULT;
    struct canopus_ui_layout_v1 l=CANOPUS_UI_LAYOUT_DEFAULT;
    struct canopus_ui_visual_v1 v=CANOPUS_UI_VISUAL_DEFAULT;
    s.variant=CANOPUS_UI_VARIANT_PLAIN;
    l.width=(int16_t)U.row_width;l.height=48;
    l.padding_left=14;l.padding_right=13;
    v.flags=CANOPUS_UI_VISUAL_FLAG_ABSOLUTE;v.x=0;v.y=104;
    TRY(U.ctx->ui->node_set_layout(t,key,&l));
    TRY(U.ctx->ui->node_set_style(t,key,&s));
    return U.ctx->ui->node_set_visual(t,key,&v);
}
/* The trailing 32px check mark of the Vela list. It is a sibling of the card at
 * scroll-content level so one coordinate rule covers both variants, and it is
 * placed through the same post-commit TRANSLATE path as the cards. A missing
 * asset drops only the icon, never the row, so a selection stays operable. */
static int choice_mark(struct canopus_ui_tree_v1 *t,uint32_t key,uint8_t selected,int32_t y) {
    const char *path;
    if(rh_manager_asset_ensure(selected?RH_MANAGER_ASSET_CHECK:RH_MANAGER_ASSET_UNCHECK,&path)) return 0;
    if(U.position_count>=2*WINDOW+4) return CANOPUS_UI_ERR_CAPACITY;
    U.position_keys[U.position_count]=key;U.position_y[U.position_count++]=y;
    return image_path(t,key,path,32,32,(int)(U.width-U.row_width)/2+(int)U.row_width-13-32,0);
}
/* One card: the row owns its own surface, so its box write must carry the size,
 * the inset and the placement together (a second layout write would reset it). */
static int choice_card(struct canopus_ui_tree_v1 *t,uint32_t key,int height,int32_t y,int radius) {
    struct canopus_ui_style_v1 s=CANOPUS_UI_STYLE_DEFAULT;
    struct canopus_ui_layout_v1 l=CANOPUS_UI_LAYOUT_DEFAULT;
    struct canopus_ui_visual_v1 v=CANOPUS_UI_VISUAL_DEFAULT;
    if(U.position_count>=2*WINDOW+4) return CANOPUS_UI_ERR_CAPACITY;
    U.position_keys[U.position_count]=key;U.position_y[U.position_count++]=y;
    l.width=(int16_t)U.row_width;l.height=(int16_t)height;
    l.padding_left=14;l.padding_right=13;
    v.flags=CANOPUS_UI_VISUAL_FLAG_ABSOLUTE|CANOPUS_UI_VISUAL_FLAG_BACKGROUND_RGB;
    v.x=(int16_t)((U.width-U.row_width)/2);v.background_rgb=0x262626;
    s.corner_radius=(int16_t)radius;
    /* VARIANT_PLAIN suppresses the host's synthesized (●) glyph. The row keeps
       the trailing column reserved, so the same key can paint the 32px check
       icon there once the asset pass lands. */
    s.variant=CANOPUS_UI_VARIANT_PLAIN;
    TRY(U.ctx->ui->node_set_layout(t,key,&l));
    TRY(U.ctx->ui->node_set_style(t,key,&s));
    return U.ctx->ui->node_set_visual(t,key,&v);
}
static int build_choices(struct canopus_ui_tree_v1 *t) {
    struct rh_manager_option option;uint32_t i,total=32;int32_t y=0;int h;
    /* Options mix compact and preview heights. Walk the shared catalog, without
       allocating a duplicate choices array or assuming all rows are equal. */
    for(i=0;i<U.count;i++) {
        TRY(rh_manager_data_option_get(U.data,U.mix_key,i,&option));h=option_height(&option);
        if(total>0x7fffffffu-(uint32_t)h) return CANOPUS_UI_ERR_CAPACITY;total+=(uint32_t)h;
    }
    TRY(scroll_begin(t,total,(int)U.content_height,0));U.shown=0;
    {
    /* The window is a contiguous height range, not a row index: options mix
       compact and preview heights, so an index would not describe the span. */
    int32_t window_from=0,window_to=(int32_t)total;int started=0;
    for(i=0;i<U.count;i++) {
        uint32_t key=KEY_ROW+i*OPTION_SLOTS;
        TRY(rh_manager_data_option_get(U.data,U.mix_key,i,&option));h=option_height(&option);
        if(y+h<=U.scroll[SELECT]) {y+=h;continue;}
        if(U.shown>=WINDOW) break;
        /* Worst case is an all-preview window: OPTION_SLOTS nodes per row on
           top of the page, header and scroll. Stop before the bounded
           snapshot is full instead of failing the whole commit. */
        if(t->_snapshot.node_count+OPTION_SLOTS>CANOPUS_UI_MAX_NODES) break;
        /* name + preview path + the checked/unchecked icon */
        if(len(option.name)+len(option.preview)+96u>string_room(t)) {if(U.shown) break;return CANOPUS_UI_ERR_CAPACITY;}
        if(!started) {started=1;window_from=y;}
        if(option.preview[0]) {
            TRY(section(t,key));TRY(choice_card(t,key,160,y,24));
            /* A bare image is never clickable, so the preview is wrapped in a
               LIST that owns the pick event: the same event carrier as the
               bottom image buttons (plan 3.2). */
            TRY(component(t,key+1,CANOPUS_UI_NODE_LIST,"","",EV_CHOOSE+i,ON));
            TRY(box(t,key+1,96,96,((int)U.row_width-96)/2,8,0,0,-1,1));
            TRY(image_path(t,key+2,option.preview,96,96,0,0));TRY(U.ctx->ui->end(t));
            TRY(component(t,key+3,CANOPUS_UI_NODE_RADIO_ROW,option.name,"",EV_OPTION+i,
                          ON|(option.selected?CANOPUS_UI_NODE_FLAG_SELECTED:0)));
            TRY(choice_style(t,key+3));TRY(U.ctx->ui->end(t));
            /* The mark follows the card so it stays a scroll-content sibling. */
            TRY(choice_mark(t,key+4,option.selected,y+112));
        } else {
            TRY(component(t,key,CANOPUS_UI_NODE_RADIO_ROW,option.name,"",EV_OPTION+i,
                          ON|(option.selected?CANOPUS_UI_NODE_FLAG_SELECTED:0)));
            TRY(choice_card(t,key,76,y,24));
            TRY(choice_mark(t,key+1,option.selected,y+22));
        }
        y+=h;U.shown++;window_to=y;
    }
    U.window_from=window_from;U.window_to=window_to;
    }
    return U.ctx->ui->end(t);
}
static const char *transfer_error(uint8_t detail) {
    switch(detail) {
    case 1:return "主题包数据无效，请重新发送";
    case 2:return "接收任务忙，请稍后重试";
    case 3:return "接收会话已失效，请重新连接";
    case 4:return "数据偏移不一致，请重新连接后续传";
    case 5:return "存储读写失败，请检查空间后重试";
    case 6:return "主题包完整性校验失败，请重新发送";
    case 7:return "主题包格式或资源校验失败";
    case 8:return "存储空间或资源容量不足";
    case 9:return "旧资源包正在使用，请切换后重试";
    case 10:return "资源包列表已变化，请重试";
    case 11:return "当前设备不支持此操作";
    default:return "传输中断，请重新连接后重试";
    }
}
/* Left-aligned UTF-8 wrap with a caller-owned line budget: the banner clamps
   its protocol errors to the two lines its card has room for, while the
   confirmation page spends up to four on a full sentence. */
static int wrap_lines(struct canopus_ui_tree_v1 *t,uint32_t key,const char *s,int y,uint16_t font,uint32_t color,uint32_t max_lines) {
    uint32_t offset=0,line=0,n=len(s),maximum=U.row_width-28;char buffer[256];
    while(offset<n && line<max_lines) {
        uint32_t bytes=0,pixels=0;
        while(offset+bytes<n) {
            unsigned char c=(unsigned char)s[offset+bytes];
            uint32_t size=c<0x80?1u:(c&0xe0u)==0xc0u?2u:(c&0xf0u)==0xe0u?3u:4u;
            uint32_t advance=c<0x80?font/2u:font;
            if(bytes && pixels+advance>maximum) break;
            if(bytes+size>=sizeof(buffer)) break;
            canopus_memcpy(buffer+bytes,s+offset+bytes,size);bytes+=size;pixels+=advance;
        }
        if(!bytes) return CANOPUS_UI_ERR_CAPACITY;
        buffer[bytes]=0;
        TRY(label_box(t,key+line,buffer,(int)pixels,(int)font+4,14,y+(int)line*((int)font+4),font,color));
        offset+=bytes;line++;
    }
    return 0;
}
/* Transfer progress is pinned above the resources list instead of owning a
   page: the receiver runs headless, so this banner is a passive surface that
   never gates a transfer, and the idle phase deliberately shows nothing. */
static int banner_active(void) {
    int phase=U.transfer.phase;
    if(U.route!=RESOURCES) return 0;
    if(phase>=RH_XFER_PHASE_RECEIVING && phase<=RH_XFER_PHASE_INSTALLING) return 1;
    return (phase==RH_XFER_PHASE_ERROR || phase==RH_XFER_PHASE_CANCELED) && !U.banner_hidden;
}
static int build_banner(struct canopus_ui_tree_v1 *t) {
    char composed[40];const char *title,*message="";uint32_t n;int phase=U.transfer.phase;
    if(phase==RH_XFER_PHASE_RECEIVING) {
        copy(composed,sizeof(composed),"正在接收 · ");n=len(composed);
        n+=number(composed+n,U.transfer.percent);composed[n++]='%';composed[n]=0;title=composed;
    } else if(phase==RH_XFER_PHASE_VERIFYING) title="正在校验完整性";
    else if(phase==RH_XFER_PHASE_INSTALLING) title="正在解压并登记";
    else {
        title="传输中断";
        message=phase==RH_XFER_PHASE_CANCELED?"任务已取消":transfer_error(U.transfer.detail);
    }
    /* The card is its own dismiss target and its click is the release inside
       slop, like the bottom image buttons. Progress states carry no event. */
    TRY(component(t,KEY_BANNER,CANOPUS_UI_NODE_LIST,"","",phase>=RH_XFER_PHASE_ERROR?EV_DISMISS:0,ON));
    TRY(box(t,KEY_BANNER,(int)U.row_width,BANNER_H,((int)U.width-(int)U.row_width)/2,0,0x222222,1,24,0));
    TRY(label_box(t,KEY_BANNER+1,title,(int)U.row_width-28,34,14,10,22,0xffffff));
    if(phase>=RH_XFER_PHASE_RECEIVING && phase<=RH_XFER_PHASE_INSTALLING) {
        struct canopus_ui_component_props_v1 p={sizeof(p),"",0,"",0,0,ON,phase==RH_XFER_PHASE_RECEIVING?U.transfer.percent:100,0,100,0,0};
        TRY(U.ctx->ui->component(t,KEY_BANNER+2,CANOPUS_UI_NODE_PROGRESS,&p));
        TRY(box(t,KEY_BANNER+2,(int)U.row_width-32,12,16,58,0,0,-1,0));
    } else TRY(wrap_lines(t,KEY_BANNER+2,message,44,20,0xc5cbd5,2));
    return U.ctx->ui->end(t);
}
static int translate_rows(void) {
    uint32_t i;
    for(i=0;i<U.position_count;i++) {
        int32_t y=U.position_y[i];uint32_t key=U.position_keys[i];
        if(U.drag) {
            uint32_t index=key-KEY_ROW;
            if(index==U.drag_from) y+=U.drag_delta;
            else if(U.drag_from<index && index<=U.drag_to) y-=ROW_STEP;
            else if(U.drag_to<=index && index<U.drag_from) y+=ROW_STEP;
        }
        TRY(U.ctx->ui->control(U.ui,U.generation,key,CANOPUS_UI_CONTROL_TRANSLATE,0,y,0));
    }
    return 0;
}
static int render(void) {
    static const char *const titles[]={"Corona","资源管理","资源详情","混搭微调","选择来源","删除资源"};
    struct canopus_ui_tree_v1 *t;const char *title=U.route==SELECT && U.mix_title[0]?U.mix_title:titles[U.route];int rc;
    struct canopus_ui_navigation_page_props_v1 p={sizeof(p),title,len(title)};
    /* The root page is the app, not a sub-view: the stock firmware apps keep
     * no back affordance there and leave the exit to the system back gesture,
     * which draws the page-out transition. A header back would close the page
     * from inside its own callback and cost that transition. */
    struct canopus_ui_navigation_header_props_v1 h={sizeof(h),title,len(title),"",0,EV_BACK,U.route!=INDEX,0,0};
    if(!U.opened || !U.ui) return 0;
    rc=U.ctx->ui->tree_begin(U.ui,&t);
    if(rc) {
#ifdef RH_MANAGER_UI_TEST
        write_probe(rc);
#endif
        return rc;
    }
    U.position_count=0;
    rc=U.ctx->ui->navigation_page(t,1u+U.route*10000u,&p);
    if(!rc) rc=U.ctx->ui->navigation_header(t,2,&h);
    if(!rc) rc=U.ctx->ui->end(t);
    if(!rc && U.error[0]) rc=label_box(t,50,U.error,(int)U.width-28,160,14,140,24,0xbbbbbb);
    else if(!rc) {
        switch(U.route) {
        case INDEX:rc=build_home(t);break;
        case RESOURCES:case MIX:rc=build_list(t);break;
        case DETAIL:rc=build_detail(t);break;
        case SELECT:rc=build_choices(t);break;
        case DELETE:rc=build_confirm(t);break;
        default:rc=CANOPUS_UI_ERR_ARGUMENT;break;
        }
    }
    if(!rc) rc=U.ctx->ui->end(t);
    if(rc) {U.ctx->ui->tree_abort(t);U.render_failed=1;
#ifdef RH_MANAGER_UI_TEST
        write_probe(rc);
#endif
        return rc;}
    rc=U.ctx->ui->tree_commit(t);
    if(!rc) {U.generation=U.ui->committed.generation;rc=translate_rows();}
    U.render_failed=rc!=0;
#ifdef RH_MANAGER_UI_TEST
    write_probe(rc);
#endif
    return rc;
}
static void cancel_gesture(void) {
    clear_hold();
    if(U.drag && U.ui && U.opened)
        (void)U.ctx->ui->control(U.ui,U.generation,KEY_ROW+U.drag_from,CANOPUS_UI_CONTROL_RELEASE_CAPTURE,0,0,0);
    if(U.edge_timer>=0) {(void)U.ctx->time->timer_destroy(U.ctx,U.edge_timer);U.edge_timer=-1;}
    U.drag=U.pressed=U.moved=0;U.drag_delta=0;
}
static int update_drag(void) {
    int32_t origin=(int32_t)(U.drag_from*ROW_STEP),top;
    top=clamp(origin+U.last_y-U.start_y+U.scroll[RESOURCES]-U.start_scroll,0,(int32_t)((U.count-1)*ROW_STEP));
    U.drag_delta=top-origin;U.drag_to=(uint32_t)(top+ROW_STEP/2)/ROW_STEP;
    return translate_rows();
}
static void edge_tick(const struct canopus_context_v1 *ctx,int32_t timer,void *cookie) {
    int32_t depth=0,step,target,maximum;struct canopus_ui_scroll_metrics_v1 metrics;
    (void)ctx;(void)cookie;if(timer!=U.edge_timer || !U.drag || !U.opened) return;
    if(U.ctx->ui->control(U.ui,U.generation,KEY_SCROLL,CANOPUS_UI_CONTROL_SCROLL_QUERY,0,0,&metrics)) return;
    /* The host normalizes native points to the content viewport origin. */
    if(U.last_y<U.list_top+EDGE) depth=U.last_y-(U.list_top+EDGE);
    else if(U.last_y>U.list_top+metrics.viewport_height-EDGE)
        depth=U.last_y-(U.list_top+metrics.viewport_height-EDGE);
    if(!depth) return;
    step=2+6*clamp(depth<0?-depth:depth,0,EDGE)/EDGE;if(depth<0) step=-step;
    maximum=metrics.content_height-metrics.viewport_height;if(maximum<0) maximum=0;
    target=clamp(metrics.y+step,0,maximum);
    if(target==metrics.y) return;
    if(!U.ctx->ui->control(U.ui,U.generation,KEY_SCROLL,CANOPUS_UI_CONTROL_SCROLL_TO,target,0,0)) {
        U.scroll[RESOURCES]=target;(void)update_drag();
        if(!window_covers(target)) (void)render();
    }
}
static int begin_drag(void) {
    int rc;if(U.busy || !U.pressed || U.moved || U.drag || U.route!=RESOURCES || U.press_id<EV_ORDER || U.press_id>=EV_MIX_ITEM) return 0;
    clear_hold();U.drag_from=U.press_id-EV_ORDER;
    if(U.drag_from>=U.count) return CANOPUS_UI_ERR_ARGUMENT;
    rc=U.ctx->ui->control(U.ui,U.generation,U.press_key,CANOPUS_UI_CONTROL_CAPTURE,0,0,0);if(rc) return rc;
    U.drag=1;U.drag_to=U.drag_from;U.suppressed=1;
    if(U.ctx->haptics && U.ctx->haptics->vibrate_preset) (void)U.ctx->haptics->vibrate_preset(U.ctx,CANOPUS_HAPTIC_TICK);
    U.edge_timer=U.ctx->time->timer_create(U.ctx,40,1,edge_tick,0);
    if(U.edge_timer>=0 && U.ctx->time->timer_start(U.ctx,U.edge_timer)<0) {
        (void)U.ctx->time->timer_destroy(U.ctx,U.edge_timer);U.edge_timer=-1;
    }
    return render();
}
static void hold_tick(const struct canopus_context_v1 *ctx,int32_t timer,void *cookie) {
    (void)ctx;(void)cookie;if(timer!=U.hold_timer) return;
    clear_hold();(void)begin_drag();
}
static int back(void) {
    /* The root page is the app: never close it from inside its own callback, or
     * the firmware sees descriptor +104 consumed and skips its page-out
     * transition. Declining lets the stock handler finish the app with the
     * animation. A single-page backend has no such gesture path, so it keeps
     * closing explicitly. */
    if(U.route==INDEX) return U.ctx->ui->route?0:U.ctx->ui->close_page(U.ctx);
    if(U.route==DETAIL) return navigate(RESOURCES,1);
    if(U.route==DELETE) return navigate(DETAIL,0);
    if(U.route==SELECT) return navigate(MIX,0);
    return navigate(INDEX,0);
}
static int next_config_dir(char dir[32]) {
    struct canopus_fs_stat stat;int rc;
    if(!U.ctx->fs || !U.ctx->fs->stat) return CANOPUS_SVC_ERR_UNSUPPORTED;
    rh_module_get_status_snapshot(&U.status);
    if(U.status.reload_generation>U.reload_generation) U.reload_generation=U.status.reload_generation;
    for(;;) {
        if(U.reload_generation==0xffffffffu) return CANOPUS_SVC_ERR_NOSPACE;
        U.reload_generation++;
        copy(dir,32,"active/");number(dir+7,U.reload_generation);
        rc=U.ctx->fs->stat(U.ctx,dir,&stat);
        if(rc==CANOPUS_SVC_ERR_NOTFOUND) return 0;
        if(rc<0) return rc;
    }
}
static void reload_timeout(const struct canopus_context_v1 *ctx,int32_t timer,void *cookie) {
    (void)ctx;(void)cookie;if(timer!=U.reload_timer) return;
    reload_timer_stop();
    if(!U.busy) return;
    /* The generation never reported back. Roll the staged catalog back or every
       later prepare is refused by a stuck apply, refresh from disk because the
       module may still commit unobserved, and give the action back. Watching
       this here is what keeps the button from greying out for good. */
    (void)rh_manager_data_complete_apply(U.data,0);U.busy=0;
    (void)rh_manager_data_refresh(U.data);
    notice_set(RH_NOTICE_TIMEOUT,0,0);
    rh_module_get_status_snapshot(&U.status);(void)render();
}
static void reload_watchdog(void) {
    reload_timer_stop();
    if(!U.ctx || !U.ctx->time || !U.ctx->time->timer_create) return;
    U.reload_timer=U.ctx->time->timer_create(U.ctx,RELOAD_WATCHDOG_MS,0,reload_timeout,0);
    if(U.reload_timer>=0 && U.ctx->time->timer_start(U.ctx,U.reload_timer)<0) reload_timer_stop();
}
static int request_reload(void) {
    int rc;char dir[32];if(U.busy) return 0;
#ifdef RH_MANAGER_UI_TEST
    probe_state.reload++;
#endif
    /* A second tap replaces a lingering result immediately; without this the
       card would keep the last outcome while a new transaction is running. */
    notice_clear();
    rc=next_config_dir(dir);
    if(!rc) rc=ensure_data();
    if(!rc) rc=rh_manager_data_refresh(U.data);
    if(rc) {notice_set(RH_NOTICE_LOAD_FAILED,0,rc);return render();}
    rc=rh_manager_data_prepare_apply(U.data,dir);
    if(rc) {notice_set(RH_NOTICE_PREPARE_FAILED,0,rc);return render();}
    U.busy=1;
    rc=rh_module_request_reload_from(U.reload_generation,dir);
    if(rc) {(void)rh_manager_data_complete_apply(U.data,0);U.busy=0;notice_set(RH_NOTICE_REJECTED,0,rc);}
    else reload_watchdog();
    rh_module_get_status_snapshot(&U.status);return render();
}
/* The confirmation action is a stock ACTION_ROW, not the detail page's bitmap
   button: a reference-protected package has to show a disabled state, and a
   bitmap cannot take the grey foreground. Disabled is drawn two ways at once
   because they cost nothing: the host's CANOPUS_UI_COLOR_DISABLED foreground,
   plus style.opacity, which every shipped target now binds through
   FW_UI_STYLE_OPA (the .043 generator emits it, EVID-UI-1043-OPACITY-001). The
   title turns DANGER red while the action is live; the disabled copy keeps its
   event id because action_row rejects a zero one, and the runtime refuses the
   dispatch on its own through the cleared ENABLED flag. */
static int confirm_action(struct canopus_ui_tree_v1 *t,int32_t y,uint8_t enabled) {
    static const char *const label="删除";
    const char *detail=enabled?"删除后无法恢复":"当前不可用";
    struct canopus_ui_action_row_props_v1 p={sizeof(p),label,len(label),detail,len(detail),EV_DELETE,enabled};
    struct canopus_ui_style_v1 s=CANOPUS_UI_STYLE_DEFAULT;
    TRY(U.ctx->ui->action_row(t,KEY_ACTION,&p));
    TRY(box(t,KEY_ACTION,(int)U.row_width,96,((int)U.width-(int)U.row_width)/2,y,0,0,-1,0));
    if(enabled) s.variant=CANOPUS_UI_VARIANT_DESTRUCTIVE;
    else {s.foreground=CANOPUS_UI_COLOR_DISABLED;s.opacity=500;}
    return U.ctx->ui->node_set_style(t,KEY_ACTION,&s);
}
static int build_confirm(struct canopus_ui_tree_v1 *t) {
    static const char *const blocked_text="正在使用中，无法删除。请先在「混搭微调」中改回「默认」。";
    const char *name=U.metadata.name[0]?U.metadata.name:U.detail_id;
    const char *message="删除后，资源包文件与索引将一并移除，且无法恢复。";
    uint32_t color=0xc5cbd5;
    if(U.delete_blocked) {message=U.delete_blocked==RH_MANAGER_ERR_ACTIVE?blocked_text:"无法确认该资源包的使用状态，删除可能失败。";color=0xda593c;}
    else if(U.confirm_error[0]) {message=U.confirm_error;color=0xda593c;}
    TRY(row(t,KEY_ROW,"主题包",name,0));
    TRY(box(t,KEY_ROW,(int)U.row_width,96,((int)U.width-(int)U.row_width)/2,0,0,0,-1,0));
    TRY(wrap_lines(t,KEY_ROW+2,message,110,20,color,4));
    /* Only a live reference is a permanent refusal. Any other preflight error
       (a busy catalog, a stale index) keeps the action so it can be retried. */
    return confirm_action(t,(int32_t)U.content_height-106,U.delete_blocked!=RH_MANAGER_ERR_ACTIVE);
}
static int delete_confirm(void) {
    int rc;
    /* Advisory preflight answered at page load; the transaction re-checks under
       its own guard, so a stale answer only costs a refused confirm. */
    if(U.busy || U.delete_blocked==RH_MANAGER_ERR_ACTIVE) return 0;
    U.busy=1;rc=rh_manager_data_delete(U.data,U.detail_id);U.busy=0;
    /* The disappearing entry is the success signal. A refusal keeps the page so
       the reason stays readable and the action can be retried. */
    if(!rc) return navigate(RESOURCES,1);
    if(rc==RH_MANAGER_ERR_ACTIVE)
        copy(U.confirm_error,sizeof(U.confirm_error),"正在使用中，无法删除。请先在「混搭微调」中改回「默认」。");
    else if(rc==RH_MANAGER_ERR_INVALID)
        copy(U.confirm_error,sizeof(U.confirm_error),"资源包索引损坏，删除失败。");
    else {const char *reason=U.data?rh_manager_data_error(U.data):0;
        copy(U.confirm_error,sizeof(U.confirm_error),reason&&*reason?reason:"删除失败，请稍后重试。");}
    return render();
}
static int choose_option(uint32_t index) {
    struct rh_manager_option option;
    if(U.busy) return 0;
#ifdef RH_MANAGER_UI_TEST
    probe_state.pick++;
#endif
    TRY(rh_manager_data_option_get(U.data,U.mix_key,index,&option));
    /* A failed save used to be a toast; the page keeps the old selection and
       reports nothing while no non-blocking notice surface exists. */
    (void)rh_manager_data_choose(U.data,U.mix_key,option.id);
    return render();
}
static int on_event(const struct canopus_context_v1 *ctx,struct canopus_ui_context_v1 *ui,
                    const struct canopus_ui_event_v1 *e,void *cookie) {
    uint32_t event=e->event_id;(void)ctx;(void)ui;(void)cookie;
    if(!U.opened || e->generation!=U.generation) return CANOPUS_UI_ERR_STALE;
    if(e->key==KEY_ACTION+1 && !event) {
        if(U.route==INDEX) event=EV_RELOAD;
        /* The same bottom asset carries one event on both pages: open the
           confirmation here, run the delete there. */
        else if(U.route==DETAIL || U.route==DELETE) event=EV_DELETE;
    }
    /* Failed commits retain the old native tree. Never strand its captured
       pointer or timers: release/cancel/back still own gesture cleanup. */
    if(U.render_failed && e->type==CANOPUS_UI_EVENT_RELEASE) {
        cancel_gesture();U.suppressed=1;return render();
    }
    if(U.render_failed && e->type!=CANOPUS_UI_EVENT_CANCEL &&
       !(event==EV_BACK && e->type==CANOPUS_UI_EVENT_ACTIVATE)) return render();
    if(U.route==RESOURCES && !event && e->key>=KEY_ROW && e->key-KEY_ROW<U.count)
        event=EV_ORDER+e->key-KEY_ROW;
    /* The preview wrapper, its bare image child and the check marks report
       only their keys with a zero event id. Map any key inside an option's
       slot span to that option's pick, so the whole card stays operable. */
    if(U.route==SELECT && !event && e->key>=KEY_ROW && (e->key-KEY_ROW)/OPTION_SLOTS<U.count)
        event=EV_CHOOSE+(e->key-KEY_ROW)/OPTION_SLOTS;
#ifdef RH_MANAGER_UI_TEST
    /* Acceptance-only: a long hold cycles the transfer banner states. The home
       card is gesture-free; on resources only non-row holds cycle, so the
       drag-to-reorder long press keeps its meaning. */
    if(e->type==CANOPUS_UI_EVENT_LONG_PRESS &&
       (U.route==INDEX || (U.route==RESOURCES && !(e->key>=KEY_ROW && e->key-KEY_ROW<U.count)))) {
        struct rh_transfer_ui_state s={0};uint32_t cycle=(probe_state.cycle+1u)%7u;
        probe_state.cycle=cycle;
        if(cycle>=1) {
            s.remote_connected=1;s.archive_bytes=1000;s.written_offset=cycle==1?680:1000;
            s.percent=cycle==1?68:100;s.durable_offset=cycle==1?0:1000;
            s.phase=cycle==1?RH_XFER_PHASE_RECEIVING:cycle==2?RH_XFER_PHASE_VERIFYING:
                    cycle==3?RH_XFER_PHASE_INSTALLING:cycle==4?RH_XFER_PHASE_INSTALLED:
                    cycle==5?RH_XFER_PHASE_ERROR:RH_XFER_PHASE_CANCELED;
            if(cycle==5) s.detail=6;
        }
        return rh_manager_set_transfer_state(&s);
    }
#endif
    if(event==EV_SCROLL && e->type==CANOPUS_UI_EVENT_SCROLL) {
#ifdef RH_MANAGER_UI_TEST
        probe_state.scroll++;
#endif
        U.scroll[U.route]=e->scroll_y;
        if(U.drag) (void)update_drag();else if(U.pressed && e->scroll_y!=U.start_scroll) {U.moved=1;clear_hold();}
        /* Rebuild only when the sample leaves the committed window. Rebuilding
           on every sample would recreate the tree (and re-issue a position
           write) mid-gesture, which the native overscroll spring cannot
           survive; the previous first-row test did that at a fixed stride that
           the variable-height pages do not share. */
        if(window_covers(e->scroll_y)) return 0;
        return render();
    }
    if(e->type==CANOPUS_UI_EVENT_PRESS) {
        U.pressed=1;U.suppressed=0;U.press_id=event;U.press_key=e->key;U.press_time=e->time_ms;U.start_x=e->x;U.start_y=U.last_y=e->y;
        U.start_scroll=U.scroll[U.route];U.moved=0;
        clear_hold();
        if(U.route==RESOURCES && event>=EV_ORDER && event<EV_MIX_ITEM) {
            U.hold_timer=U.ctx->time->timer_create(U.ctx,450,0,hold_tick,0);
            if(U.hold_timer>=0 && U.ctx->time->timer_start(U.ctx,U.hold_timer)<0) clear_hold();
        }
        return 0;
    }
    if(e->type==CANOPUS_UI_EVENT_MOVE && U.pressed) {
        int32_t dx=e->x-U.start_x,dy=e->y-U.start_y;U.last_y=e->y;
        if(dx>SLOP || dx<-SLOP || dy>SLOP || dy<-SLOP) {U.moved=1;clear_hold();}
        if(U.drag) return update_drag();return 0;
    }
    if(e->type==CANOPUS_UI_EVENT_LONG_PRESS && e->time_ms-U.press_time>=450u) return begin_drag();
    if(e->type==CANOPUS_UI_EVENT_CANCEL) {cancel_gesture();U.suppressed=1;return render();}
    if(e->type==CANOPUS_UI_EVENT_RELEASE) {
        int click=U.pressed && !U.moved && U.press_id==event;clear_hold();
        if(U.drag) {
            uint32_t from=U.drag_from,to=U.drag_to;cancel_gesture();U.suppressed=1;
#ifdef RH_MANAGER_UI_TEST
            probe_state.drag++;
#endif
            (void)rh_manager_data_move(U.data,from,to);
            return render();
        }
        U.pressed=0;
        if(event==EV_RELOAD && click) return request_reload();
        if(event==EV_DELETE && click) {
            if(U.route==DETAIL && !U.busy) {
#ifdef RH_MANAGER_UI_TEST
                /* The acceptance probe counts confirmation pages, not presses:
                   a refused package has no action to press. */
                probe_state.delete_taps++;
#endif
                return navigate(DELETE,0);
            }
            if(U.route==DELETE) return delete_confirm();
        }
        if(event==EV_DISMISS && click) {U.banner_hidden=1;return render();}
        /* The preview wrapper is not a prefab row, so it never sees ACTIVATE;
           its click is the release inside slop, like the image buttons. */
        if(click && event>=EV_CHOOSE && U.route==SELECT) return choose_option(event-EV_CHOOSE);
        return 0;
    }
    if(e->type!=CANOPUS_UI_EVENT_ACTIVATE && e->type!=CANOPUS_UI_EVENT_VALUE_CHANGED) return 0;
    if(event==EV_BACK) return back();
    if(event==EV_DELETE && U.route==DELETE) return delete_confirm();
    if(event==EV_RESOURCES) {U.scroll[RESOURCES]=0;return navigate(RESOURCES,1);}
    if(event==EV_MIX) {U.scroll[MIX]=0;return navigate(MIX,1);}
    if(event>=EV_OPTION && event<EV_CHOOSE && U.route==SELECT) return choose_option(event-EV_OPTION);
    if(event>=EV_MIX_ITEM && event<EV_OPTION && U.route==MIX) {
        struct rh_manager_mix_item item;
        TRY(rh_manager_data_mix_get(U.data,event-EV_MIX_ITEM,&item));
        copy(U.mix_key,sizeof(U.mix_key),item.key);copy(U.mix_title,sizeof(U.mix_title),item.feature?item.label:"");
        U.scroll[SELECT]=0;return navigate(SELECT,0);
    }
    if(event>=EV_ORDER && event<EV_MIX_ITEM && U.route==RESOURCES) {
        if(U.suppressed || U.moved) {U.suppressed=0;return 0;}
        TRY(rh_manager_data_order_get(U.data,event-EV_ORDER,U.detail_id,&U.metadata));
        if(equal(U.detail_id,RH_MANAGER_SYSTEM)) return 0;
        U.scroll[DETAIL]=0;return navigate(DETAIL,0);
    }
    return 0;
}
static void posted(const struct canopus_context_v1 *ctx,void *cookie) {
    uint8_t prior=U.transfer.phase;(void)ctx;(void)cookie;
    rh_module_get_status_snapshot(&U.status);rh_manager_get_transfer_state(&U.transfer);
    if(U.busy && U.status.reload_generation==U.reload_generation && U.status.reload_result!=1) {
        int success=U.status.reload_result==0;
        int completion;reload_timer_stop();
        completion=rh_manager_data_complete_apply(U.data,success);U.busy=0;
        /* Success with a failed protection record is its own kind: the refresh
           did work, so the card must not claim a reload failure. */
        if(!success) notice_set(RH_NOTICE_MODULE_FAILED,0,U.status.reload_result);
        else if(completion) notice_set(RH_NOTICE_PROTECT_FAILED,0,completion);
        else notice_set(RH_NOTICE_DONE,U.status.last_changed,0);
        if(U.opened && U.route!=INDEX) (void)load_route(U.route,1);
    }
    if(prior!=U.transfer.phase) {
        U.banner_hidden=0;
        if(U.opened && U.transfer.phase==RH_XFER_PHASE_INSTALLED && U.route==RESOURCES)
            (void)load_route(RESOURCES,1);
    }
    (void)render();
}
static void changed(void *cookie,uint32_t changes) {
    (void)cookie;(void)changes;
    if(U.ctx && U.ctx->ui && U.ctx->ui->post) (void)U.ctx->ui->post(U.ctx,posted,0);
}
static int on_open(const struct canopus_context_v1 *ctx,struct canopus_ui_context_v1 *ui,void *cookie) {
    uint32_t caps;int rc;(void)cookie;U.ctx=ctx;U.ui=ui;
    caps=ctx->ui->capabilities(ui);
    {uint32_t need=CANOPUS_UI_CAP_NAVIGATION_HEADER|CANOPUS_UI_CAP_LAYOUT|CANOPUS_UI_CAP_VISUAL|CANOPUS_UI_CAP_STYLE|CANOPUS_UI_CAP_FONT|CANOPUS_UI_CAP_IMAGE|CANOPUS_UI_CAP_IMAGE_FIT_CONTAIN|CANOPUS_UI_CAP_POINTER_EVENTS|CANOPUS_UI_CAP_CAPTURE|CANOPUS_UI_CAP_TRANSLATE|CANOPUS_UI_CAP_VIRTUAL_CONTENT|CANOPUS_UI_CAP_SCROLL|CANOPUS_UI_CAP_PROGRESS;
    if((caps&need)!=need) return CANOPUS_UI_ERR_UNSUPPORTED;}
    ctx->display->get_resolution(ctx,&U.width,&U.height);U.wide=U.width>=300;
    U.content_height=U.wide?424:444;U.row_width=U.wide?336:204;
    if((U.wide && (U.width!=336 || U.height!=480)) || (!U.wide && (U.width!=212 || U.height!=520))) return CANOPUS_UI_ERR_UNSUPPORTED;
    rc=rh_manager_assets_install(ctx);if(rc) return rc;
    U.opened=1;U.render_failed=0;U.route=INDEX;U.error[0]=0;U.suppressed=0;U.banner_hidden=0;
    U.delete_blocked=0;U.confirm_error[0]=0;
    reload_timer_stop();notice_clear();
    rh_module_get_status_snapshot(&U.status);rh_manager_get_transfer_state(&U.transfer);
    rc=render();if(rc) {U.opened=0;U.ui=0;}return rc;
}
static int on_back(const struct canopus_context_v1 *ctx,struct canopus_ui_context_v1 *ui,void *cookie) {
    (void)ctx;(void)ui;(void)cookie;
    if(U.route==INDEX) return 1;
    /* The host interprets every nonzero result as permission to close the app.
     * An internal pop owns this event even if its replacement tree fails. */
    (void)back();return 0;
}
static void on_close(const struct canopus_context_v1 *ctx,void *cookie) {
    (void)ctx;(void)cookie;cancel_gesture();reload_timer_stop();notice_timer_stop();
    U.notice=RH_NOTICE_NONE;U.opened=0;U.ui=0;
}
/* One firmware page per route, so a route change is a stock push/pop. A
 * backend without ctx->ui->route ignores page_count and replaces the tree in
 * the single page as before, so this stays correct on older targets. */
static const struct canopus_ui_entry_v1 entry={sizeof(entry),"Corona",on_open,on_event,on_back,on_close,0,ROUTES};
int rh_manager_ui_set_data_hooks(const struct rh_manager_data_hooks *hooks) {
    struct rh_manager_data_hooks empty={0};const struct rh_manager_data_hooks *next=hooks?hooks:&empty;
    if(U.data && (U.hooks.remove_tree!=next->remove_tree || U.hooks.quickapp_icon!=next->quickapp_icon || U.hooks.cookie!=next->cookie))
        return CANOPUS_SVC_ERR_STATE;
    U.hooks=*next;return 0;
}
int rh_manager_ui_register(const struct canopus_context_v1 *ctx) {
    int rc;
    if(!ctx || ctx->struct_size<sizeof(*ctx) || !ctx->ui || !ctx->ui->set_entry || !ctx->ui->post || !ctx->display || !ctx->display->get_resolution || !ctx->time || !ctx->time->monotonic_ms) return CANOPUS_SVC_ERR_UNSUPPORTED;
    if(U.ctx && U.ctx!=ctx) return CANOPUS_SVC_ERR_STATE;
    U.ctx=ctx;
    if(!U.subscribed) {rc=rh_manager_subscribe(changed,0);if(rc) return rc;U.subscribed=1;}
    return ctx->ui->set_entry(ctx,&entry);
}
int rh_manager_launcher_register(const struct canopus_context_v1 *ctx) {
    const char *icon=0;int32_t rc;struct canopus_app_entry_v1 app={sizeof(app),"Corona",0,0,0};
    if(!ctx || ctx->struct_size<sizeof(*ctx) || !ctx->launcher || !ctx->launcher->register_app) return CANOPUS_SVC_ERR_UNSUPPORTED;
    TRY(rh_manager_assets_install(ctx));TRY(rh_manager_asset_ensure(RH_MANAGER_ASSET_LOGO,&icon));
    app.icon=icon;rc=ctx->launcher->register_app(ctx,&app);return rc<0?rc:0;
}

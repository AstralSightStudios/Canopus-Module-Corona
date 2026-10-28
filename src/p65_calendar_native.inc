/* P65 native calendar is a 96x96 FILE icon. Module-owned banks preserve stock. */
#include "resource_hook_p65_calendar.h"
#define P65_N(a,b,c,d,e) (((uint32_t (*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t)) \
    (uintptr_t)((a)|1u))((uintptr_t)(b),(uintptr_t)(c),(uintptr_t)(d),(uintptr_t)(e)))
static uint32_t p65_rd(uint32_t a) { return *(volatile const uint32_t *)(uintptr_t)a; }
static uint32_t p65_half(uint32_t a) { return *(volatile const uint16_t *)(uintptr_t)a; }
static void p65_zero(void *p, unsigned n) { uint8_t *s=p; while(n--) *s++=0; }
static void p65_barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static int p65_calendar_audited(void) {
    return p65_rd(RH_P65_CN_GENERATE)==0x41f0e92du &&
           p65_rd(RH_P65_CN_CLOCK)==0xb084b5b0u &&
           p65_rd(RH_P65_CN_SNAPSHOT)==0x4ff0e92du &&
           p65_rd(RH_P65_CN_HANDLER_TABLE+12)==(RH_P65_CN_CACHE_INVALIDATE|1u) &&
           p65_rd(RH_P65_CN_HANDLER_TABLE+16)==(RH_P65_CN_CACHE_CLEAN|1u);
}
/* Tree, wrappers and output buffer remain owned until a successful GPU finish.
 * A failed draw retains exactly one bounded generation until retry/reset. */
static struct {
    uint32_t root, buffer, fonts[3], managers[3];
    char paths[3][RH_PATH];
    unsigned busy;
} p65_calendar;
/* Successful GPU finish precedes retirement of native borrowed glyph refs.
 * Finish alone is insufficient: the exact VG wait also drains CPU containers. */
static int p65_calendar_finish(void) {
    uint32_t unit,dispatch;unsigned count=0;
    if((int32_t)P65_N(RH_P65_CN_GPU_FINISH,0,0,0,0)) return -1;
    for(unit=p65_rd(RH_P65_CN_DRAW_UNITS);unit;unit=p65_rd(unit)) {
        if(++count>32 || p65_rd(unit+24)) return -1;
        dispatch=p65_rd(unit+12);
        if(dispatch!=RH_P65_CN_VG_DISPATCH && dispatch!=RH_P65_CN_SW_DISPATCH) return -1;
    }
    for(unit=p65_rd(RH_P65_CN_DRAW_UNITS);unit;unit=p65_rd(unit))
        if(p65_rd(unit+12)==RH_P65_CN_VG_DISPATCH) P65_N(RH_P65_CN_NATIVE_WAIT,unit,0,0,0);
    return 0;
}
static void p65_calendar_cleanup(void) {
    unsigned i;
    if(p65_calendar.root) P65_N(RH_P65_CN_OBJECT_DELETE,p65_calendar.root,0,0,0);
    p65_calendar.root=0;
    if(p65_calendar.buffer) P65_N(RH_P65_CN_DRAW_BUF_DESTROY,p65_calendar.buffer,0,0,0);
    p65_calendar.buffer=0;
    for(i=0;i<3;i++) if(p65_calendar.fonts[i]) {
        P65_N(RH_P65_CN_FONT_RELEASE,p65_calendar.managers[i],p65_calendar.fonts[i],0,0);
        p65_calendar.fonts[i]=p65_calendar.managers[i]=0;
    }
}
int rh_calendar_native_date(struct rh_calendar_date *out) {
    static const char *const keys[]={"Sun.common","Mon.common","Tue.common","Wed.common","Thu.common","Fri.common","Sat.common"};
    uint32_t ts[5],tm[11],i; struct rh_calendar_date d; const char *localized;
    if(!out || !p65_calendar_audited()) return -1;
    p65_zero(ts,sizeof(ts)); p65_zero(tm,sizeof(tm)); ts[2]=0xffffffffu;
    if((int32_t)P65_N(RH_P65_CN_CLOCK,0,ts,0,0)<0 || ts[2]>=1000000000u ||
       !P65_N(RH_P65_CN_LOCALTIME,ts[0],ts[1],tm,0) || tm[5]>8099u || tm[6]>6u) return -1;
    d.year=(int32_t)tm[5]+1900;d.month=(int32_t)tm[4]+1;d.day=(int32_t)tm[3];
    d.weekday=tm[6]?(int32_t)tm[6]-1:6;
    if(!rh_calendar_date_valid(&d)) return -1;
    localized=(const char *)(uintptr_t)P65_N(RH_P65_CN_LOCALIZED,keys[tm[6]],0,0,0);
    if(!localized) return -1;
    d.locale=2166136261u;
    for(i=0;i<16 && localized[i];i++) d.locale=(d.locale^(uint8_t)localized[i])*16777619u;
    *out=d;return 0;
}
static int p65_calendar_item(uint32_t image,const struct rh_calendar_config *c,
                             const struct rh_calendar_date *d,unsigned i) {
    static const char *const keys[]={"Mon.common","Tue.common","Wed.common","Thu.common","Fri.common","Sat.common","Sun.common"};
    const struct rh_calendar_item *p=&c->items[i];const char *context;uint32_t obj,font;
    char text[16];
    if(rh_calendar_item_context(c,d,i,&context)) return -1;
    if(p->type==RH_CALENDAR_NONE) return 0;
    if(p->type==RH_CALENDAR_IMAGE) {
        uint16_t info[6];p65_zero(info,sizeof(info));
        if(!context || rh_calendar_image_path(p65_calendar.paths[i],context) ||
           P65_N(RH_P65_CN_IMAGE_INFO,p65_calendar.paths[i],info,0,0)!=1 || !info[2] || !info[3]) return -1;
        obj=P65_N(RH_P65_CN_IMAGE_CREATE,image,0,0,0);if(!obj) return -1;
        P65_N(RH_P65_CN_IMAGE_SOURCE,obj,p65_calendar.paths[i],0,0);
        if(!p65_rd(obj+52) || p65_rd(obj+64)!=info[2] || p65_rd(obj+68)!=info[3]) return -1;
    } else {
        obj=P65_N(RH_P65_CN_LABEL_CREATE,image,0,0,0);if(!obj) return -1;
        p65_zero(text,sizeof(text));
        if(!context) {
            if(i==2) context=(const char *)(uintptr_t)P65_N(RH_P65_CN_LOCALIZED,keys[d->weekday],0,0,0);
            else {rh_calendar_decimal(text,i==0?(unsigned)d->month:(unsigned)d->day);context=text;}
        }
        if(!context) return -1;
        P65_N(RH_P65_CN_LABEL_TEXT,obj,context,0,0);
        P65_N(RH_P65_CN_COLOR,obj,p->color_native?(i==2?0x3482ffu:0x1a1a1au):p->color,0,0);
        P65_N(RH_P65_CN_TEXT_ALIGN,obj,2,0,0);
        if(!p->size && i) font=P65_N(RH_P65_CN_FONT_RESOLVE,i==1?RH_P65_CN_FONT_DAY:RH_P65_CN_FONT_WEEK,0,0,0);
        else {
            uint32_t root=p65_rd(RH_P65_CN_UIKIT_SLOT),manager;
            struct {const char *family;uint16_t pixels,style;} key={"MiSans-Medium",(uint16_t)(p->size?p->size:20),0};
            if(!root || !(manager=p65_rd(root+28))) return -1;
            font=P65_N(RH_P65_CN_FONT_CREATE,manager,&key,0,0);if(!font) return -1;
            p65_calendar.fonts[i]=font;p65_calendar.managers[i]=manager;
        }
        if(!font) return -1;
        P65_N(RH_P65_CN_FONT_STYLE,obj,font,0,0);
    }
    P65_N(RH_P65_CN_ALIGN,obj,p->align,p->x,p->y);return 0;
}
static int p65_calendar_save(uint32_t buf,const char *output) {
    uint8_t header[12];uint32_t stride,size,pixels,i;int fd,rc;
    if(!buf || *(volatile uint8_t *)(uintptr_t)(buf+1)!=16 || p65_half(buf+4)!=96 || p65_half(buf+6)!=96) return -1;
    stride=p65_half(buf+8);size=p65_rd(buf+12);pixels=p65_rd(buf+16);
    if(stride<384 || (stride&3) || size!=stride*96 || size>128u*1024u || !pixels || (pixels&3) ||
       (p65_half(buf+2)&~0x33u) || !(p65_half(buf+2)&2u) || p65_half(buf+10)) return -1;
    /* P65's exact platform cache clean/invalidate handlers are no-op bodies.
     * Require their installed identities and honor the native contract rather
     * than borrowing another target's cache instructions. Finish has succeeded. */
    p65_barrier();P65_N(RH_P65_CN_BUF_INVALIDATE,pixels,size,16,0);p65_barrier();
    P65_N(RH_P65_CN_PREMULTIPLY,buf,0,0,0);
    if(!(p65_half(buf+2)&1u)) return -1;
    for(i=0;i<12;i++) header[i]=*(volatile uint8_t *)(uintptr_t)(buf+i);
    header[0]=25;header[2]&=(uint8_t)~0x32u;
    fd=rh_platform_open(output,RH_FILE_REPLACE);if(fd<0) return -1;
    rc=rh_calendar_full_write(fd,header,sizeof(header));
    if(!rc) rc=rh_calendar_full_write(fd,(const void *)(uintptr_t)pixels,size);
    rh_platform_close(fd);return rc;
}
int rh_calendar_native_render(const struct rh_calendar_config *c,const struct rh_calendar_date *d,const char *output) {
    uint32_t image;unsigned i;int rc=-1,stock;
    if(!c || !c->custom || !rh_calendar_date_valid(d) || !rh_calendar_bounded(output,RH_PATH) ||
       output[0]!='/' || c->pool_used>RH_CALENDAR_POOL_BYTES || p65_calendar.busy || !p65_calendar_audited()) return -1;
    /* Native launcher_update selects this logical key only if stock generated
     * file exists. Refuse before drawing when its producer has not run yet;
     * never report a published custom bank that the launcher cannot select. */
    p65_calendar.busy=1;
    stock=rh_platform_open("/data/quickapp/app/perpetual_calendar/calendar_icon.bin",RH_FILE_READ);
    if(stock<0) goto done;
    rh_platform_close(stock);
    if(p65_calendar_finish()) goto done;
    p65_calendar_cleanup();
    p65_calendar.root=P65_N(RH_P65_CN_ROOT_CREATE,0,0,0,0);if(!p65_calendar.root) goto cleanup;
    P65_N(RH_P65_CN_SIZE,p65_calendar.root,96,96,0);
    image=P65_N(RH_P65_CN_IMAGE_CREATE,p65_calendar.root,0,0,0);if(!image) goto cleanup;
    P65_N(RH_P65_CN_IMAGE_SOURCE,image,"/resource/app/perpetual_calendar/launcher_icon.bin",0,0);
    if(!p65_rd(image+52) || !p65_rd(image+64) || !p65_rd(image+68)) goto cleanup;
    P65_N(RH_P65_CN_SIZE,image,96,96,0);P65_N(RH_P65_CN_ALIGN,image,9,0,0);
    for(i=0;i<3;i++) if(p65_calendar_item(image,c,d,i)) goto cleanup;
    p65_calendar.buffer=P65_N(RH_P65_CN_DRAW_BUF_CREATE,96,96,16,0);if(!p65_calendar.buffer) goto cleanup;
    P65_N(RH_P65_CN_CACHE_CLEAN,p65_calendar.buffer,0,0,0);p65_barrier();
    rc=P65_N(RH_P65_CN_SNAPSHOT,image,16,p65_calendar.buffer,0);
    if(p65_calendar_finish()) {rc=-1;goto done;}
    if(rc!=1) {rc=-1;goto cleanup;}
    rc=p65_calendar_save(p65_calendar.buffer,output);
cleanup:
    p65_calendar_cleanup();
done:
    p65_calendar.busy=0;return rc;
}
void rh_p65_calendar_publish(void) { P65_N(RH_P65_CN_LAUNCHER_UPDATE,69,0,0,0); }
int rh_p65_calendar_reset(void) {
    if(p65_calendar.busy || !p65_calendar_audited()) return -1;
    if(p65_calendar.root || p65_calendar.buffer) {
        if(p65_calendar_finish()) return -1;
        p65_calendar_cleanup();
    }
    return 0;
}
#undef P65_N

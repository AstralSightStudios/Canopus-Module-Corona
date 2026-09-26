#include "resource_hook_font_reload.h"

#if !defined(RH_TARGET_155) || !RH_TARGET_155 || \
    !defined(RH_EXPERIMENTAL_FONT_RELOAD) || !RH_EXPERIMENTAL_FONT_RELOAD
int rh_font_reload(const struct rh_mapping_view *current, uint32_t *changed) {
    (void)current;
    if (changed) *changed = 0;
    return 0;
}
void rh_font_reload_disable(void) {}
#else
#include "resource_hook_platform.h"
#include <stddef.h>

/* Q66 4.100.155 ONLY. PCs/layouts independently inspected in the fingerprinted
 * IDA database; see font-reload-investigation.md. No native wrapper creation,
 * cache constructor, registry remove/add, GPU wait, drain or reset is called.
 *
 * Support: managed outline FreeType records, including fallback wrappers;
 * ordinary style text and exact vector-label class in registered screen trees.
 * Each affected family must have (or previously have had in this instance) an
 * audited active/idle descriptor. A bare, never-used registry pathname cannot
 * establish its backend/style contract. Only scalar exemplar keys persist.
 * Not supported: copied/app-owned fonts, canvas pixels, custom cached text,
 * other font backends, framework restart, GPU error/reset/concurrent drawing.
 * An empty queue cannot prove hardware completion after GPU recovery. Explicit
 * opt-in assumes the healthy serialized standard UI path, not fault safety.
 *
 * Generation files MUST remain immutable and available. This module makes no
 * filesystem copies. A used non-stock pathname cannot be selected again after
 * leaving it, even with identical content. Current-path and stock fingerprints
 * detect accidental overwrites, not adversarial hash collisions. This is NOT a
 * substitute for the immutable-file contract. Limits fail closed, not truncate.
 */
#define FAMILIES 32u
#define BACKING_SCAN 256u
#define WRAPPERS 4096u
/* Backing records and wrapper references have independent cardinalities. */
#define RESOURCES (2u * BACKING_SCAN)
#define INTERN_IDS 96u
#define OBJECTS 512u
#define HISTORY 64u
#define CACHE_NODES 1024u
/* Stock .155 MiSans-Regular-All.ttf is 11,637,064 bytes. Bound reads,
 * not allocation: hashing streams the file and never loads it wholesale. */
#define FILE_LIMIT (32u * 1024u * 1024u)
#define MAGIC 1600079444u
#define CNT_CLASS 0x2ca16934u
#define SIZE_CLASS 0x2ca168b4u
#define METRICS 0x0c396b5du
#define OUTLINE 0x0c3a7c45u
#define RELEASE 0x0c3a0993u
#define VECTOR_CLASS 0x2ca6ee48u
#define UIKIT_SLOT 0x200bd1e8u
#define CONTEXT_SLOT 0x200bd3ecu
#define DRAW_SLOT 0x200bd318u
#define VECTOR_SLOT 0x200d3280u

/* Test builds execute the actual transaction against a 32-bit memory model;
 * only memory/native leaves are injected. No test models a physical GPU. */
#ifdef RH_FONT_RELOAD_TEST
extern uint32_t rh_fr_read(uint32_t, unsigned);
extern void rh_fr_write(uint32_t, uint32_t, unsigned);
extern uint32_t rh_fr_call(uint32_t, uintptr_t, uintptr_t, uintptr_t);
#define rd(a) rh_fr_read((a), 4)
#define rb(a) rh_fr_read((a), 1)
#define wr(a,v) rh_fr_write((a),(v),4)
#define wb(a,v) rh_fr_write((a),(v),1)
#define call(a,b,c,d) rh_fr_call((a),(uintptr_t)(b),(uintptr_t)(c),(uintptr_t)(d))
#else
static uint32_t rd(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static uint32_t rb(uint32_t a) { return *(volatile unsigned char *)(uintptr_t)a; }
static void wr(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
static void wb(uint32_t a, uint32_t v) { *(volatile unsigned char *)(uintptr_t)a = (unsigned char)v; }
#define call(a,b,c,d) (((uint32_t (*)(uintptr_t,uintptr_t,uintptr_t)) \
    (uintptr_t)((a)|1u))((uintptr_t)(b),(uintptr_t)(c),(uintptr_t)(d)))
#endif
static void zero(void *p, uint32_t n) { unsigned char *s=p; while(n--) *s++=0; }
static void nz(uint32_t p, uint32_t n) { while(n--) wb(p++,0); }
static void copy(char *d, const char *s) { while ((*d++=*s++) != 0) {} }
static int eq(const char *a, const char *b) { while(*a && *a==*b) {a++; b++;} return *a==*b; }
static uint32_t length(const char *s) { uint32_t n=0; while(s[n]) n++; return n; }
static int string(uint32_t p, char *out) {
    uint32_t i;
    if (!p) return 0;
    for(i=0;i<RH_PATH;i++) { out[i]=(char)rb(p+i); if(!out[i]) return 1; }
    return 0;
}
static int native_eq(uint32_t p, const char *s) {
    uint32_t i;
    if(!p) return 0;
    for(i=0;i<RH_PATH;i++) { if(rb(p+i)!=(unsigned char)s[i]) return 0; if(!s[i]) return 1; }
    return 0;
}
static uint32_t alloc(uint32_t n) {
    uint32_t p=call(0x0c3abe20u,n,0,0);
    if(p) nz(p,n);
    return p;
}
static void release_mem(uint32_t p) { if(p) (void)call(0x0c3abe58u,p,0,0); }
static uint32_t duplicate(const char *s) {
    uint32_t i,n=length(s)+1,p=alloc(n);
    if(p) for(i=0;i<n;i++) wb(p+i,(unsigned char)s[i]);
    return p;
}
struct digest { uint32_t a,b,size; };
static int fingerprint(const char *path, struct digest *out) {
    unsigned char buf[2048]; uint32_t i; int n,fd;
    struct digest h={2166136261u,0x9e3779b9u,0};
    fd=rh_platform_open(path,1);
    if(fd<0) return -2210;
    while((n=rh_platform_read(fd,buf,sizeof(buf)))>0) {
        if(n>(int)sizeof(buf)) { rh_platform_close(fd); return -2210; }
        if(h.size>FILE_LIMIT-(uint32_t)n) { rh_platform_close(fd); return -2211; }
        for(i=0;i<(uint32_t)n;i++) { h.a=(h.a^buf[i])*16777619u; h.b=(h.b<<5 | h.b>>27)^buf[i]; h.b+=h.a; }
        h.size+=(uint32_t)n;
    }
    rh_platform_close(fd);
    if(n<0 || !h.size) return -2210;
    *out=h; return 0;
}
static int digest_eq(struct digest a, struct digest b) { return a.a==b.a && a.b==b.b && a.size==b.size; }
struct family {
    uint32_t node,name_ptr,path_ptr,hashed,audited,size,style;
    char name[RH_PATH],stock[RH_PATH],current[RH_PATH];
    struct digest stock_hash,current_hash;
};
static struct {
    uint32_t disabled,initialized,running,uikit,manager,context,face_cache,count,history_count;
    struct family family[FAMILIES];
    char history[HISTORY][RH_PATH];
} state;
void rh_font_reload_disable(void) { state.disabled=1; }
static int disable(void) { rh_font_reload_disable(); return -1; }

/* Read current global roots first, never dereference a saved native identity
 * before establishing membership from live roots. Pointer equality is NOT an
 * epoch: parent must irreversibly disable on observed shutdown/restart. */
static int roots(uint32_t *manager, uint32_t *context) {
    uint32_t ui,ctx,mgr;
    if(state.disabled) return -1;
    ui=rd(UIKIT_SLOT); ctx=rd(CONTEXT_SLOT);
    if(!ui || !ctx) return state.initialized ? disable() : 1;
    if(state.initialized && (ui!=state.uikit || ctx!=state.context)) return disable();
    mgr=rd(ui+28);
    if(!mgr) return state.initialized ? disable() : 1;
    if(state.initialized && (mgr!=state.manager || rd(ctx+24)!=state.face_cache)) return disable();
    if(rd(mgr)!=48 || rd(mgr+12)!=40 || rd(mgr+24)!=8 || rd(ctx+4)!=8 ||
       !rd(ctx) || !rd(ctx+24) || rd(ctx+20)==0 || rd(ctx+20)>1024 ||
       rd(ctx+16)!=0x0c3981d1u) return disable();
    *manager=mgr; *context=ctx; return 0;
}
/* Generic audited intrusive list. Check both links and tail, bounded cycle
 * detection by count. node_size is validated by each caller. */
static int list(uint32_t head, uint32_t size, uint32_t *out, uint32_t max, uint32_t *count) {
    uint32_t p,prev=0,n=0;
    if(rd(head)!=size) return -1;
    for(p=rd(head+4);p;p=rd(p+size+4)) {
        if(n==max) return -2;
        if(rd(p+size)!=prev) return -3;
        if(out) out[n]=p;
        n++; prev=p;
    }
    if(rd(head+8)!=prev) return -4;
    *count=n; return 0;
}
static int registry(uint32_t mgr, uint32_t ctx) {
    uint32_t nodes[FAMILIES],n,i,j;
    if(list(mgr+24,8,nodes,FAMILIES,&n)) return disable();
    if(state.initialized && n!=state.count) return disable();
    for(i=0;i<n;i++) {
        struct family *f=&state.family[i]; uint32_t p=nodes[i];
        if(state.initialized) {
            if(p!=f->node || rd(p)!=f->name_ptr || rd(p+4)!=f->path_ptr ||
               !native_eq(rd(p),f->name) || !native_eq(rd(p+4),f->current)) return disable();
        } else {
            if(!string(rd(p),f->name) || !string(rd(p+4),f->stock) ||
               !f->name[0] || f->stock[0]!='/') return disable();
            for(j=0;j<i;j++) if(eq(f->name,state.family[j].name)) return disable();
            f->node=p; f->name_ptr=rd(p); f->path_ptr=rd(p+4); copy(f->current,f->stock);
        }
    }
    if(!state.initialized) {
        state.count=n; state.uikit=rd(UIKIT_SLOT); state.manager=mgr;
        state.context=ctx; state.face_cache=rd(ctx+24); state.initialized=1;
    }
    return 0;
}
/* LRU nodes contain RB-node pointer; RB+16 contains payload, followed by
 * cache-entry {cache, refcount, payload_offset, invalid}. Do not poll entries
 * after release: invalid entries can be freed by that call. */
static int cache_check(uint32_t cache, uint32_t clz, uint32_t size, int holds, int vector) {
    uint32_t p,prev=0,n=0;
    /* Stable diagnostic ranges: face 23xx, metrics 24xx, outline 25xx,
     * vector 28xx. Negative results remain refusals; held entries stay busy. */
    int base=vector ? 2800 : size==28 ? 2300 : size==32 ? 2400 : 2500;
    if(!cache) return -base-1;
    if(rd(cache)!=clz) return -base-2;
    if(rd(cache+4)!=size) return -base-3;
    if(rd(cache+48)!=4) return -base-4;
    if(clz==CNT_CLASS) {
        uint32_t compare,create,destroy;
        if(size==28) { compare=0x0c396785u; create=0x0c3967b5u; destroy=0x0c396b25u; }
        else if(size==32) { compare=0x0c39fd9bu; create=0x0c3a5ef1u; destroy=0x0c39fd95u; }
        else if(size==8) { compare=0x0c39fdcdu; create=0x0c3a8bb9u; destroy=0x0c3a09f5u; }
        else return -base-20;
        if(rd(cache+16)!=compare) return -base-5;
        if(rd(cache+20)!=create) return -base-6;
        if(rd(cache+24)!=destroy) return -base-7;
    } else {
        if(!vector) return -base-20;
        if(rd(cache+16)!=0x0c69feb1u) return -base-5;
        if(rd(cache+20)) return -base-6;
        if(rd(cache+24)!=0x0c6a12d5u) return -base-7;
    }
    for(p=rd(cache+52);p;p=rd(p+8)) {
        uint32_t tree,data,entry,k;
        if(++n>CACHE_NODES) return -base-8;
        if(rd(p+4)!=prev) return -base-9;
        if(!(tree=rd(p))) return -base-10;
        if(!(data=rd(tree+16))) return -base-11;
        entry=data+size;
        if(rd(entry)!=cache) return -base-12;
        if(rd(entry+8)!=size) return -base-13;
        if(rb(entry+12)) return -base-14;
        if(rd(entry+4)>=0x7ffffffeu) return -base-15;
        if(holds && rd(entry+4)) return 1;
        if(vector) {
            uint32_t paths=rd(data+8),count=rd(data+12);
            if(count>1024 || (count && !paths)) return -base-16;
            for(k=0;k<count;k++) { uint32_t path=rd(paths+20*k); if(!path || (rb(path+36)&1)) return -base-17; }
        }
        prev=p;
    }
    if(rd(cache+56)!=prev) return -base-18;
    if(clz==CNT_CLASS && rd(cache+12)!=n) return -base-19;
    return 0;
}
static int queue(uint32_t p, uint32_t element, uint32_t cb) {
    uint32_t i;
    if(!p || rd(p+36)!=cb) return -1;
    for(i=4;i<=20;i+=16) {
        uint32_t capacity=rd(p+i+8),count=rd(p+i+4);
        if(rd(p+i+12)!=element || capacity>4096 || count>capacity || (capacity && !rd(p+i))) return -1;
        if(count) return 1;
    }
    return 0;
}
static int boundary(void) {
    uint32_t p,n=0,vg=0,sw=0,displays[4],nd,i; int r;
    if(!rb(0x200bd1ecu) || !rb(0x200bd210u)) return -1;
    if(list(0x200bd1f0u,792,displays,4,&nd) || !nd) return -1;
    for(i=0;i<nd;i++) {
        uint32_t layer,j=0,d=displays[i];
        if(rd(d+720)>OBJECTS || (rd(d+720) && !rd(d+692))) return -1;
        if(rb(d+58)&2) return 1;
        for(layer=rd(d+680);layer;layer=rd(layer+108)) {
            if(++j>64) return -1;
            if(rd(layer+100)) return 1;
        }
    }
    for(p=rd(DRAW_SLOT);p;p=rd(p)) {
        uint32_t dispatch=rd(p+16);
        if(++n>2) return -1;
        if(rd(p+32)) return 1;
        if(dispatch==0x0c3913edu) {
            uint32_t grad=rd(p+40);
            if(vg++ || !grad) return -1;
            if((r=queue(rd(p+48),24,0x0c399b63u))!=0) return r;
            if((r=queue(rd(p+36),76,0x0c395be1u))!=0) return r;
            if((r=queue(rd(grad+8),4,0x0c395bd1u))!=0) return r;
            if(rd(p+52)) return 1;
            if(rd(0x200d327cu) && rd(0x200d327cu)!=p) return -1;
        } else if(dispatch==0x0c3948adu) { if(sw++) return -1; }
        else return -1;
    }
    if(vg!=1 || sw!=1) return -1;
    p=rd(VECTOR_SLOT);
    return p ? cache_check(p,SIZE_CLASS,16,1,1) : 0;
}
/* idle: 0 active backing, 1 idle eviction, 2 temporary validation-only face. */
struct replacement { uint32_t family,node,font,dsc,fresh,refs,idle,size,style; };
struct wrap { uint32_t ptr,record,fallback,user; };
struct transaction {
    /* Deduplicate stock/current and shared generation reads within this turn.
     * Paths refer to stable family/transaction storage until commit. */
    uint32_t file_count;
    struct { const char *path; struct digest hash; } files[FAMILIES * 3u];
    uint32_t path[FAMILIES],dirty[FAMILIES],count,nres,nwrap,nobj;
    char target[FAMILIES][RH_PATH]; struct digest hash[FAMILIES];
    struct replacement res[RESOURCES];
    struct wrap *wrapper; /* Actual list count, at most WRAPPERS, on the heap. */
    uint32_t objects[OBJECTS],overflow;
};
static int family_index(const char *name) {
    uint32_t i; for(i=0;i<state.count;i++) if(eq(name,state.family[i].name)) return (int)i;
    return -1;
}
static int descriptor(uint32_t font, uint32_t ctx, const char *path) {
    uint32_t d,face,e; int r;
    if(!font) return -2601;
    if(rd(font)!=METRICS) return -2602;
    if(rd(font+4)!=OUTLINE) return -2603;
    if(rd(font+8)!=RELEASE) return -2604;
    d=rd(font+24);
    if(!d || font!=d+4) return -2605;
    if(rd(d)!=MAGIC) return -2606;
    if(rd(d+48)!=ctx) return -2607;
    if(rd(d+28)!=d) return -2608;
    if(!rd(d+40) || rd(d+40)>512) return -2609;
    if(rb(d+46)!=1 || rb(d+47)!=0) return -2610;
    if(!native_eq(rd(d+60),path)) return -2611;
    face=rd(d+52); e=rd(d+56);
    if(!face) return -2612;
    if(!e || e!=face+28) return -2613;
    if(rd(e)!=rd(ctx+24)) return -2614;
    if(rd(e+8)!=28) return -2615;
    if(!rd(e+4)) return -2616;
    if(rb(e+12)) return -2617;
    if(rd(face)!=rd(d+60)) return -2618;
    if(rd(face+4)!=rd(d+44)) return -2619;
    if(!rd(face+12)) return -2620;
    if(!(rd(rd(face+12)+8)&1u)) return -2621;
    if((r=cache_check(rd(face+16),CNT_CLASS,32,1,0))!=0) return r;
    return cache_check(rd(face+20),CNT_CLASS,8,1,0);
}
static int resources(struct transaction *t, uint32_t mgr, uint32_t ctx) {
    uint32_t nodes[BACKING_SCAN],n,i,j,p,idle=rd(mgr+556); char name[RH_PATH]; int f,r;
    r=cache_check(rd(ctx+24),CNT_CLASS,28,0,0); if(r) return r;
    if(rd(rd(ctx+24)+8)!=0x7fffffffu) return -2701;
    /* Scan all records, including unrelated families, with the existing
     * scratch capacity. RESOURCES bounds replacements, not registry occupancy. */
    r=list(mgr,48,nodes,BACKING_SCAN,&n); if(r) return -2740+r;
    for(i=0;i<n;i++) {
        uint32_t p=nodes[i];
        if(!string(rd(p+4),name)) return -2703;
        f=family_index(name);
        if(f<0 || !t->dirty[f]) continue;
        if(length(name)>=32) return -2704;
        if(rd(p+4)!=p+12) return -2705;
        if(!rd(p+44)) return -2706;
        if(t->nres==RESOURCES) return -2707;
        r=descriptor(rd(p),ctx,state.family[f].current); if(r) return r;
        if(rd(rd(rd(p)+24)+40)!=(rd(p+8)&65535u)) return -2708;
        if((rd(rd(rd(p)+24)+44)&65535u)!=(rd(p+8)>>16)) return -2709;
        t->res[t->nres++]=(struct replacement){(uint32_t)f,p,rd(p),rd(rd(p)+24),0,rd(p+44),0,
            rd(p+8)&65535u,rd(p+8)>>16};
    }
    r=list(mgr+12,40,0,WRAPPERS,&n); if(r) return -2760+r;
    if(n) {
        t->wrapper=rh_platform_alloc(n*sizeof(*t->wrapper));
        if(!t->wrapper) return -2;
    }
    p=rd(mgr+16);
    for(i=0;i<n;i++) {
        uint32_t rec;
        if(!p) return -2765;
        rec=rd(p+36);
        for(j=0;j<t->nres;j++) if(rec==t->res[j].node) {
            uint32_t k;
            /* Identify the exact differing copied word without overwriting
             * wrapper-specific state merely to make validation pass. */
            for(k=0;k<28;k+=4) if(rd(p+k)!=rd(t->res[j].font+k)) return -2730-(int)(k/4u);
            t->wrapper[t->nwrap++]=(struct wrap){p,rec,rd(p+28),rd(p+32)};
            break;
        }
        p=rd(p+44);
    }
    if(p) return -2765;
    for(i=0;i<t->nres;i++) {
        uint32_t count=0;
        for(j=0;j<t->nwrap;j++) if(t->wrapper[j].record==t->res[i].node) count++;
        if(count!=t->res[i].refs) return -2712;
    }
    if(idle) {
        r=list(idle,44,nodes,BACKING_SCAN,&n); if(r) return -2750+r;
        for(i=0;i<n;i++) {
            uint32_t p=nodes[i];
            if(!string(rd(p),name)) return -2714;
            f=family_index(name);
            if(f<0 || !t->dirty[f]) continue;
            if(length(name)>=32) return -2715;
            if(t->nres==RESOURCES) return -2716;
            r=descriptor(rd(p+40),ctx,state.family[f].current); if(r) return r;
            if(rd(rd(rd(p+40)+24)+40)!=(rd(p+4)&65535u)) return -2717;
            if((rd(rd(rd(p+40)+24)+44)&65535u)!=(rd(p+4)>>16)) return -2718;
            t->res[t->nres++]=(struct replacement){(uint32_t)f,p,rd(p+40),rd(rd(p+40)+24),0,0,1,
                rd(rd(rd(p+40)+24)+40),rd(rd(rd(p+40)+24)+44)&65535u};
        }
    }
    /* One descriptor must have exactly one manager backing owner. */
    for(i=0;i<t->nres;i++) for(j=0;j<i;j++) if(t->res[i].dsc==t->res[j].dsc) return -2719;
    for(i=0;i<state.count;i++) if(t->dirty[i]) {
        struct family *f=&state.family[i];
        for(j=0;j<t->nres && t->res[j].family!=i;j++) {}
        if(j==t->nres) {
            if(!f->audited) return -2206;
            if(t->nres==RESOURCES) return -2720;
            t->res[t->nres++]=(struct replacement){i,0,0,0,0,0,2,f->size,f->style};
        } else {
            f->audited=1; f->size=t->res[j].size; f->style=t->res[j].style;
        }
    }
    return 0;
}
/* Checked face-ID interning. Firmware drop_face_id owns these allocations.
 * Preallocate both node and string, then append without unchecked native list
 * allocation. Payload=8, prev=+8, next=+12, list at context+4. */
static uint32_t intern(uint32_t ctx, const char *path) {
    uint32_t nodes[INTERN_IDS],n,i,p,s,tail;
    if(list(ctx+4,8,nodes,INTERN_IDS,&n)) return 0;
    for(i=0;i<n;i++) if(native_eq(rd(nodes[i]),path)) {
        p=nodes[i]; if(!rd(p+4) || rd(p+4)>=0x7ffffffeu) return 0;
        wr(p+4,rd(p+4)+1); return rd(p);
    }
    if(n==INTERN_IDS) return 0;
    s=duplicate(path); if(!s) return 0;
    p=alloc(16); if(!p) { release_mem(s); return 0; }
    wr(p,s); wr(p+4,1); tail=rd(ctx+12); wr(p+8,tail);
    if(tail) wr(tail+12,p); else wr(ctx+8,p);
    wr(ctx+12,p); return s;
}
static uint32_t new_cache(uint32_t size,uint32_t count,uint32_t compare,uint32_t create,uint32_t destroy) {
    uint32_t p=alloc(64);
    if(!p) return 0;
    wr(p,CNT_CLASS); wr(p+4,size); wr(p+8,count);
    wr(p+16,compare); wr(p+20,create); wr(p+24,destroy);
    if(!call(0x0c3a9e48u,p,0,0)) { release_mem(p); return 0; }
    return p;
}
/* Entry invalid=0 and a held descriptor reference are checked before this
 * function. A valid zero-ref entry survives release until explicit drop. */
static void destroy_descriptor(uint32_t d) {
    uint32_t ctx=rd(d+48),cache=rd(ctx+24),e=rd(d+56),face=rd(d+52),path=rd(d+60);
    (void)call(0x0c8b9780u,cache,e,0);
    if(rd(e+4)==0) (void)call(0x0c8b8c9eu,cache,face,0);
    (void)call(0x0c39a424u,ctx,path,0);
    release_mem(d);
}
/* Fully checked replacement for the allocation-unsafe native font factory.
 * A preexisting target face is deliberately unsupported: it might belong to
 * an untracked consumer. Sharing is allowed only between this transaction's
 * newly prepared descriptors. Stock restoration works after prior generations
 * have evicted their old manager backing and idle entries. */
static uint32_t prepare(struct transaction *t, struct replacement *r, uint32_t ctx) {
    uint32_t d=0,path=0,e=0,face=0,i,found=0,key[7],ft,size,metrics,v;
    zero(key,sizeof(key));
    d=alloc(64); if(!d) return 0;
    path=intern(ctx,t->target[r->family]); if(!path) goto fail;
    key[0]=path; key[1]=65536u | r->style;
    e=call(0x0c3a3860u,rd(ctx+24),key,0);
    if(e) {
        for(i=0;i<t->nres;i++) if(t->res[i].fresh && rd(t->res[i].fresh+56)==e) found=1;
        if(!found || rd(e+4)==0xffffffffu) {
            (void)call(0x0c8b9780u,rd(ctx+24),e,0); e=0; goto fail;
        }
    } else {
        e=call(0x0c3a7b78u,rd(ctx+24),key,0);
        if(!e) goto fail;
        found=0;
    }
    face=e-rd(e+8);
    wr(d,MAGIC); wr(d+40,r->size); wr(d+44,key[1]); wr(d+48,ctx);
    wr(d+52,face); wr(d+56,e); wr(d+60,path);
    if(!found) {
        v=new_cache(32,2*rd(ctx+20),0x0c39fd9bu,0x0c3a5ef1u,0x0c39fd95u);
        if(!v) goto owned_fail;
        wr(face+16,v);
        v=new_cache(8,rd(ctx+20),0x0c39fdcdu,0x0c3a8bb9u,0x0c3a09f5u);
        if(!v) goto owned_fail;
        wr(face+20,v);
    }
    ft=rd(face+12); size=rd(d+40);
    /* The installed callbacks require scalable outlines, not a bitmap-only
     * face which happens to pass FT parsing and pixel-size setup. */
    if(!ft || !(rd(ft+8)&1u) || call(0x0c8b8a54u,ft,size,0)) goto owned_fail;
    metrics=rd(ft+88); if(!metrics) goto owned_fail;
    wr(d+4,METRICS); wr(d+8,OUTLINE); wr(d+12,RELEASE);
    wr(d+16,(uint32_t)((int32_t)rd(metrics+32)>>6));
    wr(d+20,(uint32_t)(-((int32_t)rd(metrics+28)>>6)));
    wr(d+28,d);
    v=call(0x0c424304u,rd(metrics+20),(int16_t)(rb(ft+80)|(rb(ft+81)<<8)),0);
    wb(d+25,(uint32_t)((int32_t)v>>6));
    v=call(0x0c424304u,rd(metrics+20),(int16_t)(rb(ft+82)|(rb(ft+83)<<8)),0);
    v=(uint32_t)((int32_t)(v<<18)>>24); wb(d+26,(int32_t)v<1 ? 1 : v);
    return d;
owned_fail:
    destroy_descriptor(d); return 0;
fail:
    if(path) (void)call(0x0c39a424u,ctx,path,0);
    release_mem(d); return 0;
}
struct membership { uint32_t wanted,found,visited,overflow; };
static int depth_ok(uint32_t p) { uint32_t n=0; while(p) { if(++n>32) return 0; p=rd(p+4); } return 1; }
static int collect(uint32_t p, void *cookie) {
    struct transaction *t=cookie;
    if(rb(p+51)&16) return 1;
    if(t->nobj==OBJECTS || !depth_ok(p)) { t->overflow=1; return 2; }
    t->objects[t->nobj++]=p; return 0;
}
static int find(uint32_t p, void *cookie) {
    struct membership *m=cookie;
    if(rb(p+51)&16) return 1;
    if(++m->visited>OBJECTS || !depth_ok(p)) { m->overflow=1; return 2; }
    if(p==m->wanted) { m->found=1; return 2; } return 0;
}
static void walk(int (*cb)(uint32_t,void *), void *cookie) { (void)call(0x0c380574u,0,cb,cookie); }
static int live(uint32_t p) { struct membership m={p,0,0,0}; walk(find,&m); return m.overflow ? -1 : (int)m.found; }
static int refresh(struct transaction *t) {
    uint32_t i;
    for(i=0;i<t->nobj;i++) {
        uint32_t p=t->objects[i]; int r=live(p);
        if(r<0) return -1;
        if(!r) continue;
        /* Drop copied non-uploaded vector paths, then property refresh lets the
         * next draw rebuild lazily. Avoid unchecked eager vector allocations. */
        if(rd(p)==VECTOR_CLASS) (void)call(0x0c6a1304u,p,0,0);
        r=live(p); if(r<0) return -1; if(!r) continue;
        (void)call(0x0c38525cu,p,0x000f0000u,90); /* LV_PART_ANY */
        if(state.disabled || rd(UIKIT_SLOT)!=state.uikit || rd(CONTEXT_SLOT)!=state.context) return disable();
    }
    return 0;
}
static int file_digest(struct transaction *t, const char *path, struct digest *out) {
    uint32_t i; int r;
    for(i=0;i<t->file_count;i++) if(eq(path,t->files[i].path)) {
        *out=t->files[i].hash; return 0;
    }
    if(t->file_count==FAMILIES*3u) return -2212;
    r=fingerprint(path,out); if(r) return r;
    t->files[t->file_count].path=path;
    t->files[t->file_count++].hash=*out;
    return 0;
}
static int select_paths(struct transaction *t, const struct rh_mapping_view *mapping) {
    uint32_t i;
    for(i=0;i<state.count;i++) {
        struct family *f=&state.family[i];
        int r=mapping->count ? rh_resolve_view(mapping,f->stock,t->target[i]) : 0;
        if(r<0) return -1;
        if(!r) copy(t->target[i],f->stock);
        if(!eq(t->target[i],f->current)) { t->dirty[i]=1; t->count++; }
    }
    return 0;
}
static int plan(struct transaction *t) {
    uint32_t i,j,needed=0;
    for(i=0;i<state.count;i++) {
        struct family *f=&state.family[i]; struct digest h; int r;
        if(!t->dirty[i] && !f->hashed) continue;
        if(!f->hashed) {
            r=file_digest(t,f->stock,&f->stock_hash); if(r) return r;
            f->current_hash=f->stock_hash; f->hashed=1;
        }
        r=file_digest(t,f->current,&h); if(r) return r;
        if(!digest_eq(h,f->current_hash)) return -2213;
        if(!t->dirty[i]) continue;
        if(length(f->name)>=32) return -1;
        r=file_digest(t,t->target[i],&t->hash[i]); if(r) return r;
        if(eq(t->target[i],f->stock)) {
            if(!digest_eq(t->hash[i],f->stock_hash)) return -1;
        } else {
            for(j=0;j<state.history_count;j++) if(eq(t->target[i],state.history[j])) return -1;
            /* Reject aliasing a different family's original stock resource. */
            for(j=0;j<state.count;j++) if(eq(t->target[i],state.family[j].stock)) return -1;
            needed++;
        }
    }
    return needed>HISTORY-state.history_count ? -1 : 0;
}
/* Re-establish membership before dereferencing any transaction snapshot. */
static int revalidate(struct transaction *t,uint32_t mgr,uint32_t ctx) {
    uint32_t nodes[BACKING_SCAN],n,i,j,k,p; int r;
    r=list(mgr,48,nodes,BACKING_SCAN,&n); if(r) return -2740+r;
    for(i=0;i<t->nres;i++) if(!t->res[i].idle) {
        struct replacement *v=&t->res[i];
        for(j=0;j<n && nodes[j]!=v->node;j++) {}
        if(j==n || rd(v->node)!=v->font || rd(v->node+44)!=v->refs) return -1;
        r=descriptor(v->font,ctx,state.family[v->family].current); if(r) return r;
    }
    if(rd(mgr+556)) {
        r=list(rd(mgr+556),44,nodes,BACKING_SCAN,&n); if(r) return -2750+r;
    } else n=0;
    for(i=0;i<t->nres;i++) if(t->res[i].idle==1) {
        struct replacement *v=&t->res[i];
        for(j=0;j<n && nodes[j]!=v->node;j++) {}
        if(j==n || rd(v->node+40)!=v->font) return -1;
        r=descriptor(v->font,ctx,state.family[v->family].current); if(r) return r;
    }
    r=list(mgr+12,40,0,WRAPPERS,&n); if(r) return -2760+r;
    /* Captured affected wrappers form an ordered subsequence of the live list.
     * Re-establish membership without a large stack array or stale dereference. */
    p=rd(mgr+16); j=0;
    for(i=0;i<n;i++) {
        if(!p) return -2765;
        if(j<t->nwrap && p==t->wrapper[j].ptr) {
            struct wrap *w=&t->wrapper[j++];
            if(rd(p+36)!=w->record || rd(p+28)!=w->fallback || rd(p+32)!=w->user) return -1;
            for(k=0;k<28;k+=4) if(rd(p+k)!=rd(rd(w->record)+k)) return -1;
        }
        p=rd(p+44);
    }
    if(p) return -2765;
    if(j!=t->nwrap) return -2766;
    return 0;
}
int rh_font_reload(const struct rh_mapping_view *mapping, uint32_t *changed) {
    struct transaction *t; uint32_t mgr,ctx,i,j; int r;
    if(changed) *changed=0;
    if(!changed || !mapping || rh_validate_rules(mapping->rules,mapping->count)) return -1;
    if(state.running) return 1;
    if((r=roots(&mgr,&ctx))!=0) return r<0 ? -2201 : r;
    if(registry(mgr,ctx)) return -2202;
    t=rh_platform_alloc(sizeof(*t)); if(!t) return -2;
    state.running=1;
    zero(t,sizeof(*t));
    r=select_paths(t,mapping); if(r) goto done;
    if(!t->count) { r=plan(t); goto done; }
    r=boundary(); if(r) { if(r<0) r=-2204; goto done; }
    r=resources(t,mgr,ctx); if(r) goto done;
    walk(collect,t); if(t->overflow) { r=-2207; goto done; }
    /* Do not stream large font files on every busy 50 ms retry. First prove
     * an idle boundary and known ownership, then hash each unique file once. */
    r=plan(t); if(r) goto done;
    for(i=0;i<state.count;i++) if(t->dirty[i]) {
        t->path[i]=duplicate(t->target[i]); if(!t->path[i]) { r=-2; goto done; }
    }
    /* Idle-only families are also parsed with their audited key before their
     * obsolete idle entries are evicted. The staged idle descriptors are never
     * published, and are released below in this same callback. */
    for(i=0;i<t->nres;i++) {
        t->res[i].fresh=prepare(t,&t->res[i],ctx);
        if(!t->res[i].fresh) { r=-2; goto done; }
    }
    /* Native factory leaves do not run UI callbacks. Still validate roots,
     * registry, barrier and ownership immediately before allocation-free
     * publication. Never retain prepared native objects over a UI callback. */
    r=roots(&mgr,&ctx);
    if(r) {
        /* Unexpected lifecycle change inside native leaves: old allocations
         * may already be gone. Leak the bounded staged set rather than walk
         * stale native ownership. The disabled latch prevents further work. */
        if(t->wrapper) rh_platform_free(t->wrapper);
        state.running=0; rh_platform_free(t); return disable();
    }
    if(registry(mgr,ctx)) { r=-1; goto done; }
    r=boundary(); if(r) goto done;
    r=revalidate(t,mgr,ctx); if(r) goto done;
    /* COMMIT. No allocation/callback until every registry/record/wrapper has
     * switched. +28 fallback, +32 user data, +36 record and list links stay. */
    for(i=0;i<t->nres;i++) if(!t->res[i].idle) {
        struct replacement *v=&t->res[i];
        wr(v->node,v->fresh+4);
        for(j=0;j<t->nwrap;j++) if(t->wrapper[j].record==v->node) {
            uint32_t k; for(k=0;k<28;k+=4) wr(t->wrapper[j].ptr+k,rd(v->fresh+4+k));
        }
    }
    for(i=0;i<state.count;i++) if(t->dirty[i]) {
        struct family *f=&state.family[i]; uint32_t old=f->path_ptr;
        wr(f->node+4,t->path[i]); f->path_ptr=t->path[i]; t->path[i]=0;
        copy(f->current,t->target[i]); f->current_hash=t->hash[i];
        if(!eq(f->stock,f->current)) copy(state.history[state.history_count++],f->current);
        release_mem(old); (*changed)++;
    }
    for(i=0;i<t->nres;i++) {
        struct replacement *v=&t->res[i];
        if(v->idle==1) {
            (void)call(0x0c3a46dcu,rd(mgr+556),v->node,0);
            release_mem(v->node);
        }
        if(v->idle) destroy_descriptor(v->fresh);
        if(v->dsc) destroy_descriptor(v->dsc);
        v->fresh=0; /* now live; cleanup must NEVER free it */
    }
    r=refresh(t);
    /* Publication cannot be rolled back after UI callbacks. A same-mapping
     * retry would otherwise be a no-op and falsely report owner completion.
     * Keep new live backing valid, but require a fresh module/UI lifetime. */
    if(r) rh_font_reload_disable();
done:
    /* Reverse descriptor acquisition order handles shared staged faces. */
    for(i=t->nres;i>0;i--) if(t->res[i-1].fresh) destroy_descriptor(t->res[i-1].fresh);
    for(i=0;i<state.count;i++) release_mem(t->path[i]);
    if(t->wrapper) rh_platform_free(t->wrapper);
    state.running=0; rh_platform_free(t); return r;
}
#endif

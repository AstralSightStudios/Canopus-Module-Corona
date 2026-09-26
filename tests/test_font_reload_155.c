/* Executable transaction tests, not native FT parsing or GPU simulation.
 * cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude \
 *    tests/test_font_reload_155.c src/resource_hook.c -o build/test_font_reload_155
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef RH_FONT_STUB_TEST
#include "../src/font_reload_155.c"
int main(void) {
    uint32_t changed=99;
    rh_font_reload_disable();
    assert(rh_font_reload(NULL,&changed)==0 && changed==0);
    assert(rh_font_reload(NULL,NULL)==0);
    puts("font reload default stub passed"); return 0;
}
#else
#define RH_TARGET_155 1
#define RH_EXPERIMENTAL_FONT_RELOAD 1
#define RH_FONT_RELOAD_TEST 1
#include "../src/font_reload_155.c"

#define MEM_BASE 0x30000000u
#define MEM_SIZE (4u*1024u*1024u)
static unsigned char memory[MEM_SIZE],globals[0x100000];
struct allocation { uint32_t p,size,live; };
static struct allocation allocations[8192];
static uint32_t bump,nalloc,alloc_calls,fail_alloc,fail_parse,fail_size,fail_cache_init,bitmap_only;
static uint32_t owner_list[8],owner_count,refresh_calls,vector_drops,mutate_owner,post_fail,post_overflow;
static uint32_t mgr,ctx,cache,reg,record,record2,wrapper,wrapper2,wrapper3,old_dsc,idle_node;
static uint32_t display,vg,glyph_queue,old_live,read_count,fail_temp;
static uint32_t temp_calls,fail_temp_at,temp_live;
static uint32_t stock_content,a_content,b_content,file_offset,lifecycle_in_prepare,busy_in_prepare;
static int open_file;
static uint32_t simulated_file_size, file_opens;
static const char stock[]="/system/fonts/Regular.ttf";
static const char path_a[]=RH_THEME_ROOT "g001/Regular.ttf";
static const char path_b[]=RH_THEME_ROOT "g002/Regular.ttf";
static unsigned char *address(uint32_t p,unsigned size) {
    if(p>=MEM_BASE && p-MEM_BASE<=MEM_SIZE-size) return memory+(p-MEM_BASE);
    assert(p>=0x20000000 && p-0x20000000<=sizeof(globals)-size);
    return globals+(p-0x20000000);
}
uint32_t rh_fr_read(uint32_t p,unsigned size) {
    unsigned char *b=address(p,size); uint32_t v=0; unsigned i;
    read_count++;
    for(i=0;i<size;i++) v|=(uint32_t)b[i]<<(i*8);
    return v;
}
void rh_fr_write(uint32_t p,uint32_t v,unsigned size) {
    unsigned char *b=address(p,size); unsigned i;
    for(i=0;i<size;i++) b[i]=(unsigned char)(v>>(i*8));
}
static uint32_t heap(uint32_t size) {
    uint32_t p;
    if(++alloc_calls==fail_alloc) return 0;
    assert(nalloc<8192 && bump+size+8<MEM_SIZE);
    p=MEM_BASE+bump; bump+=(size+7)&~7u;
    allocations[nalloc++]=(struct allocation){p,size,1};
    memset(address(p,size),0,size); return p;
}
static void free_heap(uint32_t p) {
    uint32_t i; if(!p) return;
    for(i=0;i<nalloc;i++) if(allocations[i].p==p) {
        assert(allocations[i].live); allocations[i].live=0;
        memset(address(p,allocations[i].size),0xed,allocations[i].size); return;
    }
    assert(!"foreign free");
}
static uint32_t live_allocations(void) { uint32_t i,n=0; for(i=0;i<nalloc;i++) n+=allocations[i].live; return n; }
static int alive(uint32_t p) { uint32_t i; for(i=0;i<nalloc;i++) if(allocations[i].p==p) return (int)allocations[i].live; return 0; }
static void append(uint32_t list_head,uint32_t p) {
    uint32_t size=rd(list_head),tail=rd(list_head+8);
    wr(p+size,tail); wr(p+size+4,0);
    if(tail) wr(tail+size+4,p); else wr(list_head+4,p);
    wr(list_head+8,p);
}
static void unlink_node(uint32_t head,uint32_t p) {
    uint32_t size=rd(head),prev=rd(p+size),next=rd(p+size+4);
    if(prev) wr(prev+size+4,next); else wr(head+4,next);
    if(next) wr(next+size,prev); else wr(head+8,prev);
}
static uint32_t key_word(uintptr_t key,uint32_t off) {
    if(key>=MEM_BASE && key<MEM_BASE+MEM_SIZE) return rd((uint32_t)key+off);
    return ((const uint32_t *)key)[off/4];
}
static uint32_t lookup(uint32_t c,uintptr_t key) {
    uint32_t p; char a[RH_PATH],b[RH_PATH];
    for(p=rd(c+52);p;p=rd(p+8)) {
        uint32_t data=rd(rd(p)+16);
        assert(string(rd(data),a)); assert(string(key_word(key,0),b));
        if(eq(a,b) && rd(data+4)==key_word(key,4)) return data+28;
    }
    return 0;
}
static uint32_t face_create(uint32_t c,uintptr_t key) {
    uint32_t data=0,tree=0,ll=0,ft=0,metrics=0;
    data=heap(48); if(!data) goto fail;
    tree=heap(20); if(!tree) goto fail;
    ll=heap(12); if(!ll) goto fail;
    if(fail_parse) goto fail;
    ft=heap(160); if(!ft) goto fail;
    metrics=heap(64); if(!metrics) goto fail;
    wr(data,key_word(key,0)); wr(data+4,key_word(key,4)); wr(data+12,ft);
    wr(ft+8,bitmap_only ? 0u : 1u); /* FT_FACE_FLAG_SCALABLE */
    wr(ft+88,metrics); wb(ft+80,2); wb(ft+82,1);
    wr(data+28,c); wr(data+32,1); wr(data+36,28);
    wr(tree+16,data); wr(ll,tree); append(c+48,ll); wr(data+44,ll);
    wr(c+12,rd(c+12)+1); return data+28;
fail:
    free_heap(metrics); free_heap(ft); free_heap(ll); free_heap(tree); free_heap(data); return 0;
}
static void drop_face(uint32_t c,uint32_t data) {
    uint32_t ll=rd(data+44),tree=rd(ll),ft=rd(data+12);
    assert(rd(data+32)==0);
    free_heap(rd(data+16)); free_heap(rd(data+20));
    free_heap(rd(ft+88)); free_heap(ft);
    unlink_node(c+48,ll); wr(c+12,rd(c+12)-1);
    free_heap(ll); free_heap(tree); free_heap(data);
}
uint32_t rh_fr_call(uint32_t pc,uintptr_t a,uintptr_t b,uintptr_t c) {
    uint32_t p=(uint32_t)a,q=(uint32_t)b;
    switch(pc) {
    case 0x0c3abe20: return heap(p);
    case 0x0c3abe58: free_heap(p); return 0;
    case 0x0c3a9e48:
        if(fail_cache_init && --fail_cache_init==0) return 0;
        assert(rd(p)==CNT_CLASS && rd(p+16) && rd(p+24));
        wr(p+40,rd(p+16)); wr(p+44,rd(p+4)+20); wr(p+48,4); return 1;
    case 0x0c3a3860:
        q=lookup(p,b); if(q) wr(q+4,rd(q+4)+1); return q;
    case 0x0c3a7b78: return face_create(p,b);
    case 0x0c8b9780: assert(rd(q+4)>0 && !rb(q+12)); wr(q+4,rd(q+4)-1); return 0;
    case 0x0c8b8c9e: drop_face(p,q); return 0;
    case 0x0c39a424:
        for(q=rd(p+8);q;q=rd(q+12)) if(rd(q)==(uint32_t)b) {
            assert(rd(q+4)); wr(q+4,rd(q+4)-1);
            if(!rd(q+4)) { unlink_node(p+4,q); free_heap(rd(q)); free_heap(q); }
            return 0;
        }
        assert(!"missing face id"); return 0;
    case 0x0c8b8a54:
        if(lifecycle_in_prepare) wr(CONTEXT_SLOT,0);
        if(busy_in_prepare) wr(glyph_queue+8,1);
        if(fail_size) return 1;
        wr(rd(p+88)+32,q*64); wr(rd(p+88)+28,(uint32_t)-128);
        wr(rd(p+88)+20,65536); return 0;
    case 0x0c424304: return (uint32_t)b;
    case 0x0c3a46dc: unlink_node(p,q); return 0;
    case 0x0c380574: {
        uint32_t i; int (*cb)(uint32_t,void *)=(int (*)(uint32_t,void *))b;
        if(post_overflow && refresh_calls) {
            /* A callback-expanded tree exhausts membership traversal before
             * reaching the next snapshot owner. No lifecycle hook fired. */
            for(i=0;i<=OBJECTS;i++) if(cb(owner_list[0],(void *)c)==2) break;
        } else for(i=0;i<owner_count;i++) if(cb(owner_list[i],(void *)c)==2) break;
        return 0;
    }
    case 0x0c6a1304: vector_drops++; return 0;
    case 0x0c38525c:
        assert(q==0x000f0000 && c==90); refresh_calls++;
        if(mutate_owner && owner_count>1) { free_heap(owner_list[1]); owner_count=1; }
        if(post_fail) rh_font_reload_disable();
        return 0;
    default: fprintf(stderr,"Unexpected native call %08x\n",pc); abort();
    }
}
void *rh_platform_alloc(uint32_t n) {
    void *p;
    temp_calls++;
    if(fail_temp || temp_calls==fail_temp_at) return NULL;
    p=malloc(n); if(p) temp_live++;
    return p;
}
void rh_platform_free(void *p) { assert(p && temp_live); temp_live--; free(p); }
int rh_platform_open(const char *p,int mode) {
    assert(mode==1); file_offset=0; file_opens++;
    if(eq(p,stock)) open_file=1;
    else if(eq(p,path_a)) open_file=2;
    else if(eq(p,path_b)) open_file=3;
    else return -1;
    return open_file;
}
int rh_platform_read(int fd,void *out,uint32_t n) {
    uint32_t i,v; unsigned char *bytes=out;
    assert(fd==open_file && n>=4);
    if(n>simulated_file_size-file_offset) n=simulated_file_size-file_offset;
    v=fd==1?stock_content:fd==2?a_content:b_content;
    for(i=0;i<n;i++) bytes[i]=(unsigned char)(v>>(8u*((file_offset+i)%4u)));
    file_offset+=n; return (int)n;
}
void rh_platform_close(int fd) { assert(fd==open_file); open_file=0; }
/* Unused by mapping resolver; keep resource_hook.c linked as-is. */
static void init_queue(uint32_t p,uint32_t size,uint32_t cb) {
    wr(p+16,size); wr(p+32,size); wr(p+36,cb);
}
static uint32_t fixture_dsc(uint32_t size,uint32_t face) {
    uint32_t d=heap(64);
    wr(d,MAGIC); wr(d+4,METRICS); wr(d+8,OUTLINE); wr(d+12,RELEASE);
    wr(d+16,size); wr(d+28,d); wr(d+40,size); wr(d+44,65536);
    wr(d+48,ctx); wr(d+52,face); wr(d+56,face+28); wr(d+60,rd(face));
    return d;
}
static uint32_t fixture_record(uint32_t d,uint32_t size,uint32_t refs) {
    uint32_t p=heap(56); wr(p,d+4); wr(p+4,p+12); wr(p+8,size); wr(p+44,refs);
    memcpy(address(p+12,8),"Regular",8); append(mgr,p); return p;
}
static uint32_t fixture_wrapper(uint32_t rec) {
    uint32_t p=heap(48),i,font=rd(rec);
    for(i=0;i<36;i+=4) wr(p+i,rd(font+i));
    wr(p+36,rec); append(mgr+12,p); return p;
}
static void setup(void) {
    uint32_t ui,path,id,face,key[7]={0},d2,di,idle,gradient,imgq,gradq,sw,obj;
    assert(temp_live==0); temp_calls=fail_temp_at=0;
    memset(&state,0,sizeof(state)); memset(memory,0,sizeof(memory)); memset(globals,0,sizeof(globals));
    memset(allocations,0,sizeof(allocations));
    bump=8; nalloc=alloc_calls=fail_alloc=fail_parse=fail_size=fail_cache_init=bitmap_only=0;
    owner_count=refresh_calls=vector_drops=mutate_owner=post_fail=post_overflow=fail_temp=lifecycle_in_prepare=busy_in_prepare=0;
    stock_content=11; a_content=22; b_content=33;
    simulated_file_size=4; file_opens=0;
    ui=heap(64); mgr=heap(560); ctx=heap(28);
    wr(UIKIT_SLOT,ui); wr(ui+28,mgr); wr(CONTEXT_SLOT,ctx);
    wr(mgr,48); wr(mgr+12,40); wr(mgr+24,8);
    wr(ctx,heap(4)); wr(ctx+4,8); wr(ctx+16,0x0c3981d1); wr(ctx+20,256);
    cache=new_cache(28,0x7fffffff,0x0c396785,0x0c3967b5,0x0c396b25); wr(ctx+24,cache);
    reg=heap(16); wr(reg,duplicate("Regular")); wr(reg+4,duplicate(stock)); append(mgr+24,reg);
    path=duplicate(stock); id=heap(16); wr(id,path); wr(id+4,3); append(ctx+4,id);
    key[0]=path; key[1]=65536; face=face_create(cache,(uintptr_t)key)-28; wr(face+32,3);
    wr(face+16,new_cache(32,512,0x0c39fd9b,0x0c3a5ef1,0x0c39fd95));
    wr(face+20,new_cache(8,256,0x0c39fdcd,0x0c3a8bb9,0x0c3a09f5));
    old_dsc=fixture_dsc(24,face); d2=fixture_dsc(30,face); di=fixture_dsc(20,face);
    record=fixture_record(old_dsc,24,2); record2=fixture_record(d2,30,1);
    wrapper=fixture_wrapper(record); wrapper2=fixture_wrapper(record); wrapper3=fixture_wrapper(record2);
    wr(wrapper+28,wrapper3); wr(wrapper+32,0x76543210);
    idle=heap(12); wr(idle,44); wr(mgr+556,idle);
    idle_node=heap(52); wr(idle_node,idle_node+8); memcpy(address(idle_node+8,8),"Regular",8);
    wr(idle_node+4,20); wr(idle_node+40,di+4); append(idle,idle_node);
    wr(0x200bd1f0,792); display=heap(800); append(0x200bd1f0,display);
    wb(0x200bd1ec,1); wb(0x200bd210,1);
    vg=heap(292); sw=heap(40); wr(vg,sw); wr(vg+16,0x0c3913ed); wr(sw+16,0x0c3948ad); wr(DRAW_SLOT,vg);
    glyph_queue=heap(44); init_queue(glyph_queue,24,0x0c399b63); wr(vg+48,glyph_queue);
    imgq=heap(44); init_queue(imgq,76,0x0c395be1); wr(vg+36,imgq);
    gradient=heap(12); gradq=heap(44); init_queue(gradq,4,0x0c395bd1); wr(gradient+8,gradq); wr(vg+40,gradient);
    obj=heap(128); wr(obj,VECTOR_CLASS); owner_list[owner_count++]=obj;
    obj=heap(128); wr(obj,0x2ca177e0); owner_list[owner_count++]=obj;
    old_live=live_allocations(); alloc_calls=0;
}
static int reload_path(const char *path,uint32_t *changed) {
    struct rh_rule rule; struct rh_mapping_view view={&rule,path?1u:0u};
    memset(&rule,0,sizeof(rule)); strcpy(rule.source,stock);
    if(path) strcpy(rule.destination,path);
    else view.rules=NULL;
    return rh_font_reload(&view,changed);
}
static void unchanged(void) {
    assert(rd(record)==old_dsc+4 && rd(wrapper+24)==old_dsc);
    assert(native_eq(rd(reg+4),stock)); assert(alive(old_dsc) && alive(idle_node));
    assert(live_allocations()==old_live);
}
static void committed(const char *path,uint32_t changed) {
    uint32_t d=rd(wrapper+24),d2=rd(wrapper3+24);
    assert(changed==1 && d!=old_dsc && alive(d));
    assert(rd(record)==d+4 && rd(record2)==d2+4 && rd(wrapper2+24)==d);
    assert(rd(wrapper+28)==wrapper3 && rd(wrapper+32)==0x76543210 && rd(wrapper+36)==record);
    assert(rd(record+44)==2 && rd(record2+44)==1);
    assert(rd(d+52)==rd(d2+52) && rd(rd(d+56)+4)==2);
    assert(!alive(old_dsc) && !alive(idle_node));
    assert(native_eq(rd(reg+4),path) && native_eq(rd(d+60),path));
}
static void test_switch_restore(void) {
    uint32_t changed;
    setup(); assert(!reload_path(path_a,&changed)); committed(path_a,changed);
    assert(refresh_calls==2 && vector_drops==1);
    assert(!reload_path(path_a,&changed) && changed==0);
    assert(!reload_path(path_b,&changed)); committed(path_b,changed);
    assert(!reload_path(NULL,&changed)); committed(stock,changed);
    assert(reload_path(path_a,&changed)<0 && changed==0); /* generation cannot be reused */
    assert(native_eq(rd(reg+4),stock));
}
static void test_large_stock_files(void) {
    uint32_t changed; struct digest h;
    setup(); simulated_file_size=11637064u;
    assert(reload_path(path_a,&changed)==0); committed(path_a,changed);
    assert(file_opens==2); /* stock=current read once; shared target once */
    setup(); simulated_file_size=FILE_LIMIT;
    assert(fingerprint(stock,&h)==0 && h.size==FILE_LIMIT);
    setup(); simulated_file_size=FILE_LIMIT+1u;
    assert(reload_path(path_a,&changed)==-2211 && !changed); unchanged();
    assert(!open_file);
}
static void expect_diagnostic(uint32_t address_to_change, uint32_t value, int expected) {
    uint32_t changed=99; int result;
    wr(address_to_change,value);
    result=reload_path(path_a,&changed);
    if(result!=expected) fprintf(stderr,"diagnostic expected %d, got %d at %08x\n",expected,result,address_to_change);
    assert(result==expected && changed==0);
    unchanged();
    assert(file_opens==0); /* These checks precede expensive file I/O. */
}
static void test_ownership_diagnostics(void) {
    setup(); expect_diagnostic(cache,0,-2302);
    setup(); expect_diagnostic(cache+4,32,-2303);
    setup(); expect_diagnostic(cache+48,8,-2304);
    setup(); expect_diagnostic(cache+16,0,-2305);
    setup(); expect_diagnostic(cache+8,0,-2701);
    setup(); expect_diagnostic(old_dsc+4,0,-2602);
    setup(); expect_diagnostic(old_dsc+8,0,-2603);
    setup(); expect_diagnostic(old_dsc+12,0,-2604);
    setup(); expect_diagnostic(old_dsc,0,-2606);
    setup(); expect_diagnostic(old_dsc+48,0,-2607);
    setup(); expect_diagnostic(old_dsc+28,0,-2605);
    setup(); expect_diagnostic(old_dsc+40,0,-2609);
    setup(); expect_diagnostic(old_dsc+44,0,-2610);
    setup(); expect_diagnostic(rd(old_dsc+52)+4,0,-2619);
    setup(); expect_diagnostic(rd(rd(old_dsc+52)+12)+8,0,-2621);
    setup(); expect_diagnostic(rd(rd(old_dsc+52)+16)+20,0,-2406);
    setup(); expect_diagnostic(rd(rd(old_dsc+52)+20)+24,0,-2507);
    setup(); expect_diagnostic(record+8,99,-2708);
    setup(); expect_diagnostic(wrapper+12,99,-2733);
    setup(); expect_diagnostic(record+44,3,-2712);
    setup(); expect_diagnostic(idle_node+4,99,-2717);
}
static void add_unrelated_records(uint32_t head, uint32_t size, uint32_t count) {
    uint32_t i;
    for(i=0;i<count;i++) {
        uint32_t p=heap(size+8),name=p+(size==48 ? 12u : 8u);
        memcpy(address(name,10),"Unrelated",10);
        wr(p+(size==48 ? 4u : 0u),name);
        /* Opaque non-target family payload is not part of this transaction. */
        append(head,p);
    }
    old_live=live_allocations();
}
static void test_resource_scan_limits(void) {
    uint32_t changed;
    setup(); add_unrelated_records(mgr,48,100);
    assert(reload_path(path_a,&changed)==0); committed(path_a,changed);
    setup(); add_unrelated_records(rd(mgr+556),44,100);
    assert(reload_path(path_a,&changed)==0); committed(path_a,changed);
    setup(); add_unrelated_records(mgr,48,BACKING_SCAN);
    assert(reload_path(path_a,&changed)==-2742 && !changed); unchanged();
    assert(!file_opens);
    setup(); expect_diagnostic(record+48,1,-2743);
    setup(); expect_diagnostic(mgr+8,0,-2744);
    setup(); expect_diagnostic(idle_node+44,1,-2753);
    setup(); expect_diagnostic(wrapper+40,1,-2763);
}
static void add_affected_records(uint32_t active_extra, uint32_t idle_extra) {
    uint32_t i,size=1,face=rd(old_dsc+52),id=rd(ctx+8);
    for(i=0;i<active_extra;i++) {
        uint32_t d,rec;
        while(size==20 || size==24 || size==30) size++;
        d=fixture_dsc(size,face); rec=fixture_record(d,size++,1);
        (void)fixture_wrapper(rec);
    }
    for(i=0;i<idle_extra;i++) {
        uint32_t p=heap(52),d=fixture_dsc(257+i,face);
        wr(p,p+8); memcpy(address(p+8,8),"Regular",8);
        wr(p+4,257+i); wr(p+40,d+4); append(rd(mgr+556),p);
    }
    wr(face+32,rd(face+32)+active_extra+idle_extra);
    wr(id+4,rd(id+4)+active_extra+idle_extra);
    old_live=live_allocations(); alloc_calls=0;
}
static void assert_batch_commit(const char *path, uint32_t changed, uint32_t active_count) {
    uint32_t nodes[WRAPPERS],n,i,d,face;
    assert(changed==1 && native_eq(rd(reg+4),path));
    assert(list(mgr,48,nodes,WRAPPERS,&n)==0 && n==active_count);
    for(i=0;i<n;i++) {
        d=rd(rd(nodes[i])+24);
        assert(alive(d) && native_eq(rd(d+60),path));
        assert(descriptor(d+4,ctx,path)==0);
    }
    d=rd(wrapper+24); face=rd(d+52);
    assert(rd(face+32)==active_count);
    assert(!rd(rd(mgr+556)+4)); /* All affected idle descriptors were evicted. */
    assert(list(mgr+12,40,nodes,WRAPPERS,&n)==0 && n==active_count+1);
    for(i=0;i<n;i++) assert(rd(nodes[i]+24)==rd(rd(rd(nodes[i]+36))+24));
    assert(rd(wrapper+28)==wrapper3 && rd(wrapper+32)==0x76543210);
}
static void test_large_affected_transaction(void) {
    uint32_t changed,calls,i,failures[3];
    /* 255 active backings, 256 live wrappers and 256 idle backings. */
    setup(); add_affected_records(253,255);
    assert(reload_path(path_a,&changed)==0); calls=alloc_calls;
    assert_batch_commit(path_a,changed,255);
    assert(!alive(old_dsc) && !alive(idle_node));
    assert(reload_path(NULL,&changed)==0); assert_batch_commit(stock,changed,255);
    failures[0]=1; failures[1]=calls/2; failures[2]=calls;
    for(i=0;i<3;i++) {
        uint32_t before_face,before_path;
        setup(); add_affected_records(253,255);
        before_face=rd(rd(old_dsc+52)+32); before_path=rd(rd(ctx+8)+4);
        fail_alloc=failures[i];
        assert(reload_path(path_a,&changed)<0 && !changed); unchanged();
        assert(rd(rd(old_dsc+52)+32)==before_face && rd(rd(ctx+8)+4)==before_path);
        fail_alloc=0;
        assert(reload_path(path_a,&changed)==0); assert_batch_commit(path_a,changed,255);
    }
    printf("511-backing transaction, restore and early/middle/late OOM rollback passed (%u allocations)\n",calls);
}
static uint32_t add_extra_wrappers(uint32_t count) {
    uint32_t i,p=0;
    for(i=0;i<count;i++) {
        p=fixture_wrapper(record);
        wr(p+28,wrapper3); wr(p+32,0x12345678u+i);
    }
    wr(record+44,rd(record+44)+count);
    old_live=live_allocations(); alloc_calls=0;
    return p;
}
static void assert_extra_wrappers(uint32_t extra, uint32_t last) {
    uint32_t p,n=0;
    assert(list(mgr+12,40,0,WRAPPERS,&n)==0 && n==extra+3);
    for(p=rd(mgr+16);p;p=rd(p+44))
        assert(rd(p+24)==rd(rd(rd(p+36))+24));
    assert(rd(record+44)==extra+2);
    assert(rd(last+28)==wrapper3 && rd(last+32)==0x12345678u+extra-1);
    assert(temp_live==0);
}
static void test_dynamic_wrappers(void) {
    uint32_t counts[]={300,1021,WRAPPERS-3},i,changed,last;
    for(i=0;i<3;i++) {
        setup(); last=add_extra_wrappers(counts[i]);
        assert(reload_path(path_a,&changed)==0 && changed==1);
        assert_extra_wrappers(counts[i],last);
        assert(reload_path(NULL,&changed)==0 && changed==1);
        assert_extra_wrappers(counts[i],last);
    }
    setup(); last=add_extra_wrappers(1000); fail_temp_at=2;
    assert(reload_path(path_a,&changed)==-2902 && !changed); unchanged();
    assert(temp_live==0 && !file_opens);
    fail_temp_at=0;
    assert(reload_path(path_a,&changed)==0 && changed==1);
    assert_extra_wrappers(1000,last);
    setup(); add_extra_wrappers(WRAPPERS-2);
    assert(reload_path(path_a,&changed)==-2762 && !changed); unchanged();
    assert(temp_live==0 && temp_calls==1 && !file_opens);
    puts("dynamic snapshots: 303/1024/4096 wrappers, restore, snapshot OOM and 4097 refusal passed");
}
static void test_wrapper_snapshot_membership(void) {
    struct transaction *t; uint32_t manager,context,p;
    setup(); assert(roots(&manager,&context)==0 && registry(manager,context)==0);
    t=calloc(1,sizeof(*t)); assert(t); t->dirty[0]=1; t->count=1;
    assert(resources(t,manager,context)==0 && t->nwrap==3);
    p=heap(48); append(mgr+12,p); /* Unrelated wrapper need not be snapshotted. */
    assert(revalidate(t,manager,context)==0);
    unlink_node(mgr+12,p); free_heap(p);
    unlink_node(mgr+12,wrapper2); free_heap(wrapper2);
    (void)fixture_wrapper(record); /* Same owner count, different identity. */
    assert(revalidate(t,manager,context)==-2766);
    rh_platform_free(t->wrapper); free(t); assert(temp_live==0);
}
static void test_busy(void) {
    uint32_t changed;
    setup(); wr(vg+32,123); assert(reload_path(path_a,&changed)==1 && !changed); unchanged();
    wr(vg+32,0); wr(glyph_queue+4,heap(24)); wr(glyph_queue+12,1); wr(glyph_queue+8,1);
    old_live=live_allocations(); assert(reload_path(path_a,&changed)==1); unchanged();
    wr(glyph_queue+8,0); wr(glyph_queue+20,heap(24)); wr(glyph_queue+28,1); wr(glyph_queue+24,1);
    old_live=live_allocations(); assert(reload_path(path_a,&changed)==1); unchanged();
    assert(file_opens==0); /* Busy polling must not repeatedly hash large files. */
    wr(glyph_queue+24,0); assert(!reload_path(path_a,&changed)); committed(path_a,changed);
    setup(); wr(glyph_queue+4,heap(24)); wr(glyph_queue+12,1); old_live=live_allocations();
    busy_in_prepare=1; assert(reload_path(path_a,&changed)==1 && !changed); unchanged();
    busy_in_prepare=0; wr(glyph_queue+8,0);
    assert(!reload_path(path_a,&changed)); committed(path_a,changed);
}
static void test_failures(void) {
    uint32_t changed,calls,i;
    setup(); assert(!reload_path(path_a,&changed)); calls=alloc_calls;
    for(i=1;i<=calls;i++) {
        setup(); fail_alloc=i;
        assert(reload_path(path_a,&changed)<0 && !changed); unchanged();
    }
    setup(); fail_temp=1; assert(reload_path(path_a,&changed)==-2901); unchanged();
    setup(); fail_parse=1; assert(reload_path(path_a,&changed)==-2910); unchanged();
    setup(); fail_size=1; assert(reload_path(path_a,&changed)==-2914); unchanged();
    setup(); bitmap_only=1; assert(reload_path(path_a,&changed)==-2913 && !changed); unchanged();
    setup(); fail_cache_init=1; assert(reload_path(path_a,&changed)==-2911); unchanged();
    setup(); fail_cache_init=2; assert(reload_path(path_a,&changed)==-2912); unchanged();
    printf("checked %u native allocation failure positions\n",calls);
}
static void test_paths_and_lifecycle(void) {
    uint32_t changed,reads;
    setup(); assert(!reload_path(path_a,&changed)); a_content++;
    assert(reload_path(path_a,&changed)<0 && !changed); committed(path_a,1);
    setup(); assert(!reload_path(path_a,&changed)); stock_content++;
    assert(reload_path(NULL,&changed)<0 && !changed); committed(path_a,1);
    setup(); assert(!reload_path(NULL,&changed)); wr(CONTEXT_SLOT,0);
    assert(reload_path(path_a,&changed)<0 && state.disabled);
    wr(CONTEXT_SLOT,ctx); reads=read_count;
    assert(reload_path(path_a,&changed)<0 && read_count==reads); unchanged();
    setup(); rh_font_reload_disable(); reads=read_count;
    assert(reload_path(path_a,&changed)<0 && read_count==reads); unchanged();
    setup(); assert(!reload_path(NULL,&changed)); wr(mgr+28,0); wr(mgr+32,0);
    assert(reload_path(path_a,&changed)<0 && state.disabled);
}
static void test_unknown_and_owners(void) {
    uint32_t changed;
    setup(); wr(vg+16,0x12345679); assert(reload_path(path_a,&changed)<0); unchanged();
    setup(); wr(old_dsc+8,0x0c3a7bd9); wr(wrapper+4,0x0c3a7bd9); wr(wrapper2+4,0x0c3a7bd9);
    assert(reload_path(path_a,&changed)<0); unchanged();
    setup(); wr(record+44,3); assert(reload_path(path_a,&changed)<0); unchanged();
    setup(); mutate_owner=1; assert(!reload_path(path_a,&changed)); committed(path_a,changed);
    assert(refresh_calls==1); /* deleted second snapshot owner is not dereferenced */
    setup(); post_fail=1; assert(reload_path(path_a,&changed)<0); committed(path_a,changed);
    assert(state.disabled && alive(rd(wrapper+24))); /* new backing stays live */
}
static void test_postcommit_overflow_latches(void) {
    uint32_t changed,reads;
    setup(); post_overflow=1;
    assert(reload_path(path_a,&changed)<0); committed(path_a,changed);
    assert(refresh_calls==1 && state.disabled && alive(rd(wrapper+24)));
    /* Even if traversal would now succeed, do not clear an incomplete owner
     * refresh by taking the no-dirty-path shortcut. */
    post_overflow=0; reads=read_count;
    assert(reload_path(path_a,&changed)<0 && !changed && read_count==reads);
    assert(alive(rd(wrapper+24)));
}
static uint32_t held_entry(uint32_t c,uint32_t size) {
    uint32_t data=heap(size+20),tree=heap(20),ll=heap(12);
    wr(tree+16,data); wr(ll,tree); append(c+48,ll); wr(c+12,1);
    wr(data+size,c); wr(data+size+4,1); wr(data+size+8,size); return data;
}
static void test_holds_limits_and_external_faces(void) {
    uint32_t changed,child,vec,data,path,key[7]={0},before; int error;
    setup(); child=rd(rd(old_dsc+52)+20); held_entry(child,8); old_live=live_allocations();
    assert(reload_path(path_a,&changed)==1); unchanged();
    setup(); vec=heap(64); wr(vec,SIZE_CLASS); wr(vec+4,16); wr(vec+48,4);
    wr(vec+16,0x0c69feb1); wr(vec+24,0x0c6a12d5); wr(VECTOR_SLOT,vec);
    data=held_entry(vec,16); old_live=live_allocations();
    assert(reload_path(path_a,&changed)==1); unchanged();
    wr(data+20,0); path=heap(128); wb(path+36,1); wr(data+8,heap(20)); wr(rd(data+8),path); wr(data+12,1);
    old_live=live_allocations(); assert(reload_path(path_a,&changed)<0); unchanged();
    setup(); wr(glyph_queue+12,4097); assert(reload_path(path_a,&changed)<0); unchanged();
    setup(); wr(wrapper3+44,wrapper); assert(reload_path(path_a,&changed)<0); unchanged();
    setup(); path=intern(ctx,path_a,&error); assert(path && !error); key[0]=path; key[1]=65536;
    data=face_create(cache,(uintptr_t)key); old_live=live_allocations(); before=rd(data+4);
    assert(reload_path(path_a,&changed)<0); unchanged(); assert(rd(data+4)==before);
    setup(); wr(rd(old_dsc+56)+4,0x7ffffffe); assert(reload_path(path_a,&changed)<0); unchanged();
    setup(); lifecycle_in_prepare=1; assert(reload_path(path_a,&changed)<0 && state.disabled);
    before=read_count; assert(reload_path(path_a,&changed)<0 && before==read_count);
}
static void close_active(void) {
    uint32_t d2=rd(wrapper3+24);
    free_heap(wrapper); free_heap(wrapper2); free_heap(wrapper3);
    free_heap(record); free_heap(record2);
    wr(mgr+4,0); wr(mgr+8,0); wr(mgr+16,0); wr(mgr+20,0);
    destroy_descriptor(old_dsc); destroy_descriptor(d2);
}
static void test_idle_only_and_unowned_family(void) {
    uint32_t changed,n;
    setup(); close_active(); n=live_allocations(); fail_parse=1;
    assert(reload_path(path_a,&changed)<0 && !changed);
    assert(native_eq(rd(reg+4),stock) && alive(idle_node) && live_allocations()==n);
    fail_parse=0; assert(!reload_path(path_a,&changed) && changed==1);
    assert(native_eq(rd(reg+4),path_a) && !alive(idle_node));
    assert(!rd(ctx+8) && !rd(cache+12)); /* no staged idle face left over */
    /* Scalar audited key permits stock restoration after all owners closed. */
    assert(!reload_path(NULL,&changed) && changed==1);
    assert(native_eq(rd(reg+4),stock) && !rd(ctx+8) && !rd(cache+12));
    setup(); close_active();
    destroy_descriptor(rd(rd(idle_node+40)+24));
    unlink_node(rd(mgr+556),idle_node); free_heap(idle_node);
    /* Never observed a descriptor in this instance: fail closed. */
    assert(reload_path(path_a,&changed)<0 && !changed);
}
static void test_unchanged_primary_with_changed_fallback(void) {
    uint32_t changed,other,primary,font,reg2;
    setup(); reg2=heap(16); wr(reg2,duplicate("Other")); wr(reg2+4,duplicate("/other/Other.ttf")); append(mgr+24,reg2);
    other=heap(56); font=heap(36); wr(font,0x12345679); wr(other,font); wr(other+4,other+12);
    memcpy(address(other+12,6),"Other",6); wr(other+44,1); append(mgr,other);
    primary=fixture_wrapper(other); wr(primary+28,wrapper);
    assert(!reload_path(path_a,&changed)); committed(path_a,changed);
    assert(rd(primary)==0x12345679 && rd(primary+28)==wrapper && rd(primary+36)==other);
}
static uint32_t alias_dsc,alias_record,alias_wrapper,alias_reg;
static void setup_alias(void) {
    uint32_t face,path_node;
    setup(); face=rd(old_dsc+52); path_node=rd(ctx+8);
    wr(face+32,rd(face+32)+1); wr(path_node+4,rd(path_node+4)+1);
    alias_dsc=fixture_dsc(26,face); alias_record=fixture_record(alias_dsc,26,1);
    memcpy(address(alias_record+12,6),"Alias",6); alias_wrapper=fixture_wrapper(alias_record);
    alias_reg=heap(16); wr(alias_reg,duplicate("Alias")); wr(alias_reg+4,duplicate(stock)); append(mgr+24,alias_reg);
    old_live=live_allocations(); alloc_calls=0;
}
static void test_intern_diagnostics(void) {
    uint32_t changed,i,node; char name[RH_PATH]; int error;
    setup(); wr(ctx+4,12);
    assert(intern(ctx,path_a,&error)==0 && error==-2921); unchanged();
    setup(); wr(rd(ctx+8)+8,123);
    assert(reload_path(path_a,&changed)==-2923 && !changed); unchanged();
    setup(); wr(ctx+12,0);
    assert(reload_path(path_a,&changed)==-2924 && !changed); unchanged();
    setup(); wr(rd(ctx+8)+4,0);
    assert(intern(ctx,stock,&error)==0 && error==-2931); unchanged();
    setup(); fail_alloc=1;
    assert(intern(ctx,path_a,&error)==0 && error==-2933); unchanged();
    setup(); fail_alloc=2;
    assert(intern(ctx,path_a,&error)==0 && error==-2934); unchanged();
    setup();
    for(i=1;i<INTERN_IDS;i++) {
        snprintf(name,sizeof(name),"/other/font-%u.ttf",i);
        assert(intern(ctx,name,&error)!=0 && !error);
    }
    old_live=live_allocations();
    assert(reload_path(path_a,&changed)==-2932 && !changed); unchanged();
    /* Existing IDs remain usable at capacity; scanning beyond it is distinct. */
    assert(intern(ctx,stock,&error)!=0 && !error);
    node=heap(16); wr(node,duplicate("/other/overflow.ttf")); wr(node+4,1); append(ctx+4,node);
    old_live=live_allocations();
    assert(reload_path(path_a,&changed)==-2922 && !changed); unchanged();
}
static void test_restore_failures(void) {
    uint32_t changed,calls,start,i,before,current,face,refs,history;
    setup(); assert(reload_path(path_a,&changed)==0);
    start=alloc_calls; assert(reload_path(NULL,&changed)==0); calls=alloc_calls-start;
    for(i=1;i<=calls;i++) {
        setup(); assert(reload_path(path_a,&changed)==0);
        before=live_allocations(); current=rd(wrapper+24); face=rd(current+52);
        refs=rd(face+32); history=state.history_count; fail_alloc=alloc_calls+i;
        assert(reload_path(NULL,&changed)<0 && !changed);
        assert(live_allocations()==before && rd(wrapper+24)==current);
        assert(rd(face+32)==refs && state.history_count==history && !temp_live);
        committed(path_a,1);
        fail_alloc=0; assert(reload_path(NULL,&changed)==0); committed(stock,changed);
    }
    printf("stock restoration: %u allocation failure positions preserve the replacement and allow retry\n",calls);
}
static void test_restore_with_retained_stock_face(void) {
    uint32_t changed,face,entry,path,id,before,current;
    setup();
    /* Another consumer owns the original face independently of manager
     * records. Switching away must leave that consumer alive. */
    face=rd(old_dsc+52); entry=face+28; id=rd(ctx+8); path=rd(id);
    wr(entry+4,rd(entry+4)+1); wr(id+4,rd(id+4)+1);
    assert(reload_path(path_a,&changed)==0); committed(path_a,changed);
    assert(alive(face) && rd(entry+4)==1 && rd(id+4)==1);
    before=live_allocations(); current=rd(wrapper+24);
    /* This is an ownership refusal, not an allocation failure. */
    assert(reload_path(NULL,&changed)==-2908 && !changed);
    assert(live_allocations()==before && rd(wrapper+24)==current);
    assert(rd(entry+4)==1 && rd(id+4)==1 && alive(face));
    committed(path_a,1); assert(!temp_live);
    /* Only the original consumer may relinquish its ownership. */
    (void)call(0x0c8b9780u,cache,entry,0);
    (void)call(0x0c8b8c9eu,cache,face,0);
    (void)call(0x0c39a424u,ctx,path,0);
    assert(reload_path(NULL,&changed)==0); committed(stock,changed);
}
static void test_multi_family_atomicity(void) {
    uint32_t changed,calls,i;
    setup_alias(); assert(!reload_path(path_a,&changed) && changed==2); calls=alloc_calls;
    assert(rd(alias_record)==rd(alias_wrapper+24)+4 && !alive(alias_dsc));
    assert(rd(rd(alias_wrapper+24)+52)==rd(rd(wrapper+24)+52));
    assert(rd(rd(rd(wrapper+24)+56)+4)==3);
    assert(!reload_path(path_b,&changed) && changed==2);
    assert(!reload_path(NULL,&changed) && changed==2);
    for(i=1;i<=calls;i++) {
        setup_alias(); fail_alloc=i;
        assert(reload_path(path_a,&changed)<0 && !changed); unchanged();
        assert(rd(alias_wrapper+24)==alias_dsc && rd(alias_record)==alias_dsc+4);
        assert(native_eq(rd(alias_reg+4),stock));
    }
    printf("checked %u two-family allocation failure positions\n",calls);
}
int main(void) {
    test_wrapper_snapshot_membership(); test_dynamic_wrappers(); test_large_affected_transaction(); test_resource_scan_limits(); test_ownership_diagnostics(); test_large_stock_files(); test_switch_restore(); test_busy(); test_failures(); test_paths_and_lifecycle(); test_unknown_and_owners();
    test_postcommit_overflow_latches();
    test_holds_limits_and_external_faces(); test_idle_only_and_unowned_family();
    test_unchanged_primary_with_changed_fallback(); test_multi_family_atomicity();
    test_intern_diagnostics(); test_restore_failures(); test_restore_with_retained_stock_face();
    puts("font reload .155 transaction tests passed (modeled native leaves)"); return 0;
}
#endif

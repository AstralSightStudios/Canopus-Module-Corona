#include "manager_data.h"
#include "canopus_memory.h"
#include "resource_hook_target.h"

/* One shared catalog. Manifests are loaded one at a time; full file inventories
 * and page copies never survive a package scan. All growth is checked and an
 * exhausted Supervisor budget is reported instead of hiding installed items. */
struct text { char *p; uint32_t n, cap; };
struct json { const char *p, *e; };
struct mapping { char *source, *destination; };
struct pack {
    char *document;
    struct rh_manager_metadata meta;
    struct mapping *maps;
    uint32_t count;
    uint8_t backgrounds[4]; /* absent, system, blur, image */
    char *images[4];
    struct json calendar;
};
struct registration { uint32_t theme; char *destination; };
struct path_item { char *source; struct registration *options; uint32_t count; };
struct override { char *key, *choice; };
union pool_block { struct { uint32_t bytes, used; } value; uint64_t alignment; };
struct pool_chunk { struct pool_chunk *next; uint32_t bytes; uint64_t alignment; };
struct pool { struct pool_chunk *chunks; };
struct rh_manager_data {
    const struct canopus_context_v1 *ctx;
    struct pool *pool;
    struct rh_manager_data_hooks hooks;
    char root[256];
    char **ids, **order;
    uint32_t count, order_count;
    struct path_item *paths;
    uint32_t path_count;
    struct override *overrides;
    uint32_t override_count;
    char *features[5];
    char *pending_protection;
    char pending_overlay[33];
    char pending_config[32];
    uint8_t catalog_ready, busy;
    const char *error;
};
static const char *const feature_ids[] = {"launcher", "controlCenter", "notifications", "widgets", "calendar"};
static const char *const feature_labels[] = {"启动器背景", "控制中心背景", "通知中心背景", "快捷组件背景", "日历样式"};
static const char *const background_keys[] = {"launcher", "control_center", "notifications", "widgets"};
static uint32_t slen(const char *s) { uint32_t n=0; if(s) while(s[n]) n++; return n; }
static int scmp(const char *a,const char *b) { while(*a && *a==*b) { a++; b++; } return (unsigned char)*a-(unsigned char)*b; }
static int equal(const char *a,const char *b) { return scmp(a,b)==0; }
static int prefix(const char *s,const char *p) { while(*p) if(*s++!=*p++) return 0; return 1; }
static void zero(void *p,uint32_t n) { uint8_t *b=p; while(n--) *b++=0; }
static void copy(void *d,const void *s,uint32_t n) { canopus_memcpy(d,s,n); }
static int put(char *out,uint32_t cap,const char *s) { uint32_t n=slen(s); if(n>=cap) return -22; copy(out,s,n+1u); return 0; }
/* Supervisor limits service allocations to 128 blocks. Small catalog strings
 * therefore live in coalescing 16 KiB chunks, not one service block per path.
 * Chunks contain no page-specific copies and empty chunks return immediately. */
static void *alloc(struct rh_manager_data *d,uint32_t n) {
    struct pool_chunk *chunk; union pool_block *b; uint32_t aligned,required;
    if(!d->pool || n>UINT32_MAX-15u) return 0;
    aligned=(n+7u)&~7u; if(!aligned) aligned=8u;
    for(chunk=d->pool->chunks;chunk;chunk=chunk->next) {
        uint8_t *end=(uint8_t *)(chunk+1)+chunk->bytes;
        for(b=(union pool_block *)(chunk+1);(uint8_t *)b<end;b=(union pool_block *)((uint8_t *)(b+1)+b->value.bytes)) {
            if(!b->value.used && b->value.bytes>=aligned) goto found;
        }
    }
    required=aligned+(uint32_t)sizeof(*b);
    if(required<16384u) required=16384u;
    if(required>UINT32_MAX-sizeof(*chunk)) return 0;
    chunk=d->ctx->mem->alloc(d->ctx,required+(uint32_t)sizeof(*chunk)); if(!chunk) return 0;
    chunk->next=d->pool->chunks; chunk->bytes=required; d->pool->chunks=chunk;
    b=(union pool_block *)(chunk+1); b->value.bytes=required-(uint32_t)sizeof(*b); b->value.used=0;
found:
    if(b->value.bytes-aligned>=sizeof(*b)+8u) {
        union pool_block *next=(union pool_block *)((uint8_t *)(b+1)+aligned);
        next->value.bytes=b->value.bytes-aligned-(uint32_t)sizeof(*b); next->value.used=0; b->value.bytes=aligned;
    }
    b->value.used=1; return b+1;
}
static void release(struct rh_manager_data *d,void *p) {
    struct pool_chunk **link; union pool_block *b,*previous;
    if(!p) return;
    for(link=&d->pool->chunks;*link;link=&(*link)->next) {
        struct pool_chunk *chunk=*link; uint8_t *end=(uint8_t *)(chunk+1)+chunk->bytes;
        if((uintptr_t)p>(uintptr_t)chunk && (uintptr_t)p<(uintptr_t)end) {
            b=(union pool_block *)p-1; b->value.used=0;
            if((uint8_t *)(b+1)+b->value.bytes<end) {
                union pool_block *next=(union pool_block *)((uint8_t *)(b+1)+b->value.bytes);
                if(!next->value.used) b->value.bytes+=(uint32_t)sizeof(*b)+next->value.bytes;
            }
            previous=(union pool_block *)(chunk+1);
            if(previous!=b) {
                while((uint8_t *)(previous+1)+previous->value.bytes<(uint8_t *)b) previous=(union pool_block *)((uint8_t *)(previous+1)+previous->value.bytes);
                if(!previous->value.used) { previous->value.bytes+=(uint32_t)sizeof(*b)+b->value.bytes; b=previous; }
            }
            if(b==(union pool_block *)(chunk+1) && b->value.bytes+sizeof(*b)==chunk->bytes) {
                *link=chunk->next; d->ctx->mem->free(d->ctx,chunk);
            }
            return;
        }
    }
}
static char *dup(struct rh_manager_data *d,const char *s) { uint32_t n=slen(s)+1u; char *p=alloc(d,n); if(p) copy(p,s,n); return p; }
static int fail(struct rh_manager_data *d,int rc,const char *message) { d->error=message; return rc; }
static int grow(struct rh_manager_data *d,void **ptr,uint32_t old,uint32_t count,uint32_t size) {
    void *p; if(count>UINT32_MAX/size) return -12;
    p=alloc(d,count*size); if(!p) return -12;
    if(old) copy(p,*ptr,old*size); release(d,*ptr); *ptr=p; return 0;
}
static int append_n(struct rh_manager_data *d,struct text *t,const char *s,uint32_t n) {
    uint32_t cap; int rc;
    if(n>UINT32_MAX-t->n-1u) return -12;
    if(t->n+n+1u>t->cap) {
        cap=t->cap ? t->cap:128u;
        while(cap<t->n+n+1u) { if(cap>UINT32_MAX/2u) { cap=t->n+n+1u; break; } cap*=2u; }
        rc=grow(d,(void **)&t->p,t->n,cap,1u); if(rc) return rc; t->cap=cap;
    }
    copy(t->p+t->n,s,n); t->n+=n; t->p[t->n]=0; return 0;
}
static int append(struct rh_manager_data *d,struct text *t,const char *s) { return append_n(d,t,s,slen(s)); }
static int number(struct rh_manager_data *d,struct text *t,int32_t v) {
    char tmp[12],rev[10]; uint32_t n=0,k=0,u=v<0 ? 0u-(uint32_t)v:(uint32_t)v;
    if(v<0) tmp[n++]='-'; do { rev[k++]=(char)('0'+u%10u); u/=10u; } while(u);
    while(k) tmp[n++]=rev[--k]; return append_n(d,t,tmp,n);
}
static int quoted(struct rh_manager_data *d,struct text *t,const char *s) {
    int rc=append(d,t,"\""); char escaped[6]={'\\','u','0','0',0,0};
    while(!rc && *s) {
        uint8_t c=(uint8_t)*s++;
        if(c=='"' || c=='\\') { char a[2]={'\\',(char)c}; rc=append_n(d,t,a,2); }
        else if(c<32u) { escaped[4]="0123456789abcdef"[c>>4]; escaped[5]="0123456789abcdef"[c&15]; rc=append_n(d,t,escaped,6); }
        else { char a=(char)c; rc=append_n(d,t,&a,1); }
    }
    return rc ? rc:append(d,t,"\"");
}
static int path_join(char out[256],const char *a,const char *b) {
    uint32_t n=slen(a),m=slen(b); if(n+m>=256u) return -22; copy(out,a,n); copy(out+n,b,m+1u); return 0;
}
static int valid_id(const char *s) {
    uint32_t n=0; while(s[n]) { char c=s[n++]; if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-')) return 0; }
    return n>0u && n<=64u;
}
static int relative(const char *s) {
    uint32_t i=0,start=0,n=slen(s); if(!n || s[0]=='/' || prefix(s,"@quickapp-icon/")) return 0;
    for(i=0;i<=n;i++) {
        unsigned char c=(unsigned char)s[i];
        if(c=='\\'||c==':'||(c && (c<32u || c==127u))) return 0;
        if(c=='/' || !c) {
            uint32_t k=i-start;
            if((!k && i!=n)||(k==1u && s[start]=='.')||(k==2u && s[start]=='.' && s[start+1u]=='.')) return 0;
            start=i+1u;
        }
    }
    return 1;
}
static int source_valid(const char *s) {
    uint32_t n=slen(s),i;
    if(!n || n>255u) return 0;
    if(prefix(s,"@quickapp-icon/")) { for(i=0;i<n;i++) if((unsigned char)s[i]<32u || (unsigned char)s[i]==127u) return 0; return 1; }
    return s[0]=='/' && (s[1]==0 || relative(s+1));
}
static int directory_source(const char *s) { uint32_t n=slen(s); return n && !prefix(s,"@quickapp-icon/") && s[n-1u]=='/'; }
static int is_bin(const char *s) { uint32_t n=slen(s); return n>=4u && equal(s+n-4u,".bin"); }
static int reserved(const char *s) { return equal(s,"corona.json")||equal(s,"canora.json")||prefix(s,"corona.json/")||prefix(s,"canora.json/"); }
static int destination_valid(struct rh_manager_data *d,const char *id,const char *s) {
    return relative(s) && !reserved(s) && slen(d->root)+7u+slen(id)+1u+slen(s)<=255u;
}

/* A span parser avoids a DOM/token allocation proportional to manifest fields.
 * Schema readers only retain the strings they consume. The explicit depth
 * guard makes malformed input fail before exhausting an embedded C stack. */
static const char *ws(const char *p,const char *e) { while(p<e && (*p==' '||*p=='\t'||*p=='\r'||*p=='\n')) p++; return p; }
static int hex(char c) { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; }
static int utf8(const char **p,const char *e,uint32_t *v) {
    uint32_t code,min,k; unsigned char c;
    if(*p>=e) return -22; c=(unsigned char)*(*p)++;
    if(c<128u) { *v=c; return 0; }
    if(c>=194u && c<=223u) { k=1; code=c&31u; min=128u; }
    else if(c>=224u && c<=239u) { k=2; code=c&15u; min=2048u; }
    else if(c>=240u && c<=244u) { k=3; code=c&7u; min=65536u; }
    else return -22;
    while(k--) { if(*p>=e) return -22; c=(unsigned char)*(*p)++; if((c&192u)!=128u) return -22; code=(code<<6)|(c&63u); }
    if(code<min || code>0x10ffffu || (code>=0xd800u && code<=0xdfffu)) return -22;
    *v=code; return 0;
}
static int unicode4(const char **p,const char *e,uint32_t *v) {
    uint32_t i,c=0; int h; for(i=0;i<4;i++) { if(*p>=e || (h=hex(*(*p)++))<0) return -22; c=c*16u+(uint32_t)h; } *v=c; return 0;
}
static int string_code(const char **p,const char *e,uint32_t *v) {
    uint32_t low; char c;
    if(*p>=e) return -22;
    if(**p!='\\') { int rc=utf8(p,e,v); return rc ? rc:(*v<32u ? -22:0); }
    (*p)++; if(*p>=e) return -22; c=*(*p)++;
    if(c=='"'||c=='/'||c=='\\') { *v=(uint8_t)c; return 0; }
    if(c=='b') *v=8; else if(c=='f') *v=12; else if(c=='n') *v=10; else if(c=='r') *v=13; else if(c=='t') *v=9;
    else if(c=='u') {
        if(unicode4(p,e,v)) return -22;
        if(*v>=0xd800u && *v<=0xdbffu) {
            if(e-*p<2 || (*p)[0]!='\\' || (*p)[1]!='u') return -22; *p+=2;
            if(unicode4(p,e,&low)||low<0xdc00u||low>0xdfffu) return -22;
            *v=0x10000u+((*v-0xd800u)<<10)+(low-0xdc00u);
        } else if(*v>=0xdc00u && *v<=0xdfffu) return -22;
    } else return -22;
    return 0;
}
static int scan_string(const char **p,const char *e) {
    uint32_t c; if(*p>=e || *(*p)++!='"') return -22;
    while(*p<e && **p!='"') if(string_code(p,e,&c)) return -22;
    if(*p>=e) return -22; (*p)++; return 0;
}
static int scan_value(const char **p,const char *e,uint32_t depth) {
    char close; const char *start; uint32_t n;
    *p=ws(*p,e); if(*p>=e || depth>64u) return -22;
    if(**p=='"') return scan_string(p,e);
    if(**p=='{' || **p=='[') {
        close=**p=='{' ? '}':']'; (*p)++; *p=ws(*p,e);
        if(*p<e && **p==close) { (*p)++; return 0; }
        for(;;) {
            if(close=='}') { if(scan_string(p,e)) return -22; *p=ws(*p,e); if(*p>=e || *(*p)++!=':') return -22; }
            if(scan_value(p,e,depth+1u)) return -22; *p=ws(*p,e);
            if(*p>=e) return -22; if(**p==close) { (*p)++; return 0; }
            if(*(*p)++!=',') return -22; *p=ws(*p,e);
        }
    }
    start=*p;
    if(**p=='-' || (**p>='0'&&**p<='9')) {
        if(**p=='-') (*p)++; if(*p>=e) return -22;
        if(**p=='0') (*p)++;
        else { if(**p<'1'||**p>'9') return -22; do { (*p)++; } while(*p<e && **p>='0' && **p<='9'); }
        if(*p<e && **p=='.') { (*p)++; start=*p; while(*p<e && **p>='0'&&**p<='9') (*p)++; if(start==*p) return -22; }
        if(*p<e && (**p=='e'||**p=='E')) { (*p)++; if(*p<e && (**p=='+'||**p=='-')) (*p)++; start=*p; while(*p<e && **p>='0'&&**p<='9') (*p)++; if(start==*p) return -22; }
        return 0;
    }
    n=(uint32_t)(e-*p);
    if(n>=4u && (prefix(*p,"true")||prefix(*p,"null"))) { *p+=4; return 0; }
    if(n>=5u && prefix(*p,"false")) { *p+=5; return 0; }
    return -22;
}
static int parse(char *text,uint32_t n,struct json *j) {
    const char *p=ws(text,text+n),*end=text+n,*start=p;
    if(scan_value(&p,end,0)||ws(p,end)!=end) return -22;
    j->p=start; j->e=p; return 0;
}
static int decoded(struct json j,char *out,uint32_t cap,uint32_t *length) {
    const char *p=j.p; uint32_t code,n=0,i,k; char bytes[4];
    if(!p || p>=j.e || *p++!='"') return -22;
    while(p<j.e && *p!='"') {
        if(string_code(&p,j.e,&code) || !code) return -22; /* native strings cannot represent embedded NUL */
        if(code<128u) { bytes[0]=(char)code; k=1; }
        else if(code<2048u) { bytes[0]=(char)(192u|(code>>6)); bytes[1]=(char)(128u|(code&63u)); k=2; }
        else if(code<65536u) { bytes[0]=(char)(224u|(code>>12)); bytes[1]=(char)(128u|((code>>6)&63u)); bytes[2]=(char)(128u|(code&63u)); k=3; }
        else { bytes[0]=(char)(240u|(code>>18)); bytes[1]=(char)(128u|((code>>12)&63u)); bytes[2]=(char)(128u|((code>>6)&63u)); bytes[3]=(char)(128u|(code&63u)); k=4; }
        for(i=0;i<k;i++) { if(out && n+1u<cap) out[n]=bytes[i]; n++; }
    }
    if(p>=j.e || *p!='"') return -22;
    if(length) *length=n;
    if(out) { if(n>=cap) return -22; out[n]=0; }
    return 0;
}
static int jeq(struct json j,const char *s) {
    char buf[128]; return slen(s)<sizeof(buf) && !decoded(j,buf,sizeof(buf),0) && equal(buf,s);
}
static struct json field(struct json j,const char *name) {
    const char *p; struct json found={0,0},key,v;
    if(!j.p || *j.p!='{') return found; p=ws(j.p+1,j.e);
    while(p<j.e && *p!='}') {
        key.p=p; if(scan_string(&p,j.e)) return (struct json){0,0}; key.e=p;
        p=ws(p,j.e); if(p>=j.e || *p++!=':') return (struct json){0,0}; p=ws(p,j.e); v.p=p;
        if(scan_value(&p,j.e,0)) return (struct json){0,0}; v.e=p;
        if(jeq(key,name)) found=v; p=ws(p,j.e); if(p<j.e && *p==',') p=ws(p+1,j.e); else break;
    }
    return found;
}
static int integer(struct json j,int64_t *out) {
    const char *p=j.p,*mantissa,*mantissa_end; uint64_t v=0;
    uint32_t significant=0,trailing=0,fractional=0,seen=0,index=0;
    int neg=0,after_decimal=0,exponent_negative=0; int32_t exponent=0,scale;
    if(!p || p>=j.e) return -22; if(*p=='-') { neg=1; p++; }
    mantissa=p;
    while(p<j.e && *p!='e' && *p!='E') {
        if(*p=='.') { if(after_decimal) return -22; after_decimal=1; p++; continue; }
        if(*p<'0'||*p>'9') return -22;
        if(after_decimal) fractional++;
        if(*p!='0' || seen) { seen=1; significant++; if(*p=='0') trailing++; else trailing=0; }
        p++;
    }
    mantissa_end=p;
    if(p<j.e) {
        p++; if(p<j.e && (*p=='-'||*p=='+')) exponent_negative=*p++=='-';
        if(p>=j.e) return -22;
        while(p<j.e) { if(*p<'0'||*p>'9') return -22; if(exponent<1000000) exponent=exponent*10+(*p-'0'); p++; }
    }
    if(!seen) { *out=0; return 0; }
    if(exponent_negative) exponent=-exponent;
    significant-=trailing;
    if(significant>16u) return -22;
    scale=exponent-(int32_t)fractional+(int32_t)trailing;
    if(scale<0 || scale>16) return -22;
    seen=0;
    for(p=mantissa;p<mantissa_end;p++) {
        if(*p=='.') continue;
        if(*p!='0' || seen) { seen=1; if(index++<significant) v=v*10u+(uint8_t)(*p-'0'); }
    }
    while(scale--) { if(v>9007199254740991ULL/10u) return -22; v*=10u; }
    if(v>9007199254740991ULL) return -22;
    *out=neg ? -(int64_t)v:(int64_t)v; return 0;
}
static int int_field(struct json j,const char *key,int32_t fallback,int32_t *out) {
    int64_t v; struct json f=field(j,key); if(!f.p) { *out=fallback; return 0; }
    if(integer(f,&v)||v<INT32_MIN||v>INT32_MAX) return -22; *out=(int32_t)v; return 0;
}
static int next_array(struct json a,const char **cursor,struct json *out) {
    const char *p=*cursor;
    if(!a.p || *a.p!='[') return -22;
    if(!p) p=ws(a.p+1,a.e); else { p=ws(p,a.e); if(p<a.e && *p==',') p=ws(p+1,a.e); }
    if(p>=a.e) return -22; if(*p==']') return 0;
    out->p=p; if(scan_value(&p,a.e,0)) return -22; out->e=p; *cursor=p; return 1;
}
static int json_text(struct rh_manager_data *d,struct json j,char **out,uint32_t max) {
    uint32_t n; char *p; if(decoded(j,0,0,&n)||n>max) return -22;
    p=alloc(d,n+1u); if(!p) return -12; if(decoded(j,p,n+1u,0)) { release(d,p); return -22; } *out=p; return 0;
}
static int read_text(struct rh_manager_data *d,const char *path,uint32_t limit,char **out,struct json *j) {
    int fd,rc,got; int64_t size; uint32_t n=0; char *p;
    *out=0; fd=d->ctx->fs->open(d->ctx,path,CANOPUS_FS_READ); if(fd<0) return fd;
    size=d->ctx->fs->size(d->ctx,fd);
    if(size<0 || (uint64_t)size>limit || (uint64_t)size>=UINT32_MAX) { rc=size<0 ? (int)size:RH_MANAGER_ERR_LIMIT; goto done; }
    p=alloc(d,(uint32_t)size+1u); if(!p) { rc=-12; goto done; }
    while(n<(uint32_t)size) { got=d->ctx->fs->read(d->ctx,fd,p+n,(uint32_t)size-n); if(got<=0 || (uint32_t)got>(uint32_t)size-n) { release(d,p); rc=got<0 ? got:-5; goto done; } n+=(uint32_t)got; }
    p[n]=0;
    if(slen(p)!=n || (j && parse(p,n,j))) { release(d,p); rc=-22; goto done; } *out=p; rc=0;
done:
    got=d->ctx->fs->close(d->ctx,fd);
    if(!rc && got<0) {
        release(d,*out); *out=0;
        if(j) *j=(struct json){0,0};
        rc=got;
    }
    return rc;
}
static int write_text(struct rh_manager_data *d,const char *path,const char *data,uint32_t n) {
    uint32_t used=0; int fd,got,rc=0;
    fd=d->ctx->fs->open(d->ctx,path,CANOPUS_FS_WRITE); if(fd<0) return fd;
    while(used<n) { got=d->ctx->fs->write(d->ctx,fd,data+used,n-used); if(got<=0 || (uint32_t)got>n-used) { rc=got<0 ? got:-5; break; } used+=(uint32_t)got; }
    if(!rc && d->ctx->fs->sync) { got=d->ctx->fs->sync(d->ctx,fd); if(got<0 && got!=CANOPUS_SVC_ERR_UNSUPPORTED) rc=got; }
    got=d->ctx->fs->close(d->ctx,fd); return !rc && got<0 ? got:rc;
}
static int atomic_text(struct rh_manager_data *d,const char *path,const char *text,uint32_t n) {
    char temporary[256]; int rc;
    if(path_join(temporary,path,".manager-tmp")) return -22;
    rc=write_text(d,temporary,text,n); if(!rc) rc=d->ctx->fs->rename(d->ctx,temporary,path);
    if(rc) (void)d->ctx->fs->remove(d->ctx,temporary); return rc;
}
static int find_id(char **ids,uint32_t n,const char *id) { uint32_t i; for(i=0;i<n;i++) if(equal(ids[i],id)) return (int)i; return -1; }
static void free_strings(struct rh_manager_data *d,char **ids,uint32_t n) { while(n) release(d,ids[--n]); release(d,ids); }
static int string_list(struct rh_manager_data *d,struct json j,char ***out,uint32_t *count,int system) {
    char **ids=0,*s=0; uint32_t n=0; const char *p=0; struct json v; int rc;
    while((rc=next_array(j,&p,&v))>0) {
        rc=json_text(d,v,&s,64); if(rc) break;
        if((!valid_id(s) && !(system && equal(s,RH_MANAGER_SYSTEM))) || find_id(ids,n,s)>=0) { rc=-22; break; }
        rc=grow(d,(void **)&ids,n,n+1u,sizeof(*ids)); if(rc) break; ids[n++]=s; s=0;
    }
    release(d,s); if(rc<0) { free_strings(d,ids,n); return rc; } *out=ids; *count=n; return 0;
}
static int serialize_ids(struct rh_manager_data *d,struct text *t,char **ids,uint32_t n) {
    uint32_t i; int rc=append(d,t,"["); for(i=0;!rc && i<n;i++) { if(i) rc=append(d,t,","); if(!rc) rc=quoted(d,t,ids[i]); } return rc ? rc:append(d,t,"]");
}
static void free_pack(struct rh_manager_data *d,struct pack *p) {
    uint32_t i; for(i=0;i<p->count;i++) { release(d,p->maps[i].source); release(d,p->maps[i].destination); }
    for(i=0;i<4;i++) release(d,p->images[i]); release(d,p->maps); release(d,p->document); zero(p,sizeof(*p));
}
static int metadata_text(struct json j,const char *key,char *out,uint32_t cap,int required,uint8_t *exists) {
    struct json f=field(j,key); if(exists) *exists=(uint8_t)(f.p!=0);
    if(!f.p) { out[0]=0; return required ? -22:0; }
    if(decoded(f,out,cap,0)) return -22; return required && !out[0] ? -22:0;
}
static int background(struct rh_manager_data *d,struct pack *p,uint32_t index,struct json v) {
    char mode[16],image[256]; struct json f;
    if(!v.p) return 0;
    if(*v.p=='"') {
        if(decoded(v,image,sizeof(image),0)) return -22;
        if(equal(image,"system")) p->backgrounds[index]=1;
        else if(equal(image,"blur")) p->backgrounds[index]=2;
        else { p->backgrounds[index]=3; p->images[index]=dup(d,image); }
    } else {
        f=field(v,"mode"); if(decoded(f,mode,sizeof(mode),0)) return -22;
        if(equal(mode,"system")) p->backgrounds[index]=1;
        else if(equal(mode,"blur")) p->backgrounds[index]=2;
        else if(equal(mode,"image")) {
            if(decoded(field(v,"image"),image,sizeof(image),0)) return -22;
            p->backgrounds[index]=3; p->images[index]=dup(d,image);
        } else return -22;
        /* Unknown settings are rejected, matching the existing manifest parser. */
        {
            const char *cur=ws(v.p+1,v.e); struct json k;
            while(cur<v.e && *cur!='}') {
                k.p=cur; if(scan_string(&cur,v.e)) return -22; k.e=cur;
                if(!jeq(k,"mode") && !(p->backgrounds[index]==3 && jeq(k,"image"))) return -22;
                cur=ws(cur,v.e)+1; if(scan_value(&cur,v.e,0)) return -22; cur=ws(cur,v.e); if(*cur==',') cur=ws(cur+1,v.e); else break;
            }
        }
    }
    if(p->backgrounds[index]==3) {
        if(!p->images[index]) return -12;
        if(!destination_valid(d,p->meta.id,p->images[index]) || p->images[index][slen(p->images[index])-1u]=='/') return -22;
    }
    return 0;
}
static int add_mapping(struct rh_manager_data *d,struct pack *p,struct json v,int icon) {
    struct mapping m={0,0}; struct json source=field(v,icon ? "package":"source"); char package[256]; int rc;
    if(p->count>=256u) return RH_MANAGER_ERR_LIMIT;
    if(icon) {
        struct text t={0,0,0}; if(decoded(source,package,sizeof(package),0)) return -22;
        rc=append(d,&t,"@quickapp-icon/"); if(!rc) rc=append(d,&t,package); if(rc) { release(d,t.p); return rc; } m.source=t.p;
    } else { rc=json_text(d,source,&m.source,255); if(rc) return rc; }
    rc=json_text(d,field(v,"destination"),&m.destination,255);
    if(!rc && (!source_valid(m.source) || !destination_valid(d,p->meta.id,m.destination))) rc=-22;
    if(!rc && prefix(m.source,"@quickapp-icon/") && !is_bin(m.destination)) rc=-22;
    if(!rc && !prefix(m.source,"@quickapp-icon/") && directory_source(m.source)!=(m.destination[slen(m.destination)-1u]=='/')) rc=-22;
    if(!rc) { uint32_t i; for(i=0;i<p->count;i++) if(equal(p->maps[i].source,m.source)) { rc=-22; break; } }
    if(!rc) rc=grow(d,(void **)&p->maps,p->count,p->count+1u,sizeof(*p->maps));
    if(rc) { release(d,m.source); release(d,m.destination); return rc; }
    p->maps[p->count++]=m; return 0;
}
static int calendar_text(struct rh_manager_data *,const struct pack *,struct text *,int);
static int load_pack_metadata(struct rh_manager_data *d,const char *id,struct pack *p,struct json *manifest) {
    char file[256],base[80]; struct json j,f,v; const char *cur=0; int rc; int64_t number_value;
    zero(p,sizeof(*p)); if(!valid_id(id)) return -22;
    if(path_join(base,"themes/",id)||path_join(file,base,"/corona.json")) return -22;
    /* Installed canonical metadata takes precedence. An unused legacy path
     * must not make a readable package depend on another stat or parse. */
    rc=read_text(d,file,65536,&p->document,&j);
    if(rc==CANOPUS_SVC_ERR_NOTFOUND) {
        if(path_join(file,base,"/canora.json")) return -22;
        rc=read_text(d,file,65536,&p->document,&j);
    }
    if(rc) goto bad;
    if(*j.p!='{' || !jeq(field(j,"format"),"canopus-resource-pack") || integer(field(j,"formatVersion"),&number_value) || number_value!=1) { rc=-22; goto bad; }
    if(metadata_text(j,"themeId",p->meta.id,sizeof(p->meta.id),1,0)||!equal(p->meta.id,id)||
       metadata_text(j,"name",p->meta.name,sizeof(p->meta.name),1,0)||
       metadata_text(j,"author",p->meta.author,sizeof(p->meta.author),0,&p->meta.has_author)||
       metadata_text(j,"version",p->meta.version,sizeof(p->meta.version),0,&p->meta.has_version)||
       metadata_text(j,"description",p->meta.description,sizeof(p->meta.description),0,0)) { rc=-22; goto bad; }
    f=field(j,"versionCode"); if(f.p) { if(integer(f,&number_value)||number_value<0) { rc=-22; goto bad; } p->meta.version_code=(uint64_t)number_value; p->meta.has_version_code=1; }
    f=field(j,"targets"); if(f.p) {
        struct text targets={0,0,0}; uint32_t n=0; char item[129];
        while((rc=next_array(f,&cur,&v))>0) {
            if(++n>16u || decoded(v,item,sizeof(item),0) || !item[0]) { rc=-22; break; }
            if(n>1u) rc=append(d,&targets,"、"); else rc=0;
            if(!rc) rc=append(d,&targets,item); if(rc) break;
        }
        if(rc>=0) rc=put(p->meta.targets,sizeof(p->meta.targets),targets.p ? targets.p:"");
        release(d,targets.p); if(rc) goto bad;
    }
    p->meta.available=1; if(manifest) *manifest=j; return 0;
bad:
    free_pack(d,p); return fail(d,rc,"资源包 corona.json 无效或不可读取");
}
static int load_pack(struct rh_manager_data *d,const char *id,struct pack *p) {
    struct json j,f,v; const char *cur=0; int rc; uint32_t i;
    rc=load_pack_metadata(d,id,p,&j); if(rc) return rc;
    f=field(j,"mappings"); cur=0; while((rc=next_array(f,&cur,&v))>0) { rc=add_mapping(d,p,v,0); if(rc) break; } if(rc<0) goto bad;
    f=field(j,"quickappIcons"); cur=0; if(f.p) { while((rc=next_array(f,&cur,&v))>0) { rc=add_mapping(d,p,v,1); if(rc) break; } if(rc<0) goto bad; }
    f=field(j,"backgrounds"); if(f.p) {
        const char *b=ws(f.p+1,f.e); struct json k;
        if(*f.p!='{') { rc=-22; goto bad; }
        while(b<f.e && *b!='}') {
            int found=-1; k.p=b; if(scan_string(&b,f.e)) { rc=-22; goto bad; } k.e=b;
            for(i=0;i<4;i++) if(jeq(k,feature_ids[i])) found=(int)i;
            if(found<0) { rc=-22; goto bad; }
            b=ws(b,f.e)+1; if(scan_value(&b,f.e,0)) { rc=-22; goto bad; } b=ws(b,f.e); if(*b==',') b=ws(b+1,f.e); else break;
        }
        for(i=0;i<4;i++) { rc=background(d,p,i,field(f,feature_ids[i])); if(rc) goto bad; }
    }
    f=field(j,"launcherBackground"); if(f.p) {
        if(!p->backgrounds[0]) { rc=background(d,p,0,f); if(rc) goto bad; }
        else {
            struct pack legacy_background; zero(&legacy_background,sizeof(legacy_background)); legacy_background.meta=p->meta;
            rc=background(d,&legacy_background,0,f); release(d,legacy_background.images[0]); if(rc) goto bad;
        }
    }
    p->calendar=field(j,"calendar");
    {
        struct text validation={0,0,0}; uint32_t bytes=0;
        rc=calendar_text(d,p,&validation,0); release(d,validation.p); if(rc) goto bad;
        for(i=0;i<p->count;i++) {
            uint32_t line=slen(p->maps[i].source)+1u+7u+slen(id)+1u+slen(p->maps[i].destination)+1u;
            if(line>32768u-bytes) { rc=RH_MANAGER_ERR_LIMIT; goto bad; } bytes+=line;
        }
    }
    p->meta.available=1; return 0;
bad:
    free_pack(d,p); return fail(d,rc,"资源包 corona.json 无效或不可读取");
}
static int mkdir_ok(struct rh_manager_data *d,const char *path) {
    int rc=d->ctx->fs->mkdir(d->ctx,path); struct canopus_fs_stat st;
    if(rc==CANOPUS_SVC_ERR_EXIST) { rc=d->ctx->fs->stat(d->ctx,path,&st); if(!rc && !st.is_dir) rc=-20; }
    return rc;
}
static int copy_file(struct rh_manager_data *d,const char *from,const char *to) {
    char buffer[1024]; int src,dst,got,written,rc=0; uint32_t used;
    src=d->ctx->fs->open(d->ctx,from,CANOPUS_FS_READ); if(src<0) return src;
    dst=d->ctx->fs->open(d->ctx,to,CANOPUS_FS_WRITE); if(dst<0) { (void)d->ctx->fs->close(d->ctx,src); return dst; }
    for(;;) {
        got=d->ctx->fs->read(d->ctx,src,buffer,sizeof(buffer));
        if(got<0 || (uint32_t)got>sizeof(buffer)) { rc=got<0 ? got:-5; break; }
        if(!got) break;
        used=0; while(used<(uint32_t)got) { written=d->ctx->fs->write(d->ctx,dst,buffer+used,(uint32_t)got-used); if(written<=0 || (uint32_t)written>(uint32_t)got-used) { rc=written<0 ? written:-5; break; } used+=(uint32_t)written; }
        if(rc) break;
    }
    if(!rc && d->ctx->fs->sync) { got=d->ctx->fs->sync(d->ctx,dst); if(got<0 && got!=CANOPUS_SVC_ERR_UNSUPPORTED) rc=got; }
    got=d->ctx->fs->close(d->ctx,src); if(!rc && got<0) rc=got;
    got=d->ctx->fs->close(d->ctx,dst); if(!rc && got<0) rc=got; return rc;
}
static int copy_directory(struct rh_manager_data *d,const char *from,const char *to,uint32_t depth) {
    void *dir; struct canopus_fs_dirent entry; char source[256],target[256],base[256]; int rc;
    if(depth>16u) return RH_MANAGER_ERR_LIMIT;
    rc=mkdir_ok(d,to); if(rc) return rc;
    dir=d->ctx->fs->opendir(d->ctx,from); if(!dir) return -5;
    while((rc=d->ctx->fs->readdir(d->ctx,dir,&entry))==0) {
        if(equal(entry.name,".")||equal(entry.name,"..")) continue;
        if(!relative(entry.name)||slen(entry.name)==0u) { rc=-22; break; }
        if(path_join(base,from,"/")||path_join(source,base,entry.name)||path_join(base,to,"/")||path_join(target,base,entry.name)) { rc=-22; break; }
        rc=entry.is_dir ? copy_directory(d,source,target,depth+1u):copy_file(d,source,target); if(rc) break;
    }
    if(rc==1) rc=0; /* Supervisor readdir: zero entry, one EOF. */
    { int close_rc=d->ctx->fs->closedir(d->ctx,dir); if(!rc && close_rc<0) rc=close_rc; }
    return rc;
}
static int migrate_legacy(struct rh_manager_data *d) {
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    const char *legacy="/data/files/ng.lst.corona/";
#else
    const char *legacy="/data/quickapp/files/ng.lst.corona/";
#endif
    static const char *const files[]={"resource-order.json","resource-files.json","resource-overrides.json","resource-active-generations.json","mappings.tsv","backgrounds.cfg","calendar.cfg"};
    static const char marker[]="resource-legacy-imported";
    char source[256],target[256],temporary[256],*index=0,*native=0,*complete=0;
    struct json j; struct text merged={0,0,0}; struct canopus_fs_stat st;
    uint32_t i,native_count=0,count=0,added=0; char **ids=0,**native_ids=0; int rc,fresh;
    rc=read_text(d,marker,2u,&complete,0);
    if(!rc) { rc=equal(complete,"1\n") ? 0:-22; release(d,complete); return rc; }
    if(rc!=CANOPUS_SVC_ERR_NOTFOUND) return rc;
    rc=read_text(d,"interconnect-themes.json",UINT32_MAX-1u,&native,&j);
    fresh=rc==CANOPUS_SVC_ERR_NOTFOUND;
    if(!fresh && !rc) rc=string_list(d,j,&native_ids,&native_count,0);
    if(!fresh && rc) goto done;
    if(path_join(source,legacy,"interconnect-themes.json")) { rc=-22; goto done; }
    rc=read_text(d,source,UINT32_MAX-1u,&index,&j);
    if(rc==CANOPUS_SVC_ERR_NOTFOUND) { rc=0; goto done; }
    if(rc) goto done;
    rc=string_list(d,j,&ids,&count,0); if(rc) goto done;
    rc=mkdir_ok(d,"themes"); if(rc) goto done;
    if(fresh) {
        if(path_join(source,legacy,"themes")) { rc=-22; goto done; }
        /* A first import also retains materialized .active-* resources needed
         * by the old root configuration. Never rewrite the source install. */
        rc=d->ctx->fs->stat(d->ctx,source,&st);
        if(rc==CANOPUS_SVC_ERR_NOTFOUND && count==0u) rc=0;
        else if(!rc) rc=st.is_dir ? copy_directory(d,source,"themes",0):-20;
        for(i=0;!rc && i<sizeof(files)/sizeof(files[0]);i++) {
            rc=d->ctx->fs->stat(d->ctx,files[i],&st);
            if(!rc) continue;
            if(rc!=CANOPUS_SVC_ERR_NOTFOUND) break;
            if(path_join(source,legacy,files[i])||path_join(temporary,files[i],".manager-tmp")) { rc=-22; break; }
            rc=copy_file(d,source,temporary);
            if(!rc) rc=d->ctx->fs->rename(d->ctx,temporary,files[i]);
            if(rc) (void)d->ctx->fs->remove(d->ctx,temporary);
            if(rc==CANOPUS_SVC_ERR_NOTFOUND) rc=0;
        }
        if(!rc) rc=atomic_text(d,"interconnect-themes.json",index,slen(index));
    } else {
        /* Registered native packages win as a whole, including their metadata.
         * Copy only new IDs; existing ordering, choices and active references
         * belong to the native installation and must remain untouched. */
        rc=append(d,&merged,"[");
        for(i=0;!rc && i<native_count;i++) {
            if(i) rc=append(d,&merged,",");
            if(!rc) rc=quoted(d,&merged,native_ids[i]);
        }
        for(i=0;!rc && i<count;i++) {
            if(find_id(native_ids,native_count,ids[i])>=0) continue;
            if(path_join(target,"themes/",ids[i])||path_join(source,legacy,target)) { rc=-22; break; }
            rc=copy_directory(d,source,target,0);
            if(!rc && (native_count || added)) rc=append(d,&merged,",");
            if(!rc) rc=quoted(d,&merged,ids[i]);
            if(!rc) added++;
        }
        if(!rc) rc=append(d,&merged,"]");
        /* No partially copied package is visible before registry publication. */
        if(!rc && added) rc=atomic_text(d,"interconnect-themes.json",merged.p,merged.n);
    }
    /* Written last so interrupted imports can retry. Once committed, deleting
     * a migrated package must not resurrect it on refresh or next launch. */
    if(!rc) rc=atomic_text(d,marker,"1\n",2u);
done:
    free_strings(d,ids,count); free_strings(d,native_ids,native_count);
    release(d,index); release(d,native); release(d,merged.p); return rc;
}
static int valid_choice(const char *s,int blur) { return equal(s,RH_MANAGER_DEFAULT)||equal(s,RH_MANAGER_SYSTEM)||(blur&&equal(s,RH_MANAGER_BLUR))||valid_id(s); }
static int feature_index(const char *key) { uint32_t i; if(!prefix(key,"feature:")) return -1; for(i=0;i<5;i++) if(equal(key+8,feature_ids[i])) return (int)i; return -1; }
static int read_overrides(struct rh_manager_data *d) {
    char *doc=0,*key=0,*choice=0; struct json j,o,k,v; const char *p; int64_t version; int rc;
    rc=read_text(d,"resource-overrides.json",65536,&doc,&j); if(rc==CANOPUS_SVC_ERR_NOTFOUND) return 0; if(rc) return rc;
    if(integer(field(j,"version"),&version)||(version!=1 && version!=2)) { rc=-22; goto done; }
    o=field(j,"overrides"); if(!o.p||*o.p!='{') { rc=-22; goto done; }
    p=ws(o.p+1,o.e);
    while(p<o.e && *p!='}') {
        k.p=p; if(scan_string(&p,o.e)) { rc=-22; goto done; } k.e=p;
        p=ws(p,o.e)+1; p=ws(p,o.e); v.p=p; if(scan_value(&p,o.e,0)) { rc=-22; goto done; } v.e=p;
        rc=json_text(d,k,&key,255); if(!rc) rc=json_text(d,v,&choice,64); if(rc) goto done;
        if(!source_valid(key)||directory_source(key)||!valid_choice(choice,0)) { rc=-22; goto done; }
        { uint32_t i; for(i=0;i<d->override_count;i++) if(equal(d->overrides[i].key,key)) break;
          if(i==d->override_count) { rc=grow(d,(void **)&d->overrides,i,i+1u,sizeof(*d->overrides)); if(rc) goto done; d->override_count++; }
          else { release(d,d->overrides[i].key); release(d,d->overrides[i].choice); }
          d->overrides[i]=(struct override){key,choice}; key=choice=0; }
        p=ws(p,o.e); if(*p==',') p=ws(p+1,o.e); else break;
    }
    if(version==2) {
        o=field(j,"features"); if(!o.p||*o.p!='{') { rc=-22; goto done; } p=ws(o.p+1,o.e);
        while(p<o.e && *p!='}') {
            uint32_t i; k.p=p; if(scan_string(&p,o.e)) { rc=-22; goto done; } k.e=p;
            p=ws(p,o.e)+1; p=ws(p,o.e); v.p=p; if(scan_value(&p,o.e,0)) { rc=-22; goto done; } v.e=p;
            for(i=0;i<5;i++) if(jeq(k,feature_ids[i])) break;
            if(i==5) { rc=-22; goto done; }
            rc=json_text(d,v,&choice,64); if(rc) goto done; if(!valid_choice(choice,i!=4)) { rc=-22; goto done; }
            release(d,d->features[i]); d->features[i]=choice; choice=0;
            p=ws(p,o.e); if(*p==',') p=ws(p+1,o.e); else break;
        }
    }
    rc=0;
done:
    release(d,key); release(d,choice); release(d,doc); return rc;
}
static void clear_catalog(struct rh_manager_data *d) {
    uint32_t i,k; for(i=0;i<d->path_count;i++) { release(d,d->paths[i].source); for(k=0;k<d->paths[i].count;k++) release(d,d->paths[i].options[k].destination); release(d,d->paths[i].options); }
    release(d,d->paths); d->paths=0; d->path_count=0; d->catalog_ready=0;
}
static void clear_data(struct rh_manager_data *d) {
    uint32_t i; clear_catalog(d); free_strings(d,d->ids,d->count); free_strings(d,d->order,d->order_count);
    for(i=0;i<d->override_count;i++) { release(d,d->overrides[i].key); release(d,d->overrides[i].choice); }
    release(d,d->overrides); for(i=0;i<5;i++) release(d,d->features[i]); release(d,d->pending_protection);
    d->ids=d->order=0; d->count=d->order_count=d->override_count=0; d->overrides=0; d->pending_protection=0; zero(d->features,sizeof(d->features));
}
static int add_order(struct rh_manager_data *d,const char *s) {
    char *id=dup(d,s); int rc; if(!id) return -12; rc=grow(d,(void **)&d->order,d->order_count,d->order_count+1u,sizeof(*d->order)); if(rc) release(d,id); else d->order[d->order_count++]=id; return rc;
}
static int save_order(struct rh_manager_data *d) {
    struct text t={0,0,0}; int rc=append(d,&t,"{\"version\":1,\"order\":"); if(!rc) rc=serialize_ids(d,&t,d->order,d->order_count); if(!rc) rc=append(d,&t,"}"); if(!rc) rc=atomic_text(d,"resource-order.json",t.p,t.n); release(d,t.p); return rc;
}
int rh_manager_data_refresh(struct rh_manager_data *d) {
    struct rh_manager_data next; char *doc=0,**stored=0; uint32_t stored_n=0,i; struct json j; int rc; int64_t version;
    if(!d) return -22; if(d->busy || d->pending_protection) return -16;
    zero(&next,sizeof(next)); next.ctx=d->ctx; next.pool=d->pool; next.hooks=d->hooks; copy(next.root,d->root,sizeof(next.root));
    rc=migrate_legacy(d); if(rc) return fail(d,rc,"快应用数据迁移失败，原数据已保留");
    rc=read_text(d,"interconnect-themes.json",UINT32_MAX-1u,&doc,&j);
    if(rc==CANOPUS_SVC_ERR_NOTFOUND) rc=0;
    else if(!rc) rc=string_list(&next,j,&next.ids,&next.count,0);
    release(d,doc); doc=0; if(rc) goto bad;
    rc=read_text(d,"resource-order.json",UINT32_MAX-1u,&doc,&j);
    if(rc==CANOPUS_SVC_ERR_NOTFOUND) {
        rc=0; for(i=0;!rc && i<next.count;i++) rc=add_order(&next,next.ids[i]); if(!rc) rc=add_order(&next,RH_MANAGER_SYSTEM);
    } else if(!rc) {
        if(integer(field(j,"version"),&version)||version!=1) rc=-22;
        else rc=string_list(&next,field(j,"order"),&stored,&stored_n,1);
        if(!rc && find_id(stored,stored_n,RH_MANAGER_SYSTEM)<0) rc=-22;
        for(i=next.count;!rc && i>0;i--) if(find_id(stored,stored_n,next.ids[i-1u])<0) rc=add_order(&next,next.ids[i-1u]);
        for(i=0;!rc && i<stored_n;i++) if(equal(stored[i],RH_MANAGER_SYSTEM)||find_id(next.ids,next.count,stored[i])>=0) rc=add_order(&next,stored[i]);
    }
    release(d,doc); doc=0; free_strings(&next,stored,stored_n); stored=0; stored_n=0; if(rc) goto bad;
    rc=read_overrides(&next); if(rc) goto bad;
    /* Persist normalization before publishing a new model. A failed write leaves
     * the previous visible ordering and overrides intact. */
    rc=save_order(&next); if(rc) goto bad;
    clear_data(d); *d=next; return 0;
bad:
    release(d,doc); free_strings(&next,stored,stored_n); clear_data(&next); return fail(d,rc,"资源包索引、排序或混搭配置不可读取");
}
int rh_manager_data_create(const struct canopus_context_v1 *ctx,const struct rh_manager_data_hooks *hooks,struct rh_manager_data **out) {
    struct rh_manager_data *d; char root[256]; uint32_t n; int rc;
    if(!ctx||!out||!ctx->fs||!ctx->mem||!ctx->mem->alloc||!ctx->mem->free||!ctx->fs->get_data_path||!ctx->fs->size||!ctx->fs->open||!ctx->fs->read||!ctx->fs->write||!ctx->fs->close||!ctx->fs->rename||!ctx->fs->remove||!ctx->fs->stat||!ctx->fs->mkdir||!ctx->fs->exists||!ctx->fs->opendir||!ctx->fs->readdir||!ctx->fs->closedir) return -38;
    *out=0; zero(root,sizeof(root)); rc=ctx->fs->get_data_path(ctx,root,sizeof(root)); if(rc<0) return rc;
    n=slen(root); if(n && root[n-1u]=='/') root[--n]=0;
    if(!equal(root,"/data/canopus/m.corona")) return -22;
    d=ctx->mem->alloc(ctx,sizeof(*d)); if(!d) return -12; zero(d,sizeof(*d)); d->ctx=ctx; if(hooks) d->hooks=*hooks;
    d->pool=ctx->mem->alloc(ctx,sizeof(*d->pool)); if(!d->pool) { ctx->mem->free(ctx,d); return -12; } zero(d->pool,sizeof(*d->pool));
    copy(d->root,root,n); d->root[n++]='/'; d->root[n]=0;
    rc=rh_manager_data_refresh(d); if(rc) { rh_manager_data_destroy(d); return rc; } *out=d; return 0;
}
void rh_manager_data_destroy(struct rh_manager_data *d) {
    if(d) {
        const struct canopus_context_v1 *ctx=d->ctx; struct pool_chunk *chunk,*next;
        clear_data(d);
        for(chunk=d->pool->chunks;chunk;chunk=next) { next=chunk->next; ctx->mem->free(ctx,chunk); }
        ctx->mem->free(ctx,d->pool); ctx->mem->free(ctx,d);
    }
}
const char *rh_manager_data_root(const struct rh_manager_data *d) { return d ? d->root:""; }
const char *rh_manager_data_error(const struct rh_manager_data *d) { return d && d->error ? d->error:""; }
uint32_t rh_manager_data_order_count(const struct rh_manager_data *d) { return d ? d->order_count:0; }
int rh_manager_data_metadata(struct rh_manager_data *d,const char *id,struct rh_manager_metadata *out) {
    struct pack p; int rc; if(!d||!id||!out||find_id(d->ids,d->count,id)<0) return -22;
    zero(out,sizeof(*out)); put(out->id,sizeof(out->id),id); put(out->name,sizeof(out->name),id);
    rc=load_pack_metadata(d,id,&p,0); if(!rc) { *out=p.meta; free_pack(d,&p); }
    else if(d->ctx->log && d->ctx->log->warn) {
        char message[96],digits[10]; uint32_t n,k=0,value=rc<0 ? 0u-(uint32_t)rc:(uint32_t)rc;
        put(message,sizeof(message),"metadata "); n=slen(message);
        put(message+n,sizeof(message)-n,id); n=slen(message);
        put(message+n,sizeof(message)-n," rc="); n=slen(message);
        if(rc<0) message[n++]='-';
        do { digits[k++]=(char)('0'+value%10u); value/=10u; } while(value);
        while(k) message[n++]=digits[--k]; message[n]=0;
        d->ctx->log->warn(d->ctx,message);
    }
    /* Display reads metadata only; rule validation belongs to catalog/apply.
     * Unreadable metadata remains represented in resource rows and detail UI. */
    return rc;
}
int rh_manager_data_order_get(struct rh_manager_data *d,uint32_t index,char id[65],struct rh_manager_metadata *out) {
    if(!d||!id||index>=d->order_count) return -22; put(id,65,d->order[index]);
    if(out) { if(equal(id,RH_MANAGER_SYSTEM)) { zero(out,sizeof(*out)); put(out->id,sizeof(out->id),id); put(out->name,sizeof(out->name),"系统样式"); out->available=1; }
        else { (void)rh_manager_data_metadata(d,id,out); } }
    return 0;
}
int rh_manager_data_move(struct rh_manager_data *d,uint32_t from,uint32_t to) {
    uint32_t i; char *moved; int rc; if(!d||from>=d->order_count||to>=d->order_count) return -22; if(d->busy||d->pending_protection) return -16; if(from==to) return 0;
    moved=d->order[from]; if(from<to) for(i=from;i<to;i++) d->order[i]=d->order[i+1u]; else for(i=from;i>to;i--) d->order[i]=d->order[i-1u]; d->order[to]=moved;
    rc=save_order(d); if(rc) { if(from<to) for(i=to;i>from;i--) d->order[i]=d->order[i-1u]; else for(i=to;i<from;i++) d->order[i]=d->order[i+1u]; d->order[from]=moved; }
    return rc ? fail(d,rc,"排序保存失败，已恢复原顺序"):0;
}
static int register_file(struct rh_manager_data *d,const struct pack *pack,uint32_t theme,const char *relative_path,uint32_t bytes) {
    uint32_t i,k,lo,hi,index; char source[256]; const struct mapping *m; struct path_item *item; char *destination;
    for(i=0;i<pack->count;i++) {
        m=&pack->maps[i];
        if(directory_source(m->source)) {
            if(!prefix(relative_path,m->destination)||!relative_path[slen(m->destination)]) continue;
            /* The JS catalog omits only concrete expansions beyond the
             * native path limit; the valid prefix and other files survive. */
            if(path_join(source,m->source,relative_path+slen(m->destination))) continue;
        } else { if(!equal(relative_path,m->destination)) continue; put(source,sizeof(source),m->source); }
        if(!source_valid(source)||directory_source(source)) return -22;
        for(k=0;k<pack->count;k++) {
            const char *other=pack->maps[k].source;
            if(k!=i && slen(other)>slen(m->source) && (directory_source(other) ? prefix(source,other):equal(source,other))) break;
        }
        if(k<pack->count) continue;
        lo=0; hi=d->path_count;
        while(lo<hi) { uint32_t mid=lo+(hi-lo)/2u; if(scmp(d->paths[mid].source,source)<0) lo=mid+1u; else hi=mid; }
        index=lo;
        if(index==d->path_count || !equal(d->paths[index].source,source)) {
            char *s=dup(d,source); int rc; if(!s) return -12;
            rc=grow(d,(void **)&d->paths,d->path_count,d->path_count+1u,sizeof(*d->paths)); if(rc) { release(d,s); return rc; }
            for(k=d->path_count;k>index;k--) d->paths[k]=d->paths[k-1u]; zero(&d->paths[index],sizeof(d->paths[index])); d->paths[index].source=s; d->path_count++;
        }
        item=&d->paths[index];
        for(k=0;k<item->count;k++) if(item->options[k].theme==theme) break;
        destination=dup(d,relative_path); if(!destination) return -12;
        if(k==item->count) {
            int rc=grow(d,(void **)&item->options,item->count,item->count+1u,sizeof(*item->options)); if(rc) { release(d,destination); return rc; } item->count++;
        } else release(d,item->options[k].destination);
        item->options[k].theme=theme; item->options[k].destination=destination;
        (void)bytes;
    }
    return 0;
}
/* Directory handles are bounded by the original inventory depth limit (16),
 * not by file count. One path buffer and one dirent serve the complete walk. */
static int walk_theme(struct rh_manager_data *d,const char *id,const struct pack *pack,uint32_t theme,int deleting) {
    void *dirs[17]; uint32_t offsets[17],depth=0,root_len; char absolute[256],path[256],base[80]; struct canopus_fs_dirent entry; struct canopus_fs_stat st; int rc=0;
    if(path_join(base,"themes/",id)||path_join(path,base,"/")) return -22;
    root_len=slen(path); offsets[0]=root_len; dirs[0]=d->ctx->fs->opendir(d->ctx,path); if(!dirs[0]) return -5;
    for(;;) {
        rc=d->ctx->fs->readdir(d->ctx,dirs[depth],&entry);
        if(rc!=0) {
            int close_rc=d->ctx->fs->closedir(d->ctx,dirs[depth]); dirs[depth]=0;
            if(rc<0 || close_rc<0) { if(rc>=0) rc=close_rc; break; }
            rc=0;
            if(!depth) break; depth--; path[offsets[depth]]=0; continue;
        }
        if(equal(entry.name,".")||equal(entry.name,"..")) continue;
        if(!relative(entry.name)||entry.name[0]==0 || slen(entry.name)+offsets[depth]>=sizeof(path)) { rc=-22; break; }
        copy(path+offsets[depth],entry.name,slen(entry.name)+1u);
        if(entry.is_dir) {
            uint32_t n=slen(path); if(depth==16u || n+1u>=sizeof(path)) { rc=RH_MANAGER_ERR_LIMIT; break; }
            path[n++]='/'; path[n]=0; depth++; offsets[depth]=n; dirs[depth]=d->ctx->fs->opendir(d->ctx,path); if(!dirs[depth]) { rc=-5; break; }
        } else if(deleting) {
            rc=d->ctx->fs->remove(d->ctx,path); if(rc) break;
        } else {
            rc=d->ctx->fs->stat(d->ctx,path,&st); if(rc || st.is_dir || st.size>64u*1024u*1024u) { if(!rc) rc=-22; break; }
            if(path_join(absolute,d->root,path)) { rc=-22; break; }
            (void)absolute;
            if(!equal(path+root_len,"mappings.tsv")) { rc=register_file(d,pack,theme,path+root_len,st.size); if(rc) break; }
        }
    }
    while(depth) { if(dirs[depth]) (void)d->ctx->fs->closedir(d->ctx,dirs[depth]); depth--; }
    if(dirs[0]) (void)d->ctx->fs->closedir(d->ctx,dirs[0]);
    return rc;
}
static int ensure_catalog(struct rh_manager_data *d) {
    uint32_t i; int rc; struct pack p;
    if(d->catalog_ready) return 0;
    for(i=0;i<d->count;i++) {
        rc=load_pack(d,d->ids[i],&p); if(rc) { clear_catalog(d); return rc; }
        rc=walk_theme(d,d->ids[i],&p,i,0); free_pack(d,&p);
        if(rc) { clear_catalog(d); return fail(d,rc,"无法枚举资源包文件"); }
    }
    d->catalog_ready=1; return 0;
}
static int path_index(struct rh_manager_data *d,const char *key) {
    uint32_t lo=0,hi=d->path_count;
    while(lo<hi) { uint32_t mid=lo+(hi-lo)/2u; int c=scmp(d->paths[mid].source,key); if(c<0) lo=mid+1u; else hi=mid; }
    return lo<d->path_count && equal(d->paths[lo].source,key) ? (int)lo:-1;
}
static int theme_available(struct rh_manager_data *d,const char *key,const char *id) {
    int feature=feature_index(key),theme=find_id(d->ids,d->count,id),path; uint32_t i; struct pack p; int rc;
    if(theme<0) return 0;
    if(feature>=0) { rc=load_pack(d,id,&p); if(rc) return rc; rc=feature==4 ? p.calendar.p!=0:p.backgrounds[feature]!=0; free_pack(d,&p); return rc; }
    path=path_index(d,key); if(path<0) return 0;
    for(i=0;i<d->paths[path].count;i++) if(d->paths[path].options[i].theme==(uint32_t)theme) return 1;
    return 0;
}
static const char *stored_choice(struct rh_manager_data *d,const char *key) {
    int feature=feature_index(key); uint32_t i;
    if(feature>=0) return d->features[feature] ? d->features[feature]:RH_MANAGER_DEFAULT;
    for(i=0;i<d->override_count;i++) if(equal(d->overrides[i].key,key)) return d->overrides[i].choice;
    return RH_MANAGER_DEFAULT;
}
static int resolve_choice(struct rh_manager_data *d,const char *key,char choice[65]) {
    const char *s=stored_choice(d,key); int feature=feature_index(key),rc;
    if(equal(s,RH_MANAGER_DEFAULT)||equal(s,RH_MANAGER_SYSTEM)||(feature>=0 && feature<4 && equal(s,RH_MANAGER_BLUR))) return put(choice,65,s);
    rc=theme_available(d,key,s); if(rc<0) return rc; return put(choice,65,rc ? s:RH_MANAGER_DEFAULT);
}
int rh_manager_data_mix_count(struct rh_manager_data *d,uint32_t *out) {
    int rc; if(!d||!out) return -22; rc=ensure_catalog(d); if(rc) return rc; if(d->path_count>UINT32_MAX-5u) return -12; *out=d->path_count+5u; return 0;
}
static int mix_key(struct rh_manager_data *d,uint32_t index,char key[256],char label[256]) {
    if(index<5u) { if(path_join(key,"feature:",feature_ids[index])) return -22; return put(label,256,feature_labels[index]); }
    index-=5u; if(index>=d->path_count) return -22; put(key,256,d->paths[index].source); return put(label,256,d->paths[index].source);
}
int rh_manager_data_mix_get(struct rh_manager_data *d,uint32_t index,struct rh_manager_mix_item *out) {
    uint32_t count,i,at=0,partition; char key[256],label[256],choice[65]; int rc;
    if(!d||!out) return -22; rc=rh_manager_data_mix_count(d,&count); if(rc || index>=count) return rc ? rc:-22;
    for(partition=0;partition<2;partition++) for(i=0;i<count;i++) {
        rc=mix_key(d,i,key,label); if(rc) return rc; rc=resolve_choice(d,key,choice); if(rc) return rc;
        if((equal(choice,RH_MANAGER_DEFAULT) ? 1u:0u)!=partition) continue;
        if(at++!=index) continue;
        zero(out,sizeof(*out)); put(out->key,sizeof(out->key),key); put(out->label,sizeof(out->label),label); put(out->choice,sizeof(out->choice),choice); out->feature=(uint8_t)(i<5u);
        if(equal(choice,RH_MANAGER_DEFAULT)) put(out->choice_name,sizeof(out->choice_name),"默认");
        else if(equal(choice,RH_MANAGER_SYSTEM)) put(out->choice_name,sizeof(out->choice_name),"系统");
        else if(equal(choice,RH_MANAGER_BLUR)) put(out->choice_name,sizeof(out->choice_name),"模糊");
        else { struct rh_manager_metadata meta; rc=rh_manager_data_metadata(d,choice,&meta); if(rc) return rc; put(out->choice_name,sizeof(out->choice_name),meta.name); }
        return 0;
    }
    return -22;
}
static int key_valid(struct rh_manager_data *d,const char *key) { return feature_index(key)>=0 || path_index(d,key)>=0; }
int rh_manager_data_option_count(struct rh_manager_data *d,const char *key,uint32_t *out) {
    uint32_t i,n; int feature,rc; if(!d||!key||!out) return -22; rc=ensure_catalog(d); if(rc) return rc; if(!key_valid(d,key)) return -22;
    feature=feature_index(key); n=feature>=0 && feature<4 ? 3u:2u;
    for(i=0;i<d->order_count;i++) { rc=theme_available(d,key,d->order[i]); if(rc<0) return rc; if(rc) n++; } *out=n; return 0;
}
static int relative_destination(struct rh_manager_data *d,const char *key,const char *id,char out[256]) {
    int feature=feature_index(key),theme=find_id(d->ids,d->count,id),path; uint32_t i; struct pack p; int rc;
    out[0]=0; if(theme<0) return -22;
    if(feature>=0) {
        rc=load_pack(d,id,&p); if(rc) return rc;
        if(feature<4 && p.backgrounds[feature]==3) rc=put(out,256,p.images[feature]);
        else if(feature==4) {
            const struct mapping *best=0; uint32_t best_length=0;
            /* Calendar background remains an ordinary mapping and obeys the
             * same longest-prefix rule as the open hook. */
            for(i=0;i<p.count;i++) {
                const char *source=p.maps[i].source; uint32_t length=slen(source);
                if((directory_source(source) ? prefix(RH_CALENDAR_BACKGROUND,source):equal(source,RH_CALENDAR_BACKGROUND)) && length>best_length) { best=&p.maps[i]; best_length=length; }
            }
            if(best) {
                if(directory_source(best->source)) rc=path_join(out,best->destination,&RH_CALENDAR_BACKGROUND[best_length]);
                else rc=put(out,256,best->destination);
            }
        }
        free_pack(d,&p); return rc;
    }
    path=path_index(d,key); if(path<0) return -22;
    for(i=0;i<d->paths[path].count;i++) if(d->paths[path].options[i].theme==(uint32_t)theme) return put(out,256,d->paths[path].options[i].destination);
    return -22;
}
static int preview_valid(struct rh_manager_data *d,char path[256]) {
    struct canopus_fs_stat st; int rc;
    if(!path[0] || !is_bin(path)) { path[0]=0; return 0; }
    /* Alias paths cannot be stat'ed through the Supervisor filesystem. Probe
     * the real ROM path; the image driver later uses the independent key. */
    rc=d->ctx->fs->stat(d->ctx,prefix(path,"/.rh_sys/") ? path+8:path,&st);
    if(rc==CANOPUS_SVC_ERR_NOTFOUND || (!rc && (st.is_dir||!st.size))) { path[0]=0; return 0; }
    if(rc) { path[0]=0; return rc; } return 0;
}
static int system_preview(struct rh_manager_data *d,const char *key,char out[256]) {
    int feature=feature_index(key),rc=0; char original[256]; out[0]=0;
    if(feature>=0) {
        if(feature!=4) return 0; /* other system backgrounds have no static image */
        if(put(original,sizeof(original),RH_CALENDAR_BACKGROUND)) return -22;
    } else if(prefix(key,"@quickapp-icon/")) {
        if(!d->hooks.quickapp_icon) return 0; rc=d->hooks.quickapp_icon(d->hooks.cookie,key+15,original); if(rc<=0) return rc<0 ? rc:0;
    } else { if(put(original,sizeof(original),key)) return -22; }
    if(original[0]!='/' || path_join(out,"/.rh_sys",original)) return -22;
    return preview_valid(d,out);
}
static int winner_for(struct rh_manager_data *d,const char *key,char id[65]) {
    uint32_t i; int rc; id[0]=0;
    for(i=0;i<d->order_count;i++) {
        if(equal(d->order[i],RH_MANAGER_SYSTEM)) break;
        rc=theme_available(d,key,d->order[i]); if(rc<0) return rc;
        if(rc) return put(id,65,d->order[i]);
    }
    return 0;
}
static int theme_preview(struct rh_manager_data *d,const char *key,const char *id,char out[256]) {
    char relative_path[256],base[80],full[256],destination[256]; int rc=relative_destination(d,key,id,relative_path); out[0]=0; if(rc || !relative_path[0]) return rc;
    if(path_join(base,"themes/",id)||path_join(full,base,"/")) return -22;
    if(path_join(destination,full,relative_path)) return -22;
    if(path_join(out,d->root,destination)) return -22; return preview_valid(d,out);
}
int rh_manager_data_option_get(struct rh_manager_data *d,const char *key,uint32_t index,struct rh_manager_option *out) {
    uint32_t count,i,at,base; int rc,feature; char selected[65],winner[65];
    if(!d||!key||!out) return -22; rc=rh_manager_data_option_count(d,key,&count); if(rc || index>=count) return rc ? rc:-22;
    zero(out,sizeof(*out)); feature=feature_index(key); base=feature>=0 && feature<4 ? 3u:2u;
    if(index<base) {
        const char *id=index==0 ? RH_MANAGER_DEFAULT:(index==1 ? RH_MANAGER_SYSTEM:RH_MANAGER_BLUR);
        put(out->id,sizeof(out->id),id); put(out->name,sizeof(out->name),index==0 ? "默认":(index==1 ? "系统":"模糊"));
        if(index==0) { rc=winner_for(d,key,winner); if(!rc) rc=winner[0] ? theme_preview(d,key,winner,out->preview):system_preview(d,key,out->preview); }
        else if(index==1) rc=system_preview(d,key,out->preview); else rc=0;
    } else {
        at=base; rc=-22;
        for(i=0;i<d->order_count;i++) {
            int available=theme_available(d,key,d->order[i]); if(available<0) return available; if(!available) continue;
            if(at++==index) { struct rh_manager_metadata meta; rc=rh_manager_data_metadata(d,d->order[i],&meta); if(rc) break;
                put(out->id,sizeof(out->id),d->order[i]); put(out->name,sizeof(out->name),meta.name); rc=theme_preview(d,key,d->order[i],out->preview); break; }
        }
    }
    if(rc) return rc; rc=resolve_choice(d,key,selected); if(rc) return rc; out->selected=(uint8_t)equal(selected,out->id); return 0;
}
static int save_overrides(struct rh_manager_data *d) {
    struct text t={0,0,0}; uint32_t i; int rc=append(d,&t,"{\"version\":2,\"overrides\":{");
    for(i=0;!rc && i<d->override_count;i++) { if(i) rc=append(d,&t,","); if(!rc) rc=quoted(d,&t,d->overrides[i].key); if(!rc) rc=append(d,&t,":"); if(!rc) rc=quoted(d,&t,d->overrides[i].choice); }
    if(!rc) rc=append(d,&t,"},\"features\":{");
    { uint32_t n=0; for(i=0;!rc && i<5;i++) if(d->features[i]) { if(n++) rc=append(d,&t,","); if(!rc) rc=quoted(d,&t,feature_ids[i]); if(!rc) rc=append(d,&t,":"); if(!rc) rc=quoted(d,&t,d->features[i]); } }
    if(!rc) rc=append(d,&t,"}}"); if(!rc && t.n>65536u) rc=RH_MANAGER_ERR_LIMIT;
    if(!rc) rc=atomic_text(d,"resource-overrides.json",t.p,t.n); release(d,t.p); return rc;
}
int rh_manager_data_choose(struct rh_manager_data *d,const char *key,const char *choice) {
    int feature,rc; uint32_t i; char *replacement,*old; if(!d||!key||!choice) return -22; if(d->busy||d->pending_protection) return -16;
    rc=ensure_catalog(d); if(rc) return rc; if(!key_valid(d,key)) return -22;
    feature=feature_index(key); if(!valid_choice(choice,feature>=0 && feature<4)) return -22;
    if(valid_id(choice)) { rc=theme_available(d,key,choice); if(rc<=0) return rc<0 ? rc:-22; }
    if(equal(stored_choice(d,key),choice)) return 0;
    replacement=dup(d,choice); if(!replacement) return -12;
    if(feature>=0) { old=d->features[feature]; d->features[feature]=replacement; rc=save_overrides(d); if(rc) d->features[feature]=old; }
    else {
        for(i=0;i<d->override_count;i++) if(equal(d->overrides[i].key,key)) break;
        if(i==d->override_count) {
            char *k=dup(d,key); if(!k) { release(d,replacement); return -12; }
            rc=grow(d,(void **)&d->overrides,i,i+1u,sizeof(*d->overrides)); if(rc) { release(d,k); release(d,replacement); return rc; }
            d->overrides[i]=(struct override){k,replacement}; d->override_count++; old=0; rc=save_overrides(d);
            if(rc) { d->override_count--; release(d,k); }
        } else { old=d->overrides[i].choice; d->overrides[i].choice=replacement; rc=save_overrides(d); if(rc) d->overrides[i].choice=old; }
    }
    release(d,rc ? replacement:old); return rc ? fail(d,rc,"混搭选择保存失败"):0;
}
static int keys_allowed(struct json o,const char *const *allowed,uint32_t n) {
    const char *p; uint32_t i; struct json k;
    if(!o.p || *o.p!='{') return -22;
    p=ws(o.p+1,o.e);
    while(p<o.e && *p!='}') {
        k.p=p; if(scan_string(&p,o.e)) return -22; k.e=p;
        for(i=0;i<n;i++) if(jeq(k,allowed[i])) break;
        if(i==n) return -22;
        p=ws(p,o.e)+1; if(scan_value(&p,o.e,0)) return -22; p=ws(p,o.e); if(*p==',') p=ws(p+1,o.e); else break;
    }
    return 0;
}
static int calendar_text(struct rh_manager_data *d,const struct pack *p,struct text *t,int check_files) {
    static const char *const names[]={"month","day","weekday"};
    static const char *const fields[]={"type","x","y","align","size","color","context"};
    static const char *const none_fields[]={"type"};
    static const uint32_t counts[]={12,31,7};
    uint32_t i,k; int rc=append(d,t,"version=1\n");
    if(rc || !p || !p->calendar.p) return rc;
    if(keys_allowed(p->calendar,names,3)) return -22;
    for(i=0;i<3;i++) {
        struct json item=field(p->calendar,names[i]),type,context,entry,align,color; const char *cur=0; int32_t x,y,size; uint32_t count=0; char context_text[256],align_text[16]="top_mid",color_text[8]="native"; int image;
        if(!item.p) continue;
        type=field(item,"type"); image=jeq(type,"image");
        if(jeq(type,"none")) {
            if(keys_allowed(item,none_fields,1)) return -22;
            rc=append(d,t,names[i]); if(!rc) rc=append(d,t,"\tnone\ttop_left\t0\t0\t0\tnative\t0\n"); if(rc) return rc; continue;
        }
        if(!image && !jeq(type,"text")) return -22;
        if(keys_allowed(item,fields,7)||int_field(item,"x",0,&x)||int_field(item,"y",i==1u ? 34:17,&y)||int_field(item,"size",0,&size)||x < -112||x>112||y < -112||y>112||(size!=0 && (size<8||size>72))) return -22;
        align=field(item,"align"); if(align.p && decoded(align,align_text,sizeof(align_text),0)) return -22;
        if(!equal(align_text,"top_left")&&!equal(align_text,"top_mid")&&!equal(align_text,"top_right")&&!equal(align_text,"center")) return -22;
        color=field(item,"color");
        if(image && (size || color.p)) return -22;
        if(color.p) {
            char raw[8]; if(decoded(color,raw,sizeof(raw),0)||raw[0]!='#'||slen(raw)!=7u) return -22;
            for(k=0;k<6;k++) { int h=hex(raw[k+1u]); if(h<0) return -22; color_text[k]="0123456789ABCDEF"[h]; } color_text[6]=0;
        }
        context=field(item,"context");
        if(context.p) { while((rc=next_array(context,&cur,&entry))>0) count++; if(rc<0) return rc; }
        if((image && count!=counts[i])||(!image && count && count!=counts[i])) return -22;
        rc=append(d,t,names[i]); if(!rc) rc=append(d,t,image ? "\timage\t":"\ttext\t"); if(!rc) rc=append(d,t,align_text); if(!rc) rc=append(d,t,"\t"); if(!rc) rc=number(d,t,x); if(!rc) rc=append(d,t,"\t"); if(!rc) rc=number(d,t,y); if(!rc) rc=append(d,t,"\t"); if(!rc) rc=number(d,t,size); if(!rc) rc=append(d,t,"\t"); if(!rc) rc=append(d,t,color_text); if(!rc) rc=append(d,t,"\t"); if(!rc) rc=number(d,t,(int32_t)count); if(!rc) rc=append(d,t,"\n"); if(rc) return rc;
        cur=0;
        while(context.p && (rc=next_array(context,&cur,&entry))>0) {
            const char *u; uint32_t code; if(decoded(entry,context_text,sizeof(context_text),0)) return -22;
            u=context_text; while(*u) { if(utf8(&u,context_text+slen(context_text),&code)||code<32u||(code>=127u && code<=159u)) return -22; }
            if(image) {
                char base[80],full[256],path[256]; struct canopus_fs_stat st;
                if(!destination_valid(d,p->meta.id,context_text)||!is_bin(context_text)) return -22;
                if(path_join(base,"themes/",p->meta.id)||path_join(full,base,"/")||path_join(path,full,context_text)) return -22;
                if(check_files) { rc=d->ctx->fs->stat(d->ctx,path,&st); if(rc || st.is_dir) return rc ? rc:-22; }
                rc=append(d,t,path);
            } else { if(slen(context_text)>63u) return -22; rc=append(d,t,context_text); }
            if(!rc) rc=append(d,t,"\n"); if(rc) return rc;
        }
        if(rc<0) return rc;
    }
    return 0;
}
struct compiled_mapping { char *source,*destination; uint32_t theme,group; };
static void free_compiled(struct rh_manager_data *d,struct compiled_mapping *m,uint32_t n) {
    uint32_t i; for(i=0;i<n;i++) { release(d,m[i].source); release(d,m[i].destination); } release(d,m);
}
static int overlaps(const char *a,const char *b) { return equal(a,b)||(directory_source(a)&&prefix(b,a))||(directory_source(b)&&prefix(a,b)); }
static int compile_mappings(struct rh_manager_data *d,struct compiled_mapping **out,uint32_t *out_count) {
    struct compiled_mapping *all=0; uint32_t count=0,i,k,j; int rc=0; struct pack p;
    for(i=0;i<d->order_count;i++) {
        int theme; if(equal(d->order[i],RH_MANAGER_SYSTEM)) break;
        theme=find_id(d->ids,d->count,d->order[i]); if(theme<0) { rc=-22; break; }
        rc=load_pack(d,d->order[i],&p); if(rc) break;
        if(p.count>UINT32_MAX-count) { free_pack(d,&p); rc=-12; break; }
        rc=grow(d,(void **)&all,count,count+p.count,sizeof(*all)); if(rc) { free_pack(d,&p); break; }
        for(k=0;k<p.count;k++) {
            uint32_t group=count;
            all[count]=(struct compiled_mapping){p.maps[k].source,p.maps[k].destination,(uint32_t)theme,count}; p.maps[k].source=p.maps[k].destination=0;
            /* Merge transitive overlap groups without a separate union array. */
            for(j=0;j<count;j++) if(overlaps(all[j].source,all[count].source)) {
                uint32_t old=all[j].group,l; if(old<group) { uint32_t old_group=group; group=old; for(l=0;l<count;l++) if(all[l].group==old_group) all[l].group=group; }
                else for(l=0;l<count;l++) if(all[l].group==old) all[l].group=group;
            }
            all[count].group=group; count++;
        }
        free_pack(d,&p);
    }
    if(rc) { free_compiled(d,all,count); return rc; } *out=all; *out_count=count; return 0;
}
static int line_mapping(struct rh_manager_data *d,struct text *t,const char *source,const char *id,const char *destination) {
    int rc=append(d,t,source); if(!rc) rc=append(d,t,"\t");
    if(!rc && id) { rc=append(d,t,"themes/"); if(!rc) rc=append(d,t,id); if(!rc) rc=append(d,t,"/"); }
    if(!rc) rc=append(d,t,destination); if(!rc) rc=append(d,t,"\n"); return rc;
}
static int overridden(struct rh_manager_data *d,const char *source) {
    const char *choice=stored_choice(d,source); return !equal(choice,RH_MANAGER_DEFAULT);
}
static int candidate_registration(struct rh_manager_data *d,struct path_item *path,uint32_t *theme,const char **destination) {
    uint32_t i,k; const char *choice=stored_choice(d,path->source);
    if(equal(choice,RH_MANAGER_SYSTEM)) return 0;
    if(!equal(choice,RH_MANAGER_DEFAULT)) {
        int selected=find_id(d->ids,d->count,choice); if(selected<0) return -22;
        for(k=0;k<path->count;k++) if(path->options[k].theme==(uint32_t)selected) { *theme=(uint32_t)selected; *destination=path->options[k].destination; return 1; }
        return -22;
    }
    for(i=0;i<d->order_count;i++) {
        int selected; if(equal(d->order[i],RH_MANAGER_SYSTEM)) break; selected=find_id(d->ids,d->count,d->order[i]);
        for(k=0;k<path->count;k++) if(selected>=0 && path->options[k].theme==(uint32_t)selected) { *theme=(uint32_t)selected; *destination=path->options[k].destination; return 1; }
    }
    return 0;
}
static int mkdir_parents(struct rh_manager_data *d,const char *relative_path) {
    char path[256]; uint32_t i; int rc=0; if(!relative(relative_path)||slen(relative_path)>=sizeof(path)) return -22;
    put(path,sizeof(path),relative_path);
    for(i=0;path[i];i++) if(path[i]=='/') { path[i]=0; rc=mkdir_ok(d,path); path[i]='/'; if(rc) return rc; }
    return 0;
}
static int make_overlay_path(struct rh_manager_data *d,char out[256],const char *generation,uint32_t group,const char *suffix) {
    struct text t={0,0,0}; int rc=append(d,&t,"themes/.active-");
    if(!rc) rc=append(d,&t,generation); if(!rc) rc=append(d,&t,"/r"); if(!rc) rc=number(d,&t,(int32_t)group); if(!rc) rc=append(d,&t,"/"); if(!rc) rc=append(d,&t,suffix);
    if(!rc && slen(d->root)+t.n>255u) rc=RH_MANAGER_ERR_LIMIT;
    if(!rc) rc=put(out,256,t.p); release(d,t.p); return rc;
}
static int generate_mappings(struct rh_manager_data *d,struct compiled_mapping *m,uint32_t n,const char *generation,int overlay,struct text *t,uint32_t *rules) {
    uint32_t group,i,k,group_count=0,theme; int rc; const char *destination;
    *rules=0;
    for(group=0;group<n;group++) {
        const char *root=0; uint32_t root_len=UINT32_MAX,first=UINT32_MAX,count=0; int directory=0;
        for(i=0;i<n;i++) if(m[i].group==group) {
            if(first==UINT32_MAX) first=i; count++;
            if(directory_source(m[i].source)) { uint32_t len=slen(m[i].source); directory=1; if(len<root_len) { root=m[i].source; root_len=len; } }
        }
        if(!count) continue;
        if(count==1u || !directory) {
            if(overridden(d,m[first].source)) continue;
            rc=line_mapping(d,t,m[first].source,d->ids[m[first].theme],m[first].destination); if(rc) return rc; (*rules)++; continue;
        }
        if(!root) return -22;
        if(overlay) {
            char overlay_root[256];
            rc=make_overlay_path(d,overlay_root,generation,group_count,""); if(rc) return rc;
            rc=line_mapping(d,t,root,0,overlay_root); if(rc) return rc; (*rules)++;
        }
        for(k=0;k<d->path_count;k++) {
            struct path_item *path=&d->paths[k]; int in_group=0;
            if(!prefix(path->source,root)||overridden(d,path->source)) continue;
            for(i=0;i<n;i++) if(m[i].group==group && (directory_source(m[i].source) ? prefix(path->source,m[i].source):equal(path->source,m[i].source))) { in_group=1; break; }
            if(!in_group) continue;
            rc=candidate_registration(d,path,&theme,&destination); if(rc<0) return rc; if(!rc) continue;
            {
                char from[256],base[80],with_slash[80]; struct canopus_fs_stat st;
                if(path_join(base,"themes/",d->ids[theme])||path_join(with_slash,base,"/")||path_join(from,with_slash,destination)) return -22;
                rc=d->ctx->fs->stat(d->ctx,from,&st); if(rc || st.is_dir || !st.size) return rc ? rc:-22;
            }
            if(!overlay) { rc=line_mapping(d,t,path->source,d->ids[theme],destination); if(rc) return rc; (*rules)++; }
            else {
                char to[256],from[256],base[80],with_slash[80]; struct canopus_fs_stat st;
                rc=make_overlay_path(d,to,generation,group_count,path->source+root_len); if(rc) return rc;
                if(path_join(base,"themes/",d->ids[theme])||path_join(with_slash,base,"/")||path_join(from,with_slash,destination)) return -22;
                rc=d->ctx->fs->stat(d->ctx,from,&st); if(rc || st.is_dir || !st.size) return rc ? rc:-22;
                rc=mkdir_parents(d,to); if(!rc) rc=copy_file(d,from,to); if(rc) return rc;
            }
        }
        group_count++;
    }
    for(i=0;i<d->override_count;i++) {
        struct override *o=&d->overrides[i]; int path;
        if(equal(o->choice,RH_MANAGER_DEFAULT)) continue;
        path=path_index(d,o->key); if(path<0) continue; /* reconcile vanished registrations */
        if(equal(o->choice,RH_MANAGER_SYSTEM)) rc=line_mapping(d,t,o->key,0,RH_MANAGER_SYSTEM);
        else { rc=candidate_registration(d,&d->paths[path],&theme,&destination); if(rc<=0) return rc<0 ? rc:-22; rc=line_mapping(d,t,o->key,d->ids[theme],destination); }
        if(rc) return rc; (*rules)++;
    }
    if(!*rules) return append(d,t,"# No active resource replacements.\n"); return 0;
}
static int feature_winner(struct rh_manager_data *d,uint32_t feature,char id[65]) {
    char key[64],choice[65]; int rc;
    if(path_join(key,"feature:",feature_ids[feature])) return -22;
    rc=resolve_choice(d,key,choice); if(rc) return rc; id[0]=0;
    if(equal(choice,RH_MANAGER_SYSTEM)||equal(choice,RH_MANAGER_BLUR)) return put(id,65,choice);
    if(!equal(choice,RH_MANAGER_DEFAULT)) return put(id,65,choice);
    return winner_for(d,key,id);
}
static int generate_features(struct rh_manager_data *d,struct text *backgrounds,struct text *calendar) {
    uint32_t i; char id[65]; int rc=append(d,backgrounds,"version=1\n");
    for(i=0;!rc && i<4;i++) {
        struct pack p; rc=feature_winner(d,i,id); if(rc) break;
        rc=append(d,backgrounds,background_keys[i]); if(!rc) rc=append(d,backgrounds,"="); if(rc) break;
        if(!id[0]||equal(id,RH_MANAGER_SYSTEM)) rc=append(d,backgrounds,"system\n");
        else if(equal(id,RH_MANAGER_BLUR)) rc=append(d,backgrounds,"blur\n");
        else {
            rc=load_pack(d,id,&p); if(rc) break;
            if(p.backgrounds[i]==3) {
                char base[80],with_slash[80],path[256]; struct canopus_fs_stat st;
                if(path_join(base,"themes/",id)||path_join(with_slash,base,"/")||path_join(path,with_slash,p.images[i])) rc=-22;
                else { rc=d->ctx->fs->stat(d->ctx,path,&st); if(!rc && st.is_dir) rc=-22; if(!rc) rc=append(d,backgrounds,path); if(!rc) rc=append(d,backgrounds,"\n"); }
            } else rc=append(d,backgrounds,p.backgrounds[i]==2 ? "blur\n":"system\n");
            free_pack(d,&p);
        }
    }
    if(rc) return rc;
    rc=feature_winner(d,4,id); if(rc) return rc;
    if(!id[0]||equal(id,RH_MANAGER_SYSTEM)) return calendar_text(d,0,calendar,1);
    { struct pack p; rc=load_pack(d,id,&p); if(!rc) { rc=calendar_text(d,&p,calendar,1); free_pack(d,&p); } }
    return rc;
}
static int reconcile_choices(struct rh_manager_data *d) {
    struct rh_manager_data next=*d;
    uint32_t i,n=0; int rc=0,changed=0; char choice[65],key[64];
    next.overrides=0; next.override_count=0; zero(next.features,sizeof(next.features));
    for(i=0;i<d->override_count;i++) {
        char *k,*c;
        if(path_index(d,d->overrides[i].key)<0) { changed=1; continue; }
        rc=resolve_choice(d,d->overrides[i].key,choice); if(rc) goto done;
        if(!equal(choice,d->overrides[i].choice)) changed=1;
        k=dup(d,d->overrides[i].key); c=dup(d,choice); if(!k||!c) { release(d,k); release(d,c); rc=-12; goto done; }
        rc=grow(d,(void **)&next.overrides,n,n+1u,sizeof(*next.overrides)); if(rc) { release(d,k); release(d,c); goto done; }
        next.overrides[n++]=(struct override){k,c}; next.override_count=n;
    }
    for(i=0;i<5;i++) if(d->features[i]) {
        if(path_join(key,"feature:",feature_ids[i])) { rc=-22; goto done; }
        rc=resolve_choice(d,key,choice); if(rc) goto done;
        if(!equal(choice,d->features[i])) changed=1;
        next.features[i]=dup(d,choice); if(!next.features[i]) { rc=-12; goto done; }
    }
    if(changed) {
        rc=save_overrides(&next); if(rc) goto done;
        for(i=0;i<d->override_count;i++) { release(d,d->overrides[i].key); release(d,d->overrides[i].choice); }
        release(d,d->overrides); for(i=0;i<5;i++) release(d,d->features[i]);
        d->overrides=next.overrides; d->override_count=next.override_count; copy(d->features,next.features,sizeof(d->features));
        next.overrides=0; next.override_count=0; zero(next.features,sizeof(next.features));
    }
done:
    for(i=0;i<next.override_count;i++) { release(d,next.overrides[i].key); release(d,next.overrides[i].choice); }
    release(d,next.overrides); for(i=0;i<5;i++) release(d,next.features[i]); return rc;
}
static int add_unique_id(struct rh_manager_data *d,char ***ids,uint32_t *n,const char *id) {
    char *s; int rc; if(!valid_id(id)) return -22; if(find_id(*ids,*n,id)>=0) return 0;
    s=dup(d,id); if(!s) return -12; rc=grow(d,(void **)ids,*n,*n+1u,sizeof(**ids)); if(rc) release(d,s); else (*ids)[(*n)++]=s; return rc;
}
static int add_reference(struct rh_manager_data *d,char ***ids,uint32_t *n,const char *p,uint32_t bytes) {
    char id[65]; uint32_t i=0;
    if(bytes<7u || !prefix(p,"themes/")) return 0; p+=7; bytes-=7;
    while(i<bytes && p[i]!='/') { if(i>=64u) return -22; id[i]=p[i]; i++; }
    if(i==bytes || !i) return -22; id[i]=0;
    /* Materialized immutable overlays live below themes/.active-<gen>. They
     * depend on their own copied bytes, not a deletable source package. */
    if(prefix(id,".active-")) return 0;
    return add_unique_id(d,ids,n,id);
}
static int collect_references(struct rh_manager_data *d,const char *text,int kind,char ***ids,uint32_t *n) {
    const char *p=text,*line,*end,*value; uint32_t image_lines=0; int rc;
    if(!text) return 0;
    while(*p) {
        line=p; while(*p && *p!='\n') p++; end=p; if(*p) p++;
        if(end>line && end[-1]=='\r') end--;
        if(line==end || *line=='#') continue;
        if(kind==0) {
            value=line; while(value<end && *value!='\t') value++;
            if(value==end) return -22; value++;
            rc=add_reference(d,ids,n,value,(uint32_t)(end-value)); if(rc) return rc;
        } else if(kind==1) {
            value=line; while(value<end && *value!='=') value++; if(value==end) return -22; value++;
            rc=add_reference(d,ids,n,value,(uint32_t)(end-value)); if(rc) return rc;
        } else {
            if(image_lines) { rc=add_reference(d,ids,n,line,(uint32_t)(end-line)); if(rc) return rc; image_lines--; }
            else {
                const char *f[8]; uint32_t i=0,tabs=0,count=0; f[0]=line;
                for(value=line;value<end;value++) if(*value=='\t') { if(++tabs<8u) f[tabs]=value+1; }
                if(tabs!=7u) { if((uint32_t)(end-line)==9u && prefix(line,"version=1")) continue; return -22; }
                value=f[7]; while(value<end) { if(*value<'0'||*value>'9'||count>31u) return -22; count=count*10u+(uint8_t)(*value++-'0'); } if(count>31u) return -22;
                if(f[2]-f[1]==6 && prefix(f[1],"image")) image_lines=count;
                else { /* Skip text contexts without treating path-like labels as references. */
                    for(i=0;i<count;i++) { if(!*p) return -22; while(*p && *p!='\n') p++; if(*p) p++; }
                }
            }
        }
    }
    return image_lines ? -22:0;
}
/* Must match manager_config_store's canonical on-disk grammar. A different
 * spelling must never direct deletion guards or GC to a different config. */
static int canonical_config_directory(const char *name) {
    uint32_t i,value=0,length=slen(name);
    if(length<8u || length>17u || !prefix(name,"active/") || name[7]=='0') return -22;
    for(i=7;i<length;i++) {
        uint32_t digit;
        if(name[i]<'0'||name[i]>'9') return -22;
        digit=(uint32_t)(name[i]-'0'); if(value>(UINT32_MAX-digit)/10u) return -22;
        value=value*10u+digit;
    }
    return value ? 0:-22;
}
static int active_directory(struct rh_manager_data *d,char out[256]) {
    char *text=0; uint32_t n; int rc=read_text(d,"active-config",31,&text,0); out[0]=0;
    if(rc==CANOPUS_SVC_ERR_NOTFOUND) return 0; if(rc) return rc;
    n=slen(text);
    if(!n || text[n-1u]!='\n') rc=-22;
    else { text[--n]=0; rc=canonical_config_directory(text); if(!rc) rc=put(out,256,text); }
    release(d,text); return rc;
}
static int cleanup_config_files(struct rh_manager_data *d,const char *current) {
    static const char *const files[]={"mappings.tsv","backgrounds.cfg","calendar.cfg"};
    void *dir=d->ctx->fs->opendir(d->ctx,"active"); struct canopus_fs_dirent entry; uint32_t i; int rc=0,first_error=0;
    if(!dir) return -5;
    while((rc=d->ctx->fs->readdir(d->ctx,dir,&entry))==0) {
        char candidate[256],base[256],path[256];
        if(!entry.is_dir || equal(entry.name,".") || equal(entry.name,"..")) continue;
        if(path_join(candidate,"active/",entry.name)||canonical_config_directory(candidate)||equal(candidate,current)) continue;
        /* Only known config files are retired. Unknown directory names and
         * unknown files stay intact for diagnosis or future schema owners. */
        if(path_join(base,candidate,"/")) { first_error=-22; break; }
        for(i=0;i<3;i++) {
            int step;
            if(path_join(path,base,files[i])) { first_error=-22; break; }
            step=d->ctx->fs->remove(d->ctx,path);
            if(step && step!=CANOPUS_SVC_ERR_NOTFOUND && !first_error) first_error=step;
        }
    }
    if(rc<0 && !first_error) first_error=rc;
    rc=d->ctx->fs->closedir(d->ctx,dir); if(rc<0 && !first_error) first_error=rc;
    return first_error;
}
static int live_references(struct rh_manager_data *d,char ***ids,uint32_t *n) {
    static const char *const files[]={"mappings.tsv","backgrounds.cfg","calendar.cfg"};
    char directory[256],path[256],base[256],*text=0; uint32_t i; int rc=active_directory(d,directory);
    if(rc) return rc;
    for(i=0;i<3;i++) {
        if(directory[0]) { if(path_join(base,directory,"/")||path_join(path,base,files[i])) return -22; }
        else put(path,sizeof(path),files[i]);
        rc=read_text(d,path,i==0u ? 32768u:65536u,&text,0);
        if(rc==CANOPUS_SVC_ERR_NOTFOUND && !directory[0]) continue;
        if(rc) return rc;
        rc=collect_references(d,text,(int)i,ids,n); release(d,text); text=0; if(rc) return rc;
    }
    return 0;
}
static int protection_index(struct rh_manager_data *d,char ***ids,uint32_t *n,char ***generations,uint32_t *gn) {
    char *text=0,**protected_ids=0,**gens=0; uint32_t pn=0,gcount=0,i; struct json j,f; int64_t version; int rc;
    rc=read_text(d,"resource-active-generations.json",UINT32_MAX-1u,&text,&j);
    if(rc==CANOPUS_SVC_ERR_NOTFOUND) return 0; if(rc) return rc;
    if(integer(field(j,"version"),&version)||version!=1) { rc=-22; goto done; }
    rc=string_list(d,field(j,"generations"),&gens,&gcount,0); if(rc) goto done;
    for(i=0;i<gcount;i++) if(slen(gens[i])>32u) { rc=-22; goto done; }
    f=field(j,"protectedThemes"); if(f.p) { rc=string_list(d,f,&protected_ids,&pn,0); if(rc) goto done; }
    for(i=0;i<pn;i++) { rc=add_unique_id(d,ids,n,protected_ids[i]); if(rc) goto done; }
    if(generations) { *generations=gens; *gn=gcount; gens=0; gcount=0; }
    rc=0;
done:
    free_strings(d,protected_ids,pn); free_strings(d,gens,gcount); release(d,text); return rc;
}
static int serialize_protection(struct rh_manager_data *d,struct text *t,char **ids,uint32_t n,char **gens,uint32_t gn) {
    int rc=append(d,t,"{\"version\":1,\"generations\":"); if(!rc) rc=serialize_ids(d,t,gens,gn); if(!rc) rc=append(d,t,",\"protectedThemes\":"); if(!rc) rc=serialize_ids(d,t,ids,n); if(!rc) rc=append(d,t,"}"); return rc;
}
int rh_manager_data_prepare_apply(struct rh_manager_data *d,const char *config_dir) {
    struct compiled_mapping *maps=0; uint32_t map_count=0,rules=0,i,reference_n=0,previous_n=0,gn=0; char **refs=0,**previous=0,**gens=0;
    struct text mappings={0,0,0},backgrounds={0,0,0},calendar={0,0,0},protection={0,0,0},candidate={0,0,0};
    const char *generation; char path[256],base[256]; int rc,overlay=0;
    if(!d||!config_dir||canonical_config_directory(config_dir)) return -22;
    if(d->busy||d->pending_protection) return -16; generation=config_dir+7; d->busy=1;
    { struct canopus_fs_stat existing;
      rc=d->ctx->fs->stat(d->ctx,config_dir,&existing); }
    if(!rc) { rc=CANOPUS_SVC_ERR_EXIST; goto done; }
    if(rc!=CANOPUS_SVC_ERR_NOTFOUND) goto done;
    rc=ensure_catalog(d); if(rc) goto done;
    rc=reconcile_choices(d); if(rc) goto done;
    rc=compile_mappings(d,&maps,&map_count); if(rc) goto done;
    rc=generate_mappings(d,maps,map_count,generation,0,&mappings,&rules); if(rc) goto done;
    if(rules>256u || mappings.n>32768u) { release(d,mappings.p); zero(&mappings,sizeof(mappings)); overlay=1;
        char overlay_directory[256];
        rc=make_overlay_path(d,overlay_directory,generation,0,""); if(rc) goto done;
        /* Reserve the generation parent before materialization; never rewrite
         * bytes that might still be held by a live decoder. */
        { uint32_t separator=slen(overlay_directory); while(separator && overlay_directory[separator-1u]!='/') separator--; if(separator) separator--; while(separator && overlay_directory[separator-1u]!='/') separator--; if(!separator) { rc=-22; goto done; } overlay_directory[separator-1u]=0; }
        rc=d->ctx->fs->mkdir(d->ctx,overlay_directory); if(rc) goto done;
        rc=generate_mappings(d,maps,map_count,generation,1,&mappings,&rules); if(rc) goto done;
        if(rules>256u || mappings.n>32768u) { rc=RH_MANAGER_ERR_LIMIT; goto done; }
    }
    rc=generate_features(d,&backgrounds,&calendar); if(rc) goto done;
    rc=collect_references(d,mappings.p,0,&refs,&reference_n); if(!rc) rc=collect_references(d,backgrounds.p,1,&refs,&reference_n); if(!rc) rc=collect_references(d,calendar.p,2,&refs,&reference_n); if(rc) goto done;
    rc=protection_index(d,&previous,&previous_n,&gens,&gn); if(!rc) rc=live_references(d,&previous,&previous_n); if(rc) goto done;
    if(overlay) { rc=add_unique_id(d,&gens,&gn,generation); if(rc) goto done; }
    rc=serialize_protection(d,&candidate,refs,reference_n,gens,gn); if(rc) goto done;
    for(i=0;i<reference_n;i++) { rc=add_unique_id(d,&previous,&previous_n,refs[i]); if(rc) goto done; }
    rc=serialize_protection(d,&protection,previous,previous_n,gens,gn); if(rc) goto done;
    rc=atomic_text(d,"resource-active-generations.json",protection.p,protection.n); if(rc) goto done;
    rc=mkdir_ok(d,"active"); if(rc) goto done;
    /* Config generations are immutable. A collision is never overwritten. */
    rc=d->ctx->fs->mkdir(d->ctx,config_dir); if(rc) goto done;
    if(path_join(base,config_dir,"/")||path_join(path,base,"mappings.tsv")) { rc=-22; goto done; }
    rc=write_text(d,path,mappings.p,mappings.n); if(rc) goto done;
    if(path_join(path,base,"backgrounds.cfg")) { rc=-22; goto done; } rc=write_text(d,path,backgrounds.p,backgrounds.n); if(rc) goto done;
    if(path_join(path,base,"calendar.cfg")) { rc=-22; goto done; } rc=write_text(d,path,calendar.p,calendar.n); if(rc) goto done;
    d->pending_protection=candidate.p; candidate.p=0;
    put(d->pending_overlay,sizeof(d->pending_overlay),overlay ? generation:"");
    put(d->pending_config,sizeof(d->pending_config),config_dir);
done:
    free_compiled(d,maps,map_count); free_strings(d,refs,reference_n); free_strings(d,previous,previous_n); free_strings(d,gens,gn);
    release(d,mappings.p); release(d,backgrounds.p); release(d,calendar.p); release(d,protection.p); release(d,candidate.p); d->busy=0;
    return rc ? fail(d,rc,"活动资源配置生成失败，未发布新代次"):0;
}
int rh_manager_data_complete_apply(struct rh_manager_data *d,int success) {
    int rc=0,cleanup_rc=0; char **ids=0,**generations=0,**retained=0; uint32_t n=0,gn=0,rn=0,i; struct text index={0,0,0}; struct json j; char *pending=0;
    if(!d) return -22; if(!d->pending_protection) return -16;
    if(success) {
        char current[256];
        rc=active_directory(d,current);
        if(!rc && !equal(current,d->pending_config)) rc=CANOPUS_SVC_ERR_STATE;
        /* An invalid or changed pointer leaves the protection union and all
         * old generations intact. Only the matching published generation ACK
         * authorizes config and decoder backing-file retirement. */
        if(!rc) rc=cleanup_config_files(d,current);
        /* Acknowledged refresh released the old decoder references. Reclaim
         * obsolete overlay file bytes, retaining only empty directory shells
         * unless a full remove_tree adapter is supplied. */
        if(!rc) { pending=dup(d,d->pending_protection); if(!pending) rc=-12; }
        if(!rc && parse(pending,slen(pending),&j)) rc=-22;
        if(!rc) rc=string_list(d,field(j,"protectedThemes"),&ids,&n,0);
        if(!rc) rc=string_list(d,field(j,"generations"),&generations,&gn,0);
        for(i=0;!rc && i<gn;i++) {
            int keep=equal(generations[i],d->pending_overlay),step=0;
            if(!keep) {
                char directory[80],absolute[256]; struct canopus_fs_stat st;
                if(path_join(directory,".active-",generations[i])) { rc=-22; break; }
                { char relative_path[96]; if(path_join(relative_path,"themes/",directory)||path_join(absolute,d->root,relative_path)) { rc=-22; break; }
                  step=d->ctx->fs->stat(d->ctx,relative_path,&st); }
                if(step==CANOPUS_SVC_ERR_NOTFOUND) step=0;
                else if(!step) step=d->hooks.remove_tree ? d->hooks.remove_tree(d->hooks.cookie,absolute):walk_theme(d,directory,0,0,1);
                if(step) { keep=1; if(!cleanup_rc) cleanup_rc=step; }
            }
            if(keep) rc=add_unique_id(d,&retained,&rn,generations[i]);
        }
        if(!rc) rc=serialize_protection(d,&index,ids,n,retained,rn);
        if(!rc) rc=atomic_text(d,"resource-active-generations.json",index.p,index.n);
    }
    free_strings(d,ids,n); free_strings(d,generations,gn); free_strings(d,retained,rn); release(d,index.p); release(d,pending);
    release(d,d->pending_protection); d->pending_protection=0; d->pending_overlay[0]=0; d->pending_config[0]=0;
    if(!rc) rc=cleanup_rc;
    return rc ? fail(d,rc,"刷新成功，但旧代次清理或活动保护记录保存失败"):0;
}
static int remove_inventory_entry(struct rh_manager_data *d,const char *id) {
    char *doc=0; struct json j,themes,k,v; const char *p; struct text t={0,0,0}; uint32_t n=0; int64_t version; int rc;
    rc=read_text(d,"resource-files.json",2u*1024u*1024u,&doc,&j); if(rc==CANOPUS_SVC_ERR_NOTFOUND) return 0; if(rc) return rc;
    if(integer(field(j,"version"),&version)||version!=1) { rc=-22; goto done; }
    themes=field(j,"themes"); if(!themes.p || *themes.p!='{') { rc=-22; goto done; }
    rc=append(d,&t,"{\"version\":1,\"themes\":{"); p=ws(themes.p+1,themes.e);
    while(!rc && p<themes.e && *p!='}') {
        k.p=p; if(scan_string(&p,themes.e)) { rc=-22; break; } k.e=p;
        p=ws(p,themes.e)+1; p=ws(p,themes.e); v.p=p; if(scan_value(&p,themes.e,0)) { rc=-22; break; } v.e=p;
        if(!jeq(k,id)) { if(n++) rc=append(d,&t,","); if(!rc) rc=append_n(d,&t,k.p,(uint32_t)(k.e-k.p)); if(!rc) rc=append(d,&t,":"); if(!rc) rc=append_n(d,&t,v.p,(uint32_t)(v.e-v.p)); }
        p=ws(p,themes.e); if(*p==',') p=ws(p+1,themes.e); else break;
    }
    if(!rc) rc=append(d,&t,"}}"); if(!rc) rc=atomic_text(d,"resource-files.json",t.p,t.n);
done:
    release(d,doc); release(d,t.p); return rc;
}
int rh_manager_data_delete(struct rh_manager_data *d,const char *id) {
    char **protected_ids=0; uint32_t n=0,i; int rc; struct text registry={0,0,0}; char relative_root[80],absolute[256];
    if(!d||!id||!valid_id(id)||find_id(d->ids,d->count,id)<0) return -22;
    if(d->busy||d->pending_protection) return -16;
    rc=protection_index(d,&protected_ids,&n,0,0); if(!rc) rc=live_references(d,&protected_ids,&n);
    if(!rc && find_id(protected_ids,n,id)>=0) rc=RH_MANAGER_ERR_ACTIVE;
    free_strings(d,protected_ids,n); if(rc) return fail(d,rc,rc==RH_MANAGER_ERR_ACTIVE ? "正在使用的资源包不能删除":"活动资源引用记录不可读取");
    d->busy=1;
    if(path_join(relative_root,"themes/",id)||path_join(absolute,d->root,relative_root)) { rc=-22; goto done; }
    {
        struct canopus_fs_stat st;
        rc=d->ctx->fs->stat(d->ctx,relative_root,&st);
        if(rc==CANOPUS_SVC_ERR_NOTFOUND) rc=0;
        else if(!rc && !st.is_dir) rc=-20;
        else if(!rc) rc=d->hooks.remove_tree ? d->hooks.remove_tree(d->hooks.cookie,absolute):walk_theme(d,id,0,0,1);
    }
    if(rc) goto done;
    /* Preserve original partial-failure semantics: after an I/O failure no
     * success is reported; a damaged still-registered package remains visible.
     * Empty shells are intentionally retained when only fs.remove is present. */
    for(i=0;i<d->override_count;i++) if(equal(d->overrides[i].choice,id)) { char *choice=dup(d,RH_MANAGER_DEFAULT); if(!choice) { rc=-12; goto done; } release(d,d->overrides[i].choice); d->overrides[i].choice=choice; }
    for(i=0;i<5;i++) if(d->features[i] && equal(d->features[i],id)) { char *choice=dup(d,RH_MANAGER_DEFAULT); if(!choice) { rc=-12; goto done; } release(d,d->features[i]); d->features[i]=choice; }
    rc=save_overrides(d); if(rc) goto done;
    rc=append(d,&registry,"[");
    { uint32_t at=0; for(i=0;!rc && i<d->count;i++) if(!equal(d->ids[i],id)) { if(at++) rc=append(d,&registry,","); if(!rc) rc=quoted(d,&registry,d->ids[i]); } }
    if(!rc) rc=append(d,&registry,"]");
    if(!rc) rc=atomic_text(d,"interconnect-themes.json",registry.p,registry.n); if(rc) goto done;
    rc=remove_inventory_entry(d,id);
    /* refresh normalizes order after native registry commit, even when an
     * optional inventory cleanup failed. Never retain a catalog of old files. */
    d->busy=0; { int refresh_rc=rh_manager_data_refresh(d); if(!rc) rc=refresh_rc; } d->busy=1;
done:
    d->busy=0; clear_catalog(d); release(d,registry.p);
    return rc ? fail(d,rc,"资源包删除未完成，请重试"):0;
}

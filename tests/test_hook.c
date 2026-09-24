#include "resource_hook.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static int calls, missing;
static char last[RH_PATH];
static int original(void *d, const char *p, int m) {
    (void)d; (void)m; calls++;
    if (p) {strncpy(last,p,sizeof(last)-1);last[sizeof(last)-1]=0;}
    if (missing && p && strstr(p,"/themes/")) return missing == 1 ? 0 : -1;
    return 1;
}
static int wrapper(void *d,const char *p,int m) {(void)d;(void)p;(void)m;return 0;}
struct reader { const char *text; uint32_t size, offset, chunk; int error; };
static int read_chunk(void *cookie, void *out, uint32_t size) {
    struct reader *r = cookie;
    uint32_t n = r->size-r->offset;
    if (r->error && r->offset >= r->chunk) return -1;
    if (n > size) n = size;
    if (n > r->chunk) n = r->chunk;
    memcpy(out, r->text+r->offset, n); r->offset += n;
    return (int)n;
}
int main(void) {
    static struct rh_state s;
    static struct rh_rule rules[RH_RULES];
    char out[RH_PATH], before[RH_PATH];
    rh_open_fn slot=original;
    strcpy(rules[0].source,"/resource/");strcpy(rules[0].destination,RH_THEME_ROOT "base/");
    strcpy(rules[1].source,"/resource/icons/");strcpy(rules[1].destination,RH_THEME_ROOT "icons/");
    assert(rh_configure(&s,rules,2)==0);
    assert(rh_resolve(&s,"/resource/icons/sub/a.bin",out)==1);
    assert(!strcmp(out,RH_THEME_ROOT "icons/sub/a.bin"));
    assert(rh_resolve(&s,"/resource/icons-other/a.bin",out)==1);
    assert(!strcmp(out,RH_THEME_ROOT "base/icons-other/a.bin"));
    assert(rh_resolve(&s,"/resource2/a",out)==0);
    assert(rh_resolve(&s,"/resource/../x",out)<0);
    assert(rh_resolve(&s,"/resource//x",out)<0);
    assert(rh_resolve(&s,"/resource/./x",out)<0);
    memset(out,'x',sizeof(out));out[0]='/';assert(rh_resolve(&s,out,before)<0);
    strcpy(before,s.rules[0].destination);
    strcpy(rules[0].destination,RH_THEME_ROOT "new/");
    strcpy(rules[1].destination,"/bad/");
    assert(rh_configure(&s,rules,2)<0);
    assert(!strcmp(before,s.rules[0].destination));
    assert(s.count==2);
    assert(rh_install(&s,&s,&slot,wrapper)==0);
    assert(rh_reinstall_posix(&s,&s,&slot,original,wrapper)==0);
    s.original=0;
    assert(rh_reinstall_posix(&s,&s,&slot,original,wrapper)<0);
    assert(slot==wrapper);
    s.original=original;
    slot=original; s.installed=0;
    assert(rh_reinstall_posix(&s,&s,&slot,original,wrapper)==0);
    assert(slot==wrapper);
    assert(rh_open(&s,&s,"/resource/icons/a.bin",2)==1);
    assert(!strcmp(last,RH_THEME_ROOT "icons/a.bin"));
    for(missing=1;missing<=2;missing++) {
        calls=0;assert(rh_open(&s,&s,"/resource/a",2)==1);assert(calls==2);
        assert(!strcmp(last,"/resource/a"));
    }
    calls=0;rh_open(&s,&s,"/resource/a",3);assert(calls==1);
    assert(!strcmp(last,"/resource/a"));
    missing=0; calls=0;
    assert(rh_posix_open(&s,&s,"resource/icons/a.bin",2)==1);
    assert(calls==1&&!strcmp(last,"data/quickapp/files/ng.lst.corona/themes/icons/a.bin"));
    for (missing=1;missing<=2;missing++) {
        calls=0;assert(rh_posix_open(&s,&s,"resource/a",2)==1);
        assert(calls==2&&!strcmp(last,"resource/a"));
    }
    missing=0;calls=0;
    assert(rh_posix_open(&s,&s,"resource/a",3)==1);
    assert(calls==1&&!strcmp(last,"resource/a"));
    assert(rh_configure(&s,rules,0)<0);
    {
        const char text[] = "# comment\n/resource/\t" RH_THEME_ROOT "current/\r\n";
        uint32_t count=99;
        assert(rh_parse_config(text,sizeof(text)-1,rules,RH_RULES,&count)==0);
        assert(count==1);
        s.installed=0;assert(rh_configure(&s,rules,count)==0);
        count=99;
        assert(rh_parse_config("bad",3,rules,RH_RULES,&count)<0&&count==99);
        assert(rh_parse_config(text,sizeof(text)-1,rules,0,&count)<0);
        assert(rh_parse_config("x\0y",3,rules,RH_RULES,&count)<0);
    }
    {
        const char text[] = "/resource/\t" RH_THEME_ROOT "loaded/\n";
        char buffer[sizeof(text)];
        struct reader r = {text,sizeof(text)-1,0,3,0};
        assert(rh_read_config(&s,read_chunk,&r,buffer,sizeof(buffer),rules)==0);
        assert(!strcmp(s.rules[0].destination,RH_THEME_ROOT "loaded/"));
        strcpy(before,s.rules[0].destination);
        r.offset=0; r.error=1;
        assert(rh_read_config(&s,read_chunk,&r,buffer,sizeof(buffer),rules)<0);
        assert(!strcmp(before,s.rules[0].destination));
        r.offset=0; r.error=0;
        assert(rh_read_config(&s,read_chunk,&r,buffer,sizeof(text)-2,rules)<0);
        assert(!strcmp(before,s.rules[0].destination));
        r.offset=0;
        assert(rh_read_config(&s,read_chunk,&r,buffer,sizeof(text)-1,rules)==0);
        s.installed=1;r.offset=0;
        assert(rh_read_config(&s,read_chunk,&r,buffer,sizeof(buffer),rules)<0);
        assert(r.offset==0);
    }
    puts("prefix, boundary, transactional configuration, parser, reader and fallback tests passed");
    return 0;
}

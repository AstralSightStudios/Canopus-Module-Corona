/* Host-only protocol experiment; this is not the device receiver. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include "sha256.h"

#define FRAME 480u
#define BUFFER 4096u
#define TOKEN 0x12345678u
struct receiver {
    uint32_t received, written, durable, total, used, error;
    uint32_t writes, syncs, checkpoint_writes, read_calls;
    uint32_t short_limit, fail_after, checkpoint_bytes;
    int fd, state_fd;
    unsigned char data[BUFFER];
};
struct work {
    unsigned char frame[FRAME];
    union {
        unsigned char reply[128];
        SHA256_CTX hash;
    } phase;
};
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void put32(unsigned char *p, uint32_t v) {
    for (unsigned i=0;i<4;i++) p[i]=(unsigned char)(v>>(i*8));
}
static double now_ms(void) {
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return t.tv_sec*1000.0+t.tv_nsec/1000000.0;
}
static void checkpoint(struct receiver *r) {
    if (r->written==r->durable) return;
    assert(fsync(r->fd)==0); r->syncs++;
    unsigned char record[16]={0};
    put32(record,r->written); put32(record+4,TOKEN);
    put32(record+8,(uint32_t)crc32(0,record,8));
    assert(pwrite(r->state_fd,record,sizeof(record),0)==sizeof(record));
    r->checkpoint_writes++;
    assert(fsync(r->state_fd)==0); r->syncs++;
    r->durable=r->written;
}
static int flush(struct receiver *r) {
    uint32_t done=0;
    while (done<r->used) {
        uint32_t n=r->used-done;
        if (r->short_limit && n>r->short_limit) n=r->short_limit;
        if (r->fail_after && done>=r->fail_after) { r->error=1; return -1; }
        ssize_t wrote=pwrite(r->fd,r->data+done,n,r->written+done);
        assert(wrote>0 && (uint32_t)wrote<=n); r->writes++; done+=(uint32_t)wrote;
    }
    r->written+=r->used; r->used=0;
    if (r->checkpoint_bytes && r->written-r->durable>=r->checkpoint_bytes) checkpoint(r);
    return 0;
}
/* Compact experiment uses opcode/reserved/seq/token/offset. Old hot path uses CRN1. */
static void encode(unsigned char *p, unsigned header, uint32_t offset, uint16_t seq) {
    memset(p,0,header);
    if (header==12) { p[0]=3; put32(p+4,TOKEN); put32(p+8,offset); }
    else { memcpy(p,"CRN1",4); p[4]=1; p[5]=3; p[6]=4; put32(p+8,seq); put32(p+16,TOKEN); put32(p+24,offset); }
}
static int receive(struct receiver *r, const unsigned char *p, uint32_t n, unsigned header, int buffered) {
    uint32_t off,token;
    if (r->error) return -1;
    if (n<=header || n>FRAME) return -2;
    if (header==12) {
        if (p[0]!=3 || p[1]!=0) return -2;
        token=u32(p+4); off=u32(p+8);
    } else {
        if (memcmp(p,"CRN1",4) || p[4]!=1 || p[5]!=3) return -2;
        token=u32(p+16); off=u32(p+24);
    }
    uint32_t len=n-header;
    if (token!=TOKEN || off>r->received || len>r->total || off>r->total-len) return -2;
    /* Only cumulative progress is retained; final disk SHA detects conflicting replay. */
    if (off<r->received) return off+len<=r->received ? 0 : -2;
    if (r->used+len>BUFFER && flush(r)) return -1;
    memcpy(r->data+r->used,p+header,len); r->used+=len; r->received+=len;
    return buffered ? 0 : flush(r);
}
static uint32_t reply(struct receiver *r, unsigned header, int json, unsigned char *p) {
    assert(flush(r)==0);
    memset(p,0,128);
    if (json) {
        int n=snprintf((char *)p+header,128-header,
            "{\"phase\":\"receiving\",\"acceptedOffset\":%u,\"durableOffset\":%u}",r->written,r->durable);
        assert(n>0 && (unsigned)n<128-header); return header+(uint32_t)n;
    }
    p[0]=0x84; put32(p+4,TOKEN); put32(p+8,r->written); put32(p+12,r->durable);
    p[16]=8; p[18]=1; /* Advertised frame credit and receiving phase. */
    return 20;
}
static void disk_hash(struct receiver *r, struct work *w, unsigned char result[32]) {
    assert(r->used==0);
    /* Verification reuses the drained upload buffer instead of allocating another 4 KiB. */
    sha256_init(&w->phase.hash);
    uint32_t off=0;
    while (off<r->total) {
        uint32_t n=r->total-off; if(n>BUFFER)n=BUFFER;
        ssize_t got=pread(r->fd,r->data,n,off); assert(got>0);
        r->read_calls++; sha256_update(&w->phase.hash,r->data,(size_t)got); off+=(uint32_t)got;
    }
    sha256_final(&w->phase.hash,result);
}
static void input_hash(FILE *f, unsigned char result[32]) {
    SHA256_CTX hash; unsigned char buf[BUFFER]; size_t n;
    sha256_init(&hash); rewind(f);
    while((n=fread(buf,1,sizeof(buf),f))) sha256_update(&hash,buf,n);
    sha256_final(&hash,result); rewind(f);
}
static void init(struct receiver *r,uint32_t size,int fd,int statefd) {
    memset(r,0,sizeof(*r)); r->total=size; r->fd=fd; r->state_fd=statefd;
}
static void selftest(void) {
    FILE *f=tmpfile(),*state=tmpfile(); assert(f && state);
    struct receiver r; struct work w; init(&r,20000,fileno(f),fileno(state));
    unsigned char expected[1872]; for(unsigned i=0;i<sizeof(expected);i++)expected[i]=(unsigned char)i;
    for(unsigned i=0;i<4;i++) {
        encode(w.frame,12,i*468,(uint16_t)i); memcpy(w.frame+12,expected+i*468,468);
        assert(receive(&r,w.frame,480,12,1)==0);
    }
    assert(r.written==0 && r.received==1872);
    /* Lost query ACK: repeat query and delayed DATA must neither append nor duplicate. */
    reply(&r,12,0,w.phase.reply); uint32_t writes=r.writes;
    assert(receive(&r,w.frame,480,12,1)==0); reply(&r,12,0,w.phase.reply);
    assert(r.written==1872 && r.writes==writes);
    /* Gap, wrong task and malformed length are rejected. */
    encode(w.frame,12,2000,5); assert(receive(&r,w.frame,480,12,1)==-2);
    encode(w.frame,12,1872,5); put32(w.frame+4,1); assert(receive(&r,w.frame,480,12,1)==-2);
    assert(receive(&r,w.frame,11,12,1)==-2);
    /* A dropped input frame blocks later offsets; retry the suffix from QUERY offset. */
    encode(w.frame,12,2340,6); assert(receive(&r,w.frame,480,12,1)==-2);
    encode(w.frame,12,1872,5); memset(w.frame+12,7,468); assert(receive(&r,w.frame,480,12,1)==0);
    encode(w.frame,12,2340,6); assert(receive(&r,w.frame,480,12,1)==0);
    r.short_limit=37; reply(&r,12,0,w.phase.reply); assert(r.written==2808);
    checkpoint(&r); assert(r.durable==2808);
    /* Process reboot discards buffered and uncheckpointed progress. */
    encode(w.frame,12,2808,7); assert(receive(&r,w.frame,480,12,1)==0);
    uint32_t durable=r.durable; init(&r,20000,fileno(f),fileno(state));
    r.received=r.written=r.durable=durable;
    assert(r.received==2808);
    /* Partial pwrite failure must latch: later frames cannot advance the broken batch. */
    encode(w.frame,12,2808,7); assert(receive(&r,w.frame,480,12,1)==0);
    r.short_limit=37; r.fail_after=37; assert(flush(&r)==-1); assert(r.written==2808);
    assert(receive(&r,w.frame,480,12,1)==-1);
    /* In-memory credit exhaustion is bounded even if a sender exceeds the batch. */
    init(&r,20000,fileno(f),fileno(state));
    for(unsigned i=0;i<20;i++) {
        encode(w.frame,12,i*468,(uint16_t)i); assert(receive(&r,w.frame,480,12,1)==0); assert(r.used<=BUFFER);
    }
    /* Same length, wrong bytes: disk SHA must reject rather than "install". */
    r.total=r.received; assert(flush(&r)==0); unsigned char before[32],after[32];
    disk_hash(&r,&w,before); unsigned char changed=0xa5; assert(pwrite(r.fd,&changed,1,8)==1);
    disk_hash(&r,&w,after); assert(memcmp(before,after,32)!=0);
    fclose(f); fclose(state); puts("{\"fault_checks\":10,\"passed\":true}");
}
struct allocation { size_t size; };
struct heap { size_t live,peak; };
static voidpf tracked_alloc(voidpf opaque,uInt items,uInt size) {
    struct heap *h=opaque; size_t n=(size_t)items*size;
    struct allocation *a=malloc(sizeof(*a)+n); assert(a); a->size=n;
    h->live+=n; if(h->live>h->peak)h->peak=h->live; return a+1;
}
static void tracked_free(voidpf opaque,voidpf p) {
    if(!p)return; struct heap *h=opaque; struct allocation *a=(struct allocation *)p-1;
    h->live-=a->size; free(a);
}
static void inflate_bench(const char *path, int window_bits) {
    FILE *f=fopen(path,"rb"); assert(f);
    struct heap heap={0}; z_stream z={0}; z.zalloc=tracked_alloc; z.zfree=tracked_free; z.opaque=&heap;
    assert(inflateInit2(&z,window_bits)==Z_OK);
    unsigned char in[1024],out[4096]; size_t input=0,output=0; int rc=Z_OK;
    double start=now_ms();
    while(rc!=Z_STREAM_END) {
        if(!z.avail_in) {size_t n=fread(in,1,sizeof(in),f); assert(n); input+=n; z.next_in=in; z.avail_in=(uInt)n;}
        z.next_out=out; z.avail_out=sizeof(out); rc=inflate(&z,Z_NO_FLUSH);
        if(rc!=Z_OK && rc!=Z_STREAM_END) break;
        output+=sizeof(out)-z.avail_out;
    }
    double ms=now_ms()-start; assert(inflateEnd(&z)==Z_OK); assert(heap.live==0); fclose(f);
    printf("{\"input_bytes\":%zu,\"output_bytes\":%zu,\"codec_heap_peak\":%zu,\"io_buffers\":%zu,\"zstream_bytes\":%zu,\"host_ms\":%.4f,\"stream_end\":%s,\"window_bits\":%d}\n",
        input,output,heap.peak,sizeof(in)+sizeof(out),sizeof(z),ms,rc==Z_STREAM_END ? "true" : "false",window_bits);
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--selftest")) {selftest();return 0;}
    if(argc==3 && !strcmp(argv[1],"--inflate")) {inflate_bench(argv[2],-15);return 0;}
    if(argc==3 && !strcmp(argv[1],"--inflate-small")) {inflate_bench(argv[2],-13);return 0;}
    if(argc!=8) {fprintf(stderr,"usage: bench file header json group buffer checkpoint verify\n");return 2;}
    unsigned header=(unsigned)atoi(argv[2]),json=(unsigned)atoi(argv[3]),group=(unsigned)atoi(argv[4]);
    int buffered=atoi(argv[5]),verify=atoi(argv[7]); unsigned cp=(unsigned)strtoul(argv[6],0,10);
    assert((header==12 || header==32) && group && group<=32);
    FILE *input=fopen(argv[1],"rb"),*output=tmpfile(),*state=tmpfile(); assert(input && output && state);
    assert(fseek(input,0,SEEK_END)==0); long size=ftell(input); assert(size>0 && size<UINT32_MAX); rewind(input);
    struct receiver r; struct work w; init(&r,(uint32_t)size,fileno(output),fileno(state)); r.checkpoint_bytes=cp;
    unsigned char expected[32],actual[32]; input_hash(input,expected);
    uint32_t frames=0,acks=0,reply_bytes=0; uint64_t data_bytes=0; double start=now_ms();
    while(r.received<r.total) {
        uint32_t off=r.received; size_t n=fread(w.frame+header,1,FRAME-header,input); assert(n);
        encode(w.frame,header,off,(uint16_t)frames); assert(receive(&r,w.frame,(uint32_t)n+header,header,buffered)==0);
        frames++; data_bytes+=header+n;
        if(frames%group==0 || r.received==r.total) {reply_bytes+=reply(&r,header,(int)json,w.phase.reply); acks++;}
    }
    assert(r.written==r.total); checkpoint(&r); double write_ms=now_ms()-start;
    start=now_ms(); if(verify) {disk_hash(&r,&w,actual);assert(memcmp(expected,actual,32)==0);} double verify_ms=now_ms()-start;
    /* Count only explicitly owned objects; excludes stack frames and libc/OS state. */
    size_t tx_memory=sizeof(r)+FRAME+128;
    size_t peak_memory=sizeof(r)+sizeof(w);
    printf("{\"bytes\":%u,\"frames\":%u,\"acks\":%u,\"data_wire_bytes\":%llu,\"reply_wire_bytes\":%u,\"payload_writes\":%u,\"checkpoint_writes\":%u,\"syncs\":%u,\"verify_reads\":%u,\"receiver_state_bytes\":%zu,\"upload_owned_bytes\":%zu,\"verify_owned_peak_bytes\":%zu,\"host_write_ms\":%.4f,\"host_verify_ms\":%.4f}\n",
        r.total,frames,acks,(unsigned long long)data_bytes,reply_bytes,r.writes,r.checkpoint_writes,r.syncs,r.read_calls,sizeof(r),tx_memory,peak_memory,write_ms,verify_ms);
    fclose(input);fclose(output);fclose(state);return 0;
}

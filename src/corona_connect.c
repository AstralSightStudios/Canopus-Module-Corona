#include "corona_connect.h"
#include "canopus_memory.h"

#define LIMIT_BYTES (64u * 1024u * 1024u)
#define CHECKPOINT_BYTES 65536u
#define WORK_BYTES 4096u
#define RECORD_BYTES 128u
#define HELLO 1u
#define OPEN 2u
#define DATA 3u
#define QUERY 4u
#define FINISH 5u
#define CANCEL 6u
#define LIST 7u
#define BAD 1u
#define BUSY 2u
#define STALE 3u
#define OFFSET 4u
#define IO 5u
#define HASH 6u
#define LIMIT 8u
#define ACTIVE 9u
#define UNSUPPORTED 11u

/* SHA-256, FIPS 180-4. Input is read back from the archive, never the RX stream.
 * Length is bounded to 64 MiB, so a 32-bit byte count suffices. */
struct sha { uint32_t h[8], bytes, used; uint8_t block[64]; };
static uint32_t rotr(uint32_t a,unsigned n) { return (a>>n)|(a<<(32u-n)); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static void sha_block(struct sha *s,const uint8_t *p) {
    static const uint32_t k[64]={
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
    uint32_t w[64],a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7],i;
    for(i=0;i<16u;i++) w[i]=be32(p+4u*i);
    for(;i<64u;i++) {
        uint32_t x=w[i-15u],y=w[i-2u];
        w[i]=w[i-16u]+(rotr(x,7)^rotr(x,18)^(x>>3))+w[i-7u]+(rotr(y,17)^rotr(y,19)^(y>>10));
    }
    for(i=0;i<64u;i++) {
        uint32_t t=h+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^((~e)&g))+k[i]+w[i];
        uint32_t u=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void sha_init(struct sha *s) {
    static const uint32_t initial[]={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    canopus_memcpy(s->h,initial,sizeof(initial));s->bytes=s->used=0;
}
static void sha_update(struct sha *s,const uint8_t *p,uint32_t n) {
    s->bytes+=n;
    while(n--) { s->block[s->used++]=*p++;if(s->used==64u) { sha_block(s,s->block);s->used=0; } }
}
static void sha_final(struct sha *s,uint8_t out[32]) {
    uint32_t bits=s->bytes*8u,i;s->block[s->used++]=0x80;
    if(s->used>56u) { while(s->used<64u)s->block[s->used++]=0;sha_block(s,s->block);s->used=0; }
    while(s->used<60u)s->block[s->used++]=0;
    for(i=0;i<4u;i++)s->block[60u+i]=(uint8_t)(bits>>(24u-8u*i));
    sha_block(s,s->block);
    for(i=0;i<32u;i++)out[i]=(uint8_t)(s->h[i/4u]>>(24u-8u*(i%4u)));
}

struct progress { uint32_t token,total,written,durable;uint8_t phase,detail; };
struct rh_connect {
    const struct canopus_context_v1 *ctx;
    struct rh_connect_hooks hooks;
    struct progress p, terminal;
    uint32_t revision, high, install_generation, used, done, frames, verify_offset;
    int32_t fd;
    uint8_t hash[32],nonce[8],open_reply[20];
    uint8_t control[52], outgoing[480];
    uint32_t control_len,out_len;
    uint8_t *work;
    struct sha sha;
    char incoming[256],archive[256];
    uint8_t task,nonce_valid,lease,started,ack_dirty,flush_requested,install_started;
    uint8_t notified,notifying,remote,checkpoint_failed,terminal_dirty;
    struct rh_transfer_ui_state last_ui;
};
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0]|(uint16_t)p[1]<<8); }
static void put32(uint8_t *p,uint32_t v) { unsigned i;for(i=0;i<4u;i++)p[i]=(uint8_t)(v>>(8u*i)); }
static void put16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
static int same(const void *a,const void *b,uint32_t n) { const uint8_t *x=a,*y=b;while(n--)if(*x++!=*y++)return 0;return 1; }
static uint32_t crc(const uint8_t *p,uint32_t n) {
    uint32_t c=~0u,i;while(n--) { c^=*p++;for(i=0;i<8u;i++)c=(c>>1)^(0xedb88320u&(0u-(c&1u))); }return ~c;
}
static int path(char out[256],const char *a,const char *b) {
    uint32_t n=(uint32_t)canopus_strnlen(a,256),m=(uint32_t)canopus_strnlen(b,256);
    if(n+m>=256u)return -LIMIT;canopus_memcpy(out,a,n);canopus_memcpy(out+n,b,m+1u);return 0;
}
static unsigned error_code(int rc) { return rc<0 && rc>=-11 ? (unsigned)-rc:IO; }
static void notify(struct rh_connect *r) {
    struct rh_transfer_ui_state ui;
    canopus_memset(&ui,0,sizeof(ui));ui.phase=r->p.phase;ui.detail=r->p.detail;ui.remote_connected=r->remote;
    ui.written_offset=r->p.written;ui.durable_offset=r->p.durable;ui.archive_bytes=r->p.total;
    /* Avoid a potentially overflowing written*100 at the 64 MiB bound. */
    if(ui.archive_bytes) {
        uint32_t i,a=0;ui.percent=0;
        for(i=0;i<100u;i++) { if(a>=ui.archive_bytes-ui.written_offset) { a-=ui.archive_bytes-ui.written_offset;ui.percent++; }else a+=ui.written_offset; }
    }
    if(!r->notified || !same(&ui,&r->last_ui,sizeof(ui))) {
        r->last_ui=ui;r->notified=1;
        if(r->hooks.changed) { r->notifying=1;r->hooks.changed(r->hooks.cookie,&ui);r->notifying=0; }
    }
}
static void latch(struct rh_connect *r,unsigned code) {
    if(r->p.phase==RH_XFER_PHASE_INSTALLED) {
        r->terminal_dirty=1;r->ack_dirty=1;return;
    }
    if(!r->p.detail) { r->p.detail=(uint8_t)code;r->p.phase=RH_XFER_PHASE_ERROR; }
    r->ack_dirty=1;
}
static int close_archive(struct rh_connect *r) {
    int rc=0;if(r->fd>=0) { rc=r->ctx->fs->close(r->ctx,r->fd);r->fd=-1; }return rc<0 ? -IO:0;
}
static int workspace(struct rh_connect *r) {
    if(!r->work)r->work=r->ctx->mem->alloc(r->ctx,WORK_BYTES);return r->work ? 0:-LIMIT;
}
static void free_workspace(struct rh_connect *r) {
    if(r->work)r->ctx->mem->free(r->ctx,r->work);r->work=0;
}
static void progress_bytes(uint8_t *p,const struct progress *s) {
    put32(p,s->durable);p[4]=8;p[5]=4;p[6]=s->phase;p[7]=s->detail;
}
static void header(uint8_t *p,uint8_t op,uint8_t code,uint16_t seq,uint32_t token,uint32_t arg) {
    p[0]=(uint8_t)(op|0x80u);p[1]=code;put16(p+2,seq);put32(p+4,token);put32(p+8,arg);
}
static void response(struct rh_connect *r,uint8_t op,unsigned code,uint16_t seq,uint32_t token,uint32_t arg,const struct progress *s) {
    if(r->out_len && r->outgoing[0]==(DATA|0x80u) && op!=DATA)r->ack_dirty=1;
    header(r->outgoing,op,(uint8_t)code,seq,token,s ? s->written:arg);r->out_len=12;
    if(s) { progress_bytes(r->outgoing+12,s);r->out_len=20; }
}
static void save_progress(uint8_t *b,const struct progress *p) {
    put32(b,p->token);put32(b+4,p->total);put32(b+8,p->written);put32(b+12,p->durable);b[16]=p->phase;b[17]=p->detail;
}
static void load_progress(struct progress *p,const uint8_t *b) {
    p->token=get32(b);p->total=get32(b+4);p->written=get32(b+8);p->durable=get32(b+12);p->phase=b[16];p->detail=b[17];
}
static int valid_progress(const struct progress *p) {
    return p->total<=LIMIT_BYTES && p->durable<=p->written && p->written<=p->total && p->phase<=6u && p->detail<=11u;
}
/* A token is not exposed until BOTH slots contain its high water mark. Thus
 * loss of either slot can roll progress back but cannot reissue an exposed token.
 * The fixed record has explicit little-endian fields and a CRC, never ABI structs. */
static int save_record(struct rh_connect *r) {
    uint8_t b[RECORD_BYTES];char name[256];uint32_t off=0;int32_t fd,n;int rc;
    if(r->revision==UINT32_MAX)return -LIMIT;
    canopus_memset(b,0,sizeof(b));put32(b,0x31524343u);put32(b+4,1);put32(b+8,r->revision+1u);put32(b+12,r->high);
    save_progress(b+16,&r->p);b[34]=r->task;b[35]=r->nonce_valid;
    canopus_memcpy(b+36,r->hash,32);canopus_memcpy(b+68,r->nonce,8);canopus_memcpy(b+76,r->open_reply,20);
    put32(b+96,r->install_generation);save_progress(b+100,&r->terminal);put32(b+124,crc(b,124));
    rc=path(name,r->incoming,(r->revision&1u) ? "state.0":"state.1");if(rc)return rc;
    fd=r->ctx->fs->open(r->ctx,name,CANOPUS_FS_WRITE|CANOPUS_FS_CREATE|CANOPUS_FS_TRUNC);if(fd<0)return -IO;
    while(off<sizeof(b)) {
        n=r->ctx->fs->write(r->ctx,fd,b+off,(uint32_t)sizeof(b)-off);
        if(n<=0 || (uint32_t)n>sizeof(b)-off)break;off+=(uint32_t)n;
    }
    rc=off==sizeof(b) && r->ctx->fs->sync(r->ctx,fd)==0 ? 0:-IO;
    if(r->ctx->fs->close(r->ctx,fd)<0)rc=-IO;
    /* Even an ambiguous failed sync/close may have left this revision on disk.
     * Never overwrite the other (known-good) slot until a later successful save. */
    if(!rc)r->revision++;
    return rc;
}
/* Missing is distinct from damaged or unreadable. Never reset token high water
 * to zero when records exist but cannot be validated. */
static int read_record(struct rh_connect *r,unsigned slot,uint8_t b[RECORD_BYTES]) {
    char name[256];int32_t fd,n;uint32_t off=0;int rc=0;struct progress p,t;
    if(path(name,r->incoming,slot ? "state.1":"state.0"))return -LIMIT;
    fd=r->ctx->fs->open(r->ctx,name,CANOPUS_FS_READ);
    if(fd<0) { int exists=r->ctx->fs->exists(r->ctx,name);return exists==0 ? 0:-IO; }
    if(r->ctx->fs->size(r->ctx,fd)!=RECORD_BYTES)rc=-IO;
    while(!rc && off<RECORD_BYTES) { n=r->ctx->fs->read(r->ctx,fd,b+off,RECORD_BYTES-off);if(n<=0 || (uint32_t)n>RECORD_BYTES-off)rc=-IO;else off+=(uint32_t)n; }
    if(r->ctx->fs->close(r->ctx,fd)<0)rc=-IO;
    if(rc)return rc;
    load_progress(&p,b+16);load_progress(&t,b+100);
    if(get32(b)!=0x31524343u || get32(b+4)!=1u || !get32(b+8) || get32(b+124)!=crc(b,124) ||
       !valid_progress(&p) || !valid_progress(&t) || b[34]>1 || b[35]>1 ||
       p.token>get32(b+12) || t.token>get32(b+12) || (b[34] && (!p.token || !p.total)))return -IO;
    return 1;
}
static int checkpoint(struct rh_connect *r) {
    uint32_t previous=r->p.durable;int rc;
    if(r->p.written>r->p.durable) {
        if(r->fd<0 || r->ctx->fs->sync(r->ctx,r->fd)<0)return -IO;
        r->p.durable=r->p.written;
    }
    rc=save_record(r);
    if(rc) { r->p.durable=previous;r->checkpoint_failed=1; }
    else r->checkpoint_failed=0;
    return rc;
}
static int open_archive(struct rh_connect *r,int fresh) {
    int64_t size;
    if(r->fd>=0)return 0;
    r->fd=r->ctx->fs->open(r->ctx,r->archive,CANOPUS_FS_RDWR|(fresh ? CANOPUS_FS_CREATE|CANOPUS_FS_TRUNC:0u));
    if(r->fd<0)return -IO;
    size=r->ctx->fs->size(r->ctx,r->fd);
    if(size<0 || (uint64_t)size<r->p.written || r->ctx->fs->seek(r->ctx,r->fd,r->p.written,CANOPUS_FS_SEEK_SET)!=(int64_t)r->p.written) {
        (void)close_archive(r);return -IO;
    }
    return 0;
}
/* One native write per tick. A batch remains owned until every short write has
 * completed. A failed partial batch cannot advance the acknowledged prefix. */
static int flush_step(struct rh_connect *r) {
    int32_t n;
    if(!r->used)return 1;
    if(r->p.detail)return -1;
    r->flush_requested=1;
    if(open_archive(r,0)) { latch(r,IO);return -1; }
    if(!r->done && r->ctx->fs->seek(r->ctx,r->fd,r->p.written,CANOPUS_FS_SEEK_SET)!=(int64_t)r->p.written) { latch(r,IO);return -1; }
    n=r->ctx->fs->write(r->ctx,r->fd,r->work+r->done,r->used-r->done);
    if(n<=0 || (uint32_t)n>r->used-r->done) { latch(r,IO);return -1; }
    r->done+=(uint32_t)n;
    if(r->done<r->used)return 0;
    r->p.written+=r->used;r->used=r->done=r->frames=0;r->flush_requested=0;r->ack_dirty=1;
    if(r->p.written-r->p.durable>=CHECKPOINT_BYTES && checkpoint(r))latch(r,IO);
    return r->p.detail ? -1:1;
}
static void send_pending(struct rh_connect *r) {
    int rc;
    if(!r->out_len && r->ack_dirty && r->task && r->lease) {
        response(r,DATA,r->p.detail,0,r->p.token,0,&r->p);r->ack_dirty=0;
    } else if(r->out_len && r->outgoing[0]==(DATA|0x80u) && r->ack_dirty) {
        response(r,DATA,r->p.detail,0,r->p.token,0,&r->p);r->ack_dirty=0;
    }
    if(!r->out_len)return;
    rc=r->ctx->connect->send(r->ctx,CANOPUS_CONNECT_BINARY,r->outgoing,r->out_len);
    if(rc==0)r->out_len=0; /* BUSY, not listening and transport errors retain it. */
}
static void abort_install(struct rh_connect *r) {
    if(r->install_started && r->hooks.install_abort)r->hooks.install_abort(r->hooks.cookie);
    r->install_started=0;
}
static void remember_terminal(struct rh_connect *r) {
    if(r->task && (r->p.phase==RH_XFER_PHASE_INSTALLED || r->p.phase==RH_XFER_PHASE_CANCELED))r->terminal=r->p;
}
static void receive(const struct canopus_context_v1 *ctx,uint32_t kind,const void *data,uint32_t len,void *cookie) {
    struct rh_connect *r=cookie;const uint8_t *p=data;uint32_t off,n,token;uint16_t seq;uint8_t op;(void)ctx;
    if(!r || !r->started || r->notifying || kind!=CANOPUS_CONNECT_BINARY || !p || len<12u || len>480u)return;
    op=p[0];seq=get16(p+2);token=get32(p+4);
    if(op==DATA) {
        if(!r->task || !r->lease || token!=r->p.token)return;
        if(r->p.phase!=RH_XFER_PHASE_RECEIVING || r->p.detail)return;
        if(p[1] || seq || len==12u) { latch(r,BAD);return; }
        off=get32(p+8);n=len-12u;
        if(n>r->p.total || off>r->p.total-n || off>r->p.written+r->used) { latch(r,OFFSET);return; }
        if(off<r->p.written+r->used) {
            if(off+n>r->p.written+r->used)latch(r,OFFSET);
            return;
        }
        if(r->control_len || (r->out_len && r->outgoing[0]!=(DATA|0x80u)) || r->flush_requested || r->used+n>WORK_BYTES || !r->work) { latch(r,BUSY);return; }
        canopus_memcpy(r->work+r->used,p+12,n);r->used+=n;r->frames++;
        /* Threshold requests a flush; copying up to the advertised 8-frame
         * window is still legal until poll begins the actual write. */
        return;
    }
    if(op<HELLO || op>LIST || op==DATA || !seq)return;
    if(r->control_len || (r->out_len && r->outgoing[0]!=(DATA|0x80u)))return;
    /* No borrowed transport pointer survives the callback. Invalid controls
     * need only their header to generate a correlated BAD in poll. */
    if(len>sizeof(r->control))canopus_memcpy(r->control,p,12);
    else canopus_memcpy(r->control,p,len);
    r->control_len=len;
}
static int valid_control(const uint8_t *p,uint32_t n) {
    uint8_t op=p[0];uint32_t token=get32(p+4),arg=get32(p+8);
    if(p[1])return 0;
    if(op==HELLO)return n==14u && !token && !arg && get16(p+12)==1u;
    if(op==OPEN)return n==52u && !token && arg && arg<=LIMIT_BYTES;
    if(op==LIST)return n==16u && !token;
    return n==12u && token && !arg;
}
static void terminal_response(struct rh_connect *r,uint8_t op,uint16_t seq,uint32_t token,uint32_t arg) {
    if(r->terminal.token==token && token && op==QUERY)response(r,op,r->terminal.detail,seq,token,arg,&r->terminal);
    else response(r,op,STALE,seq,token,arg,0);
}
static int cached_open(struct rh_connect *r,const uint8_t *c) {
    return r->nonce_valid && get32(c+8)==r->p.total && same(c+12,r->hash,32) && same(c+44,r->nonce,8);
}
struct open_snapshot {
    struct progress p,terminal;
    uint8_t hash[32],nonce[8],reply[20],task,nonce_valid,lease;
    uint32_t install_generation;
};
static void undo_open(struct rh_connect *r,const struct open_snapshot *s) {
    /* High-water mark/revision remain burned even after ambiguous sync/close. */
    r->p=s->p;r->terminal=s->terminal;r->task=s->task;r->nonce_valid=s->nonce_valid;r->lease=s->lease;
    r->install_generation=s->install_generation;canopus_memcpy(r->hash,s->hash,32);
    canopus_memcpy(r->nonce,s->nonce,8);canopus_memcpy(r->open_reply,s->reply,20);
}
static void process_open(struct rh_connect *r,const uint8_t *c) {
    uint32_t total=get32(c+8);uint16_t seq=get16(c+2);int rc,same_task;
    struct open_snapshot before;
    if(cached_open(r,c)) {
        /* A crash between the original two writes may leave only one durable
         * high-water slot. Repair both before exposing the cached token. */
        rc=save_record(r);if(!rc)rc=save_record(r);
        if(rc) { response(r,OPEN,error_code(rc),seq,0,total,0);return; }
        if(r->out_len && r->outgoing[0]==(DATA|0x80u))r->ack_dirty=1;
        canopus_memcpy(r->outgoing,r->open_reply,20);put16(r->outgoing+2,seq);r->out_len=20;r->lease=1;return;
    }
    same_task=r->task && r->p.total==total && same(r->hash,c+12,32);
    if(same_task && r->p.phase==RH_XFER_PHASE_INSTALLED && r->hooks.find_hash) {
        rc=r->hooks.find_hash(r->hooks.cookie,r->hash);
        if(rc<0) { response(r,OPEN,error_code(rc),seq,0,total,0);return; }
        /* The native catalog can delete an installed package independently of
         * this receiver's recent terminal. Missing content is a new task. */
        if(!rc)same_task=0;
    }
    if(r->task && !same_task && r->p.phase!=RH_XFER_PHASE_INSTALLED && r->p.phase!=RH_XFER_PHASE_CANCELED) {
        response(r,OPEN,BUSY,seq,0,total,0);return;
    }
    if(r->high==UINT32_MAX || r->revision>UINT32_MAX-2u) { response(r,OPEN,LIMIT,seq,0,total,0);return; }
    before.p=r->p;before.terminal=r->terminal;before.task=r->task;before.nonce_valid=r->nonce_valid;before.lease=r->lease;
    before.install_generation=r->install_generation;canopus_memcpy(before.hash,r->hash,32);
    canopus_memcpy(before.nonce,r->nonce,8);canopus_memcpy(before.reply,r->open_reply,20);
    /* Lease barrier: discard only uncommitted RAM / partial-write tail. */
    r->used=r->done=r->frames=0;r->flush_requested=0;r->lease=0;r->ack_dirty=0;
    if(r->out_len && r->outgoing[0]==(DATA|0x80u))r->out_len=0;
    if(!same_task) {
        remember_terminal(r);abort_install(r);r->nonce_valid=0;
        rc=close_archive(r);if(rc)goto failed;
        canopus_memset(&r->p,0,sizeof(r->p));r->p.total=total;r->p.phase=RH_XFER_PHASE_RECEIVING;
        canopus_memcpy(r->hash,c+12,32);r->task=1;r->install_generation=r->high+1u;
        rc=r->hooks.find_hash ? r->hooks.find_hash(r->hooks.cookie,r->hash):0;
        if(rc<0)goto failed;
        if(rc==1) { r->p.phase=RH_XFER_PHASE_INSTALLED;r->p.written=r->p.durable=total; }
        else {
            rc=workspace(r);if(!rc)rc=open_archive(r,1);if(rc)goto failed;
        }
    } else if(r->p.phase==RH_XFER_PHASE_RECEIVING ||
              (r->p.phase==RH_XFER_PHASE_ERROR && (r->p.detail==BUSY || r->p.detail==OFFSET || r->p.detail==IO))) {
        /* Reopen and verify the committed prefix still exists before clearing IO. */
        rc=close_archive(r);if(!rc)rc=open_archive(r,0);if(!rc)rc=workspace(r);if(rc)goto failed;
        r->p.phase=RH_XFER_PHASE_RECEIVING;r->p.detail=0;
    }
    /* A canceled same-content task starts afresh only through a new OPEN. */
    if(same_task && r->p.phase==RH_XFER_PHASE_CANCELED) {
        r->p.written=r->p.durable=0;r->p.detail=0;r->p.phase=RH_XFER_PHASE_RECEIVING;
        r->install_generation=r->high+1u;rc=workspace(r);if(!rc)rc=open_archive(r,1);if(rc)goto failed;
    }
    r->high++;r->p.token=r->high;r->nonce_valid=0;
    if(r->p.written>r->p.durable) {
        if(r->fd<0 || r->ctx->fs->sync(r->ctx,r->fd)<0) { rc=-IO;goto failed; }
        r->p.durable=r->p.written;
    }
    canopus_memcpy(r->nonce,c+44,8);r->nonce_valid=1;
    header(r->open_reply,OPEN,0,seq,r->p.token,r->p.written);progress_bytes(r->open_reply+12,&r->p);
    rc=save_record(r);if(!rc)rc=save_record(r);if(rc)goto failed;
    r->checkpoint_failed=0;r->lease=1;
    canopus_memcpy(r->outgoing,r->open_reply,20);r->out_len=20;return;
failed:
    undo_open(r,&before);
    if(r->task)latch(r,error_code(rc));
    response(r,OPEN,error_code(rc),seq,0,total,0);
}
static void start_verify(struct rh_connect *r) {
    sha_init(&r->sha);r->verify_offset=0;r->p.phase=RH_XFER_PHASE_VERIFYING;r->p.detail=0;r->ack_dirty=1;
}
static void installed(struct rh_connect *r);
static void process_control(struct rh_connect *r) {
    uint8_t *c=r->control,op=c[0];uint16_t seq=get16(c+2);uint32_t token=get32(c+4),arg=get32(c+8);int rc;
    if(!valid_control(c,r->control_len)) { response(r,op,op==OPEN && arg>LIMIT_BYTES ? LIMIT:BAD,seq,token,arg,0);r->control_len=0;return; }
    if(op==OPEN) { process_open(r,c);r->control_len=0;return; }
    if(op==CANCEL && r->task && r->lease && token==r->p.token && r->p.phase!=RH_XFER_PHASE_INSTALLED) {
        /* Commit revocation before abort/unlink; a crash cannot resume canceled installation. */
        struct progress before=r->p;
        rc=r->hooks.install_cancel ? r->hooks.install_cancel(r->hooks.cookie,r->hash):0;
        if(rc) {
            if(rc==1)installed(r);
            response(r,op,rc==1 ? BUSY:error_code(rc),seq,token,arg,&r->p);
            r->control_len=0;return;
        }
        r->p.phase=RH_XFER_PHASE_CANCELED;r->p.detail=0;
        rc=save_record(r);
        if(rc) { r->p=before;latch(r,IO);response(r,op,IO,seq,token,arg,&r->p); }
        else {
            r->used=r->done=r->frames=0;r->flush_requested=0;abort_install(r);(void)close_archive(r);
            (void)r->ctx->fs->remove(r->ctx,r->archive);free_workspace(r);remember_terminal(r);
            response(r,op,0,seq,token,arg,&r->p);
        }
        r->control_len=0;return;
    }
    /* QUERY/FINISH are ordered write barriers. HELLO/LIST also preserve the
     * shared workspace while earlier DATA is being drained. */
    if(r->used && !r->p.detail) { if(flush_step(r)==0)return;if(r->used && !r->p.detail)return; }
    if(op==HELLO) {
        response(r,op,0,seq,0,0,0);put16(r->outgoing+12,1);put16(r->outgoing+14,468);
        r->outgoing[16]=2;r->outgoing[17]=8;r->outgoing[18]=4;r->outgoing[19]=1;
        put32(r->outgoing+20,LIMIT_BYTES);put32(r->outgoing+24,LIMIT_BYTES);put32(r->outgoing+28,CHECKPOINT_BYTES);r->out_len=32;
    } else if(op==LIST) {
        uint32_t size=468;response(r,op,0,seq,0,arg,0);
        rc=r->hooks.list ? r->hooks.list(r->hooks.cookie,get32(c+12),arg,r->outgoing+12,&size):-UNSUPPORTED;
        if(rc || size>468u || size<12u)response(r,op,rc ? error_code(rc):IO,seq,0,arg,0);
        else r->out_len=12u+size;
    } else if(!r->task || !r->lease || token!=r->p.token)terminal_response(r,op,seq,token,arg);
    else if(op==QUERY)response(r,op,r->p.detail,seq,token,arg,&r->p);
    else if(op==CANCEL)response(r,op,BUSY,seq,token,arg,&r->p);
    else if(op==FINISH) {
        if(r->p.phase==RH_XFER_PHASE_INSTALLED || r->p.phase==RH_XFER_PHASE_VERIFYING || r->p.phase==RH_XFER_PHASE_INSTALLING)
            response(r,op,0,seq,token,arg,&r->p);
        else if(r->p.phase==RH_XFER_PHASE_CANCELED)response(r,op,STALE,seq,token,arg,&r->p);
        else if(r->p.detail && r->p.detail!=ACTIVE)response(r,op,r->p.detail,seq,token,arg,&r->p);
        else if(r->p.written!=r->p.total)response(r,op,OFFSET,seq,token,arg,&r->p);
        else if(!r->hooks.install_begin || !r->hooks.install_step || !r->hooks.install_abort)response(r,op,UNSUPPORTED,seq,token,arg,&r->p);
        else {
            if(r->p.detail==ACTIVE) { abort_install(r);r->p.detail=0; }
            rc=workspace(r);if(!rc)rc=open_archive(r,0);if(!rc)rc=checkpoint(r);
            if(rc)latch(r,error_code(rc));
            else { start_verify(r);if(save_record(r))latch(r,IO); }
            response(r,op,r->p.detail,seq,token,arg,&r->p);
        }
    }
    r->control_len=0;
}
static void persist_installed(struct rh_connect *r) {
    /* The catalog has already committed. A checkpoint error cannot revoke that
     * fact or make CANCEL pretend it undid the install. Retain the ZIP until
     * terminal persistence succeeds so a restart can find/recover the commit. */
    if(save_record(r)) { r->terminal_dirty=1;return; }
    r->terminal_dirty=0;remember_terminal(r);
    (void)r->ctx->fs->remove(r->ctx,r->archive);
}
static void installed(struct rh_connect *r) {
    r->p.phase=RH_XFER_PHASE_INSTALLED;r->p.detail=0;r->terminal_dirty=1;
    remember_terminal(r);abort_install(r);(void)close_archive(r);free_workspace(r);
    persist_installed(r);r->ack_dirty=1;
}
static void verify_step(struct rh_connect *r) {
    int32_t n;uint32_t wanted;uint8_t digest[32];int rc;
    if(workspace(r) || open_archive(r,0)) { latch(r,IO);return; }
    if(r->verify_offset<r->p.total) {
        wanted=r->p.total-r->verify_offset;if(wanted>WORK_BYTES)wanted=WORK_BYTES;
        if(r->ctx->fs->seek(r->ctx,r->fd,r->verify_offset,CANOPUS_FS_SEEK_SET)!=(int64_t)r->verify_offset) { latch(r,IO);return; }
        n=r->ctx->fs->read(r->ctx,r->fd,r->work,wanted);
        if(n<=0 || (uint32_t)n>wanted) { latch(r,IO);return; }
        sha_update(&r->sha,r->work,(uint32_t)n);r->verify_offset+=(uint32_t)n;return;
    }
    sha_final(&r->sha,digest);
    if(!same(digest,r->hash,32)) { latch(r,HASH);(void)save_record(r);return; }
    rc=close_archive(r);if(rc) { latch(r,IO);return; }
    free_workspace(r);r->p.phase=RH_XFER_PHASE_INSTALLING;
    if(save_record(r)) { latch(r,IO);return; }
    r->ack_dirty=1;
}
static void installation_step(struct rh_connect *r) {
    int rc;
    if(!r->hooks.install_begin || !r->hooks.install_step || !r->hooks.install_abort) { latch(r,UNSUPPORTED);return; }
    if(!r->install_started) {
        rc=r->hooks.find_hash ? r->hooks.find_hash(r->hooks.cookie,r->hash):0;
        if(rc==1) { installed(r);return; }
        if(rc==-(int)BUSY)return;
        if(rc<0) { latch(r,error_code(rc));return; }
        r->install_started=1;
        rc=r->hooks.install_begin(r->hooks.cookie,r->archive,r->p.total,r->install_generation);
        if(rc==-(int)BUSY) { r->install_started=0;return; }
    } else rc=r->hooks.install_step(r->hooks.cookie,r->hash);
    if(rc==-(int)BUSY)return;
    if(rc==1)installed(r);
    else if(rc<0) { latch(r,error_code(rc));abort_install(r);(void)save_record(r); }
}
int rh_connect_create(const struct canopus_context_v1 *ctx,const struct rh_connect_hooks *hooks,struct rh_connect **out) {
    struct rh_connect *r;uint8_t a[RECORD_BYTES],b[RECORD_BYTES],*chosen;char root[256];int x,y,rc;uint32_t n;
    if(!out)return -BAD;*out=0;
    if(!ctx || !ctx->mem || !ctx->mem->alloc || !ctx->mem->free || !ctx->fs ||
       !ctx->fs->open || !ctx->fs->read || !ctx->fs->write || !ctx->fs->seek || !ctx->fs->size ||
       !ctx->fs->sync || !ctx->fs->close || !ctx->fs->exists || !ctx->fs->mkdir || !ctx->fs->remove || !ctx->fs->get_data_path)
        return -UNSUPPORTED;
    r=ctx->mem->alloc(ctx,sizeof(*r));if(!r)return -LIMIT;canopus_memset(r,0,sizeof(*r));r->ctx=ctx;r->fd=-1;if(hooks)r->hooks=*hooks;
    canopus_memset(root,0,sizeof(root));rc=ctx->fs->get_data_path(ctx,root,sizeof(root));n=(uint32_t)canopus_strnlen(root,sizeof(root));
    if(rc<0 || !n || n==sizeof(root)) { rc=-IO;goto failed; }
    while(n && root[n-1u]=='/')root[--n]=0;
    rc=path(r->incoming,root,"/incoming/");if(rc)goto failed;
    rc=path(r->archive,r->incoming,"archive.zip");if(rc)goto failed;
    r->incoming[canopus_strlen(r->incoming)-1u]=0;
    x=ctx->fs->exists(ctx,r->incoming);if(x==0)rc=ctx->fs->mkdir(ctx,r->incoming);else rc=x<0 ? -IO:0;
    r->incoming[canopus_strlen(r->incoming)]='/'; /* restore trailing slash; zeroed struct supplies the terminator. */
    if(rc<0) { rc=-IO;goto failed; }
    x=read_record(r,0,a);y=read_record(r,1,b);
    if(x<=0 && y<=0) {
        if(x || y) { rc=-IO;goto failed; }
        /* Establish a known-good empty record before the first OPEN can tear
         * its other slot. Initial task creation failures then remain recoverable. */
        rc=save_record(r);if(!rc)rc=save_record(r);if(rc)goto failed;
    }
    else {
        chosen=x>0 && (y<=0 || get32(a+8)>=get32(b+8)) ? a:b;
        r->revision=get32(chosen+8);r->high=get32(chosen+12);load_progress(&r->p,chosen+16);
        r->task=chosen[34];r->nonce_valid=chosen[35];canopus_memcpy(r->hash,chosen+36,32);
        canopus_memcpy(r->nonce,chosen+68,8);canopus_memcpy(r->open_reply,chosen+76,20);r->install_generation=get32(chosen+96);
        load_progress(&r->terminal,chosen+100);r->p.written=r->p.durable;
        /* Lost progress is replayed only after OPEN. Verification/installation
         * restarts from disk; hooks must recover an already committed install. */
        if(r->p.phase==RH_XFER_PHASE_VERIFYING || r->p.phase==RH_XFER_PHASE_INSTALLING)start_verify(r);
    }
    if(r->task && r->p.phase!=RH_XFER_PHASE_INSTALLED && r->p.phase!=RH_XFER_PHASE_CANCELED) {
        rc=workspace(r);if(rc)goto failed;
        rc=open_archive(r,0);if(rc) { r->p.detail=IO;r->p.phase=RH_XFER_PHASE_ERROR; }
    }
    *out=r;notify(r);return 0;
failed:
    (void)close_archive(r);free_workspace(r);ctx->mem->free(ctx,r);return rc;
}
int rh_connect_start(struct rh_connect *r) {
    int rc;
    if(!r || r->notifying)return -BAD;
    if(r->started)return 0;
    if(!r->ctx->connect || !r->ctx->connect->listen || !r->ctx->connect->send || !r->ctx->connect->remote_listening)return -UNSUPPORTED;
    r->started=1;rc=r->ctx->connect->listen(r->ctx,receive,r);
    if(rc) { r->started=0;return -(int)error_code(rc); }
    return 0;
}
void rh_connect_poll(struct rh_connect *r) {
    int connected;
    if(!r || !r->started || r->notifying)return;
    connected=r->ctx->connect->remote_listening(r->ctx);r->remote=(uint8_t)(connected>0);
    if(r->control_len)process_control(r);
    else if(r->p.phase==RH_XFER_PHASE_RECEIVING && !r->p.detail &&
            (r->flush_requested || r->frames>=4u || (r->used && r->p.written+r->used==r->p.total)))
        (void)flush_step(r);
    else if(r->p.phase==RH_XFER_PHASE_VERIFYING)verify_step(r);
    else if(r->p.phase==RH_XFER_PHASE_INSTALLING)installation_step(r);
    else if(r->p.phase==RH_XFER_PHASE_INSTALLED && r->terminal_dirty)persist_installed(r);
    send_pending(r);notify(r);
}
int rh_connect_pause(struct rh_connect *r) {
    int rc=0,closed;
    if(!r || r->notifying)return -BAD;
    if(r->started) {
        rc=r->ctx->connect->listen(r->ctx,0,0);if(rc)return -IO;r->started=0;
    }
    /* Shutdown is deliberately synchronous but bounded by one 4 KiB batch.
     * Background progress uses a single write per poll instead. */
    while(r->used && !r->p.detail)if(flush_step(r)<0)break;
    if(r->task) {
        if(r->p.detail && r->used)rc=-IO;
        if(!rc)rc=checkpoint(r);
    }
    closed=close_archive(r);if(closed)rc=closed;
    if(rc) {
        if(r->p.phase==RH_XFER_PHASE_INSTALLED)r->terminal_dirty=1;
        else latch(r,IO);
    }
    r->remote=0;r->control_len=r->out_len=0;r->ack_dirty=0;notify(r);return rc;
}
void rh_connect_destroy(struct rh_connect *r) {
    if(!r || r->notifying)return;
    (void)rh_connect_pause(r);abort_install(r);free_workspace(r);r->ctx->mem->free(r->ctx,r);
}

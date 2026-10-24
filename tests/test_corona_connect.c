#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "corona_connect.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* These adapters exercise the production state machine. Writes may be short;
 * sync, read, close, checkpoint and event failures are independently injected. */
static char sandbox[256];
static struct rh_connect *receiver;
static canopus_connect_callback_t listener;
static void *listener_cookie;
static struct rh_transfer_ui_state ui;
static uint8_t reply[480];
static uint32_t reply_size,sends,io_calls,short_write,short_read;
static int write_fail_after=-1,fail_sync,fail_record_write,fail_read,fail_close,record_sync_fail_after=-1;
static int send_busy,remote=1,find_result,install_result,begin_result,begin_count,step_count,abort_count;
static int callback_active,alloc_fail,list_result;
static uint32_t list_revision=7,live_bytes,peak_bytes;
static uint8_t fd_record[256],fd_open[256];
struct allocation { struct allocation *next;uint32_t bytes;uint64_t alignment; };
static struct allocation *allocations;
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t crc(const uint8_t *p,uint32_t n) { uint32_t c=~0u;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return ~c; }
static void realpath_for(char out[512],const char *p) {
    const char *root="/data/canopus/m.corona";assert(!strncmp(p,root,strlen(root)));
    assert(snprintf(out,512,"%s%s",sandbox,p+strlen(root))<512);
}
static void io(void) { assert(!callback_active);io_calls++; }
static int fs_open(const struct canopus_context_v1 *c,const char *p,uint32_t f) {
    char name[512];int flags,fd;(void)c;io();realpath_for(name,p);
    flags=(f&CANOPUS_FS_WRITE) ? ((f&CANOPUS_FS_READ) ? O_RDWR:O_WRONLY):O_RDONLY;
    if(f&CANOPUS_FS_CREATE)flags|=O_CREAT;
    if((f&CANOPUS_FS_TRUNC) || ((f&CANOPUS_FS_WRITE) && !(f&CANOPUS_FS_READ)))flags|=O_TRUNC;
    fd=open(name,flags,0600);if(fd<0)return -errno;assert(fd<256);fd_open[fd]=1;fd_record[fd]=strstr(p,"state.")!=0;return fd;
}
static int fs_read(const struct canopus_context_v1 *c,int fd,void *p,uint32_t n) {
    ssize_t rc;(void)c;io();if(fail_read)return -EIO;if(short_read && n>short_read)n=short_read;rc=read(fd,p,n);return rc<0 ? -errno:(int)rc;
}
static int fs_write(const struct canopus_context_v1 *c,int fd,const void *p,uint32_t n) {
    ssize_t rc;(void)c;io();
    if((fd_record[fd] && fail_record_write) || write_fail_after==0)return -EIO;
    if(write_fail_after>0)write_fail_after--;
    if(short_write && n>short_write)n=short_write;
    rc=write(fd,p,n);return rc<0 ? -errno:(int)rc;
}
static int64_t fs_seek(const struct canopus_context_v1 *c,int fd,int64_t off,int32_t whence) { (void)c;io();off=lseek(fd,(off_t)off,whence);return off<0 ? -errno:off; }
static int64_t fs_size(const struct canopus_context_v1 *c,int fd) { struct stat st;(void)c;io();return fstat(fd,&st)<0 ? -errno:st.st_size; }
static int fs_sync(const struct canopus_context_v1 *c,int fd) {
    (void)c;io();
    if(fd_record[fd] && record_sync_fail_after==0)return -EIO;
    if(fd_record[fd] && record_sync_fail_after>0)record_sync_fail_after--;
    return fail_sync ? -EIO:(fsync(fd)<0 ? -errno:0);
}
static int fs_close(const struct canopus_context_v1 *c,int fd) { int rc;(void)c;io();fd_open[fd]=0;rc=close(fd);return fail_close ? -EIO:(rc<0 ? -errno:0); }
static int fs_exists(const struct canopus_context_v1 *c,const char *p) { char name[512];struct stat st;(void)c;io();realpath_for(name,p);return stat(name,&st)==0 ? 1:(errno==ENOENT ? 0:-errno); }
static int fs_mkdir(const struct canopus_context_v1 *c,const char *p) { char name[512];(void)c;io();realpath_for(name,p);return mkdir(name,0700)<0 ? -errno:0; }
static int fs_remove(const struct canopus_context_v1 *c,const char *p) { char name[512];(void)c;io();realpath_for(name,p);return unlink(name)<0 ? -errno:0; }
static int fs_root(const struct canopus_context_v1 *c,char *out,uint32_t size) { (void)c;io();assert(size>24);strcpy(out,"/data/canopus/m.corona");return 0; }
static void *mem_alloc(const struct canopus_context_v1 *c,uint32_t n) {
    struct allocation *p;(void)c;assert(!callback_active);if(alloc_fail)return 0;
    p=malloc(sizeof(*p)+n);assert(p);p->bytes=n;p->next=allocations;allocations=p;
    live_bytes+=n;if(live_bytes>peak_bytes)peak_bytes=live_bytes;return p+1;
}
static void mem_free(const struct canopus_context_v1 *c,void *ptr) {
    struct allocation **link=&allocations,*p=(struct allocation *)ptr-1;(void)c;assert(!callback_active);
    while(*link!=p) { assert(*link);link=&(*link)->next; }*link=p->next;live_bytes-=p->bytes;free(p);
}
static int connect_listen(const struct canopus_context_v1 *c,canopus_connect_callback_t cb,void *cookie) { (void)c;listener=cb;listener_cookie=cookie;return 0; }
static int connect_send(const struct canopus_context_v1 *c,uint32_t kind,const void *data,uint32_t n) {
    (void)c;assert(!callback_active && kind==CANOPUS_CONNECT_BINARY && n<=480);
    if(!remote)return CANOPUS_CONNECT_NOT_LISTENING;
    if(send_busy)return CANOPUS_SVC_ERR_BUSY;
    memcpy(reply,data,n);reply_size=n;sends++;return 0;
}
static int listening(const struct canopus_context_v1 *c) { (void)c;return remote; }
static int install_begin(void *cookie,const char *archive,uint32_t n,uint32_t gen) {
    (void)cookie;assert(!callback_active && n && gen);assert(strstr(archive,"/incoming/archive.zip"));begin_count++;return begin_result;
}
static int install_step(void *cookie,const uint8_t hash[32]) { (void)cookie;assert(!callback_active && hash);step_count++;return install_result; }
static void install_abort(void *cookie) { (void)cookie;assert(!callback_active);abort_count++; }
static int find_hash(void *cookie,const uint8_t hash[32]) { (void)cookie;(void)hash;assert(!callback_active);return find_result; }
static int list(void *cookie,uint32_t revision,uint32_t index,uint8_t *p,uint32_t *n) {
    (void)cookie;assert(!callback_active && *n==468 && index==0);
    if(list_result)return list_result;if(revision && revision!=list_revision)return -10;
    put32(p,list_revision);put32(p+4,0);put32(p+8,UINT32_MAX);*n=12;return 0;
}
static void changed(void *cookie,const struct rh_transfer_ui_state *state) { (void)cookie;ui=*state; }
static const struct canopus_fs_obj fs={.open=fs_open,.read=fs_read,.write=fs_write,.seek=fs_seek,.size=fs_size,.sync=fs_sync,.close=fs_close,.exists=fs_exists,.mkdir=fs_mkdir,.remove=fs_remove,.get_data_path=fs_root};
static const struct canopus_mem_obj mem={.alloc=mem_alloc,.free=mem_free};
static const struct canopus_connect_obj connect={.send=connect_send,.listen=connect_listen,.remote_listening=listening};
static const struct canopus_context_v1 ctx={.struct_size=sizeof(ctx),.fs=&fs,.mem=&mem,.connect=&connect};
static const struct rh_connect_hooks hooks={.install_begin=install_begin,.install_step=install_step,.install_abort=install_abort,.find_hash=find_hash,.list=list,.changed=changed};
static const uint8_t abc_hash[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
static const uint8_t large_hash[32]={0x6d,0x1c,0xf2,0x2d,0x7c,0xc0,0x9b,0x08,0x5d,0xfc,0x25,0xee,0x1a,0x1f,0x3a,0xe0,0x26,0x58,0x04,0xc6,0x07,0xbc,0x20,0x74,0xad,0x25,0x3b,0xcc,0x82,0xfd,0x81,0xee};
static void cleanup_tree(const char *name) {
    DIR *dir=opendir(name);struct dirent *entry;char child[512];assert(dir);
    while((entry=readdir(dir))) {
        struct stat st;if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        assert(snprintf(child,sizeof(child),"%s/%s",name,entry->d_name)<(int)sizeof(child));assert(!lstat(child,&st));
        if(S_ISDIR(st.st_mode))cleanup_tree(child);else assert(!unlink(child));
    }
    assert(!closedir(dir));assert(!rmdir(name));
}
static void setup(void) {
    strcpy(sandbox,"/tmp/corona-connect-XXXXXX");assert(mkdtemp(sandbox));
    listener=0;receiver=0;memset(&ui,0,sizeof(ui));reply_size=sends=io_calls=short_write=short_read=0;
    write_fail_after=record_sync_fail_after=-1;fail_sync=fail_record_write=fail_read=fail_close=send_busy=find_result=begin_result=begin_count=step_count=abort_count=alloc_fail=list_result=0;
    remote=1;install_result=1;peak_bytes=0;
    assert(!rh_connect_create(&ctx,&hooks,&receiver));assert(!rh_connect_start(receiver));assert(listener);
}
static void teardown(void) {
    fail_sync=fail_record_write=fail_read=fail_close=0;write_fail_after=record_sync_fail_after=-1;
    rh_connect_destroy(receiver);assert(!listener && !allocations && !live_bytes);cleanup_tree(sandbox);
}
static void crash(void) {
    /* Simulate abrupt owner loss: do not run pause or destructors. */
    for(int fd=0;fd<256;fd++)if(fd_open[fd]) { assert(!close(fd));fd_open[fd]=0; }
    while(allocations) { struct allocation *p=allocations;allocations=p->next;live_bytes-=p->bytes;free(p); }
    listener=0;listener_cookie=0;receiver=0;assert(!live_bytes);
}
static void restart_receiver(void) { crash();assert(!rh_connect_create(&ctx,&hooks,&receiver));assert(!rh_connect_start(receiver)); }
static void ticks(unsigned n) { while(n--)rh_connect_poll(receiver); }
static void deliver(const uint8_t *p,uint32_t n,uint32_t kind) {
    uint32_t before=io_calls;assert(listener);callback_active=1;listener(&ctx,kind,p,n,listener_cookie);callback_active=0;assert(before==io_calls);
}
static void request(uint8_t op,uint16_t seq,uint32_t token,uint32_t arg,const void *payload,uint32_t n) {
    uint8_t p[480]={0};assert(n<=468);p[0]=op;p[2]=(uint8_t)seq;p[3]=(uint8_t)(seq>>8);put32(p+4,token);put32(p+8,arg);if(n)memcpy(p+12,payload,n);deliver(p,12+n,CANOPUS_CONNECT_BINARY);
}
static uint32_t open_task(uint32_t total,const uint8_t hash[32],uint8_t nonce,uint16_t seq) {
    uint8_t body[40]={0};memcpy(body,hash,32);body[32]=nonce;request(2,seq,0,total,body,40);ticks(1);
    assert(reply_size==20 && reply[0]==0x82 && !reply[1]);return get32(reply+4);
}
static void query(uint32_t token,uint16_t seq) { request(4,seq,token,0,0,0);ticks(1); }
static void upload(uint32_t token,uint32_t begin,uint32_t end) {
    uint8_t p[468];memset(p,'a',sizeof(p));
    while(begin<end) {
        for(unsigned i=0;i<4 && begin<end;i++) { uint32_t n=end-begin;if(n>468)n=468;request(3,0,token,begin,p,n);begin+=n; }
        ticks(600);
    }
}
static void test_roundtrip(void) {
    uint8_t version[2]={1,0};uint32_t token;setup();
    request(1,1,0,0,version,2);ticks(1);assert(reply_size==32 && reply[0]==0x81 && reply[1]==0);
    assert(reply[14]==0xd4 && reply[15]==1 && reply[16]==2 && reply[17]==8 && reply[18]==4 && reply[19]==1);
    token=open_task(3,abc_hash,1,2);assert(token && ui.phase==RH_XFER_PHASE_RECEIVING);
    request(3,0,token,0,"abc",3);ticks(1);assert(ui.written_offset==3 && reply[0]==0x83 && get32(reply+8)==3);
    query(token,3);assert(reply[0]==0x84 && reply_size==20 && get32(reply+8)==3 && get32(reply+12)==0);
    request(5,4,token,0,0,0);ticks(1);assert(reply[0]==0x85 && reply[18]==RH_XFER_PHASE_VERIFYING);
    ticks(8);assert(ui.phase==RH_XFER_PHASE_INSTALLED && begin_count==1 && step_count==1 && abort_count==1);
    assert(fs_exists(&ctx,"/data/canopus/m.corona/incoming/archive.zip")==0);
    request(5,5,token,0,0,0);ticks(1);assert(!reply[1] && reply[18]==RH_XFER_PHASE_INSTALLED);
    request(6,6,token,0,0,0);ticks(1);assert(reply[1]==2 && ui.phase==RH_XFER_PHASE_INSTALLED);
    assert(peak_bytes<7000u);teardown();
}
static void test_retry_and_barriers(void) {
    uint32_t token,old;uint8_t saved[20];setup();
    token=open_task(100000,large_hash,1,1);memcpy(saved,reply,20);
    request(3,0,token,0,"a",1);query(token,2);assert(get32(reply+8)==1);
    old=sends;assert(open_task(100000,large_hash,1,3)==token);assert(get32(reply+8)==get32(saved+8));assert(sends==old+1);
    token=open_task(100000,large_hash,2,4);assert(token>get32(saved+4) && get32(reply+8)==1 && get32(reply+12)==1);
    request(3,0,get32(saved+4),1,"BAD",3);ticks(1);assert(ui.written_offset==1);
    /* DATA after QUERY cannot overtake its barrier. */
    request(3,0,token,1,"a",1);request(4,5,token,0,0,0);request(3,0,token,2,"a",1);ticks(1);
    assert(reply[0]==0x84 && reply[1]==2 && get32(reply+8)==1);
    token=open_task(100000,large_hash,3,6);assert(get32(reply+8)==1 && ui.phase==1 && !ui.detail);
    /* Bytes still borrowed by the caller are copied during the callback. */
    uint8_t bytes[3]={'a','a','a'};request(3,0,token,1,bytes,3);memset(bytes,'x',3);query(token,7);assert(get32(reply+8)==4);
    int fd=fs_open(&ctx,"/data/canopus/m.corona/incoming/archive.zip",CANOPUS_FS_READ);char check[4];assert(fs_read(&ctx,fd,check,4)==4 && !memcmp(check,"aaaa",4));assert(!fs_close(&ctx,fd));
    teardown();
}
static void test_backpressure_and_ack(void) {
    uint32_t token,before;uint8_t p[468];setup();memset(p,'a',sizeof(p));token=open_task(100000,large_hash,1,1);
    send_busy=1;upload(token,0,1872);assert(ui.written_offset==1872);before=sends;ticks(3);assert(sends==before);
    upload(token,1872,3744);send_busy=0;ticks(1);assert(reply[0]==0x83 && get32(reply+8)==3744 && sends==before+1);
    remote=0;upload(token,3744,5616);before=sends;ticks(3);assert(sends==before);remote=1;ticks(1);assert(get32(reply+8)==5616);
    /* Overflow must latch and preserve the batch; later DATA cannot overwrite. */
    for(unsigned i=0;i<9;i++)request(3,0,token,5616+i*468,p,468);
    ticks(1);assert(ui.detail==2 && ui.written_offset==5616);request(3,0,token,5616,p,468);ticks(1);assert(ui.written_offset==5616);
    token=open_task(100000,large_hash,2,2);assert(ui.phase==1 && ui.written_offset==5616);
    /* A full duplicate is ignored; a crossing overlap and gap latch OFFSET. */
    request(3,0,token,0,p,468);ticks(1);assert(!ui.detail);
    request(3,0,token,5500,p,468);ticks(1);assert(ui.detail==4);
    token=open_task(100000,large_hash,3,3);request(3,0,token,6000,p,468);ticks(1);assert(ui.detail==4);
    teardown();
}
static void test_short_write_failure(void) {
    uint32_t token;uint8_t p[468];setup();memset(p,'a',sizeof(p));token=open_task(100000,large_hash,1,1);
    for(unsigned i=0;i<4;i++)request(3,0,token,i*468,p,468);
    short_write=37;write_fail_after=1;ticks(2);assert(ui.detail==5 && ui.written_offset==0);
    query(token,2);assert(reply[0]==0x84 && reply[1]==5 && get32(reply+8)==0);
    write_fail_after=-1;short_write=0;token=open_task(100000,large_hash,2,3);assert(!ui.detail && get32(reply+8)==0);
    upload(token,0,1872);assert(ui.written_offset==1872);
    /* Checkpoint failure may freeze acknowledged progress, never move it back. */
    fail_record_write=1;assert(rh_connect_pause(receiver)==-5);assert(ui.written_offset==1872 && ui.durable_offset==0);
    fail_record_write=0;assert(!rh_connect_start(receiver));token=open_task(100000,large_hash,3,4);
    assert(get32(reply+8)==1872 && get32(reply+12)==1872);teardown();
}
static void test_reboot_checkpoint(void) {
    uint32_t token,durable;setup();token=open_task(100000,large_hash,1,1);
    upload(token,0,70000);assert(ui.written_offset==69264); /* final partial batch has <4 frames */
    query(token,2);assert(ui.written_offset==70000);durable=ui.durable_offset;assert(durable>=65536 && durable<70000);
    restart_receiver();assert(ui.written_offset==durable && ui.durable_offset==durable);
    request(3,0,token,durable,"a",1);ticks(1);assert(ui.written_offset==durable); /* lease is inactive until OPEN */
    uint32_t newer=open_task(100000,large_hash,2,3);assert(newer>token && get32(reply+8)==durable);
    short_write=113;upload(newer,durable,100000);query(newer,4);ticks(600);assert(ui.written_offset==100000);
    short_write=0;request(5,5,newer,0,0,0);ticks(100);assert(ui.phase==4);
    restart_receiver();assert(ui.phase==4 && ui.written_offset==100000 && begin_count==1);
    teardown();
}
static void test_hash_and_install_recovery(void) {
    uint32_t token;setup();token=open_task(3,abc_hash,1,1);request(3,0,token,0,"abd",3);ticks(1);
    request(5,2,token,0,0,0);ticks(8);assert(ui.detail==6 && !begin_count);
    token=open_task(3,abc_hash,2,3);assert(ui.detail==6 && ui.phase==5);
    request(6,4,token,0,0,0);ticks(1);assert(ui.phase==6);
    token=open_task(3,abc_hash,3,5);request(3,0,token,0,"abc",3);ticks(1);
    install_result=-9;request(5,6,token,0,0,0);ticks(10);assert(ui.detail==9 && ui.phase==5);
    install_result=0;request(5,7,token,0,0,0);ticks(10);assert(ui.phase==3);
    /* Crash after registry commit but before terminal record: find_hash prevents
     * a duplicate registration after restart. */
    find_result=1;restart_receiver();ticks(10);assert(ui.phase==4);
    token=open_task(3,abc_hash,4,8);assert(ui.phase==4);
    teardown();
}
static void test_corruption_and_high_water(void) {
    uint32_t token;char name[512];setup();token=open_task(3,abc_hash,1,1);
    /* Either slot alone must retain every token already exposed to the peer. */
    realpath_for(name,"/data/canopus/m.corona/incoming/state.0");assert(!unlink(name));restart_receiver();
    assert(open_task(3,abc_hash,2,2)>token);token=get32(reply+4);
    realpath_for(name,"/data/canopus/m.corona/incoming/state.1");int fd=open(name,O_WRONLY|O_TRUNC);assert(fd>=0 && write(fd,"torn",4)==4 && !close(fd));restart_receiver();
    assert(open_task(3,abc_hash,3,3)>token);
    crash();
    for(unsigned slot=0;slot<2;slot++) { snprintf(name,sizeof(name),"%s/incoming/state.%u",sandbox,slot);fd=open(name,O_WRONLY|O_TRUNC);assert(fd>=0 && write(fd,"bad",3)==3 && !close(fd)); }
    assert(rh_connect_create(&ctx,&hooks,&receiver)==-5 && !receiver && !live_bytes);
    cleanup_tree(sandbox);
}
static void test_failed_open_and_limits(void) {
    uint8_t body[40]={0};uint32_t old_sends;setup();memcpy(body,abc_hash,32);body[32]=1;
    fail_record_write=1;request(2,1,0,3,body,40);ticks(1);assert(reply_size==12 && reply[1]==5);
    fail_record_write=0;uint32_t token=open_task(3,abc_hash,2,2);assert(token>1);
    /* Invalid framing, opcode, seq and text must not enter the RX path. */
    old_sends=sends;request(4,0,token,0,0,0);ticks(1);assert(sends==old_sends);
    uint8_t p[12]={4,0,1};deliver(p,11,CANOPUS_CONNECT_BINARY);deliver(p,12,CANOPUS_CONNECT_TEXT);ticks(1);assert(sends==old_sends);
    request(4,3,token,0,body,40);ticks(1);assert(reply_size==12 && reply[1]==1);
    request(4,3,token+1,0,0,0);ticks(1);assert(reply_size==12 && reply[1]==3 && get32(reply+4)==token+1);
    request(2,4,0,64u*1024u*1024u+1u,body,40);ticks(1);assert(reply[1]==8);
    request(5,5,token,0,0,0);ticks(1);assert(reply[1]==4);
    request(7,6,0,0,"\0\0\0\0",4);ticks(1);assert(reply_size==24 && get32(reply+12)==7 && get32(reply+20)==UINT32_MAX);
    put32(body,6);request(7,7,0,0,body,4);ticks(1);assert(reply_size==12 && reply[1]==10);
    /* Busy controls remain pending intact; no second in-flight control can
     * replace the first response while the event queue is blocked. */
    send_busy=1;query(token,8);old_sends=sends;request(6,9,token,0,0,0);ticks(1);assert(sends==old_sends && ui.phase==1);
    send_busy=0;ticks(1);assert(reply[0]==0x84 && reply[2]==8);teardown();
}
static void test_failed_open_shutdown(void) {
    uint8_t body[40]={0};setup();memcpy(body,abc_hash,32);body[32]=1;
    fail_record_write=1;request(2,1,0,3,body,40);ticks(1);assert(reply[1]==5 && ui.archive_bytes==0);
    fail_record_write=0;assert(!rh_connect_pause(receiver));restart_receiver();
    assert(ui.archive_bytes==0);uint32_t token=open_task(3,abc_hash,2,2);
    request(3,0,token,0,"abc",3);ticks(1);query(token,3);assert(ui.written_offset==3);
    fail_sync=1;memcpy(body,abc_hash,32);body[32]=3;request(2,4,0,3,body,40);ticks(1);
    assert(reply[1]==5 && ui.written_offset==3);fail_sync=0;
    /* Failure preserves the old lease so a known token can still cancel. */
    request(6,5,token,0,0,0);ticks(1);assert(ui.phase==6);
    teardown();
}
static void test_committed_install_checkpoint_failure(void) {
    uint32_t token;setup();token=open_task(3,abc_hash,1,1);request(3,0,token,0,"abc",3);ticks(1);
    request(5,2,token,0,0,0);ticks(4);assert(ui.phase==3 && begin_count==1 && step_count==0);
    fail_record_write=1;ticks(1);assert(step_count==1 && ui.phase==4 && ui.detail==0);
    assert(fs_exists(&ctx,"/data/canopus/m.corona/incoming/archive.zip")==1);
    request(6,3,token,0,0,0);ticks(1);assert(reply[1]==2 && ui.phase==4);
    assert(rh_connect_pause(receiver)==-5 && ui.phase==4 && ui.detail==0);
    fail_record_write=0;find_result=1;restart_receiver();ticks(10);
    assert(ui.phase==4 && begin_count==1 && step_count==1);
    assert(fs_exists(&ctx,"/data/canopus/m.corona/incoming/archive.zip")==0);
    teardown();
    /* Also cover an in-process retry without a restart. */
    setup();token=open_task(3,abc_hash,1,1);request(3,0,token,0,"abc",3);ticks(1);
    request(5,2,token,0,0,0);ticks(4);fail_record_write=1;ticks(1);assert(ui.phase==4);
    fail_record_write=0;ticks(1);assert(ui.phase==4 && fs_exists(&ctx,"/data/canopus/m.corona/incoming/archive.zip")==0);
    teardown();
}
static void test_deleted_package_reinstall(void) {
    setup();uint32_t token=open_task(3,abc_hash,1,1);request(3,0,token,0,"abc",3);ticks(1);
    request(5,2,token,0,0,0);ticks(10);assert(ui.phase==4);
    find_result=1;uint32_t newer=open_task(3,abc_hash,2,3);assert(newer>token && ui.phase==4);
    /* Native deletion removes the hash from the registry. A fresh nonce must
     * create a new archive rather than replaying the receiver's old terminal. */
    find_result=0;token=open_task(3,abc_hash,3,4);assert(token>newer && ui.phase==1 && ui.written_offset==0);
    request(3,0,token,0,"abc",3);ticks(1);request(5,5,token,0,0,0);ticks(10);
    assert(ui.phase==4 && begin_count==2 && step_count==2);teardown();
}
static void test_cached_open_repairs_high_water(void) {
    uint8_t baseline[128],body[40]={0};char name[512];setup();
    realpath_for(name,"/data/canopus/m.corona/incoming/state.0");int fd=open(name,O_RDONLY);
    assert(fd>=0 && read(fd,baseline,sizeof(baseline))==sizeof(baseline) && !close(fd));
    memcpy(body,abc_hash,32);body[32]=1;record_sync_fail_after=1;
    request(2,1,0,3,body,40);ticks(1);assert(reply[1]==5);crash();record_sync_fail_after=-1;
    /* Simulate power loss before the second slot's sync completed. */
    fd=open(name,O_WRONLY|O_TRUNC);assert(fd>=0 && write(fd,baseline,sizeof(baseline))==sizeof(baseline) && !close(fd));
    assert(!rh_connect_create(&ctx,&hooks,&receiver));assert(!rh_connect_start(receiver));
    uint32_t token=open_task(3,abc_hash,1,2);assert(token==1);
    /* Lose the original surviving slot after the cached token was exposed. */
    realpath_for(name,"/data/canopus/m.corona/incoming/state.1");fd=open(name,O_WRONLY|O_TRUNC);
    assert(fd>=0 && write(fd,"torn",4)==4 && !close(fd));restart_receiver();
    assert(open_task(3,abc_hash,2,3)>token);teardown();
}
static void test_oversized_controls(void) {
    uint8_t extra[60]={0};setup();uint32_t token=open_task(3,abc_hash,1,1);
    for(uint8_t op=4;op<=6;op++) {
        request(op,(uint16_t)(op+1u),token,0,extra,sizeof(extra));ticks(1);
        assert(reply_size==12 && reply[0]==(uint8_t)(op|0x80u) && reply[1]==1 && ui.phase==1);
    }
    teardown();
}
static void test_ack_survives_control_reply(void) {
    uint8_t version[2]={1,0},revision[4]={0};setup();uint32_t token=open_task(100000,large_hash,1,1);
    send_busy=1;upload(token,0,1872);request(1,2,0,0,version,2);ticks(1);
    send_busy=0;ticks(1);assert(reply[0]==0x81);ticks(1);assert(reply[0]==0x83 && get32(reply+8)==1872);
    send_busy=1;upload(token,1872,3744);request(7,3,0,0,revision,4);ticks(1);
    send_busy=0;ticks(1);assert(reply[0]==0x87);ticks(1);assert(reply[0]==0x83 && get32(reply+8)==3744);
    teardown();
}
static void test_installer_busy_is_retryable(void) {
    setup();uint32_t token=open_task(3,abc_hash,1,1);request(3,0,token,0,"abc",3);ticks(1);
    request(5,2,token,0,0,0);ticks(3);assert(ui.phase==3);
    find_result=-2;ticks(3);assert(ui.phase==3 && !ui.detail && !begin_count);
    find_result=0;begin_result=-2;ticks(3);assert(ui.phase==3 && !ui.detail && begin_count==3 && !step_count);
    begin_result=0;install_result=-2;ticks(3);assert(ui.phase==3 && !ui.detail && begin_count==4 && step_count==2);
    install_result=1;ticks(1);assert(ui.phase==4 && step_count==3 && abort_count==1);
    teardown();
}
static void test_token_exhaustion(void) {
    char name[512];uint8_t record[128];setup();(void)open_task(3,abc_hash,1,1);crash();
    for(unsigned slot=0;slot<2;slot++) {
        snprintf(name,sizeof(name),"%s/incoming/state.%u",sandbox,slot);int fd=open(name,O_RDWR);assert(fd>=0 && read(fd,record,sizeof(record))==sizeof(record));
        put32(record+12,UINT32_MAX);put32(record+124,crc(record,124));assert(lseek(fd,0,SEEK_SET)==0 && write(fd,record,sizeof(record))==sizeof(record));assert(!close(fd));
    }
    assert(!rh_connect_create(&ctx,&hooks,&receiver));assert(!rh_connect_start(receiver));uint8_t body[40]={0};memcpy(body,abc_hash,32);body[32]=2;
    request(2,2,0,3,body,40);ticks(1);assert(reply[1]==8 && reply_size==12);teardown();
}
int main(void) {
    test_roundtrip();test_retry_and_barriers();test_backpressure_and_ack();test_short_write_failure();test_reboot_checkpoint();
    test_hash_and_install_recovery();test_corruption_and_high_water();test_failed_open_and_limits();test_failed_open_shutdown();test_token_exhaustion();
    test_committed_install_checkpoint_failure();test_deleted_package_reinstall();test_cached_open_repairs_high_water();
    test_oversized_controls();test_ack_survives_control_reply();test_installer_busy_is_retryable();
    puts("corona_connect: production receiver, persistence and fault checks passed");return 0;
}

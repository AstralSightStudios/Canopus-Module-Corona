#define _POSIX_C_SOURCE 200809L
#include "corona_archive.h"
#include "canopus_services.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned live, peak, handles, writes, write_limit, read_limit, fail_write, fail_sync, fail_close, fail_size, short_size;
static int fs_open(const struct canopus_context_v1 *ctx,const char *name,uint32_t flags) {
    (void)ctx;int fd=open(name,(flags&CANOPUS_FS_WRITE)?O_WRONLY|O_CREAT|O_TRUNC:O_RDONLY,0600);
    if(fd>=0)handles++;return fd<0?-errno:fd;
}
static int fs_read(const struct canopus_context_v1 *ctx,int fd,void *p,uint32_t n) {
    (void)ctx;if(read_limit && n>read_limit)n=read_limit;ssize_t got=read(fd,p,n);return got<0?-errno:(int)got;
}
static int fs_write(const struct canopus_context_v1 *ctx,int fd,const void *p,uint32_t n) {
    (void)ctx;if(fail_write && ++writes==fail_write)return -EIO;
    if(write_limit && n>write_limit)n=write_limit;ssize_t wrote=write(fd,p,n);return wrote<0?-errno:(int)wrote;
}
static int64_t fs_seek(const struct canopus_context_v1 *ctx,int fd,int64_t at,int whence) {
    (void)ctx;return lseek(fd,(off_t)at,whence==CANOPUS_FS_SEEK_SET?SEEK_SET:whence==CANOPUS_FS_SEEK_CUR?SEEK_CUR:SEEK_END);
}
static int64_t fs_size(const struct canopus_context_v1 *ctx,int fd) {(void)ctx;struct stat s;if(fail_size)return -EIO;if(fstat(fd,&s))return -errno;return s.st_size-(short_size?1:0);}
static int fs_sync(const struct canopus_context_v1 *ctx,int fd) {(void)ctx;if(fail_sync)return -EIO;return fsync(fd)?-errno:0;}
static int fs_close(const struct canopus_context_v1 *ctx,int fd) {(void)ctx;handles--;int rc=close(fd);return fail_close?-EIO:(rc?-errno:0);}
static int fs_exists(const struct canopus_context_v1 *ctx,const char *p) {(void)ctx;struct stat s;return stat(p,&s)?(errno==ENOENT?0:-errno):1;}
static int fs_stat(const struct canopus_context_v1 *ctx,const char *p,struct canopus_fs_stat *out) {(void)ctx;struct stat s;if(stat(p,&s))return -errno;out->is_dir=S_ISDIR(s.st_mode);out->size=(uint32_t)s.st_size;return 0;}
static int fs_mkdir(const struct canopus_context_v1 *ctx,const char *p) {(void)ctx;return mkdir(p,0700)?-errno:0;}
static void *mem_alloc(const struct canopus_context_v1 *ctx,uint32_t n) {(void)ctx;uint32_t *p=malloc(n+sizeof(uint64_t));if(!p)return 0;*p=n;live+=n;if(live>peak)peak=live;return (uint8_t *)p+sizeof(uint64_t);}
static void mem_free(const struct canopus_context_v1 *ctx,void *p) {(void)ctx;if(p){uint32_t *h=(uint32_t *)((uint8_t *)p-sizeof(uint64_t));live-=*h;free(h);}}
static int fs_root(const struct canopus_context_v1 *ctx,char *p,uint32_t n) {(void)ctx;return getcwd(p,n)?0:-errno;}
static const struct canopus_fs_obj fs={.get_data_path=fs_root,.open=fs_open,.read=fs_read,.write=fs_write,.seek=fs_seek,.size=fs_size,.sync=fs_sync,.close=fs_close,.exists=fs_exists,.stat=fs_stat,.mkdir=fs_mkdir};
static const struct canopus_mem_obj mem={.alloc=mem_alloc,.free=mem_free};
static const struct canopus_context_v1 ctx={.fs=&fs,.mem=&mem};
int main(int argc,char **argv) {
    assert(argc>=5);struct stat st;assert(!stat(argv[1],&st));
    int expected=atoi(argv[3]);unsigned mode=(unsigned)atoi(argv[4]);
    if(mode==1){read_limit=7;write_limit=13;}
    if(mode==2)fail_write=2;
    if(mode==3)fail_sync=1;
    if(mode==4)fail_close=1;
    if(mode==6)fail_size=1;
    if(mode==7)short_size=1;
    const char *staging=argv[2];
    if(mode==5){char parent[1024];snprintf(parent,sizeof(parent),"%s",argv[2]);char *slash=strrchr(parent,'/');assert(slash);*slash=0;assert(!chdir(parent));staging=strrchr(argv[2],'/')+1;}
    struct rh_archive *a=0;int rc=rh_archive_create(&ctx,argv[1],(uint32_t)st.st_size,staging,&a);unsigned steps=0;
    if(!rc){char not_ready[256];assert(rh_archive_manifest(a,not_ready)==-7);}
    while(!rc && ++steps<500000u)rc=rh_archive_step(a);
    if(rc==1){char manifest[256];assert(!rh_archive_manifest(a,manifest));assert(strstr(manifest,"corona.json")||strstr(manifest,"canora.json"));assert(rh_archive_step(a)==1);}
    if(rc<0 && a)assert(rh_archive_step(a)==rc);
    rh_archive_destroy(a);assert(!live && !handles);assert(peak<48u*1024u);
    if(rc!=expected){fprintf(stderr,"%s: got %d, expected %d after %u steps\n",argv[1],rc,expected,steps);return 1;}
    printf("%d %u %u\n",rc,steps,peak);return 0;
}

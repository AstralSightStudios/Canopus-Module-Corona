/* End-to-end host integration of the production runtime, receiver, ZIP reader
 * and catalog. The only adapters are SDK I/O, timers and page notifications. */
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "corona_runtime.h"
#include "manager_data.h"
#include "manager_state.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *sandbox;
static struct rh_manager_data *catalog;
static struct rh_transfer_ui_state state;
static canopus_connect_callback_t receive_callback;
static void *receive_cookie;
static canopus_timer_callback_t timer_callback;
static uint8_t response[480];
static uint32_t response_bytes, events, background, live, peak;
static int fail_timer, fail_listen, fail_install_rename, fail_cancel_remove, fail_rmdir;
static uint32_t rmdir_calls;
static void path(char out[1024],const char *p) {
    const char *root="/data/canopus/m.corona";
    if(!strncmp(p,root,strlen(root))) {p+=strlen(root);if(*p=='/')p++;}
    if(*p=='/') {assert(snprintf(out,1024,"%s/unavailable%s",sandbox,p)<1024);return;}
    assert(snprintf(out,1024,"%s/%s",sandbox,p)<1024);
}
static int fs_open(const struct canopus_context_v1 *c,const char *p,uint32_t flags) {
    char out[1024];int f=O_RDONLY,fd;(void)c;path(out,p);
    if(flags&CANOPUS_FS_WRITE) {
        f=((flags&CANOPUS_FS_READ)?O_RDWR:O_WRONLY)|O_CREAT;
        if((flags&CANOPUS_FS_TRUNC)||(!(flags&CANOPUS_FS_READ)&&!(flags&CANOPUS_FS_APPEND)))f|=O_TRUNC;
        if(flags&CANOPUS_FS_APPEND)f|=O_APPEND;
    }
    fd=open(out,f,0600);return fd<0?-errno:fd;
}
static int fs_read(const struct canopus_context_v1 *c,int fd,void *p,uint32_t n) {(void)c;ssize_t r=read(fd,p,n);return r<0?-errno:(int)r;}
static int fs_write(const struct canopus_context_v1 *c,int fd,const void *p,uint32_t n) {(void)c;if(n>97)n=97;ssize_t r=write(fd,p,n);return r<0?-errno:(int)r;}
static int64_t fs_seek(const struct canopus_context_v1 *c,int fd,int64_t offset,int32_t whence) {(void)c;off_t r=lseek(fd,(off_t)offset,whence);return r<0?-errno:r;}
static int64_t fs_size(const struct canopus_context_v1 *c,int fd) {struct stat s;(void)c;return fstat(fd,&s)<0?-errno:s.st_size;}
static int fs_close(const struct canopus_context_v1 *c,int fd) {(void)c;return close(fd)<0?-errno:0;}
static int fs_sync(const struct canopus_context_v1 *c,int fd) {(void)c;return fsync(fd)<0?-errno:0;}
static int fs_remove(const struct canopus_context_v1 *c,const char *p) {char out[1024];(void)c;path(out,p);if(fail_cancel_remove&&!strcmp(p,"registry-install.pending"))return -EIO;return unlink(out)<0?-errno:0;}
static int fs_rmdir(const struct canopus_context_v1 *c,const char *p) {
    char out[1024];
    assert(canopus_services_has(c,CANOPUS_SVC_FS,2));
    path(out,p);rmdir_calls++;
    if(fail_rmdir)return -EIO;
    if(rmdir(out)==0)return 0;
    return errno==ENOTEMPTY ? CANOPUS_SVC_ERR_NOTEMPTY:-errno;
}
static int fs_rename(const struct canopus_context_v1 *c,const char *a,const char *b) {char x[1024],y[1024];(void)c;path(x,a);path(y,b);if(fail_install_rename && strstr(b,"themes/integration/"))return -EIO;return rename(x,y)<0?-errno:0;}
static int fs_stat(const struct canopus_context_v1 *c,const char *p,struct canopus_fs_stat *out) {char x[1024];struct stat s;(void)c;path(x,p);if(stat(x,&s)<0)return -errno;out->size=(uint32_t)s.st_size;out->is_dir=S_ISDIR(s.st_mode);return 0;}
static int fs_exists(const struct canopus_context_v1 *c,const char *p) {struct canopus_fs_stat s;int r=fs_stat(c,p,&s);return r==-ENOENT?0:(r?r:1);}
static int fs_mkdir(const struct canopus_context_v1 *c,const char *p) {char x[1024];(void)c;path(x,p);return mkdir(x,0700)<0?-errno:0;}
static void *fs_opendir(const struct canopus_context_v1 *c,const char *p) {char x[1024];(void)c;path(x,p);return opendir(x);}
static int fs_readdir(const struct canopus_context_v1 *c,void *dir,struct canopus_fs_dirent *out) {struct dirent *e;(void)c;errno=0;e=readdir(dir);if(!e)return errno?-errno:1;assert(strlen(e->d_name)<sizeof(out->name));strcpy(out->name,e->d_name);out->is_dir=e->d_type==DT_DIR;return 0;}
static int fs_closedir(const struct canopus_context_v1 *c,void *dir) {(void)c;return closedir(dir)<0?-errno:0;}
static int fs_root(const struct canopus_context_v1 *c,char *p,uint32_t size) {(void)c;assert(size>23);strcpy(p,"/data/canopus/m.corona");return 0;}
struct allocation { uint64_t size,alignment; };
static void *mem_alloc(const struct canopus_context_v1 *c,uint32_t n) {(void)c;struct allocation *p=malloc(sizeof(*p)+n);assert(p);p->size=n;live+=n;if(live>peak)peak=live;return p+1;}
static void mem_free(const struct canopus_context_v1 *c,void *p) {(void)c;if(p){struct allocation *a=(struct allocation *)p-1;assert(live>=a->size);live-=(uint32_t)a->size;free(a);}}
static int connect_listen(const struct canopus_context_v1 *c,canopus_connect_callback_t cb,void *cookie) {(void)c;if(cb&&fail_listen)return -5;receive_callback=cb;receive_cookie=cookie;return 0;}
static int connect_send(const struct canopus_context_v1 *c,uint32_t kind,const void *p,uint32_t n) {(void)c;assert(kind==CANOPUS_CONNECT_BINARY&&n<=480);memcpy(response,p,n);response_bytes=n;events++;return 0;}
static int remote_listening(const struct canopus_context_v1 *c) {(void)c;return 1;}
static int timer_create(const struct canopus_context_v1 *c,uint32_t ms,int periodic,canopus_timer_callback_t cb,void *cookie) {(void)c;(void)cookie;assert(ms==10&&periodic);timer_callback=cb;return 3;}
static int timer_start(const struct canopus_context_v1 *c,int32_t id) {(void)c;assert(id==3);return fail_timer?-5:0;}
static int timer_destroy(const struct canopus_context_v1 *c,int32_t id) {(void)c;assert(id==3);timer_callback=0;return 0;}
static int timer_background(const struct canopus_context_v1 *c,int32_t id,int on) {(void)c;assert(id==3&&on);background++;return 0;}
static const struct canopus_fs_obj fs={.open=fs_open,.read=fs_read,.write=fs_write,.seek=fs_seek,.size=fs_size,.close=fs_close,.sync=fs_sync,.remove=fs_remove,.rename=fs_rename,.stat=fs_stat,.exists=fs_exists,.mkdir=fs_mkdir,.opendir=fs_opendir,.readdir=fs_readdir,.closedir=fs_closedir,.get_data_path=fs_root,.rmdir=fs_rmdir};
static const struct canopus_mem_obj mem={.alloc=mem_alloc,.free=mem_free};
static const struct canopus_connect_obj connect_api={.listen=connect_listen,.send=connect_send,.remote_listening=remote_listening};
static const struct canopus_time_obj time_api={.timer_create=timer_create,.timer_start=timer_start,.timer_destroy=timer_destroy,.timer_set_background=timer_background};
static struct canopus_services_v1 services;
static const struct canopus_context_v1 ctx={.struct_size=sizeof(ctx),.services=&services,.fs=&fs,.mem=&mem,.connect=&connect_api,.time=&time_api};
int rh_manager_ui_get_data(const struct canopus_context_v1 *c,struct rh_manager_data **out) {int rc=0;if(!catalog)rc=rh_manager_data_create(c,0,&catalog);if(!rc)*out=catalog;return rc;}
int rh_manager_set_transfer_state(const struct rh_transfer_ui_state *s) {state=*s;return 0;}
static void assert_staging_empty(void) {
    struct canopus_fs_dirent entry;void *dir=fs_opendir(&ctx,"staging");int rc;
    assert(dir);
    while((rc=fs_readdir(&ctx,dir,&entry))==0)
        assert(!strcmp(entry.name,".")||!strcmp(entry.name,".."));
    assert(rc==1);assert(!fs_closedir(&ctx,dir));
}
static void put32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void tick(void) {uint32_t before=rmdir_calls;assert(timer_callback);timer_callback(&ctx,3,0);assert(rmdir_calls-before<=1u);}
static void request(uint8_t op,uint16_t seq,uint32_t token,uint32_t arg,const void *payload,uint32_t n) {
    uint8_t frame[480]={0};assert(receive_callback&&n<=468);frame[0]=op;frame[2]=(uint8_t)seq;frame[3]=(uint8_t)(seq>>8);put32(frame+4,token);put32(frame+8,arg);if(n)memcpy(frame+12,payload,n);receive_callback(&ctx,CANOPUS_CONNECT_BINARY,frame,n+12,receive_cookie);
}
static void control(uint8_t op,uint16_t seq,uint32_t token,uint32_t arg,const void *payload,uint32_t n) {
    uint32_t before=events;request(op,seq,token,arg,payload,n);
    for(unsigned i=0;i<1000;i++){tick();if(events>before&&response[0]==(op|0x80)&&response[2]==(uint8_t)seq)return;}
    assert(!"control response timed out");
}
int main(int argc,char **argv) {
    uint8_t *data,open_payload[40]={0},version[2]={1,0},revision[4]={0};uint32_t size,token;FILE *f;
    assert(argc==5);sandbox=argv[1];services.group_version[CANOPUS_SVC_TIME]=2;
    services.group_version[CANOPUS_SVC_FS]=!strcmp(argv[4],"legacy") ? 1:2;
    fail_rmdir=!strcmp(argv[4],"rmdir-failure");
    f=fopen(argv[2],"rb");assert(f);assert(!fseek(f,0,SEEK_END));size=(uint32_t)ftell(f);rewind(f);data=malloc(size);assert(data&&fread(data,1,size,f)==size);fclose(f);
    assert(strlen(argv[3])==64);for(unsigned i=0;i<32;i++){unsigned v;assert(sscanf(argv[3]+2*i,"%2x",&v)==1);open_payload[i]=(uint8_t)v;}open_payload[32]=1;
    fail_timer=1;assert(rh_connect_runtime_start(&ctx)<0);assert(!timer_callback&&!receive_callback&&!live);fail_timer=0;
    fail_listen=1;assert(rh_connect_runtime_start(&ctx)<0);assert(!timer_callback&&!receive_callback&&!live);fail_listen=0;
    assert(!rh_connect_runtime_start(&ctx));assert(!rh_connect_runtime_start(&ctx));assert(background>=1);
    control(1,1,0,0,version,2);assert(response_bytes==32&&!response[1]);
    control(2,2,0,size,open_payload,40);assert(!response[1]);token=get32(response+4);assert(token);
    for(uint32_t offset=0;offset<size;){uint32_t n=size-offset;if(n>468)n=468;request(3,0,token,offset,data+offset,n);offset+=n;tick();}
    control(4,3,token,0,0,0);assert(!response[1]&&get32(response+8)==size);
    assert(catalog && !rh_manager_data_prepare_apply(catalog,"active/900"));
    control(5,4,token,0,0,0);assert(!response[1]);
    for(unsigned i=0;i<2000;i++)tick();
    if(state.phase!=RH_XFER_PHASE_INSTALLING)fprintf(stderr,"held apply phase=%u detail=%u\n",state.phase,state.detail);
    assert(state.phase==RH_XFER_PHASE_INSTALLING);
    assert(!rh_manager_data_complete_apply(catalog,0));
    fail_install_rename=!strcmp(argv[4],"recovery") || !strcmp(argv[4],"cancel");
    for(unsigned i=0;i<200000&&state.phase!=RH_XFER_PHASE_INSTALLED&&state.phase!=RH_XFER_PHASE_ERROR;i++)tick();
    if(fail_install_rename) {
        assert(state.phase==RH_XFER_PHASE_ERROR && state.detail==5);
        for(unsigned i=0;i<1000;i++)tick();
        assert(fs_exists(&ctx,"registry-install.pending")==1);
        if(!strcmp(argv[4],"cancel")) {
            fail_cancel_remove=1;control(6,11,token,0,0,0);
            assert(response[1]==5 && response[18]!=RH_XFER_PHASE_CANCELED);
            fail_cancel_remove=0;control(6,12,token,0,0,0);
            assert(!response[1]&&response[18]==RH_XFER_PHASE_CANCELED);
            assert(!fs_exists(&ctx,"registry-install.pending"));
            fail_install_rename=0;for(unsigned i=0;i<1000;i++)tick();
            control(7,13,0,0,revision,4);assert(!response[1]&&!get32(response+16));
            assert_staging_empty();
            rh_connect_runtime_stop();rh_manager_data_destroy(catalog);catalog=0;
            assert(!rh_connect_runtime_start(&ctx));
            control(7,14,0,0,revision,4);assert(!response[1]&&!get32(response+16));
            rh_connect_runtime_stop();rh_manager_data_destroy(catalog);catalog=0;assert(!live);
            free(data);puts("Corona Connect cancellation revokes pending registry recovery.");return 0;
        }
        rh_connect_runtime_stop();rh_manager_data_destroy(catalog);catalog=0;
        fail_install_rename=0;
        assert(!rh_connect_runtime_start(&ctx));
        assert(fs_exists(&ctx,"registry-install.pending")==0);
        /* Registry replay must consume staging before background cleanup. */
        struct rh_manager_metadata recovered;
        assert(!rh_manager_data_metadata(catalog,"integration",&recovered) && recovered.available);
        control(5,8,token,0,0,0);
        if(response[1]) {
            open_payload[32]=3;control(2,9,0,size,open_payload,40);assert(!response[1]);
            token=get32(response+4);control(5,10,token,0,0,0);assert(!response[1]);
        }
        for(unsigned i=0;i<200000&&state.phase!=RH_XFER_PHASE_INSTALLED;i++)tick();
    }
    if(state.phase!=RH_XFER_PHASE_INSTALLED)fprintf(stderr,"phase=%u detail=%u\n",state.phase,state.detail);
    assert(state.phase==RH_XFER_PHASE_INSTALLED);assert(catalog);
    for(unsigned i=0;i<1000;i++)tick();
    assert(!fs_exists(&ctx,"staging/abandoned/nested/old.bin"));
    if(!strcmp(argv[4],"legacy")) {
        assert(!rmdir_calls && fs_exists(&ctx,"staging/abandoned")==1);
    } else if(fail_rmdir) {
        assert(rmdir_calls && fs_exists(&ctx,"staging/abandoned")==1);
        /* A GC failure cannot undo an installed package. Retry on restart. */
        fail_rmdir=0;
    } else assert(!fs_exists(&ctx,"staging/abandoned"));
    control(7,5,0,0,revision,4);assert(!response[1]&&response_bytes>=38&&get32(response+16)==1);
    struct rh_manager_metadata metadata;assert(!rh_manager_data_metadata(catalog,"integration",&metadata)&&metadata.available);
    rh_connect_runtime_stop();assert(!timer_callback&&!receive_callback);rh_manager_data_destroy(catalog);catalog=0;
    assert(!rh_connect_runtime_start(&ctx));open_payload[32]=2;
    control(7,6,0,0,revision,4);assert(!response[1]&&get32(response+16)==1);
    control(2,7,0,size,open_payload,40);assert(!response[1]&&response[18]==RH_XFER_PHASE_INSTALLED);
    for(unsigned i=0;i<1000;i++)tick();
    if(services.group_version[CANOPUS_SVC_FS]>=2) {
        assert(!fs_exists(&ctx,"staging/abandoned"));assert_staging_empty();
    }
    rh_connect_runtime_stop();if(catalog)rh_manager_data_destroy(catalog);catalog=0;assert(!live);
    free(data);printf("Corona Connect production runtime integration passed (peak %u bytes).\n",peak);return 0;
}

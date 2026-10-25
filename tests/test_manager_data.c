#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "manager_data.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char sandbox[512];
static int fail_write,fail_rename,fail_unlink,fail_close,alloc_budget=-1;
static const char *fail_write_path,*fail_rename_path,*fail_stat_path,*fail_remove_path,*fail_sync_path;
static int sync_fault_fd=-1;
static const char *fail_rmdir_path;
static int rmdir_error;
static uint32_t rmdir_calls;
static uint32_t directory_reads,file_syncs,open_directories;
static uint32_t blocks,peak_blocks,live_bytes,peak_bytes;
struct allocation_header { uint64_t bytes; uint64_t alignment; };
static int resolve(char out[1024],const char *p) {
    if(p[0]=='/') {
        if(strncmp(p,"/data/canopus/m.corona/",23)==0) p+=23;
        else if(strcmp(p,"/data/canopus/m.corona")==0) p="";
        else if(strncmp(p,"/data/quickapp/files/ng.lst.corona/",34)==0) {
            return snprintf(out,1024,"%s/legacy/%s",sandbox,p+34)>=1024 ? -22:0;
        }
        else if(strncmp(p,"/data/files/ng.lst.corona/",25)==0) {
            return snprintf(out,1024,"%s/legacy/%s",sandbox,p+25)>=1024 ? -22:0;
        }
        else if(strncmp(p,"/resource/",10)==0) p+=1;
        else return -ENOENT;
    }
    return snprintf(out,1024,"%s/%s",sandbox,p)>=1024 ? -22:0;
}
static int fs_open(const struct canopus_context_v1 *c,const char *p,uint32_t f) {
    char path[1024]; int rc;(void)c;if((f&CANOPUS_FS_WRITE)&&fail_write_path&&!strcmp(p,fail_write_path))return -EIO;rc=resolve(path,p);if(rc)return rc;
    rc=open(path,(f&CANOPUS_FS_WRITE) ? O_WRONLY|O_TRUNC|O_CREAT:O_RDONLY,0600);if(rc>=0&&fail_sync_path&&!strcmp(p,fail_sync_path))sync_fault_fd=rc;return rc<0?-errno:rc;
}
static int fs_read(const struct canopus_context_v1 *c,int fd,void *p,uint32_t n) {(void)c;ssize_t rc=read(fd,p,n);return rc<0?-errno:(int)rc;}
static int fs_write(const struct canopus_context_v1 *c,int fd,const void *p,uint32_t n) {(void)c;if(fail_write)return -EIO; if(n>7)n=7;ssize_t rc=write(fd,p,n);return rc<0?-errno:(int)rc;}
static int fs_close(const struct canopus_context_v1 *c,int fd) {(void)c;if(fd==sync_fault_fd)sync_fault_fd=-1;int rc=close(fd);return fail_close ? -EIO:(rc<0?-errno:0);}
static int64_t fs_size(const struct canopus_context_v1 *c,int fd) {struct stat s;(void)c;return fstat(fd,&s)<0?-errno:s.st_size;}
static int fs_sync(const struct canopus_context_v1 *c,int fd) {(void)c;file_syncs++;if(fd==sync_fault_fd)return -EIO;return fsync(fd)<0?-errno:0;}
static int fs_remove(const struct canopus_context_v1 *c,const char *p) {char path[1024];(void)c;if(fail_unlink||(fail_remove_path&&!strcmp(p,fail_remove_path)))return -EIO;int rc=resolve(path,p);return rc?rc:(unlink(path)<0?-errno:0);}
static int fs_rmdir(const struct canopus_context_v1 *c,const char *p) {
    char path[1024];int rc;
    assert(canopus_services_has(c,CANOPUS_SVC_FS,2));rmdir_calls++;
    if(fail_rmdir_path&&!strcmp(p,fail_rmdir_path))return rmdir_error;
    rc=resolve(path,p);if(rc)return rc;
    if(rmdir(path)==0)return 0;
    return errno==ENOTEMPTY ? CANOPUS_SVC_ERR_NOTEMPTY:-errno;
}
static int fs_rename(const struct canopus_context_v1 *c,const char *a,const char *b) {char x[1024],y[1024];(void)c;if(fail_rename||(fail_rename_path&&!strcmp(b,fail_rename_path)))return -EIO;int rc=resolve(x,a);if(!rc)rc=resolve(y,b);return rc?rc:(rename(x,y)<0?-errno:0);}
static int fs_stat(const struct canopus_context_v1 *c,const char *p,struct canopus_fs_stat *o) {char path[1024];struct stat s;(void)c;if(fail_stat_path&&!strcmp(p,fail_stat_path))return -EIO;int rc=resolve(path,p);if(rc)return rc;if(stat(path,&s)<0)return -errno;o->size=(uint32_t)s.st_size;o->is_dir=(uint8_t)S_ISDIR(s.st_mode);return 0;}
static int fs_exists(const struct canopus_context_v1 *c,const char *p) {struct canopus_fs_stat st;int rc=fs_stat(c,p,&st);return rc==-ENOENT?0:(rc?rc:1);}
static int fs_mkdir(const struct canopus_context_v1 *c,const char *p) {char path[1024];(void)c;int rc=resolve(path,p);return rc?rc:(mkdir(path,0700)<0?-errno:0);}
static void *fs_opendir(const struct canopus_context_v1 *c,const char *p) {char path[1024];void *dir;(void)c;if(resolve(path,p))return 0;dir=opendir(path);if(dir)open_directories++;return dir;}
static int fs_readdir(const struct canopus_context_v1 *c,void *dir,struct canopus_fs_dirent *out) {
    struct dirent *e;(void)c;directory_reads++;errno=0;e=readdir(dir);if(!e)return errno?-errno:1;
    if(strlen(e->d_name)>=sizeof(out->name))return -22;strcpy(out->name,e->d_name);out->is_dir=e->d_type==DT_DIR;return 0;
}
static int fs_closedir(const struct canopus_context_v1 *c,void *dir) {(void)c;assert(open_directories);open_directories--;return closedir(dir)<0?-errno:0;}
static int fs_root(const struct canopus_context_v1 *c,char *p,uint32_t n) {(void)c;if(n<24)return -22;strcpy(p,"/data/canopus/m.corona");return 0;}
static void *mem_alloc(const struct canopus_context_v1 *c,uint32_t n) {
    (void)c;if(alloc_budget==0||blocks>=128u||n>262144u-live_bytes)return 0;if(alloc_budget>0)alloc_budget--;
    struct allocation_header *p=malloc(sizeof(*p)+n);if(!p)return 0;p->bytes=n;
    blocks++;live_bytes+=n;if(blocks>peak_blocks)peak_blocks=blocks;if(live_bytes>peak_bytes)peak_bytes=live_bytes;return p+1;
}
static void mem_free(const struct canopus_context_v1 *c,void *p) {(void)c;if(p){struct allocation_header *h=(struct allocation_header *)p-1;assert(blocks&&live_bytes>=h->bytes);blocks--;live_bytes-=(uint32_t)h->bytes;free(h);}}
static const struct canopus_fs_obj fs={.open=fs_open,.read=fs_read,.write=fs_write,.close=fs_close,.size=fs_size,.sync=fs_sync,.remove=fs_remove,.rename=fs_rename,.stat=fs_stat,.exists=fs_exists,.mkdir=fs_mkdir,.opendir=fs_opendir,.readdir=fs_readdir,.closedir=fs_closedir,.get_data_path=fs_root,.rmdir=fs_rmdir};
static const struct canopus_mem_obj mem={.alloc=mem_alloc,.free=mem_free};
static const struct canopus_context_v1 ctx={.struct_size=sizeof(ctx),.fs=&fs,.mem=&mem};
static void fixture(const char *name,const char *data) {int fd=fs_open(&ctx,name,CANOPUS_FS_WRITE);assert(fd>=0);assert(write(fd,data,strlen(data))==(ssize_t)strlen(data));assert(!close(fd));}
static char *read_fixture(const char *name) {int fd=fs_open(&ctx,name,CANOPUS_FS_READ);assert(fd>=0);int64_t n=fs_size(&ctx,fd);assert(n>=0);char *s=malloc((size_t)n+1);assert(s);assert(read(fd,s,(size_t)n)==n);s[n]=0;close(fd);return s;}
static void make_pack(const char *id,const char *body) {char path[160];snprintf(path,sizeof(path),"themes/%s",id);assert(!fs_mkdir(&ctx,path));snprintf(path,sizeof(path),"themes/%s/corona.json",id);fixture(path,body);}
static const char *alpha="{\"format\":\"canopus-resource-pack\",\"formatVersion\":1,\"themeId\":\"alpha\",\"name\":\"主题 A\",\"author\":\"Alice\",\"version\":\"1.2\",\"versionCode\":9007199254740991,\"description\":\"详情内容\",\"targets\":[\"Band 11\"],\"mappings\":[{\"source\":\"/resource/a/\",\"destination\":\"icons/\"}],\"backgrounds\":{\"launcher\":\"bg.bin\"},\"calendar\":{\"day\":{\"type\":\"text\",\"color\":\"#aBcD01\"}}}";
static const char *beta="{\"format\":\"canopus-resource-pack\",\"formatVersion\":1,\"themeId\":\"beta\",\"name\":\"B\",\"mappings\":[{\"source\":\"/resource/a/x.bin\",\"destination\":\"x.bin\"}],\"backgrounds\":{\"launcher\":\"blur\"}}";
static void setup(void) {
    strcpy(sandbox,"/tmp/corona-manager-data-XXXXXX");assert(mkdtemp(sandbox));
    assert(!fs_mkdir(&ctx,"themes"));make_pack("alpha",alpha);make_pack("beta",beta);
    assert(!fs_mkdir(&ctx,"themes/alpha/icons"));fixture("themes/alpha/icons/x.bin","A");fixture("themes/alpha/icons/y.bin","Y");fixture("themes/alpha/bg.bin","BG");fixture("themes/beta/x.bin","B");
    fixture("interconnect-themes.json","[\"alpha\",\"beta\"]");
    assert(!fs_mkdir(&ctx,"resource"));assert(!fs_mkdir(&ctx,"resource/a"));fixture("resource/a/x.bin","SYSTEM");
}
static void cleanup_dir(const char *path) {DIR *d=opendir(path);assert(d);struct dirent *e;char file[1024];while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;snprintf(file,sizeof(file),"%s/%s",path,e->d_name);struct stat s;assert(!lstat(file,&s));if(S_ISDIR(s.st_mode))cleanup_dir(file);else assert(!unlink(file));}closedir(d);assert(!rmdir(path));}
static void test_core(void) {
    struct rh_manager_data *d=0;struct rh_manager_metadata meta;struct rh_manager_mix_item item;struct rh_manager_option option;char id[65];uint32_t n;char *text;
    setup();assert(!rh_manager_data_create(&ctx,0,&d));assert(d);assert(!strcmp(rh_manager_data_root(d),"/data/canopus/m.corona/"));assert(rh_manager_data_order_count(d)==3);
    assert(!rh_manager_data_order_get(d,0,id,&meta));assert(!strcmp(id,"alpha"));assert(meta.available && meta.has_version_code && meta.version_code==9007199254740991ULL);assert(!strcmp(meta.name,"主题 A"));assert(!strcmp(meta.targets,"Band 11"));
    fail_rename=1;assert(rh_manager_data_move(d,0,2)==-EIO);fail_rename=0;assert(!rh_manager_data_order_get(d,0,id,0));assert(!strcmp(id,"alpha"));
    assert(!rh_manager_data_mix_count(d,&n) && n==7);assert(!rh_manager_data_option_count(d,"/resource/a/x.bin",&n)&&n==4);
    assert(!rh_manager_data_option_get(d,"/resource/a/x.bin",0,&option));assert(!strcmp(option.preview,"/data/canopus/m.corona/themes/alpha/icons/x.bin"));
    assert(!rh_manager_data_option_get(d,"/resource/a/x.bin",1,&option));assert(!strcmp(option.preview,"/.rh_sys/resource/a/x.bin"));
    assert(!rh_manager_data_option_count(d,"feature:launcher",&n)&&n==5);
    fail_stat_path="/data/canopus/m.corona/themes/alpha/bg.bin";
    for(uint32_t i=0;i<n;i++) {
        assert(!rh_manager_data_option_get(d,"feature:launcher",i,&option));
        assert(!option.preview[0]);
        if(i==0) assert(!strcmp(option.id,RH_MANAGER_DEFAULT)&&option.selected);
        if(i==3) assert(!strcmp(option.id,"alpha"));
    }
    fail_stat_path=0;
    assert(!rh_manager_data_choose(d,"/resource/a/x.bin","beta"));assert(!rh_manager_data_mix_get(d,0,&item));assert(!strcmp(item.key,"/resource/a/x.bin")&&!strcmp(item.choice_name,"B"));
    assert(!rh_manager_data_move(d,2,0));assert(!rh_manager_data_option_get(d,"/resource/a/x.bin",0,&option));assert(!strcmp(option.preview,"/.rh_sys/resource/a/x.bin"));
    assert(!rh_manager_data_choose(d,"feature:launcher","alpha"));assert(!rh_manager_data_prepare_apply(d,"active/1"));assert(rh_manager_data_move(d,0,1)==-16);
    text=read_fixture("active/1/mappings.tsv");assert(strstr(text,"/resource/a/x.bin\tthemes/beta/x.bin\n"));assert(!strstr(text,"y.bin"));free(text);
    text=read_fixture("active/1/backgrounds.cfg");assert(strstr(text,"launcher=themes/alpha/bg.bin"));free(text);
    fixture("active-config","active/1\n");assert(!rh_manager_data_complete_apply(d,1));assert(rh_manager_data_delete(d,"alpha")==RH_MANAGER_ERR_ACTIVE);assert(rh_manager_data_delete(d,"beta")==RH_MANAGER_ERR_ACTIVE);
    assert(!rh_manager_data_choose(d,"feature:launcher",RH_MANAGER_SYSTEM));assert(!rh_manager_data_choose(d,"/resource/a/x.bin",RH_MANAGER_SYSTEM));assert(!rh_manager_data_prepare_apply(d,"active/2"));fixture("active-config","active/2\n");assert(!rh_manager_data_complete_apply(d,1));
    assert(fs_exists(&ctx,"active/1/mappings.tsv")==0);assert(fs_exists(&ctx,"active/2/mappings.tsv")==1);
    fail_unlink=1;assert(rh_manager_data_delete(d,"beta")==-EIO);fail_unlink=0;assert(rh_manager_data_order_count(d)==3);assert(!rh_manager_data_delete(d,"beta"));assert(rh_manager_data_order_count(d)==2);assert(fs_exists(&ctx,"themes/beta")==1);assert(fs_exists(&ctx,"themes/beta/corona.json")==0);
    fixture("resource-active-generations.json","broken");assert(rh_manager_data_delete(d,"alpha")==-22);
    rh_manager_data_destroy(d);assert(!blocks);cleanup_dir(sandbox);
}
static void test_apply_overlap(void) {
    struct rh_manager_data *d=0;char *text;
    setup();assert(!rh_manager_data_create(&ctx,0,&d));assert(!rh_manager_data_prepare_apply(d,"active/3"));
    text=read_fixture("active/3/mappings.tsv");assert(strstr(text,"/resource/a/x.bin\tthemes/alpha/icons/x.bin"));assert(strstr(text,"/resource/a/y.bin\tthemes/alpha/icons/y.bin"));free(text);
    text=read_fixture("active/3/calendar.cfg");assert(strstr(text,"day\ttext\ttop_mid\t0\t34\t0\tABCD01\t0"));free(text);
    assert(!rh_manager_data_complete_apply(d,0));assert(rh_manager_data_prepare_apply(d,"active/3")==-EEXIST);
    rh_manager_data_destroy(d);assert(!blocks);cleanup_dir(sandbox);
}
static void test_faults(void) {
    struct rh_manager_data *d=0;uint32_t baseline;int i,rc;setup();
    fixture("resource-order.json","{\"version\":1,\"order\":[\"@system\",\"@system\"]}");assert(rh_manager_data_create(&ctx,0,&d)==-22);assert(!d&&!blocks);
    assert(!fs_remove(&ctx,"resource-order.json"));
    for(i=0;i<30;i++) {alloc_budget=i;rc=rh_manager_data_create(&ctx,0,&d);alloc_budget=-1;if(!rc)rh_manager_data_destroy(d);else assert(rc==-12);assert(!blocks);}
    assert(!rh_manager_data_create(&ctx,0,&d));baseline=blocks;alloc_budget=0;rc=rh_manager_data_refresh(d);assert(rc==0||rc==-12);alloc_budget=-1;assert(blocks==baseline);assert(rh_manager_data_order_count(d)==3);
    fixture("themes/alpha/corona.json","broken");struct rh_manager_metadata meta;assert(rh_manager_data_metadata(d,"alpha",&meta)==-22);assert(!meta.available&&!strcmp(meta.name,"alpha"));
    rh_manager_data_destroy(d);assert(!blocks);cleanup_dir(sandbox);
}
static void test_installed_metadata(void) {
    struct rh_manager_data *d=0;struct rh_manager_metadata meta;uint32_t count;char *text;
    setup();assert(!rh_manager_data_create(&ctx,0,&d));
    /* Reading installed metadata must not depend on an unused legacy stat. */
    fail_stat_path="themes/alpha/canora.json";
    assert(!rh_manager_data_metadata(d,"alpha",&meta));assert(meta.available&&!strcmp(meta.name,"主题 A"));
    assert(!rh_manager_data_prepare_apply(d,"active/8"));
    text=read_fixture("active/8/mappings.tsv");assert(strstr(text,"/resource/a/x.bin\tthemes/alpha/icons/x.bin"));free(text);
    assert(!rh_manager_data_complete_apply(d,0));fail_stat_path=0;
    /* Canonical installed metadata wins over a leftover legacy manifest. */
    fixture("themes/alpha/canora.json",beta);
    assert(!rh_manager_data_metadata(d,"alpha",&meta));assert(meta.available&&!strcmp(meta.name,"主题 A"));
    assert(!rh_manager_data_refresh(d));assert(!rh_manager_data_prepare_apply(d,"active/9"));assert(!rh_manager_data_complete_apply(d,0));
    /* A mapping error belongs to catalog/apply, not the metadata display. */
    fixture("themes/alpha/corona.json","{\"format\":\"canopus-resource-pack\",\"formatVersion\":1,\"themeId\":\"alpha\",\"name\":\"Readable metadata\",\"author\":\"Alice\",\"mappings\":false}");
    assert(!rh_manager_data_refresh(d));
    assert(!rh_manager_data_metadata(d,"alpha",&meta));assert(meta.available&&!strcmp(meta.name,"Readable metadata")&&!strcmp(meta.author,"Alice"));
    assert(rh_manager_data_mix_count(d,&count)==-22);assert(rh_manager_data_prepare_apply(d,"active/10")==-22);
    /* Legacy-only installs work; a malformed canonical file is not masked. */
    fixture("themes/alpha/canora.json",alpha);fixture("themes/alpha/corona.json","broken");
    assert(rh_manager_data_metadata(d,"alpha",&meta)==-22&&!meta.available);
    assert(!fs_remove(&ctx,"themes/alpha/corona.json"));assert(!rh_manager_data_refresh(d));
    assert(!rh_manager_data_metadata(d,"alpha",&meta));assert(meta.available&&!strcmp(meta.name,"主题 A"));
    assert(!rh_manager_data_prepare_apply(d,"active/11"));assert(!rh_manager_data_complete_apply(d,0));
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
}
static void test_overlay(void) {
    struct rh_manager_data *d=0; char name[160],*text; uint32_t i;
    setup();
    for(i=0;i<260;i++) { snprintf(name,sizeof(name),"themes/alpha/icons/file-%03u.bin",i); fixture(name,"OVERLAY"); }
    assert(!rh_manager_data_create(&ctx,0,&d));
    assert(!rh_manager_data_prepare_apply(d,"active/4"));
    text=read_fixture("active/4/mappings.tsv"); assert(!strcmp(text,"/resource/a/\tthemes/.active-4/r0/\n")); free(text);
    text=read_fixture("themes/.active-4/r0/x.bin"); assert(!strcmp(text,"A")); free(text);
    fixture("active-config","active/4\n"); assert(!rh_manager_data_complete_apply(d,1));
    assert(!rh_manager_data_move(d,2,0)); assert(!rh_manager_data_prepare_apply(d,"active/5")); fixture("active-config","active/5\n"); assert(!rh_manager_data_complete_apply(d,1));
    assert(fs_exists(&ctx,"themes/.active-4/r0/x.bin")==0); assert(fs_exists(&ctx,"themes/.active-4/r0")==1);
    rh_manager_data_destroy(d); assert(!blocks); cleanup_dir(sandbox);
}
static void test_migration(void) {
    struct rh_manager_data *d=0; char oldpath[1024],newpath[1024];
    setup(); assert(!fs_mkdir(&ctx,"legacy"));
    assert(!resolve(oldpath,"themes")); assert(!resolve(newpath,"legacy/themes")); assert(!rename(oldpath,newpath));
    const char *files[]={"interconnect-themes.json"};
    for(uint32_t i=0;i<1;i++) { assert(!resolve(oldpath,files[i])); snprintf(newpath,sizeof(newpath),"%s/legacy/%s",sandbox,files[i]); assert(!rename(oldpath,newpath)); }
    fixture("legacy/mappings.tsv","/resource/a/x.bin\tthemes/alpha/icons/x.bin\n");
    assert(!rh_manager_data_create(&ctx,0,&d)); assert(rh_manager_data_order_count(d)==3);
    assert(fs_exists(&ctx,"themes/alpha/icons/x.bin")==1); assert(fs_exists(&ctx,"legacy/themes/alpha/icons/x.bin")==1);
    assert(rh_manager_data_delete(d,"alpha")==RH_MANAGER_ERR_ACTIVE);
    rh_manager_data_destroy(d); assert(!blocks); cleanup_dir(sandbox);
}
static void setup_legacy_merge(void) {
    setup();assert(!fs_mkdir(&ctx,"legacy"));assert(!fs_mkdir(&ctx,"legacy/themes"));
    assert(!fs_mkdir(&ctx,"legacy/themes/alpha"));fixture("legacy/themes/alpha/corona.json","OLD METADATA");
    assert(!fs_mkdir(&ctx,"legacy/themes/alpha/icons"));fixture("legacy/themes/alpha/icons/x.bin","OLD A");
    assert(!fs_mkdir(&ctx,"legacy/themes/gamma"));
    fixture("legacy/themes/gamma/corona.json","{\"format\":\"canopus-resource-pack\",\"formatVersion\":1,\"themeId\":\"gamma\",\"name\":\"Imported\",\"mappings\":[{\"source\":\"/resource/g.bin\",\"destination\":\"g.bin\"}]}");
    fixture("legacy/themes/gamma/g.bin","G");fixture("legacy/interconnect-themes.json","[\"alpha\",\"gamma\"]");
    fixture("legacy/resource-order.json","{\"version\":1,\"order\":[\"gamma\",\"@system\",\"alpha\"]}");
    fixture("legacy/resource-overrides.json","{\"version\":1,\"overrides\":{\"/resource/g.bin\":\"gamma\"}}");
    fixture("legacy/mappings.tsv","OLD MAPPINGS");
}
static void test_migration_merge(void) {
    struct rh_manager_data *d=0;struct rh_manager_metadata meta;char id[65],*text;
    setup_legacy_merge();
    fixture("resource-order.json","{\"version\":1,\"order\":[\"beta\",\"@system\",\"alpha\"]}");
    fixture("resource-overrides.json","{\"version\":1,\"overrides\":{\"/resource/a/x.bin\":\"beta\"}}");
    fixture("mappings.tsv","/resource/a/x.bin\tthemes/beta/x.bin\n");
    assert(!rh_manager_data_create(&ctx,0,&d));assert(rh_manager_data_order_count(d)==4);
    assert(!rh_manager_data_metadata(d,"alpha",&meta)&&!strcmp(meta.name,"主题 A"));
    assert(!rh_manager_data_metadata(d,"gamma",&meta)&&!strcmp(meta.name,"Imported"));
    text=read_fixture("themes/alpha/icons/x.bin");assert(!strcmp(text,"A"));free(text);
    text=read_fixture("themes/gamma/g.bin");assert(!strcmp(text,"G"));free(text);
    text=read_fixture("interconnect-themes.json");assert(!strcmp(text,"[\"alpha\",\"beta\",\"gamma\"]"));free(text);
    text=read_fixture("resource-overrides.json");assert(!strcmp(text,"{\"version\":1,\"overrides\":{\"/resource/a/x.bin\":\"beta\"}}"));free(text);
    text=read_fixture("mappings.tsv");assert(!strcmp(text,"/resource/a/x.bin\tthemes/beta/x.bin\n"));free(text);
    assert(!rh_manager_data_order_get(d,1,id,0)&&!strcmp(id,"beta"));
    assert(!rh_manager_data_order_get(d,2,id,0)&&!strcmp(id,"@system"));
    assert(!rh_manager_data_order_get(d,3,id,0)&&!strcmp(id,"alpha"));
    assert(!rh_manager_data_delete(d,"gamma"));assert(!rh_manager_data_refresh(d));assert(rh_manager_data_order_count(d)==3);
    rh_manager_data_destroy(d);assert(!rh_manager_data_create(&ctx,0,&d));assert(rh_manager_data_order_count(d)==3);
    assert(fs_exists(&ctx,"legacy/themes/gamma/g.bin")==1&&fs_exists(&ctx,"themes/gamma/g.bin")==0);
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
}
static void test_migration_empty_native(void) {
    struct rh_manager_data *d=0;char *text;
    setup_legacy_merge();fixture("interconnect-themes.json","[]");
    assert(!rh_manager_data_create(&ctx,0,&d));assert(rh_manager_data_order_count(d)==3);
    text=read_fixture("interconnect-themes.json");assert(!strcmp(text,"[\"alpha\",\"gamma\"]"));free(text);
    assert(fs_exists(&ctx,"mappings.tsv")==0&&fs_exists(&ctx,"resource-overrides.json")==0);
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
}
static void test_migration_retry(void) {
    struct rh_manager_data *d=0;char *text;const char *failures[]={"copy","registry","marker"};
    for(uint32_t i=0;i<3;i++) {
        setup_legacy_merge();
        if(!strcmp(failures[i],"copy")) fail_write_path="themes/gamma/g.bin";
        else fail_rename_path=i==1?"interconnect-themes.json":"resource-legacy-imported";
        assert(rh_manager_data_create(&ctx,0,&d)==-EIO);assert(!d&&!blocks&&!live_bytes);
        assert(fs_exists(&ctx,"resource-legacy-imported")==0);
        text=read_fixture("interconnect-themes.json");
        assert(!strcmp(text,i==2?"[\"alpha\",\"beta\",\"gamma\"]":"[\"alpha\",\"beta\"]"));free(text);
        fail_write_path=fail_rename_path=0;
        assert(!rh_manager_data_create(&ctx,0,&d));assert(rh_manager_data_order_count(d)==4);
        text=read_fixture("themes/gamma/g.bin");assert(!strcmp(text,"G"));free(text);
        assert(fs_exists(&ctx,"resource-legacy-imported")==1);
        rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
    }
}
static void test_migration_fresh_retry(void) {
    struct rh_manager_data *d=0;char *text;
    setup_legacy_merge();assert(!fs_remove(&ctx,"interconnect-themes.json"));
    fixture("resource-order.json","{\"version\":1,\"order\":[\"@system\"]}");
    fail_write_path="resource-overrides.json.manager-tmp";
    assert(rh_manager_data_create(&ctx,0,&d)==-EIO);assert(!d&&!blocks&&!live_bytes);
    assert(fs_exists(&ctx,"resource-overrides.json")==0);
    assert(fs_exists(&ctx,"interconnect-themes.json")==0);
    fail_write_path=0;
    assert(!rh_manager_data_create(&ctx,0,&d));assert(rh_manager_data_order_count(d)==3);
    text=read_fixture("resource-overrides.json");assert(!strcmp(text,"{\"version\":1,\"overrides\":{\"/resource/g.bin\":\"gamma\"}}"));free(text);
    text=read_fixture("resource-order.json");assert(!strcmp(text,"{\"version\":1,\"order\":[\"gamma\",\"alpha\",\"@system\"]}"));free(text);
    assert(fs_exists(&ctx,"resource-legacy-imported")==1);
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
}
static void test_migration_invalid_registry(void) {
    struct rh_manager_data *d=0;char *text;
    const char *invalid[]={"broken","{}","[\"../alpha\"]","[\"alpha\",\"alpha\"]"};
    for(uint32_t i=0;i<4;i++) {
        setup_legacy_merge();fixture("legacy/interconnect-themes.json",invalid[i]);
        assert(rh_manager_data_create(&ctx,0,&d)==-22);assert(!d&&!blocks&&!live_bytes);
        text=read_fixture("interconnect-themes.json");assert(!strcmp(text,"[\"alpha\",\"beta\"]"));free(text);
        assert(fs_exists(&ctx,"themes/gamma/g.bin")==0&&fs_exists(&ctx,"resource-legacy-imported")==0);
        cleanup_dir(sandbox);
    }
}
static void test_256_rules(void) {
    struct rh_manager_data *d=0; char *manifest=malloc(40000),path[160],*text; size_t used; uint32_t i,count;
    assert(manifest);setup();
    used=(size_t)snprintf(manifest,40000,"{\"format\":\"canopus-resource-pack\",\"formatVersion\":1e0,\"themeId\":\"alpha\",\"name\":\"256 rules\",\"versionCode\":1.0,\"mappings\":[");
    for(i=0;i<256;i++) {
        used+=(size_t)snprintf(manifest+used,40000-used,"%s{\"source\":\"/resource/exact/%03u.bin\",\"destination\":\"%03u.bin\"}",i?",":"",i,i);
        snprintf(path,sizeof(path),"themes/alpha/%03u.bin",i);fixture(path,"BINARY");
    }
    assert(used+3u<40000u);strcpy(manifest+used,"]}");fixture("themes/alpha/corona.json",manifest);free(manifest);
    assert(!rh_manager_data_create(&ctx,0,&d));assert(!rh_manager_data_move(d,2,1));
    assert(!rh_manager_data_mix_count(d,&count)&&count==262);
    assert(!rh_manager_data_prepare_apply(d,"active/6"));text=read_fixture("active/6/mappings.tsv");assert(strstr(text,"/resource/exact/255.bin\tthemes/alpha/255.bin"));free(text);assert(!rh_manager_data_complete_apply(d,0));
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
}
static void test_pointer_and_config_gc(void) {
    static const char *const malformed[]={"active/1","active/foo\n","active/01\n","active/0\n","active/4294967296\n","active/1/nested\n","active/1/\n","active/1\n\n"};
    struct rh_manager_data *d=0;uint32_t i;setup();assert(!rh_manager_data_create(&ctx,0,&d));
    assert(rh_manager_data_prepare_apply(d,"active/foo")==-22);assert(rh_manager_data_prepare_apply(d,"active/0")==-22);assert(rh_manager_data_prepare_apply(d,"active/01")==-22);assert(rh_manager_data_prepare_apply(d,"active/4294967296")==-22);
    assert(!rh_manager_data_prepare_apply(d,"active/1"));fixture("active-config","active/1\n");assert(!rh_manager_data_complete_apply(d,1));
    for(i=0;i<sizeof(malformed)/sizeof(malformed[0]);i++) {fixture("active-config",malformed[i]);assert(rh_manager_data_delete(d,"beta")==-22);assert(fs_exists(&ctx,"themes/beta/x.bin")==1);}
    {int fd=fs_open(&ctx,"active-config",CANOPUS_FS_WRITE);const char raw[]="active/1\n\0junk";assert(fd>=0);assert(write(fd,raw,sizeof(raw)-1)==sizeof(raw)-1);close(fd);assert(rh_manager_data_delete(d,"beta")==-22);}
    fixture("active-config","active/1\n");assert(!rh_manager_data_prepare_apply(d,"active/2"));assert(rh_manager_data_complete_apply(d,1)==-16);assert(fs_exists(&ctx,"active/1/mappings.tsv")==1&&fs_exists(&ctx,"active/2/mappings.tsv")==1);
    assert(!fs_mkdir(&ctx,"active/diagnostic"));fixture("active/diagnostic/mappings.tsv","diagnostic");
    assert(!rh_manager_data_prepare_apply(d,"active/3"));fixture("active-config","active/3\n");fail_unlink=1;assert(rh_manager_data_complete_apply(d,1)==-EIO);fail_unlink=0;assert(fs_exists(&ctx,"active/1/mappings.tsv")==1&&fs_exists(&ctx,"active/3/mappings.tsv")==1);
    assert(!rh_manager_data_prepare_apply(d,"active/4"));fixture("active-config","active/4\n");assert(!rh_manager_data_complete_apply(d,1));
    for(i=1;i<4;i++){char file[80];snprintf(file,sizeof(file),"active/%u/mappings.tsv",i);assert(fs_exists(&ctx,file)==0);}
    assert(fs_exists(&ctx,"active/4/mappings.tsv")==1);assert(fs_exists(&ctx,"active/diagnostic/mappings.tsv")==1);
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
}
static void test_overlong_concrete_path(void) {
    struct rh_manager_data *d=0; char prefix_path[206],filename[61],asset[100],manifest[1024];uint32_t count;
    setup();prefix_path[0]='/';memset(prefix_path+1,'a',203);prefix_path[204]='/';prefix_path[205]=0;
    memset(filename,'b',56);strcpy(filename+56,".bin");snprintf(asset,sizeof(asset),"themes/alpha/icons/%s",filename);fixture(asset,"TOO LONG SOURCE");fixture("themes/alpha/icons/ok.bin","VALID");
    snprintf(manifest,sizeof(manifest),"{\"format\":\"canopus-resource-pack\",\"formatVersion\":1,\"themeId\":\"alpha\",\"name\":\"long prefix\",\"mappings\":[{\"source\":\"%s\",\"destination\":\"icons/\"}]}",prefix_path);fixture("themes/alpha/corona.json",manifest);
    assert(!rh_manager_data_create(&ctx,0,&d));assert(!rh_manager_data_mix_count(d,&count)&&count==9);
    assert(!rh_manager_data_prepare_apply(d,"active/7"));assert(!rh_manager_data_complete_apply(d,0));
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
}
static void test_failed_close_buffer_ownership(void) {
    struct rh_manager_data *d=0;uint32_t baseline,i;setup();assert(!rh_manager_data_create(&ctx,0,&d));fixture("active-config","active/1\n");baseline=live_bytes;
    fail_close=1;
    for(i=0;i<1024;i++) assert(rh_manager_data_delete(d,"beta")==-EIO);
    fail_close=0;
    assert(live_bytes==baseline);assert(fs_exists(&ctx,"themes/beta/x.bin")==1);
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
}
static const char *gamma="{\"format\":\"canopus-resource-pack\",\"formatVersion\":1,\"themeId\":\"gamma\",\"name\":\"New gamma\",\"versionCode\":0,\"mappings\":[{\"source\":\"/resource/g.bin\",\"destination\":\"g.bin\"}],\"backgrounds\":{\"launcher\":\"g.bin\"},\"calendar\":{\"weekday\":{\"type\":\"image\",\"context\":[\"g.bin\",\"g.bin\",\"g.bin\",\"g.bin\",\"g.bin\",\"g.bin\",\"g.bin\"]}}}";
static void make_staging(const char *name,const char *manifest) {
    char path[256]; int rc=fs_mkdir(&ctx,"staging"); assert(!rc||rc==-EEXIST);
    snprintf(path,sizeof(path),"staging/%s",name); assert(!fs_mkdir(&ctx,path));
    snprintf(path,sizeof(path),"staging/%s/corona.json",name); fixture(path,manifest);
    snprintf(path,sizeof(path),"staging/%s/g.bin",name); fixture(path,"G");
}
static uint32_t list_read32(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void test_connect_install_and_list(void) {
    struct rh_manager_data *d=0; struct rh_manager_metadata meta; struct rh_manager_option option;
    uint8_t hash[32]={1},other[32]={2},payload[468]; uint32_t revision,size,n; char id[65],*text;
    setup(); assert(!rh_manager_data_create(&ctx,0,&d)); revision=rh_manager_data_revision(d); assert(revision==1u);
    assert(!rh_manager_data_find_hash(d,hash,id)); make_staging("7",gamma);
    assert(!rh_manager_data_install(d,"staging/7",7u,hash,id)&&!strcmp(id,"gamma"));
    assert(!rh_manager_data_last_installed(d,id)&&!strcmp(id,"gamma"));
    assert(rh_manager_data_revision(d)==revision+1u); assert(!rh_manager_data_metadata(d,"gamma",&meta)&&!strcmp(meta.name,"New gamma"));
    assert(fs_exists(&ctx,"staging/7")==0 && fs_exists(&ctx,"themes/gamma/7/g.bin")==1);
    assert(fs_exists(&ctx,"active-config")==0); assert(rh_manager_data_find_hash(d,hash,id)==1&&!strcmp(id,"gamma"));
    assert(!rh_manager_data_find_hash(d,other,id)&&!id[0]);
    make_staging("dedup",gamma); assert(!rh_manager_data_install(d,"staging/dedup",8u,hash,id));
    assert(rh_manager_data_revision(d)==revision+1u&&fs_exists(&ctx,"staging/dedup/g.bin")==1);
    assert(!rh_manager_data_order_get(d,0,id,0)&&!strcmp(id,"gamma"));
    assert(!rh_manager_data_option_count(d,"/resource/g.bin",&n)&&n==3u);
    assert(!rh_manager_data_option_get(d,"/resource/g.bin",0,&option)); assert(strstr(option.preview,"themes/gamma/7/g.bin"));
    assert(!rh_manager_data_prepare_apply(d,"active/20"));
    text=read_fixture("active/20/mappings.tsv"); assert(strstr(text,"/resource/g.bin\tthemes/gamma/7/g.bin\n")); free(text);
    text=read_fixture("active/20/backgrounds.cfg");assert(strstr(text,"launcher=themes/gamma/7/g.bin\n"));free(text);
    text=read_fixture("active/20/calendar.cfg");assert(strstr(text,"\nthemes/gamma/7/g.bin\n")&&!strstr(text,"staging/"));free(text);
    assert(!rh_manager_data_complete_apply(d,0));
    size=sizeof(payload); assert(rh_manager_data_connect_list(d,revision,0,payload,&size)==-10&&size==0);
    revision=rh_manager_data_revision(d);
    size=sizeof(payload); assert(!rh_manager_data_connect_list(d,0,0,payload,&size));
    assert(list_read32(payload)==revision && list_read32(payload+4)==3u && list_read32(payload+8)==1u);
    assert(payload[12]==7u && payload[13]==5u && payload[17]==0u && !memcmp(payload+26,"alpha",5));
    assert(list_read32(payload+18)==UINT32_MAX && list_read32(payload+22)==2097151u);
    size=sizeof(payload); assert(!rh_manager_data_connect_list(d,revision,1,payload,&size));
    assert(!payload[12] && !payload[15] && !payload[16] && !memcmp(payload+26,"beta",4));
    size=sizeof(payload); assert(!rh_manager_data_connect_list(d,revision,2,payload,&size));
    assert(list_read32(payload+8)==UINT32_MAX && payload[12]==2u && !list_read32(payload+18)&&!list_read32(payload+22));
    assert(!memcmp(payload+26,"gamma",5));
    size=12u; assert(rh_manager_data_connect_list(d,revision,2,payload,&size)==-8&&size==0);
    size=sizeof(payload); assert(rh_manager_data_connect_list(d,revision,3,payload,&size)==-1);
    fixture("themes/gamma/7/corona.json","broken");size=sizeof(payload);assert(!rh_manager_data_connect_list(d,revision,2,payload,&size));
    assert(payload[17]==1u && !payload[12] && payload[14]==5u && !memcmp(payload+31,"gamma",5));
    fixture("themes/gamma/7/corona.json",gamma);
    rh_manager_data_destroy(d); assert(!blocks); assert(!rh_manager_data_create(&ctx,0,&d));
    assert(rh_manager_data_revision(d)==revision && rh_manager_data_find_hash(d,hash,id)==1);
    assert(rh_manager_data_delete(d,"gamma")==RH_MANAGER_ERR_ACTIVE);
    assert(!rh_manager_data_move(d,3u,0u)); assert(!rh_manager_data_prepare_apply(d,"active/22")); fixture("active-config","active/22\n"); assert(!rh_manager_data_complete_apply(d,1));
    assert(!rh_manager_data_delete(d,"gamma")); assert(rh_manager_data_revision(d)==revision+1u);
    assert(fs_exists(&ctx,"themes/gamma/7")==1&&fs_exists(&ctx,"themes/gamma/7/g.bin")==0);
    assert(!rh_manager_data_find_hash(d,hash,id));
    fixture("interconnect-themes.json","broken"); size=sizeof(payload); assert(rh_manager_data_connect_list(d,0,0,payload,&size)==-5);
    assert(rh_manager_data_find_hash(d,hash,id)==-5);
    rh_manager_data_destroy(d); assert(!blocks&&!live_bytes); cleanup_dir(sandbox);
}
static void test_connect_install_recovery(void) {
    for(uint32_t i=0;i<4u;i++) {
        struct rh_manager_data *d=0; uint8_t hash[32]={3}; char id[65]; uint32_t revision;
        setup(); assert(!rh_manager_data_create(&ctx,0,&d)); make_staging("7",gamma);
        if(i==0) fail_write_path="registry-install.pending.manager-tmp";
        else if(i==1) fail_rename_path="themes/gamma/7";
        else if(i==2) fail_rename_path="interconnect-themes.json";
        else fail_unlink=1;
        assert(rh_manager_data_install(d,"staging/7",7u,hash,id)==(i==3u ? 0:-EIO));
        assert(fs_exists(&ctx,"registry-install.pending")== (i!=0));
        assert(fs_exists(&ctx,"staging/7")== (i<2u));
        fail_write_path=fail_rename_path=0;fail_unlink=0;
        rh_manager_data_destroy(d); assert(!blocks&&!live_bytes);
        assert(!rh_manager_data_create(&ctx,0,&d));
        if(i==0) { assert(!rh_manager_data_find_hash(d,hash,id)); assert(!rh_manager_data_install(d,"staging/7",7u,hash,id)); }
        assert(rh_manager_data_find_hash(d,hash,id)==1&&!strcmp(id,"gamma"));
        assert(fs_exists(&ctx,"registry-install.pending")==0);
        revision=rh_manager_data_revision(d);assert(revision==2u);
        assert(!rh_manager_data_refresh(d)&&rh_manager_data_revision(d)==revision);
        assert(!rh_manager_data_install(d,"staging/7",7u,hash,id)&&rh_manager_data_revision(d)==revision);
        rh_manager_data_destroy(d); assert(!blocks&&!live_bytes); cleanup_dir(sandbox);
    }
}
static void test_connect_install_update_and_validation(void) {
    struct rh_manager_data *d=0; uint8_t hash[32]={4},second[32]={5}; char id[65],*text; uint32_t revision;
    setup(); assert(!rh_manager_data_create(&ctx,0,&d));
    make_staging("beta",beta); fixture("staging/beta/x.bin","NEW B");
    fixture("mappings.tsv","/resource/a/x.bin\tthemes/beta/x.bin\n");
    assert(rh_manager_data_install(d,"staging/beta",12u,hash,id)==RH_MANAGER_ERR_ACTIVE);
    assert(fs_exists(&ctx,"themes/beta/12")==0&&fs_exists(&ctx,"staging/beta/x.bin")==1);
    assert(!fs_remove(&ctx,"mappings.tsv"));
    fixture("resource-active-generations.json","{\"version\":1,\"generations\":[],\"protectedThemes\":[\"beta\"]}");
    assert(rh_manager_data_install(d,"staging/beta",12u,hash,id)==RH_MANAGER_ERR_ACTIVE);
    assert(!fs_remove(&ctx,"resource-active-generations.json"));
    assert(!rh_manager_data_install(d,"staging/beta",12u,hash,id));
    assert(!rh_manager_data_choose(d,"/resource/a/x.bin","beta"));
    assert(!rh_manager_data_prepare_apply(d,"active/21")); text=read_fixture("active/21/mappings.tsv"); assert(strstr(text,"themes/beta/12/x.bin"));free(text);assert(!rh_manager_data_complete_apply(d,0));
    assert(!rh_manager_data_choose(d,"/resource/a/x.bin",RH_MANAGER_SYSTEM));assert(!rh_manager_data_move(d,2u,0u));assert(!rh_manager_data_prepare_apply(d,"active/23"));fixture("active-config","active/23\n");assert(!rh_manager_data_complete_apply(d,1));
    make_staging("collision",beta);fixture("staging/collision/x.bin","B2");
    assert(rh_manager_data_install(d,"staging/collision",12u,second,id)==-EEXIST);assert(fs_exists(&ctx,"themes/beta/12/x.bin")==1);
    make_staging("bad",gamma); revision=rh_manager_data_revision(d);
    fixture("staging/bad/mappings.tsv","derived");assert(rh_manager_data_install(d,"staging/bad",13u,second,id)==-22);assert(!fs_remove(&ctx,"staging/bad/mappings.tsv"));
    fixture("staging/bad/canora.json",gamma);assert(rh_manager_data_install(d,"staging/bad",13u,second,id)==-22);assert(!fs_remove(&ctx,"staging/bad/canora.json"));
    assert(!fs_remove(&ctx,"staging/bad/g.bin"));assert(rh_manager_data_install(d,"staging/bad",13u,second,id)==-22);fixture("staging/bad/g.bin","G");
    fixture("staging/bad/corona.json","{\"format\":\"canopus-resource-pack\",\"formatVersion\":1,\"themeId\":\"gamma\",\"name\":\"bad\",\"mappings\":[{\"source\":\"/resource/g.bin\",\"destination\":\"../g.bin\"}]}");
    assert(rh_manager_data_install(d,"staging/bad",13u,second,id)==-22);fixture("staging/bad/corona.json",gamma);
    assert(rh_manager_data_install(d,"themes/beta",13u,second,id)==-22);assert(rh_manager_data_install(d,"staging/../bad",13u,second,id)==-22);
    assert(rh_manager_data_install(d,"staging/bad",0u,second,id)==-22);assert(rh_manager_data_revision(d)==revision);
    fail_close=1;assert(rh_manager_data_install(d,"staging/bad",13u,second,id)==-EIO);fail_close=0;
    assert(rh_manager_data_revision(d)==revision&&fs_exists(&ctx,"staging/bad/g.bin")==1);
    assert(!fs_remove(&ctx,"staging/bad/corona.json"));fixture("staging/bad/canora.json",gamma);
    assert(!rh_manager_data_install(d,"staging/bad",UINT32_MAX,second,id));assert(fs_exists(&ctx,"themes/gamma/4294967295/g.bin")==1);
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
}
static void test_connect_cleanup_and_sync(void) {
    struct rh_manager_data *d=0;uint8_t hash[32]={8};char id[65];
    setup();assert(!rh_manager_data_create(&ctx,0,&d));make_staging("sync",gamma);
    fail_sync_path="staging/sync/g.bin";assert(rh_manager_data_install(d,"staging/sync",7u,hash,id)==-EIO);fail_sync_path=0;
    assert(fs_exists(&ctx,"registry-install.pending")==0&&fs_exists(&ctx,"themes/gamma/7")==0);
    fail_sync_path="registry-install.pending.manager-tmp";assert(rh_manager_data_install(d,"staging/sync",7u,hash,id)==-EIO);fail_sync_path=0;
    assert(fs_exists(&ctx,"registry-install.pending")==0&&fs_exists(&ctx,"staging/sync/g.bin")==1);
    fail_remove_path="registry-install.pending";assert(!rh_manager_data_install(d,"staging/sync",7u,hash,id));
    assert(rh_manager_data_revision(d)==2u&&fs_exists(&ctx,"registry-install.pending")==1);
    assert(!rh_manager_data_delete(d,"gamma"));assert(rh_manager_data_revision(d)==3u);
    assert(fs_exists(&ctx,"registry-install.pending")==1);
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);fail_remove_path=0;
    assert(!rh_manager_data_create(&ctx,0,&d));assert(!rh_manager_data_find_hash(d,hash,id));
    assert(rh_manager_data_revision(d)==3u&&fs_exists(&ctx,"registry-install.pending")==0);
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
    {struct canopus_fs_obj no_sync=fs;struct canopus_context_v1 unsupported=ctx;no_sync.sync=0;unsupported.fs=&no_sync;
        setup();make_staging("sync",gamma);assert(!rh_manager_data_create(&unsupported,0,&d));
        assert(rh_manager_data_install(d,"staging/sync",7u,hash,id)==CANOPUS_SVC_ERR_UNSUPPORTED);
        assert(fs_exists(&ctx,"registry-install.pending")==0&&fs_exists(&ctx,"staging/sync/g.bin")==1);
        rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);}
}
static void test_connect_empty_list(void) {
    struct rh_manager_data *d=0;uint8_t payload[468];uint32_t size=sizeof(payload);
    setup();fixture("interconnect-themes.json","[]");assert(!rh_manager_data_create(&ctx,0,&d));
    assert(!rh_manager_data_connect_list(d,0,0,payload,&size)&&size==12u&&!list_read32(payload+4)&&list_read32(payload+8)==UINT32_MAX);
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
}
static int install_budget_step(struct rh_manager_data *d,char id[65]) {
    uint32_t reads=directory_reads,syncs=file_syncs;int rc=rh_manager_data_install_step(d,id);
    assert(directory_reads-reads<=1u);assert(file_syncs-syncs<=1u);
    assert((directory_reads-reads)+(file_syncs-syncs)<=1u);
    if(!rc)assert(!id[0]);return rc;
}
static void test_incremental_install(void) {
    struct rh_manager_data *d=0;uint8_t hash[32]={11},payload[468];char id[65],path[160];uint32_t i,size,steps=0;
    setup();assert(!rh_manager_data_create(&ctx,0,&d));make_staging("incremental",gamma);
    assert(!fs_mkdir(&ctx,"staging/incremental/nested"));assert(!fs_mkdir(&ctx,"staging/incremental/nested/deep"));
    for(i=0;i<80u;i++){snprintf(path,sizeof(path),"staging/incremental/nested/deep/%u.bin",i);fixture(path,"PAYLOAD");}
    {uint32_t reads=directory_reads;assert(!rh_manager_data_install_begin(d,"staging/incremental",14u,hash));assert(directory_reads==reads);}
    assert(rh_manager_data_install_begin(d,"staging/incremental",14u,hash)==-16);
    assert(rh_manager_data_refresh(d)==-16);assert(rh_manager_data_move(d,0,1)==-16);
    assert(rh_manager_data_find_hash(d,hash,id)==-2);size=sizeof(payload);assert(rh_manager_data_connect_list(d,0,0,payload,&size)==-2);
    {int rc;do{rc=install_budget_step(d,id);assert(++steps<500u);}while(!rc);assert(rc==1&&!strcmp(id,"gamma"));}
    assert(steps>160u&&!open_directories&&fs_exists(&ctx,"themes/gamma/14/nested/deep/79.bin")==1);
    assert(rh_manager_data_install_step(d,id)==-22);assert(rh_manager_data_find_hash(d,hash,id)==1);
    assert(!rh_manager_data_install_begin(d,"staging/incremental",14u,hash));assert(install_budget_step(d,id)==1&&!strcmp(id,"gamma"));
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes&&!open_directories);cleanup_dir(sandbox);
}
static void test_incremental_abort_and_fault(void) {
    struct rh_manager_data *d=0;uint8_t hash[32]={12};char id[65];int rc;uint32_t i;
    setup();assert(!rh_manager_data_create(&ctx,0,&d));make_staging("abort",gamma);
    assert(!rh_manager_data_install_begin(d,"staging/abort",15u,hash));assert(!install_budget_step(d,id));assert(open_directories==1u);
    rh_manager_data_install_abort(d);assert(!open_directories&&!fs_exists(&ctx,"registry-install.pending"));assert(!rh_manager_data_refresh(d));
    assert(!rh_manager_data_install_begin(d,"staging/abort",15u,hash));fail_sync_path="staging/abort/g.bin";
    for(i=0;i<30u;i++){rc=install_budget_step(d,id);if(rc)break;}assert(rc==-EIO);fail_sync_path=0;
    assert(!open_directories&&!fs_exists(&ctx,"registry-install.pending"));assert(!rh_manager_data_refresh(d));
    assert(!rh_manager_data_install_begin(d,"staging/abort",15u,hash));assert(!install_budget_step(d,id));
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes&&!open_directories);
    assert(!rh_manager_data_create(&ctx,0,&d));assert(!rh_manager_data_install_begin(d,"staging/abort",15u,hash));
    for(i=0;i<30u;i++){assert(!install_budget_step(d,id));if(fs_exists(&ctx,"registry-install.pending"))break;}assert(i<30u);
    rh_manager_data_install_abort(d);assert(!open_directories);
    {uint32_t reads=directory_reads;fail_sync_path="staging/abort/g.bin";assert(!rh_manager_data_refresh(d));fail_sync_path=0;assert(directory_reads==reads);}
    assert(rh_manager_data_find_hash(d,hash,id)==1&&!strcmp(id,"gamma"));
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes&&!open_directories);cleanup_dir(sandbox);
}
static void test_incremental_recovery_budget(void) {
    for(uint32_t phase=0;phase<2u;phase++) {
        struct rh_manager_data *d=0;uint8_t hash[32]={13};char id[65];uint32_t i,reads;
        setup();assert(!rh_manager_data_create(&ctx,0,&d));make_staging("recovery",gamma);
        assert(!rh_manager_data_install_begin(d,"staging/recovery",16u,hash));
        for(i=0;i<30u;i++){assert(!install_budget_step(d,id));if(fs_exists(&ctx,"registry-install.pending"))break;}assert(i<30u);
        if(phase){fail_rename_path="interconnect-themes.json";assert(install_budget_step(d,id)==-EIO);fail_rename_path=0;assert(fs_exists(&ctx,"themes/gamma/16/g.bin"));}
        rh_manager_data_destroy(d);assert(!blocks&&!open_directories);
        reads=directory_reads;
        fail_sync_path=phase ? "themes/gamma/16/g.bin":"staging/recovery/g.bin";
        assert(!rh_manager_data_create(&ctx,0,&d));fail_sync_path=0;assert(directory_reads==reads);
        assert(rh_manager_data_find_hash(d,hash,id)==1&&!strcmp(id,"gamma"));
        assert(!fs_exists(&ctx,"registry-install.pending"));rh_manager_data_destroy(d);assert(!blocks&&!live_bytes&&!open_directories);cleanup_dir(sandbox);
    }
}
static void test_install_cancel_and_terminal_commit(void) {
    for(unsigned boundary=0;boundary<2;boundary++) {
        struct rh_manager_data *d=0;uint8_t hash[32]={21};char id[65];
        setup();assert(!rh_manager_data_create(&ctx,0,&d));make_staging("cancel",gamma);
        fail_rename_path=boundary ? "interconnect-themes.json":"themes/gamma/21";
        assert(rh_manager_data_install(d,"staging/cancel",21,hash,id)==-EIO);
        assert(fs_exists(&ctx,"registry-install.pending")==1);
        fail_remove_path="registry-install.pending";
        assert(rh_manager_data_install_cancel(d,hash)==-EIO);
        fail_remove_path=0;
        assert(!rh_manager_data_install_cancel(d,hash));
        assert(!fs_exists(&ctx,"registry-install.pending"));
        fail_rename_path=0;assert(!rh_manager_data_refresh(d));
        assert(!rh_manager_data_find_hash(d,hash,id));
        rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);
        assert(!rh_manager_data_create(&ctx,0,&d));assert(!rh_manager_data_find_hash(d,hash,id));
        rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
    }
    {
        struct rh_manager_data *d=0;uint8_t hash[32]={22};char id[65];int rc;unsigned steps=0;
        setup();assert(!rh_manager_data_create(&ctx,0,&d));make_staging("commit",gamma);
        assert(!rh_manager_data_install_begin(d,"staging/commit",22,hash));
        do {
            rc=install_budget_step(d,id);assert(++steps<100);
            if(fs_exists(&ctx,"themes/gamma/22/g.bin") && !fs_exists(&ctx,"registry-install.pending"))
                fail_write_path="resource-order.json.manager-tmp";
        } while(!rc);
        assert(rc==1&&!strcmp(id,"gamma"));
        assert(rh_manager_data_install_cancel(d,hash)==1);
        fail_write_path=0;assert(rh_manager_data_find_hash(d,hash,id)==1);
        rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
    }
}
static void test_damaged_hash_reinstall(void) {
    struct rh_manager_data *d=0;uint8_t hash[32]={23};char id[65];
    setup();assert(!rh_manager_data_create(&ctx,0,&d));make_staging("original",gamma);
    assert(!rh_manager_data_install(d,"staging/original",23,hash,id));
    assert(!fs_remove(&ctx,"themes/gamma/23/g.bin"));
    assert(!rh_manager_data_find_hash(d,hash,id));
    make_staging("repair",gamma);assert(!rh_manager_data_install(d,"staging/repair",24,hash,id));
    assert(rh_manager_data_find_hash(d,hash,id)==1);
    assert(fs_exists(&ctx,"themes/gamma/24/g.bin"));
    fixture("themes/gamma/24/corona.json","broken");assert(!rh_manager_data_find_hash(d,hash,id));
    make_staging("repair-manifest",gamma);assert(!rh_manager_data_install(d,"staging/repair-manifest",25,hash,id));
    assert(rh_manager_data_find_hash(d,hash,id)==1);
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes);cleanup_dir(sandbox);
}
static void test_versioned_directory_cleanup(void) {
    for(unsigned scenario=0;scenario<5;scenario++) {
        struct canopus_services_v1 svc={0};struct canopus_context_v1 versioned=ctx;
        struct canopus_fs_obj current=fs;struct rh_manager_data *d=0;
        void *legacy=0;uint32_t calls=rmdir_calls;
        svc.group_version[CANOPUS_SVC_FS]=scenario==0 ? 1:2;versioned.services=&svc;
        if(scenario==0) {
            /* An actual v1-sized object: touching appended rmdir is an ASan error. */
            legacy=malloc(offsetof(struct canopus_fs_obj,rmdir));assert(legacy);
            memcpy(legacy,&fs,offsetof(struct canopus_fs_obj,rmdir));versioned.fs=legacy;
        } else {if(scenario==2)current.rmdir=0;versioned.fs=&current;}
        setup();assert(!fs_mkdir(&ctx,"themes/beta/nested"));fixture("themes/beta/nested/file.bin","B");
        assert(!fs_mkdir(&ctx,"themes/beta/9"));fixture("themes/beta/9/old.bin","OLD");
        assert(!rh_manager_data_create(&versioned,0,&d));
        if(scenario>=3) {
            fail_rmdir_path="themes/beta/nested";
            rmdir_error=scenario==3 ? -EIO:CANOPUS_SVC_ERR_NOTEMPTY;
            assert(rh_manager_data_delete(d,"beta")==rmdir_error);
            assert(rh_manager_data_order_count(d)==3);
            fail_rmdir_path=0;
        }
        assert(!rh_manager_data_delete(d,"beta"));
        assert(!fs_exists(&ctx,"themes/beta/9/old.bin"));
        if(scenario==0 || scenario==2) {
            assert(rmdir_calls==calls && fs_exists(&ctx,"themes/beta")==1);
        } else assert(rmdir_calls>calls && !fs_exists(&ctx,"themes/beta"));
        rh_manager_data_destroy(d);free(legacy);assert(!blocks&&!live_bytes&&!open_directories);cleanup_dir(sandbox);
    }
}
static void test_v2_config_and_overlay_cleanup(void) {
    struct canopus_services_v1 svc={0};struct canopus_context_v1 versioned=ctx;
    struct rh_manager_data *d=0;char path[160];
    svc.group_version[CANOPUS_SVC_FS]=2;versioned.services=&svc;
    setup();
    for(unsigned i=0;i<260;i++){snprintf(path,sizeof(path),"themes/alpha/icons/v2-%03u.bin",i);fixture(path,"V2");}
    assert(!rh_manager_data_create(&versioned,0,&d));
    assert(!rh_manager_data_prepare_apply(d,"active/30"));fixture("active-config","active/30\n");assert(!rh_manager_data_complete_apply(d,1));
    assert(fs_exists(&ctx,"themes/.active-30")==1);
    assert(!fs_mkdir(&ctx,"active/28"));fixture("active/28/unknown.bin","KEEP");
    assert(!fs_mkdir(&ctx,"active/29"));fixture("active/29/mappings.tsv","OLD");
    assert(!rh_manager_data_move(d,2,0));
    assert(!rh_manager_data_prepare_apply(d,"active/31"));fixture("active-config","active/31\n");assert(!rh_manager_data_complete_apply(d,1));
    assert(!fs_exists(&ctx,"active/29")&&!fs_exists(&ctx,"active/30"));
    assert(fs_exists(&ctx,"active/31/mappings.tsv")==1);
    assert(fs_exists(&ctx,"active/28/unknown.bin")==1);
    assert(!fs_exists(&ctx,"themes/.active-30"));
    assert(fs_exists(&ctx,"themes/alpha/icons/x.bin")==1);
    rh_manager_data_destroy(d);assert(!blocks&&!live_bytes&&!open_directories);cleanup_dir(sandbox);
}
int main(void) {test_versioned_directory_cleanup();test_v2_config_and_overlay_cleanup();test_install_cancel_and_terminal_commit();test_damaged_hash_reinstall();test_incremental_install();test_incremental_abort_and_fault();test_incremental_recovery_budget();test_connect_cleanup_and_sync();test_connect_install_and_list();test_connect_install_recovery();test_connect_install_update_and_validation();test_connect_empty_list();test_core();test_apply_overlap();test_faults();test_installed_metadata();test_overlay();test_migration();test_migration_merge();test_migration_empty_native();test_migration_retry();test_migration_fresh_retry();test_migration_invalid_registry();test_256_rules();test_pointer_and_config_gc();test_overlong_concrete_path();test_failed_close_buffer_ownership();printf("native manager data: all checks passed (service cap128 blocks/256KiB; peak %u blocks, %u bytes)\n",peak_blocks,peak_bytes);return 0;}

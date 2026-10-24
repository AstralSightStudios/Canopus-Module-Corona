#include "corona_runtime.h"
#include "corona_connect.h"
#include "corona_archive.h"
#include "manager_data.h"
#include "manager_state.h"
#include "manager_ui.h"

/* The receiver, installer and pages all run on the same owner and share one
 * catalog. Closing a page never suspends or destroys an upload. */
static struct {
    const struct canopus_context_v1 *ctx;
    struct rh_connect *receiver;
    struct rh_archive *archive;
    struct rh_manager_data *catalog;
    int32_t timer;
    uint32_t generation, attempt, archive_bytes;
    int extracted, preparing, registering;
    char staging[64], archive_path[256];
    /* One directory iterator and scalar traversal state; FS v2 removes
     * drained directories in postorder, while FS v1 retains their shells. */
    void *cleanup_dir;
    char cleanup_path[256], cleanup_after[64], cleanup_best[64];
    uint8_t cleanup_active, cleanup_best_dir, cleanup_validate, cleanup_remove;
} R = { .timer = -1 };

static int catalog(void) {
    return R.catalog ? 0 : rh_manager_ui_get_data(R.ctx, &R.catalog);
}
static int protocol_error(int rc) {
    if (rc == RH_MANAGER_ERR_ACTIVE) return -9;
    if (rc == CANOPUS_SVC_ERR_BUSY) return -2;
    if (rc == CANOPUS_SVC_ERR_UNSUPPORTED) return -11;
    if (rc == RH_MANAGER_ERR_LIMIT || rc == -12 || rc == CANOPUS_SVC_ERR_NOSPACE) return -8;
    if (rc == RH_MANAGER_ERR_INVALID) return -7;
    return -5;
}
static uint32_t length(const char *s) { uint32_t n=0; while(s[n]) ++n; return n; }
static void copy_text(char *to,const char *from) { do { *to++=*from; } while(*from++); }
static int compare(const char *a,const char *b) { while(*a && *a==*b) {++a;++b;} return (unsigned char)*a-(unsigned char)*b; }
static void cleanup_wake(void) {
    /* Catalog creation replays its durable install journal before any staging
     * file is eligible for garbage collection. */
    if(!R.catalog) return;
    R.cleanup_validate=1;
    /* A just-released staging task may sort before our current cursor. Restart
     * from the root so a directory skipped while protected is not stranded. */
    if(R.cleanup_dir) (void)R.ctx->fs->closedir(R.ctx,R.cleanup_dir);
    R.cleanup_dir=0;
    copy_text(R.cleanup_path,"staging");
    R.cleanup_remove=0;
    R.cleanup_after[0]=R.cleanup_best[0]=0;
    R.cleanup_active=1;
}
static void cleanup_step(void) {
    struct canopus_fs_dirent entry;
    char child[256];
    uint32_t n,i;
    int rc,closed;
    if(!R.cleanup_active) return;
    if(R.cleanup_validate) {
        R.cleanup_validate=0;
        if(rh_manager_data_refresh(R.catalog)) {
            if(R.cleanup_dir) (void)R.ctx->fs->closedir(R.ctx,R.cleanup_dir);
            R.cleanup_dir=0;R.cleanup_active=0;return;
        }
    }
    if(R.cleanup_remove) {
        /* Do not read the appended FS member on an older Supervisor. Keep
         * staging itself: begin_install creates children below this root. */
        if(canopus_services_has(R.ctx,CANOPUS_SVC_FS,2) && R.ctx->fs->rmdir) {
            rc=R.ctx->fs->rmdir(R.ctx,R.cleanup_path);
            if(rc<0 && rc!=CANOPUS_SVC_ERR_NOTFOUND) {R.cleanup_active=0;return;}
        }
        R.cleanup_remove=0;
        n=length(R.cleanup_path);while(n && R.cleanup_path[n-1u]!='/') --n;
        if(!n) {R.cleanup_active=0;return;}
        copy_text(R.cleanup_after,R.cleanup_path+n);R.cleanup_path[n-1u]=0;
        return;
    }
    if(!R.cleanup_dir) {
        R.cleanup_dir=R.ctx->fs->opendir(R.ctx,R.cleanup_path);
        if(!R.cleanup_dir) {R.cleanup_active=0;return;}
        R.cleanup_best[0]=0;
    }
    rc=R.ctx->fs->readdir(R.ctx,R.cleanup_dir,&entry);
    if(!rc) {
        if(!entry.name[0] || !compare(entry.name,".") || !compare(entry.name,"..")) return;
        n=length(R.cleanup_path);i=length(entry.name);
        if(n+1u+i>=sizeof(child)) return;
        copy_text(child,R.cleanup_path);child[n++]='/';copy_text(child+n,entry.name);
        if(R.staging[0] && !compare(child,R.staging)) return;
        if(compare(entry.name,R.cleanup_after)>0 && (!R.cleanup_best[0] || compare(entry.name,R.cleanup_best)<0)) {
            copy_text(R.cleanup_best,entry.name);R.cleanup_best_dir=entry.is_dir;
        }
        return;
    }
    closed=R.ctx->fs->closedir(R.ctx,R.cleanup_dir);R.cleanup_dir=0;
    if(rc<0 || closed<0) {R.cleanup_active=0;return;}
    if(R.cleanup_best[0]) {
        n=length(R.cleanup_path);copy_text(child,R.cleanup_path);child[n++]='/';copy_text(child+n,R.cleanup_best);
        if(R.cleanup_best_dir) {copy_text(R.cleanup_path,child);R.cleanup_after[0]=0;}
        else {
            if(R.ctx->fs->remove(R.ctx,child)<0) {R.cleanup_active=0;return;}
            copy_text(R.cleanup_after,R.cleanup_best);
        }
    } else if(!compare(R.cleanup_path,"staging")) R.cleanup_active=0;
    else R.cleanup_remove=1;
}
static void abort_install(void *cookie) {
    (void)cookie;
    if (R.archive) rh_archive_destroy(R.archive);
    if(R.catalog)rh_manager_data_install_abort(R.catalog);
    R.archive = 0;
    R.extracted = R.preparing = R.registering = 0;
    R.staging[0]=0;
    if(R.ctx) cleanup_wake();
}
static int begin_install(void *cookie, const char *path, uint32_t bytes,
                         uint32_t generation) {
    int rc;
    (void)cookie;
    if(R.extracted && R.generation==generation && R.archive_bytes==bytes) return 0;
    abort_install(0);
    if (!generation || length(path)>=sizeof(R.archive_path)) return -7;
    R.generation=generation;R.archive_bytes=bytes;R.attempt=0;R.preparing=1;
    copy_text(R.archive_path,path);
    rc = catalog();
    if (rc) return protocol_error(rc);
    rc=R.ctx->fs->mkdir(R.ctx,"staging");
    if(rc<0 && rc!=CANOPUS_SVC_ERR_EXIST) return -5;
    return 0;
}
static char *decimal(char *p,uint32_t value) {
    char reverse[10];uint32_t count=0;
    do {reverse[count++]=(char)('0'+value%10u);value/=10u;} while(value);
    while(count) *p++=reverse[--count];*p=0;return p;
}
static int install_step(void *cookie, const uint8_t hash[32]) {
    char id[65],*p;
    struct canopus_fs_stat st;
    int rc;
    (void)cookie;
    if(R.preparing) {
        copy_text(R.staging,"staging/");p=decimal(R.staging+8,R.generation);*p++='-';decimal(p,R.attempt);
        rc=R.ctx->fs->stat(R.ctx,R.staging,&st);
        if(!rc) {if(R.attempt==UINT32_MAX)return -8;++R.attempt;return 0;}
        if(rc!=CANOPUS_SVC_ERR_NOTFOUND) return -5;
        rc=rh_archive_create(R.ctx,R.archive_path,R.archive_bytes,R.staging,&R.archive);
        if(rc) return rc;
        R.preparing=0;return 0;
    }
    if (!R.extracted) {
        if (!R.archive) return -7;
        rc = rh_archive_step(R.archive);
        if (rc <= 0) return rc;
        rh_archive_destroy(R.archive);
        R.archive = 0;
        R.extracted = 1;
        return 0;
    }
    if(!R.registering) {
        rc=rh_manager_data_install_begin(R.catalog,R.staging,R.generation,hash);
        if(rc==CANOPUS_SVC_ERR_BUSY)return 0;
        if(rc)return protocol_error(rc);
        R.registering=1;return 0;
    }
    rc=rh_manager_data_install_step(R.catalog,id);
    if(rc<0) {R.registering=0;return protocol_error(rc);}
    if(!rc)return 0;
    R.extracted=R.registering=0;
    R.staging[0]=0;cleanup_wake();
    return 1;
}
static int cancel_install(void *cookie,const uint8_t hash[32]) {
    int rc;
    (void)cookie;
    rc=catalog();
    if(rc)return rc==CANOPUS_SVC_ERR_NOMEM ? -8:-5;
    rc=rh_manager_data_install_cancel(R.catalog,hash);
    if(rc>=0) {
        R.registering=0;
        /* An earlier install error already released the receiver's hook.
         * Successful revocation must still rearm the journal-gated cleanup. */
        if(!rc)abort_install(0);
        return rc;
    }
    return protocol_error(rc);
}
static int find_hash(void *cookie, const uint8_t hash[32]) {
    char id[65];
    int rc;
    (void)cookie;
    rc = catalog();
    return rc ? (rc==CANOPUS_SVC_ERR_NOMEM ? -8:-5) : rh_manager_data_find_hash(R.catalog, hash, id);
}
static int list(void *cookie, uint32_t revision, uint32_t index,
                uint8_t *payload, uint32_t *size) {
    int rc;
    (void)cookie;
    rc = catalog();
    return rc ? (rc==CANOPUS_SVC_ERR_NOMEM ? -8:-5) :
        rh_manager_data_connect_list(R.catalog, revision, index, payload, size);
}
static void changed(void *cookie, const struct rh_transfer_ui_state *state) {
    (void)cookie;
    (void)rh_manager_set_transfer_state(state);
}
static void tick(const struct canopus_context_v1 *ctx, int32_t timer, void *cookie) {
    (void)ctx; (void)timer; (void)cookie;
    cleanup_step();
    rh_connect_poll(R.receiver);
}
int rh_connect_runtime_start(const struct canopus_context_v1 *ctx) {
    struct rh_connect_hooks hooks = {0};
    int rc;
    if (R.receiver) return R.ctx == ctx ? 0 : CANOPUS_SVC_ERR_STATE;
    if (!ctx || !ctx->connect || !ctx->connect->listen || !ctx->connect->send ||
        !ctx->time || !ctx->time->timer_create || !ctx->time->timer_start ||
        !ctx->time->timer_destroy) return CANOPUS_SVC_ERR_UNSUPPORTED;
    R.ctx = ctx;
    hooks.install_begin = begin_install;
    hooks.install_step = install_step;
    hooks.install_abort = abort_install;
    hooks.install_cancel = cancel_install;
    hooks.find_hash = find_hash;
    hooks.list = list;
    hooks.changed = changed;
    rc = rh_connect_create(ctx, &hooks, &R.receiver);
    if (rc) return rc==-11 ? CANOPUS_SVC_ERR_UNSUPPORTED:rc;
    R.timer = ctx->time->timer_create(ctx, 10u, 1, tick, 0);
    if (R.timer < 0) { rc = R.timer; goto failed; }
    if (canopus_services_has(ctx, CANOPUS_SVC_TIME, 2) && ctx->time->timer_set_background) {
        rc = ctx->time->timer_set_background(ctx, R.timer, 1);
        if (rc) goto failed;
    }
    rc = ctx->time->timer_start(ctx, R.timer);
    if (!rc) rc = rh_connect_start(R.receiver);
    if (!rc) {
        /* A corrupt registry remains queryable as an error; never clean files
         * that its recovery record might still reference. */
        if(!catalog()) cleanup_wake();
        return 0;
    }
failed:
    rh_connect_runtime_stop();
    return rc==-11 ? CANOPUS_SVC_ERR_UNSUPPORTED:rc;
}
void rh_connect_runtime_stop(void) {
    if (R.timer >= 0) (void)R.ctx->time->timer_destroy(R.ctx, R.timer);
    R.timer = -1;
    if (R.receiver) {
        (void)rh_connect_pause(R.receiver);
        rh_connect_destroy(R.receiver);
    }
    R.receiver = 0;
    abort_install(0);
    if(R.cleanup_dir) (void)R.ctx->fs->closedir(R.ctx,R.cleanup_dir);
    R.cleanup_dir=0;R.cleanup_active=R.cleanup_remove=0;
    R.catalog=0;
}

#include "canopus_abi.h"
#include "canopus_module_registration.h"
#include "resource_hook_platform.h"

#define RH_MODULE_ID "resource_hook"
#define RH_VERSION "0.3.0"
#if defined(RH_TARGET_155) && RH_TARGET_155
#define RH_TARGET_ID "xiaomi-band-11-4.100.155"
#else
#define RH_TARGET_ID "xiaomi-band-11-4.100.139"
#endif
/* Bind offline receipt verification to the same selection as the descriptor
 * and platform adapter, rather than trusting a receipt label alone. */
#if defined(__arm__) && defined(__ELF__)
__attribute__((used, section(".rh.target")))
static const char rh_artifact_target[48] = RH_TARGET_ID;
#endif
static struct rh_state S;
static int configured;
static uint32_t images_dropped;
static uint32_t redraws;
/* Reserved RHQ1 v5 field: no forced page rebuilds in the reload path. */
static const uint32_t rebuilds;
static uint32_t fonts_retargeted;
static void *refresh_timer;
static unsigned refresh_pending, cache_dropped, images_done, fonts_done, refreshing;

/* Point every registered font family whose file falls under a mapping rule at
 * the themed file. A retargeted entry is re-added at the end of the registry, so
 * the same index is re-examined; a themed path normally no longer matches a
 * rule, which ends the walk. The bound stops a pathological rule set that maps a
 * theme directory back onto itself from looping. */
static void retarget_fonts(void) {
    char name[RH_PATH], path[RH_PATH], mapped[RH_PATH];
    uint32_t index = 0, guard = 0;
    while (guard++ < RH_RULES * 8u) {
        if (rh_platform_font_path_get(index, name, path)) return;
        if (rh_resolve(&S, path, mapped) == 1 &&
            rh_platform_font_retarget(index, mapped) == 0) {
            if (fonts_retargeted != UINT32_MAX) fonts_retargeted++;
            continue;
        }
        index++;
    }
}

/* One request, coalesced across activations. The temporary UI timer retries only
 * until the selected target's verified retirement/owner stages and dirty-area
 * request complete; a failed adapter never counts as a completed retirement. */
static void refresh_step(void *timer) {
    (void)timer;
    if (!refresh_pending || refreshing || !rh_platform_redraw_ready()) return;
    refreshing = 1;
    if (!cache_dropped) {
        int rc = rh_platform_retire_images(&S);
        if (rc >= 0) {
            cache_dropped = 1;
            if (!rc && images_dropped != UINT32_MAX) images_dropped++;
        }
    }
    if (cache_dropped && !images_done && rh_platform_refresh_images(&S) >= 0)
        images_done = 1;
    /* Registry retargeting affects future resolution, not live/idle wrappers.
     * Never force page teardown just to adopt a resource mapping. */
    if (cache_dropped && !fonts_done) {
        fonts_done = 1;
        retarget_fonts();
    }
    if (cache_dropped && images_done && rh_platform_request_full_redraw() == 0) {
        void *done = refresh_timer;
        if (redraws != UINT32_MAX) redraws++;
        refresh_pending = 0;
        refresh_timer = 0;
        if (done) rh_platform_refresh_timer_delete(done);
    }
    refreshing = 0;
}
static int request_refresh(void) {
    if (!refresh_pending) {
        cache_dropped = 0;
        fonts_done = 0;
        images_done = 0;
        refresh_pending = 1;
    }
    refresh_step(0);
    if (refresh_pending && !refresh_timer)
        refresh_timer = rh_platform_refresh_timer_create(refresh_step);
    /* The redirect remains resident on allocation failure; do not report a
     * completed activation. A subsequent activate can retry scheduling. */
    return refresh_pending && !refresh_timer ? -2011 : 0;
}

static int rh_wrapper(void *d, const char *p, int m) {
    return rh_posix_open(&S, d, p, m);
}
static int config_read(void *cookie, void *out, uint32_t size) {
    return rh_platform_read(*(int *)cookie, out, size);
}
static int32_t prepare(const struct canopus_context_v1 *c) {
    struct rh_rule *staging;
    char *text;
    int fd, rc;
    (void)c;
    if (S.installed) return -2004;
    fd = rh_platform_open("/data/canopus/themes/mappings.tsv", 1);
    if (fd < 0) {
        if (rh_platform_errno() != RH_ENOENT) return -2005;
        /* No published hook exists here. Discard any prepared snapshot and
         * leave configuration retryable on the next activation. */
        S.count = 0;
        configured = 0;
        return 0;
    }
    staging = rh_platform_alloc(sizeof(*staging) * RH_RULES + RH_CONFIG_BYTES);
    if (!staging) rc = -2006;
    else {
        text = (char *)(staging + RH_RULES);
        rc = rh_read_config(&S, config_read, &fd, text, RH_CONFIG_BYTES, staging);
        rh_platform_free(staging);
        if (rc) rc = -2007;
    }
    rh_platform_close(fd);
    if (!rc && !S.count) rc = -2008;
    if (!S.count) configured = 0;
    if (!rc) configured = 1;
    return rc;
}
/* Rebinding requires a caller-owned UI transaction. Installing this callback
 * does not restart miwear or tear down font wrappers already held by live
 * pages. Affected cached file sources and verified owners adopt the immutable
 * mapping through the platform adapter; other owners need their own lifecycle. */
static int32_t activate(const struct canopus_context_v1 *c) {
    rh_open_fn *slot;
    uint32_t irq;
    int rc = 0;
    /* Supervisor marks descriptors READY without calling prepare. */
    if (!configured) {
        rc = prepare(c);
        if (rc) return rc;
        /* Optional configuration absent: no callback, timer or UI changes. */
        if (!configured) return 0;
    }
    /* No allocation or firmware I/O is allowed while publishing the callback.
     * On the single-core target, prevent scheduling between state and slot. */
    irq = rh_platform_lock();
    slot = rh_platform_slot();
    if (!S.count || !rh_platform_driver_valid()) rc = -2008;
    else if (!slot || (*slot != rh_platform_original() && *slot != rh_wrapper)) rc = -2009;
    else if (rh_reinstall_posix(&S, rh_platform_driver(), slot,
                               rh_platform_original(), rh_wrapper)) rc = -2010;
    rh_platform_unlock(irq);
    if (!rc) rc = request_refresh();
    return rc;
}
static int32_t stop(const struct canopus_context_v1 *c) {
    (void)c;
    return S.installed ? CANOPUS_RESULT_REBOOT_REQUIRED : 0;
}
static int32_t query(struct canopus_status_writer_v1 *writer) {
    uint32_t irq, installed, count, redirected, fallback, dropped, redrawn, rebuilt, fonts;
    if (!writer || !writer->buf || writer->state != CANOPUS_STATUS_WRITER_WRITING ||
        writer->used > writer->capacity || writer->capacity - writer->used < 40u)
        return -1;
    irq = rh_platform_lock();
    installed = (uint32_t)S.installed;
    count = S.count;
    redirected = S.redirected;
    fallback = S.fallback;
    dropped = images_dropped;
    redrawn = redraws;
    rebuilt = rebuilds;
    fonts = fonts_retargeted;
    rh_platform_unlock(irq);
    if (canopus_status_put_u32(writer, 0x31514852u) ||
        canopus_status_put_u32(writer, 5u) ||
        canopus_status_put_u32(writer, installed) ||
        canopus_status_put_u32(writer, count) ||
        canopus_status_put_u32(writer, redirected) ||
        canopus_status_put_u32(writer, fallback) ||
        canopus_status_put_u32(writer, dropped) ||
        canopus_status_put_u32(writer, redrawn) ||
        canopus_status_put_u32(writer, rebuilt) ||
        canopus_status_put_u32(writer, fonts)) return -1;
    return canopus_status_writer_publish(writer);
}
__attribute__((used)) struct canopus_module_descriptor_v1 canopus_module_descriptor;
static void cp(char *d, const char *s, unsigned n) {
    unsigned i = 0;
    while (i < n && s[i]) { d[i] = s[i]; i++; }
    while (i < n) d[i++] = 0;
}
__attribute__((constructor)) static void ctor(void) {
    struct canopus_module_registration_v1 r;
    int fd;
    canopus_module_descriptor.struct_size = sizeof(canopus_module_descriptor);
    canopus_module_descriptor.abi_major = 1;
    canopus_module_descriptor.abi_minor = 2;
    canopus_module_descriptor.flags = CANOPUS_FLAG_REACTIVATE_AFTER_UI_RESTART;
    cp((char *)canopus_module_descriptor.module_id, RH_MODULE_ID, 32);
    cp((char *)canopus_module_descriptor.module_version, RH_VERSION, 16);
    cp((char *)canopus_module_descriptor.build_id, "resource-hook-0.3.0", 32);
    cp((char *)canopus_module_descriptor.target_id, RH_TARGET_ID, 32);
    canopus_module_descriptor.prepare = prepare;
    canopus_module_descriptor.activate = activate;
    canopus_module_descriptor.deactivate = stop;
    canopus_module_descriptor.stop = stop;
    canopus_module_descriptor.query = query;
    r.magic = CANOPUS_MODULE_REGISTRATION_MAGIC;
    r.descriptor = (uint32_t)(uintptr_t)&canopus_module_descriptor;
    cp((char *)r.module_id, RH_MODULE_ID, 32);
    fd = rh_platform_open("/dev/canopus", 2);
    if (fd >= 0) {
        /* Registration is one device message; Supervisor rejects a missing or
         * incomplete descriptor registration before activation. */
        (void)rh_platform_write(fd, &r, sizeof(r));
        rh_platform_close(fd);
    }
}
__attribute__((destructor)) static void dtor(void) { (void)stop(0); }

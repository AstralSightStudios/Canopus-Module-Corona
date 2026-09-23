#include "resource_hook_platform.h"
#include "canopus_abi.h"
#include "canopus_module_registration.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char config[] = "/resource/\t/data/canopus/themes/current/\n";
static const char *input = config, *control_config, *control_signal;
static unsigned position, control_position, signal_position;
static unsigned config_opens, control_opens, signal_opens, closes, allocations, frees, persistent_allocs, registrations;
static unsigned image_drops, redraws, rebuilds, retargets, timers_created, timers_deleted;
static int font_retargeted;
static int fail_timer, reject_redraw, reject_metadata, unsupported;
static unsigned metadata_refreshes;
static int open_errno = 5;
static int fail_open, fail_alloc, fail_read, driver_valid = 1, image_cache_ready = 1,
           redraw_ready = 1, locked;
static int driver;
static const char *backend_expected = "data/canopus/themes/current/icon.bin";
struct mock_timer { uint32_t interval; void (*callback)(void *); int active; };
static struct mock_timer mock_timers[64];
static int backend(void *d, const char *p, int mode) {
    assert(d == &driver && mode == 2);
    assert(!strcmp(p, backend_expected));
    return 7;
}
static int other(void *d, const char *p, int mode) { (void)d; (void)p; (void)mode; return 0; }
static rh_open_fn slot = backend;
int rh_platform_open(const char *path, int mode) {
    assert(!locked);
    if (!strcmp(path, "/dev/canopus")) { assert(mode == 2); return 10; }
    assert(mode == 1);
    if (!strcmp(path, "/data/canopus/themes/mappings.tsv")) {
        config_opens++;
        position = 0;
        if (fail_open) { open_errno = open_errno ? open_errno : 5; return -1; }
        return 11;
    }
    if (!strcmp(path, "/data/files/ng.lst.corona/reload.request")) {
        signal_opens++;
        signal_position = 0;
        if (!control_signal) { open_errno = RH_ENOENT; return -1; }
        return 12;
    }
    if (!strcmp(path, "/data/files/ng.lst.corona/mappings.tsv")) {
        control_opens++;
        control_position = 0;
        if (!control_config) { open_errno = RH_ENOENT; return -1; }
        return 13;
    }
    assert(!"unexpected file path");
    return -1;
}
int rh_platform_errno(void) { assert(!locked); return open_errno; }
int rh_platform_read(int fd, void *out, uint32_t size) {
    const char *data;
    unsigned *cursor;
    unsigned remaining;
    assert(!locked);
    if (fail_read) return -1;
    if (fd == 11) { data = input; cursor = &position; }
    else if (fd == 12) { data = control_signal; cursor = &signal_position; }
    else { assert(fd == 13); data = control_config; cursor = &control_position; }
    remaining = (unsigned)strlen(data) - *cursor;
    if (size > 3u) size = 3u;
    if (size > remaining) size = remaining;
    memcpy(out, data + *cursor, size);
    *cursor += size;
    return (int)size;
}
int rh_platform_write(int fd, const void *data, uint32_t size) {
    const struct canopus_module_registration_v1 *r = data;
    assert(!locked && fd == 10 && size == sizeof(*r));
    assert(r->magic == CANOPUS_MODULE_REGISTRATION_MAGIC);
    assert(!strcmp((const char *)r->module_id, "resource_hook"));
    registrations++;
    return (int)size;
}
void rh_platform_close(int fd) { assert(!locked && (fd == 10 || fd == 11 || fd == 12 || fd == 13)); closes++; }
void *rh_platform_alloc(uint32_t size) {
    assert(!locked && (size == sizeof(struct rh_rule) * RH_RULES + RH_CONFIG_BYTES ||
                       size == sizeof(struct rh_rule) * RH_RULES));
    if (fail_alloc) return NULL;
    allocations++;
    if (size == sizeof(struct rh_rule) * RH_RULES && !persistent_allocs)
        persistent_allocs++;
    return malloc(size);
}
void rh_platform_free(void *p) { assert(!locked && p); frees++; free(p); }
void *rh_platform_driver(void) { assert(locked); return &driver; }
rh_open_fn *rh_platform_slot(void) { assert(locked); return &slot; }
rh_open_fn rh_platform_original(void) { assert(locked); return backend; }
int rh_platform_driver_valid(void) { assert(locked); return driver_valid; }
uint32_t rh_platform_lock(void) { assert(!locked); locked = 1; return 42; }
void rh_platform_unlock(uint32_t irq) { assert(locked && irq == 42); locked = 0; }
int rh_platform_retire_mapped_images(const struct rh_mapping_view *view) {
    assert(view && view->rules && view->count == 1);
    assert(!rh_validate_rules(view->rules, view->count));
    if (unsupported) return 1;
    assert(!locked);  /* image invalidation must run outside the interrupt lock */
    if (!image_cache_ready) return -1;
    image_drops++;
    return 0;
}
int rh_platform_refresh_mapped_images(const struct rh_mapping_view *previous,
                                      const struct rh_mapping_view *current) {
    char mapped[RH_PATH];
    int old_match = previous ? rh_resolve_view(previous, "/resource/icon.bin", mapped) : 0;
    int new_match;
    assert(!locked && current && current->rules && current->count == 1);
    new_match = rh_resolve_view(current, "/resource/icon.bin", mapped);
    assert(old_match == 1 || new_match == 1);
    if (unsupported) return 1;
    if (reject_metadata) return -1;
    metadata_refreshes++;
    return 0;
}
int rh_platform_redraw_ready(void) { assert(!locked); return redraw_ready; }
void *rh_platform_timer_create(uint32_t interval, void (*cb)(void *)) {
    unsigned i;
    assert(!locked && interval && cb);
    if (fail_timer) return NULL;
    for (i = 0; i < sizeof(mock_timers) / sizeof(mock_timers[0]); i++) {
        if (!mock_timers[i].active) {
            mock_timers[i].interval = interval;
            mock_timers[i].callback = cb;
            mock_timers[i].active = 1;
            timers_created++;
            return &mock_timers[i];
        }
    }
    return NULL;
}
void rh_platform_timer_delete(void *timer) {
    struct mock_timer *mock = timer;
    assert(!locked && mock && mock->active);
    mock->active = 0;
    timers_deleted++;
}
static void fire_timers(uint32_t interval) {
    unsigned i;
    for (i = 0; i < sizeof(mock_timers) / sizeof(mock_timers[0]); i++) {
        struct mock_timer *timer = &mock_timers[i];
        if (timer->active && timer->interval == interval) timer->callback(timer);
    }
}
static unsigned active_timers(uint32_t interval) {
    unsigned i, count = 0;
    for (i = 0; i < sizeof(mock_timers) / sizeof(mock_timers[0]); i++)
        if (mock_timers[i].active && mock_timers[i].interval == interval) count++;
    return count;
}
/* Two registered families: one whose file falls under a mapping rule, one that
 * does not. Once retargeted the first resolves to the themed file, so it stops
 * matching and the walk terminates. */
#define THEMED_FONT "/data/canopus/themes/current/font/MiSans-Regular.ttf"
int rh_platform_font_path_get(uint32_t index, char *name, char *path) {
    assert(!locked && name && path);
    if (index >= 2u) return -1;
    if (index) {
        strcpy(name, "Other");
        strcpy(path, "/system/fonts/Other.ttf");
    } else {
        strcpy(name, "MiSans-Regular");
        strcpy(path, font_retargeted ? THEMED_FONT : "/resource/font/MiSans-Regular.ttf");
    }
    return 0;
}
int rh_platform_font_retarget(uint32_t index, const char *path) {
    assert(!locked && path && index == 0u && !font_retargeted);
    assert(!strcmp(path, THEMED_FONT));
    font_retargeted = 1;
    retargets++;
    return 0;
}
int rh_platform_rebuild_active_page(void) {
    assert(!"automatic page rebuild forbidden");
    return -1;
}
int rh_platform_request_full_redraw(void) {
    assert(!locked && redraw_ready);
    if (reject_redraw) return -1;
    redraws++;
    return 0;
}

static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
extern struct canopus_module_descriptor_v1 canopus_module_descriptor;
int main(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    struct canopus_status_writer_v1 w;
    unsigned char status[48];
    unsigned before;
    assert(registrations == 1 && closes == 1);
    assert(d->struct_size == sizeof(*d) && d->abi_major == 1 && d->abi_minor == 2);
    assert(!strcmp((const char *)d->module_id, "resource_hook"));
    assert(!strcmp((const char *)d->module_version, "0.3.0"));
#if defined(RH_TARGET_155) && RH_TARGET_155
    assert(!strcmp((const char *)d->target_id, "xiaomi-band-11-4.100.155"));
#else
    assert(!strcmp((const char *)d->target_id, "xiaomi-band-11-4.100.139"));
#endif
    assert(d->stop(NULL) == 0 && d->deactivate(NULL) == 0);
    assert(d->query(NULL) == -1);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w));
    assert(w.used == 40 && u32(status) == 0x31514852u && u32(status + 4) == 5);
    assert(u32(status + 8) == 0 && u32(status + 12) == 0 && u32(status + 24) == 0 &&
           u32(status + 28) == 0 && u32(status + 32) == 0 && u32(status + 36) == 0);
    assert(d->query(&w) == -1);
    assert(!canopus_status_writer_init(&w, status, 39));
    memset(status, 0xab, sizeof(status));
    assert(d->query(&w) == -1 && w.used == 0 && status[0] == 0xab);

    fail_open = 1;
    before = closes;
    assert(d->activate(NULL) == -2005 && slot == backend && closes == before);
    open_errno = 13;  /* EACCES is not optional absence. */
    assert(d->activate(NULL) == -2005 && slot == backend);
    open_errno = 0;  /* Unknown failure must not become success. */
    assert(d->activate(NULL) == -2005);
    open_errno = RH_ENOENT;
    driver_valid = 0;  /* No driver inspection is needed for a no-op. */
    assert(d->prepare(NULL) == 0);
    assert(d->activate(NULL) == 0 && slot == backend && closes == before);
    assert(d->activate(NULL) == 0 && slot == backend);
    assert(!allocations && !frees && !image_drops && !metadata_refreshes && !redraws &&
           !retargets && !timers_created && !active_timers(50u) && !active_timers(1000u));
    assert(d->stop(NULL) == 0 && d->deactivate(NULL) == 0);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w));
    for (unsigned i = 8; i < 40; i += 4) assert(u32(status + i) == 0);
    driver_valid = 1;
    fail_open = 0; fail_alloc = 1;
    assert(d->activate(NULL) == -2006 && slot == backend && closes == before + 1);
    fail_alloc = 0; fail_read = 1;
    assert(d->activate(NULL) == -2007 && allocations == frees);
    fail_read = 0; input = "/resource/\t/outside/\n";
    assert(d->activate(NULL) == -2007 && slot == backend && allocations == frees);
    input = "# empty\n";
    assert(d->prepare(NULL) == -2008);
    assert(d->activate(NULL) == -2008 && slot == backend);
    input = config;
    /* A prepared but unpublished snapshot must not survive missing config. */
    assert(d->prepare(NULL) == 0);
    fail_open = 1;
    assert(d->prepare(NULL) == 0);
    assert(d->activate(NULL) == 0 && slot == backend && !timers_created);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w) && u32(status + 12) == 0);
    fail_open = 0;
    driver_valid = 0;
    assert(d->activate(NULL) == -2008 && slot == backend && !locked);
    assert(allocations == frees + persistent_allocs);
    driver_valid = 1; slot = other;
    assert(d->activate(NULL) == -2009 && slot == other && !locked);
    slot = NULL;
    assert(d->activate(NULL) == -2009 && !slot);
    slot = backend;
    before = config_opens;
    image_cache_ready = 0;
    redraw_ready = 0;
    assert(d->activate(NULL) == 0 && slot != backend && !locked);
    assert(image_drops == 0);  /* no drop while the image caches do not yet exist */
    assert(redraws == 0);      /* and no redraw while no display exists yet */
    image_cache_ready = 1;
    redraw_ready = 1;
    slot = backend;
    assert(d->activate(NULL) == 0 && slot != backend && image_drops == 1 && redraws == 1);
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    assert(d->prepare(NULL) == -2004 && config_opens == before);
    fail_open = 1;
    assert(d->activate(NULL) == 0 && config_opens == before && image_drops == 2 && redraws == 2);
    slot = backend;
    assert(d->activate(NULL) == 0 && slot != backend && config_opens == before);
    assert(image_drops == 3 && redraws == 3);
    fire_timers(50u);  /* a retry that completed manually self-deletes on its next tick */
    assert(!active_timers(50u));
    assert(d->stop(NULL) == CANOPUS_RESULT_REBOOT_REQUIRED);
    assert(d->deactivate(NULL) == CANOPUS_RESULT_REBOOT_REQUIRED);
    assert(slot != backend);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w));
    assert(u32(status + 8) == 1 && u32(status + 12) == 1);
    assert(u32(status + 16) == 1 && u32(status + 20) == 0);
    assert(u32(status + 24) == image_drops && image_drops == 3);
    assert(u32(status + 28) == redraws && redraws == 3);
    assert(u32(status + 32) == rebuilds && rebuilds == 0);
    assert(u32(status + 36) == retargets && retargets == 1);
    /* Busy UI: requests coalesce and the cache isn't touched mid-render. */
    redraw_ready = 0;
    assert(d->activate(NULL) == 0 && active_timers(50u) == 1);
    before = timers_created;
    assert(d->activate(NULL) == 0 && timers_created == before + 1);
    fire_timers(50u);
    assert(image_drops == 3 && redraws == 3);
    redraw_ready = 1;
    reject_redraw = 1;
    fire_timers(50u);
    assert(image_drops == 4 && redraws == 3 && active_timers(50u) == 1);
    fire_timers(50u);
    assert(image_drops == 4);  /* failed invalidation doesn't repeatedly drop */
    reject_redraw = 0;
    fire_timers(50u);
    assert(redraws == 4 && !active_timers(50u));
    fire_timers(1000u);  /* stale watcher timers self-delete after reactivation */
    assert(active_timers(1000u) == 1);
    /* Metadata retry does not retire again, rebuild, or prematurely redraw. */
    reject_metadata = 1;
    before = metadata_refreshes;
    assert(d->activate(NULL) == 0 && active_timers(50u) == 1);
    assert(image_drops == 5 && redraws == 4);
    fire_timers(50u);
    assert(image_drops == 5 && metadata_refreshes == before && rebuilds == 0);
    reject_metadata = 0;
    fire_timers(50u);
    assert(metadata_refreshes == before + 1 && redraws == 5 && !active_timers(50u));
    fire_timers(1000u);
    /* Unsupported-target adapters complete a repaint, never claim retirement. */
    unsupported = 1;
    assert(d->activate(NULL) == 0 && !active_timers(50u));
    assert(image_drops == 5 && redraws == 6 && metadata_refreshes == before + 1);
    unsupported = 0;
    fire_timers(1000u);
    /* OOM remains visible, but doesn't remove the resident redirect. */
    redraw_ready = 0;
    fail_timer = 1;
    assert(d->activate(NULL) == -2011 && slot != backend);
    fail_timer = 0;
    assert(d->activate(NULL) == 0 && active_timers(50u) == 1);
    redraw_ready = 1;
    fire_timers(50u);
    assert(redraws == 7 && !active_timers(50u));
    fire_timers(1000u);

    /* A package-qualified signal publishes the manager's complete new snapshot. */
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tr1\n";
    control_config = "/resource/\t/data/canopus/themes/alternate/\n";
    before = image_drops;
    fire_timers(1000u);
    backend_expected = "data/canopus/themes/alternate/icon.bin";
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    assert(image_drops == before + 2 && redraws == 8);
    before = control_opens;
    fire_timers(1000u);
    assert(control_opens == before);  /* unchanged revision does not reread config */
    control_signal = "garbage-reload-v1\tng.lst.corona\tbad\n";
    fire_timers(1000u);
    assert(control_opens == before);  /* malformed/wrong-version prefix is ignored */

    /* A malformed config does not replace the last-known-good map or consume
     * its signal; fixing the file under the same revision is retried. */
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tr2\n";
    control_config = "/resource/\t/outside/\n";
    before = image_drops;
    fire_timers(1000u);
    assert(image_drops == before && slot(&driver, "resource/icon.bin", 2) == 7);
    control_config = "/resource/\t/data/canopus/themes/alternate2/\n";
    fire_timers(1000u);
    backend_expected = "data/canopus/themes/alternate2/icon.bin";
    assert(image_drops == before + 2 && slot(&driver, "resource/icon.bin", 2) == 7);

    /* Removed mappings are included in owner refresh, so the original path is
     * restored rather than leaving an image stuck on the previous theme. */
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tr3\n";
    control_config = "/other/\t/data/canopus/themes/other/\n";
    fire_timers(1000u);
    backend_expected = "resource/icon.bin";
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    backend_expected = "data/canopus/themes/other/icon.bin";
    assert(slot(&driver, "other/icon.bin", 2) == 7);

    /* A second revision arriving during a pending refresh waits until the first
     * retirement/owner transaction completes, then is applied in order. */
    redraw_ready = 0;
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tr4\n";
    control_config = "/resource/\t/data/canopus/themes/pending/\n";
    fire_timers(1000u);
    assert(active_timers(50u) == 1);
    before = control_opens;
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tr5\n";
    control_config = "/resource/\t/data/canopus/themes/final/\n";
    fire_timers(1000u);
    assert(control_opens == before);
    redraw_ready = 1;
    fire_timers(50u);
    fire_timers(1000u);
    backend_expected = "data/canopus/themes/final/icon.bin";
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    fire_timers(1000u);
    assert(allocations == frees + persistent_allocs && persistent_allocs == 1 &&
           active_timers(1000u) == 1 && !locked);
    puts("module registration, activation, config polling, atomic snapshots and targeted refresh passed");
    return 0;
}

#include "resource_hook_platform.h"
#include "canopus_abi.h"
#include "canopus_module_registration.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char config[] = "/resource/\t/data/canopus/themes/current/\n";
static const char *input = config;
static unsigned position, config_opens, closes, allocations, frees, registrations;
static unsigned image_drops, redraws, rebuilds, retargets, timers_created, timers_deleted;
static int font_retargeted;
static void (*timer_callback)(void *);
static int timer_token, fail_timer, reject_redraw, reject_rebuild;
static int fail_open, fail_alloc, fail_read, driver_valid = 1, image_cache_ready = 1,
           redraw_ready = 1, locked;
static int driver;
static int backend(void *d, const char *p, int mode) {
    assert(d == &driver && mode == 2);
    assert(!strcmp(p, "data/canopus/themes/current/icon.bin"));
    return 7;
}
static int other(void *d, const char *p, int mode) { (void)d; (void)p; (void)mode; return 0; }
static rh_open_fn slot = backend;
int rh_platform_open(const char *path, int mode) {
    assert(!locked);
    if (!strcmp(path, "/dev/canopus")) { assert(mode == 2); return 10; }
    assert(!strcmp(path, "/data/canopus/themes/mappings.tsv") && mode == 1);
    config_opens++;
    position = 0;
    return fail_open ? -1 : 11;
}
int rh_platform_read(int fd, void *out, uint32_t size) {
    unsigned remaining = (unsigned)strlen(input) - position;
    assert(!locked && fd == 11);
    if (fail_read) return -1;
    if (size > 3u) size = 3u;
    if (size > remaining) size = remaining;
    memcpy(out, input + position, size);
    position += size;
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
void rh_platform_close(int fd) { assert(!locked && (fd == 10 || fd == 11)); closes++; }
void *rh_platform_alloc(uint32_t size) {
    assert(!locked && size == sizeof(struct rh_rule) * RH_RULES + RH_CONFIG_BYTES);
    if (fail_alloc) return NULL;
    allocations++;
    return malloc(size);
}
void rh_platform_free(void *p) { assert(!locked && p); frees++; free(p); }
void *rh_platform_driver(void) { assert(locked); return &driver; }
rh_open_fn *rh_platform_slot(void) { assert(locked); return &slot; }
rh_open_fn rh_platform_original(void) { assert(locked); return backend; }
int rh_platform_driver_valid(void) { assert(locked); return driver_valid; }
uint32_t rh_platform_lock(void) { assert(!locked); locked = 1; return 42; }
void rh_platform_unlock(uint32_t irq) { assert(locked && irq == 42); locked = 0; }
int rh_platform_image_cache_drop_all(void) {
    assert(!locked);  /* image invalidation must run outside the interrupt lock */
    if (!image_cache_ready) return -1;
    image_drops++;
    return 0;
}
int rh_platform_redraw_ready(void) { assert(!locked); return redraw_ready; }
void *rh_platform_refresh_timer_create(void (*cb)(void *)) {
    assert(!locked && !timer_callback);
    if (fail_timer) return NULL;
    timer_callback = cb;
    timers_created++;
    return &timer_token;
}
void rh_platform_refresh_timer_delete(void *timer) {
    assert(!locked && timer == &timer_token && timer_callback);
    timer_callback = NULL;
    timers_deleted++;
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
    assert(!locked);  /* the page rebuild must also run outside the lock */
    if (reject_rebuild) return -1;
    rebuilds++;
    return 0;
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
    assert(!strcmp((const char *)d->target_id, "xiaomi-band-11-4.100.139"));
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
    driver_valid = 0;
    assert(d->activate(NULL) == -2008 && slot == backend && !locked);
    assert(allocations == frees);
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
    assert(d->stop(NULL) == CANOPUS_RESULT_REBOOT_REQUIRED);
    assert(d->deactivate(NULL) == CANOPUS_RESULT_REBOOT_REQUIRED);
    assert(slot != backend);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w));
    assert(u32(status + 8) == 1 && u32(status + 12) == 1);
    assert(u32(status + 16) == 1 && u32(status + 20) == 0);
    assert(u32(status + 24) == image_drops && image_drops == 3);
    assert(u32(status + 28) == redraws && redraws == 3);
    assert(u32(status + 32) == rebuilds && rebuilds == 3);
    assert(u32(status + 36) == retargets && retargets == 1);
    /* Busy UI: requests coalesce and the cache isn't touched mid-render. */
    redraw_ready = 0;
    before = timers_created;
    assert(d->activate(NULL) == 0 && timer_callback);
    assert(d->activate(NULL) == 0 && timers_created == before + 1);
    timer_callback(&timer_token);
    assert(image_drops == 3 && redraws == 3);
    redraw_ready = 1;
    reject_redraw = 1;
    timer_callback(&timer_token);
    assert(image_drops == 4 && redraws == 3 && timer_callback);
    timer_callback(&timer_token);
    assert(image_drops == 4);  /* failed invalidation doesn't repeatedly drop */
    reject_redraw = 0;
    timer_callback(&timer_token);
    assert(redraws == 4 && !timer_callback && timers_created == timers_deleted);
    /* The rebuild is attempted once per request and a refusal (a policy the
     * forced teardown declines, or a deferred destroy) must not be counted,
     * retried, or allowed to hold up the repaint. */
    reject_rebuild = 1;
    before = rebuilds;
    redraw_ready = 0;
    assert(d->activate(NULL) == 0 && timer_callback);
    redraw_ready = 1;
    timer_callback(&timer_token);
    assert(rebuilds == before && redraws == 5 && !timer_callback);
    reject_rebuild = 0;
    /* OOM remains visible, but doesn't remove the resident redirect. */
    redraw_ready = 0;
    fail_timer = 1;
    assert(d->activate(NULL) == -2011 && slot != backend);
    fail_timer = 0;
    assert(d->activate(NULL) == 0 && timer_callback);
    redraw_ready = 1;
    timer_callback(&timer_token);
    assert(redraws == 6 && !timer_callback);
    assert(allocations == frees && !locked);
    puts("module registration, activation failures, publication, query and resident lifecycle passed");
    return 0;
}

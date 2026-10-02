#include "resource_hook_platform.h"
#include "resource_hook_quickapp.h"
#include "canopus_abi.h"
#include "canopus_module_registration.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char config[] = "/resource/\tthemes/current/\n";
static const char *input = config, *control_config, *control_signal;
static unsigned position, control_position, signal_position;
static unsigned config_opens, control_opens, signal_opens, closes, allocations, frees, persistent_allocs, registrations;
struct temp_allocation { void *pointer; uint32_t size; int live; };
static struct temp_allocation temp_allocations[4096];
static uint32_t live_bytes, peak_bytes;
static unsigned alloc_attempts, fail_alloc_at;
static int reenter_read, nested_reload, activate_during_read;
static int reenter_signal_read, reenter_result_open, reenter_result_write;
static void fire_timers(uint32_t);
extern struct canopus_module_descriptor_v1 canopus_module_descriptor;
static int watcher_scheduled;
static char result_record[256];
static unsigned result_position, result_writes;
static int fail_result_write, fail_result_open, fail_result_after;
static char startup_record[2049];
static unsigned startup_position, startup_writes, startup_opens;
static int fail_startup_open, fail_startup_write, clobber_startup_errno;
static int registration_fd = 10;
static unsigned image_drops, redraws, rebuilds, retargets, timers_created, timers_deleted;
static int font_retargeted;
static int fail_timer, reject_redraw, reject_metadata, unsupported;
static unsigned metadata_refreshes;
static int open_errno = 5;
static int fail_open, fail_alloc, fail_read, driver_valid = 1, image_cache_ready = 1,
           redraw_ready = 1, locked;
static int driver;
static const char *backend_expected = (RH_THEME_ROOT "current/icon.bin") + 1;
struct mock_timer { uint32_t interval; void (*callback)(void *); int active; };
static struct mock_timer mock_timers[64];
static unsigned in_ui_timer, calendar_calls, calendar_queries, calendar_old_only;
static int calendar_mode, calendar_rc;
static int quickapp_mode, quickapp_result, reenter_lookup;
static unsigned quickapp_lookups, quickapp_owner_calls, quickapp_restores;
#if defined(RH_TARGET_1043) && RH_TARGET_1043
#define QUICKAPP_ROOT "/data/app/"
#else
#define QUICKAPP_ROOT "/data/quickapp/app/"
#endif
#define QUICKAPP_PACKAGE "org.example-app"
#define QUICKAPP_KEY RH_QUICKAPP_ICON_PREFIX QUICKAPP_PACKAGE
#define QUICKAPP_ICON QUICKAPP_ROOT QUICKAPP_PACKAGE "/install/res/icon.bin"
#define QUICKAPP_ICON2 QUICKAPP_ROOT QUICKAPP_PACKAGE "/reinstall/res/icon.bin"
static const char *quickapp_path = QUICKAPP_ICON;
int rh_platform_quickapp_icon_path(const char *package, char out[RH_PATH]) {
    assert(!locked && package && out);
    if (!quickapp_mode) return 0; /* Legacy modes have no installed apps. */
    assert(in_ui_timer && !strcmp(package, QUICKAPP_PACKAGE));
    quickapp_lookups++;
    if (reenter_lookup) {
        unsigned before = quickapp_lookups;
        reenter_lookup = 0;
        fire_timers(1000u);
        assert(quickapp_lookups == before); /* The outer writer owns lookup. */
    }
    if (quickapp_result == 1) {
        assert(strlen(quickapp_path) < RH_PATH);
        strcpy(out, quickapp_path);
    }
    return quickapp_result;
}
#define CALENDAR_RESOURCE "/resource/calendar/background.bin"
int rh_platform_calendar_affected(const struct rh_mapping_view *previous,
                                  const struct rh_mapping_view *current) {
    char mapped[RH_PATH];
    int old_match, new_match;
    assert(!locked && current && !rh_validate_view(current));
    if (!calendar_mode) return 0; /* Keep legacy scenarios calendar-neutral. */
    assert(!previous || !rh_validate_view(previous));
    old_match = previous ? rh_resolve_view(previous, CALENDAR_RESOURCE, mapped) : 0;
    new_match = rh_resolve_view(current, CALENDAR_RESOURCE, mapped);
    assert(old_match >= 0 && new_match >= 0);
    calendar_queries++;
    if (old_match && !new_match) calendar_old_only++;
    return old_match || new_match;
}
int rh_platform_refresh_calendar(void) {
    assert(calendar_mode && in_ui_timer && !locked && redraw_ready);
    calendar_calls++;
    return calendar_rc;
}
#if defined(RH_EXPERIMENTAL_FONT_RELOAD) && RH_EXPERIMENTAL_FONT_RELOAD
static unsigned font_calls, font_disabled, font_changes;
static int font_rc;
static char last_font_path[RH_PATH];
int rh_font_reload(const struct rh_mapping_view *current, uint32_t *changed) {
    assert(in_ui_timer && !locked && current && changed);
    font_calls++;
    last_font_path[0] = 0;
    (void)rh_resolve_view(current, "/resource/font/MiSans-Regular-All.ttf", last_font_path);
    *changed = !font_disabled && !font_rc ? font_changes : 0;
    return font_disabled ? -2099 : font_rc;
}
void rh_font_reload_disable(void) { assert(!locked); font_disabled++; }
#endif
static int backend(void *d, const char *p, int mode) {
    assert(d == &driver && mode == 2);
    assert(!strcmp(p, backend_expected));
    if (nested_reload) {
        unsigned i, live = 0;
        struct temp_allocation *old = NULL;
        nested_reload = 0;
        for (i = 0; i < allocations; i++) if (temp_allocations[i].live) {
            old = &temp_allocations[i]; live++;
        }
        if (quickapp_mode) {
            old = NULL;
            for (i = 0; i < allocations; i++) if (temp_allocations[i].live) {
                struct rh_snapshot *s = temp_allocations[i].pointer;
                if (!rh_snapshot_has_quickapps(s)) old = &temp_allocations[i];
            }
            assert(old && live == 2);
        } else assert(live == 1 && old);
        fire_timers(1000u);
        /* Refresh has completed, but this backend still pins its old map. */
        assert(old->live && persistent_allocs == (quickapp_mode ? 3u : 2u));
    }
    return 7;
}
static int other(void *d, const char *p, int mode) { (void)d; (void)p; (void)mode; return 0; }
static rh_open_fn slot = backend;
int rh_platform_open(const char *path, int mode) {
    assert(!locked);
    if (!strcmp(path, RH_STARTUP_LOG_PATH)) {
        assert(mode == RH_STARTUP_LOG_FLAGS);
        startup_opens++;
        if (clobber_startup_errno && config_opens) open_errno = 24;
        if (fail_startup_open) return -1;
        startup_position = 0;
        memset(startup_record, 0, sizeof(startup_record));
        return 15;
    }
    if (!strcmp(path, "/dev/canopus")) {
        const char *fault = getenv("RH_TEST_REGISTRATION");
        assert(mode == 2);
        if (fault && !strcmp(fault, "open-fail")) { open_errno = 13; return -1; }
        if (fault && !strcmp(fault, "fd-zero")) registration_fd = 0;
        return registration_fd;
    }
    if (!strcmp(path, RH_CONTROL_RESPONSE_PATH)) {
        assert(mode == 2);
        if (fail_result_open) return -1;
        if (reenter_result_open) {
            unsigned signals = signal_opens, draws = redraws;
            reenter_result_open = 0;
            fire_timers(50u); fire_timers(1000u);
            assert(signal_opens == signals && redraws == draws);
        }
        result_position=0; return 14;
    }
    assert(mode == 1);
    if (!strcmp(path, RH_CONTROL_REQUEST_PATH)) {
        signal_opens++;
        signal_position = 0;
        if (!control_signal) { open_errno = RH_ENOENT; return -1; }
        return 12;
    }
    if (!strcmp(path, RH_CONFIG_PATH)) {
        if (watcher_scheduled) {
            control_opens++;
            control_position = 0;
            if (!control_config) { open_errno = RH_ENOENT; return -1; }
            return 13;
        }
        config_opens++;
        position = 0;
        if (fail_open) return -1;
        return 11;
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
    if (fd == 12 && reenter_signal_read) {
        unsigned signals = signal_opens;
        reenter_signal_read = 0;
        fire_timers(1000u);
        assert(signal_opens == signals);
    }
    if (fd == 13 && reenter_read) {
        unsigned opened = control_opens;
        reenter_read = 0;
        fire_timers(1000u);
        assert(control_opens == opened); /* The outer control writer owns I/O. */
    }
    if (fd == 13 && activate_during_read) {
        activate_during_read = 0;
        assert(canopus_module_descriptor.activate(NULL) == 0);
    }
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
    if (fd == 15) {
        assert(!locked);
        startup_writes++;
        if (fail_startup_write == 1) return -1;
        if (fail_startup_write == 2) return 0;
        if (fail_startup_write == 3) return (int)size + 1;
        if (size > 31u) size = 31u; /* Exercise bounded short-write completion. */
        assert(startup_position + size < sizeof(startup_record));
        memcpy(startup_record + startup_position, data, size);
        startup_position += size;
        return (int)size;
    }
    if(fd==14) {
        assert(!locked);
        if (reenter_result_write) {
            unsigned signals = signal_opens, writes = result_writes, draws = redraws;
            reenter_result_write = 0;
            fire_timers(50u); fire_timers(1000u);
            assert(signal_opens == signals && result_writes == writes && redraws == draws);
        }
        if(fail_result_write || (fail_result_after && result_position >= 7u)) return -1;
        if(size>7) size=7; /* Exercise short-write completion. */
        assert(result_position+size<=sizeof(result_record));
        memcpy(result_record+result_position,data,size);
        result_position+=size; result_writes++;
        return (int)size;
    }
    assert(!locked && fd == registration_fd && size == sizeof(*r));
    assert(r->magic == CANOPUS_MODULE_REGISTRATION_MAGIC);
    assert(!strcmp((const char *)r->module_id, "corona"));
    registrations++;
    {
        const char *fault = getenv("RH_TEST_REGISTRATION");
        if (fault && !strcmp(fault, "write-fail")) { open_errno = 5; return -1; }
        if (fault && !strcmp(fault, "short-write")) return (int)size - 1;
    }
    return (int)size;
}
void rh_platform_close(int fd) {
    assert(!locked);
    if(fd==14 || fd==15) return;
    assert(fd == registration_fd || fd == 11 || fd == 12 || fd == 13); closes++;
}
void *rh_platform_alloc(uint32_t size) {
    void *pointer;
    assert(!locked && size && size <= RH_CONFIG_BYTES + 1u +
           sizeof(struct rh_snapshot) + RH_RULES * sizeof(struct rh_indexed_rule));
    if (++alloc_attempts == fail_alloc_at || fail_alloc) return NULL;
    assert(allocations < sizeof(temp_allocations) / sizeof(temp_allocations[0]));
    pointer = malloc(size);
    assert(pointer);
    temp_allocations[allocations++] = (struct temp_allocation){pointer, size, 1};
    persistent_allocs++;
    live_bytes += size;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return pointer;
}
void rh_platform_free(void *p) {
    unsigned i;
    assert(!locked && p);
    for (i = 0; i < allocations; i++) if (temp_allocations[i].pointer == p && temp_allocations[i].live) {
        temp_allocations[i].live = 0;
        live_bytes -= temp_allocations[i].size;
        persistent_allocs--;
        frees++;
        free(p);
        return;
    }
    assert(!"foreign or double free");
}
void *rh_platform_driver(void) { assert(locked); return &driver; }
rh_open_fn *rh_platform_slot(void) { assert(locked); return &slot; }
rh_open_fn rh_platform_original(void) { assert(locked); return backend; }
int rh_platform_driver_valid(void) { assert(locked); return driver_valid; }
uint32_t rh_platform_lock(void) { assert(!locked); locked = 1; return 42; }
void rh_platform_unlock(uint32_t irq) { assert(locked && irq == 42); locked = 0; }
int rh_platform_retire_mapped_images(const struct rh_mapping_view *view) {
    assert(view && view->count <= RH_RULES);
    assert(!rh_validate_view(view));
    if (!view->count || unsupported) return 1;
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
    assert(!locked && current && current->count <= RH_RULES && !rh_validate_view(current));
    new_match = rh_resolve_view(current, "/resource/icon.bin", mapped);
    if (calendar_mode) {
        int old_calendar = previous ? rh_resolve_view(previous, CALENDAR_RESOURCE, mapped) : 0;
        int new_calendar = rh_resolve_view(current, CALENDAR_RESOURCE, mapped);
        if (old_calendar || new_calendar) assert(calendar_calls && !calendar_rc);
    }
    assert(old_match >= 0 && new_match >= 0); /* Font-only exact rules need no image owner. */
    if (quickapp_mode) {
        int old_icon = previous ? rh_resolve_view(previous, QUICKAPP_ICON, mapped) : 0;
        int new_icon = rh_resolve_view(current, QUICKAPP_ICON, mapped);
        int old_icon2 = previous ? rh_resolve_view(previous, QUICKAPP_ICON2, mapped) : 0;
        int new_icon2 = rh_resolve_view(current, QUICKAPP_ICON2, mapped);
        assert(old_icon >= 0 && new_icon >= 0 &&
               old_icon2 >= 0 && new_icon2 >= 0);
        if (old_icon || new_icon || old_icon2 || new_icon2) {
            assert(in_ui_timer);
            quickapp_owner_calls++;
        }
        if ((old_icon && !new_icon) || (old_icon2 && !new_icon2)) quickapp_restores++;
    }
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
            if (interval == 1000u) watcher_scheduled = 1;
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
        if (timer->active && timer->interval == interval) {
            unsigned previous_ui_timer = in_ui_timer;
            in_ui_timer = 1;
            timer->callback(timer);
            in_ui_timer = previous_ui_timer;
        }
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
#define THEMED_FONT RH_THEME_ROOT "current/font/MiSans-Regular-All.ttf"
int rh_platform_font_path_get(uint32_t index, char *name, char *path) {
    assert(!locked && name && path);
    if (index >= 2u) return -1;
    if (index) {
        strcpy(name, "Other");
        strcpy(path, "/system/fonts/Other.ttf");
    } else {
        strcpy(name, "MiSans-Regular");
        strcpy(path, font_retargeted ? THEMED_FONT : "/resource/font/MiSans-Regular-All.ttf");
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
static void assert_result(const char *signal, const char *row) {
    uint32_t hash = 2166136261u;
    unsigned i, n = (unsigned)(strlen(signal) + strlen(row));
    char *end;
    assert(n + 12u <= sizeof(result_record));
    assert(result_position == sizeof(result_record));
    assert(!memcmp(result_record, signal, strlen(signal)));
    assert(!memcmp(result_record + strlen(signal), row, strlen(row)));
    for (i = 0; i < n; i++) hash = (hash ^ (unsigned char)result_record[i]) * 16777619u;
    assert(strtoul(result_record + n, &end, 10) == hash && *end++ == '\n');
    for (; end < result_record + sizeof(result_record); end++) assert(!*end);
}
static void assert_status(int error, unsigned count, unsigned pending) {
    char row[80];
    snprintf(row, sizeof(row), "RHST1\t1\t%s\t%d\t%u\t%u\n",
             error ? "config_error" : "running", error, count, pending);
    assert_result(control_signal, row);
}
static void query_status(const char *signal, int error, unsigned count, unsigned pending) {
    unsigned opens = control_opens, allocs = allocations, lookups = quickapp_lookups;
    control_signal = signal;
    fire_timers(1000u);
    assert_status(error, count, pending);
    assert(control_opens == opens && allocations == allocs && quickapp_lookups == lookups);
}
static int test_control_status(const char *startup) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    unsigned writes, opens, allocs, timers, draws;
    char bad[160], longest[160], saved[256];
    const char *invalid[] = {
        "resource-hook-status-v2\tng.lst.corona\tq\n",
        "resource-hook-status-v1\tother.package\tq\n",
        "resource-hook-status-v1\tq\n",
        "resource-hook-status-v1\tng.lst.corona\t\n",
        "resource-hook-status-v1\tng.lst.corona\tq/unsafe\n",
        "resource-hook-status-v1\tng.lst.corona\tq\textra\n",
        "resource-hook-status-v1\tng.lst.corona\tq",
        "resource-hook-status-v1\tng.lst.corona\tq\nextra\n"
    };
    unsigned i, count = !strcmp(startup, "valid");
    if (!strcmp(startup, "missing")) { fail_open = 1; open_errno = RH_ENOENT; }
    else if (!strcmp(startup, "empty")) input = "";
    else if (!strcmp(startup, "comments")) input = "# no active themes\n";
    else assert(count);
    assert(!active_timers(1000u) && !result_writes);
    assert(d->activate(NULL) == 0);
    fail_open = 0;
#if defined(RH_EXPERIMENTAL_FONT_RELOAD) && RH_EXPERIMENTAL_FONT_RELOAD
    font_rc = -2090; /* A font-refresh rejection is not a config error. */
#endif
    fire_timers(50u);
    timers = timers_created; draws = redraws;
    query_status("resource-hook-status-v1\tng.lst.corona\tfirst._-0\n", 0, count, 0);
#if defined(RH_EXPERIMENTAL_FONT_RELOAD) && RH_EXPERIMENTAL_FONT_RELOAD
    font_rc = 0;
#endif
    assert(timers_created == timers && redraws == draws && active_timers(1000u) == 1);
    writes = result_writes;
    fire_timers(1000u);
    assert(result_writes == writes); /* Unchanged observations do not wear flash. */
    memcpy(saved, result_record, sizeof(saved));
    opens = control_opens; allocs = allocations;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        control_signal = invalid[i];
        fire_timers(1000u);
        assert(result_writes == writes && !memcmp(saved, result_record, sizeof(saved)));
    }
    strcpy(longest, "resource-hook-status-v1\tng.lst.corona\t");
    i = (unsigned)strlen(longest);
    memset(longest + i, 'x', 64u); strcpy(longest + i + 64u, "\n");
    query_status(longest, 0, count, 0);
    strcpy(bad, longest); bad[i + 64u] = 'x'; strcpy(bad + i + 65u, "\n");
    control_signal = bad; writes = result_writes;
    fire_timers(1000u);
    assert(result_writes == writes && control_opens == opens && allocations == allocs);
    /* Failed open, failed write and partial write retry without reloading. */
    for (i = 0; i < 3u; i++) {
        snprintf(bad, sizeof(bad), "resource-hook-status-v1\tng.lst.corona\tretry-%u\n", i);
        control_signal = bad;
        fail_result_open = i == 0; fail_result_write = i == 1; fail_result_after = i == 2;
        fire_timers(1000u);
        fail_result_open = fail_result_write = fail_result_after = 0;
        fire_timers(1000u);
        assert_status(0, count, 0);
        assert(control_opens == opens && allocations == allocs);
    }
    /* Reload failures preserve the map and remain visible to later queries. */
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tmissing-runtime\n";
    control_config = NULL;
    fire_timers(1000u);
    query_status("resource-hook-status-v1\tng.lst.corona\tmissing-config\n", -2102, count, 0);
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tinvalid-runtime\n";
    control_config = "/resource/\t/outside/\n";
    fire_timers(1000u);
    query_status("resource-hook-status-v1\tng.lst.corona\tinvalid-config\n", -2103, count, 0);
    fail_alloc = 1;
    control_signal = "resource-hook-reload-v1\tng.lst.corona\toom-runtime\n";
    control_config = config;
    fire_timers(1000u); fail_alloc = 0;
    query_status("resource-hook-status-v1\tng.lst.corona\toom-config\n", -2101, count, 0);
    /* A published good config clears errors before its UI refresh completes. */
    redraw_ready = 0;
    control_signal = "resource-hook-reload-v1\tng.lst.corona\trepair\n";
    control_config = "/resource/\tthemes/repaired/\n";
    fire_timers(1000u);
    assert(active_timers(50u) == 1);
    /* Even with a ready UI, a fresh query must not execute refresh work. */
    redraw_ready = 1;
    reenter_signal_read = reenter_result_open = reenter_result_write = 1;
    draws = redraws;
    query_status("resource-hook-status-v1\tng.lst.corona\tpending\n", 0, 1, 1);
    assert(!reenter_signal_read && !reenter_result_open && !reenter_result_write);
    assert(redraws == draws && active_timers(50u) == 1);
    redraw_ready = 0;
    /* Also guard a response flush initiated outside the watch reservation. */
    control_signal = "resource-hook-status-v1\tng.lst.corona\tpending-retry\n";
    fail_result_write = 1; fire_timers(1000u); fail_result_write = 0;
    reenter_result_open = reenter_result_write = 1;
    fire_timers(50u);
    assert(!reenter_result_open && !reenter_result_write);
    assert_status(0, 1, 1);
    memcpy(saved, result_record, sizeof(saved));
    fire_timers(50u);
    assert(!memcmp(saved, result_record, sizeof(saved)));
    redraw_ready = 1;
    fire_timers(50u);
    assert_status(0, 1, 0); /* Late reload completion cannot overwrite RHST1. */
    backend_expected = (RH_THEME_ROOT "repaired/icon.bin") + 1;
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    control_signal = "resource-hook-reload-v1\tng.lst.corona\trepair\n";
    allocs = allocations; draws = redraws;
    fire_timers(1000u);
    assert(!memcmp(result_record, control_signal, strlen(control_signal)));
    memcpy(saved, result_record, sizeof(saved));
    query_status("resource-hook-status-v1\tng.lst.corona\tbetween-retries\n", 0, 1, 0);
    control_signal = "resource-hook-reload-v1\tng.lst.corona\trepair\n";
    fire_timers(1000u);
    assert(!memcmp(saved, result_record, sizeof(saved)));
    assert(allocations == allocs && redraws == draws); /* Receipt retry, no reload. */
    /* A newer reload also owns the slot while the old refresh is pending. */
    redraw_ready = 0;
    control_signal = "resource-hook-reload-v1\tng.lst.corona\told-pending\n";
    control_config = "/resource/\tthemes/old/\n";
    fire_timers(1000u); memcpy(saved, result_record, sizeof(saved));
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tnew-pending\n";
    control_config = "";
    fire_timers(1000u);
    assert(!memcmp(saved, result_record, sizeof(saved)));
    redraw_ready = 1; fire_timers(50u);
    assert(!memcmp(saved, result_record, sizeof(saved)));
    fire_timers(1000u); fire_timers(50u);
    query_status("resource-hook-status-v1\tng.lst.corona\tcleared\n", 0, 0, 0);
    assert(!persistent_allocs && allocations == frees && !locked);
    assert(active_timers(1000u) == 1 && !active_timers(50u));
    printf("control status (%s), validation, retry, LKG and shared-slot ownership passed\n", startup);
    return 0;
}
static void assert_reload_status(int result, unsigned pending, unsigned changed,
                                 int error, unsigned count) {
    char row[96];
    snprintf(row, sizeof(row), "RHRS2\t1\t%d\t%u\t%u\t%s\t%d\t%u\n",
             result, pending, changed, error ? "config_error" : "running", error, count);
    assert_result(control_signal, row);
}
static int test_control_reload_status(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    unsigned writes, opens, allocs, draws, i;
    char longest[160], bad[160], saved[256];
    const char *invalid[] = {
        "resource-hook-reload-v3\tng.lst.corona\tid\n",
        "resource-hook-reload-v2\tid\n",
        "resource-hook-reload-v2\tother.package\tid\n",
        "resource-hook-reload-v2\tng.lst.corona\t\n",
        "resource-hook-reload-v2\tng.lst.corona\tid/unsafe\n",
        "resource-hook-reload-v2\tng.lst.corona\tid\textra\n",
        "resource-hook-reload-v2\tng.lst.corona\tid\r\n",
        "resource-hook-reload-v2\tng.lst.corona\tid",
        "resource-hook-reload-v2\tng.lst.corona\tid\nextra\n",
        "resource-hook-reload-v2\tng.lst.corona\t\200\n"
    };
#if defined(RH_EXPERIMENTAL_FONT_RELOAD) && RH_EXPERIMENTAL_FONT_RELOAD
    const int pending_result = 1;
#else
    const int pending_result = 0;
#endif
    assert(d->activate(NULL) == 0);
    fire_timers(50u);
    control_config = config;
    opens = control_opens; allocs = allocations; writes = result_writes;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        control_signal = invalid[i]; fire_timers(1000u);
        assert(control_opens == opens && allocations == allocs && result_writes == writes);
    }
    strcpy(longest, "resource-hook-reload-v2\tng.lst.corona\t");
    i = (unsigned)strlen(longest);
    memset(longest + i, 'x', 64u); strcpy(longest + i + 64u, "\n");
    control_signal = longest; fire_timers(1000u);
    assert_reload_status(0, 0, 0, 0, 1);
    opens = control_opens; allocs = allocations; writes = result_writes;
    fire_timers(1000u);
    assert(control_opens == opens && allocations == allocs && result_writes == writes);
    strcpy(bad, longest); bad[i + 64u] = 'x'; strcpy(bad + i + 65u, "\n");
    control_signal = bad; fire_timers(1000u);
    assert(control_opens == opens && allocations == allocs && result_writes == writes);
    /* The legacy maximum is still 128 bytes; framing always stays in bounds. */
    strcpy(longest, "resource-hook-reload-v1\t"); i = (unsigned)strlen(longest);
    memset(longest + i, 'y', 127u - i); strcpy(longest + 127u, "\n");
    control_signal = longest; fire_timers(1000u);
#if defined(RH_EXPERIMENTAL_FONT_RELOAD) && RH_EXPERIMENTAL_FONT_RELOAD
    assert_result(longest, "RHRS1\t6\t0\t0\t0\n");
#else
    assert_result(longest, "RHRS1\t5\t0\t0\t0\n");
#endif
    writes = result_writes; longest[127] = 'y'; strcpy(longest + 128u, "\n");
    fire_timers(1000u); assert(result_writes == writes);

    /* Snapshot counts the newly published map, even before refresh finishes. */
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tpending\n";
    control_config = "/resource/a.bin\tthemes/new/a.bin\n"
                     "/resource/b.bin\tthemes/new/b.bin\n";
    redraw_ready = 0;
    reenter_signal_read = reenter_read = reenter_result_open = reenter_result_write = 1;
    fire_timers(1000u);
    assert(!reenter_signal_read && !reenter_read && !reenter_result_open && !reenter_result_write);
    assert_reload_status(pending_result, 1, 0, 0, 2);
    opens = control_opens; writes = result_writes;
    fire_timers(1000u);
    assert(control_opens == opens && result_writes == writes);
#if defined(RH_EXPERIMENTAL_FONT_RELOAD) && RH_EXPERIMENTAL_FONT_RELOAD
    font_rc = -2090;
#endif
    redraw_ready = 1; fire_timers(50u);
#if defined(RH_EXPERIMENTAL_FONT_RELOAD) && RH_EXPERIMENTAL_FONT_RELOAD
    assert_reload_status(-2090, 0, 0, 0, 2); /* Font failure is still running. */
    font_rc = 0;
#else
    assert_reload_status(0, 0, 0, 0, 2);
#endif
    /* Failed config keeps the active count; no status round trip is needed. */
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tbad-config\n";
    control_config = "/resource/\t/outside/\n";
    fire_timers(1000u); assert_reload_status(-2103, 0, 0, -2103, 2);
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tmissing-config\n";
    control_config = NULL;
    fire_timers(1000u); assert_reload_status(-2102, 0, 0, -2102, 2);
    control_signal = "resource-hook-reload-v2\tng.lst.corona\toom-config\n";
    control_config = config; fail_alloc = 1;
    fire_timers(1000u); fail_alloc = 0;
    assert_reload_status(-2101, 0, 0, -2101, 2);

    /* Shared slot ownership in both directions; neither version cancels work. */
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tv2-old\n";
    control_config = config; redraw_ready = 0; fire_timers(1000u);
    assert_reload_status(pending_result, 1, 0, 0, 1);
    memcpy(saved, result_record, sizeof(saved));
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tv1-new\n";
    control_config = ""; fire_timers(1000u);
    redraw_ready = 1; fire_timers(50u);
    assert(!memcmp(saved, result_record, sizeof(saved)));
    fire_timers(1000u); fire_timers(50u);
#if defined(RH_EXPERIMENTAL_FONT_RELOAD) && RH_EXPERIMENTAL_FONT_RELOAD
    assert_result(control_signal, "RHRS1\t6\t0\t0\t0\n");
#else
    assert_result(control_signal, "RHRS1\t5\t0\t0\t0\n");
#endif
    opens = control_opens; allocs = allocations; draws = redraws; writes = result_writes;
    fire_timers(1000u);
    assert(control_opens == opens && allocations == allocs && redraws == draws && result_writes == writes);
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tv1-old\n";
    control_config = config; redraw_ready = 0; fire_timers(1000u);
    memcpy(saved, result_record, sizeof(saved));
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tv2-new\n";
    control_config = ""; fire_timers(1000u);
    redraw_ready = 1; fire_timers(50u);
    assert(!memcmp(saved, result_record, sizeof(saved)));
    fire_timers(1000u); fire_timers(50u);
    assert_reload_status(0, 0, 0, 0, 0);
    opens = control_opens; allocs = allocations; draws = redraws;
    query_status("resource-hook-status-v1\tng.lst.corona\tbetween-v2\n", 0, 0, 0);
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tv2-new\n";
    fire_timers(1000u); assert_reload_status(0, 0, 0, 0, 0);
    assert(control_opens == opens && allocations == allocs && redraws == draws);
    /* Write/open/partial retries use cached responses, not another reload. */
    for (i = 0; i < 3u; i++) {
        snprintf(bad, sizeof(bad), "resource-hook-reload-v2\tng.lst.corona\tretry-%u\n", i);
        control_signal = bad;
        fail_result_open = i == 0; fail_result_write = i == 1; fail_result_after = i == 2;
        fire_timers(1000u); opens = control_opens; allocs = allocations;
        fail_result_open = fail_result_write = fail_result_after = 0;
        reenter_result_open = reenter_result_write = 1;
        fire_timers(1000u); assert_reload_status(0, 0, 0, 0, 0);
        assert(control_opens == opens && allocations == allocs);
    }
    /* Identical IDs across versions are distinct revisions; each version
     * deduplicates its own exact completed request. */
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tsame-id\n";
    fire_timers(1000u); opens = control_opens;
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tsame-id\n";
    fire_timers(1000u); assert_reload_status(0, 0, 0, 0, 0);
    assert(control_opens == opens + 1u);
    opens = control_opens; writes = result_writes;
    fire_timers(1000u);
    assert(control_opens == opens && result_writes == writes);
    /* Status takes ownership while v2 work is pending, including completion. */
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tstatus-owner\n";
    control_config = config; redraw_ready = 0; fire_timers(1000u);
    query_status("resource-hook-status-v1\tng.lst.corona\tnew-owner\n", 0, 1, 1);
    redraw_ready = 1; fire_timers(50u); assert_status(0, 1, 0);

    /* Autonomous QuickApp failures use memory status, with sticky config priority. */
    quickapp_mode = 1; quickapp_result = 1;
    control_config = "/resource/\tthemes/current/\n"
                     QUICKAPP_KEY "\tthemes/current/icon.bin\n";
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tquickapp\n";
    fire_timers(1000u); fire_timers(50u); assert_reload_status(0, 0, 0, 0, 2);
    quickapp_result = -1; fire_timers(1000u);
    assert_reload_status(-2104, 0, 0, -2104, 2);
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tsticky\n";
    control_config = "/resource/\t/outside/\n";
    fire_timers(1000u); assert_reload_status(-2103, 0, 0, -2103, 2);
    /* Return ownership to the last completed reload: autonomous error differs
     * from config error, which remains latched and takes priority. */
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tquickapp\n";
    fire_timers(1000u); assert_reload_status(-2104, 0, 0, -2103, 2);
    quickapp_result = 1; fire_timers(1000u);
    assert_reload_status(0, 0, 0, -2103, 2);
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tclear\n";
    control_config = ""; fire_timers(1000u); fire_timers(50u);
    assert_reload_status(0, 0, 0, 0, 0);
#if defined(RH_EXPERIMENTAL_FONT_RELOAD) && RH_EXPERIMENTAL_FONT_RELOAD
    /* Exercise maximum-width numeric serialization with the longest v2 ID. */
    strcpy(longest, "resource-hook-reload-v2\tng.lst.corona\t");
    i = (unsigned)strlen(longest);
    memset(longest + i, 'z', 64u); strcpy(longest + i + 64u, "\n");
    control_signal = longest; control_config = config; redraw_ready = 0;
    font_changes = UINT32_MAX;
    fire_timers(1000u); assert_reload_status(1, 1, 0, 0, 1);
    redraw_ready = 1; fire_timers(50u);
    assert_reload_status(0, 0, UINT32_MAX, 0, 1);
    longest[i] = 'w'; control_config = ""; font_rc = INT32_MIN;
    fire_timers(1000u); fire_timers(50u);
    assert_reload_status(INT32_MIN, 0, 0, 0, 0);
#endif
    assert(!persistent_allocs && allocations == frees && !locked);
    assert(active_timers(1000u) == 1 && !active_timers(50u));
    puts("control reload v2 snapshots, validation, v1 compatibility, retries and ownership passed");
    return 0;
}
static int test_control_quickapp_status(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    quickapp_mode = 1; quickapp_result = 1;
    input = "/resource/\tthemes/current/\n"
            QUICKAPP_KEY "\tthemes/current/icon.bin\n";
    assert(d->activate(NULL) == 0);
    fire_timers(50u); fire_timers(1000u); fire_timers(50u);
    query_status("resource-hook-status-v1\tng.lst.corona\tquickapp-initial\n", 0, 2, 0);
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tbad-quickapp-config\n";
    control_config = "/resource/\t/outside/\n";
    fire_timers(1000u);
    query_status("resource-hook-status-v1\tng.lst.corona\tsticky-error\n", -2103, 2, 0);
    fire_timers(1000u);
    assert_status(-2103, 2, 0); /* Background success cannot clear config failure. */
    quickapp_result = -1; fire_timers(1000u);
    assert_status(-2103, 2, 0); /* Explicit config error wins over lookup error. */
    quickapp_result = 1; fire_timers(1000u);
    assert_status(-2103, 2, 0);
    control_signal = "resource-hook-reload-v1\tng.lst.corona\trepair-quickapp-config\n";
    control_config = input;
    fire_timers(1000u); fire_timers(50u);
    query_status("resource-hook-status-v1\tng.lst.corona\tquickapp-repaired\n", 0, 2, 0);
    quickapp_result = -1; fire_timers(1000u);
    assert_status(-2104, 2, 0);
    quickapp_result = -2; fire_timers(1000u);
    assert_status(-2105, 2, 0);
    quickapp_result = 1; fire_timers(1000u);
    assert_status(0, 2, 0);
    fail_alloc = 1; fire_timers(1000u); fail_alloc = 0;
    assert_status(-2101, 2, 0);
    fire_timers(1000u);
    assert_status(0, 2, 0);
    quickapp_result = 0; fire_timers(1000u); fire_timers(50u);
    assert_status(0, 1, 0); /* Absent package is not a config error. */
    assert(quickapp_restores && allocations == frees + persistent_allocs && !locked);
    assert(active_timers(1000u) == 1 && !active_timers(50u));
    puts("status query keeps QuickApp watcher alive and config errors independently sticky");
    return 0;
}
static int test_startup_diagnostics(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    const char *fault = getenv("RH_TEST_REGISTRATION");
    unsigned before, i;
    assert(!strcmp(RH_STARTUP_LOG_PATH, "/data/offlinelog/resource-hook-startup.log"));
    assert(strstr(startup_record, "RHSTART1 target="));
    assert(strstr(startup_record, "config=" RH_CONFIG_PATH "\n"));
    assert(strstr(startup_record, "ctor.begin rc=0"));
    if (fault && !strcmp(fault, "open-fail")) {
        assert(!registrations && !closes);
        assert(strstr(startup_record, "register.open rc=-1 errno=13"));
        assert(strstr(startup_record, "ctor.end rc=-1 errno=13"));
        return 0;
    }
    assert(registrations == 1 && closes == 1);
    if (fault && !strcmp(fault, "write-fail")) {
        assert(strstr(startup_record, "register.write rc=-1 errno=5"));
        assert(strstr(startup_record, "ctor.end rc=-1 errno=5"));
        return 0;
    }
    if (fault && !strcmp(fault, "short-write")) {
        assert(strstr(startup_record, "register.write rc=39 errno=0"));
        assert(strstr(startup_record, "ctor.end rc=-1 errno=0"));
        return 0;
    }
    assert(strstr(startup_record, fault ? "register.open rc=0" : "register.open rc=10"));
    assert(strstr(startup_record, "register.write rc=40 errno=0"));
    assert(strstr(startup_record, "ctor.end rc=0"));
    assert(startup_writes > startup_opens); /* Logger completes short writes. */

    /* Diagnostic opens must not replace the errno belonging to config.open. */
    fail_open = 1; open_errno = 13; clobber_startup_errno = 1;
    driver_valid = 0; /* Config failure is soft; the driver check still fails. */
    assert(d->activate(NULL) == -2008 && slot == backend);
    assert(strstr(startup_record, "config.open rc=-1 errno=13"));
    assert(strstr(startup_record, "config.fallback rc=-2005 errno=13"));
    assert(strstr(startup_record, "prepare.end rc=0"));
    assert(strstr(startup_record, "activate.end rc=-2008"));
    clobber_startup_errno = 0;

    /* Log open/write failures are best-effort, never a new lifecycle failure. */
    fail_startup_open = 1; open_errno = RH_ENOENT;
    assert(d->prepare(NULL) == 0);
    before = startup_writes;
    driver_valid = 0;
    assert(d->activate(NULL) == -2008 && startup_writes == before);
    fail_startup_open = 0;
    for (i = 1; i <= 3; i++) {
        fail_startup_write = (int)i;
        assert(d->activate(NULL) == -2008 && !locked && slot == backend);
    }
    fail_startup_write = 0;
    assert(d->activate(NULL) == -2008);
    assert(strstr(startup_record, "driver.valid rc=0"));
    assert(strstr(startup_record, "hook.end rc=-2008"));
    driver_valid = 1; slot = other;
    assert(d->activate(NULL) == -2009);
    assert(strstr(startup_record, "slot.expected rc=0"));
    assert(strstr(startup_record, "slot.observed rc=0"));
    assert(strstr(startup_record, "hook.end rc=-2009"));
    slot = backend; fail_timer = 1;
    assert(d->activate(NULL) == -2012);
    assert(strstr(startup_record, "watch.end rc=-2012"));
    fail_timer = 0;
    assert(d->activate(NULL) == 0);
    assert(strstr(startup_record, "activate.end rc=0"));
    assert(slot != backend && active_timers(1000u) == 1);
    for (i = 0; i < 40; i++) {
        assert(d->activate(NULL) == 0);
        assert(startup_position <= 2048u && startup_record[startup_position] == 0);
        assert(!strncmp(startup_record, "RHSTART1", 8));
        assert(strstr(startup_record, "activate.end rc=0"));
        fire_timers(1000u);
    }
    puts("startup logging, registration faults, errno capture and bounded failures passed");
    return 0;
}
static int test_calendar_integration(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    unsigned drops, owners, calls, draws;
    calendar_mode = 1;
    input = "/resource/calendar/background.bin\tthemes/calendar/background.bin\n";
    calendar_rc = -1;
    assert(d->activate(NULL) == 0);
    assert(!calendar_calls && !metadata_refreshes && !redraws);
    assert(active_timers(50u) == 1);

    /* Neither non-UI activation nor a busy redraw may request native work. */
    redraw_ready = 0;
    fire_timers(50u);
    assert(!calendar_calls && !metadata_refreshes && !redraws);
    redraw_ready = 1;
    fire_timers(50u);
    assert(calendar_calls == 1 && !metadata_refreshes && !redraws);
    drops = image_drops;
    fire_timers(50u);
    assert(calendar_calls == 2 && image_drops == drops && !metadata_refreshes);

    /* A completed calendar stage survives both owner and redraw retries. */
    calendar_rc = 0;
    reject_metadata = 1;
    fire_timers(50u);
    assert(calendar_calls == 3 && !metadata_refreshes && !redraws);
    fire_timers(50u);
    assert(calendar_calls == 3 && image_drops == drops && !metadata_refreshes);
    reject_metadata = 0;
    reject_redraw = 1;
    fire_timers(50u);
    assert(calendar_calls == 3 && metadata_refreshes == 1 && !redraws);
    fire_timers(50u);
    assert(calendar_calls == 3 && metadata_refreshes == 1 && !redraws);
    reject_redraw = 0;
    fire_timers(50u);
    assert(calendar_calls == 3 && metadata_refreshes == 1 && redraws == 1);
    assert(!active_timers(50u));

    /* Reactivation creates a fresh calendar stage, still deferred to UI. */
    calls = calendar_calls;
    assert(d->activate(NULL) == 0 && calendar_calls == calls);
    fire_timers(50u);
    assert(calendar_calls == calls + 1);
    fire_timers(1000u); /* Delete the superseded watcher. */

    control_config = input;
    control_signal = "resource-hook-reload-v1\tcalendar-revision-1\n";
    drops = image_drops; owners = metadata_refreshes; calls = calendar_calls;
    draws = redraws;
    fire_timers(1000u);
    assert(calendar_calls == calls + 1 && image_drops == drops);
    assert(metadata_refreshes == owners + 1 && redraws == draws + 1);
    control_signal = "resource-hook-reload-v1\tcalendar-revision-2\n";
    fire_timers(1000u);
    assert(calendar_calls == calls + 2 && image_drops == drops);
    assert(metadata_refreshes == owners + 2 && redraws == draws + 2);
    fire_timers(1000u);
    assert(calendar_calls == calls + 2); /* Repeated signal is not a revision. */

    /* Removing the mapping must consult the old view to restore stock art. */
    control_config = "# stock calendar\n";
    control_signal = "resource-hook-reload-v1\tcalendar-remove\n";
    calls = calendar_calls;
    fire_timers(1000u);
    assert(calendar_calls == calls + 1 && calendar_old_only);

    /* An unrelated theme, including a new revision, has no calendar work. */
    control_config = "/resource/icon.bin\tthemes/unrelated/icon.bin\n";
    control_signal = "resource-hook-reload-v1\tcalendar-unrelated-1\n";
    calls = calendar_calls;
    fire_timers(1000u);
    assert(calendar_calls == calls);
    control_signal = "resource-hook-reload-v1\tcalendar-unrelated-2\n";
    fire_timers(1000u);
    assert(calendar_calls == calls && calendar_queries && !locked);
    assert(allocations == frees + persistent_allocs);
    puts("calendar UI ownership, retries, revisions, removal and unrelated themes passed");
    return 0;
}

static int test_empty_startup_then_theme_reload(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    struct canopus_status_writer_v1 w;
    unsigned char status[48];
    unsigned before;

    fail_open = 1;
    open_errno = RH_ENOENT;
    assert(d->activate(NULL) == 0);
    assert(slot != backend && active_timers(1000u) == 1 && !active_timers(50u));
    assert(!image_drops && !metadata_refreshes && !redraws && !retargets);
    backend_expected = "resource/icon.bin";
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w) && u32(status + 8) == 1 && u32(status + 12) == 0);

    before = signal_opens;
    fire_timers(1000u);
    assert(signal_opens == before + 1 && !control_opens);
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tlate-theme\n";
    control_config = "/resource/\tthemes/current/\n";
    backend_expected = (RH_THEME_ROOT "current/icon.bin") + 1;
    before = image_drops;
    fire_timers(1000u);
    assert(image_drops == before + 1 && metadata_refreshes == 1 && redraws == 1);
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    assert(active_timers(1000u) == 1);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w) && u32(status + 8) == 1 && u32(status + 12) == 1);
    assert(allocations == frees + persistent_allocs && persistent_allocs == 1);
    puts("empty startup stays resident and applies a later theme reload");
    return 0;
}
static int test_startup_fallback_then_reload(const char *fault) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    struct canopus_status_writer_v1 w;
    unsigned char status[48];
    char oversized[RH_CONFIG_BYTES + 2u];
    const char *fallback;
    unsigned before;

    if (!strcmp(fault, "eacces") || !strcmp(fault, "open-io") ||
        !strcmp(fault, "unknown-errno")) {
        fail_open = 1;
        open_errno = !strcmp(fault, "eacces") ? 13 :
                     (!strcmp(fault, "open-io") ? 5 : 0);
        fallback = "config.fallback rc=-2005";
    } else if (!strcmp(fault, "scratch-oom") || !strcmp(fault, "snapshot-oom")) {
        fail_alloc_at = !strcmp(fault, "scratch-oom") ? 1u : 2u;
        fallback = "config.fallback rc=-2006";
    } else if (!strcmp(fault, "materialize-oom") || !strcmp(fault, "materialized-map-oom")) {
        input = "/resource/\tthemes/current/\n"
                QUICKAPP_KEY "\tthemes/current/icon.bin\n";
        fail_alloc_at = !strcmp(fault, "materialize-oom") ? 3u : 4u;
        fallback = "config.fallback rc=-2006";
    } else {
        fallback = "config.fallback rc=-2007";
        if (!strcmp(fault, "read-io")) fail_read = 1;
        else if (!strcmp(fault, "malformed")) input = "/resource/ themes/current/\n";
        else if (!strcmp(fault, "outside")) input = "/resource/\t/outside/\n";
        else if (!strcmp(fault, "duplicate"))
            input = "/resource/\tthemes/current/\n/resource/\tthemes/other/\n";
        else {
            assert(!strcmp(fault, "oversized"));
            memset(oversized, '#', sizeof(oversized) - 1u);
            oversized[sizeof(oversized) - 1u] = 0;
            input = oversized;
        }
    }
    assert(d->activate(NULL) == 0);
    assert(strstr(startup_record, fallback));
    assert(strstr(startup_record, "prepare.end rc=0"));
    assert(strstr(startup_record, "activate.end rc=0"));
    assert(slot != backend && active_timers(1000u) == 1 && !active_timers(50u));
    assert(!persistent_allocs && !live_bytes && allocations == frees);
    assert(closes == 1u + (fail_open ? 0u : 1u));
    assert(!image_drops && !metadata_refreshes && !redraws && !retargets);
    backend_expected = "resource/icon.bin";
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w) && u32(status + 8) == 1 && u32(status + 12) == 0);

    /* A bad Manager revision still rejects the transaction, not the module.
     * It remains retryable under the same signal once Manager repairs it. */
    fail_open = fail_read = 0;
    fail_alloc_at = 0;
    query_status("resource-hook-status-v1\tng.lst.corona\tstartup-error\n",
                 (int)strtol(strchr(fallback, '=') + 1, NULL, 10), 0, 0);
    control_signal = "resource-hook-reload-v1\tng.lst.corona\trepair-startup\n";
    control_config = "/resource/\t/outside/\n";
    before = control_opens;
    fire_timers(1000u);
    assert(control_opens == before + 1 && !persistent_allocs && !redraws);
    assert(result_position == sizeof(result_record) && strstr(result_record, "\t-2103\t0\t0\n"));
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    control_config = config;
    fire_timers(1000u);
    backend_expected = (RH_THEME_ROOT "current/icon.bin") + 1;
    assert(control_opens == before + 2 && slot(&driver, "resource/icon.bin", 2) == 7);
    assert(image_drops == 1 && metadata_refreshes == 1 && redraws == 1);
    assert(active_timers(1000u) == 1 && !active_timers(50u));
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w) && u32(status + 8) == 1 && u32(status + 12) == 1);
    assert(strstr(result_record, "\t0\t0\t0\n"));
    assert(allocations == frees + persistent_allocs && persistent_allocs == 1);
    query_status("resource-hook-status-v1\tng.lst.corona\tstartup-repaired\n", 0, 1, 0);

    control_signal = "resource-hook-reload-v1\tng.lst.corona\tinvalid-after-repair\n";
    control_config = "/resource/\t/outside/\n";
    fire_timers(1000u);
    assert(slot(&driver, "resource/icon.bin", 2) == 7 && redraws == 1);
    assert(persistent_allocs == 1 && allocations == frees + persistent_allocs && !locked);
    printf("startup fallback (%s), Manager repair and last-known-good passed\n", fault);
    return 0;
}
#if defined(RH_EXPERIMENTAL_FONT_RELOAD) && RH_EXPERIMENTAL_FONT_RELOAD
static void font_status(int32_t result, uint32_t pending, uint32_t changed) {
    struct canopus_status_writer_v1 w;
    unsigned char status[48];
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!canopus_module_descriptor.query(&w));
    assert(w.used == 48 && u32(status + 4) == 6);
    assert((int32_t)u32(status + 40) == result && u32(status + 44) == pending);
    assert(u32(status + 36) == changed);
    assert(!canopus_status_writer_init(&w, status, 47));
    assert(canopus_module_descriptor.query(&w) == -1 && !w.used);
}
static int test_experimental_font_integration(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    unsigned before;
    assert(!strcmp((const char *)d->build_id, "resource-hook-0.3.0-font-exp"));
    font_status(0, 0, 0);
    font_rc = 1;
    assert(d->activate(NULL) == 0);
    assert(!font_calls && !retargets && !redraws && active_timers(50u) == 1);
    font_status(1, 1, 0);
    fire_timers(50u);
    assert(font_calls == 1 && !redraws);
    before = font_calls;
    assert(d->activate(NULL) == 0 && font_calls == before);
    font_rc = 0; font_changes = 3;
    fire_timers(50u);
    assert(redraws == 1 && !active_timers(50u));
    assert(!strcmp(last_font_path, THEMED_FONT));
    font_status(0, 0, 3);
    fire_timers(1000u);
    assert(active_timers(1000u) == 1);

    control_config = "/resource/font/MiSans-Regular-All.ttf\tthemes/"
        "font-g2/FusionPixel.ttf\n";
    control_signal = "resource-hook-reload-v1\tfont-g2\n";
    font_rc = 1;
    fire_timers(1000u);
    font_status(1, 1, 3);
    assert_result(control_signal,"RHRS1\t6\t1\t1\t0\n");
    assert(!strcmp(last_font_path, RH_THEME_ROOT "font-g2/FusionPixel.ttf"));
    before = control_opens;
    fire_timers(1000u);
    assert(control_opens == before); /* pending keeps the mapping bank pinned */
    font_rc = -2090;
    fire_timers(50u);
    font_status(-2090, 0, 3);
    assert_result(control_signal,"RHRS1\t6\t-2090\t0\t0\n");
    assert(!active_timers(50u));

    /* A distinct signal retries a rejected transaction without re-retiring
     * images or republishing an identical mapping bank. */
    before = image_drops;
    control_signal = "resource-hook-reload-v1\tretry-font-g2\n";
    font_rc = 0; font_changes = 1;
    fire_timers(1000u);
    assert(image_drops == before);
    font_status(0, 0, 4);
    assert_result(control_signal,"RHRS1\t6\t0\t0\t1\n");
    before = font_calls;
    control_signal = "resource-hook-reload-v1\tunchanged-font-g2\n";
    fail_result_write=1;
    fire_timers(1000u);
    assert(result_position==0);
    fail_result_write=0;
    fire_timers(1000u); /* Completed result retries I/O without reloading fonts. */
    assert(font_calls == before);
    assert_result(control_signal,"RHRS1\t6\t0\t0\t0\n");

    control_config = "# restore stock\n";
    control_signal = "resource-hook-reload-v1\trestore-fonts\n";
    fire_timers(1000u);
    font_status(0, 0, 5);
    assert(!last_font_path[0]);
    assert(!retargets && !rebuilds);

    /* A nonempty active map schedules image refresh after a restart too; the
     * restart latch must not be overwritten by request_refresh(). */
    control_config = config;
    control_signal = "resource-hook-reload-v1\tfont-before-restart\n";
    font_rc = 0; font_changes = 1;
    fire_timers(1000u);
    font_status(0, 0, 6);

    slot = backend; /* UI restart loses the old callback slot. */
    assert(d->activate(NULL) == 0 && font_disabled == 1);
    font_status(-2014, 1, 6);
    fire_timers(50u);
    font_status(-2099, 0, 6);
    control_config = config;
    control_signal = "resource-hook-reload-v1\tpost-restart\n";
    fire_timers(1000u);
    font_status(-2099, 0, 6);
    assert(!retargets && !locked);
    assert(allocations == frees + persistent_allocs);
    puts("experimental font scheduling, busy retry, error status, restore and restart latch passed");
    return 0;
}
#endif
static void test_relative_config(void) {
    static const char portable[] =
        "/resource/\tthemes/current/\r\n"
        "/resource/icons/stock.bin\t@system\n";
    static const char *invalid[] = {
        "/resource/\t/data/quickapp/files/ng.lst.corona/themes/current/\n",
        "/resource/\t/data/files/ng.lst.corona/themes/current/\n",
        "/resource/\t/themes/current/\n",
        "/resource/\tfiles/themes/current/\n",
        "/resource/\tthemes-evil/current/\n",
        "/resource/\tthemes/\n",
        "/resource/\tthemes/../outside/\n",
        "/resource/\tthemes/./current/\n",
        "/resource/\tthemes//current/\n",
        "/resource/\tthemes/a\\b/\n",
        "/resource/\tthemes/a:b/\n",
        "/resource/\tthemes/a\177/\n",
        "/resource/\tthemes/current/\textra\n",
        "/resource/\t@system\n"
    };
    struct rh_rule rules[2];
    uint32_t count, i;
    char text[RH_PATH + 32];
    unsigned prefix, relative;
    assert(!rh_parse_config(portable, sizeof(portable)-1u, rules, 2, &count));
    assert(count == 2 && !rh_validate_rules(rules, count));
    assert(!strcmp(rules[0].destination, RH_THEME_ROOT "current/"));
    assert(!strcmp(rules[1].destination, RH_SYSTEM_DESTINATION));
    for (i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        int rc = rh_parse_config(invalid[i], (uint32_t)strlen(invalid[i]),
                                 rules, 2, &count);
        assert(rc < 0 || rh_validate_rules(rules, count) < 0);
    }
    prefix = (unsigned)snprintf(text, sizeof(text), "/resource/file.bin\tthemes/");
    relative = RH_PATH - 1u - (unsigned)strlen(RH_APP_FILES_ROOT);
    memset(text + prefix, 'x', relative - 7u);
    text[prefix + relative - 7u] = 0;
    assert(!rh_parse_config(text, (uint32_t)strlen(text), rules, 2, &count));
    assert(!rh_validate_rules(rules, count));
    assert(strlen(rules[0].destination) == RH_PATH - 1u);
    strcat(text, "x");
    assert(rh_parse_config(text, (uint32_t)strlen(text), rules, 2, &count) == -4);
}
static char capacity_config[RH_CONFIG_BYTES + 1u];
static void capacity_rules(unsigned count, int reverse, const char *generation) {
    unsigned i, used = 0;
    for (i = 0; i < count; i++) {
        unsigned index = reverse ? count - i - 1u : i;
        int written = snprintf(capacity_config + used, sizeof(capacity_config) - used,
            "/resource/%u.bin\tthemes/%s/%u.bin\n", index, generation, index);
        assert(written > 0 && (unsigned)written < sizeof(capacity_config) - used);
        used += (unsigned)written;
    }
    control_config = capacity_config;
}
static int test_compact_snapshots(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    struct canopus_status_writer_v1 w;
    unsigned char status[48];
    unsigned before, resident;
    uint32_t bytes;
    assert(d->activate(NULL) == 0);
    fire_timers(50u);
    assert(persistent_allocs == 1 && live_bytes < 256u);

    capacity_rules(RH_RULES, 0, "p");
    control_signal = "resource-hook-reload-v1\tcapacity-256\n";
    fire_timers(1000u);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w) && u32(status + 12) == RH_RULES);
    assert(persistent_allocs == 1 && live_bytes < 16384u);
    backend_expected = (RH_THEME_ROOT "p/255.bin") + 1;
    assert(slot(&driver, "resource/255.bin", 2) == 7);
    bytes = live_bytes;
    before = image_drops;

    capacity_rules(RH_RULES + 1u, 0, "p");
    control_signal = "resource-hook-reload-v1\tcapacity-retry\n";
    fire_timers(1000u);
    assert(strstr(result_record, "\t-2103\t"));
    assert(live_bytes == bytes && persistent_allocs == 1 && image_drops == before);
    assert(slot(&driver, "resource/255.bin", 2) == 7);
    /* Reordering the same set is a no-op, even after fixing a failed revision. */
    capacity_rules(RH_RULES, 1, "p");
    fire_timers(1000u);
    assert(live_bytes == bytes && image_drops == before);

    capacity_rules(RH_RULES, 0, "q");
    control_signal = "resource-hook-reload-v1\tcapacity-oom\n";
    fail_alloc_at = alloc_attempts + 1u;
    fire_timers(1000u);
    assert(strstr(result_record, "\t-2101\t"));
    assert(live_bytes == bytes && persistent_allocs == 1);
    fail_alloc_at = alloc_attempts + 2u;
    fire_timers(1000u);
    assert(strstr(result_record, "\t-2101\t"));
    assert(live_bytes == bytes && persistent_allocs == 1 && image_drops == before);
    fail_alloc_at = 0;
    fire_timers(1000u);
    backend_expected = (RH_THEME_ROOT "q/255.bin") + 1;
    assert(slot(&driver, "resource/255.bin", 2) == 7);
    assert(persistent_allocs == 1 && image_drops == before + 2u);

    control_config = "/resource/\tthemes/nested/\n";
    control_signal = "resource-hook-reload-v1\tnested-read\n";
    reenter_read = 1;
    fire_timers(1000u);
    assert(!reenter_read && persistent_allocs == 1);
    backend_expected = (RH_THEME_ROOT "nested/icon.bin") + 1;
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    bytes = live_bytes;

    /* A nested lifecycle request owns the old refresh. Parsing must discard
     * its candidate and retry the same revision after that refresh completes. */
    redraw_ready = 0;
    activate_during_read = 1;
    control_config = "/resource/\tthemes/canceled/\n";
    control_signal = "resource-hook-reload-v1\tnested-activation\n";
    fire_timers(1000u);
    assert(!activate_during_read && persistent_allocs == 1 && live_bytes == bytes);
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    redraw_ready = 1;
    fire_timers(50u);
    fire_timers(1000u);
    backend_expected = (RH_THEME_ROOT "canceled/icon.bin") + 1;
    assert(slot(&driver, "resource/icon.bin", 2) == 7);

    /* Publication inside the old backend callback completes its UI refresh,
     * but cannot reclaim the old snapshot until that callback returns. */
    resident = persistent_allocs;
    before = frees;
    control_config = "/resource/\tthemes/after-open/\n";
    control_signal = "resource-hook-reload-v1\tbackend-pin\n";
    nested_reload = 1;
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    assert(!nested_reload && persistent_allocs == resident && frees > before);
    backend_expected = (RH_THEME_ROOT "after-open/icon.bin") + 1;
    assert(slot(&driver, "resource/icon.bin", 2) == 7);

    control_config = "# empty\n";
    control_signal = "resource-hook-reload-v1\treclaim-empty\n";
    fire_timers(1000u);
    backend_expected = "resource/icon.bin";
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    assert(!persistent_allocs && !live_bytes && allocations == frees);
    assert(peak_bytes < 96u * 1024u && !locked);
    printf("256 rules, OOM/reentrancy, backend pins and reclamation passed (peak %u bytes)\n",
           peak_bytes);
    return 0;
}
static void quickapp_count(uint32_t count) {
    struct canopus_status_writer_v1 w;
    unsigned char status[48];
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!canopus_module_descriptor.query(&w));
    assert(u32(status + 12) == count);
}
static void quickapp_open(const char *path, const char *expected) {
    unsigned before = quickapp_lookups;
    int nested = nested_reload;
    backend_expected = expected;
    assert(slot(&driver, path, 2) == 7);
    if (!nested) assert(quickapp_lookups == before); /* No launcher work inside open. */
}
static int test_quickapp_integration(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    static const char declarations[] =
        "/resource/icon.bin\tthemes/base/icon.bin\n"
        QUICKAPP_KEY "\tthemes/quick/icon.bin\n";
    unsigned drops, owners, lookups, before;
    quickapp_mode = 1;
    quickapp_result = 0; reenter_lookup = 0; quickapp_path = QUICKAPP_ICON;
    input = declarations;
    /* Preparation stores declarations separately and projects normal rules,
     * without consulting launcher state on the non-UI startup path. */
    assert(!d->prepare(NULL) && slot == backend && !quickapp_lookups);
    quickapp_count(1);
    assert(!d->activate(NULL) && !quickapp_lookups);
    fire_timers(50u);
    assert(!quickapp_lookups && active_timers(1000u) == 1);
    quickapp_open("resource/icon.bin", (RH_THEME_ROOT "base/icon.bin") + 1);
    quickapp_open(QUICKAPP_KEY, QUICKAPP_KEY);
    quickapp_open((QUICKAPP_ICON) + 1, (QUICKAPP_ICON) + 1);

    drops = image_drops; before = control_opens;
    fire_timers(1000u); /* No reload signal: keep the unresolved declaration. */
    assert(quickapp_lookups == 1 && control_opens == before && image_drops == drops);
    quickapp_count(1);
    quickapp_result = 1;
    fire_timers(1000u); /* Installation activates the retained declaration. */
    quickapp_count(2);
    quickapp_open((QUICKAPP_ICON) + 1, (RH_THEME_ROOT "quick/icon.bin") + 1);
    assert(image_drops > drops && quickapp_owner_calls);
    owners = quickapp_owner_calls; drops = image_drops; lookups = quickapp_lookups;
    reenter_lookup = 1;
    fire_timers(1000u);
    assert(!reenter_lookup && quickapp_lookups == lookups + 1);
    assert(image_drops == drops && quickapp_owner_calls == owners);

    quickapp_path = QUICKAPP_ICON2;
    fire_timers(1000u); /* A reinstall changes the exact native BIN key. */
    quickapp_open((QUICKAPP_ICON) + 1, (QUICKAPP_ICON) + 1);
    quickapp_open((QUICKAPP_ICON2) + 1, (RH_THEME_ROOT "quick/icon.bin") + 1);
    assert(quickapp_restores && quickapp_owner_calls > owners);
    drops = image_drops; owners = quickapp_owner_calls;
    quickapp_result = -1;
    fire_timers(1000u);
    quickapp_count(2);
    quickapp_open((QUICKAPP_ICON2) + 1, (RH_THEME_ROOT "quick/icon.bin") + 1);
    quickapp_result = 1; quickapp_path = "/resource/not-a-launcher-icon.bin";
    fire_timers(1000u);
    assert(image_drops == drops && quickapp_owner_calls == owners);
    quickapp_path = QUICKAPP_ICON2;
    for (before = 1; before <= 2; before++) {
        fail_alloc_at = alloc_attempts + before;
        fire_timers(1000u);
        quickapp_count(2);
        quickapp_open((QUICKAPP_ICON2) + 1, (RH_THEME_ROOT "quick/icon.bin") + 1);
        assert(image_drops == drops && quickapp_owner_calls == owners);
    }
    fail_alloc_at = 0;

    control_config = declarations;
    control_signal = "resource-hook-reload-v1\tquickapp-lookup-error\n";
    quickapp_result = -1;
    fire_timers(1000u);
    assert(strstr(result_record, "\t-2104\t") && image_drops == drops);
    quickapp_result = -2;
    fire_timers(1000u);
    assert(strstr(result_record, "\t-2105\t") && image_drops == drops);
    quickapp_count(2);
    quickapp_open((QUICKAPP_ICON2) + 1, (RH_THEME_ROOT "quick/icon.bin") + 1);
    quickapp_result = 1;
    control_signal = "resource-hook-reload-v1\tquickapp-same-map\n";
    for (before = 1; before <= 4; before++) {
        fail_alloc_at = alloc_attempts + before;
        fire_timers(1000u);
        assert(strstr(result_record, "\t-2101\t"));
        assert(image_drops == drops && quickapp_owner_calls == owners);
        quickapp_count(2);
    }
    fail_alloc_at = 0;
    reenter_read = 1;
    fire_timers(1000u);
    assert(!reenter_read && image_drops > drops && quickapp_owner_calls > owners);
    drops = image_drops; before = control_opens; lookups = quickapp_lookups;
    fire_timers(1000u);
    assert(control_opens == before && quickapp_lookups == lookups + 1 && image_drops == drops);

    /* A failed periodic query/OOM must not leave the accepted receipt stuck
     * in an error after recovery to the same unchanged mapping. */
    quickapp_result = -1;
    fire_timers(1000u);
    assert(strstr(result_record, "\t-2104\t") && image_drops == drops);
    quickapp_result = 1;
    fire_timers(1000u);
    assert(strstr(result_record, "\t0\t0\t") && !strstr(result_record, "\t-2104\t"));
    assert(image_drops == drops && control_opens == before);
    fail_alloc_at = alloc_attempts + 1u;
    fire_timers(1000u);
    assert(strstr(result_record, "\t-2101\t") && image_drops == drops);
    fail_alloc_at = 0;
    fire_timers(1000u);
    assert(strstr(result_record, "\t0\t0\t") && !strstr(result_record, "\t-2101\t"));
    assert(image_drops == drops && control_opens == before);

    /* Expanding an icon onto an explicitly declared native file is atomic. */
    control_config = "/resource/icon.bin\tthemes/base/icon.bin\n"
        QUICKAPP_KEY "\tthemes/quick/icon.bin\n"
        QUICKAPP_ICON2 "\tthemes/conflict.bin\n";
    control_signal = "resource-hook-reload-v1\tquickapp-conflict\n";
    fire_timers(1000u);
    assert(strstr(result_record, "\t-2103\t") && image_drops == drops);
    quickapp_count(2);
    quickapp_open((QUICKAPP_ICON2) + 1, (RH_THEME_ROOT "quick/icon.bin") + 1);
    control_config = declarations;
    fire_timers(1000u); /* Correcting the same failed revision is retried. */
    assert(image_drops > drops);
    control_signal = NULL;

    owners = quickapp_restores;
    quickapp_result = 0;
    fire_timers(1000u); /* Uninstall restores stock while retaining intent. */
    quickapp_count(1);
    quickapp_open((QUICKAPP_ICON2) + 1, (QUICKAPP_ICON2) + 1);
    assert(quickapp_restores > owners);
    quickapp_result = 1;
    fire_timers(1000u);
    quickapp_count(2);
    quickapp_open((QUICKAPP_ICON2) + 1, (RH_THEME_ROOT "quick/icon.bin") + 1);

    /* An explicit @system declaration restores stock while retaining polling
     * intent. Repeating its accepted revision does not refresh image owners. */
    owners = quickapp_restores;
    control_config = "/resource/icon.bin\tthemes/base/icon.bin\n"
        QUICKAPP_KEY "\t@system\n";
    control_signal = "resource-hook-reload-v1\tquickapp-system\n";
    fire_timers(1000u);
    quickapp_count(2);
    quickapp_open((QUICKAPP_ICON2) + 1, (QUICKAPP_ICON2) + 1);
    assert(quickapp_restores > owners && strstr(result_record, "\t0\t0\t"));
    drops = image_drops; owners = quickapp_owner_calls;
    fire_timers(1000u);
    assert(image_drops == drops && quickapp_owner_calls == owners);
    control_config = declarations;
    control_signal = "resource-hook-reload-v1\tquickapp-theme-reapply\n";
    fire_timers(1000u);
    quickapp_count(2);
    quickapp_open((QUICKAPP_ICON2) + 1, (RH_THEME_ROOT "quick/icon.bin") + 1);
    assert(quickapp_owner_calls > owners && image_drops > drops);

    /* Publication while the backend owns the old bank cannot free that bank. */
    control_config = "/resource/icon.bin\tthemes/base/icon.bin\n"
        QUICKAPP_KEY "\tthemes/next/icon.bin\n";
    control_signal = "resource-hook-reload-v1\tquickapp-backend-pin\n";
    nested_reload = 1;
    quickapp_open((QUICKAPP_ICON2) + 1, (RH_THEME_ROOT "quick/icon.bin") + 1);
    assert(!nested_reload && persistent_allocs == 2);
    quickapp_open((QUICKAPP_ICON2) + 1, (RH_THEME_ROOT "next/icon.bin") + 1);

    control_config = "# remove declarations too\n";
    control_signal = "resource-hook-reload-v1\tquickapp-remove\n";
    fire_timers(1000u);
    quickapp_count(0);
    quickapp_open((QUICKAPP_ICON2) + 1, (QUICKAPP_ICON2) + 1);
    lookups = quickapp_lookups;
    fire_timers(1000u);
    assert(quickapp_lookups == lookups && !persistent_allocs && !live_bytes);
    assert(allocations == frees && !locked && active_timers(1000u) == 1);
    puts("QuickApp UI polling, install/reinstall/uninstall, @system restore, revisions, LKG, OOM and pins passed");
    return 0;
}
int main(int argc, char **argv) {
    test_relative_config();
    if (argc == 2 && !strcmp(argv[1], "--control-reload-status"))
        return test_control_reload_status();
    if (argc == 2 && !strcmp(argv[1], "--control-quickapp-status"))
        return test_control_quickapp_status();
    if (argc == 3 && !strcmp(argv[1], "--control-status"))
        return test_control_status(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--quickapp"))
        return test_quickapp_integration();
    if (argc > 1 && !strcmp(argv[1], "--calendar"))
        return test_calendar_integration();
    if (argc == 2 && !strcmp(argv[1], "--snapshots"))
        return test_compact_snapshots();
    if (argc == 2 && !strcmp(argv[1], "--startup-diagnostics"))
        return test_startup_diagnostics();
#if defined(RH_EXPERIMENTAL_FONT_RELOAD) && RH_EXPERIMENTAL_FONT_RELOAD
    if (argc == 2 && !strcmp(argv[1], "--experimental-fonts"))
        return test_experimental_font_integration();
#endif
    if (argc == 2 && !strcmp(argv[1], "--empty-startup"))
        return test_empty_startup_then_theme_reload();
    if (argc == 3 && !strcmp(argv[1], "--startup-fallback"))
        return test_startup_fallback_then_reload(argv[2]);
    assert(argc == 1);
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    struct canopus_status_writer_v1 w;
    unsigned char status[48];
    unsigned before;
#if defined(RH_TARGET_1043) && RH_TARGET_1043
#define EXPECTED_FILES_ROOT "/data/files/ng.lst.corona/"
#define FOREIGN_THEME_ROOT "/data/quickapp/files/ng.lst.corona/themes/"
#else
#define EXPECTED_FILES_ROOT "/data/quickapp/files/ng.lst.corona/"
#define FOREIGN_THEME_ROOT "/data/files/ng.lst.corona/themes/"
#endif
    assert(!strcmp(RH_APP_FILES_ROOT, EXPECTED_FILES_ROOT));
    assert(!strcmp(RH_CONFIG_PATH, EXPECTED_FILES_ROOT "mappings.tsv"));
    assert(!strcmp(RH_CONTROL_REQUEST_PATH, EXPECTED_FILES_ROOT "control.request"));
    assert(!strcmp(RH_CONTROL_RESPONSE_PATH, EXPECTED_FILES_ROOT "control.response"));
    assert(!strcmp(RH_THEME_ROOT, EXPECTED_FILES_ROOT "themes/"));
    {
        const struct rh_rule native = {
            "/resource/", EXPECTED_FILES_ROOT "themes/current/"
        };
        const struct rh_rule foreign = {
            "/resource/", FOREIGN_THEME_ROOT "current/"
        };
        assert(rh_validate_rules(&native, 1) == 0);
        assert(rh_validate_rules(&foreign, 1) == -2);
    }
    assert(registrations == 1 && closes == 1);
    assert(d->struct_size == sizeof(*d) && d->abi_major == 1 && d->abi_minor == 2);
    assert(!strcmp((const char *)d->module_id, "corona"));
    assert(!strcmp((const char *)d->module_version, "0.3.0"));
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    assert(!strcmp((const char *)d->target_id, "xiaomi-band-10-pro-3.101.043"));
#elif defined(RH_TARGET_155) && RH_TARGET_155
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
    assert(d->prepare(NULL) == 0 && slot == backend && closes == before);
    assert(strstr(startup_record, "config.fallback rc=-2005"));
    open_errno = 13;  /* EACCES degrades to a resident pass-through. */
    assert(d->prepare(NULL) == 0 && slot == backend);
    open_errno = 0;  /* Unknown config failure is also nonfatal. */
    assert(d->prepare(NULL) == 0);
    open_errno = RH_ENOENT;
    driver_valid = 0;
    assert(d->prepare(NULL) == 0);  /* Missing mappings prepare an empty snapshot. */
    assert(slot == backend && allocations == frees + persistent_allocs &&
           persistent_allocs == 0 && !image_drops && !metadata_refreshes && !redraws &&
           !retargets && !timers_created && !active_timers(50u) && !active_timers(1000u));
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w) && u32(status + 8) == 0 && u32(status + 12) == 0);
    assert(d->stop(NULL) == 0 && d->deactivate(NULL) == 0);

    driver_valid = 1;
    fail_open = 0; fail_alloc = 1;
    assert(d->prepare(NULL) == 0 && slot == backend && closes == before + 1);
    assert(strstr(startup_record, "config.fallback rc=-2006"));
    fail_alloc = 0; fail_read = 1;
    assert(d->prepare(NULL) == 0 && allocations == frees && !persistent_allocs);
    assert(strstr(startup_record, "config.fallback rc=-2007"));
    fail_read = 0; input = "/resource/\t/outside/\n";
    assert(d->prepare(NULL) == 0 && slot == backend && allocations == frees);
    assert(strstr(startup_record, "config.fallback rc=-2007"));
    input = "# empty\n";
    driver_valid = 0;
    assert(d->prepare(NULL) == 0);
    input = "";
    assert(d->prepare(NULL) == 0);
    assert(slot == backend && allocations == frees + persistent_allocs &&
           !image_drops && !metadata_refreshes && !redraws && !timers_created);
    driver_valid = 1;
    input = config;
    /* A failed re-prepare preserves the previously prepared good snapshot. */
    assert(d->prepare(NULL) == 0);
    fail_open = 1; open_errno = 13;
    assert(d->prepare(NULL) == 0);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w) && u32(status + 8) == 0 && u32(status + 12) == 1);
    assert(persistent_allocs == 1 && allocations == frees + persistent_allocs);
    /* Optional absence still explicitly prepares a zero-rule snapshot. */
    open_errno = RH_ENOENT;
    assert(d->prepare(NULL) == 0);
    assert(slot == backend && !timers_created);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w) && u32(status + 8) == 0 && u32(status + 12) == 0);
    fail_open = 0;
    input = config;
    assert(d->prepare(NULL) == 0);
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
    control_config = "/resource/\tthemes/alternate/\n";
    before = image_drops;
    fire_timers(1000u);
    backend_expected = (RH_THEME_ROOT "alternate/icon.bin") + 1;
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
    control_config = "/resource/\tthemes/alternate2/\n";
    fire_timers(1000u);
    backend_expected = (RH_THEME_ROOT "alternate2/icon.bin") + 1;
    assert(image_drops == before + 2 && slot(&driver, "resource/icon.bin", 2) == 7);

    /* Removed mappings are included in owner refresh, so the original path is
     * restored rather than leaving an image stuck on the previous theme. */
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tr3\n";
    control_config = "/other/\tthemes/other/\n";
    fire_timers(1000u);
    backend_expected = "resource/icon.bin";
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    backend_expected = (RH_THEME_ROOT "other/icon.bin") + 1;
    assert(slot(&driver, "other/icon.bin", 2) == 7);

    /* A second revision arriving during a pending refresh waits until the first
     * retirement/owner transaction completes, then is applied in order. */
    redraw_ready = 0;
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tr4\n";
    control_config = "/resource/\tthemes/pending/\n";
    fire_timers(1000u);
    assert(active_timers(50u) == 1);
    before = control_opens;
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tr5\n";
    control_config = "/resource/\tthemes/final/\n";
    fire_timers(1000u);
    assert(control_opens == before);
    redraw_ready = 1;
    fire_timers(50u);
    fire_timers(1000u);
    backend_expected = (RH_THEME_ROOT "final/icon.bin") + 1;
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    fire_timers(1000u);

    /* Empty runtime config removes the last mapping and restores the base path. */
    control_signal = "resource-hook-reload-v1\tng.lst.corona\tr6\n";
    control_config = "# no active themes\n";
    before = image_drops;
    fire_timers(1000u);
    backend_expected = "resource/icon.bin";
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    assert(image_drops == before + 1);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w) && u32(status + 12) == 0 && u32(status + 8) == 1);
    assert(d->activate(NULL) == 0);  /* a zero-rule resident snapshot can rebind */
    fire_timers(1000u);  /* discard the superseded watcher handle */

    assert(allocations == frees && persistent_allocs == 0 && !live_bytes &&
           active_timers(1000u) == 1 && !locked);
    puts("module registration, activation, config polling, atomic snapshots and targeted refresh passed");
    return 0;
}

#include "resource_hook_platform.h"
#include "resource_hook_target.h"
#include "resource_hook_quickapp.h"
#include "resource_hook_font_reload.h"
#include "resource_hook_background.h"
#include "resource_hook_calendar_runtime.h"
#include "canopus_abi.h"
#include "canopus_services.h"
#include "canopus_hook.h"
#include "manager_state.h"
#if RH_NATIVE_MANAGER
#include "manager_config_store.h"
#include "manager_ui.h"
#endif
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
static unsigned in_open_task;
static int reenter_signal_read, reenter_result_open, reenter_result_write;
static void fire_timers(uint32_t);
extern struct canopus_module_descriptor_v1 canopus_module_descriptor;
extern void rh_module_entry(const struct canopus_context_v1 *);
static int watcher_scheduled;
static char result_record[256];
static unsigned result_position, result_writes;
static int fail_result_write, fail_result_open, fail_result_after;
static char startup_record[2049];
static unsigned startup_position, startup_writes, startup_opens;
static int fail_startup_open, fail_startup_write, clobber_startup_errno;
#if defined(RH_TARGET_1043) && RH_TARGET_1043 && \
    defined(RH_P67_BLUR_DIAGNOSTICS) && RH_P67_BLUR_DIAGNOSTICS
static char blur_record[1024];
static unsigned blur_position, blur_opens, blur_writes, blur_gets;
static int blur_write_fault, blur_open_fault, blur_bad_snapshot;
static uint32_t blur_epoch, blur_root = 42u, blur_counters, blur_publications;
static uint32_t blur_core_gate = 1u, blur_pending, blur_armed = 1u, blur_cleanup_armed;
static int blur_capture_result = -7;
#endif
static unsigned image_drops, redraws, rebuilds, retargets, timers_created, timers_deleted;
static int font_retargeted;
static int fail_timer, fail_timer_start, reject_redraw, reject_metadata, unsupported;
static uint32_t fail_timer_interval;
static unsigned metadata_refreshes;
static int open_errno = 5;
static int fail_open, fail_alloc, fail_read, driver_valid = 1, image_cache_ready = 1,
           redraw_ready = 1, locked;
static int driver;
static const char *backend_expected = (RH_THEME_ROOT "current/icon.bin") + 1;
struct mock_timer {
    uint32_t interval;
    canopus_timer_callback_t callback;
    void *user_data;
    int allocated, active;
};
static struct mock_timer mock_timers[16];
static int32_t timer_create(const struct canopus_context_v1 *, uint32_t, int,
                            canopus_timer_callback_t, void *);
static int32_t timer_start(const struct canopus_context_v1 *, int32_t);
static int32_t timer_destroy(const struct canopus_context_v1 *, int32_t);
static struct canopus_time_obj time_services = {
    .timer_create = timer_create, .timer_start = timer_start, .timer_destroy = timer_destroy,
};
static int32_t hook_install(const struct canopus_context_v1 *, uintptr_t, uintptr_t,
                            uintptr_t *, canopus_hook_type_t);
static int32_t hook_uninstall(const struct canopus_context_v1 *, int32_t);
static int hook_is_hooked(const struct canopus_context_v1 *, uintptr_t);
static const struct canopus_hook_obj hook_services = {
    .install = hook_install, .uninstall = hook_uninstall, .is_hooked = hook_is_hooked,
};
static int32_t fs_open(const struct canopus_context_v1 *, const char *, uint32_t);
static int32_t fs_read(const struct canopus_context_v1 *, int32_t, void *, uint32_t);
static int32_t fs_write(const struct canopus_context_v1 *, int32_t, const void *, uint32_t);
static int32_t fs_close(const struct canopus_context_v1 *, int32_t);
#if RH_NATIVE_MANAGER
static int32_t native_data_path(const struct canopus_context_v1 *, char *, uint32_t);
static int32_t native_config_rename(const struct canopus_context_v1 *, const char *, const char *);
#endif
static int32_t module_register(const struct canopus_context_v1 *,
                               const struct canopus_module_descriptor_v1 *);
static const struct canopus_fs_obj fs_services = {
    .open = fs_open, .read = fs_read, .write = fs_write, .close = fs_close,
#if RH_NATIVE_MANAGER
    .get_data_path = native_data_path, .rename = native_config_rename,
#endif
};
static void *mem_alloc(const struct canopus_context_v1 *, uint32_t);
static void mem_free(const struct canopus_context_v1 *, void *);
static struct canopus_mem_obj mem_services = {
    .alloc = mem_alloc, .free = mem_free,
};
static unsigned hooks_installed, hooks_uninstalled;
static int fail_hook_install, fail_hook_uninstall;
static struct canopus_services_v1 services = {
    .struct_size = sizeof(struct canopus_services_v1),
    .group_version = {[CANOPUS_SVC_MODULE] = 1, [CANOPUS_SVC_FS] = 1,
                      [CANOPUS_SVC_MEM] = 1, [CANOPUS_SVC_TIME] = 1, [CANOPUS_SVC_HOOK] = 1},
    .module_register = module_register,
};
static struct canopus_context_v1 test_context = {
    .struct_size = sizeof(struct canopus_context_v1),
    .abi = (CANOPUS_ABI_MAJOR << 16) | CANOPUS_ABI_MINOR,
    .services = &services, .fs = &fs_services, .mem = &mem_services,
    .time = &time_services, .hook = &hook_services,
};
static unsigned in_ui_timer, calendar_calls, calendar_queries, calendar_old_only;
#if defined(RH_TARGET_1043) && RH_TARGET_1043 && \
    defined(RH_P67_BLUR_DIAGNOSTICS) && RH_P67_BLUR_DIAGNOSTICS
uint32_t rh_platform_p67_blur_diagnostics(char *out, uint32_t capacity) {
    int size;
    assert(in_ui_timer && !locked);
    blur_gets++;
    if (blur_bad_snapshot) return capacity;
    size = snprintf(out, capacity,
        "P67BLUR1\ncore_gate=%u\ndemand=1\npending=%u\narmed=%u\ncleanup_armed=%u\nsource=123\n"
        "root=%u\nroot_generation=1\ndisplay=456\nconfig_epoch=%u\n"
        "native_error=0\nidle_error=0\nlast_gpu=0\ncapture_result=%d\n"
        "input=789\nwidth=100\nheight=100\nstride=400\nnonzero=1\n"
        "frame52=%u\nattempts=%u\ncopies=%u\npublishes=%u\nwakes=%u\ndraws=%u\n"
        "fences=%u\ninput_allocs=%u\noutput_allocs=%u\ndestroys=%u\ncapture_calls=%u\nwake_calls=%u\n"
        "pm_stays=%u\npm_relaxes=%u\n",
        blur_core_gate, blur_pending, blur_armed, blur_cleanup_armed,
        blur_root, blur_epoch, blur_capture_result,
        blur_counters, blur_counters, blur_counters, blur_publications,
        blur_counters, blur_counters, blur_counters, blur_counters, blur_counters, blur_counters,
        blur_counters, blur_counters, blur_counters, blur_counters);
    assert(size > 0 && (uint32_t)size < capacity);
    return (uint32_t)size;
}
#endif
static int calendar_mode, calendar_rc;
static unsigned calendar_hook_mode, calendar_hook_fail, calendar_hook_calls;
static unsigned calendar_poll_calls, calendar_reenter_poll;
static int calendar_stage_rc, calendar_start_rc;
static unsigned calendar_force_pending;
static char calendar_hook_path[RH_PATH];
/* Coordinator model only; renderer/parser/bank ownership have separate tests. */
int rh_calendar_stage(struct rh_calendar_config **out) { *out = NULL; return calendar_stage_rc; }
int rh_calendar_commit(struct rh_calendar_config *config) { assert(!config); return 0; }
#if RH_NATIVE_MANAGER
int rh_calendar_stage_from(const char *path, struct rh_calendar_config **out) {
    const char *name = strrchr(path, '/');
    assert(name && !strcmp(name, "/calendar.cfg"));
    return rh_calendar_stage(out);
}
int rh_calendar_exchange(struct rh_calendar_config *next, struct rh_calendar_config **previous) {
    assert(!next); *previous = NULL; return 0;
}
#endif
void rh_calendar_discard(struct rh_calendar_config *config) { assert(!config); }
int rh_calendar_start(void) { assert(in_ui_timer && !locked); return calendar_start_rc; }
int rh_calendar_poll(void) {
    assert(in_ui_timer && !locked);
    ++calendar_poll_calls;
    if (calendar_reenter_poll) {
        unsigned before = image_drops;
        calendar_reenter_poll = 0;
        assert(canopus_module_descriptor.activate(&test_context) == 0);
        assert(image_drops == before);
    }
    return 0;
}
int rh_calendar_pending(void) { return (int)calendar_force_pending; }
const char *rh_calendar_redirect(const char *path) {
    return calendar_hook_mode && path && !strcmp(path, &RH_CALENDAR_OUTPUT_PATH[1])
        ? &RH_CALENDAR_BANK0[1] : NULL;
}
#if RH_Q66_BACKGROUND || (defined(RH_TARGET_1043) && RH_TARGET_1043)
#define BACKGROUNDS_CONFIG_PATH RH_APP_FILES_ROOT "backgrounds.cfg"
static const char *background_config; /* Missing in all legacy scenarios. */
static unsigned background_position, background_opens, background_reads, background_closes;
static unsigned background_stages, background_commits, background_polls;
static int background_stage_rc, background_commit_rc;
static char staged_background[RH_PATH], committed_background[RH_PATH];
static struct rh_backgrounds staged_backgrounds, committed_backgrounds;

int rh_platform_backgrounds_stage(const struct rh_backgrounds *bg) {
    assert(!locked && in_ui_timer && bg);
    background_stages++;
    if (background_stage_rc) return background_stage_rc; /* e.g. image_info failure */
    staged_backgrounds = *bg;
    strcpy(staged_background, bg->launcher);
    return 0;
}
int rh_platform_backgrounds_commit(const struct rh_backgrounds *bg) {
    assert(!locked && in_ui_timer && bg);
    background_commits++;
    if (background_commit_rc) return background_commit_rc;
    committed_backgrounds = *bg;
#if defined(RH_TARGET_1043) && RH_TARGET_1043 && \
    defined(RH_P67_BLUR_DIAGNOSTICS) && RH_P67_BLUR_DIAGNOSTICS
    blur_epoch++;
#endif
    strcpy(committed_background, bg->launcher);
    return 0;
}
void rh_platform_backgrounds_poll(void) {
    assert(!locked && in_ui_timer);
    background_polls++;
}

#endif
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
static int backend(void *d, const char *p, int mode) {
    assert(d == &driver && mode == 2);
    if (calendar_hook_mode) {
        ++calendar_hook_calls;
        strcpy(calendar_hook_path, p);
        return calendar_hook_fail && !strcmp(p, &RH_CALENDAR_BANK0[1]) ? -1 : 7;
    }
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
static int32_t hook_install(const struct canopus_context_v1 *ctx, uintptr_t target,
                            uintptr_t wrapper, uintptr_t *original, canopus_hook_type_t type) {
    int32_t id;
    assert(ctx == &test_context && locked && target == (uintptr_t)&slot);
    assert(type == CANOPUS_HOOK_POINTER);
    if (fail_hook_install) return CANOPUS_SVC_ERR_NOMEM;
    id = canopus_hook_install(ctx->slot, target, wrapper, original, type);
    if (id >= 0) hooks_installed++;
    return id;
}
static int32_t hook_uninstall(const struct canopus_context_v1 *ctx, int32_t id) {
    int32_t rc;
    assert(ctx == &test_context && locked);
    if (fail_hook_uninstall) return CANOPUS_SVC_ERR_IO;
    rc = canopus_hook_uninstall(ctx->slot, id);
    if (!rc) hooks_uninstalled++;
    return rc;
}
static int hook_is_hooked(const struct canopus_context_v1 *ctx, uintptr_t target) {
    assert(ctx == &test_context && locked && target == (uintptr_t)&slot);
    return canopus_hook_is_hooked(target);
}
#if RH_NATIVE_MANAGER
static char native_active_config[32], native_config_part[32];
static unsigned native_pointer_position, native_pointer_writes;
static int native_rename_fail;
static int32_t native_data_path(const struct canopus_context_v1 *ctx, char *out, uint32_t cap) {
    assert(ctx == &test_context && cap >= sizeof(RH_APP_FILES_ROOT));
    memcpy(out, RH_APP_FILES_ROOT, sizeof(RH_APP_FILES_ROOT));
    return (int32_t)(sizeof(RH_APP_FILES_ROOT) - 1u);
}
static int32_t native_config_rename(const struct canopus_context_v1 *ctx, const char *from, const char *to) {
    assert(ctx == &test_context && !locked);
    assert(!strcmp(from, RH_APP_FILES_ROOT "active-config.part"));
    assert(!strcmp(to, RH_APP_FILES_ROOT "active-config"));
    if (native_rename_fail) return CANOPUS_SVC_ERR_IO;
    memcpy(native_active_config, native_config_part, sizeof(native_active_config));
    return 0;
}
int rh_manager_ui_set_data_hooks(const struct rh_manager_data_hooks *hooks) { (void)hooks; return 0; }
int rh_manager_ui_register(const struct canopus_context_v1 *ctx) { (void)ctx; return 0; }
int rh_manager_launcher_register(const struct canopus_context_v1 *ctx) { (void)ctx; return 0; }
#endif
static int32_t fs_open(const struct canopus_context_v1 *ctx, const char *path, uint32_t mode) {
    assert(ctx == &test_context && !locked);
#if RH_NATIVE_MANAGER
    if (!strcmp(path, RH_APP_FILES_ROOT "active-config")) {
        assert(mode == CANOPUS_FS_READ);
        native_pointer_position = 0;
        return native_active_config[0] ? 19 : -RH_ENOENT;
    }
    if (!strcmp(path, RH_APP_FILES_ROOT "active-config.part")) {
        assert(mode == CANOPUS_FS_WRITE);
        memset(native_config_part, 0, sizeof(native_config_part));
        native_pointer_position = 0;
        return 18;
    }
    if (!strncmp(path, RH_APP_FILES_ROOT "active/", sizeof(RH_APP_FILES_ROOT "active/") - 1u)) {
        const char *name = strrchr(path, '/');
        assert(name);
        if (!strcmp(name, "/mappings.tsv")) path = RH_CONFIG_PATH;
        else if (!strcmp(name, "/backgrounds.cfg")) path = RH_APP_FILES_ROOT "backgrounds.cfg";
        else assert(!"unexpected candidate file");
    }
#endif
#if defined(RH_TARGET_1043) && RH_TARGET_1043 && \
    defined(RH_P67_BLUR_DIAGNOSTICS) && RH_P67_BLUR_DIAGNOSTICS
    if (!strcmp(path, "/data/offlinelog/resource-hook-p67-blur.log")) {
        assert(in_ui_timer && mode == CANOPUS_FS_WRITE);
        blur_opens++;
        if (blur_open_fault) return -1;
        blur_position = 0;
        memset(blur_record, 0, sizeof(blur_record));
        return 17;
    }
#endif
    if (!strcmp(path, RH_STARTUP_LOG_PATH)) {
        assert(mode == CANOPUS_FS_WRITE);
        startup_opens++;
        if (clobber_startup_errno && config_opens) open_errno = 24;
        if (fail_startup_open) return -1;
        startup_position = 0;
        memset(startup_record, 0, sizeof(startup_record));
        return 15;
    }
    if (!strcmp(path, RH_CONTROL_RESPONSE_PATH)) {
        assert(mode == CANOPUS_FS_WRITE);
        if (fail_result_open) return -1;
        if (reenter_result_open) {
            unsigned signals = signal_opens, draws = redraws;
            reenter_result_open = 0;
            fire_timers(50u); fire_timers(1000u);
            assert(signal_opens == signals && redraws == draws);
        }
        result_position=0; return 14;
    }
    assert(mode == CANOPUS_FS_READ);
#if RH_Q66_BACKGROUND || (defined(RH_TARGET_1043) && RH_TARGET_1043)
    if (!strcmp(path, BACKGROUNDS_CONFIG_PATH)) {
        background_opens++;
        background_position = 0;
        if (!background_config) return -RH_ENOENT;
        return 16;
    }
#endif
    if (!strcmp(path, RH_CONTROL_REQUEST_PATH)) {
        signal_opens++;
        signal_position = 0;
        if (!control_signal) return -RH_ENOENT;
        return 12;
    }
    if (!strcmp(path, RH_CONFIG_PATH)) {
        if (watcher_scheduled) {
            control_opens++;
            control_position = 0;
            if (!control_config) return -RH_ENOENT;
            return 13;
        }
        config_opens++;
        position = 0;
        if (fail_open) return open_errno ? -open_errno : CANOPUS_SVC_ERR_IO;
        return 11;
    }
    assert(!"unexpected file path");
    return -1;
}
static int32_t fs_read(const struct canopus_context_v1 *ctx, int32_t fd, void *out, uint32_t size) {
    assert(ctx == &test_context);
    const char *data;
    unsigned *cursor;
    unsigned remaining;
    assert(!locked);
    if (fail_read) return -1;
#if RH_NATIVE_MANAGER
    if (fd == 19) {
        unsigned left = (unsigned)strlen(native_active_config) - native_pointer_position;
        if (size > left) size = left;
        if (size > 3u) size = 3u;
        memcpy(out, native_active_config + native_pointer_position, size);
        native_pointer_position += size;
        return (int32_t)size;
    }
#endif
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
        assert(canopus_module_descriptor.activate(&test_context) == 0);
    }
    if (fd == 11) { data = input; cursor = &position; }
    else if (fd == 12) { data = control_signal; cursor = &signal_position; }
#if RH_Q66_BACKGROUND || (defined(RH_TARGET_1043) && RH_TARGET_1043)
    else if (fd == 16) {
        assert(background_config);
        background_reads++;
        data = background_config; cursor = &background_position;
    }
#endif
    else { assert(fd == 13); data = control_config; cursor = &control_position; }
    remaining = (unsigned)strlen(data) - *cursor;
    if (size > 3u) size = 3u;
    if (size > remaining) size = remaining;
    memcpy(out, data + *cursor, size);
    *cursor += size;
    return (int)size;
}
static int32_t fs_write(const struct canopus_context_v1 *ctx, int32_t fd, const void *data, uint32_t size) {
    assert(ctx == &test_context);
#if RH_NATIVE_MANAGER
    if (fd == 18) {
        if (size > 3u) size = 3u;
        assert(native_pointer_position + size < sizeof(native_config_part));
        memcpy(native_config_part + native_pointer_position, data, size);
        native_pointer_position += size;
        ++native_pointer_writes;
        return (int32_t)size;
    }
#endif
#if defined(RH_TARGET_1043) && RH_TARGET_1043 && \
    defined(RH_P67_BLUR_DIAGNOSTICS) && RH_P67_BLUR_DIAGNOSTICS
    if (fd == 17) {
        assert(in_ui_timer && !locked);
        blur_writes++;
        if (blur_write_fault == 1 && blur_position >= 13u) return -1;
        if (blur_write_fault == 2) return 0;
        if (blur_write_fault == 3) return (int)size + 1;
        if (size > 13u) size = 13u;
        assert(blur_position + size < sizeof(blur_record));
        memcpy(blur_record + blur_position, data, size);
        blur_position += size;
        return (int)size;
    }
#endif
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
    assert(!"unexpected write handle");
    return CANOPUS_SVC_ERR_STATE;
}
static int32_t fs_close(const struct canopus_context_v1 *ctx, int32_t fd) {
    assert(ctx == &test_context && !locked);
    if(fd==14 || fd==15) return 0;
#if RH_NATIVE_MANAGER
    if (fd == 18 || fd == 19) return 0;
#endif
#if defined(RH_TARGET_1043) && RH_TARGET_1043 && \
    defined(RH_P67_BLUR_DIAGNOSTICS) && RH_P67_BLUR_DIAGNOSTICS
    if (fd == 17) { assert(in_ui_timer); return 0; }
#endif
#if RH_Q66_BACKGROUND || (defined(RH_TARGET_1043) && RH_TARGET_1043)
    if (fd == 16) { background_closes++; return 0; }
#endif
    assert(fd == 11 || fd == 12 || fd == 13); closes++;
    return 0;
}
static int32_t module_register(const struct canopus_context_v1 *ctx,
                               const struct canopus_module_descriptor_v1 *descriptor) {
    const char *fault = getenv("RH_TEST_REGISTRATION");
    assert(ctx == &test_context && descriptor == &canopus_module_descriptor && !locked);
    assert(!strcmp((const char *)descriptor->module_id, "corona"));
    if (fault) {
        assert(!strcmp(fault, "service-fail"));
        return CANOPUS_SVC_ERR_IO;
    }
    registrations++;
    return 0;
}
static void *mem_alloc(const struct canopus_context_v1 *ctx, uint32_t size) {
    void *pointer;
    assert(ctx == &test_context && !in_open_task);
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
static void mem_free(const struct canopus_context_v1 *ctx, void *p) {
    unsigned i;
    assert(ctx == &test_context && !in_open_task);
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
static int32_t timer_create(const struct canopus_context_v1 *ctx, uint32_t interval,
                            int periodic, canopus_timer_callback_t cb, void *user_data) {
    unsigned i;
    assert(ctx == &test_context && !locked && interval && periodic == 1 && cb);
    if (fail_timer || interval == fail_timer_interval) return CANOPUS_SVC_ERR_NOMEM;
    for (i = 0; i < sizeof(mock_timers) / sizeof(mock_timers[0]); i++) {
        if (!mock_timers[i].allocated) {
            mock_timers[i].interval = interval;
            mock_timers[i].callback = cb;
            mock_timers[i].user_data = user_data;
            mock_timers[i].allocated = 1;
            timers_created++;
            return (int32_t)i;
        }
    }
    return CANOPUS_SVC_ERR_NOMEM;
}
static int32_t timer_start(const struct canopus_context_v1 *ctx, int32_t id) {
    assert(ctx == &test_context && !locked && id >= 0 && id < 16);
    assert(mock_timers[id].allocated && !mock_timers[id].active);
    if (fail_timer_start) return CANOPUS_SVC_ERR_UNSUPPORTED;
    mock_timers[id].active = 1;
    if (mock_timers[id].interval == 1000u) watcher_scheduled = 1;
    return 0;
}
static int32_t timer_destroy(const struct canopus_context_v1 *ctx, int32_t id) {
    assert(ctx == &test_context && !locked && id >= 0 && id < 16);
    assert(mock_timers[id].allocated);
    if (mock_timers[id].interval == 1000u) watcher_scheduled = 0;
    memset(&mock_timers[id], 0, sizeof(mock_timers[id]));
    timers_deleted++;
    return 0;
}
static void fire_timers(uint32_t interval) {
    unsigned i;
    for (i = 0; i < sizeof(mock_timers) / sizeof(mock_timers[0]); i++) {
        struct mock_timer *timer = &mock_timers[i];
        if (timer->active && timer->interval == interval) {
            unsigned previous_ui_timer = in_ui_timer, previous_open_task = in_open_task;
            in_ui_timer = 1;
            in_open_task = 0;
            timer->callback(&test_context, (int32_t)i, timer->user_data);
            in_open_task = previous_open_task;
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
    assert(d->activate(&test_context) == 0);
    fail_open = 0;
    font_rc = -2090; /* A font-refresh rejection is not a config error. */
    fire_timers(50u);
    timers = timers_created; draws = redraws;
    query_status("resource-hook-status-v1\tng.lst.corona\tfirst._-0\n", 0, count, 0);
    font_rc = 0;
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
#if RH_Q66_BACKGROUND || (defined(RH_TARGET_1043) && RH_TARGET_1043)
static void assert_background_map_unchanged(const struct temp_allocation *original) {
    assert(original && original->live && persistent_allocs == 1u);
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
}
static int test_launcher_background(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    const struct temp_allocation *original = NULL;
    unsigned i, opens, reads, stages, commits, polls, writes;
    unsigned drops, metadata, fonts, draws;
    background_config = "version=1\nmode=image\nimage=themes/current/start.bin\n";
    control_config = config;
    assert(d->activate(&test_context) == 0);
    assert(active_timers(1000u) == 1u && !background_opens && !background_commits);
    fire_timers(50u);
    fire_timers(1000u); /* Startup config is applied by the existing UI watcher. */
    assert(background_opens == 1u && background_closes == 1u && background_reads);
    assert(background_stages == 1u && background_commits == 1u && background_polls == 1u);
    assert(!strcmp(committed_background, RH_THEME_ROOT "current/start.bin"));
    for (i = 0; i < allocations; i++)
        if (temp_allocations[i].live) { assert(!original); original = &temp_allocations[i]; }
    assert_background_map_unchanged(original);
    drops = image_drops; metadata = metadata_refreshes; fonts = font_calls; draws = redraws;
    opens = background_opens; reads = background_reads;
    query_status("resource-hook-status-v1\tng.lst.corona\tbackground-startup\n", 0, 1, 0);
    assert(background_opens == opens && background_reads == reads);

    /* Same mapping, different background: no map publication or generic refresh. */
    background_config = "version=1\nmode=image\nimage=themes/current/next.bin\n";
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tbackground-image\n";
    fire_timers(1000u);
    assert_reload_status(0, 0, 0, 0, 1);
    assert(background_opens == opens + 1u && background_stages == 2u && background_commits == 2u);
    assert(!strcmp(committed_background, RH_THEME_ROOT "current/next.bin"));
    assert_background_map_unchanged(original);
    opens = background_opens; reads = background_reads; polls = background_polls;
    stages = background_stages; commits = background_commits; writes = result_writes;
    background_config = "malformed but not a new revision\n";
    for (i = 0; i < 3u; i++) fire_timers(1000u);
    assert_reload_status(0, 0, 0, 0, 1);
    assert(background_opens == opens && background_reads == reads && result_writes == writes);
    assert(background_stages == stages && background_commits == commits && background_polls == polls + 3u);
    assert(!strcmp(committed_background, RH_THEME_ROOT "current/next.bin"));

    background_config = "version=1\nmode=system\n";
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tbackground-system\n";
    fire_timers(1000u);
    assert_reload_status(0, 0, 0, 0, 1);
    assert(!committed_background[0] && background_stages == stages + 1u && background_commits == commits + 1u);
    assert_background_map_unchanged(original);

    background_config = "version=1\nmode=image\nimage=themes/current/next.bin\n";
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tbackground-restore\n";
    fire_timers(1000u);
    assert_reload_status(0, 0, 0, 0, 1);
    assert(!strcmp(committed_background, RH_THEME_ROOT "current/next.bin"));
    stages = background_stages; commits = background_commits;

    /* Reject even a changed candidate map before publishing it or the background. */
    control_config = "/resource/\tthemes/other/\n";
    background_config = "version=1\nmode=image\nimage=themes/current/../bad.bin\n";
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tbackground-malformed\n";
    fire_timers(1000u);
    assert_reload_status(-2142, 0, 0, -2142, 1);
    assert(background_stages == stages && background_commits == commits);
    assert(!strcmp(committed_background, RH_THEME_ROOT "current/next.bin"));
    assert_background_map_unchanged(original);
    opens = background_opens; reads = background_reads;
    query_status("resource-hook-status-v1\tng.lst.corona\tbackground-invalid-status\n", -2142, 1, 0);
    assert(background_opens == opens && background_reads == reads);

    /* The adapter reports failed image_info/decode as a negative stage status. */
    background_config = "version=1\nmode=image\nimage=themes/current/failed.bin\n";
    background_stage_rc = -2143;
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tbackground-stage-failed\n";
    fire_timers(1000u);
    assert_reload_status(-2143, 0, 0, -2143, 1);
    assert(background_stages == stages + 1u && background_commits == commits);
    assert(!strcmp(committed_background, RH_THEME_ROOT "current/next.bin"));
    assert_background_map_unchanged(original);
    opens = background_opens; reads = background_reads;
    query_status("resource-hook-status-v1\tng.lst.corona\tbackground-stage-status\n", -2143, 1, 0);
    assert(background_opens == opens && background_reads == reads);

    /* Test multi-background keys (launcher, control_center, notifications). */
    background_stage_rc = 0;
    background_config =
        "version=1\n"
        "launcher=themes/current/l.bin\n"
        "control_center=themes/current/c.bin\n"
        "notifications=themes/current/n.bin\n"
        "widgets=themes/current/w.bin\n";
    control_config = config;
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tmulti-bg\n";
    fire_timers(1000u);
    assert_reload_status(0, 0, 0, 0, 1);
    assert(!strcmp(committed_backgrounds.launcher, RH_THEME_ROOT "current/l.bin"));
    assert(!strcmp(committed_backgrounds.control_center, RH_THEME_ROOT "current/c.bin"));
    assert(!strcmp(committed_backgrounds.notifications, RH_THEME_ROOT "current/n.bin"));
    assert(!strcmp(committed_backgrounds.widgets, RH_THEME_ROOT "current/w.bin"));
    assert_background_map_unchanged(original);
    commits = background_commits;

    /* Widget-only configs clear the other three surfaces; system clears widgets. */
    background_config = "version=1\nwidgets=blur\n";
    control_signal = "resource-hook-reload-v2\tng.lst.corona\twidget-only\n";
    fire_timers(1000u);
    assert_reload_status(0, 0, 0, 0, 1);
    assert(!committed_backgrounds.launcher[0] && !committed_backgrounds.control_center[0] &&
           !committed_backgrounds.notifications[0]);
    assert(rh_background_is_blur(committed_backgrounds.widgets));
    background_config = "version=1\nwidgets=system\n";
    control_signal = "resource-hook-reload-v2\tng.lst.corona\twidget-system\n";
    fire_timers(1000u);
    assert_reload_status(0, 0, 0, 0, 1);
    assert(!committed_backgrounds.widgets[0]);
    commits = background_commits;

    /* A missing legacy cfg is ENOENT/system, not a parser or stage failure. */
    background_stage_rc = 0;
    background_config = NULL;
    control_config = config;
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tbackground-missing\n";
    stages = background_stages;
    fire_timers(1000u);
    assert_reload_status(0, 0, 0, 0, 1);
    assert(!committed_background[0] && background_stages == stages && background_commits == commits + 1u);
    assert(!committed_backgrounds.widgets[0]);
    assert_background_map_unchanged(original);
    assert(image_drops == drops && metadata_refreshes == metadata && font_calls == fonts && redraws == draws);
    assert(background_closes + 1u == background_opens); /* Only the missing open has no fd. */
    assert(active_timers(1000u) == 1u && !active_timers(50u));
    puts("background transaction: startup, four targets, same-map reload, failures and unchanged ticks passed");
    return 0;
}
#endif
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
    const int pending_result = 1;
    assert(d->activate(&test_context) == 0);
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
    assert_result(longest, "RHRS1\t6\t0\t0\t0\n");
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
    font_rc = -2090;
    redraw_ready = 1; fire_timers(50u);
    assert_reload_status(-2090, 0, 0, 0, 2); /* Font failure is still running. */
    font_rc = 0;
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
    assert_result(control_signal, "RHRS1\t6\t0\t0\t0\n");
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
    assert(d->activate(&test_context) == 0);
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
    if (fault) {
        assert(!strcmp(fault, "service-fail"));
        assert(!registrations && !closes);
        assert(strstr(startup_record, "register.end rc=-5 errno=5"));
        assert(strstr(startup_record, "ctor.end rc=-5 errno=5"));
        return 0;
    }
    assert(registrations == 1 && !closes);
    assert(strstr(startup_record, "register.end rc=0 errno=0"));
    assert(strstr(startup_record, "ctor.end rc=0"));
    assert(startup_writes > startup_opens); /* Logger completes short writes. */

    /* Diagnostic I/O must not replace the negative error from config.open. */
    fail_open = 1; open_errno = 13; clobber_startup_errno = 1;
    driver_valid = 0; /* Config failure is soft; the driver check still fails. */
    assert(d->activate(&test_context) == -2008 && slot == backend);
    assert(strstr(startup_record, "config.open rc=-13 errno=13"));
    assert(strstr(startup_record, "config.fallback rc=-2005 errno=13"));
    assert(strstr(startup_record, "prepare.end rc=0"));
    assert(strstr(startup_record, "activate.end rc=-2008"));
    clobber_startup_errno = 0;

    /* Log open/write failures are best-effort, never a new lifecycle failure. */
    fail_startup_open = 1; open_errno = RH_ENOENT;
    assert(d->prepare(&test_context) == 0);
    before = startup_writes;
    driver_valid = 0;
    assert(d->activate(&test_context) == -2008 && startup_writes == before);
    fail_startup_open = 0;
    for (i = 1; i <= 3; i++) {
        fail_startup_write = (int)i;
        assert(d->activate(&test_context) == -2008 && !locked && slot == backend);
    }
    fail_startup_write = 0;
    assert(d->activate(&test_context) == -2008);
    assert(strstr(startup_record, "driver.valid rc=0"));
    assert(strstr(startup_record, "hook.end rc=-2008"));
    driver_valid = 1; slot = other;
    assert(d->activate(&test_context) == -2009);
    assert(strstr(startup_record, "slot.expected rc=0"));
    assert(strstr(startup_record, "slot.observed rc=0"));
    assert(strstr(startup_record, "hook.end rc=-2009"));
    slot = backend; fail_timer = 1;
    assert(d->activate(&test_context) == -2012);
    assert(strstr(startup_record, "watch.end rc=-2012"));
    fail_timer = 0;
    assert(d->activate(&test_context) == 0);
    assert(strstr(startup_record, "activate.end rc=0"));
    assert(slot != backend && active_timers(1000u) == 1);
    for (i = 0; i < 40; i++) {
        assert(d->activate(&test_context) == 0);
        assert(startup_position <= 2048u && startup_record[startup_position] == 0);
        assert(!strncmp(startup_record, "RHSTART1", 8));
        assert(strstr(startup_record, "activate.end rc=0"));
        fire_timers(1000u);
    }
    puts("startup logging, service registration faults, error capture and bounded failures passed");
    return 0;
}
static int test_calendar_initial_invalid(void) {
    calendar_start_rc = -2153;
    calendar_force_pending = 1; /* Real engine has no active configuration yet. */
    assert(canopus_module_descriptor.activate(&test_context) == 0);
    fire_timers(1000u);
    assert(metadata_refreshes && redraws && !calendar_calls);
    query_status("resource-hook-status-v1\tng.lst.corona\tcalendar-initial-invalid\n", -2153, 1, 0);
    calendar_start_rc = 0; calendar_force_pending = 0;
    control_signal = NULL; fire_timers(1000u);
    assert(calendar_poll_calls); /* Repair permits autonomous startup without a revision. */
    query_status("resource-hook-status-v1\tng.lst.corona\tcalendar-initial-repaired\n", 0, 1, 0);
    puts("invalid initial calendar config preserves stock fallback and unrelated redraws");
    return 0;
}
static int test_calendar_coordinator_guards(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    unsigned polls;
    assert(d->activate(&test_context) == 0);
    fire_timers(50u); fire_timers(1000u);
    polls = calendar_poll_calls;
    calendar_stage_rc = -2153;
    control_config = "/resource/a.bin\tthemes/current/a.bin\n/resource/b.bin\tthemes/current/b.bin\n";
    control_signal = "resource-hook-reload-v1\tcalendar-invalid-replacement\n";
    fire_timers(1000u);
    assert(calendar_poll_calls == polls + 1); /* Rejected CFG must not freeze old calendar. */
    query_status("resource-hook-status-v1\tng.lst.corona\tcalendar-invalid-status\n", -2153, 1, 0);
    polls = calendar_poll_calls;
    control_signal = NULL; fire_timers(1000u);
    assert(calendar_poll_calls == polls + 1);
    calendar_reenter_poll = 1; fire_timers(1000u);
    assert(!calendar_reenter_poll); /* Nested activate must not retire caches inside poll. */
    fire_timers(50u);
    calendar_stage_rc = 0;
    control_config = config;
    control_signal = "resource-hook-reload-v1\tcalendar-recover\n";
    fire_timers(1000u);
#if RH_Q66_BACKGROUND || (defined(RH_TARGET_1043) && RH_TARGET_1043)
    background_commit_rc = -2145;
    control_config = "/resource/a.bin\tthemes/current/a.bin\n/resource/b.bin\tthemes/current/b.bin\n";
    control_signal = "resource-hook-reload-v1\tcalendar-failed-background-commit\n";
    fire_timers(1000u);
    query_status("resource-hook-status-v1\tng.lst.corona\tcalendar-commit-status\n", -2145, 1, 0);
    background_commit_rc = 0;
#endif
    puts("calendar coordinator keeps prior date polling, reserves reentrant activation and rejects partial commits");
    return 0;
}
static int test_calendar_alpha_hook(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    calendar_hook_mode = 1;
    assert(d->activate(&test_context) == 0);
    assert(slot(&driver, &RH_CALENDAR_OUTPUT_PATH[1], 2) == 7);
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    /* Even a stub offering a legacy bank must not redirect P67 POSIX reads. */
    assert(calendar_hook_calls == 1 && !strcmp(calendar_hook_path, &RH_CALENDAR_OUTPUT_PATH[1]));
#else
    assert(calendar_hook_calls == 1 && !strcmp(calendar_hook_path, &RH_CALENDAR_BANK0[1]));
#endif
    calendar_hook_fail = 1;
    assert(slot(&driver, &RH_CALENDAR_OUTPUT_PATH[1], 2) == 7);
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    assert(calendar_hook_calls == 2 && !strcmp(calendar_hook_path, &RH_CALENDAR_OUTPUT_PATH[1]));
#else
    assert(calendar_hook_calls == 3 && !strcmp(calendar_hook_path, &RH_CALENDAR_OUTPUT_PATH[1]));
#endif
    fire_timers(50u);
    control_config = RH_CALENDAR_OUTPUT_PATH "\tthemes/current/final.bin\n";
    control_signal = "resource-hook-reload-v1\tcalendar-static-override\n";
    fire_timers(1000u);
    calendar_hook_fail = 0;
    assert(slot(&driver, &RH_CALENDAR_OUTPUT_PATH[1], 2) == 7);
    assert(!strcmp(calendar_hook_path, &(RH_THEME_ROOT "current/final.bin")[1]));
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    puts("P67 skips calendar file-bank redirects and preserves ordinary explicit mappings");
#else
    puts("calendar read-only hook, incomplete-bank fallback and explicit final-icon override passed");
#endif
    return 0;
}
static int test_calendar_integration(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    unsigned drops, owners, calls, draws;
    calendar_mode = 1;
    input = "/resource/calendar/background.bin\tthemes/calendar/background.bin\n";
    calendar_rc = -1;
    assert(d->activate(&test_context) == 0);
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
    assert(d->activate(&test_context) == 0 && calendar_calls == calls);
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
    assert(d->activate(&test_context) == 0);
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
        !strcmp(fault, "unknown-error")) {
        fail_open = 1;
        open_errno = !strcmp(fault, "eacces") ? 13 :
                     (!strcmp(fault, "open-io") ? 5 : 123);
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
    assert(d->activate(&test_context) == 0);
    assert(strstr(startup_record, fallback));
    assert(strstr(startup_record, "prepare.end rc=0"));
    assert(strstr(startup_record, "activate.end rc=0"));
    assert(slot != backend && active_timers(1000u) == 1 && !active_timers(50u));
    assert(!persistent_allocs && !live_bytes && allocations == frees);
    assert(closes == (fail_open ? 0u : 1u));
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
static int test_font_integration(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    unsigned before;
    assert(!strcmp((const char *)d->build_id, "resource-hook-0.3.0"));
    font_status(0, 0, 0);
    font_rc = 1;
    assert(d->activate(&test_context) == 0);
    assert(!font_calls && !retargets && !redraws && active_timers(50u) == 1);
    font_status(1, 1, 0);
    fire_timers(50u);
    assert(font_calls == 1 && !redraws);
    before = font_calls;
    assert(d->activate(&test_context) == 0 && font_calls == before);
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
    assert(d->activate(&test_context) == 0 && font_disabled == 1);
    font_status(-2014, 1, 6);
    fire_timers(50u);
    font_status(-2099, 0, 6);
    control_config = config;
    control_signal = "resource-hook-reload-v1\tpost-restart\n";
    fire_timers(1000u);
    font_status(-2099, 0, 6);
    assert(!retargets && !locked);
    assert(allocations == frees + persistent_allocs);
    puts("font scheduling, busy retry, error status, restore and restart latch passed");
    return 0;
}
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
    assert(d->activate(&test_context) == 0);
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
    in_open_task = 1;
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    in_open_task = 0;
    assert(!nested_reload && persistent_allocs == resident + 1u);
    /* The hook leaves the dead bank queued; a valid owner callback reclaims it. */
    fire_timers(1000u);
    assert(persistent_allocs == resident && frees > before);
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
    in_open_task = 1;
    assert(slot(&driver, path, 2) == 7);
    in_open_task = 0;
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
    assert(!d->prepare(&test_context) && slot == backend && !quickapp_lookups);
    quickapp_count(1);
    assert(!d->activate(&test_context) && !quickapp_lookups);
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
    assert(!nested_reload && persistent_allocs == 3);
    fire_timers(1000u);
    assert(persistent_allocs == 2);
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
#if defined(RH_TARGET_1043) && RH_TARGET_1043 && \
    defined(RH_P67_BLUR_DIAGNOSTICS) && RH_P67_BLUR_DIAGNOSTICS
static int test_blur_diagnostic_writer(void) {
    unsigned opens, gets, i;
    char saved_startup[sizeof(startup_record)];
    background_config = "version=1\nmode=image\nimage=themes/current/start.bin\n";
    control_config = config;
    assert(canopus_module_descriptor.activate(&test_context) == 0);
    memcpy(saved_startup, startup_record, sizeof(saved_startup));
    assert(!blur_opens && !blur_gets);
    fire_timers(50u);
    assert(!blur_opens && !blur_gets);
    fire_timers(1000u);
    assert(blur_opens == 1u && blur_gets == 1u && blur_epoch == 1u);
    assert(strstr(blur_record,"P67BLUR1\n") == blur_record);
    assert(strstr(blur_record,"config_epoch=1\n")); /* After accepted commit. */
    assert(strstr(blur_record,"refresh_pending=0\ncache_dropped=1\nfonts_done=1\n"));
    assert(strstr(blur_record,"calendar_done=1\nimages_done=1\nfont_result=0\n"));
    assert(strstr(blur_record,"calendar_error=0\nconfig_error=0\n"));
    assert(strstr(blur_record,"background_error=0\nbackground_initialized=1\n"));
    assert(blur_writes > blur_opens); /* Completes partial writes. */
    assert(!memcmp(saved_startup,startup_record,sizeof(saved_startup)));
    opens = blur_opens;
    for (i = 1; i <= 12u; i++) {
        blur_counters = i;
        blur_publications = i;
        fire_timers(1000u);
    }
    assert(blur_opens == opens); /* Every monotonic counter excluded from key. */
    assert(strstr(blur_record,"frame52=0\n"));
    blur_capture_result = -8;
    fire_timers(1000u);
    assert(blur_opens == opens + 1u && strstr(blur_record,"frame52=12\n"));
    opens = blur_opens;
    for (i = 0; i < 20u; i++) fire_timers(1000u);
    assert(blur_opens == opens); /* Persistent failure written once. */
    for (i = 0; i < 20u; i++) {
        blur_capture_result = -20 - (int)i;
        fire_timers(1000u);
    }
    assert(blur_opens == 8u); /* Alternating errors bounded per config epoch. */
    opens = blur_opens;
    blur_root++;
    fire_timers(1000u);
    assert(blur_opens == opens + 1u && strstr(blur_record,"root=43\n"));
    blur_capture_result = 0;
    blur_publications++;
    fire_timers(1000u);
    assert(blur_opens == opens + 2u && strstr(blur_record,"capture_result=0\n"));
    /* A real accepted same-map reload resets the budget and observes new epoch.
     * Diagnostic failure does not contaminate its RHRS2 result. */
    blur_write_fault = 1;
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tblur-log-fault\n";
    fire_timers(1000u);
    assert(blur_epoch == 2u);
    assert_reload_status(0, 0, 0, 0, 1);
    assert(blur_position == 13u);
    opens = blur_opens;
    gets = blur_gets;
    fire_timers(50u);
    assert(blur_opens == opens && blur_gets == gets);
    blur_write_fault = 0;
    fire_timers(1000u);
    assert(blur_opens == opens + 1u && strstr(blur_record,"config_epoch=2\n"));
    for (i = 2; i <= 3u; i++) {
        blur_epoch++;
        blur_write_fault = (int)i;
        fire_timers(1000u);
        opens = blur_opens;
        blur_write_fault = 0;
        fire_timers(1000u);
        assert(blur_opens == opens + 1u && blur_position > 13u);
    }
    blur_epoch++;
    blur_open_fault = 1;
    fire_timers(1000u);
    opens = blur_opens;
    blur_open_fault = 0;
    fire_timers(1000u);
    assert(blur_opens == opens + 1u);
    /* O_TRUNC failure must invalidate the old dedup key, even if the next
     * observation returns to the formerly logged state. */
    blur_capture_result = -99;
    blur_write_fault = 1;
    fire_timers(1000u);
    assert(blur_position == 13u);
    opens = blur_opens;
    blur_write_fault = 0;
    blur_capture_result = 0;
    fire_timers(1000u);
    assert(blur_opens == opens + 1u && strstr(blur_record,"capture_result=0\n"));
    blur_bad_snapshot = 1;
    blur_root++;
    opens = blur_opens;
    fire_timers(1000u);
    assert(blur_opens == opens);
    blur_bad_snapshot = 0;
    fire_timers(1000u);
    assert(blur_opens == opens + 1u && strstr(blur_record,"root=44\n"));
    assert(active_timers(1000u) == 1u && !active_timers(50u));
    assert_reload_status(0, 0, 0, 0, 1);
    assert(!memcmp(saved_startup,startup_record,sizeof(saved_startup)));
    /* Model a slow ACK one retirement stage at a time. The same P67 snapshot
     * names the blocker; no diagnostic writes run in the fast refresh timer. */
    control_config = "/resource/\tthemes/next/\n";
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tblur-slow-ack\n";
    calendar_mode = 1;
    calendar_rc = -1;
    font_rc = 1;
    reject_metadata = 1;
    fire_timers(1000u);
    assert(strstr(blur_record,"refresh_pending=1\ncache_dropped=1\nfonts_done=0\n"));
    assert(strstr(blur_record,"calendar_done=0\nimages_done=0\nfont_result=1\n"));
    opens = blur_opens;
    gets = blur_gets;
    for (i = 0; i < 20u; i++) fire_timers(50u);
    assert(blur_opens == opens && blur_gets == gets);
    font_rc = -1;
    fire_timers(1000u);
    assert(strstr(blur_record,"fonts_done=1\ncalendar_done=0\nimages_done=0\nfont_result=-1\n"));
    assert(strstr(blur_record,"calendar_error=-1\n"));
    calendar_rc = 0;
    fire_timers(1000u);
    assert(strstr(blur_record,"calendar_done=1\nimages_done=0\n"));
    reject_metadata = 0;
    reject_redraw = 1;
    fire_timers(1000u);
    assert(strstr(blur_record,"refresh_pending=1\n"));
    assert(strstr(blur_record,"images_done=1\n"));
    reject_redraw = 0;
    fire_timers(1000u);
    assert(strstr(blur_record,"refresh_pending=0\n"));
    opens = blur_opens;
    fire_timers(50u); /* The completed watch's stale refresh handle self-deletes. */
    assert(blur_opens == opens);
    assert(active_timers(1000u) == 1u && !active_timers(50u));
    control_config = "invalid mapping\n";
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tblur-invalid-config\n";
    fire_timers(1000u);
    assert(strstr(blur_record,"config_error=-2103\n"));
    control_config = "/resource/\tthemes/next/\n";
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tblur-background-stage\n";
    background_stage_rc = -88;
    fire_timers(1000u);
    assert(strstr(blur_record,"background_error=-88\nbackground_initialized=1\n"));
    background_stage_rc = 0;
    background_commit_rc = -89;
    control_signal = "resource-hook-reload-v2\tng.lst.corona\tblur-background-commit\n";
    fire_timers(1000u);
    assert(strstr(blur_record,"background_error=-89\nbackground_initialized=1\n"));
    background_commit_rc = 0;
    font_rc = 0;
    fire_timers(1000u);
    assert(strstr(blur_record,"background_error=0\nbackground_initialized=1\n"));
    assert(strstr(blur_record,"config_error=0\n"));
    /* Error-gate/wake-budget oscillation is not an important lifecycle event.
     * No publication, root/source/display or module stage changes in 1000 polls. */
    control_signal = NULL;
    blur_epoch++;
    opens = blur_opens;
    for (i = 0; i < 1000u; i++) {
        blur_core_gate = i % 2u ? 7u : 3u;
        blur_pending = i % 2u;
        blur_armed = 1u - blur_pending;
        blur_cleanup_armed = blur_pending;
        fire_timers(1000u);
    }
    assert(blur_opens == opens + 8u);
    /* The eighth attempt truncates a previously complete file and fails. Once
     * I/O recovers, exactly one cooldown-delayed repair restores CURRENT state,
     * with no new epoch/reload and no immediate retry burst. */
    blur_epoch++;
    opens = blur_opens;
    for (i = 0; i < 7u; i++) {
        blur_capture_result = -1000 - (int)i;
        fire_timers(1000u);
    }
    assert(blur_opens == opens + 7u && blur_position > 13u);
    blur_capture_result = -2000;
    blur_write_fault = 1;
    fire_timers(1000u);
    assert(blur_opens == opens + 8u && blur_position == 13u);
    opens = blur_opens;
    blur_write_fault = 0;
    blur_capture_result = -2001;
    blur_counters++;
    for (i = 0; i < 31u; i++) fire_timers(1000u);
    assert(blur_opens == opens && blur_position == 13u);
    fire_timers(1000u);
    assert(blur_opens == opens + 1u && blur_position > 13u);
    assert(strstr(blur_record,"capture_result=-2001\n"));
    assert(strstr(blur_record,"frame52=13\n"));
    opens = blur_opens;
    for (i = 0; i < 100u; i++) fire_timers(1000u);
    assert(blur_opens == opens); /* Successful repair restores stable dedup. */
    /* Permanently failing writes remain bounded to one repair per 32 polls. */
    blur_epoch++;
    for (i = 0; i < 7u; i++) {
        blur_capture_result = -3000 - (int)i;
        fire_timers(1000u);
    }
    blur_write_fault = 1;
    blur_capture_result = -4000;
    fire_timers(1000u);
    assert(blur_position == 13u);
    opens = blur_opens;
    for (i = 0; i < 1000u; i++) {
        fire_timers(1000u);
        assert(blur_opens == opens + (i+1u)/32u);
        assert(blur_position == 13u);
    }
    /* A repair open failure also consumes the cooldown rather than spinning. */
    blur_write_fault = 0;
    blur_open_fault = 1;
    opens = blur_opens;
    for (i = 0; i < 32u; i++) fire_timers(1000u);
    assert(blur_opens == opens + 1u);
    blur_open_fault = 0;
    opens = blur_opens;
    for (i = 0; i < 24u; i++) fire_timers(1000u);
    assert(blur_opens == opens + 1u && blur_position > 13u);
    puts(".043 blur log: bounded gate oscillation, truncation repair and reload/background gates passed");
    return 0;
}
#endif
static int test_mem_services(void) {
    struct canopus_context_v1 foreign;
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    assert(rh_platform_alloc(16u) == NULL);
    rh_platform_free(NULL);
    assert(rh_platform_bind_mem(NULL) == CANOPUS_SVC_ERR_UNSUPPORTED);
    services.group_version[CANOPUS_SVC_MEM] = 0;
    assert(d->prepare(&test_context) == CANOPUS_SVC_ERR_UNSUPPORTED);
    services.group_version[CANOPUS_SVC_MEM] = 1;
    test_context.mem = NULL;
    assert(d->activate(&test_context) == CANOPUS_SVC_ERR_UNSUPPORTED);
    test_context.mem = &mem_services;
    mem_services.alloc = NULL;
    assert(d->prepare(&test_context) == CANOPUS_SVC_ERR_UNSUPPORTED);
    mem_services.alloc = mem_alloc;
    mem_services.free = NULL;
    assert(d->prepare(&test_context) == CANOPUS_SVC_ERR_UNSUPPORTED);
    mem_services.free = mem_free;
    assert(!config_opens && !allocations && !timers_created && slot == backend);
    assert(rh_platform_bind_mem(&test_context) == 0);
    assert(rh_platform_bind_mem(&test_context) == 0);
    foreign = test_context;
    assert(rh_platform_bind_mem(&foreign) == CANOPUS_SVC_ERR_STATE);
    assert(d->prepare(&test_context) == 0 && persistent_allocs == 1);
    input = "";
    assert(d->prepare(&test_context) == 0);
    assert(!persistent_allocs && allocations == frees);
    puts("MEM service binding, required callbacks and owner-side release passed");
    return 0;
}
static int test_time_services(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    unsigned before, i;
    assert(d->prepare(NULL) == CANOPUS_SVC_ERR_UNSUPPORTED);
    assert(d->activate(NULL) == CANOPUS_SVC_ERR_UNSUPPORTED);
    services.group_version[CANOPUS_SVC_TIME] = 0;
    assert(d->activate(&test_context) == CANOPUS_SVC_ERR_UNSUPPORTED);
    services.group_version[CANOPUS_SVC_TIME] = 1;
    test_context.time = NULL;
    assert(d->prepare(&test_context) == CANOPUS_SVC_ERR_UNSUPPORTED);
    test_context.time = &time_services;
    services.group_version[CANOPUS_SVC_FS] = 0;
    assert(d->prepare(&test_context) == CANOPUS_SVC_ERR_UNSUPPORTED);
    services.group_version[CANOPUS_SVC_FS] = 1;
    test_context.fs = NULL;
    assert(d->activate(&test_context) == CANOPUS_SVC_ERR_UNSUPPORTED);
    test_context.fs = &fs_services;
    assert(!config_opens && !timers_created && slot == backend);

    input = "";
    fail_timer = 1;
    assert(d->activate(&test_context) == -2012);
    assert(!timers_created && !active_timers(1000u));
    fail_timer = 0;
    fail_timer_start = 1;
    assert(d->activate(&test_context) == -2012);
    assert(timers_created == 1 && timers_deleted == 1 && !mock_timers[0].allocated);
    fail_timer_start = 0;
    assert(d->activate(&test_context) == 0);
    assert(mock_timers[0].active && mock_timers[0].interval == 1000u);
    before = timers_created;
    for (i = 0; i < 40u; i++) assert(d->activate(&test_context) == 0);
    assert(timers_created == before && active_timers(1000u) == 1);

    redraw_ready = 0;
    control_config = config;
    control_signal = "resource-hook-reload-v1\tng.lst.corona\ttime-services\n";
    fail_timer_start = 1;
    fire_timers(1000u);
    assert(!active_timers(50u) && timers_created == timers_deleted + 1);
    fail_timer_start = 0;
    fire_timers(1000u);
    assert(active_timers(50u) == 1);
    redraw_ready = 1;
    fire_timers(50u);
    assert(!active_timers(50u) && timers_created == timers_deleted + 1);
    assert(active_timers(1000u) == 1 && slot != backend && !locked);
    puts("time services: missing capability, ID zero, start rollback, reuse and retry cleanup passed");
    return 0;
}
static int test_time_activation_failure(const char *interval) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    unsigned opens;
    fail_timer_interval = (uint32_t)strtoul(interval, NULL, 10);
    assert(fail_timer_interval == 50u || fail_timer_interval == 1000u);
    assert(d->activate(&test_context) == (fail_timer_interval == 50u ? -2011 : -2012));
    assert(timers_created == 1 && timers_deleted == 1);
    assert(!active_timers(50u) && !active_timers(1000u));
    assert(!hooks_installed && slot == backend);
    /* Supervisor also reclaims MEM on initial failure. No prepared reference
     * may survive it; retrying this descriptor must read a fresh configuration. */
    for (unsigned i = 0; i < allocations; i++)
        if (temp_allocations[i].live) mem_free(&test_context, temp_allocations[i].pointer);
    assert(!persistent_allocs && allocations == frees);
    opens = config_opens;
    memset(mock_timers, 0, sizeof(mock_timers));
    canopus_hook_release_slot(test_context.slot);
    assert(slot == backend);
    fail_timer_interval = 0;
    assert(d->activate(&test_context) == 0);
    assert(config_opens == opens + 1u && persistent_allocs == 1u);
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    assert(active_timers(50u) == 1 && active_timers(1000u) == 1);
    fire_timers(50u);
    assert(!active_timers(50u) && active_timers(1000u) == 1);
    fire_timers(1000u);
    assert(timers_created == timers_deleted + 1 && !locked);
    puts("partial timer scheduling rolls back IDs before Supervisor cleanup and retry");
    return 0;
}
static int test_hook_services(void) {
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    uintptr_t foreign_slots[CANOPUS_HOOK_MAX_SLOTS], original;
    unsigned before, i;
    int32_t foreign;
    services.group_version[CANOPUS_SVC_HOOK] = 0;
    assert(d->activate(&test_context) == CANOPUS_SVC_ERR_UNSUPPORTED);
    services.group_version[CANOPUS_SVC_HOOK] = 1;
    test_context.hook = NULL;
    assert(d->prepare(&test_context) == CANOPUS_SVC_ERR_UNSUPPORTED);
    test_context.hook = &hook_services;
    assert(!config_opens && !hooks_installed && slot == backend);

    fail_hook_install = 1;
    assert(d->activate(&test_context) == -2010 && slot == backend);
    assert(!active_timers(50u) && !active_timers(1000u) && timers_created == timers_deleted);
    assert(!canopus_hook_is_hooked((uintptr_t)&slot));
    fail_hook_install = 0;
    /* A foreign hook can leave the native pointer unchanged. Its ownership
     * still prevents our install, and must not be removed on failure. */
    foreign = canopus_hook_install(1u, (uintptr_t)&slot, (uintptr_t)backend,
                                   &original, CANOPUS_HOOK_POINTER);
    assert(foreign == 0 && original == (uintptr_t)backend);
    assert(d->activate(&test_context) == -2010 && slot == backend);
    assert(canopus_hook_is_active(foreign) && !hooks_uninstalled);
    assert(canopus_hook_uninstall(1u, foreign) == 0);
    for (i = 0; i < CANOPUS_HOOK_MAX_SLOTS; i++) {
        foreign_slots[i] = (uintptr_t)backend;
        assert(canopus_hook_install(1u, (uintptr_t)&foreign_slots[i], (uintptr_t)other,
                                   &original, CANOPUS_HOOK_POINTER) == (int32_t)i);
    }
    assert(d->activate(&test_context) == -2010 && slot == backend);
    assert(!hooks_installed && !hooks_uninstalled && timers_created == timers_deleted);
    canopus_hook_release_slot(1u);

    assert(d->activate(&test_context) == 0 && slot != backend);
    assert(canopus_hook_is_active(0) && hooks_installed == 1);
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    before = hooks_installed;
    for (i = 0; i < 40u; i++) assert(d->activate(&test_context) == 0);
    assert(hooks_installed == before && !hooks_uninstalled);

    slot = other;
    assert(d->activate(&test_context) == -2009 && slot == other);
    assert(hooks_installed == before && !hooks_uninstalled);
    slot = backend; /* Firmware reset the slot, while the record remains live. */
    fail_hook_uninstall = 1;
    assert(d->activate(&test_context) == -2010 && slot == backend);
    assert(hooks_installed == before && canopus_hook_is_active(0));
    fail_hook_uninstall = 0;
    assert(d->activate(&test_context) == 0 && slot != backend);
    assert(hooks_installed == before + 1 && hooks_uninstalled == 1 && font_disabled);

    canopus_hook_release_slot(test_context.slot); /* Supervisor removed its record too. */
    assert(slot == backend && !canopus_hook_is_hooked((uintptr_t)&slot));
    assert(d->activate(&test_context) == 0 && slot != backend);
    assert(hooks_installed == before + 2 && hooks_uninstalled == 1);
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    assert(d->stop(&test_context) == CANOPUS_RESULT_REBOOT_REQUIRED);
    assert(slot != backend && hooks_uninstalled == 1 && !locked);
    puts("hook services: ID zero, conflict, capacity, reset/release recovery and residency passed");
    return 0;
}
static unsigned manager_notifications, manager_status_notifications, manager_transfer_notifications;
static void manager_state_changed(void *cookie, uint32_t changes) {
    struct rh_module_status_snapshot status;
    struct rh_transfer_ui_state transfer;
    assert(cookie == &manager_notifications && !locked);
    ++manager_notifications;
    if (changes & RH_MANAGER_CHANGED_STATUS) ++manager_status_notifications;
    if (changes & RH_MANAGER_CHANGED_TRANSFER) ++manager_transfer_notifications;
    rh_module_get_status_snapshot(&status);
    rh_manager_get_transfer_state(&transfer);
    /* A notification cannot reenter the config publisher or transfer setter. */
    if (status.reload_generation)
        assert(rh_module_request_reload(status.reload_generation + 100u) == CANOPUS_SVC_ERR_BUSY);
    assert(rh_manager_set_transfer_state(&transfer) == CANOPUS_SVC_ERR_BUSY);
}
static int test_manager_internal_state(void) {
    struct rh_module_status_snapshot status;
    struct rh_transfer_ui_state transfer = {0}, copied;
    unsigned reads, signals, writes, notifications;
    rh_module_get_status_snapshot(&status);
    assert(status.state == RH_MGR_STATE_UNAVAILABLE && !status.active_rules);
    assert(rh_manager_subscribe(NULL, NULL) == CANOPUS_SVC_ERR_ARGUMENT);
    assert(!rh_manager_subscribe(manager_state_changed, &manager_notifications));
    assert(manager_notifications == 1 && manager_status_notifications == 1 &&
           manager_transfer_notifications == 1);
    assert(rh_manager_subscribe(manager_state_changed, NULL) == CANOPUS_SVC_ERR_BUSY);
    assert(canopus_module_descriptor.activate(&test_context) == 0);
    fire_timers(50u);
    rh_module_get_status_snapshot(&status);
    assert(status.state == RH_MGR_STATE_RUNNING && status.active_rules == 1 && !status.refresh_pending);
    reads = config_opens + control_opens;
    signals = signal_opens; writes = result_writes;
    rh_module_get_status_snapshot(&status);
    rh_manager_get_transfer_state(&copied);
    assert(config_opens + control_opens == reads && signal_opens == signals && result_writes == writes);

    control_config = "/resource/\tthemes/second/\n";
    reject_redraw = 1;
    in_ui_timer = 1;
    assert(rh_module_request_reload(0) == CANOPUS_SVC_ERR_ARGUMENT);
    assert(!rh_module_request_reload(42u));
    in_ui_timer = 0;
    rh_module_get_status_snapshot(&status);
    assert(status.reload_generation == 42 && status.reload_result == 1 && status.refresh_pending);
    assert(rh_module_request_reload(43u) == CANOPUS_SVC_ERR_BUSY);
    assert(signal_opens == signals && result_writes == writes && control_opens == 1);
    reject_redraw = 0;
    fire_timers(50u);
    rh_module_get_status_snapshot(&status);
    assert(status.reload_generation == 42 && !status.reload_result && !status.refresh_pending);
    assert(rh_module_request_reload(42u) == CANOPUS_SVC_ERR_ARGUMENT);
    notifications = manager_notifications;
    fire_timers(50u);
    assert(manager_notifications == notifications);

    control_config = "/resource/\t/outside/\n";
    in_ui_timer = 1;
    assert(!rh_module_request_reload(43u));
    in_ui_timer = 0;
    rh_module_get_status_snapshot(&status);
    assert(status.state == RH_MGR_STATE_CONFIG_ERROR && status.reload_result == -2103);
    assert(status.active_rules == 1 && !status.refresh_pending);
    backend_expected = (RH_THEME_ROOT "second/icon.bin") + 1;
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    backend_expected = "resource/icon.bin";
    assert(slot(&driver, ".rh_sys/resource/icon.bin", 2) == 7);
    assert(slot(&driver, ".rh_sys/../resource/icon.bin", 2) == -1);
    assert(slot(&driver, ".rh_sys/resource//icon.bin", 2) == -1);
    assert(slot(&driver, ".rh_sys/", 2) == -1);
    backend_expected = "r";
    assert(slot(&driver, "r", 2) == 7); /* Short strings are safe under ASan. */

    transfer.phase = RH_XFER_PHASE_RECEIVING;
    transfer.remote_connected = 1;
    transfer.archive_bytes = UINT32_MAX;
    transfer.written_offset = UINT32_MAX / 2u;
    transfer.durable_offset = 65536u;
    transfer.percent = 99; /* Caller percentages do not advance disk progress. */
    assert(!rh_manager_set_transfer_state(&transfer));
    rh_manager_get_transfer_state(&copied);
    assert(copied.percent == 49 && copied.written_offset == transfer.written_offset);
    notifications = manager_notifications;
    assert(!rh_manager_set_transfer_state(&transfer));
    assert(manager_notifications == notifications);
    transfer.durable_offset = UINT32_MAX;
    assert(rh_manager_set_transfer_state(&transfer) == CANOPUS_SVC_ERR_ARGUMENT);
    rh_manager_get_transfer_state(&transfer);
    transfer.phase = 7;
    assert(rh_manager_set_transfer_state(&transfer) == CANOPUS_SVC_ERR_ARGUMENT);
    assert(rh_manager_set_transfer_state(NULL) == CANOPUS_SVC_ERR_ARGUMENT);
    assert(signal_opens == signals && result_writes == writes);
    rh_manager_unsubscribe(manager_state_changed, &manager_notifications);
    transfer = (struct rh_transfer_ui_state){0};
    assert(!rh_manager_set_transfer_state(&transfer));
    assert(manager_notifications == notifications);
#if RH_NATIVE_MANAGER
    fire_timers(1000u);
    assert(signal_opens == signals && result_writes == writes);
#endif
    puts("native manager: direct state, event completion, failure retention, stock previews and disk progress passed");
    return 0;
}
static int test_manager_reentrant_reload(void) {
    struct rh_module_status_snapshot status;
    input = "";
    assert(!canopus_module_descriptor.activate(&test_context));
    rh_module_get_status_snapshot(&status);
    assert(!status.active_rules && !status.refresh_pending);
    control_config = config;
    activate_during_read = 1;
    reject_redraw = 1;
    in_ui_timer = 1;
    assert(!rh_module_request_reload(10u));
    in_ui_timer = 0;
    rh_module_get_status_snapshot(&status);
    assert(status.reload_generation == 10u && status.reload_result == 1 && status.refresh_pending);
    reject_redraw = 0;
    fire_timers(50u);
    rh_module_get_status_snapshot(&status);
    assert(!status.reload_result && !status.refresh_pending && status.active_rules == 1u);
    control_config = "/resource/\t/outside/\n";
    in_ui_timer = 1;
    assert(!rh_module_request_reload(11u));
    in_ui_timer = 0;
    rh_module_get_status_snapshot(&status);
    assert(status.reload_result == -2103 && status.error_code == -2103);
#if RH_Q66_BACKGROUND || (defined(RH_TARGET_1043) && RH_TARGET_1043)
    control_config = config;
    background_config = "version=1\nmode=system\n";
    background_stage_rc = -2142;
    in_ui_timer = 1;
    assert(!rh_module_request_reload(12u));
    in_ui_timer = 0;
    rh_module_get_status_snapshot(&status);
    assert(status.reload_result == -2142 && status.error_code == -2103);
#endif
    puts("native manager: reentrant activation cannot acknowledge early; each attempt keeps its own error");
    return 0;
}
#if RH_NATIVE_MANAGER
static int test_manager_config_generations(void) {
    struct rh_module_status_snapshot status;
    assert(!canopus_module_descriptor.activate(&test_context));
    fire_timers(50u);
    control_config = "/resource/\tthemes/second/\n";
    in_ui_timer = 1;
    assert(rh_module_request_reload_from(41u, "active/42") == CANOPUS_SVC_ERR_ARGUMENT);
    assert(!rh_module_request_reload_from(42u, "active/42"));
    in_ui_timer = 0;
    rh_module_get_status_snapshot(&status);
    assert(!status.reload_result && status.reload_generation == 42u);
    assert(!strcmp(native_active_config, "active/42\n") && native_pointer_writes > 1u);
    assert(!strcmp(rh_manager_config_current_dir(), RH_APP_FILES_ROOT "active/42/"));
    native_rename_fail = 1;
    control_config = "/resource/\tthemes/third/\n";
    in_ui_timer = 1;
    assert(!rh_module_request_reload_from(43u, "active/43"));
    in_ui_timer = 0;
    rh_module_get_status_snapshot(&status);
    assert(status.reload_result == CANOPUS_SVC_ERR_IO && !status.refresh_pending);
    assert(!strcmp(native_active_config, "active/42\n"));
    assert(!strcmp(rh_manager_config_current_dir(), RH_APP_FILES_ROOT "active/42/"));
    backend_expected = (RH_THEME_ROOT "second/icon.bin") + 1;
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    native_rename_fail = 0;
    in_ui_timer = 1;
    assert(!rh_module_request_reload_from(44u, "active/44"));
    in_ui_timer = 0;
    rh_module_get_status_snapshot(&status);
    assert(!status.reload_result && !strcmp(native_active_config, "active/44\n"));
    assert(!signal_opens && !result_writes);
    puts("native manager: config generation commit, rename failure retention and retry passed");
    return 0;
}
#endif
int main(int argc, char **argv) {
    rh_module_entry(&test_context);
    canopus_hook_engine_init();
#if RH_NATIVE_MANAGER
    if (argc == 2 && !strcmp(argv[1], "--manager-generations"))
        return test_manager_config_generations();
#endif
    if (argc == 2 && !strcmp(argv[1], "--manager-reentrant"))
        return test_manager_reentrant_reload();
    if (argc == 2 && !strcmp(argv[1], "--manager-internal"))
        return test_manager_internal_state();
    if (argc == 3 && !strcmp(argv[1], "--time-activation-failure"))
        return test_time_activation_failure(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--hook-services")) return test_hook_services();
    if (argc == 2 && !strcmp(argv[1], "--mem-services")) return test_mem_services();
    if (argc == 2 && !strcmp(argv[1], "--time-services")) return test_time_services();
#if defined(RH_TARGET_1043) && RH_TARGET_1043 && \
    defined(RH_P67_BLUR_DIAGNOSTICS) && RH_P67_BLUR_DIAGNOSTICS
    if (argc == 2 && !strcmp(argv[1], "--blur-diagnostics"))
        return test_blur_diagnostic_writer();
#endif
    test_relative_config();
#if RH_Q66_BACKGROUND || (defined(RH_TARGET_1043) && RH_TARGET_1043)
    if (argc == 2 && !strcmp(argv[1], "--launcher-background"))
        return test_launcher_background();
#endif
    if (argc == 2 && !strcmp(argv[1], "--control-reload-status"))
        return test_control_reload_status();
    if (argc == 2 && !strcmp(argv[1], "--control-quickapp-status"))
        return test_control_quickapp_status();
    if (argc == 3 && !strcmp(argv[1], "--control-status"))
        return test_control_status(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--quickapp"))
        return test_quickapp_integration();
    if (argc > 1 && !strcmp(argv[1], "--calendar-initial-invalid"))
        return test_calendar_initial_invalid();
    if (argc > 1 && !strcmp(argv[1], "--calendar-coordinator-guards"))
        return test_calendar_coordinator_guards();
    if (argc > 1 && !strcmp(argv[1], "--calendar-alpha-hook"))
        return test_calendar_alpha_hook();
    if (argc > 1 && !strcmp(argv[1], "--calendar"))
        return test_calendar_integration();
    if (argc == 2 && !strcmp(argv[1], "--snapshots"))
        return test_compact_snapshots();
    if (argc == 2 && !strcmp(argv[1], "--startup-diagnostics"))
        return test_startup_diagnostics();
    if (argc == 2 && !strcmp(argv[1], "--font-reload"))
        return test_font_integration();
    if (argc == 2 && !strcmp(argv[1], "--empty-startup"))
        return test_empty_startup_then_theme_reload();
    if (argc == 3 && !strcmp(argv[1], "--startup-fallback"))
        return test_startup_fallback_then_reload(argv[2]);
    assert(argc == 1);
    struct canopus_module_descriptor_v1 *d = &canopus_module_descriptor;
    struct canopus_status_writer_v1 w;
    unsigned char status[48];
    unsigned before, fonts_before;
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
    assert(registrations == 1 && !closes);
    assert(d->struct_size == sizeof(*d) && d->abi_major == CANOPUS_ABI_MAJOR &&
           d->abi_minor == CANOPUS_ABI_MINOR);
    assert(!strcmp((const char *)d->module_id, "corona"));
    assert(!strcmp((const char *)d->module_version, "0.3.0"));
    assert(!strcmp((const char *)d->build_id, "resource-hook-0.3.0"));
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    assert(!strcmp((const char *)d->target_id, "xiaomi-band-10-pro-3.101.043"));
#elif defined(RH_TARGET_177) && RH_TARGET_177
    assert(!strcmp((const char *)d->target_id, "xiaomi-band-11-4.100.177"));
#elif defined(RH_TARGET_155) && RH_TARGET_155
    assert(!strcmp((const char *)d->target_id, "xiaomi-band-11-4.100.155"));
#else
    assert(!strcmp((const char *)d->target_id, "xiaomi-band-11-4.100.139"));
#endif
    assert(d->stop(NULL) == 0 && d->deactivate(NULL) == 0);
    assert(d->query(NULL) == -1);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w));
    assert(w.used == 48 && u32(status) == 0x31514852u && u32(status + 4) == 6);
    assert(u32(status + 8) == 0 && u32(status + 12) == 0 && u32(status + 24) == 0 &&
           u32(status + 28) == 0 && u32(status + 32) == 0 && u32(status + 36) == 0 &&
           u32(status + 40) == 0 && u32(status + 44) == 0);
    assert(d->query(&w) == -1);
    assert(!canopus_status_writer_init(&w, status, 47));
    memset(status, 0xab, sizeof(status));
    assert(d->query(&w) == -1 && w.used == 0 && status[0] == 0xab);

    fail_open = 1;
    before = closes;
    assert(d->prepare(&test_context) == 0 && slot == backend && closes == before);
    assert(strstr(startup_record, "config.fallback rc=-2005"));
    open_errno = 13;  /* EACCES degrades to a resident pass-through. */
    assert(d->prepare(&test_context) == 0 && slot == backend);
    open_errno = 123;  /* Unknown negative service error is also nonfatal. */
    assert(d->prepare(&test_context) == 0);
    open_errno = RH_ENOENT;
    driver_valid = 0;
    assert(d->prepare(&test_context) == 0);  /* Missing mappings prepare an empty snapshot. */
    assert(slot == backend && allocations == frees + persistent_allocs &&
           persistent_allocs == 0 && !image_drops && !metadata_refreshes && !redraws &&
           !retargets && !timers_created && !active_timers(50u) && !active_timers(1000u));
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w) && u32(status + 8) == 0 && u32(status + 12) == 0);
    assert(d->stop(NULL) == 0 && d->deactivate(NULL) == 0);

    driver_valid = 1;
    fail_open = 0; fail_alloc = 1;
    assert(d->prepare(&test_context) == 0 && slot == backend && closes == before + 1);
    assert(strstr(startup_record, "config.fallback rc=-2006"));
    fail_alloc = 0; fail_read = 1;
    assert(d->prepare(&test_context) == 0 && allocations == frees && !persistent_allocs);
    assert(strstr(startup_record, "config.fallback rc=-2007"));
    fail_read = 0; input = "/resource/\t/outside/\n";
    assert(d->prepare(&test_context) == 0 && slot == backend && allocations == frees);
    assert(strstr(startup_record, "config.fallback rc=-2007"));
    input = "# empty\n";
    driver_valid = 0;
    assert(d->prepare(&test_context) == 0);
    input = "";
    assert(d->prepare(&test_context) == 0);
    assert(slot == backend && allocations == frees + persistent_allocs &&
           !image_drops && !metadata_refreshes && !redraws && !timers_created);
    driver_valid = 1;
    input = config;
    /* A failed re-prepare preserves the previously prepared good snapshot. */
    assert(d->prepare(&test_context) == 0);
    fail_open = 1; open_errno = 13;
    assert(d->prepare(&test_context) == 0);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w) && u32(status + 8) == 0 && u32(status + 12) == 1);
    assert(persistent_allocs == 1 && allocations == frees + persistent_allocs);
    /* Optional absence still explicitly prepares a zero-rule snapshot. */
    open_errno = RH_ENOENT;
    assert(d->prepare(&test_context) == 0);
    assert(slot == backend && !timers_created);
    assert(!canopus_status_writer_init(&w, status, sizeof(status)));
    assert(!d->query(&w) && u32(status + 8) == 0 && u32(status + 12) == 0);
    fail_open = 0;
    input = config;
    assert(d->prepare(&test_context) == 0);
    driver_valid = 0;
    assert(d->activate(&test_context) == -2008 && slot == backend && !locked);
    assert(allocations == frees + persistent_allocs);
    driver_valid = 1; slot = other;
    assert(d->activate(&test_context) == -2009 && slot == other && !locked);
    slot = NULL;
    assert(d->activate(&test_context) == -2009 && !slot);
    slot = backend;
    image_cache_ready = 0;
    redraw_ready = 0;
    assert(d->activate(&test_context) == 0 && slot != backend && !locked);
    before = config_opens;
    assert(image_drops == 0);  /* no drop while the image caches do not yet exist */
    assert(redraws == 0);      /* and no redraw while no display exists yet */
    image_cache_ready = 1;
    redraw_ready = 1;
    slot = backend;
    fonts_before = font_calls;
    assert(d->activate(&test_context) == 0 && slot != backend && image_drops == 1 && !redraws);
    assert(font_calls == fonts_before && font_disabled == 1 && active_timers(50u) == 1);
    font_status(-2014, 1, 0);
    fire_timers(50u);
    assert(redraws == 1 && font_calls == fonts_before + 1 && !active_timers(50u));
    font_status(-2099, 0, 0);
    assert(slot(&driver, "resource/icon.bin", 2) == 7);
    assert(d->prepare(&test_context) == -2004 && config_opens == before);
    fail_open = 1;
    assert(d->activate(&test_context) == 0 && config_opens == before && image_drops == 2 && redraws == 1);
    fire_timers(50u);
    assert(redraws == 2 && !active_timers(50u));
    slot = backend;
    assert(d->activate(&test_context) == 0 && slot != backend && config_opens == before);
    assert(image_drops == 3 && redraws == 2 && active_timers(50u) == 1);
    fire_timers(50u);
    assert(redraws == 3);
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
    assert(u32(status + 36) == 0 && !retargets);
    assert((int32_t)u32(status + 40) == -2099 && !u32(status + 44));
    /* Busy UI: requests coalesce and the cache isn't touched mid-render. */
    redraw_ready = 0;
    assert(d->activate(&test_context) == 0 && active_timers(50u) == 1);
    before = timers_created;
    assert(d->activate(&test_context) == 0 && timers_created == before);
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
    fire_timers(1000u);  /* reactivation retains the one service watcher */
    assert(active_timers(1000u) == 1);
    /* Metadata retry does not retire again, rebuild, or prematurely redraw. */
    reject_metadata = 1;
    before = metadata_refreshes;
    assert(d->activate(&test_context) == 0 && active_timers(50u) == 1);
    assert(image_drops == 5 && redraws == 4);
    fire_timers(50u);
    assert(image_drops == 5 && metadata_refreshes == before && rebuilds == 0);
    reject_metadata = 0;
    fire_timers(50u);
    assert(metadata_refreshes == before + 1 && redraws == 5 && !active_timers(50u));
    fire_timers(1000u);
    /* Unsupported-target adapters complete a repaint, never claim retirement. */
    unsupported = 1;
    assert(d->activate(&test_context) == 0 && active_timers(50u) == 1);
    assert(image_drops == 5 && redraws == 5 && metadata_refreshes == before + 1);
    fire_timers(50u);
    assert(image_drops == 5 && redraws == 6 && metadata_refreshes == before + 1 &&
           !active_timers(50u));
    unsupported = 0;
    fire_timers(1000u);
    /* OOM remains visible, but doesn't remove the resident redirect. */
    redraw_ready = 0;
    fail_timer = 1;
    assert(d->activate(&test_context) == -2011 && slot != backend);
    fail_timer = 0;
    assert(d->activate(&test_context) == 0 && active_timers(50u) == 1);
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
    assert(d->activate(&test_context) == 0);  /* a zero-rule resident snapshot can rebind */
    fire_timers(1000u);  /* the service watcher survives reactivation */

    assert(allocations == frees && persistent_allocs == 0 && !live_bytes &&
           active_timers(1000u) == 1 && !locked);
    puts("module registration, activation, config polling, atomic snapshots and targeted refresh passed");
    return 0;
}

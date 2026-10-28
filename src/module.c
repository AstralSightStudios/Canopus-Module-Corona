#include "canopus_abi.h"
#include "canopus_services.h"
#include "canopus_module_entry.h"
#include "canopus_memory.h"
#include "resource_hook_platform.h"
#include "resource_hook_target.h"
#include "resource_hook_quickapp.h"
#include "resource_hook_font_reload.h"
#include "resource_hook_background.h"
#include "resource_hook_calendar_runtime.h"
#include "manager_state.h"
#if RH_NATIVE_MANAGER
#include "manager_ui.h"
#include "manager_config_store.h"
#include "corona_runtime.h"
#endif

#define RH_MODULE_ID "corona"
#define RH_VERSION "0.3.0"
/* Resolve app-scoped control paths using the selected firmware target. */
#define RH_CONTROL_CONFIG RH_CONFIG_PATH
#define RH_RELOAD_PREFIX "resource-hook-reload-v1\t"
#define RH_RELOAD_V2_PREFIX "resource-hook-reload-v2\tng.lst.corona\t"
#define RH_STATUS_PREFIX "resource-hook-status-v1\tng.lst.corona\t"
#define RH_CONTROL_REQUEST_MAX 128u
#define RH_RELOAD_POLL_MS 1000u
#if defined(RH_TARGET_P65) && RH_TARGET_P65
#define RH_TARGET_ID "redmi-watch-6-3.100.043"
#elif defined(RH_TARGET_1043) && RH_TARGET_1043
#define RH_TARGET_ID "xiaomi-band-10-pro-3.101.043"
#elif defined(RH_TARGET_179) && RH_TARGET_179
#define RH_TARGET_ID "xiaomi-band-11-4.100.179"
#elif defined(RH_TARGET_177) && RH_TARGET_177
#define RH_TARGET_ID "xiaomi-band-11-4.100.177"
#elif defined(RH_TARGET_155) && RH_TARGET_155
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
static int configured, activated_once;
/* Explicit config failures stay latched until a successful publication.
 * Autonomous QuickApp resolution has an independently recoverable error. */
static int32_t config_error, quickapp_error, calendar_config_error, calendar_error;
static unsigned calendar_started;
static int32_t background_error;
static unsigned background_initialized;
static uint32_t images_dropped;
static uint32_t redraws;
/* Reserved RHQ1 v5 field: no forced page rebuilds in the reload path. */
static const uint32_t rebuilds;
static uint32_t fonts_retargeted;
/* 1 = pending/busy, 0 = completed, negative = rejected/failed. */
static int32_t font_result;
static uint32_t font_last_changed;
static const struct canopus_context_v1 *module_context;
static int32_t refresh_timer = -1, watch_timer = -1;
static int32_t open_hook = -1;
static rh_open_fn *open_hook_slot;
#if RH_NATIVE_MANAGER
static int manager_config_error;
static char manager_candidate_dir[RH_PATH];
static const char *manager_candidate_relative;
static struct rh_backgrounds manager_background_current;
static int manager_quickapp_icon(void *, const char *, char out[RH_PATH]);
static int manager_bind_data_hooks(void) {
    struct rh_manager_data_hooks hooks = {0, manager_quickapp_icon, 0};
    return rh_manager_ui_set_data_hooks(&hooks);
}
#endif
static int manager_config_path(const char *name, char out[RH_PATH]) {
    const char *root = RH_APP_FILES_ROOT;
    unsigned i = 0, j = 0;
#if RH_NATIVE_MANAGER
    if (manager_config_error && !manager_candidate_dir[0]) return manager_config_error;
    root = manager_candidate_dir[0] ? manager_candidate_dir : rh_manager_config_current_dir();
    if (!root) return CANOPUS_SVC_ERR_STATE;
#endif
    while (root[i]) {
        if (i + 1u >= RH_PATH) return CANOPUS_SVC_ERR_ARGUMENT;
        out[i] = root[i];
        ++i;
    }
    while (name[j]) {
        if (i + 1u >= RH_PATH) return CANOPUS_SVC_ERR_ARGUMENT;
        out[i++] = name[j++];
    }
    out[i] = 0;
    return 0;
}
static int manager_calendar_stage(struct rh_calendar_config **out) {
#if RH_NATIVE_MANAGER
    char path[RH_PATH];
    int rc = manager_config_path("calendar.cfg", path);
    return rc ? rc : rh_calendar_stage_from(path, out);
#else
    return rh_calendar_stage(out);
#endif
}
static int manager_calendar_start(void) {
#if RH_NATIVE_MANAGER
    struct rh_calendar_config *candidate = 0;
    int rc = manager_calendar_stage(&candidate);
    if (!rc) rc = rh_calendar_commit(candidate);
    if (rc) rh_calendar_discard(candidate);
    return rc;
#else
    return rh_calendar_start();
#endif
}

static int bind_context(const struct canopus_context_v1 *ctx) {
    int rc;
    if (!canopus_services_has(ctx, CANOPUS_SVC_TIME, 1) || !ctx->time ||
        !ctx->time->timer_create || !ctx->time->timer_start ||
        !ctx->time->timer_destroy) return CANOPUS_SVC_ERR_UNSUPPORTED;
    if (!canopus_services_has(ctx, CANOPUS_SVC_HOOK, 1) || !ctx->hook ||
        !ctx->hook->install || !ctx->hook->uninstall ||
        !ctx->hook->is_hooked) return CANOPUS_SVC_ERR_UNSUPPORTED;
    if (module_context && module_context != ctx) return CANOPUS_SVC_ERR_STATE;
    rc = rh_platform_bind_mem(ctx);
    if (rc < 0) return rc;
    rc = rh_platform_bind_fs(ctx);
    if (rc < 0) return rc;
#if RH_NATIVE_MANAGER
    /* Connect may query the shared catalog before native app publication. */
    rc = manager_bind_data_hooks();
    if (rc) return rc;
    if (!module_context) manager_config_error = rh_manager_config_bind(ctx);
#endif
    module_context = ctx;
    return 0;
}
static int32_t create_timer(uint32_t interval, canopus_timer_callback_t callback) {
    int32_t id = module_context->time->timer_create(module_context, interval, 1,
                                                   callback, 0);
    int32_t rc;
    if (id < 0) return id;
    rc = module_context->time->timer_start(module_context, id);
    if (rc < 0) {
        (void)module_context->time->timer_destroy(module_context, id);
        return rc;
    }
    return id;
}
static void delete_timer(int32_t id) {
    (void)module_context->time->timer_destroy(module_context, id);
}
static struct rh_snapshot *active_snapshot, *refresh_previous_snapshot;
/* Semantic declarations survive absent packages and app upgrades. Only the
 * materialized file snapshot is visible to the open hook. */
static struct rh_snapshot *quickapp_declarations;
static unsigned control_busy, result_busy;
static char last_reload_signal[RH_CONTROL_REQUEST_MAX];
static uint32_t last_reload_signal_size;
static unsigned last_reload_signal_valid;
/* Only the latest observed valid request owns the shared response slot. */
#define RH_REQUEST_RELOAD 1u
#define RH_REQUEST_STATUS 2u
static char response_signal[RH_CONTROL_REQUEST_MAX];
static uint32_t response_signal_size;
static unsigned response_kind;
static unsigned refresh_pending, cache_dropped, images_done, fonts_done, calendar_done, refreshing;
static void publish_current_result(void);
static void notify_manager_status(void);
static char *result_number(char *, uint32_t);
static rh_manager_state_listener manager_listener;
static void *manager_listener_cookie;
static unsigned manager_notifying, manager_status_valid;
static struct rh_module_status_snapshot manager_last_status;
static struct rh_transfer_ui_state manager_transfer;
static uint32_t manager_reload_generation;
static int32_t manager_reload_result;

static void manager_notify(uint32_t changes) {
    if (!manager_listener || manager_notifying) return;
    manager_notifying = 1;
    manager_listener(manager_listener_cookie, changes);
    manager_notifying = 0;
}
void rh_manager_get_transfer_state(struct rh_transfer_ui_state *out) {
    if (out) *out = manager_transfer;
}
int rh_manager_set_transfer_state(const struct rh_transfer_ui_state *state) {
    struct rh_transfer_ui_state candidate;
    uint32_t accumulator = 0, i;
    uint8_t percent = 0;
    if (!state || state->phase > RH_XFER_PHASE_CANCELED || state->detail > 11u ||
        state->remote_connected > 1u ||
        state->durable_offset > state->written_offset ||
        state->written_offset > state->archive_bytes)
        return CANOPUS_SVC_ERR_ARGUMENT;
    if (manager_notifying) return CANOPUS_SVC_ERR_BUSY;
    candidate = *state;
    /* Exact floor(100 * written / total) without a 64-bit divide runtime
     * import. The validated written<=total bound makes each addition safe. */
    if (state->archive_bytes) {
        for (i = 0; i < 100u; ++i) {
            if (accumulator >= state->archive_bytes - state->written_offset) {
                accumulator -= state->archive_bytes - state->written_offset;
                ++percent;
            } else accumulator += state->written_offset;
        }
    }
    candidate.percent = percent;
    if (manager_transfer.phase == candidate.phase &&
        manager_transfer.detail == candidate.detail &&
        manager_transfer.remote_connected == candidate.remote_connected &&
        manager_transfer.percent == candidate.percent &&
        manager_transfer.written_offset == candidate.written_offset &&
        manager_transfer.durable_offset == candidate.durable_offset &&
        manager_transfer.archive_bytes == candidate.archive_bytes) return 0;
    manager_transfer = candidate;
    manager_notify(RH_MANAGER_CHANGED_TRANSFER);
    return 0;
}
int rh_manager_subscribe(rh_manager_state_listener listener, void *cookie) {
    if (!listener) return CANOPUS_SVC_ERR_ARGUMENT;
    if (manager_notifying || (manager_listener &&
        (manager_listener != listener || manager_listener_cookie != cookie)))
        return CANOPUS_SVC_ERR_BUSY;
    manager_listener = listener;
    manager_listener_cookie = cookie;
    manager_notify(RH_MANAGER_CHANGED_STATUS | RH_MANAGER_CHANGED_TRANSFER);
    return 0;
}
void rh_manager_unsubscribe(rh_manager_state_listener listener, void *cookie) {
    if (manager_listener == listener && manager_listener_cookie == cookie) {
        manager_listener = 0;
        manager_listener_cookie = 0;
    }
}

static void *snapshot_alloc(void *cookie, uint32_t size) {
    (void)cookie;
    return rh_platform_alloc(size);
}
static void snapshot_free(void *cookie, void *pointer) {
    (void)cookie;
    rh_platform_free(pointer);
}
static const struct rh_allocator snapshot_allocator = {0, snapshot_alloc, snapshot_free};
/* Active ownership, refresh ownership and open/adapter pins share a reference
 * count. A hook's final release only queues the dead snapshot under the IRQ
 * lock. Its header then stores an intrusive next pointer, preserving the
 * compact live layout and requiring no allocation in the hook. */
_Static_assert(sizeof(struct rh_snapshot) >= sizeof(void *), "retirement link fits");
static struct rh_snapshot *retired_snapshots;
static void snapshot_unpin(struct rh_snapshot *snapshot, unsigned from_hook) {
    uint32_t irq;
    unsigned release = 0;
    if (!snapshot) return;
    irq = rh_platform_lock();
    if (snapshot->references && --snapshot->references == 0u) {
        if (from_hook) {
            canopus_memcpy(snapshot, &retired_snapshots, sizeof(retired_snapshots));
            retired_snapshots = snapshot;
        } else release = 1;
    }
    rh_platform_unlock(irq);
    if (release) rh_free_snapshot(&snapshot_allocator, snapshot);
}
static void snapshot_release(struct rh_snapshot *snapshot) {
    snapshot_unpin(snapshot, 0u);
}
/* Only lifecycle/UI owner callbacks drain this list. Detach first so a hook
 * can retire another generation while the service frees the previous batch. */
static void collect_snapshots(void) {
    struct rh_snapshot *snapshot, *next;
    uint32_t irq = rh_platform_lock();
    snapshot = retired_snapshots;
    retired_snapshots = 0;
    rh_platform_unlock(irq);
    while (snapshot) {
        canopus_memcpy(&next, snapshot, sizeof(next));
        rh_platform_free(snapshot);
        snapshot = next;
    }
}

/* Best-effort, bounded lifecycle diagnostics. No heap, graphics calls or I/O
 * under the IRQ lock; the lifecycle caller must already be serialized.
 * Rewrite a snapshot so neither repeated failures nor UI restarts grow a file
 * indefinitely. A failed log write never changes registration/start results. */
#define RH_STARTUP_LOG_BYTES 2048u
static char startup_log[RH_STARTUP_LOG_BYTES];
static uint32_t startup_log_used;
static char *log_text(char *out, const char *text) {
    while (*text) *out++ = *text++;
    return out;
}
static void startup_record(const char *stage, int32_t rc, int error, uintptr_t detail) {
    char *p;
    uint32_t used = 0, shift;
    int fd, written;
    /* All stage names are fixed literals, at most 32 bytes. Reserve one full
     * line before formatting; a rollover keeps the newest activation visible. */
    if (!startup_log_used || startup_log_used > RH_STARTUP_LOG_BYTES - 160u) {
        p = log_text(startup_log, "RHSTART1 target=" RH_TARGET_ID " version=" RH_VERSION
                     "\nconfig=" RH_CONTROL_CONFIG "\n");
        startup_log_used = (uint32_t)(p - startup_log);
    }
    p = log_text(startup_log + startup_log_used, stage);
    p = log_text(p, " rc=");
    if (rc < 0) *p++ = '-';
    p = result_number(p, rc < 0 ? 0u - (uint32_t)rc : (uint32_t)rc);
    p = log_text(p, " errno=");
    p = result_number(p, error > 0 ? (uint32_t)error : 0u);
    p = log_text(p, " detail=0x");
    for (shift = (uint32_t)sizeof(detail) * 8u; shift; shift -= 4u)
        *p++ = "0123456789abcdef"[(detail >> (shift - 4u)) & 15u];
    *p++ = '\n';
    startup_log_used = (uint32_t)(p - startup_log);
    fd = rh_platform_open(RH_STARTUP_LOG_PATH, RH_FILE_REPLACE);
    if (fd < 0) return;
    while (used < startup_log_used) {
        written = rh_platform_write(fd, startup_log + used, startup_log_used - used);
        if (written <= 0 || (uint32_t)written > startup_log_used - used) break;
        used += (uint32_t)written;
    }
    rh_platform_close(fd);
}

#if defined(RH_TARGET_1043) && RH_TARGET_1043 && \
    defined(RH_P67_BLUR_DIAGNOSTICS) && RH_P67_BLUR_DIAGNOSTICS
#define RH_P67_BLUR_LOG_PATH "/data/offlinelog/resource-hook-p67-blur.log"
#define RH_P67_BLUR_LOG_BYTES 1024u
#define RH_P67_BLUR_LOG_LIMIT 8u
#define RH_P67_BLUR_REPAIR_POLLS 32u
/* Separate from startup/Manager protocols. Only the serialized one-second UI
 * watcher calls this; neither a display callback nor the 50ms refresh retries
 * may do diagnostic I/O. AstroBox log export can read /data/offlinelog. */
static char blur_log[RH_P67_BLUR_LOG_BYTES];
static char blur_logged_key[RH_P67_BLUR_LOG_BYTES];
static char blur_attempted_lifecycle[512];
static uint32_t blur_logged_size, blur_lifecycle_size;
static uint32_t blur_log_epoch, blur_log_attempts, blur_log_publications;
static unsigned blur_log_observed, blur_log_needs_repair;
static uint32_t blur_log_repair_cooldown;
static int blur_bytes_equal(const char *a, const char *b, uint32_t size) {
    uint32_t i;
    for (i = 0; i < size; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int blur_field_is(const char *line, uint32_t size, const char *name) {
    uint32_t i = 0;
    while (name[i]) {
        if (i >= size || line[i] != name[i]) return 0;
        i++;
    }
    return i < size && line[i] == '=';
}
static int blur_counter(const char *line, uint32_t size) {
    uint32_t name_size = 0;
    while (name_size < size && line[name_size] != '=') name_size++;
    /* Provider counters may grow new operation-specific *_calls fields. */
    if (name_size >= 6u && name_size < size &&
        blur_bytes_equal(line+name_size-6u,"_calls",6u)) return 1;
    return blur_field_is(line,size,"frame52") || blur_field_is(line,size,"attempts") ||
        blur_field_is(line,size,"copies") || blur_field_is(line,size,"publishes") ||
        blur_field_is(line,size,"wakes") || blur_field_is(line,size,"draws") ||
        blur_field_is(line,size,"begin_skips") || blur_field_is(line,size,"fences") || blur_field_is(line,size,"capture_calls") ||
        blur_field_is(line,size,"input_allocs") ||
        blur_field_is(line,size,"output_allocs") || blur_field_is(line,size,"destroys") ||
        blur_field_is(line,size,"pm_stays") || blur_field_is(line,size,"pm_relaxes");
}
static int blur_lifecycle(const char *line, uint32_t size) {
    /* Core error gates and wake-budget pending/armed bits can oscillate on
     * every poll without any lifecycle transition. They remain in the stable
     * key, but must not bypass the per-epoch error budget. */
    return blur_field_is(line,size,"source") || blur_field_is(line,size,"root") ||
        blur_field_is(line,size,"root_generation") || blur_field_is(line,size,"display") ||
        blur_field_is(line,size,"config_epoch") ||
        blur_field_is(line,size,"refresh_pending") || blur_field_is(line,size,"cache_dropped") ||
        blur_field_is(line,size,"fonts_done") || blur_field_is(line,size,"calendar_done") ||
        blur_field_is(line,size,"images_done") ||
        blur_field_is(line,size,"background_initialized");
}
/* Keys here are fixed literals, at most 22 bytes; the 48-byte local line also
 * fits a sign and every int32 value. Errors are signed decimal (-1 means -1,
 * not an unsigned word needing two's-complement decoding). */
static int blur_append_module_field(uint32_t *size, const char *name, int32_t value) {
    char line[48], *p = log_text(line,name);
    uint32_t count, i;
    *p++ = '=';
    if (value < 0) *p++ = '-';
    p = result_number(p,value < 0 ? 0u-(uint32_t)value : (uint32_t)value);
    *p++ = '\n';
    count = (uint32_t)(p-line);
    if (count >= sizeof(blur_log)-*size) return 0;
    for (i = 0; i < count; i++) blur_log[(*size)++] = line[i];
    blur_log[*size] = 0;
    return 1;
}
static uint32_t blur_field_number(const char *line, uint32_t size) {
    uint32_t i = 0, value = 0;
    while (i < size && line[i] != '=') i++;
    while (++i < size && line[i] >= '0' && line[i] <= '9')
        value = value * 10u + (uint32_t)(line[i] - '0');
    return value;
}
static void poll_blur_diagnostics(void) {
    char key[RH_P67_BLUR_LOG_BYTES], lifecycle[512];
    uint32_t size, at, end, i, key_size = 0, lifecycle_size = 0;
    uint32_t epoch = 0, publications = 0, used = 0;
    int fd, written, important;
    /* Only the existing one-second UI watch advances this cooldown. No new
     * timer, draw/display I/O, GPU work, or clock reads are involved. */
    if (blur_log_needs_repair && blur_log_repair_cooldown)
        blur_log_repair_cooldown--;
    size = rh_platform_p67_blur_diagnostics(blur_log,sizeof(blur_log));
    /* Never persist an incomplete/truncated observation. All parsing/copying
     * stays inside the snapshot's byte count; no unbounded string operations. */
    if (size < 9u || size >= sizeof(blur_log) ||
        !blur_bytes_equal(blur_log,"P67BLUR1\n",9u) || blur_log[size-1u] != '\n') return;
    /* The core cannot see module-private reload gates. Append them to the same
     * bounded snapshot after the accepted control transaction, never RHST/RHRS.
     * Stage transitions bypass the error budget so a delayed ACK's blocker and
     * eventual progress remain visible even after recurring capture failures. */
    if (!blur_append_module_field(&size,"refresh_pending",(int32_t)refresh_pending) ||
        !blur_append_module_field(&size,"cache_dropped",(int32_t)cache_dropped) ||
        !blur_append_module_field(&size,"fonts_done",(int32_t)fonts_done) ||
        !blur_append_module_field(&size,"calendar_done",(int32_t)calendar_done) ||
        !blur_append_module_field(&size,"images_done",(int32_t)images_done) ||
        !blur_append_module_field(&size,"font_result",font_result) ||
        !blur_append_module_field(&size,"calendar_error",calendar_error) ||
        !blur_append_module_field(&size,"config_error",config_error)) return;
    /* Mapping config_error does not describe backgrounds.cfg parse/stage or
     * commit failures. Keep that coordinator's own state alongside the gates. */
    if (!blur_append_module_field(&size,"background_error",background_error) ||
        !blur_append_module_field(&size,"background_initialized",(int32_t)background_initialized)) return;
    /* A truncated log cannot be mistaken for a complete diagnostic snapshot. */
    if (!blur_append_module_field(&size,"snapshot_complete",1)) return;
    for (at = 9u; at < size; at = end + 1u) {
        for (end = at; end < size && blur_log[end] != '\n'; end++) {}
        if (blur_field_is(blur_log+at,end-at,"config_epoch"))
            epoch = blur_field_number(blur_log+at,end-at);
        if (blur_field_is(blur_log+at,end-at,"publishes"))
            publications = blur_field_number(blur_log+at,end-at);
        if (!blur_counter(blur_log+at,end-at)) {
            for (i = at; i <= end; i++) key[key_size++] = blur_log[i];
        }
        if (blur_lifecycle(blur_log+at,end-at)) {
            if (end-at+1u > sizeof(lifecycle)-lifecycle_size) return;
            for (i = at; i <= end; i++) lifecycle[lifecycle_size++] = blur_log[i];
        }
    }
    if (!blur_log_observed || epoch != blur_log_epoch) {
        blur_log_epoch = epoch;
        blur_log_attempts = 0;
    }
    if (key_size == blur_logged_size &&
        blur_bytes_equal(key,blur_logged_key,key_size)) return;
    important = !blur_log_observed || lifecycle_size != blur_lifecycle_size ||
        !blur_bytes_equal(lifecycle,blur_attempted_lifecycle,lifecycle_size) ||
        publications != blur_log_publications;
    if (!important && blur_log_attempts >= RH_P67_BLUR_LOG_LIMIT &&
        !(blur_log_needs_repair && !blur_log_repair_cooldown)) return;
    /* Limit attempts too: an unavailable log path cannot turn alternating
     * capture errors into endless writes. New publications/root/config/lifecycle
     * transitions bypass the budget, but counters alone never invalidate key. */
    for (i = 0; i < lifecycle_size; i++) blur_attempted_lifecycle[i] = lifecycle[i];
    blur_lifecycle_size = lifecycle_size;
    blur_log_publications = publications;
    blur_log_observed = 1;
    if (blur_log_attempts < RH_P67_BLUR_LOG_LIMIT) blur_log_attempts++;
    /* A post-budget repair attempt, including an open failure, consumes its
     * cooldown. Permanent failures get at most one repair per 32 watch polls. */
    if (blur_log_needs_repair) blur_log_repair_cooldown = RH_P67_BLUR_REPAIR_POLLS;
    fd = rh_platform_open(RH_P67_BLUR_LOG_PATH, RH_FILE_REPLACE);
    if (fd < 0) return;
    /* Best-effort, NOT atomic: replacing the file discards the last snapshot.
     * A failed replacement may temporarily leave a partial file. Invalidate
     * dedup and retain bounded repair eligibility even after attempt eight, so
     * recovered I/O can restore a complete current snapshot without a reload. */
    blur_logged_size = 0;
    blur_log_needs_repair = 1;
    blur_log_repair_cooldown = RH_P67_BLUR_REPAIR_POLLS;
    while (used < size) {
        written = rh_platform_write(fd,blur_log+used,size-used);
        if (written <= 0 || (uint32_t)written > size-used) break;
        used += (uint32_t)written;
    }
    rh_platform_close(fd);
    /* Commit dedup only after a complete write. Short writes are completed in
     * this transaction; failures can retry on a later watch, never change reload. */
    if (used != size) return;
    blur_log_needs_repair = 0;
    blur_log_repair_cooldown = 0;
    for (i = 0; i < key_size; i++) blur_logged_key[i] = key[i];
    blur_logged_size = key_size;
}
#endif

/* One request, coalesced across activations. The temporary UI timer retries only
 * until the selected target's verified retirement/owner stages and dirty-area
 * request complete; a failed adapter never counts as a completed retirement. */
static void refresh_step(unsigned ui_owner) {
    struct rh_mapping_view current, previous;
    struct rh_snapshot *current_pin, *previous_pin, *previous_owner = 0;
    const struct rh_mapping_view *old;
    uint32_t irq;
    irq = rh_platform_lock();
    current_pin = active_snapshot;
    previous_pin = refresh_previous_snapshot;
    if (!refresh_pending || refreshing ||
        (current_pin && current_pin->references == UINT32_MAX) ||
        (previous_pin && previous_pin->references == UINT32_MAX)) {
        rh_platform_unlock(irq);
        return;
    }
    refreshing = 1;
    if (current_pin) current_pin->references++;
    if (previous_pin) previous_pin->references++;
    current = rh_snapshot_view(current_pin);
    previous = rh_snapshot_view(previous_pin);
    old = previous_pin ? &previous : 0;
    rh_platform_unlock(irq);
    if (!rh_platform_redraw_ready()) goto done;
    if (!cache_dropped) {
        int old_rc = old ? rh_platform_retire_mapped_images(old) : 1;
        int current_rc = old_rc >= 0 ? rh_platform_retire_mapped_images(&current) : -1;
        if (old_rc >= 0 && current_rc >= 0) {
            cache_dropped = 1;
            if ((old && old_rc == 0) || current_rc == 0) {
                if (images_dropped != UINT32_MAX) images_dropped++;
            }
        }
    }
    if (cache_dropped && !fonts_done && ui_owner) {
        uint32_t changed = 0;
        int rc = rh_font_reload(&current, &changed);
        font_result = rc;
        font_last_changed = changed;
        if (rc != 1) {
            /* A permanent font error does not stall unrelated image reloads.
             * It remains visible in RHQ1 v6 until an explicit new request. */
            fonts_done = 1;
            if (UINT32_MAX - fonts_retargeted < changed) fonts_retargeted = UINT32_MAX;
            else fonts_retargeted += changed;
        }
    }
    /* Calendar snapshots consume the new background/fonts and must be generated
     * before refreshing their launcher owner. Never invoke native snapshot work
     * from activate(); only serialized UI timer callbacks may run this stage. */
    if (cache_dropped && fonts_done && !calendar_done) {
        if (!calendar_started && calendar_config_error) {
            /* No accepted startup configuration exists. Keep the stock file
             * pass-through and the latched error, but do not block unrelated
             * font/image refreshes behind an impossible calendar render. */
            calendar_error = 0;
            calendar_done = 1;
        } else if (!rh_calendar_pending() && !rh_platform_calendar_affected(old, &current)) calendar_done = 1;
        else if (ui_owner) {
            calendar_error = rh_platform_refresh_calendar();
            if (!calendar_error) calendar_done = 1;
        }
    }
    if (cache_dropped && fonts_done && calendar_done && !images_done &&
        rh_platform_refresh_mapped_images(old, &current) >= 0)
        images_done = 1;
    if (cache_dropped && images_done && fonts_done && calendar_done &&
        rh_platform_request_full_redraw() == 0) {
        int32_t done;
        irq = rh_platform_lock();
        done = refresh_timer;
        if (redraws != UINT32_MAX) redraws++;
        refresh_pending = 0;
        refresh_timer = -1;
        previous_owner = refresh_previous_snapshot;
        refresh_previous_snapshot = 0;
        rh_platform_unlock(irq);
        /* Service timers belong to the module, not a native UI object. Retire
         * the retry even when the watcher completes this transaction. */
        if (done >= 0) delete_timer(done);
    }
done:
    snapshot_release(previous_owner);
    snapshot_release(previous_pin);
    snapshot_release(current_pin);
    irq = rh_platform_lock();
    refreshing = 0;
    rh_platform_unlock(irq);
}
static void refresh_timer_step(const struct canopus_context_v1 *ctx,
                               int32_t timer, void *user_data) {
    (void)ctx;
    (void)user_data;
    if (timer != refresh_timer) return;
    collect_snapshots();
    /* Reentrant firmware I/O must not run a second publisher or mutate the
     * response buffer while the watcher/result writer owns its transaction. */
    if (control_busy || result_busy) return;
    refresh_step(1u);
    publish_current_result();
}
static int ensure_refresh_timer(void) {
    if (refresh_pending && refresh_timer < 0)
        refresh_timer = create_timer(50u, refresh_timer_step);
    return refresh_pending && refresh_timer < 0 ? -2011 : 0;
}
static int request_refresh(void) {
    uint32_t irq = rh_platform_lock();
    if (!refresh_pending) {
        cache_dropped = 0;
        fonts_done = 0;
        font_result = 1;
        images_done = calendar_done = 0;
        refresh_previous_snapshot = 0;
        refresh_pending = 1;
    }
    rh_platform_unlock(irq);
    refresh_step(0u);
    /* The redirect remains resident on allocation failure; do not report a
     * completed activation. A subsequent activate can retry scheduling. */
    return ensure_refresh_timer();
}

static int stock_preview_prefix(const char *path) {
    static const char prefix[] = ".rh_sys/";
    unsigned i;
    if (!path) return 0;
    for (i = 0; i < sizeof(prefix) - 1u; ++i)
        if (!path[i] || path[i] != prefix[i]) return 0;
    return 1;
}
static int rh_wrapper(void *d, const char *p, int m) {
    struct rh_mapping_view view;
    struct rh_snapshot *snapshot;
    uint32_t irq;
    int result;
    /* A distinct LVGL key reads stock preview bytes even when the public path
     * is redirected or cached. Never interpret this namespace for writes or a
     * different driver. The suffix is the original driver's slashless path. */
    if (d == S.driver && m == 2 && S.original && p &&
        stock_preview_prefix(p)) {
        const char *segment = p + 8;
        const char *at = segment;
        if (!*segment || *segment == '/') return -1;
        for (;;) {
            if (*at && *at != '/') {
                if ((unsigned char)*at < 32u || *at == '\\') return -1;
                ++at;
                continue;
            }
            if (at == segment || (at - segment == 1 && segment[0] == '.') ||
                (at - segment == 2 && segment[0] == '.' && segment[1] == '.')) return -1;
            if (!*at) break;
            segment = ++at;
        }
        return S.original(d, p + 8, m);
    }
    irq = rh_platform_lock();
    snapshot = active_snapshot;
    if (snapshot && snapshot->references == UINT32_MAX) {
        rh_platform_unlock(irq);
        return S.original ? S.original(d, p, m) : 0;
    }
    if (snapshot) snapshot->references++;
    view = rh_snapshot_view(snapshot);
    rh_platform_unlock(irq);
    result = 0;
#if !defined(RH_TARGET_1043) || !RH_TARGET_1043
    if (d == S.driver && m == 2 && S.original) {
        const char *derived = rh_calendar_redirect(p);
        char mapped[RH_PATH];
        /* Explicit final-icon mappings keep precedence over the derived icon.
         * Never render or read calendar configuration in this callback. */
        if (derived && rh_resolve_view(&view, RH_CALENDAR_OUTPUT_PATH, mapped) != 1)
            result = S.original(d, derived, m);
    }
#endif
    if (!result || result == -1) result = rh_posix_open_view(&S, &view, d, p, m);
    snapshot_unpin(snapshot, 1u);
    return result;
}
static int config_read(void *cookie, void *out, uint32_t size) {
    return rh_platform_read(*(int *)cookie, out, size);
}
static int read_bounded_file(const char *path, char *out, uint32_t capacity,
                             uint32_t *used) {
    uint32_t total = 0;
    int fd, got, rc = 0;
    char extra;
    if (!path || !out || !capacity || !used) return -1;
    fd = rh_platform_open(path, RH_FILE_READ);
    if (fd < 0) return fd == -RH_ENOENT ? 1 : -1;
    while (total < capacity) {
        got = rh_platform_read(fd, out + total, capacity - total);
        if (got < 0 || (uint32_t)got > capacity - total) { rc = -1; break; }
        if (!got) break;
        total += (uint32_t)got;
    }
    if (!rc && total == capacity) {
        got = rh_platform_read(fd, &extra, 1);
        if (got != 0) rc = -1;
    }
    rh_platform_close(fd);
    if (!rc) *used = total;
    return rc;
}
static int background_stage(struct rh_backgrounds *candidate) {
    char data[RH_PATH * 4 + 128u], path[RH_PATH];
    uint32_t used = 0;
    int rc = manager_config_path("backgrounds.cfg", path);
    if (!rc) rc = read_bounded_file(path, data, sizeof(data), &used);
    candidate->launcher[0] = 0;
    candidate->control_center[0] = 0;
    candidate->notifications[0] = 0;
    candidate->widgets[0] = 0;
    if (rc == 1) return 0; /* Missing file means system background. */
    if (rc || rh_backgrounds_parse(data, used, candidate)) return -2142;
    return rh_platform_backgrounds_stage(candidate);
}
static void background_start(void) {
    struct rh_backgrounds candidate;
    if (background_initialized) return;
    background_error = background_stage(&candidate);
    if (!background_error) {
        background_error = rh_platform_backgrounds_commit(&candidate);
#if RH_NATIVE_MANAGER
        if (!background_error) manager_background_current = candidate;
#endif
    }
    background_initialized = 1;
}
static int valid_reload_signal(const char *signal, uint32_t size) {
    static const char package[] = "ng.lst.corona\t";
    uint32_t i, start = (uint32_t)(sizeof(RH_RELOAD_PREFIX) - 1u);
    if (!signal || size <= start + 1u || size > RH_CONTROL_REQUEST_MAX ||
        signal[size - 1u] != '\n') return 0;
    for (i = 0; i < start; i++) if (signal[i] != RH_RELOAD_PREFIX[i]) return 0;
    /* Accept the original v1 marker and the package-qualified form. */
    if (size >= start + sizeof(package)) {
        uint32_t matched = 1;
        for (i = 0; i < sizeof(package) - 1u; i++)
            if (signal[start + i] != package[i]) { matched = 0; break; }
        if (matched) start += (uint32_t)(sizeof(package) - 1u);
    }
    if (start >= size - 1u) return 0;
    for (i = start; i < size - 1u; i++) {
        char c = signal[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return 0;
    }
    return 1;
}
static int valid_qualified_signal(const char *signal, uint32_t size,
                                  const char *prefix, uint32_t start) {
    uint32_t i;
    if (!signal || size <= start + 1u || size > start + 65u ||
        size > RH_CONTROL_REQUEST_MAX || signal[size - 1u] != '\n') return 0;
    for (i = 0; i < start; i++) if (signal[i] != prefix[i]) return 0;
    for (i = start; i < size - 1u; i++) {
        char c = signal[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return 0;
    }
    return 1;
}
static int valid_status_signal(const char *signal, uint32_t size) {
    return valid_qualified_signal(signal, size, RH_STATUS_PREFIX,
                                  sizeof(RH_STATUS_PREFIX) - 1u);
}
static int valid_reload_v2_signal(const char *signal, uint32_t size) {
    return valid_qualified_signal(signal, size, RH_RELOAD_V2_PREFIX,
                                  sizeof(RH_RELOAD_V2_PREFIX) - 1u);
}
static int response_matches(const char *signal, uint32_t size) {
    uint32_t i;
    if (!response_kind || size != response_signal_size) return 0;
    for (i = 0; i < size; i++) if (signal[i] != response_signal[i]) return 0;
    return 1;
}
static void remember_response_signal(const char *signal, uint32_t size, unsigned kind) {
    uint32_t i;
    for (i = 0; i < size; i++) response_signal[i] = signal[i];
    response_signal_size = size;
    response_kind = kind;
}
static void remember_reload_signal(const char *signal, uint32_t size) {
    uint32_t i;
    for (i = 0; i < size; i++) last_reload_signal[i] = signal[i];
    last_reload_signal_size = size;
    last_reload_signal_valid = 1;
}
static int reload_signal_seen(const char *signal, uint32_t size) {
    uint32_t i;
    if (!last_reload_signal_valid || size != last_reload_signal_size) return 0;
    for (i = 0; i < size; i++) if (signal[i] != last_reload_signal[i]) return 0;
    return 1;
}
/* Manager creates this app-scoped file before sending a revision. Use only
 * O_WRONLY (2): no guessed create/truncate flags or vararg permissions.
 * A fixed-size, checksummed record lets readers reject partial/torn writes. */
#define RH_RESULT_BYTES 256u
/* Worst RHRS2 row: 69 bytes (INT32_MIN/UINT32_MAX), checksum <=11.
 * Reserve 80 for all rows, including a full 128-byte legacy request. */
_Static_assert(RH_CONTROL_REQUEST_MAX + 80u + 11u <= RH_RESULT_BYTES,
               "control response must fit the maximum request and fields");
static char last_result[RH_RESULT_BYTES];
static unsigned last_result_valid, result_written;
static void flush_result(void) {
    uint32_t used=0; int fd,n;
    if(result_busy || !last_result_valid || result_written || !response_kind ||
       !response_matches(last_result,response_signal_size)) return;
    result_busy=1;
    fd=rh_platform_open(RH_CONTROL_RESPONSE_PATH,RH_FILE_REPLACE);
    if(fd<0) { result_busy=0; return; }
    while(used<RH_RESULT_BYTES) {
        n=rh_platform_write(fd,last_result+used,RH_RESULT_BYTES-used);
        if(n<=0 || (uint32_t)n>RH_RESULT_BYTES-used) break;
        used+=(uint32_t)n;
    }
    rh_platform_close(fd);
    result_written=used==RH_RESULT_BYTES;
    result_busy=0;
}
static char *result_number(char *out, uint32_t n) {
    char digits[10]; uint32_t count=0;
    do { digits[count++]=(char)('0'+n%10u); n/=10u; } while(n);
    while(count) *out++=digits[--count];
    return out;
}
static void commit_result(char buffer[RH_RESULT_BYTES], char *p) {
    uint32_t i, hash=2166136261u;
    if(result_busy) return;
    for(i=0;i<(uint32_t)(p-buffer);i++) hash=(hash^(unsigned char)buffer[i])*16777619u;
    p=result_number(p,hash); *p++='\n';
    if(last_result_valid) {
        for(i=0;i<RH_RESULT_BYTES && buffer[i]==last_result[i];i++) {}
        if(i==RH_RESULT_BYTES) { flush_result(); return; }
    }
    for(i=0;i<RH_RESULT_BYTES;i++) last_result[i]=buffer[i];
    last_result_valid=1; result_written=0;
    flush_result();
}
struct control_status_snapshot { int32_t error; uint32_t count, pending; };
static struct control_status_snapshot control_status_snapshot(void) {
    struct control_status_snapshot status;
    uint32_t irq = rh_platform_lock();
    status.error = config_error ? config_error : (quickapp_error ? quickapp_error :
                   (calendar_config_error ? calendar_config_error : calendar_error));
    if (!status.error) status.error = background_error;
    status.count = S.count;
    status.pending = refresh_pending;
    rh_platform_unlock(irq);
    return status;
}
void rh_module_get_status_snapshot(struct rh_module_status_snapshot *out) {
    struct control_status_snapshot status;
    if (!out) return;
    status = control_status_snapshot();
    out->state = !configured ? RH_MGR_STATE_UNAVAILABLE :
        (!S.installed ? RH_MGR_STATE_UNRESPONSIVE :
         (status.error ? RH_MGR_STATE_CONFIG_ERROR : RH_MGR_STATE_RUNNING));
    out->refresh_pending = (uint8_t)(status.pending != 0u);
    out->active_rules = (uint16_t)status.count;
    out->error_code = status.error;
    out->last_font_result = font_result;
    out->last_changed = font_last_changed;
    out->reload_generation = manager_reload_generation;
    out->reload_result = manager_reload_result;
}
static void notify_manager_status(void) {
    struct rh_module_status_snapshot current;
    if (!control_busy && manager_reload_generation && manager_reload_result == 1 && !refresh_pending) {
        struct control_status_snapshot status = control_status_snapshot();
        manager_reload_result = status.error ? status.error : (font_result < 0 ? font_result : 0);
    }
    rh_module_get_status_snapshot(&current);
    if (manager_status_valid && current.state == manager_last_status.state &&
        current.refresh_pending == manager_last_status.refresh_pending &&
        current.active_rules == manager_last_status.active_rules &&
        current.error_code == manager_last_status.error_code &&
        current.last_font_result == manager_last_status.last_font_result &&
        current.last_changed == manager_last_status.last_changed &&
        current.reload_generation == manager_last_status.reload_generation &&
        current.reload_result == manager_last_status.reload_result) return;
    manager_last_status = current;
    manager_status_valid = 1;
    manager_notify(RH_MANAGER_CHANGED_STATUS);
}
static void publish_result(const char *signal, uint32_t size, int32_t result,
                           uint32_t pending, uint32_t changed) {
    char buffer[RH_RESULT_BYTES], *p=buffer;
    uint32_t i;
    unsigned v2;
    struct control_status_snapshot status;
    if(response_kind != RH_REQUEST_RELOAD || !response_matches(signal,size)) return;
    v2 = valid_reload_v2_signal(signal,size);
    if(v2) {
        status = control_status_snapshot();
        pending = status.pending;
    }
    for(i=0;i<RH_RESULT_BYTES;i++) ((volatile char *)buffer)[i]=0;
    for(i=0;i<size;i++) *p++=signal[i];
    p=log_text(p,v2 ? "RHRS2\t" : "RHRS1\t");
    p=result_number(p,v2 ? 1u : 6u); *p++='\t';
    if(result<0) *p++='-';
    p=result_number(p,result<0 ? 0u-(uint32_t)result : (uint32_t)result); *p++='\t';
    p=result_number(p,pending); *p++='\t';
    p=result_number(p,changed);
    if(v2) {
        p=log_text(p,status.error ? "\tconfig_error\t" : "\trunning\t");
        if(status.error<0) *p++='-';
        p=result_number(p,status.error<0 ? 0u-(uint32_t)status.error :
                                         (uint32_t)status.error); *p++='\t';
        p=result_number(p,status.count);
    }
    *p++='\n';
    commit_result(buffer,p);
}
static void publish_status_result(void) {
    char buffer[RH_RESULT_BYTES], *p=buffer;
    uint32_t i, count, pending;
    int32_t error;
    struct control_status_snapshot status;
    if(response_kind != RH_REQUEST_STATUS) return;
    status = control_status_snapshot();
    error=status.error; count=status.count; pending=status.pending;
    for(i=0;i<RH_RESULT_BYTES;i++) ((volatile char *)buffer)[i]=0;
    for(i=0;i<response_signal_size;i++) *p++=response_signal[i];
    p=log_text(p,error ? "RHST1\t1\tconfig_error\t" : "RHST1\t1\trunning\t");
    if(error<0) *p++='-';
    p=result_number(p,error<0 ? 0u-(uint32_t)error : (uint32_t)error); *p++='\t';
    p=result_number(p,count); *p++='\t';
    p=result_number(p,pending); *p++='\n';
    commit_result(buffer,p);
}
static void publish_current_result(void) {
    notify_manager_status();
    if(response_kind == RH_REQUEST_STATUS) {
        publish_status_result();
        return;
    }
    if(!last_reload_signal_valid) return;
    publish_result(last_reload_signal,last_reload_signal_size,font_result,
                   refresh_pending,font_last_changed);
}
static int resolve_quickapp(void *cookie, const char *package, char path[RH_PATH]) {
    (void)cookie;
    return rh_platform_quickapp_icon_path(package, path);
}
/* Called with the watcher's control reservation already held. */
static int poll_control_file(const char *signal, uint32_t signal_size,
                              unsigned reload_request) {
    uint32_t irq;
    struct rh_snapshot *captured = 0, *candidate = 0, *declarations = 0;
    struct rh_snapshot *new_declarations = 0, *old_declarations = 0;
    struct rh_mapping_view captured_view;
    struct rh_calendar_config *calendar_candidate = 0;
    int fd, rc, identical, calendar_affected, revision = 0, quickapp_affected;
    int outcome = CANOPUS_SVC_ERR_BUSY;
    unsigned commit_reserved = 0;
    struct rh_backgrounds background_candidate;
#if RH_NATIVE_MANAGER
    struct rh_calendar_config *calendar_previous = 0;
    unsigned background_committed = 0, calendar_committed = 0;
#endif
    char config_path[RH_PATH];

    /* Active maps may be empty; NULL is the transparent zero-rule snapshot. */
    irq = rh_platform_lock();
    if (refresh_pending || refreshing || !S.installed) {
        rh_platform_unlock(irq);
        return CANOPUS_SVC_ERR_BUSY;
    }
    captured = active_snapshot;
    declarations = quickapp_declarations;
    if ((declarations && declarations->references == UINT32_MAX) ||
        (captured && captured->references == UINT32_MAX)) {
        rh_platform_unlock(irq);
        return CANOPUS_SVC_ERR_BUSY;
    }
    if (captured) captured->references++;
    if (declarations) declarations->references++;
    rh_platform_unlock(irq);

    revision = reload_request && !reload_signal_seen(signal, signal_size);
    if (revision) {
        rc = manager_config_path("mappings.tsv", config_path);
        fd = rc ? rc : rh_platform_open(config_path, RH_FILE_READ);
        if (fd < 0) {
            config_error = -2102;
            outcome = -2102;
            publish_result(signal, signal_size, -2102, 0, 0);
            goto done;
        }
        rc = rh_read_snapshot(config_read, &fd, &snapshot_allocator, &candidate);
        rh_platform_close(fd);
        if (!rc && rh_snapshot_has_quickapps(candidate)) {
            new_declarations = candidate;
            candidate = 0;
            rc = rh_materialize_snapshot(new_declarations, resolve_quickapp, 0,
                                         &snapshot_allocator, &candidate);
        }
    } else {
        /* Installation/reinstallation can change a file key without a manager
         * revision. This timer is the serialized UI owner; never query in open. */
        if (!declarations) {
            /* A completed revision may retake the slot after a status query. */
            publish_current_result();
            goto done;
        }
        rc = rh_materialize_snapshot(declarations, resolve_quickapp, 0,
                                     &snapshot_allocator, &candidate);
    }
    if (rc) {
        int error = rc == -7 ? -2101 : (rc == -9 ? -2105 :
                    (rc == -8 ? -2104 : -2103));
        outcome = error;
        if (revision) {
            config_error = error;
            publish_result(signal, signal_size, error, 0, 0);
        } else {
            quickapp_error = error;
            if (last_reload_signal_valid)
                publish_result(last_reload_signal, last_reload_signal_size, error, 0, 0);
            publish_status_result();
        }
        goto done;
    }

    if (revision) {
        rc = background_stage(&background_candidate);
        if (rc) {
            background_error = rc;
            outcome = rc;
            publish_result(signal, signal_size, rc, 0, 0);
            goto done;
        }
    }
    if (revision) {
        rc = manager_calendar_stage(&calendar_candidate);
        if (rc) {
            calendar_config_error = rc;
            outcome = rc;
            publish_result(signal, signal_size, rc, 0, 0);
            goto done;
        }
    }
    quickapp_error = 0;
    identical = rh_snapshot_equal(captured, candidate);
    if (identical && !revision) {
        /* Recover an error receipt or reclaim the slot after a status query.
         * The response cache deduplicates unchanged completions. */
        publish_current_result();
        goto done;
    }
    captured_view = rh_snapshot_view(captured);
    calendar_affected = revision && identical &&
                        (calendar_candidate != 0 || rh_platform_calendar_affected(0, &captured_view));
    quickapp_affected = revision && identical && new_declarations != 0;
    irq = rh_platform_lock();
    /* A nested activation may have requested a refresh during parsing. Never
     * replace its owner transaction, or publish against a changed generation. */
    if (active_snapshot != captured || quickapp_declarations != declarations ||
        refresh_pending || refreshing) {
        rh_platform_unlock(irq);
        goto done;
    }
    /* Reserve before invoking auxiliary callbacks. A failed background commit
     * must not expose candidate mappings/declarations with the old calendar. */
    refreshing = 1;
    commit_reserved = 1;
    rh_platform_unlock(irq);
    if (revision) {
        background_error = rh_platform_backgrounds_commit(&background_candidate);
        background_initialized = 1;
        if (background_error) {
            outcome = background_error;
            publish_result(signal, signal_size, background_error, refresh_pending, 0);
            goto done;
        }
#if RH_NATIVE_MANAGER
        background_committed = 1;
#endif
    }
    if (revision && calendar_candidate) {
#if RH_NATIVE_MANAGER
        calendar_config_error = rh_calendar_exchange(calendar_candidate, &calendar_previous);
#else
        calendar_config_error = rh_calendar_commit(calendar_candidate);
#endif
        if (calendar_config_error) {
            outcome = calendar_config_error;
            publish_result(signal, signal_size, calendar_config_error, refresh_pending, 0);
            goto done;
        }
        calendar_candidate = 0;
#if RH_NATIVE_MANAGER
        calendar_committed = 1;
#endif
    }
#if RH_NATIVE_MANAGER
    if (revision && manager_candidate_relative) {
        rc = rh_manager_config_commit(manager_candidate_relative);
        if (rc) {
            config_error = rc;
            outcome = rc;
            goto done;
        }
        manager_config_error = 0;
    }
    if (revision) {
        manager_background_current = background_candidate;
        background_committed = calendar_committed = 0;
        rh_calendar_discard(calendar_previous);
        calendar_previous = 0;
        calendar_started = 1;
    }
#endif
    irq = rh_platform_lock();
    if (revision) {
        config_error = calendar_config_error = 0;
        old_declarations = quickapp_declarations;
        quickapp_declarations = new_declarations;
        new_declarations = 0;
    }
    if (!identical) {
        /* Transfer the old active owner to refresh; candidate owns the new
         * active map. Publish all stage flags atomically with the pointer. */
        refresh_previous_snapshot = active_snapshot;
        active_snapshot = candidate;
        S.count = candidate ? candidate->count : 0u;
        candidate = 0;
        cache_dropped = images_done = calendar_done = 0;
        fonts_done = 0;
        font_result = 1;
        font_last_changed = 0;
        refresh_pending = 1;
    } else {
        /* An explicit revision can replace calendar assets at unchanged paths.
         * Regenerate once without retiring unrelated images or republishing the
         * immutable map. The adapter evicts its own exact background keys. */
        if (calendar_affected) {
            cache_dropped = fonts_done = 1;
            images_done = calendar_done = 0;
            refresh_previous_snapshot = 0;
            refresh_pending = 1;
        }
        font_last_changed = 0;
        /* A rejected font transaction may also be retried at this revision. */
        if (font_result < 0) {
            fonts_done = 0;
            font_result = 1;
            cache_dropped = 1;
            images_done = calendar_affected ? 0u : 1u;
            calendar_done = calendar_affected ? 0u : 1u;
            refresh_previous_snapshot = 0;
            refresh_pending = 1;
        }
        /* An explicit same-map revision may replace icon bytes in place. Do not
         * silently acknowledge it without file-cache retirement/owner refresh. */
        if (quickapp_affected) {
            cache_dropped = images_done = 0;
            calendar_done = calendar_affected ? 0u : 1u;
            refresh_previous_snapshot = 0;
            refresh_pending = 1;
        }
    }
    refreshing = 0; commit_reserved = 0;
    rh_platform_unlock(irq);
    if (revision) remember_reload_signal(signal, signal_size);
    outcome = 0;
    if (refresh_pending) {
        refresh_step(1u);
        (void)ensure_refresh_timer();
    }
    publish_current_result();
done:
#if RH_NATIVE_MANAGER
    if (calendar_committed) {
        struct rh_calendar_config *rejected = 0;
        int rollback = rh_calendar_exchange(calendar_previous, &rejected);
        if (!rollback) { calendar_previous = 0; rh_calendar_discard(rejected); }
        else calendar_config_error = rollback;
    }
    if (background_committed) {
        int rollback = rh_platform_backgrounds_commit(&manager_background_current);
        if (rollback) background_error = rollback;
    }
    rh_calendar_discard(calendar_previous);
#endif
    if (commit_reserved) {
        irq = rh_platform_lock(); refreshing = 0; rh_platform_unlock(irq);
    }
    rh_calendar_discard(calendar_candidate);
    snapshot_release(candidate);
    snapshot_release(new_declarations);
    snapshot_release(old_declarations);
    snapshot_release(declarations);
    snapshot_release(captured);
    return outcome;
}
int rh_module_request_reload(uint32_t generation) {
    char signal[RH_CONTROL_REQUEST_MAX], *end;
    struct control_status_snapshot status;
    uint32_t size;
    int rc;
    if (!generation || generation == manager_reload_generation)
        return CANOPUS_SVC_ERR_ARGUMENT;
    if (!module_context || !S.installed) return CANOPUS_SVC_ERR_STATE;
    if (manager_notifying || control_busy || result_busy || refreshing || refresh_pending)
        return CANOPUS_SVC_ERR_BUSY;
    /* The same validated publisher serves both callers. A local revision has
     * no response-slot owner, so it never writes a file receipt. */
    control_busy = 1;
    manager_reload_generation = generation;
    manager_reload_result = 1;
    end = log_text(signal, RH_RELOAD_PREFIX "ng.lst.corona\tnative_");
    end = result_number(end, generation);
    *end++ = '\n';
    size = (uint32_t)(end - signal);
#if RH_NATIVE_MANAGER
    /* Candidate auxiliary configs are staged and committed by the transaction
     * below; do not apply them as startup defaults before pointer publication. */
    if (!manager_candidate_relative) {
#endif
        if (!calendar_started) {
            calendar_config_error = manager_calendar_start();
            if (!calendar_config_error) calendar_started = 1;
        }
        background_start();
#if RH_NATIVE_MANAGER
    }
#endif
    rc = poll_control_file(signal, size, 1u);
    status = control_status_snapshot();
    if (rc)
        manager_reload_result = rc;
    else if (refresh_pending && (rc = ensure_refresh_timer()) < 0)
        manager_reload_result = rc;
    else if (!refresh_pending)
        manager_reload_result = status.error ? status.error : (font_result < 0 ? font_result : 0);
    control_busy = 0;
    notify_manager_status();
    return 0;
}
int rh_module_request_reload_from(uint32_t generation, const char *relative_config_dir) {
#if RH_NATIVE_MANAGER
    char expected[32], *end;
    uint32_t i;
    int rc;
    if (!generation || !relative_config_dir) return CANOPUS_SVC_ERR_ARGUMENT;
    end = log_text(expected, "active/");
    end = result_number(end, generation);
    *end = 0;
    for (i = 0; expected[i] && relative_config_dir[i] == expected[i]; ++i) {}
    if (expected[i] || relative_config_dir[i]) return CANOPUS_SVC_ERR_ARGUMENT;
    if (manager_candidate_relative || manager_notifying || control_busy || result_busy ||
        refreshing || refresh_pending) return CANOPUS_SVC_ERR_BUSY;
    rc = rh_manager_config_candidate(relative_config_dir, manager_candidate_dir);
    if (rc) return rc;
    manager_candidate_relative = relative_config_dir;
    rc = rh_module_request_reload(generation);
    manager_candidate_relative = 0;
    manager_candidate_dir[0] = 0;
    return rc;
#else
    (void)generation;
    (void)relative_config_dir;
    return CANOPUS_SVC_ERR_UNSUPPORTED;
#endif
}
static void watch_step(const struct canopus_context_v1 *ctx,
                       int32_t timer, void *user_data) {
    char signal[RH_CONTROL_REQUEST_MAX];
    uint32_t signal_size = 0, irq;
    unsigned kind = 0, new_status = 0;
    int rc;
    (void)ctx;
    (void)user_data;
    if (timer != watch_timer) return;
    collect_snapshots();
    /* Reserve request observation as well as config I/O against nested timers.
     * Read status before the pending-refresh branch, without reading mappings. */
    irq = rh_platform_lock();
    if (control_busy || result_busy || refreshing || !S.installed) {
        rh_platform_unlock(irq);
        return;
    }
    control_busy = 1;
    rh_platform_unlock(irq);
    if (!calendar_started) {
        calendar_config_error = manager_calendar_start();
        if (!calendar_config_error) calendar_started = 1;
    }
    background_start();
    rh_platform_backgrounds_poll();
    /* Native Manager is event-driven. Retain the old external diagnostic
     * channel only in the legacy host compatibility build, not production. */
    rc = RH_NATIVE_MANAGER ? 1 : read_bounded_file(RH_CONTROL_REQUEST_PATH, signal,
                                                  sizeof(signal), &signal_size);
    if (!rc) {
        if (valid_status_signal(signal, signal_size)) kind = RH_REQUEST_STATUS;
        else if (valid_reload_signal(signal, signal_size) ||
                 valid_reload_v2_signal(signal, signal_size)) kind = RH_REQUEST_RELOAD;
    }
    if (kind) {
        new_status = kind == RH_REQUEST_STATUS && !response_matches(signal,signal_size);
        remember_response_signal(signal,signal_size,kind);
    } else {
        /* Invalid/absent input must not let a delayed old receipt own the slot. */
        response_kind = 0;
    }
    flush_result();
    if (kind == RH_REQUEST_STATUS) publish_status_result();
    if (refresh_pending && !new_status) {
        refresh_step(1u);
        (void)ensure_refresh_timer();
        publish_current_result();
    } else if (!new_status) {
        /* A fresh query performs no config/lookup work. On later ticks the
         * existing autonomous QuickApp watcher continues even if it stays. */
        poll_control_file(signal,signal_size,kind == RH_REQUEST_RELOAD);
    }
    /* Date/language changes and missing-owner startup are independent of
     * mapping revisions. Native firmware may rewrite its file at any time. */
    irq = rh_platform_lock();
    if (!refresh_pending && !new_status && calendar_started) {
        /* Protect mapped caches as well as calendar-local ownership throughout
         * autonomous rendering. Nested activate can request a future refresh,
         * but must not retire source decoders before this render completes. */
        refreshing = 1;
        rh_platform_unlock(irq);
        calendar_error = rh_calendar_poll();
        irq = rh_platform_lock();
        refreshing = 0;
    }
    rh_platform_unlock(irq);
#if defined(RH_TARGET_1043) && RH_TARGET_1043 && \
    defined(RH_P67_BLUR_DIAGNOSTICS) && RH_P67_BLUR_DIAGNOSTICS
    /* Observe accepted control-file commits/config epochs, not the state from
     * before reload. Keep the reservation through best-effort diagnostic I/O. */
    poll_blur_diagnostics();
#endif
    irq = rh_platform_lock();
    control_busy = 0;
    rh_platform_unlock(irq);
    notify_manager_status();
}
static int schedule_watch_timer(void) {
    if (watch_timer >= 0) return 0;
    watch_timer = create_timer(RH_RELOAD_POLL_MS, watch_step);
    return watch_timer < 0 ? -2012 : 0;
}
static int32_t prepare(const struct canopus_context_v1 *c) {
    struct rh_snapshot *candidate = 0, *previous, *declarations = 0, *old_declarations;
    uint32_t irq;
    int fd, rc = 0, error;
    char config_path[RH_PATH];
    rc = bind_context(c);
    if (rc) return rc;
    collect_snapshots();
    startup_record("prepare.begin", 0, 0, (uintptr_t)S.installed);
    if (S.installed) {
        startup_record("prepare.end", -2004, 0, S.count);
        return -2004;
    }
    rc = manager_config_path("mappings.tsv", config_path);
    fd = rc ? rc : rh_platform_open(config_path, RH_FILE_READ);
    error = fd < 0 ? -fd : 0;
    startup_record("config.open", fd, error, 0);
    if (fd < 0) {
        if (error != RH_ENOENT) rc = -2005;
        /* Missing or unreadable configuration must not block other modules.
         * The resident pass-through can accept a later Manager revision. */
    } else {
        rc = rh_read_snapshot(config_read, &fd, &snapshot_allocator, &candidate);
        startup_record("config.alloc", rc == -7 ? -2006 : 0, 0, (uintptr_t)candidate);
        startup_record("config.read", rc, 0, candidate ? candidate->count : 0u);
        rh_platform_close(fd);
        if (!rc && rh_snapshot_has_quickapps(candidate)) {
            declarations = candidate;
            candidate = 0;
            rc = rh_materialize_snapshot(declarations, 0, 0,
                                         &snapshot_allocator, &candidate);
            startup_record("config.materialize", rc, 0, candidate ? candidate->count : 0u);
        }
        if (rc) rc = rc == -7 ? -2006 : -2007;
    }
    if (!rc) {
        irq = rh_platform_lock();
        previous = active_snapshot;
        old_declarations = quickapp_declarations;
        quickapp_declarations = declarations;
        declarations = 0;
        active_snapshot = candidate;
        S.count = candidate ? candidate->count : 0u;
        config_error = quickapp_error = 0;
        configured = 1;
        rh_platform_unlock(irq);
        startup_record("snapshot.alloc", 0, 0, (uintptr_t)candidate);
        snapshot_release(previous);
        snapshot_release(old_declarations);
    } else {
        snapshot_release(candidate);
        /* Keep a previously prepared good map, or the allocation-free NULL
         * snapshot on first startup. Never publish partial rules or rewrite
         * the bad file: Manager can repair it through the normal watcher. */
        irq = rh_platform_lock();
        config_error = rc;
        quickapp_error = 0;
        configured = 1;
        rh_platform_unlock(irq);
        startup_record("config.fallback", rc, error, S.count);
    }
    snapshot_release(declarations);
    startup_record("prepare.end", 0, 0, S.count);
    notify_manager_status();
    return 0;
}
/* Caller holds the publication lock. Only the known original or our own
 * callback may occupy the slot; never detach another module's hook. */
static int install_open_hook(void *driver, rh_open_fn *slot, rh_open_fn expected) {
    const struct canopus_hook_obj *hook = module_context->hook;
    uintptr_t original = 0;
    int32_t id;
    if (S.installed && (S.driver != driver || S.original != expected)) return -1;
    if (*slot == rh_wrapper)
        return S.installed && open_hook >= 0 && open_hook_slot == slot &&
               hook->is_hooked(module_context, (uintptr_t)slot) ? 0 : -1;
    if (open_hook >= 0) {
        if (open_hook_slot != slot) return -1;
        /* A UI reset may restore the native callback without updating the
         * service record. Supervisor cleanup may instead have removed both. */
        if (hook->is_hooked(module_context, (uintptr_t)slot) &&
            hook->uninstall(module_context, open_hook) < 0) return -1;
        open_hook = -1;
    }
    /* Publish the wrapper's native calling contract before the service writes
     * its pointer. The short IRQ lock covers both state and slot publication. */
    S.driver = driver;
    S.original = expected;
    id = hook->install(module_context, (uintptr_t)slot, (uintptr_t)rh_wrapper,
                       &original, CANOPUS_HOOK_POINTER);
    if (id < 0) return -1;
    if (original != (uintptr_t)expected) {
        (void)hook->uninstall(module_context, id);
        return -1;
    }
    open_hook = id;
    open_hook_slot = slot;
    S.installed = 1;
    return 0;
}
/* Before the first successful activation, Supervisor failure cleanup reclaims
 * all MEM blocks. Forget our prepared state before returning that failure, and
 * release our references while the owner can still use the bound service. A
 * resident reactivation keeps its state because Supervisor retains services. */
static void rollback_initial_activation(void) {
    struct rh_snapshot *active, *previous, *declarations;
    uint32_t irq = rh_platform_lock();
    if (open_hook >= 0 && module_context->hook->is_hooked(
            module_context, (uintptr_t)open_hook_slot))
        (void)module_context->hook->uninstall(module_context, open_hook);
    open_hook = -1;
    open_hook_slot = 0;
    active = active_snapshot;
    previous = refresh_previous_snapshot;
    declarations = quickapp_declarations;
    active_snapshot = refresh_previous_snapshot = quickapp_declarations = 0;
    configured = 0;
    S.installed = 0;
    S.count = 0;
    refresh_pending = cache_dropped = images_done = fonts_done = calendar_done = refreshing = 0;
    rh_platform_unlock(irq);
    snapshot_release(active);
    snapshot_release(previous);
    snapshot_release(declarations);
    collect_snapshots();
}
/* Rebinding requires a caller-owned UI transaction. Installing this callback
 * does not restart miwear or tear down font wrappers already held by live
 * pages. Affected cached file sources and verified owners adopt the immutable
 * mapping through the platform adapter; other owners need their own lifecycle. */
static int32_t activate(const struct canopus_context_v1 *c) {
    rh_open_fn *slot;
    uint32_t irq;
    int rc = 0, valid;
    uintptr_t observed = 0, expected;
    unsigned font_restart = 0;
    rc = bind_context(c);
    if (rc) return rc;
    collect_snapshots();
    /* Keep a complete activation together, including the final return code. */
    if (startup_log_used > RH_STARTUP_LOG_BYTES - 1536u) startup_log_used = 0;
    startup_record("activate.begin", 0, 0, (uintptr_t)configured);
    /* Supervisor marks descriptors READY without calling prepare. */
    if (!configured) {
        rc = prepare(c);
        if (rc || !configured) {
            startup_record("activate.end", rc, 0, (uintptr_t)S.installed);
            return rc;
        }
    }
    /* Initial failure makes Supervisor reclaim all MEM, including banks pinned
     * by an in-flight hook. Reserve every fallible timer before publishing the
     * hook so no timer failure can expose those banks to service cleanup. */
    if (!activated_once) {
        int refresh_rc = 0, watch_rc;
        if ((S.count || refresh_pending) && refresh_timer < 0) {
            refresh_timer = create_timer(50u, refresh_timer_step);
            if (refresh_timer < 0) refresh_rc = -2011;
        }
        watch_rc = schedule_watch_timer();
        rc = refresh_rc ? refresh_rc : watch_rc;
#if RH_NATIVE_MANAGER
        if (!rc) {
            int connect_rc = rh_connect_runtime_start(c);
            startup_record("connect.start", connect_rc, 0, 0);
            /* Hook use remains available on older Supervisors without Connect. */
            if (connect_rc != CANOPUS_SVC_ERR_UNSUPPORTED) rc = connect_rc;
        }
#endif
        if (rc) {
            startup_record("refresh.end", refresh_rc, 0, (uintptr_t)refresh_timer);
            startup_record("watch.end", watch_rc, 0, (uintptr_t)watch_timer);
            goto activation_done;
        }
    }
    startup_record("hook.begin", 0, 0, (uintptr_t)active_snapshot);
    /* No allocation or firmware I/O is allowed while publishing the callback.
     * On the single-core target, prevent scheduling between state and slot. */
    irq = rh_platform_lock();
    slot = rh_platform_slot();
    valid = rh_platform_driver_valid();
    expected = (uintptr_t)rh_platform_original();
    if (slot) observed = (uintptr_t)*slot;
    if (!configured || !valid) rc = -2008;
    else if (!slot || (observed != expected && *slot != rh_wrapper)) rc = -2009;
    else {
        font_restart = S.installed && *slot != rh_wrapper;
        if (install_open_hook(rh_platform_driver(), slot, rh_platform_original()))
            rc = -2010;
    }
    rh_platform_unlock(irq);
    startup_record("driver.valid", valid, 0, (uintptr_t)slot);
    startup_record("slot.expected", 0, 0, expected);
    startup_record("slot.observed", 0, 0, observed);
    startup_record("hook.end", rc, 0, (uintptr_t)S.installed);
    if (font_restart) {
        /* Never dereference a font snapshot from the previous UI lifetime. */
        rh_font_reload_disable();
    }
    if (!rc) {
        int refresh_rc, watch_rc;
        startup_record("refresh.begin", 0, 0, S.count);
        refresh_rc = S.count || refresh_pending ? request_refresh() : 0;
        startup_record("refresh.end", refresh_rc, 0, (uintptr_t)refresh_timer);
        startup_record("watch.begin", 0, 0, RH_RELOAD_POLL_MS);
        watch_rc = schedule_watch_timer();
        startup_record("watch.end", watch_rc, 0, (uintptr_t)watch_timer);
        rc = refresh_rc ? refresh_rc : watch_rc;
    }
activation_done:
    /* request_refresh initializes the new transaction status; the observed
     * restart must remain visible until the timer reaches the disabled adapter. */
    if (font_restart) font_result = -2014;
    if (rc) {
        /* Initial activation failures make the Supervisor reclaim services.
         * Clear our IDs before returning, including partial scheduling success;
         * a UI rebind failure must also leave no stale retry/watcher handles. */
        if (refresh_timer >= 0) delete_timer(refresh_timer);
        if (watch_timer >= 0) delete_timer(watch_timer);
        refresh_timer = watch_timer = -1;
    }
    if (rc && !activated_once) {
#if RH_NATIVE_MANAGER
        rh_connect_runtime_stop();
        rh_manager_ui_reset_data();
#endif
        rollback_initial_activation();
    }
    if (!rc) activated_once = 1;
    startup_record("activate.end", rc, 0, (uintptr_t)S.installed);
    notify_manager_status();
    return rc;
}
static int32_t stop(const struct canopus_context_v1 *c) {
    (void)c;
    if (!S.installed) {
#if RH_NATIVE_MANAGER
        rh_connect_runtime_stop();
#endif
    }
    return S.installed ? CANOPUS_RESULT_REBOOT_REQUIRED : 0;
}
static int32_t query(struct canopus_status_writer_v1 *writer) {
    uint32_t irq, installed, count, redirected, fallback, dropped, redrawn, rebuilt, fonts;
    int32_t font_status;
    uint32_t font_pending;
    if (!writer || !writer->buf || writer->state != CANOPUS_STATUS_WRITER_WRITING ||
        writer->used > writer->capacity || writer->capacity - writer->used < 48u)
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
    font_status = font_result;
    font_pending = refresh_pending && !fonts_done;
    rh_platform_unlock(irq);
    if (canopus_status_put_u32(writer, 0x31514852u) ||
        canopus_status_put_u32(writer, 6u) ||
        canopus_status_put_u32(writer, installed) ||
        canopus_status_put_u32(writer, count) ||
        canopus_status_put_u32(writer, redirected) ||
        canopus_status_put_u32(writer, fallback) ||
        canopus_status_put_u32(writer, dropped) ||
        canopus_status_put_u32(writer, redrawn) ||
        canopus_status_put_u32(writer, rebuilt) ||
        canopus_status_put_u32(writer, fonts)) return -1;
    if (canopus_status_put_u32(writer, (uint32_t)font_status) ||
        canopus_status_put_u32(writer, font_pending)) return -1;
    return canopus_status_writer_publish(writer);
}
#if RH_NATIVE_MANAGER
static int manager_quickapp_icon(void *cookie, const char *package, char out[RH_PATH]) {
    (void)cookie;
    return rh_platform_quickapp_icon_path(package, out);
}
static int32_t publish_native_app_stage(const struct canopus_context_v1 *ctx, uint32_t stage) {
    int rc = bind_context(ctx);
    if (rc) return rc;
    if (stage == 1u) return rh_manager_ui_register(ctx);
    if (stage == 2u) return rh_manager_launcher_register(ctx);
    return CANOPUS_SVC_ERR_ARGUMENT;
}
#endif
__attribute__((used)) struct canopus_module_descriptor_v1 canopus_module_descriptor;
static void cp(char *d, const char *s, unsigned n) {
    unsigned i = 0;
    while (i < n && s[i]) { d[i] = s[i]; i++; }
    while (i < n) d[i++] = 0;
}
#if defined(CANOPUS_HOST)
void rh_module_entry(const struct canopus_context_v1 *ctx) {
#else
__attribute__((constructor)) static void rh_module_entry(
    const struct canopus_context_v1 *ctx) {
#endif
    int rc;
#if !defined(CANOPUS_MODULE_STOCK_MODLIB)
    (void)rh_platform_bind_fs(ctx);
#endif
    /* Stock modlib supplies no context. Buffer its entry diagnostics until
     * prepare/activate binds FS; the SDK owns its atomic CMR1 transport. */
    startup_record("ctor.begin", 0, 0, 0);
    canopus_module_descriptor.struct_size = sizeof(canopus_module_descriptor);
    canopus_module_descriptor.abi_major = CANOPUS_ABI_MAJOR;
    canopus_module_descriptor.abi_minor = CANOPUS_ABI_MINOR;
    canopus_module_descriptor.flags = CANOPUS_FLAG_REACTIVATE_AFTER_UI_RESTART;
#if RH_NATIVE_MANAGER
    canopus_module_descriptor.flags |= CANOPUS_FLAG_HAS_NATIVE_APP |
        CANOPUS_FLAG_NATIVE_APP_INTEGRATED | CANOPUS_FLAG_REGISTERS_LAUNCHER_ENTRY |
        CANOPUS_FLAG_REQUIRES_UI_DISPATCHER | CANOPUS_FLAG_APP_UNREGISTER_REBOOT_REQUIRED;
    canopus_module_descriptor.publish_native_app_stage = publish_native_app_stage;
#endif
    cp((char *)canopus_module_descriptor.module_id, RH_MODULE_ID, 32);
    cp((char *)canopus_module_descriptor.module_version, RH_VERSION, 16);
    cp((char *)canopus_module_descriptor.build_id,
       "resource-hook-0.3.0", 32);
    cp((char *)canopus_module_descriptor.target_id, RH_TARGET_ID, 32);
    canopus_module_descriptor.prepare = prepare;
    canopus_module_descriptor.activate = activate;
    canopus_module_descriptor.deactivate = stop;
    canopus_module_descriptor.stop = stop;
    canopus_module_descriptor.query = query;
    startup_record("register.begin", 0, 0, (uintptr_t)&canopus_module_descriptor);
    rc = canopus_module_register(ctx, &canopus_module_descriptor);
    startup_record("register.end", rc, rc < 0 ? -rc : 0, 0);
    startup_record("ctor.end", rc, rc < 0 ? -rc : 0, 0);
}
__attribute__((destructor)) static void dtor(void) { (void)stop(0); }

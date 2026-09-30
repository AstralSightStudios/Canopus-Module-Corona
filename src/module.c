#include "canopus_abi.h"
#include "canopus_module_registration.h"
#include "resource_hook_platform.h"
#if defined(RH_EXPERIMENTAL_FONT_RELOAD) && RH_EXPERIMENTAL_FONT_RELOAD
#include "resource_hook_font_reload.h"
#define RH_FONT_EXPERIMENT 1
#else
#define RH_FONT_EXPERIMENT 0
#endif

#define RH_MODULE_ID "corona"
#define RH_VERSION "0.3.0"
/* Resolve app-scoped control paths using the selected firmware target. */
#define RH_CONTROL_CONFIG RH_CONFIG_PATH
#define RH_CONTROL_SIGNAL RH_RELOAD_SIGNAL_PATH
#define RH_RELOAD_PREFIX "resource-hook-reload-v1\t"
#define RH_RELOAD_SIGNAL_MAX 128u
#define RH_RELOAD_POLL_MS 1000u
#if defined(RH_TARGET_1043) && RH_TARGET_1043
#define RH_TARGET_ID "xiaomi-band-10-pro-3.101.043"
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
static int configured;
static uint32_t images_dropped;
static uint32_t redraws;
/* Reserved RHQ1 v5 field: no forced page rebuilds in the reload path. */
static const uint32_t rebuilds;
static uint32_t fonts_retargeted;
#if RH_FONT_EXPERIMENT
/* 1 = pending/busy, 0 = completed, negative = rejected/failed. */
static int32_t font_result;
static uint32_t font_last_changed;
#endif
static void *refresh_timer, *watch_timer;
static struct rh_snapshot *active_snapshot, *refresh_previous_snapshot;
static unsigned control_busy;
static char last_reload_signal[RH_RELOAD_SIGNAL_MAX];
static uint32_t last_reload_signal_size;
static unsigned last_reload_signal_valid;
static unsigned refresh_pending, cache_dropped, images_done, fonts_done, refreshing;
static void publish_current_result(void);
static char *result_number(char *, uint32_t);

static void *snapshot_alloc(void *cookie, uint32_t size) {
    (void)cookie;
    return rh_platform_alloc(size);
}
static void snapshot_free(void *cookie, void *pointer) {
    (void)cookie;
    rh_platform_free(pointer);
}
static const struct rh_allocator snapshot_allocator = {0, snapshot_alloc, snapshot_free};
/* Active ownership, refresh ownership and open/adapter pins all use the same
 * reference count. The last release detaches under the IRQ lock and frees only
 * after unlocking; a backend callback may keep an old generation alive. */
static void snapshot_release(struct rh_snapshot *snapshot) {
    uint32_t irq;
    unsigned release = 0;
    if (!snapshot) return;
    irq = rh_platform_lock();
    if (snapshot->references && --snapshot->references == 0u) release = 1;
    rh_platform_unlock(irq);
    if (release) rh_free_snapshot(&snapshot_allocator, snapshot);
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
    fd = rh_platform_open(RH_STARTUP_LOG_PATH, RH_STARTUP_LOG_FLAGS);
    if (fd < 0) return;
    while (used < startup_log_used) {
        written = rh_platform_write(fd, startup_log + used, startup_log_used - used);
        if (written <= 0 || (uint32_t)written > startup_log_used - used) break;
        used += (uint32_t)written;
    }
    rh_platform_close(fd);
}

/* Point every registered font family whose file falls under a mapping rule at
 * the themed file. A retargeted entry is re-added at the end of the registry, so
 * the same index is re-examined; a themed path normally no longer matches a
 * rule, which ends the walk. The bound stops a pathological rule set that maps a
 * theme directory back onto itself from looping. */
#if !RH_FONT_EXPERIMENT
static void retarget_fonts(const struct rh_mapping_view *current) {
    char name[RH_PATH], path[RH_PATH], mapped[RH_PATH];
    uint32_t index = 0, guard = 0;
    while (guard++ < RH_RULES * 8u) {
        if (rh_platform_font_path_get(index, name, path)) return;
        if (rh_resolve_view(current, path, mapped) == 1 &&
            rh_platform_font_retarget(index, mapped) == 0) {
            if (fonts_retargeted != UINT32_MAX) fonts_retargeted++;
            continue;
        }
        index++;
    }
}

#endif

/* One request, coalesced across activations. The temporary UI timer retries only
 * until the selected target's verified retirement/owner stages and dirty-area
 * request complete; a failed adapter never counts as a completed retirement. */
static void refresh_step(void *timer, unsigned ui_owner) {
    struct rh_mapping_view current, previous;
    struct rh_snapshot *current_pin, *previous_pin, *previous_owner = 0;
    const struct rh_mapping_view *old;
    uint32_t irq;
    (void)ui_owner;
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
    if (cache_dropped && !images_done && rh_platform_refresh_mapped_images(old, &current) >= 0)
        images_done = 1;
#if RH_FONT_EXPERIMENT
    if (cache_dropped && images_done && !fonts_done && ui_owner) {
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
#else
    /* Legacy path only affects future resolutions and cannot restore paths. */
    if (cache_dropped && !fonts_done) {
        fonts_done = 1;
        retarget_fonts(&current);
    }
#endif
    if (cache_dropped && images_done && fonts_done && rh_platform_request_full_redraw() == 0) {
        void *done;
        irq = rh_platform_lock();
        done = refresh_timer;
        if (redraws != UINT32_MAX) redraws++;
        refresh_pending = 0;
        refresh_timer = 0;
        previous_owner = refresh_previous_snapshot;
        refresh_previous_snapshot = 0;
        rh_platform_unlock(irq);
        /* A watch/manual retry may complete while an old UI timer handle is
         * stale after restart. Delete only the callback's own live timer; any
         * other still-live timer self-deletes when its wrapper next runs. */
        if (done && timer == done) rh_platform_timer_delete(done);
    }
done:
    snapshot_release(previous_owner);
    snapshot_release(previous_pin);
    snapshot_release(current_pin);
    irq = rh_platform_lock();
    refreshing = 0;
    rh_platform_unlock(irq);
}
static void refresh_timer_step(void *timer) {
    if (timer != refresh_timer) {
        if (timer) rh_platform_timer_delete(timer);
        return;
    }
    refresh_step(timer, 1u);
    publish_current_result();
}
static int ensure_refresh_timer(void) {
    if (refresh_pending && !refresh_timer)
        refresh_timer = rh_platform_timer_create(50u, refresh_timer_step);
    return refresh_pending && !refresh_timer ? -2011 : 0;
}
static int request_refresh(void) {
    uint32_t irq = rh_platform_lock();
    if (!refresh_pending) {
        cache_dropped = 0;
        fonts_done = 0;
#if RH_FONT_EXPERIMENT
        font_result = 1;
#endif
        images_done = 0;
        refresh_previous_snapshot = 0;
        refresh_pending = 1;
    }
    rh_platform_unlock(irq);
    refresh_step(0, 0u);
    /* The redirect remains resident on allocation failure; do not report a
     * completed activation. A subsequent activate can retry scheduling. */
    return ensure_refresh_timer();
}

static int rh_wrapper(void *d, const char *p, int m) {
    struct rh_mapping_view view;
    struct rh_snapshot *snapshot;
    uint32_t irq;
    int result;
    irq = rh_platform_lock();
    snapshot = active_snapshot;
    if (snapshot && snapshot->references == UINT32_MAX) {
        rh_platform_unlock(irq);
        return S.original ? S.original(d, p, m) : 0;
    }
    if (snapshot) snapshot->references++;
    view = rh_snapshot_view(snapshot);
    rh_platform_unlock(irq);
    result = rh_posix_open_view(&S, &view, d, p, m);
    snapshot_release(snapshot);
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
    fd = rh_platform_open(path, 1);
    if (fd < 0) return rh_platform_errno() == RH_ENOENT ? 1 : -1;
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
static int valid_reload_signal(const char *signal, uint32_t size) {
    static const char package[] = "ng.lst.corona\t";
    uint32_t i, start = (uint32_t)(sizeof(RH_RELOAD_PREFIX) - 1u);
    if (!signal || size <= start + 1u || size > RH_RELOAD_SIGNAL_MAX ||
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
#define RH_CONTROL_RESULT RH_RELOAD_RESULT_PATH
#define RH_RESULT_BYTES 256u
static char last_result[RH_RESULT_BYTES];
static unsigned last_result_valid, result_written;
static void flush_result(void) {
    uint32_t used=0; int fd,n;
    if(!last_result_valid || result_written) return;
    fd=rh_platform_open(RH_CONTROL_RESULT,2);
    if(fd<0) return;
    while(used<RH_RESULT_BYTES) {
        n=rh_platform_write(fd,last_result+used,RH_RESULT_BYTES-used);
        if(n<=0 || (uint32_t)n>RH_RESULT_BYTES-used) break;
        used+=(uint32_t)n;
    }
    rh_platform_close(fd);
    result_written=used==RH_RESULT_BYTES;
}
static char *result_number(char *out, uint32_t n) {
    char digits[10]; uint32_t count=0;
    do { digits[count++]=(char)('0'+n%10u); n/=10u; } while(n);
    while(count) *out++=digits[--count];
    return out;
}
static void publish_result(const char *signal, uint32_t size, int32_t result,
                           uint32_t pending, uint32_t changed) {
    static const char prefix[]="RHRS1\t";
    char buffer[RH_RESULT_BYTES], *p=buffer;
    uint32_t i, hash=2166136261u;
    if(!size || size>RH_RELOAD_SIGNAL_MAX) return;
    for(i=0;i<RH_RESULT_BYTES;i++) ((volatile char *)buffer)[i]=0;
    for(i=0;i<size;i++) *p++=signal[i];
    for(i=0;i<sizeof(prefix)-1u;i++) *p++=prefix[i];
    p=result_number(p,RH_FONT_EXPERIMENT ? 6u : 5u); *p++='\t';
    if(result<0) *p++='-';
    p=result_number(p,result<0 ? 0u-(uint32_t)result : (uint32_t)result); *p++='\t';
    p=result_number(p,pending); *p++='\t';
    p=result_number(p,changed); *p++='\n';
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
static void publish_current_result(void) {
    if(!last_reload_signal_valid) return;
#if RH_FONT_EXPERIMENT
    publish_result(last_reload_signal,last_reload_signal_size,font_result,
                   refresh_pending,font_last_changed);
#else
    publish_result(last_reload_signal,last_reload_signal_size,0,refresh_pending,0);
#endif
}
static void poll_control_file(void) {
    char signal[RH_RELOAD_SIGNAL_MAX];
    uint32_t signal_size, irq;
    struct rh_snapshot *captured = 0, *candidate = 0;
    int fd, rc, identical;

    /* Reserve the complete transaction before any reentrant firmware I/O.
     * Active maps may be empty; NULL is the transparent zero-rule snapshot. */
    irq = rh_platform_lock();
    if (control_busy || refresh_pending || refreshing || !S.installed) {
        rh_platform_unlock(irq);
        return;
    }
    control_busy = 1;
    captured = active_snapshot;
    if (captured && captured->references == UINT32_MAX) {
        control_busy = 0;
        rh_platform_unlock(irq);
        return;
    }
    if (captured) captured->references++;
    rh_platform_unlock(irq);

    rc = read_bounded_file(RH_CONTROL_SIGNAL, signal, sizeof(signal), &signal_size);
    if (rc || !valid_reload_signal(signal, signal_size) ||
        reload_signal_seen(signal, signal_size)) goto done;
    fd = rh_platform_open(RH_CONTROL_CONFIG, 1);
    if (fd < 0) {
        publish_result(signal, signal_size, -2102, 0, 0);
        goto done;
    }
    rc = rh_read_snapshot(config_read, &fd, &snapshot_allocator, &candidate);
    rh_platform_close(fd);
    if (rc) {
        publish_result(signal, signal_size, rc == -7 ? -2101 : -2103, 0, 0);
        goto done;
    }

    identical = rh_snapshot_equal(captured, candidate);
    irq = rh_platform_lock();
    /* A nested activation may have requested a refresh during parsing. Never
     * replace its owner transaction, or publish against a changed generation. */
    if (active_snapshot != captured || refresh_pending || refreshing) {
        rh_platform_unlock(irq);
        goto done;
    }
    if (!identical) {
        /* Transfer the old active owner to refresh; candidate owns the new
         * active map. Publish all stage flags atomically with the pointer. */
        refresh_previous_snapshot = active_snapshot;
        active_snapshot = candidate;
        S.count = candidate ? candidate->count : 0u;
        candidate = 0;
        cache_dropped = images_done = 0;
        fonts_done = RH_FONT_EXPERIMENT ? 0u : 1u;
#if RH_FONT_EXPERIMENT
        font_result = 1;
        font_last_changed = 0;
#endif
        refresh_pending = 1;
    }
#if RH_FONT_EXPERIMENT
    else {
        font_last_changed = 0;
        /* A new revision may retry a rejected font transaction, but reordered
         * identical mappings never retire images or republish a generation. */
        if (font_result < 0) {
            fonts_done = 0;
            font_result = 1;
            cache_dropped = images_done = 1;
            refresh_previous_snapshot = 0;
            refresh_pending = 1;
        }
    }
#endif
    rh_platform_unlock(irq);
    remember_reload_signal(signal, signal_size);
    if (refresh_pending) {
        refresh_step(0, 1u);
        (void)ensure_refresh_timer();
    }
    publish_current_result();
done:
    snapshot_release(candidate);
    snapshot_release(captured);
    irq = rh_platform_lock();
    control_busy = 0;
    rh_platform_unlock(irq);
}
static void watch_step(void *timer) {
    if (timer != watch_timer) {
        if (timer) rh_platform_timer_delete(timer);
        return;
    }
    flush_result();
    if (refresh_pending) {
        refresh_step(0, 1u);
        (void)ensure_refresh_timer();
        publish_current_result();
        return;
    }
    poll_control_file();
}
static int schedule_watch_timer(void) {
    void *next = rh_platform_timer_create(RH_RELOAD_POLL_MS, watch_step);
    if (!next) return -2012;
    watch_timer = next;
    return 0;
}
static int32_t prepare(const struct canopus_context_v1 *c) {
    struct rh_snapshot *candidate = 0, *previous;
    uint32_t irq;
    int fd, rc = 0, error;
    (void)c;
    startup_record("prepare.begin", 0, 0, (uintptr_t)S.installed);
    if (S.installed) {
        startup_record("prepare.end", -2004, 0, S.count);
        return -2004;
    }
    fd = rh_platform_open(RH_CONTROL_CONFIG, 1);
    error = fd < 0 ? rh_platform_errno() : 0;
    startup_record("config.open", fd, error, 0);
    if (fd < 0) {
        if (error != RH_ENOENT) {
            startup_record("prepare.end", -2005, error, 0);
            return -2005;
        }
        /* Missing configuration starts a resident pass-through without heap
         * storage. A later revision can publish the first nonempty snapshot. */
    } else {
        rc = rh_read_snapshot(config_read, &fd, &snapshot_allocator, &candidate);
        startup_record("config.alloc", rc == -7 ? -2006 : 0, 0, (uintptr_t)candidate);
        startup_record("config.read", rc, 0, candidate ? candidate->count : 0u);
        rh_platform_close(fd);
        if (rc) rc = rc == -7 ? -2006 : -2007;
    }
    if (!rc) {
        irq = rh_platform_lock();
        previous = active_snapshot;
        active_snapshot = candidate;
        S.count = candidate ? candidate->count : 0u;
        configured = 1;
        rh_platform_unlock(irq);
        startup_record("snapshot.alloc", 0, 0, (uintptr_t)candidate);
        snapshot_release(previous);
    }
    startup_record("prepare.end", rc, 0, S.count);
    return rc;
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
#if RH_FONT_EXPERIMENT
    unsigned font_restart = 0;
#endif
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
#if RH_FONT_EXPERIMENT
        font_restart = S.installed && *slot != rh_wrapper;
#endif
        if (rh_reinstall_posix(&S, rh_platform_driver(), slot,
                              rh_platform_original(), rh_wrapper)) rc = -2010;
    }
    rh_platform_unlock(irq);
    startup_record("driver.valid", valid, 0, (uintptr_t)slot);
    startup_record("slot.expected", 0, 0, expected);
    startup_record("slot.observed", 0, 0, observed);
    startup_record("hook.end", rc, 0, (uintptr_t)S.installed);
#if RH_FONT_EXPERIMENT
    if (font_restart) {
        /* Never dereference a font snapshot from the previous UI lifetime. */
        rh_font_reload_disable();
    }
#endif
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
#if RH_FONT_EXPERIMENT
    /* request_refresh initializes the new transaction status; the observed
     * restart must remain visible until the timer reaches the disabled adapter. */
    if (font_restart) font_result = -2014;
#endif
    startup_record("activate.end", rc, 0, (uintptr_t)S.installed);
    return rc;
}
static int32_t stop(const struct canopus_context_v1 *c) {
    (void)c;
    return S.installed ? CANOPUS_RESULT_REBOOT_REQUIRED : 0;
}
static int32_t query(struct canopus_status_writer_v1 *writer) {
    uint32_t irq, installed, count, redirected, fallback, dropped, redrawn, rebuilt, fonts;
#if RH_FONT_EXPERIMENT
    int32_t font_status;
    uint32_t font_pending;
#endif
    if (!writer || !writer->buf || writer->state != CANOPUS_STATUS_WRITER_WRITING ||
        writer->used > writer->capacity || writer->capacity - writer->used < (RH_FONT_EXPERIMENT ? 48u : 40u))
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
#if RH_FONT_EXPERIMENT
    font_status = font_result;
    font_pending = refresh_pending && !fonts_done;
#endif
    rh_platform_unlock(irq);
    if (canopus_status_put_u32(writer, 0x31514852u) ||
        canopus_status_put_u32(writer, RH_FONT_EXPERIMENT ? 6u : 5u) ||
        canopus_status_put_u32(writer, installed) ||
        canopus_status_put_u32(writer, count) ||
        canopus_status_put_u32(writer, redirected) ||
        canopus_status_put_u32(writer, fallback) ||
        canopus_status_put_u32(writer, dropped) ||
        canopus_status_put_u32(writer, redrawn) ||
        canopus_status_put_u32(writer, rebuilt) ||
        canopus_status_put_u32(writer, fonts)) return -1;
#if RH_FONT_EXPERIMENT
    if (canopus_status_put_u32(writer, (uint32_t)font_status) ||
        canopus_status_put_u32(writer, font_pending)) return -1;
#endif
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
    int fd, error, written = -1;
    startup_record("ctor.begin", 0, 0, 0);
    canopus_module_descriptor.struct_size = sizeof(canopus_module_descriptor);
    canopus_module_descriptor.abi_major = 1;
    canopus_module_descriptor.abi_minor = 2;
    canopus_module_descriptor.flags = CANOPUS_FLAG_REACTIVATE_AFTER_UI_RESTART;
    cp((char *)canopus_module_descriptor.module_id, RH_MODULE_ID, 32);
    cp((char *)canopus_module_descriptor.module_version, RH_VERSION, 16);
    cp((char *)canopus_module_descriptor.build_id,
       RH_FONT_EXPERIMENT ? "resource-hook-0.3.0-font-exp" : "resource-hook-0.3.0", 32);
    cp((char *)canopus_module_descriptor.target_id, RH_TARGET_ID, 32);
    canopus_module_descriptor.prepare = prepare;
    canopus_module_descriptor.activate = activate;
    canopus_module_descriptor.deactivate = stop;
    canopus_module_descriptor.stop = stop;
    canopus_module_descriptor.query = query;
    r.magic = CANOPUS_MODULE_REGISTRATION_MAGIC;
    r.descriptor = (uint32_t)(uintptr_t)&canopus_module_descriptor;
    cp((char *)r.module_id, RH_MODULE_ID, 32);
    startup_record("register.begin", 0, 0, r.descriptor);
    fd = rh_platform_open("/dev/canopus", 2);
    error = fd < 0 ? rh_platform_errno() : 0;
    startup_record("register.open", fd, error, 0);
    if (fd >= 0) {
        /* Registration is one atomic device message: never retry short writes.
         * Log errors without changing descriptor/image lifetime semantics. */
        written = rh_platform_write(fd, &r, sizeof(r));
        error = written < 0 ? rh_platform_errno() : 0;
        startup_record("register.write", written, error, sizeof(r));
        rh_platform_close(fd);
    }
    startup_record("ctor.end", written == (int)sizeof(r) ? 0 : -1, error, 0);
}
__attribute__((destructor)) static void dtor(void) { (void)stop(0); }

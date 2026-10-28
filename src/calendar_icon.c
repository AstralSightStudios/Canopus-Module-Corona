#include "resource_hook_calendar_runtime.h"
#include "resource_hook_calendar_native.h"
#include "resource_hook_platform.h"
#include "resource_hook_target.h"

static struct rh_calendar_config *active;
static struct rh_calendar_date generated;
static unsigned initialized, busy, dirty = 1, ready;
#if !defined(RH_TARGET_1043) || !RH_TARGET_1043
static unsigned bank;
#endif

void rh_calendar_discard(struct rh_calendar_config *c) { if (c) rh_platform_free(c); }
int rh_calendar_stage_from(const char *path, struct rh_calendar_config **out) {
    struct rh_calendar_config *c;
    char *text;
    uint32_t used = 0;
    int fd, n, rc = 0;
    if (!path || !out) return -2151;
    *out = 0;
    c = rh_platform_alloc(sizeof(*c));
    if (!c) return -2151;
    rh_calendar_defaults(c);
    fd = rh_platform_open(path, RH_FILE_READ);
    if (fd < 0) {
        if (fd != -RH_ENOENT) { rh_calendar_discard(c); return -2152; }
        *out = c; return 0;
    }
    text = rh_platform_alloc(RH_CALENDAR_CONFIG_BYTES + 1u);
    if (!text) { rh_platform_close(fd); rh_calendar_discard(c); return -2151; }
    while (used <= RH_CALENDAR_CONFIG_BYTES) {
        n = rh_platform_read(fd, text + used, RH_CALENDAR_CONFIG_BYTES + 1u - used);
        if (n < 0 || (uint32_t)n > RH_CALENDAR_CONFIG_BYTES + 1u - used) { rc = -2152; break; }
        if (!n) break;
        used += (uint32_t)n;
        if (used > RH_CALENDAR_CONFIG_BYTES) { rc = -2153; break; }
    }
    rh_platform_close(fd);
    if (!rc && rh_calendar_parse(text, used, c)) rc = -2153;
    if (!rc && !RH_HAS_NATIVE_CALENDAR && c->custom) rc = -2157;
    rh_platform_free(text);
    if (rc) rh_calendar_discard(c); else *out = c;
    return rc;
}
int rh_calendar_stage(struct rh_calendar_config **out) {
    return rh_calendar_stage_from(RH_CALENDAR_CONFIG_PATH, out);
}
int rh_calendar_exchange(struct rh_calendar_config *c, struct rh_calendar_config **previous) {
    struct rh_calendar_config *old;
    uint32_t irq;
    if (!previous || (!RH_HAS_NATIVE_CALENDAR && c && c->custom)) return -2153;
    irq = rh_platform_lock();
    if (busy) { rh_platform_unlock(irq); return -2154; }
    old = active; active = c; initialized = c != 0;
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    dirty = c && c->custom ? 1u : 0u;
    if (!c || !c->custom) ready = 0;
#elif defined(RH_TARGET_P65) && RH_TARGET_P65
    /* Keep the previous FILE bank selected until default restoration drains
     * retained render work. A fresh default has nothing to clean or publish. */
    dirty = c && (c->custom || old) ? 1u : 0u;
    if (!c) ready = 0;
#else
    dirty = 1;
#endif
    rh_platform_unlock(irq);
    *previous = old;
    return 0;
}
int rh_calendar_commit(struct rh_calendar_config *c) {
    struct rh_calendar_config *old = 0;
    int rc;
    if (!c) return -2153;
    rc = rh_calendar_exchange(c, &old);
    if (!rc) rh_calendar_discard(old);
    return rc;
}
int rh_calendar_start(void) {
    struct rh_calendar_config *c = 0;
    int rc;
    if (initialized) return 0;
    rc = rh_calendar_stage(&c);
    if (!rc) rc = rh_calendar_commit(c);
    if (!rc) initialized = 1;
    if (rc) rh_calendar_discard(c);
    return rc;
}
static int same_date(const struct rh_calendar_date *a, const struct rh_calendar_date *b) {
    return a->year == b->year && a->month == b->month &&
           a->day == b->day && a->weekday == b->weekday && a->locale == b->locale;
}
static int generate(unsigned force) {
    struct rh_calendar_date now;
#if !defined(RH_TARGET_1043) || !RH_TARGET_1043
    const char *path;
    unsigned next;
#endif
    int rc;
    uint32_t irq;
    if (!RH_HAS_NATIVE_CALENDAR) return 0;
    if (!initialized || !active || !rh_platform_redraw_ready()) return -2154;
    irq = rh_platform_lock();
    if (busy) { rh_platform_unlock(irq); return -2154; }
    busy = 1;
    rh_platform_unlock(irq);
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    /* On P67 (Band 10 Pro), default calendar icon is already transparent natively.
     * Do not generate or hook when there is no custom configuration. */
    if (!active->custom) {
        if (rh_p67_calendar_reset()) { rc = -2154; goto done; }
        irq = rh_platform_lock();
        ready = dirty = 0;
        rh_platform_unlock(irq);
        rc = 0; goto done;
    }
#endif
#if defined(RH_TARGET_P65) && RH_TARGET_P65
    if (!active->custom) {
        if (!dirty && !ready) { rc = 0; goto done; }
        if (rh_p65_calendar_reset()) { rc = -2154; goto done; }
        /* The logical filename stays unchanged when restoring stock. Retire
         * its derived decoder/header keys before native same-source setters. */
        if (ready && rh_platform_calendar_retire(active)) { rc = -2154; goto done; }
        irq = rh_platform_lock();
        ready = dirty = 0;
        rh_platform_unlock(irq);
        rh_platform_calendar_publish();
        rc = 0; goto done;
    }
#endif
    rc = rh_calendar_native_date(&now);
    if (rc) { rc = -2155; goto done; }
    if (!force && !dirty && ready && same_date(&generated, &now)) {
#if defined(RH_TARGET_1043) && RH_TARGET_1043
        /* An app registered after generation adopts the existing RAM image. */
        rh_platform_calendar_publish();
#endif
        rc = 0; goto done;
    }
#if !defined(RH_TARGET_1043) || !RH_TARGET_1043
    next = ready ? bank ^ 1u : 0u;
    path = next ? RH_CALENDAR_BANK1 : RH_CALENDAR_BANK0;
#endif
    /* Retire target-specific input/output keys, never global caches. Keep
     * the previous RAM/file publication selected throughout rendering. */
    if (rh_platform_calendar_retire(active)) { rc = -2154; goto done; }
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    rc = rh_calendar_native_render(active, &now, 0);
#else
    rc = rh_calendar_native_render(active, &now, path);
#endif
    if (rc) { rc = -2156; goto done; }
    /* Rendering dispatches callbacks. Retire reentrantly cached inputs and
     * (Band 11 only) old file decoders at the publication boundary. */
    if (rh_platform_calendar_retire(active)) {
#if defined(RH_TARGET_1043) && RH_TARGET_1043
        rh_p67_calendar_cancel();
#endif
        rc = -2154; goto done;
    }
    irq = rh_platform_lock();
#if !defined(RH_TARGET_1043) || !RH_TARGET_1043
    bank = next;
#endif
    ready = 1; dirty = 0; generated = now;
    rh_platform_unlock(irq);
    /* P67 binds the prepared RAM descriptor; Band 11 binds the logical key
     * whose reads select the completed independent Alpha file. */
    rh_platform_calendar_publish();
    rc = 0;
done:
    irq = rh_platform_lock(); busy = 0; rh_platform_unlock(irq);
    return rc;
}
int rh_calendar_refresh(void) { return generate(1); }
int rh_calendar_poll(void) { return generate(0); }
int rh_calendar_pending(void) {
    if (!RH_HAS_NATIVE_CALENDAR) return 0;
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    return active && active->custom && (dirty || !ready);
#elif defined(RH_TARGET_P65) && RH_TARGET_P65
    return active && (dirty || (active->custom && !ready));
#else
    return dirty || !ready;
#endif
}
const char *rh_calendar_redirect(const char *path) {
    if (!RH_HAS_NATIVE_CALENDAR) return 0;
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    (void)path;
    return 0; /* P67 publishes a RAM source, never a derived file. */
#else
    static const char logical[] = RH_CALENDAR_OUTPUT_PATH;
    uint32_t i, irq;
    const char *out = 0;
    if (!path) return 0;
    /* The native '/' POSIX driver receives paths WITHOUT the leading slash. */
    for (i = 0; i < sizeof(logical) - 2u; ++i)
        if (!path[i] || path[i] != logical[i + 1u]) return 0;
    if (path[i]) return 0;
    irq = rh_platform_lock();
    if (ready) out = (bank ? RH_CALENDAR_BANK1 : RH_CALENDAR_BANK0) + 1;
    rh_platform_unlock(irq);
    return out;
#endif
}

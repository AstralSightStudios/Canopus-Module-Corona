#include "resource_hook_calendar_runtime.h"
#include "resource_hook_calendar_native.h"
#include "resource_hook_platform.h"

#if defined(RH_TARGET_1043) && RH_TARGET_1043
void rh_p67_calendar_reset(void);
#endif

static struct rh_calendar_config *active;
static struct rh_calendar_date generated;
static unsigned initialized, busy, dirty = 1, ready, bank;

void rh_calendar_discard(struct rh_calendar_config *c) { if (c) rh_platform_free(c); }
int rh_calendar_stage(struct rh_calendar_config **out) {
    struct rh_calendar_config *c;
    char *text;
    uint32_t used = 0;
    int fd, n, rc = 0;
    if (!out) return -2151;
    *out = 0;
    c = rh_platform_alloc(sizeof(*c));
    if (!c) return -2151;
    rh_calendar_defaults(c);
    fd = rh_platform_open(RH_CALENDAR_CONFIG_PATH, 1);
    if (fd < 0) {
        if (rh_platform_errno() != RH_ENOENT) { rh_calendar_discard(c); return -2152; }
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
    rh_platform_free(text);
    if (rc) rh_calendar_discard(c); else *out = c;
    return rc;
}
int rh_calendar_commit(struct rh_calendar_config *c) {
    struct rh_calendar_config *old;
    uint32_t irq;
    if (!c) return -2153;
    irq = rh_platform_lock();
    if (busy) { rh_platform_unlock(irq); return -2154; }
    old = active; active = c; initialized = 1;
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    dirty = c->custom ? 1 : 0;
    if (!c->custom) ready = 0;
#else
    dirty = 1;
#endif
    rh_platform_unlock(irq);
    rh_calendar_discard(old);
    return 0;
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
    const char *path;
    unsigned next;
    int rc;
    uint32_t irq;
    if (!initialized || !active || !rh_platform_redraw_ready()) return -2154;
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    /* On P67 (Band 10 Pro), default calendar icon is already transparent natively.
     * Do not generate or hook when there is no custom configuration. */
    if (!active->custom) {
        rh_p67_calendar_reset();
        irq = rh_platform_lock();
        ready = dirty = 0;
        rh_platform_unlock(irq);
        return 0;
    }
#endif
    irq = rh_platform_lock();
    if (busy) { rh_platform_unlock(irq); return -2154; }
    busy = 1;
    rh_platform_unlock(irq);
    rc = rh_calendar_native_date(&now);
    if (rc) { rc = -2155; goto done; }
    if (!force && !dirty && ready && same_date(&generated, &now)) { rc = 0; goto done; }
    next = ready ? bank ^ 1u : 0u;
    path = next ? RH_CALENDAR_BANK1 : RH_CALENDAR_BANK0;
    /* Retire all three logical/physical keys, never global caches. Keep the
     * old ready bank selected while the other file is being written. */
    if (rh_platform_calendar_retire(active)) { rc = -2154; goto done; }
    rc = rh_calendar_native_render(active, &now, path);
    if (rc) { rc = -2156; goto done; }
    /* Rendering dispatches callbacks. A nested logical read may have cached
     * the old ready bank after the pre-render retirement. Retire again at the
     * publication boundary so the publisher cannot reuse that stale decoder. */
    if (rh_platform_calendar_retire(active)) { rc = -2154; goto done; }
    irq = rh_platform_lock();
    bank = next; ready = 1; dirty = 0; generated = now;
    rh_platform_unlock(irq);
    /* No system GENERATE/NOTIFY call: publication only binds the logical key
     * whose reads select the just-completed independent Alpha file. */
    rh_platform_calendar_publish();
    rc = 0;
done:
    irq = rh_platform_lock(); busy = 0; rh_platform_unlock(irq);
    return rc;
}
int rh_calendar_refresh(void) { return generate(1); }
int rh_calendar_poll(void) { return generate(0); }
int rh_calendar_pending(void) {
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    return active && active->custom && (dirty || !ready);
#else
    return dirty || !ready;
#endif
}
const char *rh_calendar_redirect(const char *path) {
    static const char logical[] = RH_CALENDAR_OUTPUT_PATH;
    uint32_t i, irq;
    const char *out = 0;
    if (!path) return 0;
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    if (!active || !active->custom) return 0;
#endif
    /* The native '/' POSIX driver receives paths WITHOUT the leading slash. */
    for (i = 0; i < sizeof(logical) - 2u; ++i)
        if (!path[i] || path[i] != logical[i + 1u]) return 0;
    if (path[i]) return 0;
    irq = rh_platform_lock();
    if (ready) out = (bank ? RH_CALENDAR_BANK1 : RH_CALENDAR_BANK0) + 1;
    rh_platform_unlock(irq);
    return out;
}

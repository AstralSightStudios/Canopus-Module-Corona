#ifndef RESOURCE_HOOK_CALENDAR_INTERNAL_H
#define RESOURCE_HOOK_CALENDAR_INTERNAL_H
#include "resource_hook_calendar.h"
#include "resource_hook_platform.h"

/* Pure calendar policy and checked file transport shared by the exact native
 * adapters. Clock, font, GPU/cache and publication ABIs remain target-owned. */

static inline int rh_calendar_date_valid(const struct rh_calendar_date *d) {
    int days;
    if (!d || d->year < 1900 || d->year > 9999 || d->month < 1 || d->month > 12 ||
        d->day < 1 || d->weekday < 0 || d->weekday > 6) return 0;
    days = d->month == 2 ? 28 + (!(d->year % 4) &&
           ((d->year % 100) || !(d->year % 400))) :
           (d->month == 4 || d->month == 6 || d->month == 9 || d->month == 11 ? 30 : 31);
    return d->day <= days;
}

static inline void rh_calendar_decimal(char *out, unsigned value) {
    if (value >= 10) *out++ = (char)('0' + value / 10);
    *out++ = (char)('0' + value % 10); *out = 0;
}

static inline int rh_calendar_bounded(const char *s, unsigned limit) {
    unsigned i;
    if (!s) return 0;
    for (i = 0; i < limit; ++i) if (!s[i]) return 1;
    return 0;
}

/* Resolve only the configured context. The renderer validates the date once
 * before preparing all three items. Native defaults, fonts and object creation
 * stay target-owned. No native calls or borrowed file buffers. */
static inline int rh_calendar_item_context(const struct rh_calendar_config *c,
        const struct rh_calendar_date *d, unsigned item, const char **out) {
    const struct rh_calendar_item *p;
    const char *context = 0;
    unsigned index, available, limit;
    if (!c || !d || !out || item >= 3 || c->pool_used > RH_CALENDAR_POOL_BYTES) return -1;
    p = &c->items[item];
    if (p->type == RH_CALENDAR_NONE) { *out = 0; return 0; }
    if (p->type > RH_CALENDAR_IMAGE ||
        (p->align != 1 && p->align != 2 && p->align != 3 && p->align != 9) ||
        p->x < -112 || p->x > 112 || p->y < -112 || p->y > 112 ||
        p->size > 72 || (p->size && p->size < 8) ||
        p->color_native > 1 || p->color > 0xffffffu ||
        (p->count && p->count != rh_calendar_context_count(item))) return -1;
    if (p->count) {
        index = item == 0 ? (unsigned)d->month - 1u :
                item == 1 ? (unsigned)d->day - 1u : (unsigned)d->weekday;
        context = rh_calendar_context(c, item, index);
        if (!context) return -1;
        available = c->pool_used - p->context[index];
        limit = p->type == RH_CALENDAR_TEXT ? 64u : RH_PATH;
        if (available > limit) available = limit;
        if (!rh_calendar_bounded(context, available)) return -1;
    }
    *out = context;
    return 0;
}

static inline int rh_calendar_image_path(char *out, const char *relative) {
    const char *root = RH_APP_FILES_ROOT;
    unsigned n = 0, i = 0;
    const char *prefix = "themes/";
    if (!relative) return -1;
    while (prefix[i]) { if (relative[i] != prefix[i]) return -1; ++i; }
    i = 0;
    while (root[n]) { out[n] = root[n]; ++n; }
    while (relative[i]) {
        if (n + 1 >= RH_PATH) return -1;
        out[n++] = relative[i++];
    }
    out[n] = 0; return 0;
}

static inline int rh_calendar_full_write(int fd, const void *data, uint32_t size) {
    const uint8_t *p = data;
    while (size) {
        int n = rh_platform_write(fd, p, size);
        if (n <= 0 || (uint32_t)n > size) return -1;
        p += (unsigned)n; size -= (unsigned)n;
    }
    return 0;
}

#endif

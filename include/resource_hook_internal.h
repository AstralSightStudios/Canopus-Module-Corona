#ifndef RESOURCE_HOOK_INTERNAL_H
#define RESOURCE_HOOK_INTERNAL_H
#include "resource_hook.h"

static inline const char *rh_snapshot_pool(const struct rh_snapshot *snapshot) {
    return (const char *)(snapshot->rules + snapshot->count);
}
/* Explicit unsigned-byte ordering also handles UTF-8 and arbitrary high bytes. */
static inline int rh_key_compare(const char *a, uint32_t an,
                                  const char *b, uint32_t bn) {
    uint32_t i, n = an < bn ? an : bn;
    for (i = 0; i < n; i++) {
        unsigned char ac = (unsigned char)a[i], bc = (unsigned char)b[i];
        if (ac != bc) return ac < bc ? -1 : 1;
    }
    return an == bn ? 0 : (an < bn ? -1 : 1);
}
#endif

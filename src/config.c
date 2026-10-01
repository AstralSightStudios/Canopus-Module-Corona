#include "resource_hook_internal.h"
#include <stddef.h>
/* Decode one row into a reusable temporary. Compact construction strictly
 * validates each decoded row before measuring it; legacy parsing preserves its
 * existing split between syntax parsing and later whole-rule validation. */
static int next_row(const char *text, uint32_t size, uint32_t *position,
                     struct rh_rule *row, uint32_t *source_length,
                     uint32_t *destination_length, uint8_t *flags) {
    static const char root[] = RH_APP_FILES_ROOT;
    static const char themes[] = "themes/";
    static const char system[] = RH_SYSTEM_DESTINATION;
    while (*position < size) {
        uint32_t start = *position, end, split, i, a, b, prefix = 0;
        int is_system;
        while (*position < size && text[*position] != '\n') {
            if (!text[*position]) return -2;
            (*position)++;
        }
        end = *position;
        if (*position < size) (*position)++;
        if (end > start && text[end-1u] == '\r') end--;
        if (end == start || text[start] == '#') continue;
        split = start;
        while (split < end && text[split] != '\t') split++;
        if (split == end || split == start || split+1u == end) return -3;
        a = split-start; b = end-split-1u;
        if (a >= RH_PATH || b >= RH_PATH) return -4;
        is_system = b == sizeof(system)-1u &&
                    !rh_key_compare(text+split+1u, b, system, sizeof(system)-1u);
        if (!is_system) {
            if (b <= sizeof(themes)-1u ||
                rh_key_compare(text+split+1u, sizeof(themes)-1u,
                               themes, sizeof(themes)-1u)) return -2;
            prefix = sizeof(root)-1u;
            if (prefix+b >= RH_PATH) return -4;
        }
        for (i = 0; i < RH_PATH; i++) {
            row->source[i] = i < a ? text[start+i] : 0;
            row->destination[i] = i < prefix ? root[i] :
                (i < prefix+b ? text[split+1u+i-prefix] : 0);
        }
        *source_length = a;
        *destination_length = is_system ? 0 : b;
        *flags = rh_quickapp_package(row->source) ? RH_INDEX_QUICKAPP :
            (text[split-1u] == '/' ? RH_INDEX_DIRECTORY : 0u);
        if (is_system) *flags |= RH_INDEX_SYSTEM;
        return 1;
    }
    return 0;
}

/* Format: absolute path or @quickapp-icon/package TAB themes/... or @system.
 * Package declarations are resolved separately on the UI owner, never in open. */
int rh_parse_config(const char *text, uint32_t size, struct rh_rule *staging,
                    uint32_t capacity, uint32_t *count) {
    struct rh_rule row;
    uint32_t p = 0, n = 0, a, b;
    uint8_t flags;
    int rc;
    if (!text || !staging || !count || capacity > RH_RULES) return -1;
    while ((rc = next_row(text, size, &p, &row, &a, &b, &flags)) > 0) {
        uint32_t i;
        if (n == capacity) return -3;
        for (i = 0; i < RH_PATH; i++) {
            staging[n].source[i] = row.source[i];
            staging[n].destination[i] = row.destination[i];
        }
        n++;
    }
    if (rc) return rc;
    *count = n;
    return 0;
}

static int read_text(rh_read_fn read, void *cookie, char *text,
                       uint32_t capacity, uint32_t *size) {
    uint32_t used = 0;
    int got;
    char extra;
    while (used < capacity) {
        got = read(cookie, text+used, capacity-used);
        if (got < 0 || (uint32_t)got > capacity-used) return -5;
        if (!got) break;
        used += (uint32_t)got;
    }
    if (used == capacity) {
        got = read(cookie, &extra, 1);
        if (got < 0 || got > 1) return -5;
        if (got) return -6;
    }
    *size = used;
    return 0;
}

int rh_read_staged_config(rh_read_fn read, void *cookie, char *text,
                          uint32_t capacity, struct rh_rule *staging,
                          uint32_t *count) {
    uint32_t used;
    int rc;
    if (!read || !text || !staging || !count ||
        !capacity || capacity > RH_CONFIG_BYTES) return -1;
    rc = read_text(read, cookie, text, capacity, &used);
    if (rc) return rc;
    rc = rh_parse_config(text, used, staging, RH_RULES, count);
    if (rc) return rc;
    return rh_validate_rules(staging, *count);
}

int rh_read_config(struct rh_state *s, rh_read_fn read, void *cookie,
                   char *text, uint32_t capacity, struct rh_rule *staging) {
    uint32_t count;
    int rc;
    if (!s || s->installed || !s->rules || !s->rules_capacity ||
        !read || !text || !staging || !capacity || capacity > RH_CONFIG_BYTES) return -1;
    rc = rh_read_staged_config(read, cookie, text, capacity, staging, &count);
    if (rc) return rc;
    return rh_configure(s, staging, count);
}

static int snapshot_rule_compare(const struct rh_indexed_rule *a,
                                  const struct rh_indexed_rule *b,
                                  const char *pool) {
    return rh_key_compare(pool+a->source_offset, a->source_length,
                          pool+b->source_offset, b->source_length);
}
static void snapshot_sift(struct rh_indexed_rule *rules, uint32_t start,
                           uint32_t n, const char *pool) {
    while (start < n / 2u) {
        uint32_t child = start * 2u + 1u;
        struct rh_indexed_rule tmp;
        if (child+1u < n && snapshot_rule_compare(&rules[child], &rules[child+1u], pool) < 0)
            child++;
        if (snapshot_rule_compare(&rules[start], &rules[child], pool) >= 0) return;
        tmp = rules[start]; rules[start] = rules[child]; rules[child] = tmp;
        start = child;
    }
}
static int snapshot_sort(struct rh_snapshot *snapshot) {
    const char *pool = rh_snapshot_pool(snapshot);
    uint32_t i;
    for (i = snapshot->count / 2u; i > 0; i--)
        snapshot_sift(snapshot->rules, i-1u, snapshot->count, pool);
    for (i = snapshot->count; i > 1; i--) {
        struct rh_indexed_rule tmp = snapshot->rules[0];
        snapshot->rules[0] = snapshot->rules[i-1u]; snapshot->rules[i-1u] = tmp;
        snapshot_sift(snapshot->rules, 0, i-1u, pool);
    }
    for (i = 1; i < snapshot->count; i++)
        if (!snapshot_rule_compare(&snapshot->rules[i-1u], &snapshot->rules[i], pool))
            return -3;
    return 0;
}

int rh_parse_snapshot(const char *text, uint32_t size,
                       const struct rh_allocator *allocator,
                       struct rh_snapshot **out) {
    struct rh_rule row;
    struct rh_snapshot *snapshot;
    uint32_t position = 0, count = 0, pool_bytes = 0, a, b, i, n, used;
    uint32_t allocation_bytes;
    uint8_t flags;
    char *pool;
    int rc;
    if (!text || !allocator || !allocator->alloc || !allocator->free || !out) return -1;
    if (size > RH_CONFIG_BYTES) return -6;
    /* First pass measures actual strings using just one expanded rule. */
    while ((rc = next_row(text, size, &position, &row, &a, &b, &flags)) > 0) {
        uint32_t bytes = a+1u + ((flags & RH_INDEX_SYSTEM) ? 0u : b+1u);
        if (count == RH_RULES) return -3;
        rc = rh_validate_rules(&row, 1);
        if (rc) return rc;
        if (bytes > RH_CONFIG_BYTES+1u-pool_bytes) return -6;
        pool_bytes += bytes;
        count++;
    }
    if (rc) return rc;
    if (!count) { *out = NULL; return 0; }
    allocation_bytes = (uint32_t)sizeof(*snapshot) +
                       count * (uint32_t)sizeof(snapshot->rules[0]) + pool_bytes;
    snapshot = allocator->alloc(allocator->cookie, allocation_bytes);
    if (!snapshot) return -7;
    snapshot->references = 1;
    snapshot->allocation_bytes = allocation_bytes;
    snapshot->pool_bytes = pool_bytes;
    snapshot->count = count;
    pool = (char *)(snapshot->rules + count);
    /* Second pass stores relative ordinary destinations, never the app root.
     * The bounded input must remain immutable for both passes. */
    position = 0; n = 0; used = 0;
    while ((rc = next_row(text, size, &position, &row, &a, &b, &flags)) > 0) {
        struct rh_indexed_rule *indexed;
        uint32_t bytes = a+1u + ((flags & RH_INDEX_SYSTEM) ? 0u : b+1u);
        if (n == count || bytes > pool_bytes-used) { rc = -2; break; }
        indexed = &snapshot->rules[n++];
        indexed->source_offset = (uint16_t)used;
        indexed->source_length = (uint8_t)a;
        indexed->destination_offset = 0;
        indexed->destination_length = (uint8_t)b;
        indexed->flags = flags;
        indexed->reserved = 0;
        for (i = 0; i <= a; i++) pool[used++] = row.source[i];
        if (!(flags & RH_INDEX_SYSTEM)) {
            indexed->destination_offset = (uint16_t)used;
            for (i = 0; i <= b; i++)
                pool[used++] = row.destination[sizeof(RH_APP_FILES_ROOT)-1u+i];
        }
    }
    if (!rc && (n != count || used != pool_bytes)) rc = -2;
    if (!rc) rc = snapshot_sort(snapshot);
    if (rc) { allocator->free(allocator->cookie, snapshot); return rc; }
    *out = snapshot;
    return 0;
}

int rh_read_snapshot(rh_read_fn read, void *cookie,
                      const struct rh_allocator *allocator,
                      struct rh_snapshot **out) {
    char *text;
    uint32_t size;
    int rc;
    if (!read || !allocator || !allocator->alloc || !allocator->free || !out) return -1;
    text = allocator->alloc(allocator->cookie, RH_CONFIG_BYTES);
    if (!text) return -7;
    rc = read_text(read, cookie, text, RH_CONFIG_BYTES, &size);
    if (!rc) rc = rh_parse_snapshot(text, size, allocator, out);
    allocator->free(allocator->cookie, text);
    return rc;
}

void rh_free_snapshot(const struct rh_allocator *allocator, struct rh_snapshot *snapshot) {
    if (snapshot && allocator && allocator->free) allocator->free(allocator->cookie, snapshot);
}

int rh_snapshot_equal(const struct rh_snapshot *a, const struct rh_snapshot *b) {
    const char *ap, *bp;
    uint32_t i;
    if (a == b) return 1;
    if (!a || !b || a->count != b->count) return 0;
    ap = rh_snapshot_pool(a); bp = rh_snapshot_pool(b);
    for (i = 0; i < a->count; i++) {
        const struct rh_indexed_rule *ar = &a->rules[i], *br = &b->rules[i];
        if ((ar->flags & (RH_INDEX_DIRECTORY | RH_INDEX_SYSTEM | RH_INDEX_QUICKAPP)) !=
            (br->flags & (RH_INDEX_DIRECTORY | RH_INDEX_SYSTEM | RH_INDEX_QUICKAPP)) ||
            rh_key_compare(ap+ar->source_offset, ar->source_length,
                           bp+br->source_offset, br->source_length)) return 0;
        if (!(ar->flags & RH_INDEX_SYSTEM) &&
            rh_key_compare(ap+ar->destination_offset, ar->destination_length,
                           bp+br->destination_offset, br->destination_length)) return 0;
    }
    return 1;
}

int rh_snapshot_has_quickapps(const struct rh_snapshot *snapshot) {
    uint32_t i;
    if (snapshot) for (i = 0; i < snapshot->count; i++)
        if (snapshot->rules[i].flags & RH_INDEX_QUICKAPP) return 1;
    return 0;
}

int rh_materialize_snapshot(const struct rh_snapshot *declarations,
                            rh_quickapp_resolver resolve, void *cookie,
                            const struct rh_allocator *allocator,
                            struct rh_snapshot **out) {
    static const char app_root[] =
#if defined(RH_TARGET_1043) && RH_TARGET_1043
        "/data/app/";
#else
        "/data/quickapp/app/";
#endif
    const char *pool;
    char *text;
    uint32_t i, used = 0, capacity;
    int rc = 0;
    struct rh_mapping_view view = rh_snapshot_view(declarations);
    if (!out || !allocator || !allocator->alloc || !allocator->free ||
        rh_validate_view(&view)) return -1;
    if (!declarations) { *out = NULL; return 0; }
    pool = rh_snapshot_pool(declarations);
    capacity = declarations->pool_bytes + declarations->count * (RH_PATH + 2u);
    if (capacity > RH_CONFIG_BYTES) capacity = RH_CONFIG_BYTES;
    text = allocator->alloc(allocator->cookie, capacity);
    if (!text) return -7;
    for (i = 0; i < declarations->count; i++) {
        const struct rh_indexed_rule *rule = &declarations->rules[i];
        const char *source = pool + rule->source_offset;
        const char *destination = (rule->flags & RH_INDEX_SYSTEM) ?
            RH_SYSTEM_DESTINATION : pool + rule->destination_offset;
        uint32_t a = rule->source_length;
        uint32_t b = (rule->flags & RH_INDEX_SYSTEM) ?
            sizeof(RH_SYSTEM_DESTINATION)-1u : rule->destination_length;
        char path[RH_PATH];
        if (rule->flags & RH_INDEX_QUICKAPP) {
            const char *package = rh_quickapp_package(source);
            uint32_t root_length = sizeof(app_root)-1u;
            if (!package) { rc = -2; break; }
            if (!resolve) continue;
            {
                uint32_t k;
                for (k = 0; k < RH_PATH; k++) path[k] = 0;
            }
            rc = resolve(cookie, package, path);
            if (rc < 0) { rc = rc == -2 ? -9 : -8; break; }
            if (!rc) continue;
            if (rc != 1) { rc = -8; break; }
            /* Never trust the resolver's path length. */
            a = 0;
            while (a < RH_PATH && path[a]) a++;
            /* The exact registry record owns this path; its package is not a path component. */
            if (a >= RH_PATH || a < root_length+5u ||
                rh_key_compare(path, root_length, app_root, root_length) ||
                rh_key_compare(path+a-4u, 4u, ".bin", 4u)) { rc = -8; break; }
            source = path;
        }
        if (a+1u+b+1u > capacity-used) { rc = -6; break; }
        {
            uint32_t k;
            for (k = 0; k < a; k++) text[used++] = source[k];
            text[used++] = '\t';
            for (k = 0; k < b; k++) text[used++] = destination[k];
            text[used++] = '\n';
        }
        rc = 0;
    }
    if (!rc) rc = rh_parse_snapshot(text, used, allocator, out);
    allocator->free(allocator->cookie, text);
    return rc;
}

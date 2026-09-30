#include "resource_hook_internal.h"
#include <stddef.h>

static uint32_t length(const char *s) {
    uint32_t n = 0;
    if (!s) return RH_PATH;
    while (n < RH_PATH && s[n]) n++;
    return n;
}
static int prefix(const char *a, const char *b, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) if (!a[i] || a[i] != b[i]) return 0;
    return 1;
}
static int valid_length(const char *s, uint32_t n) {
    uint32_t i, start;
    if (!n || n >= RH_PATH || s[0] != '/') return 0;
    start = 1;
    for (i = 1; i <= n; i++) {
        if (i < n && ((unsigned char)s[i] < 32 || (unsigned char)s[i] == 127 ||
                      s[i] == '\\' || s[i] == ':')) return 0;
        if (i == n || s[i] == '/') {
            uint32_t size = i - start;
            if ((!size && i < n) || (size == 1 && s[start] == '.') ||
                (size == 2 && s[start] == '.' && s[start+1] == '.')) return 0;
            start = i + 1;
        }
    }
    return 1;
}
static int valid(const char *s) {
    return valid_length(s, length(s));
}
static int system_destination(const char *destination) {
    static const char system[] = RH_SYSTEM_DESTINATION;
    return length(destination) == sizeof(system)-1u &&
           prefix(destination, system, sizeof(system)-1u);
}
static int rule_ok(const struct rh_rule *r) {
    static const char root[] = RH_THEME_ROOT;
    uint32_t a = length(r->source), b = length(r->destination);
    if (!valid(r->source)) return 0;
    /* System means leave the exact file on its original firmware path. */
    if (system_destination(r->destination)) return r->source[a-1u] != '/';
    if (!valid(r->destination) || !prefix(r->destination, root, sizeof(root)-1u)) return 0;
    /* Directory rules append a suffix; file rules replace one exact path. */
    return (r->source[a-1u] == '/') == (r->destination[b-1u] == '/');
}

static int legacy_compare(const struct rh_rule *rules, uint16_t a, uint16_t b) {
    return rh_key_compare(rules[a].source, length(rules[a].source),
                          rules[b].source, length(rules[b].source));
}
static void index_sift(uint16_t *order, uint32_t start, uint32_t n,
                        const struct rh_rule *rules) {
    uint32_t child;
    while (start < n / 2u) {
        uint16_t tmp;
        child = start * 2u + 1u;
        if (child + 1u < n && legacy_compare(rules, order[child], order[child+1u]) < 0)
            child++;
        if (legacy_compare(rules, order[start], order[child]) >= 0) return;
        tmp = order[start]; order[start] = order[child]; order[child] = tmp;
        start = child;
    }
}
int rh_validate_rules(const struct rh_rule *r, uint32_t n) {
    uint16_t order[RH_RULES];
    uint32_t i;
    if (n > RH_RULES || (n && !r)) return -1;
    for (i = 0; i < n; i++) {
        if (!rule_ok(&r[i])) return -2;
        order[i] = (uint16_t)i;
    }
    for (i = n / 2u; i > 0; i--) index_sift(order, i-1u, n, r);
    for (i = n; i > 1; i--) {
        uint16_t tmp = order[0]; order[0] = order[i-1u]; order[i-1u] = tmp;
        index_sift(order, 0, i-1u, r);
    }
    for (i = 1; i < n; i++)
        if (!legacy_compare(r, order[i-1u], order[i])) return -3;
    return 0;
}

struct rh_mapping_view rh_rules_view(const struct rh_rule *rules, uint32_t count) {
    struct rh_mapping_view view = {rules, count, NULL};
    return view;
}
struct rh_mapping_view rh_snapshot_view(const struct rh_snapshot *snapshot) {
    struct rh_mapping_view view = {NULL, snapshot ? snapshot->count : 0, snapshot};
    return view;
}
static int snapshot_header_ok(const struct rh_snapshot *snapshot) {
    return snapshot->count && snapshot->count <= RH_RULES &&
           snapshot->pool_bytes <= RH_CONFIG_BYTES + 1u &&
           snapshot->allocation_bytes == sizeof(*snapshot) +
               snapshot->count * sizeof(snapshot->rules[0]) + snapshot->pool_bytes;
}
int rh_validate_view(const struct rh_mapping_view *view) {
    if (!view) return -1;
    if (view->snapshot)
        return !view->rules && view->count == view->snapshot->count &&
               snapshot_header_ok(view->snapshot) ? 0 : -1;
    return rh_validate_rules(view->rules, view->count);
}

static void copy_rules(struct rh_state *s, const struct rh_rule *r, uint32_t n) {
    uint32_t i, k;
    for (i = 0; i < n; i++) for (k = 0; k < RH_PATH; k++) {
        s->rules[i].source[k] = r[i].source[k];
        s->rules[i].destination[k] = r[i].destination[k];
    }
    s->count = n;
}

int rh_configure(struct rh_state *s, const struct rh_rule *r, uint32_t n) {
    int rc;
    if (!s || s->installed || n > RH_RULES || n > s->rules_capacity ||
        (n && (!r || !s->rules))) return -1;
    /* Validate the complete transaction before changing any existing rule. */
    rc = rh_validate_rules(r, n);
    if (rc) return rc;
    copy_rules(s, r, n);
    return 0;
}

static const struct rh_indexed_rule *find_key(const struct rh_snapshot *snapshot,
                                              const char *path, uint32_t n) {
    const char *pool = rh_snapshot_pool(snapshot);
    uint32_t low = 0, high = snapshot->count;
    while (low < high) {
        uint32_t middle = low + (high-low) / 2u;
        const struct rh_indexed_rule *rule = &snapshot->rules[middle];
        int cmp = rh_key_compare(path, n, pool + rule->source_offset, rule->source_length);
        if (!cmp) return rule;
        if (cmp < 0) high = middle;
        else low = middle + 1u;
    }
    return NULL;
}
static int resolve_indexed(const struct rh_snapshot *snapshot, const char *path,
                            uint32_t n, char out[RH_PATH]) {
    static const char root[] = RH_APP_FILES_ROOT;
    const struct rh_indexed_rule *selected = find_key(snapshot, path, n);
    const char *pool = rh_snapshot_pool(snapshot);
    uint32_t i, dst, base = sizeof(root)-1u;
    /* Only slash-terminated ancestors are eligible. The first hit is deepest. */
    for (i = n; !selected && i > 0; i--) {
        if (i < n && path[i-1u] == '/') selected = find_key(snapshot, path, i);
    }
    if (!selected || (selected->flags & RH_INDEX_SYSTEM)) return 0;
    dst = base + selected->destination_length;
    if (dst + n - selected->source_length >= RH_PATH) return -2;
    for (i = 0; i < base; i++) out[i] = root[i];
    for (i = 0; i < selected->destination_length; i++)
        out[base+i] = pool[selected->destination_offset+i];
    for (i = selected->source_length; i <= n; i++)
        out[dst+i-selected->source_length] = path[i];
    return 1;
}

/* Public paths are absolute. This function does no I/O, allocation or recursion. */
int rh_resolve_view(const struct rh_mapping_view *view, const char *path,
                    char out[RH_PATH]) {
    uint32_t i, selected = RH_RULES, best = 0, n = length(path), dst, j;
    if (!view || !out || !valid_length(path, n) || view->count > RH_RULES) return -1;
    if (view->snapshot) {
        if (view->rules || view->count != view->snapshot->count ||
            !snapshot_header_ok(view->snapshot)) return -1;
        return resolve_indexed(view->snapshot, path, n, out);
    }
    if (view->count && !view->rules) return -1;
    for (i = 0; i < view->count; i++) {
        uint32_t size = length(view->rules[i].source);
        int directory;
        if (!size || size >= RH_PATH) continue;
        directory = view->rules[i].source[size - 1u] == '/';
        if (size > best && size <= n && (directory || size == n) &&
            prefix(path, view->rules[i].source, size)) {
            best = size; selected = i;
        }
    }
    if (selected == RH_RULES || system_destination(view->rules[selected].destination)) return 0;
    dst = length(view->rules[selected].destination);
    if (dst + n - best >= RH_PATH) return -2;
    for (j = 0; j < dst; j++) out[j] = view->rules[selected].destination[j];
    for (j = best; j <= n; j++) out[dst+j-best] = path[j];
    return 1;
}

int rh_resolve(const struct rh_state *s, const char *path, char out[RH_PATH]) {
    struct rh_mapping_view view;
    if (!s) return -1;
    view = rh_rules_view(s->rules, s->count);
    return rh_resolve_view(&view, path, out);
}

int rh_open(struct rh_state *s, void *driver, const char *path, int mode) {
    char mapped[RH_PATH];
    int result;
    if (!s || !s->original || !path) return 0;
    if (driver == s->driver && mode == 2 && rh_resolve(s, path, mapped) == 1) {
        result = s->original(driver, mapped, mode);
        /* .139 POSIX backend returns fd+1; zero and -1 are failure sentinels. */
        if (result != 0 && result != -1) {
            if (s->redirected != UINT32_MAX) s->redirected++;
            return result;
        }
        if (s->fallback != UINT32_MAX) s->fallback++;
    }
    return s->original(driver, path, mode);
}

int rh_install(struct rh_state *s, void *d, rh_open_fn *slot, rh_open_fn w) {
    if (!s || !d || !slot || !*slot || !w || *slot == w || s->installed) return -1;
    s->driver = d; s->original = *slot; s->installed = 1;
    *slot = w;
    return 0;
}

/* .139 strips the leading slash before calling its '/' driver's open slot.
 * The saved callback adds it back. Never feed that callback an absolute path.
 * Installation and all calls must be serialized by the UI owner. */
int rh_posix_open_view(struct rh_state *s, const struct rh_mapping_view *view,
                       void *driver, const char *path, int mode) {
    char absolute[RH_PATH], mapped[RH_PATH];
    uint32_t i, n;
    int result;
    if (!s || !s->original || !path) return 0;
    n = length(path);
    if (driver != s->driver || mode != 2 || !n || n >= RH_PATH-1 || path[0] == '/')
        return s->original(driver, path, mode);
    absolute[0] = '/';
    for (i = 0; i <= n; i++) absolute[i+1] = path[i];
    if (rh_resolve_view(view, absolute, mapped) == 1) {
        result = s->original(driver, mapped+1, mode);
        if (result != 0 && result != -1) {
            if (s->redirected != UINT32_MAX) s->redirected++;
            return result;
        }
        if (s->fallback != UINT32_MAX) s->fallback++;
    }
    return s->original(driver, path, mode);
}

int rh_posix_open(struct rh_state *s, void *driver, const char *path, int mode) {
    struct rh_mapping_view view;
    if (!s) return 0;
    view = rh_rules_view(s->rules, s->count);
    return rh_posix_open_view(s, &view, driver, path, mode);
}

int rh_reinstall_posix(struct rh_state *s, void *driver, rh_open_fn *slot,
                       rh_open_fn expected, rh_open_fn wrapper) {
    if (!s || !driver || !slot || !expected || !wrapper || expected == wrapper ||
        (*slot != expected && *slot != wrapper)) return -1;
    if (s->installed && (s->driver != driver || s->original != expected)) return -1;
    if (*slot == wrapper) {
        if (s->driver != driver || s->original != expected) return -1;
        s->installed = 1;
        return 0;
    }
    s->driver = driver; s->original = expected; s->installed = 0;
    *slot = wrapper; s->installed = 1;
    return 0;
}

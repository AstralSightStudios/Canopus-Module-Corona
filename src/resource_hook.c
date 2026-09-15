#include "resource_hook.h"
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
static int valid(const char *s) {
    uint32_t i, start, n = length(s);
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
static int rule_ok(const struct rh_rule *r) {
    static const char root[] = "/data/canopus/themes/";
    uint32_t a = length(r->source), b = length(r->destination);
    return valid(r->source) && valid(r->destination) &&
           r->source[a-1] == '/' && r->destination[b-1] == '/' &&
           prefix(r->destination, root, sizeof(root)-1);
}

int rh_configure(struct rh_state *s, const struct rh_rule *r, uint32_t n) {
    uint32_t i, j, k;
    if (!s || s->installed || n > RH_RULES || (n && !r)) return -1;
    /* Validate the complete transaction before changing any existing rule. */
    for (i = 0; i < n; i++) {
        if (!rule_ok(&r[i])) return -2;
        for (j = 0; j < i; j++) {
            uint32_t a = length(r[i].source);
            if (a == length(r[j].source) && prefix(r[i].source, r[j].source, a)) return -3;
        }
    }
    for (i = 0; i < n; i++) for (k = 0; k < RH_PATH; k++) {
        s->rules[i].source[k] = r[i].source[k];
        s->rules[i].destination[k] = r[i].destination[k];
    }
    s->count = n;
    return 0;
}

/* Public paths are absolute. This function does no I/O, allocation or recursion. */
int rh_resolve(const struct rh_state *s, const char *path, char out[RH_PATH]) {
    uint32_t i, selected = RH_RULES, best = 0, n, dst, j;
    if (!s || !out || !valid(path)) return -1;
    n = length(path);
    for (i = 0; i < s->count; i++) {
        uint32_t size = length(s->rules[i].source);
        if (size > best && size <= n && prefix(path, s->rules[i].source, size)) {
            best = size; selected = i;
        }
    }
    if (selected == RH_RULES) return 0;
    dst = length(s->rules[selected].destination);
    if (dst + n - best >= RH_PATH) return -2;
    for (j = 0; j < dst; j++) out[j] = s->rules[selected].destination[j];
    for (j = best; j <= n; j++) out[dst+j-best] = path[j];
    return 1;
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
int rh_posix_open(struct rh_state *s, void *driver, const char *path, int mode) {
    char absolute[RH_PATH], mapped[RH_PATH];
    uint32_t i, n;
    int result;
    if (!s || !s->original || !path) return 0;
    n = length(path);
    if (driver != s->driver || mode != 2 || !n || n >= RH_PATH-1 || path[0] == '/')
        return s->original(driver, path, mode);
    absolute[0] = '/';
    for (i = 0; i <= n; i++) absolute[i+1] = path[i];
    if (rh_resolve(s, absolute, mapped) == 1) {
        result = s->original(driver, mapped+1, mode);
        if (result != 0 && result != -1) {
            if (s->redirected != UINT32_MAX) s->redirected++;
            return result;
        }
        if (s->fallback != UINT32_MAX) s->fallback++;
    }
    return s->original(driver, path, mode);
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

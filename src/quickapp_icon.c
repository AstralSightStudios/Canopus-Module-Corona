#include "resource_hook_quickapp.h"

#if !(defined(RH_TARGET_1043) && RH_TARGET_1043)
/* Independently recovered from the exact AP IDBs, not a relocation rule.
 * Slot 3 is the active package-name registry; slot 4 is a different registry.
 * See targets/<exact-target>/quickapp-icons.md for instruction fingerprints. */
#define QA_SERVICE_SLOT 0x20084ec4u
#define QA_REGISTRY_SLOT 0x20084ebcu
#if defined(RH_TARGET_155) && RH_TARGET_155
#define QA_SERVICE 0x2ca3da58u
#define QA_LOOKUP 0x0c6a1693u
#else
#define QA_SERVICE 0x2ca3da68u
#define QA_LOOKUP 0x0c6a16a3u
#endif

/* Host memory/native-leaf injection follows the font adapter convention.
 * Production builds have no imports, heap, libc or SDK veneer dependencies. */
#ifdef RH_QUICKAPP_ICON_TEST
extern uint32_t rh_qa_read(uint32_t, unsigned);
extern uint32_t rh_qa_lookup(uint32_t, const char *);
#define word(a) rh_qa_read((a), 4)
#define byte(a) rh_qa_read((a), 1)
#define lookup(a,p) rh_qa_lookup((a),(p))
#else
static uint32_t word(uint32_t a) { return *(volatile const uint32_t *)(uintptr_t)a; }
static uint32_t byte(uint32_t a) { return *(volatile const unsigned char *)(uintptr_t)a; }
#define lookup(a,p) (((uint32_t (*)(const char *))(uintptr_t)(a))(p))
#endif

static int region(uint32_t p, uint32_t n, uint32_t lo, uint32_t hi) {
    return p >= lo && p < hi && n <= hi - p;
}
static int ram(uint32_t p, uint32_t n) {
    return region(p, n, 0x20000000u, 0x20160000u) ||
           region(p, n, 0x3c000000u, 0x3d000000u);
}
static int readable(uint32_t p) {
    return ram(p, 1) || region(p, 1, 0x0c0c0000u, 0x0cd00000u) ||
           region(p, 1, 0x2c0c0000u, 0x2cd00000u);
}
static int string(uint32_t p, char out[RH_PATH]) {
    uint32_t i;
    for (i = 0; i < RH_PATH; ++i) {
        if (!readable(p + i)) return -1;
        out[i] = (char)byte(p + i);
        if (!out[i]) return (int)i;
    }
    return -1;
}
static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static int name_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}
/* Native hashmap lookup asserts on a null root and follows unchecked chains.
 * Preflight precisely the selected bucket with bounded, readable keys/nodes;
 * the owner-task contract keeps this view stable through the native call.
 * The real native lookup still selects the record (we do not substitute a
 * private name map or scan the secondary registry). */
static int registry_valid(uint32_t map, const char *package, char scratch[RH_PATH]) {
    uint32_t count, hash = 5381u, node, steps = 0, i;
    if ((map & 3u) || !ram(map, 4)) return 0;
    count = word(map);
    if (!count || count > 4096u || (count & (count - 1u)) ||
        !ram(map, 4u * (count + 1u))) return 0;
    for (i = 0; package[i]; ++i) hash = hash * 33u + (unsigned char)package[i];
    node = word(map + 4u * (1u + (hash & (count - 1u))));
    while (node) {
        int n;
        if (++steps > 1024u || (node & 3u) || !ram(node, 20)) return 0;
        n = string(word(node + 4u), scratch);
        if (n < 0 || word(node + 8u) != (uint32_t)n + 1u) return 0;
        node = word(node + 16u);
    }
    return 1;
}
/* Absolute, canonical ASCII path. Reject empty/dot/traversal segments and
 * protocol/descriptor sources. Package-root identity is checked separately. */
static int safe_path(const char *s) {
    uint32_t i = 1, start = 1;
    if (s[0] != '/') return 0;
    for (;;) {
        if (s[i] == '/' || !s[i]) {
            uint32_t n = i - start;
            if (!n || (n == 1 && s[start] == '.') ||
                (n == 2 && s[start] == '.' && s[start + 1] == '.')) return 0;
            if (!s[i]) return 1;
            start = i + 1;
        } else if (!name_char((unsigned char)s[i])) return 0;
        ++i;
    }
}
#endif

int rh_platform_quickapp_icon_path(const char *package, char out[RH_PATH]) {
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    (void)package;
    if (out) out[0] = 0;
    /* Different record layout/root and unavailable display ROM: deliberately
     * unsupported, with no service reads or speculative ROM calls. */
    return -2;
#else
    static const char root[] = "/data/quickapp/app/";
    char identity[RH_PATH], path[RH_PATH];
    uint32_t service, map, app, pkg, icon, i, n;
    int length;
    if (!out) return -1;
    out[0] = 0;
    if (!package) return -1;
    for (n = 0; n < RH_PATH && package[n]; ++n)
        if (!name_char((unsigned char)package[n])) return -1;
    if (!n || n == RH_PATH || equal(package, ".") || equal(package, "..")) return -1;
    service = word(QA_SERVICE_SLOT);
    if (!service) return 0;
    /* Never dereference a substituted table or call an unverified callback. */
    if (service != QA_SERVICE || word(QA_SERVICE + 12u) != QA_LOOKUP) return -1;
    map = word(QA_REGISTRY_SLOT);
    if (!map) return 0;
    if (!registry_valid(map, package, identity)) return -1;
    app = lookup(QA_LOOKUP, package);
    if (!app) return 0;
    if ((app & 3u) || !ram(app, 20u)) return -1;
    pkg = word(app + 12u);
    icon = word(app + 16u);
    if (string(pkg, identity) < 0 || !equal(identity, package)) return -1;
    if (!icon) return 0;
    length = string(icon, path);
    if (length < 0) return -1;
    if (!length) return 0;
    if (path[0] != '/') return -2;
    if (!safe_path(path)) return -1;
    for (i = 0; i < sizeof(root) - 1u; ++i)
        if (path[i] != root[i]) return -2;
    /* Verify exact package boundary, not a prefix of another package. */
    if ((uint32_t)length < sizeof(root) - 1u + n + 1u) return -1;
    for (i = 0; i < n; ++i)
        if (path[sizeof(root) - 1u + i] != package[i]) return -1;
    i += sizeof(root) - 1u;
    if (i >= (uint32_t)length || path[i] != '/') return -1;
    if ((uint32_t)length < i + 6u || !equal(path + length - 4, ".bin")) return -2;
    /* Recheck the borrowed owner fields before publishing the independent copy.
     * This detects replacement, not arbitrary races; owner task is mandatory. */
    if (word(QA_SERVICE_SLOT) != service || word(QA_REGISTRY_SLOT) != map ||
        word(app + 12u) != pkg || word(app + 16u) != icon) return -1;
    for (i = 0; i <= (uint32_t)length; ++i) out[i] = path[i];
    return 1;
#endif
}

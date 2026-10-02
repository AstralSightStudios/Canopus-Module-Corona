#include "resource_hook_quickapp.h"

/* Independently recovered from the exact AP IDBs, not a relocation rule.
 * Slot 3 is the active package-name registry; slot 4 is a different registry.
 * See targets/<exact-target>/quickapp-icons.md for instruction fingerprints. */
#if defined(RH_TARGET_1043) && RH_TARGET_1043
#define QA_SERVICE_SLOT 0x200eb658u
#define QA_REGISTRY_SLOT 0x200eb650u
#define QA_SERVICE 0x2cdbb054u
#define QA_LOOKUP 0x0ca69e81u
#define QA_PACKAGE_OFFSET 8u
#define QA_ICON_OFFSET 12u
#define QA_APP_ROOT "/data/app/"
#define QA_FLASH_END 0x0cde8190u
#else
#define QA_SERVICE_SLOT 0x20084ec4u
#define QA_REGISTRY_SLOT 0x20084ebcu
#if defined(RH_TARGET_155) && RH_TARGET_155
#define QA_SERVICE 0x2ca3da58u
#define QA_LOOKUP 0x0c6a1693u
#else
#define QA_SERVICE 0x2ca3da68u
#define QA_LOOKUP 0x0c6a16a3u
#endif
#define QA_PACKAGE_OFFSET 12u
#define QA_ICON_OFFSET 16u
#define QA_APP_ROOT "/data/quickapp/app/"
#define QA_FLASH_END 0x0cd00000u
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
           region(p, n, 0x3c000000u, 0x3d000000u)
#if defined(RH_TARGET_1043) && RH_TARGET_1043
           /* .043 initializes Umem at 0x3c271400 with size 0x01d4ec00
            * (AP 0x0c19d4fc, consumed at 0x0c202f38). Registry nodes,
            * records and strings may occupy the upper part of this heap. */
           || region(p, n, 0x3c271400u, 0x3dfc0000u)
#endif
           ;
}
static int readable(uint32_t p) {
    return ram(p, 1) || region(p, 1, 0x0c0c0000u, QA_FLASH_END) ||
           region(p, 1, 0x2c0c0000u, QA_FLASH_END + 0x20000000u);
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
/* Absolute, canonical byte pathname. Reject empty/dot/traversal segments and
 * protocol/descriptor sources. The native record, not its package spelling,
 * determines the icon pathname beneath the target app root. */
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
        } else {
            unsigned char c = (unsigned char)s[i];
            if (c < 32u || c == 127u || c == ':' || c == '\\') return 0;
        }
        ++i;
    }
}

int rh_platform_quickapp_icon_path(const char *package, char out[RH_PATH]) {
    static const char root[] = QA_APP_ROOT;
    char identity[RH_PATH], path[RH_PATH];
    uint32_t service, map, app, pkg, icon, i, n;
    int length;
    if (!out) return -1;
    out[0] = 0;
    if (!package) return -1;
    /* Opaque registry key, including empty: constrain only byte length and
     * C-string/TSV transport controls, never interpret it as a pathname. */
    for (n = 0; n < RH_PATH && package[n]; ++n) {
        unsigned char c = (unsigned char)package[n];
        if (c < 32u || c == 127u) return -1;
    }
    if (n == RH_PATH) return -1;
    service = word(QA_SERVICE_SLOT);
    if (!service) return 0;
    /* Never dereference a substituted table or call an unverified callback. */
    if (service != QA_SERVICE || word(QA_SERVICE + 12u) != QA_LOOKUP) return -1;
    map = word(QA_REGISTRY_SLOT);
    if (!map) return 0;
    if (!registry_valid(map, package, identity)) return -1;
    app = lookup(QA_LOOKUP, package);
    if (!app) return 0;
    if ((app & 3u) || !ram(app, QA_ICON_OFFSET + 4u)) return -1;
    pkg = word(app + QA_PACKAGE_OFFSET);
    icon = word(app + QA_ICON_OFFSET);
    if (string(pkg, identity) < 0 || !equal(identity, package)) return -1;
    if (!icon) return 0;
    length = string(icon, path);
    if (length < 0) return -1;
    if (!length) return 0;
    if (path[0] != '/') return -2;
    if (!safe_path(path)) return -1;
    for (i = 0; i < sizeof(root) - 1u; ++i)
        if (path[i] != root[i]) return -2;
    /* Trust the exact app record's registered path, not a package directory.
     * Retain a minimum five-byte relative pathname and lowercase BIN suffix. */
    if ((uint32_t)length < sizeof(root) - 1u + 5u ||
        !equal(path + length - 4, ".bin")) return -2;
    /* Recheck the borrowed owner fields before publishing the independent copy.
     * This detects replacement, not arbitrary races; owner task is mandatory. */
    if (word(QA_SERVICE_SLOT) != service || word(QA_REGISTRY_SLOT) != map ||
        word(app + QA_PACKAGE_OFFSET) != pkg || word(app + QA_ICON_OFFSET) != icon) return -1;
    for (i = 0; i <= (uint32_t)length; ++i) out[i] = path[i];
    return 1;
}

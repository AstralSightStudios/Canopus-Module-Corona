#include "resource_hook.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(RH_TARGET_1043) && RH_TARGET_1043
#define APP_ROOT "/data/app/"
#define FOREIGN_ROOT "/data/quickapp/app/"
#else
#define APP_ROOT "/data/quickapp/app/"
#define FOREIGN_ROOT "/data/app/"
#endif
#define PACKAGE "org.example-app"
#define KEY RH_QUICKAPP_ICON_PREFIX PACKAGE
#define ICON APP_ROOT PACKAGE "/install/res/icon.bin"
#define NEW_ICON APP_ROOT PACKAGE "/reinstall/res/icon.bin"

static unsigned live, calls, fail_at;
static void *allocate(void *cookie, uint32_t size) {
    void *p;
    assert(cookie == &live && size);
    if (++calls == fail_at) return NULL;
    p = malloc(size);
    assert(p);
    live++;
    return p;
}
static void release(void *cookie, void *p) {
    assert(cookie == &live && p && live);
    live--;
    free(p);
}
static const struct rh_allocator allocator = {&live, allocate, release};
static struct rh_snapshot *parse(const char *text) {
    struct rh_snapshot *s = NULL;
    assert(!rh_parse_snapshot(text, (uint32_t)strlen(text), &allocator, &s));
    return s;
}
static void dispose(struct rh_snapshot *s) { rh_free_snapshot(&allocator, s); }
static void bad_text(const char *text) {
    struct rh_snapshot *old = parse("/old.bin\tthemes/old.bin\n"), *out = old;
    unsigned before = live;
    assert(rh_parse_snapshot(text, (uint32_t)strlen(text), &allocator, &out) < 0);
    assert(out == old && live == before);
    dispose(old);
}
static void expect_path(const struct rh_snapshot *s, const char *path, const char *expected) {
    struct rh_mapping_view view = rh_snapshot_view(s);
    char out[RH_PATH];
    assert(!rh_validate_view(&view));
    int rc = rh_resolve_view(&view, path, out);
    if (rc != (expected ? 1 : 0))
        fprintf(stderr, "resolve %s: got %d, expected %d\n", path, rc, expected ? 1 : 0);
    assert(rc == (expected ? 1 : 0));
    if (expected) assert(!strcmp(out, expected));
}
struct lookup {
    int result;
    const char *path;
    unsigned calls;
    int unterminated;
};
static int resolve(void *cookie, const char *package, char out[RH_PATH]) {
    struct lookup *l = cookie;
    assert(!strcmp(package, PACKAGE) || !strcmp(package, "org.other"));
    l->calls++;
    if (l->result == 1) {
        if (l->unterminated) memset(out, 'x', RH_PATH);
        else { assert(l->path && strlen(l->path) < RH_PATH); strcpy(out, l->path); }
    }
    return l->result;
}
static void grammar(void) {
    static const char *valid[] = {
        "a.b", "_._", "0.9", "org.example-app", "a.b-c_d.ef", "A.B", "a.b-"
    };
    static const char *invalid[] = {
        "", "a", ".a", "a.", "a..b", "-a.b", "a.-b", "a/b.c", "a.b/",
        "a.b c", "a.b\t", "a.b\n", "a.b\\c", "a.b:c", "a.b@c", "a.b\177", "a.b\200"
    };
    char key[RH_PATH];
    unsigned i;
    assert(!rh_quickapp_package(NULL));
    assert(!rh_quickapp_package("/resource/a.bin"));
    assert(!rh_quickapp_package("@quickapp-iconic/a.b"));
    for (i = 0; i < sizeof(valid)/sizeof(valid[0]); i++) {
        snprintf(key, sizeof(key), "%s%s", RH_QUICKAPP_ICON_PREFIX, valid[i]);
        assert(rh_quickapp_package(key) == key + strlen(RH_QUICKAPP_ICON_PREFIX));
    }
    for (i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        snprintf(key, sizeof(key), "%s%s", RH_QUICKAPP_ICON_PREFIX, invalid[i]);
        assert(!rh_quickapp_package(key));
    }
    strcpy(key, RH_QUICKAPP_ICON_PREFIX "a.");
    i = (unsigned)strlen(RH_QUICKAPP_ICON_PREFIX);
    memset(key+i+2, 'x', RH_QUICKAPP_PACKAGE_MAX-2);
    key[i+RH_QUICKAPP_PACKAGE_MAX] = 0;
    assert(rh_quickapp_package(key));
    key[i+RH_QUICKAPP_PACKAGE_MAX] = 'x';
    key[i+RH_QUICKAPP_PACKAGE_MAX+1] = 0;
    assert(!rh_quickapp_package(key));
}
static void declarations(void) {
    static const char *invalid[] = {
        "@quickapp-icon/a\tthemes/icon.bin\n",
        "@quickapp-icon/a..b\tthemes/icon.bin\n",
        "@quickapp-icon/a.b/\tthemes/icon.bin\n",
        KEY "\tthemes/icons/\n", KEY "\tthemes/icon.png\n",
        KEY "\tthemes/icon.BIN\n", KEY "\tthemes/icon.bin/\n",
        KEY "\tthemes/../icon.bin\n", KEY "\tthemes/a//icon.bin\n",
        KEY "\tthemes/a\\icon.bin\n", KEY "\tthemes/a:icon.bin\n",
        KEY "\t" RH_THEME_ROOT "icon.bin\n",
        KEY "\t@system/\n", KEY "\t@system\textra\n",
        KEY "\tthemes/a.bin\n" KEY "\tthemes/b.bin\n"
    };
    struct rh_snapshot *s = parse("# icons\r\n" KEY "\tthemes/icons/theme.bin\r\n"
        "@quickapp-icon/org.other\t@system\n/resource/\tthemes/base/\n");
    struct rh_snapshot *out = s;
    unsigned i, before = live;
    assert(s && s->count == 3 && rh_snapshot_has_quickapps(s));
    assert(!rh_snapshot_has_quickapps(NULL));
    {
        struct rh_mapping_view view = rh_snapshot_view(s);
        char mapped[RH_PATH];
        assert(rh_resolve_view(&view, KEY, mapped) != 1);
    } /* Declarations are never native open keys. */
    expect_path(s, ICON, NULL);
    for (i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) bad_text(invalid[i]);
    fail_at = calls+1;
    assert(rh_parse_snapshot(KEY "\tthemes/icon.bin\n", (uint32_t)strlen(KEY "\tthemes/icon.bin\n"),
                             &allocator, &out) == -7);
    assert(out == s && live == before);
    fail_at = 0;
    dispose(s);
}
static void materialization(void) {
    const char text[] = "/resource/\tthemes/base/\n" KEY "\tthemes/quick/icon.bin\n";
    struct rh_snapshot *decls = parse(text), *out = NULL, *old, *again = NULL;
    struct lookup l = {0, ICON, 0, 0};
    unsigned before, i;
    assert(!rh_materialize_snapshot(decls, NULL, NULL, &allocator, &out));
    assert(out && out->count == 1 && !rh_snapshot_has_quickapps(out));
    expect_path(out, "/resource/icon.bin", RH_THEME_ROOT "base/icon.bin");
    {
        struct rh_mapping_view view = rh_snapshot_view(out);
        char mapped[RH_PATH];
        assert(rh_resolve_view(&view, KEY, mapped) != 1);
    }
    expect_path(out, ICON, NULL);
    old = out;
    assert(!rh_materialize_snapshot(decls, resolve, &l, &allocator, &again));
    assert(l.calls == 1 && rh_snapshot_equal(old, again));
    dispose(again);
    l.result = 1;
    assert(!rh_materialize_snapshot(decls, resolve, &l, &allocator, &out));
    assert(out != old && out->count == 2 && !rh_snapshot_has_quickapps(out));
    expect_path(out, ICON, RH_THEME_ROOT "quick/icon.bin");
    expect_path(out, ICON "/extra", NULL); /* Exact, never a directory mapping. */
    {
        struct rh_mapping_view view = rh_snapshot_view(out);
        char mapped[RH_PATH];
        assert(rh_resolve_view(&view, KEY, mapped) != 1);
    }
    assert(decls->count == 2 && rh_snapshot_has_quickapps(decls));
    assert(!rh_materialize_snapshot(decls, resolve, &l, &allocator, &again));
    assert(rh_snapshot_equal(out, again));
    dispose(again);
    l.path = NEW_ICON;
    assert(!rh_materialize_snapshot(decls, resolve, &l, &allocator, &again));
    assert(!rh_snapshot_equal(out, again));
    expect_path(again, ICON, NULL);
    expect_path(again, NEW_ICON, RH_THEME_ROOT "quick/icon.bin");
    dispose(again);
    l.result = 0;
    assert(!rh_materialize_snapshot(decls, resolve, &l, &allocator, &again));
    assert(rh_snapshot_equal(old, again));
    dispose(again);
    before = live;
    l.result = -1; again = out;
    assert(rh_materialize_snapshot(decls, resolve, &l, &allocator, &again) < 0);
    assert(again == out && live == before);
    l.result = -2;
    assert(rh_materialize_snapshot(decls, resolve, &l, &allocator, &again) == -9);
    assert(again == out && live == before);
    l.result = 1; l.path = ICON;
    for (i = 1; i <= 2; i++) {
        fail_at = calls+i;
        assert(rh_materialize_snapshot(decls, resolve, &l, &allocator, &again) == -7);
        assert(again == out && live == before);
    }
    fail_at = 0;
    dispose(old); dispose(out); dispose(decls);
    decls = parse(KEY "\tthemes/icon.bin\n"); out = decls;
    assert(!rh_materialize_snapshot(decls, NULL, NULL, &allocator, &out) && !out);
    assert(!rh_materialize_snapshot(decls, resolve, &(struct lookup){0, NULL, 0, 0}, &allocator, &out) && !out);
    dispose(decls);
}
static void paths_and_conflicts(void) {
    static const char *bad_paths[] = {
        "", "data/quickapp/app/" PACKAGE "/icon.bin", "/resource/icon.bin",
        FOREIGN_ROOT PACKAGE "/icon.bin", APP_ROOT "org.other/icon.bin",
        APP_ROOT PACKAGE "-evil/icon.bin", APP_ROOT PACKAGE "/../icon.bin",
        APP_ROOT PACKAGE "/./icon.bin", APP_ROOT PACKAGE "//icon.bin",
        APP_ROOT PACKAGE "/res/a\\b.bin", APP_ROOT PACKAGE "/res/a:b.bin",
        APP_ROOT PACKAGE "/res/a\177.bin",
        APP_ROOT PACKAGE "/res/icon.png", APP_ROOT PACKAGE "/res/icon.BIN",
        APP_ROOT PACKAGE "/res/icon.bin/", APP_ROOT PACKAGE "/res/"
    };
    struct rh_snapshot *decls = parse(KEY "\tthemes/icon.bin\n"), *old = parse("/old.bin\tthemes/old.bin\n"), *out = old;
    struct lookup l = {1, NULL, 0, 0};
    unsigned i, before = live;
    char text[1024], longest[RH_PATH];
    for (i = 0; i < sizeof(bad_paths)/sizeof(bad_paths[0]); i++) {
        l.path = bad_paths[i];
        int rc = rh_materialize_snapshot(decls, resolve, &l, &allocator, &out);
        if (rc >= 0) fprintf(stderr, "unsafe resolver path accepted: %s\n", l.path);
        assert(rc < 0);
        assert(out == old && live == before);
    }
    l.unterminated = 1;
    assert(rh_materialize_snapshot(decls, resolve, &l, &allocator, &out) < 0);
    assert(out == old && live == before);
    l.unterminated = 0;
    /* A maximum-length exact key is legal; no slash suffix may be appended. */
    strcpy(longest, APP_ROOT PACKAGE "/");
    memset(longest + strlen(longest), 'x', RH_PATH-1u-4u-strlen(longest));
    memcpy(longest + RH_PATH-1u-4u, ".bin", 5);
    l.path = longest; out = NULL;
    assert(!rh_materialize_snapshot(decls, resolve, &l, &allocator, &out));
    expect_path(out, longest, RH_THEME_ROOT "icon.bin");
    dispose(out); out = old;
    l.path = ICON;
    dispose(decls);
    snprintf(text, sizeof(text), "%s\tthemes/native.bin\n%s\tthemes/icon.bin\n", ICON, KEY);
    decls = parse(text); before = live;
    assert(rh_materialize_snapshot(decls, resolve, &l, &allocator, &out) < 0);
    assert(out == old && live == before);
    dispose(decls);
    /* Even identical destinations must not hide a native key collision. */
    snprintf(text, sizeof(text), "%s\tthemes/icon.bin\n%s\tthemes/icon.bin\n", ICON, KEY);
    decls = parse(text); before = live;
    assert(rh_materialize_snapshot(decls, resolve, &l, &allocator, &out) < 0);
    assert(out == old && live == before);
    dispose(decls);
    decls = parse(KEY "\t@system\n" APP_ROOT PACKAGE "/\tthemes/broad/\n");
    out = NULL;
    assert(!rh_materialize_snapshot(decls, resolve, &l, &allocator, &out));
    expect_path(out, ICON, NULL);
    expect_path(out, APP_ROOT PACKAGE "/other.bin", RH_THEME_ROOT "broad/other.bin");
    dispose(out); dispose(decls); dispose(old);
}
static void limits(void) {
    static char text[RH_CONFIG_BYTES+1];
    struct rh_snapshot *s = NULL, *old, *out;
    uint32_t used = 0, i;
    for (i = 0; i < RH_RULES; i++)
        used += (uint32_t)snprintf(text+used, sizeof(text)-used,
            "@quickapp-icon/org.app%u\tthemes/icon.bin\n", (unsigned)i);
    assert(!rh_parse_snapshot(text, used, &allocator, &s));
    assert(s && s->count == RH_RULES && rh_snapshot_has_quickapps(s));
    old = s; out = old;
    used += (uint32_t)snprintf(text+used, sizeof(text)-used,
        "@quickapp-icon/org.extra\tthemes/icon.bin\n");
    assert(rh_parse_snapshot(text, used, &allocator, &out) < 0 && out == old);
    memset(text, '#', sizeof(text));
    assert(!rh_parse_snapshot(text, RH_CONFIG_BYTES, &allocator, &out) && !out);
    out = old;
    assert(rh_parse_snapshot(text, RH_CONFIG_BYTES+1, &allocator, &out) == -6 && out == old);
    dispose(old);
}
int main(void) {
    grammar(); declarations(); materialization(); paths_and_conflicts(); limits();
    assert(!live);
    puts("QuickApp package grammar, declarations, safe BIN materialization, conflicts and limits passed");
    return 0;
}

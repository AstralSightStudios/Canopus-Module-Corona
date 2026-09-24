#include "resource_hook.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct rh_state state, snapshot;
static struct rh_rule rules[RH_RULES];
static char text[RH_CONFIG_BYTES + 1], buffer[RH_CONFIG_BYTES];
struct input { const char *p; uint32_t left; int oversized; };
static int read_input(void *cookie, void *out, uint32_t size) {
    struct input *r = cookie;
    if (r->oversized) return (int)size + 1;
    if (size > r->left) size = r->left;
    memcpy(out, r->p, size); r->p += size; r->left -= size;
    return (int)size;
}
static void rule(const char *source, const char *destination) {
    memset(rules, 0, sizeof(rules));
    strcpy(rules[0].source, source);
    strcpy(rules[0].destination, destination);
}
static void rejected(const char *source, const char *destination) {
    rule(source, destination);
    snapshot = state;
    assert(rh_configure(&state, rules, 1) < 0);
    assert(!memcmp(&state, &snapshot, sizeof(state)));
}
static int calls, fail;
static int original(void *d, const char *p, int mode) {
    (void)d; (void)mode; calls++;
    assert(p);
    return fail && strstr(p, "themes/") ? 0 : 1;
}
static int wrapper(void *d, const char *p, int mode) { (void)d; (void)p; (void)mode; return 0; }
int main(void) {
    char out[RH_PATH], path[RH_PATH];
    uint32_t count, i;
    struct input input;
    rh_open_fn slot = original, second = original;
    rule("/resource/", RH_THEME_ROOT "base/");
    assert(!rh_configure(&state, rules, 1));
    rejected("relative/", RH_THEME_ROOT "base/");
    rejected("/resource", RH_THEME_ROOT "base/");
    rejected("/resource/", "/data/quickapp/files/ng.lst.corona/themes-evil/");
    rejected("/resource/", RH_THEME_ROOT "../outside/");
    rejected("/resource/", RH_THEME_ROOT "./base/");
    rejected("/resource/", RH_THEME_ROOT "/base/");
    rejected("/resource/", RH_THEME_ROOT "a\\b/");
    rejected("/resource/", RH_THEME_ROOT "a:b/");
    rejected("/resource/", RH_THEME_ROOT "a\177/");
    rejected("/resource/", "");
    rejected("", RH_THEME_ROOT "base/");
    for (i = 1; i < 32; i++) {
        strcpy(path, "/resource/x/"); path[10] = (char)i;
        rejected(path, RH_THEME_ROOT "base/");
    }
    rule("/resource/", RH_THEME_ROOT "base/");
    rules[1] = rules[0];
    assert(rh_configure(&state, rules, 2) == -3);
    assert(rh_configure(&state, rules, RH_RULES + 1) < 0);
    for (i = 0; i < RH_RULES; i++) {
        snprintf(rules[i].source, RH_PATH, "/resource/%u/", i);
        strcpy(rules[i].destination, RH_THEME_ROOT "base/");
    }
    assert(!rh_configure(&state, rules, RH_RULES));
    assert(rh_resolve(&state, "/resource/63/x", out) == 1);
    rule("/", RH_THEME_ROOT "base/");
    assert(!rh_configure(&state, rules, 1));
    assert(rh_resolve(&state, "/resource/x", out) == 1);
    assert(!strcmp(out, RH_THEME_ROOT "base/resource/x"));
    memset(path, 'x', sizeof(path)); path[0] = '/'; path[RH_PATH-1] = 0;
    assert(rh_resolve(&state, path, out) == -2);
    assert(rh_resolve(&state, NULL, out) < 0);
    assert(rh_resolve(&state, "/x", NULL) < 0);

    count = 99;
    assert(!rh_parse_config("/a/\t" RH_THEME_ROOT "a/", sizeof("/a/\t" RH_THEME_ROOT "a/") - 1, rules, RH_RULES, &count));
    assert(count == 1 && !rh_configure(&state, rules, count));
    count = 99;
    assert(rh_parse_config("#\0x", 3, rules, RH_RULES, &count) < 0 && count == 99);
    assert(rh_parse_config("/a/\t\n", 5, rules, RH_RULES, &count) < 0);
    assert(rh_parse_config("\t/d/\n", 5, rules, RH_RULES, &count) < 0);
    assert(!rh_parse_config("/a/\t" RH_THEME_ROOT "a/\textra", sizeof("/a/\t" RH_THEME_ROOT "a/\textra") - 1, rules, RH_RULES, &count));
    assert(rh_configure(&state, rules, count) < 0);

    memset(text, '#', sizeof(text));
    input = (struct input){text, RH_CONFIG_BYTES, 0};
    assert(!rh_read_config(&state, read_input, &input, buffer, sizeof(buffer), rules));
    assert(state.count == 0);
    snapshot = state;
    input = (struct input){text, RH_CONFIG_BYTES + 1, 0};
    assert(rh_read_config(&state, read_input, &input, buffer, sizeof(buffer), rules) == -6);
    assert(!memcmp(&state, &snapshot, sizeof(state)));
    input = (struct input){text, 0, 1};
    assert(rh_read_config(&state, read_input, &input, buffer, sizeof(buffer), rules) == -5);
    assert(!memcmp(&state, &snapshot, sizeof(state)));

    rule("/resource/", RH_THEME_ROOT "base/");
    assert(!rh_configure(&state, rules, 1));
    assert(!rh_reinstall_posix(&state, &state, &slot, original, wrapper));
    assert(rh_reinstall_posix(&state, &snapshot, &second, original, wrapper) < 0);
    assert(second == original && slot == wrapper);
    state.redirected = UINT32_MAX; state.fallback = UINT32_MAX;
    assert(rh_posix_open(&state, &state, "resource/a", 2) == 1);
    assert(state.redirected == UINT32_MAX);
    fail = 1; calls = 0;
    assert(rh_posix_open(&state, &state, "resource/a", 2) == 1 && calls == 2);
    assert(state.fallback == UINT32_MAX);
    calls = 0;
    assert(rh_posix_open(&state, &state, NULL, 2) == 0 && !calls);
    assert(rh_open(&state, &state, NULL, 2) == 0 && !calls);
    assert(rh_posix_open(&state, &snapshot, "resource/a", 2) == 1 && calls == 1);
    puts("path/control-byte limits, 64 rules, exact 32KiB, transaction and saturation tests passed");
    return 0;
}

/* Host CPU benchmark only; not Cortex-M or physical-device acceptance.
 * cc -O2 -std=c11 -Wall -Wextra -Werror -Iinclude src/resource_hook.c \
 *    src/config.c tools/benchmark_rules.c -o build/benchmark-rules
 */
#include "resource_hook.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ITERATIONS 50000u
static struct rh_rule legacy_rules[RH_RULES];
static char text[RH_CONFIG_BYTES];
static volatile unsigned checksum;
static void *allocate(void *cookie, uint32_t size) { (void)cookie; return malloc(size); }
static void release(void *cookie, void *pointer) { (void)cookie; free(pointer); }
static double measure(const struct rh_mapping_view *view, const char *path) {
    char out[RH_PATH];
    unsigned i;
    clock_t start = clock();
    for (i = 0; i < ITERATIONS; i++) {
        int rc = rh_resolve_view(view, path, out);
        checksum += (unsigned)(rc + 2);
        if (rc == 1) checksum += (unsigned char)out[0];
    }
    return (double)(clock() - start) / CLOCKS_PER_SEC;
}
static void benchmark(unsigned count) {
    const struct rh_allocator allocator = {NULL, allocate, release};
    const char *names[] = {"exact", "directory", "miss", "system"};
    const char *paths[] = {NULL, "/resource/icons/not-listed.bin", "/system/missing.bin",
                           "/resource/icons/system.bin"};
    struct rh_snapshot *snapshot = NULL;
    struct rh_mapping_view legacy, indexed;
    char exact[RH_PATH], a[RH_PATH], b[RH_PATH];
    unsigned i, used = 0;
    uint32_t parsed = 0;
    int written = snprintf(text, sizeof(text), "/resource/\tthemes/base/\n");
    assert(written > 0);
    used = (unsigned)written;
    for (i = 1; i + 1u < count; i++) {
        written = snprintf(text + used, sizeof(text) - used,
            "/resource/icons/%03u.bin\tthemes/g001/%03u.bin\n", i, i);
        assert(written > 0 && (unsigned)written < sizeof(text) - used);
        used += (unsigned)written;
    }
    written = snprintf(text + used, sizeof(text) - used,
        "/resource/icons/system.bin\t@system\n");
    assert(written > 0 && (unsigned)written < sizeof(text) - used);
    used += (unsigned)written;
    assert(!rh_parse_config(text, used, legacy_rules, RH_RULES, &parsed) && parsed == count);
    assert(!rh_parse_snapshot(text, used, &allocator, &snapshot));
    legacy = rh_rules_view(legacy_rules, count);
    indexed = rh_snapshot_view(snapshot);
    snprintf(exact, sizeof(exact), "/resource/icons/%03u.bin", count - 2u);
    paths[0] = exact;
    printf("%u rules: packed snapshot %u bytes; old fixed rule bank %u bytes\n",
        count, snapshot->allocation_bytes, count * (unsigned)sizeof(struct rh_rule));
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int ar = rh_resolve_view(&legacy, paths[i], a);
        int br = rh_resolve_view(&indexed, paths[i], b);
        double linear, sorted;
        assert(ar == br && (ar != 1 || !strcmp(a, b)));
        linear = measure(&legacy, paths[i]);
        sorted = measure(&indexed, paths[i]);
        printf("  %-9s linear %.6fs / indexed %.6fs / %.2fx (%u calls)\n",
            names[i], linear, sorted, sorted > 0.0 ? linear / sorted : 0.0, ITERATIONS);
    }
    rh_free_snapshot(&allocator, snapshot);
}
int main(void) {
    benchmark(64u);
    benchmark(RH_RULES);
    puts("Host synthetic paths; includes validation and output assembly, excludes file I/O.");
    return checksum == 0u;
}

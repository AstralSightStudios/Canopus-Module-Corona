#include "resource_hook.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(RH_RULES == 256, "approved rule limit");
_Static_assert(sizeof(struct rh_indexed_rule) == 8, "compact record size");
_Static_assert(sizeof(struct rh_snapshot) == 16, "compact header size");

struct accounting {
    void *blocks[8];
    uint32_t sizes[8], bytes, peak, calls, frees, fail_at, last_size;
};
static void *allocate(void *cookie, uint32_t size) {
    struct accounting *a = cookie;
    uint32_t i;
    void *p;
    a->calls++; a->last_size = size;
    if (a->calls == a->fail_at) return NULL;
    p = malloc(size);
    assert(p);
    for (i = 0; i < 8 && a->blocks[i]; i++) {}
    assert(i < 8);
    a->blocks[i] = p; a->sizes[i] = size; a->bytes += size;
    if (a->bytes > a->peak) a->peak = a->bytes;
    return p;
}
static void release(void *cookie, void *p) {
    struct accounting *a = cookie;
    uint32_t i;
    for (i = 0; i < 8 && a->blocks[i] != p; i++) {}
    assert(i < 8 && p);
    a->bytes -= a->sizes[i]; a->blocks[i] = NULL; a->frees++;
    free(p);
}
static struct accounting accounting;
static const struct rh_allocator allocator = {&accounting, allocate, release};
static const char *pool(const struct rh_snapshot *s) {
    return (const char *)(s->rules + s->count);
}
static struct rh_snapshot *parse(const char *text) {
    struct rh_snapshot *s = NULL;
    assert(!rh_parse_snapshot(text, (uint32_t)strlen(text), &allocator, &s));
    return s;
}
static void dispose(struct rh_snapshot *s) {
    rh_free_snapshot(&allocator, s);
}
static void bad(const char *text, uint32_t size) {
    struct rh_snapshot *old = parse("/old\tthemes/old.bin\n"), *out = old;
    uint32_t before = accounting.bytes;
    assert(rh_parse_snapshot(text, size, &allocator, &out) < 0);
    assert(out == old && accounting.bytes == before);
    dispose(old);
}

struct input {
    const char *text;
    uint32_t size, used, chunk, calls;
    int malformed, probe_error;
};
static int read_input(void *cookie, void *out, uint32_t size) {
    struct input *input = cookie;
    uint32_t n = input->size-input->used;
    input->calls++;
    if (input->malformed) return input->malformed < 0 ? -1 : (int)size+1;
    if (!n && input->probe_error) return input->probe_error;
    if (n > size) n = size;
    if (input->chunk && n > input->chunk) n = input->chunk;
    memcpy(out, input->text+input->used, n); input->used += n;
    return (int)n;
}

static void exact_size_and_oom(void) {
    const char text[] = "/resource/\tthemes/base/\n/resource/a.bin\t@system";
    uint32_t expected_pool = (uint32_t)(strlen("/resource/")+1 +
        strlen("themes/base/")+1 + strlen("/resource/a.bin")+1);
    uint32_t calls = accounting.calls;
    struct rh_snapshot *s = parse(text), *out = s, *empty;
    struct rh_mapping_view view = rh_snapshot_view(s);
    struct input input = {text, sizeof(text)-1, 0, 3, 0, 0, 0};
    const struct rh_indexed_rule *system;
    char mapped[RH_PATH];
    assert(s && s->references == 1 && s->count == 2);
    assert(s->pool_bytes == expected_pool);
    assert(s->allocation_bytes == 16+2*8+expected_pool);
    assert(accounting.calls == calls+1 && accounting.last_size == s->allocation_bytes);
    assert(!rh_validate_view(&view));
    calls = accounting.calls;
    assert(s->rules[0].flags == RH_INDEX_DIRECTORY);
    assert(!strcmp(pool(s)+s->rules[0].destination_offset, "themes/base/"));
    assert(!strstr(pool(s)+s->rules[0].destination_offset, RH_APP_FILES_ROOT));
    system = &s->rules[1];
    assert(system->flags == RH_INDEX_SYSTEM && !system->destination_length &&
           !system->destination_offset && !system->reserved);
    assert(rh_resolve_view(&view, "/resource/a.bin", mapped) == 0);
    assert(rh_resolve_view(&view, "/resource/b.bin", mapped) == 1);
    assert(!strcmp(mapped, RH_THEME_ROOT "base/b.bin"));
    assert(accounting.calls == calls);
    assert(rh_snapshot_equal(s, s) && !rh_snapshot_equal(s, NULL));

    accounting.fail_at = accounting.calls+1;
    assert(rh_parse_snapshot(text, sizeof(text)-1, &allocator, &out) == -7 && out == s);
    assert(accounting.bytes == s->allocation_bytes);
    accounting.fail_at = accounting.calls+1;
    assert(rh_read_snapshot(read_input, &input, &allocator, &out) == -7 && out == s);
    assert(!input.calls && accounting.bytes == s->allocation_bytes);
    accounting.fail_at = accounting.calls+2;
    assert(rh_read_snapshot(read_input, &input, &allocator, &out) == -7 && out == s);
    assert(input.used == sizeof(text)-1 && accounting.bytes == s->allocation_bytes);
    accounting.fail_at = 0; input.used = 0;
    calls = accounting.calls; accounting.peak = accounting.bytes;
    assert(!rh_read_snapshot(read_input, &input, &allocator, &out));
    assert(out != s && rh_snapshot_equal(s, out));
    assert(accounting.calls == calls+2);
    assert(accounting.peak == RH_CONFIG_BYTES+2*s->allocation_bytes);
    assert(accounting.bytes == 2*s->allocation_bytes);
    dispose(out);

    empty = s; calls = accounting.calls;
    assert(!rh_parse_snapshot("# empty\n\r\n", 10, &allocator, &empty));
    assert(!empty && accounting.calls == calls);
    view = rh_snapshot_view(empty);
    assert(!rh_validate_view(&view) && rh_resolve_view(&view, "/anything", mapped) == 0);
    assert(rh_snapshot_equal(NULL, NULL));
    dispose(s);
    rh_free_snapshot(&allocator, NULL);
    assert(!accounting.bytes);
}

static void limits_and_reader(void) {
    static char text[RH_CONFIG_BYTES+1];
    struct rh_snapshot *s = NULL, *old, *out;
    struct input input;
    uint32_t i, used = 0, calls;
    char mapped[RH_PATH];
    for (i = 0; i < RH_RULES; i++)
        used += (uint32_t)snprintf(text+used, sizeof(text)-used, "/r/%03u/\tthemes/base/\n", (unsigned)i);
    assert(!rh_parse_snapshot(text, used, &allocator, &s) && s->count == RH_RULES);
    {
        struct rh_mapping_view view = rh_snapshot_view(s);
        assert(!rh_validate_view(&view));
        assert(rh_resolve_view(&view, "/r/255/a", mapped) == 1);
        assert(!strcmp(mapped, RH_THEME_ROOT "base/a"));
    }
    calls = accounting.calls; out = s;
    used += (uint32_t)snprintf(text+used, sizeof(text)-used, "/r/256/\tthemes/base/\n");
    assert(rh_parse_snapshot(text, used, &allocator, &out) == -3 && out == s);
    assert(accounting.calls == calls);
    dispose(s);

    /* Fill the maximum pool: raw TSV is exactly 32KiB, final row has no LF. */
    used = 0;
    for (i = 0; i < RH_RULES; i++) {
        uint32_t source = i ? 118 : 119, start = used;
        used += (uint32_t)snprintf(text+used, sizeof(text)-used, "/r%03u", (unsigned)i);
        while (used-start < source) text[used++] = 'x';
        memcpy(text+used, "\tthemes/x\n", 10); used += 10;
    }
    assert(used == RH_CONFIG_BYTES+1); used--;
    assert(!rh_parse_snapshot(text, used, &allocator, &s));
    assert(s->pool_bytes == RH_CONFIG_BYTES+1);
    assert(s->allocation_bytes == 16+8*RH_RULES+RH_CONFIG_BYTES+1);
    for (i = 0; i < s->count; i++) {
        assert((uint32_t)s->rules[i].source_offset+s->rules[i].source_length < s->pool_bytes);
        assert((uint32_t)s->rules[i].destination_offset+s->rules[i].destination_length < s->pool_bytes);
    }
    dispose(s);

    memset(text, '#', sizeof(text));
    old = parse("/old\tthemes/old.bin"); out = old;
    input = (struct input){text, RH_CONFIG_BYTES, 0, 97, 0, 0, 0};
    calls = accounting.calls;
    assert(!rh_read_snapshot(read_input, &input, &allocator, &out) && !out);
    assert(input.used == RH_CONFIG_BYTES && accounting.calls == calls+1);
    assert(accounting.bytes == old->allocation_bytes);
    input = (struct input){text, RH_CONFIG_BYTES+1, 0, 0, 0, 0, 0}; out = old;
    assert(rh_read_snapshot(read_input, &input, &allocator, &out) == -6 && out == old);
    assert(input.used == RH_CONFIG_BYTES+1 && input.calls == 2);
    calls = accounting.calls;
    assert(rh_parse_snapshot(text, sizeof(text), &allocator, &out) == -6 && out == old);
    assert(accounting.calls == calls);
    input = (struct input){text, 0, 0, 0, 0, 1, 0};
    assert(rh_read_snapshot(read_input, &input, &allocator, &out) == -5 && out == old);
    input.malformed = -1;
    assert(rh_read_snapshot(read_input, &input, &allocator, &out) == -5 && out == old);
    input = (struct input){text, RH_CONFIG_BYTES, 0, 0, 0, 0, -1};
    assert(rh_read_snapshot(read_input, &input, &allocator, &out) == -5 && out == old);
    input = (struct input){text, RH_CONFIG_BYTES, 0, 0, 0, 0, 2};
    assert(rh_read_snapshot(read_input, &input, &allocator, &out) == -5 && out == old);
    assert(accounting.bytes == old->allocation_bytes);
    dispose(old);

    /* Maximum raw size can also end with a non-comment, LF-less row. */
    {
        const char row[] = "/last/\tthemes/last/";
        uint32_t row_size = sizeof(row)-1;
        memset(text, '#', sizeof(text));
        text[RH_CONFIG_BYTES-row_size-1] = '\n';
        memcpy(text+RH_CONFIG_BYTES-row_size, row, row_size);
        input = (struct input){text, RH_CONFIG_BYTES, 0, 17, 0, 0, 0};
        assert(!rh_read_snapshot(read_input, &input, &allocator, &s));
        assert(s->count == 1 && s->pool_bytes == strlen("/last/")+1+strlen("themes/last/")+1);
        dispose(s);
    }
    assert(!accounting.bytes);
}

struct row { const char *source, *destination; };
static const struct row rows[] = {
    {"/", "themes/root/"},
    {"/resource/", "themes/base/"},
    {"/resource/icons/", "themes/icons/"},
    {"/resource/icons/a.bin", "themes/exact.bin"},
    {"/resource/icons/sub/", "themes/deep/"},
    {"/resource/icons/stock.bin", "@system"},
    {"/resource/icons/sub/stock.bin", "@system"},
    {"/other", "themes/other.bin"},
    {"/resource/\xc3\xa9.bin", "themes/\xe5\xad\x97.bin"},
    {"/resource/\xff.bin", "themes/\x80.bin"},
    {"/resource/icons", "themes/no-slash.bin"},
    {"/r/", "themes/r/"},
    {"/resource/icons/sub/exact/", "themes/exact-dir/"},
    {"/resource/a/b/", "themes/parent/"},
    {"/resource/a/b/c/", "themes/nested/"},
    {"/resource/a/b/c", "themes/file.bin"}
};
#define ROW_COUNT ((uint32_t)(sizeof(rows)/sizeof(rows[0])))
static uint32_t make_text(char *text, const uint32_t *order, int crlf) {
    uint32_t i, used = 0;
    for (i = 0; i < ROW_COUNT; i++) {
        const struct row *row = &rows[order[i]];
        used += (uint32_t)snprintf(text+used, 4096-used, "%s\t%s%s", row->source, row->destination,
                                  i+1 == ROW_COUNT ? "" : (crlf ? "\r\n" : "\n"));
    }
    return used;
}
static void differential_path(const struct rh_mapping_view *indexed,
                                const struct rh_mapping_view *legacy, const char *path) {
    char a[RH_PATH], b[RH_PATH], untouched[RH_PATH];
    int ar, br;
    memset(a, 0xa5, sizeof(a)); memset(b, 0xa5, sizeof(b));
    memset(untouched, 0xa5, sizeof(untouched));
    ar = rh_resolve_view(indexed, path, a); br = rh_resolve_view(legacy, path, b);
    assert(ar == br);
    if (ar == 1) assert(!strcmp(a, b));
    else assert(!memcmp(a, untouched, sizeof(a)) && !memcmp(b, untouched, sizeof(b)));
}
static void differential_and_equality(void) {
    static struct rh_rule legacy_rules[RH_RULES];
    static const char *paths[] = {
        "/", "/resource", "/resource/", "/resource/icons", "/resource/icons/",
        "/resource/icons/a.bin", "/resource/icons/a.bin.more", "/resource/icons/stock.bin",
        "/resource/icons/sub/stock.bin", "/resource/icons/sub/a.bin",
        "/resource/icons/sub/exact/", "/resource/icons/sub/exact/a.bin",
        "/resource/icons-other/a.bin", "/resource2/a.bin", "/other", "/other/file",
        "/r/", "/r/a", "/resource/\xc3\xa9.bin", "/resource/\xff.bin",
        "/resource/a/b/c", "/resource/a/b/c/", "/resource/a/b/c/d/e",
        "", "relative", "/resource//a", "/resource/../a", "/resource/./a", "/resource/a:b",
        "/resource/a\\b", "/resource/\177a", "/resource/\001a", NULL
    };
    char text[4096], path[RH_PATH];
    uint32_t order[ROW_COUNT], i, iteration, count, used, seed = 0x12345678u;
    struct rh_snapshot *baseline = NULL, *s;
    for (i = 0; i < ROW_COUNT; i++) order[i] = i;
    for (iteration = 0; iteration < 40; iteration++) {
        struct rh_mapping_view indexed, legacy;
        if (iteration) {
            for (i = ROW_COUNT; i > 1; i--) {
                uint32_t j, temp;
                seed = seed*1664525u+1013904223u; j = seed % i;
                temp = order[i-1]; order[i-1] = order[j]; order[j] = temp;
            }
        }
        used = make_text(text, order, (int)(iteration % 2));
        s = NULL; assert(!rh_parse_snapshot(text, used, &allocator, &s));
        assert(!rh_parse_config(text, used, legacy_rules, RH_RULES, &count));
        indexed = rh_snapshot_view(s); legacy = rh_rules_view(legacy_rules, count);
        assert(!rh_validate_view(&indexed) && !rh_validate_view(&legacy));
        for (i = 0; i < sizeof(paths)/sizeof(paths[0]); i++)
            differential_path(&indexed, &legacy, paths[i]);
        for (i = 0; i < 160; i++) {
            snprintf(path, sizeof(path), "/resource/%s/%s%u%s",
                     i % 3 == 0 ? "icons/sub" : (i % 3 == 1 ? "icons" : "a/b/c"),
                     i % 2 ? "file" : "dir/", (unsigned)i, i % 5 ? ".bin" : "/");
            differential_path(&indexed, &legacy, path);
        }
        memset(path, 'x', sizeof(path)); path[0] = '/'; path[RH_PATH-1] = 0;
        differential_path(&indexed, &legacy, path);
        path[RH_PATH-1] = 'x'; differential_path(&indexed, &legacy, path);
        if (!baseline) baseline = s;
        else {
            assert(rh_snapshot_equal(baseline, s));
            if (iteration == 1) {
                assert(s->allocation_bytes == baseline->allocation_bytes);
                assert(memcmp(s, baseline, s->allocation_bytes));
                /* Equality ignores owner refcounts and record reserved bytes. */
                s->references = 99; s->rules[0].reserved = 123;
                assert(rh_snapshot_equal(baseline, s));
            }
            dispose(s);
        }
    }
    s = parse("/\tthemes/different/");
    assert(!rh_snapshot_equal(baseline, s)); dispose(s); dispose(baseline);
    s = parse("/same\tthemes/a.bin"); baseline = parse("/same\tthemes/b.bin");
    assert(!rh_snapshot_equal(baseline, s)); dispose(s); dispose(baseline);
    s = parse("/same\t@system"); baseline = parse("/same\tthemes/b.bin");
    assert(!rh_snapshot_equal(baseline, s)); dispose(s); dispose(baseline);
    assert(!accounting.bytes);
}

static void validation_and_selected_overflow(void) {
    static const char *invalid[] = {
        "bad", "/a/\t", "\tthemes/a", "/a/\t/data/themes/a/", "/a/\t@system",
        "/a/\tthemes/file", "/a\tthemes/dir/", "relative\tthemes/a",
        "/a//b\tthemes/a", "/a/../b\tthemes/a", "/a/./b\tthemes/a",
        "/a\tthemes/../bad", "/a\tthemes/./bad", "/a\tthemes/a:b",
        "/a\tthemes/a\\b", "/a\tthemes/a\textra", "/a\tthemes/a\001",
        "/a\tthemes/a\177", "/a\t@system-more", "/a\tthemes/",
        "/a\tthemes/a\n/a\t@system", "/a/\tthemes/a/\n/a/\tthemes/b/",
        "/z\tthemes/z\n/a\tthemes/a\n/z\t@system",
        "/\tthemes/root/\n/\tthemes/other/",
        "/\xff\tthemes/high\n/a\tthemes/a\n/\xff\t@system"
    };
    char text[2048], destination[RH_PATH], path[RH_PATH], output[RH_PATH];
    uint32_t i, base = (uint32_t)strlen(RH_APP_FILES_ROOT), source_length = 255;
    struct rh_snapshot *s, *out;
    struct rh_mapping_view view;
    for (i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++)
        bad(invalid[i], (uint32_t)strlen(invalid[i]));
    bad("#\0ignored", 9); bad("/a\tthemes/a\0", 12);
    memset(path, 'x', sizeof(path)); path[0] = '/'; path[source_length] = 0;
    snprintf(text, sizeof(text), "%s\t@system", path);
    s = parse(text); view = rh_snapshot_view(s);
    assert(s->rules[0].source_length == 255 && rh_resolve_view(&view, path, output) == 0);
    dispose(s);
    path[source_length] = 'x';
    memcpy(text, path, sizeof(path)); strcpy(text+sizeof(path), "\t@system");
    bad(text, (uint32_t)strlen(text));

    /* Destination absolute length 255 is accepted, 256 is rejected. */
    strcpy(destination, "themes/");
    for (i = 7; i < 255-base; i++) destination[i] = 'x';
    destination[i] = 0;
    snprintf(text, sizeof(text), "/a\t%s", destination);
    s = parse(text); view = rh_snapshot_view(s);
    assert(rh_resolve_view(&view, "/a", output) == 1 && strlen(output) == 255);
    dispose(s);
    destination[i++] = 'x'; destination[i] = 0;
    snprintf(text, sizeof(text), "/a\t%s", destination); bad(text, (uint32_t)strlen(text));

    /* A shorter rule would fit, but selected longest overflow must not retry. */
    strcpy(destination, "themes/");
    for (i = 7; i < 254-base; i++) destination[i] = 'x';
    destination[i++] = '/'; destination[i] = 0;
    snprintf(text, sizeof(text), "/\tthemes/root/\n/a/\t%s", destination);
    s = parse(text); view = rh_snapshot_view(s);
    memset(output, 'z', sizeof(output));
    assert(rh_resolve_view(&view, "/a/x", output) == -2 && output[0] == 'z');
    dispose(s);
    out = NULL;
    assert(rh_parse_snapshot("", 0, &allocator, &out) == 0 && !out);
    assert(rh_validate_view(NULL) == -1);
    view = rh_rules_view(NULL, 1); assert(rh_validate_view(&view) == -1);
    assert(!accounting.bytes);
}

int main(void) {
    exact_size_and_oom();
    limits_and_reader();
    differential_and_equality();
    validation_and_selected_overflow();
    assert(!accounting.bytes);
    puts("compact allocation, OOM, 256 rules, 32KiB, indexing, equality and differential tests passed");
    return 0;
}

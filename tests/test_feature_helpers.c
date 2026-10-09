/* Shared policy regression; native clock/GPU/font ABIs are not modeled here. */
#include "resource_hook_calendar_internal.h"
#include "resource_hook_blur.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned writes, written, write_mode;
static unsigned char received[32];
int rh_platform_write(int fd, const void *data, uint32_t size) {
    unsigned n = size > 3u ? 3u : size;
    assert(fd == 0);
    ++writes;
    if (write_mode == 1) return 0;
    if (write_mode == 2) return -1;
    if (write_mode == 3) return (int)size + 1;
    memcpy(received + written, data, n);
    written += n;
    return (int)n;
}

static void test_calendar(void) {
    struct rh_calendar_date d = {2000, 2, 29, 0, 0};
    char text[4], path[RH_PATH], relative[RH_PATH];
    const char payload[] = "complete short writes";
    assert(rh_calendar_date_valid(&d));
    d.year = 1900; assert(!rh_calendar_date_valid(&d));
    d.year = 2024; assert(rh_calendar_date_valid(&d));
    d.month = 4; d.day = 31; assert(!rh_calendar_date_valid(&d));
    d.day = 30; assert(rh_calendar_date_valid(&d));
    d.weekday = 7; assert(!rh_calendar_date_valid(&d));
    assert(!rh_calendar_date_valid(NULL));
    for (unsigned i = 0; i <= 31; ++i) {
        char expected[4];
        snprintf(expected, sizeof(expected), "%u", i);
        rh_calendar_decimal(text, i);
        assert(!strcmp(text, expected));
    }
    assert(rh_calendar_bounded("", 1));
    assert(!rh_calendar_bounded(NULL, 4));
    assert(!rh_calendar_bounded("abc", 3));
    assert(rh_calendar_bounded("abc", 4));
    assert(!rh_calendar_image_path(path, "themes/a/icon.bin"));
    assert(!strcmp(path, RH_THEME_ROOT "a/icon.bin"));
    assert(rh_calendar_image_path(path, NULL) < 0);
    assert(rh_calendar_image_path(path, "other/a/icon.bin") < 0);
    unsigned maximum = RH_PATH - sizeof(RH_APP_FILES_ROOT);
    memcpy(relative, "themes/a/", 9);
    memset(relative + 9, 'x', maximum - 9);
    relative[maximum] = 0;
    assert(!rh_calendar_image_path(path, relative));
    assert(strlen(path) == RH_PATH - 1u);
    relative[maximum] = 'x'; relative[maximum + 1] = 0;
    assert(rh_calendar_image_path(path, relative) < 0);
    assert(!rh_calendar_full_write(0, payload, sizeof(payload)));
    assert(writes > 1 && written == sizeof(payload));
    assert(!memcmp(received, payload, sizeof(payload)));
    for (write_mode = 1; write_mode <= 3; ++write_mode) {
        writes = 0;
        assert(rh_calendar_full_write(0, payload, sizeof(payload)) < 0);
        assert(writes == 1);
    }
    writes = 0;
    assert(!rh_calendar_full_write(0, NULL, 0) && !writes);
}

static void test_item_context(void) {
    static struct rh_calendar_config c;
    struct rh_calendar_date d = {2026, 12, 31, 6, 0};
    const char *context, *unchanged = "unchanged";
    assert(rh_calendar_context_count(0) == 12);
    assert(rh_calendar_context_count(1) == 31);
    assert(rh_calendar_context_count(2) == 7);
    assert(!rh_calendar_context_count(3));
    rh_calendar_defaults(&c);
    for (unsigned item = 0; item < 3; ++item) {
        context = unchanged;
        assert(!rh_calendar_item_context(&c, &d, item, &context) && !context);
    }
    for (unsigned item = 0; item < 3; ++item) {
        for (unsigned type = RH_CALENDAR_TEXT; type <= RH_CALENDAR_IMAGE; ++type) {
            rh_calendar_defaults(&c);
            struct rh_calendar_item *p = &c.items[item];
            p->type = type; p->count = rh_calendar_context_count(item);
            for (unsigned i = 0; i < p->count; ++i) {
                p->context[i] = (uint16_t)c.pool_used;
                int n = snprintf(c.pool + c.pool_used, sizeof(c.pool) - c.pool_used,
                                 type == RH_CALENDAR_TEXT ? "slot-%u" : "themes/a/%u.bin", i);
                assert(n > 0); c.pool_used += (unsigned)n + 1;
            }
            context = unchanged;
            assert(!rh_calendar_item_context(&c, &d, item, &context));
            assert(context == c.pool + p->context[p->count - 1]);
            struct rh_calendar_date first = {2026, 1, 1, 0, 0};
            assert(!rh_calendar_item_context(&c, &first, item, &context));
            assert(context == c.pool + p->context[0]);
            unsigned saved = p->context[0];
            p->context[0] = (uint16_t)c.pool_used;
            context = unchanged;
            assert(rh_calendar_item_context(&c, &first, item, &context) < 0);
            assert(context == unchanged);
            p->context[0] = (uint16_t)saved;
            --p->count;
            assert(rh_calendar_item_context(&c, &first, item, &context) < 0);
            assert(context == unchanged);
        }
    }
    rh_calendar_defaults(&c);
    struct rh_calendar_item *p = &c.items[1];
    struct rh_calendar_item saved = *p;
#define REJECT(field, value) do { \
    *p = saved; p->field = (value); context = unchanged; \
    assert(rh_calendar_item_context(&c, &d, 1, &context) < 0); \
    assert(context == unchanged); \
} while (0)
    REJECT(type, RH_CALENDAR_IMAGE + 1);
    REJECT(align, 4); REJECT(x, -113); REJECT(y, 113);
    REJECT(size, 7); REJECT(size, 73);
    REJECT(color_native, 2); REJECT(color, 0x1000000);
    REJECT(count, 1);
#undef REJECT
    *p = saved; p->count = 31; c.pool_used = 64;
    memset(c.pool, 'x', c.pool_used);
    context = unchanged;
    assert(rh_calendar_item_context(&c, &d, 1, &context) < 0 && context == unchanged);
    c.pool_used = RH_CALENDAR_POOL_BYTES + 1;
    assert(rh_calendar_item_context(&c, &d, 1, &context) < 0);
    assert(rh_calendar_item_context(NULL, &d, 1, &context) < 0);
    assert(rh_calendar_item_context(&c, NULL, 1, &context) < 0);
    assert(rh_calendar_item_context(&c, &d, 3, &context) < 0);
    assert(rh_calendar_item_context(&c, &d, 1, NULL) < 0);
}

/* Independent scalar oracle preserves the original per-axis RGB565 rounding,
 * shrunken edge windows, pass order and pre-filter darkening. */
static void reference(uint16_t *src, uint16_t *tmp, unsigned w, unsigned h,
                      unsigned radius, unsigned passes, unsigned darken) {
    for (unsigned i = 0; i < w * h; ++i) {
        unsigned v = src[i];
        src[i] = (uint16_t)((((v >> 11) * darken / 100) << 11) |
            ((((v >> 5) & 63) * darken / 100) << 5) | ((v & 31) * darken / 100));
    }
    for (unsigned pass = 0; pass < passes; ++pass) {
        for (unsigned axis = 0; axis < 2; ++axis) {
            uint16_t *in = axis ? tmp : src, *out = axis ? src : tmp;
            for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
                unsigned r = 0, g = 0, b = 0, count = 0;
                unsigned center = axis ? y : x, limit = axis ? h : w;
                unsigned lo = center > radius ? center - radius : 0;
                unsigned hi = center + radius < limit ? center + radius : limit - 1;
                for (unsigned n = lo; n <= hi; ++n) {
                    unsigned v = in[axis ? n * w + x : y * w + n];
                    r += v >> 11; g += (v >> 5) & 63; b += v & 31; ++count;
                }
                out[y * w + x] = (uint16_t)(((r / count) << 11) |
                    ((g / count) << 5) | (b / count));
            }
        }
    }
}

static void test_blur(void) {
    static uint16_t actual[84 * 130], expected[84 * 130];
    static uint16_t work[84 * 130], reference_work[84 * 130];
    const unsigned shapes[][2] = {{53, 130}, {84, 120}, {1, 1}, {2, 3}};
    uint32_t seed = 12345;
    for (unsigned shape = 0; shape < 4; ++shape)
        for (unsigned pattern = 0; pattern < 4; ++pattern)
            for (unsigned darken = 70; darken <= 100; darken += 30) {
                unsigned w = shapes[shape][0], h = shapes[shape][1];
                for (unsigned i = 0; i < w * h; ++i) {
                    seed = seed * 1664525u + 1013904223u;
                    actual[i] = expected[i] = pattern == 0 ? 0 :
                        pattern == 1 ? 0xffff : pattern == 2 ? (uint16_t)seed : (uint16_t)i;
                }
                reference(expected, reference_work, w, h, 2, 2, darken);
                assert(rh_blur_filter_rgb565(actual, work, w, h, 2, 2, darken));
                assert(!memcmp(actual, expected, w * h * sizeof(*actual)));
            }
    assert(!rh_blur_filter_rgb565(NULL, work, 53, 130, 2, 2, 100));
    assert(!rh_blur_filter_rgb565(actual, actual, 53, 130, 2, 2, 100));
    assert(!rh_blur_filter_rgb565(actual, work, 0, 130, 2, 2, 100));
    assert(!rh_blur_filter_rgb565(actual, work, 85, 130, 2, 2, 100));
    assert(!rh_blur_filter_rgb565(actual, work, 53, 131, 2, 2, 100));
    assert(!rh_blur_filter_rgb565(actual, work, 53, 130, 9, 2, 100));
    assert(!rh_blur_filter_rgb565(actual, work, 53, 130, 2, 9, 100));
    assert(!rh_blur_filter_rgb565(actual, work, 53, 130, 2, 2, 101));
}

int main(void) {
    test_calendar();
    test_item_context();
    test_blur();
    puts("Shared calendar helpers and RGB565 filter pixel equivalence passed");
    return 0;
}

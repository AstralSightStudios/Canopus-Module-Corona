#include "resource_hook_calendar_runtime.h"
#include "resource_hook_calendar_native.h"
#include "resource_hook_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned allocs, frees, locked, renders, publishes, retires, date_calls;
static int supported = 1, display_ready = 1, render_fail, read_fail, open_error = RH_ENOENT;
static const char *file;
static unsigned cursor;
static struct rh_calendar_date now = {2026, 10, 6, 1, 0};
static const char *last_output, *logical_cache;
static unsigned fail_retire_at;
void *rh_platform_alloc(uint32_t size) { assert(!locked); ++allocs; return calloc(1, size); }
void rh_platform_free(void *p) { assert(!locked); ++frees; free(p); }
int rh_platform_open(const char *path, int flags) {
    assert(!locked && flags == 1 && !strcmp(path, RH_CALENDAR_CONFIG_PATH));
    cursor = 0; return file ? 0 : -1;
}
int rh_platform_errno(void) { return open_error; }
int rh_platform_read(int fd, void *out, uint32_t size) {
    unsigned n;
    assert(fd == 0 && !locked);
    if (read_fail) return -1;
    n = (unsigned)strlen(file) - cursor;
    if (n > size) n = size;
    memcpy(out, file + cursor, n); cursor += n; return (int)n;
}
void rh_platform_close(int fd) { assert(fd == 0 && !locked); }
uint32_t rh_platform_lock(void) { assert(!locked); locked = 1; return 0; }
void rh_platform_unlock(uint32_t irq) { assert(!irq && locked); locked = 0; }
int rh_platform_redraw_ready(void) { return display_ready; }
int rh_calendar_native_supported(void) { return supported; }
int rh_calendar_native_date(struct rh_calendar_date *d) { ++date_calls; *d = now; return 0; }
int rh_platform_calendar_retire(const struct rh_calendar_config *c) {
    assert(c && !locked);
    if (++retires == fail_retire_at) return -1;
    logical_cache = NULL;
    return 0;
}
void rh_platform_calendar_publish(void) {
    assert(!locked && !logical_cache && rh_calendar_redirect(&RH_CALENDAR_OUTPUT_PATH[1])); ++publishes;
}
int rh_calendar_native_render(const struct rh_calendar_config *c,
                              const struct rh_calendar_date *d, const char *path) {
    struct rh_calendar_config *candidate;
    assert(!locked && c && d->day == now.day);
    ++renders; last_output = path;
    /* Simulate a reentrant read repopulating the OLD logical cache mid-render. */
    logical_cache = rh_calendar_redirect(&RH_CALENDAR_OUTPUT_PATH[1]);
    /* Nested timer/config commit must not replace the render's borrowed state. */
    assert(rh_calendar_poll() == -2154);
    candidate = rh_platform_alloc(sizeof(*candidate)); rh_calendar_defaults(candidate);
    assert(rh_calendar_commit(candidate) == -2154); rh_calendar_discard(candidate);
    return render_fail ? -1 : 0;
}
static void parse_cases(void) {
    struct rh_calendar_config *c = malloc(sizeof(*c));
    const char *text = "version=1\nday\ttext\ttop_mid\t0\t34\t48\t000000\t0\n";
    char image[4096];
    assert(!rh_calendar_parse("version=1\n", 10, c));
    assert(c->items[0].type == RH_CALENDAR_NONE);
    assert(c->items[1].type == RH_CALENDAR_TEXT && c->items[1].y == 34);
    assert(c->items[2].type == RH_CALENDAR_TEXT && c->items[2].y == 17);
    assert(c->items[1].color_native && !c->items[1].size && !c->items[2].count);
    assert(!rh_calendar_parse(text, (uint32_t)strlen(text), c));
    assert(c->items[1].size == 48 && !c->items[1].color_native);
    strcpy(image, "version=1\nweekday\timage\tcenter\t-112\t112\t0\tnative\t7\n");
    for (unsigned i = 0; i < 7; ++i) strcat(image, "themes/example/week.bin\n");
    assert(!rh_calendar_parse(image, (uint32_t)strlen(image), c));
    assert(!strcmp(rh_calendar_context(c, 2, 6), "themes/example/week.bin"));
    assert(!rh_calendar_context(c, 2, 7));
    const char *bad[] = {
        "", "version=2\n", "version=1\nunknown\tnone\ttop_mid\t0\t0\t0\tnative\t0\n",
        "version=1\nday\ttext\ttop_mid\t113\t0\t0\tnative\t0\n",
        "version=1\nday\ttext\ttop_mid\t0\t0\t7\tnative\t0\n",
        "version=1\nday\timage\ttop_mid\t0\t0\t0\tnative\t0\n",
        "version=1\nday\ttext\ttop_mid\t0\t0\t0\tnative\t1\na\n",
        "version=1\nday\tnone\ttop_mid\t0\t0\t0\tnative\t0\nday\tnone\ttop_mid\t0\t0\t0\tnative\t0\n",
        "version=1\nday\ttext\ttop_mid\t0\t0\t0\t#00000\t0\n",
        "version=1\nday\ttext\ttop_mid\t0\t0\t0\tgggggg\t0\n",
        "version=1\nday\ttext\ttop_mid\t0\t0\t0\tnative\t0",
        "version=1\nday\ttext\ttop_mid\t99999999999999\t0\t0\tnative\t0\n"
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(rh_calendar_parse(bad[i], (uint32_t)strlen(bad[i]), c));
    strcpy(image, "version=1\nmonth\ttext\ttop_left\t0\t0\t0\tnative\t12\n");
    for (unsigned i = 0; i < 12; ++i) strcat(image, "\xe4\xb8\x80\xe6\x9c\x88\n");
    assert(!rh_calendar_parse(image, (uint32_t)strlen(image), c));
    image[55] = '\t'; assert(rh_calendar_parse(image, (uint32_t)strlen(image), c));
    strcpy(image, "version=1\nweekday\timage\tcenter\t0\t0\t0\tnative\t7\n");
    for (unsigned i = 0; i < 7; ++i) strcat(image, "themes/example/../week.bin\n");
    assert(rh_calendar_parse(image, (uint32_t)strlen(image), c));
    free(c);
}
int main(void) {
    struct rh_calendar_config *candidate = NULL;
    unsigned before;
    parse_cases();
    assert(!rh_calendar_redirect(&RH_CALENDAR_OUTPUT_PATH[1]));
    file = "malformed\n";
    assert(rh_calendar_start() == -2153 && rh_calendar_pending());
    assert(!rh_calendar_redirect(&RH_CALENDAR_OUTPUT_PATH[1]));
    file = NULL;
    assert(!rh_calendar_start() && rh_calendar_pending());
    assert(!rh_calendar_poll() && renders == 1 && publishes == 1);
    assert(!strcmp(last_output, RH_CALENDAR_BANK0));
    assert(!strcmp(rh_calendar_redirect(&RH_CALENDAR_OUTPUT_PATH[1]), &RH_CALENDAR_BANK0[1]));
    assert(!rh_calendar_redirect(RH_CALENDAR_OUTPUT_PATH));
    assert(!rh_calendar_redirect("data/app/perpetual_calendar/calendar_icon.bin.extra"));
    assert(!rh_calendar_redirect("data/app/perpetual_calendar/calendar_icon.bi"));
    assert(!rh_calendar_poll() && renders == 1);
    before = date_calls;
    display_ready = 0; assert(rh_calendar_poll() == -2154 && date_calls == before);
    display_ready = 1; now.day = 7; now.weekday = 2; render_fail = 1;
    assert(rh_calendar_poll() == -2156 && publishes == 1);
    assert(!strcmp(rh_calendar_redirect(&RH_CALENDAR_OUTPUT_PATH[1]), &RH_CALENDAR_BANK0[1]));
    render_fail = 0; assert(!rh_calendar_poll() && publishes == 2);
    assert(!strcmp(last_output, RH_CALENDAR_BANK1));
    now.locale = 123; assert(!rh_calendar_poll() && publishes == 3);
    now.day = 8; now.weekday = 3;
    fail_retire_at = retires + 2; /* Publication-boundary retirement failure. */
    assert(rh_calendar_poll() == -2154 && publishes == 3);
    assert(!strcmp(rh_calendar_redirect(&RH_CALENDAR_OUTPUT_PATH[1]), &RH_CALENDAR_BANK0[1]));
    fail_retire_at = 0;
    now.day = 7; now.weekday = 2; /* No successful publication changed generated. */
    file = "malformed\n"; assert(rh_calendar_stage(&candidate) == -2153 && !candidate);
    assert(!strcmp(rh_calendar_redirect(&RH_CALENDAR_OUTPUT_PATH[1]), &RH_CALENDAR_BANK0[1]));
    file = "version=1\n"; read_fail = 1;
    assert(rh_calendar_stage(&candidate) == -2152); read_fail = 0;
    file = NULL; open_error = 13; assert(rh_calendar_stage(&candidate) == -2152);
    open_error = RH_ENOENT; file = "version=1\nmonth\tnone\ttop_left\t0\t0\t0\tnative\t0\n";
    assert(!rh_calendar_stage(&candidate) && candidate);
    assert(!rh_calendar_commit(candidate) && rh_calendar_pending());
    assert(!rh_calendar_poll() && !rh_calendar_pending() && publishes == 4);
    assert(!rh_calendar_refresh() && publishes == 5);
    before = renders; supported = 0;
    assert(!rh_calendar_pending() && !rh_calendar_poll() && !rh_calendar_refresh());
    assert(!rh_calendar_redirect(&RH_CALENDAR_OUTPUT_PATH[1]) && renders == before);
    candidate = NULL;
    assert(rh_calendar_stage(&candidate) == -2157 && !candidate);
    file = "version=1\n";
    assert(!rh_calendar_stage(&candidate) && !candidate);
    assert(retires == 2 * renders - 1 && allocs == frees + 1); /* Only active config is retained. */
    puts("calendar defaults/parser, banks, UI retries, language/date rollover and failure fallback passed");
    return 0;
}

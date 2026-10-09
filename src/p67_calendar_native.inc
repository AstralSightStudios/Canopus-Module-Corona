/* Exact .043 AP calendar adapter: Xiaomi Band 10 Pro 3.101.043.
 * AP SHA256 519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec.
 * Generates module-owned 112x112 ARGB8888 draw buffer in RAM and publishes
 * to perpetual_calendar app descriptor with EventBus 43 notification.
 */
#ifndef P67_CALENDAR_NATIVE_INC
#define P67_CALENDAR_NATIVE_INC

#include "resource_hook_calendar_native.h"
#include "resource_hook_calendar_internal.h"
#include "resource_hook_platform.h"
#include "resource_hook_target.h"
#include "resource_hook_calendar.h"

#define P67_CAL_N(a,b,c,d,e) (((uint32_t (*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t)) \
    (uintptr_t)((a)|1u))((uintptr_t)(b),(uintptr_t)(c),(uintptr_t)(d),(uintptr_t)(e)))

#define P67_CAL_CLOCK               0x0c201fe0u
#define P67_CAL_LOCALTIME           0x0c4e35acu
#define P67_CAL_LOCALIZED_TEXT      0x0c1e5c38u
#define P67_CAL_ROOT_CREATE         0x0c5881e0u
#define P67_CAL_SIZE                0x0c588628u
#define P67_CAL_IMAGE_CREATE        0x0c588d58u
#define P67_CAL_IMAGE_SOURCE        0x0c587fa0u
#define P67_CAL_LABEL_CREATE        0x0c588f30u
#define P67_CAL_LABEL_TEXT          0x0c589490u
#define P67_CAL_COLOR               0x0c588a70u
#define P67_CAL_FONT_DAY            0x0c4bf87cu
#define P67_CAL_FONT_WEEK           0x0c4bf898u
#define P67_CAL_ALIGN               0x0c589188u
#define P67_CAL_TEXT_ALIGN          0x0c588bf8u
#define P67_CAL_DRAW_BUF_CREATE     0x0c587f38u
#define P67_CAL_DRAW_BUF_DESTROY    0x0cac0db8u
#define P67_CAL_SNAPSHOT            0x0ca5fb40u
#define P67_CAL_OBJECT_DELETE       0x0c588de8u
#define P67_CAL_GPU_FINISH          0x0c187bb0u
#define P67_CAL_CACHE_CLEAN         0x0c588f88u
#define P67_CAL_CACHE_INVALIDATE    0x0c588448u

#define P67_CAL_APP_LOOKUP          0x0ca69e80u
#define P67_CAL_EVENTBUS_SEND       0x0ca41584u
#define P67_CAL_APP_SIGNAL_STOCK    0x0c4efde8u

static uint32_t p67_cal_draw_bufs[2];
static uint32_t p67_cal_active_buf;
static uint32_t p67_cal_original_signal;
static uint32_t p67_cal_hooked_app;
static unsigned p67_cal_busy;

void rh_p67_calendar_reset(void);

static uint32_t rd_1043(uint32_t a) { return *(volatile const uint32_t *)(uintptr_t)a; }
static uint32_t half_1043(uint32_t a) { return *(volatile const uint16_t *)(uintptr_t)a; }
static unsigned byte_1043(uint32_t a) { return *(volatile const uint8_t *)(uintptr_t)a; }
static void barrier_1043(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static void zero_1043(void *p, unsigned n) {
    volatile uint8_t *s = p;
    while (n--) *s++ = 0;
}

static int audited_1043(void) {
    return rd_1043(P67_CAL_APP_SIGNAL_STOCK) == 0xd0002906u &&
           rd_1043(0x0ca6a004u) == 0x4e0eb5f8u &&
           rd_1043(P67_CAL_CLOCK) == 0xb085b5f0u &&
           rd_1043(P67_CAL_LOCALTIME) == 0xb082b580u &&
           rd_1043(P67_CAL_SNAPSHOT) == 0x41f0e92du &&
           rd_1043(P67_CAL_DRAW_BUF_CREATE) == 0xf000f85fu;
}

int rh_calendar_native_date(struct rh_calendar_date *out) {
    uint32_t ts[5], tm[11];
    struct rh_calendar_date d;
    unsigned i;
    if (!out || !audited_1043()) return -1;
    zero_1043(ts, sizeof(ts)); zero_1043(tm, sizeof(tm));
    P67_CAL_N(P67_CAL_CLOCK, 0, (uintptr_t)ts, 0, 0);
    if (!P67_CAL_N(P67_CAL_LOCALTIME, ts[0], ts[1], (uintptr_t)tm, 0) ||
        tm[5] > 8099u || tm[6] > 6u) return -1;
    d.year = (int32_t)tm[5] + 1900;
    d.month = (int32_t)tm[4] + 1;
    d.day = (int32_t)tm[3];
    d.weekday = tm[6] ? (int32_t)tm[6] - 1 : 6;
    if (!rh_calendar_date_valid(&d)) return -1;
    {
        static const char *const week_keys[7] = {
            "Sun.common", "Mon.common", "Tue.common", "Wed.common",
            "Thu.common", "Fri.common", "Sat.common"
        };
        const char *loc = (const char *)(uintptr_t)P67_CAL_N(P67_CAL_LOCALIZED_TEXT,
            (uintptr_t)week_keys[tm[6] <= 6 ? tm[6] : 0], 0, 0, 0);
        d.locale = 2166136261u;
        if (loc) {
            for (i = 0; i < 16 && loc[i]; ++i)
                d.locale = (d.locale ^ (uint8_t)loc[i]) * 16777619u;
        }
    }
    *out = d;
    return 0;
}

static int item_1043(uint32_t image, const struct rh_calendar_config *c,
                     const struct rh_calendar_date *d, unsigned i) {
    const struct rh_calendar_item *p = &c->items[i];
    uint32_t obj, font;
    const char *context;
    char text[16], path[RH_PATH];
    if (rh_calendar_item_context(c, d, i, &context)) return -1;
    if (p->type == RH_CALENDAR_NONE) return 0;
    zero_1043(text, sizeof(text));
    if (p->type == RH_CALENDAR_IMAGE) {
        if (!context || rh_calendar_image_path(path, context)) return -1;
        obj = P67_CAL_N(P67_CAL_IMAGE_CREATE, image, 0, 0, 0);
        if (!obj) return -1;
        P67_CAL_N(P67_CAL_IMAGE_SOURCE, obj, (uintptr_t)path, 0, 0);
    } else {
        obj = P67_CAL_N(P67_CAL_LABEL_CREATE, image, 0, 0, 0);
        if (!obj) return -1;
        if (!context) {
            if (i == 2) {
                static const char *const week_keys[7] = {
                    "Mon.common", "Tue.common", "Wed.common",
                    "Thu.common", "Fri.common", "Sat.common", "Sun.common"
                };
                context = (const char *)(uintptr_t)P67_CAL_N(P67_CAL_LOCALIZED_TEXT,
                    (uintptr_t)week_keys[d->weekday <= 6 ? d->weekday : 0], 0, 0, 0);
            } else {
                rh_calendar_decimal(text, i == 0 ? (unsigned)d->month : (unsigned)d->day);
                context = text;
            }
        }
        if (!context) return -1;
        P67_CAL_N(P67_CAL_LABEL_TEXT, obj, (uintptr_t)context, 0, 0);
        P67_CAL_N(P67_CAL_COLOR, obj, p->color_native ? (i == 2 ? 0x3482ffu : 0u) : p->color, 0, 0);
        P67_CAL_N(P67_CAL_TEXT_ALIGN, obj, 2, 0, 0);
        if (i == 1) {
            font = P67_CAL_N(P67_CAL_FONT_DAY, 0x2010cf58u, 0, 0, 0);
            P67_CAL_N(0x0c588e78u, obj, font, 0, 0);
        } else if (i == 2) {
            P67_CAL_N(P67_CAL_FONT_WEEK, obj, 0x2010d024u, 240, 0);
        }
    }
    P67_CAL_N(P67_CAL_ALIGN, obj, p->align, p->x, p->y);
    return 0;
}

static int save_1043(uint32_t buf, const char *output) {
    uint8_t header[12];
    unsigned i;
    uint32_t size, pixels, stride;
    int fd, rc;
    if (!buf || byte_1043(buf + 1) != 16u || half_1043(buf + 4) != 112u || half_1043(buf + 6) != 112u)
        return -1;
    stride = half_1043(buf + 8); size = rd_1043(buf + 12); pixels = rd_1043(buf + 16);
    if (stride < 448u || (stride & 3u) || size != stride * 112u ||
        size > 128u * 1024u || !pixels || (pixels & 3u)) return -1;

    barrier_1043();
    P67_CAL_N(P67_CAL_CACHE_CLEAN, 1, 0, 0x10000, 0);
    barrier_1043();
    P67_CAL_N(P67_CAL_CACHE_INVALIDATE, 1, pixels, size, 0);
    barrier_1043();

    for (i = 0; i < 12; ++i) header[i] = (uint8_t)byte_1043(buf + i);
    header[0] = 25;
    header[2] = 0x01; /* Premultiplied ARGB8888 */

    fd = rh_platform_open(output, 0x426);
    if (fd < 0) return -1;
    rc = rh_calendar_full_write(fd, header, sizeof(header));
    if (!rc) rc = rh_calendar_full_write(fd, (const void *)(uintptr_t)pixels, size);
    rh_platform_close(fd);
    return rc;
}

int rh_calendar_native_render(const struct rh_calendar_config *c,
                              const struct rh_calendar_date *d, const char *output) {
    uint32_t image, root, next_buf;
    unsigned i, next_slot;
    int rc = -1;
    if (!c || !c->custom) return 0;
    if (!rh_calendar_date_valid(d) || !rh_calendar_bounded(output, RH_PATH) || output[0] != '/' ||
        c->pool_used > RH_CALENDAR_POOL_BYTES || p67_cal_busy || !audited_1043()) return -1;
    p67_cal_busy = 1;
    if ((int32_t)P67_CAL_N(P67_CAL_GPU_FINISH, 0, 0, 0, 0)) goto done;

    next_slot = (p67_cal_active_buf == p67_cal_draw_bufs[0]) ? 1 : 0;
    if (!p67_cal_draw_bufs[next_slot]) {
        p67_cal_draw_bufs[next_slot] = P67_CAL_N(P67_CAL_DRAW_BUF_CREATE, 112, 112, 16, 0);
        if (!p67_cal_draw_bufs[next_slot]) goto done;
    }
    next_buf = p67_cal_draw_bufs[next_slot];

    root = P67_CAL_N(P67_CAL_ROOT_CREATE, 0, 0, 0, 0);
    if (!root) goto done;
    P67_CAL_N(P67_CAL_SIZE, root, 112, 112, 0);

    image = P67_CAL_N(P67_CAL_IMAGE_CREATE, root, 0, 0, 0);
    if (!image) { P67_CAL_N(P67_CAL_OBJECT_DELETE, root, 0, 0, 0); goto done; }
    P67_CAL_N(P67_CAL_IMAGE_SOURCE, image,
              (uintptr_t)"/resource/app/perpetual_calendar/calendar_background_icon.bin", 0, 0);
    P67_CAL_N(P67_CAL_SIZE, image, 112, 112, 0);
    P67_CAL_N(P67_CAL_ALIGN, image, 9, 0, 0);

    for (i = 0; i < 3; ++i) {
        if (item_1043(image, c, d, i)) {
            P67_CAL_N(P67_CAL_OBJECT_DELETE, root, 0, 0, 0);
            goto done;
        }
    }

    barrier_1043();
    P67_CAL_N(P67_CAL_CACHE_CLEAN, 1, 0, 0x10000, 0);
    barrier_1043();
    rc = P67_CAL_N(P67_CAL_SNAPSHOT, image, 16, next_buf, 0);
    P67_CAL_N(P67_CAL_OBJECT_DELETE, root, 0, 0, 0);

    if (rc != 1) { rc = -1; goto done; }
    if ((int32_t)P67_CAL_N(P67_CAL_GPU_FINISH, 0, 0, 0, 0)) { rc = -1; goto done; }

    rc = save_1043(next_buf, output);
    if (!rc) {
        p67_cal_active_buf = next_buf;
    }
done:
    p67_cal_busy = 0;
    return rc;
}

static void p67_calendar_signal_hook(uint32_t app, uint32_t signal) {
    if (signal == 6) {
        if (p67_cal_active_buf) {
            *(uint32_t *)(app + 12) = p67_cal_active_buf;
            return;
        }
    }
    if (p67_cal_original_signal) {
        ((void (*)(uint32_t, uint32_t))(uintptr_t)p67_cal_original_signal)(app, signal);
    }
}

void rh_p67_calendar_publish(void) {
    uint32_t app;
    typedef uint32_t (*lookup_fn)(const char *);
    typedef int (*eventbus_fn)(int, uint32_t, int);
    lookup_fn lookup = (lookup_fn)(uintptr_t)(P67_CAL_APP_LOOKUP | 1u);
    eventbus_fn send = (eventbus_fn)(uintptr_t)(P67_CAL_EVENTBUS_SEND | 1u);
    app = lookup("com.xiaomi.miwear.perpetual_calendar");
    if (!app) return;

    if (!p67_cal_active_buf) {
        rh_p67_calendar_reset();
        return;
    }

    if (*(uint32_t *)(app + 56) != (uint32_t)(uintptr_t)p67_calendar_signal_hook) {
        p67_cal_original_signal = *(uint32_t *)(app + 56);
        p67_cal_hooked_app = app;
        *(uint32_t *)(app + 56) = (uint32_t)(uintptr_t)p67_calendar_signal_hook;
    }

    *(uint32_t *)(app + 12) = p67_cal_active_buf;
    send(0x2b, app, 0);
}

void rh_p67_calendar_reset(void) {
    if (p67_cal_hooked_app) {
        if (p67_cal_original_signal) {
            *(uint32_t *)(p67_cal_hooked_app + 56) = p67_cal_original_signal;
        }
        ((void (*)(const char *))(uintptr_t)(0x0ca6a004u | 1u))("com.xiaomi.miwear.perpetual_calendar");
    }
    p67_cal_active_buf = 0;
    p67_cal_hooked_app = 0;
    p67_cal_original_signal = 0;
}

#endif

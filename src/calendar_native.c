#include "resource_hook_calendar_native.h"
#include "resource_hook_platform.h"
#include "resource_hook_target.h"

#if RH_Q66_CALENDAR && (defined(__arm__) || defined(__thumb__))
/* Exact backport identities are in target evidence and selected headers.
 * Original .177 read-only recovery: q66-ap.i64, AP SHA256
 * ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b.
 * No .139/.155 address delta is used. sub_C547870 creates root v2/image v3,
 * 112x112 each, centers v3, and snapshots v3 (NOT the opaque black v2).
 * sub_C502DE0 is the native root style; day/week labels retain native defaults.
 * Font style wrappers resolve property 90 through sub_C50934C:
 * 200c45dc = MiSans-Medium 48; 200c5af0 = MiSans-Demibold 20 (C7E6C00).
 * C898B1C indexes names at 2ca6f430: Demibold, Medium. C495008 creates
 * independent managed wrappers, including fallbacks; C4924FC releases them.
 * The adapter does not directly mutate shared wrappers, FT face sizes,
 * registry paths or fallback links. Native managed creation may resize shared
 * cached faces internally; ownership is of wrappers, not independent FT faces.
 * LVGL lv_color_t is byte-packed B,G,R; C3839AE packs low16+BYTE2
 * into property 88 without a channel swap; native week stores ff,82,34,
 * so the exact packed/RGB-hex value is 0x3482ff, NOT #F0403F.
 * ARGB8888 = cf 16 (C907918 case 16, C384C40 alpha byte +3); RAM/file
 * header is {magic:u8,cf:u8,flags:u16,w:u16,h:u16,stride:u16,res:u16}.
 * C8B7720 dispatches optional draw-buffer handler+12, not a guaranteed fence
 * or cache synchronization. Explicit GPU/cache operations remain required.
 * C4F8698 saves that 12-byte header (magic=25) + draw_buf.data_size bytes
 * from aligned data +16, after C907918 premultiply. Its caller ignores errors.
 * Here full writes are checked, and publication belongs to the caller.
 */
#define N(a,b,c,d,e) (((uint32_t (*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t)) \
    (uintptr_t)((a)|1u))((uintptr_t)(b),(uintptr_t)(c),(uintptr_t)(d),(uintptr_t)(e)))
#define CLOCK RH_CN_CLOCK
#define LOCALTIME RH_CN_LOCALTIME
#define ROOT_CREATE RH_CN_ROOT_CREATE
#define SIZE RH_CN_SIZE
#define ROOT_STYLE RH_CN_ROOT_STYLE
#define IMAGE_CREATE RH_CN_IMAGE_CREATE
#define IMAGE_SOURCE RH_CN_IMAGE_SOURCE
#define IMAGE_INFO RH_CN_IMAGE_INFO
#define LABEL_CREATE RH_CN_LABEL_CREATE
#define LABEL_TEXT RH_CN_LABEL_TEXT
#define COLOR RH_CN_COLOR
#define TEXT_ALIGN RH_CN_TEXT_ALIGN
#define FONT_STYLE RH_CN_FONT_STYLE
#define ALIGN RH_CN_ALIGN
#define FONT_RESOLVE RH_CN_FONT_RESOLVE
#define FONT_CREATE RH_CN_FONT_CREATE
#define FONT_DESTROY RH_CN_FONT_DESTROY
#define WEEKDAY RH_CN_WEEKDAY
#define SNAPSHOT RH_CN_SNAPSHOT
#define SNAPSHOT_FREE RH_CN_SNAPSHOT_FREE
#define OBJECT_DELETE RH_CN_OBJECT_DELETE
#define GPU_FINISH RH_CN_GPU_FINISH
#define CACHE_SYNC RH_CN_CACHE_SYNC
#define CACHE_INVALIDATE RH_CN_CACHE_INVALIDATE
#define PREMULTIPLY RH_CN_PREMULTIPLY
#define INVALIDATE_BUF RH_CN_INVALIDATE_BUF

static uint32_t rd(uint32_t a) { return *(volatile const uint32_t *)(uintptr_t)a; }
static uint32_t half(uint32_t a) { return *(volatile const uint16_t *)(uintptr_t)a; }
static unsigned byte(uint32_t a) { return *(volatile const uint8_t *)(uintptr_t)a; }
static void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static void zero(void *p, unsigned n) {
    volatile uint8_t *s = p;
    while (n--) *s++ = 0;
}
/* Small immutable instruction receipts detect a wrong selected AP before any
 * clock/graphics call; full AP SHA is checked by the offline firmware tests. */
int rh_calendar_native_supported(void) { return 1; }
static int audited(void) {
    return rd(RH_CN_GENERATE) == RH_CN_RECEIPT_GENERATE &&
           rd(RH_CN_GENERATE + 4u) == RH_CN_RECEIPT_GENERATE_NEXT &&
           rd(CLOCK) == RH_CN_RECEIPT_CLOCK && rd(LOCALTIME) == RH_CN_RECEIPT_LOCALTIME &&
           rd(SNAPSHOT) == RH_CN_RECEIPT_SNAPSHOT && rd(FONT_CREATE) == RH_CN_RECEIPT_FONT_CREATE &&
           rd(FONT_DESTROY) == RH_CN_RECEIPT_FONT_DESTROY && rd(GPU_FINISH) == RH_CN_RECEIPT_GPU_FINISH;
}
static int date_valid(const struct rh_calendar_date *d) {
    int days;
    if (!d || d->year < 1900 || d->year > 9999 || d->month < 1 || d->month > 12 ||
        d->day < 1 || d->weekday < 0 || d->weekday > 6) return 0;
    days = d->month == 2 ? 28 + (!(d->year % 4) &&
           ((d->year % 100) || !(d->year % 400))) :
           (d->month == 4 || d->month == 6 || d->month == 9 || d->month == 11 ? 30 : 31);
    return d->day <= days;
}
int rh_calendar_native_date(struct rh_calendar_date *out) {
    uint32_t ts[5], tm[11];
    struct rh_calendar_date d;
    char localized[16];
    unsigned i;
    if (!out || !audited()) return -1;
    zero(ts, sizeof(ts)); zero(tm, sizeof(tm)); zero(localized, sizeof(localized));
    /* Same realtime/localtime_r pair as C547870. CLOCK returns the previous
     * BASEPRI (MRS R0,BASEPRI), NOT a POSIX success code. Validate its output
     * rather than rejecting a legitimate nonzero interrupt-mask return.
     * time_t is 64-bit; tm: mday+12, mon+16, year+20, Sunday wday+24. */
    ts[2] = 0xffffffffu;
    N(CLOCK, ts, 0, 0, 0);
    if (ts[2] >= 1000000000u || !N(LOCALTIME, ts, tm, 0, 0) ||
        tm[5] > 8099u || tm[6] > 6u) return -1;
    d.year = (int32_t)tm[5] + 1900;
    d.month = (int32_t)tm[4] + 1; d.day = (int32_t)tm[3];
    d.weekday = tm[6] ? (int32_t)tm[6] - 1 : 6;
    if (!date_valid(&d)) return -1;
    N(WEEKDAY, tm[6], localized, 0, 0);
    d.locale = 2166136261u;
    for (i = 0; i < sizeof(localized) && localized[i]; ++i)
        d.locale = (d.locale ^ (uint8_t)localized[i]) * 16777619u;
    *out = d; return 0;
}
/* Native snapshot drains draw tasks, but that may only submit GPU commands.
 * Keep ALL potentially referenced objects/fonts/path storage on a failed
 * finish; bounded single pending generation, not a heap-growing retry list. */
static struct {
    uint32_t root, snapshot, fonts[3];
    char paths[3][RH_PATH];
    unsigned busy;
} owned;
static void cleanup(void) {
    unsigned i;
    if (owned.root) N(OBJECT_DELETE, owned.root, 0, 0, 0);
    if (owned.snapshot) N(SNAPSHOT_FREE, owned.snapshot, 0, 0, 0);
    for (i = 0; i < 3; ++i) {
        if (owned.fonts[i]) N(FONT_DESTROY, owned.fonts[i], 0, 0, 0);
        owned.fonts[i] = 0;
    }
    owned.root = owned.snapshot = 0;
}
static void decimal(char *out, unsigned value) {
    if (value >= 10) *out++ = (char)('0' + value / 10);
    *out++ = (char)('0' + value % 10); *out = 0;
}
static int bounded(const char *s, unsigned limit) {
    unsigned i;
    if (!s) return 0;
    for (i = 0; i < limit; ++i) if (!s[i]) return 1;
    return 0;
}
static int image_path(char *out, const char *relative) {
    const char *root = RH_APP_FILES_ROOT;
    unsigned n = 0, i = 0;
    const char *prefix = "themes/";
    if (!relative) return -1;
    while (prefix[i]) { if (relative[i] != prefix[i]) return -1; ++i; }
    i = 0;
    while (root[n]) { out[n] = root[n]; ++n; }
    while (relative[i]) {
        if (n + 1 >= RH_PATH) return -1;
        out[n++] = relative[i++];
    }
    out[n] = 0; return 0;
}
static int item(uint32_t image, const struct rh_calendar_config *c,
                const struct rh_calendar_date *d, unsigned i) {
    const struct rh_calendar_item *p = &c->items[i];
    uint32_t obj, font;
    unsigned index = i == 0 ? (unsigned)d->month - 1u :
                     i == 1 ? (unsigned)d->day - 1u : (unsigned)d->weekday;
    unsigned count = i == 0 ? 12u : i == 1 ? 31u : 7u;
    const char *context = 0;
    char text[16];
    if (p->type == RH_CALENDAR_NONE) return 0;
    zero(text, sizeof(text));
    if (p->type > RH_CALENDAR_IMAGE ||
        (p->align != 1 && p->align != 2 && p->align != 3 && p->align != 9) ||
        p->x < -112 || p->x > 112 || p->y < -112 || p->y > 112 ||
        p->size > 72 || (p->size && p->size < 8) || p->color_native > 1 || p->color > 0xffffffu ||
        (p->count && p->count != count)) return -1;
    if (p->count) {
        unsigned available;
        context = rh_calendar_context(c, i, index);
        if (!context || p->context[index] >= c->pool_used) return -1;
        available = c->pool_used - p->context[index];
        if (available > (p->type == RH_CALENDAR_TEXT ? 64u : RH_PATH))
            available = p->type == RH_CALENDAR_TEXT ? 64u : RH_PATH;
        if (!bounded(context, available)) return -1;
    }
    if (p->type == RH_CALENDAR_IMAGE) {
        uint16_t info[6];
        zero(info, sizeof(info));
        if (!context || image_path(owned.paths[i], context) ||
            N(IMAGE_INFO, owned.paths[i], info, 0, 0) != 1 || !info[2] || !info[3]) return -1;
        obj = N(IMAGE_CREATE, image, 0, 0, 0);
        if (!obj) return -1;
        N(IMAGE_SOURCE, obj, owned.paths[i], 0, 0);
        if (!rd(obj + 52) || rd(obj + 68) != info[2] || rd(obj + 72) != info[3]) return -1;
        /* size is text-only. Images retain the BIN's native dimensions. */
    } else {
        obj = N(LABEL_CREATE, image, 0, 0, 0);
        if (!obj) return -1;
        if (!context) {
            if (i == 2) N(WEEKDAY, (d->weekday + 1) % 7, text, 0, 0);
            else decimal(text, i == 0 ? (unsigned)d->month : (unsigned)d->day);
            context = text;
        }
        N(LABEL_TEXT, obj, context, 0, 0);
        N(COLOR, obj, p->color_native ? (i == 2 ? 0x3482ffu : 0u) : p->color, 0, 0);
        N(TEXT_ALIGN, obj, 2, 0, 0);
        if (!p->size) {
            font = N(FONT_RESOLVE, i == 1 ? RH_CN_FONT_DAY : RH_CN_FONT_WEEK, 0, 0, 0);
        } else {
            /* C898B1C's exact family, with a fresh module-owned size wrapper.
             * Managed creation may return the selected native fallback on failure: reject it. */
            font = N(FONT_CREATE, i == 1 ? RH_CN_FONT_NAME_DAY : RH_CN_FONT_NAME_WEEK, p->size, 0, 0);
            if (font && font != RH_CN_FONT_FALLBACK) owned.fonts[i] = font;
            else return -1;
        }
        if (!font) return -1;
        N(FONT_STYLE, obj, font, 0, 0);
    }
    N(ALIGN, obj, p->align, p->x, p->y);
    return 0;
}
static int full_write(int fd, const void *data, uint32_t size) {
    const uint8_t *p = data;
    while (size) {
        int n = rh_platform_write(fd, p, size);
        if (n <= 0 || (uint32_t)n > size) return -1;
        p += (unsigned)n; size -= (unsigned)n;
    }
    return 0;
}
static int save(uint32_t buf, const char *output) {
    uint8_t header[12];
    unsigned i;
    uint32_t size, pixels, stride;
    int fd, rc;
    if (!buf || byte(buf + 1) != 16u || half(buf + 4) != 112u || half(buf + 6) != 112u)
        return -1;
    stride = half(buf + 8); size = rd(buf + 12); pixels = rd(buf + 16);
    if (stride < 448u || (stride & 3u) || size != stride * 112u ||
        size > 128u * 1024u || !pixels || (pixels & 3u) ||
        (half(buf + 2) & ~0x31u) || !(half(buf + 2) & 0x10u) ||
        (!(half(buf + 2) & 1u) && !(half(buf + 2) & 0x20u)) || half(buf + 10)) return -1;
    /* The existing Band11 blur fence/cache contract: sync unrelated dirty lines
     * before invalidating GPU-written pixels. Never read them before finish. */
    barrier(); N(CACHE_SYNC, 1, 0, 0, 0); barrier();
    N(CACHE_INVALIDATE, 1, pixels, size, 0); barrier();
    N(PREMULTIPLY, buf, 0, 0, 0);
    if (!(half(buf + 2) & 1u)) return -1;
    N(INVALIDATE_BUF, buf, 0, 0, 0);
    for (i = 0; i < 12; ++i) header[i] = (uint8_t)byte(buf + i);
    header[0] = 25;
    /* RAM ownership/mutability flags are not a filesystem image contract.
     * Preserve PREMULTIPLIED, reject compressed/reserved metadata above. */
    header[2] &= (uint8_t)~0x30u;
    /* Exact C4F8698 open mode. Platform supplies permissions and normalizes
     * native errors; the complete-write loop never claims a partial bank. */
    fd = rh_platform_open(output, 0x426);
    if (fd < 0) return -1;
    rc = full_write(fd, header, sizeof(header));
    if (!rc) rc = full_write(fd, (const void *)(uintptr_t)pixels, size);
    rh_platform_close(fd);
    return rc;
}
int rh_calendar_native_render(const struct rh_calendar_config *c,
                              const struct rh_calendar_date *d, const char *output) {
    uint32_t image;
    unsigned i;
    int rc = -1;
    if (!c || !date_valid(d) || !bounded(output, RH_PATH) || output[0] != '/' ||
        c->pool_used > RH_CALENDAR_POOL_BYTES || owned.busy || !audited()) return -1;
    owned.busy = 1;
    if ((int32_t)N(GPU_FINISH, 0, 0, 0, 0)) goto done;
    cleanup();
    owned.root = N(ROOT_CREATE, 0, 0, 0, 0);
    if (!owned.root) goto release;
    N(SIZE, owned.root, 112, 112, 0);
    N(ROOT_STYLE, owned.root, 0, 0, 0);
    image = N(IMAGE_CREATE, owned.root, 0, 0, 0);
    if (!image) goto release;
    N(IMAGE_SOURCE, image, "/resource/app/perpetual_calendar/launcher_icon.bin", 0, 0);
    if (!rd(image + 52) || !rd(image + 68) || !rd(image + 72)) goto release;
    N(SIZE, image, 112, 112, 0);
    N(ALIGN, image, 9, 0, 0);
    for (i = 0; i < 3; ++i) if (item(image, c, d, i)) goto release;
    barrier(); N(CACHE_SYNC, 1, 0, 0, 0); barrier();
    /* ARGB8888 rather than stock RGB565 (18): preserve transparent corners. */
    owned.snapshot = N(SNAPSHOT, image, 16, 0, 0);
    /* Even a failed snapshot call must obtain a completion boundary before
     * deleting potential resources. Native allocation failures occur before
     * draw dispatch, but no replacement assumption is necessary here. */
    if ((int32_t)N(GPU_FINISH, 0, 0, 0, 0)) goto done;
    if (owned.snapshot) rc = save(owned.snapshot, output);
release:
    cleanup();
done:
    owned.busy = 0;
    return rc;
}
#elif defined(RH_TARGET_1043) && RH_TARGET_1043 && (defined(__arm__) || defined(__thumb__))
#include "p67_calendar_native.inc"
#else
int rh_calendar_native_supported(void) {
#if RH_Q66_CALENDAR || (defined(RH_TARGET_1043) && RH_TARGET_1043)
    return 1;
#else
    return 0;
#endif
}
int rh_calendar_native_date(struct rh_calendar_date *out) { (void)out; return -1; }
int rh_calendar_native_render(const struct rh_calendar_config *c,
                              const struct rh_calendar_date *d, const char *output) {
    (void)c; (void)d; (void)output; return -1;
}
#if defined(RH_TARGET_1043) && RH_TARGET_1043
void rh_p67_calendar_publish(void) {}
void rh_p67_calendar_reset(void) {}
uint32_t rh_p67_calendar_get_draw_buf(void) { return 0; }
#endif
#endif


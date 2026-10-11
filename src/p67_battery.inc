/* Exact .043 Control Center battery: capture at idle, draw an immutable image.
 * A derived per-instance class suppresses only DRAW_MAIN. PREPROCESS cannot
 * suppress class drawing on this firmware (2BF690 -> 2BF710).
 * Default geometry is 62x32 plus 30px extra on all sides: 122x92, NOT 64x36.
 */
#include "resource_hook_p67_battery.h"

#define P67_BATTERY_CLASS 0x2cd198dcu
#define P67_BATTERY_EVENT 0x0c4c22cdu
#define P67_BATTERY_ROOT_SLOT 0x2010fe50u
#define P67_BATTERY_STATUS_SLOT 0x2010fe60u

struct p67_battery_key {
    uint32_t width, height, extra;
    unsigned char state[42];
};
static uint32_t p67_battery_class[9];
static uint32_t p67_battery_obj, p67_battery_root, p67_battery_status;
static uint32_t p67_battery_generation, p67_battery_buffer;
static unsigned p67_battery_busy, p67_battery_ready;
static struct p67_battery_key p67_battery_cached;
static void p67_battery_event(uint32_t clz, void *event);

static uint32_t p67_battery_shim(void) {
    return (uint32_t)(uintptr_t)p67_battery_class;
}
static uint32_t p67_battery_parent(uint32_t obj) {
    return ((uint32_t (*)(uint32_t))(uintptr_t)P67_NATIVE(0x0c589579u))(obj);
}
/* Draw dispatch owns obj and its attached parents. Never walk the global
 * object registry during rendering or dereference a retained, detached parent. */
static int p67_battery_attached(uint32_t obj) {
    uint32_t status;
    if (!obj || obj != p67_battery_obj ||
        load32(P67_BATTERY_ROOT_SLOT) != p67_battery_root ||
        load32(P67_BATTERY_STATUS_SLOT) != p67_battery_status) return 0;
    status = p67_battery_parent(obj);
    return status && status == p67_battery_status &&
           p67_battery_parent(status) == p67_battery_root;
}
static int p67_battery_key_read(uint32_t obj, struct p67_battery_key *key,
                                 int32_t area[4]) {
    int32_t width, height;
    uint32_t extra;
    unsigned i;
    for (i = 0; i < 4u; ++i) area[i] = (int32_t)P67_U32(obj + 28u + 4u * i);
    /* Firmware coordinates are screen-sized. Reject pathological values
     * before signed subtraction and before passing dimensions to allocation. */
    for (i = 0; i < 4u; ++i)
        if (area[i] < -4096 || area[i] > 4096) return 0;
    width = area[2] - area[0] + 1;
    height = area[3] - area[1] + 1;
    extra = ((uint32_t (*)(uint32_t))(uintptr_t)P67_NATIVE(0x0c587e71u))(obj);
    if (width <= 0 || height <= 0 || width > 128 || height > 128 ||
        extra > 63u || (uint32_t)width + 2u * extra > 128u ||
        (uint32_t)height + 2u * extra > 128u) return 0;
    key->width = (uint32_t)width; key->height = (uint32_t)height; key->extra = extra;
    for (i = 0; i < 20u; ++i) key->state[i] = P67_U8(obj + 52u + i);
    for (i = 0; i < 22u; ++i) key->state[20u + i] = P67_U8(obj + 80u + i);
    return 1;
}
static int p67_battery_key_equal(const struct p67_battery_key *a,
                                  const struct p67_battery_key *b) {
    unsigned i;
    if (a->width != b->width || a->height != b->height || a->extra != b->extra) return 0;
    for (i = 0; i < 42u; ++i) if (a->state[i] != b->state[i]) return 0;
    return 1;
}
static void p67_battery_key_store(const struct p67_battery_key *key) {
    unsigned i;
    p67_battery_cached.width = key->width;
    p67_battery_cached.height = key->height;
    p67_battery_cached.extra = key->extra;
    for (i = 0; i < 42u; ++i) p67_battery_cached.state[i] = key->state[i];
}
/* Styles and resource/font transactions can change pixels without changing
 * native source pointers. Invalidation is logical only, safe during callbacks. */
static void p67_battery_invalidate(void) {
    p67_battery_ready = 0; ++p67_battery_generation;
}
static void p67_battery_native_event(void *event) {
    ((void (*)(uint32_t, void *))(uintptr_t)P67_NATIVE(P67_BATTERY_EVENT))
        (P67_BATTERY_CLASS, event);
}
static void p67_battery_event(uint32_t clz, void *event) {
    uint32_t obj = *((uint32_t *)event + 1);
    unsigned code = *(uint16_t *)((unsigned char *)event + 8) & 0x7fffu;
    struct p67_battery_key key;
    int32_t area[4];
    (void)clz;
    /* C107104 sends STYLE_CHANGED=45; forwarded styles may change fonts,
     * opacity or pixels while all battery scalar/source fields stay equal. */
    if (code == 45u && obj == p67_battery_obj) p67_battery_invalidate();
    if (code == 36u && obj == p67_battery_obj) {
        /* No registry walk, class mutation, GPU fence or buffer free in DELETE.
         * The destructor walker will visit the original class exactly once. */
        p67_battery_obj = p67_battery_root = p67_battery_status = 0;
        p67_battery_ready = 0; ++p67_battery_generation;
    }
    if (code == 24u && !p67_battery_busy && p67_battery_ready &&
        p67_battery_attached(obj) && p67_battery_key_read(obj, &key, area) &&
        p67_battery_key_equal(&key, &p67_battery_cached)) {
        uint32_t layer = *((uint32_t *)event + 4);
        if (layer) {
            uint32_t descriptor[28];
            int32_t extra = (int32_t)key.extra;
            area[0] -= extra; area[1] -= extra;
            area[2] += extra; area[3] += extra;
            ((void (*)(void *))(uintptr_t)P67_NATIVE(0x0c5882b9u))(descriptor);
            descriptor[7] = p67_battery_buffer;
            /* Native image tasks inherit the live layer's clip, transform and
             * opacity. No framebuffer pointers or wallpaper coordinates. */
            ((void (*)(uint32_t, const void *, const int32_t *))(uintptr_t)
                P67_NATIVE(0x0c5882a1u))(layer, descriptor, area);
            return;
        }
    }
    /* Changed state, missing buffer or capture: preserve the complete native
     * implementation, including refresh-extra, cover checks and base events. */
    p67_battery_native_event(event);
}
static void p67_battery_detach(void) {
    uint32_t obj = p67_battery_obj;
    p67_battery_obj = p67_battery_root = p67_battery_status = 0;
    p67_battery_ready = 0; ++p67_battery_generation;
    if (p67_live(obj) && load32(obj) == p67_battery_shim())
        P67_U32(obj) = P67_BATTERY_CLASS;
    /* Buffer remains pinned until idle LVGL queues AND GPU completion. */
}
static uint32_t p67_battery_find(void) {
    uint32_t root = load32(P67_BATTERY_ROOT_SLOT);
    uint32_t status = load32(P67_BATTERY_STATUS_SLOT), count, found = 0;
    unsigned i;
    if (!p67_live(root) || !p67_live(status) || p67_battery_parent(status) != root) return 0;
    count = ((uint32_t (*)(uint32_t))(uintptr_t)P67_NATIVE(0x0c5881d9u))(status);
    if (count > 16u) return 0;
    for (i = 0; i < count; ++i) {
        uint32_t child = ((uint32_t (*)(uint32_t, int32_t))(uintptr_t)
            P67_NATIVE(0x0c5886e1u))(status, (int32_t)i);
        uint32_t clz;
        if (!child) continue;
        clz = load32(child);
        if (clz != P67_BATTERY_CLASS && clz != p67_battery_shim()) continue;
        if (found || !p67_live(child) || p67_battery_parent(child) != status) return 0;
        found = child;
    }
    return found;
}
static int p67_battery_buffer_view(uint32_t width, uint32_t height,
                                    uint32_t *pixels, uint32_t *stride, uint32_t *size) {
    uint32_t buf = p67_battery_buffer, allocation;
    if (!p67_blur_native_ram(buf, 28u) || (P67_U32(buf) & 0xffffu) != 0x1019u ||
        P67_U32(buf + 4u) != (height << 16 | width)) return 0;
    *stride = P67_U32(buf + 8u) & 0xffffu;
    *size = P67_U32(buf + 12u); *pixels = P67_U32(buf + 16u);
    allocation = P67_U32(buf + 20u);
    return !(*pixels & 63u) && *pixels >= allocation && *pixels - allocation <= 63u &&
           !(*stride & 3u) && *stride >= width * 4u && *stride <= 2048u &&
           *size >= *stride * height && *size <= 128u * 2048u &&
           p67_blur_native_ram(allocation, *size + *pixels - allocation) &&
           P67_U32(buf + 24u) == 0x20103244u;
}
static void p67_battery_free(void) {
    uint32_t buffer = p67_battery_buffer;
    p67_battery_buffer = 0; p67_battery_ready = 0;
    if (buffer) ((void (*)(uint32_t))(uintptr_t)P67_NATIVE(0x0cac0db9u))(buffer);
}
/* Caller is the reserved UI-owner background poll, never a draw callback.
 * One persistent allocation; safe idle fences precede ALL reuse/free. */
static void p67_battery_poll(int enabled) {
    uint32_t obj, generation, width, height, pixels, stride, size;
    struct p67_battery_key key, after;
    int32_t area[4];
    int captured;
    unsigned i, powered = 0;
    if (p67_battery_busy) return;
    p67_battery_busy = 1;
    if (!enabled) {
        p67_battery_detach();
        if (p67_battery_buffer && p67_blur_native_idle() && p67_blur_native_fence() &&
            p67_blur_native_idle()) p67_battery_free();
        goto done;
    }
    if (!p67_blur_native_idle()) goto done;
    obj = p67_battery_find();
    if (obj != p67_battery_obj || (obj && !p67_battery_attached(obj))) {
        p67_battery_detach();
        if (obj && load32(obj) == P67_BATTERY_CLASS) {
            for (i = 0; i < 9u; ++i) p67_battery_class[i] = P67_U32(P67_BATTERY_CLASS + 4u * i);
            p67_battery_class[0] = P67_BATTERY_CLASS;
            p67_battery_class[1] = p67_battery_class[2] = 0;
            p67_battery_class[3] = (uint32_t)(uintptr_t)p67_battery_event;
            p67_battery_obj = obj;
            p67_battery_root = load32(P67_BATTERY_ROOT_SLOT);
            p67_battery_status = load32(P67_BATTERY_STATUS_SLOT);
            P67_U32(obj) = p67_battery_shim();
        }
    }
    obj = p67_battery_obj; generation = p67_battery_generation;
    if (!obj) {
        if (p67_battery_buffer && p67_blur_native_idle() && p67_blur_native_fence() &&
            p67_blur_native_idle()) p67_battery_free();
        goto done;
    }
    if (!p67_battery_key_read(obj, &key, area)) { p67_battery_ready = 0; goto done; }
    if (p67_battery_ready && p67_battery_key_equal(&key, &p67_battery_cached)) goto done;
    p67_battery_ready = 0;
    /* Keep the native snapshot power guard across capture and CPU readback;
     * nested snapshot/fence helpers borrow the same refcounted lock. */
    p67_blur_native_power_begin(); powered = 1;
    if (!p67_blur_native_fence() || !p67_blur_native_idle()) goto done;
    if (p67_battery_obj != obj || p67_battery_generation != generation ||
        !p67_live(obj) || !p67_battery_attached(obj) || load32(obj) != p67_battery_shim()) goto done;
    width = key.width + 2u * key.extra; height = key.height + 2u * key.extra;
    if (p67_battery_buffer && !p67_battery_buffer_view(width, height, &pixels, &stride, &size))
        p67_battery_free();
    if (!p67_battery_buffer)
        p67_battery_buffer = ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))
            (uintptr_t)P67_NATIVE(0x0c587f39u))(width, height, 16u, 0u);
    if (!p67_battery_buffer || !p67_battery_buffer_view(width, height, &pixels, &stride, &size)) goto done;
    /* Allocators/cache/PM calls may invoke native callbacks. Revalidate the
     * captured identity before handing any borrowed object to snapshot. */
    p67_blur_native_clean();
    if (p67_battery_obj != obj || p67_battery_generation != generation ||
        !p67_live(obj) || !p67_battery_attached(obj) || load32(obj) != p67_battery_shim()) goto done;
    captured = ((int (*)(uint32_t, uint32_t, uint32_t))(uintptr_t)
        P67_NATIVE(0x0ca5fb41u))(obj, 16u, p67_battery_buffer);
    /* The snapshot drains its LVGL tasks; a successful GPU fence is still
     * required before CPU reads. Failed completion pins storage for retry. */
    if (!p67_blur_native_fence() || !p67_blur_native_idle() || captured != 1) goto done;
    if (p67_battery_obj != obj || p67_battery_generation != generation ||
        !p67_live(obj) || !p67_battery_attached(obj) ||
        !p67_battery_key_read(obj, &after, area)) goto done;
    /* Layout and first draw may update geometry and cached source dimensions. */
    if (after.width + 2u * after.extra != width || after.height + 2u * after.extra != height ||
        !p67_battery_buffer_view(width, height, &pixels, &stride, &size)) goto done;
    p67_blur_native_clean();
    p67_blur_native_invalidate(pixels, size);
#ifdef P67_BATTERY_TEST_PIXELS
    if (!p67_battery_key_black(P67_BATTERY_TEST_PIXELS(pixels), size, stride, width, height)) goto done;
#else
    if (!p67_battery_key_black((void *)(uintptr_t)pixels, size, stride, width, height)) goto done;
#endif
    p67_blur_native_clean();
    p67_battery_key_store(&after); p67_battery_ready = 1;
    p67_wake_pending = 1;
done:
    if (powered) p67_blur_native_power_end();
    p67_battery_busy = 0;
}

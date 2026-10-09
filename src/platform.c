#include "resource_hook_platform.h"
#include "resource_hook_target.h"
#include "resource_hook_background.h"
#include "resource_hook_blur.h"
#include "resource_hook_calendar_runtime.h"
#include "resource_hook_calendar_internal.h"
#include "canopus_veneer.h"

static int *errno_location(void) {
#if defined(RH_FW_ERRNO_LOCATION)
    return ((int *(*)(void))(uintptr_t)RH_FW_ERRNO_LOCATION)();
#else
    return canopus_fw_errno_location();
#endif
}

/* Only the selected, fingerprinted .139, .155 or 10 Pro .043 AP image is supported. */
int rh_platform_open(const char *path, int mode) {
    /* Always supply permissions when O_CREAT is present (NuttX bit 2).
     * Extra varargs are ignored by opens of existing files/devices. */
    int rc = ((int (*)(const char *, int, ...))(uintptr_t)RH_FW_OPEN)(path, mode, 0600);
#if RH_FW_OPEN_NEGATIVE_ERRNO
    /* nx_open returns -errno without updating task errno. Normalize to the
     * POSIX contract used by the module. */
    if (rc < 0) {
        int *value = errno_location();
        if (value) *value = -rc;
        return -1;
    }
#endif
    return rc;
}
int rh_platform_errno(void) {
    const int *value = errno_location();
    return value ? *value : 0;
}
int rh_platform_read(int fd, void *out, uint32_t size) {
    return ((int (*)(int, void *, uint32_t))(uintptr_t)RH_FW_READ)(fd, out, size);
}
int rh_platform_write(int fd, const void *data, uint32_t size) {
    return ((int (*)(int, const void *, uint32_t))(uintptr_t)RH_FW_WRITE)(fd, data, size);
}
void rh_platform_close(int fd) {
    ((int (*)(int))(uintptr_t)RH_FW_CLOSE)(fd);
}
void *rh_platform_driver(void) { return (void *)(uintptr_t)RH_FW_POSIX_DRIVER; }
rh_open_fn *rh_platform_slot(void) { return (rh_open_fn *)(uintptr_t)RH_FW_POSIX_OPEN_SLOT; }
rh_open_fn rh_platform_original(void) { return (rh_open_fn)(uintptr_t)RH_FW_POSIX_OPEN; }
int rh_platform_driver_valid(void) {
    return *(volatile unsigned char *)(uintptr_t)RH_FW_POSIX_DRIVER == '/' &&
           *(volatile uint32_t *)(uintptr_t)RH_FW_POSIX_CACHE_SIZE == 4096u;
}
/* Exact owner adapter. The supported APs use the validated LRU/RB entry-list
 * shape; target-specific cache/object classes and callable addresses live in
 * resource_hook_target.h. Never retire unrelated global cache entries. */
#define RH_RELOAD_LIMIT 1024u
static uint32_t load32(uint32_t address) {
    return *(volatile uint32_t *)(uintptr_t)address;
}
static int affected(const struct rh_mapping_view *previous,
                    const struct rh_mapping_view *current, uint32_t source) {
    char mapped[RH_PATH];
    const char *path;
    if (!source || *(const char *)(uintptr_t)source != '/') return 0;
    path = (const char *)(uintptr_t)source;
    /* A changed rule may need to restore an owner covered only by the old
     * mapping, or redirect one covered only by the new mapping. */
    return (previous && previous->count && rh_resolve_view(previous, path, mapped) == 1) ||
           (current && current->count && rh_resolve_view(current, path, mapped) == 1);
}
/* Read-only bounded validation before any retirement. Both exact APs initialize
 * ll.node_size=4; unlink reads next at node+node_size+4. The list payload points
 * to an RB node, whose +16 points to entry data. */
static int cache_valid(uint32_t cache, uint32_t clz) {
    uint32_t node, n = 0;
    if (!cache || load32(cache) != clz || load32(cache + 48u) != 4u) return 0;
    for (node = load32(cache + 52u); node; node = load32(node + 8u)) {
        uint32_t rb;
        if (++n > 4096u || !(rb = load32(node)) || !load32(rb + 16u)) return 0;
    }
    return 1;
}
static int same_path(uint32_t source, const char *path) {
    uint32_t i;
    const char *value = (const char *)(uintptr_t)source;
    if (!source) return 0;
    for (i = 0; i < RH_PATH; i++) {
        if (value[i] != path[i]) return 0;
        if (!path[i]) return 1;
    }
    return 0;
}
static int retire_cache(uint32_t cache, uint32_t source_offset,
                        const struct rh_mapping_view *mapping, const char *exact) {
    uint32_t node = load32(cache + 52u), n = 0;
    while (node) {
        uint32_t next, data, source;
        if (++n > 4096u) return -1;
        data = load32(load32(node) + 16u);
        next = load32(node + 8u);
        source = load32(data + source_offset);
        /* Header {src,type,...}; decoded {buffer,src,type,...}. Never read a
         * descriptor/symbol as a pathname, or retire an unrelated cache key. */
        if (*(const unsigned char *)(uintptr_t)(data + source_offset + 4u) == 1u &&
            ((mapping && affected(mapping, 0, source)) ||
             (exact && same_path(source, exact)))) {
            uint32_t cursor, guard = 0;
            ((void (*)(uint32_t, uint32_t))(uintptr_t)RH_FW_CACHE_DROP)(cache, data);
            /* No dereference of the retired node/data/source after drop. Check
             * that unlink really happened, even when dropping a non-head key.
             * Class lookup may move the target to the head before unlinking. */
            for (cursor = load32(cache + 52u); cursor; cursor = load32(cursor + 8u)) {
                if (cursor == node || ++guard > 4096u) return -1;
            }
        }
        node = next;
    }
    return 0;
}
int rh_platform_retire_mapped_images(const struct rh_mapping_view *mapping) {
    uint32_t data = load32(RH_FW_IMAGE_CACHE_SLOT);
    uint32_t header = load32(RH_FW_HEADER_CACHE_SLOT);
    if (rh_validate_view(mapping)) return -1;
    if (!mapping->count) return 1;
    if (!cache_valid(data, RH_FW_IMAGE_CACHE_CLASS) ||
        !cache_valid(header, RH_FW_HEADER_CACHE_CLASS)) return -1;
    if (retire_cache(header, 0u, mapping, 0)) return -1;
    return retire_cache(data, 4u, mapping, 0);
}
#if RH_Q66_BACKGROUND
/* Screen-fixed native backgrounds for launcher, control center, and
 * notifications. ARM ownership maps replace scrolling property-40 images
 * with per-object DRAW_MAIN_END callbacks, before native child traversal. */
static char bg_paths[3][2][RH_PATH];
static unsigned bg_path_index;
static unsigned bg_active[3];

static const uint32_t launcher_slots[4] = {
    RH_LB_MAIN_ROOT_SLOT, RH_LB_MAIN_CACHE_SLOT,
    RH_LB_MORE_ROOT_SLOT, RH_LB_MORE_CACHE_SLOT
};

static uint32_t bg_child(uint32_t root) {
#if defined(RH_LB_CHILD_GET)
    return ((uint32_t (*)(uint32_t, int))(uintptr_t)RH_LB_CHILD_GET)(root, 0);
#endif
    uint32_t spec;
    if (!root) return 0;
    spec = load32(root + 8u);
    if (!spec || *(volatile const uint16_t *)(uintptr_t)(spec + 48u) == 0) return 0;
    return load32(load32(spec));
}

static uint32_t launcher_list(unsigned i) {
    uint32_t root = load32(launcher_slots[i]);
    uint32_t list;
    if (!root) return 0;
    list = bg_child(root);
    return list && load32(list) == RH_LB_LIST_CLASS ? list : 0;
}

static uint32_t control_center_container(void) {
    uint32_t pres = load32(RH_LB_CONTROL_PRESENTER_SLOT);
    uint32_t root, container;
    if (!pres) return 0;
    root = load32(pres + 8u);
    if (!root) return 0;
    container = bg_child(root);
    return container && load32(container) == RH_LB_BASE_CLASS ? container : 0;
}

static uint32_t notify_container(void) {
    uint32_t container = load32(RH_LB_NOTIFY_CONTAINER_SLOT);
    return container && load32(container) == RH_LB_BASE_CLASS ? container : 0;
}

#if defined(__arm__) || defined(__thumb__)
struct screen_background { uint32_t obj, source, original; };
static struct screen_background screen_backgrounds[7];
static void on_screen_background(void *event);
static struct screen_background *screen_background_find(uint32_t obj) {
    unsigned i;
    for (i = 0; i < 7u; ++i)
        if (screen_backgrounds[i].obj == obj) return &screen_backgrounds[i];
    return 0;
}
#endif

static uint32_t bg_source(uint32_t obj) {
#if defined(__arm__) || defined(__thumb__)
    struct screen_background *owned = screen_background_find(obj);
    if (owned) {
        uint32_t native = ((uint32_t (*)(uint32_t, uint32_t, uint32_t))
            (uintptr_t)RH_FW_OBJECT_STYLE_GET)(obj, 0, 40u);
        return native ? native : owned->source;
    }
#endif
    return ((uint32_t (*)(uint32_t, uint32_t, uint32_t))
        (uintptr_t)RH_FW_OBJECT_STYLE_GET)(obj, 0, 40u);
}

#if defined(__arm__) || defined(__thumb__)
static int s_blur_needed = 1;
static uint32_t background_display;
static unsigned background_epoch;
static int background_frame_valid(uint32_t, unsigned);
#endif

static void bg_set(uint32_t obj, uint32_t source) {
#if defined(__arm__) || defined(__thumb__)
    /* Control Center retains its original container-relative rendering. */
    if (obj == control_center_container()) {
        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t))
            (uintptr_t)RH_LB_STYLE_SET)(obj, 40u, source, 0);
        return;
    }
    struct screen_background *owned = screen_background_find(obj);
    unsigned i;
    if (!owned && source) {
        for (i = 0; i < 7u; ++i) if (!screen_backgrounds[i].obj) {
            owned = &screen_backgrounds[i];
            owned->original = bg_source(obj);
            /* A recreated widget may inherit our descriptor/style value; it
             * is not a system source and must never be restored on teardown. */
            if (owned->original == source) owned->original = 0;
            /* Publish before registration; DELETE retires the identity before
             * native storage can be reused. One ALL callback per owned object. */
            owned->obj = obj;
            if (!((void *(*)(uint32_t, void (*)(void *), uint32_t, void *))
                (uintptr_t)RH_LB_OBJECT_EVENT_ADD)(obj, on_screen_background, 0, 0)) {
                *owned = (struct screen_background){0, 0, 0};
                return;
            }
            break;
        }
    }
    if (!owned && source) return;
    if (owned && !source) {
        uint32_t original = owned->original;
        uint32_t native = ((uint32_t (*)(uint32_t, uint32_t, uint32_t))
            (uintptr_t)RH_FW_OBJECT_STYLE_GET)(obj, 0, 40u);
        /* Do not erase a system style installed while we owned the callback. */
        if (native && native != owned->source) original = native;
        ((int (*)(uint32_t, void (*)(void *)))
            (uintptr_t)RH_LB_OBJECT_EVENT_REMOVE)(obj, on_screen_background);
        *owned = (struct screen_background){0, 0, 0};
        source = original;
    } else if (owned) {
        owned->source = source;
        /* Keep our source in the identity map, NOT property 40: native image
         * centering follows scrolling coords and would submit duplicate tasks. */
        source = 0;
    }
#endif
    ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t))
        (uintptr_t)RH_LB_STYLE_SET)(obj, 40u, source, 0);
}

#if defined(__arm__) || defined(__thumb__)
static void downsample_4x_and_darken(const uint16_t *src, uint32_t src_stride_px, uint16_t *dst) {
    uint32_t y, x;
    for (y = 0; y < 130u; ++y) {
        for (x = 0; x < 53u; ++x) {
            uint16_t p = src[(y * 4u + 1u) * src_stride_px + (x * 4u + 1u)];
            uint32_t r = ((p >> 11) & 0x1Fu) * 7u / 10u;
            uint32_t g = ((p >> 5)  & 0x3Fu) * 7u / 10u;
            uint32_t b = (p         & 0x1Fu) * 7u / 10u;
            dst[y * 53u + x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

static void bilinear_upscale_4x(const uint16_t *src, uint16_t *dst, uint32_t dst_stride_px) {
    uint32_t sy, sx, dy, dx;
    for (sy = 0; sy < 130u; ++sy) {
        uint32_t sy1 = (sy + 1u < 130u) ? sy + 1u : sy;
        for (sx = 0; sx < 53u; ++sx) {
            uint32_t sx1 = (sx + 1u < 53u) ? sx + 1u : sx;
            uint16_t p00 = src[sy  * 53u + sx];
            uint16_t p10 = src[sy  * 53u + sx1];
            uint16_t p01 = src[sy1 * 53u + sx];
            uint16_t p11 = src[sy1 * 53u + sx1];

            uint32_t r00 = (p00 >> 11) & 0x1Fu, g00 = (p00 >> 5) & 0x3Fu, b00 = p00 & 0x1Fu;
            uint32_t r10 = (p10 >> 11) & 0x1Fu, g10 = (p10 >> 5) & 0x3Fu, b10 = p10 & 0x1Fu;
            uint32_t r01 = (p01 >> 11) & 0x1Fu, g01 = (p01 >> 5) & 0x3Fu, b01 = p01 & 0x1Fu;
            uint32_t r11 = (p11 >> 11) & 0x1Fu, g11 = (p11 >> 5) & 0x3Fu, b11 = p11 & 0x1Fu;

            for (dy = 0; dy < 4u; ++dy) {
                uint32_t wy1 = dy, wy0 = 4u - dy;
                uint32_t r0 = (r00 * wy0 + r01 * wy1) >> 2;
                uint32_t g0 = (g00 * wy0 + g01 * wy1) >> 2;
                uint32_t b0 = (b00 * wy0 + b01 * wy1) >> 2;
                uint32_t r1 = (r10 * wy0 + r11 * wy1) >> 2;
                uint32_t g1 = (g10 * wy0 + g11 * wy1) >> 2;
                uint32_t b1 = (b10 * wy0 + b11 * wy1) >> 2;

                uint32_t y = sy * 4u + dy;
                for (dx = 0; dx < 4u; ++dx) {
                    uint32_t wx1 = dx, wx0 = 4u - dx;
                    uint32_t r = (r0 * wx0 + r1 * wx1) >> 2;
                    uint32_t g = (g0 * wx0 + g1 * wx1) >> 2;
                    uint32_t b = (b0 * wx0 + b1 * wx1) >> 2;
                    uint32_t x = sx * 4u + dx;
                    dst[y * dst_stride_px + x] = (uint16_t)((r << 11) | (g << 5) | b);
                }
            }
        }
    }
}

static uint32_t g_blur_draw_buf;
static int g_blur_valid;
static int g_blur_updating;
static uint32_t g_blur_pending_snapshot;
static uint16_t g_blur_small_a[53 * 130];
static uint16_t g_blur_small_b[53 * 130];

/* The fixed-size kernels require a full-screen, unpacked RGB565 draw buffer.
 * Reject malformed metadata before reading or writing its pixel storage. */
static int blur_pixels(uint32_t buf, uint16_t **pixels, uint32_t *stride_px) {
    uint32_t stride, data;
    if (!buf || *(volatile const uint8_t *)(uintptr_t)(buf + 1u) != 18u ||
        *(volatile const uint16_t *)(uintptr_t)(buf + 4u) != 212u ||
        *(volatile const uint16_t *)(uintptr_t)(buf + 6u) != 520u) return 0;
    stride = *(volatile const uint16_t *)(uintptr_t)(buf + 8u);
    /* +16 is the aligned pixel pointer used by native goto_xy and GPU draws.
     * +20 is the raw allocation retained only for eventual destruction. */
    data = load32(buf + 16u);
    if (stride < 424u || (stride & 1u) || !data || (data & 1u) ||
        load32(buf + 12u) < stride * 520u) return 0;
    *pixels = (uint16_t *)(uintptr_t)data;
    *stride_px = stride / 2u;
    return 1;
}

static void blur_cache_sync(void) {
    RH_LB_MEMORY_BARRIER();
    ((void (*)(unsigned))(uintptr_t)RH_LB_DCACHE_SYNC)(1u);
    RH_LB_MEMORY_BARRIER();
}

static void blur_cache_invalidate(uint32_t buf) {
    /* The ROM range invalidator may invalidate the entire D-cache for a large
     * frame. Write back unrelated dirty data first, then discard stale lines. */
    blur_cache_sync();
    ((void (*)(unsigned, uint32_t, uint32_t))(uintptr_t)RH_LB_DCACHE_INVALIDATE)
        (1u, load32(buf + 16u), load32(buf + 12u));
    RH_LB_MEMORY_BARRIER();
}

static int rh_platform_update_blur(void) {
    uint32_t snap = 0, frame_display = background_display;
    unsigned frame_epoch = background_epoch;
    uint16_t *src_data, *dst_data;
    uint32_t src_stride_px, dst_stride_px;
    int success = 0;

    /* Snapshot rendering dispatches LVGL events too. Never recursively mutate
     * the shared scratch buffers or acquire a second snapshot while updating. */
    if (g_blur_updating || !background_frame_valid(frame_display, frame_epoch)) return 0;
    g_blur_updating = 1;
    /* REFR_START precedes new draw tasks. Finish already queued VG-Lite work
     * before writing a source that the preceding frame might still consume. */
    if (((int (*)(void))(uintptr_t)RH_LB_GPU_FINISH)() != 0 ||
        !background_frame_valid(frame_display, frame_epoch)) goto done;
    if (g_blur_pending_snapshot) {
        ((void (*)(uint32_t))(uintptr_t)RH_LB_SNAPSHOT_FREE)(g_blur_pending_snapshot);
        g_blur_pending_snapshot = 0;
    }
    if (!g_blur_draw_buf) {
        g_blur_draw_buf = ((uint32_t (*)(void *, int, int, int, int))(uintptr_t)RH_LB_DRAW_BUF_CREATE)
            ((void *)(uintptr_t)RH_LB_DRAW_BUF_HANDLERS, 212, 520, 18, 0);
    }
    if (!blur_pixels(g_blur_draw_buf, &dst_data, &dst_stride_px)) goto done;

    blur_cache_sync();
    snap = ((uint32_t (*)(void))(uintptr_t)RH_LB_SNAPSHOT_TAKE)();
    if (!snap) goto done;
    /* Native task completion can mean command submission, not GPU idle.
     * Never read or release GPU-owned snapshot pixels after a failed fence. */
    if (((int (*)(void))(uintptr_t)RH_LB_GPU_FINISH)() != 0) {
        g_blur_pending_snapshot = snap;
        snap = 0;
        goto done;
    }
    if (!background_frame_valid(frame_display, frame_epoch) ||
        !blur_pixels(snap, &src_data, &src_stride_px)) goto done;
    blur_cache_invalidate(snap);

    downsample_4x_and_darken(src_data, src_stride_px, g_blur_small_a);
    ((void (*)(uint32_t))(uintptr_t)RH_LB_SNAPSHOT_FREE)(snap);
    snap = 0;

    (void)rh_blur_filter_rgb565(g_blur_small_a, g_blur_small_b, 53u, 130u, 2u, 2u, 100u);
    if (!background_frame_valid(frame_display, frame_epoch)) goto done;
    bilinear_upscale_4x(g_blur_small_a, dst_data, dst_stride_px);
    blur_cache_sync();
    /* Allocation alone is not a usable background. Publish only filled pixels;
     * on refresh failure keep both the last good pixels and the dirty flag. */
    g_blur_valid = 1;
    s_blur_needed = 0;
    success = 1;
done:
    if (snap) ((void (*)(uint32_t))(uintptr_t)RH_LB_SNAPSHOT_FREE)(snap);
    g_blur_updating = 0;
    return success;
}

static uint32_t rh_platform_get_blur_draw_buf(void) {
    return g_blur_valid ? g_blur_draw_buf : 0;
}

static void on_screen_background(void *event) {
    uint32_t obj = *(const uint32_t *)event;
    unsigned code = *(const uint16_t *)((const unsigned char *)event + 8u);
    struct screen_background *owned = screen_background_find(obj);
    uint32_t descriptor[29]; /* Native lv_draw_rect_dsc_t: 116 bytes. */
    int32_t area[4] = {0, 0, 211, 519};
    uint32_t layer;
    /* Event +0 is current target, +4 is original target. Never paint (or
     * retire ownership) for a descendant's bubbled event. */
    if (!owned || obj != *((const uint32_t *)event + 1)) return;
    if (code == 36u) {
        *owned = (struct screen_background){0, 0, 0};
        return;
    }
    /* DRAW_MAIN_END is after the object's native black/fill/border, but
     * before children. The native renderer has already installed obj ∩ layer
     * clipping. Full-screen GLOBAL coords stay fixed even on offscreen layers;
     * the GPU consumer alone subtracts the layer origin. */
    if (code != 25u || g_blur_updating || !owned->source) return;
    layer = *(const uint32_t *)((const unsigned char *)event + 16u);
    if (!layer) return;
    ((void (*)(void *))(uintptr_t)RH_LB_RECT_INIT)(descriptor);
    ((unsigned char *)descriptor)[32] = 0; /* No rectangle fill. */
    descriptor[12] = owned->source; /* bg_image_src at +48. */
    ((void (*)(uint32_t, const void *, const int32_t *))
        (uintptr_t)RH_LB_RECT_DRAW)(layer, descriptor, area);
    /* Native rectangle submission copies the descriptor and duplicates file
     * names for deferred tasks. Blur draw buffers retain the existing GPU
     * fence/cache lifetime; no pointer to this stack descriptor escapes. */
}
#endif

#if defined(__arm__) || defined(__thumb__)
static uint32_t current_bg_source(unsigned type) {
    const char *path = bg_paths[type][bg_path_index];
    if (rh_background_is_blur(path)) {
        return rh_platform_get_blur_draw_buf();
    }
    return (uint32_t)(uintptr_t)path;
}
#endif
#if !defined(__arm__) && !defined(__thumb__)
static uint32_t current_bg_source(unsigned type) {
    return (uint32_t)(uintptr_t)bg_paths[type][bg_path_index];
}
#endif

static int stage_one_path(const char *path) {
    uint32_t header[3] = {0, 0, 0};
    uint32_t cache;
    if (!path[0] || rh_background_is_blur(path)) return 0;
    /* An explicit reload may replace bytes at the same filename. Drop only
     * that key before preflight so a cached header cannot mask a bad update. */
    cache = load32(RH_FW_HEADER_CACHE_SLOT);
    if (cache && (!cache_valid(cache, RH_FW_HEADER_CACHE_CLASS) ||
        retire_cache(cache, 0, 0, path))) return -2145;
    cache = load32(RH_FW_IMAGE_CACHE_SLOT);
    if (cache && (!cache_valid(cache, RH_FW_IMAGE_CACHE_CLASS) ||
        retire_cache(cache, 4, 0, path))) return -2145;
    /* One preflight per activation; never decode/check files in poll or draw. */
    return ((int (*)(const char *, void *))(uintptr_t)RH_FW_IMAGE_GET_INFO)
        (path, header) == 1 ? 0 : -2143;
}

/* Widgets are configuration-only on Band11: do not validate, load, blur,
 * or register native hooks for that unsupported surface. */
int rh_platform_backgrounds_stage(const struct rh_backgrounds *bg) {
    int rc;
    if (!bg) return -2142;
    rc = stage_one_path(bg->launcher);
    if (rc) return rc;
    rc = stage_one_path(bg->control_center);
    if (rc) return rc;
    return stage_one_path(bg->notifications);
}

#if defined(__arm__) || defined(__thumb__)
static int background_refreshing;
static uint32_t background_frame_display;
static int background_retry;
static int background_wake_pending;
static uint32_t last_bg_objects[7];
static unsigned last_bg_visible[7];

static unsigned background_type(unsigned i) { return i < 4u ? 0u : i == 4u ? 1u : 2u; }
static uint32_t background_object(unsigned i) {
    uint32_t obj;
    if (i < 4u) return launcher_list(i);
    if (i == 4u) return control_center_container();
    if (i == 5u) return notify_container();
    obj = load32(RH_LB_NOTIFY_LIST_SLOT);
    return obj && load32(obj) == RH_LB_NOTIFY_LIST_CLASS ? obj : 0;
}

/* Establish membership from the live registry before dereferencing any saved
 * display identity, including during default-display changes or deletion. */
static int background_display_live(uint32_t display) {
    uint32_t p, prev = 0, count = 0;
    int found = 0;
    if (!display || load32(RH_FW_FR_DISPLAY_LIST) != 792u) return 0;
    for (p = load32(RH_FW_FR_DISPLAY_LIST + 4u); p; p = load32(p + 796u)) {
        if (++count > 4u || load32(p + 792u) != prev) return 0;
        if (p == display) found = 1;
        prev = p;
    }
    return load32(RH_FW_FR_DISPLAY_LIST + 8u) == prev && found;
}

static int background_frame_valid(uint32_t display, unsigned epoch) {
    return display && display == background_display && epoch == background_epoch &&
        display == load32(RH_FW_DISPLAY_SLOT) && background_display_live(display);
}

static int backgrounds_release_pending(void) {
    if (!g_blur_pending_snapshot) return 1;
    if (g_blur_updating || ((int (*)(void))(uintptr_t)RH_LB_GPU_FINISH)() != 0) return 0;
    ((void (*)(uint32_t))(uintptr_t)RH_LB_SNAPSHOT_FREE)(g_blur_pending_snapshot);
    g_blur_pending_snapshot = 0;
    return 1;
}

static void backgrounds_wake_display(uint32_t);

static void backgrounds_reset_visibility(void) {
    unsigned i;
    for (i = 0; i < 7u; ++i) {
        last_bg_objects[i] = 0;
        last_bg_visible[i] = 0;
    }
}

static void on_background_refresh(void *event) {
    uint32_t display = *(const uint32_t *)event;
    unsigned code = *(const uint16_t *)((const unsigned char *)event + 8u);
    unsigned i, visible_blur = 0, epoch = background_epoch;
    static const unsigned screen_offsets[5] = {696u, 700u, 708u, 712u, 716u};
    if (display != background_display) return;
    if (code == 36u) { /* Display delete, before event list/native storage free. */
        background_display = 0;
        ++background_epoch;
        background_retry = 0;
        background_wake_pending = 0;
        backgrounds_reset_visibility();
        s_blur_needed = 1;
        return;
    }
    if (background_refreshing || g_blur_updating ||
        !background_frame_valid(display, epoch)) return;
    if (code == 53u) { /* REFR_READY: invalidations now belong to the next frame. */
        if (background_retry || background_wake_pending) backgrounds_wake_display(display);
        return;
    }
    if (code != 52u) return;
    background_refreshing = 1;
    background_frame_display = display;
    background_retry = 0;
    background_wake_pending = 0;
    if (!backgrounds_release_pending()) {
        background_retry = 1;
        goto done;
    }
    if (!background_frame_valid(display, epoch)) goto done;
    /* Native REFR_START precedes native layout. Settle the same five screen
     * roots first so visibility uses this frame's coordinates and clipping. */
    for (i = 0; i < 5u; ++i) {
        uint32_t screen = load32(display + screen_offsets[i]);
        if (screen) ((void (*)(uint32_t))(uintptr_t)RH_LB_LAYOUT_UPDATE)(screen);
        if (!background_frame_valid(display, epoch)) goto done;
    }
    for (i = 0; i < 7u; ++i) {
        unsigned type = background_type(i), visible = 0;
        uint32_t obj = bg_active[type] ? background_object(i) : 0;
        int32_t area[4] = {0, 0, 211, 519};
        if (obj) visible = ((int (*)(uint32_t, int32_t *))
            (uintptr_t)RH_LB_VISIBLE_AREA)(obj, area) != 0;
        if (visible && rh_background_is_blur(bg_paths[type][bg_path_index])) {
            visible_blur = 1;
            if (!last_bg_visible[i] || last_bg_objects[i] != obj ||
                bg_source(obj) != current_bg_source(type)) s_blur_needed = 1;
        }
        last_bg_objects[i] = obj;
        last_bg_visible[i] = visible;
    }
    if (visible_blur && (s_blur_needed || !g_blur_valid))
        background_retry = !rh_platform_update_blur();
    if (!background_frame_valid(display, epoch)) goto done;
    /* Rediscover after snapshot rendering: never dereference an object retained
     * across a native operation that can synchronously dispatch other events. */
    for (i = 0; i < 7u; ++i) {
        unsigned type = background_type(i);
        uint32_t obj, source;
        if (!bg_active[type]) continue;
        obj = background_object(i);
        source = current_bg_source(type);
        if (source && obj &&
            ((type != 1u && !screen_background_find(obj)) || bg_source(obj) != source)) {
            bg_set(obj, source);
            /* A failed registration leaves native styles untouched. Retry on
             * the next native frame rather than silently reporting success. */
            if (type != 1u && !screen_background_find(obj)) background_retry = 1;
        }
        if (!background_frame_valid(display, epoch)) goto done;
    }
done:
    background_frame_display = 0;
    background_refreshing = 0;
}

static void backgrounds_wake_display(uint32_t display) {
    int32_t area[4] = {0, 0, 211, 519};
    /* A reentrant configuration change must be reissued at REFR_READY, not
     * lost when the interrupted frame clears its invalid-area collection. */
    if (background_refreshing && display == background_frame_display) {
        background_wake_pending = 1;
        return;
    }
    background_wake_pending = 0;
    /* RH_FW_INVALIDATE_AREA is _lv_inv_area(display, area), NOT the object
     * wrapper at 0x0c382198. Keep the existing display-based redraw ABI. */
    ((void (*)(uint32_t, const int32_t *))
        (uintptr_t)RH_FW_INVALIDATE_AREA)(display, area);
}

static void backgrounds_watch_display(void) {
    uint32_t display = load32(RH_FW_DISPLAY_SLOT);
    int active = bg_active[0] || bg_active[1] || bg_active[2];
    /* Fault retirement is independent of panel visibility/configuration. This
     * maintenance never generates pixels; only a successful fence releases
     * a snapshot pinned by an earlier GPU failure. */
    (void)backgrounds_release_pending();
    if (background_display && (background_display != display || !active)) {
        /* Failed registry validation is NOT successful unregistration. Keep
         * ownership unresolved instead of adding duplicates if it recovers. */
        if (!background_display_live(background_display)) return;
        ((int (*)(uint32_t, void (*)(void *), void *))
            (uintptr_t)RH_LB_DISPLAY_EVENT_REMOVE)(background_display, on_background_refresh, 0);
        background_display = 0;
        ++background_epoch;
        background_retry = 0;
        background_wake_pending = 0;
        backgrounds_reset_visibility();
    }
    if (!active) return;
    if (!background_display_live(display)) return;
    if (background_display && background_wake_pending && !background_refreshing)
        backgrounds_wake_display(background_display);
    if (!background_display) {
        /* One callback per live display, never one callback per widget/style.
         * Filter ALL|PREPROCESS handles deletion and precedes ordinary callbacks. */
        if (((void *(*)(uint32_t, void (*)(void *), uint32_t, void *))
             (uintptr_t)RH_LB_DISPLAY_EVENT_ADD)(display + 736u, on_background_refresh, 0x8000u, 0)) {
            background_display = display;
            ++background_epoch;
            backgrounds_reset_visibility();
            /* Adding to a generic event list does not wake an idle refresh
             * timer. Invalidate the active screen once to deliver first setup. */
            backgrounds_wake_display(display);
        }
    }
}
#endif

void rh_platform_backgrounds_poll(void) {
#if defined(__arm__) || defined(__thumb__)
    /* Configuration watcher only maintains the display subscription. No panel
     * presence/visibility scan, snapshot or style mutation is timer-driven. */
    backgrounds_watch_display();
#endif
#if !defined(__arm__) && !defined(__thumb__)
    unsigned i;
    if (bg_active[0]) {
        uint32_t source = current_bg_source(0);
        for (i = 0; i < 4; ++i) {
            uint32_t list = launcher_list(i);
            if (source && list && bg_source(list) != source) {
                bg_set(list, source);
            }
        }
    }
    if (bg_active[1]) {
        uint32_t cc = control_center_container();
        uint32_t source = current_bg_source(1);
        if (source && cc && bg_source(cc) != source) {
            bg_set(cc, source);
        }
    }
    if (bg_active[2]) {
        uint32_t ntf = notify_container();
        uint32_t list = load32(RH_LB_NOTIFY_LIST_SLOT);
        uint32_t source = current_bg_source(2);
        if (source && ntf && bg_source(ntf) != source) {
            bg_set(ntf, source);
        }
        if (source && list && load32(list) == RH_LB_NOTIFY_LIST_CLASS && bg_source(list) != source) {
            bg_set(list, source);
        }
    }
#endif
}

int rh_platform_backgrounds_commit(const struct rh_backgrounds *bg) {
    unsigned i, k, next = bg_path_index ^ 1u;
    uint32_t header_cache = load32(RH_FW_HEADER_CACHE_SLOT);
    uint32_t image_cache = load32(RH_FW_IMAGE_CACHE_SLOT);
    const char *new_paths[3];

    if (!bg) return -2142;
    new_paths[0] = bg->launcher;
    new_paths[1] = bg->control_center;
    new_paths[2] = bg->notifications;

    /* All fallible retirement must precede style/active-state mutation. A
     * failed auxiliary commit must leave the previously selected paths live. */
    for (k = 0; k < 3; ++k) {
        const char *old = bg_paths[k][bg_path_index];
        if (old[0] && !rh_background_is_blur(old)) {
            if (header_cache && (!cache_valid(header_cache, RH_FW_HEADER_CACHE_CLASS) ||
                retire_cache(header_cache, 0, 0, old))) return -2145;
            if (image_cache && (!cache_valid(image_cache, RH_FW_IMAGE_CACHE_CLASS) ||
                retire_cache(image_cache, 4, 0, old))) return -2145;
        }
    }

    /* Reset active objects to 0 if their current style points to our old paths */
    for (i = 0; i < 4; ++i) {
        uint32_t list = launcher_list(i);
        if (list) {
            uint32_t s = bg_source(list);
            if (s == (uint32_t)(uintptr_t)bg_paths[0][0] ||
                s == (uint32_t)(uintptr_t)bg_paths[0][1]
#if defined(__arm__) || defined(__thumb__)
                || (screen_background_find(list) && !new_paths[0][0])
                || (g_blur_draw_buf && s == g_blur_draw_buf &&
                    !rh_background_is_blur(new_paths[0]))
#endif
                ) bg_set(list, 0);
        }
    }
    {
        uint32_t cc = control_center_container();
        if (cc) {
            uint32_t s = bg_source(cc);
            if (s == (uint32_t)(uintptr_t)bg_paths[1][0] ||
                s == (uint32_t)(uintptr_t)bg_paths[1][1]
#if defined(__arm__) || defined(__thumb__)
                || (screen_background_find(cc) && !new_paths[1][0])
                || (g_blur_draw_buf && s == g_blur_draw_buf &&
                    !rh_background_is_blur(new_paths[1]))
#endif
                ) bg_set(cc, 0);
        }
    }
    {
        uint32_t ntf = notify_container();
        uint32_t list;
        if (ntf) {
            uint32_t s = bg_source(ntf);
            if (s == (uint32_t)(uintptr_t)bg_paths[2][0] ||
                s == (uint32_t)(uintptr_t)bg_paths[2][1]
#if defined(__arm__) || defined(__thumb__)
                || (g_blur_draw_buf && s == g_blur_draw_buf &&
                    !rh_background_is_blur(new_paths[2]))
#endif
#if defined(__arm__) || defined(__thumb__)
                || (screen_background_find(ntf) && !new_paths[2][0])
#endif
                ) bg_set(ntf, 0);
        }
        /* A container setter can synchronously delete/rebuild its sibling. */
        list = load32(RH_LB_NOTIFY_LIST_SLOT);
        if (list && load32(list) == RH_LB_NOTIFY_LIST_CLASS) {
            uint32_t s = bg_source(list);
            if (s == (uint32_t)(uintptr_t)bg_paths[2][0] ||
                s == (uint32_t)(uintptr_t)bg_paths[2][1]
#if defined(__arm__) || defined(__thumb__)
                || (g_blur_draw_buf && s == g_blur_draw_buf &&
                    !rh_background_is_blur(new_paths[2]))
                || (screen_background_find(list) && !new_paths[2][0])
#endif
                ) bg_set(list, 0);
        }
    }

    for (k = 0; k < 3; ++k) bg_active[k] = 0;

    /* Store new paths */
    for (k = 0; k < 3; ++k) {
        for (i = 0; i < RH_PATH; ++i) {
            bg_paths[k][next][i] = new_paths[k][i];
            if (!new_paths[k][i]) break;
        }
        bg_active[k] = new_paths[k][0] != 0;
    }
    bg_path_index = next;
#if defined(__arm__) || defined(__thumb__)
    ++background_epoch;
    background_retry = 0;
    backgrounds_reset_visibility();
    s_blur_needed = rh_background_is_blur(new_paths[0]) ||
                    rh_background_is_blur(new_paths[1]) ||
                    rh_background_is_blur(new_paths[2]);
#endif
    rh_platform_backgrounds_poll();
#if defined(__arm__) || defined(__thumb__)
    if (background_display && background_display_live(background_display))
        backgrounds_wake_display(background_display);
#endif
    return 0;
}

#elif defined(RH_TARGET_1043) && RH_TARGET_1043
#include "p67_backgrounds.inc"
#endif

int rh_platform_retire_images(const struct rh_state *state) {
    struct rh_mapping_view view;
    if (!state) return -1;
    view = rh_rules_view(state->rules, state->count);
    return rh_platform_retire_mapped_images(&view);
}
/* Calendar source keys are not discovered by walking the launcher image: on
 * .043 that image holds a RAM snapshot; on .139/.155 it holds the generated
 * /data file. Retire only the exact compositing background, even for an explicit
 * revision with identical mappings (assets may change at unchanged paths). */
int rh_platform_calendar_affected(const struct rh_mapping_view *previous,
                                  const struct rh_mapping_view *current) {
    if (rh_validate_view(current) || (previous && rh_validate_view(previous))) return 0;
    return affected(previous, current, (uint32_t)(uintptr_t)RH_CALENDAR_BACKGROUND) ||
           affected(previous, current, (uint32_t)(uintptr_t)RH_CALENDAR_OUTPUT) ||
           affected(previous, current, (uint32_t)(uintptr_t)"/resource/app/launcher/calendar.bin") ||
           affected(previous, current, (uint32_t)(uintptr_t)"/resource/font/MiSans-Regular-All.ttf");
}
int rh_platform_refresh_calendar(void) { return rh_calendar_refresh(); }
int rh_platform_calendar_retire(const struct rh_calendar_config *config) {
    static const char *const keys[] = {RH_CALENDAR_BACKGROUND, RH_CALENDAR_OUTPUT,
                                      RH_CALENDAR_BANK0, RH_CALENDAR_BANK1};
    uint32_t data, header, i, j, seen_count = 0;
    /* At most 12 month + 31 day + 7 weekday images; retain pool references,
     * not 50 absolute path buffers. Compare contents, not pool offsets. */
    const char *seen[50];
    char path[RH_PATH];
    if (!rh_platform_redraw_ready()) return -1;
    data = load32(RH_FW_IMAGE_CACHE_SLOT);
    header = load32(RH_FW_HEADER_CACHE_SLOT);
    if (!cache_valid(data, RH_FW_IMAGE_CACHE_CLASS) ||
        !cache_valid(header, RH_FW_HEADER_CACHE_CLASS)) return -1;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (retire_cache(header, 0u, 0, keys[i]) || retire_cache(data, 4u, 0, keys[i])) return -1;
    if (config) for (i = 0; i < 3; ++i) {
        if (config->items[i].type != RH_CALENDAR_IMAGE) continue;
        if (config->pool_used > RH_CALENDAR_POOL_BYTES ||
            config->items[i].count != rh_calendar_context_count(i)) return -1;
        for (j = 0; j < config->items[i].count; ++j) {
            const char *relative = rh_calendar_context(config, i, j);
            uint32_t k;
            if (!relative || !rh_calendar_bounded(relative,
                    config->pool_used - config->items[i].context[j])) return -1;
            for (k = 0; k < seen_count; ++k)
                if (same_path((uint32_t)(uintptr_t)seen[k], relative)) break;
            if (k < seen_count) continue;
            if (seen_count >= sizeof(seen) / sizeof(seen[0]) ||
                rh_calendar_image_path(path, relative)) return -1;
            seen[seen_count++] = relative;
            if (retire_cache(header, 0u, 0, path) || retire_cache(data, 4u, 0, path)) return -1;
        }
    }
    return 0;
}
#if defined(RH_TARGET_1043) && RH_TARGET_1043
void rh_p67_calendar_publish(void);
#endif

void rh_platform_calendar_publish(void) {
#if defined(RH_TARGET_1043) && RH_TARGET_1043
    rh_p67_calendar_publish();
#elif !defined(RH_TARGET_1043) || !RH_TARGET_1043
    if (load32(RH_FW_CALENDAR_LAUNCHER_SLOT))
        ((void (*)(void))(uintptr_t)RH_FW_CALENDAR_PUBLISH)();
#endif
}
struct object_list { uint32_t objects[RH_RELOAD_LIMIT], count, overflow; };
/* Bound native walk recursion as well as the snapshot size. Parent +4 is
 * independently established by lv_obj_get_screen (0x0c3802a8). */
static int depth_ok(uint32_t object) {
    uint32_t n = 0;
    while (object) {
        if (++n > 32u) return 0;
        object = load32(object + 4u);
    }
    return 1;
}
static int collect_object(uint32_t object, void *cookie) {
    struct object_list *list = cookie;
    /* Native deletion marks object+51 bit 4 before dispatching owner events
     * (0x0c380404). Skip that whole subtree if activation is reentered there. */
    if (*(const unsigned char *)(uintptr_t)(object + 51u) & 16u) return 1;
    if (list->count == RH_RELOAD_LIMIT || !depth_ok(object)) {
        list->overflow = 1;
        return 2;
    }
    list->objects[list->count++] = object;
    return 0;
}
static void walk(int (*callback)(uint32_t, void *), void *cookie) {
    ((void (*)(uint32_t, int (*)(uint32_t, void *), void *))
        (uintptr_t)RH_FW_OBJECT_TREE_WALK)(0, callback, cookie);
}
/* A native refresh can send events that delete a later snapshot object, or
 * itself. Membership is checked without dereferencing the candidate pointer.
 * Reused addresses are treated as new live objects, with class/source checked
 * anew. No object or source is retained across UI ticks. */
struct membership { uint32_t wanted, found, visited; };
static int find_object(uint32_t object, void *cookie) {
    struct membership *m = cookie;
    if (*(const unsigned char *)(uintptr_t)(object + 51u) & 16u) return 1;
    if (object == m->wanted) { m->found = 1; return 2; }
    return ++m->visited > RH_RELOAD_LIMIT || !depth_ok(object) ? 2 : 0;
}
static int live_object(uint32_t object) {
    struct membership m = {object, 0, 0};
    walk(find_object, &m);
    return m.found;
}
int rh_platform_refresh_mapped_images(const struct rh_mapping_view *previous,
                                      const struct rh_mapping_view *current) {
    struct object_list *list;
    uint32_t i;
    if (rh_validate_view(current) || (previous && rh_validate_view(previous))) return -1;
    if (!current->count && (!previous || !previous->count)) return 0;
    list = rh_platform_alloc(sizeof(*list));
    if (!list) return -1;
    list->count = list->overflow = 0;
    /* Native NULL-root walk includes registered screens on all displays (also
     * offscreen cached pages), not objects outside the LVGL screen trees. */
    walk(collect_object, list);
    if (list->overflow) { rh_platform_free(list); return -1; }
    for (i = 0; i < list->count; i++) {
        uint32_t object = list->objects[i], source;
        uint32_t header[3] = {0, 0, 0};
        uint32_t header_cache = load32(RH_FW_HEADER_CACHE_SLOT);
        if (!live_object(object)) continue;
        /* Exact image class only: never assume animation/canvas subclasses
         * share the image ownership contract, even if their base is image. */
        if (header_cache && load32(header_cache) == RH_FW_HEADER_CACHE_CLASS &&
            load32(header_cache + 8u) && load32(object) == RH_FW_IMAGE_OBJECT_CLASS &&
            (*(const unsigned char *)(uintptr_t)(object + 96u) & 3u) == 1u) {
            source = load32(object + 52u);
            /* Preflight: set_src's failure path clears the existing source.
             * Keep theme files immutable throughout this UI transaction.
             * With header caching enabled, native get_info returns success
             * only after insertion (0x0c38e47c..0x0c38e4b8); the subsequent
             * same-pointer setter reuses it. Disabled header caches are skipped.
             * As for any native setter, callbacks must not delete its receiver
             * or invalidate its source during the setter itself. */
            if (affected(previous, current, source) &&
                ((int (*)(uint32_t, void *))(uintptr_t)RH_FW_IMAGE_GET_INFO)(source, header) == 1 &&
                load32(RH_FW_HEADER_CACHE_SLOT) == header_cache &&
                load32(header_cache + 8u) && live_object(object) &&
                load32(object) == RH_FW_IMAGE_OBJECT_CLASS &&
                load32(object + 52u) == source &&
                (*(const unsigned char *)(uintptr_t)(object + 96u) & 3u) == 1u) {
                ((void (*)(uint32_t, uint32_t))(uintptr_t)RH_FW_IMAGE_SET_SRC)(object, source);
            }
        }
        if (!live_object(object)) continue;
        /* System image-button setter uses property 40 with main-part states
         * 0/32/128. Query the effective current-state main-part value, then
         * explicitly refresh the property: reassigning the same style pointer
         * need not trigger refresh. Do not change state, selectors or styles. */
        source = ((uint32_t (*)(uint32_t, uint32_t, uint32_t))
            (uintptr_t)RH_FW_OBJECT_STYLE_GET)(object, 0, 40u);
        if (affected(previous, current, source))
            ((void (*)(uint32_t, uint32_t, uint32_t))
                (uintptr_t)RH_FW_OBJECT_STYLE_REFRESH)(object, 0, 40u);
    }
    rh_platform_free(list);
    return 0;
}
int rh_platform_refresh_images(const struct rh_state *state) {
    struct rh_mapping_view view;
    if (!state) return -1;
    view = rh_rules_view(state->rules, state->count);
    return rh_platform_refresh_mapped_images(0, &view);
}
/* Both targets' lv_display_get_screen_active (0x0c3807ec) read +696,
 * not +24 (DPI). This is a UI mutation/invalidation guard, NOT GPU idle.
 * Deferred draw-unit decoders retain cache references until native release. */
int rh_platform_redraw_ready(void) {
    uint32_t disp = *(volatile uint32_t *)(uintptr_t)RH_FW_DISPLAY_SLOT;
    if (!disp || !*(volatile uint32_t *)(uintptr_t)(disp + 696u)) return 0;
    if (*(volatile unsigned char *)(uintptr_t)(disp + 58u) & 2u) return 0;
    return *(volatile int32_t *)(uintptr_t)(disp + 608u) > 0;
}
/* Request, rather than synchronously execute, a full redraw. Return success
 * only if the firmware retained a dirty area covering the entire display. */
int rh_platform_request_full_redraw(void) {
    int32_t area[4] = {0, 0, 0x7fff, 0x7fff};
    uint32_t disp = *(volatile uint32_t *)(uintptr_t)RH_FW_DISPLAY_SLOT;
    int32_t width, height;
    uint32_t i, count;
    if (!rh_platform_redraw_ready()) return -1;
    ((void (*)(uint32_t, const void *))(uintptr_t)RH_FW_INVALIDATE_AREA)(disp, area);
    /* Ask the firmware for the resolution rather than reading disp+0/disp+4:
     * which of those two is horizontal depends on the rotation bit at disp+756,
     * and _lv_inv_area clips with these same accessors. Using them keeps this
     * check identical to the firmware's own clip at any rotation. */
    width = ((int32_t (*)(uint32_t))(uintptr_t)RH_FW_DISPLAY_WIDTH)(disp);
    height = ((int32_t (*)(uint32_t))(uintptr_t)RH_FW_DISPLAY_HEIGHT)(disp);
    count = *(volatile uint32_t *)(uintptr_t)(disp + 604u);
    if (count > 32u || width <= 0 || height <= 0) return -1;
    for (i = 0; i < count; i++) {
        volatile int32_t *a = (volatile int32_t *)(uintptr_t)(disp + 60u + i * 16u);
        if (a[0] <= 0 && a[1] <= 0 && a[2] >= width - 1 && a[3] >= height - 1)
            return 0;
    }
    return -1;
}
/* The font manager hangs off the uikit global at *0x200bd1e8 + 28. Its
 * registered-path list has the payload size at +24, the head at +28 and each
 * node laid out {name, path, prev, next} with next at node + payload + 4.
 * font_manager_generate_def_path (0x0c490edc) returns the first entry whose name
 * matches, else builds "<base>/<name>.ttf" — so fonts never reach the hooked
 * LVGL POSIX open at all; they are resolved to a native path and opened through
 * access()/FreeType. Retargeting this registry is therefore the only way a theme
 * can replace a font. */
static uint32_t font_manager(void) {
    uint32_t uikit = *(volatile uint32_t *)(uintptr_t)RH_FW_UIKIT_SLOT;
    return uikit ? *(volatile uint32_t *)(uintptr_t)(uikit + 28u) : 0u;
}
static uint32_t font_path_node(uint32_t manager, uint32_t index) {
    uint32_t next = *(volatile uint32_t *)(uintptr_t)(manager + 24u) + 4u;
    uint32_t node = *(volatile uint32_t *)(uintptr_t)(manager + 28u);
    while (node && index--) node = *(volatile uint32_t *)(uintptr_t)(node + next);
    return node;
}
static int copy_string(char *out, uint32_t source) {
    uint32_t i;
    if (!source) return 0;
    for (i = 0; i < RH_PATH; i++) {
        out[i] = (char)*(volatile unsigned char *)(uintptr_t)(source + i);
        if (!out[i]) return 1;
    }
    return 0;
}
static int same_string(uint32_t source, const char *text) {
    uint32_t i;
    if (!source) return 0;
    for (i = 0; i < RH_PATH; i++) {
        char value = (char)*(volatile unsigned char *)(uintptr_t)(source + i);
        if (value != text[i]) return 0;
        if (!value) return 1;
    }
    return 0;
}
int rh_platform_font_path_get(uint32_t index, char *name, char *path) {
    uint32_t manager = font_manager(), node;
    if (!manager || !name || !path) return -1;
    node = font_path_node(manager, index);
    if (!node) return -1;
    if (!copy_string(name, *(volatile uint32_t *)(uintptr_t)node)) return -1;
    return copy_string(path, *(volatile uint32_t *)(uintptr_t)(node + 4u)) ? 0 : -1;
}
/* Replace one registry entry's path. add_path appends at the tail while lookup
 * takes the first name match, so an already-registered family cannot be
 * overridden by adding alone — the old entry has to be removed first. The old
 * name string is freed by the removal, so it is copied out beforehand. The
 * result is confirmed by asking the manager to resolve the family again. */
int rh_platform_font_retarget(uint32_t index, const char *path) {
    char name[RH_PATH];
    uint32_t manager = font_manager(), node, resolved;
    if (!manager || !path) return -1;
    node = font_path_node(manager, index);
    if (!node) return -1;
    if (!copy_string(name, *(volatile uint32_t *)(uintptr_t)node)) return -1;
    ((void (*)(uint32_t))(uintptr_t)RH_FW_FONT_REMOVE_PATH)(node);
    ((uint32_t (*)(const char *, const char *))(uintptr_t)RH_FW_FONT_ADD_PATH)(name, path);
#if defined(RH_FW_FONT_RESOLVE_PATH) && RH_FW_FONT_RESOLVE_PATH
    resolved = ((uint32_t (*)(uint32_t, const char *))(uintptr_t)RH_FW_FONT_RESOLVE_PATH)(manager, name);
    return same_string(resolved, path) ? 0 : -1;
#else
    node = font_path_node(manager, index);
    resolved = node ? *(volatile uint32_t *)(uintptr_t)(node + 4u) : 0u;
    return same_string(resolved, path) ? 0 : -1;
#endif
}
void *rh_platform_timer_create(uint32_t interval_ms, void (*callback)(void *)) {
    if (!interval_ms || !callback) return 0;
    return ((void *(*)(void (*)(void *), uint32_t, void *))(uintptr_t)RH_FW_TIMER_CREATE)
        (callback, interval_ms, 0);
}
void rh_platform_timer_delete(void *timer) {
    ((void (*)(void *))(uintptr_t)RH_FW_TIMER_DELETE)(timer);
}

#include "resource_hook_platform.h"
#include "resource_hook_target.h"
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
static int retire_cache(uint32_t cache, uint32_t source_offset,
                        const struct rh_mapping_view *mapping) {
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
            affected(mapping, 0, source)) {
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
    if (!mapping || mapping->count > RH_RULES ||
        (mapping->count && !mapping->rules)) return -1;
    if (!mapping->count) return 1;
    if (!cache_valid(data, RH_FW_IMAGE_CACHE_CLASS) ||
        !cache_valid(header, RH_FW_HEADER_CACHE_CLASS)) return -1;
    if (retire_cache(header, 0u, mapping)) return -1;
    return retire_cache(data, 4u, mapping);
}
int rh_platform_retire_images(const struct rh_state *state) {
    struct rh_mapping_view view;
    if (!state) return -1;
    view.rules = state->rules;
    view.count = state->count;
    return rh_platform_retire_mapped_images(&view);
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
    if (!current || current->count > RH_RULES ||
        (current->count && !current->rules) ||
        (previous && (previous->count > RH_RULES ||
                      (previous->count && !previous->rules)))) return -1;
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
    view.rules = state->rules;
    view.count = state->count;
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

#include "resource_hook_platform.h"
#include "canopus_band11_memory.h"

/* Addresses and layout apply only to the fingerprinted 4.100.139 target. */
int rh_platform_open(const char *path, int mode) {
    return ((int (*)(const char *, int, ...))(uintptr_t)0x0c342c55u)(path, mode);
}
int rh_platform_read(int fd, void *out, uint32_t size) {
    return ((int (*)(int, void *, uint32_t))(uintptr_t)0x0c33d785u)(fd, out, size);
}
int rh_platform_write(int fd, const void *data, uint32_t size) {
    return ((int (*)(int, const void *, uint32_t))(uintptr_t)0x0c33dc4fu)(fd, data, size);
}
void rh_platform_close(int fd) {
    ((int (*)(int))(uintptr_t)0x0c33818du)(fd);
}
void *rh_platform_alloc(uint32_t size) { return b11_temp_alloc(8u, size); }
void rh_platform_free(void *p) { b11_temp_free(p); }
void *rh_platform_driver(void) { return (void *)(uintptr_t)0x200bd3b8u; }
rh_open_fn *rh_platform_slot(void) { return (rh_open_fn *)(uintptr_t)0x200bd3c4u; }
rh_open_fn rh_platform_original(void) { return (rh_open_fn)(uintptr_t)0x0c3a6195u; }
int rh_platform_driver_valid(void) {
    return *(volatile unsigned char *)(uintptr_t)0x200bd3b8u == '/' &&
           *(volatile uint32_t *)(uintptr_t)0x200bd3bcu == 4096u;
}
uint32_t rh_platform_lock(void) { return b11_irq_lock(); }
void rh_platform_unlock(uint32_t irq) {
    b11_barrier();
    b11_irq_unlock(irq);
}
/* Retire entries individually: lv_cache_drop marks held entries invalid and
 * unlinks them without freeing their payload until the last release. In contrast,
 * the .139 drop_all implementation also destroys tree storage for held entries.
 * This traversal is specific to the recovered LRU/RB class, never other classes. */
/* Always retire the entry currently at the list head (cache+52) and confirm the
 * head advanced, so the node's own next-pointer layout is never relied on. The
 * key lv_cache_drop expects is a pointer to a {src, type, ...} record, which is
 * exactly the layout of an entry's data — lv_image_cache_drop builds a
 * temporary one of the same shape for a single-source drop. It is *(node); the
 * word at data+16 is an internal pool offset, not a key. */
static int retire_cache(uint32_t cache) {
    uint32_t n;
    for (n = 0; n < 4096u; n++) {
        uint32_t node = *(volatile uint32_t *)(uintptr_t)(cache + 52u);
        uint32_t key;
        if (!node) return 0;
        key = *(volatile uint32_t *)(uintptr_t)node;
        if (!key) return -1;
        ((void (*)(uint32_t, uint32_t))(uintptr_t)0x0c8b8cafu)(cache, key);
        if (*(volatile uint32_t *)(uintptr_t)(cache + 52u) == node) return -1;
    }
    return -1;
}
/* lv_init creates the two image caches with DISTINCT class objects: the decoded
 * cache with 0x2ca168c4 ("IMAGE") and the header cache with 0x2ca16944
 * ("IMAGE_HEADER"). They are the same LRU/RB implementation — their vtables
 * differ only at +4, and share the +12 lookup, +20 unlink and +28 drop_all this
 * traversal depends on — but they are not the same pointer, so each cache is
 * checked against the class lv_init actually gave it. */
int rh_platform_image_cache_drop_all(void) {
    uint32_t data = *(volatile uint32_t *)(uintptr_t)0x200bd310u;
    uint32_t header = *(volatile uint32_t *)(uintptr_t)0x200bd314u;
    if (!data || !header) return -1;
    if (*(volatile uint32_t *)(uintptr_t)data != 0x2ca168c4u) return -1;
    if (*(volatile uint32_t *)(uintptr_t)header != 0x2ca16944u) return -1;
    if (retire_cache(header)) return -1;
    return retire_cache(data);
}
/* The exact .139 lv_display_get_screen_active (0x0c3807ec) reads +696,
 * not +24 (DPI). Never clear caches or invalidate while rendering. */
int rh_platform_redraw_ready(void) {
    uint32_t disp = *(volatile uint32_t *)(uintptr_t)0x200bd200u;
    if (!disp || !*(volatile uint32_t *)(uintptr_t)(disp + 696u)) return 0;
    if (*(volatile unsigned char *)(uintptr_t)(disp + 58u) & 2u) return 0;
    return *(volatile int32_t *)(uintptr_t)(disp + 608u) > 0;
}
/* Request, rather than synchronously execute, a full redraw. Return success
 * only if the firmware retained a dirty area covering the entire display. */
int rh_platform_request_full_redraw(void) {
    int32_t area[4] = {0, 0, 0x7fff, 0x7fff};
    uint32_t disp = *(volatile uint32_t *)(uintptr_t)0x200bd200u;
    uint32_t i, count;
    if (!rh_platform_redraw_ready()) return -1;
    ((void (*)(uint32_t, const void *))(uintptr_t)0x0c382429u)(disp, area);
    count = *(volatile uint32_t *)(uintptr_t)(disp + 604u);
    if (count > 32u) return -1;
    for (i = 0; i < count; i++) {
        volatile int32_t *a = (volatile int32_t *)(uintptr_t)(disp + 60u + i * 16u);
        if (a[0] <= 0 && a[1] <= 0 &&
            a[2] >= *(volatile int32_t *)(uintptr_t)disp - 1 &&
            a[3] >= *(volatile int32_t *)(uintptr_t)(disp + 4u) - 1) return 0;
    }
    return -1;
}
/* Force the stack-top page to rebuild, so widgets holding a resource are
 * destroyed and recreated instead of merely repainted. Recovered .139 ladder:
 * a page keeps its state at +40, its cache policy at +41, its root view at +48
 * and an async-destroy hint at +36. exec_pop_lifecycle_without_cachepolicy
 * (0x0c696e34) runs pause, stop and destroy, ending in lv_obj_delete(page+48).
 * Ordinary navigation uses the with-cachepolicy variant instead, which stops
 * early for policy 2/5/6/7 and leaves a cached page's widgets alive — that is
 * why navigating away and back does not pick up a theme. on_resume_wrapped
 * (0x0c696c18) then runs create, start and resume, rebuilding every widget
 * through the page's on_create callback at page[19], so each resource is
 * re-opened through the redirect.
 *
 * The forced path is NOT fully policy independent: a policy-2 page returns
 * after the pause leg and is never destroyed. Success is therefore judged by
 * the page actually reaching the destroyed state and coming back with a
 * different root view, never by the final state alone.
 *
 * on_resume_wrapped climbs from any of states 4/5/8/9/18 back to 17, so it is
 * also the recovery path and is always called once the teardown has started.
 * A page whose destroy would be deferred onto the pagemanager's ui-destroy
 * stack (+36 set) is skipped: its root view would still be set when the
 * re-create ran, which trips the activitymanager assert. */
int rh_platform_rebuild_active_page(void) {
    uint32_t page = ((uint32_t (*)(int))(uintptr_t)0x0c697451u)(-1);
    uint32_t previous;
    int torn_down;
    if (!page) return -1;
    if (*(volatile unsigned char *)(uintptr_t)0x200c2a28u != 2u) return -1;
    if (!*(volatile unsigned char *)(uintptr_t)0x20096085u) return -1;
    if (*(volatile uint32_t *)(uintptr_t)(page + 36u)) return -1;
    if (*(volatile unsigned char *)(uintptr_t)(page + 41u) == 2u) return -1;
    if (*(volatile unsigned char *)(uintptr_t)(page + 40u) != 17u) return -1;
    previous = *(volatile uint32_t *)(uintptr_t)(page + 48u);
    if (!previous) return -1;
    ((void (*)(uint32_t))(uintptr_t)0x0c696e35u)(page);
    torn_down = *(volatile unsigned char *)(uintptr_t)(page + 40u) == 4u &&
                !*(volatile uint32_t *)(uintptr_t)(page + 48u);
    /* Always bring the page back up, including when the teardown stopped short,
     * so a refused rebuild does not leave the page paused. */
    ((void (*)(uint32_t))(uintptr_t)0x0c696c19u)(page);
    if (!torn_down || *(volatile unsigned char *)(uintptr_t)(page + 40u) != 17u)
        return -1;
    page = *(volatile uint32_t *)(uintptr_t)(page + 48u);
    return page && page != previous ? 0 : -1;
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
    uint32_t uikit = *(volatile uint32_t *)(uintptr_t)0x200bd1e8u;
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
    ((void (*)(uint32_t))(uintptr_t)0x0c904cfdu)(node);
    ((uint32_t (*)(const char *, const char *))(uintptr_t)0x0c4924e1u)(name, path);
    resolved = ((uint32_t (*)(uint32_t, const char *))(uintptr_t)0x0c490eddu)(manager, name);
    return same_string(resolved, path) ? 0 : -1;
}
void *rh_platform_refresh_timer_create(void (*callback)(void *)) {
    return ((void *(*)(void (*)(void *), uint32_t, void *))(uintptr_t)0x0c3abd21u)
        (callback, 50u, 0);
}
void rh_platform_refresh_timer_delete(void *timer) {
    ((void (*)(void *))(uintptr_t)0x0c3abe71u)(timer);
}

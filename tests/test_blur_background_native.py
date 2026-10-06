"""Exercise the verbatim .177 ARM blur adapter with native-call host mocks.

Only descriptors/pixels are dereferenced as firmware uint32_t pointers. They
live in a verified low-address, non-fixed mmap on both macOS and Linux. This is
an adapter regression, not device/GPU or native event-delivery acceptance.
macOS uses an x86_64 executable (Rosetta on Apple Silicon) to allow low mappings.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "resource_hook.h"
#include "resource_hook_background.h"
#include "resource_hook_target_177.h"
/* Execute the verbatim ARM block, but never call firmware addresses. */
#define __arm__ 1

#define WIDTH 212u
#define HEIGHT 520u
#define STRIDE 432u
#define MAP_SIZE (1024u * 1024u)
#define FOREIGN 0x76543210u
#define IMAGE_PATH "/data/background/test.bin"
#define DISPLAY 0x6000u
#define DISPLAY2 0x7000u
struct object {
    uint32_t id, clz, source;
    unsigned gets, sets;
    int present, hidden, ancestor_hidden, offscreen, inactive;
};
static struct object objects[7];
static unsigned creates, snapshots, releases, setters, getters, events, infos;
static unsigned removes, layouts, visibility_calls, classdraws, finishes, wakeups;
static int fail_create, fail_snapshot, reenter_snapshot, fail_event;
static int delete_in_layout, delete_in_snapshot, commit_in_layout, commit_in_setter;
static int switch_in_layout, suppress_ready, rebuild_notify_list_mode = -1;
static unsigned fail_finish_at, reentries;
static uint8_t *arena, *src_desc, *dst_desc;
static uint16_t *src_pixels, *dst_pixels;
static uint32_t src_id, dst_id;
static uint32_t default_display = DISPLAY, live_displays[5] = {DISPLAY};
static unsigned live_count = 1;
static uint32_t registry_size = 792u, bad_prev, bad_tail;
static uint32_t callback_display;
static void (*registered_callback)(void *);
static void (*saved_callback)(void *);
static char operations[4096];
static unsigned operation_count, frame_layouts;
static int snapshot_inflight, snapshot_finished, destination_synced, cpu_source_ready;
static void dispatch(uint32_t target, uint16_t code);
static void pixels_initialized(void);
static void commit_blur_reentrantly(void);
static void switch_display_reentrantly(void);
static void record(char c) {
    assert(operation_count + 1 < sizeof(operations));
    operations[operation_count++] = c;
    operations[operation_count] = 0;
}
static void reset_operations(void) { operation_count = 0; operations[0] = 0; }
static uint32_t pointer32(const void *p) {
    assert((uintptr_t)p <= UINT32_MAX);
    return (uint32_t)(uintptr_t)p;
}
static struct object *object(uint32_t id) {
    for (unsigned i = 0; i < 7; ++i) if (objects[i].id == id) return &objects[i];
    assert(!"unexpected object");
    return 0;
}
static int live(uint32_t display) {
    for (unsigned i = 0; i < live_count; ++i)
        if (live_displays[i] == display) return 1;
    return 0;
}
static uint32_t load32(uint32_t address) {
    switch (address) {
    case RH_FW_DISPLAY_SLOT: return default_display;
    case RH_FW_FR_DISPLAY_LIST: return registry_size;
    case RH_FW_FR_DISPLAY_LIST + 4u: return live_count ? live_displays[0] : 0;
    case RH_FW_FR_DISPLAY_LIST + 8u: return bad_tail ? bad_tail : live_count ? live_displays[live_count - 1] : 0;
    case RH_LB_MAIN_ROOT_SLOT: return 0x2000u;
    case RH_LB_MAIN_CACHE_SLOT: return objects[1].present ? 0x2100u : 0;
    case RH_LB_MORE_ROOT_SLOT: return 0x2200u;
    case RH_LB_MORE_CACHE_SLOT: return objects[3].present ? 0x2300u : 0;
    case RH_LB_CONTROL_PRESENTER_SLOT: return 0x2400u;
    case 0x2408u: return 0x2500u;
    case RH_LB_NOTIFY_CONTAINER_SLOT: return objects[5].present ? objects[5].id : 0;
    case RH_LB_NOTIFY_LIST_SLOT: return objects[6].present ? objects[6].id : 0;
    case RH_FW_HEADER_CACHE_SLOT:
    case RH_FW_IMAGE_CACHE_SLOT: return 0;
    default:
        for (unsigned i = 0; i < live_count; ++i) {
            uint32_t display = live_displays[i];
            if (address == display + 792u) return display == bad_prev ? 0xbad0000u : i ? live_displays[i - 1] : 0;
            if (address == display + 796u) return i + 1 < live_count ? live_displays[i + 1] : 0;
            static const unsigned offsets[5] = {696, 700, 708, 712, 716};
            for (unsigned j = 0; j < 5; ++j)
                if (address == display + offsets[j]) return 0x8000u + j * 0x100u;
        }
        for (unsigned i = 0; i < 7; ++i)
            if (objects[i].id == address) { assert(objects[i].present); return objects[i].clz; }
        /* Poison default/stale display pointers: membership must be checked first. */
        assert(address >= pointer32(arena) &&
               address <= pointer32(arena) + MAP_SIZE - sizeof(uint32_t));
        return *(const uint32_t *)(uintptr_t)address;
    }
}
static uint32_t native_child(uint32_t root, int index) {
    assert(index == 0);
    unsigned i;
    if (root >= 0x2000u && root <= 0x2300u && !(root & 0xffu))
        i = (root - 0x2000u) / 0x100u;
    else { assert(root == 0x2500u); i = 4; }
    return objects[i].present ? objects[i].id : 0;
}
static void (*object_callbacks[7])(void *);
static uint32_t logical_background_source(uint32_t id);
static void delete_background_identity(unsigned i) {
    if (object_callbacks[i]) {
        uint32_t event[7] = {objects[i].id, objects[i].id, 36u, 0, 0, 0, 0};
        object_callbacks[i](event);
        object_callbacks[i] = 0;
    }
}
static int fail_object_event;
static void *native_object_event(uint32_t id, void (*callback)(void *),
                                 uint32_t filter, void *data) {
    unsigned i = (unsigned)(object(id) - objects);
    assert(!object_callbacks[i] && !filter && !data);
    if (fail_object_event) return 0;
    object_callbacks[i] = callback;
    return &object_callbacks[i];
}
static int native_object_remove(uint32_t id, void (*callback)(void *)) {
    unsigned i = (unsigned)(object(id) - objects);
    assert(object_callbacks[i] == callback);
    object_callbacks[i] = 0;
    return 1;
}
static void native_rect_init(void *p) {
    memset(p, 0, 116);
    ((unsigned char *)p)[32] = 255;
    ((unsigned char *)p)[59] = 255;
}
static unsigned rectangle_draws;
static void native_rect_draw(uint32_t layer, const void *descriptor, const int32_t *area) {
    const uint32_t *p = descriptor;
    assert(layer == 0x9900u && p[12]);
    assert(!((const unsigned char *)p)[32]);
    assert(area[0] == 0 && area[1] == 0 && area[2] == 211 && area[3] == 519);
    ++rectangle_draws;
}
static uint32_t native_get(uint32_t id, uint32_t part, uint32_t property) {
    assert(part == 0 && property == 40u);
    ++getters;
    ++object(id)->gets;
    unsigned i = (unsigned)(object(id) - objects);
    return object_callbacks[i] && object(id)->source == logical_background_source(id)
        ? 0 : object(id)->source;
}
static void native_set(uint32_t id, uint32_t property, uint32_t source,
                       uint32_t selector) {
    assert(property == 40u && selector == 0);
    if (source == dst_id) { assert(destination_synced); pixels_initialized(); }
    record('P');
    ++setters;
    ++object(id)->sets;
    object(id)->source = source ? source : logical_background_source(id);
    if (id == objects[5].id && !source && rebuild_notify_list_mode >= 0) {
        delete_background_identity(6);
        if (!rebuild_notify_list_mode) objects[6].present = 0;
        else {
            objects[6].id += 0x1000u; /* old sibling address is now poisoned */
            objects[6].source = rebuild_notify_list_mode == 1 ? dst_id : FOREIGN;
            objects[6].gets = objects[6].sets = 0;
        }
        rebuild_notify_list_mode = -1;
    }
    if (commit_in_setter) { commit_in_setter = 0; commit_blur_reentrantly(); }
}
static void *native_event(uint32_t list, void (*callback)(void *),
                          uint32_t filter, void *data) {
    assert(live(list - 736u));
    assert(callback && filter == 0x8000u && !data);
    ++events;
    if (fail_event) return 0;
    assert(!registered_callback && "no object hooks or duplicate display hook");
    callback_display = list - 736u;
    registered_callback = saved_callback = callback;
    return (void *)(uintptr_t)1;
}
static int native_remove(uint32_t display, void (*callback)(void *), void *data) {
    assert(live(display));
    assert(display == callback_display && callback == registered_callback && !data);
    ++removes;
    registered_callback = 0;
    callback_display = 0;
    return 1;
}
static void native_wake(uint32_t display, const int32_t area[4]) {
    assert(live(display) && display == default_display);
    assert(area[0] == 0 && area[1] == 0 && area[2] == 211 && area[3] == 519);
    ++wakeups;
    /* Native invalidation queues event 51; it is not synchronous REFR_START. */
}
static void native_delete_display(void) {
    dispatch(callback_display, 36u);
    /* Native destruction retires the callback list and registry node. */
    registered_callback = 0;
    callback_display = 0;
    live_count = 0;
}
static void native_layout(uint32_t screen) {
    assert(screen == 0x8000u + frame_layouts * 0x100u);
    ++frame_layouts;
    ++layouts;
    if (delete_in_layout) { delete_in_layout = 0; native_delete_display(); }
    if (commit_in_layout) { commit_in_layout = 0; commit_blur_reentrantly(); }
    if (switch_in_layout) { switch_in_layout = 0; switch_display_reentrantly(); }
}
static int native_visible(uint32_t id, int32_t area[4]) {
    struct object *o = object(id);
    assert(frame_layouts == 5 && "all native screens need layout before visibility");
    assert(area[0] == 0 && area[1] == 0 && area[2] == 211 && area[3] == 519);
    ++visibility_calls;
    return o->present && !o->hidden && !o->ancestor_hidden && !o->offscreen && !o->inactive;
}
static uint32_t native_create(void *handlers, int width, int height,
                               int format, int stride) {
    assert((uintptr_t)handlers == RH_LB_DRAW_BUF_HANDLERS);
    assert(width == WIDTH && height == HEIGHT && format == 18 && stride == 0);
    ++creates;
    return fail_create ? 0 : dst_id;
}
static int native_finish(void) {
    record('F');
    ++finishes;
    if (finishes == fail_finish_at) return -1;
    if (snapshot_inflight) snapshot_finished = 1;
    else cpu_source_ready = 0;
    return 0;
}
static void native_barrier(void) { record('B'); }
static void native_sync(unsigned id) {
    assert(id == 1);
    record('S');
    if (cpu_source_ready) {
        pixels_initialized();
        destination_synced = 1;
    }
}
static void native_invalidate(unsigned id, uint32_t ptr, uint32_t length) {
    assert(id == 1 && ptr == pointer32(src_pixels) && length == STRIDE * HEIGHT);
    assert(snapshot_inflight && snapshot_finished);
    record('I');
    cpu_source_ready = 1;
    /* GPU-owned pixels become CPU-visible only after the invalidate. */
    for (unsigned i = 0; i < STRIDE * HEIGHT / 2; ++i) src_pixels[i] = 0xffffu;
}
static uint32_t native_snapshot(void) {
    record('T');
    assert(!snapshot_inflight && "pending GPU snapshot must stay pinned");
    ++snapshots;
    if (reenter_snapshot) {
        reenter_snapshot = 0;
        ++reentries;
        dispatch(callback_display, 52u);
    }
    if (fail_snapshot) return 0;
    if (delete_in_snapshot) { delete_in_snapshot = 0; native_delete_display(); }
    snapshot_inflight = 1;
    snapshot_finished = 0;
    for (unsigned i = 0; i < STRIDE * HEIGHT / 2; ++i) src_pixels[i] = 0x1111u;
    return src_id;
}
static void native_release(uint32_t snap) {
    assert(snap == src_id && snapshot_inflight && snapshot_finished);
    record('R');
    snapshot_inflight = 0;
    ++releases;
}
static int native_info(const char *path, void *header) {
    assert(!strcmp(path, IMAGE_PATH));
    memset(header, 0, 12);
    ++infos;
    return 1;
}
static int cache_valid(uint32_t cache, uint32_t clz) {
    (void)cache; (void)clz;
    assert(!"no caches in this fixture");
    return 0;
}
static int retire_cache(uint32_t cache, uint32_t offset,
                        const struct rh_mapping_view *mapping, const char *exact) {
    (void)cache; (void)offset; (void)mapping; (void)exact;
    assert(!"no caches in this fixture");
    return -1;
}
#undef RH_LB_CHILD_GET
#undef RH_LB_STYLE_SET
#undef RH_FW_OBJECT_STYLE_GET
#undef RH_FW_IMAGE_GET_INFO
#undef RH_LB_DISPLAY_EVENT_ADD
#undef RH_LB_DISPLAY_EVENT_REMOVE
#undef RH_FW_INVALIDATE_AREA
#undef RH_LB_LAYOUT_UPDATE
#undef RH_LB_VISIBLE_AREA
#undef RH_LB_GPU_FINISH
#undef RH_LB_DCACHE_SYNC
#undef RH_LB_DCACHE_INVALIDATE
#undef RH_LB_MEMORY_BARRIER
#undef RH_LB_DRAW_BUF_CREATE
#undef RH_LB_SNAPSHOT_TAKE
#undef RH_LB_SNAPSHOT_FREE
#undef RH_LB_OBJECT_EVENT_ADD
#undef RH_LB_OBJECT_EVENT_REMOVE
#undef RH_LB_RECT_INIT
#undef RH_LB_RECT_DRAW
#define RH_LB_OBJECT_EVENT_ADD ((uintptr_t)native_object_event)
#define RH_LB_OBJECT_EVENT_REMOVE ((uintptr_t)native_object_remove)
#define RH_LB_RECT_INIT ((uintptr_t)native_rect_init)
#define RH_LB_RECT_DRAW ((uintptr_t)native_rect_draw)
#define RH_LB_CHILD_GET ((uintptr_t)native_child)
#define RH_LB_STYLE_SET ((uintptr_t)native_set)
#define RH_FW_OBJECT_STYLE_GET ((uintptr_t)native_get)
#define RH_FW_IMAGE_GET_INFO ((uintptr_t)native_info)
#define RH_LB_DISPLAY_EVENT_ADD ((uintptr_t)native_event)
#define RH_LB_DISPLAY_EVENT_REMOVE ((uintptr_t)native_remove)
#define RH_FW_INVALIDATE_AREA ((uintptr_t)native_wake)
#define RH_LB_LAYOUT_UPDATE ((uintptr_t)native_layout)
#define RH_LB_VISIBLE_AREA ((uintptr_t)native_visible)
#define RH_LB_GPU_FINISH ((uintptr_t)native_finish)
#define RH_LB_DCACHE_SYNC ((uintptr_t)native_sync)
#define RH_LB_DCACHE_INVALIDATE ((uintptr_t)native_invalidate)
#define RH_LB_MEMORY_BARRIER() native_barrier()
#define RH_LB_DRAW_BUF_CREATE ((uintptr_t)native_create)
#define RH_LB_SNAPSHOT_TAKE ((uintptr_t)native_snapshot)
#define RH_LB_SNAPSHOT_FREE ((uintptr_t)native_release)
'''

CHECKS = r'''
static uint32_t logical_background_source(uint32_t id) {
    struct screen_background *owned = screen_background_find(id);
    return owned ? owned->source : 0;
}
static void put16(uint8_t *p, unsigned offset, uint16_t value) {
    memcpy(p + offset, &value, sizeof(value));
}
static void put32(uint8_t *p, unsigned offset, uint32_t value) {
    memcpy(p + offset, &value, sizeof(value));
}
static void descriptor(uint8_t *p, const void *pixels) {
    memset(p, 0, 24);
    p[1] = 18; /* RGB565 */
    put16(p, 4, WIDTH);
    put16(p, 6, HEIGHT);
    put16(p, 8, STRIDE);
    put32(p, 12, STRIDE * HEIGHT);
    /* Native descriptor data is aligned +16; +20 owns the raw allocation.
     * A 64-byte prefix catches either source reads or destination writes there. */
    put32(p, 16, pointer32(pixels));
    put32(p, 20, pointer32(pixels) - 64u);
}
static void setup(void) {
    /* Hints, never MAP_FIXED: do not overwrite ASan or the host's mappings. */
    for (uintptr_t hint = 0x10000000u; hint <= 0xe0000000u; hint += 0x10000000u) {
        void *p = mmap((void *)hint, MAP_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) continue;
        if ((uintptr_t)p && (uintptr_t)p <= UINT32_MAX - MAP_SIZE) {
            arena = p;
            break;
        }
        assert(munmap(p, MAP_SIZE) == 0);
    }
    assert(arena && "host must support a non-fixed low-32-bit mmap");
    src_desc = arena;
    dst_desc = arena + 64;
    src_pixels = (uint16_t *)(void *)(arena + 4096);
    dst_pixels = (uint16_t *)(void *)(arena + 512 * 1024);
    src_id = pointer32(src_desc);
    dst_id = pointer32(dst_desc);
    descriptor(src_desc, src_pixels);
    descriptor(dst_desc, dst_pixels);
    memset((uint8_t *)src_pixels - 64, 0xa7, 64);
    memset((uint8_t *)dst_pixels - 64, 0xa7, 64);
    for (unsigned i = 0; i < STRIDE * HEIGHT / 2; ++i) {
        src_pixels[i] = 0xffffu;
        dst_pixels[i] = 0x1234u;
    }
    for (unsigned i = 0; i < 7; ++i) {
        objects[i].id = 0x3000u + i * 0x100u;
        objects[i].clz = i < 4 ? RH_LB_LIST_CLASS :
                         i < 6 ? RH_LB_BASE_CLASS : RH_LB_NOTIFY_LIST_CLASS;
        /* Native default background source is zero on all seven receivers. */
        objects[i].source = 0;
        objects[i].present = 1;
    }
}
static void select_paths(const char *launcher, const char *cc, const char *ntf) {
    const char *paths[3] = {launcher, cc, ntf};
    for (unsigned i = 0; i < 3; ++i) {
        snprintf(bg_paths[i][bg_path_index], RH_PATH, "%s", paths[i]);
        bg_active[i] = paths[i][0] != 0;
    }
}
static void select_blur(void) { select_paths(RH_BG_BLUR, RH_BG_BLUR, RH_BG_BLUR); }
/* Direct native event structure: target at 0, uint16 code at 8. */
static void dispatch(uint32_t target, uint16_t code) {
    struct { uint32_t target, current_target; uint16_t code, pad; } e = {target, 0, code, 0};
    assert(registered_callback);
    registered_callback(&e);
}
static void fire_frame(void) {
    assert(registered_callback);
    frame_layouts = 0;
    uint32_t frame_display = callback_display;
    dispatch(frame_display, 52u);
    /* A simulated native frame consumes styles only after REFR_START returns.
     * This is ordering evidence, not real GPU/device acceptance. */
    record('D');
    ++classdraws;
    for (unsigned i = 0; i < 7; ++i)
        if (objects[i].present && !objects[i].hidden && !objects[i].ancestor_hidden &&
            !objects[i].offscreen && !objects[i].inactive && objects[i].source == dst_id) {
            assert(destination_synced);
            pixels_initialized();
        }
    /* REFR_READY follows native drawing; retry invalidation schedules NEXT frame. */
    if (registered_callback && !suppress_ready) dispatch(frame_display, 53u);
}
static void refresh(void) {
    rh_platform_backgrounds_poll();
    fire_frame();
}
static void commit_blur_reentrantly(void) {
    struct rh_backgrounds bg = {RH_BG_BLUR, RH_BG_BLUR, RH_BG_BLUR};
    unsigned old_wakeups = wakeups;
    assert(rh_platform_backgrounds_commit(&bg) == 0);
    assert(wakeups == old_wakeups && "in-frame commit must defer invalidation to REFR_READY");
}
static void switch_display_reentrantly(void) {
    live_displays[1] = DISPLAY2;
    live_count = 2;
    default_display = DISPLAY2;
    unsigned old_wakeups = wakeups;
    rh_platform_backgrounds_poll();
    assert(callback_display == DISPLAY2 && wakeups == old_wakeups + 1);
}
static void padding_untouched(void) {
    for (unsigned i = 0; i < 64; ++i) {
        assert(((uint8_t *)src_pixels - 64)[i] == 0xa7);
        assert(((uint8_t *)dst_pixels - 64)[i] == 0xa7);
    }
}
static void pixels_initialized(void) {
    padding_untouched();
    for (unsigned y = 0; y < HEIGHT; ++y)
        for (unsigned x = 0; x < STRIDE / 2; ++x)
            assert(dst_pixels[y * STRIDE / 2 + x] ==
                   (x < WIDTH ? 0xad95u : 0x1234u));
}
static void pixels_untouched(void) {
    padding_untouched();
    for (unsigned i = 0; i < STRIDE * HEIGHT / 2; ++i)
        assert(dst_pixels[i] == 0x1234u);
}
static void all_published(void) {
    for (unsigned i = 0; i < 7; ++i) assert(objects[i].source == dst_id);
    for (unsigned i = 0; i < 3; ++i) assert(current_bg_source(i) == dst_id);
    pixels_initialized();
}
static void no_pixels_published(void) {
    for (unsigned i = 0; i < 7; ++i) assert(objects[i].source == 0);
    assert(!setters);
    pixels_untouched();
}
static void none_published(void) {
    for (unsigned i = 0; i < 3; ++i) assert(current_bg_source(i) == 0);
    no_pixels_published();
}
static void stable_polls(void) {
    unsigned old_snapshots = snapshots, old_sets = setters, old_creates = creates;
    for (unsigned i = 0; i < 20; ++i) rh_platform_backgrounds_poll();
    assert(snapshots == old_snapshots && setters == old_sets && creates == old_creates);
}
static void test_zero_default(void) {
    select_blur();
    refresh();
    assert(creates == 1 && snapshots == 1 && releases == 1);
    assert(setters == 7);
    all_published();
    stable_polls();
}
static void test_cold_commit(void) {
    struct rh_backgrounds bg = {RH_BG_BLUR, RH_BG_BLUR, RH_BG_BLUR};
    assert(rh_platform_backgrounds_stage(&bg) == 0);
    assert(!creates && !snapshots && !setters && !events);
    for (unsigned i = 0; i < 7; ++i) assert(objects[i].source == 0);
    assert(rh_platform_backgrounds_commit(&bg) == 0);
    assert(!creates && !snapshots && !setters && events == 1 && wakeups >= 1);
    refresh();
    assert(creates == 1 && snapshots == 1 && releases == 1);
    assert(setters == 7 && g_blur_valid && !s_blur_needed);
    for (unsigned i = 0; i < 7; ++i) assert(objects[i].sets == 1);
    all_published();
    stable_polls();
}
static void test_foreign_snapshot_retry(void) {
    for (unsigned i = 0; i < 7; ++i) objects[i].source = FOREIGN;
    struct rh_backgrounds bg = {RH_BG_BLUR, RH_BG_BLUR, RH_BG_BLUR};
    fail_snapshot = 1;
    assert(rh_platform_backgrounds_stage(&bg) == 0);
    assert(rh_platform_backgrounds_commit(&bg) == 0);
    assert(!creates && !snapshots && !setters && events == 1 && wakeups >= 1);
    refresh();
    assert(creates == 1 && snapshots == 1 && releases == 0);
    assert(!setters && events == 1 && !g_blur_valid && s_blur_needed);
    for (unsigned i = 0; i < 7; ++i)
        assert(objects[i].source == FOREIGN && !objects[i].sets);
    for (unsigned i = 0; i < 3; ++i) assert(current_bg_source(i) == 0);
    pixels_untouched();
    fail_snapshot = 0;
    refresh();
    assert(creates == 1 && snapshots == 2 && releases == 1 && setters == 7);
    for (unsigned i = 0; i < 7; ++i) assert(objects[i].sets == 1);
    all_published();
    stable_polls();
}
static void test_retry(int allocation) {
    select_blur();
    fail_create = allocation;
    fail_snapshot = !allocation;
    refresh();
    assert(creates == 1 && snapshots == (allocation ? 0u : 1u) && releases == 0);
    none_published();
    refresh();
    assert(creates == (allocation ? 2u : 1u));
    assert(snapshots == (allocation ? 0u : 2u));
    none_published();
    fail_create = fail_snapshot = 0;
    refresh();
    assert(creates == (allocation ? 3u : 1u) && releases == 1 && setters == 7);
    all_published();
    stable_polls();
}
static unsigned receiver_type(unsigned i) { return i < 4 ? 0u : i == 4 ? 1u : 2u; }
static void test_mixed(unsigned image_type) {
    select_blur();
    snprintf(bg_paths[image_type][bg_path_index], RH_PATH, "%s", IMAGE_PATH);
    fail_snapshot = 1;
    refresh();
    assert(creates == 1 && snapshots == 1 && releases == 0);
    unsigned image_count = image_type == 0 ? 4u : image_type == 1 ? 1u : 2u;
    assert(setters == image_count && getters >= image_count);
    uint32_t image = (uint32_t)(uintptr_t)bg_paths[image_type][bg_path_index];
    for (unsigned i = 0; i < 7; ++i) {
        if (receiver_type(i) == image_type) assert(objects[i].source == image);
        else assert(!objects[i].source && !objects[i].sets);
    }
    for (unsigned i = 0; i < 3; ++i)
        if (i != image_type) assert(current_bg_source(i) == 0);
    fail_snapshot = 0;
    refresh();
    assert(creates == 1 && snapshots == 2 && releases == 1 && setters == 7);
    for (unsigned i = 0; i < 7; ++i)
        assert(objects[i].source == (receiver_type(i) == image_type ? image : dst_id));
    pixels_initialized();
    stable_polls();
}
static void test_invalid(const char *side, const char *field) {
    select_blur();
    uint8_t *p = !strcmp(side, "src") ? src_desc : dst_desc;
    if (!strcmp(field, "pixels")) put32(p, 16, 0);
    else if (!strcmp(field, "odd-pixels")) put32(p, 16, pointer32(src_pixels) + 1);
    else if (!strcmp(field, "format")) p[1] = 17;
    else if (!strcmp(field, "width")) put16(p, 4, WIDTH - 1);
    else if (!strcmp(field, "height")) put16(p, 6, HEIGHT - 1);
    else if (!strcmp(field, "short-stride")) put16(p, 8, WIDTH * 2 - 2);
    else if (!strcmp(field, "odd-stride")) put16(p, 8, STRIDE - 1);
    else if (!strcmp(field, "size")) put32(p, 12, STRIDE * HEIGHT - 1);
    else assert(!"unknown metadata field");
    refresh();
    assert(creates == 1);
    /* Validate destination before acquisition; release rejected sources. */
    assert(snapshots == (p == src_desc ? 1u : 0u) && releases == snapshots);
    none_published();
    descriptor(p, p == src_desc ? (void *)src_pixels : (void *)dst_pixels);
    refresh();
    assert(creates == 1 && setters == 7 && releases == snapshots);
    all_published();
}
static void test_callback_retry(void) {
    test_zero_default();
    assert(registered_callback);
    s_blur_needed = 1;
    fail_snapshot = 1;
    unsigned before = snapshots, old_sets = setters;
    refresh();
    assert(snapshots == before + 1 && s_blur_needed);
    all_published(); /* Failure does not discard the last valid pixels. */
    fail_snapshot = 0;
    refresh();
    assert(snapshots == before + 2 && !s_blur_needed);
    refresh();
    assert(snapshots == before + 2 && setters == old_sets);
    all_published();
}
static void test_reentrant_callback(void) {
    test_zero_default();
    s_blur_needed = 1;
    reenter_snapshot = 1;
    unsigned before = snapshots, old_sets = setters;
    refresh();
    assert(reentries == 1 && snapshots == before + 1 && releases == snapshots);
    assert(!s_blur_needed && setters == old_sets);
    all_published();
}
static void test_update_result(void) {
    select_blur();
    fail_snapshot = 1;
    refresh();
    assert(!g_blur_valid && s_blur_needed && !rh_platform_get_blur_draw_buf());
    none_published();
    fail_snapshot = 0;
    refresh();
    assert(g_blur_valid && !s_blur_needed);
    pixels_initialized();
    assert(rh_platform_get_blur_draw_buf() == dst_id);
}
static void test_reload(void) {
    test_zero_default();
    s_blur_needed = 0;
    struct rh_backgrounds bg = {RH_BG_BLUR, RH_BG_BLUR, RH_BG_BLUR};
    unsigned before = snapshots, old_wakeups = wakeups;
    assert(rh_platform_backgrounds_stage(&bg) == 0);
    assert(rh_platform_backgrounds_commit(&bg) == 0);
    assert(s_blur_needed && snapshots == before && events == 1 && wakeups == old_wakeups + 1);
    all_published(); /* Reload marks dirty without invalidating initialized pixels. */
    fail_snapshot = 1;
    refresh();
    assert(s_blur_needed && snapshots == before + 1);
    all_published();
    fail_snapshot = 0;
    refresh();
    assert(!s_blur_needed && snapshots == before + 2);
    stable_polls();
}
static void test_system(void) {
    struct rh_backgrounds bg = {{0}, {0}, {0}};
    assert(rh_platform_backgrounds_stage(&bg) == 0);
    assert(rh_platform_backgrounds_commit(&bg) == 0);
    for (unsigned i = 0; i < 20; ++i) rh_platform_backgrounds_poll();
    assert(!creates && !snapshots && !releases && !events && !setters && !infos);
    assert(!s_blur_needed);
}
static void test_system_after_blur(void) {
    test_zero_default();
    struct rh_backgrounds bg = {{0}, {0}, {0}};
    unsigned old_snapshots = snapshots, old_creates = creates, old_releases = releases;
    assert(rh_platform_backgrounds_stage(&bg) == 0);
    assert(rh_platform_backgrounds_commit(&bg) == 0);
    assert(!s_blur_needed);
    for (unsigned i = 0; i < 20; ++i) rh_platform_backgrounds_poll();
    assert(removes == 1 && !registered_callback);
    /* Stale callback delivery must still be harmless after unregistration. */
    struct { uint32_t target, current; uint16_t code, pad; } e = {DISPLAY, 0, 52, 0};
    saved_callback(&e);
    assert(snapshots == old_snapshots && creates == old_creates && releases == old_releases);
    for (unsigned i = 0; i < 7; ++i) assert(objects[i].source == 0);
}
static void test_poll_registration_only(void) {
    select_blur();
    for (unsigned i = 0; i < 20; ++i) rh_platform_backgrounds_poll();
    assert(events == 1 && wakeups == 1 && !removes && registered_callback);
    assert(!creates && !snapshots && !setters && !getters && !layouts && !visibility_calls);
    none_published();
    refresh();
    assert(events == 1 && snapshots == 1 && setters == 7 && classdraws == 1);
    all_published();
}
static void test_frame_order(void) {
    select_blur();
    reset_operations();
    refresh();
    assert(!strcmp(operations, "FBSBTFBSBIBRBSBPPPPPPPD"));
    assert(layouts == 5 && visibility_calls == 7 && events == 1);
    all_published();
    unsigned before = snapshots, old_sets = setters;
    reset_operations();
    for (unsigned i = 0; i < 12; ++i) refresh();
    assert(!strcmp(operations, "DDDDDDDDDDDD"));
    assert(snapshots == before && setters == old_sets && creates == 1 && events == 1);
}
static void test_gpu_failure(int after, int last_good) {
    if (last_good) test_zero_default();
    else select_blur();
    s_blur_needed = 1;
    unsigned old_snapshots = snapshots, old_creates = creates, old_sets = setters, old_releases = releases;
    fail_finish_at = finishes + (after ? 2u : 1u);
    reset_operations();
    refresh();
    assert(s_blur_needed && setters == old_sets && releases == old_releases);
    assert(snapshots == old_snapshots + (after ? 1u : 0u));
    assert(creates == old_creates + (!last_good && after ? 1u : 0u));
    assert(!strcmp(operations, after ? "FBSBTFD" : "FD"));
    if (last_good) all_published(); else none_published();
    if (after) {
        assert(g_blur_pending_snapshot == src_id && snapshot_inflight && !snapshot_finished);
        /* Watch maintenance can run without any frame. A failed fence keeps
         * ownership pinned; a later successful fence only retires the pool. */
        fail_finish_at = finishes + 1;
        reset_operations();
        rh_platform_backgrounds_poll();
        assert(!strcmp(operations, "F"));
        assert(g_blur_pending_snapshot == src_id && snapshots == old_snapshots + 1 && releases == old_releases);
        if (last_good) all_published(); else none_published();
        fail_finish_at = 0;
        reset_operations();
        rh_platform_backgrounds_poll();
        assert(!strcmp(operations, "FR") && !g_blur_pending_snapshot);
        assert(snapshots == old_snapshots + 1 && releases == old_releases + 1 && setters == old_sets);
        if (last_good) all_published(); else none_published();
    } else assert(!g_blur_pending_snapshot);
    fail_finish_at = 0;
    reset_operations();
    refresh();
    assert(!s_blur_needed && !g_blur_pending_snapshot);
    assert(releases == old_releases + (after ? 2u : 1u));
    assert(snapshots == old_snapshots + (after ? 2u : 1u));
    assert(!strncmp(operations, "FBSBT", 5u));
    all_published();
}
static void test_pending_snapshot_without_visible_blur(int image) {
    select_blur();
    fail_finish_at = 2;
    refresh();
    assert(g_blur_pending_snapshot == src_id && snapshots == 1 && !releases);
    fail_finish_at = 0;
    if (image) {
        struct rh_backgrounds bg = {IMAGE_PATH, IMAGE_PATH, IMAGE_PATH};
        assert(rh_platform_backgrounds_stage(&bg) == 0);
        assert(rh_platform_backgrounds_commit(&bg) == 0);
    } else for (unsigned i = 0; i < 7; ++i) objects[i].hidden = 1;
    reset_operations();
    refresh();
    assert(!g_blur_pending_snapshot && releases == 1 && snapshots == 1 && creates == 1);
    assert(!strcmp(operations, image ? "PPPPPPPD" : "FRD"));
    pixels_untouched();
    if (image) {
        assert(!s_blur_needed && setters == 7 && events == 1);
        for (unsigned i = 0; i < 7; ++i)
            assert(objects[i].source == (uint32_t)(uintptr_t)bg_paths[receiver_type(i)][bg_path_index]);
    } else {
        assert(s_blur_needed && !setters);
        objects[6].hidden = 0;
        refresh();
        assert(snapshots == 2 && !s_blur_needed);
        all_published();
    }
}
static void test_failed_pending_release_aborts_refresh(void) {
    select_blur();
    fail_finish_at = 2;
    refresh();
    assert(g_blur_pending_snapshot == src_id && snapshots == 1 && !releases);
    unsigned old_wakeups = wakeups, old_layouts = layouts, old_finishes = finishes;
    fail_finish_at = finishes + 1;
    reset_operations();
    fire_frame(); /* no watcher maintenance before this native frame */
    assert(!strcmp(operations, "FD") && finishes == old_finishes + 1);
    assert(snapshots == 1 && !releases && !setters && layouts == old_layouts);
    assert(g_blur_pending_snapshot == src_id && s_blur_needed && wakeups == old_wakeups + 1);
    none_published();
    fail_finish_at = 0;
    reset_operations();
    fire_frame();
    assert(!strncmp(operations, "FRFBSBT", 7));
    assert(snapshots == 2 && releases == 2 && !g_blur_pending_snapshot && !s_blur_needed);
    all_published();
}
static void test_commit_during_frame(int setter, int missed_ready) {
    select_blur();
    rh_platform_backgrounds_poll();
    unsigned old_wakeups = wakeups;
    commit_in_layout = !setter;
    commit_in_setter = setter;
    suppress_ready = missed_ready;
    fire_frame();
    suppress_ready = 0;
    if (missed_ready) {
        assert(wakeups == old_wakeups);
        rh_platform_backgrounds_poll();
        assert(wakeups == old_wakeups + 1 && events == 1);
    }
    assert(events == 1 && !removes && s_blur_needed && wakeups == old_wakeups + 1);
    assert(snapshots == (setter ? 1u : 0u) && setters == (setter ? 1u : 0u));
    if (setter) pixels_initialized(); else none_published();
    refresh();
    assert(events == 1 && !s_blur_needed && setters == 7);
    assert(snapshots == (setter ? 2u : 1u) && wakeups == old_wakeups + 1);
    all_published();
}
static void test_new_display_registration_during_layout_wakes_new_display(void) {
    select_blur();
    rh_platform_backgrounds_poll();
    assert(events == 1 && wakeups == 1);
    switch_in_layout = 1;
    fire_frame();
    assert(events == 2 && removes == 1 && wakeups == 2 && callback_display == DISPLAY2);
    assert(!snapshots && !creates && !setters && layouts == 1);
    none_published();
    /* The old frame's delayed ready event cannot consume or schedule new work. */
    dispatch(DISPLAY, 53u);
    assert(wakeups == 2 && !snapshots && !setters);
    fire_frame();
    assert(snapshots == 1 && setters == 7 && events == 2 && wakeups == 2 && !s_blur_needed);
    all_published();
}
static void test_system_commit_rediscovers_notification_list(unsigned mode) {
    test_zero_default();
    unsigned old_snapshots = snapshots, old_sets = setters, old_list_sets = objects[6].sets;
    rebuild_notify_list_mode = (int)mode;
    struct rh_backgrounds bg = {{0}, {0}, {0}};
    assert(rh_platform_backgrounds_stage(&bg) == 0);
    assert(rh_platform_backgrounds_commit(&bg) == 0);
    assert(rebuild_notify_list_mode == -1 && !registered_callback && removes == 1 && events == 1);
    assert(snapshots == old_snapshots && !s_blur_needed);
    for (unsigned i = 0; i < 6; ++i) assert(!objects[i].source);
    if (!mode) assert(!objects[6].present && objects[6].sets == old_list_sets);
    else {
        assert(objects[6].id == 0x4600u && objects[6].present);
        assert(objects[6].source == (mode == 1 ? 0 : FOREIGN));
        assert(objects[6].sets == (mode == 1 ? 1u : 0u));
    }
    assert(setters == old_sets + 6u + (mode == 1 ? 1u : 0u));
    pixels_initialized();
    stable_polls();
}
static void test_pending_snapshot_watch_without_frames(int image, int invalid_registry) {
    select_blur();
    fail_finish_at = 2;
    refresh();
    assert(g_blur_pending_snapshot == src_id && snapshots == 1 && !releases);
    if (image) {
        struct rh_backgrounds bg = {IMAGE_PATH, IMAGE_PATH, IMAGE_PATH};
        fail_finish_at = finishes + 1;
        assert(rh_platform_backgrounds_stage(&bg) == 0);
        assert(rh_platform_backgrounds_commit(&bg) == 0);
        assert(g_blur_pending_snapshot == src_id && !releases && !setters);
    } else for (unsigned i = 0; i < 7; ++i) objects[i].hidden = 1;
    if (invalid_registry) registry_size = 791u;
    unsigned old_classdraws = classdraws, old_wakeups = wakeups, old_layouts = layouts;
    fail_finish_at = finishes + 1;
    reset_operations();
    rh_platform_backgrounds_poll();
    assert(!strcmp(operations, "F") && g_blur_pending_snapshot == src_id && !releases);
    fail_finish_at = 0;
    reset_operations();
    rh_platform_backgrounds_poll();
    assert(!strcmp(operations, "FR") && !g_blur_pending_snapshot && releases == 1);
    assert(snapshots == 1 && creates == 1 && !setters && events == 1 && !removes);
    assert(classdraws == old_classdraws && wakeups == old_wakeups && layouts == old_layouts);
    pixels_untouched();
    reset_operations();
    for (unsigned i = 0; i < 20; ++i) rh_platform_backgrounds_poll();
    assert(!operations[0] && snapshots == 1 && releases == 1 && !setters);
}
static void test_pending_snapshot_system_maintenance(void) {
    select_blur();
    fail_finish_at = 2;
    refresh();
    assert(g_blur_pending_snapshot == src_id && snapshot_inflight && !releases);
    no_pixels_published();
    struct rh_backgrounds bg = {{0}, {0}, {0}};
    fail_finish_at = finishes + 1;
    reset_operations();
    assert(rh_platform_backgrounds_commit(&bg) == 0);
    assert(!strcmp(operations, "F"));
    assert(removes == 1 && !registered_callback && g_blur_pending_snapshot == src_id);
    assert(snapshots == 1 && !releases && !s_blur_needed);
    no_pixels_published();
    fail_finish_at = finishes + 1;
    reset_operations();
    rh_platform_backgrounds_poll();
    assert(!strcmp(operations, "F") && !releases && g_blur_pending_snapshot == src_id);
    no_pixels_published();
    fail_finish_at = 0;
    reset_operations();
    rh_platform_backgrounds_poll();
    assert(!strcmp(operations, "FR") && releases == 1 && !g_blur_pending_snapshot);
    assert(snapshots == 1 && creates == 1 && !setters);
    no_pixels_published();
    reset_operations();
    for (unsigned i = 0; i < 20; ++i) rh_platform_backgrounds_poll();
    assert(!operations[0] && !registered_callback && events == 1 && removes == 1);
}
static void test_event_filter(void) {
    select_blur();
    rh_platform_backgrounds_poll();
    dispatch(DISPLAY, 53u);
    dispatch(DISPLAY, 24u);
    dispatch(DISPLAY2, 52u);
    assert(!creates && !snapshots && !setters && !layouts && !visibility_calls);
    refresh();
    assert(snapshots == 1 && events == 1);
    all_published();
}
static void test_registry_late(void) {
    select_blur();
    live_count = 0;
    for (unsigned i = 0; i < 10; ++i) rh_platform_backgrounds_poll();
    assert(!events && !creates && !snapshots && !setters);
    live_count = 1;
    live_displays[0] = DISPLAY;
    rh_platform_backgrounds_poll();
    assert(events == 1 && !snapshots);
    refresh();
    all_published();
}
static void test_registration_retry(void) {
    select_blur();
    fail_event = 1;
    for (unsigned i = 0; i < 3; ++i) rh_platform_backgrounds_poll();
    assert(events == 3 && !registered_callback && !snapshots && !setters);
    fail_event = 0;
    rh_platform_backgrounds_poll();
    assert(events == 4 && registered_callback && !snapshots);
    refresh();
    assert(events == 4);
    all_published();
}
static void test_registry_replaced(void) {
    test_zero_default();
    live_displays[1] = DISPLAY2;
    live_count = 2;
    default_display = DISPLAY2;
    unsigned old_snapshots = snapshots;
    rh_platform_backgrounds_poll();
    assert(events == 2 && removes == 1 && callback_display == DISPLAY2 && snapshots == old_snapshots);
    refresh();
    assert(snapshots == old_snapshots + 1);
    all_published();
    /* Never dereference a default-slot pointer outside the linked list. */
    default_display = 0xdead0000u;
    rh_platform_backgrounds_poll();
    assert(removes == 2 && !registered_callback && snapshots == old_snapshots + 1);
}
static void test_registry_limit(void) {
    select_blur();
    live_count = 5;
    for (unsigned i = 0; i < live_count; ++i) live_displays[i] = DISPLAY + i * 0x1000u;
    rh_platform_backgrounds_poll();
    assert(!events && !snapshots);
    live_count = 4;
    rh_platform_backgrounds_poll();
    assert(events == 1 && !snapshots);
    refresh();
    all_published();
}
static void test_registry_schema(unsigned kind) {
    select_blur();
    if (kind == 0) registry_size = 791u;
    else if (kind == 1) bad_prev = DISPLAY;
    else bad_tail = DISPLAY2;
    rh_platform_backgrounds_poll();
    assert(!events && !creates && !snapshots && !registered_callback);
    registry_size = 792u;
    bad_prev = bad_tail = 0;
    rh_platform_backgrounds_poll();
    assert(events == 1 && !snapshots);
    refresh();
    all_published();
}
static void test_registry_vanished(int queued_event) {
    test_zero_default();
    void (*old_callback)(void *) = registered_callback;
    unsigned old_snapshots = snapshots;
    live_count = 0;
    /* Temporarily absent registry membership is not proof of native DELETE.
     * Retain the hook; no duplicate registration when validation recovers. */
    struct { uint32_t target, current; uint16_t code, pad; } e = {DISPLAY, 0, 52, 0};
    if (queued_event) old_callback(&e); /* no stale-display screen/visibility dereference */
    rh_platform_backgrounds_poll();
    assert(!removes);
    assert(snapshots == old_snapshots);
    live_count = 1;
    live_displays[0] = DISPLAY;
    rh_platform_backgrounds_poll();
    assert(events == 1 && registered_callback);
    refresh();
    assert(snapshots == old_snapshots);
    all_published();
}
static void test_display_delete(void) {
    test_zero_default();
    unsigned old_snapshots = snapshots;
    dispatch(DISPLAY, 36u);
    assert(!background_display && !removes && snapshots == old_snapshots);
    /* Native DELETE frees its event list; adapter must not remove it again. */
    registered_callback = 0;
    callback_display = 0;
    live_count = 0;
    rh_platform_backgrounds_poll();
    assert(events == 1 && !removes);
    default_display = DISPLAY2;
    live_count = 1;
    live_displays[0] = DISPLAY2;
    rh_platform_backgrounds_poll();
    assert(events == 2 && !removes && snapshots == old_snapshots);
    refresh();
    assert(snapshots == old_snapshots + 1);
    all_published();
}
static void test_delete_during_frame(int snapshot, int last_good) {
    if (last_good) test_zero_default();
    else select_blur();
    s_blur_needed = 1;
    unsigned old_snapshots = snapshots, old_sets = setters, old_releases = releases, old_events = events;
    delete_in_layout = !snapshot;
    delete_in_snapshot = snapshot;
    refresh();
    assert(!registered_callback && !background_display && s_blur_needed);
    assert(setters == old_sets && !removes);
    assert(snapshots == old_snapshots + (snapshot ? 1u : 0u));
    assert(releases == old_releases + (snapshot ? 1u : 0u));
    if (last_good) all_published(); else none_published();
    default_display = DISPLAY2;
    live_count = 1;
    live_displays[0] = DISPLAY2;
    refresh();
    assert(events == (old_events ? old_events + 1 : 2));
    assert(snapshots == old_snapshots + (snapshot ? 2u : 1u));
    assert(!s_blur_needed);
    all_published();
}
static void test_failed_frame_wakes_next_refresh(void) {
    select_blur();
    fail_snapshot = 1;
    rh_platform_backgrounds_poll();
    assert(wakeups == 1 && !snapshots);
    frame_layouts = 0;
    dispatch(callback_display, 52u);
    assert(snapshots == 1 && wakeups == 1 && s_blur_needed);
    record('D');
    ++classdraws;
    dispatch(callback_display, 53u);
    assert(wakeups == 2 && snapshots == 1 && !setters);
    fail_snapshot = 0;
    refresh();
    assert(snapshots == 2 && wakeups == 2 && !s_blur_needed);
    all_published();
}
static void test_visibility(unsigned kind) {
    select_blur();
    for (unsigned i = 0; i < 7; ++i) {
        if (kind == 0) objects[i].hidden = 1;
        else if (kind == 1) objects[i].ancestor_hidden = 1;
        else if (kind == 2) objects[i].offscreen = 1;
        else objects[i].inactive = 1;
    }
    refresh();
    assert(!creates && !snapshots && !setters && visibility_calls == 7 && layouts == 5);
    objects[6].hidden = objects[6].ancestor_hidden = objects[6].offscreen = objects[6].inactive = 0;
    refresh();
    assert(snapshots == 1);
    all_published();
    /* First reveal dirties; unchanged frames/redraw/scroll do not. */
    unsigned before = snapshots;
    refresh();
    assert(snapshots == before);
    objects[6].offscreen = 1;
    refresh();
    assert(snapshots == before);
    objects[6].offscreen = 0;
    refresh();
    assert(snapshots == before + 1);
    all_published();
}
static void test_cache_reopen(void) {
    select_paths(RH_BG_BLUR, IMAGE_PATH, IMAGE_PATH);
    for (unsigned i = 0; i < 4; ++i) objects[i].offscreen = 1;
    objects[1].present = 0;
    refresh();
    assert(!creates && !snapshots && setters == 3); /* images, but no visible blur */
    objects[1].present = 1;
    objects[1].offscreen = 0;
    refresh();
    assert(snapshots == 1 && objects[1].source == dst_id);
    unsigned before = snapshots;
    refresh();
    assert(snapshots == before);
    objects[1].present = 0;
    refresh();
    assert(snapshots == before);
    objects[1].present = 1;
    refresh();
    assert(snapshots == before + 1 && objects[1].source == dst_id);
}
static void test_screen_fixed(void) {
    test_zero_default();
    unsigned before = setters;
    refresh();
    assert(setters == before); /* No duplicate registrations/style submissions. */
    for (unsigned i = 0; i < 7; ++i) {
        if (i == 4) {
            assert(!object_callbacks[i]);
            assert(native_get(objects[i].id, 0, 40u) == dst_id);
            continue;
        }
        unsigned expected = i > 4 ? i - 1 : i;
        assert(object_callbacks[i]);
        assert(native_get(objects[i].id, 0, 40u) == 0); /* Native default disabled. */
        uint32_t event[7] = {objects[i].id, objects[i].id, 24u, 0, 0x9900u, 0, 0};
        object_callbacks[i](event);
        assert(rectangle_draws == expected);
        event[2] = 25;
        object_callbacks[i](event);
        assert(rectangle_draws == expected + 1);
        event[1] = 0xdeadbeefu; /* Descendant bubbling to current target. */
        object_callbacks[i](event);
        assert(rectangle_draws == expected + 1);
        event[2] = 36;
        object_callbacks[i](event);
        assert(screen_background_find(objects[i].id));
        event[1] = objects[i].id;
        event[2] = 25;
        g_blur_updating = 1;
        object_callbacks[i](event);
        g_blur_updating = 0;
        assert(rectangle_draws == expected + 1);
    }
    delete_background_identity(5);
    assert(!screen_background_find(objects[5].id));
    objects[5].source = 0;
    fail_object_event = 1;
    refresh();
    assert(background_retry && !object_callbacks[5] && !objects[5].source);
    fail_object_event = 0;
    refresh();
    assert(object_callbacks[5] && screen_background_find(objects[5].id));
    struct rh_backgrounds system = {"", "", ""};
    assert(!rh_platform_backgrounds_commit(&system));
    for (unsigned i = 0; i < 7; ++i) {
        assert(!object_callbacks[i] && !screen_background_find(objects[i].id));
        assert(!objects[i].source);
    }
    /* Independent foreign source survives our temporary ownership. */
    objects[0].source = FOREIGN;
    bg_set(objects[0].id, 0x12345678u);
    bg_set(objects[0].id, 0);
    assert(objects[0].source == FOREIGN && !object_callbacks[0]);
    objects[0].source = 0;
    bg_set(objects[0].id, 0x12345678u);
    objects[0].source = FOREIGN; /* Native replacement during ownership. */
    assert(!rh_platform_backgrounds_commit(&system));
    assert(objects[0].source == FOREIGN && !object_callbacks[0]);
}
static void test_identity(unsigned kind) {
    test_zero_default();
    unsigned before = snapshots;
    if (kind < 2) delete_background_identity(4);
    if (kind == 0) objects[4].id += 0x1000u; /* delete/recreate at different address */
    else objects[4].source = kind == 1 ? 0 : FOREIGN; /* reused address/native style */
    refresh();
    assert(snapshots == before + 1 && !s_blur_needed);
    all_published();
    refresh();
    assert(snapshots == before + 1);
}
static void test_absent_then_visible(void) {
    select_blur();
    for (unsigned i = 0; i < 7; ++i) objects[i].present = 0;
    refresh();
    assert(!creates && !snapshots && !setters && !visibility_calls);
    objects[3].present = 1;
    refresh();
    assert(snapshots == 1 && setters == 1 && objects[3].source == dst_id);
    objects[3].present = 0;
    refresh();
    assert(snapshots == 1);
    objects[3].present = 1;
    refresh();
    assert(snapshots == 2 && objects[3].source == dst_id);
}
int main(int argc, char **argv) {
    assert(argc >= 2);
    setup();
    if (!strcmp(argv[1], "zero")) test_zero_default();
    else if (!strcmp(argv[1], "cold-commit")) test_cold_commit();
    else if (!strcmp(argv[1], "foreign-retry")) test_foreign_snapshot_retry();
    else if (!strcmp(argv[1], "alloc")) test_retry(1);
    else if (!strcmp(argv[1], "snapshot")) test_retry(0);
    else if (!strcmp(argv[1], "mixed")) {
        assert(argc == 3);
        unsigned type = (unsigned)atoi(argv[2]);
        assert(type < 3);
        test_mixed(type);
    }
    else if (!strcmp(argv[1], "invalid")) {
        assert(argc == 4);
        test_invalid(argv[2], argv[3]);
    } else if (!strcmp(argv[1], "callback")) test_callback_retry();
    else if (!strcmp(argv[1], "reentrant")) test_reentrant_callback();
    else if (!strcmp(argv[1], "result")) test_update_result();
    else if (!strcmp(argv[1], "reload")) test_reload();
    else if (!strcmp(argv[1], "system")) test_system();
    else if (!strcmp(argv[1], "system-after-blur")) test_system_after_blur();
    else if (!strcmp(argv[1], "poll-only")) test_poll_registration_only();
    else if (!strcmp(argv[1], "frame-order")) test_frame_order();
    else if (!strcmp(argv[1], "gpu")) {
        assert(argc == 4);
        test_gpu_failure(atoi(argv[2]), atoi(argv[3]));
    }
    else if (!strcmp(argv[1], "pending-no-visible")) { assert(argc == 3); test_pending_snapshot_without_visible_blur(atoi(argv[2])); }
    else if (!strcmp(argv[1], "pending-frame-abort")) test_failed_pending_release_aborts_refresh();
    else if (!strcmp(argv[1], "commit-frame")) { assert(argc == 4); test_commit_during_frame(atoi(argv[2]), atoi(argv[3])); }
    else if (!strcmp(argv[1], "switch-display-layout")) test_new_display_registration_during_layout_wakes_new_display();
    else if (!strcmp(argv[1], "system-rebuild-list")) { assert(argc == 3); test_system_commit_rediscovers_notification_list((unsigned)atoi(argv[2])); }
    else if (!strcmp(argv[1], "pending-watch")) { assert(argc == 4); test_pending_snapshot_watch_without_frames(atoi(argv[2]), atoi(argv[3])); }
    else if (!strcmp(argv[1], "pending-system")) test_pending_snapshot_system_maintenance();
    else if (!strcmp(argv[1], "event-filter")) test_event_filter();
    else if (!strcmp(argv[1], "registry-late")) test_registry_late();
    else if (!strcmp(argv[1], "registration-retry")) test_registration_retry();
    else if (!strcmp(argv[1], "registry-replaced")) test_registry_replaced();
    else if (!strcmp(argv[1], "registry-limit")) test_registry_limit();
    else if (!strcmp(argv[1], "registry-schema")) { assert(argc == 3); test_registry_schema((unsigned)atoi(argv[2])); }
    else if (!strcmp(argv[1], "registry-vanished")) test_registry_vanished(1);
    else if (!strcmp(argv[1], "registry-vanished-poll")) test_registry_vanished(0);
    else if (!strcmp(argv[1], "delete")) test_display_delete();
    else if (!strcmp(argv[1], "delete-frame")) { assert(argc == 4); test_delete_during_frame(atoi(argv[2]), atoi(argv[3])); }
    else if (!strcmp(argv[1], "retry-wake")) test_failed_frame_wakes_next_refresh();
    else if (!strcmp(argv[1], "visibility")) { assert(argc == 3); test_visibility((unsigned)atoi(argv[2])); }
    else if (!strcmp(argv[1], "cache-reopen")) test_cache_reopen();
    else if (!strcmp(argv[1], "screen-fixed")) test_screen_fixed();
    else if (!strcmp(argv[1], "identity")) { assert(argc == 3); test_identity((unsigned)atoi(argv[2])); }
    else if (!strcmp(argv[1], "absent-visible")) test_absent_then_visible();
    else assert(!"unknown case");
    assert(munmap(arena, MAP_SIZE) == 0);
    return 0;
}
'''


class BlurBackgroundNative(unittest.TestCase):
    def test_screen_fixed_all_receivers_and_callback_lifecycle(self):
        self.run_case('screen-fixed')

    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'src/platform.c').read_text()
        marker = '#if defined(RH_TARGET_177) && RH_TARGET_177\n'
        start = source.index(marker) + len(marker)
        end = source.index('\n#else\n', start)
        adapter = source[start:end]
        if 'int rh_platform_backgrounds_stage(' not in adapter:
            raise AssertionError('.177 backgrounds block not found')
        cls.tmp = tempfile.TemporaryDirectory(prefix='rh-blur-native-')
        cls.addClassCleanup(cls.tmp.cleanup)
        path = Path(cls.tmp.name)
        harness = path / 'test.c'
        harness.write_text(FIXTURE + adapter + CHECKS)
        cls.binary = path / 'test'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11',
                        '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined',
                        '-I' + str(ROOT / 'include'), str(harness),
                        # macOS arm64 forbids low-32-bit mappings. Use x86_64
                        # (Rosetta on Apple Silicon), with a small page zero.
                        *(['-arch', 'x86_64', '-Wl,-pagezero_size,0x10000']
                          if sys.platform == 'darwin' else []),
                        '-o', str(cls.binary)], check=True)

    def run_case(self, *args):
        subprocess.run([str(self.binary), *args], check=True)

    def test_poll_only_registers_one_display_callback(self):
        self.run_case('poll-only')

    def test_frame_pipeline_orders_fences_cache_pixels_and_publication_before_draw(self):
        self.run_case('frame-order')

    def test_gpu_failures_do_not_mutate_or_publish_and_pin_pending_snapshot(self):
        for after in (0, 1):
            for last_good in (0, 1):
                with self.subTest(after=after, last_good=last_good):
                    self.run_case('gpu', str(after), str(last_good))

    def test_pending_snapshot_is_retired_on_hidden_blur_or_image_only_refresh(self):
        for image in (0, 1):
            with self.subTest(image=image):
                self.run_case('pending-no-visible', str(image))

    def test_failed_pending_release_refresh_has_one_fence_no_snapshot_and_retries_next_frame(self):
        self.run_case('pending-frame-abort')

    def test_commit_during_layout_or_setter_aborts_old_epoch_and_defers_wake_to_ready(self):
        for setter in (0, 1):
            with self.subTest(setter=setter):
                self.run_case('commit-frame', str(setter), '0')

    def test_watch_flushes_reentrant_commit_wake_if_old_refresh_ready_was_missed(self):
        for setter in (0, 1):
            with self.subTest(setter=setter):
                self.run_case('commit-frame', str(setter), '1')

    def test_new_display_registered_during_old_layout_is_woken_immediately(self):
        self.run_case('switch-display-layout')

    def test_system_commit_rediscovers_list_after_notification_container_setter(self):
        for mode in range(3):
            with self.subTest(mode=mode):
                self.run_case('system-rebuild-list', str(mode))

    def test_watch_retires_pinned_snapshot_without_frames_even_for_images_or_bad_registry(self):
        for image in (0, 1):
            for invalid_registry in (0, 1):
                with self.subTest(image=image, invalid_registry=invalid_registry):
                    self.run_case('pending-watch', str(image), str(invalid_registry))

    def test_system_maintenance_keeps_pending_snapshot_until_successful_fence(self):
        self.run_case('pending-system')

    def test_only_native_refresh_start_on_registered_target_generates(self):
        self.run_case('event-filter')

    def test_missing_display_registry_membership_retries_later(self):
        self.run_case('registry-late')

    def test_failed_display_callback_registration_retries_without_pixels(self):
        self.run_case('registration-retry')

    def test_default_display_replacement_removes_live_old_callback(self):
        self.run_case('registry-replaced')

    def test_display_registry_traversal_has_four_member_limit(self):
        self.run_case('registry-limit')

    def test_invalid_registry_head_size_prev_links_or_tail_reject_registration(self):
        for kind in range(3):
            with self.subTest(kind=kind):
                self.run_case('registry-schema', str(kind))

    def test_nonmember_display_event_does_not_dereference_or_duplicate_on_recovery(self):
        self.run_case('registry-vanished')

    def test_poll_retains_unresolved_subscription_without_dereferencing_nonmember(self):
        self.run_case('registry-vanished-poll')

    def test_native_delete_resets_subscription_without_removing_freed_event_list(self):
        self.run_case('delete')

    def test_delete_during_layout_or_snapshot_aborts_old_frame_without_publication(self):
        for snapshot in (0, 1):
            for last_good in (0, 1):
                with self.subTest(snapshot=snapshot, last_good=last_good):
                    self.run_case('delete-frame', str(snapshot), str(last_good))

    def test_failed_visible_blur_frame_wakes_retry_only_at_refresh_ready(self):
        self.run_case('retry-wake')

    def test_hidden_ancestor_clipped_offscreen_and_inactive_screen_do_not_initialize(self):
        for kind in range(4):
            with self.subTest(kind=kind):
                self.run_case('visibility', str(kind))

    def test_cache_reopen_refreshes_without_cold_offscreen_blur(self):
        self.run_case('cache-reopen')

    def test_new_identity_and_recycled_native_style_refresh_visible_blur(self):
        for kind in range(3):
            with self.subTest(kind=kind):
                self.run_case('identity', str(kind))

    def test_absent_recipients_only_initialize_when_visible_and_reopen_dirties(self):
        self.run_case('absent-visible')

    def test_zero_default_initializes_all_seven_receivers_once(self):
        self.run_case('zero')

    def test_cold_commit_defers_initialization_until_display_refresh(self):
        self.run_case('cold-commit')

    def test_initial_snapshot_failure_preserves_foreign_styles_then_retries(self):
        self.run_case('foreign-retry')

    def test_allocation_failure_retries_once_per_refresh(self):
        self.run_case('alloc')

    def test_allocated_but_failed_snapshot_is_not_published(self):
        self.run_case('snapshot')

    def test_failed_blur_does_not_block_image_targets(self):
        for image_type in range(3):
            with self.subTest(image_type=image_type):
                self.run_case('mixed', str(image_type))

    def test_invalid_metadata_is_released_and_can_retry(self):
        for side in ('src', 'dst'):
            for field in ('pixels', 'odd-pixels', 'format', 'width', 'height',
                          'short-stride', 'odd-stride', 'size'):
                with self.subTest(side=side, field=field):
                    self.run_case('invalid', side, field)

    def test_callback_failure_retains_dirty_until_success(self):
        self.run_case('callback')

    def test_snapshot_reentrant_callback_does_not_acquire_again(self):
        self.run_case('reentrant')

    def test_blur_getter_only_returns_initialized_pixels_after_refresh_retry(self):
        self.run_case('result')

    def test_blur_reload_keeps_valid_pixels_and_marks_dirty(self):
        self.run_case('reload')

    def test_system_configuration_makes_no_blur_calls(self):
        self.run_case('system')

    def test_switch_to_system_does_not_refresh_from_lingering_callback(self):
        self.run_case('system-after-blur')


if __name__ == '__main__':
    unittest.main(verbosity=2)

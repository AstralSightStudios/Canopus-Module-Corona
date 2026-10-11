/* Actual .043 battery adapter through native seams, not device acceptance. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ROOT 0x20001000u
#define STATUS 0x20002000u
#define BATTERY 0x20003000u
#define OTHER 0x20004000u
#define DUPLICATE 0x20005000u
#define BUFFER 0x20006000u
#define ALLOCATION 0x20010001u
#define PIXELS 0x20010040u
#define LAYER 0x20008000u
#define STOCK_CLASS 0x2cd198dcu
#define PIXEL_CAPACITY (128u * 2048u)
struct object { uint32_t id, parent, words[26], extra; unsigned live; };
static struct object objects[5];
static uint32_t stock_class[9], buffer_words[7], children[17];
static uint32_t root_slot, status_slot, child_count;
static unsigned char image[PIXEL_CAPACITY];
static unsigned idle_ok, fence_ok, fence_fail_after, allocation_fail, snapshot_fail, alive;
static unsigned captures, allocations, destroys, fences, cleans, invalidates;
static unsigned native_events[64], image_draws, init_calls, live_calls, reads;
static unsigned forbid_reads, forbid_native_work, change_source_dims, delete_in_capture;
static unsigned malformed, change_capture_geometry, invalidate_in_capture;
static unsigned power_depth, power_stays, power_relaxes, delete_in_power;
static int p67_wake_pending;
static uint32_t draw_layer, last_descriptor[28];
static int32_t draw_area[4];
static uint32_t layer_clip[4];
static char operations[256];
static unsigned operation_count;
static uintptr_t native(uint32_t address);
static uint32_t *word_at(uint32_t address);
static unsigned char *byte_at(uint32_t address);
static uint32_t load32(uint32_t address);
static int p67_live(uint32_t obj);
static int p67_blur_native_ram(uint32_t address, uint32_t size);
static int p67_blur_native_idle(void);
static int p67_blur_native_fence(void);
static void p67_blur_native_power_begin(void);
static void p67_blur_native_power_end(void);
static void p67_blur_native_clean(void);
static void p67_blur_native_invalidate(uint32_t address, uint32_t size);
static void *pixels_at(uint32_t address);
#define P67_NATIVE(address) native(address)
#define P67_U32(address) (*word_at(address))
#define P67_U8(address) (*byte_at(address))
#define P67_BATTERY_TEST_PIXELS(address) pixels_at(address)
#include "../src/p67_battery.inc"

static void op(char value) {
    assert(operation_count < sizeof(operations)); operations[operation_count++] = value;
}
static struct object *object(uint32_t id) {
    for (unsigned i = 0; i < 5; ++i) if (objects[i].id == id) return &objects[i];
    assert(!"unknown object"); return NULL;
}
static uint32_t *word_at(uint32_t address) {
    assert(!forbid_reads); ++reads;
    for (unsigned i = 0; i < 5; ++i) {
        struct object *obj = &objects[i];
        if (address >= obj->id && address < obj->id + sizeof(obj->words)) {
            assert(obj->live && !((address - obj->id) & 3u));
            return &obj->words[(address - obj->id) / 4u];
        }
    }
    if (address >= STOCK_CLASS && address < STOCK_CLASS + sizeof(stock_class))
        return &stock_class[(address - STOCK_CLASS) / 4u];
    if (address >= BUFFER && address < BUFFER + sizeof(buffer_words)) {
        assert(alive); return &buffer_words[(address - BUFFER) / 4u];
    }
    /* The injected object header holds a truncated module RAM class address,
     * just as the native 32-bit target does. Resolve it without dereferencing
     * that truncated pointer on the 64-bit host. */
    uint32_t shim = p67_battery_shim();
    if (address >= shim && address - shim < sizeof(p67_battery_class))
        return &p67_battery_class[(address - shim) / 4u];
    assert(!"unsupported word read: no layer/framebuffer seam exists"); return NULL;
}
static unsigned char *byte_at(uint32_t address) {
    assert(!forbid_reads); ++reads;
    for (unsigned i = 0; i < 5; ++i) {
        struct object *obj = &objects[i];
        if (address >= obj->id && address < obj->id + sizeof(obj->words)) {
            assert(obj->live); return (unsigned char *)obj->words + address - obj->id;
        }
    }
    assert(!"unsupported byte read"); return NULL;
}
static uint32_t load32(uint32_t address) {
    assert(!forbid_reads);
    if (address == P67_BATTERY_ROOT_SLOT) return root_slot;
    if (address == P67_BATTERY_STATUS_SLOT) return status_slot;
    return *word_at(address);
}
static int p67_live(uint32_t id) {
    assert(!forbid_reads); ++live_calls;
    if (!id) return 0;
    return object(id)->live != 0;
}
static int p67_blur_native_ram(uint32_t address, uint32_t size) {
    if (address == BUFFER) return alive && size <= sizeof(buffer_words);
    return alive && address >= ALLOCATION && address <= PIXELS &&
           size <= PIXEL_CAPACITY + PIXELS - address;
}
static int p67_blur_native_idle(void) { assert(!forbid_native_work); return idle_ok != 0; }
static int p67_blur_native_fence(void) {
    assert(!forbid_native_work); ++fences; op('F');
    if (fence_fail_after && fences >= fence_fail_after) return 0;
    return fence_ok != 0;
}
static void p67_blur_native_power_begin(void) {
    assert(!forbid_native_work); ++power_depth; ++power_stays;
    if (delete_in_power && p67_battery_obj) {
        uint32_t id = p67_battery_obj, event[5] = {id, id, 36, 0, 0};
        forbid_reads = 1; p67_battery_event(p67_battery_shim(), event); forbid_reads = 0;
        object(id)->live = 0; child_count = 0;
    }
}
static void p67_blur_native_power_end(void) {
    assert(!forbid_native_work && power_depth && p67_battery_busy);
    --power_depth; ++power_relaxes;
}
static void p67_blur_native_clean(void) { assert(!forbid_native_work); ++cleans; op('C'); }
static void p67_blur_native_invalidate(uint32_t address, uint32_t size) {
    assert(!forbid_native_work && address == PIXELS && size == buffer_words[3]);
    assert(operation_count && operations[operation_count - 1] == 'C');
    ++invalidates; op('I');
}
static void *pixels_at(uint32_t address) { assert(address == PIXELS && alive); return image; }
static uint32_t parent(uint32_t id) { assert(!forbid_reads && object(id)->live); return object(id)->parent; }
static uint32_t count(uint32_t id) { assert(id == STATUS && object(id)->live); return child_count; }
static uint32_t child(uint32_t id, int32_t index) {
    assert(id == STATUS && index >= 0 && (uint32_t)index < child_count && index < 17);
    return children[index];
}
static uint32_t extra(uint32_t id) { assert(object(id)->live); return object(id)->extra; }
static void stock_event(uint32_t clz, void *event) {
    unsigned code = *(uint16_t *)((unsigned char *)event + 8) & 0x7fffu;
    assert(clz == STOCK_CLASS && code < 64u); ++native_events[code];
}
static void dispatch(uint32_t id, unsigned code, uint32_t layer) {
    uint32_t event[5] = { id, id, code, 0, layer };
    if (object(id)->words[0] == p67_battery_shim()) p67_battery_event(p67_battery_shim(), event);
    else stock_event(STOCK_CLASS, event);
}
static void image_init(void *data) {
    memset(data, 0, 28u * sizeof(uint32_t)); ++init_calls;
    ((uint32_t *)data)[5] = 112u;
    ((uint32_t *)data)[12] = ((uint32_t *)data)[13] = 256u;
}
static void image_submit(uint32_t layer, const void *descriptor, const int32_t *area) {
    assert(layer == LAYER); ++image_draws; draw_layer = layer;
    memcpy(last_descriptor, descriptor, sizeof(last_descriptor));
    memcpy(draw_area, area, sizeof(draw_area));
    assert(last_descriptor[7] == BUFFER && p67_battery_ready && !p67_battery_busy);
    assert(last_descriptor[5] == 112u && last_descriptor[12] == 256u && last_descriptor[13] == 256u);
}
static uint32_t create(uint32_t width, uint32_t height, uint32_t format, uint32_t stride) {
    assert(!forbid_native_work && !alive && format == 16u && stride == 0u);
    assert(width && width <= 128 && height && height <= 128);
    ++allocations; op('A');
    if (allocation_fail) return 0;
    alive = 1;
    buffer_words[0] = 25u | (16u << 8) | (0x30u << 16);
    buffer_words[1] = width | (height << 16);
    buffer_words[2] = (width * 4u + 15u) & ~15u;
    buffer_words[3] = buffer_words[2] * height;
    buffer_words[4] = PIXELS; buffer_words[5] = ALLOCATION; buffer_words[6] = 0x20103244u;
    assert(buffer_words[3] <= sizeof(image)); memset(image, 0xa5, sizeof(image));
    switch (malformed) {
    case 0: break;
    case 1: buffer_words[0] ^= 1u; break;
    case 2: buffer_words[1] ^= 1u; break;
    case 3: buffer_words[2] = width * 4u - 1u; break;
    case 4: buffer_words[3] = buffer_words[2] * height - 1u; break;
    case 5: buffer_words[4] = PIXELS + 1u; break;
    case 6: buffer_words[5] = PIXELS + 1u; break;
    case 7: buffer_words[6] = 0; break;
    case 8: buffer_words[3] = PIXEL_CAPACITY + 1u; break;
    default: assert(!"unknown malformed mode");
    }
    return BUFFER;
}
static void destroy(uint32_t buffer) {
    assert(!forbid_native_work && buffer == BUFFER && alive && idle_ok && fence_ok);
    alive = 0; ++destroys; op('D');
}
static int capture(uint32_t id, uint32_t format, uint32_t buffer) {
    assert(!forbid_native_work && object(id)->live && format == 16u && buffer == BUFFER && alive);
    assert(p67_battery_busy && !p67_battery_ready); ++captures; op('S');
    unsigned before = native_events[24], draws = image_draws;
    dispatch(id, 24, LAYER);
    assert(native_events[24] == before + 1 && image_draws == draws);
    unsigned width = buffer_words[1] & 0xffffu, height = buffer_words[1] >> 16;
    for (unsigned y = 0; y < height; ++y) {
        unsigned char *row = image + y * buffer_words[2];
        for (unsigned x = 0; x < width; ++x) {
            row[4*x] = row[4*x+1] = row[4*x+2] = 0; row[4*x+3] = 255;
        }
        /* White backing, colored charge and nonblack antialias pixels. */
        row[4] = row[5] = row[6] = row[7] = 255;
        row[8] = row[9] = 0; row[10] = row[11] = 255;
        row[12] = row[13] = row[14] = 1; row[15] = 127;
    }
    if (change_source_dims) { object(id)->words[15] = 62; object(id)->words[16] = 32; }
    if (change_capture_geometry) ++object(id)->words[9];
    if (delete_in_capture) {
        forbid_reads = forbid_native_work = 1; dispatch(id, 36, 0);
        forbid_reads = forbid_native_work = 0; object(id)->live = 0; child_count = 0;
    }
    if (invalidate_in_capture) p67_battery_invalidate();
    return snapshot_fail ? 0 : 1;
}
static uintptr_t native(uint32_t address) {
    switch (address) {
    case 0x0c589579u: return (uintptr_t)parent;
    case 0x0c5881d9u: return (uintptr_t)count;
    case 0x0c5886e1u: return (uintptr_t)child;
    case 0x0c587e71u: return (uintptr_t)extra;
    case P67_BATTERY_EVENT: return (uintptr_t)stock_event;
    case 0x0c5882b9u: return (uintptr_t)image_init;
    case 0x0c5882a1u: return (uintptr_t)image_submit;
    case 0x0c587f39u: return (uintptr_t)create;
    case 0x0cac0db9u: return (uintptr_t)destroy;
    case 0x0ca5fb41u: return (uintptr_t)capture;
    default: assert(!"unexpected native API"); return 0;
    }
}
static void geometry(uint32_t id, int32_t x, int32_t y, unsigned width, unsigned height, unsigned padding) {
    struct object *obj = object(id);
    obj->words[7] = (uint32_t)x; obj->words[8] = (uint32_t)y;
    obj->words[9] = (uint32_t)(x + (int32_t)width - 1);
    obj->words[10] = (uint32_t)(y + (int32_t)height - 1); obj->extra = padding;
}
static void reset(void) {
    const uint32_t ids[5] = {ROOT, STATUS, BATTERY, OTHER, DUPLICATE};
    memset(objects, 0, sizeof(objects));
    for (unsigned i = 0; i < 5; ++i) { objects[i].id = ids[i]; objects[i].live = 1; }
    object(STATUS)->parent = ROOT;
    object(BATTERY)->parent = object(OTHER)->parent = object(DUPLICATE)->parent = STATUS;
    object(BATTERY)->words[0] = object(DUPLICATE)->words[0] = STOCK_CLASS;
    object(OTHER)->words[0] = 0x2cce61ecu;
    geometry(BATTERY, 100, 20, 62, 32, 30);
    geometry(DUPLICATE, 0, 0, 62, 32, 30);
    object(BATTERY)->words[15] = object(BATTERY)->words[16] = UINT32_MAX;
    object(BATTERY)->words[20] = 50;
    for (unsigned i = 0; i < 9; ++i) stock_class[i] = 0x100u + i;
    stock_class[0] = 0x2cce0308u; stock_class[3] = P67_BATTERY_EVENT;
    stock_class[6] = 62; stock_class[7] = 32; stock_class[8] = 0x680;
    root_slot = ROOT; status_slot = STATUS; child_count = 3;
    children[0] = OTHER; children[1] = BATTERY; children[2] = 0;
    idle_ok = fence_ok = 1;
    alive = fence_fail_after = allocation_fail = snapshot_fail = malformed = change_capture_geometry = 0;
    captures = allocations = destroys = fences = cleans = invalidates = 0;
    image_draws = init_calls = live_calls = reads = 0;
    forbid_reads = forbid_native_work = delete_in_capture = invalidate_in_capture = 0;
    change_source_dims = 1; operation_count = 0;
    power_depth = power_stays = power_relaxes = delete_in_power = 0;
    memset(native_events, 0, sizeof(native_events));
    memset(p67_battery_class, 0, sizeof(p67_battery_class));
    memset(&p67_battery_cached, 0, sizeof(p67_battery_cached));
    p67_battery_obj = p67_battery_root = p67_battery_status = 0;
    p67_battery_generation = p67_battery_buffer = 0;
    p67_battery_busy = p67_battery_ready = 0; p67_wake_pending = 0;
    layer_clip[0] = 0; layer_clip[1] = 0; layer_clip[2] = 335; layer_clip[3] = 479;
}
static void ready(void) {
    p67_battery_poll(1);
    assert(p67_battery_ready && alive && p67_battery_obj == BATTERY && p67_wake_pending);
    assert(!power_depth && power_stays == power_relaxes);
}
static void draw_without_native_work(unsigned code) {
    unsigned f = fences, a = allocations, s = captures, l = live_calls;
    uint32_t clip[4]; memcpy(clip, layer_clip, sizeof(clip));
    forbid_native_work = 1; dispatch(BATTERY, code, LAYER); forbid_native_work = 0;
    assert(f == fences && a == allocations && s == captures && l == live_calls);
    assert(!memcmp(clip, layer_clip, sizeof(clip)));
}
static void test_capture_and_scroll(void) {
    reset(); ready();
    assert(allocations == 1 && captures == 1 && fences == 2 && invalidates == 1);
    assert(buffer_words[1] == (92u << 16 | 122u));
    assert(operation_count == 8 && !memcmp(operations, "FACSFCIC", 8));
    assert(p67_battery_class[0] == STOCK_CLASS && !p67_battery_class[1] && !p67_battery_class[2]);
    assert(p67_battery_class[3] == (uint32_t)(uintptr_t)p67_battery_event);
    for (unsigned i = 4; i < 9; ++i) assert(p67_battery_class[i] == stock_class[i]);
    assert(object(BATTERY)->words[0] == p67_battery_shim());
    assert(p67_battery_cached.state[8] == 62 && p67_battery_cached.state[12] == 32);
    for (unsigned y = 0; y < 92; ++y) {
        const unsigned char *row = image + y * buffer_words[2];
        assert(!row[0] && !row[1] && !row[2] && !row[3]);
        assert(row[4] == 255 && row[5] == 255 && row[6] == 255 && row[7] == 255);
        assert(!row[8] && !row[9] && row[10] == 255 && row[11] == 255);
        assert(row[12] == 1 && row[13] == 1 && row[14] == 1 && row[15] == 127);
        for (unsigned i = 122u * 4u; i < buffer_words[2]; ++i) assert(row[i] == 0xa5);
    }
    draw_without_native_work(24);
    assert(image_draws == 1 && draw_layer == LAYER);
    assert(draw_area[0] == 70 && draw_area[1] == -10 && draw_area[2] == 191 && draw_area[3] == 81);
    unsigned native = native_events[24];
    geometry(BATTERY, -200, -15, 62, 32, 30);
    object(BATTERY)->words[18] ^= 0xf00u; object(BATTERY)->words[19] ^= 0x1234u;
    draw_without_native_work(24);
    assert(image_draws == 2 && native_events[24] == native);
    assert(draw_area[0] == -230 && draw_area[1] == -45 && draw_area[2] == -109 && draw_area[3] == 46);
    p67_battery_poll(1); assert(captures == 1 && allocations == 1 && fences == 2);
    draw_without_native_work(22); assert(native_events[22] == 1 && image_draws == 2);
    uint32_t event[5] = {BATTERY, BATTERY, 24, 0, 0};
    p67_battery_event(p67_battery_shim(), event); assert(native_events[24] == native + 1);
}
static void test_state_and_geometry(void) {
    const unsigned offsets[] = {52, 56, 68, 80, 81, 85, 89, 93, 97, 101};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        reset(); ready(); *byte_at(BATTERY + offsets[i]) ^= 1u;
        unsigned native = native_events[24]; draw_without_native_work(24);
        assert(image_draws == 0 && native_events[24] == native + 1);
        p67_battery_poll(1); assert(captures == 2 && p67_battery_ready);
        draw_without_native_work(24); assert(image_draws == 1);
    }
    reset(); ready(); geometry(BATTERY, 10, 10, 60, 30, 30);
    draw_without_native_work(24); assert(image_draws == 0);
    p67_battery_poll(1); assert(captures == 2 && allocations == 2 && destroys == 1 && p67_battery_ready);
    assert(buffer_words[1] == (90u << 16 | 120u));
    reset(); change_capture_geometry = 1; p67_battery_poll(1);
    assert(captures == 1 && !p67_battery_ready && invalidates == 0);
    change_capture_geometry = 0; p67_battery_poll(1); assert(p67_battery_ready && captures == 2);
}
static void test_resource_invalidation(void) {
    reset(); ready();
    unsigned f = fences, d = destroys, r = reads;
    uint32_t generation = p67_battery_generation;
    forbid_reads = forbid_native_work = 1; p67_battery_invalidate();
    forbid_reads = forbid_native_work = 0;
    assert(!p67_battery_ready && p67_battery_generation != generation);
    assert(f == fences && d == destroys && r == reads && alive);
    draw_without_native_work(24); assert(!image_draws);
    ready(); assert(captures == 2 && allocations == 1 && !destroys);
    draw_without_native_work(24); assert(image_draws == 1);
    reset(); invalidate_in_capture = 1; p67_battery_poll(1);
    assert(captures == 1 && !p67_battery_ready && !invalidates && alive);
    invalidate_in_capture = 0; ready(); assert(captures == 2 && allocations == 1);
    reset(); ready(); draw_without_native_work(45);
    assert(!p67_battery_ready && native_events[45] == 1 && !destroys);
    draw_without_native_work(24); assert(!image_draws);
    ready(); assert(captures == 2 && allocations == 1);
}
static void test_disable_and_delete(void) {
    reset(); ready(); idle_ok = 0;
    p67_battery_poll(0);
    assert(object(BATTERY)->words[0] == STOCK_CLASS && !p67_battery_obj && !p67_battery_ready);
    assert(alive && destroys == 0 && fences == 2);
    idle_ok = 1; fence_ok = 0; p67_battery_poll(0); assert(alive && destroys == 0);
    fence_ok = 1; p67_battery_poll(0); assert(!alive && destroys == 1);
    reset(); ready(); uint32_t header = object(BATTERY)->words[0];
    unsigned f = fences, d = destroys, r = reads, l = live_calls;
    forbid_reads = forbid_native_work = 1; dispatch(BATTERY, 36, 0);
    forbid_reads = forbid_native_work = 0;
    assert(!p67_battery_obj && !p67_battery_ready && native_events[36] == 1);
    assert(header == object(BATTERY)->words[0] && f == fences && d == destroys && r == reads && l == live_calls);
    object(BATTERY)->live = 0; child_count = 0; idle_ok = 0;
    p67_battery_poll(1); assert(alive && destroys == 0);
    idle_ok = 1; p67_battery_poll(1); assert(!alive && destroys == 1);
    /* The native allocator may recycle the same object address. */
    object(BATTERY)->live = 1; object(BATTERY)->words[0] = STOCK_CLASS;
    children[0] = BATTERY; child_count = 1; ready();
    assert(captures == 2 && allocations == 2); draw_without_native_work(24); assert(image_draws == 1);
    /* Recycle before buffer maintenance: the old pixels must not survive the
     * deleted binding even when the new instance has an identical cache key. */
    reset(); ready(); dispatch(BATTERY, 36, 0);
    object(BATTERY)->words[0] = STOCK_CLASS;
    draw_without_native_work(24); assert(!image_draws);
    ready(); assert(captures == 2 && allocations == 1 && !destroys);
    draw_without_native_work(24); assert(image_draws == 1);
    reset(); delete_in_capture = 1; p67_battery_poll(1);
    assert(captures == 1 && !p67_battery_obj && !p67_battery_ready && alive && destroys == 0);
    delete_in_capture = 0; p67_battery_poll(1); assert(!alive && destroys == 1);
}
static void test_failures(void) {
    reset(); allocation_fail = 1; p67_battery_poll(1);
    assert(!alive && !p67_battery_ready && allocations == 1 && !captures);
    draw_without_native_work(24); assert(native_events[24] == 1);
    allocation_fail = 0; ready(); assert(allocations == 2 && captures == 1);
    reset(); snapshot_fail = 1; p67_battery_poll(1);
    assert(alive && !p67_battery_ready && captures == 1 && !invalidates);
    draw_without_native_work(24); assert(native_events[24] == 2);
    snapshot_fail = 0; ready(); assert(captures == 2 && allocations == 1);
    reset(); fence_ok = 0; p67_battery_poll(1);
    assert(!p67_battery_ready && !allocations && !captures);
    fence_ok = 1; ready();
    ++*byte_at(BATTERY + 80); fence_ok = 0; p67_battery_poll(1);
    assert(alive && !p67_battery_ready && captures == 1 && !destroys);
    draw_without_native_work(24); assert(image_draws == 0);
    fence_ok = 1; ready(); assert(captures == 2 && allocations == 1);
    /* Failed completion after a snapshot pins storage too. */
    reset(); fence_fail_after = 2; p67_battery_poll(1);
    assert(alive && captures == 1 && !p67_battery_ready && !invalidates);
    p67_battery_poll(0); assert(alive && !destroys);
    fence_fail_after = 0; p67_battery_poll(0); assert(!alive && destroys == 1);
    assert(!power_depth && power_stays == power_relaxes);
    /* Native PM transitions may invalidate a borrowed UI identity. */
    reset(); delete_in_power = 1; p67_battery_poll(1);
    assert(!captures && !allocations && !p67_battery_obj);
    assert(!power_depth && power_stays == 1 && power_relaxes == 1);
}
static void test_binding_and_invalid_inputs(void) {
    reset(); children[2] = DUPLICATE; p67_battery_poll(1);
    assert(!p67_battery_obj && !allocations && object(BATTERY)->words[0] == STOCK_CLASS);
    reset(); ready(); children[2] = DUPLICATE; p67_battery_poll(1);
    assert(!p67_battery_obj && !p67_battery_ready && destroys == 1);
    assert(object(BATTERY)->words[0] == STOCK_CLASS);
    reset(); child_count = 17; p67_battery_poll(1); assert(!allocations);
    reset(); object(STATUS)->parent = OTHER; p67_battery_poll(1); assert(!allocations);
    reset(); status_slot = 0; p67_battery_poll(1); assert(!allocations);
    reset(); ready(); root_slot = OTHER;
    draw_without_native_work(24); assert(!image_draws);
    p67_battery_poll(1); assert(!p67_battery_obj && object(BATTERY)->words[0] == STOCK_CLASS && destroys == 1);
    for (unsigned mode = 1; mode <= 8; ++mode) {
        reset(); malformed = mode; p67_battery_poll(1);
        assert(!p67_battery_ready && !captures && !invalidates);
        draw_without_native_work(24); assert(native_events[24] == 1);
        malformed = 0; ready(); assert(allocations == 2 && destroys == 1);
    }
    const int32_t invalid[][5] = {
        {0,0,0,32,30}, {0,0,129,32,0}, {0,0,62,129,0},
        {0,0,62,32,64}, {0,0,62,32,34}, {-4097,0,62,32,30}, {4096,0,62,32,30}
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset(); geometry(BATTERY, invalid[i][0], invalid[i][1], (unsigned)invalid[i][2],
                          (unsigned)invalid[i][3], (unsigned)invalid[i][4]);
        p67_battery_poll(1); assert(!p67_battery_ready && !allocations && !captures);
    }
}
int main(void) {
    test_capture_and_scroll(); test_state_and_geometry(); test_resource_invalidation();
    test_disable_and_delete();
    test_failures(); test_binding_and_invalid_inputs();
    puts("p67 battery native lifecycle/capture/scroll/fallback: ok (host seams only)");
    return 0;
}

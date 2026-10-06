"""Run the actual .177 launcher-background block with host native-call mocks.

Source is extracted verbatim, never edited. This checks the adapter's requests
and lifecycle, not firmware execution or the separately tested cache internals.
Firmware uint32_t style pointers are compared with consistently truncated host
addresses; they are never converted back into host pointers for dereferencing.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "resource_hook.h"
#include "resource_hook_background.h"
#include "resource_hook_target_177.h"

#define HEADER 0x1000u
#define IMAGE 0x1100u
#define A "/data/background/a.bin"
#define B "/data/background/b.bin"
#define FOREIGN 0x76543210u
struct object { uint32_t id, clz, source; unsigned sets; };
struct root { uint32_t id, children[3]; unsigned count; };
struct retirement { uint32_t cache, offset; char exact[RH_PATH]; };
static struct object objects[15];
static struct root roots[5];
static uint32_t slots[4], header_cache = HEADER, image_cache = IMAGE;
static unsigned reads, cache_reads, child_calls, style_gets, sets, infos, validations;
static struct retirement retired[64];
static unsigned retirements;
static uint32_t bad_cache, fail_retire;
static int info_result = 1;
static char expected_info[RH_PATH], events[128];
static unsigned event_count;
static const char *expected_clear_text;
static const char *path_for_source(uint32_t source);

static uint32_t pointer32(const void *p) { return (uint32_t)(uintptr_t)p; }
static void event(char c) {
    assert(event_count + 1 < sizeof(events));
    events[event_count++] = c;
    events[event_count] = 0;
}
static struct object *object(uint32_t id) {
    for (unsigned i = 0; i < 15; ++i) if (objects[i].id == id) return &objects[i];
    assert(!"unexpected object address");
    return 0;
}
static uint32_t load32(uint32_t address) {
    ++reads;
    switch (address) {
    case RH_LB_MAIN_ROOT_SLOT: return slots[0];
    case RH_LB_MAIN_CACHE_SLOT: return slots[1];
    case RH_LB_MORE_ROOT_SLOT: return slots[2];
    case RH_LB_MORE_CACHE_SLOT: return slots[3];
    case RH_LB_CONTROL_PRESENTER_SLOT: return 0;
    case RH_LB_NOTIFY_CONTAINER_SLOT: return 0;
    case RH_LB_NOTIFY_LIST_SLOT: return 0;
    case RH_FW_HEADER_CACHE_SLOT: ++cache_reads; return header_cache;
    case RH_FW_IMAGE_CACHE_SLOT: ++cache_reads; return image_cache;
    default: return object(address)->clz;
    }
}
static uint32_t native_child(uint32_t root, int index) {
    ++child_calls;
    assert(index == 0); /* No walk, insertion, reordering or extra-child lookup. */
    for (unsigned i = 0; i < 5; ++i) {
        if (roots[i].id == root) return roots[i].children[0];
    }
    assert(!"unexpected root");
    return 0;
}
static uint32_t native_get(uint32_t list, uint32_t part, uint32_t property) {
    ++style_gets;
    assert(part == 0 && property == 40u);
    assert(object(list)->clz == RH_LB_LIST_CLASS);
    return object(list)->source;
}
static void native_set(uint32_t list, uint32_t property, uint32_t source,
                       uint32_t selector) {
    struct object *o = object(list);
    assert(o->clz == RH_LB_LIST_CLASS);
    assert(property == 40u && selector == 0);
    if (source) assert(path_for_source(source));
    if (!source && expected_clear_text) {
        const char *old = path_for_source(o->source);
        assert(old && !strcmp(old, expected_clear_text));
    }
    o->source = source;
    ++o->sets;
    ++sets;
}
static int cache_valid(uint32_t cache, uint32_t clz) {
    ++validations;
    event('V');
    assert((cache == HEADER && clz == RH_FW_HEADER_CACHE_CLASS) ||
           (cache == IMAGE && clz == RH_FW_IMAGE_CACHE_CLASS));
    return cache != bad_cache;
}
static int retire_cache(uint32_t cache, uint32_t offset,
                        const struct rh_mapping_view *mapping, const char *exact) {
    struct retirement *r;
    event('R');
    assert(mapping == 0 && exact && exact[0]); /* Exact key, never global/mapped. */
    assert((cache == HEADER && offset == 0u) || (cache == IMAGE && offset == 4u));
    assert(retirements < 64);
    r = &retired[retirements++];
    r->cache = cache;
    r->offset = offset;
    snprintf(r->exact, sizeof(r->exact), "%s", exact);
    return cache == fail_retire ? -1 : 0;
}
static int native_info(const char *path, void *out) {
    uint32_t *header = out;
    ++infos;
    event('I');
    assert(!strcmp(path, expected_info));
    assert(header[0] == 0 && header[1] == 0 && header[2] == 0);
    header[0] = 0x1234u; header[1] = 320u; header[2] = 400u;
    return info_result;
}
#undef RH_LB_CHILD_GET
#undef RH_LB_STYLE_SET
#undef RH_FW_OBJECT_STYLE_GET
#undef RH_FW_IMAGE_GET_INFO
#define RH_LB_CHILD_GET ((uintptr_t)native_child)
#define RH_LB_STYLE_SET ((uintptr_t)native_set)
#define RH_FW_OBJECT_STYLE_GET ((uintptr_t)native_get)
#define RH_FW_IMAGE_GET_INFO ((uintptr_t)native_info)
'''

CHECKS = r'''
static const char *path_for_source(uint32_t source) {
    for (unsigned i = 0; i < 2; ++i)
        if (source == pointer32(launcher_paths[i])) return launcher_paths[i];
    return 0;
}
static uint32_t current_source(void) {
    return pointer32(launcher_paths[launcher_path_index]);
}
static void setup(void) {
    assert(pointer32(launcher_paths[0]) != pointer32(launcher_paths[1]));
    assert(pointer32(launcher_paths[0]) && pointer32(launcher_paths[1]));
    assert(!path_for_source(FOREIGN));
    for (unsigned i = 0; i < 5; ++i) {
        roots[i].id = 0x2000u + i * 0x100u;
        roots[i].count = 3;
        for (unsigned j = 0; j < 3; ++j) {
            unsigned n = i * 3 + j;
            objects[n].id = 0x3000u + n * 0x100u;
            objects[n].clz = RH_LB_LIST_CLASS;
            objects[n].source = FOREIGN;
            roots[i].children[j] = objects[n].id;
        }
        if (i < 4) slots[i] = roots[i].id;
    }
}
/* All three entry points must preserve every child and its order. Sentinel
 * children deliberately have the accepted class, so an accidental walk fails. */
static void tree_unchanged(const struct root *before) {
    assert(!memcmp(before, roots, sizeof(roots)));
    for (unsigned i = 0; i < 5; ++i)
        for (unsigned j = 1; j < 3; ++j) {
            struct object *o = object(roots[i].children[j]);
            assert(o->source == FOREIGN && o->sets == 0);
        }
}
static int stage(const char *path) {
    struct root before[5];
    unsigned old_sets = sets, old_gets = style_gets, old_children = child_calls;
    memcpy(before, roots, sizeof(roots));
    snprintf(expected_info, sizeof(expected_info), "%s", path);
    int result = rh_platform_launcher_background_stage(path);
    assert(sets == old_sets && style_gets == old_gets && child_calls == old_children);
    tree_unchanged(before);
    return result;
}
static int commit(const char *path) {
    struct root before[5];
    unsigned old_infos = infos;
    memcpy(before, roots, sizeof(roots));
    int result = rh_platform_launcher_background_commit(path);
    assert(infos == old_infos);
    tree_unchanged(before);
    return result;
}
static void poll(void) {
    struct root before[5];
    unsigned old_infos = infos, old_cache_reads = cache_reads;
    unsigned old_validations = validations, old_retirements = retirements;
    memcpy(before, roots, sizeof(roots));
    rh_platform_launcher_background_poll();
    assert(infos == old_infos && cache_reads == old_cache_reads);
    assert(validations == old_validations && retirements == old_retirements);
    tree_unchanged(before);
}
static void exact(unsigned n, uint32_t cache, uint32_t offset, const char *path) {
    assert(n < retirements);
    assert(retired[n].cache == cache && retired[n].offset == offset);
    assert(!strcmp(retired[n].exact, path));
}
static void all_current(void) {
    for (unsigned i = 0; i < 4; ++i)
        assert(object(roots[i].children[0])->source == current_source());
}
static void activate(const char *path) {
    assert(stage(path) == 0);
    assert(commit(path) == 0);
}

static void test_stage_once(void) {
    unsigned old_reads = reads;
    poll();
    assert(reads == old_reads);
    assert(stage(A) == 0);
    assert(infos == 1 && validations == 2 && retirements == 2);
    assert(!strcmp(events, "VRVRI")); /* Retire before checking the header. */
    exact(0, HEADER, 0, A); exact(1, IMAGE, 4, A);
    poll(); /* Staging alone must not publish. */
    assert(sets == 0 && reads == old_reads + 2);
    assert(commit(A) == 0);
    all_current();
    assert(sets == 4 && style_gets == 8 && child_calls == 8);
    assert(infos == 1 && retirements == 2);
    unsigned old_sets = sets;
    for (unsigned i = 0; i < 100; ++i) poll();
    assert(sets == old_sets && infos == 1 && retirements == 2);
}
static void test_exact_reload(void) {
    activate(A);
    assert(stage(B) == 0);
    exact(2, HEADER, 0, B); exact(3, IMAGE, 4, B);
    assert(commit(B) == 0);
    exact(4, HEADER, 0, A); exact(5, IMAGE, 4, A);
    assert(infos == 2 && retirements == 6);
    all_current();
    assert(stage(B) == 0); /* Same filename can contain different bytes. */
    assert(commit(B) == 0);
    exact(6, HEADER, 0, B); exact(7, IMAGE, 4, B);
    exact(8, HEADER, 0, B); exact(9, IMAGE, 4, B);
    assert(infos == 3 && retirements == 10);
    all_current();
    poll();
    assert(infos == 3 && retirements == 10);
}
static void test_exact_class(void) {
    objects[0].clz = RH_FW_IMAGE_OBJECT_CLASS;
    objects[3].clz = RH_LB_LIST_CLASS + 4u;
    roots[2].children[0] = 0;
    slots[3] = 0;
    activate(A);
    assert(sets == 0 && style_gets == 0 && child_calls == 6);
    poll();
    assert(sets == 0 && style_gets == 0 && child_calls == 9);
    objects[0].clz = RH_LB_LIST_CLASS;
    poll();
    assert(sets == 1 && style_gets == 1);
    assert(objects[0].source == current_source());
    objects[3].clz = RH_LB_LIST_CLASS - 4u;
    poll();
    assert(sets == 1 && objects[3].source == FOREIGN);
    objects[3].clz = RH_LB_LIST_CLASS;
    roots[2].children[0] = objects[6].id;
    slots[3] = roots[3].id;
    poll();
    assert(sets == 4);
    all_current();
}
static void test_system_ownership(void) {
    activate(A);
    uint32_t first = current_source();
    activate(B);
    uint32_t second = current_source();
    assert(first != second);
    objects[0].source = first; /* Stale pointer to our inactive buffer. */
    objects[3].source = second;
    objects[6].source = FOREIGN; /* Native/other owner's background. */
    objects[9].source = 0;
    unsigned old_reads = reads, old_infos = infos, old_sets = sets;
    assert(stage("") == 0);
    assert(reads == old_reads && infos == old_infos);
    assert(commit("") == 0);
    assert(sets == old_sets + 2);
    assert(objects[0].source == 0 && objects[3].source == 0);
    assert(objects[6].source == FOREIGN && objects[9].source == 0);
    exact(6, HEADER, 0, B); exact(7, IMAGE, 4, B);
    assert(retirements == 8 && infos == old_infos);
    old_reads = reads;
    for (unsigned i = 0; i < 10; ++i) poll();
    assert(reads == old_reads && sets == old_sets + 2);
    unsigned old_retirements = retirements;
    assert(commit("") == 0); /* Already system: no old pathname to retire. */
    assert(retirements == old_retirements && sets == old_sets + 2);
    assert(objects[6].source == FOREIGN);
}
static void test_stable_storage(void) {
    char input[RH_PATH] = A;
    activate(input);
    uint32_t first = current_source();
    assert(first != pointer32(input));
    memset(input, 'x', sizeof(input));
    assert(!strcmp(path_for_source(first), A));
    poll();
    assert(stage(B) == 0);
    assert(!strcmp(path_for_source(first), A));
    expected_clear_text = A;
    assert(commit(B) == 0); /* Old path must remain intact until detached. */
    uint32_t second = current_source();
    assert(first != second);
    assert(!strcmp(path_for_source(first), A));
    assert(!strcmp(path_for_source(second), B));
    expected_clear_text = B;
    assert(stage(A) == 0);
    assert(commit(A) == 0);
    assert(current_source() == first); /* Alternates, does not allocate/borrow. */
    assert(!strcmp(path_for_source(second), B));
    all_current();
    unsigned old_sets = sets;
    poll();
    assert(sets == old_sets);
}
static void test_reuse(void) {
    activate(A);
    unsigned old_sets = sets;
    slots[0] = 0;
    poll();
    assert(sets == old_sets);
    slots[0] = roots[4].id; /* Newly constructed main list. */
    poll();
    assert(sets == old_sets + 1);
    assert(objects[12].source == current_source());
    objects[0].source = FOREIGN; /* Different object, same old list address. */
    slots[0] = roots[0].id;
    poll();
    assert(sets == old_sets + 2 && objects[0].source == current_source());
    objects[0].clz = RH_FW_IMAGE_OBJECT_CLASS;
    objects[0].source = FOREIGN;
    poll();
    assert(sets == old_sets + 2 && objects[0].source == FOREIGN);
    objects[0].clz = RH_LB_LIST_CLASS;
    poll();
    assert(sets == old_sets + 3 && objects[0].source == current_source());
    objects[0].source = FOREIGN;
    slots[1] = slots[0]; /* Active and cached slots may alias the same owner. */
    poll();
    assert(sets == old_sets + 4); /* Getter prevents a duplicate setter. */
    poll();
    assert(sets == old_sets + 4);
}
static void test_invalid_preflight(void) {
    activate(A);
    unsigned old_sets = sets;
    uint32_t old_source = current_source();
    for (int result = -1; result <= 2; ++result) {
        if (result == 1) continue;
        info_result = result;
        unsigned old_infos = infos, old_retirements = retirements;
        assert(stage(B) == -2143);
        assert(infos == old_infos + 1 && retirements == old_retirements + 2);
        exact(old_retirements, HEADER, 0, B);
        exact(old_retirements + 1, IMAGE, 4, B);
        assert(current_source() == old_source && sets == old_sets);
        assert(!strcmp(path_for_source(old_source), A));
        all_current(); /* Failed stage leaves the published style/path intact. */
        poll();
        assert(sets == old_sets);
    }
    bad_cache = HEADER;
    unsigned old_infos = infos, old_retirements = retirements;
    assert(stage(B) == -2145);
    assert(infos == old_infos && retirements == old_retirements);
    bad_cache = IMAGE;
    assert(stage(B) == -2145);
    assert(infos == old_infos && retirements == old_retirements + 1);
    exact(old_retirements, HEADER, 0, B);
    bad_cache = 0;
    fail_retire = HEADER;
    old_retirements = retirements;
    assert(stage(B) == -2145);
    assert(infos == old_infos && retirements == old_retirements + 1);
    fail_retire = IMAGE;
    old_retirements = retirements;
    assert(stage(B) == -2145);
    assert(infos == old_infos && retirements == old_retirements + 2);
    assert(sets == old_sets && current_source() == old_source);
    assert(!strcmp(path_for_source(old_source), A));
    all_current();
    poll();
    assert(sets == old_sets);
}
static void test_invalid_cache_system_commit(uint32_t invalid_cache) {
    activate(A);
    unsigned old_index = launcher_path_index, old_sets = sets;
    unsigned old_infos = infos, old_validations = validations;
    unsigned old_retirements = retirements;
    uint32_t old_source = current_source();
    assert(stage("") == 0); /* System staging does not inspect the old cache. */
    bad_cache = invalid_cache;
    /* A nonnull invalid cache must fail, not return success for the caller's
     * configuration acknowledgement, even though our styles were detached. */
    assert(commit("") == -2145);
    assert(infos == old_infos && sets == old_sets + 4);
    assert(launcher_background_active == 0);
    assert(launcher_path_index == old_index && current_source() == old_source);
    assert(!strcmp(path_for_source(old_source), A));
    for (unsigned i = 0; i < 4; ++i)
        assert(object(roots[i].children[0])->source == 0);
    if (invalid_cache == HEADER) {
        assert(validations == old_validations + 1);
        assert(retirements == old_retirements); /* Fail before either retirement. */
    } else {
        assert(invalid_cache == IMAGE);
        assert(validations == old_validations + 2);
        assert(retirements == old_retirements + 1);
        exact(old_retirements, HEADER, 0, A);
    }
    unsigned old_reads = reads;
    poll();
    assert(reads == old_reads && sets == old_sets + 4);
    /* Repair and retry: the rejected commit retained the old key for cleanup. */
    bad_cache = 0;
    old_retirements = retirements;
    assert(commit("") == 0);
    assert(retirements == old_retirements + 2);
    exact(old_retirements, HEADER, 0, A);
    exact(old_retirements + 1, IMAGE, 4, A);
    assert(launcher_paths[launcher_path_index][0] == 0);
    assert(infos == old_infos && sets == old_sets + 4);
}
static void test_missing_caches(void) {
    header_cache = image_cache = 0;
    activate(A);
    assert(infos == 1 && validations == 0 && retirements == 0);
    all_current();
    poll();
    assert(commit("") == 0);
    assert(infos == 1 && validations == 0 && retirements == 0);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "stage")) test_stage_once();
    else if (!strcmp(argv[1], "reload")) test_exact_reload();
    else if (!strcmp(argv[1], "class")) test_exact_class();
    else if (!strcmp(argv[1], "system")) test_system_ownership();
    else if (!strcmp(argv[1], "storage")) test_stable_storage();
    else if (!strcmp(argv[1], "reuse")) test_reuse();
    else if (!strcmp(argv[1], "invalid")) test_invalid_preflight();
    else if (!strcmp(argv[1], "system-invalid-header")) test_invalid_cache_system_commit(HEADER);
    else if (!strcmp(argv[1], "system-invalid-image")) test_invalid_cache_system_commit(IMAGE);
    else if (!strcmp(argv[1], "missing")) test_missing_caches();
    else assert(!"unknown test case");
    return 0;
}
'''


class LauncherBackgroundNative(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'src/platform.c').read_text()
        marker = '#if defined(RH_TARGET_177) && RH_TARGET_177\n'
        start = source.index(marker) + len(marker)
        end = source.index('\n#else\n', start)
        adapter = source[start:end]
        # Guard the extraction against silently selecting another target block.
        if 'int rh_platform_launcher_background_stage(' not in adapter:
            raise AssertionError('launcher background block not found')
        cls.tmp = tempfile.TemporaryDirectory(prefix='rh-launcher-native-')
        cls.addClassCleanup(cls.tmp.cleanup)
        path = Path(cls.tmp.name)
        harness = path / 'test.c'
        harness.write_text(FIXTURE + adapter + CHECKS)
        cls.binary = path / 'test'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11',
                        '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined',
                        '-I' + str(ROOT / 'include'), str(harness),
                        '-o', str(cls.binary)], check=True)

    def run_case(self, case):
        subprocess.run([str(self.binary), case], check=True)

    def test_one_preflight_and_no_poll_io_or_repeated_setters(self):
        self.run_case('stage')

    def test_exact_new_and_old_key_retirement_including_same_path(self):
        self.run_case('reload')

    def test_only_exact_list_class_and_first_child(self):
        self.run_case('class')

    def test_system_clears_only_both_module_owned_pointers(self):
        self.run_case('system')

    def test_alternating_module_resident_storage_stays_stable(self):
        self.run_case('storage')

    def test_recreated_lists_address_reuse_and_cached_alias(self):
        self.run_case('reuse')

    def test_invalid_preflight_and_cache_failures_do_not_publish(self):
        self.run_case('invalid')

    def test_invalid_old_header_cache_rejects_system_commit(self):
        self.run_case('system-invalid-header')

    def test_invalid_old_image_cache_rejects_system_commit(self):
        self.run_case('system-invalid-image')

    def test_absent_caches_are_allowed(self):
        self.run_case('missing')


if __name__ == '__main__':
    unittest.main(verbosity=2)

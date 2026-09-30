"""Exact .139/.155 reload adapter probes, without signed loading or GPU simulation.

Compiles the selected target source into a temporary fixed-address ELF and runs
it against that exact AP. Native generic cache retirement/release, screen-tree
traversal, image setter and style refresh execute AP instructions. Cache
lookup/unlink/free, decoder info, style lookup and GUI leaves are explicitly
modeled. Compact-snapshot integration additionally executes the compiled module
constructor, activation, POSIX hook, control-file watcher and status writer with
modeled VFS/timers. It bypasses signed loading; no key or packaging script is read.

RESOURCE_HOOK_TARGET selects .139 (default) or .155. CLANG / LD_LLD may select
the ARM-capable toolchain. Requires capstone, Unicorn and pyelftools.
"""
from firmware_support import ROOT, CANOPUS, TARGET, PORT_TARGET, check_firmware, fw
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_PC)

REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
IMAGE_CLASS = fw(0x2ca14cb8)
DATA_CLASS = fw(0x2ca168c4)
HEADER_CLASS = fw(0x2ca16944)
DATA, HEADER, STATE = 0x3c701000, 0x3c702000, 0x3c780000
# Legacy raw rules remain 512 bytes, but state now owns a pointer/capacity.
RULES, RULE_BYTES, RULE_CAPACITY = 0x3c7a0000, 512, 256
VIEW_BYTES = 12
STOP = 0x1c73ff00


class Harness(unittest.TestCase):
    sources = tuple(ROOT / 'src' / (name + '.c') for name in
                    ('platform', 'platform_band11', 'resource_hook'))
    instruction_budget = 2000000

    @classmethod
    def setUpClass(cls):
        firmware_path = Path(os.environ.get(
            'RESOURCE_HOOK_FIRMWARE', CANOPUS / 'fwbins' / TARGET / 'vela_ap.bin'))
        check_firmware(firmware_path)
        cls.firmware = firmware_path.read_bytes()
        cls.tmp = tempfile.TemporaryDirectory(prefix='rh-reload-')
        cls.addClassCleanup(cls.tmp.cleanup)
        port = int(TARGET == PORT_TARGET)
        objects = []
        clang = os.environ.get('CLANG', shutil.which('clang'))
        linker = os.environ.get('LD_LLD', shutil.which('ld.lld'))
        for source in cls.sources:
            obj = Path(cls.tmp.name) / f'{source.stem}-{port}.o'
            subprocess.run([clang, '--target=arm-none-eabi', '-mcpu=cortex-m33',
                '-mthumb', '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
                '-fno-stack-protector', '-fno-unwind-tables', '-Os', '-Wall',
                '-Wextra', '-Werror', f'-DRH_TARGET_155={port}',
                '-I' + str(ROOT / 'include'),
                '-I' + str(CANOPUS / 'sdk/c'),
                '-I' + str(CANOPUS / 'manager/target/band11'),
                '-I' + str(CANOPUS / 'targets' / TARGET / 'generated'),
                '-c', str(source), '-o', str(obj)], check=True)
            objects.append(str(obj))
        elf = Path(cls.tmp.name) / f'adapter-{port}.elf'
        subprocess.run([linker, '-Ttext=0x1c700000', '-e', 'rh_platform_retire_images',
                        *objects, '-o', str(elf)], check=True)
        with elf.open('rb') as stream:
            f = ELFFile(stream)
            cls.segments = [(p['p_vaddr'], p.data()) for p in f.iter_segments()
                            if p['p_type'] == 'PT_LOAD' and p['p_vaddr'] >= 0x1c700000]
            cls.symbols = {s.name: s['st_value'] for s in f.get_section_by_name('.symtab').iter_symbols()}

    def setUp(self):
        self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
        n = (len(self.firmware) + 4095) & ~4095
        for address, size in [(0xc0c0000, n), (0x2c0c0000, n), (0x20000000, 0x160000),
                              (0x3c700000, 0x100000), (0x3c350000, 0x20000), (0x1c700000, 0x40000),
                              (0x00260000, 0x140000)]:
            self.u.mem_map(address, size)
        self.u.mem_write(0xc0c0000, self.firmware)
        self.u.mem_write(0x2c0c0000, self.firmware)
        # Startup-copied libc/veneers used by the real LVGL POSIX open body.
        for destination, offset, size in ((0x2006be00, 0x1b3c, 0xb710),
                                           (0x20079420, 0xd24c, 0x2f8c)):
            data = self.firmware[offset:offset + size]
            self.u.mem_write(destination, data)
            self.u.mem_write(destination - 0x1fe00000, data)
        self.load()
        self.next_memory = 0x3c703000
        self.drops, self.freed, self.queries, self.sizes, self.invalidations = [], [], [], [], []
        self.styles = {}
        self.info_ok = True
        self.alloc_fail = False
        self.unlink_noop = False
        self.entries = {}
        self.cache_nodes = {DATA: [], HEADER: []}
        heap = 0x3c356b40
        self.word(0x200b2590, heap)
        self.word(heap + 28, heap + 0x168)
        self.word(heap + 32, 0x3cfefdf8)
        def mallinfo():
            self.u.mem_write(self.reg(0), struct.pack('<7I', 0, 0, 0,
                0 if self.alloc_fail else 65536, 0, 0, 0))
            return self.reg(0)
        self.hook(0xc34f0a0, mallinfo)
        self.alloc_hook = self.hook(0xc3507e8, lambda: self.mem(self.reg(2)))
        self.free_hook = self.hook(0xc34cd2c, lambda: 0)
        self.hook(0xc38002c, lambda: self.u.mem_write(self.reg(0), bytes(self.reg(1))) or self.reg(0))
        self.hook(0xc3a4ace, self.lookup)
        self.hook(0xc3a472c, self.unlink)
        self.hook(0xc3abe58, lambda: self.freed.append(self.reg(0)) or 0)
        self.hook(0x1c73fe00, lambda: self.freed.append(self.reg(0)) or 0)
        for cache, slot, clz in [(DATA, 0x200bd310, DATA_CLASS), (HEADER, 0x200bd314, HEADER_CLASS)]:
            self.word(slot, cache)
            self.word(cache, clz)
            self.word(cache + 4, 24)
            self.word(cache + 8, 16)
            self.word(cache + 24, 0x1c73fe01)
            self.word(cache + 48, 4)
        self.u.mem_write(RULES, bytes(RULE_CAPACITY * RULE_BYTES))
        self.u.mem_write(RULES, b'/resource/\0')
        self.u.mem_write(RULES + 256, b'/data/quickapp/files/ng.lst.corona/themes/current/\0')
        # ARM rh_state: rules, count, redirected, fallback, original, driver,
        # installed, rules_capacity. Raw rule storage is NOT inline in state.
        self.u.mem_write(STATE, struct.pack('<8I', RULES, 1, 0, 0, 0, 0, 0, RULE_CAPACITY))
        # Real native NULL-root walk: display list -> screens -> spec_attr children.
        self.display = self.mem(1024)
        self.word(0x200bd1e8 + 12, self.display)
        self.word(0x200bd1e8 + 8, 1024)
        self.roots = self.mem(64)
        self.word(self.display + 692, self.roots)
        self.word(self.display + 720, 0)
        self.info_hook = self.hook(0xc38e4e4, self.info)
        self.hook(0xc38400c, lambda: self.invalidations.append(self.reg(0)) or 0)
        self.hook(fw(0xc909798), lambda: self.sizes.append(self.reg(0)) or 0)
        self.hook(0xc3ae5ea, lambda: 0)
        self.hook(0xc38414c, lambda: 0)
        self.style_hook = self.hook(0xc382620, lambda: self.style_value())
        # Enable the real explicit style-refresh function; GUI/event leaves only.
        self.byte(0x200bd1e8 + 40, 1)
        self.events = []
        self.hook(0xc37fbc8, lambda: self.events.append(tuple(self.reg(i) for i in range(3))) or 1)
        self.hook(0xc3809e2, lambda: 0)
        self.hook(0xc385194, lambda: 0)
        self.hook(0xc384668, lambda: 0)
        # Neither global image drop nor page/animation operations may run.
        for address in (0xc3a3888, 0xc8b8d50, 0xc696e24, 0xc8ed4a8):
            self.hook(address, lambda: self.fail('forbidden global drop / owner teardown'))

    def load(self):
        for address, data in self.segments:
            self.u.mem_write(address, data)

    def reg(self, index):
        return self.u.reg_read(REGS[index])

    def word(self, address, value=None):
        if value is None:
            return struct.unpack('<I', self.u.mem_read(address, 4))[0]
        self.u.mem_write(address, struct.pack('<I', value))

    def byte(self, address, value):
        self.u.mem_write(address, bytes([value]))

    def mem(self, size=128):
        address = self.next_memory
        self.next_memory += (size + 15) & ~15
        self.u.mem_write(address, bytes(size))
        return address

    def text(self, text):
        p = self.mem(len(text) + 1)
        self.u.mem_write(p, text.encode() + b'\0')
        return p

    def hook(self, address, callback):
        address &= ~1
        def run(u, pc, size, cookie):
            result = callback()
            if result is not None:
                u.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
            u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
        return self.u.hook_add(UC_HOOK_CODE, run, None, address, address)

    def call(self, name, *args):
        for reg in REGS:
            self.u.reg_write(reg, 0)
        for reg, value in zip(REGS, args):
            self.u.reg_write(reg, value)
        self.u.reg_write(UC_ARM_REG_SP, 0x20150000)
        self.u.reg_write(UC_ARM_REG_LR, STOP | 1)
        pc = self.symbols[name] if isinstance(name, str) else name
        self.u.emu_start(pc | 1, STOP, count=self.instruction_budget)
        self.assertEqual(self.u.reg_read(UC_ARM_REG_PC), STOP, 'instruction budget exhausted')
        return self.reg(0)

    def links(self, cache):
        nodes = self.cache_nodes[cache]
        self.word(cache + 52, nodes[0] if nodes else 0)
        for i, node in enumerate(nodes):
            self.word(node + 8, nodes[i + 1] if i + 1 < len(nodes) else 0)

    def cached(self, cache, path, kind=1, refs=2):
        node, rb, data = self.mem(), self.mem(), self.mem()
        source = self.text(path)
        self.word(node, rb)
        self.word(rb + 16, data)
        offset = 4 if cache == DATA else 0
        self.word(data + offset, source)
        self.byte(data + offset + 4, kind)
        self.word(data + 28, refs)
        self.word(data + 32, 24)
        self.entries[data] = (cache, node)
        self.cache_nodes[cache].append(node)
        self.links(cache)
        return node, data, source

    def lookup(self):
        cache, data = self.reg(0), self.reg(1)
        expected, node = self.entries[data]
        self.assertEqual(cache, expected)
        self.drops.append(data)
        # Emulate LRU lookup moving a non-head match before unlink.
        self.cache_nodes[cache].remove(node)
        self.cache_nodes[cache].insert(0, node)
        self.links(cache)
        return data + 24

    def unlink(self):
        cache, entry = self.reg(0), self.reg(1)
        if not self.unlink_noop:
            node = self.entries[entry - 24][1]
            self.cache_nodes[cache].remove(node)
            self.links(cache)
            # Poison list node immediately: module must not read it after drop.
            self.u.mem_write(node, b'\xff' * 16)
        return 0

    def obj(self, path=None, clz=IMAGE_CLASS, kind=1, parent=0):
        object = self.mem()
        self.word(object, clz)
        self.word(object + 4, parent)
        if path is not None:
            self.word(object + 52, self.text(path))
        self.byte(object + 96, kind)
        self.word(object + 68, 10)
        self.word(object + 72, 20)
        self.word(object + 80, 256)
        self.word(object + 84, 256)
        return object

    def screens(self, *objects):
        self.word(self.display + 720, len(objects))
        for i, object in enumerate(objects):
            self.word(self.roots + i * 4, object)

    def children(self, parent, *objects):
        spec, array = self.mem(), self.mem(len(objects) * 4)
        self.word(parent + 8, spec)
        self.word(spec, array)
        self.u.mem_write(spec + 48, struct.pack('<H', len(objects)))
        for i, object in enumerate(objects):
            self.word(array + i * 4, object)
            self.word(object + 4, parent)

    def info(self):
        self.queries.append(self.reg(0))
        # LVGL native header = magic/cf/flags, w/h, stride/reserved (12 bytes).
        self.u.mem_write(self.reg(1), struct.pack('<BBHHHHH', 0x19, 4, 0, 77, 99, 154, 0))
        return int(self.info_ok)

    def style_value(self):
        if self.reg(2) != 40:
            return 0
        self.assertEqual(self.reg(1), 0)
        return self.styles.get(self.reg(0), 0)

    def view(self, rules=0, count=0):
        view = self.mem(VIEW_BYTES)
        # ARM rh_mapping_view: legacy rules, count, compact snapshot (NULL for
        # these manually built legacy-rule probes).
        self.u.mem_write(view, struct.pack('<3I', rules, count, 0))
        return view


class Reload(Harness):
    def test_raw_pointer_state_and_view_support_rule_256(self):
        # Legacy raw views deliberately revalidate all rows; the compact
        # production path avoids that quadratic work on each resolution.
        self.instruction_budget = 20000000
        for index in range(RULE_CAPACITY):
            rule = RULES + index * RULE_BYTES
            self.u.mem_write(rule, f'/resource/icon{index:03d}.bin\0'.encode())
            self.u.mem_write(rule + 256,
                f'/data/quickapp/files/ng.lst.corona/themes/current/icon{index:03d}.bin\0'.encode())
        self.word(STATE + 4, RULE_CAPACITY)
        view = self.view(RULES, RULE_CAPACITY)
        affected = [self.cached(DATA, '/resource/icon255.bin')[1],
                    self.cached(DATA, '/resource/icon000.bin')[1]]
        self.cached(DATA, '/resource/icon256.bin')
        self.assertEqual(self.call('rh_platform_retire_images', STATE), 0)
        self.assertEqual(self.drops, affected)
        image = self.obj('/resource/icon255.bin')
        self.screens(image)
        self.assertEqual(self.call('rh_platform_refresh_mapped_images', 0, view), 0)
        self.assertEqual(self.word(image + 68), 77)
        self.assertEqual(self.word(STATE + 28), RULE_CAPACITY)

    def test_retire_only_affected_file_keys_in_both_caches(self):
        retained = []
        affected = []
        for cache in (HEADER, DATA):
            retained.append(self.cached(cache, '/unrelated/a.bin')[0])
            affected.append(self.cached(cache, '/resource/a.bin')[1])
            retained.append(self.cached(cache, '/resource/descriptor', kind=0)[0])
            retained.append(self.cached(cache, '/resource/symbol', kind=2)[0])
            retained.append(self.cached(cache, 'S:/resource/other-drive')[0])
            affected.append(self.cached(cache, '/resource/b.bin', refs=0)[1])
        self.assertEqual(self.call('rh_platform_retire_images', STATE), 0)
        self.assertEqual(self.drops, affected)
        self.assertEqual(self.cache_nodes[HEADER] + self.cache_nodes[DATA], retained)
        for data in affected[::2]:
            self.assertEqual(self.u.mem_read(data + 36, 1), b'\x01')
            self.assertNotIn(data, self.freed)
        self.assertEqual(self.freed, [affected[1], affected[1], affected[3], affected[3]])
        self.assertEqual(self.call('rh_platform_retire_images', STATE), 0)
        self.assertEqual(len(self.drops), 4)

    def test_empty_mapping_refreshes_old_sources_to_original(self):
        rules = self.mem(RULE_BYTES)
        self.u.mem_write(rules, b'/resource/\0')
        self.u.mem_write(rules + 256, b'/data/quickapp/files/ng.lst.corona/themes/current/\0')
        previous, current = self.view(rules, 1), self.view()

        image = self.obj('/resource/app/settings/launcher.bin')
        self.screens(image)
        source = self.word(image + 52)
        self.assertEqual(self.call('rh_platform_retire_mapped_images', current), 1)
        self.assertEqual(self.call('rh_platform_refresh_mapped_images', previous, current), 0)
        self.assertEqual(self.queries, [source, source])
        self.assertEqual((self.word(image + 68), self.word(image + 72)), (77, 99))
        self.assertEqual(self.word(image + 52), source)

    def test_held_payload_survives_until_native_last_release(self):
        node, data, source = self.cached(DATA, '/resource/held.bin')
        self.assertEqual(self.call('rh_platform_retire_images', STATE), 0)
        self.assertEqual(self.freed, [])
        for refs in (1, 0):
            self.call(fw(0xc8b9790), DATA, data + 24)
            self.assertEqual(self.word(data + 28), refs)
            self.assertEqual(self.freed, [] if refs else [data, data])

    def test_wrong_class_layout_cycles_and_null_rb_fail_before_mutation(self):
        node, data, source = self.cached(DATA, '/resource/a.bin')
        for field, invalid in ((HEADER, DATA_CLASS), (HEADER + 48, 8), (node, 0), (node + 8, node)):
            saved = self.word(field)
            self.word(field, invalid)
            self.assertEqual(self.call('rh_platform_retire_images', STATE), 0xffffffff)
            self.assertEqual(self.drops, [])
            self.word(field, saved)

    def test_no_progress_unlink_is_retry_not_infinite_loop(self):
        self.cached(DATA, '/resource/a.bin')
        self.unlink_noop = True
        self.assertEqual(self.call('rh_platform_retire_images', STATE), 0xffffffff)
        self.assertEqual(len(self.drops), 1)

    def test_invalid_unmapped_and_overlong_mapping_results_are_not_retired(self):
        for path in ('/resource/../a', '/resource//a', '/resource/' + 'x' * 245, 'resource/a'):
            self.cached(DATA, path)
        self.assertEqual(self.call('rh_platform_retire_images', STATE), 0)
        self.assertEqual(self.drops, [])

    def test_real_same_pointer_setter_updates_dimensions_without_source_churn(self):
        object = self.obj('/resource/a.bin')
        self.screens(object)
        source = self.word(object + 52)
        self.assertEqual(self.call('rh_platform_refresh_images', STATE), 0)
        self.assertEqual(self.queries, [source, source])
        self.assertEqual((self.word(object + 68), self.word(object + 72)), (77, 99))
        self.assertEqual(self.word(object + 52), source)
        self.assertEqual(self.freed, [])
        self.assertIn(object, self.sizes)

    def test_failed_info_preserves_original_source_and_dimensions(self):
        object = self.obj('/resource/broken.bin')
        self.screens(object)
        source = self.word(object + 52)
        self.info_ok = False
        self.assertEqual(self.call('rh_platform_refresh_images', STATE), 0)
        self.assertEqual(self.queries, [source])
        self.assertEqual((self.word(object + 52), self.word(object + 68), self.word(object + 72)),
                         (source, 10, 20))
        self.assertEqual(self.freed, [])

    def test_deleting_owner_subtree_is_skipped(self):
        root, image = self.obj(clz=0x2ca1081c), self.obj('/resource/a')
        self.children(root, image)
        self.screens(root)
        self.byte(root + 51, 16)  # real native deleting bit
        self.assertEqual(self.call('rh_platform_refresh_images', STATE), 0)
        self.assertEqual(self.queries, [])
        self.assertEqual(self.word(image + 68), 10)

    def test_disabled_header_cache_skips_destructive_setter_risk(self):
        object = self.obj('/resource/a')
        self.screens(object)
        self.word(HEADER + 8, 0)
        self.assertEqual(self.call('rh_platform_refresh_images', STATE), 0)
        self.assertEqual(self.queries, [])
        self.assertEqual(self.word(object + 68), 10)

    def test_source_change_during_info_is_not_overwritten(self):
        object = self.obj('/resource/a')
        self.screens(object)
        changed = self.text('/unrelated/new-owner-source')
        self.u.hook_del(self.info_hook)
        def change_source():
            self.info()
            self.word(object + 52, changed)
            return 1
        self.hook(0xc38e4e4, change_source)
        self.assertEqual(self.call('rh_platform_refresh_images', STATE), 0)
        self.assertEqual(self.word(object + 52), changed)
        self.assertEqual(self.word(object + 68), 10)
        self.assertEqual(len(self.queries), 1)

    def test_native_get_info_cache_hit_copies_header_and_releases_reference(self):
        self.u.hook_del(self.info_hook)
        _, data, source = self.cached(HEADER, '/resource/a', refs=1)
        header = struct.pack('<BBHHHHH', 0x19, 4, 0, 77, 99, 154, 0)
        self.u.mem_write(data + 8, header)
        self.word(data + 20, 0x3c7f0000)  # nonnull native decoder identity
        def acquire():
            self.assertEqual(self.reg(0), HEADER)
            self.assertEqual(self.word(self.reg(1)), source)
            self.word(data + 28, self.word(data + 28) + 1)
            return data + 24
        self.hook(0xc3a3860, acquire)
        out = self.mem()
        self.assertEqual(self.call(0xc38e4e4, source, out), 1)
        self.assertEqual(bytes(self.u.mem_read(out, 12)), header)
        self.assertEqual(self.word(data + 28), 1)
        self.assertEqual(self.freed, [])

    def test_skip_derived_memory_symbol_and_unaffected_sources(self):
        objects = [self.obj('/resource/a', clz=IMAGE_CLASS + 36),
                   self.obj('/resource/a', kind=0), self.obj('/resource/a', kind=2),
                   self.obj('/unrelated/a'), self.obj('S:/resource/a')]
        self.screens(*objects)
        self.assertEqual(self.call('rh_platform_refresh_images', STATE), 0)
        self.assertEqual(self.queries, [])

    def test_offscreen_trees_and_image_bearing_style_on_nonimage_object(self):
        image, button, unrelated = self.obj('/resource/a'), self.obj(clz=0x2ca1081c), self.obj()
        root = self.obj(clz=0x2ca1081c)
        self.children(root, image, button, unrelated)
        self.screens(self.obj(clz=0x2ca1081c), root)
        self.styles[button] = self.text('/resource/button.bin')
        self.styles[unrelated] = self.text('/unrelated/button.bin')
        self.assertEqual(self.call('rh_platform_refresh_images', STATE), 0)
        self.assertEqual(self.word(image + 68), 77)
        self.assertIn(button, self.invalidations)  # real explicit native refresh
        self.assertNotIn(unrelated, self.invalidations)

    def test_deletion_of_later_snapshot_object_is_revalidated(self):
        first, second = self.obj('/resource/a'), self.obj('/resource/b')
        root = self.obj(clz=0x2ca1081c)
        self.children(root, first, second)
        self.screens(root)
        def resize():
            # A native size refresh invokes owner callbacks; model deletion of
            # the next snapshot candidate and poison its old memory.
            self.u.mem_write(self.word(root + 8) + 48, struct.pack('<H', 1))
            self.u.mem_write(second, b'\xff' * 128)
            return 0
        self.hook(fw(0xc909798), resize)
        self.assertEqual(self.call('rh_platform_refresh_images', STATE), 0)
        self.assertEqual(len(self.queries), 2)

    def test_allocation_failure_and_tree_overflow_do_not_partially_refresh(self):
        root = self.obj('/resource/root')
        self.screens(root)
        self.alloc_fail = True
        self.assertEqual(self.call('rh_platform_refresh_images', STATE), 0xffffffff)
        self.alloc_fail = False
        self.children(root, *(self.obj() for _ in range(1024)))
        self.assertEqual(self.call('rh_platform_refresh_images', STATE), 0xffffffff)
        self.assertEqual(self.queries, [])

    def test_native_style_lookup_selects_current_main_state(self):
        self.u.hook_del(self.style_hook)
        button = self.obj(clz=0x2ca1081c)
        self.screens(button)
        styles = self.mem(24)
        self.word(button + 12, styles)
        self.word(button + 16, 1 << (40 // 8))
        self.u.mem_write(button + 50, struct.pack('<H', 3 << 4))
        values = {}
        for i, state in enumerate((0, 32, 128)):
            style = self.mem()
            self.word(style + 4, 1 << (40 // 4))
            self.word(styles + i * 8, style)
            self.word(styles + i * 8 + 4, state)
            values[style] = self.text('/resource/button' + str(state))
        def property_value():
            self.assertEqual(self.reg(1), 40)
            self.word(self.reg(2), values[self.reg(0)])
            return 1
        # Only the style's property-storage leaf is modeled, not selector/state
        # choice: native get-style-prop and its state precedence run for real.
        self.hook(0xc378f1e, property_value)
        for i, state in enumerate((0, 32, 128)):
            self.u.mem_write(button + 48, struct.pack('<H', state))
            self.assertEqual(self.call(0xc382620, button, 0, 40),
                             values[self.word(styles + i * 8)])
            before = bytes(self.u.mem_read(styles, 24))
            self.assertEqual(self.call('rh_platform_refresh_images', STATE), 0)
            self.assertEqual(bytes(self.u.mem_read(styles, 24)), before)
            self.assertEqual(bytes(self.u.mem_read(button + 48, 2)), struct.pack('<H', state))
        self.assertIn(button, self.invalidations)

    def test_depth_bound_prevents_unbounded_native_recursion(self):
        root = self.obj('/resource/root')
        self.screens(root)
        parent = root
        for _ in range(40):
            child = self.obj()
            self.children(parent, child)
            parent = child
        self.assertEqual(self.call('rh_platform_refresh_images', STATE), 0xffffffff)
        self.assertEqual(self.queries, [])

    def test_selected_target_class_guards_reject_foreign_cache_layout(self):
        self.word(HEADER, DATA_CLASS)
        self.assertEqual(self.call('rh_platform_retire_images', STATE), 0xffffffff)
        self.assertEqual(self.drops, [])
        self.word(HEADER, HEADER_CLASS)
        self.assertEqual(self.call('rh_platform_retire_images', STATE), 0)


class ModuleControl(Harness):
    """Real compact module and exact AP; signed Supervisor loading is bypassed."""
    sources = Harness.sources + (ROOT / 'src/config.c', ROOT / 'src/module.c',
                                  CANOPUS / 'runtime/control/canopus_control.c')
    instruction_budget = 20000000
    app_root = '/data/quickapp/files/ng.lst.corona/'
    config_path = app_root + 'mappings.tsv'
    request_path = app_root + 'reload.request'
    result_path = app_root + 'reload.result'
    theme_root = app_root + 'themes/'

    def setUp(self):
        super().setUp()
        self.disk = {self.result_path: bytes(256)}
        self.files, self.allocations = {}, {}
        self.next_fd = 3
        self.descriptor = None
        self.errno = self.mem(4)
        self.timers = {}
        self.next_timer = 0x3c7e0000
        self.fail_allocation = False
        self.u.hook_del(self.alloc_hook)
        self.u.hook_del(self.free_hook)
        self.hook(0xc3507e8, self.allocate)
        self.hook(0xc34cd2c, self.free)
        self.hook(0xc342c54, self.opened)
        self.hook(0xc33d784, self.read_file)
        self.hook(0xc33dc4e, self.write_file)
        self.hook(0xc33818c, self.close_file)
        self.hook(0xc349538, lambda: self.errno)
        self.hook(0xc3a5e34, lambda: 0)  # native POSIX failure logging only
        self.hook(0xc3abd20, self.create_timer)
        self.hook(0xc3abe70, self.delete_timer)
        self.hook(0xc3809a4, lambda: 1)  # invalidation event leaf only
        self.word(0x200bd3b8, ord('/'))
        self.word(0x200bd3bc, 4096)
        self.word(0x200bd3c4, 0xc3a6195)
        self.word(self.display, 192)
        self.word(self.display + 4, 490)
        self.word(self.display + 696, self.display + 800)
        self.word(self.display + 56, 1 << 8)
        self.word(self.display + 608, 1)
        self.word(0x200bd200, self.display)
        # Execute the real constructor/registration message, with /dev/canopus
        # modeled locally rather than booting a signed Supervisor fixture.
        self.call('ctor')
        self.assertEqual(self.descriptor, self.symbols['canopus_module_descriptor'])

    def string(self, address):
        out = bytearray()
        for index in range(1024):
            byte = self.u.mem_read(address + index, 1)[0]
            if not byte:
                return out.decode()
            out.append(byte)
        self.fail('unterminated VFS path')

    def allocate(self):
        if self.fail_allocation:
            return 0
        size = self.reg(2)
        address = self.mem(size)
        self.assertLessEqual(self.next_memory, STATE, 'modeled heap overlaps fixtures')
        self.allocations[address] = size
        return address

    def free(self):
        address = self.reg(0)
        self.assertIn(address, self.allocations)
        self.assertNotIn(address, self.freed, 'double free')
        self.freed.append(address)
        # Make stale snapshot accesses observable, not accidentally valid rules.
        self.u.mem_write(address, b'\xdd' * self.allocations[address])
        return 0

    def live_allocations(self):
        return {address: size for address, size in self.allocations.items()
                if address not in self.freed}

    def assert_compact_snapshot(self, config):
        # Check the production allocation itself, not just status.count: one
        # 16-byte header, 8-byte indexes and pool-relative TSV destinations.
        live = self.live_allocations()
        self.assertEqual(len(live), 1)
        address, allocated = next(iter(live.items()))
        references, size, pool_bytes, count = struct.unpack('<4I', self.u.mem_read(address, 16))
        expected = dict(line.split(b'\t') for line in config.splitlines())
        self.assertEqual(references, 1)
        self.assertEqual(count, len(expected))
        self.assertEqual(pool_bytes, sum(len(source) + len(destination) + 2
                                        for source, destination in expected.items()))
        self.assertEqual(size, allocated)
        self.assertEqual(size, 16 + count * 8 + pool_bytes)
        pool = address + 16 + count * 8
        actual = {}
        for index in range(count):
            source, destination, source_length, destination_length, flags, reserved = struct.unpack(
                '<HHBBBB', self.u.mem_read(address + 16 + index * 8, 8))
            self.assertEqual((flags, reserved), (0, 0))
            self.assertLess(source + source_length, pool_bytes)
            self.assertLess(destination + destination_length, pool_bytes)
            key = bytes(self.u.mem_read(pool + source, source_length + 1))
            value = bytes(self.u.mem_read(pool + destination, destination_length + 1))
            self.assertEqual(key[-1:], b'\0')
            self.assertEqual(value[-1:], b'\0')
            actual[key[:-1]] = value[:-1]
        self.assertEqual(actual, expected)

    def opened(self):
        path, mode = self.string(self.reg(0)), self.reg(1)
        if path == '/dev/canopus':
            data = None
        elif mode & 4:
            data = bytearray()
        elif path in self.disk:
            data = bytearray(self.disk[path])
        else:
            self.word(self.errno, 2)
            return -1
        fd = self.next_fd
        self.next_fd += 1
        self.files[fd] = [data, 0, path]
        return fd

    def read_file(self):
        data, offset, _ = self.files[self.reg(0)]
        block = data[offset:offset + min(self.reg(2), 173)]
        self.u.mem_write(self.reg(1), bytes(block))
        self.files[self.reg(0)][1] += len(block)
        return len(block)

    def write_file(self):
        data, offset, path = self.files[self.reg(0)]
        if path == '/dev/canopus':
            self.assertEqual(self.reg(2), 40)
            self.assertEqual(self.word(self.reg(1)), 0x31524d43)
            self.descriptor = self.word(self.reg(1) + 4)
            return 40
        size = min(self.reg(2), 173)
        data[offset:offset + size] = self.u.mem_read(self.reg(1), size)
        self.files[self.reg(0)][1] += size
        return size

    def close_file(self):
        data, _, path = self.files.pop(self.reg(0))
        if data is not None:
            self.disk[path] = bytes(data)
        return 0

    def create_timer(self):
        callback, period, cookie = (self.reg(index) for index in range(3))
        self.assertIn(period, (50, 1000))
        self.assertEqual(cookie, 0)
        timer = self.next_timer
        self.next_timer += 32
        self.timers[timer] = (period, callback)
        return timer

    def delete_timer(self):
        self.assertIn(self.reg(0), self.timers)
        del self.timers[self.reg(0)]
        return 0

    def tick(self, period=1000):
        timers = [timer for timer, (ms, _) in self.timers.items() if ms == period]
        self.assertTrue(timers, f'no live {period} ms timer')
        timer = timers[-1]
        self.call(self.timers[timer][1], timer)

    def status(self):
        writer, output = self.mem(24), self.mem(40)
        self.u.mem_write(writer, struct.pack('<6I', output, 40, 0, 0, 1, 1))
        self.assertEqual(self.call(self.word(self.descriptor + 140), writer), 0)
        return struct.unpack('<10I', self.u.mem_read(output, 40))

    def open_path(self, path):
        # Keep the exact AP's POSIX slash addition and fd+1 encoding in the path.
        handle = self.call(self.word(0x200bd3c4), 0x200bd3b8, self.text(path.lstrip('/')), 2)
        self.assertGreater(handle, 0)
        return self.files[handle - 1][2]

    def revision(self, name, config):
        signal = f'resource-hook-reload-v1\tng.lst.corona\t{name}\n'.encode()
        self.disk[self.config_path] = config
        self.disk[self.request_path] = signal
        self.tick()
        result = self.disk[self.result_path]
        self.assertEqual(len(result), 256)
        lines = result.rstrip(b'\0').splitlines(keepends=True)
        self.assertEqual(lines[0], signal)
        self.assertEqual(len(lines), 3)
        checksum = 2166136261
        for byte in lines[0] + lines[1]:
            checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
        self.assertEqual(lines[2], f'{checksum}\n'.encode())
        return lines[1].decode().rstrip('\n').split('\t')

    def config(self, count=RULE_CAPACITY, pack='current'):
        return ''.join(f'/resource/icon{index:03d}.bin\tthemes/{pack}/icon{index:03d}.bin\n'
                       for index in range(count)).encode()

    def test_256_rule_start_reload_identical_and_empty_snapshots(self):
        self.disk[self.config_path] = self.config()
        for index in (0, 255):
            self.disk[self.theme_root + f'current/icon{index:03d}.bin'] = b'first theme'
            self.disk[self.theme_root + f'next/icon{index:03d}.bin'] = b'next theme'
            self.disk[f'/resource/icon{index:03d}.bin'] = b'original'
        self.assertEqual(self.call('activate', 0), 0)
        self.assertEqual(self.status()[2:4], (1, RULE_CAPACITY))
        self.assert_compact_snapshot(self.config())
        resident_hook = self.word(0x200bd3c4)
        for index in (0, 255):
            self.assertEqual(self.open_path(f'/resource/icon{index:03d}.bin'),
                             self.theme_root + f'current/icon{index:03d}.bin')
        # Production keeps a compact allocation, not two 256 x 512-byte banks.
        self.assertLess(sum(self.live_allocations().values()), RULE_CAPACITY * RULE_BYTES)
        before = self.live_allocations()
        self.assertEqual(self.revision('same', self.config()), ['RHRS1', '5', '0', '0', '0'])
        self.assertEqual(self.live_allocations(), before)
        self.assertEqual(self.revision('next', self.config(pack='next')),
                         ['RHRS1', '5', '0', '0', '0'])
        self.assertEqual(self.status()[3], RULE_CAPACITY)
        self.assert_compact_snapshot(self.config(pack='next'))
        self.assertTrue(set(before).issubset(set(self.freed)), 'old snapshot not reclaimed')
        self.assertEqual(self.word(0x200bd3c4), resident_hook)
        for index in (0, 255):
            self.assertEqual(self.open_path(f'/resource/icon{index:03d}.bin'),
                             self.theme_root + f'next/icon{index:03d}.bin')
        self.assertEqual(self.revision('empty', b''), ['RHRS1', '5', '0', '0', '0'])
        self.assertEqual(self.status()[2:4], (1, 0))
        self.assertEqual(self.open_path('/resource/icon255.bin'), '/resource/icon255.bin')
        self.assertEqual(self.live_allocations(), {})
        self.assertEqual(self.word(0x200bd3c4), resident_hook)

    def test_synthetic_driver_reset_keeps_snapshot_and_replaces_only_watcher(self):
        self.disk[self.config_path] = self.config()
        self.disk[self.theme_root + 'current/icon255.bin'] = b'theme'
        self.word(self.display + 56, (1 << 8) | (2 << 16))
        self.assertEqual(self.call('activate', 0), 0)
        resident_hook = self.word(0x200bd3c4)
        resident_allocations = self.live_allocations()
        old_watcher, = [timer for timer, (ms, _) in self.timers.items() if ms == 1000]
        refresh_timer, = [timer for timer, (ms, _) in self.timers.items() if ms == 50]
        self.disk[self.config_path] = b'invalid config'
        self.word(0x200bd3c4, 0xc3a6195)  # synthetic lv_init slot reset only
        self.assertEqual(self.call('activate', 0), 0)
        self.assertEqual(self.word(0x200bd3c4), resident_hook)
        self.assertEqual(self.live_allocations(), resident_allocations)
        self.assertEqual([timer for timer, (ms, _) in self.timers.items() if ms == 50],
                         [refresh_timer])
        new_watcher = [timer for timer, (ms, _) in self.timers.items() if ms == 1000][-1]
        self.assertNotEqual(new_watcher, old_watcher)
        self.call(self.timers[old_watcher][1], old_watcher)
        self.assertNotIn(old_watcher, self.timers)
        self.assertIn(new_watcher, self.timers)
        self.assertIn(refresh_timer, self.timers)
        self.assertEqual(self.open_path('/resource/icon255.bin'),
                         self.theme_root + 'current/icon255.bin')
        self.word(self.display + 56, 1 << 8)
        self.tick(50)
        self.assertNotIn(refresh_timer, self.timers)
        self.assertIn(new_watcher, self.timers)
        self.assert_compact_snapshot(self.config())

    def test_old_compact_snapshot_survives_until_50ms_owner_refresh(self):
        self.disk[self.config_path] = self.config()
        self.disk[self.theme_root + 'next/icon255.bin'] = b'next theme'
        self.assertEqual(self.call('activate', 0), 0)
        previous = self.live_allocations()
        image = self.obj('/resource/icon255.bin')
        self.screens(image)
        source = self.word(image + 52)
        self.word(self.display + 56, (1 << 8) | (2 << 16))
        self.assertEqual(self.revision('pending', self.config(pack='next')),
                         ['RHRS1', '5', '0', '1', '0'])
        self.assertEqual(len(self.live_allocations()), 2)
        self.assertTrue(set(previous).issubset(self.live_allocations()))
        self.assertEqual(self.open_path('/resource/icon255.bin'),
                         self.theme_root + 'next/icon255.bin')
        self.tick(50)
        self.assertTrue(set(previous).issubset(self.live_allocations()))
        self.assertEqual(self.queries, [])  # busy render is never mutated
        self.word(self.display + 56, 1 << 8)
        self.tick(50)
        self.assertTrue(set(previous).issubset(set(self.freed)))
        self.assert_compact_snapshot(self.config(pack='next'))
        self.assertEqual(self.queries, [source, source])
        self.assertEqual(self.word(image + 52), source)
        self.assertEqual(self.word(image + 68), 77)
        self.assertEqual([period for period, _ in self.timers.values()], [1000])

    def test_failed_257_rule_reload_preserves_256_rule_snapshot(self):
        self.disk[self.config_path] = self.config()
        self.disk[self.theme_root + 'current/icon255.bin'] = b'theme'
        self.assertEqual(self.call('activate', 0), 0)
        before = self.live_allocations()
        result = self.revision('overflow', self.config(count=RULE_CAPACITY + 1))
        self.assertEqual(result[0:2], ['RHRS1', '5'])
        self.assertLess(int(result[2]), 0)
        self.assertEqual(self.live_allocations(), before)
        self.assertEqual(self.status()[3], RULE_CAPACITY)
        self.assertEqual(self.open_path('/resource/icon255.bin'),
                         self.theme_root + 'current/icon255.bin')

    def test_missing_config_starts_empty_and_loads_256_rules_by_signal(self):
        self.assertEqual(self.call('activate', 0), 0)
        self.assertEqual(self.status()[2:4], (1, 0))
        self.assertEqual([period for period, _ in self.timers.values()], [1000])
        self.disk[self.theme_root + 'current/icon255.bin'] = b'theme'
        self.assertEqual(self.revision('late', self.config()), ['RHRS1', '5', '0', '0', '0'])
        self.assertEqual(self.status()[2:4], (1, RULE_CAPACITY))
        self.assert_compact_snapshot(self.config())
        self.assertEqual(self.open_path('/resource/icon255.bin'),
                         self.theme_root + 'current/icon255.bin')


if __name__ == '__main__':
    unittest.main(verbosity=2)

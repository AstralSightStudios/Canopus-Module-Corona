"""Bounded .043 QuickApp BIN reload probe; no rendering/device-success claim.

Run: build/firmware-tests/bin/python tests/firmware_quickapp_reload_1043.py
Compiles production platform/platform_band10pro/resource_hook with RH_TARGET_1043.
Does not import/extend firmware_support or probe app registry/launcher lookup.

Capstone audit of the fingerprinted AP: drop 0xc1667dc calls class +12 lookup
(0xc272b34), +20 unlink (0xc272ce8); entry +4 refs, +12 invalid, +8 data
back-offset are read by native 0xc1668fc..0xc1669fc. Release 0xc1666ec executes
native decrement and frees only invalid zero-ref entries via cache+24 free_cb.
Those two LRU/RB class leaves are modeled (including lookup moving to head),
as is a synthetic payload free_cb. Actual generic drop/release/helpers execute.
NULL-root 0xc13c690 -> display-next 0xc13ccbc -> native LL accessors traverses
uikit+8/+12, display+692/+720, object+8 children/+48 count; no walk hook.
Setter 0xc17a2f4 calls native source-type 0xc13fc54 and compares old/new pointers
at 0xc17a438..45a: same pointer avoids strdup/free yet updates +68/+72 dimensions.
Native alignment helper 0xc17a27c executes with default alignment.
Modeled GUI leaves: invalidate 0xc138744, size refresh 0xc137df8, transform refresh
0xc1372fc, effective style getter 0xc1068a8 (no styles in these probes).
Modeled info 0xc143798 supplies an immutable successful BIN header (or fails);
no decoder, VFS, signed loading, actual BIN bytes, GPU or display simulation.
Modeled native heap leaves: malloc/free 0xc1f903c/0xc1f8ff8 and LVGL core free
0xc187e48. Instruction allowlist rejects every other AP path, including global
cache reset/drop and owner teardown. Firmware and ELF exist only as inputs/
temporary build products; mapping views, caches and object trees are synthetic.
"""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_R0,
    UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP,
    UC_ARM_REG_LR, UC_ARM_REG_PC)

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'build/firmware-analysis/vela_ap_3.101.043.bin'
SHA256 = '519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec'
KEY = '/data/app/org.example.clock/images/icon.bin'
THEME = '/data/files/ng.lst.corona/themes/current/icon.bin'
DATA, HEADER, STOP, PAYLOAD_FREE = 0x3c701000, 0x3c702000, 0x1c73ff00, 0x1c73fe00
IMAGE_CLASS, DATA_CLASS, HEADER_CLASS = 0x2cce61ec, 0x2cce56f4, 0x2cce571c
REGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
DROP, RELEASE, WALK, SETTER = 0xc1667dc, 0xc1666ec, 0xc13c690, 0xc17a2f4
# Only audited native bodies, not broad firmware-address ranges.
NATIVE = ((DROP, 0xc16682a), (RELEASE, 0xc166726),
          (0xc1668fc, 0xc1669a2), (0xc1669f0, 0xc1669fe),
          (WALK, 0xc13c70a), (0xc13bc70, 0xc13bcee),
          (0xc13ccbc, 0xc13ccd4), (0xc169e18, 0xc169e4a),
          (SETTER, 0xc17a4ea), (0xc17a27c, SETTER),
          (0xc13fc54, 0xc13fc84), (0xc16dae4, 0xc16daf8),
          (0xc16d9dc, 0xc16d9e6))


class QuickAppReload1043(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.firmware = FIRMWARE.read_bytes()
        if hashlib.sha256(cls.firmware).hexdigest() != SHA256:
            raise RuntimeError('.043 firmware fingerprint mismatch')
        cls.tmp = tempfile.TemporaryDirectory(prefix='rh-quickapp-reload-1043-')
        cls.addClassCleanup(cls.tmp.cleanup)
        sdk = Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus'))
        if not sdk.is_dir():
            sdk = ROOT.parent / 'Canopus-Private'
        objects = []
        for name in ('platform', 'platform_band10pro', 'resource_hook'):
            obj = Path(cls.tmp.name) / (name + '.o')
            subprocess.run([os.environ.get('CLANG', shutil.which('clang')),
                '--target=arm-none-eabi', '-mcpu=cortex-m33', '-mthumb',
                '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
                '-fno-stack-protector', '-fno-unwind-tables', '-Os', '-Wall',
                '-Wextra', '-Werror', '-DRH_TARGET_1043=1',
                '-I' + str(ROOT / 'include'), '-I' + str(sdk / 'sdk/c'),
                '-I' + str(sdk / 'manager/target/band11'),
                '-I' + str(sdk / 'targets/xiaomi-band-10-pro-3.101.043/generated'),
                '-c', str(ROOT / 'src' / (name + '.c')), '-o', str(obj)], check=True)
            objects.append(str(obj))
        elf = Path(cls.tmp.name) / 'reload.elf'
        subprocess.run([os.environ.get('LD_LLD', shutil.which('ld.lld')),
            '-Ttext=0x1c700000', '-e', 'rh_platform_retire_mapped_images',
            *objects, '-o', str(elf)], check=True)
        with elf.open('rb') as stream:
            f = ELFFile(stream)
            cls.segments = [(p['p_vaddr'], p.data()) for p in f.iter_segments()
                           if p['p_type'] == 'PT_LOAD' and p['p_vaddr'] >= 0x1c700000]
            cls.symbols = {s.name: s['st_value'] for s in
                          f.get_section_by_name('.symtab').iter_symbols()}

    def setUp(self):
        self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
        n = (len(self.firmware) + 4095) & ~4095
        for addr, size in ((0xc0c0000, n), (0x2c0c0000, n),
                           (0x20000000, 0x160000), (0x3c700000, 0x100000),
                           (0x1c700000, 0x40000)):
            self.u.mem_map(addr, size)
        for base in (0xc0c0000, 0x2c0c0000):
            self.u.mem_write(base, self.firmware)
        for addr, data in self.segments:
            self.u.mem_write(addr, data)
        self.next_memory = 0x3c703000
        self.executed, self.drops, self.freed, self.queries = set(), [], [], []
        self.setters, self.sizes, self.invalidations = [], [], []
        self.entries, self.nodes = {}, {DATA: [], HEADER: []}
        self.info_ok, self.dimensions = True, (77, 99)
        self.leaves = {}
        self.leaves[0xc272b34] = self.lookup
        self.leaves[0xc272ce8] = self.unlink
        self.leaves[PAYLOAD_FREE] = lambda: self.freed.append(('payload', self.reg(0))) or 0
        self.leaves[0xc187e48] = lambda: self.freed.append(('entry', self.reg(0))) or 0
        self.snapshot_allocations = set()
        def allocate():
            addr = self.mem(self.reg(0))
            self.snapshot_allocations.add(addr)
            return addr
        def free_snapshot():
            self.assertIn(self.reg(0), self.snapshot_allocations, 'owner/source teardown')
            self.snapshot_allocations.remove(self.reg(0))
            return 0
        self.leaves[0xc1f903c] = allocate
        self.leaves[0xc1f8ff8] = free_snapshot
        self.leaves[0xc143798] = self.info
        self.leaves[0xc138744] = lambda: self.invalidations.append(self.reg(0)) or 0
        self.leaves[0xc137df8] = lambda: self.sizes.append(self.reg(0)) or 0
        self.leaves[0xc1372fc] = lambda: 0
        self.leaves[0xc1068a8] = lambda: 0
        self.u.hook_add(UC_HOOK_CODE, self.trace)
        for cache, slot, clz in ((DATA, 0x2010329c, DATA_CLASS),
                                  (HEADER, 0x201032a0, HEADER_CLASS)):
            for off, value in ((0, clz), (4, 24), (8, 16),
                               (24, PAYLOAD_FREE | 1), (48, 4)):
                self.word(cache + off, value)
            self.word(slot, cache)
            self.assertEqual(self.word(clz + 12), 0xc272b35)
            self.assertEqual(self.word(clz + 20), 0xc272ce9)
        self.display, self.roots = self.mem(1040), self.mem(64)
        # .043 native display-next literal points at uikit+8 (2010317c).
        self.word(0x2010317c, 1024)
        self.word(0x20103180, self.display)
        self.word(self.display + 1028, 0)  # native LL next after payload
        self.word(0x2010318c, self.display)
        self.word(self.display + 692, self.roots)

    def reg(self, i):
        return self.u.reg_read(REGS[i])

    def word(self, addr, value=None):
        if value is None:
            return struct.unpack('<I', self.u.mem_read(addr, 4))[0]
        self.u.mem_write(addr, struct.pack('<I', value))

    def mem(self, size=128):
        addr = self.next_memory
        self.next_memory += (size + 15) & ~15
        self.u.mem_write(addr, bytes(size))
        return addr

    def text(self, value):
        addr = self.mem(len(value) + 1)
        self.u.mem_write(addr, value.encode() + b'\0')
        return addr

    def trace(self, u, pc, size, cookie):
        self.executed.add(pc)
        if pc == SETTER:
            self.setters.append((self.reg(0), self.reg(1)))
        if pc in self.leaves:
            result = self.leaves[pc]()
            if result is not None:
                u.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
            u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
        elif not (0x1c700000 <= pc < 0x1c73fe00 or
                  any(lo <= pc < hi for lo, hi in NATIVE)):
            self.fail(f'unaudited AP path/global drop/owner teardown: {pc:#x}')

    def call(self, entry, *args):
        for i, reg in enumerate(REGS):
            self.u.reg_write(reg, args[i] if i < len(args) else 0)
        self.u.reg_write(UC_ARM_REG_SP, 0x20150000)
        self.u.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.u.emu_start((self.symbols[entry] if isinstance(entry, str) else entry) | 1,
                         STOP, count=1000000)
        self.assertEqual(self.u.reg_read(UC_ARM_REG_PC), STOP, 'instruction budget')
        return self.reg(0)

    def links(self, cache):
        nodes = self.nodes[cache]
        self.word(cache + 52, nodes[0] if nodes else 0)
        for i, node in enumerate(nodes):
            self.word(node + 8, nodes[i + 1] if i + 1 < len(nodes) else 0)

    def cached(self, cache, path=KEY, kind=1, refs=2):
        node, rb, data = self.mem(), self.mem(), self.mem()
        self.word(node, rb)
        self.word(rb + 16, data)
        off = 4 if cache == DATA else 0
        self.word(data + off, self.text(path))
        self.u.mem_write(data + off + 4, bytes([kind]))
        self.word(data + 28, refs)
        self.word(data + 32, 24)
        self.entries[data] = (cache, node)
        self.nodes[cache].append(node)
        self.links(cache)
        return node, data

    def lookup(self):
        cache, data = self.reg(0), self.reg(1)
        expected, node = self.entries[data]
        self.assertEqual(cache, expected)
        self.drops.append(data)
        self.nodes[cache].remove(node)
        self.nodes[cache].insert(0, node)
        self.links(cache)
        return data + 24

    def unlink(self):
        cache, entry = self.reg(0), self.reg(1)
        node = self.entries[entry - 24][1]
        self.nodes[cache].remove(node)
        self.links(cache)
        self.u.mem_write(node, b'\xff' * 16)
        return 0

    def view(self, destination=None):
        view = self.mem(12)
        if destination is not None:
            rules = self.mem(512)
            self.u.mem_write(rules, KEY.encode() + b'\0')
            self.u.mem_write(rules + 256, destination.encode() + b'\0')
            self.u.mem_write(view, struct.pack('<3I', rules, 1, 0))
        return view

    def image(self, path=KEY, kind=1, clz=IMAGE_CLASS):
        obj = self.mem()
        self.word(obj, clz)
        self.word(obj + 52, self.text(path))
        self.word(obj + 68, 10)
        self.word(obj + 72, 20)
        self.word(obj + 80, 256)
        self.word(obj + 84, 256)
        self.u.mem_write(obj + 96, bytes([kind]))
        return obj

    def screens(self, *objects):
        self.word(self.display + 720, len(objects))
        for i, obj in enumerate(objects):
            self.word(self.roots + i * 4, obj)

    def info(self):
        self.queries.append(self.reg(0))
        self.u.mem_write(self.reg(1), struct.pack('<BBHHHHH',
            0x19, 4, 0, *self.dimensions, 154, 0))
        return int(self.info_ok)

    def test_capstone_relevant_native_contract(self):
        md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        def instruction(pc):
            off = pc - 0xc0c0000
            i = next(md.disasm(self.firmware[off:off + 4], pc))
            return i.mnemonic, i.op_str
        for pc, expected in ((0xc1667e2, ('ldr', 'r3, [r3, #0xc]')),
                              (0xc166824, ('ldr', 'r3, [r3, #0x14]')),
                              (0xc166714, ('ldr', 'r6, [r6, #0x18]')),
                              (0xc17a438, ('ldr.w', 'r8, [r4, #0x34]')),
                              (0xc17a43c, ('cmp', 'r8, r5')),
                              (0xc13c6fe, ('bl', '#0xc13ccbc'))):
            self.assertEqual(instruction(pc), expected)

    def test_exact_key_retirement_and_native_last_release(self):
        retained, held, idle = [], [], []
        for cache in (HEADER, DATA):
            retained.append(self.cached(cache, '/data/app/org.other/images/icon.bin')[0])
            held.append((cache, self.cached(cache)[1]))  # non-head lookup
            for kind in (0, 2):
                retained.append(self.cached(cache, KEY, kind=kind)[0])
            retained.append(self.cached(cache, KEY + '.bak')[0])
            idle.append(self.cached(cache, refs=0)[1])
        view = self.view(THEME)
        self.assertEqual(self.call('rh_platform_retire_mapped_images', view), 0)
        self.assertEqual(self.drops, [held[0][1], idle[0], held[1][1], idle[1]])
        self.assertEqual(self.nodes[HEADER] + self.nodes[DATA], retained)
        self.assertEqual(self.freed, [(kind, data) for data in idle for kind in ('payload', 'entry')])
        for cache, data in held:
            self.assertEqual(bytes(self.u.mem_read(data + 36, 1)), b'\x01')
            for refs in (1, 0):
                self.call(RELEASE, cache, data + 24, 0)
                self.assertEqual(self.word(data + 28), refs)
                for kind in ('entry', 'payload'):
                    self.assertEqual((kind, data) in self.freed, refs == 0)
        self.assertTrue({DROP, RELEASE, 0xc166954, 0xc1669f0} <= self.executed)

    def test_live_same_pointer_after_retirement_and_restore(self):
        old = self.view(THEME)
        for cache in (HEADER, DATA):
            self.cached(cache)
        obj = self.image()
        others = [self.image('/data/app/org.other/images/icon.bin'),
                  self.image(kind=0), self.image(kind=2), self.image(clz=0)]
        self.screens(obj, *others)
        other_sources = [self.word(other + 52) for other in others]
        source = self.word(obj + 52)
        self.assertEqual(self.call('rh_platform_retire_mapped_images', old), 0)
        self.assertEqual(self.call('rh_platform_refresh_mapped_images', 0, old), 0)
        self.assertEqual(self.setters, [(obj, source)])
        self.assertEqual(self.queries, [source, source])
        self.assertEqual((self.word(obj + 68), self.word(obj + 72)), (77, 99))
        self.assertEqual(self.word(obj + 52), source)
        self.assertEqual(self.freed, [])
        self.dimensions = (10, 20)
        # Explicit system restore and complete removal both use the old view;
        # neither current view redirects this key. Same-pointer owner refresh
        # must still run so later decoder opens can observe the original file.
        for current, retirement_rc in ((self.view('@system'), 0), (self.view(), 1)):
            self.assertEqual(self.call('rh_platform_retire_mapped_images', current), retirement_rc)
            self.assertEqual(self.call('rh_platform_refresh_mapped_images', old, current), 0)
            self.assertEqual(self.word(obj + 52), source)
            self.assertEqual((self.word(obj + 68), self.word(obj + 72)), (10, 20))
        self.assertEqual(self.setters, [(obj, source)] * 3)
        self.assertEqual(self.queries, [source] * 6)
        self.assertEqual(self.freed, [])
        for other, original in zip(others, other_sources):
            self.assertEqual(self.word(other + 52), original)
            self.assertEqual((self.word(other + 68), self.word(other + 72)), (10, 20))
        self.assertTrue({DROP, WALK, SETTER, 0xc13bc70, 0xc13ccbc,
                         0xc17a43e, 0xc13fc54} <= self.executed)
        self.assertIn(obj, self.sizes)

    def test_bad_info_preserves_old_source_and_dimensions(self):
        obj = self.image()
        self.screens(obj)
        source = self.word(obj + 52)
        self.info_ok = False
        old, empty = self.view(THEME), self.view()
        self.assertEqual(self.call('rh_platform_refresh_mapped_images', old, empty), 0)
        self.assertEqual(self.queries, [source])
        self.assertEqual(self.word(obj + 52), source)
        self.assertEqual((self.word(obj + 68), self.word(obj + 72)), (10, 20))
        self.assertEqual(self.setters, [])
        self.assertEqual(self.invalidations, [])
        self.assertNotIn(SETTER, self.executed)
        self.assertIn(WALK, self.executed)


if __name__ == '__main__':
    unittest.main(verbosity=2)

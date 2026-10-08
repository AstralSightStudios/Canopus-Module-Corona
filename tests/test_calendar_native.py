"""Exact .177 read-only calendar receipts and source-compiled native adapter.

Native leaves model UI/clock/files/GPU, NOT physical pixels or GPU fault safety.
The stock C547870 instructions execute to compare default object attributes;
compiled replacement C executes for metadata, write, ownership and date guards.
Run with a Python providing unicorn, capstone, pyelftools; optionally set
RH_CALENDAR_FIRMWARE to the fingerprinted .177 vela_ap.bin.
"""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_R0,
    UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_PC,
    UC_ARM_REG_SP, UC_ARM_REG_LR)

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus'))
if not SDK.exists():
    SDK = ROOT.parent / 'Canopus-Private'
AP = Path(os.environ.get('RH_CALENDAR_FIRMWARE',
    SDK / 'fwbins/xiaomi-band-11-4.100.177/vela_ap.bin'))
SHA = 'ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b'
BASE, STOP = 0x0c0c0000, 0x1c73ff00
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
LEAVES = [0xc34d45c, 0xc34fcec, 0xc382ff0, 0xc3836d2, 0xc502de0,
    0xc3ae134, 0xc3b2b8c, 0xc3ab228, 0xc3b38ac, 0xc3839ae, 0xc383a10,
    0xc3839e4, 0xc38361c, 0xc50934c, 0xc495008, 0xc4924fc, 0xc542cdc,
    0xc6a29c8, 0xc6a2a24, 0xc384488, 0xc91e58c, 0xc92e5f4, 0xc721248,
    0xc907918, 0xc8b7720, 0xc38e44c]


class NativeCalendar(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not AP.exists():
            raise unittest.SkipTest('exact .177 AP unavailable; set RH_CALENDAR_FIRMWARE')
        cls.firmware = AP.read_bytes()
        if hashlib.sha256(cls.firmware).hexdigest() != SHA:
            raise RuntimeError('wrong .177 firmware fingerprint')
        cls.tmp = tempfile.TemporaryDirectory(prefix='rh-calendar-native-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        shim = tmp / 'io.c'
        shim.write_text('#include <stdint.h>\n'
            'int rh_platform_open(const char *p,int m){(void)p;(void)m;return -1;}\n'
            'int rh_platform_write(int f,const void *p,uint32_t n){(void)f;(void)p;(void)n;return -1;}\n'
            'void rh_platform_close(int f){(void)f;}\n')
        cls.images = {}
        for target in ('177', '139', '155', '1043'):
            objs = []
            for path in [ROOT / 'src/calendar_native.c', ROOT / 'src/calendar_config.c', shim]:
                obj = tmp / (target + '-' + path.stem + '.o')
                subprocess.run([os.environ.get('CLANG', shutil.which('clang')),
                    '--target=arm-none-eabi', '-mcpu=cortex-m33', '-mthumb',
                    '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
                    '-fno-stack-protector', '-fno-unwind-tables', '-Os',
                    '-Wall', '-Wextra', '-Werror', '-DRH_TARGET_' + target + '=1',
                    '-I' + str(ROOT / 'include'), '-c', str(path), '-o', str(obj)], check=True)
                objs.append(str(obj))
            elf = tmp / (target + '.elf')
            subprocess.run([os.environ.get('LD_LLD', shutil.which('ld.lld')),
                '-Ttext=0x1c700000', '-e', 'rh_calendar_native_render',
                *objs, '-o', str(elf)], check=True)
            with elf.open('rb') as stream:
                f = ELFFile(stream)
                cls.images[target] = ([(p['p_vaddr'], p.data()) for p in f.iter_segments()
                    if p['p_type'] == 'PT_LOAD' and p['p_vaddr'] >= 0x1c700000],
                    {s.name: s['st_value'] for s in f.get_section_by_name('.symtab').iter_symbols()})

    def setUp(self):
        self.machine()

    def machine(self, target='177'):
        self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
        for p, n in [(0xc000000, 0x1000000), (0x2c000000, 0x1000000),
                     (0x20000000, 0x200000), (0x3c700000, 0x200000),
                     (0x1c700000, 0x40000)]:
            self.u.mem_map(p, n)
        self.u.mem_write(BASE, self.firmware)
        self.u.mem_write(BASE + 0x20000000, self.firmware)
        segments, self.symbols = self.images[target]
        for p, data in segments:
            self.u.mem_write(p, data)
        self.next = 0x3c701000
        self.root = self.mem(128)
        self.image = self.mem(128)
        self.snap = self.mem(64)
        self.pixels = self.mem(448 * 112)
        self.config = self.mem(13092)
        self.date = self.mem(20)
        self.output = self.text('/data/quickapp/files/ng.lst.corona/calendar-alpha-0.bin')
        self.trace, self.objects, self.fonts, self.writes = [], {}, [], bytearray()
        self.labels, self.image_count = [], 0
        self.fences, self.write_results = [], []
        self.snapshot_ok, self.info_ok, self.alloc_ok = True, True, True
        self.locale = 'Wednesday'
        self.u.mem_write(self.date, struct.pack('<5i', 2026, 9, 23, 2, 0))
        self.metadata()
        self.call('rh_calendar_defaults', self.config)
        self.native_hooks = {}
        for address in LEAVES:
            self.native_hooks[address] = self.hook(address, lambda a=address: self.leaf(a))
        for name in ('rh_platform_open', 'rh_platform_write', 'rh_platform_close'):
            self.hook(self.symbols[name], lambda n=name: self.io(n))

    def mem(self, size):
        p = self.next
        self.next += (size + 15) & ~15
        self.u.mem_write(p, bytes(size))
        return p

    def text(self, value):
        p = self.mem(len(value.encode()) + 1)
        self.u.mem_write(p, value.encode() + b'\0')
        return p

    def string(self, p):
        return bytes(self.u.mem_read(p, 256)).split(b'\0')[0].decode()

    def word(self, p, value=None):
        if value is None:
            return struct.unpack('<I', self.u.mem_read(p, 4))[0]
        self.u.mem_write(p, struct.pack('<I', value & 0xffffffff))

    def metadata(self, cf=16, flags=0x31, width=112, height=112, stride=448,
                 size=448 * 112, pointer=None):
        self.u.mem_write(self.snap, struct.pack('<BB5HIII', 0, cf, flags, width,
            height, stride, 0, size, self.pixels if pointer is None else pointer, self.pixels))

    def hook(self, address, callback):
        def invoke(u, pc, size, cookie):
            result = callback()
            if result is not None:
                u.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
            u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
        return self.u.hook_add(UC_HOOK_CODE, invoke, None, address & ~1, address & ~1)

    def call(self, name, *args):
        pc = self.symbols[name] if isinstance(name, str) else name
        for r in REGS:
            self.u.reg_write(r, 0)
        for r, v in zip(REGS, args):
            self.u.reg_write(r, v)
        self.u.reg_write(UC_ARM_REG_SP, 0x201f0000)
        self.u.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.u.emu_start(pc | 1, STOP, count=1000000)
        self.assertEqual(self.u.reg_read(UC_ARM_REG_PC), STOP)
        return self.u.reg_read(UC_ARM_REG_R0)

    def render(self):
        return self.call('rh_calendar_native_render', self.config, self.date, self.output)

    def leaf(self, a):
        r = [self.u.reg_read(reg) for reg in REGS]
        self.trace.append((a, *r))
        if a == 0xc34d45c:
            self.u.mem_write(r[0], bytes(20)); return 0
        if a == 0xc34fcec:
            self.u.mem_write(r[1], struct.pack('<11i', 0, 0, 12, 23, 8, 126, 3, 0, 0, 0, 0)); return r[1]
        if a == 0xc382ff0:
            self.image_count = 0
            self.objects[self.root] = {'kind': 'root'}; return self.root
        if a == 0xc3ae134:
            self.image_count += 1
            p = self.image if self.image_count == 1 else self.mem(128)
            self.objects[p] = {'kind': 'image', 'parent': r[0]}; return p
        if a == 0xc3ab228:
            p = self.mem(128); self.labels.append(p)
            self.objects[p] = {'kind': 'label', 'parent': r[0]}; return p
        if a == 0xc3836d2:
            self.objects[r[0]]['size'] = (r[1], r[2])
        if a == 0xc3b2b8c:
            self.objects[r[0]]['source'] = self.string(r[1])
            self.word(r[0] + 52, r[1]); self.word(r[0] + 68, 64); self.word(r[0] + 72, 40)
        if a == 0xc3b38ac:
            self.objects[r[0]]['text'] = self.string(r[1])
        if a == 0xc3839ae:
            self.objects[r[0]]['color'] = r[1] & 0xffffff
        if a == 0xc383a10:
            self.objects[r[0]]['text-align'] = r[1]
        if a == 0xc3839e4:
            self.objects[r[0]]['font'] = r[1]
        if a == 0xc38361c:
            self.objects[r[0]]['align'] = tuple(r[1:])
        if a == 0xc50934c:
            return 0x20010000 if r[0] == 0x200c45dc else 0x20010100
        if a == 0xc495008:
            if not self.alloc_ok: return 0x2ca12930
            p = self.mem(64); self.fonts.append(p); return p
        if a == 0xc542cdc:
            self.u.mem_write(r[1], self.locale.encode()[:15] + b'\0')
        if a == 0xc6a29c8:
            self.assertEqual(r[:2], [self.image, 16] if not getattr(self, 'stock', False) else [self.image, 18])
            return self.snap if self.snapshot_ok else 0
        if a == 0xc91e58c:
            return self.fences.pop(0) if self.fences else 0
        if a == 0xc38e44c:
            if not self.info_ok: return 0
            self.u.mem_write(r[1], struct.pack('<BB5H', 25, 16, 0, 64, 40, 256, 0)); return 1
        return 0

    def io(self, name):
        r = [self.u.reg_read(reg) for reg in REGS]
        self.trace.append((name, *r))
        if name == 'rh_platform_open':
            self.assertEqual(self.string(r[0]), self.string(self.output))
            self.assertEqual(r[1], 0x426); return 7
        if name == 'rh_platform_write':
            self.assertEqual(r[0], 7)
            n = self.write_results.pop(0) if self.write_results else r[2]
            if 0 < n <= r[2]: self.writes.extend(self.u.mem_read(r[1], n))
            return n
        return 0

    def events(self, address):
        return [r for r in self.trace if r[0] == address]

    def test_defaults_match_exact_native_instructions_except_alpha_format(self):
        self.assertEqual(self.render(), 0)
        actual = list(self.objects.values())
        self.assertEqual(len(actual), 4)
        self.assertEqual([x['color'] for x in actual if x['kind'] == 'label'], [0, 0x3482ff])
        self.assertEqual(len(self.events(0xc495008)), 0, 'defaults must use shared style wrappers without ownership')
        self.assertEqual(len(self.events(0xc4924fc)), 0)
        self.assertEqual(self.writes[:12], struct.pack('<BB5H', 25, 16, 1, 112, 112, 448, 0))
        self.assertEqual(len(self.writes), 12 + 448 * 112)
        self.machine()
        self.stock = True
        md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        targets = {int(i.op_str.lstrip('#'), 16) for i in md.disasm(
            self.firmware[0xc547870 - BASE:0xc547a0e - BASE], 0xc547870) if i.mnemonic == 'bl'}
        def zero():
            p, _, size = [self.u.reg_read(x) for x in REGS[:3]]
            self.u.mem_write(p, bytes(size)); return p
        def format_text():
            p, size, fmt, value = [self.u.reg_read(x) for x in REGS]
            text = self.string(fmt).replace('%ld', str(value))
            self.u.mem_write(p, text.encode()[:size - 1] + b'\0'); return len(text)
        for a in targets - set(LEAVES):
            self.hook(a, zero if a == 0xc720f08 else format_text if a == 0xc34af78 else lambda: 0)
        self.hook(0xc4f87fc, lambda: self.objects[self.image].update(align=(9, 0, 0)) or 0)
        self.call(0xc547870)
        native = list(self.objects.values())
        # Handle numbers differ, so compare all native attributes except parent identity.
        normalize = lambda xs: [{k: v for k, v in x.items() if k != 'parent'} for x in xs]
        self.assertEqual(normalize(actual), normalize(native))

    def test_native_color_packer_and_argb_premultiply_bytes(self):
        # Execute .177 C3839AE, not a replacement: it packs the 3-byte color
        # as low16 + BYTE2 at high16 for property 88, with no R/B swap.
        self.u.hook_del(self.native_hooks[0xc3839ae])
        packed = []
        self.hook(0xc383434, lambda: packed.append(
            [self.u.reg_read(r) for r in REGS]) or 0)
        self.call(0xc3839ae, self.root, 0x3482ff, 0)
        self.assertEqual(packed, [[self.root, 88, 0x3482ff, 0]])
        self.assertEqual(struct.pack('<I', packed[0][2]), b'\xff\x82\x34\0')
        # C384C40 in lv_draw_buf conversion uses byte +3 alpha; native
        # premultiply is >>8, not divide by 255. Test unequal components.
        self.u.mem_write(self.pixels, b'\xff\x82\x34\x80')
        self.call(0xc384c40, self.pixels)
        self.assertEqual(bytes(self.u.mem_read(self.pixels, 4)), b'\x7f\x41\x1a\x80')

    def test_date_locale_and_bad_date_leave_output_unchanged(self):
        self.assertEqual(self.call('rh_calendar_native_date', self.date), 0)
        value = bytes(self.u.mem_read(self.date, 20))
        self.assertEqual(struct.unpack('<5I', value)[:4], (2026, 9, 23, 2))
        self.locale = 'Wed-localized'
        self.assertEqual(self.call('rh_calendar_native_date', self.date), 0)
        self.assertNotEqual(value[16:], bytes(self.u.mem_read(self.date + 16, 4)))
        self.word(self.date + 4, 2); self.word(self.date + 8, 30)
        self.trace.clear()
        self.assertNotEqual(self.render(), 0)
        self.assertEqual(self.trace, [])

    def test_failed_fence_keeps_tree_snapshot_fonts_until_next_success(self):
        self.word(self.config + 96 + 16, 37)
        self.fences = [0, 1]
        self.assertNotEqual(self.render(), 0)
        self.assertEqual(len(self.events(0xc495008)), 1)
        for a in (0xc384488, 0xc6a2a24, 0xc4924fc, 'rh_platform_open'):
            self.assertEqual(self.events(a), [])
        self.fences = [1]
        self.assertNotEqual(self.render(), 0)
        self.assertEqual(len(self.events(0xc495008)), 1, 'no unbounded pending generations')
        self.fences = [0, 0]
        self.assertEqual(self.render(), 0)
        self.assertEqual(len(self.events(0xc4924fc)), 2)
        self.assertEqual(len(self.events(0xc6a2a24)), 2)

    def test_malformed_snapshot_and_short_writes_never_claim_success(self):
        for values in [dict(cf=18), dict(width=111), dict(height=111),
                       dict(stride=447), dict(size=1), dict(pointer=0),
                       dict(flags=0x39), dict(flags=1), dict(flags=0x10)]:
            with self.subTest(values=values):
                self.machine(); self.metadata(**values)
                self.assertNotEqual(self.render(), 0)
                self.assertEqual(self.events('rh_platform_open'), [])
                self.assertEqual(len(self.events(0xc6a2a24)), 1)
        for results in ([12, 5, 0], [12, -1], [13]):
            with self.subTest(results=results):
                self.machine(); self.write_results = list(results)
                self.assertNotEqual(self.render(), 0)
                self.assertEqual(len(self.events('rh_platform_close')), 1)
        self.machine(); self.write_results = [4, 8, 8, 100]
        self.assertEqual(self.render(), 0)
        self.assertEqual(len(self.writes), 12 + 448 * 112)

    def test_images_keep_natural_sizes_and_missing_headers_refuse(self):
        # Day image config: context offsets packed u16, pool begins at +292.
        self.word(self.config + 96, 2); self.word(self.config + 96 + 28, 31)
        self.word(self.config + 96 + 16, 40)
        path = b'themes/day.bin\0'
        self.u.mem_write(self.config + 292, path); self.word(self.config + 288, len(path))
        self.assertEqual(self.render(), 0)
        custom = [x for x in self.objects.values() if x.get('source', '').endswith('themes/day.bin')][0]
        self.assertNotIn('size', custom)
        self.assertTrue(custom['source'].startswith('/data/quickapp/files/ng.lst.corona/'))
        self.trace.clear(); self.info_ok = False
        self.assertNotEqual(self.render(), 0)
        self.assertEqual(self.events('rh_platform_open'), [])

    def test_font_fallback_and_snapshot_failure_release_bounded_ownership(self):
        self.word(self.config + 96 + 16, 37)
        self.alloc_ok = False
        self.assertNotEqual(self.render(), 0)
        self.assertEqual(len(self.events(0xc495008)), 1)
        self.assertEqual(self.events(0xc4924fc), [], 'never destroy the system fallback')
        self.assertEqual(self.events('rh_platform_open'), [])
        self.machine(); self.snapshot_ok = False
        self.assertNotEqual(self.render(), 0)
        self.assertEqual(len(self.events(0xc384488)), 1)
        self.assertEqual(self.events(0xc6a2a24), [])
        self.assertEqual(self.events('rh_platform_open'), [])

    def test_clock_output_is_validated_not_saved_basepri_return(self):
        self.u.hook_del(self.native_hooks[0xc34d45c])
        def valid_clock():
            self.u.mem_write(self.u.reg_read(UC_ARM_REG_R0), struct.pack('<QI', 0, 0))
            return 0x80  # Native ABI returns prior BASEPRI, not success/failure.
        handle = self.hook(0xc34d45c, valid_clock)
        self.assertEqual(self.call('rh_calendar_native_date', self.date), 0)
        previous = bytes(self.u.mem_read(self.date, 20))
        self.u.hook_del(handle)
        # An unwritten timestamp must not be converted from zeroed storage.
        self.hook(0xc34d45c, lambda: 0)
        self.assertNotEqual(self.call('rh_calendar_native_date', self.date), 0)
        self.assertEqual(bytes(self.u.mem_read(self.date, 20)), previous)

    def test_wrong_receipt_is_failure_not_silent_unsupported(self):
        self.u.mem_write(0xc547870, b'\0\0\0\0')
        self.assertEqual(self.call('rh_calendar_native_supported'), 1)
        self.assertNotEqual(self.call('rh_calendar_native_date', self.date), 0)
        self.assertNotEqual(self.render(), 0)
        self.assertEqual(self.trace, [])

    def test_other_targets_make_no_native_calls(self):
        for target in ('139', '155'):
            with self.subTest(target=target):
                self.machine(target)
                self.assertEqual(self.call('rh_calendar_native_supported'), 0)
                self.assertNotEqual(self.call('rh_calendar_native_date', self.date), 0)
                self.assertNotEqual(self.render(), 0)
                self.assertEqual(self.trace, [])


if __name__ == '__main__':
    unittest.main(verbosity=2)

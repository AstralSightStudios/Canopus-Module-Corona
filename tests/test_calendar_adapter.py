"""Source-compiled calendar adapter guards for all exact targets.

Native lookup/notify/generation and cache unlink leaves are modeled here;
firmware_calendar.py separately executes the exact native AP instructions.
No device, GPU, file-write or renderer success is asserted.
"""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR)

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus-Private'))
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
STOP = 0x1c73ff00
TARGETS = {
    '139': (0x200bd200, 0x200bd310, 0x200bd314, 0x2ca168c4, 0x2ca16944, 0xc8b8cae),
    '155': (0x200bd200, 0x200bd310, 0x200bd314, 0x2ca168b4, 0x2ca16934, 0xc8b8c9e),
    '1043': (0x2010318c, 0x2010329c, 0x201032a0, 0x2cce56f4, 0x2cce571c, 0xc1667dc),
}


class CalendarAdapter(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='rh-calendar-adapter-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.images = {}
        clang = os.environ.get('CLANG', shutil.which('clang'))
        linker = os.environ.get('LD_LLD', shutil.which('ld.lld'))
        for version in TARGETS:
            target = ('xiaomi-band-10-pro-3.101.043' if version == '1043'
                      else 'xiaomi-band-11-4.100.' + version)
            objects = []
            for name in ('platform', 'resource_hook',
                         'platform_band10pro' if version == '1043' else 'platform_band11'):
                obj = Path(cls.tmp.name) / f'{version}-{name}.o'
                defines = ([] if version == '139' else [f'-DRH_TARGET_{version}=1'])
                subprocess.run([clang, '--target=arm-none-eabi', '-mcpu=cortex-m33',
                    '-mthumb', '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
                    '-fno-stack-protector', '-fno-unwind-tables', '-Os', '-Wall',
                    '-Wextra', '-Werror', *defines, '-I' + str(ROOT / 'include'),
                    '-I' + str(SDK / 'sdk/c'), '-I' + str(SDK / 'manager/target/band11'),
                    '-I' + str(SDK / 'targets' / target / 'generated'),
                    '-c', str(ROOT / 'src' / (name + '.c')), '-o', str(obj)], check=True)
                objects.append(str(obj))
            elf = Path(cls.tmp.name) / f'{version}.elf'
            subprocess.run([linker, '-Ttext=0x1c700000', '-e',
                'rh_platform_refresh_calendar', *objects, '-o', str(elf)], check=True)
            with elf.open('rb') as stream:
                f = ELFFile(stream)
                cls.images[version] = (
                    [(p['p_vaddr'], p.data()) for p in f.iter_segments()
                     if p['p_type'] == 'PT_LOAD' and p['p_vaddr'] >= 0x1c700000],
                    {s.name: s['st_value'] for s in f.get_section_by_name('.symtab').iter_symbols()})

    def machine(self, version):
        self.version = version
        self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
        for address, size in ((0xc000000, 0xe00000), (0x2c000000, 0xe00000),
                              (0x20000000, 0x160000), (0x3c000000, 0x1000000),
                              (0x1c700000, 0x40000)):
            self.u.mem_map(address, size)
        segments, self.symbols = self.images[version]
        for address, data in segments:
            self.u.mem_write(address, data)
        self.next_memory = 0x3c710000
        self.calls, self.drops = [], []
        display_slot, data_slot, header_slot, data_class, header_class, drop = TARGETS[version]
        self.display, self.data, self.header = self.mem(1024), self.mem(128), self.mem(128)
        self.word(display_slot, self.display)
        self.word(self.display + 696, self.mem())
        self.word(self.display + 608, 1)
        for slot, cache, clz in ((data_slot, self.data, data_class),
                                 (header_slot, self.header, header_class)):
            self.word(slot, cache)
            self.word(cache, clz)
            self.word(cache + 48, 4)
        self.background = ('/resource/app/perpetual_calendar/calendar_background_icon.bin'
                           if version == '1043' else
                           '/resource/app/perpetual_calendar/launcher_icon.bin')
        self.output = ('/resource/app/perpetual_calendar/launcher.bin' if version == '1043'
                       else '/data/app/perpetual_calendar/calendar_icon.bin')
        self.entries = {}
        self.hook(drop, self.drop)
        if version == '1043':
            self.word(0x200eb658, 0x2cdbb054)
            for offset, value in ((12, 0xca69e81), (16, 0xca69e55), (44, 0xca69aa1)):
                self.word(0x2cdbb054 + offset, value)
            self.app = self.mem(64)
            self.appid = self.text('com.xiaomi.miwear.perpetual_calendar')
            self.word(self.app + 8, self.appid)
            self.u.mem_write(self.app + 16, struct.pack('<H', 69))
            self.word(self.app + 56, 0xc4efde9)
            self.fallback = False
            self.hook(0xca69934, lambda: self.lookup(False))
            self.hook(0xca6996c, lambda: self.lookup(True))
            self.hook(0xca6a004, self.notify)
        else:
            self.word(0x200c6010, self.mem())
            self.hook(0xc5487a4, lambda: self.calls.append('generate') or 0)
            self.hook(0xc5486d0, lambda: self.calls.append('publish') or 0)

    def mem(self, size=128):
        p = self.next_memory
        self.next_memory += (size + 15) & ~15
        self.u.mem_write(p, bytes(size))
        return p

    def word(self, p, value=None):
        if value is None:
            return struct.unpack('<I', self.u.mem_read(p, 4))[0]
        self.u.mem_write(p, struct.pack('<I', value))

    def text(self, value):
        p = self.mem(len(value) + 1)
        self.u.mem_write(p, value.encode() + b'\0')
        return p

    def string(self, p):
        data = bytearray()
        for _ in range(256):
            byte = self.u.mem_read(p, 1)[0]
            if not byte:
                return data.decode()
            data.append(byte)
            p += 1
        self.fail('unterminated path')

    def reg(self, i):
        return self.u.reg_read(REGS[i])

    def hook(self, address, callback):
        def invoke(u, pc, size, cookie):
            value = callback()
            if value is not None:
                u.reg_write(UC_ARM_REG_R0, value & 0xffffffff)
            u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
        return self.u.hook_add(UC_HOOK_CODE, invoke, None, address, address)

    def call(self, name, *args):
        for reg in REGS:
            self.u.reg_write(reg, 0)
        for reg, value in zip(REGS, args):
            self.u.reg_write(reg, value)
        self.u.reg_write(UC_ARM_REG_SP, 0x20150000)
        self.u.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.u.emu_start(self.symbols[name] | 1, STOP, count=2000000)
        self.assertEqual(self.u.reg_read(UC_ARM_REG_PC), STOP)
        return self.reg(0)

    def lookup(self, other):
        self.assertEqual(self.reg(0), 69)
        self.calls.append('lookup-other' if other else 'lookup')
        return self.app if other == self.fallback else 0

    def notify(self):
        self.assertEqual(self.string(self.reg(0)), 'com.xiaomi.miwear.perpetual_calendar')
        self.calls.append('notify')
        return 0

    def cached(self, cache, path, offset, kind=1):
        node, rb, data = self.mem(), self.mem(), self.mem()
        self.word(node, rb)
        self.word(rb + 16, data)
        self.word(node + 8, self.word(cache + 52))
        self.word(cache + 52, node)
        self.word(data + offset, self.text(path))
        self.u.mem_write(data + offset + 4, bytes([kind]))
        self.entries[data] = (cache, node)
        return data

    def drop(self):
        cache, data = self.reg(0), self.reg(1)
        expected, node = self.entries[data]
        self.assertEqual(cache, expected)
        self.drops.append(data)
        cursor, previous = self.word(cache + 52), 0
        while cursor != node:
            previous, cursor = cursor, self.word(cursor + 8)
        self.word(previous + 8 if previous else cache + 52, self.word(node + 8))
        return 0

    def view(self, source=None):
        view = self.mem(12)
        if source:
            rules = self.mem(512)
            self.u.mem_write(rules, source.encode() + b'\0')
            root = ('/data/files/' if self.version == '1043' else '/data/quickapp/files/')
            self.u.mem_write(rules + 256,
                (root + 'ng.lst.corona/themes/calendar.bin').encode() + b'\0')
            self.word(view, rules)
            self.word(view + 4, 1)
        return view

    def test_affected_inputs_old_rules_and_unrelated_sources(self):
        for version in TARGETS:
            with self.subTest(version=version):
                self.machine(version)
                empty = self.view()
                for path in (self.background, self.output):
                    view = self.view(path)
                    self.assertEqual(self.call('rh_platform_calendar_affected', 0, view), 1)
                    self.assertEqual(self.call('rh_platform_calendar_affected', view, empty), 1)
                view = self.view('/resource/app/settings/launcher.bin')
                self.assertEqual(self.call('rh_platform_calendar_affected', 0, view), 0)

    def test_only_background_keys_retire_before_native_calls(self):
        for version in TARGETS:
            with self.subTest(version=version):
                self.machine(version)
                expected = []
                for cache, offset in ((self.header, 0), (self.data, 4)):
                    self.cached(cache, '/unrelated/icon.bin', offset)
                    expected.append(self.cached(cache, self.background, offset))
                    self.cached(cache, self.output, offset)
                    self.cached(cache, self.background, offset, kind=0)
                self.assertEqual(self.call('rh_platform_refresh_calendar'), 0)
                self.assertEqual(self.drops, expected)
                self.assertEqual(self.calls,
                    ['lookup', 'notify'] if version == '1043' else ['generate', 'publish'])

    def test_busy_display_and_invalid_cache_do_not_generate(self):
        for version in TARGETS:
            with self.subTest(version=version):
                self.machine(version)
                self.u.mem_write(self.display + 58, b'\2')
                self.assertEqual(self.call('rh_platform_refresh_calendar'), 0xffffffff)
                self.assertEqual(self.calls, [])
                self.u.mem_write(self.display + 58, b'\0')
                self.word(self.header, 0)
                self.assertEqual(self.call('rh_platform_refresh_calendar'), 0xffffffff)
                self.assertNotIn('notify', self.calls)
                self.assertNotIn('generate', self.calls)
                self.assertEqual(self.drops, [])

    def test_10pro_secondary_registry_and_owner_identity_guards(self):
        self.machine('1043')
        self.fallback = True
        self.assertEqual(self.call('rh_platform_refresh_calendar'), 0)
        self.assertEqual(self.calls, ['lookup', 'lookup-other', 'notify'])
        for fault in ('vtable', 'callback', 'id', 'appid-null', 'appid-invalid', 'appid-wrong', 'app-invalid'):
            with self.subTest(fault=fault):
                self.machine('1043')
                if fault == 'vtable':
                    self.word(0x200eb658, 0)
                elif fault == 'callback':
                    self.word(self.app + 56, 0)
                elif fault == 'id':
                    self.u.mem_write(self.app + 16, struct.pack('<H', 27))
                elif fault == 'appid-null':
                    self.word(self.app + 8, 0)
                elif fault == 'appid-invalid':
                    self.word(self.app + 8, 0xdead0000)
                elif fault == 'appid-wrong':
                    self.word(self.app + 8, self.text('com.xiaomi.miwear.calendar'))
                else:
                    self.app = 0xdead0000
                self.assertEqual(self.call('rh_platform_refresh_calendar'), 0)
                self.assertNotIn('notify', self.calls)
                self.assertEqual(self.drops, [])


if __name__ == '__main__':
    unittest.main(verbosity=2)

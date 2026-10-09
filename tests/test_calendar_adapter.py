"""Source-compiled calendar adapter guards for every exact target.

Only cache unlink, publisher and a refresh-owner stub are modeled. No native
renderer, generator, application notification, GPU or file I/O is executed.
"""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR)

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus-Private'))
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
STOP = 0x1c73ff00
# display slot, decoded/header slots, decoded/header classes, cache drop leaf
TARGETS = {
    '139': (0x200bd200, 0x200bd310, 0x200bd314, 0x2ca168c4, 0x2ca16944, 0xc8b8cae),
    '155': (0x200bd200, 0x200bd310, 0x200bd314, 0x2ca168b4, 0x2ca16934, 0xc8b8c9e),
    '177': (0x200bd0a4, 0x200bd1b4, 0x200bd1b8, 0x2ca14980, 0x2ca14a00, 0xc8b708a),
    '1043': (0x2010318c, 0x2010329c, 0x201032a0, 0x2cce56f4, 0x2cce571c, 0xc1667dc),
}
LAUNCHERS = {
    '139': (0x200c6010, 0xc5487a4, 0xc5486d0),
    '155': (0x200c6010, 0xc5487a4, 0xc5486d0),
    '177': (0x200c5d88, 0xc547870, 0xc54779c),
}
EXPORTS = ('rh_platform_refresh_calendar', 'rh_platform_calendar_retire',
           'rh_platform_calendar_publish', 'rh_platform_calendar_affected',
           'rh_calendar_parse', 'rh_calendar_defaults', 'test_config_size')


class CalendarAdapter(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='rh-calendar-adapter-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.images = {}
        clang = os.environ.get('CLANG', shutil.which('clang'))
        linker = os.environ.get('LD_LLD', shutil.which('ld.lld'))
        # The adapter must delegate to the owner, not implement refresh itself.
        # Use the actual config parser/context accessor, but never the renderer.
        fixture = Path(cls.tmp.name) / 'fixture.c'
        fixture.write_text('''#include "resource_hook_calendar.h"
volatile int test_refresh_result;
volatile unsigned test_refresh_calls;
volatile unsigned test_p67_publish_calls;
void rh_p67_calendar_publish(void) { ++test_p67_publish_calls; }
int rh_calendar_refresh(void) {
    ++test_refresh_calls;
    return test_refresh_result;
}
unsigned test_config_size(void) { return sizeof(struct rh_calendar_config); }
''')
        for version in TARGETS:
            target = ('xiaomi-band-10-pro-3.101.043' if version == '1043'
                      else 'xiaomi-band-11-4.100.' + version)
            objects = []
            sources = [ROOT / 'src' / (name + '.c') for name in
                       ('platform', 'resource_hook', 'calendar_config')]
            sources.append(fixture)
            for source in sources:
                obj = Path(cls.tmp.name) / f'{version}-{source.stem}.o'
                defines = ([] if version == '139' else [f'-DRH_TARGET_{version}=1'])
                subprocess.run([clang, '--target=arm-none-eabi', '-mcpu=cortex-m33',
                    '-mthumb', '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
                    '-fno-stack-protector', '-fno-unwind-tables',
                    '-ffunction-sections', '-fdata-sections', '-Os', '-Wall',
                    '-Wextra', '-Werror', *defines, '-I' + str(ROOT / 'include'),
                    '-I' + str(SDK / 'sdk/c'), '-I' + str(SDK / 'manager/target/band11'),
                    '-I' + str(SDK / 'targets' / target / 'generated'),
                    '-c', str(source), '-o', str(obj)], check=True)
                objects.append(str(obj))
            elf = Path(cls.tmp.name) / f'{version}.elf'
            subprocess.run([linker, '-Ttext=0x1c700000', '--gc-sections', '-e',
                'rh_platform_refresh_calendar',
                *[f'--undefined={name}' for name in EXPORTS],
                *objects, '-o', str(elf)], check=True)
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
        self.root = ('/data/files/' if version == '1043' else '/data/quickapp/files/') + 'ng.lst.corona/'
        self.file_keys = [self.output, self.root + 'calendar-alpha-0.bin',
                          self.root + 'calendar-alpha-1.bin']
        self.keys = [self.background] + ([] if version == '1043' else self.file_keys)
        self.entries = {}
        self.unlink = True
        self.hook(drop, self.drop)
        if version == '1043':
            # Even a live application registry must not be consulted/notified.
            self.word(0x200eb658, 0x2cdbb054)
            for address, label in ((0xca69934, 'lookup'), (0xca6996c, 'lookup-other'),
                                   (0xca6a004, 'notify')):
                self.hook(address, lambda label=label: self.calls.append(label) or 0)
        else:
            slot, generate, publish = LAUNCHERS[version]
            self.word(slot, self.mem())
            self.hook(generate, lambda: self.calls.append('generate') or 0)
            self.hook(publish, lambda: self.calls.append('publish') or 0)

    def mem(self, size=128):
        p = self.next_memory
        self.next_memory += (size + 15) & ~15
        self.u.mem_write(p, bytes(size))
        return p

    def word(self, p, value=None):
        if value is None:
            return struct.unpack('<I', self.u.mem_read(p, 4))[0]
        self.u.mem_write(p, struct.pack('<I', value & 0xffffffff))

    def text(self, value):
        p = self.mem(len(value.encode()) + 1)
        self.u.mem_write(p, value.encode() + b'\0')
        return p

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
        if not self.unlink:
            return 0
        cursor, previous = self.word(cache + 52), 0
        while cursor != node:
            previous, cursor = cursor, self.word(cursor + 8)
        self.word(previous + 8 if previous else cache + 52, self.word(node + 8))
        # Any subsequent access to retired list storage must not be needed.
        self.u.mem_write(node, b'\xdd' * 128)
        self.u.mem_write(data, b'\xdd' * 128)
        return 0

    def survivors(self):
        nodes = set()
        for cache in (self.header, self.data):
            node = self.word(cache + 52)
            while node:
                self.assertNotIn(node, nodes)
                nodes.add(node)
                node = self.word(node + 8)
        return {data for data, (_, node) in self.entries.items() if node in nodes}

    def seed_keys(self, paths):
        expected, unrelated = [], []
        for cache, offset in ((self.header, 0), (self.data, 4)):
            for path in paths:
                # Match path strings, not pointer identity; keep duplicate keys.
                expected.extend(self.cached(cache, path, offset) for _ in range(2))
                unrelated.append(self.cached(cache, path, offset, kind=0))
                unrelated.append(self.cached(cache, path + '.unrelated', offset))
            unrelated.append(self.cached(cache, '/unrelated/icon.bin', offset))
        return expected, unrelated

    def view(self, source=None):
        view = self.mem(12)
        if source:
            rules = self.mem(512)
            self.u.mem_write(rules, source.encode() + b'\0')
            self.u.mem_write(rules + 256, (self.root + 'themes/calendar.bin').encode() + b'\0')
            self.word(view, rules)
            self.word(view + 4, 1)
        return view

    def config(self, images=False):
        config = self.mem(self.call('test_config_size'))
        if not images:
            self.call('rh_calendar_defaults', config)
            return config, [], []
        source = 'version=1\n'
        image_paths, text_paths = [], []
        for item, kind, count in (('month', 'image', 12), ('day', 'text', 31),
                                  ('weekday', 'image', 7)):
            source += f'{item}\t{kind}\ttop_mid\t0\t0\t0\tnative\t{count}\n'
            for index in range(count):
                path = f'themes/{item}-{index}.bin'
                source += path + '\n'
                (image_paths if kind == 'image' else text_paths).append(self.root + path)
        self.assertEqual(self.call('rh_calendar_parse', self.text(source), len(source), config), 0)
        return config, image_paths, text_paths

    def test_affected_inputs_old_rules_and_unrelated_sources(self):
        for version in TARGETS:
            with self.subTest(version=version):
                self.machine(version)
                empty = self.view()
                for path in (self.background, self.output, '/resource/app/launcher/calendar.bin',
                             '/resource/font/MiSans-Regular-All.ttf'):
                    view = self.view(path)
                    self.assertEqual(self.call('rh_platform_calendar_affected', 0, view), 1)
                    self.assertEqual(self.call('rh_platform_calendar_affected', view, empty), 1)
                view = self.view('/resource/app/settings/launcher.bin')
                self.assertEqual(self.call('rh_platform_calendar_affected', 0, view), 0)

    def test_retire_exact_keys_and_optional_configured_images(self):
        for version in TARGETS:
            for mode in ('null', 'defaults', 'images'):
                with self.subTest(version=version, config=mode):
                    self.machine(version)
                    config, images, texts = ((0, [], []) if mode == 'null'
                                            else self.config(mode == 'images'))
                    expected, unrelated = self.seed_keys(self.keys + images)
                    for cache, offset in ((self.header, 0), (self.data, 4)):
                        for path in texts + [self.root + 'themes/not-configured.bin']:
                            unrelated.append(self.cached(cache, path, offset))
                    self.assertEqual(self.call('rh_platform_calendar_retire', config), 0)
                    self.assertCountEqual(self.drops, expected)
                    self.assertEqual(self.survivors(), set(unrelated))
                    self.assertEqual(self.calls, [])
                    self.assertEqual(self.word(self.symbols['test_refresh_calls']), 0)

    def test_p67_retirement_does_not_touch_stock_output_or_legacy_banks(self):
        self.machine('1043')
        expected, unrelated = self.seed_keys(self.keys)
        retained, extra = self.seed_keys(self.file_keys +
            ['/data/app/perpetual_calendar/calendar_icon.bin'])
        self.assertEqual(self.call('rh_platform_calendar_retire', 0), 0)
        self.assertCountEqual(self.drops, expected)
        self.assertEqual(self.survivors(), set(unrelated + retained + extra))

    def repeated_images(self, paths):
        config = self.mem(self.call('test_config_size'))
        source = 'version=1\n'
        for name, count in (('month', 12), ('day', 31), ('weekday', 7)):
            source += f'{name}\timage\ttop_mid\t0\t0\t0\tnative\t{count}\n'
            source += ''.join(paths[index % len(paths)] + '\n' for index in range(count))
        self.assertEqual(self.call('rh_calendar_parse', self.text(source), len(source), config), 0)
        return config

    def test_repeated_images_scan_once_per_unique_path_by_content(self):
        for version in TARGETS:
            for paths in (['themes/shared/a.bin'],
                          ['themes/shared/a.bin', 'themes/shared/b.bin']):
                with self.subTest(version=version, unique=len(paths)):
                    self.machine(version)
                    config = self.repeated_images(paths)
                    # No matches: count scan-start reads without unlink-check noise.
                    self.seed_keys(['/unrelated/icon.bin'])
                    heads = []
                    def read_head(u, access, address, size, value, cookie):
                        if address in (self.header + 52, self.data + 52):
                            heads.append(address)
                    handle = self.u.hook_add(UC_HOOK_MEM_READ, read_head)
                    self.assertEqual(self.call('rh_platform_calendar_retire', config), 0)
                    self.u.hook_del(handle)
                    # One validation + target-specific fixed keys + unique inputs.
                    scans = 1 + len(self.keys) + len(paths)
                    self.assertEqual(heads.count(self.header + 52), scans)
                    self.assertEqual(heads.count(self.data + 52), scans)
                    self.assertEqual(self.drops, [])
                    # Distinct pool offsets with equal strings must still retire
                    # every matching native entry, including duplicate cache keys.
                    self.machine(version)
                    config = self.repeated_images(paths)
                    expected, unrelated = self.seed_keys(self.keys + [self.root + p for p in paths])
                    self.assertEqual(self.call('rh_platform_calendar_retire', config), 0)
                    self.assertCountEqual(self.drops, expected)
                    self.assertEqual(self.survivors(), set(unrelated))

    def test_invalid_image_metadata_refuses_without_unrelated_cache_changes(self):
        for version in TARGETS:
            for fault in ('count', 'pool', 'offset', 'unterminated', 'prefix'):
                with self.subTest(version=version, fault=fault):
                    self.machine(version)
                    config = self.repeated_images(['themes/shared/a.bin'])
                    expected, unrelated = self.seed_keys(['/unrelated/icon.bin'])
                    # Layout: three 96-byte items, pool_used at +288, pool at +292.
                    if fault == 'count':
                        self.word(config + 28, 32)
                    elif fault == 'pool':
                        self.word(config + 288, 12797)
                    elif fault == 'offset':
                        self.u.mem_write(config + 32, struct.pack('<H', 0xffff))
                    elif fault == 'unterminated':
                        self.u.mem_write(config + 292, b'x' * self.word(config + 288))
                    else:
                        self.u.mem_write(config + 292, b'other/')
                    self.assertEqual(self.call('rh_platform_calendar_retire', config), 0xffffffff)
                    self.assertEqual(self.drops, [])
                    self.assertEqual(self.survivors(), set(expected + unrelated))

    def test_display_and_cache_guards_precede_any_retirement(self):
        faults = ('display-null', 'screen-null', 'busy', 'invalidation-off',
                  'invalidation-negative', 'data-null', 'header-null',
                  'data-class', 'header-class', 'data-node-size', 'header-node-size',
                  'data-rb-null', 'header-rb-null', 'data-entry-null', 'header-entry-null')
        for version in TARGETS:
            for fault in faults:
                with self.subTest(version=version, fault=fault):
                    self.machine(version)
                    expected, unrelated = self.seed_keys(self.keys)
                    display_slot, data_slot, header_slot, *_ = TARGETS[version]
                    if fault == 'display-null':
                        self.word(display_slot, 0)
                    elif fault == 'screen-null':
                        self.word(self.display + 696, 0)
                    elif fault == 'busy':
                        self.u.mem_write(self.display + 58, b'\2')
                    elif fault.startswith('invalidation-'):
                        self.word(self.display + 608, -1 if fault.endswith('negative') else 0)
                    else:
                        cache = self.data if fault.startswith('data-') else self.header
                        slot = data_slot if cache == self.data else header_slot
                        if fault.endswith('-null') and fault in ('data-null', 'header-null'):
                            self.word(slot, 0)
                        elif fault.endswith('-class'):
                            self.word(cache, 0)
                        elif fault.endswith('-node-size'):
                            self.word(cache + 48, 8)
                        elif fault.endswith('-rb-null'):
                            self.word(self.word(cache + 52), 0)
                        else:
                            self.word(self.word(self.word(cache + 52)) + 16, 0)
                    self.assertEqual(self.call('rh_platform_calendar_retire', 0), 0xffffffff)
                    self.assertEqual(self.drops, [])
                    self.assertEqual(self.survivors(), set(expected + unrelated))
                    self.assertEqual(self.calls, [])

    def test_failed_unlink_is_not_reported_as_success(self):
        for version in TARGETS:
            with self.subTest(version=version):
                self.machine(version)
                expected, unrelated = self.seed_keys(self.keys)
                self.unlink = False
                self.assertEqual(self.call('rh_platform_calendar_retire', 0), 0xffffffff)
                self.assertEqual(len(self.drops), 1)
                self.assertEqual(self.survivors(), set(expected + unrelated))
                self.assertEqual(self.calls, [])

    def test_publish_uses_exact_native_adapter_and_band11_launcher_guard(self):
        for version in TARGETS:
            for initialized in (False, True):
                with self.subTest(version=version, initialized=initialized):
                    self.machine(version)
                    expected, unrelated = self.seed_keys(self.keys)
                    if version in LAUNCHERS:
                        self.word(LAUNCHERS[version][0], self.mem() if initialized else 0)
                    self.call('rh_platform_calendar_publish')
                    self.assertEqual(self.calls, ['publish'] if initialized and version != '1043' else [])
                    if version == '1043':
                        self.assertEqual(self.word(self.symbols['test_p67_publish_calls']), 1)
                    self.assertEqual(self.drops, [])
                    self.assertEqual(self.survivors(), set(expected + unrelated))
                    self.assertEqual(self.word(self.symbols['test_refresh_calls']), 0)

    def test_refresh_forwards_once_and_propagates_owner_return(self):
        for version in TARGETS:
            with self.subTest(version=version):
                self.machine(version)
                expected, unrelated = self.seed_keys(self.keys)
                # Refresh is delegation even if the adapter's retirement guard
                # would fail; the owner decides readiness and transaction order.
                self.word(TARGETS[version][0], 0)
                self.word(self.header, 0)
                for count, result in enumerate((0, 1, -1, -2145, 73), 1):
                    self.word(self.symbols['test_refresh_result'], result)
                    self.assertEqual(self.call('rh_platform_refresh_calendar'), result & 0xffffffff)
                    self.assertEqual(self.word(self.symbols['test_refresh_calls']), count)
                    self.assertEqual(self.calls, [])
                    self.assertEqual(self.drops, [])
                    self.assertEqual(self.survivors(), set(expected + unrelated))


if __name__ == '__main__':
    unittest.main(verbosity=2)

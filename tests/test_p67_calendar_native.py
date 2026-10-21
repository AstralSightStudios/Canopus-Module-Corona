"""Source-compiled .043 calendar owner and native RAM publication model.

Clock, LVGL, buffers, GPU/cache leaves and application registry are modeled;
no pixels, firmware execution, device state or GPU fault recovery are claimed.
Receipt words are synthetic fixtures unless RH_AP_1043 supplies the exact AP.
"""
import hashlib
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
    UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_PC,
    UC_ARM_REG_SP, UC_ARM_REG_LR)

ROOT = Path(__file__).resolve().parents[1]
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
STOP = 0x1c73ff00
CLOCK, LOCALTIME, LOCALIZED = 0xc201fe0, 0xc4e35ac, 0xc1e5c38
ROOT_CREATE, SIZE, IMAGE_CREATE, IMAGE_SOURCE = 0xc5881e0, 0xc588628, 0xc588d58, 0xc587fa0
LABEL_CREATE, LABEL_TEXT, COLOR = 0xc588f30, 0xc589490, 0xc588a70
FONT_DAY, FONT_WEEK, FONT_SET = 0xc4bf87c, 0xc4bf898, 0xc588e78
ALIGN, TEXT_ALIGN = 0xc589188, 0xc588bf8
BUF_CREATE, SNAPSHOT, DELETE = 0xc587f38, 0xca5fb40, 0xc588de8
FENCE, CLEAN, INVALIDATE = 0xc187bb0, 0xc588f88, 0xc588448
LOOKUP, EVENTBUS, STOCK, NOTIFY = 0xca69e80, 0xca41584, 0xc4efde8, 0xca6a004
RECEIPTS = {STOCK: 0xd0002906, NOTIFY: 0x4e0eb5f8, CLOCK: 0xb085b5f0,
            LOCALTIME: 0xb082b580, SNAPSHOT: 0x41f0e92d, BUF_CREATE: 0xf000f85f}
EXPORTS = ('rh_calendar_native_date', 'rh_calendar_native_render',
           'rh_p67_calendar_publish', 'rh_p67_calendar_reset',
           'rh_calendar_commit', 'rh_calendar_poll', 'rh_calendar_refresh',
           'rh_calendar_pending', 'rh_calendar_redirect', 'rh_calendar_parse',
           'rh_calendar_defaults')


class P67Calendar(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='rh-p67-calendar-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        shim = tmp / 'fixture.c'
        shim.write_text('''#include "resource_hook_calendar_native.h"
#include "resource_hook_platform.h"
void rh_p67_calendar_publish(void);
void *rh_platform_alloc(uint32_t n) { (void)n; return 0; }
void rh_platform_free(void *p) { (void)p; }
uint32_t rh_platform_lock(void) { return 0; }
void rh_platform_unlock(uint32_t irq) { (void)irq; }
int rh_platform_redraw_ready(void) { return 1; }
int rh_platform_open(const char *p, int m) { (void)p; (void)m; return -1; }
int rh_platform_read(int fd, void *p, uint32_t n) { (void)fd; (void)p; (void)n; return -1; }
int rh_platform_write(int fd, const void *p, uint32_t n) { (void)fd; (void)p; return n; }
void rh_platform_close(int fd) { (void)fd; }
int rh_platform_calendar_retire(const struct rh_calendar_config *c) { (void)c; return 0; }
void rh_platform_calendar_publish(void) { rh_p67_calendar_publish(); }
''')
        objects = []
        for source in [ROOT / 'src' / (name + '.c') for name in
                       ('calendar_native', 'calendar_config', 'calendar_icon')] + [shim]:
            obj = tmp / (source.stem + '.o')
            subprocess.run([os.environ.get('CLANG', shutil.which('clang')),
                '--target=arm-none-eabi', '-mcpu=cortex-m33', '-mthumb',
                '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
                '-fno-stack-protector', '-fno-unwind-tables', '-ffunction-sections',
                '-fdata-sections', '-Os', '-Wall', '-Wextra', '-Werror',
                '-DRH_TARGET_1043=1', '-I' + str(ROOT / 'include'),
                '-c', str(source), '-o', str(obj)], check=True)
            objects.append(str(obj))
        elf = tmp / 'calendar.elf'
        subprocess.run([os.environ.get('LD_LLD', shutil.which('ld.lld')),
            '-Ttext=0x1c700000', '--gc-sections', '-e', 'rh_calendar_poll',
            *['--undefined=' + name for name in EXPORTS], *objects,
            '-o', str(elf)], check=True)
        with elf.open('rb') as stream:
            f = ELFFile(stream)
            cls.segments = [(p['p_vaddr'], p.data()) for p in f.iter_segments()
                            if p['p_type'] == 'PT_LOAD' and p['p_vaddr'] >= 0x1c700000]
            cls.symbols = {s.name: s['st_value'] for s in f.get_section_by_name('.symtab').iter_symbols()}
        cls.firmware = None
        if os.environ.get('RH_AP_1043'):
            cls.firmware = Path(os.environ['RH_AP_1043']).read_bytes()
            if hashlib.sha256(cls.firmware).hexdigest() != '519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec':
                raise RuntimeError('wrong exact .043 AP fingerprint')

    def setUp(self):
        self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
        for address, size in ((0xc000000, 0x1000000), (0x20000000, 0x200000),
                              (0x3c700000, 0x200000), (0x1c700000, 0x40000)):
            self.u.mem_map(address, size)
        if self.firmware:
            self.u.mem_write(0xc0c0000, self.firmware)
        else:
            for address, value in RECEIPTS.items():
                self.word(address, value)
        for address, data in self.segments:
            self.u.mem_write(address, data)
        self.next_memory = 0x3c701000
        self.trace, self.buffers, self.snapshots, self.deleted = [], [], [], []
        self.fences = []
        self.snapshot_ok, self.alloc_ok, self.root_ok = True, True, True
        self.open_ok, self.app_available, self.localtime_ok = True, True, True
        self.retire_calls, self.fail_retire_at = 0, 0
        self.reenter_at, self.nested_results = None, None
        self.now = (2026, 9, 23, 2)
        self.locale = 'Wednesday'
        self.app, self.stock_buf = self.mem(128), self.mem(64)
        self.word(self.app + 56, STOCK | 1)
        self.word(self.app + 12, self.stock_buf)
        self.date, self.config = self.mem(20), self.mem(13092)
        self.u.mem_write(self.date, struct.pack('<5I', *self.now, 0))
        self.call('rh_calendar_defaults', self.config)
        self.word(self.config + 13088, 1)
        self.output = self.text('/data/files/ng.lst.corona/calendar-alpha-0.bin')
        self.reentrant_config = self.mem(13092)
        self.call('rh_calendar_defaults', self.reentrant_config)
        for address in (CLOCK, LOCALTIME, LOCALIZED, ROOT_CREATE, SIZE, IMAGE_CREATE,
                        IMAGE_SOURCE, LABEL_CREATE, LABEL_TEXT, COLOR, FONT_DAY,
                        FONT_WEEK, FONT_SET, ALIGN, TEXT_ALIGN, BUF_CREATE,
                        SNAPSHOT, DELETE, FENCE, CLEAN, INVALIDATE, LOOKUP,
                        EVENTBUS, STOCK, NOTIFY):
            self.hook(address, lambda a=address: self.leaf(a))
        for name in ('rh_platform_open', 'rh_platform_write', 'rh_platform_close',
                     'rh_platform_calendar_retire'):
            if name in self.symbols:
                self.hook(self.symbols[name], lambda n=name: self.platform(n))

    def mem(self, size=128):
        address = self.next_memory
        self.next_memory += (size + 15) & ~15
        self.u.mem_write(address, bytes(size))
        return address

    def word(self, address, value=None):
        if value is None:
            return struct.unpack('<I', self.u.mem_read(address, 4))[0]
        self.u.mem_write(address, struct.pack('<I', value & 0xffffffff))

    def text(self, value):
        address = self.mem(len(value.encode()) + 1)
        self.u.mem_write(address, value.encode() + b'\0')
        return address

    def string(self, address):
        return bytes(self.u.mem_read(address, 256)).split(b'\0')[0].decode()

    def hook(self, address, callback):
        def invoke(u, pc, size, cookie):
            result = callback()
            if result is not None:
                u.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
            u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
        self.u.hook_add(UC_HOOK_CODE, invoke, None, address & ~1, address & ~1)

    def call(self, name, *args):
        pc = self.symbols[name] if isinstance(name, str) else name
        for register in REGS:
            self.u.reg_write(register, 0)
        for register, value in zip(REGS, args):
            self.u.reg_write(register, value)
        self.u.reg_write(UC_ARM_REG_SP, 0x201f0000)
        self.u.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.u.emu_start(pc | 1, STOP, count=1000000)
        self.assertEqual(self.u.reg_read(UC_ARM_REG_PC), STOP)
        return self.u.reg_read(UC_ARM_REG_R0)

    def leaf(self, address):
        args = [self.u.reg_read(r) for r in REGS]
        self.trace.append((address, *args))
        if address == self.reenter_at:
            self.reenter_at = None
            context = self.u.context_save()
            self.nested_results = (self.call('rh_calendar_poll'),
                self.call('rh_calendar_commit', self.reentrant_config))
            self.u.context_restore(context)
        if address == CLOCK:
            self.u.mem_write(args[1], struct.pack('<5I', 1, 0, 0, 0, 0))
        elif address == LOCALTIME:
            year, month, day, weekday = self.now
            self.u.mem_write(args[2], struct.pack('<11I', 0, 0, 12, day, month - 1,
                year - 1900, (weekday + 1) % 7, 0, 0, 0, 0))
            return args[2] if self.localtime_ok else 0
        elif address == LOCALIZED:
            return self.text(self.locale)
        elif address == ROOT_CREATE:
            return self.mem() if self.root_ok else 0
        elif address in (IMAGE_CREATE, LABEL_CREATE):
            return self.mem()
        elif address == BUF_CREATE:
            if not self.alloc_ok:
                return 0
            self.assertEqual(args[:3], [112, 112, 16])
            buffer = self.mem(32)
            pixels = self.mem(448 * 112)
            self.u.mem_write(buffer, struct.pack('<BB5HIII', 0, 16, 0x31, 112,
                112, 448, 0, 448 * 112, pixels, pixels))
            self.buffers.append(buffer)
            return buffer
        elif address == SNAPSHOT:
            self.assertEqual(args[1], 16)
            self.snapshots.append(args[2])
            return int(self.snapshot_ok)
        elif address == DELETE:
            self.deleted.append(args[0])
        elif address == FENCE:
            return self.fences.pop(0) if self.fences else 0
        elif address == LOOKUP:
            self.assertEqual(self.string(args[0]), 'com.xiaomi.miwear.perpetual_calendar')
            return self.app if self.app_available else 0
        elif address == EVENTBUS:
            self.assertEqual(args[:3], [43, self.app, 0])
        elif address == NOTIFY:
            self.assertEqual(self.word(self.app + 56), STOCK | 1)
            self.word(self.app + 12, self.stock_buf)
        elif address == FONT_DAY:
            return 0x20110000
        return 0

    def platform(self, name):
        args = [self.u.reg_read(r) for r in REGS]
        self.trace.append((name, *args))
        if name == 'rh_platform_open':
            self.assertEqual(args[1], 2)
            return 7 if self.open_ok else -1
        if name == 'rh_platform_write':
            return args[2]
        if name == 'rh_platform_calendar_retire':
            self.retire_calls += 1
            return -1 if self.retire_calls == self.fail_retire_at else 0
        return 0

    def render(self):
        return self.call('rh_calendar_native_render', self.config, self.date, self.output)

    def start_custom(self):
        self.assertEqual(self.call('rh_calendar_commit', self.config), 0)
        self.assertEqual(self.call('rh_calendar_pending'), 1)
        self.assertEqual(self.call('rh_calendar_poll'), 0)

    def events(self, address):
        return [entry for entry in self.trace if entry[0] == address]

    def test_render_publish_and_signal_forwarding(self):
        self.assertEqual(self.render(), 0)
        self.assertEqual(len(self.buffers), 1)
        self.call('rh_p67_calendar_publish')
        self.assertEqual(self.word(self.app + 12), self.snapshots[-1])
        self.assertEqual(len(self.events(EVENTBUS)), 1)
        hook = self.word(self.app + 56)
        self.assertNotEqual(hook, STOCK | 1)
        self.word(self.app + 12, 0)
        self.call(hook, self.app, 6)
        self.assertEqual(self.word(self.app + 12), self.snapshots[-1])
        self.assertEqual(self.events(STOCK), [])
        self.call(hook, self.app, 7)
        self.assertEqual(self.events(STOCK)[-1][1:3], (self.app, 7))

    def test_date_locale_updates_reuse_two_buffers(self):
        self.start_custom()
        first = self.word(self.app + 12)
        self.assertEqual(self.call('rh_calendar_pending'), 0)
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertEqual(len(self.snapshots), 1)
        self.now = (2026, 9, 24, 3)
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        second = self.word(self.app + 12)
        self.assertNotEqual(first, second)
        self.locale = 'Thu-localized'
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertEqual(self.word(self.app + 12), first)
        self.now = (2027, 1, 1, 4)
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertEqual(self.word(self.app + 12), second)
        self.assertEqual(len(self.buffers), 2)
        self.assertEqual(self.retire_calls, 2 * len(self.snapshots))

    def test_failed_render_keeps_published_buffer_and_retries(self):
        self.start_custom()
        previous = self.word(self.app + 12)
        self.now = (2026, 9, 24, 3)
        self.snapshot_ok = False
        self.assertEqual(self.call('rh_calendar_poll'), (-2156) & 0xffffffff)
        self.assertEqual(self.word(self.app + 12), previous)
        self.call(self.word(self.app + 56), self.app, 6)
        self.assertEqual(self.word(self.app + 12), previous)
        self.snapshot_ok = True
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertNotEqual(self.word(self.app + 12), previous)

    def test_default_configuration_restores_stock_callback_and_descriptor(self):
        self.start_custom()
        defaults = self.mem(13092)
        self.call('rh_calendar_defaults', defaults)
        self.assertEqual(self.call('rh_calendar_commit', defaults), 0)
        self.assertEqual(self.call('rh_calendar_pending'), 0)
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertEqual(self.word(self.app + 56), STOCK | 1)
        self.assertEqual(self.word(self.app + 12), self.stock_buf)
        self.assertEqual(len(self.events(NOTIFY)), 1)
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertEqual(len(self.events(NOTIFY)), 1)
        self.assertEqual(len(self.snapshots), 1)

    def test_initial_defaults_do_not_generate_or_hook(self):
        self.call('rh_calendar_defaults', self.config)
        self.assertEqual(self.call('rh_calendar_commit', self.config), 0)
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertEqual(self.call('rh_calendar_pending'), 0)
        self.assertEqual(self.trace, [])
        self.assertEqual(self.word(self.app + 56), STOCK | 1)

    def test_wrong_receipt_preserves_previous_publication(self):
        self.start_custom()
        previous = self.word(self.app + 12)
        self.word(STOCK, 0)
        self.now = (2026, 9, 24, 3)
        self.trace.clear()
        self.assertEqual(self.call('rh_calendar_poll'), (-2155) & 0xffffffff)
        self.assertEqual(self.render(), 0xffffffff)
        self.assertEqual(self.trace, [])
        self.assertEqual(self.word(self.app + 12), previous)

    def test_ram_render_needs_no_output_path_or_file_io(self):
        self.open_ok = False
        self.output = 0
        self.assertEqual(self.render(), 0)
        self.call('rh_p67_calendar_publish')
        self.assertEqual(self.word(self.app + 12), self.snapshots[-1])
        self.assertEqual([entry for entry in self.trace if isinstance(entry[0], str)], [])
        self.assertNotIn('rh_platform_write', self.symbols)
        self.assertFalse(any(b'calendar-alpha-' in data for _, data in self.segments))
        for path in ('data/app/perpetual_calendar/calendar_icon.bin',
                     'resource/app/perpetual_calendar/launcher.bin',
                     'data/files/ng.lst.corona/calendar-alpha-0.bin'):
            self.assertEqual(self.call('rh_calendar_redirect', self.text(path)), 0)

    def test_post_render_retirement_failure_cannot_leak_candidate_through_signal(self):
        self.start_custom()
        previous = self.word(self.app + 12)
        self.now = (2026, 9, 24, 3)
        self.fail_retire_at = self.retire_calls + 2
        self.assertEqual(self.call('rh_calendar_poll'), (-2154) & 0xffffffff)
        self.call(self.word(self.app + 56), self.app, 6)
        self.assertEqual(self.word(self.app + 12), previous)
        self.now = (2026, 9, 23, 2)
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertEqual(self.word(self.app + 12), previous)
        self.assertEqual(len(self.events(EVENTBUS)), 1)
        self.fail_retire_at = 0
        self.now = (2026, 9, 24, 3)
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertNotEqual(self.word(self.app + 12), previous)

    def test_missing_app_adopts_completed_ram_on_later_poll_without_rerender(self):
        self.app_available = False
        self.start_custom()
        self.assertEqual(self.events(EVENTBUS), [])
        self.assertEqual(len(self.snapshots), 1)
        self.app_available = True
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertEqual(self.word(self.app + 12), self.snapshots[-1])
        self.assertEqual(len(self.events(EVENTBUS)), 1)
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertEqual(len(self.events(EVENTBUS)), 1)
        self.assertEqual(len(self.snapshots), 1)

    def test_failed_completion_fence_retains_tree_until_successful_fence(self):
        self.start_custom()
        previous = self.word(self.app + 12)
        self.now = (2026, 9, 24, 3)
        before = len(self.deleted)
        self.fences = [0, 1]
        self.assertEqual(self.call('rh_calendar_poll'), (-2156) & 0xffffffff)
        self.assertEqual(len(self.deleted), before)
        self.assertEqual(self.word(self.app + 12), previous)
        self.fences = [1]
        self.assertEqual(self.call('rh_calendar_poll'), (-2156) & 0xffffffff)
        self.assertEqual(len(self.deleted), before)
        self.fences = [0, 0]
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertEqual(len(self.deleted), before + 2)
        self.assertNotEqual(self.word(self.app + 12), previous)

    def test_render_and_stock_reset_keep_owner_reserved_during_callbacks(self):
        self.reenter_at = SNAPSHOT
        self.start_custom()
        self.assertEqual(self.nested_results, ((-2154) & 0xffffffff,) * 2)
        self.assertEqual(self.call('rh_calendar_commit', self.reentrant_config), 0)
        self.reenter_at, self.nested_results = NOTIFY, None
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertEqual(self.nested_results, ((-2154) & 0xffffffff,) * 2)
        self.assertEqual(self.word(self.app + 12), self.stock_buf)
        self.assertEqual(self.call('rh_calendar_pending'), 0)

    def test_default_reset_waits_for_fence_and_retains_failed_tree(self):
        self.start_custom()
        previous, hook = self.word(self.app + 12), self.word(self.app + 56)
        self.now = (2026, 9, 24, 3)
        self.fences = [0, 1]
        self.assertEqual(self.call('rh_calendar_poll'), (-2156) & 0xffffffff)
        before = len(self.deleted)
        defaults = self.mem(13092)
        self.call('rh_calendar_defaults', defaults)
        self.assertEqual(self.call('rh_calendar_commit', defaults), 0)
        self.fences = [1]
        self.assertEqual(self.call('rh_calendar_poll'), (-2154) & 0xffffffff)
        self.assertEqual(len(self.deleted), before)
        self.assertEqual(self.word(self.app + 12), previous)
        self.assertEqual(self.word(self.app + 56), hook)
        self.assertEqual(self.events(NOTIFY), [])
        self.fences = [0]
        self.assertEqual(self.call('rh_calendar_poll'), 0)
        self.assertEqual(len(self.deleted), before + 1)
        self.assertEqual(self.word(self.app + 12), self.stock_buf)
        self.assertEqual(self.word(self.app + 56), STOCK | 1)

    def test_configured_image_paths_survive_render_stack_and_failed_fence(self):
        source = 'version=1\n'
        for item, count in (('month', 12), ('day', 31), ('weekday', 7)):
            source += f'{item}\timage\tcenter\t0\t0\t0\tnative\t{count}\n'
            source += ''.join(f'themes/{item}-{index}.bin\n' for index in range(count))
        self.assertEqual(self.call('rh_calendar_parse', self.text(source), len(source), self.config), 0)
        self.fences = [0, 1]
        self.assertEqual(self.render(), 0xffffffff)
        paths = [(entry[2], self.string(entry[2])) for entry in self.events(IMAGE_SOURCE)[1:]]
        self.assertEqual([value for _, value in paths], [
            '/data/files/ng.lst.corona/themes/month-8.bin',
            '/data/files/ng.lst.corona/themes/day-22.bin',
            '/data/files/ng.lst.corona/themes/weekday-2.bin'])
        self.u.mem_write(0x201ec000, b'\xdd' * 0x4000)
        self.assertEqual([self.string(pointer) for pointer, _ in paths],
                         [value for _, value in paths])
        before = len(self.deleted)
        self.fences = [1]
        self.assertEqual(self.render(), 0xffffffff)
        self.assertEqual(len(self.deleted), before)
        self.assertEqual([self.string(pointer) for pointer, _ in paths],
                         [value for _, value in paths])
        self.fences = [0, 0]
        self.assertEqual(self.render(), 0)
        self.assertEqual(len(self.deleted), before + 2)

    def test_malformed_ram_metadata_never_publishes(self):
        self.start_custom()
        previous = self.word(self.app + 12)
        self.assertEqual(self.render(), 0)
        candidate = self.snapshots[-1]
        original = bytes(self.u.mem_read(candidate, 24))
        for offset, value in ((1, b'\x12'), (4, struct.pack('<H', 111)),
                              (6, struct.pack('<H', 111)), (8, struct.pack('<H', 447)),
                              (12, struct.pack('<I', 1)), (16, bytes(4)),
                              (16, struct.pack('<I', 3))):
            with self.subTest(offset=offset, value=value):
                self.u.mem_write(candidate, original)
                self.u.mem_write(candidate + offset, value)
                self.assertEqual(self.render(), 0xffffffff)
                self.call(self.word(self.app + 56), self.app, 6)
                self.assertEqual(self.word(self.app + 12), previous)
        self.u.mem_write(candidate, original)
        self.assertEqual(self.render(), 0)
        self.call('rh_p67_calendar_publish')
        self.assertNotEqual(self.word(self.app + 12), previous)

    def test_initial_allocation_and_fence_failures_do_not_publish(self):
        for fault in ('allocation', 'fence', 'root'):
            with self.subTest(fault=fault):
                self.setUp()
                self.alloc_ok = fault != 'allocation'
                self.root_ok = fault != 'root'
                self.fences = [1] if fault == 'fence' else []
                self.assertEqual(self.render(), 0xffffffff)
                self.call('rh_p67_calendar_publish')
                self.assertEqual(self.word(self.app + 12), self.stock_buf)
                self.assertEqual(self.word(self.app + 56), STOCK | 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)

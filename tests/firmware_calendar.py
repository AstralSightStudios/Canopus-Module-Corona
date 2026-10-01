"""Exact AP calendar ABI probes; no signed loading, GPU or device acceptance.

Run with build/firmware-tests/bin/python tests/firmware_calendar.py
  --target {139,155,1043} --firmware PATH
PATH may be an AP image or an OTA ZIP containing vela_ap.bin. The default is
CANOPUS_ROOT/fwbins/<exact-target>/vela_ap.bin. Every run checks SHA256 before
executing instructions. GUI, clock, formatting, heap, string-comparison and
file-writer leaves are modeled. Generator/publisher (11) and native name-map
lookup, notify, by-id lookup, dispatcher and callback (10 Pro) execute selected
firmware instructions, not compiled replacement C.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import unittest
import zipfile

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_R0,
    UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP,
    UC_ARM_REG_LR, UC_ARM_REG_PC)

TARGETS = {
    '139': ('xiaomi-band-11-4.100.139',
            '31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74'),
    '155': ('xiaomi-band-11-4.100.155',
            'ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f'),
    '1043': ('xiaomi-band-10-pro-3.101.043',
             '519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec'),
}
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
BASE, ALIAS, STOP = 0x0c0c0000, 0x2c0c0000, 0x1c73ff00
BACKGROUND = '/resource/app/perpetual_calendar/launcher_icon.bin'
OUTPUT = '/data/app/perpetual_calendar/calendar_icon.bin'
SELECTED, FIRMWARE = '139', None


class CalendarProbe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        target, fingerprint = TARGETS[SELECTED]
        root = Path(__file__).resolve().parents[1]
        sdk = Path(os.environ.get('CANOPUS_ROOT', root.parent / 'Canopus'))
        if not sdk.exists():
            sdk = root.parent / 'Canopus-Private'
        path = Path(FIRMWARE) if FIRMWARE else sdk / 'fwbins' / target / 'vela_ap.bin'
        if zipfile.is_zipfile(path):
            with zipfile.ZipFile(path) as archive:
                cls.firmware = archive.read('vela_ap.bin')
        else:
            cls.firmware = path.read_bytes()
        digest = hashlib.sha256(cls.firmware).hexdigest()
        if digest != fingerprint:
            raise RuntimeError(f'{target}: firmware fingerprint mismatch: {digest}')
        print(f'Exact AP: {target}, SHA256 {digest}', flush=True)

    def setUp(self):
        self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
        size = (len(self.firmware) + 4095) & ~4095
        for address, count in [(BASE, size), (ALIAS, size),
                               (0x20000000, 0x200000), (0x3c700000, 0x100000),
                               (0x1c700000, 0x40000)]:
            self.u.mem_map(address, count)
        self.u.mem_write(BASE, self.firmware)
        self.u.mem_write(ALIAS, self.firmware)
        self.next_memory = 0x3c701000
        self.trace = []
        self.executed = set()
        self.u.hook_add(UC_HOOK_CODE, lambda u, pc, size, data: self.executed.add(pc))

    def reg(self, index):
        return self.u.reg_read(REGS[index])

    def word(self, address, value=None):
        if value is None:
            return struct.unpack('<I', self.u.mem_read(address, 4))[0]
        self.u.mem_write(address, struct.pack('<I', value))

    def mem(self, size=128):
        address = self.next_memory
        self.next_memory += (size + 15) & ~15
        self.u.mem_write(address, bytes(size))
        return address

    def text(self, address):
        result = bytearray()
        for offset in range(1024):
            value = self.u.mem_read(address + offset, 1)[0]
            if not value:
                return result.decode()
            result.append(value)
        self.fail('unterminated native string')

    def hook(self, address, callback):
        def run(u, pc, size, data):
            result = callback()
            if result is not None:
                u.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
            u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
        self.u.hook_add(UC_HOOK_CODE, run, None, address & ~1, address & ~1)

    def call(self, address, *args):
        for reg in REGS:
            self.u.reg_write(reg, 0)
        for reg, value in zip(REGS, args):
            self.u.reg_write(reg, value)
        self.u.reg_write(UC_ARM_REG_SP, 0x201f0000)
        self.u.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.u.emu_start(address | 1, STOP, count=200000)
        self.assertEqual(self.u.reg_read(UC_ARM_REG_PC), STOP, 'instruction budget exhausted')
        return self.reg(0)

    def gui_leaves(self, start, end, special):
        """Stub only direct BL leaf targets in the audited native GUI body.

        The function under test and all its branches remain native. Special
        leaves record the relevant AAPCS arguments and order, not pixel output.
        """
        md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        targets = {int(i.op_str.lstrip('#'), 16) for i in md.disasm(
            self.firmware[start - BASE:end - BASE], start) if i.mnemonic == 'bl'}
        for address in targets:
            self.hook(address, special.get(address, lambda: 0))
        self.assertTrue(set(special).issubset(targets))

    def test_native_calendar(self):
        if SELECTED == '1043':
            self.probe_notify()
        else:
            self.probe_generator_publish()

    def probe_generator_publish(self):
        root, image, label, snapshot = [self.mem() for _ in range(4)]
        def size():
            self.trace.append(('size', self.reg(0), self.reg(1), self.reg(2)))
            return 0
        def image_source():
            self.trace.append(('source', self.reg(0), self.text(self.reg(1))))
            return 0
        def format_text():
            text = self.text(self.reg(2)).replace('%d', str(self.reg(3)))
            raw = text.encode()[:self.reg(1) - 1] + b'\0'
            self.u.mem_write(self.reg(0), raw)
            return len(raw) - 1
        def zero():
            self.u.mem_write(self.reg(0), bytes(self.reg(2)))
            return self.reg(0)
        def localtime():
            self.u.mem_write(self.reg(1), struct.pack('<11I', 0, 0, 12, 23, 8, 126, 3, 0, 0, 0, 0))
            return self.reg(1)
        self.snapshot_ready = True
        def take_snapshot():
            self.assertEqual((self.reg(0), self.reg(1)), (image, 0x12))
            self.trace.append(('snapshot', image))
            return snapshot if self.snapshot_ready else 0
        def write():
            self.assertEqual(self.reg(0), snapshot)
            self.assertEqual(self.text(self.reg(1)), OUTPUT)
            self.trace.append(('write', snapshot, OUTPUT))
            return 0
        # Exact call targets from each fingerprinted AP, not a range delta.
        zero_pc, snapshot_pc, free_pc = {
            '139': (0xc720ea8, 0xc6a4600, 0xc6a465c),
            '155': (0xc720e98, 0xc6a45f0, 0xc6a464c),
        }[SELECTED]
        self.gui_leaves(0xc5487a4, 0xc548942, {
            zero_pc: zero, 0xc34b028: format_text,
            0xc34fdfc: localtime, 0xc380686: lambda: root,
            0xc3ae1d0: lambda: image, 0xc3ab300: lambda: label,
            0xc387ba8: size, 0xc3b2c28: image_source,
            snapshot_pc: take_snapshot, 0xc4fba8c: write,
            free_pc: lambda: self.trace.append(('free', self.reg(0))) or 0,
            0xc384e6c: lambda: self.trace.append(('delete', self.reg(0))) or 0,
        })
        self.call(0xc5487a4)
        self.assertIn(('source', image, BACKGROUND), self.trace)
        self.assertIn(('size', root, 112, 112), self.trace)
        self.assertIn(('size', image, 112, 112), self.trace)
        self.assertLess(self.trace.index(('source', image, BACKGROUND)),
                        self.trace.index(('snapshot', image)))
        self.assertLess(self.trace.index(('snapshot', image)),
                        self.trace.index(('write', snapshot, OUTPUT)))
        self.assertLess(self.trace.index(('write', snapshot, OUTPUT)),
                        self.trace.index(('free', snapshot)))
        self.snapshot_ready = False
        self.trace.clear()
        self.call(0xc5487a4)
        self.assertIn(('delete', root), self.trace)
        self.assertFalse(any(row[0] in ('write', 'free') for row in self.trace))

        # The service returns a runtime list: stride at +0, head at +4;
        # each next pointer lives at node + stride + 4. Native traversal must
        # skip an unrelated id before finding calendar id 69.
        service, table, listing, other, calendar = [self.mem() for _ in range(5)]
        self.word(0x200c6010, service)
        self.word(service, table | 1)
        self.hook(table, lambda: self.word(self.reg(0), listing) or 0)
        self.word(listing, 28)
        self.word(listing + 4, other)
        self.u.mem_write(other, struct.pack('<H', 12))
        self.word(other + 32, calendar)
        self.u.mem_write(calendar, struct.pack('<H', 69))
        self.word(calendar + 20, root)
        self.hook(0xc37f960, lambda: 1)
        self.hook(0xc380320, lambda: image if self.reg(0) == root else label)
        before = len(self.trace)
        self.call(0xc5486d0)
        self.assertEqual(self.trace[before:], [('source', label, OUTPUT)])
        self.u.mem_write(calendar + 24, b'\x01')
        before = len(self.trace)
        self.call(0xc5486d0)
        self.assertEqual(len(self.trace), before, 'hidden calendar must not publish')
        self.u.mem_write(calendar + 24, b'\x00')
        self.u.mem_write(calendar, struct.pack('<H', 70))
        before = len(self.trace)
        self.call(0xc5486d0)
        self.assertEqual(len(self.trace), before)
        self.assertTrue({0xc5487a4, 0xc5486d0, 0xc548772}.issubset(self.executed))

    def probe_notify(self):
        # These are the native init vtable and by-id wrappers, not invented
        # function slots. Hash-map storage is modeled, but lookup/hash control
        # flow and the separate circular-list by-id routines execute natively.
        vtable = 0x2cdbb054
        self.word(0x200eb658, vtable)
        self.assertEqual(self.word(vtable + 12), 0xca69e81)
        self.assertEqual(self.word(vtable + 16), 0xca69e55)
        self.assertEqual(self.word(vtable + 44), 0xca69aa1)
        app, first, second = [self.mem() for _ in range(3)]
        for head in (first, second):
            self.word(head + 4, head)
        self.word(0x200eb640, first)
        self.word(0x200eb644, second)
        self.word(first + 4, app)
        self.word(app + 4, first)
        appname = 'com.xiaomi.miwear.perpetual_calendar'
        appid = self.mem(len(appname) + 1)
        self.u.mem_write(appid, appname.encode() + b'\0')
        self.u.mem_write(app + 16, struct.pack('<H', 69))
        self.word(app + 8, appid)
        self.word(app + 56, 0xc4efde9)
        self.assertEqual(self.call(0xca69934, 69), app)
        self.assertEqual(self.call(0xca69934, 70), 0)
        self.assertEqual(self.call(0xca6996c, 69), 0)
        self.word(first + 4, first)
        self.word(second + 4, app)
        self.word(app + 4, second)
        self.assertEqual(self.call(0xca6996c, 69), app)
        primary, secondary, node, collision = [self.mem() for _ in range(4)]
        self.word(0x200eb650, primary)
        self.word(0x200eb654, secondary)
        buckets = 8
        for table in (primary, secondary):
            self.word(table, buckets)
        keyhash = 5381
        for value in appname.encode():
            keyhash = (keyhash * 33 + value) & 0xffffffff
        slot = 4 + 4 * (keyhash & (buckets - 1))
        # Native layout: hash, key, strlen+1, value, next. An unrelated
        # chain head forces native next traversal before the matching node.
        self.u.mem_write(node, struct.pack('<5I', keyhash, appid,
                                           len(appname) + 1, app, 0))
        self.u.mem_write(collision, struct.pack('<5I', keyhash ^ buckets,
                                                appid, len(appname) + 1, app, node))
        self.word(secondary + slot, collision)
        self.name_lookups = []
        def observe_lookup(u, pc, size, data):
            self.assertIn(self.reg(0), (primary, secondary))
            self.assertEqual(self.text(self.reg(1)), appname)
            self.name_lookups.append(self.reg(0))
        # Observe entry without replacing it: native LDRB hashes the real
        # string pointer and performs bucket/node traversal at 0xcabe234.
        self.u.hook_add(UC_HOOK_CODE, observe_lookup, None, 0xcabe234, 0xcabe234)
        self.hook(0xcac0a48, lambda: (self.text(self.reg(0)) > self.text(self.reg(1))) -
                  (self.text(self.reg(0)) < self.text(self.reg(1))))
        # Exercise native callback failure and success control flow. Opaque
        # GUI handles and raster bytes are synthetic: no GPU rendering occurs.
        pixels, container, image, label = [self.mem() for _ in range(4)]
        self.pixel_ready = False
        def allocate_pixels():
            self.trace.append(('allocate', self.reg(0), self.reg(1)))
            return pixels if self.pixel_ready else 0
        def rasterize():
            self.assertEqual((self.reg(0), self.reg(2)), (image, pixels))
            self.trace.append(('raster', image, pixels))
            return 1
        def zero_text():
            self.u.mem_write(self.reg(0), bytes(self.reg(2)))
            return self.reg(0)
        self.word(0x2010f05c, 0)
        self.gui_leaves(0xc4efde8, 0xc4effe2, {
            0xc587f38: allocate_pixels, 0xca5fb40: rasterize,
            0xc5881e0: lambda: container, 0xc588d58: lambda: image,
            0xc588f30: lambda: label, 0xc588980: zero_text,
        })
        self.u.hook_add(UC_HOOK_CODE,
            lambda u, pc, size, data: self.trace.append(('signal', self.reg(0), self.reg(1))),
            None, 0xc4efde8, 0xc4efde8)
        self.hook(0xca41584, lambda: self.trace.append(
            ('event', self.reg(0), self.reg(1), self.reg(2))) or 0)
        self.call(0xca6a004, appid)
        self.assertEqual(self.trace[0], ('signal', app, 6))
        self.assertEqual(self.trace[-1], ('event', 0x2b, app, 1))
        self.assertIn(('allocate', 112, 112), self.trace)
        self.assertTrue({0xca6a004, 0xca69e80, 0xca69e54, 0xca69aa0,
                         0xc4efde8, 0xca69934, 0xca6996c}.issubset(self.executed))
        self.assertEqual(self.name_lookups, [primary, secondary])
        self.assertTrue({0xcabe234, 0xcabe24c, 0xcabe280, 0xcabe294}.issubset(self.executed))
        self.word(secondary + slot, 0)
        self.word(primary + slot, collision)
        self.name_lookups.clear()
        self.trace.clear()
        self.call(0xca6a004, appid)
        self.assertEqual(self.trace[0], ('signal', app, 6))
        self.assertEqual(self.trace[-1], ('event', 0x2b, app, 1))
        self.assertEqual(self.name_lookups, [primary])
        self.pixel_ready = True
        self.trace.clear()
        self.call(0xca6a004, appid)
        self.assertEqual(self.word(app + 12), pixels)
        self.assertEqual(self.trace[0], ('signal', app, 6))
        self.assertIn(('raster', image, pixels), self.trace)
        self.assertEqual(self.trace[-1], ('event', 0x2b, app, 1))
        self.word(app + 8, 0)
        self.trace.clear()
        self.call(0xca6a004, appid)
        self.assertEqual(self.trace, [], 'app without name metadata must not dispatch')
        self.word(app + 8, appid)
        self.word(primary + slot, 0)
        self.name_lookups.clear()
        self.trace.clear()
        self.call(0xca6a004, appid)
        self.assertEqual(self.name_lookups, [primary, secondary])
        self.assertEqual(self.trace, [], 'absent app must not dispatch signal or event')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=[*TARGETS, *(v[0] for v in TARGETS.values())],
                        default=os.environ.get('RESOURCE_HOOK_TARGET', '139'))
    parser.add_argument('--firmware', default=os.environ.get('RESOURCE_HOOK_FIRMWARE'))
    args, remaining = parser.parse_known_args()
    SELECTED = next((key for key, value in TARGETS.items() if value[0] == args.target), args.target)
    if SELECTED not in TARGETS:
        parser.error('unsupported exact target')
    FIRMWARE = args.firmware
    unittest.main(argv=[__file__, *remaining])

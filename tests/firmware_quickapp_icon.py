"""Compile the production adapter and execute exact .139/.155 AP lookup code.

Run: build/firmware-tests/bin/python tests/firmware_quickapp_icon.py
CLANG/LD_LLD and CANOPUS_ROOT may be overridden as in the calendar tests.
No device, rendering or file-existence claim: registry/records are synthetic;
lookup wrapper, hashmap lookup/hash/bucket search are real firmware. The known
strcmp AP thunk is a modeled leaf: it targets ROM 0x0026c095, absent from the AP.
The .043 stub is compiled and exercised without any firmware memory access.
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
from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS,
                     UC_HOOK_CODE, UC_HOOK_MEM_WRITE)
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_R0,
    UC_ARM_REG_R1, UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR)

ROOT = Path(__file__).resolve().parents[1]
STOP = 0x1c73ff00
PACKAGE = 'org.example.clock'
PATH = '/data/quickapp/app/' + PACKAGE + '/images/icon.bin'
# Independently checked IDBs, including extract155 rather than the small IDB.
TARGETS = {
    '139': (0x2ca3da68, 0xc6a16a2, 0xc6ac6c0, 0xc6d2d84, 0xc6d2698,
            0xc720f48, '31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74'),
    '155': (0x2ca3da58, 0xc6a1692, 0xc6ac6b0, 0xc6d2d74, 0xc6d2688,
            0xc720f38, 'ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f'),
}


class QuickAppIconNative(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='rh-quickapp-icon-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.images, cls.firmware = {}, {}
        clang = os.environ.get('CLANG', shutil.which('clang'))
        linker = os.environ.get('LD_LLD', shutil.which('ld.lld'))
        for version in (*TARGETS, '1043'):
            obj = Path(cls.tmp.name) / (version + '.o')
            defines = [] if version == '139' else [f'-DRH_TARGET_{version}=1']
            subprocess.run([clang, '--target=arm-none-eabi', '-mcpu=cortex-m33',
                '-mthumb', '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
                '-fno-stack-protector', '-fno-unwind-tables', '-Os', '-Wall',
                '-Wextra', '-Werror', *defines, '-I' + str(ROOT / 'include'),
                '-c', str(ROOT / 'src/quickapp_icon.c'), '-o', str(obj)], check=True)
            elf = Path(cls.tmp.name) / (version + '.elf')
            subprocess.run([linker, '-Ttext=0x1c700000', '-e',
                'rh_platform_quickapp_icon_path', str(obj), '-o', str(elf)], check=True)
            with elf.open('rb') as stream:
                f = ELFFile(stream)
                cls.images[version] = (
                    [(p['p_vaddr'], p.data()) for p in f.iter_segments()
                     if p['p_type'] == 'PT_LOAD' and p['p_vaddr'] >= 0x1c700000],
                    next(s['st_value'] for s in f.get_section_by_name('.symtab').iter_symbols()
                         if s.name == 'rh_platform_quickapp_icon_path'))
            if version == '1043':
                continue
            path = ROOT / 'build/firmware-analysis' / f'vela_ap_4.100.{version}.bin'
            if not path.exists():
                sdk = Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus-Private'))
                path = sdk / 'fwbins' / ('xiaomi-band-11-4.100.' + version) / 'vela_ap.bin'
            cls.firmware[version] = path.read_bytes()
            digest = hashlib.sha256(cls.firmware[version]).hexdigest()
            if digest != TARGETS[version][-1]:
                raise RuntimeError(f'{version}: firmware fingerprint mismatch: {digest}')

    def machine(self, version, path=PATH, identity=PACKAGE):
        self.version = version
        self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
        for address, size in ((0xc000000, 0xe00000), (0x2c000000, 0xe00000),
                              (0x20000000, 0x160000), (0x3c000000, 0x1000000),
                              (0x1c700000, 0x40000)):
            self.u.mem_map(address, size)
        if version in TARGETS:
            for base in (0xc0c0000, 0x2c0c0000):
                self.u.mem_write(base, self.firmware[version])
        segments, self.entry = self.images[version]
        for address, data in segments:
            self.u.mem_write(address, data)
        self.next_memory = 0x3c710000
        self.package = self.text(PACKAGE)
        self.output = self.mem(256)
        self.executed, self.writes = set(), []
        self.u.hook_add(UC_HOOK_CODE,
            lambda u, pc, size, data: self.executed.add(pc))
        self.u.hook_add(UC_HOOK_MEM_WRITE,
            lambda u, access, addr, size, value, data: self.writes.append((addr, size)))
        if version in TARGETS:
            table, lookup, *_ = TARGETS[version]
            # Literal table slot is restored as firmware initialization does.
            self.word(0x20084ec4, table)
            self.assertEqual(self.word(table + 12), lookup | 1)
            self.map = self.mem(4 * 17)
            self.word(self.map, 16)
            self.word(0x20084ebc, self.map)
            self.app = self.mem(80)
            self.identity = self.text(identity) if identity is not None else 0
            self.icon = self.text(path) if path is not None else 0
            self.word(self.app + 12, self.identity)
            self.word(self.app + 16, self.icon)
            self.node = self.add(PACKAGE, self.app)
            strcmp = TARGETS[version][5]
            self.assertEqual(bytes(self.u.mem_read(strcmp, 8)),
                             bytes.fromhex('5ff800f095c02600'))
            def compare(u, pc, size, data):
                def value(p):
                    raw = bytes(u.mem_read(p, 256))
                    return raw.split(b'\0', 1)[0]
                a, b = value(u.reg_read(UC_ARM_REG_R0)), value(u.reg_read(UC_ARM_REG_R1))
                u.reg_write(UC_ARM_REG_R0, ((a > b) - (a < b)) & 0xffffffff)
                u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
            self.u.hook_add(UC_HOOK_CODE, compare, None, strcmp, strcmp)

    def mem(self, n=128):
        p = self.next_memory
        self.next_memory += (n + 15) & ~15
        self.u.mem_write(p, bytes(n))
        return p

    def text(self, s):
        p = self.mem(len(s) + 1)
        self.u.mem_write(p, s.encode() + b'\0')
        return p

    def word(self, p, value=None):
        if value is None:
            return struct.unpack('<I', self.u.mem_read(p, 4))[0]
        self.u.mem_write(p, struct.pack('<I', value))

    def add(self, key, record):
        h = 5381
        for b in key.encode():
            h = (h * 33 + b) & 0xffffffff
        bucket = self.map + 4 * (1 + (h & 15))
        node = self.mem(20)
        for off, value in ((0, h), (4, self.text(key)), (8, len(key) + 1),
                           (12, record), (16, self.word(bucket))):
            self.word(node + off, value)
        self.word(bucket, node)
        return node

    def call(self, package=None):
        self.executed.clear()
        self.writes.clear()
        self.u.mem_write(self.output, b'X' * 256)
        self.u.reg_write(UC_ARM_REG_R0, self.package if package is None else package)
        self.u.reg_write(UC_ARM_REG_R1, self.output)
        self.u.reg_write(UC_ARM_REG_SP, 0x20150000)
        self.u.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.u.emu_start(self.entry | 1, STOP, count=2000000)
        self.assertEqual(self.u.reg_read(UC_ARM_REG_PC), STOP)
        value = self.u.reg_read(UC_ARM_REG_R0)
        # Only owner stack and caller output may be written by the whole chain.
        for address, size in self.writes:
            self.assertTrue((self.output <= address and address + size <= self.output + 256)
                or (0x20140000 <= address and address + size <= 0x20150000), hex(address))
        if value != 1:
            self.assertEqual(self.u.mem_read(self.output, 1), b'\0')
        return value if value < 0x80000000 else value - 0x100000000

    def native_called(self):
        return TARGETS[self.version][1] in self.executed

    def test_real_lookup_chain_and_independent_copy(self):
        for version in TARGETS:
            with self.subTest(version=version):
                self.machine(version)
                # Insert a same-bucket different-hash node ahead of the match.
                # Real bucket traversal must skip it, not return the first node.
                def bucket(s):
                    h = 5381
                    for b in s.encode():
                        h = (h * 33 + b) & 0xffffffff
                    return h & 15
                collision = next('other.app' + str(i) for i in range(100)
                                 if bucket('other.app' + str(i)) == bucket(PACKAGE))
                self.add(collision, self.mem(80))
                self.assertEqual(self.call(), 1)
                for address in TARGETS[version][1:6]:
                    self.assertIn(address, self.executed)
                self.assertIn(0xc5db990, self.executed)
                self.u.mem_write(self.icon, b'Z')
                self.assertEqual(bytes(self.u.mem_read(self.output, len(PATH) + 1)),
                                 PATH.encode() + b'\0')
                self.assertNotIn(0xc582c00, self.executed)
                self.assertNotIn(0xc582f90, self.executed)

    def test_absent_uninitialized_and_exact_service_guards(self):
        for version in TARGETS:
            for fault, expected in (('service-null', 0), ('service-wrong', -1),
                    ('callback-wrong', -1), ('registry-null', 0), ('registry-invalid', -1),
                    ('registry-zero-buckets', -1), ('absent', 0), ('missing-key', 0)):
                with self.subTest(version=version, fault=fault):
                    self.machine(version)
                    if fault.startswith('service-'):
                        self.word(0x20084ec4, 0 if fault.endswith('null') else 0xdead0000)
                    elif fault == 'callback-wrong':
                        self.word(TARGETS[version][0] + 12, TARGETS[version][1])
                    elif fault.startswith('registry-'):
                        if fault == 'registry-zero-buckets':
                            self.word(self.map, 0)
                        else:
                            self.word(0x20084ebc, 0 if fault.endswith('null') else 0xdead0000)
                    elif fault == 'absent':
                        self.word(self.node + 12, 0)
                    if fault == 'missing-key':
                        self.assertEqual(self.call(self.text('org.example.absent')), expected)
                    else:
                        self.assertEqual(self.call(), expected)
                    self.assertEqual(self.native_called(), fault in ('absent', 'missing-key'))

    def test_invalid_records_and_bounded_strings(self):
        for version in TARGETS:
            for fault in ('app-invalid', 'app-unaligned', 'pkg-null', 'pkg-invalid',
                          'pkg-wrong', 'pkg-unterminated', 'icon-invalid',
                          'icon-unterminated', 'icon-boundary', 'node-invalid', 'node-cycle'):
                with self.subTest(version=version, fault=fault):
                    self.machine(version)
                    if fault.startswith('app-'):
                        self.word(self.node + 12, 0xdead0000 if fault.endswith('invalid') else self.app + 1)
                    elif fault.startswith('pkg-'):
                        p = {'pkg-null': 0, 'pkg-invalid': 0xdead0000,
                             'pkg-wrong': self.text(PACKAGE + '.other'),
                             'pkg-unterminated': self.text('p' * 256)}[fault]
                        self.word(self.app + 12, p)
                    elif fault.startswith('icon-'):
                        p = {'icon-invalid': 0xdead0000,
                             'icon-unterminated': self.text('/' + 'x' * 255),
                             'icon-boundary': 0x3cffffff}[fault]
                        self.word(self.app + 16, p)
                        if fault == 'icon-boundary':
                            self.u.mem_write(p, b'/')
                    else:
                        self.word(self.node + 16, 0xdead0000 if fault.endswith('invalid') else self.node)
                    self.assertEqual(self.call(), -1)

    def test_source_format_root_and_canonical_path_guards(self):
        root = '/data/quickapp/app/' + PACKAGE + '/'
        cases = [(None, 0), ('', 0), (root + 'icon.png', -2),
                 (root + 'icon.BIN', -2), ('descriptor', -2),
                 ('internal://files/icon.bin', -2), ('/resource/icon.bin', -2),
                 (root + '../icon.bin', -1), (root + './icon.bin', -1),
                 (root + 'a//icon.bin', -1), (root + 'icon.bin?x', -1),
                 (root + 'icon\\.bin', -1), (root + 'icon\n.bin', -1),
                 (root + '%2e%2e/icon.bin', -1),
                 ('/data/quickapp/app/' + PACKAGE + '.other/icon.bin', -1),
                 ('/data/quickapp/app/' + PACKAGE, -1),
                 (root + 'i.bin', 1), (root + 'x' * (255 - len(root) - 4) + '.bin', 1)]
        for version in TARGETS:
            for path, expected in cases:
                with self.subTest(version=version, path=path):
                    self.machine(version, path=path)
                    self.assertEqual(self.call(), expected)

    def test_null_bad_package_and_1043_do_not_call_native(self):
        for version in (*TARGETS, '1043'):
            with self.subTest(version=version):
                self.machine(version)
                self.assertEqual(self.call(0), -2 if version == '1043' else -1)
                self.assertEqual(self.call(self.text('../bad')), -2 if version == '1043' else -1)
                if version == '1043':
                    self.assertEqual(self.call(), -2)
                    self.assertTrue(all(0x1c700000 <= pc < 0x1c740000 for pc in self.executed))
                else:
                    self.assertFalse(self.native_called())


if __name__ == '__main__':
    unittest.main(verbosity=2)

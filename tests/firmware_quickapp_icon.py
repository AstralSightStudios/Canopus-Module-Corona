"""Compile the production adapter and execute exact-target AP lookup code.

Run: build/firmware-tests/bin/python tests/firmware_quickapp_icon.py
CLANG/LD_LLD and CANOPUS_ROOT may be overridden as in the calendar tests.
No device, rendering or file-existence claim: registry/records are synthetic;
lookup wrapper, hashmap lookup/hash/bucket search are real firmware. The strcmp
AP thunk executes normally; only its documented ROM destination (absent from
the AP image) is modeled.
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
                     UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ)
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_R0,
    UC_ARM_REG_R1, UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR)

ROOT = Path(__file__).resolve().parents[1]
STOP = 0x1c73ff00
PACKAGE = 'org.example.clock'
# Independently checked IDBs, including extract155 rather than the small IDB.
TARGETS = {
    '1043': dict(service=0x2cdbb054, lookup=0x0ca69e80, registry_lookup=0x0cabe234,
                 strcmp=0x0cac0a48, strcmp_rom=0x00276855,
                 service_slot=0x200eb658, registry_slot=0x200eb650,
                 package_offset=8, icon_offset=12, root='/data/app/',
                 flash_high=0x0cde8190, target='xiaomi-band-10-pro-3.101.043',
                 size=13795728,
                 sha256='519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec',
                 code_fingerprints={
                     'lookup': (40, '54580c3172264b9fe8b74afc4bc0108768b3105bb582b0e08399f7c463e74392'),
                     'registry_lookup': (158, '6965575cc2f8fca691779a8478e6d39953aa6bc93580511fbd900a66a3918619')}),
    '139': dict(service=0x2ca3da68, lookup=0xc6a16a2, registry_lookup=0xc6ac6c0,
                hash=0xc6d2d84, bucket_search=0xc6d2698, strcmp=0xc720f48, strcmp_rom=0x0026c095,
                strlen=0xc5db990, service_slot=0x20084ec4, registry_slot=0x20084ebc,
                package_offset=12, icon_offset=16, root='/data/quickapp/app/',
                flash_high=0x0cd00000, target='xiaomi-band-11-4.100.139',
                sha256='31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74'),
    '155': dict(service=0x2ca3da58, lookup=0xc6a1692, registry_lookup=0xc6ac6b0,
                hash=0xc6d2d74, bucket_search=0xc6d2688, strcmp=0xc720f38, strcmp_rom=0x0026c095,
                strlen=0xc5db990, service_slot=0x20084ec4, registry_slot=0x20084ebc,
                package_offset=12, icon_offset=16, root='/data/quickapp/app/',
                flash_high=0x0cd00000, target='xiaomi-band-11-4.100.155',
                sha256='ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f'),
}


class QuickAppIconNative(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='rh-quickapp-icon-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.images, cls.firmware = {}, {}
        clang = os.environ.get('CLANG', shutil.which('clang'))
        linker = os.environ.get('LD_LLD', shutil.which('ld.lld'))
        for version in TARGETS:
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
            target = TARGETS[version]
            release = target['target'].rsplit('-', 1)[1]
            path = ROOT / 'build/firmware-analysis' / f'vela_ap_{release}.bin'
            if not path.exists():
                sdk = Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus-Private'))
                path = sdk / 'fwbins' / target['target'] / 'vela_ap.bin'
            cls.firmware[version] = path.read_bytes()
            digest = hashlib.sha256(cls.firmware[version]).hexdigest()
            if 'size' in target and len(cls.firmware[version]) != target['size']:
                raise RuntimeError(f'{version}: firmware size mismatch')
            if digest != target['sha256']:
                raise RuntimeError(f'{version}: firmware fingerprint mismatch: {digest}')

    def machine(self, version, path=Ellipsis, identity=PACKAGE):
        self.version = version
        self.target = TARGETS[version]
        self.path = self.target['root'] + PACKAGE + '/images/icon.bin'
        if path is Ellipsis:
            path = self.path
        self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
        for address, size in ((0xc000000, 0xe00000), (0x2c000000, 0xe00000),
                              (0x20000000, 0x160000), (0x3c000000, 0x1000000),
                              (0x1c700000, 0x40000)):
            self.u.mem_map(address, size)
        for base in (0xc0c0000, 0x2c0c0000):
            self.u.mem_write(base, self.firmware[version])
        segments, self.entry = self.images[version]
        for address, data in segments:
            self.u.mem_write(address, data)
        self.next_memory = 0x3c710000
        self.package = self.text(PACKAGE)
        self.output = self.mem(256)
        self.executed, self.writes, self.reads, self.comparisons = set(), [], [], []
        self.u.hook_add(UC_HOOK_CODE,
            lambda u, pc, size, data: self.executed.add(pc))
        self.u.hook_add(UC_HOOK_MEM_READ,
            lambda u, access, addr, size, value, data:
                self.reads.append((u.reg_read(UC_ARM_REG_PC), addr, size)))
        self.u.hook_add(UC_HOOK_MEM_WRITE,
            lambda u, access, addr, size, value, data: self.writes.append((addr, size)))
        table, lookup = self.target['service'], self.target['lookup']
        # Literal table slot is restored as firmware initialization does.
        self.word(self.target['service_slot'], table)
        self.assertEqual(self.word(table + 12), lookup | 1)
        self.map = self.mem(4 * 17)
        self.word(self.map, 16)
        self.word(self.target['registry_slot'], self.map)
        if version == '1043':
            self.word(0x200eb654, 0xdead0000)  # secondary/uninstalled registry is forbidden
        self.app = self.mem(64 if version == '1043' else 80)
        self.identity = self.text(identity) if identity is not None else 0
        self.icon = self.text(path) if path is not None else 0
        self.word(self.app + self.target['package_offset'], self.identity)
        self.word(self.app + self.target['icon_offset'], self.icon)
        self.node = self.add(PACKAGE, self.app)
        strcmp = self.target['strcmp']
        rom = self.target['strcmp_rom'] & ~1
        self.assertEqual(bytes(self.u.mem_read(strcmp, 8)),
                         bytes.fromhex('5ff800f0') + struct.pack('<I', self.target['strcmp_rom']))
        # Run the available AP ldr-PC thunk, modeling only the absent ROM leaf.
        self.u.mem_map(rom & ~4095, 4096)
        def compare(u, pc, size, data):
            def value(p):
                raw = bytearray()
                for i in range(256):
                    b = u.mem_read(p + i, 1)[0]
                    if not b:
                        return bytes(raw)
                    raw.append(b)
                raise AssertionError('Unbounded strcmp input escaped preflight')
            a, b = value(u.reg_read(UC_ARM_REG_R0)), value(u.reg_read(UC_ARM_REG_R1))
            self.comparisons.append((a, b))
            u.reg_write(UC_ARM_REG_R0, ((a > b) - (a < b)) & 0xffffffff)
            u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
        self.u.hook_add(UC_HOOK_CODE, compare, None, rom, rom)

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

    def bucket(self, key=PACKAGE):
        h = 5381
        for b in key.encode():
            h = (h * 33 + b) & 0xffffffff
        return self.map + 4 * (1 + (h & 15))

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

    def call(self, package=None, output=None):
        self.executed.clear()
        self.writes.clear()
        self.reads.clear()
        self.comparisons.clear()
        self.u.mem_write(self.output, b'X' * 256)
        self.u.reg_write(UC_ARM_REG_R0, self.package if package is None else package)
        caller_output = self.output if output is None else output
        self.u.reg_write(UC_ARM_REG_R1, caller_output)
        self.u.reg_write(UC_ARM_REG_SP, 0x20150000)
        self.u.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.u.emu_start(self.entry | 1, STOP, count=2000000)
        self.assertEqual(self.u.reg_read(UC_ARM_REG_PC), STOP)
        value = self.u.reg_read(UC_ARM_REG_R0)
        # Only owner stack and caller output may be written by the whole chain.
        for address, size in self.writes:
            self.assertTrue((caller_output != 0 and caller_output <= address
                             and address + size <= caller_output + 256)
                or (0x2014f000 <= address and address + size <= 0x20150000),
                f'forbidden native write: {address:#x}+{size}')
        if self.version == '1043':
            self.assertFalse(any(address == 0x200eb654 for _, address, _ in self.reads))
        if output == 0:
            self.assertEqual(bytes(self.u.mem_read(self.output, 256)), b'X' * 256)
        elif value != 1:
            self.assertEqual(self.u.mem_read(self.output, 1), b'\0')
        return value if value < 0x80000000 else value - 0x100000000

    def native_called(self):
        return self.target['lookup'] in self.executed

    def test_fixtures_cover_all_supported_exact_targets(self):
        self.assertEqual(set(TARGETS), {'139', '155', '1043'})

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
                collision_node = self.add(collision, self.mem(80))
                self.assertEqual(self.call(), 1)
                chain = ('lookup', 'registry_lookup', 'strcmp') if version == '1043' else (
                    'lookup', 'registry_lookup', 'hash', 'bucket_search', 'strcmp')
                for key in chain:
                    self.assertIn(self.target[key], self.executed)
                # .043 inlines DJB2 and bucket traversal into hashmap_get;
                # its entire verified body is AP code, never a Python lookup.
                for key, (size, digest) in self.target.get('code_fingerprints', {}).items():
                    address = self.target[key]
                    self.assertEqual(hashlib.sha256(bytes(self.u.mem_read(address, size))).hexdigest(), digest)
                    self.assertGreater(len(self.executed & set(range(address, address + size))), 8)
                if 'strlen' in self.target:
                    self.assertIn(self.target['strlen'], self.executed)
                else:
                    start = self.target['registry_lookup']
                    native_reads = {address for pc, address, _ in self.reads
                                    if start <= pc < start + 158}
                    self.assertTrue({self.bucket(), collision_node, collision_node + 16,
                                     self.node, self.node + 12}.issubset(native_reads),
                                    sorted(hex(p) for p in native_reads))
                self.assertIn(self.target['strcmp_rom'] & ~1, self.executed)
                self.u.mem_write(self.icon, b'Z')
                self.assertEqual(bytes(self.u.mem_read(self.output, len(self.path) + 1)),
                                 self.path.encode() + b'\0')
                self.assertNotIn(0xc582c00, self.executed)
                self.assertNotIn(0xc582f90, self.executed)

    def test_same_hash_same_length_key_comparison_is_not_a_record_shortcut(self):
        for version in TARGETS:
            with self.subTest(version=version):
                self.machine(version)
                wrong_key = 'org.example.other'
                self.assertEqual(len(wrong_key), len(PACKAGE))
                decoy = self.mem(20)
                for off, value in ((0, self.word(self.node)), (4, self.text(wrong_key)),
                                   (8, len(wrong_key) + 1), (12, self.mem(80)), (16, self.node)):
                    self.word(decoy + off, value)
                self.word(self.bucket(), decoy)
                self.assertEqual(self.call(), 1)
                self.assertTrue(any(a != b for a, b in self.comparisons))
                self.assertTrue(any(a == b == PACKAGE.encode() for a, b in self.comparisons))
                self.assertEqual(bytes(self.u.mem_read(self.output, len(self.path) + 1)),
                                 self.path.encode() + b'\0')

    def test_absent_uninitialized_and_exact_service_guards(self):
        for version in TARGETS:
            for fault, expected in (('service-null', 0), ('service-wrong', -1),
                    ('callback-wrong', -1), ('registry-null', 0), ('registry-invalid', -1),
                    ('registry-zero-buckets', -1), ('absent', 0), ('missing-key', 0)):
                with self.subTest(version=version, fault=fault):
                    self.machine(version)
                    if fault.startswith('service-'):
                        self.word(self.target['service_slot'], 0 if fault.endswith('null') else 0xdead0000)
                    elif fault == 'callback-wrong':
                        self.word(self.target['service'] + 12, self.target['lookup'])
                    elif fault.startswith('registry-'):
                        if fault == 'registry-zero-buckets':
                            self.word(self.map, 0)
                        else:
                            self.word(self.target['registry_slot'], 0 if fault.endswith('null') else 0xdead0000)
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
                        self.word(self.app + self.target['package_offset'], p)
                    elif fault.startswith('icon-'):
                        p = {'icon-invalid': 0xdead0000,
                             'icon-unterminated': self.text('/' + 'x' * 255),
                             'icon-boundary': 0x3cffffff}[fault]
                        self.word(self.app + self.target['icon_offset'], p)
                        if fault == 'icon-boundary':
                            self.u.mem_write(p, b'/')
                    else:
                        self.word(self.node + 16, 0xdead0000 if fault.endswith('invalid') else self.node)
                    self.assertEqual(self.call(), -1)

    def test_selected_bucket_invalid_nodes_keys_lengths_and_cycles(self):
        for version in TARGETS:
            for fault in ('node-unaligned', 'key-null', 'key-invalid', 'key-long',
                          'length-zero', 'length-short', 'length-long', 'two-node-cycle'):
                with self.subTest(version=version, fault=fault):
                    self.machine(version)
                    if fault == 'node-unaligned':
                        self.word(self.bucket(), self.node + 1)
                    elif fault.startswith('key-'):
                        self.word(self.node + 4, {'key-null': 0, 'key-invalid': 0xdead0000,
                                                 'key-long': self.text('x' * 256)}[fault])
                    elif fault == 'two-node-cycle':
                        other = self.add(PACKAGE, self.app)
                        self.word(self.node + 16, other)
                    else:
                        self.word(self.node + 8, {'length-zero': 0,
                            'length-short': len(PACKAGE), 'length-long': len(PACKAGE) + 2}[fault])
                    self.assertEqual(self.call(), -1)
                    self.assertFalse(self.native_called())
            self.machine(version)
            self.word(self.map + 4 + ((self.bucket() - self.map - 4 + 4) % 64), 0xdead0000)
            self.assertEqual(self.call(), 1)
            self.assertTrue(self.native_called())

    def test_invalid_map_counts_alignment_and_extent(self):
        for version in TARGETS:
            for count in (0, 3, 4097, 8192, 0xffffffff):
                with self.subTest(version=version, count=count):
                    self.machine(version)
                    self.word(self.map, count)
                    self.assertEqual(self.call(), -1)
                    self.assertFalse(self.native_called())
            for pointer in (0x2015fffc, 0x3cffffff, 0x0c0c0000, 0x2c0c0000):
                with self.subTest(version=version, pointer=hex(pointer)):
                    self.machine(version)
                    self.word(self.target['registry_slot'], pointer)
                    if pointer == 0x2015fffc:
                        self.word(pointer, 16)
                    self.assertEqual(self.call(), -1)
                    self.assertFalse(self.native_called())

    def test_borrowed_owner_field_rechecks(self):
        for version in TARGETS:
            for field in ('service', 'registry', 'package', 'icon'):
                with self.subTest(version=version, field=field):
                    self.machine(version)
                    address = {'service': self.target['service_slot'],
                               'registry': self.target['registry_slot'],
                               'package': self.app + self.target['package_offset'],
                               'icon': self.app + self.target['icon_offset']}[field]
                    changed = []
                    def replace(u, access, p, size, value, data):
                        if not changed:
                            self.word(address, 0)
                            changed.append(True)
                    self.u.hook_add(UC_HOOK_MEM_READ, replace, None, self.icon, self.icon)
                    self.assertEqual(self.call(), -1)
                    self.assertEqual(changed, [True])
                    self.assertTrue(self.native_called())

    def test_pointer_regions_and_exact_flash_upper_bounds(self):
        for version in TARGETS:
            for high in (TARGETS[version]['flash_high'],
                         TARGETS[version]['flash_high'] + 0x20000000, 0x20160000, 0x3d000000):
                with self.subTest(version=version, high=hex(high)):
                    self.machine(version)
                    p = high - len(self.path) - 1
                    self.u.mem_write(p, self.path.encode() + b'\0')
                    self.word(self.app + self.target['icon_offset'], p)
                    self.assertEqual(self.call(), 1)
                    self.machine(version)
                    self.u.mem_write(high - 1, b'/')
                    self.word(self.app + self.target['icon_offset'], high - 1)
                    self.assertEqual(self.call(), -1)
                    self.machine(version)
                    self.word(self.app + self.target['icon_offset'], high)
                    self.assertEqual(self.call(), -1)
            for pointer in (0x0c0bffff, 0x2c0bffff, 0x1fffffff, 0x3bffffff, 0xffffffff):
                with self.subTest(version=version, pointer=hex(pointer)):
                    self.machine(version)
                    self.word(self.app + self.target['package_offset'], pointer)
                    self.assertEqual(self.call(), -1)

    def test_source_format_root_and_canonical_path_guards(self):
        for version in TARGETS:
            root = TARGETS[version]['root'] + PACKAGE + '/'
            cases = [(None, 0), ('', 0), (root + 'icon.png', -2),
                 (root + 'icon.BIN', -2), ('descriptor', -2),
                 (('/data/quickapp/app/' if version == '1043' else '/data/app/')
                  + PACKAGE + '/icon.bin', -2),
                 ('internal://files/icon.bin', -2), ('/resource/icon.bin', -2),
                 (root + '../icon.bin', -1), (root + './icon.bin', -1),
                 (root + 'a//icon.bin', -1), (root + 'icon.bin?x', -1),
                 (root + 'icon\\.bin', -1), (root + 'icon\n.bin', -1),
                 (root + '%2e%2e/icon.bin', -1),
                 (TARGETS[version]['root'] + PACKAGE + '.other/icon.bin', -1),
                 (TARGETS[version]['root'] + PACKAGE, -1),
                 (root + 'i.bin', 1), (root + 'x' * (255 - len(root) - 4) + '.bin', 1)]
            for path, expected in cases:
                with self.subTest(version=version, path=path):
                    self.machine(version, path=path)
                    self.assertEqual(self.call(), expected)

    def test_null_bad_package_do_not_call_native(self):
        for version in TARGETS:
            self.machine(version)
            self.assertEqual(self.call(output=0), -1)
            self.assertFalse(self.native_called())
            for package in (None, '', '.', '..', '../bad', 'x' * 256):
                with self.subTest(version=version, package=package):
                    self.machine(version)
                    self.assertEqual(self.call(0 if package is None else self.text(package)), -1)
                    self.assertFalse(self.native_called())


if __name__ == '__main__':
    unittest.main(verbosity=2)

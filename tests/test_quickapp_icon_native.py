"""Host-only production C guard tests; no third-party Python dependencies.

Firmware execution is deliberately separate in firmware_quickapp_icon.py.
Only the memory reader and verified native lookup leaf are injected here.
"""
import ctypes as C
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = b'org.example.clock'
TARGETS = {
    '1043': dict(service=0x2cdbb054, lookup=0x0ca69e81,
                 service_slot=0x200eb658, registry_slot=0x200eb650,
                 package_offset=8, icon_offset=12, root=b'/data/app/',
                 flash_high=0x0cde8190),
    '139': dict(service=0x2ca3da68, lookup=0xc6a16a3,
                service_slot=0x20084ec4, registry_slot=0x20084ebc,
                package_offset=12, icon_offset=16, root=b'/data/quickapp/app/',
                flash_high=0x0cd00000),
    '155': dict(service=0x2ca3da58, lookup=0xc6a1693,
                service_slot=0x20084ec4, registry_slot=0x20084ebc,
                package_offset=12, icon_offset=16, root=b'/data/quickapp/app/',
                flash_high=0x0cd00000),
}

READ = C.CFUNCTYPE(C.c_uint32, C.c_uint32, C.c_uint)
LOOKUP = C.CFUNCTYPE(C.c_uint32, C.c_uint32, C.c_char_p)


class QuickAppIconHost(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='rh-quickapp-host-')
        cls.addClassCleanup(cls.tmp.cleanup)
        harness = Path(cls.tmp.name) / 'harness.c'
        harness.write_text('''#include <stdint.h>
static uint32_t (*reader)(uint32_t,unsigned);
static uint32_t (*finder)(uint32_t,const char *);
void qa_setup(uint32_t (*r)(uint32_t,unsigned), uint32_t (*f)(uint32_t,const char *)) {
    reader=r; finder=f;
}
uint32_t rh_qa_read(uint32_t a,unsigned n) { return reader(a,n); }
uint32_t rh_qa_lookup(uint32_t a,const char *p) { return finder(a,p); }
''')
        cls.libs = {}
        for version in TARGETS:
            library = Path(cls.tmp.name) / (version + '.so')
            defines = [] if version == '139' else [f'-DRH_TARGET_{version}=1']
            subprocess.run([os.environ.get('CC', shutil.which('cc')), '-shared', '-fPIC',
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-fno-builtin',
                '-DRH_QUICKAPP_ICON_TEST=1', *defines, '-I' + str(ROOT / 'include'),
                str(ROOT / 'src/quickapp_icon.c'), str(harness), '-o', str(library)], check=True)
            lib = C.CDLL(str(library))
            lib.qa_setup.argtypes = [READ, LOOKUP]
            lib.rh_platform_quickapp_icon_path.argtypes = [C.c_char_p, C.c_void_p]
            lib.rh_platform_quickapp_icon_path.restype = C.c_int
            cls.libs[version] = lib

    def machine(self, version, path=Ellipsis, identity=PACKAGE):
        self.version, self.lib = version, self.libs[version]
        self.target = TARGETS[version]
        self.prefix = self.target['root'] + PACKAGE + b'/'
        self.path = self.prefix + b'images/icon.bin'
        if path is Ellipsis:
            path = self.path
        self.memory, self.calls, self.reads, self.invalid_reads = {}, [], [], []
        self.next_memory = 0x3c710000
        self.return_record, self.mutate = 0, None
        def read(a, n):
            self.reads.append((a, n))
            # Explicitly allocated memory only: unexpected reads fail the test.
            if any(a + i not in self.memory for i in range(n)):
                self.invalid_reads.append((a, n))
                return 0
            value = int.from_bytes(bytes(self.memory[a + i] for i in range(n)), 'little')
            if self.mutate and a == self.icon and n == 1:
                mutation, self.mutate = self.mutate, None
                mutation()
            return value
        def lookup(a, p):
            self.calls.append((a, p))
            return self.return_record
        self.reader, self.finder = READ(read), LOOKUP(lookup)
        self.lib.qa_setup(self.reader, self.finder)
        self.word(self.target['service_slot'], self.target['service'])
        self.word(self.target['service'] + 12, self.target['lookup'])
        self.map = self.mem(68)
        self.word(self.map, 16)
        self.word(self.target['registry_slot'], self.map)
        if version == '1043':
            self.word(0x200eb654, 0xdead0000)  # never a fallback to uninstalled records
        self.app = self.mem(80)
        self.return_record = self.app
        self.icon = self.text(path) if path is not None else 0
        self.word(self.app + self.target['package_offset'],
                  self.text(identity) if identity is not None else 0)
        self.word(self.app + self.target['icon_offset'], self.icon)

    def mem(self, n):
        p = self.next_memory
        self.next_memory += (n + 15) & ~15
        self.data(p, bytes(n))
        return p

    def data(self, p, value):
        self.memory.update({p + i: b for i, b in enumerate(value)})

    def text(self, value):
        p = self.mem(len(value) + 1)
        self.data(p, value + b'\0')
        return p

    def word(self, p, value):
        self.data(p, struct.pack('<I', value))

    def bucket(self, key=PACKAGE):
        h = 5381
        for b in key:
            h = (h * 33 + b) & 0xffffffff
        return self.map + 4 * (1 + (h & 15))

    def node(self, key=PACKAGE):
        node = self.mem(20)
        for off, value in ((4, self.text(key)), (8, len(key) + 1), (12, self.app)):
            self.word(node + off, value)
        self.word(self.bucket(key), node)
        return node

    def call(self, package=PACKAGE):
        self.calls.clear()
        self.out = C.create_string_buffer(b'X' * 255, 256)
        before = dict(self.memory)
        mutation = self.mutate
        result = self.lib.rh_platform_quickapp_icon_path(package, self.out)
        self.assertEqual(self.invalid_reads, [])
        if self.version == '1043':
            self.assertFalse(any(address == 0x200eb654 for address, _ in self.reads))
        if not mutation:
            self.assertEqual(self.memory, before)  # no native writes, ever
        if result != 1:
            self.assertEqual(self.out.value, b'')
        return result

    def test_fixtures_cover_all_supported_exact_targets(self):
        self.assertEqual(set(TARGETS), {'139', '155', '1043'})

    def test_supported_exact_targets_copy_and_no_pointer_retention(self):
        for version, target in TARGETS.items():
            with self.subTest(version=version):
                self.machine(version)
                self.assertEqual(self.call(), 1)
                self.assertEqual(self.calls, [(target['lookup'], PACKAGE)])
                self.data(self.icon, b'X')
                self.assertEqual(self.out.value, self.path)

    def test_service_registry_guards_never_invoke_invalid_leaf(self):
        for version in TARGETS:
            for fault, expected in (('service-null', 0), ('service-wrong', -1),
                    ('callback', -1), ('registry-null', 0), ('registry-invalid', -1),
                    ('registry-count', -1), ('registry-unaligned', -1)):
                with self.subTest(version=version, fault=fault):
                    self.machine(version)
                    if fault.startswith('service-'):
                        self.word(self.target['service_slot'], 0 if fault.endswith('null') else 0xdead0000)
                    elif fault == 'callback':
                        self.word(self.target['service'] + 12, self.target['lookup'] - 1)
                    elif fault == 'registry-count':
                        self.word(self.map, 3)
                    else:
                        self.word(self.target['registry_slot'], {'registry-null': 0,
                            'registry-invalid': 0xdead0000, 'registry-unaligned': self.map + 1}[fault])
                    self.assertEqual(self.call(), expected)
                    self.assertEqual(self.calls, [])

    def test_selected_bucket_preflight_guards_and_unselected_isolation(self):
        for version in TARGETS:
            for fault in ('node-invalid', 'node-unaligned', 'cycle', 'two-node-cycle',
                          'key-null', 'key-invalid', 'key-long', 'length-zero',
                          'length-short', 'length-long'):
                with self.subTest(version=version, fault=fault):
                    self.machine(version)
                    node = self.node()
                    if fault.startswith('node-'):
                        self.word(self.bucket(), 0xdead0000 if fault.endswith('invalid') else node + 1)
                    elif fault == 'cycle':
                        self.word(node + 16, node)
                    elif fault == 'two-node-cycle':
                        other = self.node()
                        self.word(node + 16, other)
                        self.word(other + 16, node)
                    elif fault.startswith('key-'):
                        self.word(node + 4, {'key-null': 0, 'key-invalid': 0xdead0000,
                                           'key-long': self.text(b'x' * 256)}[fault])
                    else:
                        self.word(node + 8, {'length-zero': 0, 'length-short': len(PACKAGE),
                                            'length-long': len(PACKAGE) + 2}[fault])
                    self.assertEqual(self.call(), -1)
                    self.assertEqual(self.calls, [])
            self.machine(version)
            self.node()
            # Poison exactly a different bucket; selected-bucket validation is not a scan.
            self.word(self.map + 4 + ((self.bucket() - self.map - 4 + 4) % 64), 0xdead0000)
            self.assertEqual(self.call(), 1)

    def test_invalid_map_counts_and_extents(self):
        for version in TARGETS:
            for count in (0, 3, 4097, 8192, 0xffffffff):
                with self.subTest(version=version, count=count):
                    self.machine(version)
                    self.word(self.map, count)
                    self.assertEqual(self.call(), -1)
                    self.assertEqual(self.calls, [])
            self.machine(version)
            self.word(self.target['registry_slot'], 0x2015fffc)
            self.word(0x2015fffc, 16)
            self.assertEqual(self.call(), -1)
            self.assertEqual(self.calls, [])

    def test_borrowed_owner_field_rechecks(self):
        for version in TARGETS:
            for field in ('service', 'registry', 'package', 'icon'):
                with self.subTest(version=version, field=field):
                    self.machine(version)
                    address = {'service': self.target['service_slot'],
                               'registry': self.target['registry_slot'],
                               'package': self.app + self.target['package_offset'],
                               'icon': self.app + self.target['icon_offset']}[field]
                    self.mutate = lambda: self.word(address, 0)
                    self.assertEqual(self.call(), -1)
                    self.assertEqual(self.calls, [(self.target['lookup'], PACKAGE)])

    def test_string_pointer_regions_and_exact_flash_upper_bounds(self):
        for version in TARGETS:
            for high in (TARGETS[version]['flash_high'],
                         TARGETS[version]['flash_high'] + 0x20000000, 0x20160000, 0x3d000000):
                with self.subTest(version=version, high=hex(high)):
                    self.machine(version)
                    p = high - len(self.path) - 1
                    self.data(p, self.path + b'\0')
                    self.word(self.app + self.target['icon_offset'], p)
                    self.assertEqual(self.call(), 1)
                    self.machine(version)
                    self.data(high - 1, b'/')
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

    def test_absent_invalid_records_identity_and_lengths(self):
        for version in TARGETS:
            for fault, expected in (('absent', 0), ('unaligned', -1), ('invalid', -1),
                    ('pkg-null', -1), ('pkg-wrong', -1), ('pkg-long', -1),
                    ('icon-null', 0), ('icon-invalid', -1), ('icon-long', -1),
                    ('icon-boundary', -1)):
                with self.subTest(version=version, fault=fault):
                    self.machine(version)
                    if fault in ('absent', 'unaligned', 'invalid'):
                        self.return_record = {'absent': 0, 'unaligned': self.app + 1,
                                              'invalid': 0xdead0000}[fault]
                    elif fault.startswith('pkg-'):
                        p = {'pkg-null': 0, 'pkg-wrong': self.text(PACKAGE + b'.other'),
                             'pkg-long': self.text(b'x' * 256)}[fault]
                        self.word(self.app + self.target['package_offset'], p)
                    else:
                        p = {'icon-null': 0, 'icon-invalid': 0xdead0000,
                             'icon-long': self.text(b'/' + b'x' * 255),
                             'icon-boundary': 0x3cffffff}[fault]
                        self.word(self.app + self.target['icon_offset'], p)
                        if fault == 'icon-boundary':
                            self.data(p, b'/')
                    self.assertEqual(self.call(), expected)

    def test_path_root_boundary_format_and_traversal(self):
        for version in TARGETS:
            prefix = TARGETS[version]['root'] + PACKAGE + b'/'
            cases = [(b'', 0), (prefix + b'icon.png', -2), (prefix + b'icon.BIN', -2),
                 (b'descriptor', -2), (b'internal://files/icon.bin', -2),
                 ((b'/data/quickapp/app/' if version == '1043' else b'/data/app/')
                  + PACKAGE + b'/icon.bin', -2),
                 (b'/resource/icon.bin', -2), (prefix + b'../icon.bin', -1),
                 (prefix + b'./icon.bin', -1), (prefix + b'a//icon.bin', -1),
                 (prefix + b'icon.bin?x', -2), (prefix + b'icon\\.bin', -1),
                 (prefix + b'icon\n.bin', -1), (prefix + b'icon\x7f.bin', -1),
                 (prefix + b'icon:.bin', -1), (prefix + b'%2e%2e/icon.bin', 1),
                 (prefix[:-1] + b'.other/icon.bin', 1), (prefix[:-1], -2),
                 (TARGETS[version]['root'] + b'i.bin', 1),
                 (TARGETS[version]['root'] + b'.bin', -2),
                 (TARGETS[version]['root'] + b'../icon.bin', -1),
                 (prefix + b'i.bin', 1),
                 (prefix + b'x' * (255 - len(prefix) - 4) + b'.bin', 1)]
            for path, expected in cases:
                with self.subTest(version=version, path=path):
                    self.machine(version, path=path)
                    self.assertEqual(self.call(), expected)

    def test_opaque_keys_use_exact_record_not_package_directory(self):
        packages = [b'', b'.', b'..', b'../bad', b'white space',
                    b'a/b:c\\d!?%#;[]()', '应用/图标 🚀'.encode(),
                    b'x' * 255, 'é'.encode() * 127 + b'x']
        for version in TARGETS:
            path = TARGETS[version]['root'] + 'other app/图标 %?#;[]()/图标 !.bin'.encode()
            for package in packages:
                with self.subTest(version=version, package=package):
                    self.machine(version, path=path, identity=package)
                    self.node(package)
                    self.assertEqual(self.call(package), 1)
                    self.assertEqual(self.calls, [(self.target['lookup'], package)])
                    self.assertEqual(self.out.value, path)
                    # A path under the right root cannot excuse wrong identity.
                    self.word(self.app + self.target['package_offset'],
                              self.text(b'wrong opaque identity'))
                    self.assertEqual(self.call(package), -1)

    def test_bad_package_and_null_output_do_not_call_native(self):
        for version in TARGETS:
            self.machine(version)
            for package in (None, b'x' * 256, b'\xc3\xa9' * 128,
                            *(b'key' + bytes([c]) for c in (*range(1, 32), 127))):
                self.assertEqual(self.call(package), -1)
                self.assertEqual(self.calls, [])
            self.assertEqual(self.lib.rh_platform_quickapp_icon_path(PACKAGE, None), -1)
            self.assertEqual(self.reads, [])


if __name__ == '__main__':
    unittest.main(verbosity=2)

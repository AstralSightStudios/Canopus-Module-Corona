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
PREFIX = b'/data/quickapp/app/' + PACKAGE + b'/'
PATH = PREFIX + b'images/icon.bin'
TARGETS = {'139': (0x2ca3da68, 0xc6a16a3), '155': (0x2ca3da58, 0xc6a1693)}
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
        for version in (*TARGETS, '1043'):
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

    def machine(self, version, path=PATH, identity=PACKAGE):
        self.version, self.lib = version, self.libs[version]
        self.memory, self.calls, self.reads, self.invalid_reads = {}, [], [], []
        self.next_memory = 0x3c710000
        self.return_record, self.replace = 0, False
        def read(a, n):
            self.reads.append((a, n))
            # Explicitly allocated memory only: unexpected reads fail the test.
            if any(a + i not in self.memory for i in range(n)):
                self.invalid_reads.append((a, n))
                return 0
            return int.from_bytes(bytes(self.memory[a + i] for i in range(n)), 'little')
        def lookup(a, p):
            self.calls.append((a, p))
            if self.replace:
                self.word(self.app + 16, self.text(PREFIX + b'new.bin'))
            return self.return_record
        self.reader, self.finder = READ(read), LOOKUP(lookup)
        self.lib.qa_setup(self.reader, self.finder)
        if version in TARGETS:
            table, finder = TARGETS[version]
            self.word(0x20084ec4, table)
            self.word(table + 12, finder)
            self.map = self.mem(68)
            self.word(self.map, 16)
            self.word(0x20084ebc, self.map)
            self.app = self.mem(80)
            self.return_record = self.app
            self.icon = self.text(path) if path is not None else 0
            self.word(self.app + 12, self.text(identity) if identity is not None else 0)
            self.word(self.app + 16, self.icon)

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

    def call(self, package=PACKAGE):
        self.calls.clear()
        self.out = C.create_string_buffer(b'X' * 255, 256)
        before = dict(self.memory)
        result = self.lib.rh_platform_quickapp_icon_path(package, self.out)
        self.assertEqual(self.invalid_reads, [])
        if not self.replace:
            self.assertEqual(self.memory, before)  # no native writes, ever
        if result != 1:
            self.assertEqual(self.out.value, b'')
        return result

    def test_supported_exact_targets_copy_and_no_pointer_retention(self):
        for version, (_, lookup) in TARGETS.items():
            with self.subTest(version=version):
                self.machine(version)
                self.assertEqual(self.call(), 1)
                self.assertEqual(self.calls, [(lookup, PACKAGE)])
                self.data(self.icon, b'X')
                self.assertEqual(self.out.value, PATH)

    def test_service_registry_guards_never_invoke_invalid_leaf(self):
        for version in TARGETS:
            for fault, expected in (('service-null', 0), ('service-wrong', -1),
                    ('callback', -1), ('registry-null', 0), ('registry-invalid', -1),
                    ('registry-count', -1), ('registry-unaligned', -1)):
                with self.subTest(version=version, fault=fault):
                    self.machine(version)
                    if fault.startswith('service-'):
                        self.word(0x20084ec4, 0 if fault.endswith('null') else 0xdead0000)
                    elif fault == 'callback':
                        self.word(TARGETS[version][0] + 12, TARGETS[version][1] - 1)
                    elif fault == 'registry-count':
                        self.word(self.map, 3)
                    else:
                        self.word(0x20084ebc, {'registry-null': 0,
                            'registry-invalid': 0xdead0000, 'registry-unaligned': self.map + 1}[fault])
                    self.assertEqual(self.call(), expected)
                    self.assertEqual(self.calls, [])

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
                        self.word(self.app + 12, p)
                    else:
                        p = {'icon-null': 0, 'icon-invalid': 0xdead0000,
                             'icon-long': self.text(b'/' + b'x' * 255),
                             'icon-boundary': 0x3cffffff}[fault]
                        self.word(self.app + 16, p)
                        if fault == 'icon-boundary':
                            self.data(p, b'/')
                    self.assertEqual(self.call(), expected)

    def test_path_root_boundary_format_and_traversal(self):
        cases = [(b'', 0), (PREFIX + b'icon.png', -2), (PREFIX + b'icon.BIN', -2),
                 (b'descriptor', -2), (b'internal://files/icon.bin', -2),
                 (b'/resource/icon.bin', -2), (PREFIX + b'../icon.bin', -1),
                 (PREFIX + b'./icon.bin', -1), (PREFIX + b'a//icon.bin', -1),
                 (PREFIX + b'icon.bin?x', -1), (PREFIX + b'icon\\.bin', -1),
                 (PREFIX + b'icon\n.bin', -1), (PREFIX + b'%2e%2e/icon.bin', -1),
                 (PREFIX[:-1] + b'.other/icon.bin', -1), (PREFIX[:-1], -1),
                 (PREFIX + b'i.bin', 1),
                 (PREFIX + b'x' * (255 - len(PREFIX) - 4) + b'.bin', 1)]
        for version in TARGETS:
            for path, expected in cases:
                with self.subTest(version=version, path=path):
                    self.machine(version, path=path)
                    self.assertEqual(self.call(), expected)

    def test_bad_package_null_output_and_1043_no_native_reads(self):
        for version in (*TARGETS, '1043'):
            self.machine(version)
            for package in (None, b'', b'../bad', b'x' * 256):
                self.assertEqual(self.call(package), -2 if version == '1043' else -1)
                self.assertEqual(self.calls, [])
            self.assertEqual(self.lib.rh_platform_quickapp_icon_path(PACKAGE, None),
                             -2 if version == '1043' else -1)
            if version == '1043':
                self.assertEqual(self.call(), -2)
                self.assertEqual(self.reads, [])


if __name__ == '__main__':
    unittest.main(verbosity=2)

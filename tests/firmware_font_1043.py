"""Exact-AP .043 selected-native font probes, not full host transactions.

Run: build/firmware-tests/bin/python tests/firmware_font_1043.py
The persisted audit is checked against AP bytes before mapping or execution.
Each test gets a fresh bounded machine. Synthetic RAM is not device state.
Only explicitly registered leaves are modeled; all other instructions execute
native bytes on the narrowly recorded paths. No filesystem, FT parsing, GPU,
OOM, concurrency, restart or whole-.155-equivalence claim is made.
"""
import copy
import hashlib
import json
from pathlib import Path
import struct
import unittest

from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS,
                     UC_HOOK_CODE, UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC)
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_R0,
    UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4,
    UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
    UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC)

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'build/firmware-analysis/vela_ap_3.101.043.bin'
AUDIT = ROOT / 'targets/xiaomi-band-10-pro-3.101.043/font-reload-audit.json'
SHA256 = '519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec'
AP_SIZE = 13795728
MAPPING_SHA256 = '3b61a7ee4eb31568445f4aa9a871ee70e2fc70f0c291a512bc3b1af48c78d2bf'
XIP, FLASH = 0x0c0c0000, 0x2c0c0000
ARGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
SAVED = (UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
         UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)
# (LDR r3 source, LDR r2 destination, LDR r1 end, source, data alias,
#  source end, execution alias). Inspected /tmp/fr043-startup.txt originals.
COPIES = (
    (0x0c0c0a1c, 0x0c0c0a1e, 0x0c0c0a20,
     0x2c0c23a4, 0x200765c0, 0x2c0fc6cc, 0x002765c0),
    (0x0c0c0a60, 0x0c0c0a62, 0x0c0c0a64,
     0x2c0fc6cc, 0x200b2860, 0x2c10d438, 0x002b2860),
    (0x0c0c0ac8, 0x0c0c0aca, 0x0c0c0acc,
     0x2c10d440, 0x3c000000, 0x2c18d940, 0x1c000000),
)


def check_image(image):
    if len(image) != AP_SIZE or hashlib.sha256(image).hexdigest() != SHA256:
        raise ValueError('.043 AP size/SHA256 mismatch; no mapping or execution')


def startup_copies(image):
    """Decode the actual Thumb literal LDRs, not a cached file-offset guess."""
    result = []
    for src_pc, dst_pc, end_pc, src, dst, end, execution in COPIES:
        for pc, register, expected in ((src_pc, 3, src), (dst_pc, 2, dst),
                                       (end_pc, 1, end)):
            opcode = struct.unpack_from('<H', image, pc - XIP)[0]
            if opcode & 0xff00 != 0x4800 | (register << 8):
                raise ValueError(f'not the expected literal LDR at {pc:#x}')
            literal = ((pc + 4) & ~3) + (opcode & 255) * 4
            actual = struct.unpack_from('<I', image, literal - XIP)[0]
            if actual != expected:
                raise ValueError(f'startup literal mismatch at {literal:#x}')
        offset, size = src - FLASH, end - src
        if offset < 0 or size <= 0 or offset + size > len(image):
            raise ValueError('startup copy outside AP')
        result.append((offset, size, dst, execution))
    return result


def native_bytes(image, address, size):
    if XIP <= address < XIP + len(image):
        offset = address - XIP
    elif FLASH <= address < FLASH + len(image):
        offset = address - FLASH
    else:
        for start, length, _data, execution in startup_copies(image):
            if execution <= address and address + size <= execution + length:
                offset = start + address - execution
                break
        else:
            raise ValueError(f'no AP byte mapping for {address:#x}')
    if offset + size > len(image):
        raise ValueError('byte witness outside AP')
    return offset, image[offset:offset + size]


def check_evidence(image, audit):
    check_image(image)
    startup_copies(image)
    if audit['ap043_sha256'] != SHA256 or audit['ap043_size'] != AP_SIZE:
        raise ValueError('audit AP identity mismatch')
    mapping = audit['map_baseline155_to_043']
    digest = hashlib.sha256(json.dumps(mapping, sort_keys=True,
                                      separators=(',', ':')).encode()).hexdigest()
    if len(mapping) != 45 or digest != MAPPING_SHA256:
        raise ValueError('incorrect exact 45-entry inventory')
    for desc, actual, persisted in zip(COPIES, startup_copies(image),
                                        audit['startup_copies'], strict=True):
        offset, size, data, execution = actual
        if (persisted['file_offset'], persisted['size'],
            int(persisted['data_address'], 16),
            int(persisted['execution_address'], 16)) != actual:
            raise ValueError('stale startup copy descriptor')
        if persisted['bytes_sha256'] != hashlib.sha256(image[offset:offset + size]).hexdigest():
            raise ValueError('stale startup copy bytes')
        for pc, witness in zip(desc[:3], persisted['literal_witnesses'], strict=True):
            opcode = struct.unpack_from('<H', image, pc - XIP)[0]
            literal = ((pc + 4) & ~3) + (opcode & 255) * 4
            value = struct.unpack_from('<I', image, literal - XIP)[0]
            if (int(witness['ldr_address'], 16) != pc or
                witness['opcode_hex'] != image[pc - XIP:pc - XIP + 2].hex() or
                int(witness['literal_address'], 16) != literal or
                int(witness['literal_value'], 16) != value):
                raise ValueError('stale startup literal witness')
    for item in (audit['selected_native_bodies'] + audit['native_execution_spans'] +
                 audit['mapped_target_byte_witnesses']):
        offset, data = native_bytes(image, int(item['address'], 16), item['size'])
        if (offset != item['file_offset'] or data.hex() != item['bytes_hex'] or
                hashlib.sha256(data).hexdigest() != item['sha256']):
            raise ValueError(f"stale native byte witness {item['address']}")
    for reference in audit['runtime_global_literal_references']:
        if mapping[reference['baseline']] != reference['target']:
            raise ValueError('stale runtime global identity')
        word = int(reference['target'], 16).to_bytes(4, 'little')
        if reference['literal_hex'] != word.hex():
            raise ValueError('stale runtime global literal encoding')
        if not reference['literal_file_offsets']:
            base = int(reference['base_literal'], 16)
            if base + reference['field_offset'] != int(reference['target'], 16):
                raise ValueError('stale runtime global base/field identity')
            offset = reference['base_literal_file_offset']
            if offset % 4 or image[offset:offset + 4] != base.to_bytes(4, 'little'):
                raise ValueError('stale runtime global base literal witness')
        for offset in reference['literal_file_offsets']:
            if offset % 4 or image[offset:offset + 4] != word:
                raise ValueError('stale runtime global literal witness')


class Machine:
    STOP, SP, STACK_LOW = 0x00100000, 0x201ff000, 0x201fd000
    BUDGET, TIMEOUT_US = 20000, 2000000

    def __init__(self, image, audit):
        # Fail closed before Uc is even created.
        check_evidence(image, audit)
        self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
        rx, rw = UC_PROT_READ | UC_PROT_EXEC, UC_PROT_READ | UC_PROT_WRITE
        rounded = (len(image) + 4095) & ~4095
        for address in (XIP, FLASH):
            self.u.mem_map(address, rounded, rx)
            self.u.mem_write(address, image)
        self.u.mem_map(0x20000000, 0x200000, rw)
        self.u.mem_map(0x00200000, 0x100000, rx)
        self.u.mem_map(0x1c000000, 0x100000, rx)
        self.u.mem_map(0x3c000000, 0x100000, rw)
        for offset, size, data, execution in startup_copies(image):
            self.u.mem_write(data, image[offset:offset + size])
            self.u.mem_write(execution, image[offset:offset + size])
        # These copies are separate synthetic views, not coherent alias emulation.
        self.u.mem_map(self.STOP, 0x1000, rx)
        self.u.mem_write(self.STOP, b'\x00\xbe')
        self.leaves, self.seen, self.executed = {}, [], {}
        self.allowed = [(int(s['address'], 16), int(s['address'], 16) + s['size'])
                        for s in audit['native_execution_spans']]
        self.u.hook_add(UC_HOOK_CODE, self.hook)

    def write(self, address, value):
        self.u.mem_write(address, struct.pack('<I', value & 0xffffffff))

    def read(self, address):
        return struct.unpack('<I', self.u.mem_read(address, 4))[0]

    def zero(self, address, size):
        self.u.mem_write(address, bytes(size))

    def hook(self, uc, address, size, _user):
        if address == self.STOP:
            uc.emu_stop()
            return
        sp = uc.reg_read(UC_ARM_REG_SP)
        if not self.STACK_LOW <= sp <= self.SP:
            raise AssertionError(f'stack escaped bounds: {sp:#x}')
        if address in self.leaves:
            args = [uc.reg_read(r) for r in ARGS]
            self.seen.append((address, args))
            result = self.leaves[address](args)
            # An AAPCS leaf may destroy caller-saved registers, not r4-r11.
            for i, register in enumerate((*ARGS[1:], UC_ARM_REG_R12)):
                uc.reg_write(register, 0xd00d0000 + i)
            uc.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
            return
        if not any(lo <= address and address + size <= hi for lo, hi in self.allowed):
            raise AssertionError(f'unrecorded native path: {address:#x}')
        self.executed[address] = size

    def run(self, address, *args):
        self.u.mem_write(self.STACK_LOW - 32, b'\x5a' * 32)
        self.u.mem_write(self.STACK_LOW, b'\xa5' * (self.SP - self.STACK_LOW))
        self.u.mem_write(self.SP, b'\x5a' * 32)
        self.u.reg_write(UC_ARM_REG_SP, self.SP)
        self.u.reg_write(UC_ARM_REG_LR, self.STOP | 1)
        for register, value in zip(ARGS, (*args, *([0xa5a5a5a5] * 4))):
            self.u.reg_write(register, value)
        patterns = [0x44440000 + i * 0x1111 for i in range(len(SAVED))]
        for register, value in zip(SAVED, patterns):
            self.u.reg_write(register, value)
        self.u.reg_write(UC_ARM_REG_R12, 0xa5a5a5a5)
        self.u.emu_start(address | 1, 0, timeout=self.TIMEOUT_US, count=self.BUDGET)
        if self.u.reg_read(UC_ARM_REG_PC) != self.STOP:
            raise AssertionError('native call failed to return within instruction/time budget')
        if self.u.reg_read(UC_ARM_REG_SP) != self.SP:
            raise AssertionError('unbalanced stack')
        for register, value in zip(SAVED, patterns):
            if self.u.reg_read(register) != value:
                raise AssertionError('callee-saved register changed')
        for guard in (self.STACK_LOW - 32, self.SP):
            if bytes(self.u.mem_read(guard, 32)) != b'\x5a' * 32:
                raise AssertionError('stack canary overwritten')
        return self.u.reg_read(UC_ARM_REG_R0)


class NativeFont1043(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = FIRMWARE.read_bytes()
        cls.audit = json.loads(AUDIT.read_text())
        check_evidence(cls.image, cls.audit)

    def setUp(self):
        self.m = Machine(self.image, self.audit)

    def test_01_face_comparator(self):
        m = self.m
        a, b, p, q = 0x20001000, 0x20001100, 0x20001200, 0x20001300
        m.u.mem_write(p, b'/same.ttf\0')
        m.u.mem_write(q, b'/same.ttf\0')
        def cstring(address):
            result = bytearray()
            for i in range(128):
                value = bytes(m.u.mem_read(address + i, 1))
                if value == b'\0':
                    return bytes(result)
                result.extend(value)
            raise AssertionError('modeled strcmp string exceeds bound')
        def strcmp(args):
            left, right = map(cstring, args[:2])
            return (left > right) - (left < right)
        m.leaves[0x1c06062c] = strcmp
        for key, path in ((a, p), (b, q)):
            m.write(key, path)
            m.write(key + 4, 65536)
            m.write(key + 8, 123)
        self.assertEqual(m.run(0x1c05682c, a, b), 0)
        self.assertEqual(len(m.seen), 1)  # Different pointers, equal content.
        m.write(b + 4, 65537)
        self.assertEqual(m.run(0x1c05682c, a, b), 0xffffffff)
        m.write(b + 4, 131072)
        self.assertEqual(m.run(0x1c05682c, a, b), 0xffffffff)
        m.write(b + 4, 65536)
        m.u.mem_write(q, b'/zzzz.ttf\0')
        self.assertEqual(m.run(0x1c05682c, a, b), 0xffffffff)

    def test_02_child_comparators(self):
        m = self.m
        a, b = 0x20001000, 0x20001100
        for key in (a, b):
            m.write(key, 42)
            m.write(key + 4, 24)
        self.assertEqual(m.run(0x1c056f04, a, b), 0)
        m.write(b + 4, 26)
        self.assertEqual(m.run(0x1c056f04, a, b), 0xffffffff)
        self.assertEqual(m.run(0x1c057258, a, b), 0)
        m.write(b, 43)
        self.assertEqual(m.run(0x1c057258, a, b), 0xffffffff)
        self.assertFalse(m.seen)

    def test_03_count_cache_init(self):
        m, cache = self.m, 0x20002000
        m.zero(cache, 64)
        for offset, value in ((0, 0x2cce571c), (4, 32), (8, 512),
                              (16, 0x1c056f05), (20, 0x1c056d25),
                              (24, 0x1c056d19)):
            m.write(cache + offset, value)
        self.assertEqual(m.run(0x0c272d38, cache, 0, 0), 1)
        for offset, expected in ((44, 52), (48, 4), (52, 0), (56, 0),
                                 (40, 0x1c056f05), (60, 0x0c272a61)):
            self.assertEqual(m.read(cache + offset), expected)
        self.assertFalse(m.seen)

    def test_04_vector_drop_r1(self):
        m, cache, obj = self.m, 0x20002000, 0x20003000
        m.write(0x2013eae0, cache)
        def drop(args):
            self.assertEqual(args[0], cache)
            self.assertEqual(args[2], 0)
            self.assertEqual(bytes(m.u.mem_read(args[1], 16)),
                             struct.pack('<IIII', 0, obj, 0, 0))
            return 0
        m.leaves[0x0cac0580] = drop
        m.run(0x0ca65c24, 0, obj, 0)
        self.assertEqual(len(m.seen), 1)

    def release_fixture(self):
        m = self.m
        font, descriptor, face, glyph, entry, cache = (
            0x20004000, 0x20004100, 0x20004200,
            0x20004300, 0x20004400, 0x20002000)
        m.write(font + 24, descriptor)
        m.write(descriptor + 52, face)
        m.write(face + 20, cache)
        m.write(glyph + 20, entry)
        def release(args):
            self.assertEqual(args[:3], [cache, entry, 0])
            return 0
        m.leaves[0x1c0592ac] = release
        return font, glyph, entry

    def test_05_outline_release(self):
        m = self.m
        font, glyph, _entry = self.release_fixture()
        m.run(0x1c057288, font, glyph, 0)
        self.assertEqual(m.read(glyph + 20), 0)
        self.assertEqual(len(m.seen), 1)
        m.run(0x1c057288, font, glyph, 0)
        self.assertEqual(len(m.seen), 1)  # Cleared saved entry: no second release.

    def test_06_mulfix(self):
        self.assertEqual(self.m.run(0x0c33d8bc, 65536, 80, 0), 80)
        self.assertFalse(self.m.seen)

    def test_07_existing_face_factory_24(self):
        m = self.m
        ctx, intern, path, entry, d, ft, metrics, cache = (
            0x20005000, 0x20005100, 0x20005200, 0x20005318,
            0x20005400, 0x20005500, 0x20005600, 0x20002000)
        face = entry - 24
        m.u.mem_write(path, b'/test.ttf\0')
        m.write(0x20103374, ctx)
        for offset, value in ((4, 8), (8, intern), (12, intern), (24, cache)):
            m.write(ctx + offset, value)
        for offset, value in ((0, path), (4, 1), (8, 0), (12, 0)):
            m.write(intern + offset, value)
        for offset, value in ((0, cache), (4, 1), (8, 24)):
            m.write(entry + offset, value)
        m.write(face + 12, ft)
        m.write(ft + 88, metrics)
        m.write(metrics + 20, 65536)
        m.write(metrics + 28, 1920)
        m.write(metrics + 32, -640)
        m.u.mem_write(ft + 80, struct.pack('<hh', 24, 16))
        m.u.mem_write(d - 16, b'\xa5' * 96)
        def lookup(args):
            self.assertEqual(args[0], cache)
            self.assertEqual(args[2], 0)
            self.assertEqual(bytes(m.u.mem_read(args[1], 24)),
                             struct.pack('<IIIIII', path, 65536, 0, 0, 0, 0))
            return entry
        def allocate_zeroed(args):
            self.assertEqual(args[0], 64)
            m.zero(d, 64)  # Native lv_malloc_zeroed contract, not malloc.
            return d
        def set_pixel_sizes(args):
            self.assertEqual(args[:3], [ft, 0, 24])
            return 0
        m.leaves = {0x0c1666d0: lookup, 0x0c16dabc: allocate_zeroed,
                    0x0c187b40: set_pixel_sizes}
        self.assertEqual(m.run(0x0c163da8, path, 1, 24, 0), d + 4)
        for offset, expected in ((0, 1600079444), (40, 24), (44, 65536),
                                 (48, ctx), (52, face), (56, entry), (60, path),
                                 (4, 0x1c056e15), (8, 0x1c0572a5),
                                 (12, 0x1c057289), (28, d)):
            self.assertEqual(m.read(d + offset), expected)
        self.assertEqual(m.read(intern + 4), 2)
        self.assertEqual([a for a, _ in m.seen], [0x0c1666d0, 0x0c16dabc, 0x0c187b40])
        for guard in (d - 16, d + 64):
            self.assertEqual(bytes(m.u.mem_read(guard, 16)), b'\xa5' * 16)

    def test_08_vector_append(self):
        m = self.m
        src, dst, spoints, dpoints = 0x20006000, 0x20006100, 0x20006200, 0x20006300
        m.zero(src, 128)
        m.zero(dst, 128)
        for address, value in ((src + 36, 1), (src + 40, 4), (src + 44, spoints),
                               (dst + 40, 3), (dst + 44, dpoints), (dst + 124, 256)):
            m.write(address, value)
        m.u.mem_write(spoints, b'abcd')
        m.u.mem_write(dpoints, b'xyz' + b'\xa5' * 253)
        m.run(0x1c04c884, dst, src, 0)
        self.assertEqual(m.read(dst + 36), 0)
        self.assertEqual(m.read(dst + 40), 7)
        self.assertEqual(bytes(m.u.mem_read(dpoints, 7)), b'xyzabcd')
        self.assertEqual(bytes(m.u.mem_read(dpoints + 7, 249)), b'\xa5' * 249)
        m.u.mem_write(spoints, b'zzzz')
        self.assertEqual(bytes(m.u.mem_read(dpoints, 7)), b'xyzabcd')
        self.assertFalse(m.seen)  # Allocation/upload branches excluded.

    def test_09_busy_dispatch(self):
        m, unit = self.m, 0x20007000
        m.write(unit + 32, 123)
        self.assertEqual(m.run(0x1c045360, unit, 0, 0), 0)
        self.assertEqual(m.run(0x1c03debc, unit, 0, 0), 0)
        self.assertEqual(m.read(unit + 32), 123)
        self.assertFalse(m.seen)

    def test_10_error_finish_drains(self):
        m = self.m
        font, _glyph, entry = self.release_fixture()
        unit, q, images, gradients, grad = (
            0x20007000, 0x20007100, 0x20007200, 0x20007300, 0x20007400)
        for pointer in (q, images, gradients):
            m.zero(pointer, 44)
        m.write(grad + 8, gradients)
        for offset, value in ((32, 0), (36, images), (40, grad), (48, q), (52, 7)):
            m.write(unit + offset, value)
        m.write(font + 8, 0x1c057289)
        for offset, buffer in ((4, 0x20007500), (20, 0x20007600)):
            for field, value in ((0, buffer), (4, 1), (8, 1), (12, 24)):
                m.write(q + offset + field, value)
            m.write(buffer, font)
            m.write(buffer + 20, entry)
        m.write(q + 36, 0x1c046cfd)
        m.leaves.update({0x1c07a770: lambda args: 1,  # GPU finish error
                         0x1c05cb60: lambda args: 0,  # Diagnostic log
                         0x1c04d388: lambda args: 0})  # Error handler
        m.run(0x1c04ec20, unit, 0, 0)
        self.assertEqual(m.read(q + 8), 0)
        self.assertEqual(m.read(q + 24), 0)
        self.assertEqual(m.read(unit + 52), 0)
        self.assertEqual([a for a, _ in m.seen],
                         [0x1c07a770, 0x1c05cb60, 0x1c04d388,
                          0x1c0592ac, 0x1c0592ac])

    def test_11_acquire_create_null_balances_entry(self):
        m = self.m
        cache, key, data, user = 0x20008000, 0x20008100, 0x20008200, 0x20008300
        entry = data + 24
        m.zero(cache, 64)
        m.zero(key, 24)
        m.write(cache, 0x2cce571c)
        m.write(cache + 4, 24)
        m.write(cache + 8, 256)
        m.write(cache + 20, 0x1c056719)  # Exact native face-create callback identity.
        live, linked, events = set(), set(), []
        def reserve(args):
            self.assertEqual(args, [cache, key, 0, user])
            return 0  # Class capacity check permits insertion; no eviction.
        def insert(args):
            self.assertEqual(args[:3], [cache, key, user])
            self.assertFalse(live)
            live.add(data)
            linked.add(entry)
            m.zero(data, 40)
            m.write(entry, cache)
            m.write(entry + 8, 24)
            m.write(cache + 12, 1)
            events.append(('allocate_insert', data))
            return entry
        def fail_create(args):
            self.assertEqual(args[:2], [data, user])
            self.assertEqual(live, {data})
            self.assertEqual(linked, {entry})
            events.append(('create_returns_null', data))
            return 0  # Models callback failure, NOT a real FT parser failure.
        def unlink(args):
            self.assertEqual(args[:3], [cache, entry, user])
            self.assertEqual(m.read(entry + 4), 0)  # No acquire on failure.
            linked.remove(entry)
            m.write(cache + 12, 0)
            events.append(('unlink', entry))
            return 0
        def free(args):
            self.assertEqual(args[0], data)  # Native free subtracts entry+8.
            self.assertFalse(linked, 'entry must be unlinked before free')
            live.remove(data)  # Double free or wrong pointer fails immediately.
            events.append(('free', data))
            return 0
        m.leaves = {0x0c272a80: reserve, 0x0c272e58: insert,
                    0x1c056718: fail_create, 0x0c272ce8: unlink,
                    0x0c16dae4: free}
        self.assertEqual(m.run(0x0c166740, cache, key, user), 0)
        self.assertEqual(events, [('allocate_insert', data),
                                  ('create_returns_null', data),
                                  ('unlink', entry), ('free', data)])
        self.assertFalse(live)
        self.assertFalse(linked)
        self.assertEqual(m.read(cache + 12), 0)
        # Generic acquire-create, capacity/insertion wrapper, payload accessor,
        # and entry-free body execute. Class insertion/unlink/heap are modeled.
        for address in (0x0c166740, 0x0c1665d0, 0x0c16692c, 0x0c1669f0):
            self.assertIn(address, m.executed)

    def face_destroy_accounting(self, child_offset):
        m = self.m
        face, ft, child, user, path = (
            0x20008000, 0x20008100, 0x20008200, 0x20008300, 0x20008400)
        m.zero(face, 24)
        m.write(face, path)
        m.write(face + 12, ft)
        if child_offset is not None:
            m.write(face + child_offset, child)
        live_ft = {ft}
        live_caches = {child} if child_offset is not None else set()
        # The parent face payload and interned pathname belong to outer owners;
        # this destructor must not free either or read a .155 seventh word.
        m.u.mem_write(face + 24, b'\xa5' * 16)
        events = []
        def done_face(args):
            self.assertEqual(args[0], ft)
            live_ft.remove(ft)
            events.append(('FT_Done_Face', ft))
            return 0
        def cache_destroy(args):
            self.assertEqual(args[:2], [child, user])
            self.assertEqual(m.read(face + child_offset), child)
            live_caches.remove(child)
            events.append(('cache_destroy', child))
            return 0
        m.leaves = {0x1c07a580: done_face, 0x1c05927c: cache_destroy}
        m.run(0x1c0566ec, face, user, 0)
        self.assertFalse(live_ft)
        self.assertFalse(live_caches)
        expected = [('FT_Done_Face', ft)]
        if child_offset is not None:
            expected.append(('cache_destroy', child))
        self.assertEqual(events, expected)
        self.assertEqual(m.read(face + 16), 0)
        self.assertEqual(m.read(face + 20), 0)
        self.assertEqual(m.read(face), path)
        self.assertEqual(m.read(face + 12), ft)  # Native code does not clear it.
        self.assertEqual(bytes(m.u.mem_read(face + 24, 16)), b'\xa5' * 16)
        self.assertIn(0x1c0566ec, m.executed)

    def test_12_face_destroy_zero_children(self):
        self.face_destroy_accounting(None)

    def test_13_face_destroy_metrics_only(self):
        self.face_destroy_accounting(16)

    def test_14_face_destroy_outline_only(self):
        self.face_destroy_accounting(20)


class EvidenceGuards1043(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = FIRMWARE.read_bytes()
        cls.audit = json.loads(AUDIT.read_text())
        check_evidence(cls.image, cls.audit)

    def test_wrong_length_rejected(self):
        with self.assertRaisesRegex(ValueError, 'size/SHA256'):
            Machine(self.image[:-1], self.audit)

    def test_same_length_wrong_hash_rejected(self):
        image = bytearray(self.image)
        image[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'size/SHA256'):
            Machine(bytes(image), self.audit)

    def test_wrong_audit_hash_rejected(self):
        audit = copy.deepcopy(self.audit)
        audit['ap043_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'audit AP identity'):
            Machine(self.image, audit)

    def test_stale_body_rejected(self):
        audit = copy.deepcopy(self.audit)
        audit['selected_native_bodies'][0]['bytes_hex'] = '00'
        with self.assertRaisesRegex(ValueError, 'stale native byte witness'):
            Machine(self.image, audit)

    def test_startup_literals_independently_checked(self):
        # Call only the literal helper to exercise validation in isolation;
        # Machine would reject this image earlier by SHA256.
        image = bytearray(self.image)
        image[0xb20] ^= 1  # PSRAM source literal 0x0c0c0b20.
        with self.assertRaisesRegex(ValueError, 'startup literal mismatch'):
            startup_copies(image)


if __name__ == '__main__':
    unittest.main(verbosity=2)

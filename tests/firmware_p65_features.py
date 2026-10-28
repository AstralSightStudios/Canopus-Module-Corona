#!/usr/bin/env python3
"""Run exact P65 native font retirement and package registry code in Unicorn.

GPU hardware finish/logging and native release callbacks are controlled leaves;
all wait/drain/array traversal and registry hash/lookup instructions are original.
No rendering, GPU coherency, native allocation or device success is claimed.
"""
from pathlib import Path
import struct
import json
import unittest
from p65_firmware import firmware, XIP
try:
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
    from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_SP
except ImportError:
    raise SystemExit("Install unicorn to run P65 feature instruction probes")


class FirmwareFeatures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.ap = firmware()
        except FileNotFoundError as error:
            raise unittest.SkipTest(str(error))

    def machine(self):
        u = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        for base, size in [(XIP, 0xc00000), (0x2c0c0000, 0xc00000), (0x1c000000, 0x400000), (0x3c000000, 0x600000), (0x20000000, 0x200000), (0x00300000, 0x100000)]:
            u.mem_map(base, size)
        u.mem_write(XIP, self.ap)
        u.mem_write(0x2c0c0000, self.ap)
        copied = self.ap[0x2fdc4:0x401e44]
        u.mem_write(0x1c000000, copied)
        u.mem_write(0x3c000000, copied)
        evidence = json.loads((Path(__file__).resolve().parents[1] / "targets/redmi-watch-6-3.100.043/core-evidence.json").read_text())
        for mapping in evidence["runtime_mapping"]:
            if "file_offset" not in mapping or mapping["kind"] == "XIP":
                continue
            start, size = int(mapping["file_offset"], 0), int(mapping["size"], 0)
            for key in ("vma", "cached_vma", "execute_vma"):
                if key in mapping:
                    u.mem_write(int(mapping[key], 0), self.ap[start:start + size])
        u.mem_write(0x20001000, b'\x70\x47')
        u.reg_write(UC_ARM_REG_SP, 0x201ff000)
        return u

    @staticmethod
    def put(u, address, *words):
        u.mem_write(address, struct.pack('<' + 'I' * len(words), *words))

    def run_native(self, u, entry, *args):
        for reg, arg in zip((UC_ARM_REG_R0, UC_ARM_REG_R1), args):
            u.reg_write(reg, arg)
        u.reg_write(UC_ARM_REG_LR, 0x20001001)
        u.emu_start(entry | 1, 0x20001000, count=100000)
        self.assertEqual(u.reg_read(UC_ARM_REG_PC), 0x20001000)
        return u.reg_read(UC_ARM_REG_R0)

    def test_native_wait_drains_all_cpu_references_after_success_or_error(self):
        for gpu_result in (0, 12):
            with self.subTest(gpu_result=gpu_result):
                u = self.machine()
                unit = 0x3c500000
                callbacks = []
                gpu_calls = []
                containers = [0x3c501000, 0x3c502000, 0x3c503000]
                for index, container in enumerate(containers):
                    first = 0x3c510000 + index * 0x1000
                    second = first + 0x100
                    self.put(u, container + 4, first, 2, 2, 4)
                    self.put(u, container + 20, second, 1, 1, 4)
                    self.put(u, container + 36, 0x20002001, 0xabc000 + index)
                self.put(u, unit + 36, containers[0])
                self.put(u, unit + 44, containers[1])
                self.put(u, unit + 28, containers[2])
                self.put(u, unit + 52, 91)
                def controlled_leaf(machine, address, size, _):
                    if address == 0xc8bc080:
                        gpu_calls.append(address)
                        machine.reg_write(UC_ARM_REG_R0, gpu_result)
                    elif address == 0x20002000:
                        callbacks.append((machine.reg_read(UC_ARM_REG_R0), machine.reg_read(UC_ARM_REG_R1)))
                    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))
                for address in (0xc8bc080, 0xc73e32c, 0x20002000):
                    u.hook_add(UC_HOOK_CODE, controlled_leaf, begin=address, end=address)
                self.run_native(u, 0xc72ef2c, unit)
                self.assertEqual(len(gpu_calls), 1)
                self.assertEqual(callbacks, [(0x3c510000 + i * 0x1000 + offset, 0xabc000 + i) for i in range(3) for offset in (0, 4, 0x100)])
                for container in containers:
                    self.assertEqual(struct.unpack('<I', u.mem_read(container + 8, 4))[0], 0)
                    self.assertEqual(struct.unpack('<I', u.mem_read(container + 24, 4))[0], 0)
                self.assertEqual(struct.unpack('<I', u.mem_read(unit + 52, 4))[0], 0)

    def test_native_package_lookup_uses_active_registry_and_record(self):
        u = self.machine()
        table, node, package, record, query = 0x3c500000, 0x3c501000, 0x3c502000, 0x3c503000, 0x3c504000
        key = b'ng.lst.corona'
        value = 5381
        for byte in key:
            value = (33 * value + byte) & 0xffffffff
        self.put(u, table, 4, 0, 0, 0, 0)
        self.put(u, table + 4 * (1 + (value & 3)), node)
        self.put(u, node, value, package, len(key) + 1, record, 0)
        self.put(u, 0x3c3cf92c, table)
        u.mem_write(package, key + b'\0')
        u.mem_write(query, key + b'\0')
        # strcmp is a controlled libc leaf; registry traversal and hashing run
        # original firmware. The early libc SRAM copy is outside this fixture.
        def strcmp_leaf(machine, address, size, _):
            def string(pointer):
                return bytes(machine.mem_read(pointer, 256)).split(b"\0", 1)[0]
            left, right = string(machine.reg_read(UC_ARM_REG_R0)), string(machine.reg_read(UC_ARM_REG_R1))
            machine.reg_write(UC_ARM_REG_R0, 0 if left == right else 1)
            machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))
        u.hook_add(UC_HOOK_CODE, strcmp_leaf, begin=0xc8bd1d8, end=0xc8bd1d8)
        self.assertEqual(self.run_native(u, 0x1c15165c, query), record)
        u.mem_write(query, b'not.installed\0')
        self.assertEqual(self.run_native(u, 0x1c15165c, query), 0)


if __name__ == '__main__':
    unittest.main()

"""Execute exact P65 Thumb leaves and the compiled resource-cache adapter.

Native OS/mutex/callback leaves are modeled. This does not boot firmware or
prove GPU/hardware behavior. No other target's firmware or address map is used.
"""
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
from elftools.elf.elffile import ELFFile
from p65_firmware import ROOT, XIP, CACHED, firmware, macros

REGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
STOP = 0x02000000
CALLBACK = 0x02000100
SCRATCH = 0x02010000


class Machine:
    def __init__(self, data, evidence, executable=None):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        for base, size in ((XIP, 0xC00000), (CACHED, 0xC00000),
                           (0x00300000, 0x100000), (0x20000000, 0x200000),
                           (0x1C000000, 0x600000), (0x3C000000, 0x600000),
                           (0x01000000, 0x100000), (STOP, 0x100000)):
            self.uc.mem_map(base, size)
        self.uc.mem_write(XIP, data)
        self.uc.mem_write(CACHED, data)
        for record in evidence["runtime_mapping"]:
            if "file_offset" not in record or record["kind"] == "XIP":
                continue
            offset, size = int(record["file_offset"], 0), int(record["size"], 0)
            for key in ("vma", "cached_vma", "execute_vma"):
                if key in record:
                    self.uc.mem_write(int(record[key], 0), data[offset:offset + size])
        self.symbols = {}
        if executable:
            elf = ELFFile(io.BytesIO(executable))
            for segment in elf.iter_segments():
                if segment["p_type"] == "PT_LOAD":
                    address = segment["p_vaddr"]
                    start = address & ~4095
                    end = (address + segment["p_memsz"] + 4095) & ~4095
                    if not any(lo <= start and end <= hi + 1 for lo, hi, _ in self.uc.mem_regions()):
                        self.uc.mem_map(start, end - start)
                    self.uc.mem_write(address, segment.data())
            self.symbols = {symbol.name: symbol["st_value"] for symbol in elf.get_section_by_name(".symtab").iter_symbols()}
        self.hooks = {}
        self.uc.hook_add(UC_HOOK_CODE, self.dispatch)

    def dispatch(self, uc, address, size, cookie):
        if address == STOP:
            uc.emu_stop()
        elif address in self.hooks:
            result = self.hooks[address](*[uc.reg_read(reg) for reg in REGS])
            if result is not None:
                uc.reg_write(UC_ARM_REG_R0, result & 0xFFFFFFFF)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def call(self, address, *args):
        for index, reg in enumerate(REGS):
            self.uc.reg_write(reg, args[index] if index < len(args) else 0)
        self.uc.reg_write(UC_ARM_REG_SP, STOP + 0xF0000)
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.uc.emu_start(address | 1, STOP + 2, count=300000)
        if self.uc.reg_read(UC_ARM_REG_PC) != STOP:
            raise AssertionError("instruction budget exhausted")
        value = self.uc.reg_read(UC_ARM_REG_R0)
        return value if value < 0x80000000 else value - 0x100000000

    def put(self, address, *values):
        self.uc.mem_write(address, struct.pack("<" + "I" * len(values), *values))

    def get(self, address):
        return struct.unpack("<I", self.uc.mem_read(address, 4))[0]

    def string(self, address):
        return bytes(self.uc.mem_read(address, 256)).split(b"\0", 1)[0]


class FirmwareP65(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = firmware()
        cls.evidence = json.loads((ROOT / "targets/redmi-watch-6-3.100.043/core-evidence.json").read_text())
        cls.macros = macros()
        cls.temporary = tempfile.TemporaryDirectory(prefix="rh-p65-thumb-")
        cls.addClassCleanup(cls.temporary.cleanup)
        temporary = Path(cls.temporary.name)
        harness = temporary / "harness.c"
        harness.write_text('''#include <stdint.h>
void _start(void) {}
void *rh_platform_alloc(uint32_t n) {(void)n; return (void *)0x02010000u;}
void rh_platform_free(void *p) {(void)p;}
''')
        executable = temporary / "adapter.elf"
        # Keep the production core entry points exercised below. Font/calendar/
        # backgrounds have their own full adapter harnesses and native leaves.
        subprocess.run([os.environ.get("CLANG", "clang"), "--target=arm-none-eabi",
            "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft", "-O2", "-fno-builtin",
            "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-nostdlib",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-Wl,--undefined=rh_platform_driver_valid",
            "-Wl,--undefined=rh_platform_retire_mapped_images",
            "-Wl,--undefined=rh_platform_redraw_ready",
            "-Wl,--undefined=rh_platform_request_full_redraw",
            "-DRH_TARGET_P65=1", "-DRH_NATIVE_MANAGER=1", "-I" + str(ROOT / "include"),
            "-fuse-ld=lld", "-Wl,-Ttext=0x01000000", "-Wl,-e,_start",
            str(ROOT / "src/platform.c"), str(ROOT / "src/resource_hook.c"),
            str(harness), "-o", str(executable)], check=True)
        cls.executable = executable.read_bytes()

    def machine(self):
        return Machine(self.data, self.evidence, self.executable)

    def test_exact_native_body_fingerprints(self):
        for record in self.evidence["functions"]:
            with self.subTest(function=record["name"]):
                witness = record["witness"]
                expected = bytes.fromhex(witness["hex"])
                offset = int(record["file_offset"], 0)
                self.assertEqual(self.data[offset:offset + witness["size"]], expected)
                self.assertEqual(hashlib.sha256(expected).hexdigest(), witness["sha256"])

    def test_posix_modes_path_success_and_failure(self):
        for mode, flags in ((1, 6), (2, 1), (3, 7)):
            for fd in (0, 7, -1):
                with self.subTest(mode=mode, fd=fd):
                    m = self.machine()
                    path = b"resource/app/launcher/icon.bin"
                    m.uc.mem_write(SCRATCH, path + b"\0")
                    calls = []
                    def snprintf(out, cap, format_address, source):
                        self.assertEqual(m.string(format_address), b"/%s")
                        value = b"/" + m.string(source)
                        m.uc.mem_write(out, value[:cap - 1] + b"\0")
                        return len(value)
                    def native_open(name, actual_flags, permissions, unused):
                        calls.append((m.string(name), actual_flags, permissions))
                        return fd
                    m.hooks[0x0C741A50] = snprintf
                    m.hooks[0x0C8BEF00] = native_open
                    m.put(SCRATCH + 512, 2)
                    m.hooks[0x0C4F793C] = lambda *args: SCRATCH + 512
                    m.hooks[0x0C73E32C] = lambda *args: 0
                    result = m.call(self.macros["RH_FW_POSIX_OPEN"], 0, SCRATCH, mode)
                    self.assertEqual(result, fd + 1 if fd >= 0 else 0)
                    self.assertEqual(calls, [(b"/" + path, flags, 0o666)])

    def test_driver_validation_uses_p65_zero_cache_size(self):
        m = self.machine()
        driver = self.macros["RH_FW_POSIX_DRIVER"]
        m.uc.mem_write(driver, b"/")
        m.put(driver + 4, 0)
        self.assertEqual(m.call(m.symbols["rh_platform_driver_valid"]), 1)
        m.put(driver + 4, 4096)
        self.assertEqual(m.call(m.symbols["rh_platform_driver_valid"]), 0)

    def test_resolution_accessors_follow_p65_rotation(self):
        m = self.machine()
        display = SCRATCH
        m.put(display, 432, 514)
        for rotated in (False, True):
            m.uc.mem_write(display + 748, bytes([2 if rotated else 0]))
            self.assertEqual(m.call(self.macros["RH_FW_DISPLAY_WIDTH"], display), 514 if rotated else 432)
            self.assertEqual(m.call(self.macros["RH_FW_DISPLAY_HEIGHT"], display), 432 if rotated else 514)

    def test_redraw_guard_uses_p65_active_screen(self):
        m = self.machine()
        display = SCRATCH
        m.put(self.macros["RH_FW_DISPLAY_SLOT"], display)
        m.put(display + 608, 1)
        m.put(display + 696, SCRATCH + 1024)
        self.assertEqual(m.call(m.symbols["rh_platform_redraw_ready"]), 0)
        m.put(display + 688, SCRATCH + 1024)
        self.assertEqual(m.call(m.symbols["rh_platform_redraw_ready"]), 1)
        m.uc.mem_write(display + 58, b"\x02")
        self.assertEqual(m.call(m.symbols["rh_platform_redraw_ready"]), 0)

    def test_adapter_retires_only_mapped_file_keys_with_zero_user_data(self):
        m = self.machine()
        source = SCRATCH + 0x3000
        foreign = source + 256
        rule, view = SCRATCH + 0x4000, SCRATCH + 0x5000
        path = b"/resource/icons/selected.bin"
        m.uc.mem_write(source, path + b"\0")
        m.uc.mem_write(foreign, b"/resource/icons/foreign.bin\0")
        m.uc.mem_write(rule, path.ljust(256, b"\0") + b"/data/canopus/m.corona/themes/demo/selected.bin".ljust(256, b"\0"))
        m.put(view, rule, 1, 0)
        expected = {}
        for index, kind in enumerate(("HEADER", "IMAGE")):
            cache = SCRATCH + index * 0x100
            first, second = SCRATCH + 0x500 + index * 0x200, SCRATCH + 0x540 + index * 0x200
            rb1, rb2 = first + 0x80, second + 0x80
            data1, data2 = SCRATCH + 0x1000 + index * 0x200, SCRATCH + 0x1040 + index * 0x200
            offset = 0 if kind == "HEADER" else 4
            m.put(self.macros["RH_FW_" + kind + "_CACHE_SLOT"], cache)
            m.put(cache, self.macros["RH_FW_" + kind + "_CACHE_CLASS"])
            m.put(cache + 44, 4, first)
            m.put(first, rb1, 0, second)
            m.put(second, rb2, first, 0)
            m.put(rb1 + 16, data1)
            m.put(rb2 + 16, data2)
            m.put(data1 + offset, source, 1)
            m.put(data2 + offset, foreign, 1)
            expected[cache] = (data1, first, second)
        calls = []
        def drop(cache, key, user, unused):
            data, first, second = expected[cache]
            self.assertEqual((key, user), (data, 0))
            calls.append((cache, key))
            m.put(cache + 48, second)
            m.put(second + 4, 0)
        m.hooks[self.macros["RH_FW_CACHE_DROP"] & ~1] = drop
        self.assertEqual(m.call(m.symbols["rh_platform_retire_mapped_images"], view), 0)
        self.assertEqual(len(calls), 2)
        for cache, (data, first, second) in expected.items():
            self.assertEqual(m.get(cache + 48), second)
        # A foreign cache layout must fail before executing any native drop.
        m.put(SCRATCH + 44, 0)
        calls.clear()
        self.assertEqual(m.call(m.symbols["rh_platform_retire_mapped_images"], view), -1)
        self.assertEqual(calls, [])

    def cache_machine(self, refs):
        m = self.machine()
        cache, clz, entry, payload = SCRATCH, SCRATCH + 256, SCRATCH + 1024, SCRATCH + 512
        m.put(cache, clz)
        m.put(cache + 24, CALLBACK + 9)
        m.put(clz + 12, CALLBACK + 1)
        m.put(clz + 20, CALLBACK + 5)
        m.put(entry, 0, refs, entry - payload, 0)
        actions = []
        m.hooks[0x0C7416C4] = lambda *args: 0
        m.hooks[0x0C7416D4] = lambda *args: 0
        def lookup(c, key, user, unused):
            self.assertEqual((c, key, user), (cache, payload, 0)); return entry
        def unlink(c, found, user, unused):
            self.assertEqual((c, found, user), (cache, entry, 0)); actions.append("unlink")
        def destroy(found, user, *unused):
            self.assertEqual((found, user), (payload, 0)); actions.append("destroy")
        m.hooks[CALLBACK] = lookup
        m.hooks[CALLBACK + 4] = unlink
        m.hooks[CALLBACK + 8] = destroy
        m.hooks[0x0C73BC94] = lambda *args: actions.append("free")
        return m, cache, entry, payload, actions

    def test_drop_retains_referenced_payload_until_release(self):
        m, cache, entry, payload, actions = self.cache_machine(1)
        m.call(self.macros["RH_FW_CACHE_DROP"], cache, payload, 0)
        self.assertEqual(actions, ["unlink"])
        self.assertEqual(m.get(entry + 4), 1)
        self.assertEqual(bytes(m.uc.mem_read(entry + 12, 1)), b"\x01")
        m.call(self.macros["RH_FW_CACHE_RELEASE"], cache, entry, 0)
        self.assertEqual(actions, ["unlink", "destroy", "free"])
        self.assertEqual(m.get(entry + 4), 0)

    def test_drop_retires_zero_reference_payload_immediately(self):
        m, cache, entry, payload, actions = self.cache_machine(0)
        m.call(self.macros["RH_FW_CACHE_DROP"], cache, payload, 0)
        self.assertEqual(actions, ["unlink", "destroy", "free"])


if __name__ == "__main__":
    unittest.main(verbosity=2)

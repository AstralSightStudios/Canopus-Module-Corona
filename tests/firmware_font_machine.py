"""Minimal exact-AP Unicorn machine for native font probes.

No installer binary, private checkout, emulated filesystem or GPU is required.
Only leaves explicitly hooked by each test are modeled. Firmware aliases and
startup-copied libc code are mapped; this is not a physical cache/MPU model.
"""
import os
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import (
    UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
    UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
    UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_PRIMASK,
)
from firmware_font_compatibility import ROOT, AUDIT, TARGETS, sha, require

TARGET = os.environ.get('RESOURCE_HOOK_TARGET', TARGETS[0])


class Machine:
    def __init__(self):
        require(TARGET in TARGETS, f'unsupported font target: {TARGET}')
        version = TARGET.rsplit('.', 1)[1]
        path = Path(os.environ.get('RESOURCE_HOOK_FIRMWARE',
                    ROOT / f'build/firmware-analysis/vela_ap_4.100.{version}.bin'))
        firmware = path.read_bytes()
        require(sha(firmware) == AUDIT['identity'][f'ap{version}_sha256'],
                f'wrong selected-target AP: {path}')
        self.uc = u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
        for base in (0x0c0c0000, 0x2c0c0000, 0x280c0000):
            u.mem_map(base, (len(firmware) + 4095) & ~4095)
            u.mem_write(base, firmware)
        for base, size in ((0x20000000, 0x160000), (0x3c700000, 0x100000),
                           (0x1c000000, 0x1000), (0x00260000, 0x140000)):
            u.mem_map(base, size)
        layout = AUDIT['identity']['layouts'][version]
        start, end = (int(layout[key], 16) for key in ('data_start', 'data_end'))
        offset = int(layout['data_lma_offset'], 16)
        u.mem_write(start, firmware[offset:offset + end - start])
        # Same exact startup copy descriptors as the existing firmware harness.
        # Their source bytes and descriptor triples are checked in the JSON.
        for copy in AUDIT['startup_code_copies']:
            offset, destination, size = (int(copy[k], 16) for k in ('offset', 'destination', 'size'))
            triples = [struct.unpack_from('<I', firmware, int(p, 16))[0]
                       for p in copy['startup_literal_offsets']]
            require(triples == [0x2c0c0000 + offset, destination, destination + size],
                    'startup copy literal triple differs')
            data = firmware[offset:offset + size]
            require(sha(data) == copy[f'sha256_{version}'], 'startup code copy differs')
            u.mem_write(destination, data)
            u.mem_write(destination - 0x1fe00000, data)
        self.firmware_hooks = {}
        self.stop = 0x1c000100
        self.stack_top = 0x2015df00

    def word(self, address, value=None):
        if value is not None:
            self.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
        return struct.unpack('<I', self.uc.mem_read(address, 4))[0]

    def reg(self, index):
        return self.uc.reg_read((UC_ARM_REG_R0, UC_ARM_REG_R1,
                                 UC_ARM_REG_R2, UC_ARM_REG_R3)[index])

    def firmware_call(self, u, address, size, callback):
        value = callback()
        require(value is not None, 'modeled font leaf must return an explicit result')
        for reg in (UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R12):
            u.reg_write(reg, 0xdeadc0de)
        u.reg_write(UC_ARM_REG_R0, value & 0xffffffff)
        u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))

    def call(self, entry, arg0=0):
        u = self.uc
        mask = u.reg_read(UC_ARM_REG_PRIMASK)
        saved = {r: 0x11220000 + r for r in (
            UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
            UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)}
        for reg, value in saved.items():
            u.reg_write(reg, value)
        u.mem_write(self.stack_top - 4096 - 32, b'\xa5' * 32)
        u.reg_write(UC_ARM_REG_SP, self.stack_top)
        u.reg_write(UC_ARM_REG_LR, self.stop | 1)
        u.reg_write(UC_ARM_REG_R0, arg0)
        u.emu_start(entry | 1, self.stop, count=1000000)
        require(u.reg_read(UC_ARM_REG_PC) == self.stop, 'native call exceeded instruction bound')
        require(u.reg_read(UC_ARM_REG_SP) == self.stack_top, 'native stack imbalance')
        require(u.reg_read(UC_ARM_REG_PRIMASK) == mask, 'native interrupt mask changed')
        for reg, value in saved.items():
            require(u.reg_read(reg) == value, f'callee-saved register changed: {reg}')
        require(bytes(u.mem_read(self.stack_top - 4096 - 32, 32)) == b'\xa5' * 32,
                'native stack guard overwritten')
        return self.reg(0)

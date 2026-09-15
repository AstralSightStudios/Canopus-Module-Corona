"""Execute .139 font ownership paths; allocation/list mutation are modeled.

These tests recover the teardown ordering needed by a UI reload. They do not
run a display, restart miwear, or claim that all UI font owners are enumerated.
"""
import os
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
CANOPUS = Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus')).resolve()
sys.path.insert(0, str(CANOPUS / 'scripts/tests'))
from band11_arm_bootstrap import Machine
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R1


class FontLifecycle(unittest.TestCase):
    def setUp(self):
        self.m = m = Machine()
        m.finish_access_monitor()
        self.manager = 0x3c750000
        self.name = 0x3c751000
        self.key = 0x3c752000
        self.record = 0x3c753000
        self.font = 0x3c754000
        self.wrapper = 0x3c755000
        self.cache = 0x3c756000
        self.node = 0x3c757000
        self.events = []
        for address, size in ((self.manager, 560), (self.record, 56),
                              (self.font, 64), (self.wrapper, 48),
                              (self.cache, 16), (self.node, 52)):
            m.uc.mem_write(address, bytes(size))
        m.uc.mem_write(self.name, b'MiSans-Regular\0')
        m.uc.mem_write(self.key, struct.pack('<IHH', self.name, 24, 0))
        m.word(self.manager, 48)
        m.word(self.manager + 12, 40)
        m.word(self.manager + 556, self.cache)
        m.word(self.cache, 44)
        m.word(self.cache + 12, 8)
        m.uc.mem_write(self.font, struct.pack('<9I', *range(0x100, 0x109)))
        self.hook(0xc3a5e34, lambda: 0)
        self.hook(0xc3abe58, lambda: self.events.append(('free', m.reg(0))) or 0)
        self.hook(0xc3a46dc, lambda: self.events.append(('unlink', m.reg(0), m.reg(1))) or 0)

    def hook(self, address, fn):
        m = self.m
        if address in m.firmware_hooks:
            m.uc.hook_del(m.firmware_hooks[address])
        m.uc.hook_add(UC_HOOK_CODE, m.firmware_call, fn, address, address)

    def forbid_file_resolution(self):
        def unexpected():
            self.fail('font reuse unexpectedly attempted pathname resolution or native access')
        self.hook(0xc490edc, unexpected)
        self.hook(0xc34f8ec, unexpected)
        self.hook(0xc342c54, unexpected)

    def allocate_list_node(self):
        owner = self.m.reg(0)
        self.events.append(('allocate', owner))
        if owner == self.manager:
            return self.record
        if owner == self.manager + 12:
            return self.wrapper
        if owner == self.cache:
            return self.node
        self.fail(f'unexpected list owner {owner:#x}')

    def create(self):
        self.hook(0xc3a43b0, self.allocate_list_node)
        self.m.uc.reg_write(UC_ARM_REG_R1, self.key)
        return self.m.call(0xc494380, self.manager)

    def test_live_font_is_shared_before_any_path_resolution(self):
        m = self.m
        m.word(self.manager + 4, self.record)
        m.word(self.record, self.font)
        m.word(self.record + 4, self.name)
        m.word(self.record + 8, 24)
        m.word(self.record + 44, 1)
        self.forbid_file_resolution()
        self.assertEqual(self.create(), self.wrapper)
        self.assertEqual(m.word(self.record + 44), 2)
        self.assertEqual(m.word(self.wrapper + 36), self.record)
        self.assertEqual(bytes(m.uc.mem_read(self.wrapper, 36)), bytes(m.uc.mem_read(self.font, 36)))
        self.assertEqual(self.events, [('allocate', self.manager + 12)])

    def test_cached_font_is_reused_before_any_path_resolution(self):
        m = self.m
        m.word(self.cache + 4, self.node)
        m.word(self.node, self.name)
        m.word(self.node + 4, 24)
        m.word(self.node + 40, self.font)
        self.forbid_file_resolution()
        self.assertEqual(self.create(), self.wrapper)
        self.assertIn(('unlink', self.cache, self.node), self.events)
        self.assertIn(('free', self.node), self.events)
        self.assertEqual(m.word(self.record), self.font)
        self.assertEqual(m.word(self.record + 44), 1)
        self.assertEqual(m.word(self.wrapper + 36), self.record)

    def test_cache_eviction_releases_freetype_after_last_wrapper(self):
        m = self.m
        descriptor = 0x3c758000
        freetype = 0x3c759000
        entry = 0x3c75a000
        entry_data = 0x3c75b000
        path = 0x3c75c000
        for address, size in ((descriptor, 64), (freetype, 32), (entry, 16)):
            m.uc.mem_write(address, bytes(size))
        m.word(0x200bd3ec, freetype)
        m.word(freetype + 24, 0x3c75d000)
        m.word(self.node + 40, descriptor + 4)
        m.word(descriptor, 1600079444)
        m.word(descriptor + 28, descriptor)
        m.word(descriptor + 48, freetype)
        m.word(descriptor + 52, entry_data)
        m.word(descriptor + 56, entry)
        m.word(descriptor + 60, path)
        m.word(entry + 4, 1)
        def release_entry():
            self.events.append(('release_entry', m.reg(0), m.reg(1)))
            m.word(entry + 4, 0)
            return 0
        self.hook(0xc8b9790, release_entry)
        self.hook(0xc8b8cae, lambda: self.events.append(('drop_entry', m.reg(0), m.reg(1))) or 0)
        self.hook(0xc39a424, lambda: self.events.append(('release_path', m.reg(0), m.reg(1))) or 0)
        m.uc.reg_write(UC_ARM_REG_R1, self.node)
        m.call(0xc4940fc, self.cache)
        self.assertEqual(self.events, [
            ('release_entry', 0x3c75d000, entry),
            ('drop_entry', 0x3c75d000, entry_data),
            ('release_path', freetype, path),
            ('free', descriptor),
            ('unlink', self.cache, self.node),
            ('free', self.node),
        ])

    def test_last_wrapper_release_caches_instead_of_freeing_font(self):
        m = self.m
        m.word(self.manager + 16, self.wrapper)
        m.word(self.wrapper + 36, self.record)
        m.word(self.record, self.font)
        m.word(self.record + 4, self.name)
        m.word(self.record + 8, 24)
        m.word(self.record + 44, 1)
        self.hook(0xc3a4a6a, lambda: 0)
        self.hook(0xc3a43b0, self.allocate_list_node)
        m.uc.reg_write(UC_ARM_REG_R1, self.wrapper)
        m.call(0xc917394, self.manager)
        self.assertEqual(m.word(self.node + 40), self.font)
        self.assertIn(('free', self.record), self.events)
        self.assertIn(('free', self.wrapper), self.events)
        self.assertNotIn(('free', self.font), self.events)
        self.assertIn(('allocate', self.cache), self.events)


if __name__ == '__main__':
    unittest.main(verbosity=2)

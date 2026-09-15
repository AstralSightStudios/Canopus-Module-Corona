"""Execute the real .139 font-path registry the module retargets.

Fonts never reach the hooked LVGL POSIX open: font_manager_create_font_warpper
resolves a family through font_manager_generate_def_path and opens the result with
the native access()/FreeType. Retargeting that registry is the only way a theme can
replace a font. These tests run the firmware's own generate_def_path, add_path and
remove_path against a modeled font manager, and pin down the trap that makes a naive
add_path silently useless. The manager object and the allocator are modeled; the
lookup, append and unlink are the firmware's instructions. No font is rendered and
no on-hardware behavior is claimed.
"""
import os
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
CANOPUS = Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus')).resolve()
sys.path.insert(0, str(CANOPUS / 'scripts/tests'))
from band11_arm_bootstrap import Machine
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1

GENERATE_DEF_PATH = 0xc490edc    # font_manager_generate_def_path(manager, name)
ADD_PATH = 0xc4924e0             # font_manager_add_path(name, path)
REMOVE_PATH = 0xc904cfc          # font_manager_remove_path(entry)
LOG = 0xc3a5e34
UIKIT_GLOBAL = 0x200bd1e8

PAYLOAD, HEAD, TAIL, BASE_DIR = 24, 28, 32, 36
MANAGER, UIKIT, ARENA = 0x3c7d0000, 0x3c7d4000, 0x3c7d8000


class FontRegistry(unittest.TestCase):
    def setUp(self):
        self.m = Machine()
        self.m.finish_access_monitor()
        self.hook(LOG, lambda: 0)
        self.next_free = ARENA
        # Model the firmware allocator the registry uses for nodes and strings.
        self.hook(0xc3abe20, self.alloc)
        self.hook(0xc3ac2bc, self.alloc)
        self.freed = []
        self.hook(0xc3abe58, self.free)
        m = self.m
        m.uc.mem_write(MANAGER, bytes(1024))
        m.uc.mem_write(UIKIT, bytes(64))
        m.word(UIKIT + 28, MANAGER)
        m.word(UIKIT_GLOBAL, UIKIT)
        m.word(MANAGER + PAYLOAD, 8)      # node payload {name, path}
        m.word(MANAGER + HEAD, 0)
        m.word(MANAGER + TAIL, 0)
        m.uc.mem_write(MANAGER + BASE_DIR, b'/resource/font\0')

    def hook(self, address, fn):
        m = self.m
        if address in m.firmware_hooks:
            m.uc.hook_del(m.firmware_hooks.pop(address))
        m.firmware_hooks[address] = m.uc.hook_add(
            UC_HOOK_CODE, m.firmware_call, fn, address, address)

    def alloc(self):
        size = (self.m.uc.reg_read(UC_ARM_REG_R0) + 15) & ~15
        block, self.next_free = self.next_free, self.next_free + max(size, 16)
        self.m.uc.mem_write(block, bytes(max(size, 16)))
        return block

    def free(self):
        self.freed.append(self.m.uc.reg_read(UC_ARM_REG_R0))
        return 0

    def text(self, addr):
        return self.m.string(addr) if addr else None

    def string(self, value):
        block = self.alloc_bytes(value.encode() + b'\0')
        return block

    def alloc_bytes(self, data):
        block, self.next_free = self.next_free, self.next_free + ((len(data) + 15) & ~15)
        self.m.uc.mem_write(block, data)
        return block

    def add(self, name, path):
        m = self.m
        m.uc.reg_write(UC_ARM_REG_R1, self.string(path))
        return m.call(ADD_PATH, self.string(name))

    def resolve(self, name):
        m = self.m
        m.uc.reg_write(UC_ARM_REG_R1, self.string(name))
        return self.text(m.call(GENERATE_DEF_PATH, MANAGER))

    def entries(self):
        m, out, node = self.m, [], self.m.word(MANAGER + HEAD)
        stride = m.word(MANAGER + PAYLOAD) + 4
        while node:
            out.append((node, self.text(m.word(node)), self.text(m.word(node + 4))))
            node = m.word(node + stride)
        return out

    def test_unregistered_family_falls_back_to_the_base_directory(self):
        self.assertEqual(self.resolve('MiSans-Regular'),
                         '/resource/font/MiSans-Regular.ttf')

    def test_registered_family_resolves_to_its_entry(self):
        self.add('MiSans-Regular', '/tmp/MiSans-Regular.ttf')
        self.assertEqual(self.resolve('MiSans-Regular'), '/tmp/MiSans-Regular.ttf')

    def test_add_path_alone_cannot_override_a_registered_family(self):
        """The trap: add_path appends at the tail, lookup takes the first match."""
        self.add('MiSans-Regular', '/tmp/MiSans-Regular.ttf')
        themed = '/data/canopus/themes/current/font/MiSans-Regular.ttf'
        self.assertNotEqual(self.add('MiSans-Regular', themed), 0)  # reports success
        self.assertEqual(len(self.entries()), 2)                    # and did append
        # ...yet the startup entry still wins, so the theme would never be used.
        self.assertEqual(self.resolve('MiSans-Regular'), '/tmp/MiSans-Regular.ttf')

    def test_remove_then_add_retargets_the_family(self):
        """What the module does instead."""
        self.add('MiSans-Regular', '/tmp/MiSans-Regular.ttf')
        self.add('MiSans-Demibold', '/tmp/MiSans-Demibold.ttf')
        node, name, path = self.entries()[0]
        self.assertEqual((name, path), ('MiSans-Regular', '/tmp/MiSans-Regular.ttf'))
        themed = '/data/canopus/themes/current/font/MiSans-Regular.ttf'
        # The name string is freed by the removal, so it must be copied first.
        copied = self.string(name)
        self.m.call(REMOVE_PATH, node)
        self.assertIn(node, self.freed)
        self.assertEqual(self.resolve('MiSans-Regular'),
                         '/resource/font/MiSans-Regular.ttf')  # back to default
        self.m.uc.reg_write(UC_ARM_REG_R1, self.string(themed))
        self.m.call(ADD_PATH, copied)
        self.assertEqual(self.resolve('MiSans-Regular'), themed)
        # The untouched family is unaffected and the registry did not grow.
        self.assertEqual(self.resolve('MiSans-Demibold'), '/tmp/MiSans-Demibold.ttf')
        self.assertEqual(len(self.entries()), 2)

    def test_retargeted_entry_moves_to_the_end_of_the_registry(self):
        self.add('A', '/tmp/A.ttf')
        self.add('B', '/tmp/B.ttf')
        node, name, _ = self.entries()[0]
        copied = self.string(name)
        self.m.call(REMOVE_PATH, node)
        self.m.uc.reg_write(UC_ARM_REG_R1, self.string('/themed/A.ttf'))
        self.m.call(ADD_PATH, copied)
        # Index 0 is now B: the module re-examines the same index after a swap.
        self.assertEqual([e[1] for e in self.entries()], ['B', 'A'])


if __name__ == '__main__':
    unittest.main(verbosity=2)

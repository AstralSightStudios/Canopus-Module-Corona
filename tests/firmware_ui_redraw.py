"""Execute real .139 invalidation and display accessors against modeled objects.

Only display event dispatch is modeled. These tests verify dirty-area bookkeeping,
not physical display output, decoder reopen behavior, or a whole UI lifecycle.
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

LV_INV_AREA = 0x0c382428          # _lv_inv_area(disp, area)
HRES = 0xc380694                  # lv_display_get_horizontal_resolution
VRES = 0xc3806b4                  # lv_display_get_vertical_resolution
SEND_EVENT = 0xc3809a4            # lv_display_send_event; return 1 to continue

W, H = 192, 490                   # modeled screen size; any positive pair works

# Display field offsets recovered from the exact .139 lv_refr.c/lv_display.c.
ACT_SCR = 696                     # verified by real accessor 0xc3807ec
MODE = 57                        # render mode byte (2 == direct/full)
FLAGS = 58                       # bit 1 == rendering_in_progress
INV_AREAS = 60                   # invalidated-area array (16 bytes each)
INV_COUNT = 604                  # invalidated-area count
INV_ENABLE = 608                 # lv_display_is_invalidation_enabled counter (>0)


class InvalidateArea(unittest.TestCase):
    def setUp(self):
        self.m = Machine()
        self.m.finish_access_monitor()
        self.hook(SEND_EVENT, lambda: 1)
        # HRES/VRES are deliberately NOT stubbed: which raw field is the
        # horizontal resolution depends on the rotation bit, and that is exactly
        # what the module has to agree with.

    def hook(self, address, fn):
        m = self.m
        if address in m.firmware_hooks:
            m.uc.hook_del(m.firmware_hooks[address])
        m.uc.hook_add(UC_HOOK_CODE, m.firmware_call, fn, address, address)

    def build_display(self, addr, *, mode, rendering, enabled, count=0, rotated=False):
        m = self.m
        m.uc.mem_write(addr, bytes(1024))
        m.word(addr, W)
        m.word(addr + 4, H)
        m.word(addr + 24, 130)  # nonzero DPI must never count as a screen
        m.word(addr + ACT_SCR, addr + 0x800)   # non-null active screen
        # bytes 56..59 in one word: MODE at +57, FLAGS at +58.
        m.word(addr + 56, (mode & 0xff) << 8 | (rendering & 0xff) << 16)
        m.word(addr + 756, 2 if rotated else 0)
        m.word(addr + INV_COUNT, count)
        m.word(addr + INV_ENABLE, enabled)

    def inv_area(self, disp, area):
        m = self.m
        m.uc.reg_write(UC_ARM_REG_R1, area)     # r0 set by call(); r1 = area
        return m.call(LV_INV_AREA, disp)

    def test_real_accessors_distinguish_dpi_from_screen(self):
        m, disp = self.m, 0x3c780000
        self.build_display(disp, mode=1, rendering=0, enabled=1)
        m.word(0x200bd200, disp)
        self.assertEqual(m.call(0xc3807bc, disp), 130)
        self.assertEqual(m.call(0xc3807ec, disp), disp + 0x800)
        self.assertEqual(m.call(0xc3807ec, 0), disp + 0x800)
        m.word(disp + ACT_SCR, 0)
        self.assertEqual(m.call(0xc3807ec, disp), 0)
        self.assertEqual(m.call(0xc3807bc, disp), 130)

    def test_partial_mode_appends_full_screen_area(self):
        m = self.m
        disp, area = 0x3c780000, 0x3c781000
        self.build_display(disp, mode=1, rendering=0, enabled=1)
        m.uc.mem_write(area, struct.pack('<4i', 0, 0, 0x7fff, 0x7fff))
        self.inv_area(disp, area)
        # The oversized request was clipped to the screen and appended as the one
        # invalidated area, so the next refresh repaints the whole display.
        self.assertEqual(m.word(disp + INV_COUNT), 1)
        self.assertEqual(struct.unpack('<4i', m.uc.mem_read(disp + INV_AREAS, 16)),
                         (0, 0, W - 1, H - 1))

    def test_direct_mode_marks_whole_screen(self):
        m = self.m
        disp, area = 0x3c780000, 0x3c781000
        self.build_display(disp, mode=2, rendering=0, enabled=1)
        m.uc.mem_write(area, struct.pack('<4i', 10, 10, 20, 20))  # even a small area
        self.inv_area(disp, area)
        self.assertEqual(m.word(disp + INV_COUNT), 1)
        self.assertEqual(struct.unpack('<4i', m.uc.mem_read(disp + INV_AREAS, 16)),
                         (0, 0, W - 1, H - 1))

    def test_disabled_invalidation_is_a_safe_noop(self):
        m = self.m
        disp, area = 0x3c780000, 0x3c781000
        self.build_display(disp, mode=1, rendering=0, enabled=0)
        m.uc.mem_write(area, struct.pack('<4i', 0, 0, 0x7fff, 0x7fff))
        self.inv_area(disp, area)
        # With invalidation disabled the firmware appends nothing and does not fault.
        self.assertEqual(m.word(disp + INV_COUNT), 0)

    def test_rotation_swaps_which_raw_field_is_horizontal(self):
        """disp+0/disp+4 are not fixed axes: the rotation bit swaps them, and
        _lv_inv_area clips with the accessors, so a full-screen dirty area is
        {0,0,vres-1,hres-1} in raw-field terms when rotated."""
        m = self.m
        disp, area = 0x3c780000, 0x3c781000
        self.build_display(disp, mode=1, rendering=0, enabled=1, rotated=True)
        self.assertEqual(m.call(0xc380694, disp), H)   # horizontal is the +4 field
        self.assertEqual(m.call(0xc3806b4, disp), W)   # vertical is the +0 field
        m.uc.mem_write(area, struct.pack('<4i', 0, 0, 0x7fff, 0x7fff))
        self.inv_area(disp, area)
        self.assertEqual(m.word(disp + INV_COUNT), 1)
        self.assertEqual(struct.unpack('<4i', m.uc.mem_read(disp + INV_AREAS, 16)),
                         (0, 0, H - 1, W - 1))


if __name__ == '__main__':
    unittest.main(verbosity=2)

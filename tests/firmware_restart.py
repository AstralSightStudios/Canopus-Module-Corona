"""Actual .139 launch/init code; OS scheduling and libuv calls are modeled.

These are counterexample tests, not a claim of an emulated process restart.
"""
import pathlib
import sys
import unittest
import os
ROOT = pathlib.Path(__file__).resolve().parents[1]
CANOPUS = pathlib.Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus')).resolve()
sys.path.insert(0, str(CANOPUS / 'scripts/tests'))
from band11_arm_bootstrap import Machine
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC


class Restart(unittest.TestCase):
    def hook(self, m, address, fn):
        if address in m.firmware_hooks:
            m.uc.hook_del(m.firmware_hooks[address])
        m.uc.hook_add(UC_HOOK_CODE, m.firmware_call, fn, address, address)

    def test_builtin_loader_returns_fixed_miwear_entry_without_resetting_globals(self):
        m = Machine()
        m.finish_access_monitor()
        image, name = 0x3c710000, 0x3c711000
        m.uc.mem_write(image, bytes(184))
        m.uc.mem_write(name, b'miwear\0')
        m.uc.mem_write(0x200da910, b'\x07')
        self.hook(m, 0xc34938c, lambda: 0)  # no slash in the supplied basename
        self.hook(m, 0xc34834c, lambda: 30)  # builtin index checked below
        base = 0x2ca02564 + 30 * 16
        self.assertEqual(m.string(m.word(base)), 'miwear')
        m.uc.reg_write(UC_ARM_REG_R1, name)
        self.assertEqual(m.call(0xc330720, image), 0)
        self.assertEqual(m.word(image), 0xc7def89)
        self.assertEqual(m.word(image + 172), 65536)
        self.assertEqual(bytes(m.uc.mem_read(image + 168, 1)), b'\x66')
        self.assertEqual(bytes(m.uc.mem_read(0x200da910, 1)), b'\x07')

    def test_task_start_dispatches_tcb_entry_in_new_owner_context(self):
        m = Machine()
        m.finish_access_monitor()
        tcb, stack, argv, name = 0x3c740000, 0x3c741000, 0x3c742000, 0x3c743000
        m.uc.mem_write(tcb, bytes(600))
        m.uc.mem_write(stack, bytes(128))
        m.uc.mem_write(name, b'miwear\0')
        m.word(argv, name)
        m.word(argv + 4, 0)
        m.word(tcb + 56, 0xc7def89)
        m.word(tcb + 100, stack)
        m.word(stack + 44, argv)
        m.word(0x200b04bc, tcb)
        m.word(0x200b98e4, 1)  # firmware constructors have already run
        m.uc.mem_write(0x200da910, b'\x07')
        seen = []
        def entry():
            seen.append((m.word(0x200b04bc), m.reg(0), m.reg(1)))
            return 23
        self.hook(m, 0xc7def88, entry)
        def exit_task(uc, address, size, data):
            seen.append(('exit', m.reg(0)))
            uc.emu_stop()
        m.uc.hook_add(UC_HOOK_CODE, exit_task, None, 0xc34ee38, 0xc34ee38)
        m.uc.reg_write(UC_ARM_REG_SP, m.stack_top)
        m.uc.reg_write(UC_ARM_REG_LR, m.stop | 1)
        m.uc.emu_start(0xc35a345, m.stop, count=10000)
        self.assertEqual(m.uc.reg_read(UC_ARM_REG_PC), 0xc34ee38)
        self.assertEqual(seen, [(tcb, 1, argv), ('exit', 23)])
        self.assertEqual(bytes(m.uc.mem_read(0x200da910, 1)), b'\x07')

    def test_empty_group_cleanup_does_not_reset_firmware_globals(self):
        m = Machine()
        m.finish_access_monitor()
        tcb, group, info = 0x3c720000, 0x3c721000, 0x3c722000
        resident = 0x3c723000
        m.uc.mem_write(tcb, bytes(600))
        m.uc.mem_write(group, bytes(400))
        m.uc.mem_write(info, bytes(608))
        m.word(tcb + 8, group)
        m.word(group + 56, info)
        m.uc.mem_write(group + 96, b'\x01\x01')  # one file row / one reference
        m.word(group + 100, group + 104)
        m.word(group + 104, 0x3c724000)
        m.uc.mem_write(0x3c724000, bytes(256))
        m.uc.mem_write(resident, b'resident-image-canary')
        m.uc.mem_write(0x200da910, b'\x07')
        released = []
        def free_group_object():
            released.append(m.reg(1))
            return 0
        self.hook(m, 0xc34d9c0, free_group_object)
        for address in (0xc34da3c, 0xc34da52, 0xc334998):
            self.hook(m, address, lambda: 0)
        # Execute actual group_leave + env_release; stdio/lock/file cleanup
        # above is modeled. No image-release callback is invented.
        m.call(0xc3533f4, tcb)
        self.assertEqual(m.word(tcb + 8), 0)
        self.assertIn(info, released)
        self.assertNotIn(resident, released)
        self.assertEqual(bytes(m.uc.mem_read(resident, len(b'resident-image-canary'))), b'resident-image-canary')
        self.assertEqual(bytes(m.uc.mem_read(0x200da910, 1)), b'\x07')

    def test_uikit_deinit_with_live_font_still_discards_context(self):
        m = Machine()
        m.finish_access_monitor()
        context, manager, font, name = 0x3c750000, 0x3c751000, 0x3c752000, 0x3c753000
        m.uc.mem_write(context, bytes(32))
        m.uc.mem_write(manager, bytes(560))
        m.uc.mem_write(font, bytes(56))
        m.uc.mem_write(name, b'live-font\0')
        m.word(0x200bd1e8, context)
        m.word(context + 28, manager)
        m.word(manager, 48)
        m.word(manager + 4, font)
        m.word(manager + 8, font)
        m.word(manager + 12, 40)
        m.word(font + 4, name)
        m.word(font + 44, 1)
        events = []
        def log():
            events.append(('log', m.string(m.word(m.uc.reg_read(UC_ARM_REG_SP)))))
            return 0
        def release():
            events.append(('free', m.reg(0)))
            return 0
        def lv_deinit():
            events.append(('lv_deinit', m.word(0x200bd1e8)))
            return 0
        self.hook(m, 0xc3a5e34, log)
        self.hook(m, 0xc3abe58, release)
        self.hook(m, 0xc3a9f40, lv_deinit)
        # The actual font-list traversal and vg_deinit run; freeing memory and
        # the subsequent LVGL teardown are recorded, not executed.
        m.call(0xc494198)
        self.assertIn(('log', 'Unfreed resource detected, delete failed!'), events)
        self.assertIn(('free', context), events)
        self.assertNotIn(('free', manager), events)
        self.assertNotIn(('free', font), events)
        self.assertEqual(events[-1], ('lv_deinit', 0))
        self.assertEqual(m.word(font + 44), 1)

    def test_font_setup_has_an_independent_one_time_guard(self):
        for already_initialized in (0, 1):
            with self.subTest(already_initialized=already_initialized):
                m = Machine()
                m.finish_access_monitor()
                m.uc.mem_write(0x200da910, b'\x02')
                m.word(0x200da864, 0)
                m.uc.mem_write(0x200da7f8, bytes([already_initialized]))
                events = []
                self.hook(m, 0xc350474, lambda: 0)
                self.hook(m, 0xc4948a0, lambda: events.append(('manager_init',)) or 0)
                self.hook(m, 0xc4ffc90, lambda: events.append(
                    ('copy', m.string(m.reg(0)), m.string(m.reg(1)))) or 0)
                self.hook(m, 0xc4924e0, lambda: events.append(
                    ('font_path', m.string(m.reg(0)), m.string(m.reg(1)))) or 0)
                m.uc.hook_add(UC_HOOK_CODE, lambda uc, address, size, data: uc.emu_stop(),
                              None, 0xc7e895a, 0xc7e895a)
                m.uc.reg_write(UC_ARM_REG_SP, m.stack_top)
                m.uc.reg_write(UC_ARM_REG_LR, m.stop | 1)
                m.uc.emu_start(0xc7e88b1, m.stop, count=10000)
                self.assertEqual(m.uc.reg_read(UC_ARM_REG_PC), 0xc7e895a)
                if already_initialized:
                    self.assertEqual(events, [])
                else:
                    self.assertEqual(events[0], ('manager_init',))
                    self.assertEqual([event for event in events if event[0] == 'font_path'], [
                        ('font_path', 'MiSans-Regular', '/tmp/MiSans-Regular.ttf'),
                        ('font_path', 'MiSans-Demibold', '/tmp/MiSans-Demibold.ttf'),
                        ('font_path', 'MiSans-Medium', '/tmp/MiSans-Medium.ttf'),
                        ('font_path', 'MiSans-Semibold', '/resource/font/MiSans-Semibold.ttf'),
                    ])
                self.assertEqual(bytes(m.uc.mem_read(0x200da7f8, 1)), b'\x01')

    def test_reentering_main_does_not_restart_completed_init_state(self):
        m = Machine()
        m.finish_access_monitor()
        scheduled = []
        self.hook(m, 0xc3f5064, lambda: 0x3c710000)
        self.hook(m, 0xc3f0934, lambda: 0)
        self.hook(m, 0xc3eab0c, lambda: 0)
        self.hook(m, 0xc350474, lambda: 0)
        def timer():
            scheduled.append(m.reg(1))
            return 0
        self.hook(m, 0xc3f0a6e, timer)
        m.uc.mem_write(0x200da910, b'\x07')
        self.assertEqual(m.call(0xc7def88), 0)
        self.assertEqual(bytes(m.uc.mem_read(0x200da910, 1)), b'\x08')
        self.assertEqual(scheduled, [0xc7e88b1])
        # State 8 reports boot completion, rather than reconstructing graphics.
        completed = []
        self.hook(m, 0xc3f0968, lambda: 0)
        def notify():
            completed.append(m.string(m.reg(0)))
            return 0
        self.hook(m, 0xc6d0fd4, notify)
        self.hook(m, 0xc4fbbc4, lambda: 1)  # suppress optional vibration branch
        self.hook(m, 0xc8ec158, lambda: 0)
        m.call(0xc7e88b0)
        self.assertEqual(completed, ['booting-completed'])
        self.assertEqual(bytes(m.uc.mem_read(0x200da910, 1)), b'\x08')


if __name__ == '__main__':
    unittest.main(verbosity=2)

"""Run signed resource ELF/Supervisor through the selected firmware harness.

The driver-slot reset is synthetic. This does not simulate killing or restarting
miwear, and does not prove startup-before-resource ordering.
"""
import pathlib
import struct
import unittest
import os
from firmware_support import (
    ROOT, TARGET, FIRMWARE_SHA256, Machine, fw, hook, require_identity_addresses,
)

# These literal PCs/globals are deliberately retained only after verifying each
# exact identity. Changed class pointers below must go through fw().
require_identity_addresses(
    0xc33dc4e, 0xc3a6195, 0xc3abd20, 0xc3abe70, 0xc3809a4, 0xc3807ec, 0xc3abe59,
    0x200bd3b8, 0x200bd3bc, 0x200bd3c4, 0x200bd310, 0x200bd314, 0x200bd200,
)
from band11_module_load import registry
from unicorn.arm_const import UC_ARM_REG_R1, UC_ARM_REG_R2

PAYLOAD = pathlib.Path(os.environ.get('RESOURCE_HOOK_PAYLOAD',
    ROOT / 'build/payload-0.3.0' / TARGET))
APP_ROOT = '/data/quickapp/files/ng.lst.corona/'
CONFIG_PATH = APP_ROOT + 'mappings.tsv'
THEME_ROOT = APP_ROOT + 'themes/'


class Rebind(unittest.TestCase):
    retirement_round = 1

    def setUp(self):
        self.load_fixture(enabled=True)

    def load_fixture(self, enabled):
        elf_path, receipt_path = PAYLOAD / 'resource-hook.elf', PAYLOAD / 'receipt.bin'
        if not elf_path.exists() or not receipt_path.exists():
            self.fail(f'build the signed {TARGET} payload in {PAYLOAD} before running firmware_rebind.py')
        elf, receipt = elf_path.read_bytes(), receipt_path.read_bytes()
        self.assertEqual(len(receipt), 256)
        self.assertEqual(receipt[32:64].split(b'\0')[0], b'corona')
        self.assertEqual(receipt[64:112], TARGET.encode().ljust(48, b'\0'),
                         'fixture receipt must match RESOURCE_HOOK_TARGET')
        self.assertEqual(receipt[112:144], bytes.fromhex(FIRMWARE_SHA256[TARGET]),
                         'fixture firmware fingerprint must match selected target')
        lifecycle, version = struct.unpack_from('<2I', receipt, 16)
        self.m = m = Machine()
        record = bytearray(registry('corona', lifecycle, version))
        struct.pack_into('<I', record, 60, int(enabled))
        m.disk['/data/canopus/registry.bin'] = bytes(record)
        m.disk['/data/canopus/inbox/corona.cmi'] = receipt
        m.disk['/data/canopus/inbox/corona.ko'] = elf
        m.disk[CONFIG_PATH] = '/resource/\tthemes/current/\n'.encode()
        m.disk[THEME_ROOT + 'current/a.bin'] = b'mapped file'
        # The bootstrap VFS deliberately accepts only Supervisor-owned paths.
        # Extend it locally for the module's app-scoped files, retaining the
        # same descriptor, partial-read/write and close/commit model.
        def opened():
            path = m.string(m.reg(0))
            if not path.startswith(APP_ROOT):
                return m.open()
            if m.reg(1) & 4:
                data = bytearray()
            elif path in m.disk:
                data = bytearray(m.disk[path])
            else:
                return -1
            fd = m.next_fd
            m.next_fd += 1
            m.files[fd] = [data, 0, path]
            return fd
        hook(m, 0xc342c54, opened)
        self.descriptor = None
        def write():
            ptr, count = m.reg(1), m.reg(2)
            if count == 40 and m.word(ptr) == 0x31524d43:
                self.descriptor = m.word(ptr + 4)
            return m.write()
        hook(m, 0xc33dc4e, write)
        self.assertEqual(m.boot(), 0)
        m.finish_access_monitor()
        m.word(0x200bd3b8, ord('/'))
        m.word(0x200bd3bc, 4096)
        m.word(0x200bd3c4, 0xc3a6195)
        # Scheduling is modeled; distinct handles let stale watcher callbacks
        # self-delete after a rebind without deleting the new 1000 ms watcher.
        # The independent 50 ms refresh callback executes real module code too.
        self.timers = {}
        self.timer_creates = {50: 0, 1000: 0}
        self.timer_deletes = {50: 0, 1000: 0}
        self.next_timer = 0x3c7b0000
        self.fail_timer = False
        self.fail_timer_period = None
        self.bind(0xc3abd20, self.create_timer)
        self.bind(0xc3abe70, self.delete_timer)

    def bind(self, addr, callback):
        hook(self.m, addr, callback)

    def create_timer(self):
        callback, period, cookie = (self.m.reg(i) for i in range(3))
        self.assertIn(period, (50, 1000))
        self.assertEqual(cookie, 0)
        self.assertNotEqual(callback, 0)
        if self.fail_timer or period == self.fail_timer_period:
            return 0
        timer = self.next_timer
        self.next_timer += 32
        self.timers[timer] = (period, callback)
        self.timer_creates[period] += 1
        return timer

    def delete_timer(self):
        timer = self.m.reg(0)
        self.assertIn(timer, self.timers, 'deleting an unknown or already deleted timer')
        period, _ = self.timers.pop(timer)
        self.timer_deletes[period] += 1
        return 0

    def timers_for(self, period):
        return tuple(timer for timer, (ms, _) in self.timers.items() if ms == period)

    def tick(self, period=50, *, timer=None):
        handles = self.timers_for(period)
        self.assertTrue(handles, f'no live {period} ms timer')
        timer = handles[-1] if timer is None else timer
        self.assertIn(timer, handles)
        self.m.call(self.timers[timer][1], timer)

    def graphics(self):
        m, disp = self.m, 0x3c790000
        # lv_init gives the two caches DISTINCT class objects: 0x2ca168c4
        # ("IMAGE") and 0x2ca16944 ("IMAGE_HEADER"). Using one value for both
        # would hide a guard that rejects the real device.
        for cache, glob, clz in ((0x3c781000, 0x200bd310, fw(0x2ca168c4)),
                                 (0x3c782000, 0x200bd314, fw(0x2ca16944))):
            m.uc.mem_write(cache, bytes(64))
            m.word(cache, clz)         # empty cache, real class
            m.word(cache + 48, 4)      # exact-target list payload size
            m.word(glob, cache)
        m.uc.mem_write(disp, bytes(1024))
        m.word(disp, 192)
        m.word(disp + 4, 490)
        m.word(disp + 24, 130)  # DPI, deliberately nonzero
        m.word(disp + 696, disp + 0x800)
        m.word(disp + 56, 1 << 8)
        m.word(disp + 608, 1)
        m.word(0x200bd200, disp)
        self.bind(0xc3809a4, lambda: 1)
        self.assertEqual(m.call(0xc3807ec, disp), disp + 0x800)
        return disp

    def status_words(self):
        m, writer, output = self.m, 0x3c730000, 0x3c731000
        m.uc.mem_write(writer, struct.pack('<6I', output, 40, 0, 0, 1, 1))
        self.assertEqual(m.call(m.word(self.descriptor + 140), writer), 0)
        return struct.unpack('<10I', m.uc.mem_read(output, 40))

    def restore(self):
        return self.command(0x4351000a)

    def command(self, opcode):
        m = self.m
        frame, status = 0x200d0000, 0x200d1000
        m.uc.mem_write(frame, struct.pack('<4I', 0x43504331, opcode, 0, 0))
        m.uc.reg_write(UC_ARM_REG_R1, frame)
        m.uc.reg_write(UC_ARM_REG_R2, 16)
        self.assertEqual(m.call(m.word(m.fops + 12)), 16)
        m.uc.reg_write(UC_ARM_REG_R1, status)
        m.uc.reg_write(UC_ARM_REG_R2, 384)
        self.assertEqual(m.call(m.word(m.fops + 8)), 384)
        self.assertEqual(m.word(status), 0x43505331)
        error = m.word(status + 32)
        if error == (-103 & 0xffffffff):  # CANOPUS_SUP_ERR_STAGE_SIGNATURE
            self.fail('Supervisor rejected the fixture receipt signature (-103); '
                      'provide a payload signed by its independently trusted key. '
                      'Do not replace that key with a bundled payload key.')
        return m.word(status + 24), error

    def test_resident_elf_reinstalls_same_callback_without_reloading_image(self):
        m = self.m
        self.assertEqual(self.restore(), (5, 0))
        hook = m.word(0x200bd3c4)
        self.assertTrue(0x1c000000 <= hook < 0x1d000000, hex(hook))
        resident = {p: n for p, n in m.allocations.items() if p not in m.frees}
        old_watcher, = self.timers_for(1000)
        refresh_timer, = self.timers_for(50)
        # A changed on-disk file must not mutate the locked live mappings.
        m.disk[CONFIG_PATH] = b'bad config'
        m.word(0x200bd3c4, 0xc3a6195)  # model the store performed by lv_init
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(m.word(0x200bd3c4), hook)
        self.assertEqual({p: n for p, n in m.allocations.items() if p not in m.frees}, resident)
        self.assertEqual(self.timers_for(50), (refresh_timer,))
        new_watcher = self.timers_for(1000)[-1]
        self.assertNotEqual(new_watcher, old_watcher)
        self.tick(1000, timer=old_watcher)
        self.assertEqual(self.timers_for(1000), (new_watcher,))
        self.assertEqual(self.timer_deletes[1000], 1)
        path = 0x3c710000
        m.uc.mem_write(path, b'resource/a.bin\0')
        m.uc.reg_write(UC_ARM_REG_R1, path)
        m.uc.reg_write(UC_ARM_REG_R2, 2)
        handle = m.call(hook, 0x200bd3b8)
        self.assertGreater(handle, 0)
        self.assertEqual(m.files[handle - 1][2], THEME_ROOT + 'current/a.bin')
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(m.word(0x200bd3c4), hook)

    def test_enable_persists_intent_without_installing_callback(self):
        self.load_fixture(enabled=False)
        m = self.m
        self.assertIsNone(self.descriptor)
        _, error = self.command(0x43510003)
        self.assertEqual(error, 0)
        self.assertEqual(m.word(0x200bd3c4), 0xc3a6195)
        self.assertIsNone(self.descriptor)
        self.assertEqual(struct.unpack_from('<I', m.disk['/data/canopus/registry.bin'], 60)[0], 1)
        # Restoration does not turn a same-session ENABLE into hot loading.
        self.assertEqual(self.restore(), (5, 0))
        self.assertIsNone(self.descriptor)
        self.assertEqual(m.word(0x200bd3c4), 0xc3a6195)
        # A fresh Supervisor restores the enabled registry as an installable slot.
        self.load_fixture(enabled=True)
        self.assertEqual(self.restore(), (5, 0))
        self.assertIsNotNone(self.descriptor)
        self.assertNotEqual(self.m.word(0x200bd3c4), 0xc3a6195)

    def test_existing_open_handle_is_not_replaced_by_activation(self):
        m = self.m
        m.disk['/data/canopus/original/a.bin'] = b'original file'
        m.disk[CONFIG_PATH] = '/data/canopus/original/\tthemes/current/\n'.encode()
        path = 0x3c710000
        m.uc.mem_write(path, b'data/canopus/original/a.bin\0')
        m.uc.reg_write(UC_ARM_REG_R1, path)
        m.uc.reg_write(UC_ARM_REG_R2, 2)
        old_handle = m.call(0xc3a6195, 0x200bd3b8)
        self.assertGreater(old_handle, 0)
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(m.files[old_handle - 1][2], '/data/canopus/original/a.bin')
        m.uc.reg_write(UC_ARM_REG_R1, path)
        m.uc.reg_write(UC_ARM_REG_R2, 2)
        new_handle = m.call(m.word(0x200bd3c4), 0x200bd3b8)
        self.assertNotEqual(old_handle, new_handle)
        self.assertEqual(m.files[new_handle - 1][2], THEME_ROOT + 'current/a.bin')

    def test_query_publishes_status_and_rejects_short_buffer(self):
        self.assertEqual(self.restore(), (5, 0))
        m = self.m
        self.assertIsNotNone(self.descriptor)
        callback = m.word(self.descriptor + 140)
        writer, output = 0x3c730000, 0x3c731000
        m.uc.mem_write(writer, struct.pack('<6I', output, 40, 0, 0, 1, 1))
        self.assertEqual(m.call(callback, writer), 0)
        # 10 u32: magic, status version 5, installed, rule count, redirected,
        # fallback, image-cache retirements, full-screen redraws, page rebuilds,
        # font retargets (the last four 0 here: this boot fixture has no image
        # cache, no display, no page stack and no font manager).
        self.assertEqual(struct.unpack('<10I', m.uc.mem_read(output, 40)),
                         (0x31514852, 5, 1, 1, 0, 0, 0, 0, 0, 0))
        self.assertEqual(m.word(writer + 8), 40)
        self.assertEqual(m.word(writer + 16), 2)
        self.assertEqual(m.word(writer + 20), 2)
        m.uc.mem_write(output, b'x' * 40)
        m.uc.mem_write(writer, struct.pack('<6I', output, 39, 0, 0, 1, 1))
        self.assertEqual(m.call(callback, writer), 0xffffffff)
        self.assertEqual(bytes(m.uc.mem_read(output, 40)), b'x' * 40)
        self.assertEqual(m.word(writer + 8), 0)

    def test_activate_completes_supported_targeted_retirement(self):
        self.graphics()
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(self.status_words()[6:8], (self.retirement_round, 1))

    def test_full_redraw_is_recognised_on_a_rotated_display(self):
        """Which raw field is the horizontal resolution depends on the rotation
        bit, so a check that read disp+0/disp+4 directly would refuse a perfectly
        good full-screen dirty area here and retry forever."""
        m = self.m
        disp = self.graphics()
        m.word(disp + 756, 2)            # rotated 90/270
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(self.status_words()[6:8], (self.retirement_round, 1))
        self.assertEqual(m.word(disp + 604), 1)
        # Clipped with the accessors, so the axes are swapped in raw-field terms.
        self.assertEqual(struct.unpack('<4i', m.uc.mem_read(disp + 60, 16)),
                         (0, 0, 489, 191))
        self.assertEqual(self.timers_for(50), ())
        self.assertEqual(len(self.timers_for(1000)), 1)

    def test_activate_refuses_an_unexpected_cache_class(self):
        """The retirement traversal is only valid for the recovered LRU/RB
        classes, so an unfamiliar class must stop the whole refresh rather than
        walk an unknown layout."""
        m = self.m
        disp = self.graphics()
        m.word(0x3c782000, fw(0x2ca168c4))   # header cache with the decoded class
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(self.status_words()[6:8], (0, 0))
        self.assertEqual(m.word(disp + 604), 0)   # nothing was invalidated either
        m.word(0x3c782000, fw(0x2ca16944))   # restore the class lv_init really uses
        self.tick()
        self.assertEqual(self.status_words()[6:8], (self.retirement_round, 1))

    def test_activate_requests_full_redraw(self):
        m = self.m
        disp = self.graphics()
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(self.status_words()[7], 1)
        self.assertEqual(m.word(disp + 604), 1)
        self.assertEqual(struct.unpack('<4i', m.uc.mem_read(disp + 60, 16)), (0, 0, 191, 489))
        self.assertEqual(self.timer_creates, {50: 0, 1000: 1})

    def test_missing_screen_not_confused_with_nonzero_dpi(self):
        disp = self.graphics()
        self.m.word(disp + 696, 0)
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(self.status_words()[6:8], (0, 0))
        self.tick()
        self.assertEqual(self.status_words()[6:8], (0, 0))
        self.m.word(disp + 696, disp + 0x800)
        self.tick()
        self.assertEqual(self.status_words()[6:8], (self.retirement_round, 1))
        self.assertEqual(self.timers_for(50), ())
        self.assertEqual(self.timer_deletes, {50: 1, 1000: 0})
        self.assertEqual(len(self.timers_for(1000)), 1)

    def test_busy_render_and_disabled_invalidation_retry_once(self):
        m, disp = self.m, self.graphics()
        m.word(disp + 56, (1 << 8) | (2 << 16))
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(self.timer_creates, {50: 1, 1000: 2})
        self.tick()
        self.assertEqual(self.status_words()[6:8], (0, 0))
        m.word(disp + 56, 1 << 8)
        m.word(disp + 608, 0)
        self.tick()
        self.assertEqual(self.status_words()[6:8], (0, 0))
        m.word(disp + 608, 1)
        self.tick()
        self.assertEqual(self.status_words()[6:8], (self.retirement_round, 1))
        self.assertEqual(self.timers_for(50), ())

    def test_rejected_dirty_area_is_not_counted_or_repeatedly_dropped(self):
        self.graphics()
        self.bind(0xc3809a4, lambda: 0)  # firmware event rejects invalidation
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(self.status_words()[6:8], (self.retirement_round, 0))
        self.tick()
        self.assertEqual(self.status_words()[6:8], (self.retirement_round, 0))
        self.bind(0xc3809a4, lambda: 1)
        self.tick()
        self.assertEqual(self.status_words()[6:8], (self.retirement_round, 1))
        self.assertEqual(self.timers_for(50), ())
        self.assertEqual(len(self.timers_for(1000)), 1)

    def test_timer_allocation_failure_is_reported_with_hook_resident(self):
        self.fail_timer = True
        state, error = self.restore()
        self.assertEqual(state, 6)
        self.assertNotEqual(error, 0)
        self.assertNotEqual(self.m.word(0x200bd3c4), 0xc3a6195)

    def test_watch_timer_failure_does_not_undo_completed_refresh(self):
        self.graphics()
        self.fail_timer_period = 1000
        state, error = self.restore()
        self.assertEqual(state, 6)
        self.assertNotEqual(error, 0)
        self.assertNotEqual(self.m.word(0x200bd3c4), 0xc3a6195)
        self.assertEqual(self.status_words()[6:8], (self.retirement_round, 1))
        self.assertEqual(self.timer_creates, {50: 0, 1000: 0})
        self.fail_timer_period = None
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(self.timer_creates, {50: 0, 1000: 1})

    def test_held_cache_entry_retired_then_freed_on_last_release(self):
        m = self.m
        self.graphics()
        # Target head -> RB node -> data at RB+16. Only affected file keys retire.
        # Generic native drop/release execute; class lookup/unlink are modeled.
        cache, node, data, pool = 0x3c781000, 0x3c7a0000, 0x3c7a0100, 0x3c7a0200
        offset = 0x40
        entry = pool + offset
        for addr in (node, data, pool):
            m.uc.mem_write(addr, bytes(256))
        m.word(cache + 52, node)
        m.word(cache + 24, 0xc3abe59)
        rb = 0x3c7a0300
        m.uc.mem_write(rb, bytes(32))
        m.word(node, rb)
        m.word(rb + 16, data)
        m.word(cache + 4, entry - data)
        m.word(entry + 4, 2)            # two outstanding references
        m.word(entry + 8, entry - data)  # payload sits this far back
        m.word(data + 4, 0x3c7a0400)
        m.uc.mem_write(data + 8, b'\x01')
        m.uc.mem_write(0x3c7a0400, b'/resource/held.bin\0')
        m.uc.mem_write(data + 64, b'held-image-canary')
        freed, looked_up = [], []
        def find_entry():
            self.assertEqual(m.reg(0), cache)
            self.assertEqual(m.reg(1), data)   # the key is the data pointer
            looked_up.append(data)
            return entry if m.word(cache + 52) else 0
        def unlink_entry():
            self.assertEqual(m.reg(0), cache)
            self.assertEqual(m.reg(1), entry)
            m.word(cache + 52, 0)
            return 0
        # Model only class lookup/unlink and frees. The signed module traversal,
        # generic lv_cache_drop and final-release instructions execute for real.
        self.bind(0xc3a4ace, find_entry)
        self.bind(0xc3a472c, unlink_entry)
        self.bind(0xc3abe58, lambda: freed.append(m.reg(0)) or 0)
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(looked_up, [data])
        self.assertEqual(m.word(cache + 52), 0)
        self.assertEqual(m.uc.mem_read(entry + 12, 1), b'\x01')
        self.assertEqual(m.word(entry + 4), 2)
        self.assertEqual(freed, [])
        self.assertEqual(m.uc.mem_read(data + 64, 17), b'held-image-canary')
        m.uc.reg_write(UC_ARM_REG_R1, entry)
        m.call(fw(0xc8b9790), cache)
        self.assertEqual(m.word(entry + 4), 1)
        self.assertEqual(freed, [])
        m.uc.reg_write(UC_ARM_REG_R1, entry)
        m.call(fw(0xc8b9790), cache)
        self.assertEqual(m.word(entry + 4), 0)
        self.assertEqual(freed, [data, data])  # payload callback, allocation free
        self.assertEqual(self.status_words()[6:8], (self.retirement_round, 1))

    def test_missing_config_keeps_empty_hook_and_watcher_resident(self):
        m = self.m
        del m.disk[CONFIG_PATH]
        # Missing configuration is an empty snapshot, not an activation failure.
        errno_cell = 0x3c732000
        m.word(errno_cell, 2)
        self.bind(0xc349538, lambda: errno_cell)
        self.assertEqual(self.restore(), (5, 0))
        resident_hook = m.word(0x200bd3c4)
        self.assertNotEqual(resident_hook, 0xc3a6195)
        self.assertEqual(self.status_words()[2:], (1, 0, 0, 0, 0, 0, 0, 0))
        self.assertEqual(self.timer_creates, {50: 0, 1000: 1})
        # Creating config alone does not mutate the locked snapshot on rebind.
        # A control-file revision makes the already resident hook adopt it.
        m.disk[CONFIG_PATH] = '/resource/\tthemes/current/\n'.encode()
        self.assertEqual(self.restore(), (5, 0))
        self.assertEqual(m.word(0x200bd3c4), resident_hook)
        self.assertEqual(self.status_words()[3], 0)
        m.disk[APP_ROOT + 'reload.request'] = b'resource-hook-reload-v1\tng.lst.corona\tmissing-1\n'
        m.disk[APP_ROOT + 'reload.result'] = bytes(256)
        self.graphics()
        self.tick(1000)
        self.assertEqual(m.word(0x200bd3c4), resident_hook)
        self.assertEqual(self.status_words()[3], 1)
        self.assertEqual(self.status_words()[6:8], (self.retirement_round, 1))

    def test_config_open_io_failure_is_not_noop(self):
        del self.m.disk[CONFIG_PATH]
        errno_cell = 0x3c732000
        self.m.word(errno_cell, 5)
        self.bind(0xc349538, lambda: errno_cell)
        state, error = self.restore()
        self.assertEqual(state, 6)
        self.assertNotEqual(error, 0)
        self.assertEqual(self.m.word(0x200bd3c4), 0xc3a6195)
        self.assertEqual(self.timer_creates, {50: 0, 1000: 0})

    def test_unknown_slot_is_not_overwritten(self):
        self.assertEqual(self.restore(), (5, 0))
        unknown = 0x1c7e0001  # modeled foreign callback, not a firmware PC
        self.m.word(0x200bd3c4, unknown)
        state, error = self.restore()
        self.assertEqual(state, 6)
        self.assertNotEqual(error, 0)
        self.assertEqual(self.m.word(0x200bd3c4), unknown)


if __name__ == '__main__':
    unittest.main(verbosity=2)

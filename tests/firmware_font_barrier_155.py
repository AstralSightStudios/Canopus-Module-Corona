"""Q66 .155 native pending-glyph drain and release-routing probes.

GPU completion and cache release are modeled leaves. This does not prove GPU
idle, thread serialization, or that all firmware font consumers are covered.
Run with RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.155.
"""
import unittest

from firmware_support import Machine, TARGET, PORT_TARGET
from unicorn import UC_HOOK_CODE, UC_ERR_WRITE_UNMAPPED, UcError
from unicorn.arm_const import UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3

if TARGET != PORT_TARGET:
    raise RuntimeError('This investigation supports only the fingerprinted Q66 .155 AP')

# Exact .155 PCs, inspected directly; not source-target relocation identities.
FINISH = 0x0c399940
GPU_FINISH = 0x0c92019c
RELEASE = 0x0c399b4c
PENDING_RELEASE = 0x0c399b62
OUTLINE_RELEASE = 0x0c3a0992
CACHE_RELEASE = 0x0c8b9780


class FontBarrier(unittest.TestCase):
    def setUp(self):
        self.m = Machine()
        self.m.finish_access_monitor()
        self.events = []
        self.unit = 0x3c750000
        self.grad = 0x3c751000
        self.grad_pending = 0x3c752000
        self.image_pending = 0x3c753000
        self.glyph_pending = 0x3c754000
        self.font = 0x3c755000
        self.old_dsc = 0x3c756000
        self.new_dsc = 0x3c757000
        self.old_face = 0x3c758000
        self.new_face = 0x3c759000
        self.old_cache = 0x3c75a000
        self.new_cache = 0x3c75b000
        self.glyphs = (0x3c75c000, 0x3c75d000)
        self.entries = (0x3c75e000, 0x3c75f000)
        self.m.uc.mem_write(self.unit, bytes(0x10000))
        w = self.m.word
        w(self.unit + 40, self.grad)
        w(self.grad + 8, self.grad_pending)
        w(self.unit + 36, self.image_pending)
        w(self.unit + 48, self.glyph_pending)
        w(self.unit + 52, 7)
        w(self.font + 8, OUTLINE_RELEASE | 1)
        w(self.font + 24, self.old_dsc)
        w(self.old_dsc + 52, self.old_face)
        w(self.new_dsc + 52, self.new_face)
        w(self.old_face + 20, self.old_cache)
        w(self.new_face + 20, self.new_cache)
        w(self.glyph_pending + 36, PENDING_RELEASE | 1)
        for array_offset, glyph, entry in zip((4, 20), self.glyphs, self.entries):
            array = self.glyph_pending + array_offset
            w(array, glyph)
            w(array + 4, 1)
            w(array + 8, 1)
            w(array + 12, 24)
            w(glyph, self.font)
            w(glyph + 20, entry)
        self.exact_hook(CACHE_RELEASE, lambda: self.events.append(
            ('release', self.m.reg(0), self.m.reg(1))) or 0)
        self.exact_hook(0x0c3a5e34, lambda: self.events.append(('log',)) or 0)
        self.exact_hook(0x0c397fb4, lambda: self.events.append(
            ('error_dump', self.m.reg(0))) or 0)

    def exact_hook(self, address, callback):
        # Deliberately .155-only: Machine validates the full firmware hash.
        # Never register these as unverified .139 -> .155 identities.
        for key in (address,):
            if key in self.m.firmware_hooks:
                self.m.uc.hook_del(self.m.firmware_hooks.pop(key))
        self.m.firmware_hooks[address] = self.m.uc.hook_add(
            UC_HOOK_CODE, self.m.firmware_call, callback, address, address)

    def finish(self, result=0):
        self.exact_hook(GPU_FINISH, lambda: self.events.append(
            ('gpu_finish', result)) or result)
        self.m.call(FINISH, self.unit)

    def test_release_uses_original_cache_without_swap(self):
        self.m.call(RELEASE, self.glyphs[0])
        self.assertEqual(self.events, [('release', self.old_cache, self.entries[0])])
        self.assertEqual(self.m.word(self.glyphs[0] + 20), 0)

    def test_early_swap_routes_old_entry_to_new_cache(self):
        self.m.word(self.font + 24, self.new_dsc)
        self.m.call(RELEASE, self.glyphs[0])
        self.assertEqual(self.events, [('release', self.new_cache, self.entries[0])])

    def test_finish_drains_both_glyph_buffers_before_swap(self):
        self.finish()
        self.assertEqual(self.events, [
            ('gpu_finish', 0),
            ('release', self.old_cache, self.entries[0]),
            ('release', self.old_cache, self.entries[1]),
        ])
        self.assertEqual(self.m.word(self.glyph_pending + 8), 0)
        self.assertEqual(self.m.word(self.glyph_pending + 24), 0)
        self.assertEqual(self.m.word(self.unit + 52), 0)
        self.m.word(self.font + 24, self.new_dsc)
        for glyph in self.glyphs:
            self.assertEqual(self.m.word(glyph + 20), 0)
            self.m.call(RELEASE, glyph)
        self.assertEqual(len(self.events), 3)

    def test_saved_old_backing_font_routes_pending_release_after_wrapper_swap(self):
        # Production creation copies 36 bytes from record->font into each
        # wrapper. Preserve that original backing font rather than freeing it.
        backing_font = self.old_dsc + 4
        self.m.uc.mem_write(backing_font, bytes(self.m.uc.mem_read(self.font, 36)))
        for glyph in self.glyphs:
            self.m.word(glyph, backing_font)
        self.m.word(self.font + 24, self.new_dsc)
        # New work follows the updated wrapper while old pending work keeps
        # the original backing font. No early pending-reference release.
        self.assertEqual(self.events, [])
        new_glyph, new_entry = self.glyphs[1] + 128, self.entries[1] + 128
        self.m.word(new_glyph, self.font)
        self.m.word(new_glyph + 20, new_entry)
        self.m.call(RELEASE, new_glyph)
        self.finish()
        self.assertEqual(self.events, [
            ('release', self.new_cache, new_entry),
            ('gpu_finish', 0),
            ('release', self.old_cache, self.entries[0]),
            ('release', self.old_cache, self.entries[1]),
        ])

    def test_active_dispatch_does_not_finish_or_drain(self):
        self.m.word(self.unit + 32, 0x3c75e000)
        self.assertEqual(self.m.call(0x0c3913ec, self.unit), 0)
        self.assertEqual(self.events, [])
        self.assertEqual(self.m.word(self.glyph_pending + 8), 1)
        self.assertEqual(self.m.word(self.glyph_pending + 24), 1)

    def test_wait_failure_can_be_hidden_by_successful_reset(self):
        def driver():
            operation = self.m.reg(0)
            self.events.append(('driver', operation))
            return 4 if operation == 5 else 0
        self.exact_hook(0x0c926874, driver)
        self.exact_hook(0x0c720e98, lambda: self.m.reg(0))
        result = self.m.call(0x0c91f624, self.unit)
        self.assertEqual(result, 0)
        self.assertEqual(self.events, [('driver', 5), ('driver', 6)])

    def test_timeout_resets_hardware_before_upper_driver_recovery(self):
        def clock():
            self.m.uc.mem_write(self.m.reg(1), bytes(16))
            return 0
        self.exact_hook(0x0c34d58c, clock)
        self.exact_hook(0x0c936494, lambda: 0)
        self.exact_hook(0x0c349628, lambda: self.events.append(('timed_wait',)) or -1)
        self.exact_hook(0x0c926d00, lambda: self.events.append(('clock_setup',)) or 0)
        self.exact_hook(0x0c921e54, lambda: self.events.append(
            ('hardware_reset', self.m.reg(0))) or 0)
        result = self.m.call(0x0c926d48, 0)
        self.assertEqual(result & 0xffffffff, 0xffffffff)
        self.assertEqual(self.events, [
            ('timed_wait',), ('clock_setup',), ('hardware_reset', 0),
        ])

    def test_vector_cache_key_compares_object_not_font_or_contents(self):
        left, right = self.old_dsc, self.new_dsc
        for offset, value in enumerate((1, self.unit, 0x11111111, 0x22222222)):
            self.m.word(left + offset * 4, value)
        for offset, value in enumerate((2, self.unit, 0x33333333, 0x44444444)):
            self.m.word(right + offset * 4, value)
        self.m.uc.reg_write(UC_ARM_REG_R1, right)
        self.assertEqual(self.m.call(0x0c69feb0, left), 0)
        self.m.word(right + 4, self.unit + 4)
        self.m.uc.reg_write(UC_ARM_REG_R1, right)
        self.assertNotEqual(self.m.call(0x0c69feb0, left), 0)

    def test_vector_drop_targets_one_object(self):
        self.m.word(0x200d3280, self.old_cache)
        def clear():
            self.m.uc.mem_write(self.m.reg(0), bytes(self.m.reg(2)))
            return self.m.reg(0)
        def drop():
            key = self.m.reg(1)
            self.events.append(('drop', self.m.reg(0),
                                tuple(self.m.word(key + i * 4) for i in range(4))))
            return 0
        self.exact_hook(0x0c720e98, clear)
        self.exact_hook(0x0c8b8c9e, drop)
        self.m.call(0x0c6a1304, self.unit)
        self.assertEqual(self.events, [('drop', self.old_cache, (0, self.unit, 0, 0))])

    def test_font_style_refresh_notifies_layout_and_inherited_children(self):
        self.m.uc.mem_write(0x200bd210, b'\x01')
        self.m.word(self.unit + 4, self.grad)
        for address, label in ((0x0c38400c, 'invalidate'),
                               (0x0c3809e2, 'layout'),
                               (0x0c384668, 'children')):
            self.exact_hook(address, lambda label=label: self.events.append(
                (label, self.m.reg(0))) or 0)
        self.exact_hook(0x0c37fbc8, lambda: self.events.append(
            ('event', self.m.reg(0), self.m.reg(1))) or 0)
        self.m.uc.reg_write(UC_ARM_REG_R1, 0)
        self.m.uc.reg_write(UC_ARM_REG_R2, 90)
        self.m.call(0x0c38525c, self.unit)
        self.assertEqual(self.events, [
            ('invalidate', self.unit), ('event', self.unit, 45),
            ('layout', self.unit), ('layout', self.grad),
            ('invalidate', self.unit), ('children', self.unit),
        ])

    def test_destroying_cache_does_not_preserve_held_entry_storage(self):
        cache, rb, node, data = self.old_cache, self.glyphs[0], self.glyphs[1], self.entries[0]
        for address in (cache, rb, node, data):
            self.m.uc.mem_write(address, bytes(64))
        self.m.word(cache, 0x2ca16934)
        self.m.word(cache + 4, 8)
        self.m.word(cache + 12, 1)
        self.m.word(cache + 24, 0x0c397fb5)
        self.m.word(cache + 36, rb)
        self.m.word(cache + 48, 4)
        self.m.word(cache + 52, node)
        self.m.word(rb + 16, data)
        self.m.word(node, rb)
        self.m.word(data + 12, 1)  # entry at data+8, refcount at entry+4
        self.m.word(data + 16, 8)
        self.exact_hook(0x0c3abe58, lambda: self.events.append(
            ('free', self.m.reg(0))) or 0)
        self.exact_hook(0x0c378b1e, lambda: self.events.append(
            ('clear_list', self.m.reg(0))) or 0)
        self.m.call(0x0c3a384c, cache)
        self.assertEqual(self.events, [
            ('log',), ('log',), ('free', data), ('free', rb),
            ('clear_list', cache + 48), ('free', cache),
        ])

    def command_fixture(self):
        context, commands = self.unit, self.glyphs[1]
        self.m.word(context + 1772, commands)
        self.m.word(context + 1780, 256)
        self.m.word(context + 1784, 0)
        self.m.word(context + 1792, 0)
        self.exact_hook(0x0c91ec34, lambda: 1)
        def copy():
            destination, source, size = (self.m.reg(i) for i in range(3))
            self.m.uc.mem_write(destination, bytes(self.m.uc.mem_read(source, size)))
            return destination
        self.exact_hook(0x0c721210, copy)
        return context, commands

    def test_inline_vector_command_copies_path_bytes(self):
        context, commands = self.command_fixture()
        source, payload = self.glyphs[0], b'old-path-data'
        self.m.uc.mem_write(source, payload)
        self.m.uc.reg_write(UC_ARM_REG_R1, len(payload))
        self.m.uc.reg_write(UC_ARM_REG_R2, source)
        self.assertEqual(self.m.call(0x0c91f950, context), 0)
        self.assertEqual(self.m.word(commands), 0x40000002)
        self.assertEqual(self.m.word(context + 1784), 24)
        self.m.uc.mem_write(source, b'X' * len(payload))
        self.assertEqual(bytes(self.m.uc.mem_read(commands + 8, len(payload))), payload)

    def test_vector_path_append_copies_outline_without_upload_flag(self):
        destination, source = self.unit, self.old_dsc
        source_bytes, destination_bytes = self.glyphs
        payload = b'outline-points'
        self.m.uc.mem_write(destination, bytes(132))
        self.m.uc.mem_write(source, bytes(132))
        self.m.word(destination + 44, destination_bytes)
        self.m.word(destination + 124, 256)
        self.m.word(source + 36, 1)  # uploaded original glyph path
        self.m.word(source + 40, len(payload))
        self.m.word(source + 44, source_bytes)
        self.m.uc.mem_write(source_bytes, payload)
        def copy():
            dst, src, size = (self.m.reg(i) for i in range(3))
            self.m.uc.mem_write(dst, bytes(self.m.uc.mem_read(src, size)))
            return dst
        self.exact_hook(0x0c908e1c, copy)
        self.m.uc.reg_write(UC_ARM_REG_R1, source)
        self.m.call(0x0c397e48, destination)
        self.assertEqual(self.m.word(destination + 36), 0)
        self.assertEqual(self.m.word(destination + 40), len(payload))
        self.m.uc.mem_write(source_bytes, b'X' * len(payload))
        self.assertEqual(bytes(self.m.uc.mem_read(destination_bytes, len(payload))), payload)

    def test_uploaded_path_command_retains_address_instead_of_copying(self):
        context, commands = self.command_fixture()
        source = self.glyphs[0]
        self.exact_hook(0x0c91f818, lambda: 0)
        self.m.uc.reg_write(UC_ARM_REG_R1, source)
        self.m.uc.reg_write(UC_ARM_REG_R2, 13)
        self.assertEqual(self.m.call(0x0c91f8c4, context), 0)
        self.assertEqual(self.m.word(commands), 0x60000002)
        self.assertEqual(self.m.word(commands + 4), source)
        self.assertEqual(self.m.word(context + 1784), 8)

    def test_one_pending_buffer_drain_does_not_release_face_ownership(self):
        self.m.uc.hook_del(self.m.firmware_hooks.pop(CACHE_RELEASE))
        face_entry = self.entries[1] + 128
        self.m.word(self.old_dsc + 56, face_entry)
        self.m.word(face_entry + 4, 1)
        for entry in self.entries:
            self.m.word(entry + 4, 1)
        self.m.word(self.glyph_pending, self.glyph_pending + 20)
        self.m.call(0x0c39596c, self.glyph_pending)
        self.assertEqual(self.m.word(self.entries[0] + 4), 0)
        self.assertEqual(self.m.word(self.entries[1] + 4), 1)
        self.assertEqual(self.m.word(face_entry + 4), 1)
        self.assertEqual(self.m.word(self.glyph_pending + 24), 1)
        self.m.call(0x0c39596c, self.glyph_pending)
        self.assertEqual(self.m.word(self.entries[1] + 4), 0)
        self.assertEqual(self.m.word(face_entry + 4), 1)

    def test_wrapper_allocation_failure_is_not_transactional(self):
        manager, record, name, key = self.unit, self.old_dsc, self.old_face, self.new_face
        self.m.uc.mem_write(manager, bytes(560))
        self.m.uc.mem_write(record, bytes(56))
        self.m.uc.mem_write(name, b'MiSans-Regular\x00')
        self.m.word(manager, 48)
        self.m.word(manager + 4, record)
        self.m.word(manager + 12, 40)
        self.m.word(record, self.font)
        self.m.word(record + 4, name)
        self.m.word(record + 8, 24)
        self.m.word(record + 44, 1)
        self.m.word(key, name)
        self.m.word(key + 4, 24)
        self.exact_hook(0x0c3abe20, lambda: 0)
        # Let the caller proceed past its unguarded memset(NULL, 40) so the
        # native wrapper-copy store, rather than a modeled leaf, faults.
        self.exact_hook(0x0c38002c, lambda: 0)
        self.m.uc.reg_write(UC_ARM_REG_R1, key)
        with self.assertRaises(UcError) as caught:
            self.m.call(0x0c494380, manager)
        self.assertEqual(caught.exception.errno, UC_ERR_WRITE_UNMAPPED)
        self.assertEqual(self.m.word(record + 44), 2)

    def test_final_normal_dispatch_drains_pending_without_reload_forcing_finish(self):
        layer, task = 0x3c760000, 0x3c761000
        self.m.uc.mem_write(layer, bytes(128))
        self.m.uc.mem_write(task, bytes(128))
        self.m.word(0x200bd318, self.unit)
        self.m.word(self.unit + 16, 0x0c3913ed)
        self.m.word(layer + 100, task)
        self.m.word(task + 108, 3)
        self.exact_hook(0x0c8b9360, lambda: 0)
        self.exact_hook(GPU_FINISH, lambda: self.events.append(('gpu_finish', 0)) or 0)
        self.m.uc.reg_write(UC_ARM_REG_R1, layer)
        self.m.call(0x0c381484, 0)
        self.assertEqual(self.m.word(layer + 100), 0)
        self.assertEqual(self.m.word(self.glyph_pending + 8), 0)
        self.assertEqual(self.m.word(self.glyph_pending + 24), 0)
        self.assertEqual(self.events, [
            ('gpu_finish', 0),
            ('release', self.old_cache, self.entries[0]),
            ('release', self.old_cache, self.entries[1]),
        ])

    def test_native_cache_constructor_writes_through_failed_allocation(self):
        self.exact_hook(0x0c351724, lambda: self.events.append(
            ('alloc_failed', self.m.reg(0))) or 0)
        self.m.uc.reg_write(UC_ARM_REG_R1, 32)
        self.m.uc.reg_write(UC_ARM_REG_R2, 512)
        self.m.uc.reg_write(UC_ARM_REG_R3, 0x0c39fd9b)
        with self.assertRaises(UcError) as fault:
            self.m.call(0x0c3a7a30, 0x2ca16934)
        self.assertEqual(fault.exception.errno, UC_ERR_WRITE_UNMAPPED)
        self.assertEqual(self.events, [('alloc_failed', 64), ('log',)])

    def test_preallocated_cache_storage_can_use_native_initializer_without_allocation(self):
        cache = self.old_cache
        self.m.uc.mem_write(cache, bytes(64))
        self.m.word(cache, 0x2ca16934)
        self.m.word(cache + 4, 32)
        self.m.word(cache + 8, 512)
        self.m.word(cache + 16, 0x0c39fd9b)
        self.m.word(cache + 20, 0x0c3a5ef1)
        self.m.word(cache + 24, 0x0c39fd95)
        def clear():
            self.m.uc.mem_write(self.m.reg(0), bytes(self.m.reg(1)))
            return self.m.reg(0)
        def unexpected_alloc():
            self.fail('native cache initializer unexpectedly allocated')
        self.exact_hook(0x0c38002c, clear)
        self.exact_hook(0x0c351724, unexpected_alloc)
        self.exact_hook(0x0c3abe20, unexpected_alloc)
        self.assertEqual(self.m.call(0x0c3a9e48, cache), 1)
        self.assertEqual(self.m.word(cache + 36), 0)
        self.assertEqual(self.m.word(cache + 40), 0x0c39fd9b)
        self.assertEqual(self.m.word(cache + 44), 52)
        self.assertEqual(self.m.word(cache + 48), 4)
        self.assertEqual(self.m.word(cache + 52), 0)
        self.assertEqual(self.m.word(cache + 56), 0)
        self.assertEqual(self.m.word(cache + 60), 0x0c3a06a7)

    def test_face_key_uses_path_style_mode_not_font_size_or_path_pointer(self):
        left, right = self.old_dsc, self.new_dsc
        first_path, second_path = self.old_face, self.new_face
        self.m.uc.mem_write(first_path, b'/font/g1.ttf\x00')
        self.m.uc.mem_write(second_path, b'/font/g1.ttf\x00')
        for key, path, size in ((left, first_path, 24), (right, second_path, 48)):
            self.m.uc.mem_write(key, bytes(28))
            self.m.word(key, path)
            self.m.word(key + 4, 1 << 16)
            self.m.word(key + 8, size)
        self.m.uc.reg_write(UC_ARM_REG_R1, right)
        self.assertEqual(self.m.call(0x0c396784, left), 0)
        self.m.uc.mem_write(second_path, b'/font/g2.ttf\x00')
        self.m.uc.reg_write(UC_ARM_REG_R1, right)
        self.assertNotEqual(self.m.call(0x0c396784, left), 0)
        self.m.uc.mem_write(second_path, b'/font/g1.ttf\x00')
        self.m.word(right + 4, (1 << 16) | 1)
        self.m.uc.reg_write(UC_ARM_REG_R1, right)
        self.assertNotEqual(self.m.call(0x0c396784, left), 0)

    def test_native_font_destroy_traverses_per_wrapper_fallback_chain(self):
        uikit, manager = self.unit, self.grad
        first, second = self.old_dsc, self.new_dsc
        self.m.word(0x200bd1e8, uikit)
        self.m.word(uikit + 28, manager)
        self.m.word(self.font + 28, first)
        self.m.word(first + 28, second)
        self.m.word(second + 28, 0)
        self.exact_hook(0x0c917384, lambda: self.events.append(
            ('delete', self.m.reg(0), self.m.reg(1))) or 0)
        self.m.call(0x0c49258c, self.font)
        self.assertEqual(self.events, [
            ('delete', manager, first), ('delete', manager, second),
            ('delete', manager, self.font),
        ])
        self.assertEqual(self.m.word(self.font + 28), 0)

    def test_builtin_default_font_is_not_destroyed_by_manager(self):
        self.m.call(0x0c49258c, 0x2ca14888)
        self.assertEqual(self.events, [])

    def test_failed_face_create_unlinks_metadata_and_returns_null(self):
        cache, entry, data = self.old_cache, self.entries[0], self.entries[0] - 28
        self.m.uc.mem_write(cache, bytes(64))
        self.m.word(cache, 0x2ca16934)
        self.m.word(cache + 8, 256)
        self.m.word(cache + 20, 0x0c3967b5)
        self.m.word(entry + 8, 28)
        self.exact_hook(0x0c3a7b00, lambda: entry)
        self.exact_hook(0x0c3967b4, lambda: self.events.append(
            ('create_failed', self.m.reg(0))) or 0)
        self.exact_hook(0x0c3a472c, lambda: self.events.append(
            ('unlink', self.m.reg(0), self.m.reg(1))) or 0)
        self.exact_hook(0x0c3abe58, lambda: self.events.append(
            ('free', self.m.reg(0))) or 0)
        self.m.uc.reg_write(UC_ARM_REG_R1, self.old_dsc)
        self.m.uc.reg_write(UC_ARM_REG_R2, 0)
        self.assertEqual(self.m.call(0x0c3a7b78, cache), 0)
        self.assertEqual(self.events, [
            ('create_failed', data), ('unlink', cache, entry), ('free', data),
        ])

    def test_gpu_error_still_drains_so_empty_buffers_are_not_success(self):
        self.finish(result=1)
        self.assertEqual(self.events, [
            ('gpu_finish', 1), ('log',), ('error_dump', 1),
            ('release', self.old_cache, self.entries[0]),
            ('release', self.old_cache, self.entries[1]),
        ])
        self.assertEqual(self.m.word(self.glyph_pending + 8), 0)
        self.assertEqual(self.m.word(self.glyph_pending + 24), 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)

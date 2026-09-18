"""Counterexample: why the module does NOT use the firmware image-cache drop-all.

lv_image_cache_drop(0) (0x0c3a3888) dispatches drop_all_cb (0x0c3a7918) to both the
decoded-image cache (*0x200bd310) and the header cache (*0x200bd314). drop_all_cb
skips the PAYLOAD free for a still-referenced entry and logs it, but then walks the
allocated red-black tree and frees every node allocation regardless of reference
state — so an entry a live widget still holds has its node freed underneath it.

The module therefore retires entries one at a time with the generic lv_cache_drop
instead (see firmware_rebind.py:test_held_cache_entry_retired_then_freed_on_last_release).
Cache objects, the entry pool and the node tree are modeled; the LRU traversal and
free dispatch are the firmware's own code. No display is run and no on-hardware
behavior is claimed.
"""
from firmware_support import Machine, fw, hook, require_identity_addresses

import unittest

from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP

LV_IMAGE_CACHE_DROP = 0x0c3a3888       # lv_image_cache_drop(src)
DROP_ALL_CB = 0x0c3a7919               # LRU-rb drop_all_cb (thumb)
DATA_CACHE_GLOBAL = 0x200bd310
HEADER_CACHE_GLOBAL = 0x200bd314
LOG = 0xc3a5e34                        # firmware log; a5 (fmt) is at *sp
RB_RESET = 0xc378b1e                   # ls-tree reset at the end of drop_all_cb
FREE = 0xc3abe58                       # generic free; used as the per-entry free cb

require_identity_addresses(LV_IMAGE_CACHE_DROP, DROP_ALL_CB, DATA_CACHE_GLOBAL,
                           HEADER_CACHE_GLOBAL, LOG, RB_RESET, FREE)


class ImageCacheDrop(unittest.TestCase):
    def setUp(self):
        self.m = Machine()
        self.m.finish_access_monitor()
        self.logs = []
        self.rb_resets = 0
        self.frees = 0
        self.freed = []
        self.hook(LOG, self.on_log)
        self.hook(RB_RESET, self.on_rb_reset)
        self.hook(FREE, self.on_free)

    def hook(self, address, fn):
        hook(self.m, address, fn)

    def on_log(self):
        self.logs.append(self.m.string(self.m.word(self.m.uc.reg_read(UC_ARM_REG_SP))))
        return 0

    def on_rb_reset(self):
        self.rb_resets += 1
        return 0

    def on_free(self):
        self.frees += 1
        self.freed.append(self.m.uc.reg_read(UC_ARM_REG_R0))
        return 0

    def build_vtable(self, addr):
        # A cache class vtable whose +28 slot is the real drop_all_cb, matching
        # lv_cache_drop_all's dispatch (clz+28). Other slots are unused here.
        self.m.uc.mem_write(addr, bytes(64))
        self.m.word(addr + 28, DROP_ALL_CB)

    def build_cache(self, addr, vtable, first_node, ref_pool, count, rb_root=0):
        self.m.uc.mem_write(addr, bytes(64))
        self.m.word(addr + 0, vtable)      # clz
        self.m.word(addr + 4, ref_pool)    # entry pool base
        self.m.word(addr + 12, count)      # entry count (must reset to 0)
        self.m.word(addr + 24, FREE + 1)   # per-entry free callback (thumb)
        self.m.word(addr + 36, rb_root)    # allocated red-black node root
        self.m.word(addr + 48, 0)          # ls base 0 => next = *(node+4)
        self.m.word(addr + 52, first_node)  # first node (0 => empty)

    def test_drop_all_visits_both_caches_and_frees_only_unreferenced(self):
        m = self.m
        vt = 0x3c760000
        data_cache, header_cache = 0x3c761000, 0x3c762000
        ref_pool = 0x3c763000
        node0, obj0 = 0x3c764000, 0x3c765000
        node1, obj1 = 0x3c766000, 0x3c767000
        for a in (data_cache, header_cache, ref_pool, node0, obj0, node1, obj1):
            m.uc.mem_write(a, bytes(64))
        self.build_vtable(vt)
        # Two entries in the data cache: node0 unreferenced (freed), node1
        # referenced (preserved). ref = *(cache[+4] + obj[+16] + 4).
        m.word(node0 + 0, obj0)
        m.word(node0 + 4, node1)
        m.word(obj0 + 16, 0)
        m.word(ref_pool + 4, 0)            # node0 ref count = 0 -> free
        m.word(node1 + 0, obj1)
        m.word(node1 + 4, 0)
        m.word(obj1 + 16, 8)
        m.word(ref_pool + 12, 1)           # node1 ref count = 1 -> preserved
        self.build_cache(data_cache, vt, node0, ref_pool, 5)
        self.build_cache(header_cache, vt, 0, 0, 9)  # header cache empty
        m.word(DATA_CACHE_GLOBAL, data_cache)
        m.word(HEADER_CACHE_GLOBAL, header_cache)

        m.call(LV_IMAGE_CACHE_DROP, 0)     # lv_image_cache_drop(NULL): drop all

        # Both caches were dropped (count reset, one end-reset each).
        self.assertEqual(m.word(data_cache + 12), 0)
        self.assertEqual(m.word(header_cache + 12), 0)
        self.assertEqual(self.rb_resets, 2)
        # Only the unreferenced entry's payload was freed; the referenced one was
        # skipped and logged. This is the part that looks safe.
        self.assertEqual(self.frees, 1)
        self.assertTrue(any('still referenced' in m for m in self.logs), self.logs)

    def test_drop_all_frees_the_node_of_a_still_referenced_entry(self):
        """The hazard: node storage is reclaimed even for a held entry.

        drop_all_cb walks the allocated-node tree at cache+36 after the reference
        scan and frees node[4] and the node itself for every node, without
        re-checking the reference count. A widget still holding that entry is then
        pointing at freed memory, which is why the module retires entries with
        lv_cache_drop instead of calling drop-all.
        """
        m = self.m
        vt = 0x3c760000
        data_cache, header_cache = 0x3c761000, 0x3c762000
        ref_pool = 0x3c763000
        node1, obj1 = 0x3c766000, 0x3c767000
        held_node, held_payload = 0x3c768000, 0x3c769000
        for a in (data_cache, header_cache, ref_pool, node1, obj1,
                  held_node, held_payload):
            m.uc.mem_write(a, bytes(64))
        self.build_vtable(vt)
        # One entry, still referenced, so the payload free is correctly skipped.
        m.word(node1 + 0, obj1)
        m.word(node1 + 4, 0)
        m.word(obj1 + 16, 8)
        m.word(ref_pool + 12, 1)           # ref count = 1 -> payload preserved
        # That same entry's node is in the allocated tree (parent/left/right = 0).
        m.word(held_node + 16, held_payload)   # node[4] = payload allocation
        self.build_cache(data_cache, vt, node1, ref_pool, 5, rb_root=held_node)
        self.build_cache(header_cache, vt, 0, 0, 9)
        m.word(DATA_CACHE_GLOBAL, data_cache)
        m.word(HEADER_CACHE_GLOBAL, header_cache)

        m.call(LV_IMAGE_CACHE_DROP, 0)

        self.assertTrue(any('still referenced' in m for m in self.logs), self.logs)
        # ...yet the held entry's node and its payload were both handed to free.
        self.assertIn(held_node, self.freed)
        self.assertIn(held_payload, self.freed)
        self.assertEqual(m.word(data_cache + 36), 0)

    def test_retired_entry_is_freed_only_after_last_reference(self):
        """Real generic drop/release independently of the signed-loader fixture.

        Only class lookup/unlink and free leaves are modeled. The firmware must
        mark a held entry invalid, unlink it, and defer both frees to refcount 0.
        The signed-module traversal of head -> RB -> data is tested in rebind.
        """
        m = self.m
        cache, node, data = 0x3c761000, 0x3c762000, 0x3c763000
        entry = data + 64
        for address in (cache, node, data):
            m.uc.mem_write(address, bytes(128))
        m.word(cache, fw(0x2ca168c4))
        m.word(cache + 4, entry - data)
        m.word(cache + 24, FREE | 1)
        m.word(cache + 52, node)
        m.word(entry + 4, 2)
        m.word(entry + 8, entry - data)
        m.uc.mem_write(data, b'held-image-canary')
        looked_up = []

        def lookup():
            self.assertEqual((m.reg(0), m.reg(1)), (cache, data))
            looked_up.append(data)
            return entry

        def unlink():
            self.assertEqual((m.reg(0), m.reg(1)), (cache, entry))
            m.word(cache + 52, 0)
            return 0

        self.hook(0xc3a4ace, lookup)
        self.hook(0xc3a472c, unlink)
        m.uc.reg_write(UC_ARM_REG_R1, data)
        m.call(fw(0xc8b8cae), cache)
        self.assertEqual(looked_up, [data])
        self.assertEqual(m.word(cache + 52), 0)
        self.assertEqual(m.uc.mem_read(entry + 12, 1), b'\x01')
        self.assertEqual(m.word(entry + 4), 2)
        self.assertEqual(self.freed, [])
        for remaining in (1, 0):
            m.uc.reg_write(UC_ARM_REG_R1, entry)
            m.call(fw(0xc8b9790), cache)
            self.assertEqual(m.word(entry + 4), remaining)
            self.assertEqual(self.freed, [] if remaining else [data, data])
            self.assertEqual(m.uc.mem_read(data, 17), b'held-image-canary')

    def test_empty_caches_drop_cleanly(self):
        m = self.m
        vt = 0x3c760000
        data_cache, header_cache = 0x3c761000, 0x3c762000
        for a in (data_cache, header_cache):
            m.uc.mem_write(a, bytes(64))
        self.build_vtable(vt)
        self.build_cache(data_cache, vt, 0, 0, 3)
        self.build_cache(header_cache, vt, 0, 0, 4)
        m.word(DATA_CACHE_GLOBAL, data_cache)
        m.word(HEADER_CACHE_GLOBAL, header_cache)
        m.call(LV_IMAGE_CACHE_DROP, 0)
        self.assertEqual(m.word(data_cache + 12), 0)
        self.assertEqual(m.word(header_cache + 12), 0)
        self.assertEqual(self.rb_resets, 2)
        self.assertEqual(self.frees, 0)
        self.assertFalse(any('still referenced' in m for m in self.logs), self.logs)


if __name__ == '__main__':
    unittest.main(verbosity=2)

"""Execute the selected firmware's page lifecycle ladder used to rebuild a page.

Historical investigation only: current activation does NOT rebuild pages. These
counterexamples run native pause/stop/destroy/resume against modeled page objects
and expose policy/refusal and owner-lifetime constraints. Page callbacks and the
view allocator are modeled; transitions execute real firmware instructions.
No display or on-hardware behavior is claimed.
"""
from firmware_support import Machine, fw, hook, require_identity_addresses

require_identity_addresses(0x200c2a28, 0x20096085, 0x200c33bc)

import unittest

from unicorn.arm_const import UC_ARM_REG_R0

POP_WITHOUT_POLICY = fw(0xc696e34)   # exec_pop_lifecycle_without_cachepolicy
POP_WITH_POLICY = fw(0xc696eb8)      # exec_pop_lifecycle_with_cachepolicy
ON_RESUME = fw(0xc696c18)            # on_resume_wrapped: create + start + resume
LOG = 0xc350474                  # pagemanager log
TRACE = 0xc6d29f8                # lifecycle trace hook
OBJ_DELETE = 0xc384e6c           # lv_obj_delete
VIEW_ALLOC = 0xc6acd04           # root view allocator
OBJ_FLAG = 0xc3840d8             # lv_obj_add_flag (resume touches the root)
OBJ_EVENT = 0xc37fea0            # event-cb removal on destroy

SCREEN_STATE = 0x200c2a28        # 2 == screen on
PAGE_LAYER = 0x20096085          # non-zero == page layer active
STACK_ROOT = 0x200c33bc          # parent used by the view allocator

STATE, POLICY, ROOT_VIEW, ASYNC = 40, 41, 48, 36
PAGE, VIEW, NEW_VIEW = 0x3c7c0000, 0x3c7c1000, 0x3c7c2000


class PageRebuild(unittest.TestCase):
    def setUp(self):
        self.m = Machine()
        self.m.finish_access_monitor()
        self.events, self.calls, self.deleted = [], [], []
        # Logging, tracing and the LVGL/activitymanager helpers the ladder calls
        # around the transitions are modeled; the transitions themselves are not.
        for addr in (LOG, TRACE, OBJ_FLAG, OBJ_EVENT, 0xc695d4c, 0xc387ba8,
                     0xc9195ba, 0xc4fbb10, 0xc383278, 0xc8b9462, 0xc6b90d0,
                     0xc37f8f0, 0xc385038):
            self.hook(addr, lambda: 0)
        self.hook(OBJ_DELETE, self.on_delete)
        self.hook(VIEW_ALLOC, lambda: NEW_VIEW)

    def stages(self):
        """Lifecycle callbacks only; the handler-chain event dispatches that
        interleave with them are recorded separately."""
        return [c for c in self.calls if c != 'event']

    def hook(self, address, fn, *, synthetic=False):
        hook(self.m, address, fn, synthetic=synthetic)

    def on_delete(self):
        self.deleted.append(self.m.uc.reg_read(UC_ARM_REG_R0))
        return 0

    def build_page(self, *, state, policy, root=VIEW, async_destroy=0):
        """A page whose handler chain is itself; callbacks are synthetic PSRAM
        stubs we trap so dispatches never depend on arbitrary firmware PCs."""
        m = self.m
        m.uc.mem_write(PAGE, bytes(256))
        m.word(PAGE, 0)                       # end of the handler chain
        m.word(PAGE + ASYNC, async_destroy)
        m.word(PAGE + STATE, state | (policy << 8))
        m.word(PAGE + ROOT_VIEW, root)
        m.word(SCREEN_STATE, 2)
        m.word(PAGE_LAYER, 1)
        m.word(STACK_ROOT, 0x3c7c3000)
        # Lifecycle callback slots: trap each to record that it ran.
        for slot, name in ((13, 'event'), (19, 'create'), (20, 'resume'),
                           (22, 'start'), (23, 'pause'), (24, 'stop'), (25, 'destroy')):
            stub = 0x1c7f0000 + slot * 8
            m.word(PAGE + slot * 4, stub | 1)
            self.hook(stub, (lambda n=name: (self.calls.append(n), 0)[1]), synthetic=True)

    def state(self):
        return self.m.uc.mem_read(PAGE + STATE, 1)[0]

    def root(self):
        return self.m.word(PAGE + ROOT_VIEW)

    def test_forced_teardown_destroys_root_view_for_every_policy_but_two(self):
        # Policies 5/6/7 are ones the with-policy path deliberately keeps alive.
        for policy in (1, 4, 5, 6, 7):
            with self.subTest(policy=policy):
                self.calls, self.deleted = [], []
                self.build_page(state=17, policy=policy)
                self.m.call(POP_WITHOUT_POLICY, PAGE)
                self.assertEqual(self.state(), 4)      # destroyed
                self.assertEqual(self.root(), 0)       # root view released
                self.assertIn(VIEW, self.deleted)
                self.assertEqual(self.stages(), ['pause', 'stop', 'destroy'])

    def test_forced_teardown_refuses_policy_two(self):
        """The forced path is not fully policy independent: policy 2 returns
        after the pause leg: final state alone cannot prove recreation."""
        self.build_page(state=17, policy=2)
        self.m.call(POP_WITHOUT_POLICY, PAGE)
        self.assertEqual(self.state(), 18)             # paused, not destroyed
        self.assertEqual(self.root(), VIEW)            # widgets still alive
        self.assertEqual(self.deleted, [])
        self.assertEqual(self.stages(), ['pause'])
        # Resume restores it, so a refused rebuild does not strand the page.
        self.m.call(ON_RESUME, PAGE)
        self.assertEqual(self.state(), 17)
        self.assertEqual(self.root(), VIEW)

    def test_cache_policy_path_keeps_a_cached_pages_widgets(self):
        """Why navigating away and back does not pick up a theme."""
        for policy in (2, 5, 6, 7):
            with self.subTest(policy=policy):
                self.calls, self.deleted = [], []
                self.build_page(state=17, policy=policy)
                self.m.call(POP_WITH_POLICY, PAGE)
                self.assertNotEqual(self.state(), 4)
                self.assertEqual(self.root(), VIEW)    # widgets survive
                self.assertEqual(self.deleted, [])
        # Policy 1 and 4 are the only ones that reach destroy.
        for policy in (1, 4):
            with self.subTest(policy=policy):
                self.calls, self.deleted = [], []
                self.build_page(state=17, policy=policy)
                self.m.call(POP_WITH_POLICY, PAGE)
                self.assertEqual(self.state(), 4)
                self.assertEqual(self.root(), 0)

    def test_resume_rebuilds_a_destroyed_page_through_on_create(self):
        self.build_page(state=17, policy=7)
        self.m.call(POP_WITHOUT_POLICY, PAGE)
        self.assertEqual(self.state(), 4)
        self.calls = []
        self.m.call(ON_RESUME, PAGE)
        # Climbed destroyed -> created -> started -> resumed, and the page's own
        # on_create ran against a freshly allocated root view.
        self.assertEqual(self.state(), 17)
        self.assertEqual(self.root(), NEW_VIEW)
        self.assertNotEqual(self.root(), VIEW)
        self.assertEqual(self.stages(), ['create', 'start', 'resume'])

    def test_resume_is_the_recovery_path_from_every_intermediate_state(self):
        # 4 is destroyed (no root yet); 5/8/9/18 already own their root view.
        for state in (4, 5, 8, 9, 18):
            with self.subTest(state=state):
                self.build_page(state=state, policy=7,
                                root=0 if state == 4 else VIEW)
                self.m.call(ON_RESUME, PAGE)
                self.assertEqual(self.state(), 17)
                self.assertNotEqual(self.root(), 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)

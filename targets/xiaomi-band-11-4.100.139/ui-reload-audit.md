# .139 miwear restart verification

## Current result

Automatic restart/reinstallation is **not complete**. The previous kill/sleep/start/
restore script was not a verified implementation and is now disabled (exit 78,
no task termination). A delay after launch cannot ensure interception before
initial font/image loading. The signed ELF is an open-hook candidate, not proof
of complete UI refresh.

## Direct firmware evidence

Firmware SHA-256:
`31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74`.

- Boot script at 0x0ca0b965 launches `miwear &`, guarded by the absence of
  `/data/nobusiness`. This script contains no respawn loop.
- The **actual builtin table** starts at 0x2ca02564. Entry 30 at 0x2ca02744 is
  `{name="miwear", priority=102, stack_size=65536, main=0x0c7def89}`.
- 0x2ca7a78c is instead a Lua library entry. It is not a process descriptor.
- Builtin loader 0x0c330720 copies the fixed entry, priority and stack size into
  a binary descriptor. It does not copy a new miwear data/BSS image.
- NSH launch path 0x0c3692c0 uses this table and task setup 0x0c357e30. The task
  startup wrapper 0x0c35a344 calls entry at TCB+56; global constructors are
  guarded by the one-time flag at 0x200b98e4.
- Main 0x0c7def88 schedules 0x0c7e88b0 through 0x0c7def38, which **increments**
  byte 0x200da910. The initializer only has cases 1..7. For retained state 7,
  another main invocation reaches state 8 and the completion-notification path,
  not graphics initialization.
- Actual default branch 0x0c7ea46c stops its timer and emits booting-completed
  and event 16; it does not rebuild fonts/pages. Hex-Rays can omit reachable
  branches in this database: raw disassembly was also checked.
- `pkill` builtin at 0x0c371f98 defaults to signal 15, finds matching cmdline
  entries in /proc, and calls kill wrapper 0x0c359ef8. Signal delivery is not
  evidence of completed graphics cleanup or successful task termination.
- Lua os.execute at 0x0c717d70 calls system at 0x0c370774. That creates a
  separate `system -c` task via 0x0c8bbdac and waits for it. This establishes
  a possible external helper launch mechanism, not a restart contract.

`tests/firmware_restart.py` executes the builtin loader and repeated main/init
paths from the real firmware. Scheduling, logging and completion notifications
are modeled explicitly. These are **counterexample tests**, not whole-process
restart tests. Both pass. They show why starting the fixed builtin entry with
retained state is not a clean reinitialization.

## Supervisor lifetime: not established by caller context

Earlier statements that killing miwear necessarily destroys the Supervisor,
module image and /dev/canopus were unsupported. The custom loader uses explicit
heap allocations and registers a pseudo-device; it is not an independent daemon,
but caller context alone does not establish allocation/node reclamation on task
exit. Likewise, reboot-only unregister and refusal to replace an existing device
node do not prove survival. Task-group cleanup/heap ownership remains unverified.

## Implemented rebind support

`rh_reinstall_posix` can restore a known driver's slot, idempotently, when called
in an established owner-thread transaction. Supervisor repeated restore requests
rebind enabled BOOT_RESIDENT modules which explicitly opt in with
`CANOPUS_FLAG_REACTIVATE_AFTER_UI_RESTART`. Other modules keep one-shot semantics.
Existing opted-in modules rebind even alongside newly loaded modules; new modules
activate once. Failures are recorded without pretending the resident image was
unloaded. C host tests cover these branches.

This support **does not** supply quiescence, reset firmware globals, arrange a new
UI-owner callback or ensure a pre-resource startup barrier. Shell writes execute
in the writer's context; sleeping before such a write does not make it a UI event.

## Resource ABI

The '/' POSIX driver is 0x200bd3b8; cache size +4 is 4096 and open +12 is
0x0c3a6195. Driver-list nodes contain pointers, with next at node+8 when node data
size is four. lv_fs_open strips the first slash; POSIX open adds it back and
returns fd+1, zero on failure. Firmware tests cover lookup and fd-zero handling.

Fonts additionally use native access/FreeType operations and caches; the LVGL
open hook alone is not a demonstrated complete font redirection mechanism.
MiSans paths are added at initialization, including /tmp paths. Existing font
wrappers/FreeType cache entries must be released or rebuilt by an actual lifecycle.

## Other examined APIs and scope

- vg_deinit 0x0c494198: graphics teardown, live font ownership constraints;
  observed callers are AIOTJS, not a native miwear restart transaction.
- Language change 0x0c697398: event 18 to existing pages, not full destruction.
- exit_and_restart 0x0c5e85bc: after-sales/password application flow.
- destroy_all_resources 0x0c50a314: activities application context only.
- Home reload 0x0c540b14 case 4 / 0x0c6a4b6c: home widget children only.
- __miwear_uninit 0x0c7285b0: AIOTJS interconnect teardown only.
- theme_apply 0x0c5048d8: class-based style application, not global rebuild.

## Remaining prerequisite

A real teardown/reset contract for the builtin task plus a resource-before-use
startup hook must be recovered or implemented. Zeroing the single init-state byte,
polling the open slot or blindly repeating startup would not clear the other
persistent managers, handles, locks and references. None is enabled as a substitute.


## Executed ownership and signed-image checks

- Actual `task_start 0x0c35a344` reads entry from TCB+56 and argv from
  *(TCB+100)+44. The test installs a synthetic current TCB, executes the real
  startup wrapper and checks that argc/argv/current-owner reach the entry,
  and its return value reaches task exit. Global init state is not reset.
- Actual `group_leave 0x0c3533f4` with an empty synthetic group releases the
  modeled per-task info but leaves a resident-image canary and init state
  unchanged. This establishes that this path is not a general heap sweep,
  not that a complete miwear shutdown is harmless.
- `tests/firmware_rebind.py` loads the signed resource ELF through the built
  Supervisor, resets only the driver slot synthetically, sends restore again,
  and verifies the same aliased callback is reinstalled without image allocation.
  Mapped file opening then executes through the real POSIX callback. Locked
  mappings, unknown-slot rejection, missing config and query serialization are
  tested. This is not described as a whole-task restart.
- This integration exposed and fixed a missing required query callback and the
  fact that the current Supervisor does not invoke prepare before activate.
  First activate now configures lazily; a rebind never rereads configuration.

## Follow-up: actual enable/use sequence (2026-09-13)

Direct reinspection of `miwear_main` at 0x0c7def88 shows it stores a loop at
0x200c36b8, schedules the retained-state initializer, runs the loop, logs
`miwear exit` and returns. This function contains no corresponding graphics
teardown, loop cleanup or global reset. References to `vg_deinit` at 0x0c494198
lead to 0x0c74e24c and 0x0c74eaec, not this native main exit. This does not prove
that no possible lifecycle can be implemented; it rules out treating the main
entry/return pair as that lifecycle.

The current framework has a second independent ordering limitation:

- `CANOPUS_SUP_CMD_ENABLE` persists an enabled intent and returns reboot-required;
  it does not call the module's activate callback.
- `activate_restored_modules` only loads enabled INSTALLED slots. A same-session
  ENABLED slot does not become hot-loaded by another RESTORE request.
- A newly loaded Supervisor restores the enabled registry entry; its UI-context
  restore operation then loads/activates the module. The native Manager calls
  `canopus_supervisor_restore_after_boot`, which is guarded once per Supervisor.
  The framework installer also has an explicit restoration step.
- Both operations happen in an already running UI environment, not before all
  system fonts/images were first used. A full device reboot alone does not
  supply an early module bootstrap.

`firmware_rebind.py` now also executes the signed module/actual Supervisor path
to check deferred enable, and verifies that a file handle opened before Hook
activation still refers to its original file while a new open is redirected.
The VFS is modeled. These are ordering/handle tests, not graphics-cache tests.

The RHQ1 descriptor query is not currently surfaced by the Manager UI; public
QUERY_MODULE returns general module metadata, not this private status record.
The install guide therefore uses the actual Manager lifecycle state and no
longer asks ordinary users to inspect counters that the UI does not expose.

Completion still requires implementing and validating a lifecycle that owns the
native event loop, every affected manager and live font/image reference, and
installs hooks before reconstruction. No kill/start/reset-byte sequence has
been added. The current bundle is incomplete for whole-system theme replacement,
not merely awaiting a physical smoke test of an otherwise finished feature.

## Follow-up: image reload implemented via lv_image_cache_drop (2026-09-13)

Images, unlike fonts, are reloadable without a page rebuild. The LVGL image
cache is a pair of global caches — decoded-image data at *0x200bd310 and image
headers at *0x200bd314 — keyed by the decoded source path. A cached image is
served before open() is ever called, so a path redirect never reaches an image
that was decoded before the hook was installed.

lv_image_cache_drop (0x0c3a3888) with src=0 drops both caches via
lv_cache_drop_all (0x0c8b8d60 -> clz[+28] drop_all_cb 0x0c3a7918). drop_all_cb
frees only unreferenced entries and logs the still-referenced ones. The firmware
itself calls this from 40+ sites (page/album/watchface teardown), so it is the
supported invalidation API. lv_init (0x0c3a9598) creates the cache objects; only
the backing heap is deferred, so the pointers and class are valid at activate
time — the module still guards on both globals and their class being non-null.

The module now calls lv_image_cache_drop(0) at the end of activate(), on the UI
owner thread and outside the interrupt lock, after the redirect is installed.
The firmware's own _lv_display_refr_timer (0x0c38956c) re-decodes on the next
redraw, re-opening the file through the redirect. Only unreferenced entries are
dropped; images currently on screen reload after the next navigation/redraw.

firmware_image_lifecycle.py executes the real lv_image_cache_drop and the real
drop_all_cb against modeled cache objects: it confirms src=0 drops BOTH caches,
frees only the unreferenced entry, preserves the referenced one, and resets each
cache. firmware_rebind.py:test_activate_drops_present_image_cache runs the real
signed ELF through the Supervisor restore and confirms activate drops a present
image cache (query images_dropped == 1). The symbol is allowlisted in the target
pack (lv_image_cache_drop.json, EVID-RESOURCE-4139-002).

This resolves the image half of "font/image refresh". Fonts remain unaddressed
on purpose: their wrappers are page-owned and reference-counted, so a blind drop
would corrupt live pages. No hardware execution has been performed.

## Follow-up: forced full-screen invalidation after the drop (2026-09-13)

Re-examining drop_all_cb (0x0c3a7918) confirms it frees every entry whose
reference count (entry+20) is 0 and only *skips* the still-referenced ones. An
on-screen lv_image widget opens its decoder when its draw task runs and closes it
afterward, so between refresh cycles its cache entry sits at ref 0. The
activate-time lv_image_cache_drop(0) therefore already evicts on-screen images in
the common case; the remaining gap was that nothing marked the (unchanged) widget
tree dirty, so the firmware only repainted whatever it happened to invalidate on
its own — the new image did not appear until the user navigated.

The module now, immediately after the cache drop, calls _lv_inv_area (0x0c382428)
on the default display (*0x200bd200) with an oversized area. lv_area_intersect
(0x0c3a39b4) clips it to {0,0,hres-1,vres-1}, so the whole active screen is
appended to the display's invalidated-area list (disp+60.., count at disp+604);
the running _lv_display_refr_timer (0x0c38956c) then repaints the full screen on
its next tick, re-opening every image through the redirect. This is the firmware's
own screen-invalidation primitive — its MiWearScreen wake handler
async_apply_screen_state_change (0x0c6d096c) uses the same function for its
"invalidate screen." path.

Guards (all on the UI owner thread, outside the interrupt lock): the call is
skipped when there is no default display, no active screen (disp+24 == 0), a
render is already in progress (bit 1 of disp+58, which _lv_inv_area would
otherwise assert on), or invalidation is disabled (lv_display_is_invalidation_enabled,
disp+608 == 0, under which _lv_inv_area is a safe no-op anyway). Success is
counted in the RHQ1 status as `redraws` (status format bumped to v3, 32 bytes).

firmware_ui_redraw.py executes the real _lv_inv_area against a modeled display and
confirms it appends the whole clipped screen in partial mode, marks the whole
screen in direct mode, and is a safe no-op when invalidation is disabled.
firmware_rebind.py:test_activate_requests_full_redraw runs the real signed ELF
through the Supervisor restore with a modeled display and confirms activate
requests the invalidation (query redraws == 1) and that the firmware appended the
full screen. The symbol is allowlisted in the target pack (lv_inv_area.json,
EVID-RESOURCE-4139-003).

What this does NOT do: it only schedules a repaint via the firmware's own refresh
timer (no forced synchronous flush), and it does not rebuild pages, so a resource
still referenced by a live widget — an animation frame, or any font wrapper —
survives the drop and is unaffected by the invalidation. Those still require the
page-rebuild lifecycle. No hardware execution has been performed; whether a given
on-screen image is at ref 0 at activate time depends on it not being mid-draw or
held open by an animation.

## Correction: wrong display field, and a droppable refresh request (2026-09-15)

Two defects in the section above were found by re-reading the firmware rather than
the module's own tests, and are now fixed.

1. **The active-screen guard read the wrong field.** The code and its fixtures both
   used `disp+24`. The exact .139 `lv_display_get_screen_active` (0x0c3807ec) reads
   **`disp+696`**; `disp+24` is the DPI, which `lv_display_get_dpi` (0x0c3807bc)
   returns and which defaults to a non-zero 130. The guard therefore passed on a
   display with no active screen. Because the emulation fixtures modeled the same
   wrong offset, the tests agreed with the bug instead of catching it —
   `firmware_ui_redraw.py:test_real_accessors_distinguish_dpi_from_screen` now
   executes both real accessors, and `firmware_rebind.py` sets a deliberately
   non-zero DPI so the confusion cannot reappear.

2. **A busy UI silently dropped the one refresh.** The old code called the drop and
   the invalidation once inside `activate()` and discarded the request if the UI was
   rendering or invalidation was disabled; the theme then stayed stale until the
   user navigated. The module now records one coalesced pending request and retries
   it from a temporary LVGL timer (`lv_timer_create` 0x0c3abd20 / `lv_timer_del`
   0x0c3abe70, 50 ms), deleting the timer as soon as it succeeds. This is still one
   refresh, not periodic polling. Success is only counted when the firmware actually
   retained a dirty area covering the whole display, and the cache is retired at most
   once per request so a rejected invalidation cannot re-retire repeatedly. If the
   timer cannot be allocated, activate returns -2011 with the redirect left resident.

## Correction: retire cache entries instead of dropping the whole table

The earlier claim that on-screen images are "already evicted because their entries
sit at ref 0 between draws" was asserted more strongly than the evidence supports —
it was never demonstrated along a real widget draw path, and it is false for any
widget that holds a decoded handle (animations).

More importantly, the firmware's `drop_all_cb` (0x0c3a7918) skips the *payload* free
for a still-referenced entry but then walks the red-black tree and frees every node
allocation regardless of reference state. The module no longer uses it.

Instead the module walks the cache's entry list and calls the generic
`lv_cache_drop` (0x0c8b8cae) per entry. That function looks the entry up through the
class vtable, and for a held entry sets the invalidation flag at `entry+12` and
unlinks it, leaving the payload valid for its holder; the deferred free happens in
`lv_cache_entry_release_data` (0x0c8b9790) when the last reference is dropped. A
zero-reference entry is freed immediately. Either way the next lookup for that source
path misses and goes through the redirect. The traversal is guarded on the cache's
class pointer being the recovered LRU/RB class (0x2ca168c4) and stops if the list
head does not advance. `firmware_rebind.py:test_held_cache_entry_retired_then_freed_on_last_release`
executes the real retirement and the real final-release path against a held entry and
asserts the payload canary survives until the last release.

`firmware_image_lifecycle.py:test_drop_all_frees_the_node_of_a_still_referenced_entry`
is the counterexample that justifies this: it executes the real drop-all against a
referenced entry whose node is in the allocated tree, and shows the firmware logs
"still referenced" (correctly skipping the payload free) and then frees that node
and its payload allocation anyway.

Symbols are allowlisted as lv_cache_drop.json (EVID-RESOURCE-4139-004).

## The decode path, traced (2026-09-15)

The earlier claim that on-screen entries "sit at ref 0 between draws" was retracted
above as unevidenced. It is now traced properly, and the mechanism holds for the
main image path:

- `lv_draw_image` (0x0c381880, lv_draw_image.c) per draw calls
  `lv_image_decoder_get_info` (0x0c38e4e4) and, on the direct branch,
  `lv_image_decoder_open` (0x0c38ea60), then `lv_image_decoder_close` (0x0c38cf50)
  before returning. The decoder is NOT retained across draws on this path.
- `image_decoder_get_info` (0x0c38e394, lv_image_decoder.c:352) looks the source up
  in the header cache (*0x200bd314). On a hit it immediately calls
  `lv_cache_entry_release_data` (0x0c8b9790) — the reference is transient. On a miss
  it calls `lv_fs_open` (0x0c3a7d7c), which is exactly the driver the module hooks,
  then caches the decoded header.
- `lv_image_decoder_open` looks the source up in the decoded-image cache
  (*0x200bd310). On a hit it stores the entry at `dsc+68` and holds that reference
  for the duration of the draw, released by `lv_image_decoder_close`. On a miss it
  falls through to `get_info` and the decoder chain, i.e. to `lv_fs_open`.

So the reference an on-screen image holds is per-draw, between open and close —
not a handle kept across frames. After per-entry retirement the next lookup misses,
`lv_fs_open` runs, and the redirect is observed. Retiring during a draw is still
handled correctly by the deferred free, and the module skips while
`rendering_in_progress` anyway.

Still NOT established: the deferred draw-task branch of `lv_draw_image` (taken when
the header flag 0x40 is clear — it queues a type-5 task via 0x0c380cb8 instead of
decoding inline), and any animation or canvas-style widget that owns a decoded
buffer rather than re-opening a source. Those may still show the old image and need
the page-rebuild lifecycle. This section is static recovery from the exact .139
decompilation; it is not emulation-executed.

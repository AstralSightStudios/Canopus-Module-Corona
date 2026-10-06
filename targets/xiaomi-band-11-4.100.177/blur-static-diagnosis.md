# Blur background failure: static diagnosis (.177)

Scope: pre-fix implementation after the DRAW_MAIN callback change. The bounded
follow-up below fixes initialization/retry; the historical findings remain as
failure evidence.
Firmware: Xiaomi Band 11 4.100.177, AP SHA256
`ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b`.
Database: `mifw/analysis-q66-177/177/q66-ap.i64`.
The original audit was a diagnosis without runtime changes. A subsequent bounded
source fix is described below; neither stage is hardware acceptance.

## P0: cold-start initialization never runs for a default background

`src/platform.c` initially stores zero in `g_blur_draw_buf`. For blur,
`current_bg_source()` returns that zero pointer. Each of the launcher, control
center and notification application branches compares the desired source with
`bg_source(obj)` BEFORE trying to allocate/generate the buffer:

```c
source = current_bg_source(type);        /* 0 at cold start */
if (obj && bg_source(obj) != source) {   /* native default is also 0 */
    /* Buffer initialization and callback installation are inside here. */
}
```

When native style property 40 is zero, the branch is never taken. Repeated polls
cannot change this state. No allocation, snapshot, setter or callback occurs.
Blur staging bypasses image preflight and commit still returns success, so a
successful reload acknowledgement does not detect this failure.

An ASan/UBSan host reproduction compiled the actual adapter verbatim with
`__arm__` defined and mocked native leaves. Seven zero-source receivers were
present: four launcher lists, a control-center container, a notification
container and notification list. This executes ARM-guarded C, not ARM machine
code or firmware. The observed counters were:

```text
commit rc=0 active=1/1/1 blur_buf=0 allocations=0 snapshots=0 setters=0 callbacks=0 getters=14
poll1  rc=0 active=1/1/1 blur_buf=0 allocations=0 snapshots=0 setters=0 callbacks=0 getters=21
poll2  rc=0 active=1/1/1 blur_buf=0 allocations=0 snapshots=0 setters=0 callbacks=0 getters=28
poll3  rc=0 active=1/1/1 blur_buf=0 allocations=0 snapshots=0 setters=0 callbacks=0 getters=35
```

All seven source pointers stayed zero. No sanitizer error occurred; allocation
and snapshot mocks were never entered, so no pixel pointer was dereferenced.

Firmware witness: `0x0c380164` is the effective-style getter. When property 40
has no matching style, its default path selects zero (neither default mask
`0x101008d` nor `0x808002` has bit `40 - 34` set). Property 41, the background
image opacity, defaults to 255, not zero. `0x0c386de2` obtains property 40 and
writes it into the rectangle descriptor at +48; this is the intended property.

Fix direction: ensure a valid nonzero blur source BEFORE source comparison.
Do not turn initialization failure into a null background assignment. Separate
allocation from valid pixel contents; retry failures without falsely reporting
successful generation. This ordering is required regardless of how object
attachment and refresh are ultimately driven.

## P1: callback order is not the claimed pre-background order

- `0x0c388f28` dispatches events 23, 24, 25 in sequence for the object main draw.
- `0x0c38754c` routes event 24 to `0x0c387202`.
- `0x0c387202` initializes the rectangle descriptor via `0x0c386de2` and submits
  the rectangle via `0x0c38e47c`.
- `0x0c37fb08` dispatches PREPROCESS user callbacks, then the class callback,
  then ordinary user callbacks.
- `0x0c3a3e08` selects PREPROCESS by filter bit `0x8000` and matches event code
  using `filter & 0x7fff`.

The module registers filter `24`, WITHOUT `0x8000`. Event 24 is indeed
DRAW_MAIN, but `on_panel_draw()` runs AFTER the class submits the background
rectangle, not before it. A queued GPU draw reading a subsequently modified
buffer is not proof that the first frame is correct or that its lifetime is
safe. A proven earlier event/preprocess path and native draw synchronization
must be considered together; changing an event number alone is insufficient.

## Other confirmed implementation defects

1. Every blur-valued `bg_set()` appends another callback. Reload detaches and
   reapplies the background, registering duplicates. Old callbacks remain when
   changing to image/system. Use native callback removal or lifecycle-aware
   deduplication; a permanent address-only registry is unsafe with address reuse.
2. `rh_platform_update_blur()` returns void and may exit on allocation, snapshot
   or pixel-pointer failure. `on_panel_draw()` unconditionally clears
   `s_blur_needed`. An allocated but uninitialized draw buffer is also exposed
   as a valid source. Failed generation must retain a retry/dirty state.
3. The dirty flag is still driven by the 1000 ms presence-mask poll. Presence
   is not visibility. Notification `ntf_close` at `0x0c55c54c` calls
   `0x0c542c60`, which scrolls its view to y=520; it does not clear
   `0x200c633c`. A container being nonzero therefore does not establish that
   the panel is currently open. Replacing a nonzero object with another also
   leaves the presence mask unchanged. The current design is not fully
   event-driven, and quick close/reopen transitions can be missed.

## Initial ABI review and corrected buffer-layout finding

- `0x0c386c14` allocates a 28-byte draw buffer, sets width at +4, height at +6,
  stride at +8, **aligned pixel data at +16**, raw allocation at +20, handlers
  at +24. The first audit and its host fixture incorrectly called +20 pixel
  data. Full Thumb disassembly and native `0x0c386904` corrected this finding.
- `0x0c5009e8` creates the firmware snapshot buffers using handlers
  `0x200bd18c`, display width/height, color format 18 and stride 0, matching the
  module's intended RGB565 buffer creation contract.
- `0x0c5f126c` obtains a snapshot draw buffer via `0x0c4f9944` and fills it via
  `0x0c6a29f0` / `0x0c3af654`. Release `0x0c8f29ec` clears the snapshot pool's
  in-use bit. The stride at +8 is correct, but pixel access at +20 WAS wrong:
  GPU/native access uses +16, whereas native destruction frees +20.
- `lv_obj_add_event_cb` at `0x0c37fbec` is STATIC_RECOVERED, device NOT_PROBED,
  approval PENDING in Canopus-Private, not publicly callable-approved as
  previously claimed.

These observations do not prove snapshot success in every watchface lifecycle,
GPU coherence after CPU pixel writes, or acceptable frame cost on hardware.

## Configuration and test coverage

The correctly spelled manifest declarations `launcher`, `controlCenter`,
`notifications`, each set to `"blur"`, serialize as the expected CFG tokens
`launcher=blur`, `control_center=blur`, `notifications=blur`. The C parser
converts these to the internal `:blur` sentinel correctly. No token mismatch
was found. Manifest unknown keys currently disappear silently; manifest
`control_center` is not equivalent to its documented `controlCenter` key.

Existing native background tests compile WITHOUT ARM/Thumb defines. Their
`current_bg_source()` is the nonzero pathname-pointer fallback, and their
initial style is FOREIGN rather than zero. Control/notification slots are
always absent. Consequently their pass does not exercise this blur deadlock,
the screenshot operation, or callback registration. ELF structural validation
likewise does not test these runtime branches.

Required regressions before another device build:

- Execute the actual ARM-guarded blur branch with mocked native functions.
- Cold start with default zero styles on ALL three panels.
- Allocation/snapshot failure and successful retry, without publishing invalid
  pixels or clearing dirty state on failure.
- Repeated blur reload, image/system transitions, object deletion/address reuse,
  and callback registration/removal counts.
- Actual pre-background callback ordering, reentrancy and GPU pixel-write
  synchronization.
- Device-only checks for opening/closing cached panels, snapshot availability,
  memory use and measured frame latency. No 0 ms / <0.4 ms / 60 FPS claim is
  justified by the existing host tests or ELF build.

## Bounded follow-up: initialization and retry fixed

`src/platform.c` now initializes blur once per poll BEFORE comparing styles.
Allocated storage is hidden until `g_blur_valid` confirms initialized pixels;
zero/invalid blur sources are not installed, so failed startup keeps native or
foreign backgrounds and can retry. Image targets remain independent of blur
failure. Update returns success/failure, clears the dirty flag only on success,
validates RGB565 dimensions/stride/data capacity and releases rejected snapshots.
A reentrancy guard prevents snapshot events from recursively acquiring snapshots
or corrupting shared scratch buffers. A failed refresh preserves the last valid
pixels. Commit marks a blur reload dirty and disables dirty refresh for an
all-system/image configuration.

`tests/test_blur_background_native.py` executes the extracted ARM-guarded C
implementation with native mocks and real low-address descriptors/pixels. All
13 tests pass under ASan/UBSan, including cold stage/commit on seven zero-source
receivers, allocation/snapshot retry, foreign-style preservation, invalid
metadata, mixed image/blur, callback failure and snapshot reentrancy. macOS runs
the fixture as x86_64 (Rosetta required on Apple Silicon) to permit verified
low-32-bit non-fixed memory mappings. These are host models, not GPU or native
LVGL event-delivery tests.

The full host suite, four exact `.177` static tests and ARM build/ELF verification
pass. Artifact: `build/resource-hook.elf` (20 sections, 0 undefined symbols).
At that bounded follow-up, callback-order, duplicate-registration,
visibility-trigger and GPU synchronization findings remained unresolved. The
full event follow-up below supersedes that implementation. Neither artifact has
hardware acceptance or measured frame timing.

## Full follow-up: refresh ownership, lifecycle and synchronization

The current implementation removes per-object DRAW_MAIN callbacks and the
panel-presence mask entirely. The 1000 ms watcher loads configuration, maintains
a display subscription and retires GPU-failure pins; it never generates blur or
attaches backgrounds. A single ALL|PREPROCESS display callback owns frame work.
This follows the native LVGL refresh cycle, not a renamed 1-second blur timer.

### Exact firmware contracts

- `0x0c381eae` builds a display event and dispatches the event list at display
  +736 in preprocess and ordinary passes. `0x0c3a3ec0` adds a descriptor;
  `0x0c381e66(display, callback, user_data)` removes only matching descriptors.
- `0x0c3894d0` emits REFR_START=52 before layout/area join/object draw;
  REFR_READY=53 follows invalid-area cleanup. Failed updates/configuration wakes
  are reissued at READY, not lost in the current frame's dirty-area clearing.
- `0x0c384528` emits DELETE=36 before freeing the event list and display.
  Live display list payload size is 792; prev/next are at +792/+796. Saved
  identities are used only after bounded membership/links/tail validation.
- `0x0c383f00` updates layout. Before inspecting visibility, our START handler
  settles the same native screen slots +696/+700/+708/+712/+716.
- `0x0c381d4c(object, area)` checks active/previous/overlay screen membership,
  hidden flags on all ancestors and clipped intersection. Nonempty object slots
  are not treated as proof that a panel is open.
- **Wake ABI:** `0x0c382080` is `_lv_inv_area(display, area)`, using display
  +58/+604 and resolution getters. Object wrapper `0x0c382198` resolves the
  display and calls it. The existing full-redraw display ABI remains unchanged.
- **Pixel ABI:** native create stores aligned data at +16 and raw allocation at
  +20. `0x0c386904` and native GPU consumers use +16; `0x0c3868bc` frees +20.
  The new mock keeps these pointers distinct and checks raw/padding sentinels.
- Native `0x0c3998a4` calls `vg_lite_finish` at `0x0c91e58c`, treating zero as
  success. Submission/wait failures propagate; native task retirement alone
  is not accepted as GPU idle. Our fences precede shared-buffer mutation and
  CPU snapshot reads. A failed post-snapshot fence pins that pool descriptor
  until an independent successful fence; it is never immediately freed/reused.
- D-cache sync `0x0c92e5f4(1)` writes command 1 to D-cache base 0x07ffc000 +0x34.
  The ID/start/length invalidation ABI is witnessed by `0x0c9252c0` calling
  `0x0c721248`, a thunk to ROM 0x00275814. Cache barriers/sync surround source
  invalidation and destination publication. BES OpenPineBuds HAL source
  corroborates enum/register/API conventions, not exact hardware operation.
  The ROM body is outside AP and is not claimed as independently decompiled.

Seven exact receiver slots are rediscovered each frame and again after snapshot
rendering. Newly visible, rebuilt, or native-style-reset blur receivers and
reloads mark pixels dirty. Stable visible lists do not recapture during scrolling.
Offscreen caches may get the current valid source, but never trigger generation.
Blur-to-blur reload retains initialized pixels AND published styles. All-system
commit clears owned styles and removes our subscription. The module-owned draw
buffer remains resident for reuse; no lifetime-unsafe GPU buffer free is added.

Frame epochs and display identities are revalidated after layout/snapshot/setter
operations, preventing stale object dereferences or publication after DELETE or
reentrant reload. Commit rediscovers the notification list after clearing its
container, rather than retaining a potentially deleted sibling. A new display
registered while an old frame unwinds wakes immediately; deferred same-display
wakes are flushed at READY or by later subscription maintenance. Invalid registry
validation preserves unresolved subscription ownership instead of adding a
second callback when the registry recovers.

### Evidence and validation

Seven restricted records and evidence bundle
`EVID-RESOURCE-BACKGROUND-REFRESH-177-001` were added under
`../Canopus-Private/targets/xiaomi-band-11-4.100.177/`. All remain
STATIC_RECOVERED, restricted, approval PENDING, device NOT_PROBED. No firmware
range was broadened, no address verifier was disabled, and no public callable
approval was invented. `tests/test_target_177.py` pins the full AP function bodies,
new constants, private records and unpromoted proof status.

The ARM-guarded host suite now contains 39 tests with distinct aligned/raw pixel
storage and a modeled display callback registry/frame pipeline. It covers
pre-draw ordering, visibility/cache reopen, receiver reconstruction/address reuse,
reload deduplication, system restoration, failed registration, deleted/replaced
or invalid displays, epochs, reentrant layout/setter reload, wake requests,
missed READY, GPU failure pins/retirement and exact cache-operation ordering.
These mocks do not prove real GPU execution or native ROM behavior.

Manager now rejects unknown outer `backgrounds` keys instead of silently ignoring
`control_center` or other typos. The manifest uses `controlCenter`; the generated
CFG uses `control_center`. Three-target blur round-trip and legacy compatibility
regressions pass. Manager typecheck/build passes; its unrelated pre-existing
layout assertion (`tests/test_manager_layout.cjs:76`, undefined versus 60px)
prevents claiming its complete npm test suite is green.

### User-reported device acceptance (2026-10-06)

After installing the current implementation, the user reported successful
hardware validation and requested a commit. This is functional acceptance reported
by the user, not an independently captured probe. No detailed scenario matrix,
GPU trace, fault injection or performance measurements were supplied. Record the
feature-level success without upgrading each private symbol to DEVICE_PROVEN or
public callable approval.

Independent GPU/cache-coherence probes and measured frame/memory cost remain
outstanding. Forced module unload, framework restart and recovery after a damaged
GPU/registry remain unsupported by the existing module contract.

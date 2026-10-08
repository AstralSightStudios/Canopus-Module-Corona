# p67 3.101.043 background adapter feasibility audit

## Status: ACTIVE module-owned static snapshot

Default builds retain disabled p67 backgrounds. Exact .043
`RH_P67_LIVE_BACKGROUNDS=1` automatically defines `RH_P67_BLUR_NATIVE=1`
for all four pages; launcher-only opt-in remains image-only. Resource mapping,
fonts and calendar are unchanged. The borrowed-pool design is superseded;
its restricted symbol/evidence records remain for existing dependencies, not
as permission to access source-pool userdata, cached flags or cross-view observers.

```sh
RH_P67_LIVE_BACKGROUNDS=1 sh scripts/build.sh xiaomi-band-10-pro-3.101.043
```

Existing version=1 configuration accepts `blur` for launcher, control_center,
notifications and widgets. No parser change or extra opt-in is required.
Capture is static and demand-driven at REFR_START (52): one prewarm after
activation and a fresh request only for a new page entry from its fully closed
endpoint, matching the Band11 entry-only policy. Exiting to the watchface, partial
reversal and release snap do not start another generation. Each allowed
request/UI-owner poll grants one consumable attempt. A failed attempt keeps pending work but
cannot request another whole-screen repaint every READY (53). The one-second
watcher rearms and schedules the next attempt; it performs no capture or GPU
fence before control-file processing. Successful output is not recaptured on
ordinary frames. Same-root reload preserves the previous immutable result. Restore system
while idle; finish gestures before reload; do not unload attached callbacks.

Input is a default drawbuf-create format15 BGR888 module-owned 336x480 buffer.
Output is format18 RGB565 at 84x120, at most two owned immutable output buffers.
All four pages share the processed result: 4x downsampling, two radius-2 separable
box passes and 30% darkening. Every new entry from the watchface requests a fresh
capture; exit and ordinary frames retain the published result with native 4x GPU
scaling. The
CPU never expands the output to 336x480. No native source-pool lease, alpha capture
or inline patch is introduced. Output retirement requires explicit
raw GPU completion and a task-free native layer chain; snapshot return status
alone is insufficient. Input allocation is bounded to one descriptor, even after
fence failure; normal capture frees it after copying. The filter scratch is
40,320 bytes, input pixels 483,840 bytes, and two output pixel buffers 40,320
bytes at default stride168, plus native descriptor/alignment overhead. This saves
604,800 output pixel bytes relative to the previous full-screen CPU upscale. Cache clean precedes the
large-range invalidator, like Band 11, to preserve unrelated dirty CPU writes.

### GPU-scaled blur output

The shared draw helper keeps static file backgrounds on the existing 188-byte
rectangle path. Blur uses image init `0x0C5882B9` (entity `0x1C0327D4`) and image
draw `0x0C5882A1` (entity `0x1C032B20`): the image descriptor is 112 bytes, source
at +28, scale_x/y at +48/+52, and pivot_x/y at +64/+68. Scale1024 is 4x; pivot
and rotation stay zero. Original image area is [0,0,83,119], not a full-screen
area scaled again. The existing page/screen/layer intersection is installed
before submission and restored afterward. Native tasks copy both the descriptor
and current clip; published source storage remains separately owned and immutable.
Draw does not capture, filter, allocate module buffers or change object visibility.

Firmware rectangle backgrounds only center or tile images; swapping their source
for a small image would not upscale it. VG-Lite image evaluator `0x1C045520`
accepts format18, and image renderer `0x1C046854` builds the transform and submits
GPU blit/pattern work. RGB565 maps to VG format5; the audited linear path permits
stride168 (four-byte alignment) and width84. Output validation checks exactly
84x120, format18, a four-byte-aligned stride and bounded storage without loosening
the full-screen snapshot input validator. Filter scratch is never used directly
as a queued GPU source. Fences, PM, cache maintenance and retirement are unchanged.

Static evidence: `four_way_draw.txt:1754-1768,1832-1843,1936-1947,2470-2530` and
`lvgl_thunks.txt:2530-2535,2554-2559` under the analysis directory below, plus
read-only IDA inspection of the exact saved AP database. The new calls are narrow
exact-target static-recovered evidence, not public approval or device acceptance.
No firmware ranges or source-pool permissions are broadened.

GPU linear filtering is not pixel-identical to the previous CPU's per-channel
truncation and edge replication. Hardware sampling phase, final row/column,
clipped reveals and frame time remain NOT_PROBED. Scaling now happens on each
background draw; reduced CPU preparation and memory do not guarantee a net GPU
frame-time improvement. Device comparison should cover gradients/edge colors,
all four directions, partial reveals, content scrolling, repeated gestures,
reversal/cancellation and system restoration. Host regressions cover the actual
image ABI/dispatch, clipping restoration, zero/stale sources, busy suppression,
immutable padded output and existing failed-fence lifecycle bounds.

### Entry-only generation, not every SCROLL_BEGIN

The earlier event-driven implementation incorrectly requested a new blur for exit
as well as entry. The intermediate input-session dedup only removed some release
snaps; it did not fix that semantic error. It is superseded by the page-entry policy,
without retaining the input-registry observer or parameter-pointer identities.

Read the validated binding's PAGE, not its content root/list, before forwarding
native code9. Exact closed endpoints and open position are:

| Surface | Page coordinate | Closed | Open |
|---|---|---:|---:|
| Launcher | signed page+32, y1 | 480 | 0 |
| Control Center | signed page+28, x1 | -336 | 0 |
| Notifications | signed page+32, y1 | -480 | 0 |
| Widgets | signed page+28, x1 | 336 | 0 |

Native callbacks read these coordinates at `0x0C517DBE`, `0x0C5036E0`,
`0x0C527BD8` and `0x0C50ABD4`. Finger begin at `0x1C0530B4` precedes movement
(`0x1C052F82 -> 0x1C02E410`); animated begin at `0x1C02D768/0x1C02D82C` also
precedes animation start. Therefore a code9 at the exact closed endpoint is a
new entry intent, whether its parameter is NULL or an animation template. Code9
at open0 is exit; at partial coordinates it is continuation/reversal, not another
new entry. Animation parameters need not be dereferenced or treated as direction.

Lock the entry as soon as it is accepted, even before output exists or publication
succeeds. A continuation keeps that original pending budget; it does not replenish
it. Returning fully closed rearms the next entry, including cancellation that
reaches closed on event12 without a final END. An unknown partial binding after
recreation fails closed rather than inferring entry merely from visible pixels.
Content-list-only replacement under an unchanged owner/page/root preserves the
entry session and pending retry budget: idle polling repairs list subscriptions
without detaching the stable page binding. Root/owner replacement still invalidates
that entry. Bubbled content scrolling at open0 does not cancel a page's pending
entry; only a native owner-target exit does.
Explicit activation/reload initialization retains the existing prewarm, so the
first transition does not lose its source to an opaque native snapshot.

A per-surface mask separates overlapping entries from exits. Once gesture direction
is managed, losing the last active entry cancels capture intent and invalidates
in-flight work, but never drops the published immutable image. START/owner poll
cannot rebuild or repeatedly wake for a pending invalidation while all entries
are closed/exiting. Disabled GPU cleanup remains independent. Native begin/end
bookkeeping, resource ownership, rendering isolation and GPU/PM protection are
unchanged. No new input callback seam or firmware permission is introduced.

`begin_skips` counts BEGIN events suppressed by the entry policy, not elapsed time
or GPU work. Like other counters it does not alone trigger diagnostic I/O. Host
regressions cover all four closed/open endpoints, NULL/animated begins, exit,
partial snap while pending/failed, reversal, closed cancellation without END,
new entry, overlapping bindings and lifecycle/configuration changes. Device
request counts and boundary frame time remain NOT_PROBED.

### Terminal-end visibility fast path

At a verified terminal end, a module-swapped pair already has live root visible
and native snapshot image hidden. Control Center (`0x0C5036C4`) and widgets
(`0x0C50ABB4`) restore flags only when root is hidden AND image is visible; their
buffer frees and remaining end bookkeeping are outside that predicate. The
adapter therefore leaves an already-final, validated pair unchanged and forwards
the entire native callback. It does not suppress release, write native buffer
slots or change begin capture behavior.

Notifications (`0x0C527BBC`) delete the image at either terminal endpoint. Its
DELETE callback `0x0C526B88` reads source, drops the decoder cache, returns the
pool buffer, clears native image/buffer slots and restores root independently of
HIDDEN. At y=0, pre-DELETE work queues callbacks and resets/resumes a timer. The
fast path additionally requires the synchronous timer-resume hook at `0x20103218`
to be NULL. A scheduling-only native hook was statically recovered, but its exact
identity is not allowlisted by the current framework pack; this change does not
embed that address, broaden permissions or bypass ELF verification. Every non-NULL
hook retains the original flags. At y=-480, synchronous systray callbacks can
observe state, so that endpoint also keeps the conservative restoration path.

Nonterminal end, reversal begin, inconsistent flags/pairs and detach also retain
the original restoration. Busy/generation guards remain in place. Native buffer
and pool ownership is unchanged; blur requests now follow entry-only semantics. Host tests
cover both endpoints, static/blur modes, native release exactly once, notification
DELETE, NULL/non-NULL resume hooks, pre-DELETE reentry, root DELETE and zero added
root-hide/image-show operations in the proven fast paths. This is static and host
validation only; device completion-frame latency remains NOT_PROBED.

### Exact-target static evidence gates

`EVID-P67-ACTIVE-STATIC-BLUR-001` records the narrow callable
`P67_BLUR_SNAPSHOT_TO_BUFFER` at `0x0CAC0879`, prototype
`int snapshot_to_buffer(uint32_t root, uint32_t format, uint32_t owned_drawbuf)`.
The thunk reaches native `lv_snapshot_take_to_draw_buf` at `0x1C06030C`.
Cover search `0x002BB46C` returns 0 for a HIDDEN root; fallback direct rendering
`0x002BA788` ignores root HIDDEN while children respect visibility.
Success is 1. Firmware task-driver/GPU errors are not propagated by the integer
return: an explicit raw GPU fence is required before CPU reads/retirement.

REFR_START52 emits at `0x002BB8A2` before the display+58 decimal (`+0x3A`) rendering mask `0x02`
is set at `0x002BBA20`. REFR_READY53 emits at `0x002BBC98` after that mask
is cleared at `0x002BBC3E`. Prior raw GPU/cache/create/destroy records remain
restricted; cache ABI is `(id,address,length)`, id1 Dcache, with module barriers.
These static findings do not prove hardware coherency.

The stock `miwear_snapshot` wrapper at `0x0CA5FB40` also calls PM stay
`0x0C1A78F1` before snapshot and relax `0x0C1A79B5` afterward, with the native
116-byte `miwear-snapshot` lock at `0x200EB538` (domain0/state0). Active COPY
keeps that balanced power contract through capture/fencing. Module-owned nesting
now calls native stay/relax only on its outermost entry/exit: a successful COPY
still performs all three explicit fences, but those nested fences no longer
re-enter native PM/governor callbacks. A standalone maintenance fence still
acquires its own balanced protection. Native lock fields are never modified
directly; diagnostic PM counters count actual native API calls.
Both PM APIs may invoke governor/driver callbacks and are treated as blocking.
The missing guard was statically confirmed, but its role in device symptoms is
not established.

Snapshot, generic event-list, and PM stay/relax/lock records are
restricted `STATIC_RECOVERED`, approval `PENDING`, device `not_probed`.
Static audit: `/Users/lesetong/develop/mifw/analysis-p67-043/ACTIVE_BLUR_AUDIT.md`.
Official generators produce C/Rust bindings; no firmware ranges are broadened
and no public approval is implied. The full frozen host suite and exact .043
ELF verifier passed. ASan/UBSan covers the actual provider plus core together:
owned input, static reuse, failed-fence bounds, old-result preservation,
next-frame recovery and disabling with pinned storage. Separate core tests cover
wake return convention, reentry, root/display generations and stale publication.
Device rendering, GPU/cache recovery, retirement, latency and frame time remain
**NOT_PROBED**. Earlier image-background feedback is not blur acceptance.
Device feedback for the earlier active package: black/system backgrounds and
approximately25-second reload response. This is a failed device report, not
functional blur acceptance. Subsequent user feedback reports working backgrounds
and blur on the CPU-upscale revision, with frame drops at gesture start and
animation completion. Later user feedback reports a large performance improvement
with GPU-scaled output and terminal-end optimization, but residual blur-only stalls
on all four pages at gesture start and finger release; static backgrounds are
reported substantially smoother. These are uninstrumented observations, not
stage timings or exhaustive acceptance. Further request/PM changes still require
device verification.
Existing older installation outputs are archived under
`dist/archive/`. Do not hot-unload callbacks; restore system while idle and reboot
when replacing the module. The verified ELF is `build/resource-hook.elf`.

### Diagnostic build and device probe

The native .043 build records memory-only stage/counter data. Only the one-second
UI-owner watcher writes `/data/offlinelog/resource-hook-p67-blur.log`, after the
control transaction. RHST1/RHRS2/Manager receipt fields are unchanged. Logs dedup
stable state, exclude counters, and limit recurring errors to eight attempts per
config epoch. Lifecycle/publication progress remains reportable. A failed
truncating write can temporarily lose a complete snapshot; exhausted-budget
repair retries occur once per32 watcher polls. Only a file ending with
`snapshot_complete=1` is a complete observation. No file I/O occurs in DRAW or
refresh callbacks, and no pixels, filenames or key material are logged.

After installing/rebooting the diagnostic package, enable blur, reload once,
open a page, then export the AstroBox offline logs. Keep the complete P67 log.
Compare a system-background reload while idle if possible; do not repeatedly
press reload against a stalled device.

Key interpretation:

- `core_gate`:0 idle,1 unsupported,2 disabled/cleanup,3 owner unsafe,4 display
  invalid,5 root unregistered,6 identity mismatch,7 COPY failed,8 publication
  failed,9 stale lifecycle,10 completed.
- `idle_error`:1 invalid display range,2 rendering,3 invalid layer,4 queued
  native tasks,5 unbounded/cyclic layers,6 display/render phase changed.
- `native_error`:1 no root,2 unsafe phase,3 pinned input,4 pre-fence,
  5 input allocation,6 input descriptor,7 snapshot return,8 post-fence,
  9 post-capture phase,10 root changed,11 reshaped descriptor,12 downsample,
  13 output slots pinned,14 output allocation,15 output descriptor,16 output copy,
  17 invalid/reentrant arguments;0 means no latched native failure.
- `width`/`height`/`stride` describe the last validated owned descriptor, which
  can be full-screen input (336x480) or low-resolution output (84x120).
- `nonzero` counts nonblack pixels in the last successfully downsampled input,
  before blur/darkening; it is not a raster acceptance test.
- `publishes`/`source` distinguish missing output from a black captured image.
  `draws` counts submitted module GPU-scaled blur-image tasks, not GPU pixels.
- `pm_stays`/`pm_relaxes`, `capture_result`, `last_gpu`, PM returns and owned-input
  state identify native progress/failure. Native integer fields are32-bit words
  (values>=2147483648 can represent negative returns); module errors are signed.
- `refresh_pending`, cache/font/calendar/image completion flags, font/calendar
  errors and `background_error` identify a slow reload's coordinator stage.

A source with nonzero input and zero draws points toward attachment/visibility;
zero publications points toward preparation/publication; all-black input after
native success still requires checking custom watchfaces and cache coherence.
Do not infer the device cause from host fixtures.

## Historical launcher-only experiment

## Experimental launcher test build and use

Build only for the fingerprinted .043 pack:

```sh
RH_P67_LIVE_LAUNCHER=1 sh scripts/build.sh xiaomi-band-10-pro-3.101.043
RH_P67_LIVE_LAUNCHER=1 sh scripts/build-install-payload.sh \
  xiaomi-band-10-pro-3.101.043 /tmp/p67-live-launcher-payload
```

The second command signs the ELF using the existing trusted local installer key;
installation remains manual via the normal Module Installer workflow. Do not use
an unverified ELF or bypass the framework address allowlist. The exact framework
pack records the independently reviewed launcher call entries as restricted
static-recovered evidence, not device acceptance. Signing requires the existing
trusted local key; no key is generated by this experiment.

Select a **static launcher** image in Manager, leaving Control Center and
notifications on system (widgets are ignored), then activate/reload normally.
No parser extension is required. To restore native launcher behavior, select
system and reload while the launcher is idle; an in-progress swipe or existing
native snapshot rejects the transaction with `-2145`, so finish the swipe and
retry. Do not replace/unload the resident module while its callbacks are active.
The experiment does not invoke alpha captures or alter native pool buffers.
All launcher layouts, editing mode, partial reveal, reversal/cancellation, and
frame-time acceptance still require device testing. No device test or flashing
was performed.

## Reproducible evidence

Read-only headless IDA 9.4/Hex-Rays analysis used the saved
`analysis-p67-043/vela_ap.bin.i64`, with the executable aliases described in
`PLAN.md`. Scripts and decompiler dumps are outside this repository under
`/Users/lesetong/develop/mifw/analysis-p67-043/`:

- `audit_four_way.py` / `four_way_audit.txt`: page creation/movement and snapshots.
- `audit_four_way_abi.py` / `four_way_abi.txt`: events, allocation and layer setup.
- `audit_four_way_deep.py` / `four_way_deep.txt`: style setters and pool ownership.
- `audit_four_way_draw.py` / `four_way_draw.txt`: draw descriptors and event removal.
- `audit_four_way_formats.py` / `four_way_formats.txt`: snapshot formats/notifications.
- `audit_four_way_final.py` / `four_way_final.txt`: stride, bit-depth and event access.
- `audit_launcher_live.py` / `launcher_live_{audit,event_fields,clip}.txt`:
  launcher binding, USER DATA getter, masked codes, rectangle and redraw clipping.

Launcher live evidence: C588310→002BFD88 reads event user data at +12,
not either target field (+0/+4). Native scroll y is page(user data)+32.
Redraw at 002BA788 installs layer+24 clipping against the object's extended draw
area before events 23/24/25; the experiment additionally intersects screen and
page coordinate bounds (+28..40), restoring the original layer clip after draw.
`tests/p67_launcher_harness.c` exercises the actual included adapter with injected
native dispatch/read seams under ASan/UBSan; this is a host lifecycle model, not
device acceptance.

AP image SHA256:
`519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec`.
Addresses below are even instruction addresses or data slots, not an enabled
callable hook table. Thumb calls need bit zero set. Tail-merged decompiler
signatures were checked against disassembly rather than treated as proof.

## Verified differences from Band11

- Rectangle descriptor init: thunk `0x0C588268` to `0x1C034324` clears **188
  bytes**, not Band11's 116. Rectangle submission: thunk `0x0C588350` to
  `0x1C034400`; image source is read at descriptor **+120**, not +48.
  Init establishes image opacity at +131. Copying the Band11 descriptor/layout
  would corrupt stack storage and submit incorrect fields.
- Background color: `0x0C588CB8` to `0x002BE4DC`, passing property **28** to
  the setter at `0x002BD924`. Background opacity: `0x0C5883F8` to
  `0x002BE508`, property **29**. Setter entry disassembly takes object r0,
  property r1, value r2, selector r3. Style restoration still needs a verified
  getter/local-style-removal contract, not just saving a resolved value.
- Object event add: `0x0C5883F0` to `0x002BF7AC`; event list lives at
  object.special(+8)+12. Event descriptors are 12 bytes: callback +0,
  user data +4, filter +8 (`0x002BFC24`). Callback removal
  `0x0C589570` to `0x002BF810` removes the **first** matching callback;
  `0x002BF860` matches callback and user data, removing all matches.
- Event getters at `0x002BFD44`, `0x002BFD54`, `0x002BFD64`,
  `0x002BFD78`, `0x002BFD88` read +0, +4, masked 16-bit code +8,
  parameter +16, user data +12 respectively. Native page callbacks directly
  read code +8. Delete callbacks are registered with filter 36.
- Snapshot layer init `0x1C030DE4` clears **120 bytes**. Capture installs this
  layer in display+680 temporarily and restores the prior pointer afterwards
  (`0x1C06030C`). That substitution is an observation, not a verified global
  capture flag: ordinary nested layers must not be misidentified as capture.
- Format bit depth uses `0x1C05C0D8` and table `0x2CCE5814` indexed by
  format minus 6. Firmware bytes show format **15 = 24 bpp**, **16 = 32 bpp**.
  Alpha predicate `0x1C05C0FC` uses `(0xF97F80 >> format) & 1`: **15 has
  no alpha; 16 has alpha**. Snapshot-supported mask `0xEFC040` in
  `0x1C06030C` permits both. Format 16 premultiplication handles four-byte
  pixels with alpha at byte 3 (`0x1C031A9C`). This proves an alpha-capable
  candidate, not correct capture/compositing on this device.
- Default stride callback `0x1C031444` computes ceil(width * bpp / 8).
  Format 15 at 336x480 uses 483840 bytes; format 16 uses 645120 bytes before
  allocator padding/scratch. Default handler installation is at
  `0x0C13E8B4` / `0x1C031474`; BSS handler slots have no valid on-disk runtime
  pointer values and must not be read from the IDB as initialized data.

## Snapshot paths and the blocking contract

All four page paths use non-alpha format 15:

- Widgets: `0x0C50A838` caches a snapshot immediately on creation; movement
  `0x0C50ABB4` swaps root `0x201100E0` with image `0x201100CC` on event 9,
  reuses the first-entry cache, and releases buffer `0x201100D0` on event 11.
  Full root/header ownership is required, not just list `0x201100DC`.
- Control Center: `0x0C5036C4` captures root `0x2010FE50`, publishes image
  `0x2010FE64`, owns buffer `0x2010FE68`, and hides the live foreground during
  horizontal movement. All backgrounds must use screen coordinates, including
  this page; the original container-relative suggestion is superseded.
- Launcher: `0x0C5176FC` obtains a pooled descriptor using `0x0C4B6AA4`,
  calls `0x0CA5FB40` with format 15, creates image `0x201102DC`, and hides
  foreground `0x201102F4`. Its image DELETE callback `0x0C517BBC` returns
  ownership to the pool rather than destroying a module-owned buffer.
- Notifications: movement `0x0C527BBC` invokes `0x0C527640`, which also
  obtains a pooled descriptor and captures format 15. Image `0x20110ED4`
  is deleted at movement end. This is not a live-only notification surface.

The pool selector `0x0C4B6AA4` uses entries at `0x2010BE8C`, 80-byte spacing,
with busy bit +76. Pool release `0x0C4B6B34` only clears that busy bit for a
recognized descriptor. These descriptors must never be resized, reformatted,
freed, or repurposed using inferred module ownership.

An image drawn into these opaque native captures will move as pixels. Suppressing
wallpaper during capture alone leaves opaque structural fill, so wallpaper behind
the moving image cannot show through. Changing only the snapshot format argument
is also unsafe for the pooled launcher/notification descriptors and their native
release callbacks.

A safe adapter needs separately allocated alpha foreground captures, native
publication/release integration, target/object-scoped reentrant capture suppression,
structural-fill restoration, and a background owner visible in both live and
snapshot mode with page-viewport clipping. It must handle creation-time caches,
rebuilds, delete/address reuse, reload cancellation and GPU-retired sources.
Event/style/draw primitives alone do not establish this lifecycle. No speculative
poller dereferences these slots, no native callback is replaced, and no opaque
snapshot is painted over. A live-foreground alternative would require independent
animation and frame-time validation, not an automatic fallback.

Remaining verification before enabling the independent alpha-snapshot path: complete display-event/remove ABI and
live-tree validation; rectangle submission/GPU source retirement; foreground alpha
capture and native snapshot replacement/cleanup; local-style restoration; all
launcher layouts and notification branches; device partial reveal, reversal,
cancellation, page/list scroll, reload and restore-to-system acceptance.

## Four-surface live extension

`RH_P67_LIVE_BACKGROUNDS=1 sh scripts/build.sh xiaomi-band-10-pro-3.101.043`
selects `src/p67_backgrounds.inc`; the old launcher flag remains launcher-only.
Static control_center, notifications and widgets now retain native begin/end
callbacks and their caches, timers and pool cleanup. Native snapshot images are
hidden while their live full-page root is shown; prior native visibility is
restored around audited scroll lifecycle callbacks only. Other ALL-filter events
are forwarded without adapter visibility or callback-registration changes. No pool
or buffer is modified. Wallpaper destinations remain screen-fixed with explicit page clips.
Full-page roots and widget/notification lists receive foreground draw handlers.
Band11 widget behavior and calendar behavior are unchanged. This historical
image-only extension predates the static-blur integration described above.
This retains the cost of opaque native captures, but does not display them.
Idle system restoration removes callbacks; active transitions reject reload.
Device rendering and frame-time acceptance for the added surfaces is NOT_PROBED.
The original four-way live test package was user-reported to reboot when revealing
new pages; it must not be treated as an accepted build. See the rendering-boundary
regression below.

Integration/regressions: the shared module gate recognizes the four-way flag,
with `build/test_module_1043_four --launcher-background` exercising startup,
four-key commits, polling and transactional failure/restore behavior under
ASan/UBSan. Native mocks also cover intermediate nonterminal end/reversal,
widget first-entry cache reuse, capture failure, populated-list DELETE/recreation,
shared scroll owners, registration failure and reentrant flag-operation DELETE.
Intermediate ends retain the transition lock until native terminal coordinates;
late install/commit rejects visible snapshots, hidden roots or nonterminal pages.
Each surface has distinct callback identities and globally unique event tokens,
so a shared native scroll owner can be detached without removing another page.

### Rendering-boundary reboot regression

The first four-way package inherited native ALL-filter scroll subscriptions but
restored/swapped root and snapshot visibility on every delivered event. An owner
DRAW_MAIN_BEGIN/MAIN/END (23/24/25) could therefore invalidate objects during
rendering. Exact firmware `_lv_inv_area` at `0x002BB2F8` (XIP copy `0x0C105164`)
asserts `!disp->rendering_in_progress` with "Invalidate area is not allowed during
rendering." The supplied device log records board_crashdump and RTC watchdog reset,
but lacks the incident PC/assertion text; this is a confirmed forbidden code path
and a strong incident candidate, not a captured fault-PC attribution.

The rendering-boundary fix originally restricted visibility restoration, listener
refresh and live-mode swapping to audited gesture events 9/11/12, with notification
12 forwarded directly. The steady-scroll optimization below now restricts these
adapter side effects to begin/end 9/11 for every surface. All other events are forwarded
without these adapter side effects; native descriptor filters and userdata remain
unchanged. Native transition-end cleanup and pooled-buffer ownership are preserved.
The accepted launcher path is unchanged.

The host regression dispatches owner ALL subscriptions with native page userdata
while a live transition is active, then dispatches root draw handlers with adapter
generation userdata. Flag and callback-registration mocks reject mutation during
these phases, including ordinary/layout notifications. The regression fails with
the pre-fix adapter and passes with the event gate under ASan/UBSan. This models
the missing event route; it does not replace device rendering or frame-time tests.

### Notification continuous-scroll optimization

The rendering-boundary fix was user-reported to render normally on hardware, with
notification sliding noticeably less smooth than the other pages. The exact
notification native callback `0x0C527BBC` handles event 12 by updating the direction
icon; capture occurs on event 9 and retirement on event 11. It does not require
restoring the snapshot/root visibility for event 12.

The initial optimization made only notification event 12 take the native-only
forwarding path. This removes
four adapter visibility changes and approximately 25 separate membership tree walks
per steady, successfully swapped event in the old path. These are static operation
counts, not a measured frame-time speedup. Other surfaces, begin/end bookkeeping,
render-event isolation and background draw handlers remain unchanged. Changed-list
listeners are still maintained by the existing UI-owner polling path.

The regression dispatches 128 owner ALL event-12 notifications with page userdata
while the notification transition is live, requiring native forwarding, zero added
tree walks, no flag/registration changes and unchanged capture/cleanup counts.
The pre-optimization path fails this guard; the optimized path passes ASan/UBSan.
At that revision Control Center/widgets retained their event-12 transitions.
The notification-optimized package was subsequently user-reported to perform well
on hardware. This is USER_REPORTED_PASS for the reported scenario, not instrumented
FPS evidence or exhaustive lifecycle/frame-time acceptance; no FPS guarantee is made.

### Static-background steady-scroll and draw-path optimization

All three surface wrappers now forward SCROLL12 directly, regardless of static
image or blur configuration. Exact CC `0x0C5036C4` and widget `0x0C50ABB4`
callbacks have no event12 action (`four_way_audit.txt:389-531,686-814`); notification
12 retains its native direction-icon updates. This removes approximately four
visibility changes and 25 global membership queries per successfully swapped
static CC/widget SCROLL event. Begin/end, first-entry widget caches, native captures,
release/pool ownership and conservative nonterminal/reversal handling are unchanged.
Changed-list subscriptions remain repaired by UI-owner polling, not SCROLL callbacks.

Surface DRAW_MAIN_END25 no longer searches the global object tree for the retained
page. After current-target, generation, busy and root-slot gates, it follows the
native dispatch-owned object's parents to the installed root (at most 16 nodes),
then requires root->page->owner identity before reading viewport coordinates.
A root draw uses two read-only parent getters; a direct list child uses three.
It never dereferences a retained page unless that pointer was obtained as the
current root's attached parent. Reparented/detached targets, stale generations,
root-slot replacement and DELETE-invalidated bindings fail closed. List-only DELETE
still preserves the page generation, while clearing list identity until polling
repairs the listener, including same-address recreation.

This is dispatch-scoped lifetime/ancestry validation, not cached global registry
membership: a native draw of an allocated off-tree hierarchy with the correct
ancestor identities can draw its background. Native dispatch must own the current
target's lifetime, and attached parents must remain alive for these read-only
getters on the serialized UI owner. No parent query is made from an arbitrary
retained pointer. Mutation-capable begin/end/install/detach/repair paths retain
full membership checks; the launcher draw path and firmware permissions are unchanged.
Page/screen/layer clipping and clip restoration are unchanged. No draw-time flag,
listener, capture, allocation or source-retirement changes are introduced.

ASan/UBSan regressions require zero tree/parent queries, visibility changes or
listener mutations across 128 steady events for static/blur on every surface.
They also cover list appearance/DELETE/reuse with poll-only repair, repeated root
and list draws with zero global walks, stale/dead retained pages, owner/root DELETE,
root address reuse, list reparent/detach, off-tree allocated ancestry, cyclic/deep
parent chains and the 16-node boundary. These are host operation/lifecycle checks;
physical-device frame time for this revision remains NOT_PROBED.

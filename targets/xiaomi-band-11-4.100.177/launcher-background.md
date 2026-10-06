# Experimental UI backgrounds: launcher, control center, notifications (.177)

This adapter is STATIC_RECOVERED / HOST_MODEL_PASS, with user-reported device
acceptance on 2026-10-06. The user reported successful hardware validation of the
current background implementation; no detailed procedure, captured trace or frame
measurements were supplied. Independent GPU/cache fault probes remain NOT_PROBED,
and the restricted ABI records retain approval PENDING.
Only Xiaomi Band 11 4.100.177 is enabled. The AP SHA256 remains
`ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b`.
No firmware patch, class-table write, inline hook or extra widget is installed.
One native display event subscription owns background attachment and refresh.

## Static recovery

- **Launcher**:
  - `0x0c547d84` contains launcher main-page creation and publishes the page root
    at `0x200c5f10`; detached-list cache root is `0x200c5f0c`.
  - `0x0c5489fc` publishes the more-page root at `0x200c5adc`; cache root is `0x200c5ad8`.
  - List objects instantiate exact class `0x2ca435bc`.
- **Control Center**:
  - `0x0c54a3cc` creates control center within the cross-view tileview.
  - Presenter structure lives at `0x200c5668`; root page is `load32(0x200c5668 + 8)`.
  - Container is child 0 (`&off_2CA3BAD4`, class `0x2ca3bad4`).
- **Notification Center**:
  - `0x0c55c54c` (`ntf_create_main_page`) creates notifications view under `0x200c5ee4`.
  - Container slot is `0x200c633c` (`&off_2CA3BAD4`, class `0x2ca3bad4`).
- `0x0c383434` sets local style property (ABI: `obj, prop, value, selector`).
- `0x0c386de2` reads property 40 into the native rectangle draw descriptor's
  background image source at +48. Effective-style getter is `0x0c380164`.

This adapter assigns native main-part background-image property 40 to the exact
launcher list, control center container, and notification container. Native
background drawing remains behind the existing icons and widgets.

## Screen-fixed background experiment

The user reported successful device validation of this rendering path after
Control Center was restored to container-relative rendering. No captured trace,
detailed test procedure or frame measurements were supplied; independent GPU
fault, coherence and performance probes remain NOT_PROBED.
Launcher and notification receivers register one native object callback each.
Control Center retains the original container-relative property-40 background
following user feedback; it does not register a fixed-background callback. Its module-owned image
source is retained separately while native property 40 is suppressed. Event 25
(DRAW_MAIN_END) submits a native rectangle background image at global screen area
`[0, 0, 211, 519]`, after the object's fill and before native child traversal.
Native object/layer clipping limits the visible panel; the GPU handles layer-origin
translation. No extra widget, class patch, rotation or scaling compensation is added.
Notification container/list both use the same screen anchor so list fill cannot hide
the replacement. Use opaque images for this experiment: transparent file images
can composite twice where the notification container and list overlap. RGB565 blur
buffers are opaque. Bubbled child events are ignored by comparing original and
current event targets. Failed registration leaves native styles intact and retries
on a later refresh; reload acceptance is not synchronous attachment confirmation. DELETE drops callback identity before object storage reuse, and
system restoration removes the callback and restores the saved native image source.

Check all three panels, empty/nonempty notifications, interrupted/reversed swipes,
launcher more-apps and cached reattachment. Landmarks must remain screen-stationary
with no trails, black strips or wallpaper outside the moving panel boundary.

## Configuration

Authoritative package declaration in `corona.json`:

```json
"backgrounds": {
  "launcher": "backgrounds/launcher.bin",
  "controlCenter": "backgrounds/control.bin",
  "notifications": "backgrounds/notifications.bin"
}
```

Or with explicit `{ "mode": "system" }` / `{ "mode": "blur" }` / `{ "mode": "image", "image": "..." }`.
Convenient string shorthands: `"blur"` or `"system"` are also accepted.
Legacy `"launcherBackground": { "mode": "image", "image": "..." }` is also accepted.
Omission inherits. The first declaration above the system-style divider wins per target.
Manager generates separate `internal://files/backgrounds.cfg`:

```text
version=1
launcher=blur
control_center=themes/dark/backgrounds/control.bin
notifications=system
```

When set to `blur`, the module takes a live watchface snapshot, performs an in-memory
4x downsample (53x130 RGB565) and 2-pass separable box blur with 30% darkening, and upscales
it to a persistent 212x520 in-memory `lv_draw_buf_t`. Property 40 draws directly from RAM
without background-generation filesystem I/O. The native aligned pixel pointer
is at +16; +20 holds the allocation base for freeing and must not be used for
pixel reads/writes. CPU sampling and GPU drawing now use the same +16 storage.

Legacy `internal://files/launcher-background.cfg` is also read as a fallback.

Image mode uses a native supported LVGL image, preferably 212 x 520 for this
target, without runtime resizing or effects. Blur mode uses the fixed RAM kernel
above; no extra widget or animation is introduced. Native header
preflight runs once per activation; dimensions are not scanned every frame. A plain
I8 diagnostic image is about 111 KiB on disk; decode/cache/GPU memory and rendering
cost depend on the native graphics pipeline and are not yet measured on device.

## Lifecycle and performance

The existing 1000 ms UI-owner watcher still loads configuration and maintains the
subscription if a display appears late or changes. It does not discover panel
visibility, generate blur or attach backgrounds. No new timer is created. Fault
maintenance may finish/release a snapshot pinned by a previous GPU error, even
when all backgrounds are system or no panel is visible.

A single ALL|PREPROCESS display callback handles REFR_START (52), REFR_READY (53)
and DELETE (36). REFR_START settles the same five screen layouts as the native
refresh timer, then rediscovers all seven exact receivers. The native visible-area
consumer checks hidden ancestors, active/previous screen membership and clipped
screen intersection, rather than testing whether a cached object pointer exists.
An offscreen-to-visible transition, rebuilt receiver/native style reset or reload
marks blur dirty. Unchanged visible receivers do not capture again while scrolling.
Background sources are attached before the native object drawing phase, not in a
late DRAW_MAIN callback. New subscription/reload explicitly invalidates the display
so an idle refresh timer wakes. Failed visible generation requests another frame
at REFR_READY, after native invalid-area cleanup.

An allocated buffer is hidden until a full RGB565 frame has been generated.
Allocation/snapshot/metadata failure leaves it unpublished and retryable. Failed
refresh and blur-to-blur reload preserve last-good pixels and styles. VG-Lite
finish is checked before mutation and after snapshot rendering. If the second
fence fails, the snapshot stays pinned until a successful later fence; it is not
released while GPU ownership is unresolved. D-cache synchronization/invalidation
and memory barriers surround CPU reads and publication. The buffer remains
module-resident for reuse rather than being freed while unknown native cache
references might retain it.

Repeated reload does not add callbacks. All-system commit removes our display
callback; DELETE clears ownership before native event storage is freed. Saved
display identities are dereferenced only after bounded live-registry validation.
Frame epochs are rechecked after event-dispatching native operations; interrupted
frames cannot apply stale objects or clear dirty state. Reentrant configuration
changes defer their wake request to REFR_READY. Invalid registry/removal ownership
fails closed rather than adding duplicate subscriptions.

These contracts have static/host validation and user-reported functional device
acceptance, not independent GPU fault/coherence or performance verification. In
particular, the cache invalidation thunk enters ROM outside the AP image; its
caller ABI is audited, not its ROM body. Framework restart/GPU recovery remain
unsupported. Reload acceptance means configuration was accepted, not that a
snapshot succeeded or a panel displayed it. Do not claim zero latency, measured
frame cost or frame rate. See `blur-static-diagnosis.md` for recovered addresses
and the corrected buffer-layout evidence.

Two module-resident path buffers keep style source storage resident. Only our own
path pointers or blur-buffer pointer are cleared when returning to system mode. Exact old/new filename
cache retirement happens on reload, using the existing native refcounted cache
adapter; unrelated keys are not touched. Same-filename revisions retire the candidate
key before preflight to avoid trusting stale header metadata.

Both declarations are parsed/preflighted before new mappings are published. Manager
writes background then mappings and sends the existing reload request only after
both writes succeed. This is not a crash-atomic two-file transaction. A native cache
retirement failure during background commit can occur after mappings publication;
it reports failure and package dependencies remain protected. Do not claim full
cross-feature rollback or GPU asynchronous lifetime safety.

Error receipts/status use -2142 (configuration/read failure), -2143 (native image
header rejection), -2145 (cache retirement failure). Stage rejection retains the
previous configuration. Background commit failure can leave the native background
restored. Non-.177 modules do not consume this experimental configuration yet.

## Device test checklist

1. Install the new .177 module and Manager build; load a background-only resource
   pack (`mappings: []`). Put it above system style and reload.
2. Check single/double-column launcher, scrolling, more page, app launch/return,
   repeated page recreation, lock/unlock, editing/reordering and fonts/icons.
3. Move a system-background pack above it, or move the background pack below the
   divider; reload and check both live/cached lists return to native appearance.
4. Reorder two background packages and repeat reload. Do not modify active files
   in place or force-release native GPU caches.
5. Record rendering, lag, memory behavior and error receipts separately. A host
   test/build pass is not hardware acceptance. Framework restart and GPU recovery
   retain the existing unsupported status.

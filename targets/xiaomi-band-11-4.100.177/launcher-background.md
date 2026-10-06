# Experimental launcher background (.177)

This new adapter is STATIC_RECOVERED / HOST_MODEL_PASS / NOT_PROBED on hardware.
Only Xiaomi Band 11 4.100.177 is enabled. The AP SHA256 remains
`ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b`.
No firmware patch, class-table write, inline hook or extra widget is installed.

## Static recovery

- `0x0c547d84` contains launcher main-page creation and publishes the page root
  at `0x200c5f10`; its detached-list cache root is `0x200c5f0c`.
- `0x0c5489fc` publishes the more-page root at `0x200c5adc`; its cache root is
  `0x200c5ad8`.
- `0x0c547a40` / `0x0c548784` create the first list child through `0x0c542384`.
  That helper instantiates exact class `0x2ca435bc`, whose constructor is
  `0x0c542744`. No background image source is assigned on these paths.
- `0x0c5486ac` / `0x0c548b74` clear the live root slot and reparent the first
  list child into the corresponding cache root. The adapter therefore checks
  both live and cached roots, without retaining/dereferencing deleted objects.
- `0x0c381090` retrieves child index zero. `0x0c383434` sets a local style
  property with ABI `(object, property, value, selector)`.
- `0x0c386de2` reads property 40 into the native rectangle draw descriptor's
  background image source at +48; property 41 supplies background-image opacity.
  The existing audited effective-style getter is `0x0c380164`.

Rather than inserting a background child and risking launcher child[0] assumptions,
this adapter assigns native main-part background-image property 40 to the exact
launcher list. Native background drawing remains behind the existing icons;
child order, layout, hit testing and scroll callbacks are unchanged. The intended
result is a fixed centered background. Actual scroll rendering, list editing,
main/more transitions and cached-list restoration still require hardware testing.

## Configuration

Authoritative package declaration in `corona.json`:

```json
"launcherBackground": { "mode": "image", "image": "backgrounds/launcher.bin" }
```

Use `{ "mode": "system" }` to mask lower-priority background packages. Omission
inherits. The first declaration above the system-style divider wins as a whole.
The image must exist in the package and use a safe relative path. Manager keeps
background package dependencies protected until a successful reload acknowledgement.

Manager generates separate `internal://files/launcher-background.cfg`:

```text
version=1
mode=image
image=themes/dark/backgrounds/launcher.bin
```

System mode is exactly `version=1\nmode=system\n`; a missing file also means system.
The format has strict ordering and a trailing newline. No entry is added to
`mappings.tsv`. Module-side path parsing is bounded and performed only at initial
watcher activation or an explicit reload revision.

Use a native supported LVGL image, preferably 212 x 520 for this target. No runtime
resizing, effects, transparency controls or animation are introduced. Native header
preflight runs once per activation; dimensions are not scanned every frame. A plain
I8 diagnostic image is about 111 KiB on disk; decode/cache/GPU memory and rendering
cost depend on the native graphics pipeline and are not yet measured on device.

## Lifecycle and performance

The existing 1000 ms UI-owner watcher is reused; no new timer is created. With
system mode, polling returns immediately. With image mode, a tick examines at most
four root slots, child[0] classes and effective source pointers. A style setter is
called only when the source is different (e.g. a newly rebuilt list). There is no
file I/O, image preflight, tree traversal or cache validation in background polling.
A newly created page may show its native background for up to one watcher interval.

Two module-resident path buffers keep style source storage resident. Only our own
path pointers are cleared when returning to system mode. Exact old/new filename
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

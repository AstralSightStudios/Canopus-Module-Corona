# Control Center battery transparency (.043 only)

AP SHA256: `519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec`.

## Rendering contract

`src/p67_battery.inc` is included only by the exact .043 ARM background
adapter. It is enabled with a successfully bound custom Control Center
background (both static images and static blur). It does not alter battery
resources or other battery instances.

```text
Native battery (white backing -> fill -> black cutout / charging icon)
                    |
          idle-only ARGB8888 snapshot
                    |
          drain LVGL tasks + GPU fence
                    |
          invalidate -> #000 alpha=0 -> clean
                    |
   immutable image task at current object coordinates + current layer clip
```

This is not wallpaper restoration. Scroll/page transforms and clipping stay
with the native image task, and the cutout has already removed square white
corners before the black pixels become transparent. No display framebuffer is
read or written by the adapter.

The literal key removes **all exact RGB black** pixels in this battery-only
image, including intentional internal black pixels. Near-black / gray pixels
are retained; this is not an antialiasing reconstruction and may retain a dark
edge on a light wallpaper. No color threshold is applied, since it would erase
valid dark content. White backing, colored fill and nonblack alpha are retained.

## Exact recovered ABI

| Address | Role |
| --- | --- |
| `0x2CD198DC` | Native `LvxBattery` class, 36 bytes, instance size 104 |
| `0x0C4C22CD` | Class event callback `(class,event)` |
| `0x0C4C1D69` | Battery drawing helper `(event)` |
| `0x2010FE50` | Current Control Center foreground root slot |
| `0x2010FE60` | Status-bar parent slot (also mirrored at `0x2010FE74`) |
| `0x0C5881D9` / `0x0C5886E1` | Child count / child getter |
| `0x0C589579` | Parent getter |
| `0x0C587E71` | Extended draw size getter |
| `0x0C587F39` | Native draw-buffer creation `(width,height,format,stride)` |
| `0x0CA5FB41` | Native PM-bracketed snapshot-to-draw-buffer wrapper |
| `0x0C187BB1` | GPU completion, zero means success |
| `0x0C588F89` / `0x0C588449` | CPU cache clean / invalidate |
| `0x0CAC0DB9` | Draw-buffer destruction |
| `0x0C5882B9` / `0x0C5882A1` | 112-byte image descriptor init / image submission |

Native Control Center construction at `0x0C504C20`, battery creation at
`0x0C504D28`, establishes the battery as a direct child of the status bar.
Discovery requires the current live parent chain, exact class and exactly one
matching child among at most 16 children; it never assumes a fixed child index.

The RAM class is a **derived shim**, not a second instance: base at +0 points
to the original class, +4 constructor and +8 destructor are null, +12 is our
event callback, remaining fields are copied. The destructor walker at
`0x1C02917C` visits each base, so copying the native destructor would execute it
twice. Type checks against the native battery class still succeed through the
base chain. Do not instantiate this shim.

Only `DRAW_MAIN` (24) with a valid immutable image replaces native drawing.
All other events, capture-time drawing and failures call the original callback
with the original class argument. `DELETE` (36) clears retained identities
without walking the deleting tree, changing its class, fencing or freeing.
Idle detach restores the original class only on a live object still carrying
our shim.

A preprocessing listener with filter `0x8000 | DRAW_MAIN` is **not** an
alternative: dispatcher `0x002BF690` continues to the class callback at
`0x002BF710` after preprocessing even when `0x002BFDAC` sets stop-processing.
Such a listener cannot prevent the native black cutout from being painted.

## Geometry, state and memory

Default object dimensions are **62x32**, with **30 pixels** of extended draw
size, giving a **122x92** snapshot. ARGB8888 raw storage is 44,896 bytes
(~43.8 KiB); actual native stride/alignment and allocation overhead apply.
The earlier 64x36 / 4.5 KiB proposal was incorrect. There is one persistent
native buffer, bounded to 128x128 geometry and a validated native allocation.
No buffer is allocated per frame.

The snapshot area is the current object coordinates (+28..+40), expanded by
extended draw size in all directions. The image is placed at that same area
on every draw, not the position used during capture.

The immutable cache key comprises width/height/extra, bytes +52..+71
(sources, cached image dimensions, extra), and bytes +80..+101 (level,
colors and charge/percentage/position flags). It excludes absolute position
and callback/event pointers at +72..+79. Capture may initialize +60/+64 from
-1 to source dimensions, and invokes layout; the adapter records the key
**after** capture and revalidates geometry, binding and generation. Resource/font
refresh and native `STYLE_CHANGED` (45) also invalidate readiness and advance
the generation, even when the native source pointers stay unchanged, so refreshed
mappings/fonts cannot reuse old pixels. Style event 45 is emitted by
`0x0C107104` and recursively propagated by `0x0C106560`. Reentrant invalidation
cancels an in-flight publication.

The reserved UI-owner background poll prepares a new image when the key
changes. Until preparation succeeds, a changed battery renders natively,
rather than showing stale charge state. This can temporarily show the native
black surround (for example, before the first successful 1-second UI poll).
The stock style-fixed Control Center is the supported consumer; arbitrary
external style/child customization is not a cache-key coverage claim.

Capture/reuse/destruction require native LVGL idle status and GPU completion.
Snapshot drains its own LVGL draw tasks, which a raw GPU fence alone cannot do.
Fence failures leave storage pinned; later idle polls retry without allocating
another buffer. GPU pixels are invalidated only after completion; edited alpha
is cleaned before publishing. The existing native snapshot PM lock is held
across capture and readback; callbacks from PM transitions are followed by
object/generation revalidation before snapshot. Source storage stays valid through queued image
tasks and is not freed from draw or DELETE callbacks.

## Verification / acceptance

Host pixel and injected-native tests: `python3 tests/test_p67_battery.py`.
They exercise the real adapter with modeled native leaves, not a physical GPU.
Exact target-pack restricted symbol evidence accompanies the build; no broad
address ranges or public approvals are added to satisfy verification.

The user reported **device test PASS** for the signed .043 battery-transparent
package on 2026-10-11. Tested module SHA256:
`f4d9ae2d0b282c81772720d31145701b3c39858b74ebd77ab875dea993df44c3`.
This establishes user-reported acceptance of the delivered rendering change,
not an instrumented cache-coherency, GPU-failure, latency or frame-time probe.
The exact exercised state/gesture matrix was not separately reported; those
broader claims remain **NOT_PROBED**. Restricted target symbols are not promoted
to approved public APIs by this feedback. No unmeasured sub-millisecond
performance claim is made. For detailed regression coverage on exact .043
hardware, check:

- White backing/corners against bright and textured backgrounds.
- Horizontal page entry/exit, direction reversals and vertical content scroll.
- Percentage on/off; low/normal/charging colors and charging-icon position.
- Charge-state changes while open; close/reopen and native page reconstruction.
- Theme reload (including same-path assets), disabling the background, and OOM.
- Repeated captures/GPU completion errors do not grow retained buffer count.

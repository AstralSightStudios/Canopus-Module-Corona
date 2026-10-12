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
          invalidate -> mask-driven alpha recovery -> clean
                    |
   immutable image task at current object coordinates + current layer clip
```

This is not wallpaper restoration. Scroll/page transforms and clipping stay
with the native image task, and the cutout has already removed square white
corners before the black pixels become transparent. No display framebuffer is
read or written by the adapter.

The original literal black key left opaque gray antialiasing fringes on the
user's device. The new path samples the **actual mapped black mask** through
the native image decoder and reconstructs coverage, rather than thresholding
battery colors. For a source-over black mask with alpha `k`, output alpha `A`
contains both the mask and battery: the corrected alpha is `max(A-k,0)`.
Premultiplied RGB already holds the desired covered battery color and is left
unchanged. Straight RGB, if reported by the native descriptor, is recovered
with `RGB * A / corrected_alpha`. Zero-alpha pixels have zero RGB.

Only covered mask pixels are corrected. Unmasked dark text, gray fill and the
later charging overlay are not blanket color-keyed. Values incompatible with
a mask-last pixel are conservatively preserved (one 8-bit unit of rounding
tolerance). A centered charging image is drawn after the mask: the integration
caches its actual 16x48 alpha and removes only the surviving black contribution,
`k * (1 - charging_alpha)`. This preserves the glyph and clears the black behind
its transparent pixels; excluding its entire rectangle would leave black strips.
Noncentered charging starts at `x2+2`, outside the used mask rectangle.
Do not run this operation twice on the same snapshot or run the old key after it.

## Strict recovery: no fallback

The legacy black-color key has been removed. A bound battery submits only a
successfully recovered image. Unsupported resources, failed sampling, stale
state and allocation/GPU failures leave the battery undrawn until recovery
succeeds; they never substitute the old key or native on-screen DRAW_MAIN.
Native DRAW_MAIN is still called for deliberate offscreen capture, and other
native events retain their original lifecycle/layout handling. Disabling custom
backgrounds restores the original native class as an explicit configuration
change, not an error fallback.

## Screen-size sampler bug and diagnostics

The first edge-recovery build created the sampling image with parent=NULL.
That makes it a screen with display-sized coordinates (336x480), even after
set_size(64,36). Size refresh at 0x1C02B374 exits when parent is NULL
(0x1C02B394 -> 0x1C02B448); it does not resize screen coordinates. Snapshot
0x1C06035E therefore requests display-sized storage, which reshape 0x1C03191C
rejects against the small alpha-sampling allocation. This static defect explains
why recovery could never become ready, while silent fallback hid the error.

The corrected sampler creates an unthemed inactive root IMAGE and a second,
unthemed CHILD image. Source, size and snapshot apply to the child only; the
display-sized root is never sampled. Deleting the retained root recursively
deletes the child after verified LVGL/GPU completion. The host model now enforces
the real parentless-screen resize prohibition and checks actual coordinates
against the requested snapshot dimensions.

The serialized UI poll writes a bounded snapshot to:

`/data/offlinelog/resource-hook-p67-battery.log`

Header `P67BATTERY2`, build `alpha-strict-diag-1`. It records mode/reason,
sample kind (mask/charge/main/draw), actual style values, source header/intrinsic
dimensions, **coordinate-derived draw dimensions**, extra size, snapshot result,
buffer dimensions/flags, alpha min/max/counts and the first nonblack sample.
Only latched integers are formatted; draw/DELETE never perform diagnostic I/O.
Writes are deduplicated and rate-limited, and replace rather than append the
file. Failed writes have a bounded retry cooldown.

A valid fractional black mask need not contain alpha exactly 255. Validation
requires transparent and nonzero coverage, not an unnecessary exact-opaque
sample. No RGB threshold is introduced.

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
| `0x0C588D59` | Image creation `(parent, init_options)`; explicit byte-zero options disable theming |
| `0x0C587FA1` / `0x0C588629` / `0x0C588DE9` | Mask image source / dimensions / deletion |

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
No buffer is allocated per frame. The coverage caches add **3,072 CPU bytes**:
2,304 for the mask and 768 for centered charging. One auxiliary ARGB8888 buffer
(64x36: 9,216 raw bytes; 16x48 charging: 3,072 raw bytes; plus native overhead)
and an unthemed inactive root image with one resizable child image exist only
while sampling a new source. Only the child is captured.
The same auxiliary ownership slots serve both captures, never concurrently. They
are released after successful queue drain and GPU completion; failed fences
pin that bounded auxiliary allocation for later safe cleanup. No new copy is
allocated until retained storage is safely released.

The native mask image is 64x36, but the battery draw passes the destination
`{x1-2,y1-1,x2,y2}` with stock properties 104/105 zero. GPU drawing uses the
supplied blit width/height: **64x33** for the stock 62x32 battery, not all 36
source rows. Recovery samples the full mask, then processes only the used
source rectangle at `(extra-2,extra-1)` within the main snapshot. Style image
opacity (68), recolor opacity (70), transform width/height (104/105) must have
stock defaults; otherwise source coverage alone is not sufficient and recovery
falls back.

GPU dispatcher `0x1C045360` sets image-header flag `PREMULTIPLIED` (bit 0 at
buffer+2). Creation starts with allocation/modifiability flags 0x30; the adapter
checks the final flag rather than assuming straight RGB. In the normal GPU
case, correcting edge pixels therefore requires alpha subtraction, **no RGB
division**. Coverage is sampled only on source/revision changes; charge-state
updates reuse the CPU coverage cache and scrolling only submits cached pixels.

The snapshot area is the current object coordinates (+28..+40), expanded by
extended draw size in all directions. The image is placed at that same area
on every draw, not the position used during capture.

The immutable cache key comprises width/height/extra, bytes +52..+71
(sources, cached image dimensions, extra), and bytes +80..+101 (level,
colors and charge/percentage/position flags). Fields +60/+64 are the cached
**charging image** dimensions from source +56, not the mask dimensions; witness
`0x0C4C2092`. It excludes absolute position
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
package on 2026-10-11, and subsequently reported antialiased gray fringes.
The mask-driven correction described above has **not yet been device-tested**.
Previously tested module SHA256:
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

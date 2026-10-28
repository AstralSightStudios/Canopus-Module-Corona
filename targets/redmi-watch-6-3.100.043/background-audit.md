# P65 native backgrounds

Exact AP SHA256 `a19b601477569765103eaf468b937bd8026b4d68ffa5c896bde222a79e475858`.
Recovered from the supplied OTA's `vela_ap.bin`, with the runtime SRAM/PSRAM
copies recorded by `core-evidence.json`. `background-functions.json` records
all exact 32-byte witnesses, hashes, file offsets and function identities.
All code here is STATIC_RECOVERED / host instruction-model evidence only;
physical display, GPU behavior and frame time are **NOT_PROBED**.

## Four independent owners

| Surface | Root slot | Native snapshot slot | Native root-position callback |
|---|---|---|---|
| launcher | `0x3c456fa8` | none | none |
| control center | `0x3c453b98` | none | none |
| notifications | `0x3c459a08` | `0x3c4599dc` | `0x0c8078d5` |
| widgets | `0x3c456b9c` | `0x3c456ba0` | `0x0c7dd919` |

The launcher app page uses its own custom live menu at `0x3c456fa0`.
`0x0c7f0510` stores the root in `0x3c456fa8` and moves retained menu children
from a cached root. It does not use P67's launcher foreground/snapshot fields.
The control-center root is stored by `0x0c79e0fc`; all of its original children
remain live. No P67 snapshot callback is substituted on these two surfaces.

Widgets `0x0c7dd664` obtains the page's parent and registers the same callback
separately for filters 12, 9, 11 and 19, with the page pointer as user data;
it then creates the widgets root as a child of that page. The adapter replaces
only these exact matching callback descriptors, preserving each filter and
user-data word. Notifications use the equivalent root-position callback.
On native begin/end the adapter restores native snapshot/live visibility,
forwards the original callback in full, then hides a currently owned native
snapshot and shows its matching live root. Terminal native deletion therefore
continues to release the native snapshot through its original callback.
Default/system configuration detaches module callbacks and restores native
callbacks and visibility. A config commit during an existing snapshot fails
with busy; no native capture pool is edited, shared or freed by the module.

## Draw and deletion event correction

P65 **does not** use P67's event numbering. `lv_obj_redraw` at `0x003838cc`
emits `22, 23, 24` before child traversal and `25, 26, 27` afterward. They are
DRAW_MAIN_BEGIN, DRAW_MAIN, DRAW_MAIN_END and DRAW_POST_BEGIN, DRAW_POST,
DRAW_POST_END respectively. The adapter draws backgrounds in **24**, below
all native children; it never paints a background over the foreground.

`lv_obj_delete` at `0x0037ff28` calls `lv_obj_send_event(obj,35,0)` at
`0x0037ff7c`, before destroying object storage. Events36 and38 notify the
parent's child list. The adapter invalidates its binding on **35**, without
walking or unregistering the currently deleting subtree. Stale generations
cannot draw on reused object addresses.

P65 image draw descriptors are **92 bytes**, initialized by `0x0c719ec5`;
source+28, scale+48/+52, pivot+64/+68. `0x0c719f85` submits the image using
this descriptor. Each image is scaled to 432×514, positioned at screen origin,
and clipped against both the current root's page coordinates and layer clip
at +24. The adapter restores all four saved layer clip coordinates afterward.
A 112-byte P67 image descriptor or its rectangle descriptor is not reused.

## Blur capture identity

The independent module-owned blur provider captures the exact watchface root
at slot `0x3c46c020`, validated by current registered object membership.
The stock watchface capture `0x0c8fa0f8` reads this slot, acquires a native pool
buffer, and invokes snapshot-to-buffer `0x0c8be3b8(root,15,descriptor)`.
The module uses only the root identity; its blur provider allocates independent
drawing buffers and does not borrow the native pool descriptor. The owner
calls `p65_blur_watch(demand)` once per serialized UI poll, uses
`p65_blur_source(path)` only while drawing, and calls `p65_blur_reset()` for
configuration changes/native entry. Native allocation and fence details are
recorded separately in the blur-provider evidence.

## Control-center battery

P65's battery is **not the P67 122×92 custom opaque battery class**.
The exact P65 component constructor `0x0c21b4b0` builds an ordinary 64×40 base
child with white background opacity76, then a normal
`/resource/app/control_center/Battery11_5green.bin` image and charging content.
The topbar container constructor `0x0c79e9d8` explicitly uses background
opacity0. These are deliberate original battery visuals; they remain native
children above the wallpaper. P67's special alpha-mask reconstruction and
snapshot storage ABI do not apply, and no threshold-based pixel removal is
introduced.

## Validation

`python3 tests/test_p65_backgrounds.py` compiles the actual production
`p65_backgrounds.inc` with ASan/UBSan and controlled native leaves. It covers:
all four independent image staging/draws; screen-fixed clip restoration;
widget/notification begin/end native callback forwarding and snapshot flags;
refusal to commit while native snapshots exist; default restoration; callback
registration failure/retry; DELETE35 and stale generations; and blur demand
configuration. The blur provider has its own actual-provider tests and evidence.
These modeled leaves do not prove actual GPU rendering or device acceptance.

# P65 owned snapshot and static blur

This adapter is specific to P65 `3.100.043`, AP SHA-256
`a19b601477569765103eaf468b937bd8026b4d68ffa5c896bde222a79e475858`.
Static identities and instruction tests are **NOT_PROBED** on physical hardware.
Full body hashes and file offsets are recorded in `blur-evidence.json`.

| Contract | Exact P65 identity |
|---|---|
| Native draw buffer create / destroy | `0x0c719551` / `0x0c719799` |
| Snapshot into caller-owned buffer | `0x0c7417c1` |
| Raw GPU finish | `0x0c8bc081` → SRAM `0x00389e41` |
| Draw-buffer hook table | `0x3c450108` |
| Native invalidate / clean | `0x0c72f021` / `0x0c72f02d` |

The draw buffer is **24 bytes**, containing header12, byte size at12, aligned
pixels at16, and original allocation at20. There is no P67-style handler pointer
at24. Constructor disassembly at `0x0c719550` allocates24 bytes, sets owned flag
`0x10`, uses64-byte alignment and allocates pixels through the firmware's native
draw-buffer allocator. GPU initialization replaces its stride callback with
`0x0c728755`: full432×514 format15 has stride1296;72×86 format18 has stride160.

The snapshot helper temporarily replaces display root layer at672 with a local
120-byte layer, queues native drawing, waits for the layer's draw tasks and restores
the original layer. Its boolean result does not propagate every GPU failure.
The module therefore runs the raw GPU finish and checks its result even after
a snapshot failure. The ordinary native font wait drains references despite GPU
errors and cannot substitute for this successful-completion check.

P65 cache initialization at `0x0c72f038` installs two actual no-op functions:
invalidate at hook-table+12 and clean at+16. The adapter checks those exact
identities and uses their native contracts, with ARM barriers. It does not call
unrelated P67 HAL cache functions. This contract is limited to the supplied AP;
these no-op bodies do not prove device coherency or safe GPU fault recovery.

The captured432×514 BGR888 image is copied into a72×86 RGB565 plane using six-by-six
averages. The bottom two scaled rows clamp to the physical panel edge. Three
box-filter passes at radius3 apply70% channel darkening. The immutable native
output uses image scale1536 (`6 * 256`) and is clipped by the screen-fixed draw
owner to432×514. Static scratch is24,768 bytes; the large input and small output
are allocated natively and do not consume the module's256 KiB service quota.

The provider API included by the background adapter is:

```c
#define P65_BLUR_WATCHFACE() exact_native_watchface_root()
#include "p65_blur_native.inc"
/* UI owner, once per scheduled poll: */
p65_blur_watch(any_region_needs_blur);
/* Draw callback, read only; ordinary paths pass through: */
uintptr_t source = p65_blur_source(path);
/* Config and lifecycle invalidation do no graphics work: */
p65_blur_reset();
p65_blur_drop();
```

There are two output slots and one input slot. Failed post-capture fences retain
input and prevent another allocation until a later idle successful fence.
Published outputs remain immutable; old outputs are retired logically before
later native destruction. Capture publication checks the configuration epoch
and source root again, rejecting reentrant configuration/root changes.
Disabling blur clears the published source immediately and retires owned buffers
at a later safe UI boundary. Rendering, active draw units, pending layer tasks,
unknown cache hooks and malformed descriptors refuse capture.

`tests/test_p65_blur.py` passes both tests under `build/p65-tests/bin/python`:

- Production provider compiled with mocked native leaves and ASan/UBSan:
  allocation failures, raw fence errors, delayed cleanup, bounded allocation,
  stale roots/configuration, reentrant watch, CPU pixel/stride/channel checks,
  bottom clamp, queue/render guards and restoration to the system background.
- Original Thumb constructor/format/stride/cache-hook/destructor instructions:
  native allocator/free leaves and hardware-capability query controlled; exact
  descriptors and original free-pointer pairing verified. All recorded native
  body fingerprints independently match the supplied AP.

These checks do not claim a full firmware boot, physical display acceptance,
GPU color/coherency acceptance, PM-governor behavior, or recovery safety.

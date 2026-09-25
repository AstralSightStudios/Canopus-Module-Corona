# Experimental Q66 .155 font reload

This is an opt-in implementation for `xiaomi-band-11-4.100.155`, not a production font reload guarantee. Default `.139` and `.155` builds retain the existing registry-only behavior. No automatic page rebuild, GPU wait/reset or global cache drop is added.

## Build and target approval

```sh
RH_EXPERIMENTAL_FONT_RELOAD=1 sh scripts/build.sh xiaomi-band-11-4.100.155
```

The experimental ELF is `build/resource-hook-font-experimental.elf`, with descriptor build ID `resource-hook-0.3.0-font-exp`. It does not overwrite the default `build/resource-hook.elf`. Other targets and values besides `0`/`1` are rejected. `build-install-payload.sh` refuses the experimental option, including when invoked by the release-delivery builder.

The selected Canopus target pack must allow the exact native font functions used by `src/font_reload_155.c`. The normal verifier is still mandatory: an existing pack that only approves the image adapter will reject the new absolute addresses. An ELF produced before that rejection is **not a verified/installable delivery**. Do not disable verification or broaden the allowed address ranges to get a green build. The original pack's target ID and AP SHA256 are checked before compilation.

The adjacent `Canopus-Private` `.155` pack was updated with user permission: 42 exact restricted symbol records and `EVID-FONT-4155-001`, plus normally generated C/Rust metadata. They remain `STATIC_RECOVERED / PENDING`, not public-callable or device approval. No address range was broadened. With these additions, the experimental ELF passes the unchanged strict verifier; removing the allocator's exact record makes verification fail again.

## What the transaction does

- Capture original registered family paths before the first experimental change. Later mappings are always resolved from this baseline, including rule removal and restoration of stock paths.
- Validate the bounded manager, wrapper, face-cache, draw-unit, pending-array and owner layouts. Busy drawing/cache references return a retry without changing live fonts.
- Check file fingerprints and prepare the whole affected set using checked native allocations. Avoid the unsafe native wrapper/cache constructors and registry remove/add sequence. The FreeType parser and audited cache leaves are reused.
- Revalidate immediately before publishing. Replace backing records and the callback/metrics/descriptor portion of existing wrappers; preserve each wrapper's address, fallback chain, user data, record and list links.
- Replace registry paths, evict affected idle descriptors, and release obsolete descriptor/face ownership. Newly published live descriptors are never passed to rollback cleanup.
- Drop audited vector-label copied geometry and issue text-font property refresh for collected registered-screen objects, including off-screen trees. Recheck object membership around callbacks. The module then requests a full redraw.

Preparation and publication happen within one serialized UI timer callback, never during direct activation. A busy result retains the request and mapping snapshot. A negative result preserves a visible error but does not indefinitely stall unrelated image updates. Font/image publication is **not** one atomic transaction: image rules can already be active when fonts are rejected.

## Resource and support limits

- Only the fingerprinted `.155` standard managed outline-FreeType path is supported. Each affected family must have an auditable active or idle descriptor at its first change (or an already audited scalar exemplar from this module lifetime).
- Unknown font callbacks/backends, unregistered/app-owned font copies, custom text caches, canvas pixels and separately uploaded paths are not supported.
- Use a **new immutable destination filename/directory for every font generation**. Keep files unchanged and available. Overwriting `themes/current/font.ttf` is not supported. Non-stock generation paths cannot be selected again after leaving them; restoration of unchanged stock files is supported.
- A target face already held outside the current preparation transaction is refused rather than borrowed from an unknown consumer. New staged descriptors may share a face with each other.
- The baseline must be captured before legacy retargeting loses the originals. Switching a running legacy instance into this implementation cannot reconstruct missing original paths; use a clean framework/module lifetime.
- Bounds: 32 registered families, 96 backing resources, 256 wrappers, 512 collected UI objects, 32 parent levels, 64 remembered non-stock family-path selections, and 8 MiB per fingerprinted font file. Exceeding a bound is a refusal, not partial publication. Persistent adapter state is approximately 42 KiB, plus bounded transaction scratch and staged native fonts/caches. File validation and font parsing run synchronously and may delay a UI tick.
- Same mapping text after a successful request remains a no-op at module level. A new signal with unchanged mapping text retries a previously failed font transaction; it is not an in-place file replacement mechanism.

For example, change a mapping destination from `themes/font-g1/` to `themes/font-g2/`, with the same font-relative paths in each immutable directory, then write a new reload-request revision. Removing that mapping restores the captured stock path. Do not reuse `font-g1` in this module lifetime.

## Safety boundary

This option assumes a healthy, serialized standard graphics pipeline. Empty software queues do **not** prove GPU completion after a timeout or reset. Host tests cannot establish this hardware property. Do not enable the experiment where GPU recovery, custom asynchronous consumers or arbitrary cross-thread UI calls must be supported.

The adapter detects changed native roots/registry identities and irreversibly disables itself. A failure after publication while refreshing owners also latches it disabled: a same-mapping retry must not falsely report success without refreshing the remaining owners. Already published fonts stay live; recovery requires a clean module/UI lifetime. Module reactivation after its driver callback has been reset also calls `rh_font_reload_disable()` before further font work. This is a conservative restart latch, **not a complete firmware lifecycle hook**: equal reused addresses are not an epoch, and unobserved teardown is unsupported. No prepared native objects are intentionally retained across timer callbacks.

## Experimental status

Default builds retain RHQ1 **v5 / 40 bytes** unchanged. The experimental build emits **v6 / 48 bytes**:

| Offset | Field |
| --- | --- |
| 0..35 | Same fields as v5; `rebuilds` remains zero |
| 36 | Committed font-registry family count, saturating; not glyph/widget count |
| 40 | Signed font result: `0` completed/no-op, `1` pending/busy, negative rejected/failed |
| 44 | `1` while the font stage is pending, otherwise `0` |

The adapter currently uses `-1` for unsupported/invalid/lifecycle conditions and `-2` for preparation/allocation failure; `-2014` records the module's observed-restart latch before a subsequent adapter call. A post-commit owner-refresh failure can have a nonzero committed count; that is not a rollback. Consumers must check the version/length and error field rather than interpreting image redraw success as font success. No manager UI changes are included.

## Verification

`build.sh` runs the existing host suite plus:

- `tests/test_module.c --experimental-fonts` in a separately compiled opt-in binary: timer-only execution, busy retry, mapping-bank lifetime, permanent-error completion, same-config explicit retry, stock-removal dispatch, v6 capacity/status, and restart latch.
- `tests/test_font_reload_155.c`: the actual transaction code against a 32-bit memory model with native leaves injected, including 13 single-family and 15 two-family allocation-failure positions and rollback, wrapper/fallback preservation, idle paths, file immutability, busy holds and owner deletion.

`tests/firmware_font_barrier_155.py` separately executes selected native instructions. Neither suite runs the complete real FreeType parser or GPU, and neither is an on-device font reload acceptance test. Repeated real reload, memory-pressure behavior, visual layout and graphics-recovery validation remain required.

Integration results: default `.139` and `.155` builds pass, as does the opt-in `.155` build with strict target verification. The 33 selected font instruction probes and 19 `.155` image regression probes pass. Host sanitizer tests additionally cover the post-commit traversal-overflow latch and reject a misleading same-mapping success afterward.

One optional dependency check remains blocked by an existing issue: `generate_band11_native_config.py --check` requires `errno_location` to be `restricted`, while the unchanged record is `managed`. No unrelated symbol policy or native-profile generation was changed to suppress that error; it does not block the direct-call module build/ELF verification.

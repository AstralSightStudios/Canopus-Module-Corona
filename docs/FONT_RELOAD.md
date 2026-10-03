# Font hot reload

Font hot reload is part of the default Resource Hook module for
`xiaomi-band-11-4.100.139`, `xiaomi-band-11-4.100.155` and
`xiaomi-band-10-pro-3.101.043`. There is no experimental compile option or
registry-only fallback. No automatic page rebuild, GPU wait/reset or global
cache drop is added.

The [.139 compatibility audit](../targets/xiaomi-band-11-4.100.139/font-reload-compatibility.md)
verifies the supplied OTA/loader, native bodies and shared layouts. The
[10 Pro .043 audit](../targets/xiaomi-band-10-pro-3.101.043/font-reload-audit.md)
recovers its 45 identities, including startup-copied PSRAM callbacks. Its face
payload is 24 bytes rather than 28; vector key-drop takes the object in r1.
These are explicit target contracts, not a blanket relocation delta.

## Device acceptance

The user reports that font reload has passed physical-device acceptance on
**all three supported firmware targets** and requests its promotion to the
ordinary build. Status: **USER_REPORTED_PASS** for the normal font-reload
workflow. This is a user report, not an independently instrumented GPU, OOM,
restart or endurance test. No per-target artifact hashes or step-by-step
acceptance log were supplied with the latest report.

The earlier [.155 device report](../targets/xiaomi-band-11-4.100.155/font-reload-device-report.md)
preserves historical replacement successes and stock-restoration errors
(`-2` / `-2908`) that motivated checked stock-face reuse. Those checkpoints
are not the current normal-workflow acceptance status; the latest report
does not separately enumerate each restoration/fault scenario.

## Build and target approval

```sh
sh scripts/build.sh xiaomi-band-11-4.100.139
sh scripts/build.sh xiaomi-band-11-4.100.155
sh scripts/build.sh xiaomi-band-10-pro-3.101.043

# Signed normal installer; repeat --target for a multi-device bundle:
python3 scripts/build-watchface.py --target xiaomi-band-10-pro-3.101.043
```

Every build emits `build/resource-hook.elf` with build ID
`resource-hook-0.3.0` and includes the checked font transaction.
The ordinary signed payload/delivery builders include it too. Old experimental
environment variables are no longer used. Installation still leaves the
module disabled until explicitly enabled; promoting fonts does not restart
the framework or automatically activate an installed module.

The selected Canopus target pack must allow the exact native font functions.
Strict verification is mandatory: an ELF rejected by an outdated pack is
**not an installable delivery**. Do not bypass verification or broaden address
ranges. Target ID and AP SHA256 are checked before compilation.

The adjacent packs retain the previously approved restricted records:
`EVID-FONT-4139-001` / `EVID-FONT-4155-001`, and 41 additional .043 font
records under `EVID-FONT-1043-001` with its existing cache-release record
reused. The .043 startup-copy execution range
`0x1c000000..0x1c080500` is declared separately from unchanged XIP ranges;
its preexisting POSIX-open callback `0x1c057c51` has an exact record.
Removed-record negative tests still reject unknown calls. Normal C/Rust
metadata generation and the verifier are unchanged. Module device acceptance
does not silently promote private-ABI records from `STATIC_RECOVERED/PENDING`
to public-callable or independently device-approved symbols.

## What the transaction does

- Capture original registered family paths before the first change. Later mappings are always resolved from this baseline, including rule removal and restoration of stock paths.
- Validate the bounded manager, wrapper, face-cache, draw-unit, pending-array and owner layouts. Busy drawing/cache references return a retry without changing live fonts.
- Check file fingerprints and prepare the whole affected set using checked native allocations. Avoid the unsafe native wrapper/cache constructors and registry remove/add sequence. The FreeType parser and audited cache leaves are reused.
- Revalidate immediately before publishing. Replace backing records and the callback/metrics/descriptor portion of existing wrappers; preserve each wrapper's address, fallback chain, user data, record and list links.
- Replace registry paths, evict affected idle descriptors, and release obsolete descriptor/face ownership. Newly published live descriptors are never passed to rollback cleanup.
- Drop audited vector-label copied geometry and issue text-font property refresh for collected registered-screen objects, including off-screen trees. Recheck object membership around callbacks. The module then requests a full redraw.

Preparation and publication happen within one serialized UI timer callback, never during direct activation. A busy result retains the request and mapping snapshot. A negative result preserves a visible error but does not indefinitely stall unrelated image updates. Font/image publication is **not** one atomic transaction: image rules can already be active when fonts are rejected.

## Resource and support limits

- Only the fingerprinted Band 11 `.139` / `.155` and Band 10 Pro `.043` standard managed outline-FreeType paths are supported. Each affected family must have an auditable active or idle descriptor at its first change (or an already audited scalar exemplar from this module lifetime).
- Unknown font callbacks/backends, unregistered/app-owned font copies, custom text caches, canvas pixels and separately uploaded paths are not supported.
- Use a **new immutable destination filename/directory for every font generation**. Keep files unchanged and available. Overwriting `themes/current/font.ttf` is not supported. Non-stock generation paths cannot be selected again after leaving them; restoration of unchanged stock files is supported.
- Existing non-stock target faces outside the current preparation transaction are still refused. An unchanged, fingerprint-verified stock face may be borrowed through a new native cache reference after checking its canonical interned pathname, style/mode key, existing positive ownership, FreeType face and both child caches. Child-cache holds remain busy/retry. Existing caches and other consumers' references are never overwritten or forcibly retired; new staged descriptors may also share a face with each other.
- The baseline must be captured before legacy retargeting loses the originals. Switching a running legacy instance into this implementation cannot reconstruct missing original paths; use a clean framework/module lifetime.
- Bounds: 32 registered families, 512 affected backing resources per transaction (the combined capacity of two 256-record active/idle scans), 96 interned file IDs, 4096 wrappers, 512 collected UI objects, 32 parent levels, 64 remembered non-stock family-path selections, and 32 MiB per fingerprinted font file. Exceeding a bound is a refusal, not partial publication. Persistent adapter state is approximately 42 KiB, plus bounded transaction scratch and staged native fonts/caches. File validation streams 2 KiB chunks rather than allocating whole files. It runs only after idle/ownership checks, deduplicates shared paths within a transaction, and accepts the 11,637,064-byte stock Regular-All font. Validation and parsing remain synchronous and may delay a UI tick.
- Same mapping text after a successful request remains a no-op at module level. A new signal with unchanged mapping text retries a previously failed font transaction; it is not an in-place file replacement mechanism.

For example, change a mapping destination from `themes/font-g1/` to `themes/font-g2/`, with the same font-relative paths in each immutable directory, then write a new reload-request revision. Removing that mapping restores the captured stock path. Do not reuse `font-g1` in this module lifetime.

The font adapter is independent of Manager and runs in the default module. Manager no longer bundles the Fusion Pixel test font or exposes test controls. A font resource pack or another trusted tool must publish immutable font generations, update mappings and send the ordinary reload request. Promotion does not add a new font-testing UI or permit in-place file overwrites.

## Safety boundary

The implementation assumes a healthy, serialized standard graphics pipeline. Empty software queues do **not** prove GPU completion after a timeout or reset. Neither host tests nor the reported normal-path acceptance establish this hardware property. GPU recovery, custom asynchronous consumers and arbitrary cross-thread UI calls remain unsupported.

The adapter detects changed native roots/registry identities and irreversibly disables itself. A failure after publication while refreshing owners also latches it disabled: a same-mapping retry must not falsely report success without refreshing the remaining owners. Already published fonts stay live; recovery requires a clean module/UI lifetime. Module reactivation after its driver callback has been reset also calls `rh_font_reload_disable()` before further font work. This is a conservative restart latch, **not a complete firmware lifecycle hook**: equal reused addresses are not an epoch, and unobserved teardown is unsupported. No prepared native objects are intentionally retained across timer callbacks.

## Runtime status

All current builds emit RHQ1 **v6 / 48 bytes**. Its first 40 bytes preserve the historical v5 field offsets; clients must check version/length and supply a 48-byte buffer:

| Offset | Field |
| --- | --- |
| 0..35 | Same fields as v5; `rebuilds` remains zero |
| 36 | Committed font-registry family count, saturating; not glyph/widget count |
| 40 | Signed font result: `0` completed/no-op, `1` pending/busy, negative rejected/failed |
| 44 | `1` while the font stage is pending, otherwise `0` |

The adapter retains generic `-1`; the legacy ambiguous preparation/allocation `-2` is now split into the `-29xx` codes below; `-2014` records observed restart. Stage codes `-2201`/`-2202` identify roots/registry rejection, `-2204` the drawing boundary, `-2205` ownership, `-2206` missing audited exemplar, and `-2207` owner overflow. File codes `-2210`/`-2211`/`-2212`/`-2213` identify I/O, size limit, file-count limit and changed current-file contents. A post-commit owner-refresh failure can have a nonzero committed count; that is not a rollback. Consumers must check the version/length and error field rather than interpreting image redraw success as font success. External diagnostic clients can read the result via the optional file protocol below.

### Ownership diagnostic codes (replacement for generic `-2205`)

The diagnostic build preserves all refusal predicates and keeps outstanding holds as `1` (busy). It no longer collapses resource validation errors into `-2205`. External clients may display these signed codes. A code identifies a failed check, not evidence that its expected firmware layout is correct on every runtime path.

Cache failures use `-(base + reason)`: base `2300` = face cache, `2400` = metrics cache, `2500` = outline cache. Reasons: `1` missing cache, `2` class, `3` payload size, `4` LRU element size, `5/6/7` comparator/create/destroy callback, `8` traversal bound, `9` previous link, `10/11` missing RB node/payload, `12` entry owner, `13` entry payload offset, `14` invalid entry, `15` invalid refcount, `18` tail, `19` count. Thus `-2406` specifically means the metrics-cache create callback differs. Vector checks use base `2800`, but the outer drawing-boundary stage still reports `-2204`.

| Codes | Failed descriptor/manager check |
| --- | --- |
| `-2601` | Missing backing font |
| `-2602/-2603/-2604` | Metrics / outline-acquire / glyph-release callback |
| `-2605/-2606/-2607/-2608` | Descriptor placement / magic / context / self pointer |
| `-2609/-2610/-2611` | Pixel size / render mode / original path |
| `-2612/-2613/-2614/-2615` | Face / face-entry position / entry owner / entry offset |
| `-2616/-2617/-2618/-2619` | Face hold / invalid entry / interned path identity / style-mode key |
| `-2620/-2621` | Missing FT face / nonscalable face |
| `-2701` | Face-cache capacity |
| `-2703/-2704/-2705/-2706/-2707` | Active name / name length / inline name pointer / zero owners / record bound |
| `-2708/-2709` | Active record size / style differs from backing descriptor |
| `-2712` | Wrapper count versus record owners |
| `-2730` through `-2736` | Wrapper copied word at byte offset `0,4,8,12,16,20,24` differs from backing font |
| `-2714/-2715/-2716` | Idle name / name length / replacement bound |
| `-2717/-2718/-2719/-2720` | Idle size / idle style / duplicate descriptor ownership / total bound |

Whole-list diagnostics now use `-2741..-2744` (active), `-2751..-2754` (idle), and `-2761..-2764` (wrappers). The suffix is `1` payload-size mismatch, `2` traversal bound (including a cycle), `3` previous-link mismatch, `4` tail mismatch. These replace the ambiguous list errors `-2702`, `-2710` and `-2713`. `-2765` indicates wrapper count changed between validation and scanning; `-2766` indicates a captured affected wrapper disappeared or changed relative order before commit.

The active/idle scan previously reused the 96-replacement limit even for unrelated records. Both scans now use their existing 256-element scratch capacity, matching the precommit scan. A subsequent device `-2707` established that more than 96 active backings are actually affected, not merely present. The replacement capacity is now 512, matching the combined active/idle scan bounds; wrapper, ownership and link checks remain unchanged. The separate interned-path limit remains 96 rather than growing with descriptor count. Host regression covers 100 unrelated active or idle records, oversized lists and broken links/tails. A large affected fixture additionally exercises 255 active backings, 256 wrappers and 256 idle backings (511 replacements), restoration, and allocation failures at the start, middle and end of a 521-allocation preparation, with unchanged old references and successful retry. The backing-array expansion adds 14,976 bytes of bounded heap storage, not a larger UI stack or full-file allocation. Real-device allocation latency and font parsing remain unverified. Exact native insert/remove instructions at `0x0c3a43b0`, `0x0c3a46a2` and `0x0c3a46dc` confirm head/tail at list+4/+8 and previous/next at node+size/+size+4. The device's former `-2702` alone does not prove whether capacity or a link check failed.

A subsequent device `-2762` reached the old 256-wrapper limit. Wrapper cardinality is now independent of backing cardinality: validate/count up to 4096 wrappers without a stack array, then allocate `actual_count * sizeof(struct wrap)` bytes (16 bytes each, at most 64 KiB). Only affected wrappers are captured; membership is rechecked as an ordered subsequence of the live list before publication. Backing scans remain 256 each and replacement capacity remains 512. Snapshot allocation failure returns `-2902` with no publication, and heap snapshots are freed on success, rollback, busy retry and lifecycle disable. Tests cover 303/1024/4096 wrappers, 4097 rejection before snapshot allocation, restoration, snapshot OOM/retry, unrelated wrappers and replacement of a captured wrapper identity. The same fixed `-2762` code now denotes the 4096 traversal bound; this is not unlimited support.

The cache constructor, callback table and manager descriptor initialization were rechecked against the exact AP instructions at `0x0c3a998e`, `0x0c3a9e48`, `0x0c494380` and `0x0c4946a4`, plus callback table `0x2ca308e4`. This does not identify which predicate produced the device's former `-2205`; the next revision-matched result is needed for that. Host fault-injection tests assert individual codes, unchanged old fonts, no prevalidation file reads, and unchanged busy/retry behavior.

### Restore/prepare diagnostics

The legacy `-2` conflated allocation, intern-list validation/capacity, an already-existing target face and native font preparation. The subsequent device `-2908` identifies the preexisting-target-face refusal. A host fixture reproduces replacement followed by failed restoration when another consumer retains the stock face. The fix borrows only verified stock faces, balances its own acquired references on failure, and revalidates all prepared descriptors before publication. Non-stock preexisting faces remain refused. Device restoration with the fix remains unverified.

The exact `.155` native manager factory (`0x0c494380`, including `0x0c4946a4`) already acquires an existing face and skips child-cache creation on that branch. Both metrics creation (`0x0c3a5ef0`) and outline creation (`0x0c3a8bb8`) select the required FT size before loading a glyph. The adapter follows that serialized shared-face contract rather than replacing child caches or requiring exclusive ownership of the immutable stock font. This does not add support for concurrent/custom rendering or GPU recovery.

| Code | Meaning |
| --- | --- |
| `-2901` | Transaction scratch allocation failed |
| `-2902` | Wrapper snapshot allocation failed |
| `-2903` | Registry pathname allocation failed |
| `-2904` | Descriptor allocation failed |
| `-2908` | Existing target face is not verified stock and cannot be borrowed |
| `-2909` | Prepared face reference count reached the refusal threshold |
| `-2910` | Native face acquire/create returned NULL; the native leaf does not distinguish open, parse and allocation failure to the caller |
| `-2911` / `-2912` | Metrics / outline child-cache allocation or initialization failed |
| `-2913` | Missing FreeType face or non-scalable font |
| `-2914` | Native pixel-size setup failed |
| `-2915` | Missing native size metrics |
| `-2921` through `-2924` | Intern-list size, traversal limit/cycle, predecessor or tail validation failed |
| `-2931` | Existing intern-ID reference count is invalid or saturated |
| `-2932` | Intern-ID capacity exhausted |
| `-2933` / `-2934` | Intern pathname / node allocation failed |
| `-2941` | Borrowed stock entry has invalid ownership/layout, no prior positive owner, or a saturated reference count |
| `-2942` | Borrowed stock face differs from the canonical interned pathname or style/mode key |

The numeric errors are available in the module's `control.response` reload receipt for external diagnostics. Before publication, failures retain the currently displayed replacement font and preserve external ownership. Host tests inject every native allocation failure during both fresh and borrowed-stock restoration, plus pixel-size, wrapper-snapshot and precommit-validation failures. They verify unchanged external descriptors/cache identities, balanced references, busy child-cache retry, repeated replace/restore cycles and successful retry after each injected failure. No native constructor, forced cache eviction or GPU recovery was added.

## Optional client acknowledgement protocol

A compatible controller must precreate `internal://files/control.response` to receive replies, and send reload signals through `internal://files/control.request`. A nonempty `pending<TAB><revision><LF>` placeholder avoids Vela's empty `writeText` rejection (code 202). Reload processing does not depend on successful receipt-file preparation, but a missing response is not success. The module opens the existing native file with NuttX write-only flag `2`, without create or truncate. The response is a 256-byte NUL-padded record:

```text
resource-hook-reload-v1<TAB>ng.lst.corona<TAB><revision><LF>
RHRS1<TAB>6<TAB><signed-result><TAB><refresh-pending><TAB><families-changed-this-request><LF>
<unsigned-decimal-FNV1a-of-the-first-two-lines-including-LFs><LF>
```

The checksum detects torn/partial writes, not malicious changes. The module retries failed/short writes without rerunning a completed font transaction, and avoids rewriting unchanged results. Configuration failures use `-2101` (allocation), `-2102` (open), `-2103` (parse/read), and QuickApp materialization errors `-2104` / `-2105`. A polling client should ignore wrong revisions, invalid checksums and unsupported schemas, and must not report success if no valid result arrives. A negative result with nonzero changed count means publication occurred but refresh failed.

The legacy v1 reload request is still accepted and current modules emit RHRS1 v6. Parsers may continue accepting historical v5 receipts. Manager now sends reload v2 and reads RHRS2 snapshots containing active rule count and configuration state without a follow-up query; negative font results do not imply configuration failure. Its initial RHST1 status query is memory-only and does not execute font transactions. Manager and the module must be upgraded together; see [MODULE_CONTROL.md](MODULE_CONTROL.md).

## Verification

Default-build promotion checkpoint: all three targets pass the ordinary host /
ASan / UBSan transaction matrix and strict ELF validation. Each normal signed
payload passes 14 delivery identity/tamper/metadata checks. The installer/target
contract suite passes 10 checks, and the seven allowlist checks include removed
allocator and metrics-destroy records on every target. Prebuilt-artifact bypass
is no longer accepted: the installer always rebuilds and verifies the exact
staged ELF before signing. The `.139` / `.155` signed module rebind suite passes
18 checks per target with synthetic empty audited font roots, while native font
probes pass 24 / 24 / 19 checks respectively. Manager's full host suite passes.
These results remain separate from physical-device acceptance reported above.
The normal multi-device signed package is
`dist/module-installer-resource-hook-0.3.0-all/`.

`cd manager && npm test` checks reusable file functions (text/binary/range/metadata operations, missing-file semantics, native callback failures and synchronous exceptions) and Interconnect handshake, path validation, transfer, resume, and completion behavior. Manager uses a native scrolling list, 18–24 px high-contrast text and 48 px full-width buttons; physical-device readability still requires acceptance.

`build.sh` runs the existing host suite plus:

- `tests/test_module.c --font-reload` in ordinary `.139` / `.155` / `.043` binaries: timer-only execution, busy retry, mapping-bank lifetime, permanent-error completion, same-config explicit retry, stock-removal dispatch, v6 capacity/status, and restart latch.
- `tests/test_font_reload.c`: all three exact address selections execute the actual transaction code against a 32-bit memory model with native leaves injected, including 13 single-family and 15 two-family allocation-failure positions and rollback, wrapper/fallback preservation, idle paths, file immutability, busy holds and owner deletion.
- `scripts/test-host.sh`: ASan/UBSan matrix covering all three targets with the default font transaction. `tests/test_font_reload_targets.py` independently checks all 45 preprocessed native address identities against the target audits, including PSRAM callback addresses, Thumb bits and the `.043` ABI parameters, and confirms always-on native identities, removal of experimental gates and per-device installer isolation.

`tests/firmware_font_barrier_155.py` retains its historical filename but separately executes 24 selected native probes on each exact target. `tests/firmware_font_compatibility.py` validates the binary comparison evidence; 16 guard tests reject wrong fingerprints, unknown targets, unmapped addresses and missing/tampered direct-target proofs. An additional 70 direct targets have bounded local proofs; 606 transitive callsites and the assertion handler beyond its entry block remain explicitly unverified (`LIMITED`), not a whole-program equivalence claim. Neither suite runs the complete real FreeType parser or GPU, and neither is an on-device font reload acceptance test. The separate user-reported normal-path device pass is recorded above; repeated switching, memory-pressure behavior, comprehensive visual layout and graphics-recovery validation remain required.

Historical promotion baseline for Band 10 Pro `.043`: the pre-promotion three-target host/sanitizer matrix and 10 target/installer contract tests passed. `build/firmware-tests/bin/python tests/firmware_font_1043.py` passes 19 checks (14 selected-native probes and 5 evidence guards), including 24-byte face ownership, vector-drop r1 ABI, failed-create cleanup and zero/one-child face destruction with documented modeled lower leaves. Both `.043` default and opt-in ELFs pass the unchanged strict verifier. Temporary packs missing the allocator, PSRAM metrics-destroy or POSIX-open exact symbol reject their ELF probes. Real `.043` signed packaging and mixed `.155`/`.043` per-device packaging both pass byte/signature checks. These are not real FreeType-parser, GPU or physical-device acceptance tests.

Historical Band 11 integration results after unification: all four default/opt-in × `.139`/`.155` builds pass strict target verification. Under identical ARM compiler flags, the original and shared `.155` font objects have byte-identical `.text` (8,304 bytes), `.bss` (42,660 bytes), and `.rel.text` (440 bytes). Each exact AP passes 24 native font probes and 19 image regression probes. Static verification covers 122 function bodies / 9,494 instructions per target, six data records, and the supplied `.139` OTA/loader provenance. Host sanitizer tests additionally cover the post-commit traversal-overflow latch and reject a misleading same-mapping success afterward. These static/native checkpoints are separate from the latest user-reported acceptance and do not independently establish stock-restore or fault coverage.

One optional dependency check remains blocked by an existing issue: `generate_band11_native_config.py --check` requires `errno_location` to be `restricted`, while the unchanged record is `managed`. No unrelated symbol policy or native-profile generation was changed to suppress that error; it does not block the direct-call module build/ELF verification.

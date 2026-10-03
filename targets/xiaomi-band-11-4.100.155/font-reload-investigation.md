# Q66 .155 font reload investigation

## Scope and identity

Target: Band 11 / Q66, `xiaomi-band-11-4.100.155` only.
AP: `/Users/lesetong/develop/temp/extract155/vela_ap.bin`.
SHA256: `ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f`.

Inspected with IDA/Hex-Rays using the BEST15xx loader mapping. Verified segments include XIP `0x0c0c0000`, cached flash `0x2c0c0000`, NC flash `0x280c0000`, DATA `0x2007dcc8..0x200adedc`, and BSS starting at `0x200adedc`. Addresses below are even instruction addresses, not Thumb function pointers. No P65 addresses or lifecycle conclusions are used here.

## Investigation decision

**Implementation follow-up:** the checked transaction in `src/font_reload.c` is now part of the default build for all three audited targets, following user-reported normal-workflow device acceptance. See [the font contract and validation limits](../../docs/FONT_RELOAD.md). The investigation below describes pre-implementation evidence, not current runtime or device status. GPU recovery, restart and arbitrary-owner coverage remain unsupported.

The static investigation is closed at the implementation-decision boundary: **a restricted, page-preserving reload is a credible implementation project, but the current firmware helpers do not provide a production-safe font reload API**. The required ownership, allocation, cache-key, fallback and text-refresh paths are identified below. No runtime implementation or device-success claim follows from that finding.

Prefer a checked prepare/commit transaction at a naturally quiescent UI boundary over forcing GPU completion or maintaining an unbounded retired-font list. This preference does **not** certify an empty pending queue as hardware completion: the demonstrated timeout/reset ambiguity remains a device/instrumentation gate. Unknown custom text consumers and firmware teardown are explicit exclusions, not assumed-safe cases. The final sections consolidate the implementable contract and remaining validation gates; preceding follow-ups record how that conclusion was reached.

## Confirmed behavior

- `0x0c494380`: font-manager wrapper creation. Searches active records first, then idle fonts, then resolves a pathname and creates a FreeType descriptor. Active/idle lookup keys are family name and packed size/style, not the replacement pathname.
- Manager active list starts at +0; wrapper list at +12; idle-cache pointer at +556. An active record contains the underlying font at +0 and reference count at +44. Wrapper creation copies 36 bytes of font data and stores the record pointer at wrapper+36.
- `0x0c917384`: wrapper deletion. Decrements the record reference count. The final ordinary-font wrapper transfers the underlying font into the idle cache; it does not necessarily destroy FreeType state. It then unlinks/frees the record and wrapper.
- `0x0c4940fc`: idle-entry eviction. Follows entry+40 -> font+24 -> FreeType descriptor. Releases the face-cache entry using `0x0c8b9780`, conditionally drops it through `0x0c8b8c9e`, releases the pathname through `0x0c39a424`, then frees the descriptor and idle-list node.
- `0x0c396b5c`: glyph metrics callback. Reads descriptor through font+24, then uses descriptor-specific metrics/glyph caches. Changing only the family registry does not change this descriptor.

These paths confirm why redraw or registry retarget alone cannot update retained wrappers, including fonts reused after a page closes.

## Additional obstacle to in-place replacement

`0x0c3a7c44` (outline glyph acquisition) gets the descriptor from the font and acquires an entry from the descriptor's outline cache. It stores that entry at glyph-description+20.

`0x0c3a0992` (outline glyph release) reads that saved entry, but obtains the cache to release it into by traversing the supplied font's **current** descriptor:

```text
entry = *(glyph_description + 20)
cache = *(*(*(font + 24) + 52) + 20)
native_cache_release(cache, entry)
```

Consequently, overwriting a live wrapper's descriptor while old glyph acquisitions remain outstanding can pair an old entry with a new cache. This is a conditional hazard established by the call paths, not proof that a particular device schedule triggers it. Keeping the old allocation alive alone does not fix cache routing. A non-rendering UI tick must not be treated as proof that all outstanding draw/glyph references have drained.

## Feasible direction, not a verified implementation

A targeted font reload can potentially preserve pages and style font-pointer identity, but requires a font-specific transaction rather than image-cache retirement:

1. Preserve original family paths and compute affected families across old/new mapping generations, including theme removal. Existing registry retargeting does not provide reversible configuration reload.
2. Identify affected active records, wrappers and idle entries. Prepare replacement descriptors without allowing existing active/idle lookup to simply return the old font. Use immutable generation-specific paths: reusing a pathname can also reuse the native face cache.
3. Establish and test a native draw/glyph lifetime barrier, or implement generation-aware acquisition/release routing. Do not assume the image adapter's mutation gate supplies this barrier.
4. Only then commit affected font data while retaining wrapper identity, list links, record ownership/reference counts and each wrapper's fallback chain. Do not blindly overwrite all wrapper bytes.
5. Retire affected idle fonts and old backing descriptors through verified ownership paths. Failure before commit must leave old fonts usable; native allocation-failure behavior needs separate verification.
6. Refresh affected text styles and layout/measurement, not just screen pixels. Identify font property IDs, parts, inherited styles and custom text owners independently before claiming coverage.

This is an implementation direction, not confirmation that all required barriers or owners have been located. It intentionally avoids global font-manager destruction or arbitrary page teardown.

## Verification performed

```sh
RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.155 \
  build/firmware-tests/bin/python tests/firmware_font_lifecycle.py
RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.155 \
  build/firmware-tests/bin/python tests/firmware_font_retarget.py
```

Results: 4 lifecycle tests and 5 registry tests passed. Existing tests execute selected firmware paths with modeled allocation/list operations; they do not test the proposed transaction, concurrent drawing, physical rendering, layout adoption or GPU completion.

Runtime source is unchanged. Physical-device font reload remains NOT_PROBED. The initial release-routing observation is now covered by the follow-up probes below.

## Follow-up: pending glyph ownership and completion boundary

### Native ownership chain

The `.155` initializer `0x0c3a9598` registers a VG_LITE draw unit in the list at `0x200bd318`. Its dispatcher is `0x0c3913ec`, active task is unit+32, and pending glyph container is unit+48. The pending container is created with 24-byte elements and initial capacity 8; callback+36 is `0x0c399b62`, callback argument+40 is zero.

The container has two array descriptors at +4 and +20. Each array has buffer+0, count+4, capacity+8 and element-size+12. The native array accessor is `0x0c3a3d96`; these fields are not inferred from a host structure.

The VG glyph drawing path `0x0c39d038` increments the held cache entry through `0x0c908f4c`, then copies the 24-byte glyph description into the pending array through `0x0c908eb0`. The ordinary draw-letter path `0x0c381be4` subsequently releases its immediate reference through `0x0c399b4c`. Thus returning from the draw-letter callback does not release the extra GPU-side pending reference.

The pending callback `0x0c399b62` calls `0x0c399b4c`. Assembly confirms the latter passes the stored font in r0 and the glyph-description pointer in r1 to font+8. For the outline font that callback is `0x0c3a0992`, which performs the current-descriptor cache routing described above.

### Located drain operation

`0x0c399940` (`lv_vg_lite_finish`) performs:

1. Call lower-level `vg_lite_finish` at `0x0c92019c`.
2. Log/dump an error if the result is nonzero.
3. Drain gradient pending entries, image pending entries, and glyph pending entries, in that order.
4. Clear unit+52.

Each drain uses `0x0c395954`, which processes **both** arrays via `0x0c395920`; the latter invokes each stored release callback and clears its array count. The old font descriptor must remain installed throughout this drain.

`0x0c3998dc` is a different operation: it flushes after its submission counter exceeds 7 and rotates pending buffers via `0x0c39596c`. It is not equivalent to draining both buffers and must not be substituted for a reload barrier.

### Error handling prevents treating this as an unconditional barrier

The finish wrapper continues draining after a nonzero GPU result. Its return value is not a reliable GPU-success status. Empty arrays after this function are therefore insufficient evidence of successful completion.

There is a second caveat even below the wrapper: wait helper `0x0c91f624` issues driver operation 5 through `0x0c926874`. If waiting fails it calls recovery `0x0c91f5fc`, which issues operation 6 (GPU reset). Successful reset makes the wait helper return zero. Therefore a zero result can mean recovery, not successful rendering of the previous commands. Whether reset safely retires every relevant hardware resource needs its own validation; this investigation does not assume it.

The platform wait leaf `0x0c926d48` uses a timed semaphore wait and a 100 ms or 1 s deadline branch. Its timing and recovery must not be hidden in an interrupt-disabled section or treated as a cheap cache operation.

### CPU-side boundary evidence

- VG dispatcher `0x0c3913ec` and SW dispatcher `0x0c3948ac` invoke their render functions synchronously, marking unit+32 while active and clearing it afterwards. No worker dispatch occurs in these inspected dispatcher paths.
- VG dispatch returns zero immediately when its active-task field is nonzero; it does not drain pending data in that case.
- Draw-task dispatcher `0x0c381484` traverses the draw-unit list. Refresh-area function `0x0c389328` waits for the layer task list at layer+100 to become empty before handing the buffer to the display flush callback. The display refresh timer is `0x0c38956c`.

These are useful static boundaries, not proof that every custom graphics caller uses the same thread or that display rendering flags alone certify GPU completion.

### Implementation decision

A native mechanism to release deferred glyph references **does exist**, so missing release machinery is no longer the blocker. A plausible in-place reload transaction can now be specified more concretely:

- Run on the serialized UI owner outside rendering and outside IRQ locks; validate draw-unit identity, inactive tasks and absence of queued old-font drawing work.
- Preserve old wrappers/descriptors while completing submitted GPU work and draining both pending arrays.
- Distinguish normal completion from errors/recovery. On unverified completion or recovery, do not swap or free old font backing data. Calling the existing finish wrapper alone is not sufficient to implement this fail-closed policy.
- Verify pending counts and relevant ownership invariants before the font transaction commits, then perform the prepared replacement and text-layout refresh.

This is a **conditional barrier design**, not a production implementation. Remaining gates are strict error/recovery observation, coverage of other font consumers/custom graphics paths, and transactional replacement/layout refresh. The dispatcher/drain evidence does not establish global device quiescence or authorize a global GPU reset.

### Added executable probes

`tests/firmware_font_barrier_155.py` is deliberately `.155`-only and rejects other targets. The existing harness verifies the full AP fingerprint. It executes native release/drain/dispatcher/wait-helper instructions with GPU driver, logger, memory-clear and cache-release leaves modeled where documented.

Six tests passed:

1. No swap: an old glyph entry is released through the old cache.
2. Early descriptor swap: that same old entry is passed to the new cache, reproducing the routing hazard.
3. Successful modeled finish: both pending buffers release through the old cache before a subsequent descriptor swap.
4. Active dispatcher: returns without draining pending glyphs.
5. GPU error: the native wrapper still drains both arrays.
6. Wait failure followed by successful modeled GPU reset: the native wait helper returns zero, demonstrating recovery masking.

```sh
RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.155 \
  build/firmware-tests/bin/python tests/firmware_font_barrier_155.py
```

The existing 4 lifecycle and 5 registry tests also passed again (15 total at this stage). These probes validate firmware control/data flow, not real GPU synchronization, cache integrity under concurrent use, display correctness or a completed font-reload implementation.

## Further investigation: lower reset path and additional text owners

### An upper reset observer would miss the first reset

The timed wait at `0x0c926d48` calls semaphore wait `0x0c349628`. On failure, before returning -1, it calls clock setup `0x0c926d00` and hardware reset/control routine `0x0c921e54(0)`. That control routine writes GPU registers and loops until register+4 satisfies mask `0x0b05`. There is no bounded retry exit in that branch. Upper wait recovery `0x0c91f5fc` occurs later.

Therefore observing only driver operation 6 or `0x0c91f5fc` is not sufficient to detect all recovery. A strict synchronous-completion design would have to observe the lower timeout/reset path as well, and still handle reset/control initiated by `0x0c926f10`. The latter performs GPU power/recovery control outside the direct finish call chain. No monotonic, reliable recovery counter has been established by this investigation. Do not derive one from the busy flag: `0x0c91f5fc` clears `0x200f537c` before its reset result is known.

This makes adding an unconditional native finish call to every font reload unattractive: it can invoke reset and an unbounded hardware-status loop, not merely wait for a cheap software event.

### Vector-label owners need separate refresh

The initialized custom vector-label class at `0x2ca6ee48` has base class `0x2ca177e0`, constructor `0x0c6a1260`, destructor `0x0c6a132c` and event handler `0x0c6a5518`.

- Constructor/init establishes a separate path cache at `0x200d3280` and discovers the VG_LITE unit by name into `0x200d327c`.
- Cache comparator `0x0c69feb0` compares only key+4, the object pointer. Font identity, theme generation and text contents are not part of comparison.
- `0x0c6a2bf4` first looks up that object-keyed cache. On a miss it rebuilds text paths from current font/style/text. Thus changing the backing font alone does not force a miss.
- Draw-event path `0x0c6a5518` acquires the cached geometry, renders it through `0x0c6a16e0`, and releases the cache reference. The latter directly submits vector paths to VG_LITE and uses its flush accounting; this is an additional text drawing path beyond ordinary glyph pending arrays.
- `0x0c6a1304(object)` builds a zeroed 16-byte key with object at +4 and calls native per-key cache drop. It does not globally clear the vector cache.
- `0x0c6a5014` (`lvx_vector_label_refresh`) drops the object key, refreshes label data, checks supported glyph formats, and attempts to regenerate cached paths. This is the stronger owner-specific refresh candidate.
- Cache destructor `0x0c6a12d4` destroys stored paths and frees their array. Its safe timing relative to submitted custom vector draws must be respected; locating the drop operation is not proof it is safe to invoke at arbitrary times.

The vector-label handler delegates non-draw events to its superclass in the inspected path; a generic font-style notification is not itself evidence that this separate object-keyed cache was invalidated. Explicit vector-owner coverage is needed.

### Font style/layout notification is now identified

`0x0c3828ac` delegates to style getter `0x0c382620` with property **90**, the font property. The property-flags table at `0x2ca147fa` gives property 90 flags `0x05`.

With global style refresh enabled (`0x200bd210 != 0`), `0x0c38525c(object, main-part selector 0, property 90)` performs invalidation, emits event 45, marks object/parent layout, and invokes the inherited-child refresh path `0x0c384668`. This gives a native way to notify a same-pointer font-data change without relying on an unchanged-value setter to trigger work. Other parts, selectors, fallback fonts and custom owners still require coverage.

### Alternative worth pursuing: preserve the old generation for pending releases

The earlier wait-before-swap scheme is not the only possibility. Native creation already keeps an underlying font in `record+0` and copies its 36-byte font payload into each page-facing wrapper. The underlying old font can remain stable while the wrapper is updated.

For the inspected VG_LITE pending-glyph buffers, the queued 24-byte glyph description stores its release font pointer at +0. A candidate transaction can redirect only affected queued descriptions from the page-facing wrapper to its **retained original underlying font**, before changing that wrapper. The queued glyph entry is not released early and its geometry does not change. Subsequent native completion follows the old underlying descriptor/cache, while newly generated glyphs follow the updated wrapper and new descriptor/cache.

An added native-instruction probe verifies exactly this routing: after rewriting the queued release-font pointers and swapping the wrapper descriptor, a new glyph releases through the new cache, and both old queued glyphs later release through the old cache. The test models GPU finish and cache-release leaves; it does not test a concurrent GPU or implement the transaction.

This is potentially preferable to forcing GPU waits/resets at switch time, but it is **not yet a safe production algorithm**. It requires:

1. Serialized UI mutation with no active callback/queued CPU draw work retaining an unconverted old glyph description.
2. A complete bounded scan of relevant pending buffers; unknown units/layouts must abort before mutation.
3. Strong ownership of the old underlying descriptor until every delayed release has finished, with bounded generations, rollback and reclamation tests. Merely retaining a dangling font pointer is insufficient.
4. Separate handling of vector-label cached geometry and any other custom owners. Rewriting ordinary pending glyph pointers does not cover vector-path lifetime.
5. Transactional wrapper/record/reference-count updates and preservation of per-wrapper fallback chains.

No pointer rewrite, reset observer or font-generation mechanism has been added to the runtime module. This is a tested release-routing building block, not a claim that all font lifetimes are solved.

### Expanded probes

The `.155`-only probe now has **11 passing tests**, adding:

- Lower timed-wait failure invokes hardware reset before returning to upper recovery.
- Vector-cache comparison ignores font/content fields and keys on the object.
- Vector-cache drop constructs an exact per-object key.
- Native font-property refresh emits the expected layout/inheritance notifications (event and layout leaves modeled).
- Retained old backing-font routing permits old/new glyph releases to use separate caches after a wrapper swap (completion/cache leaves modeled).

The existing 4 lifecycle and 5 registry tests also passed again: **20 tests total at this stage**. No physical-device font reload or global concurrency guarantee is claimed.

## Ownership follow-up: reclamation, copied vector paths and allocation failures

### Keep the descriptor/face alive, not just its glyph entries

Native cache release `0x0c8b9780` decrements entry+4. If the count reaches zero and entry+12 marks it invalid, it calls cache+24 to destroy the payload, then frees the entry storage through `0x0c3a387a`. A saved entry pointer can become invalid during release; a reload implementation must not poll stale entry pointers after native completion.

Outline/bitmap releases `0x0c3a0992` / `0x0c3a097a` release a child glyph entry, not the descriptor's face-cache reference at descriptor+56. A new test executes the real entry-decrement path while rotating the two pending arrays: one array drains while the other still holds its glyph, and the face reference remains held after both glyph counts reach zero. This gives the proposed retired generation a distinct ownership reference to retain until collection.

However, **whole-cache destruction is not reference-safe retirement**:

- `0x0c3a384c` invokes class+8 then frees the cache itself.
- Both inspected cache classes dispatch through `0x0c3a06b6` to `0x0c3a7918`.
- `0x0c3a7918` logs held entries and skips their payload destructor, but still frees their RB payload/node storage during tree teardown.
- Face destruction `0x0c396b24` destroys its child glyph/outline caches.

A native-instruction counterexample confirms that destroying a cache containing a held entry still frees the entry backing allocation and the cache. Consequently, a retired generation must preserve the **old descriptor and its face-cache ownership**, not simply rely on pending glyph refcounts to keep the entire hierarchy alive. Do not place still-needed old descriptors in the ordinary idle cache where native capacity eviction could destroy them.

### Narrowed candidate reclamation rule

For the inspected managed-font and VG_LITE path, the proposed collection condition is:

1. Existing wrappers have committed to new backing fonts, while their per-wrapper fallback chains and manager record/refcount relationships remain valid.
2. Future active/idle lookup can no longer return the retired generation.
3. On the serialized UI owner, neither live pending array contains a glyph whose release font is that retained backing font. Inspect current array contents, not stale saved entry pointers.
4. No CPU draw callback/task or separately identified consumer still owns an old description. Both buffers and all supported units must be accounted for.
5. Only then release the retained descriptor through an independently validated native teardown sequence, allowing its face-cache reference to drop normally.

This rule remains conditional on consumer coverage; array emptiness by itself is not a global lifetime proof. The recorded GPU error/recovery concerns also remain relevant to abnormal native completion. Do not claim that a corrupted or reset-in-progress graphics subsystem is made safe by these checks.

For bounded operation, a reasonable implementation policy is to allow at most one retired generation and coalesce later switch requests until it is collected. If collection cannot be proven, keep the current usable font and defer additional switching rather than accumulating retained generations indefinitely. This is a proposed policy, not implemented state.

### Vector labels deep-copy original glyph outlines

Further instruction inspection narrows the custom vector-label hazard:

- Build callback `0x0c6a27d0` (inspected as instructions; not a recognized IDA function) copies each original glyph path into a label-owned path via `0x0c397e48`, then transforms the copied points.
- `0x0c6a1338` creates the label-owned path through `0x0c3975e0`; the latter uses `0x0c92227c` to initialize it with upload flag at path+36 clear.
- `0x0c397e48` appends path+40 bytes from source path+44 into the destination's own allocation. It does not copy source path flags or take ownership of the source allocation.
- Build finalization uses `0x0c3978bc`, not the FreeType outline upload-finalization branch.

A probe confirms that appending an uploaded glyph path leaves the destination upload flag clear and copies independent point bytes.

GPU submission `0x0c922478` distinguishes two representations:

- Upload flag clear: helper `0x0c91f950` copies the path bytes into the GPU command buffer, using an inline-data command (`0x40000000` family).
- Upload flag set: helper `0x0c91f8c4` writes the uploaded address into a command (`0x60000000` family); that allocation must remain valid until consumption.

Two native instruction probes confirm these distinct encodings, and that mutating the source after inline submission does not change the copied command payload. Capacity checks are executed with sufficient modeled command-buffer space; the overflow/submit/wait branch is not covered by these probes.

Thus the inspected vector-label cache contains independent, ordinarily inline-submitted paths rather than pointers into the original FreeType outline allocation. This substantially narrows the coupling between its cache and retired font descriptors. After CPU submission is finished, rebuilding that label's copied geometry need not automatically imply retaining the original font for GPU use. Unknown/modified/uploaded path variants must still be rejected or handled separately; the primitive tests do not execute a complete label render or every transformation path.

By contrast, FreeType's outline event handler `0x0c3981d0` explicitly sets path+36 bit 0 and prepares an uploaded command block. Ordinary glyph pending references remain necessary for those uploaded paths.

### Native allocation is not a rollback-safe transaction

`0x0c494380` increments an existing record's wrapper reference count before allocating the new wrapper through `0x0c3a43b0`. The allocator returns zero on allocation failure, but the caller does not check it before zeroing/copying into the wrapper.

A new fault-injection probe makes the native list allocator's heap leaf return zero. With the clear leaf suppressed to expose the caller's subsequent store, native wrapper copy performs an unmapped write through zero. The record count remains incremented. This is a controlled emulator counterexample, not an observed physical crash.

The new-descriptor branch also allocates 64 bytes through `0x0c3b0de4` and writes fields without a caller-side null check. The error-handled pathname/cache-creation branches must not be mistaken for complete OOM handling.

Therefore “prepare a new native wrapper and roll back if it returns NULL” is not an adequate implementation strategy. Existing page wrappers should be retained; any preparation path that reuses native creation must either establish checked allocation/cleanup for its exact subpaths or use a separately verified checked factory. Sampling available heap alone does not guarantee allocation success. No such checked factory has been implemented here.

### Verification update

The `.155`-only probe now has **17 passing tests**. New checks cover:

- Whole-cache destruction frees backing storage even with an outstanding entry reference.
- Rotating one pending array leaves the other live and does not release descriptor/face ownership.
- Vector-path append copies bytes without propagating the uploaded flag.
- Inline submission copies path bytes, while uploaded submission records an address.
- Native wrapper OOM does not roll back the record count and proceeds to an invalid store.

Together with the unchanged 4 lifecycle and 5 registry tests, **26 tests passed** at this checkpoint. GPU work, allocation, copy and callback leaves are modeled as specified in each fixture. Runtime code is still unchanged; real-device repeated reload, memory pressure and full owner coverage remain unverified.

## Final closure: preparation, commit and support boundary

### Fresh font identity and reversible paths

Face-cache comparator `0x0c396784` orders rendering mode (+6), style (+4), then pathname **contents**, not pathname-pointer identity or requested pixel size. An instruction test uses two separately allocated identical paths and different +8 values: they compare equal. Changing the pathname text or style makes them unequal.

Consequences:

- Allocating a second path string, changing a registry pointer, or requesting another font size cannot force a fresh face for the same pathname.
- Each changed file needs an immutable generation-specific native filename, not an overwrite of the file still backing an old face. Distinct sizes of the same path/style/mode share a face and its child caches.
- Keep generation files available until all applicable descriptors/face-cache ownership is gone; do not infer filesystem lifetime from the GPU queue or assume unlink/open-file behavior without testing the target filesystem.
- Capture an owned original-family/path baseline **before the first retarget**, and resolve every new mapping from that baseline. This enables A -> B -> stock and rule removal. The existing `src/module.c` correctly skips font retargeting on configuration reload because it lacks this baseline.
- Do not infer original paths by reversing arbitrary mapping rules after earlier destructive retargeting. If the baseline is already lost, automatic restoration is unsupported until it is re-established.

The current remove/add registry sequence is not transactional. `0x0c4924e0` allocates a list node and both strings without checking every allocation; `0x0c904cec` frees the original strings. A future adapter should stage checked native-heap path copies, validate registry identity, then replace existing path slots without deleting and recreating the whole registry entry. Keep the baseline separately; use native-compatible allocation/free for memory later consumed by firmware teardown.

### A checked factory is feasible, but has to be written

Additional recovered primitives:

| Address | Relevant contract |
|---|---|
| `0x0c3abe20` / `0x0c3abe58` | LVGL allocation/free wrappers over native heap `0x0c351724` / `0x0c34cd2c`; positive-size allocation can return zero |
| `0x0c3a7a30` | Generic cache constructor; writes cache fields before checking the allocator result |
| `0x0c3a9d78` | Count-cache allocator; requests 64 bytes and returns zero on failure |
| `0x0c3a9e48` | Count-cache initializer for already allocated storage; initializes RB/list fields without allocating |
| `0x0c3a9df4` | RB initializer; requires nonzero comparison callback and node size |
| `0x0c3967b4` | Face-create callback; handles FT/open/parse error paths, with optional 17,408-byte glyph-L1 allocation allowed to fail |
| `0x0c3a7b78` | Cache acquire-or-create; failed create callback unlinks/frees metadata and returns zero |
| `0x0c8b8a54` | Pixel-size setup; returns an error that preparation must check rather than copying native caller behavior |

A second fault-injection test confirms the generic cache constructor also writes through a failed allocation. Conversely, with checked/zeroed 64-byte storage and valid callback fields, the native initializer establishes RB payload size `node_size + 20`, empty lists and the expected size callback, without any allocation. This identifies a way around the unsafe constructor; it is not a complete factory test.

Recommended factory contract:

1. Accept only the audited FreeType descriptor/callback layout and supported render mode; reject built-in, unknown, zero-size and unsupported font formats instead of coercing them.
2. Check every owned allocation: immutable path copy, face-ID list node, 64-byte descriptor, child-cache objects and transaction bookkeeping. Validate counters before incrementing them. Preallocate where possible; do not use available-heap sampling as a guarantee.
3. Intern the path with checked insertion, then acquire/create the native face. Track whether it was newly created or already shared. The native create callback owns its own partial FT/open failure cleanup; generic failed-acquire metadata cleanup is separately instruction-tested with that callback modeled.
4. Install fully initialized child caches for a new face; check size/metrics setup and build the complete replacement backing font. Do not allocate a new manager wrapper just to obtain it.
5. On failure unwind exactly the acquired ownership, in reverse order. A newly published but partially initialized face must be dropped, not left cached with missing child caches or a dangling path key. Never drop a shared live face or release its path too early.
6. No live wrapper, active record or registry pointer changes until the whole affected set is ready. Failure here leaves old fonts and paths intact. Failure in later ordinary glyph rendering is outside this factory guarantee.

This requires a small exact-target checked adapter, not replacing FreeType itself. Its full allocation-failure matrix, actual valid/corrupt TTF parsing and rollback balance are implementation acceptance work, **not already passing tests**.

### Fallback and manager ownership are part of the commit

`0x0c4950b0` creates a primary wrapper, reads family configuration at manager+548, creates fallback wrappers through `0x0c494380`, then links them at wrapper+28. `0x0c49258c` destroys each fallback through `0x0c917384`, clears the primary fallback link, and finally destroys the primary. A probe confirms this order; another confirms that built-in default `0x2ca14888` is excluded from manager destruction.

Therefore:

- Operate on affected records and **all** wrappers that reference those records, including fallback-only fonts. A primary family whose own file is unchanged can still display glyphs from a changed fallback.
- Replace each active record's backing font as well as the corresponding wrappers. Otherwise subsequent wrapper creation can resurrect the old descriptor from record+0.
- Preserve every wrapper's +28 fallback link, +32 user-data word, +36 record identity and intrusive list links. Do not memcpy all 36 backing-font bytes over a live wrapper. The audited callback/metrics/descriptor portion ends before +28.
- Preserve active-record keys and reference counts. Snapshot/validate membership and ownership immediately before commit. Reject unexpected layouts, unsupported descriptor kinds and ambiguous/truncated family matches.
- Retire or update affected idle entries too; otherwise a later page opening can restore an old font. Shared face references must be accounted per descriptor, not per family, size or pending glyph.
- Existing family/fallback policy is preserved; hot reload replaces font resources, not the family configuration graph. Do not replace the default font, emoji/bitmap-specific paths or arbitrary app-owned copies by assumption.

### Prefer the natural boundary; do not invent a stronger barrier

The final normal draw dispatch already provides the ordinary drain opportunity. A new instruction probe starts `0x0c381484` with a completed final task, a VG_LITE unit and both pending glyph buffers populated. It removes the task, calls the now-idle VG dispatcher, enters the existing finish path, and releases both buffers to the old cache. No reload-triggered finish call is added.

`0x0c3ac518` invokes LVGL timer callbacks synchronously. The miwear timer wrapper `0x0c4fb31c` calls it and reschedules; this gives a concrete scheduling location for a **UI-owner-only** transaction, rather than doing it opportunistically from any activation/poll caller. Its reentrancy byte is not a cross-thread mutex or a GPU-success certificate.

For a restricted first implementation:

- Queue/coalesce a request; do not mutate fonts from an arbitrary activation thread.
- In the supported UI callback, validate the draw-unit/layer/task state, both pending buffers and other audited glyph holds. If busy, leave existing fonts untouched and retry at a later natural boundary; do not force finish/reset or drain buffers from the module.
- Keep preparation and publication bounded and serialized. Prefer not retaining native preparation objects across UI turns; reject/revalidate stale snapshots if staging is split.
- Once a verified supported boundary is available, publish all registry/record/wrapper changes without allocation or reentrant UI callbacks, then retire old idle/backing ownership and refresh owners.
- A passive empty-queue check alone remains insufficient under GPU errors. The error/reset history cannot be reconstructed from the queue counters or finish return value. A production safety claim needs completion/recovery instrumentation and target testing, or must explicitly exclude that condition. This investigation has not supplied such instrumentation.

This avoids making the earlier old-backing retarget experiment the default implementation. That experiment remains valid for the tested pending-release route, but supporting it in production would additionally require a private retirement ledger, tracking every outstanding consumer, bounded generations and teardown integration. “One old generation” is a bound, not proof of safe retirement.

### Text refresh and teardown limits

For ordinary style-driven text, use the recovered text-font property refresh (`90`) so measurement, wrapping, layout and inherited values are notified. Include cached/inactive screens and relevant parts, not only a visible screen's main label. The earlier tests prove a selected inherited-style notification path, not every widget's layout implementation.

For audited vector labels, drop the per-object geometry cache and request font-style refresh/rebuild. Traverse with the same caution as image owner refresh: callbacks can mutate or destroy the tree, so do not keep dereferencing stale child pointers after notification. Full redraw alone is not sufficient.

A canvas containing previously rasterized text, a custom owner caching measurements, a separately uploaded GPU path, a copied font object outside manager ownership, or an unknown font backend requires its own invalidation/ownership contract. There is no discovered universal operation that makes those owners safe. Exclude them from the first support claim rather than silently declaring the whole UI refreshed.

`0x0c494198` warns about live manager resources but still clears the uikit global and calls `0x0c3a9f40`. The latter destroys the global face cache and FreeType context, then clears `0x200bd3ec`. A module-held pointer/reference does **not** keep those globals alive. No preparation/retirement pointer may be reused across this teardown without an explicit lifecycle contract. The first implementation should reject framework shutdown/reinitialization and cancel its work before teardown; a reliable lifecycle hook or epoch must be supplied before claiming restart-safe hot reload. Merely comparing a possibly reused context address does not solve this.

### Implementation decision and acceptance gates

| Area | Investigation result | Still required before shipping |
|---|---|---|
| File identity and restore | Keys and irreversible current behavior identified | Original-path ledger, immutable generation files, A -> B -> stock tests |
| Preparation/OOM | Unsafe constructors and usable lower-level primitives identified | Checked factory and complete fault-injection/rollback suite |
| Active/idle/fallback ownership | Record/wrapper/face and fallback contracts identified | Transaction implementation and reference-balance tests |
| Ordinary drawing | Normal end-of-dispatch drain demonstrated | Whole-transaction scheduling and supported-consumer coverage |
| GPU error/recovery | Empty/success status shown insufficient | Exact completion/recovery instrumentation or explicit unsupported gate; physical-device tests |
| Text owners | Ordinary font-property and vector-cache paths identified | Widget/part/hidden-screen integration and visual tests; custom owners excluded |
| Framework lifecycle | Global teardown invalidates retained native ownership | Reliable cancellation/lifecycle integration, not a pointer-equality guess |

These are implementation and target-validation gates, not a claim that further generic cache flushing will solve the problem. A controlled `.155` prototype can now be scoped without another open-ended search. **Do not enable a general production hot-reload path just by binding the addresses in this document.** No conclusion is transferred to `.139` or other targets.

### Final verification

The `.155`-only probe has **24 passing tests**, including the normal final-dispatch drain, face-key identity, fallback destruction, built-in exclusion, cache-constructor OOM, checked-storage initialization and failed-create metadata cleanup. Together with **4 lifecycle + 5 registry tests: 33 passing tests**.

These execute selected native instructions with the heap/GPU/I/O or callback leaves modeled as each fixture specifies. They do not run a complete replacement transaction, the actual GPU, full font-file parsing, concurrent owners or firmware restart. Runtime sources remain unchanged; device hot reload remains **NOT_PROBED**.

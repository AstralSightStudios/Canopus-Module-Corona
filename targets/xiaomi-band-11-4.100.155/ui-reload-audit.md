# Current reload implementation: affected sources and verified owners

This section supersedes the historical migration report below. The original
report remains an evidence record, **not a description of current activation**.
In particular, neither all-entry retirement nor forced page recreation remains
in the module. The unused platform page-rebuild helper was removed as well.

## Exact .155 adapter evidence

AP SHA256 `ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f`.
Instruction inspection used Capstone against this exact AP; no address delta,
stale symbol name, or .139 structure assumption was used. Callable C constants
carry the Thumb bit. `tests/firmware_reload.py` verifies the fingerprint before
loading firmware, and compiles the current source into a temporary unsigned ELF.

| Entry/layout | Witness in this AP |
|---|---|
| `lv_image_set_src` `0x0c3b2c28` | info query at +0x40 precedes pointer comparison at `0x0c3b2d36`; same-pointer branch goes to width/height stores at `0x0c3b2cd8` and sizing/invalidation calls |
| image class `0x2ca14ca8` | literal at `0x0c3b3050`; class contains constructor `0x0c3ab59d`, destructor `0x0c3abff3`, image event handler `0x0c3b2de5`; constructor and event handler agree on source +52, width/height +68/+72, type bits at +96 |
| object class pointer at +0 | class destructor `0x0c37f740` and constructor dispatch `0x0c37f760` load/use it; only exact image class is accepted, not derived classes |
| get-info `0x0c38e4e4` | 12-byte header; calls `0x0c38e394`; cache-hit branch `0x0c38e3d6..0x0c38e3f4` copies header and releases acquired reference |
| enabled-header-cache preflight | `0x0c38e3b4` reads capacity at cache+8; with nonzero capacity, `0x0c38e47c..0x0c38e4b8` must insert successfully before get-info reports success; insertion failure returns zero. Disabled header caches are not used for metadata reapplication |
| header/decoded key source/type | `0x0c3a3888` constructs header `{src@0,type@4}` and decoded `{src@4,type@8}`; source-type `0x0c381464` returns 1 for file, 0 for descriptor, 2 for symbol, 3 for null |
| LRU link payload/next | initializers `0x0c3a9e48` / `0x0c3a9eb0` set cache+48 to 4; `0x0c3a46dc` reads next at node+payload_size+4; list payload -> RB node -> data+16 established in original migration evidence |
| native tree walk `0x0c380574` | null root walks display list (`0x200bd1e8+12`), screens at display+692/count+720; nonnull root invokes callback then child accessor `0x0c380320` and count `0x0c380342`; callback 2 prunes/stops a subtree |
| parent +4, child layout | screen accessor `0x0c3802a8` follows parent+4; child accessors load object+8 spec_attr, children+0/count+48. Adapter only reads parent for a bounded depth guard; native walk owns child enumeration |
| style getter `0x0c382620` | `(object,part,property)` returns value in r0; merges object state at +48, delegates to `0x0c382540`, which selects matching main-part state precedence |
| explicit style refresh `0x0c38525c` | `(object,selector,property)` used by native local-style setter `0x0c3872b4` after a reported change; direct call avoids reliance on unchanged-pointer setter behavior |
| property 40 on system buttons | native helper `0x0c387d1c` passes property 40 to local-style setter; system image-button setter `0x0c69606c` supplies main-part selectors 0/32/128 (expanded investigation) |

## Current behavior and limits

- Both exact classes/lists are validated before retirement. Only file keys whose
  absolute `/` path resolves through the resident immutable mapping are dropped.
  Non-head entries and native LRU lookup reordering are supported. The next link
  is captured before drop; removed node/key/source are never dereferenced after
  it. A bounded live-list scan checks unlink progress. Each list is capped at
  4096 entries; unknown layouts/cycles/no-progress keep the request pending.
- Native per-key drop/release, not a module free, owns held payload lifetime.
  Header retirement precedes decoded retirement and all owner refreshes.
- Owner scan covers registered screen trees, including offscreen screens. It
  takes a temporary snapshot (1024 objects, parent depth 32), then revalidates
  membership without dereferencing stale candidates before each operation.
  Native deleting-bit subtrees (`object+51 & 16`, set at `0x0c380404`) are
  skipped. No object/source pointer survives a UI tick. Allocation/size/depth failure
  leaves owner refresh pending without partially mutating the snapshot.
- Exact image-class affected file sources use successful get-info preflight and
  same-pointer native setter; source-string churn is avoided. Header-cache
  disabled/failed-info/source-changed cases skip the setter. This depends on
  serialized UI ownership and immutable theme files; ordinary native setter
  callback rules still apply (do not delete the receiver or invalidate its
  source/header during that setter). This is not transactional decoder failure
  rollback or protection against arbitrary reentrant owner callbacks.
- Current effective **main-part property 40** is refreshed explicitly on all
  affected objects, not only image widgets. Styles/selectors/state are unchanged.
  Other parts/properties and custom owner caches are not covered.
- No watchface hook, global animation stop, global cache clear, GPU finish call,
  or page rebuild. Deferred VG_LITE descriptors live in native pending arrays;
  non-rendering status is only an invalidation/mutation gate, **not GPU idle**.
- `.139` keeps path/font-registry redirection and repaint, but both new image
  adapters return unsupported without reading cache/object layouts. Its old
  unaudited direct-key/global traversal was removed, not silently preserved.
- RHQ1 remains v5/40 bytes. `images_dropped` counts completed targeted retirement
  rounds (.139: zero), including empty affected sets; `rebuilds` is reserved zero.
  `redraws` means accepted dirty area, not verified resource adoption. Failed
  per-object info queries and unsupported owner classes are not separately
  exposed by v5; a completed round is not universal replacement success.
- Font registry retargeting remains, but active/idle wrapper reuse can bypass it.
  No live-font eviction, arbitrary page teardown or rollback was introduced.

## Focused verification

`tests/firmware_reload.py`: **18 tests passed** against fingerprinted .155 AP.
The actual native generic drop/release, screen walk/accessors, image setter,
get-info cache-hit path, state/selector style lookup and explicit style-refresh
instructions execute. Native I/O/decoder callbacks, cache lookup/unlink/free,
heap functions, style property storage and GUI event/sizing leaves are modeled
as described per test. A poisoned removed node/later-deleted object catches
post-drop/stale-snapshot dereferences. Tests cover both cache types, unrelated,
invalid/overlong and non-file sources, held/unheld entries, class/layout/cycle/
progress guards, dimensions, unchanged source identity, failed/disabled info,
source changes, main-part states 0/32/128, offscreen trees, allocation/size/depth
limits, and compiled .139 unsupported guards (not .139 AP emulation).

`tests/test_module.c` additionally tests immutable snapshot sharing, no page
rebuild, coalescing, stage retries, unsupported-adapter retirement accounting,
OOM scheduling and v5 status compatibility with ASan/UBSan.

```sh
RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.155 \
  CLANG=/path/to/arm-capable/clang LD_LLD=/path/to/ld.lld \
  build/firmware-tests/bin/python tests/firmware_reload.py
```

The older `platform-probe.py` now retains only unrelated platform entry/allocator/
redraw/font/timer checks; new cache/owner probes above supersede its former
all-entry traversal and page-rebuild sections. To link that probe, also compile
and link `src/resource_hook.c` (the adapter uses the shared resolver), and use
`rh_platform_retire_images` rather than the removed global-drop symbol as entry.

## User-reported hardware observation (.155 only)

The user reports successful installation of the current signed module using the
local temporary `~/develop/temp/settings-icon-installer-155` installer, including
its execute recovery path and directory creation (`mkdir`). The theme setup under
`/data/canopus/themes/` with `/data/canopus/themes/mappings.tsv` resulted in the
custom **Settings launcher icon visibly appearing on the .155 device**. This is
an attributed user observation, not a new instrumented hardware run or a claim
that the generic installer implements the temporary installer's workflow. See
[the installation record](../../docs/INSTALL.md#155-用户报告的单项实机记录).

**Broader hardware gates remain NOT_PROBED:** actual UI caller/callback lifetime,
filesystem immutability/fallback, deferred decoder/GPU completion under pressure,
changed dimensions/layout, animation intent, styles across navigation/state,
offscreen adoption, fonts, and repeated activation memory/watchdog behavior.
One visible icon does not establish acceptance for every owner or for .139.
Signed loading/build verification is separate from this unsigned adapter probe.

---

## Historical migration report (superseded implementation details below)

# Band 11 4.100.155 platform audit

## Identity, method and scope

- Exact AP: `build/firmware-analysis/vela_ap_4.100.155.bin`
- SHA-256: `ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f`
- Existing database: `build/firmware-analysis/vela_arm_mapped.i64`
- IDA 9.4 idalib/Hex-Rays: direct decompilation, cross-references and instruction inspection. Analysis-only instruction creation was not saved back to the database.
- XIP file offset = address minus `0x0c0c0000`; cached-rodata offset = address minus `0x2c0c0000`. Recovered function chunks and inspected instruction ranges were compared byte-for-byte against the exact AP, not inferred from a neighboring target's symbol list. `evidence.json` records offsets, SHA-256 and comparison results.
- RAM BSS bytes in IDA are not live-state evidence. Global identities below come from executable loads/stores and initialization. The initialized page-layer byte was additionally matched to the startup-described RAM copy.

This is a **static platform migration and bounded emulation result**, not physical-device acceptance, a universal refresh contract, or proof that restarting miwear is safe. `RH_TARGET_155` selects `include/resource_hook_target.h`; absence of that macro or explicit `RH_TARGET_155=0` retains .139. Use matching Canopus generated target headers for the shared memory helpers.

## All platform dependencies

Addresses in this table are even instruction entries. The C header adds the Thumb bit for calls.

| Dependency | .155 | Independent witness |
|---|---|---|
| native open | `0x0c342c54` | native wrapper -> `0x0c342bd8`; negative return becomes errno/-1; POSIX driver calls this entry |
| native read | `0x0c33d784` | builds `{buffer,size}` iovec, calls `0x0c33d740` with count 1 |
| native write | `0x0c33dc4e` | builds `{buffer,size}` iovec, calls `0x0c33dc0a` with count 1 |
| native close | `0x0c33818c` | close wrapper -> `0x0c334b2c`, errno conversion |
| original LVGL POSIX open | `0x0c3a6194` | mode translation, `"/%s"`, native open, fd+1/zero convention |
| POSIX driver/cache-size/open-slot | `0x200bd3b8 / 0x200bd3bc / 0x200bd3c4` | `lv_init` `0x0c3a9598`, stores at `0x0c3a9bcc..0x0c3a9bde`: `'/'`, 4096, original open; slot is driver+12 |
| per-key cache drop | `0x0c8b8c9e` | class+12 lookup; held entry refcount+4 -> invalid+12=1 then class+20 unlink; unheld entry additionally freed |
| image/header cache slots | `0x200bd310 / 0x200bd314` | `lv_init` creates and stores both separately; also referenced by `0x0c3a3888` |
| image/header class | `0x2ca168b4 / 0x2ca16934` | `lv_init` supplies these objects for `"IMAGE"`/`"IMAGE_HEADER"`; each has 24-byte key/data payload; exact 40-byte vtables differ only at +4 |
| default display | `0x200bd200` | active-screen/resolution/invalidation accessors load this global |
| active-screen layout | `display+696` | `0x0c3807ec` reads +696, not DPI at +24 |
| rendering/invalidation guards | `display+58 & 2`, signed `display+608 > 0` | `_lv_inv_area` `0x0c382428`, `lv_display_is_invalidation_enabled` `0x0c380b2c` |
| invalidate area | `0x0c382428` | asserts outside rendering, clips via native width/height, retains 16-byte areas at +60 with count at +604, capacity 32 |
| width/height | `0x0c380694 / 0x0c3806b4` | reads display+0/+4 with swap on display+756 bit 1 |
| page pop/resume/top | `0x0c696e24 / 0x0c696c08 / 0x0c697440` | named lifecycle log strings, state ladder, top helper delegates to `0x0c697318` where negative index adds page count |
| screen/page-layer gates | `0x200c2a28 == 2 / 0x20096085 != 0` | resume itself tests these exact globals |
| page fields | async +36, state +40, policy +41, root +48 | pop/destroy/resume/create instruction paths; details below |
| uikit/font registry | `*0x200bd1e8 + 28` | native remove/add and resolver; manager payload size+24, head+28; node name+0/path+4, next at node+payload+4 |
| font remove | `0x0c904cec` | validates registry membership, unlinks, frees name/path/node |
| font add/resolve | `0x0c4924e0 / 0x0c490edc` | appends entry; first family-name match wins; fallback `"%s/%s.ttf"` |
| timer create/delete | `0x0c3abd20 / 0x0c3abe70` | create stores period+0/callback+8/user-data+12 and links timer; delete unlinks and frees |

Only cache-drop, both classes, page pop/resume/top and font-remove move relative to the platform's .139 constants, each by -16. All other table identities were checked independently; there is no blanket relocation rule.

### Shared allocator and CPU primitives

The platform calls `b11_temp_alloc(8,size)`/`b11_temp_free`, not the unused MPU or code-publication helpers in the same SDK header. The actual dependencies were checked:

- `B11_UMEM_SLOT=0x200b2590`: `nx_start` `0x0c357f9c` assigns `mm_initialize("Umem",0x3c356b40,13210304)`. `0x0c34ef08` rounds base to 8 and returns it, establishing `B11_UMEM_DESCRIPTOR=0x3c356b40`.
- `mm_mallinfo=0x0c34f0a0` writes a 28-byte structure through r0, heap in r1; callback `0x0c347d0c` writes largest free chunk at +12. `0x0c34f028` locks while walking heap bounds +28/+32.
- `mm_memalign=0x0c3507e8` forwards alignment <=8 to `mm_malloc=0x0c35056c`. That allocator rounds `(max(size,12)+11)&~7`, tests node size, checks returned pointer against heap+28/+32 and can panic on Umem exhaustion. Existing 256-byte headroom gate is retained, not a proof against concurrent exhaustion.
- `umm_free=0x0c34cd2c` loads the same Umem slot and calls `0x0c34cc90`; heap bounds +28/+32 match the helper's checks. Upper PSRAM bound `0x3d000000` remains conservative.
- IRQ lock/unlock/barrier compile to ARMv8-M PRIMASK/CPSID/MSR and DSB/ISB instructions, not firmware addresses. Their architectural meaning is unchanged; firmware privilege and UI-thread ownership remain runtime prerequisites.

## Important cache-layout correction for .155

Do **not** preserve the old comment claiming the list payload is the key. Exact instructions establish:

1. `0x0c3a4446` loads `RB+16` as entry data; `0x0c3a444a` saves the RB-node pointer at stack+16.
2. `0x0c3a4458..0x0c3a4460` allocates a list node from cache+48.
3. `0x0c3a457c..0x0c3a4582` copies four bytes from stack+16 into that list node. Thus `*list_node` is the RB node, **not** data.
4. Lookup `0x0c3a4ace`, fast path `0x0c3a4b14..0x0c3a4b20`, loads `*head`, then `[RB+16]`, and compares that data with the supplied key. It returns data+cache[4] as the refcounted entry.
5. `lv_image_cache_drop` `0x0c3a3888` constructs different decoded/header source-key shapes. Passing the complete entry data preserves the correct shape for either class.

The .155 platform therefore takes `key = *(*head + 16)`, checks both pointers, calls per-key drop and confirms head advancement. It never follows the removed node. The 4096-iteration limit remains a module fail-safe, not a firmware capacity claim. Wrong classes are rejected. The legacy .139 traversal is deliberately unchanged to preserve its compiled behavior; this audit does **not** validate its direct-key assumption. A separate .139 correction/audit is warranted rather than silently changing the old target during migration.

## Page and font lifecycle limits

- Pop calls pause `0x0c6939d4` (17 -> 18), stop `0x0c693a1e` (8/18 -> 9), destroy `0x0c696cfc` (8/9 -> 4). Policy 2 only pauses and is refused by the platform.
- Destroy calls `0x0c696a6c` synchronously only when page+36 is zero; that path invokes the page destroy callback, deletes the root with `0x0c384e6c`, and clears page+48.
- Resume invokes create `0x0c696ad8` (2/4 -> 5), start `0x0c693986` (5/9 -> 8), then resumes (8/18 -> 17). Create calls page[19] with the new root. The platform always attempts resume after starting teardown, including partial teardown.
- A different root pointer is the retained success heuristic, not proof of new resource contents. Allocator reuse can produce a false-negative result.
- Native registry resolution and font path dispatch are established, but existing active/idle font wrappers can bypass it. Retargeting plus page recreation does not guarantee font refresh. No idle/live face eviction was added.
- Rebuilding only the top page leaves other cached pages/resources untouched. A timer callback outside rendering is not by itself proof that destroying the current page is safe.

## Focused validation

- Both target variants compiled with clang ARM Cortex-M33/Thumb soft-float, freestanding `-Os -Wall -Wextra -Werror` and their matching Canopus generated headers.
- Extracted `.text` of both default .139 and explicit `RH_TARGET_155=0` compared byte-for-byte equal to the pre-migration `git show HEAD:src/platform_band11.c` built with identical flags.
- `platform-probe.py` executed the newly compiled .155 platform against the exact AP in Unicorn: seven groups passed: driver/native I/O dispatch; generated SDK allocator ABI/entry/heap/headroom gates; real cache-drop/class-lookup with held-entry invalidation and guards; redraw readiness/real rotation-aware accessors; real page lifecycle ladder and policy/async guards; registry layout/font dispatch; timer dispatch.
- The probe models native I/O, heap query/allocation/free, cache unlink/free callbacks, GUI leaves, font retarget functions and timer scheduling. It does not test device storage, tree reclamation under concurrency, real font rendering, actual callback ownership, or display output. Native cache drop/lookup and page state transitions execute actual firmware instructions.

Reproduce (SDK points to the matching Canopus checkout):

```sh
clang -Wall -Wextra -Werror --target=arm-none-eabi -mcpu=cortex-m33 -mthumb \
  -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector \
  -fno-unwind-tables -Os -DRH_TARGET_155=1 \
  -I"$SDK/manager/target/band11" \
  -I"$SDK/targets/xiaomi-band-11-4.100.155/generated" -Iinclude \
  -c src/platform_band11.c -o /tmp/rh-platform-155.o
ld.lld -Ttext=0x1c700000 -e rh_platform_image_cache_drop_all \
  /tmp/rh-platform-155.o -o /tmp/rh-platform-155.elf
llvm-objcopy -O binary /tmp/rh-platform-155.elf /tmp/rh-platform-155.bin
llvm-nm --defined-only /tmp/rh-platform-155.elf > /tmp/rh-platform-155.symbols
build/firmware-tests/bin/python targets/xiaomi-band-11-4.100.155/platform-probe.py \
  --firmware build/firmware-analysis/vela_ap_4.100.155.bin \
  --binary /tmp/rh-platform-155.bin --symbols /tmp/rh-platform-155.symbols
```

## Integrated migration validation

The final .155 module built with `CC=/usr/bin/clang CLANG=/usr/bin/clang sh scripts/build.sh xiaomi-band-11-4.100.155` passes the strict Canopus ELF verifier, three host sanitizer suites, four configuration tests, seven receipt/ELF target-binding tests, and eight firmware-routing tests. The neighboring Canopus target pack adds six exact function symbols under `EVID-RESOURCE-4155-001`; no address-range expansion or verifier bypass is used.

Against the fingerprinted AP and native .155 stage1/stage2/Supervisor resources, 31 firmware tests pass: paths (2), restart counterexamples (6), font lifecycle (4), image lifecycle (4), display invalidation (5), page lifecycle (5), and font registry (5). These use modeled leaves as documented in each fixture, not a physical UI.

Ten offline payload delivery tests also pass using an explicitly temporary test-only signing key. The 15 signed-loader/rebind tests **do not pass** with that fixture: the real Supervisor rejects its receipt with signature error -103. No trusted production private key is available, the Supervisor trust root has not been changed, and signature checks have not been stubbed. Therefore complete delivery validation, signed-load/rebind acceptance, and production installer publication remain pending. The current `build/resource-hook.elf` is a .155 development artifact, not a trusted installable package.

The .139 host/ARM build and offline delivery checks pass; its firmware suites were not rerun because its exact AP is unavailable in this checkout. Its legacy cache-key assumption remains outside this migration's validated scope.

## Pending hardware gates (not passed)

1. Exact .155 fingerprint, compatible Supervisor/receipt allowlist, privilege and heap identity on device.
2. Install/rebind/unload lifetime, callback cancellation and owner-thread transaction/quiescence; no destructive restart shortcut.
3. Open-hook fd/mode/error behavior on actual filesystem and storage faults.
4. Held/unheld image-cache retirement, last release, repeated cache pressure and no stale decoder use/leaks/UAF.
5. Page rebuild under navigation, policy variants, screen-off transitions, deferred destroy, callback reentrancy and allocation reuse/failure.
6. Actual full-display redraw/rotation and retained resource appearance.
7. Font registry retarget, allocation/removal failures, active/shared/idle face ownership, fallback and rendered output. Registry success alone is not a refreshed font.

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

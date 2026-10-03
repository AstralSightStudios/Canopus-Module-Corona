# Q66 .139 compatibility with the checked .155 font transaction

## Current device status

The user now reports normal font-reload acceptance on all three supported
firmwares, including `.139`: **USER_REPORTED_PASS**. The checked transaction is
part of the ordinary build; no experimental gate or default stub remains.
The report does not provide per-artifact hashes or independent GPU/fault/restore
traces. See [the current font contract](../../docs/FONT_RELOAD.md). The binary
findings below remain static evidence, separate from that user report.

## Decision

**The same restricted prepare / validate / commit / retire / refresh algorithm is compatible with this exact .139 AP, after the explicit address substitutions below.** No native layout change or different ownership algorithm was found. This is a binary/native-instruction compatibility finding, **not physical-device acceptance**, a GPU-completion guarantee, or permission to remove the existing healthy, serialized UI / immutable-generation / no-restart restrictions.

The original `src/font_reload_155.c` contract contains 45 native address identities: **36 remain identical; nine must change** (six code/callback addresses and three class addresses). A blanket relocation is incorrect. In particular, the unchanged outline-release callback calls a relocated cache-release function. Runtime integration now uses `src/font_reload.c` for both targets, with the differences selected in `include/resource_hook_target.h`. The original integration retained an opt-in gate and default stub. Both have now been removed following the all-target user-reported acceptance; the audited transaction is the default.

See [machine-readable evidence](font-reload-compatibility.json), and the original [.155 investigation](../xiaomi-band-11-4.100.155/font-reload-investigation.md) and [.155 evidence](../xiaomi-band-11-4.100.155/evidence.json). This is a separate document; the pre-existing `.139/ui-reload-audit.md` edits were not modified.

## Exact firmware and loader provenance

The supplied `~/develop/temp/miwear.watch.q66tc_v4.100.139_full_a02b7af5.bin` is a ZIP container, not a flat AP image.

| Artifact | Size | SHA256 |
|---|---:|---|
| Supplied .139 full container | 66,701,577 | `a02b7af5959e1bb98d35a5b98dca075dd0aa8a4e02ebe00b8d6f7ec1fe9a1c16` |
| Its `vela_ap.bin` member | 12,304,868 | `31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74` |
| .155 analysis AP | 12,304,852 | `ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f` |

The decompressed .139 member was compared **byte-for-byte**, not just by filename or version text, with `build/firmware-analysis/vela_ap_4.100.139.bin`. They are identical. The .155 AP fingerprint agrees with its existing target investigation/evidence.

The supplied `best1503_vela.py` loader's actual `_probe` parser was executed on both APs with only IDA imports omitted; its hash and complete parsed metadata are in JSON. Both validate:

- XIP `0x0c0c0000`, cached flash `0x2c0c0000`, NC flash `0x280c0000`.
- DATA `0x2007dcc8..0x200adedc`; BSS `0x200adedc..0x201029a4`.
- MSP `0x2015fec0`, MSPLIM `0x2015f2c0`.
- .139 DATA LMA `0x2cc4be64` / boot-info offset `0xbbc078`; .155 LMA `0x2cc4be54` / boot-info offset `0xbbc068`.

The shared RAM layout is verified metadata, not inferred from the similar version numbers.

## Method and coverage

1. Exported function ranges/code-item boundaries from IDA 9.4 `build/firmware-analysis/vela_arm_mapped.i64`. Missing function definitions were created only in memory; the database was closed **without saving**. All 122 exported ranges were compared against the exact .155 AP bytes: zero database/AP mismatches.
2. Located .139 function candidates using unique exact instruction-byte anchors, then compared **entire functions**, including inline literal pools. Candidate discovery was not treated as proof and was not a range-delta mapping.
3. Capstone 5 verified **122 function bodies / 9,494 instructions per target**. Seventy complete bodies are byte-identical. In the others, changed instruction encodings are direct branch destinations; instruction widths, opcodes, register operands, field offsets and non-address scalar operands are unchanged. Every changed inline-data byte is covered by a decoded PC-relative load. The JSON contains full-body hashes, code/data partitions, every changed instruction, and every PC-relative literal value on both targets. Absolute literals must be checked even when the instruction bytes are identical.
4. Compared six rodata records: count/size cache vtables, vector class, built-in default font, face callback triplet, and metrics/bitmap/outline callback triplets. The face and glyph callback triplets are byte-identical at their explicitly mapped locations.
5. Executed the same 24 native Unicorn probes separately against each hash-checked AP. The dedicated machine has no dependency on installer artifacts or the adjacent private checkout. Startup-copied libc code is checked against exact source-byte hashes and startup literal triples, then copied to its RAM/execution aliases. This is not a cache-coherence or hardware model.

`tests/firmware_font_compatibility.py` revalidates the receipt from the AP bytes without needing IDA. It verifies the recorded instruction/data partition, rejects unexplained non-branch instruction differences or non-literal inline-data differences, and cross-checks known branch/literal identities.

Review follow-up: changed-encoding direct branches in the original 122 bodies now require a mapped destination with code evidence, not just a matching receipt string. Seventy additional targets were independently located using unique exact-byte anchors and locally compared (5,603 additional instructions). Sixty-nine have complete local-body coverage; assertion target `.155 0x0c9195aa` / `.139 0x0c9195ba` has **entry-block-only** coverage. Missing/wrong mappings or missing target proof fail validation. This bounded one-hop expansion deliberately does not claim recursive semantic equivalence: **606 transitive callsites remain explicitly unverified** in the receipt and the verifier prints `LIMITED`. These include unexpanded calls from the added layer and byte-identical branches without target-body evidence. Native probes likewise remain bounded path tests with explicitly modeled leaves.

## Complete original algorithm address map

Hex addresses below preserve the original source's Thumb bit for callback values. JSON's lookup table uses even code identities, and the probe resolver preserves the Thumb bit. No RAM or rodata low bit is treated as a Thumb bit.

### Required substitutions

| Role | .155 | .139 |
|---|---|---|
| Count-cache class | `0x2ca16934` | `0x2ca16944` |
| Size-cache class | `0x2ca168b4` | `0x2ca168c4` |
| Exact vector-label class | `0x2ca6ee48` | `0x2ca6ee58` |
| Vector compare callback | `0x0c69feb1` | `0x0c69fec1` |
| Vector destroy callback | `0x0c6a12d5` | `0x0c6a12e5` |
| Drop one object's vector cache key | `0x0c6a1304` | `0x0c6a1314` |
| FT pixel-size setup | `0x0c8b8a54` | `0x0c8b8a64` |
| Cache drop | `0x0c8b8c9e` | `0x0c8b8cae` |
| Cache release | `0x0c8b9780` | `0x0c8b9790` |

### Verified identical code/callback addresses

| Role | Address on both |
|---|---|
| Registered screen-tree walk | `0x0c380574` |
| Style-property refresh | `0x0c38525c` |
| VG dispatch callback | `0x0c3913ed` |
| SW dispatch callback | `0x0c3948ad` |
| Gradient pending callback | `0x0c395bd1` |
| Image pending callback | `0x0c395be1` |
| Face compare / create / destroy | `0x0c396785` / `0x0c3967b5` / `0x0c396b25` |
| Font metrics callback | `0x0c396b5d` |
| Context outline-event callback | `0x0c3981d1` |
| Glyph pending callback | `0x0c399b63` |
| Drop interned face ID | `0x0c39a424` |
| Metrics destroy / compare | `0x0c39fd95` / `0x0c39fd9b` |
| Outline compare callback | `0x0c39fdcd` |
| Font outline-release callback | `0x0c3a0993` |
| Outline destroy callback | `0x0c3a09f5` |
| Cache acquire existing | `0x0c3a3860` |
| Intrusive list unlink | `0x0c3a46dc` |
| Metrics create callback | `0x0c3a5ef1` |
| Cache acquire/create | `0x0c3a7b78` |
| Font outline-acquire callback | `0x0c3a7c45` |
| Outline create callback | `0x0c3a8bb9` |
| Preallocated count-cache initializer | `0x0c3a9e48` |
| Native allocation / free | `0x0c3abe20` / `0x0c3abe58` |
| FT fixed-point multiplication | `0x0c424304` |

### Verified identical RAM identities

| Role | Address on both |
|---|---|
| UIkit root | `0x200bd1e8` |
| LVGL initialized byte | `0x200bd1ec` |
| Display list header | `0x200bd1f0` |
| Style refresh enabled byte | `0x200bd210` |
| Draw-unit list | `0x200bd318` |
| FreeType context root | `0x200bd3ec` |
| Vector-label VG draw-unit pointer | `0x200d327c` |
| Vector-label geometry cache | `0x200d3280` |

For example, native initialization `0x0c3a9598` loads `0x200bd1e8`, accesses context at root+`0x204`, draw units at +`0x130`, installs the unchanged outline event at context+16 and the unchanged pending callback at pending+36. Vector constructor `.155 0x0c6a1260` / `.139 0x0c6a1270` independently loads the same `0x200d327c` and `0x200d3280` literals, but loads the remapped vector callbacks and size class. This establishes mixed unchanged/remapped identities without a universal delta.

## Native layout and ownership contract

The full scalar contract and supporting function identities are recorded under JSON `layout_contract`. Important checks are:

| Structure | Unchanged layout / evidence |
|---|---|
| Manager | UIkit+28; active list +0, wrappers +12, registry +24, idle pointer +556. Manager initializer `0x0c4948a0`, factory `0x0c494380`, release `.139 0x0c917394`. |
| Intrusive lists | Header `{payload_size,head,tail}`; node links at payload-size and payload-size+4. Native list allocation/unlink/next bodies agree. |
| Active / wrapper / idle / registry payloads | 48 / 40 / 44 / 8 bytes. Active font +0, name +4 (inline +12), packed key +8, refs +44. Idle font +40. Registry name/path +0/+4. |
| Wrapper publication | Native factory copies 36 bytes, then stores record +36. Transaction must copy only 28: preserve fallback +28, user +32, record +36 and list links +40/+44. Native fallback destruction still follows each wrapper's chain. |
| Descriptor | 64 bytes, magic `0x5f5f4654`; font at +4, self at +28 (font+24), size +40, style/mode +44/+46, context +48, face +52, held face entry +56, path +60. Native factory instructions `0x0c4946ae..0x0c4946cc` explicitly write these fields. |
| Context / face | Context intern-list +4, outline event +16, capacity +20, face-cache +24. Face payload 28; FT face +12, metrics/outline caches +16/+20. Face key compares mode, style, pathname **contents**, not pixel size or path-pointer identity. |
| Cache / entry | Cache storage 64; callbacks +16/+20/+24; RB +36; LRU payload-size/head/tail +48/+52/+56. Entry `{cache,refs,payload_offset,invalid}` at +0/+4/+8/+12; RB payload +16. Payload sizes face 28, metrics 32, outline 8, vector 16. |
| Preallocated initialization | `0x0c3a9e48` writes RB size `payload+20`, empty LRU and size callback `0x0c3a06a7`, without allocating. Both generic constructor and wrapper factory still have the demonstrated allocation-failure hazards. |
| FT metrics | FT face flags +8 bit 0 scalable; underline values +80/+82; size/metrics pointer +88. Metrics y-scale +20, descender +28, height +32; native factory uses the same fixed-point conversion. |
| Pending draw work | Unit active task +32, image queue +36, gradient context +40 (queue +8), glyph queue +48, flush count +52. Queue arrays +4/+20, callback +36; each array has buffer/count/capacity/element-size +0/+4/+8/+12. Glyph/image/gradient elements remain 24/76/4 bytes. |
| Display / object traversal | Display payload 792, rendering flag byte +58 bit 1; layers +680, screens +692/count +720; layer task +100/next +108. Object parent +4 and deleting byte +51 bit 4 remain unchanged. |
| Vector owner | Cache key object +4, path array +8/count +12; path element stride 20; upload flag at path+36. Per-object drop does not globally clear the cache. |
| Text refresh | Font property 90; `LV_PART_ANY=0x000f0000`; style event 45 plus layout/inheritance notification. Same-pointer descriptor publication still requires explicit refresh. |

The module's family/node/object/depth/file-size ceilings are **its own fail-closed policy**, not firmware layout constants. No claim that all .139 stock fonts have the .155 stock font's file size is necessary: the algorithm hashes the actual files and uses a 32 MiB streaming bound.

## Native probes and limits

`tests/firmware_font_barrier_155.py` keeps its historical filename to avoid breaking runners, but now selects `.139` or `.155` through the explicit mapping. Both targets run the same tests; unknown addresses/targets and wrong AP fingerprints are failures, not skips or fallback execution.

Results: **24/24 passed on .139 and 24/24 on .155**. Sixteen identity/fingerprint/direct-dependency guard tests also pass. Covered paths include:

- Releasing an old glyph after prematurely swapping its wrapper routes the old entry into the new cache: the counterexample remains true on .139.
- Normal final dispatch drains both pending glyph buffers before publication; a partial drain does not retire descriptor-owned face references.
- GPU errors still drain queues, and successful reset may hide wait failure. **Empty buffers do not certify successful hardware completion.**
- Object-keyed vector caching, per-owner drop, copied non-uploaded path bytes versus retained uploaded addresses, and font-style layout/inheritance notification.
- Native wrapper/cache constructor OOM faults, allocation-free initialization of checked storage, failed face-create unlink/free cleanup, content-based face keys, fallback teardown, and built-in-default exclusion.

GPU/OS wait, allocator failure, logging, selected copy/clear operations, and selected cache/event leaves are explicitly hooked in the tests. They do not test real FreeType parsing of an entire replacement font, all widgets/custom font consumers, concurrent rendering, filesystem immutability, device teardown, or the source transaction's complete commit/rollback behavior. Those require the separate source-model suite and device acceptance; no device test was performed here.

## Reproduction

From the repository root, with the existing Unicorn/Capstone environment:

```sh
build/firmware-tests/bin/python tests/firmware_font_compatibility.py \
  --container "$HOME/develop/temp/miwear.watch.q66tc_v4.100.139_full_a02b7af5.bin" \
  --loader "$HOME/develop/temp/best1503_vela.py"

for version in 139 155; do
  RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.$version \
    build/firmware-tests/bin/python tests/firmware_font_barrier_155.py
done

build/firmware-tests/bin/python tests/test_firmware_font_compatibility.py
```

Expected static output: `PASS 122 function bodies / 9494 instructions, 6 data records, 45 source address identities`, the 70-target local-proof result and explicit `LIMITED` transitive-boundary warning, then the exact OTA/member and loader-layout confirmations. Static AP paths can be overridden with `--ap139` / `--ap155`. Native probes default to the matching `build/firmware-analysis/vela_ap_4.100.<version>.bin`; `RESOURCE_HOOK_FIRMWARE` can override the path but cannot bypass the expected target hash. These native audit probes require no changes to `Canopus-Private`.

## Shared runtime integration validation

- Both targets pass strict ELF verification with experimental fonts disabled and enabled (four builds).
- `scripts/test-host.sh` passes the two-target ASan/UBSan matrix for default, explicit-disable, and experimental modes. `tests/test_font_reload_targets.py` independently binds the preprocessed implementation's 45 identities to this binary audit, rather than only testing a model against the same macros.
- Each target passes 19 image-adapter regression probes; font unification does not change the image algorithm.
- With user approval, the adjacent `.139` target pack adds 42 exact restricted records under `EVID-FONT-4139-001`, plus generated C/Rust metadata. Existing records cover the other three identities. All remain `STATIC_RECOVERED / PENDING`; address ranges and verifier policy are unchanged. Removing the allocator's record from a temporary pack copy causes the experimental ELF to fail strict verification.
- Experimental ELF snapshots are `build/resource-hook-font-experimental-139.elf` and `build/resource-hook-font-experimental-155.elf`. They are development artifacts, not signed installer packages. These historical artifact names predate the current normal `scripts/build-watchface.py`, which supports all three exact targets.

No `.139` physical-device acceptance was performed.

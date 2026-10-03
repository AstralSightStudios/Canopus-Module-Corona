# Band10Pro .043 font-reload native evidence and static mapping audit

## Current device status

The user reports normal font-reload device acceptance on all three supported
firmwares, including `.043`: **USER_REPORTED_PASS**. The checked transaction is
now in the default module and normal signed installer. This later report does
not supply per-target artifact hashes or separate fault/restore traces; it does
not expand GPU recovery, restart or custom-owner support. Static native evidence
below is unchanged. See [the current font contract](../../docs/FONT_RELOAD.md).

## Identity and method

- Exact AP: `build/firmware-analysis/vela_ap_3.101.043.bin`, 13,795,728 bytes, SHA256 `519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec` (recomputed).
- Source .155 AP SHA256 `ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f`.
- Existing `.043` evidence and `.139` font compatibility receipt inspected. All 45 original firmware-address inventory entries have an individually recovered `.043` mapping in `font-reload-audit.json`; no range delta used.
- IDA 9.4 idalib / Hex-Rays executed only against cloned `/tmp/fr043-readonly.i64`; original databases and shared source files were not edited. Existing DB has XIP/data/flash aliases but lacks PSRAM/SRAM execution mappings; native PSRAM bytes were separately decoded using Capstone and executed using exact AP startup-copy mappings in Unicorn.
- Loader `best1503_vela.py` SHA256 `6833b2e53dbd4aad1d5e900787f4279e5fbcf090417cd3b3c286d60659e87105`.
- PSRAM map witness: startup `0x0c0c0ac8..0x0c0c0b06` copies flash `0x2c10d440..0x2c18d940` to data alias `0x3c000000` (execution alias `0x1c000000`). File offset `0x4d440`, size `0x80500`.
- Additional SRAM execution aliases used by native dependency veneers: startup copy flash `0x2c0c23a4..0x2c0fc6cc` -> data `0x200765c0`; execution `0x002765c0`. Second copy flash `0x2c0fc6cc..0x2c10d438` -> data `0x200b2860`; execution `0x002b2860`. These are dependency mappings, not replacements for exact module-leaf identities.

## Mandatory target contracts (static audit; implementation handled separately)

1. Face cache payload **24 bytes** on `.043`, **28** on `.155`. Change every face-cache expected node size / `face+28` / `entry-28` / diagnostic size-classification / face-key storage contract to a target parameter. Six-word native `.043` face key versus seven-word `.155` key; passing zeroed larger storage is not itself unsafe but target-sized key is preferable.
2. `.043` vector key-drop counterpart is **class destructor `0x0ca65c24` with object in r1**, r0 unused. The `.155` standalone helper accepts object in r0. Call `(0, object, 0)` on `.043`. The `.043` body performs only zeroed 16-byte key construction and cache-drop; it does not free the object or perform other class cleanup.
3. Exact callback identities are often **PSRAM pointers**. Do not substitute XIP byte-equivalent clones for callback comparisons or fields installed into descriptors/caches. Direct callable leaves may use the independently audited XIP entries in the mapping.
4. UIKit manager offset is **28 on BOTH targets**. `.155` `0x0c4924ee` loads `[R3,#0x1c]`; `0x0c4948ac` does the same. `.043` `0x0c86037c` resolves `*(0x20103174)+28` and calls managed manager function. Legacy platform +20 must not be reused as font transaction manager offset.
5. Vector class is **`0x2cdba868`**, NOT preceding padding `0x2cdba864`. Native event callback `0x0ca66520` explicitly passes class at `0x2cdba868`; class +0 base is `0x2cce6880`, +4 constructor `0x0ca65851`, +8 destructor `0x0ca65c25`, +12 event `0x0ca66521`.

## Confirmed layouts used by src/font_reload.c

### Manager/list/ownership

- `.043` manager constructor `0x0c85f2b4`: allocates 560 bytes, initializes active list +0 payload48, wrapper list +12 payload40, path registry +24 payload8; fallback policy +548, emoji manager +552, idle cache pointer +556.
- Managed factory `0x0c85f6cc`: active backing record font+0, name pointer+4, packed size/style+8, inline name at+12 (31 characters + NUL), references+44. Name pointer is reset to inline buffer. Active record intrusive links +48/+52.
- Wrapper copies 36 bytes from backing font, stores backing record at+36. Transaction must update only initial28 bytes and preserve fallback+28, user data+32, record+36, links+40/+44.
- Wrapper deletion `0x0c85f8fc`: membership scan, decrements active record+44, moves final ordinary font to idle cache, removes/frees active record and wrapper. This preserves the required fallback/record transaction ownership contract; no native wrapper creation is needed.
- Idle cache constructor `0x0c85efc4`: 16-byte object, list at+0 with payload44, capacity+12. Insert `0x0c85f090`: key name pointer+0, packed size/style+4, inline name+8, font+40; links+44/+48.
- Registry payload8 = name/path words+0/+4, links+8/+12.
- Generic list head12 = payload size+0, head+4, tail+8; next/prev at payload-size+4/payload-size. `.043` unlink leaf `0x0c169ce8` has same contract; no allocation.

### Context/font/descriptor/FreeType

- Context constructor `0x0c163ca8`: context28, FreeType library+0, face-ID list+4 payload8, outline callback+16, max child cache count+20, face cache+24. Context global `0x20103374`.
- Context initialization passes count-cache class `0x2cce571c`, node size **24**, capacity `0x7fffffff`, callbacks compare `0x1c05682d`, create `0x1c056719`, destroy `0x1c0566ed` from table `0x2cce510c`.
- Face payload24: pathname+0, style/mode packed+4, glyph cap+8, FT face+12, metrics cache+16, outline cache+20. `.155` extra seventh word is absent.
- Native factory `0x0c163da8`: descriptor64, magic `1600079444`, embedded font at+4, size+40, style/mode+44 (outline mode1 at+46), context+48, face+52, held entry+56, interned pathname+60. Embedded font descriptor-pointer slot is font+24 (descriptor+28), and its value is descriptor.
- Native setter installs metrics `0x1c056e15`, outline acquisition `0x1c0572a5`, glyph release `0x1c057289`. Native factory execution in probe confirms all these exact values and 24-byte entry offset.
- FT scalable flag face+8 bit0; FT size/metrics+88; ascender+28/descent+32 and y_scale+20; underline position/thickness FT+80/+82 signed16. Factory computes font fields through FT_Set_Pixel_Sizes and FT_MulFix as on `.155`.
- Face comparator `0x1c05682c` (XIP clone `0x0c163c6c`): mode+6, style+4, pathname content via strcmp; pixel size is not in face identity. Immutable generation pathname requirement remains.
- Metrics child cache payload32/capacity2*max; outline child cache payload8/capacitymax. Their callback tables at `0x2cce516c` and `0x2cce529c` explicitly hold the exact PSRAM addresses in mapping.
- Font teardown `0x0c164034`: releases held face entry, drops zero-ref face, drops interned ID, frees descriptor. Release callback `0x1c057288` obtains child's current descriptor cache and uses saved glyph-description entry+20. The probe executes this routing; an old-glyph/new-descriptor mismatch remains an ownership risk, not a verified safe transaction.
- Unlike `.155`, native `.043` face-create does not allocate the optional extra glyph-L1 backing word. This explains size24 but doesn't require changing any accessed first-six-word field.

### Cache/RB/LRU

- Cache object64 unchanged: class0, payload size4, capacity8, occupancy12, compare16, create20, destroy24, name32, RB tree36 (comparator40, RB payloadsize44), LRU list48 (size4/head52/tail56), size callback60.
- Count-cache initializer `0x0c272d38`: validates size and callbacks, initializes RB payload size = node_size+20, intrusive LRU payload4, size callback `0x0c272a61`. Native execution verifies initialization without allocation. No cache constructor is needed.
- LRU nodes: RB node pointer+0, prev+4, next+8. RB node payload pointer+16.
- Cache entry follows node payload: cache+0/refcount+4/payload-offset+8/invalid-byte+12. Entry free subtracts stored offset from entry pointer. `.043` source leaf witnesses `0x0c1669d8`, `0x0c16692c`, `0x0c1669f0`.
- Exact classes: count `0x2cce571c`, size `0x2cce56f4`. Vector payload16 remains size-cache.

### Display/draw boundary

- Display constructor `0x0c13c978` zeroes **792 bytes**, root layer+680, screens+692, count+720. `.043` LV init `0x0c166408` initializes global display list `0x2010317c` payload792. No display-size parameter difference.
- Display rendering flag is byte58 bit1; layers use task-list+100 and layer-next+108. Root layer allocation is120 on `.043`; used fields match source.
- Draw-unit global `0x201032a4`; unit-next0, dispatch16, active task32 unchanged.
- VG init `0x0c1529d8` allocates292, stores exact PSRAM dispatch `0x1c045361`; SW init `0x0c14b400` allocates40, stores `0x1c03debd`. Both dispatchers are synchronous in inspected normal/busy/task paths, set active+32 around render and clear after. No different drawing backend uncovered.
- VG unit image queue+36, gradients+40, glyph queue+48, submission counter+52. Gradients pending queue is gradient+8.
- Pending container44: two array descriptors +4/+20, each buffer0/count4/capacity8/element-size12; release callback+36 and user arg+40.
- Glyph queue24-byte elements initialized at `0x0c154a58`, exact callback `0x1c046cfd`; image queue76-byte elements at `0x0c15bb1c`, callback `0x1c04d259`; gradient queue4-byte elements at `0x0c1574a0`, callback `0x1c0493bd`.
- Initialized byte `0x20103178`, style refresh enable `0x2010319c`.
- Natural VG finish `0x1c04ec20` drains gradient, image, glyph queues, clears unit+52. It STILL drains after GPU error; native probe confirms both glyph buffers drain on modeled error. Empty queues do NOT prove GPU completion/recovery safety, just as on `.155`.

### Tree/style/vector owner boundary

- Tree walk `0x0c13c690` enumerates registered screens with display+692/+720. Object parent+4 and flag-byte51 used by existing traversal remain common. Independent full custom-owner coverage is not established.
- Style refresh `0x0c1070ac`, font property90. Property table `0x2cce5bc4[90]` = **5**; LV_PART_ANY `0x000f0000` handling remains. Refresh invokes style-changed event45 and object/parent layout marking/inherited refresh paths in inspected branch.
- Vector init `0x0ca65850`: cache slot `0x2013eae0`, unit slot `0x2013eadc`, size-cache payload16, comparer `0x0ca657ed` (object+4 only), createNULL, destroy `0x0ca6581d`.
- Vector cached data paths+8/count+12, array stride20 with path pointer+0. Destructor traverses exactly these fields.
- Vector paths are native VG objects with upload-bit at path+36. `0x0ca65c4c` build callback copies ordinary glyph path into private label path using veneer `0x0cac1138` -> append `0x1c04c884`, reading source bytes path+40/+44 and not propagating flags. Native append probe confirms independent copied bytes and clear destination upload bit.
- Native init-path `0x002b5e60` clears upload flags as part of zeroing path+18 through+71. Font outline upload finalizer `0x1c04ab60` sets bit0 at+36 and constructs the uploaded command representation; label finalize `0x1c04b638` differs and does not upload.
- Key-drop counterpart `0x0ca65c24` is ABI-adapted as described above. Existing refresh constraints (no app-owned/canvas/custom cached text, unknown/uploaded vector variants excluded) still apply.

## Durable verification

Run from the repository root (the Python file resolves its inputs relative to itself):

```sh
build/firmware-tests/bin/python tests/firmware_font_1043.py
```

Result: **19 tests passed: 14 selected-native probes and 5 evidence guards**.
The first ten preserve and harden the original temporary native-probe cases;
the four additional probes address callback-NULL cleanup and zero/one child
cache face destruction. These are separate from the full host transaction
suite maintained elsewhere, including its three-target tests.

### Evidence validation and execution limits

- Exact AP size and SHA256 are checked **before creating Unicorn**. Wrong-length
  and same-length wrong-hash images are tested. Missing AP is an error, not a
  silent skip. The firmware binary is an external input, not committed here.
- All 45 exact inventory pairs are preserved, including odd/even spelling;
  their canonical JSON SHA256 is pinned in the test. This is individual static
  recovery, **not an assertion of transitive .155 equivalence**.
- Startup descriptors are decoded from actual Thumb literal LDR instructions,
  with expected register/opcode, literal address/value, range, copied-byte hash,
  file offset and data/execution destinations checked. The source literals were
  inspected in the original startup dump, not inferred from a stale PSRAM file.
- JSON includes selected **AP-derived native body bytes, file offsets and
  SHA256**, with body/prefix/literal-pool extent labeled. It also includes 32-byte
  mapped-leaf/class witnesses and runtime-global literal references (direct or
  base-literal plus statically recovered field offset; not every global has a
  standalone target literal). Prefixes/address encodings are not semantic proofs
  by themselves. Runtime
  global field semantics are the static layout conclusions above.
- Seventy narrowly recorded native execution spans (1,690 bytes) form the
  instruction allowlist. Unrecorded native paths fail closed; explicitly
  registered lower leaves are the only interceptions. Instruction allowlisting
  is path evidence, not a complete control-flow or dependency audit.
- Fresh machine/RAM per test; each call is limited to 20,000 instructions and
  2,000,000 microseconds and must return through the sentinel. Code mappings are
  RX and synthetic data/stack are RW. Data/execution aliases are independent
  copies, not coherent hardware alias emulation.
- Each call poisons the bounded stack, checks low/high canaries, restored SP and
  r4-r11. Modeled leaves poison caller-saved r1-r3/r12. Descriptor allocations
  and vector destination buffers have poison guards where practical. APSR and
  floating-point saved registers are not comprehensively poisoned/checked.

To reproduce body disassembly, use each JSON body's `file_offset`, `size` and
`address`: read that exact AP slice, check its hex/SHA256, then decode Thumb/MCLASS
at its recorded execution address (e.g. Capstone). The unittest checks those
same slices and hashes automatically, including relocated startup copies.
Historical temporary dumps are provenance only and are not runtime inputs.

### Selected-native cases and modeled boundaries

1. Face comparator mode/style/path: bounded content `strcmp` model at
   `0x1c06062c`; distinct pointers with equal strings and unequal mode/style/path.
2. Metrics and outline comparators execute PSRAM bodies without modeled leaves.
3. Count-cache checked-storage initializer executes native RB/list dependencies,
   verifies 64-byte layout and exact callbacks; no allocation/eviction branch.
4. Vector-drop destructor `0x0ca65c24` executes **r1 object ABI**, verifies zeroed
   16-byte key; lower drop veneer `0x0cac0580` modeled.
5. Outline release routes current descriptor cache and clears saved entry;
   cache release `0x1c0592ac` modeled; cleared entry prevents a second release.
6. FT_MulFix scalar result executes natively.
7. Existing-face factory executes interning lookup, 64-byte descriptor setup,
   **24-byte face entry back-offset**, and exact PSRAM callback installation.
   Cache lookup, zeroed allocation and FT pixel-size leaves modeled and validated.
8. Vector append deep-copies uploaded-source bytes and preserves a clear
   destination upload flag; ample-capacity branch only, no allocation/upload.
9. VG/SW busy dispatch branches refuse work without lower callbacks.
10. Error finish drains both glyph arrays through stored native callbacks;
    GPU/log/error/cache-release leaves modeled. Drain is **not GPU completion**.
11. Native generic acquire-create `0x0c166740`, insertion/capacity wrapper
    `0x0c1665d0`, payload accessor `0x0c16692c`, and entry-free `0x0c1669f0`
    execute after the exact face-create callback is modeled returning 0/NULL.
    Count-cache reserve/insertion/unlink and heap free are explicit models:
    one tracked allocation is linked, callback fails, native control flow orders
    unlink before free of `entry-24`, returns NULL without acquisition, and the
    ownership ledger/occupancy end empty. This establishes that selected native
    cleanup path under those class contracts, **not native RB allocation or a
    real parser's partial-construction cleanup**.
12. Exact PSRAM face destructor `0x1c0566ec` with zero child caches: modeled
    FT_Done_Face `0x1c07a580` consumes the one synthetic owned FT face; no child
    destruction call occurs.
13. Same destructor with only a metrics child: modeled cache destroy
    `0x1c05927c` consumes one owned child and receives the user argument; native
    code clears+16. FT/cache ownership ledgers finish empty.
14. Same destructor with only an outline child: corresponding native+20 branch
    clears the child pointer; ownership accounting balances. Parent face payload
    and interned path are not freed here, and surrounding poison remains intact.

The callback-NULL probe does not execute a real FT parser or the native
face-create body. Child cache destruction and FT disposal in cases 12-14 are
modeled lower operations, not recursive native-cache cleanup. These tests do
not establish two-child destruction, whole allocator/OOM coverage, full native
compiled transaction correctness, concurrency, restart or real-device success.

## Implementation and packaging checkpoint

The shared runtime now selects the exact `.043` identities, a six-word/24-byte
face key and the vector-drop r1 ABI. All three targets pass the host transaction
and ASan/UBSan matrix; `.043` default and experimental ELFs pass strict verification.
With explicit user permission, the adjacent Canopus pack has 41 missing restricted
font records plus the preexisting POSIX-open callback's exact PSRAM record. Its
XIP range is unchanged; only the exact startup-copy execution range is additionally
declared. Removed allocator, PSRAM callback and POSIX-open records reject the
corresponding ELF probes. The verifier itself is unchanged.

The separately signed `.043` installer was built at
`dist/module-installer-font-experimental-0.3.0-band10pro-3.101.043/`.
Its experimental ELF SHA256 is
`294e67c6b08f2bf50fe6e98fe8628e5575cdbb7957f2c059d97627e320f8d08e`.
Single-device and mixed `.155`/`.043` packaging checks preserve the exact ELF,
signed receipt and device-isolated archives. Hardware status at that packaging
checkpoint was **NOT_PROBED**; the later user report is recorded above separately.
Use immutable font generations and keep GPU recovery/restart/custom owners
outside the supported contract. See [the shared safety contract](../../docs/FONT_RELOAD.md).

## Original read-only pack snapshot and remaining native/device limits

Pack absence/count statements below describe the original read-only input
snapshot, not the current pack or transaction implementation status. This task
does not edit or approve pack policy, module/runtime code, headers or scripts.

- Complete exact map available, including PSRAM callback identity; no unresolved baseline address remains.
- 10 direct native call leaves absent from `.043` Canopus pack: LV malloc/free, count-cache init, drop_face_id, cache existing/acquire-create, FT pixel size/MulFix, list-remove, vector key-drop. Existing approved restricted records cover tree/style/cache-drop/cache-release.
- Full `.155` font whitelist parity requires **41 missing records** (out of42 font_reload_* records); exact names/kinds/addresses and existing equivalents in `/tmp/fr043-missing-pack-records.json`. This includes callback identities and globals/classes. No pack files were edited.
- `.043` pack target.toml advertises only XIP firmware-address ranges; approving PSRAM callback functions needs explicit reviewed policy/range treatment, not accidental addition to an unrelated range. Additional SRAM execution dependencies exist but are not directly called by module.
- Required source parameterization is payload24 + vector-drop r1 ABI; UIKit28 and display792 remain unchanged.
- Full native compiled-transaction probes, real FT partial-constructor failure balance (beyond the selected modeled-callback cases above), replacement-file parsing, corrupt-font rollback on firmware, repeated stock/gen restoration, visual adoption and real-device acceptance remain unproved here. Full host transactions are separate evidence, not native parsing/device acceptance.
- Healthy serialized standard UI/GPU path only. Error finish drains are not completion; recovery/reset/concurrent/custom rendering unsupported. Root pointer equality is not epoch/restart safety; lifecycle disable contract still required.
- `.043` manager destructor `0x0c85f348` actually refuses destruction if live resources remain, unlike `.155` teardown behavior. This does not establish general framework restart safety or remove lifecycle exclusions.

## Exact 45-entry inventory

Mappings retain original exact pointer spelling. Set Thumb bit0 for a call;
retain exact callback/global/class values for identity and storage. The mapped
vector destructor needs the distinct .043 ABI described above. A mapped address
alone does not prove whole-function, transitive-dependency or whole-target
semantic equivalence.

| Baseline .155 ID | Exact .043 ID |
| --- | --- |
| `0x0c380574` | `0x0c13c690` |
| `0x0c38525c` | `0x0c1070ac` |
| `0x0c3913ed` | `0x1c045361` |
| `0x0c3948ad` | `0x1c03debd` |
| `0x0c395bd1` | `0x1c0493bd` |
| `0x0c395be1` | `0x1c04d259` |
| `0x0c396785` | `0x1c05682d` |
| `0x0c3967b5` | `0x1c056719` |
| `0x0c396b25` | `0x1c0566ed` |
| `0x0c396b5d` | `0x1c056e15` |
| `0x0c3981d1` | `0x1c046d09` |
| `0x0c399b63` | `0x1c046cfd` |
| `0x0c39a424` | `0x0c163be8` |
| `0x0c39fd95` | `0x1c056d19` |
| `0x0c39fd9b` | `0x1c056f05` |
| `0x0c39fdcd` | `0x1c057259` |
| `0x0c3a0993` | `0x1c057289` |
| `0x0c3a09f5` | `0x1c057331` |
| `0x0c3a3860` | `0x0c1666d0` |
| `0x0c3a46dc` | `0x0c169ce8` |
| `0x0c3a5ef1` | `0x1c056d25` |
| `0x0c3a7b78` | `0x0c166740` |
| `0x0c3a7c45` | `0x1c0572a5` |
| `0x0c3a8bb9` | `0x1c057635` |
| `0x0c3a9e48` | `0x0c272d38` |
| `0x0c3abe20` | `0x0c16daa8` |
| `0x0c3abe58` | `0x0c16dae4` |
| `0x0c424304` | `0x0c33d8bc` |
| `0x0c69feb1` | `0x0ca657ed` |
| `0x0c6a12d5` | `0x0ca6581d` |
| `0x0c6a1304` | `0x0ca65c24` |
| `0x0c8b8a54` | `0x0c33ef58` |
| `0x0c8b8c9e` | `0x0c1667dc` |
| `0x0c8b9780` | `0x0c1666ec` |
| `0x200bd1e8` | `0x20103174` |
| `0x200bd1ec` | `0x20103178` |
| `0x200bd1f0` | `0x2010317c` |
| `0x200bd210` | `0x2010319c` |
| `0x200bd318` | `0x201032a4` |
| `0x200bd3ec` | `0x20103374` |
| `0x200d327c` | `0x2013eadc` |
| `0x200d3280` | `0x2013eae0` |
| `0x2ca168b4` | `0x2cce56f4` |
| `0x2ca16934` | `0x2cce571c` |
| `0x2ca6ee48` | `0x2cdba868` |

## Durable artifacts and historical provenance

- `tests/firmware_font_1043.py`: standalone bounded unittest native probe.
- `targets/xiaomi-band-10-pro-3.101.043/font-reload-audit.json`: exact inventory,
  layouts/differences, source copy descriptors, AP byte bodies/hashes, instruction
  span allowlist, modeled leaf contracts, receipt and historical input hashes.
- This document: static recovery conclusions, target contracts and limits.

The read-only `/tmp/font_reload_155_to_043_mapping.json`,
`/tmp/font_reload_043_audit.md`, `/tmp/fr043_native_probe.py` and
`/tmp/fr043-native-probe.json` were source inputs. Supporting original startup,
core and font decompilation dump hashes are recorded in JSON provenance. None of
those temporary files is needed for the persisted test. Selected AP byte bodies
are reproducible directly from the fingerprinted firmware input, rather than
trusted from independently dumped/stale execution-region files.

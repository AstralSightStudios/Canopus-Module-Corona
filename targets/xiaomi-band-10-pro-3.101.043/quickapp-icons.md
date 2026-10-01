# Read-only QuickApp icon lookup: exact 3.101.043

AP SHA256: `519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec`
(13,795,728 bytes). Audited IDB:
`build/firmware-analysis/vela_ap_3.101.043.bin.i64`.
The lookup, record layout and relocated launcher setter were independently
recovered from this exact AP, not inferred from Band 11 addresses.
IDA reference was read before execution. No deliberate IDB mutation/save APIs
were called; the managed backend nevertheless updated the IDB on shutdown, so
this audit does not claim the analysis database was never saved. Final queries
used a temporary database copy; the fingerprinted AP was not modified.

## Exact read-only lookup ABI

| Role | Exact address (even code entry; callable adds Thumb bit) |
|---|---|
| service pointer slot | `0x200eb658`, initial contents `0x2cdbb054` |
| expected service table | `0x2cdbb054` |
| table slot 3, active package/name lookup | `0x2cdbb060`, contents `0x0ca69e81` |
| active package/name map pointer slot | `0x200eb650` |
| active lookup wrapper | `0x0ca69e80`, 40 bytes |
| hashmap get (hash and bucket search inline) | `0x0cabe234`, 158 bytes |
| comparison AP veneer | `0x0cac0a48` -> unavailable ROM `0x00276854` |
| uninstalled/recoverable map (not used) | `0x200eb654`, table slot 4 `0x0ca69e55` |

The wrapper forwards the requested name to `hashmap_get(*0x200eb650, name,
&record)`, returning the initially zeroed stack output only on success.
No valid-input AP instruction allocates, locks, acquires a reference, opens a
file or mutates the registry. Invalid map/key/output triggers native assertions,
so the adapter checks the exact service table and slot before calling and
preflights the selected bucket first. Comparison is a documented modeled ROM
leaf in instruction probes, not a replacement of the native lookup.

Map `+0` is a power-of-two bucket count, with bucket pointers starting at `+4`.
The inline DJB2 hash starts at 5381 and accumulates unsigned bytes using
`hash * 33 + byte`. Index is `hash & (count - 1)`. Twenty-byte nodes contain
hash `+0`, copied key `+4`, key length including NUL `+8`, record `+12`, next
`+16`. Adapter limits count to 4096, selected chain to 1024, and bounds every
node/key read. Cycles, unsafe pointers and malformed keys fail before lookup.
Initialization `0x0ca6a634` requests 50 buckets; allocator `0x0cabe010` rounds
that to 64. The module does not initialize or modify either map.

## Record ownership and UI context

The .043 app record is 64 bytes, with **package at `+8`, icon pathname at `+12`**
and a uint16 app ID at `+16`; this differs from Band 11's `+12`/`+16` fields.

- Installation `0x0ca6a30c` copies a 64-byte descriptor and duplicates its
  package/icon strings before inserting into the active list/map.
- `quickapp_register_app` `0x0c549678` replaces icon `+12`, publishes that
  pointer into QuickApp context `+40`, then updates the launcher.
- `quickapp_unregister_app` `0x0c549948` resolves through active slot 3, removes
  the launcher entry, then calls unregister `0x0ca6a084`. This removes the active
  map key and either frees the record or moves it into the native logged
  "uninstall app list" and map `0x200eb654`.
- Reinstall `0x0ca6a4c8` moves the recoverable record back into the active map.
  The adapter never falls back to the uninstalled/recoverable registry.

Lookup returns borrowed state, not a retained/refcounted record. All required
strings are bounded and copied during the call; no firmware pointer escapes
or survives it. `miwear_main` `0x0c6fcdf0` initializes the loop at `0x20125700`;
QuickApp proxy initialization `0x0c548450` uses that same loop. Calls are
restricted to the serialized miwear/UI-owner timer, outside IRQ/manager locks.
Every external installation dispatch has not been audited. Coarse memory
regions, selected-bucket preflight and field rechecks do not prove allocation
liveness or arbitrary-thread concurrency safety.

## File source and the recovered PSRAM setter

`quickapp_make_icon_url` `0x0c5482f0` builds `/data/app/%s/%s`. Its PNG branch
may convert and write `.bin` through `0x0c4b68a8`, restoring the extension on
conversion failure. **The module never calls this generator or writer**, and
never invents a BIN sibling for an existing PNG pathname.

The actual display chain is:

```text
app record +12: /data/app/<package>/<manifest icon>.bin
  -> load_app_info 0x0c513700: launcher record +12
  -> insert_icon 0x0c5177a4
     0x0c5177ce LDR R1,[R5,#0xC]
     0x0c5177d2 BL 0x0c587fa0
  -> setter veneer 0x0c587fa0 -> 0x1c06ceb5
  -> relocated lv_image_set_src, source implementation 0x0c17a2f4
```

The earlier unsupported report incorrectly treated `0x1c06ceb4` as missing
ROM. Startup `0x0c0c0ac8` copies flash `[0x2c10d440, 0x2c18d940)` to PSRAM data
`0x3c000000`; executable alias offset `0x6ceb4` maps to flash `0x2c17a2f4`,
or execution-flash alias **`0x0c17a2f4`**, already used by the platform adapter.
Creator veneer target `0x1c06cac0` similarly maps to `0x0c179f00`, using exact
image class `0x2cce61ec`. Hardware alias initialization includes unavailable
ROM and is not emulated by these AP probes.

Classifier `0x0c13fc54` classifies a leading `/` as FILE=1. Setter duplicates
file source via `0x0c16da78`, stores it at image `+52` with type at `+96`, and
queries decoder info via `0x0c143798`. Decoder `0x0c1435ac` opens file paths
through `lv_fs_open` `0x0c169624`. Thus QuickApp launcher BIN sources can use
the existing POSIX Hook, exact cache retirement and same-pointer native image
refresh. No special launcher, generator or speculative ROM call is added.
Preserve icon dimensions; the generic refresh does not recompute launcher scale.

## Complete evidence-window fingerprints

These are bounded AP evidence windows, **not new callable permissions/ranges**.

| Start | Bytes | SHA256 |
|---|---:|---|
| `0x2cdbb054` (service table) | 64 | `ce9bcc924fa63a2038852ac7215a2e7fb9ec43bd9f0372ed7add426ab371d282` |
| `0x0ca69e80` (active lookup) | 40 | `54580c3172264b9fe8b74afc4bc0108768b3105bb582b0e08399f7c463e74392` |
| `0x0cabe234` (hashmap get) | 158 | `6965575cc2f8fca691779a8478e6d39953aa6bc93580511fbd900a66a3918619` |
| `0x0ca6a30c` (installation) | 422 | `3da326e0c3d021977d3c232d439b9a9aa7b403a2949a72985ac6af64539cad38` |
| `0x0ca6a084` (unregistration) | 310 | `377bb63b3fa2d2e927cd916768676dbbfd26b3ab8ccd2fd76d55fa486885688f` |
| `0x0ca6a4c8` (reinstall) | 206 | `ab701f5195a15563cc3a1c680e1a5f317d5e8ef0b917144e43b84c2ee297c22f` |
| `0x0c5177a4` (insert icon) | 698 | `28477a1f00eeea0bb79387e9ee76815fefb8fea395c0ef734730a157f625c086` |
| `0x0c587fa0` (setter veneer) | 8 | `120adc30d91cf13b570aa04878f5801ea226d26c043960bae9df0955050b8701` |
| `0x0c0c0ac8` (startup copy) | 124 | `a58f3a9a926d47652079636b3b6fb52c5fadca927cc7cec192c01216d3754d19` |
| `0x0c17a2f4` (image setter) | 502 | `8f5e398b35e9f4ac648d42481ca92d0e13c9a55e061250aace6317c1e583a50a` |

## Contract, framework registration and tests

`rh_platform_quickapp_icon_path` now supports .043 with the same result codes
as Band 11: 1 gives the exactly matched registry record's registered caller-owned
canonical lowercase BIN path under `/data/app/`; package keys are opaque strings
within C-string/TSV byte budgets, not directory names. Native paths allow UTF-8 and
literal punctuation, rejecting ASCII controls/DEL, colon, backslash and traversal
without URL decoding. Shared icon paths do not imply exclusive package ownership.
0 means absent/uninitialized; -1 means invalid/transient
state; -2 means an unsupported source root/type/format. Non-success empties
output. Strings may be in bounded RAM or this exact AP's code/cached-flash
regions ending at `0x0cde8190` / `0x2cde8190`, never unproven ROM.

The private target pack **reuses** its existing exact callable record
`calendar_app_lookup_name` at `0x0ca69e81`. Its legacy name remains for calendar
compatibility; generic lookup ownership/side effects are clarified and
`EVID-RESOURCE-QUICKAPP-ICON-001` is added. No duplicate alias, downstream
permission, generated SDK change, range expansion or verifier bypass is needed.
The symbol remains **restricted / STATIC_RECOVERED / PENDING / not_probed**;
blocking metadata is conservative because the comparison ROM is unavailable.

- `python3 tests/test_quickapp_icon_native.py`: host source-compiled guards,
  record/path bounds, registry preflight and independent-output tests for all
  three exact targets, including .043 upper AP/cached-flash strings.
- `sh scripts/test-host.sh`: ASan/UBSan parser, compact snapshot and module
  install/reinstall/uninstall, same-map revision, system restore, conflict,
  transient failure and OOM tests, including both .043 supported font modes.
- `build/firmware-tests/bin/python tests/firmware_quickapp_icon.py`: production
  ARM adapter with real .043 lookup/hashmap instructions; only documented
  absent comparison/libc leaves are modeled, never the package lookup.
- `build/firmware-tests/bin/python tests/firmware_quickapp_reload_1043.py`:
  production platform C plus native .043 drop/release/tree walk/image setter;
  synthetic caches/objects and documented cache-class/GUI/heap/info leaves.
- `python3 tests/test_quickapp_allowlist.py`: schemas, exact positive, nearby,
  every other target and temporary symbol-removal negative controls. The .043
  firmware range stays `{base: 0x0c000000, size: 0x00e00000}`.

The unsigned module ELF passes the unchanged exact-target verifier
(19 sections, zero undefined symbols, 643 relocations), SHA256
`d3f5591dea04ba12f441c64988a25ee15eb09e6959a9489ff88ff68d8f1f1171`.
Test artifact: `build/resource-hook-quickapp-xiaomi-band-10-pro-3.101.043.elf`.
Both Band 11 builds remain byte-identical to their previously verified artifacts.
The 10 host, 11 ARM/AP, 4 .043 native reload and 6 allowlist tests pass, as do
all supported target/font module modes, Manager tests/typecheck, and the .043
calendar AP regression. Read-only review found no actionable integration issue.

No .043 physical-device pass, actual BIN decoding, rendering/GPU completion,
allocation/storage fault acceptance or restart recovery is claimed. PNG
fallback, memory descriptors, derived or screen-tree-external owners remain
unsupported. The built ELF requires signing and an exact-target installer
receipt before deployment; host/static acceptance is not hardware approval.

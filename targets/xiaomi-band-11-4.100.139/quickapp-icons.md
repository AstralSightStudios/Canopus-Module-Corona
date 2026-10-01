# Read-only QuickApp icon lookup: exact 4.100.139

AP SHA256: `31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74`.
Audited IDB: `build/firmware-analysis/vela_ap_4.100.139.bin.i64`.
IDA reference was read before Python execution; no IDB edits were saved.
The .139 lookup chain/table/record offsets were independently decompiled and
read from this database, **not** established by subtracting a .155 delta.

## Exact ABI and fingerprints

| Role | Exact address (even entries; callable pointers add Thumb bit) |
|---|---|
| service pointer slot | `0x20084ec4`, initial contents `0x2ca3da68` |
| expected service table | `0x2ca3da68` |
| table slot 3, package-name lookup | `0x2ca3da74`, contents `0x0c6a16a3` |
| active package-name registry pointer slot | `0x20084ebc` |
| package lookup wrapper | `0x0c6a16a2..0x0c6a16ac` |
| registry lookup wrapper | `0x0c6ac6c0..0x0c6ac6e4` |
| hashmap get | `0x0c6d2d84..0x0c6d2dcc` |
| bucket search | `0x0c6d2698..0x0c6d26d4` |
| DJB2 byte hash (seed 5381) | `0x0c5db990..0x0c5db9b0` |
| strcmp AP thunk | `0x0c720f48`, bytes `5ff800f095c02600` |

Complete-range SHA256:

- package wrapper: `a67024cd98157404fae905668a3cdf63c25e2411944f753d27b70b4d2df1d960`
  (all bytes `80b500af0bf00bf880bd`).
- registry wrapper: `f4eb7f1f61d1e91fef782ee2b4b63663a27fb0455fc9d29c828fca040d07e28e`.
- hashmap get: `40ff41c7e086147dbc74be41be9aed696ea3992e91a23d0cf376484119f87c83`.
- bucket search: `f25a2848f9ab6cef95e6f1834cb047f0180d67bb74e4ee1e6016d957df7dac9c`.
- hash: `8bf2dd88e9cfd41c3bbbcca517492be69b5226dac6d89930dbb3877fcdd07142`.

Slot 3 returns a borrowed active app record by package name. Slot 4 is a
different registry and is not used. Registry lookup gets `*0x20084ebc`, passes
an initially zeroed output to hashmap get, returns zero on miss, the node's
payload on success. Null map/key would trigger native assertions, so the
adapter checks before calling. Map `+0` is power-of-two bucket count; buckets
start `+4`, index `hash & (count-1)`. Nodes: hash `+0`, key `+4`, NUL-inclusive
key length `+8`, app record `+12`, next `+16`. Selected-bucket preflight limits
count to 4096, chain to 1024 and validates readable nodes/keys before native
lookup; corrupt pointers/cycles fail closed rather than entering native code.

## Ownership and icon source

`quickapp_register_app` `0x0c582f90` stores package `record+12`, icon file
source `record+16`. Its icon updates clear/replace that field and free the old
source. Installation `0x0c6ab350` allocates/copies 80 bytes and duplicates
package `+12`, icon `+16` (also auxiliary fields `+28`, `+32`), then inserts into
`*0x20084ebc`. Lookup does not allocate, acquire a reference or transfer
ownership. Records and source strings are manager-owned and can disappear on
update/removal. UI-owner-only serialization is mandatory; no locks are held
across the call. The module copies bounded strings during the call and returns
only its caller-owned pathname, never a borrowed pointer.

`quickapp_make_icon_url` `0x0c582c00` formats
`/data/quickapp/app/%s/%s`; PNG handling may write a converted BIN through
`0x0c4fba8c`. **Do not call this function**, registration, or the conversion
writer for resolution. A registered PNG is unsupported (-2); do not invent a
`.bin` sibling or perform conversion. The existing launcher consumes file-path
image sources, but this adapter does not set sources, redraw, or prove file
existence/decoding.

## Public contract and safety

`int rh_platform_quickapp_icon_path(const char *package, char out[RH_PATH])`:

- 1: exact package identity and canonical lowercase `.bin` path under exactly
  `/data/quickapp/app/<package>/`; caller owns the bounded NUL-terminated copy.
- 0: absent/uninitialized service, registry, app or icon.
- -1: invalid/transient service identity, callback, registry/record/string or
  unsafe path/package-boundary mismatch; retry on a later owner turn.
- -2: unsupported target or source format/type (PNG, non-file, other root).

Non-success empties output. Parent supplies grammar-validated package and
non-overlapping output. The exact table pointer and slot-3 callback must match
before the explicit call. Records must be aligned/readable RAM, with package
`+12` exactly equal to the requested package and bounded source `+16`. Strings
may reside in known RAM/AP/alias regions, not unproven ROM. Region checks are
coarse and do not prove allocation liveness; owner context remains essential.
Safe paths reject non-ASCII punctuation, backslash, control/URL characters,
empty/dot/traversal components. Owner fields are rechecked before publishing.
No filesystem, allocation, notification, rendering or cache operations occur.

## Exact framework registration and audited data identities

The sibling framework now registers only `app_lookup_package` at callable
`0x0c6a16a3`, with `EVID-RESOURCE-QUICKAPP-ICON-001`. It remains
restricted / STATIC_RECOVERED / PENDING / device-not-probed; no SDK callable
export is promoted. The initial audit found no allowed package-name wrapper;
existing restricted `app_lookup` is not evidence for this API. The function
and associated audited data roles are:

- `app_lookup_package`: entry `0x0c6a16a2`, callable `0x0c6a16a3`.
- `app_manager_service_slot`: data `0x20084ec4`, size 4.
- `app_manager_active_package_registry_slot`: data `0x20084ebc`, size 4.
- `app_manager_service_vtable`: data `0x2ca3da68`; include lookup-field address
  `0x2ca3da74` (base+12, size 4) if verifier allowlists exact values, not ranges.

Only the package wrapper is a new direct native call from C; downstream
addresses are evidence of its firmware implementation, not new direct imports.
Only the exact function record and its evidence bundle were added to the
sibling framework. Data identities above are adapter guards, not additional
call permissions. Firmware address ranges, generated SDK and verifier are
unchanged. Do not invoke indirectly to evade the absolute-address verifier.

## Tests / limitations

`python3 tests/test_quickapp_icon_native.py` (5 tests) compiles host C for
.139/.155/.043 and injects only native read/lookup leaves. It needs no Unicorn.
`build/firmware-tests/bin/python tests/firmware_quickapp_icon.py` (5 tests)
compiles production ARM C, checks exact AP hashes and executes both APs'
wrappers/hashmap/hash/bucket instructions on synthetic registries. The known
strcmp thunk leads to missing ROM `0x0026c095`; comparison is explicitly modeled,
not guessed/emulated ROM code. The AP lookup is unmodified. Probes cover absent
service/registry, exact callback identity, invalid/cyclic records, string bounds,
package/path identity, PNG/non-file rejection and independent copied output;
only stack/output writes are allowed. .043 stub returns -2 with no native read.

Both target records/evidence bundles pass schema validation. Exact callable
ELF probes pass; nearby/wrong-target addresses and removal of the record fail.
The full .139 module ELF passes the unchanged verifier, SHA256
`65a3a9fe022d6b546e5fc14355cba461a36bc84ee997efcf34cff92c57bbfadd`.
This ELF is unsigned; device lifetime/context, decoded BIN acceptance and
rendering remain release gates. No hardware success is claimed.
See the .155 `quickapp-icons.md` for the corresponding independently verified
addresses and shared guard details.

# Read-only QuickApp icon lookup: exact 4.100.155

## Scope / identity

AP SHA256: `ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f`.
Audited IDB: `/Users/lesetong/develop/temp/extract155/vela_ap.bin.i64`.
The repository's small `.155` IDB is **not** the source of this evidence.
IDA reference was read before Python execution; databases were opened through
IDA Nexus without saving edits. This is static + synthetic-registry emulation
evidence, not device acceptance or a general Band 11 ABI.

## Native read path (addresses below are even instruction entries)

| Role | Exact address |
|---|---|
| package-manager service pointer slot | `0x20084ec4` |
| expected immutable service table | `0x2ca3da58` |
| service table slot 3, package-name lookup | `0x2ca3da64`, contents `0x0c6a1693` |
| active package-name registry pointer slot | `0x20084ebc` |
| package lookup wrapper | `0x0c6a1692..0x0c6a169c` |
| registry lookup wrapper | `0x0c6ac6b0..0x0c6ac6d4` |
| hashmap get | `0x0c6d2d74..0x0c6d2dbc` |
| hashmap bucket search | `0x0c6d2688..0x0c6d26c4` |
| DJB2 byte hash (seed 5381, excludes NUL; returned key length includes NUL) | `0x0c5db990..0x0c5db9b0` |
| strcmp AP thunk | `0x0c720f38`, bytes `5ff800f095c02600` |

The strcmp thunk's literal is callable ROM `0x0026c095`; that ROM is absent
from the AP. The test models this **known leaf**, not guessed ROM instructions.
Production invokes only the explicit verified AP package lookup wrapper; its
native implementation owns the normal strcmp dependency. Table slot 4 is a
separate registry and is deliberately not searched.

SHA256 of complete instruction ranges above (including the wrapper's native
literal/end branch where present):

- package wrapper: `a67024cd98157404fae905668a3cdf63c25e2411944f753d27b70b4d2df1d960`
  (all bytes `80b500af0bf00bf880bd`).
- registry wrapper: `f4eb7f1f61d1e91fef782ee2b4b63663a27fb0455fc9d29c828fca040d07e28e`.
- hashmap get: `4ec12501194f38d68c27b5786e75668ce624eb5d1b02ebc3ee4256d36eaf7fce`.
- bucket search: `f25a2848f9ab6cef95e6f1834cb047f0180d67bb74e4ee1e6016d957df7dac9c`.
- hash: `8bf2dd88e9cfd41c3bbbcca517492be69b5226dac6d89930dbb3877fcdd07142`.

The registry wrapper initializes an output pointer to zero, gets
`*0x20084ebc`, calls hashmap get, and returns the stored record on success,
zero on miss. Get asserts for null map/key; it reads node payload `+12`.
Map layout: bucket count at `+0`, bucket pointers at `+4`; selected index
`hash & (count-1)`. Nodes: hash `+0`, key `+4`, NUL-inclusive length `+8`,
record `+12`, next `+16`. Map constructor `0x0c6db39a` uses the rounded count
from `0x0c3eae6c`, allocates `(count+1)*4` zeroed bytes, stores count.

## Record/source ownership

`quickapp_register_app` `0x0c582f90` copies the app descriptor package pointer
into its temporary descriptor at `+12`, generated icon pathname into `+16`.
Installation `0x0c6ab340` allocates/copies an 80-byte manager record and
**duplicates** package `+12` and icon `+16`, then stores it in the active map.
Lookup is borrowed: it does not allocate, acquire a reference, or transfer
ownership. Updates explicitly clear/replace `record+16` and free the previous
pathname. Removal may unlink/free the record and its strings. Therefore no raw
record/string pointer can survive an owner turn. The adapter is UI-owner only,
never IRQ/locked-worker code; bounded local copies are returned, not pointers.

`quickapp_make_icon_url` `0x0c582c00` formats
`/data/quickapp/app/%s/%s`, reads PNG and can **write BIN to disk** through
`0x0c4fba8c`. Never call it for resolution, nor synthesize a BIN sibling of a
registered PNG. Native launcher uses file-path image sources; resolution does
not publish a source, redraw, check file existence, or prove decoding.

## Adapter guards / result

`rh_platform_quickapp_icon_path` validates exact service pointer and slot-3
Thumb callback **before** its explicit native invocation. Null service/map or
absent app/icon returns 0. Before native lookup, it preflights the selected map
bucket: readable aligned RAM, power-of-two count <=4096, <=1024 chain nodes,
bounded readable keys and correct NUL-inclusive lengths. Records must be
aligned/readable RAM, with exactly matching bounded package at `+12` and bounded
icon at `+16`. Strings may also reside in the known AP/alias range; no ROM
string dereference is assumed. Those region checks are coarse firmware-region
checks, not allocation-liveness proof; owner serialization remains mandatory.

Success 1 requires canonical ASCII absolute path beneath exactly
`/data/quickapp/app/<package>/`, no empty/dot/traversal components, and lowercase
`.bin`. Invalid records/unsafe paths/root-package mismatch return -1; non-file
sources, other roots or formats (including PNG) return -2. .043 returns -2.
Every failure empties output. No allocator, file open/read/write, notification,
registration, rendering, cache eviction, or make-icon call is used.

## Exact framework registration and audited data identities

The sibling framework now registers only `app_lookup_package` at callable
`0x0c6a1693`, with `EVID-RESOURCE-QUICKAPP-ICON-001`. It remains
restricted / STATIC_RECOVERED / PENDING / device-not-probed; no SDK callable
export is promoted. The initial audit found no allowed package-name lookup.
Existing restricted `app_lookup` is not evidence for this wrapper. The function
and associated audited data roles are:

- `app_lookup_package`: entry `0x0c6a1692`, Thumb callable `0x0c6a1693`.
- `app_manager_service_slot`: data `0x20084ec4`, size 4.
- `app_manager_active_package_registry_slot`: data `0x20084ebc`, size 4.
- `app_manager_service_vtable`: data `0x2ca3da58`; adapter needs base identity
  and lookup-field data `0x2ca3da64` (base+12, size 4). If the verifier uses exact
  values rather than data ranges, allowlist this field address separately.

Only the AP lookup wrapper is called directly by module C. Its downstream
registry/hashmap/hash/strcmp addresses above are transitive firmware evidence,
not additional direct-call imports. Do not route indirectly through the table
to evade verifier rejection. Only the exact function record and its evidence
bundle were added to the sibling framework; data identities above are guards,
not additional call permissions. Ranges, generated SDK and verifier are unchanged.

## Tests

- `python3 tests/test_quickapp_icon_native.py`: host C builds for all three
  targets; injects only read/lookup leaves, no Unicorn dependency (5 tests).
- `build/firmware-tests/bin/python tests/firmware_quickapp_icon.py`: builds
  production ARM C and executes both fingerprinted APs' package wrapper,
  registry wrapper, hash, hashmap get and bucket search (5 tests). strcmp is a
  documented modeled leaf. Synthetic maps/records exercise misses, identity,
  bad pointers, bounded strings, cycles, unsafe paths, PNG/non-file rejection,
  exact service guards and independent output ownership. Only stack/output
  writes are permitted; .043 executes only its unsupported module stub.

Both target records/evidence bundles pass schema validation. Exact callable
ELF probes pass; nearby/wrong-target addresses and removal of the record fail.
The full .155 module ELF passes the unchanged verifier, SHA256
`c9b6253d832ae1b23544ca3e3d19a3038674ab7a8794be26fcf1c9bea43d9816`.
This ELF is unsigned. The user reports **USER_REPORTED_PASS** for
QuickApp launcher icon replacement on Band 11 (.155). No device logs or fault
injection were supplied. Broader GPU, storage/allocation failure and
restart-recovery acceptance remain unverified.

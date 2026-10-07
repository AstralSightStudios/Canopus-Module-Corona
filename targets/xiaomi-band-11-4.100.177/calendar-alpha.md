# Calendar Alpha adapter: exact .177 restricted approval

AP SHA256: `ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b`.
Database: exact q66-ap.i64; parent/native-agent analysis was read-only.
Per-symbol status: **STATIC_RECOVERED / NOT_DEVICE_PROBED / PRIVATE_API_APPROVED**.
Visible workflow: **USER_REPORTED_PASS** (2026-10-07). The user reported successful
device validation of this workstream; no artifact hash, logs, screenshots or
fault/configuration matrix was supplied. This does not promote individual API
proofs, pixel identity, GPU recovery or restart safety.
Explicit user approval in the current session covers the 15 missing exact private
entries in the adjacent Canopus pack, revision 2. Each record remains policy
`restricted` with no public callable wrapper. Existing unrelated symbol approvals,
firmware ranges, capabilities and verifier behavior remain unchanged.

The old generator `0x0c547870` remains immutable and is never called by the
module. The default replacement's object attributes are compared against its
instructions by `tests/test_calendar_native.py`; only the snapshot format
argument changes from 18 (RGB565) to 16 (ARGB8888). Native publication remains
`0x0c54779d` (Thumb), with its ordinary app-69/hidden/valid-object checks.

## Native leaves used by the replacement

Addresses below are **instruction addresses without the Thumb bit**. Callable
entries use `address | 1`; receipt inspection reads the even instruction address.
Approval covers only independently reviewed missing exact entries. The Canopus
evidence `EVID-RESOURCE-CALENDAR-ALPHA-177-001` records full-function boundaries
and AP byte hashes, prototypes, ownership and the explicit user promotion.
This table is not authorization to bypass or broaden the allowlist.

| Address | Recovery purpose |
| --- | --- |
| 0x0c34d45c | realtime timestamp, seconds64+nsec32; R0 is saved BASEPRI, not success status |
| 0x0c34fcec | localtime_r; native tm mday+12, mon+16, year+20, Sunday-based weekday+24 |
| 0x0c382ff0 | native root creation |
| 0x0c3836d2 | object dimensions |
| 0x0c502de0 | native root style; not snapshotted into the image child |
| 0x0c3ae134 | image creation |
| 0x0c3b2b8c | image source setter |
| 0x0c38e44c | image metadata validation |
| 0x0c3ab228 | label creation |
| 0x0c3b38ac | copying label text setter |
| 0x0c3839ae | text color property 88, packed B,G,R |
| 0x0c383a10 | text alignment |
| 0x0c3839e4 | text font |
| 0x0c38361c | object alignment and offsets |
| 0x0c50934c | retained native font-style wrapper resolution |
| 0x0c495008 | independent managed font create for custom size |
| 0x0c4924fc | managed font release after successful GPU fence |
| 0x0c542cdc | native localized weekday text, 16-byte destination |
| 0x0c6a29c8 | snapshot wrapper; object child, color format 16 |
| 0x0c6a2a24 | snapshot release |
| 0x0c384488 | temporary root deletion |
| 0x0c91e58c | healthy VG-Lite finish boundary |
| 0x0c92e5f4 | data-cache synchronization |
| 0x0c721248 | data-cache invalidation |
| 0x0c907918 | ARGB premultiply, flags-aware |
| 0x0c8b7720 | optional draw-buffer handler+12 over the full pixel rectangle; not a guaranteed cache/fence operation |

Default font-style holders: `0x200c45dc` (day, MiSans-Medium 48) and
`0x200c5af0` (weekday, MiSans-Demibold 20). Independent custom-size wrappers use
native family strings `0x2cbc6578` / `0x2cbc6568`; the static allocation-failure
fallback `0x2ca12930` is rejected and never freed. The adapter does not directly mutate shared font wrappers, native fallback links
or FT face sizes. Native managed creation may resize cached/shared FT faces
internally; owned wrappers do not imply fully independent underlying faces.

The native weekday color is exactly `0x3482ff`: the stock sequence stores
low16 `0x82ff` and byte2 `0x34`; C3839AE passes property 88 without swapping
channels. The earlier proposed `#F0403F` was not a recovered .177 value.

## Snapshot/file ownership

Draw buffer: header 12 bytes, data_size+12, aligned pixels+16, raw allocation+20.
The save path validates ARGB8888, 112x112, aligned stride >=448, exact
`data_size == stride * 112`, supported flags and zero reserved header fields.
Pixels are read only after the GPU fence and cache invalidation. Premultiplied
Alpha is preserved; RAM-only flags 0x30 are removed from the file header.
Write mode is native `0x426`; platform supplies 0600. Complete-write loops detect
short/failed writes, but the existing void close API does not certify close or
storage durability. A successful file is published through an independent bank;
no system `calendar_icon.bin` is overwritten.

GPU failure keeps one bounded pending tree/buffer/fonts set. A subsequent
successful finish retires it before a new generation. This is not a general
fault recovery or framework restart contract. The old logical decoder key is
retired both before rendering and immediately before bank publication because
reentrant callbacks can repopulate the old bank during snapshot work.

## Approval and checks

- Portable parser/runtime/coordinator tests and Manager calendar tests pass.
- Ten native Unicorn tests cover default-attribute comparison against original
  instructions, exact color packer/premultiply instructions, local time/locale,
  clock-output guards, malformed snapshot/write failure and GPU-owned lifetimes.
- Six source-compiled adapter tests cover exact retire keys, configured images,
  invalid cache/display guards, publisher-only behavior and return propagation.
- ARM .177 object/ELF compilation succeeds, with no undefined symbols.
- The 15 new exact records have explicit user approval and remain restricted.
  Existing duplicate-address records were reused rather than aliased/promoted.
  C/Rust output is regenerated normally; approved restricted entries still do
  not emit public wrappers. Removal of an exact record and a wrong-target pack
  both fail the unchanged verifier in `tests/test_calendar_allowlist.py`.
- Strict `.177` ELF verification now passes: 20 sections, zero undefined
  symbols, 1425 relocations. Scoped Canopus generation stability and generated
  Rust SDK compilation pass. Its unrelated `.036` generated `poll_notify`
  staleness still fails the full workspace stability suite.
- An unsigned ELF is not a signed deployment or device acceptance result. No
  installer, key, physical probe, range expansion or verifier exception is added.

See `docs/CALENDAR_RELOAD.md` for the configuration and acceptance boundary.

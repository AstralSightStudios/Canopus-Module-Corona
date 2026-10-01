# QuickApp icon lookup: exact 3.101.043 is unsupported in v1

AP SHA256: `519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec`.
Audited IDB: `build/firmware-analysis/vela_ap_3.101.043.bin.i64`.
IDA reference was read before execution; this IDB was opened without saving.

`rh_platform_quickapp_icon_path` compiled with `RH_TARGET_1043=1` returns **-2**
and clears output. It performs no native service reads, lookup calls, ROM
calls, generation, file writes, registration, source publication or redraw.
Parent rejects package rules before install on this target; the public native
adapter also fails closed independently.

## Evidence, not support claims

The exact .043 builder `quickapp_make_icon_url` at `0x0c5482f0` was decompiled
in this IDB. It formats `/data/app/%s/%s`, not the Band 11 root. Its PNG branch
loads an image, changes `.png` to `.bin`, calls writer `0x0c4b68a8`, then
restores the extension on conversion failure. It is **not read-only** and must
never serve as a resolver. Earlier record-layout findings are package `+8`
and icon `+12`, different from .139/.155 (`+12`, `+16`). They are not used by
this version. The relevant display setter ROM thunk `0x1c06ceb4` is unavailable
in the AP; do not infer native instructions or assume a Band 11 substitute.

No .043 package-lookup/record-lifetime ABI or framework allowlist additions are
claimed by this implementation. Supporting it later requires independent
lookup/service identities, record ownership/lifetime, file source semantics,
read-only tests and device context validation. Merely changing a root or field
offset is not a port.

## Tests

- `python3 tests/test_quickapp_icon_native.py`: dependency-free host C test
  confirms unsupported for good/bad/null package and null output, no native
  read/lookup callback use.
- `build/firmware-tests/bin/python tests/firmware_quickapp_icon.py`: compiles the
  .043 production ARM stub and executes only module instructions. Unsupported
  return is checked with no AP lookup execution. This does **not** claim .043
  firmware lookup/display success.

The same suites execute supported .139/.155 guard/lookup cases separately;
their target-specific documents describe the actual native evidence.

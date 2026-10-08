# Band 11 .155 background and date/calendar backport

Enables the latest .177 launcher/control-center/notification backgrounds and
transparent calendar/date renderer on exact 4.100.155. The shared implementation
retains frame-driven blur, last-good pixels, bounded GPU-failure retirement,
checked calendar writes and locale/date refresh. .043 is unchanged.

AP SHA256:
`ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f`.
`background-calendar-evidence.json` records target-specific function blocks,
normalized Thumb bodies and ordered call/literal witnesses. The .155 header is
independent of .139; even nearby entries differ (calendar snapshot release,
GPU/cache routines, class descriptors and managed-font fallback).

Independent parent IDA audit checked 153 function starts and full flowchart
boundaries per image across .177/.139/.155. Exact AP re-fingerprinting reproduced
all 62 mapped roles per target and their ordered references. The native .155
calendar generator was independently decompiled; class descriptors preserve the
non-pointer layout fields and map parent pointers correctly. No global delta,
first-match thin-wrapper choice, public veneer or blanket allowlist was used.

Use the opt-in runner (capstone, unicorn, pyelftools required):
```sh
CANOPUS_ROOT=/path/to/Canopus-Private PYTHON=/path/to/python CC=/usr/bin/clang sh scripts/test-band11-features.sh
CANOPUS_ROOT=/path/to/Canopus-Private CC=/usr/bin/clang sh scripts/build.sh xiaomi-band-11-4.100.155
```
AP defaults are SDK `fwbins`; `RH_AP_139`, `RH_AP_155`, `RH_AP_177` override them.
The runner explicitly requires all three APs. Individual calendar runs accept
`RH_CALENDAR_TARGET=155` and `RH_CALENDAR_FIRMWARE`; background tests accept
`RH_BACKGROUND_TARGET=155`. `CC`, `CLANG` and `LD_LLD` select host/ARM tools.
`RH_TEST_BAND11_FEATURES=1` appends this suite to the ordinary host runner.

Validation results are static/model only. Native leaves are mocked except the
stock generator and color routines executed in Unicorn; tests do not certify
physical pixels, device lifecycle, GPU recovery safety or frame rate. No .155
hardware, signing or installation acceptance is claimed. Exact private ABI
approval belongs to the external SDK; static recovery alone is not approval.

Validation completed: 10 native calendar, 10 launcher and 40 blur tests for each
of .139/.155/.177, plus three AP receipt/capability/header tests and the full
portable host suite. Production .155 ARM build and Canopus verification passed
(20 sections, 0 undefined symbols, 1417 relocations in the final parent regression build). A subsequent .139 regression
build passed the same checks. Parent independently checked all 46 .155 mapped
function-block receipts in IDA and class layout equality. External SDK commit
`ce1fdbf` supplies .155 exact restricted records; parent verified positive probes,
withdrawn-record rejection and rejection of .177 addresses in the .155 pack.
No SDK files were modified by this public port commit.

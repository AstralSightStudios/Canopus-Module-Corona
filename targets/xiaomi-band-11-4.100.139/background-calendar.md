# Band 11 .139 background and date/calendar backport

Ports the latest .177 launcher/control-center/notification backgrounds (including
live blur frame ownership, last-good pixels and GPU-failure retirement) and the
transparent 112x112 calendar/date renderer. Band 10 Pro .043 remains unchanged.

Exact extracted AP SHA256:
`31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74`.
The supplied OTA was unzipped, then analyzed with IDA 9.4 and the supplied
`best1503_vela.py` loader (XIP 0x0c0c0000, cached alias 0x2c0c0000).
`background-calendar-evidence.json` records complete basic-block byte hashes,
normalized Thumb bodies and ordered caller/literal witnesses. Thin creation and
size wrappers were resolved by their callers, not duplicate-body first matches.
The visible-area routine was independently decompiled: .139/.155 obtains the
active screen through its display argument whereas .177 uses a default-display
wrapper; the object/area contract and clipping/ancestor checks agree.
No global address delta was used. Font fallback, exact MiSans name strings,
shared default styles, launcher slots and draw-buffer handlers are target-specific.

Validation (static/models only):
- 10 native calendar Unicorn tests, including actual stock generator defaults,
  native color packing, date/locale, short writes, wrong receipts and GPU faults.
- 40 blur and 10 launcher host native-call mock tests with .139 constants.
- Full host suite, ARM compile/link and production ELF policy verification with
  the updated exact restricted records in the external Canopus target pack.
- Exact function-block and global-literal receipts; .043 capabilities stay off.

The opt-in runner executes .139, .155 and .177 native calendar, launcher and blur tests,
then .139/.155 AP receipts. AP defaults are the selected SDK's `fwbins` directory;
`RH_AP_139`/`RH_AP_155`/`RH_AP_177` override them. Missing APs fail the runner explicitly.
Individual calendar tests also accept `RH_CALENDAR_FIRMWARE`. `PYTHON` selects a
Python environment with capstone, unicorn and pyelftools; `CC`, `CLANG` and
`LD_LLD` select the required host/ARM tools.

Examples:
```sh
CANOPUS_ROOT=/path/to/Canopus-Private PYTHON=/path/to/python CC=/usr/bin/clang sh scripts/test-band11-features.sh
# Or append the dependency-equipped suite to the portable host runner:
CANOPUS_ROOT=/path/to/Canopus-Private RH_TEST_BAND11_FEATURES=1 PYTHON=/path/to/python CC=/usr/bin/clang sh scripts/test-host.sh
RH_AP_139=/path/to/139/vela_ap.bin python3 tests/test_band11_feature_ports.py
RH_CALENDAR_TARGET=139 RH_CALENDAR_FIRMWARE=/path/to/139/vela_ap.bin python3 tests/test_calendar_native.py
RH_BACKGROUND_TARGET=139 CC=/usr/bin/clang python3 tests/test_blur_background_native.py
RH_BACKGROUND_TARGET=139 CC=/usr/bin/clang python3 tests/test_launcher_background_native.py
CANOPUS_ROOT=/path/to/Canopus-Private CC=/usr/bin/clang sh scripts/build.sh xiaomi-band-11-4.100.139
```

No .139 hardware validation, signing/install success, physical pixels, frame rate
or GPU safety is claimed. Static recovery is not private ABI approval; no public
SDK veneer, blanket range expansion or verifier override is part of this port.

Independent parent audit: IDA function starts and full flowchart boundaries were
checked across .177/.139/.155 (153 entries per image); all 62 mapped roles per
target reproduced exact-AP `thumb-full-v1` body hashes and ordered references.
The .139 mapped-function block receipts (46 functions), native generator and
snapshot release were checked independently. List/base/notification descriptors
retain their non-pointer layout fields and correctly mapped parent pointers.
With the updated external exact private ABI records, the .139 production build
passed Canopus ELF verification: 20 sections, 0 undefined symbols, 1417
relocations. These are static/model results, not device or GPU acceptance.

# P65 native custom calendar

Exact `vela_ap.bin` SHA256:
`a19b601477569765103eaf468b937bd8026b4d68ffa5c896bde222a79e475858`.
All identities remain restricted / PENDING / NOT_PROBED. No physical device,
GPU rendering, recovery or production approval is inferred from the tests.

P65 uses a **96×96 file icon**. Stock generator `0x0c7efc8c` creates the image
from `/resource/app/perpetual_calendar/launcher_icon.bin`, adds day and localized
weekday, then delayed callback `0x0c7efe50` snapshots the image as ARGB8888,
saves `/data/quickapp/app/perpetual_calendar/calendar_icon.bin`, and calls
launcher update `0x0c7ec744(69)`. This is different from P67's RAM descriptor.
The module never overwrites that stock file. It writes only its own alternating
file banks; the coordinator redirects the stock logical read key after a full
render and two successful targeted cache retirements.

`src/p65_calendar_native.inc` implements the exact adapter. Header identities,
32-byte original body receipts, startup mapping file offsets and ownership
contracts are recorded in `calendar-evidence.json`; corresponding private
framework records reference `EVID-RH-P65-CALENDAR-001`.

- Clock `0x0c4c205c(0, timespec64)` and localtime `0x0c761a2c(lo,hi,tm)` preserve
  the native 64-bit seconds calling convention. Month/day/weekday contexts use
  the existing checked config logic; locale is fingerprinted from native text.
- Default text uses stock managed font styles: Medium 46 day and Medium 20
  weekday. Configured sizes create independent managed wrapper ownership via
  `0x0c795db8(manager,key8)` and release with `0x0c795fe4(manager,wrapper)`.
  The module never resizes shared FreeType faces directly.
- Image contexts retain their BIN width and height (`image+64/+68`). The config
  `size` field applies only to text. The root and background are 96×96.
- Caller-owned draw buffer (`0x0c719550`) plus snapshot-to-buffer `0x0c7417c0`
  prevents the allocating snapshot helper's early failure cleanup from freeing
  a potentially submitted buffer before the caller's completion fence.
- A GPU finish failure retains the tree, paths, wrappers and buffer as one bounded
  pending generation. Retry/reset obtains a successful raw GPU finish, checks
  draw units (`dispatch+12`, `active+24`), and invokes exact native VG wait
  `0x0c72ef2c` to retire borrowed CPU references before cleanup.
- Native premultiply `0x0c719b70` is used, and the PREMULTIPLIED output flag is
  checked. File output strips RAM-only mutable/allocated flags. Complete writes
  are checked through the Supervisor file service; a failed write never commits
  a new bank. Native stock output stays intact.
- Default restoration completes any retained work, clears bank redirection in
  the coordinator, and then calls the targeted native launcher update. It does
  not regenerate or overwrite the stock calendar icon.

Validation: `build/p65-tests/bin/python tests/test_p65_calendar_native.py`
passes nine source-compiled ARM tests: date/clock failure, text size ownership and
native premultiplied pixels, natural-size custom images and missing headers,
failed snapshot/short write, failed fence retention/reset, default-stock refresh, wrong-receipt rejection and active-draw-unit refusal. Most native UI/OS/GPU leaves are modeled; the original native ARGB8888
premultiply instructions execute on the exact AP mapping. This is not pixel
rendering or a complete device boot.

The native launcher only selects the generated calendar key when its stock
producer file exists. Custom rendering preflights that original file read-only
before allocating/drawing; if absent, the request fails and preserves the old
publication until the firmware producer has run. The module never creates or
rewrites the original merely to satisfy the native access check.

P65's exact platform cache hooks are no-op bodies at `0x0c72f020` /
`0x0c72f02c`. The adapter requires their installed table identities before
work, calls clean on the descriptor before snapshot, and calls native invalidate
on pixels after successful GPU finish before premultiply and file output.

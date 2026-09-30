# Resource-Hook startup diagnostics

A Canopus `-6` is not a Resource-Hook internal return code. Do not infer its
meaning from unrelated quickapp `-6` messages in an offline-log archive.
This build records the module-side lifecycle independently of Canopus logging.

## Collecting a trace

Install the newly built, signed module, enable it, then reboot once. Copy this
file from the device filesystem **before reinstalling or retrying**:

```text
/data/offlinelog/resource-hook-startup.log
```

The supplied device archives confirm `/data/offlinelog` is the firmware log
directory. The module creates only a standalone log file there, with owner-only
permissions; it never overwrites `tmp.log` or the numbered firmware logs. Export
the device logs after the failure and look for `resource-hook-startup.log`. If
an exporter filters auxiliary files, retrieve this file separately from the
accessible device log directory. No Canopus source changes or Manager UI changes
are required.

The file begins with the exact target, module version and config path. Each row
contains a stage, its return value, an immediately captured errno where relevant,
and a hexadecimal detail value. A constructor creates a new trace. Repeated
activations retain the latest bounded snapshot (at most 2048 bytes); the module
uses no heap for logging. Log open/write failures do not change lifecycle results.
Only lifecycle transitions are recorded, not every resource open or timer tick.
Filesystem I/O always occurs outside the hook-publication IRQ lock.

Example (pointer values omitted):

```text
RHSTART1 target=xiaomi-band-10-pro-3.101.043 version=0.3.0
config=/data/quickapp/files/ng.lst.corona/mappings.tsv
ctor.begin rc=0 errno=0 detail=0x...
register.begin rc=0 errno=0 detail=0x...
register.open rc=3 errno=0 detail=0x...
register.write rc=40 errno=0 detail=0x...
ctor.end rc=0 errno=0 detail=0x...
activate.begin rc=0 errno=0 detail=0x...
prepare.begin rc=0 errno=0 detail=0x...
config.open rc=-1 errno=2 detail=0x...
...
prepare.end rc=0 errno=0 detail=0x...
hook.end rc=0 errno=0 detail=0x...
watch.end rc=0 errno=0 detail=0x...
activate.end rc=0 errno=0 detail=0x...
```

## Interpretation

- No new file: the constructor may not have run, or diagnostic file I/O may have
  failed (including an unavailable log directory), or the exporter filtered the
  file. Absence alone does **not** prove loader/signature failure. Confirm the
  installed ELF/receipt hashes and check the device log directory directly.
- `register.open` failure or `register.write` other than 40: descriptor publication
  failed. Registration writes are atomic and are never retried after a short write.
- `config.open`: negative results include errno captured **before** diagnostic
  file opens. ENOENT (2) is an empty pass-through startup, not an activation error.
- `prepare.end`: `-2004` already installed; `-2005` non-ENOENT config open;
  `-2006` staging allocation; `-2007` config read/validation; `-2013` snapshot allocation.
- `driver.valid`: detail is the POSIX open-slot address; rc is the identity check.
  `slot.expected` and `slot.observed` show the expected and actual callback values.
  `hook.end`: `-2008` driver/snapshot validation; `-2009` unexpected slot;
  `-2010` hook publication failure.
- `refresh.end`: `-2011` refresh timer allocation. `watch.end`: `-2012` watcher
  allocation. A failed timer may leave the already installed hook resident.
- A `*.begin` without its matching end helps isolate the interrupted native call,
  but a torn/failed diagnostic write can also truncate the trace.

## .043 native-open correction

The exact AP-image SHA-256 remains
`519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec`.
IDA inspection of that image shows:

- `0x0c1d0a28` (Thumb `0x0c1d0a29`, our existing `RH_FW_OPEN`) returns negative
  native errno directly, without updating task errno (`nx_open` contract).
- The adjacent POSIX wrapper at `0x0c1d0a98` explicitly writes `-result` through
  `0x0c1e45bc` and returns `-1`.
- `file_open` at `0x0c1d06b0` reads the permission vararg when flags contain bit 2.
  Regular-file creation in `mknod` at `0x0c1e6750` uses flags `38` (`0x26`) and
  an explicit permission argument.

The shared adapter now normalizes the **existing**, whitelisted .043 native
entry to POSIX `-1 + task errno`, and supplies `0600` permissions. It does not add
an unapproved firmware address. Band 11 already uses a POSIX-open entry and does
not receive this normalization.

This fixes a statically confirmed contract mismatch; it does not yet prove that
it was the only cause of the reported device activation failure. The supplied
archive also shows quickapp files under `/data/files/ng.lst.corona/`. Firmware
analysis confirms this is the Band 10 Pro app-files root; Band 11 uses
`/data/quickapp/files/ng.lst.corona/`. The module now selects the native root by
build target and expands relative TSV destinations such as `themes/current/`.
No storage migration or absolute-destination TSV compatibility is provided.

# Resource-Hook startup diagnostics

A Canopus `-6` is not a Resource-Hook internal return code. Do not infer its
meaning from unrelated quickapp `-6` messages in an offline-log archive.
This build records the module-side lifecycle independently of Canopus logging.

## Collecting a trace

Install the newly built module, enable it, then reboot once. Copy this
file from the device filesystem **before reinstalling or retrying**:

```text
/data/offlinelog/resource-hook-startup.log
```

The supplied device archives confirm `/data/offlinelog` is the firmware log
directory. The module writes a standalone file through `ctx->fs`; the current
Supervisor replaces files by unlinking and creating them with mode `0666`. It
never overwrites `tmp.log` or the numbered firmware logs. Export
the device logs after the failure and look for `resource-hook-startup.log`. If
an exporter filters auxiliary files, retrieve this file separately from the
accessible device log directory. No Canopus source changes or Manager UI changes
are required.

The file begins with the exact target, module version and config path. Each row
contains a stage, its return value, the positive error number derived from a
negative service result where relevant, and a hexadecimal detail value. No task
errno is read. A constructor creates a new trace. Repeated
activations retain the latest bounded snapshot (at most 2048 bytes); the module
uses no heap for logging. Log open/write failures do not change lifecycle results.
Only lifecycle transitions are recorded, not every resource open or timer tick.
Filesystem I/O always occurs outside the hook-publication IRQ lock.

Band 11 receives its context in the module entry and registers through the
MODULE service. Band 10 Pro stock modlib supplies no context: the SDK registers
through one atomic CMR1 write to `/dev/canopus`. Before `prepare` receives a
context, its trace remains in the bounded RAM buffer; it cannot write a log file.
A registration failure on that path may therefore leave no new file. The FS
service itself refuses `/dev/canopus`; registration never uses `ctx->fs`.

Example (pointer values omitted):

```text
RHSTART1 target=xiaomi-band-10-pro-3.101.043 version=0.3.0
config=/data/files/ng.lst.corona/mappings.tsv
ctor.begin rc=0 errno=0 detail=0x...
register.begin rc=0 errno=0 detail=0x...
register.end rc=0 errno=0 detail=0x...
ctor.end rc=0 errno=0 detail=0x...
activate.begin rc=0 errno=0 detail=0x...
prepare.begin rc=0 errno=0 detail=0x...
config.open rc=-2 errno=2 detail=0x...
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
- `register.end`: the SDK registration result, zero on success and negative on
  failure. The SDK selects MODULE service registration or stock-modlib CMR1
  transport; the latter never retries a partial device message.
- `config.open`: a negative service error is retained directly; `errno` is its
  positive magnitude. Diagnostic file opens cannot change it. NOTFOUND (`-2`,
  logged as `errno=2`) is an empty pass-through startup, not an activation error.
- `config.fallback`: `-2005` non-ENOENT config open; `-2006` input scratch,
  compact snapshot or QuickApp materialization allocation; `-2007` config
  read/parse/validation or materialization failure. These raw codes are retained
  in the in-memory configuration status as well as the log, but are not startup
  return values: `prepare.end` returns zero and activation continues with an allocation-free
  empty map on first startup, or a previously prepared good map on a failed
  re-prepare. No partial rules are published and the config file is never rewritten.
  A compatible controller can repair the file and send a reload revision without
  rebooting. Configuration errors stay sticky until successful explicit configuration
  publication; background QuickApp resolution errors are tracked separately and
  recover independently. Runtime reload failures still report an error receipt and
  preserve the good map. Missing, empty or comment-only startup configuration is
  healthy. See [MODULE_CONTROL.md](MODULE_CONTROL.md) for status requests; the
  unchanged Manager still uses the old channel and requires a separate upgrade.
- `prepare.end`: `-2004` already installed; otherwise zero, including config
  fallback. The former fixed-bank `-2013` allocation path is no longer used.
  `config.alloc` and `snapshot.alloc` detail identify the actual-sized snapshot
  (zero for an empty map); `config.materialize` records QuickApp expansion when
  present. A missing config needs no rule-storage allocation.
- `driver.valid`: detail is the POSIX open-slot address; rc is the identity check.
  `slot.expected` and `slot.observed` show the expected and actual callback values.
  `hook.end`: `-2008` driver/snapshot validation; `-2009` unexpected slot;
  `-2010` hook publication failure.
- `refresh.end`: `-2011` refresh timer allocation. `watch.end`: `-2012` watcher
  allocation. A failed timer may leave the already installed hook resident.
- A `*.begin` without its matching end helps isolate the interrupted native call,
  but a torn/failed diagnostic write can also truncate the trace.

## File-service contract

Module-owned file I/O uses `ctx->fs` on every target: configuration, control
requests/responses, font digest reads, calendar image banks and diagnostics.
The platform adapter accepts only `RH_FILE_READ` (existing file) and
`RH_FILE_REPLACE` (create/replace), mapped to `CANOPUS_FS_READ` and
`CANOPUS_FS_WRITE`. Service handles are module-private, and zero is valid;
failures are negative service error codes, with no task-errno side channel.
The adapter no longer calls the module's former native open/read/write/close
addresses or its `.043` errno-location address.

The current Supervisor's WRITE implementation unlinks before creating a file
with mode `0666`. This differs from the previous in-place `O_TRUNC` with `0600`;
replacement is not atomic and can leave no file or a partial file after an I/O
failure. Complete-write loops still reject incomplete calendar banks and control
responses; diagnostic failures remain best-effort.

Resource Open Hook callbacks retain the original LVGL driver's calling contract
and native return handle. They never return a Canopus FS handle to LVGL.

Band 10 Pro app files remain under `/data/files/ng.lst.corona/`; Band 11 uses
`/data/quickapp/files/ng.lst.corona/`. The module selects the native root by build
target and expands relative TSV destinations such as `themes/current/`.
No storage migration or absolute-destination TSV compatibility is provided.

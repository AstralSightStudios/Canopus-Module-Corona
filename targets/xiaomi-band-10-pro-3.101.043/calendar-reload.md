# Native dynamic calendar reload: xiaomi-band-10-pro-3.101.043

Implementation and limits: [calendar reload contract](../../docs/CALENDAR_RELOAD.md).

Exact AP SHA-256: `519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec` (13795728 bytes).

## Current RAM-only publication

Custom configuration snapshots to one of two module-owned 112x112 ARGB8888 RAM
buffers. The recovered signal-6 consumer stores that descriptor at `app+12`;
publication hooks `app+56` and sends EventBus 43. There is no derived file write,
file-bank selection, final-icon POSIX redirect or output-file cache retirement.
Source files/configuration and targeted input-cache retirement remain unchanged.
Prepared buffers are committed only after both cache retirements; failures keep
the previous signal source. Defaults restore the stock callback/notifier after a
successful GPU fence. The RAM buffers remain allocated for reuse.

`tests/test_p67_calendar_native.py` compiles the real renderer and calendar owner
and models native leaves; without `RH_AP_1043` it uses synthetic receipt words.
It covers rendering/publication, date/locale rollover, delayed application lookup,
metadata/allocation/snapshot/fence failures, publication-boundary cancellation,
retained tree/image-path lifetimes and stock fallback. The RAM-only revision is
**NOT_PROBED** on device; the historical user report below is not acceptance of
this changed path. See the linked contract for GPU/restart and native stock
failure limits.

## Exact native entries

| Symbol | Even entry | Thumb callable | Evidence-window bytes | AP window SHA-256 |
| --- | --- | --- | --- | --- |
| calendar_app_lookup | `0xca69934` | `0xca69935` | 56 | `9995be2ff8a60fa0f03829ff6cc0438d7710e4b8308ce49ea5727159c9e99b39` |
| calendar_app_lookup_other | `0xca6996c` | `0xca6996d` | 56 | `ac2e3c2e6c5ed330e04ec395a1fe6aebd2e581a592e1d26b0f42a6d9aaaea83b` |
| calendar_icon_notify | `0xca6a004` | `0xca6a005` | 64 | `48272b1861ab2cf2a6fb57f472fec749ffb852906fb693d3c2923c6cf071a35f` |
| calendar_app_signal | `0xc4efde8` | `0xc4efde9` | 564 | `b1f4b52a0f18f2c4ff86248d0714f8d49a88493867cd39b77c9c4c10a477e71b` |
| calendar_app_lookup_name | `0xca69e80` | `0xca69e81` | 40 | `54580c3172264b9fe8b74afc4bc0108768b3105bb582b0e08399f7c463e74392` |
| calendar_app_lookup_name_other | `0xca69e54` | `0xca69e55` | 44 | `8892aa5f16994593264dbca6e56d55be9e6943f30183326baa60b35037685a39` |
| calendar_app_dispatch | `0xca69aa0` | `0xca69aa1` | 16 | `c5be8ce548f39ae57b9d71bcc9537814ff93ed3b69d054d4767f7b8d65c2c449` |

Windows include only bounded static evidence, not allowlist ranges. The corresponding
Canopus exact-target records are restricted/STATIC_RECOVERED/PENDING; no device
approval is promoted. Native probes model GUI, I/O and snapshot leaves as documented.
The user reports **USER_REPORTED_PASS** for calendar reload on this exact
firmware using the signed artifact identified in the linked report. No device
logs or fault-injection evidence were supplied. Broader GPU, storage/allocation
failure and restart-recovery acceptance remain unverified.

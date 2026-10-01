# Native dynamic calendar reload: xiaomi-band-11-4.100.139

Implementation and limits: [calendar reload contract](../../docs/CALENDAR_RELOAD.md).

Exact AP SHA-256: `31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74` (12304868 bytes).

## Exact native entries

| Symbol | Even entry | Thumb callable | Evidence-window bytes | AP window SHA-256 |
| --- | --- | --- | --- | --- |
| calendar_icon_generate | `0xc5487a4` | `0xc5487a5` | 416 | `c9c51f4de4cba1be5ab7ba856472160fb680ff6caa7f1e616b10123280f18794` |
| calendar_icon_publish | `0xc5486d0` | `0xc5486d1` | 212 | `cae8c2d7e5254dd99fe65586887840ea6ad12890a46c04efaacf68ff664252cb` |

Windows include only bounded static evidence, not allowlist ranges. The corresponding
Canopus exact-target records are restricted/STATIC_RECOVERED/PENDING; no device
approval is promoted. Native probes model GUI, I/O and snapshot leaves as documented.
The user reports **USER_REPORTED_PASS** for calendar reload on this exact
firmware using the signed artifact identified in the linked report. No device
logs or fault-injection evidence were supplied. Broader GPU, storage/allocation
failure and restart-recovery acceptance remain unverified.

# Native dynamic calendar reload: xiaomi-band-11-4.100.155

Implementation and limits: [calendar reload contract](../../docs/CALENDAR_RELOAD.md).

Exact AP SHA-256: `ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f` (12304852 bytes).

## Exact native entries

| Symbol | Even entry | Thumb callable | Evidence-window bytes | AP window SHA-256 |
| --- | --- | --- | --- | --- |
| calendar_icon_generate | `0xc5487a4` | `0xc5487a5` | 416 | `ebe80139cf83f1dde2484100136c50aecb1c9b3ebd0dd86d92a22d0c6b789c77` |
| calendar_icon_publish | `0xc5486d0` | `0xc5486d1` | 212 | `108a6fa0e164acf149975b5be5fa81b0186f7bdab088b50442ff14502b33a8bd` |

Windows include only bounded static evidence, not allowlist ranges. The corresponding
Canopus exact-target records are restricted/STATIC_RECOVERED/PENDING; no device
approval is promoted. Native probes model GUI, I/O and snapshot leaves as documented.
The user reports **USER_REPORTED_PASS** for calendar reload on this exact
firmware using the signed artifact identified in the linked report. No device
logs or fault-injection evidence were supplied. Broader GPU, storage/allocation
failure and restart-recovery acceptance remain unverified.

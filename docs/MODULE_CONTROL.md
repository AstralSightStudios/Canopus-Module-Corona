# Module control channel

This documents the native module control protocol. The accompanying Manager uses it: migrate to the control paths, query status once on
first homepage entry, then use reload v2's memory snapshot to update status and
active rule count after each reload without another status query. Older clients
using `reload.request` / `reload.result` must migrate their paths.

## Files and transport

The module uses `control.request` and `control.response` under the app-scoped
files root for `ng.lst.corona`:

- Band 11: `/data/quickapp/files/ng.lst.corona/`
- Band 10 Pro: `/data/files/ng.lst.corona/`
- Compatible quick-app clients: `internal://files/control.request` and
  `internal://files/control.response` (do not pass native absolute paths to the
  quick-app file API).

Public native macros are `RH_CONTROL_REQUEST_PATH` and
`RH_CONTROL_RESPONSE_PATH`. There are no legacy path or macro aliases.
`mappings.tsv` stays in the same root.

A controller must precreate `control.response` before expecting a reply. The
module opens it with NuttX write-only flag `2`, without create or truncate.
A nonempty placeholder such as `pending<TAB><requestId><LF>` avoids Vela's empty
`writeText` rejection (202). Reload processing can still proceed if response
preparation fails, but the client cannot infer success from a missing receipt.

The existing UI-owner watcher handles requests; no new timer is added. Request
input is bounded to **128 bytes**. Responses are exactly **256 bytes**, with
three text lines followed by zero bytes. This is a shared single response slot,
not a queue: clients should serialize requests and correlate every reply.
Failed/short writes are retried, and unchanged requests/results are deduplicated;
receipt retries do not rerun completed reload work.

In the examples below, `<TAB>` is one byte `0x09` and `<LF>` is one byte `0x0a`,
not literal escape text.

## Read-only status request

The request must be exactly:

```text
resource-hook-status-v1<TAB>ng.lst.corona<TAB><requestId><LF>
```

`requestId` is 1–64 ASCII characters from `[A-Za-z0-9._-]`. The app ID is
mandatory and exact; extra fields, CRLF and extra input are not this schema.
Use a fresh ID for each new query rather than relying on a deduplicated request
being processed again.

The first response line echoes the **exact request line**, then:

```text
resource-hook-status-v1<TAB>ng.lst.corona<TAB><requestId><LF>
RHST1<TAB>1<TAB><running|config_error><TAB><signed configError><TAB><activeRuleCount><TAB><0|1 refreshPending><LF>
<unsigned-decimal-FNV1a><LF>
```

Numeric fields use decimal text: `configError` is signed, `activeRuleCount` is
nonnegative, and `refreshPending` is exactly `0` or `1`.

The checksum is 32-bit FNV-1a over the first two lines, including both LF bytes:
start at `2166136261`, XOR each byte, multiply by `16777619` modulo `2^32`.
Render the final unsigned value in decimal followed by LF. Padding is not hashed.
Clients must verify the echoed ID/line, schema, checksum and complete record;
ignore stale, malformed or torn replies. The checksum is not authentication.

Status reads **memory only**: it does not open, parse or validate configuration,
resolve QuickApps, publish rules, retire caches or refresh owners. It remains
available while refresh work is pending. `activeRuleCount` is the active rule
count, not the count in an unaccepted file; `refreshPending` only indicates
pending refresh work, not proof that every resource or GPU operation succeeded.

- `running` has `configError` zero. Missing configuration (ENOENT), empty files
  and comment-only files are healthy startup pass-through configurations.
- `config_error` reports a signed raw configuration error while the hook may
  still run with zero rules or last-known-good rules. Startup errors retain
  `-2005` (non-ENOENT open), `-2006` (allocation), or `-2007`
  (read/parse/validation/materialization), without converting them into startup
  failure returns. Runtime configuration errors retain `-2101` (allocation),
  `-2102` (open), `-2103` (parse/read), or `-2104` / `-2105` (QuickApp
  materialization).
- Configuration errors are sticky until successful **explicit configuration
  publication**, including a valid zero-rule configuration. A status query or
  successful background QuickApp resolution must not clear them. Background
  QuickApp resolution errors are separate and can recover independently.

There is no `stopped` or `unresponsive` native response. A timeout or missing
valid correlated receipt only means no valid reply was received; it does not
establish that hooks are stopped, unloaded, or in a configuration error state.
A healthy reply is not a firmware/GPU health test.

## Reload v2 with memory status snapshot

The optional v2 request must be exactly:

```text
resource-hook-reload-v2<TAB>ng.lst.corona<TAB><requestId><LF>
```

The app ID is mandatory and exact. `requestId` has the same 1–64 character
`[A-Za-z0-9._-]` constraint as status v1; extra fields, CRLF and trailing input
are rejected. It is the same transactional reload operation as v1, not a
separate timer or configuration path.

The response echoes the exact request, followed by:

```text
resource-hook-reload-v2<TAB>ng.lst.corona<TAB><requestId><LF>
RHRS2<TAB>1<TAB><signed reloadResult><TAB><0|1 refreshPending><TAB><unsigned changed><TAB><running|config_error><TAB><signed configError><TAB><activeRuleCount><LF>
<unsigned-decimal-FNV1a><LF>
```

Framing is the same three-line, exactly 256-byte, zero-padded FNV-1a record
specified above. `RHRS2`'s schema version is always `1`, including experimental
font builds. `reloadResult`, `refreshPending` and `changed` retain v1 reload
semantics; `changed` counts font families changed by this request, not rules.
Nonexperimental builds retain result `0` / changed `0` for accepted reloads,
even when refresh is pending. Experimental builds can return result `1` while
pending and a negative font result after completion.

The added status fields come only from memory, captured together with pending
and active count under the IRQ lock. Formatting and all file I/O happen after
unlocking. `configError` is the latched explicit configuration error if present,
otherwise the independently recoverable QuickApp error; `activeRuleCount` is
`S.count`, the active materialized map, not the proposed file or declaration
count. Configuration rejection preserves the last-known-good count. Successful
publication reports the new count immediately, even while refresh is pending.
A font/refresh result must **never** be interpreted as a configuration failure:
a negative font result may accompany `running`, config error `0`, and an active
nonzero rule count.

A pending receipt is updated to a completed snapshot as work finishes, provided
that request still owns the response slot. Use a fresh ID for a new reload;
resending a completed request can reclaim its receipt without reloading config.
Exact request bytes, including the protocol version, identify a revision: v1
and v2 requests with the same ID are distinct requests, not cross-version aliases.
The same bounded request/response storage supports both; even a 128-byte legacy
request plus maximum-width numeric fields and checksum fits within 256 bytes.

## Reload v1 compatibility on the new paths

The legacy request remains unchanged:

```text
resource-hook-reload-v1<TAB>[ng.lst.corona<TAB>]<revision><LF>
```

The existing RHRS1 v5/v6 receipt remains unchanged, including its request echo,
checksum and 256-byte zero padding:

```text
<reload-request-line><LF>
RHRS1<TAB><5-or-6><TAB><signed-result><TAB><refresh-pending><TAB><families-changed-this-request><LF>
<unsigned-decimal-FNV1a-of-first-two-lines-including-LFs><LF>
```

For v1, only the filenames changed; the original revision validation and
package-optional form are preserved (no new 64-character revision limit).
The descriptor query's RHQ1 v5 (40 bytes) / v6
(48 bytes, experimental fonts) ABI is also unchanged. See [INSTALL.md](INSTALL.md)
and [FONT_RELOAD_EXPERIMENT.md](FONT_RELOAD_EXPERIMENT.md) for reload semantics.

Only the latest observed valid request owns the shared response slot, across
status, v1 reload and v2 reload. Delayed completion or refresh retries must not
overwrite a newer owner's response with an older reload receipt. Changing the
owner does not cancel pending reload/refresh work; a newer reload waits until
the current transaction releases its mapping bank. Invalid/absent input revokes
ownership without overwriting the file. Nested watcher/response callbacks are
guarded against reentrant observation and I/O.

## Manager integration contract

The accompanying Manager upgrade migrates both paths, precreates the response
file and validates/correlates RHST1 and RHRS2 records. First homepage entry
issues **one** status query; do not add periodic queries. Existing UI reload
actions use v2, updating the displayed active count and configuration state from
each correlated snapshot, without a follow-up status request. Keep reload/font
results distinct from the configuration state. Timeouts, placeholders, malformed
or stale receipts are transport/receipt failures, not evidence of an unsupported
native state. The v1 wire protocol remains available for compatible older
controllers on the new paths.

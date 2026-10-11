# Requests

`ta_host_request` (`include/tinyactors/host.h`) is the rarely called part of
the host's API ([ADR 0009](../../docs/adr/0009-one-host-two-queues.md),
decision 2). One CBOR map goes in and one CBOR map comes out. This page lists
every operation. If an operation is not listed here, it does not exist.
Examples are in CBOR diagnostic notation.

## The envelope

- **A request** is a map with text keys. `"op"` names the operation. The
  other keys are the operation's fields. A field that the operation does not
  take is refused.
- **A reply** is a map: either the result, or
  `{"error": {"code", "path", "message"}}`.
  - `code` is one of the codes below.
  - `path` names the field: `"sample"`, `"rules[1].from"`, or `""` for the
    whole request.
  - `message` is for people. Do not match on it.
- **Keys in replies** come in the order shown on this page, which is
  always the same (ADR 0009, decision 3).
- **The status of `ta_host_request`:**
  - `TA_OK`: `reply` holds a result or an error reply.
  - `TA_ERROR_NO_MEMORY`: the reply did not fit. `reply->length` is what it
    needs and nothing was applied. Grow the buffer and send the request
    again.
  - `TA_ERROR_STATE`: the call was made during a run (from a datamodel).
    Nothing was written.
- **Limits:** a request is decoded into at most `TA_HOST_REQUEST_TOKENS`
  (256) items, on the stack. One more is the error `limit`.

| Code | Meaning |
|---|---|
| `malformed` | not one well-formed CBOR item (or, for `spans.decode`, not OTLP) |
| `type` | the request is not a map, or a field has the wrong type |
| `missing` | a required field is absent |
| `unknown` | an operation, field or name the host does not know |
| `range` | a number outside its range |
| `limit` | more than the request or the host holds |
| `stale` | the session or watch does not exist (any more) |
| `state` | the host cannot do it now (tracing is off) |
| `invalid` | the host refused a rule as malformed |
| `memory` | the host's allocator refused |

## Trace rules

`trace.configure` and `trace.watch` take a list of rules
(`ta_trace_rule`, docs/adr/0007). A request holds at most 32 of them. A rule
is a map; each absent field matches anything.

| Field | Type | Meaning |
|---|---|---|
| `at` | `"start"` or `"take"` | where the rule is asked: when a trace starts, or when an actor takes an external event (absent: both) |
| `chart` | text | a chart's name |
| `actor` | text | an actor's name; a trailing `*` makes it a prefix |
| `event` | text | an event descriptor, SCXML's prefix rule (`"error"`, `"webhook.*"`) |
| `session` | uint | one session |
| `from` | list of text | any of `"host"`, `"actor"`, `"timer"`, `"spawn"`, `"sampled"`, `"unsampled"` |
| `sample` | `"keep"`, `"sample"`, `"drop"` or a number 0–1 | what a match decides (default `"keep"`); a number samples that share of trace IDs |
| `detail` | `"summary"`, `"decisions"`, `"full"`, `"off"` | the detail a sampled trace gets (absent: its own) |
| `values` | bool | the detail includes values (event data, assigned values) |
| `perSecond`, `burst` | uint | match at most this often (default 0: no limit) |

## Operations

### `trace.configure`

Sets what every segment traces, and what new segments start with. It replaces
the previous configuration; watches stay.

| Field | Type | Default |
|---|---|---|
| `detail` | `"summary"`, `"decisions"`, `"full"`, `"off"` | `"summary"` |
| `values` | bool | `false` |
| `sample` | number 0–1, the share of new traces sampled | `1` |
| `records` | bool: `false` writes no records (traceparents still flow) | `true` |
| `rules` | list of rules | `[]` |

Reply: `{}`. Errors: `type`, `unknown`, `range`, `limit` (more than 32 rules;
or more different details or rules than the host's systems hold), `invalid`,
`state` (the host's systems do not trace).

```
{"op": "trace.configure", "detail": "summary", "sample": 0.01,
 "rules": [{"at": "start", "from": ["host"], "perSecond": 5}]}
→ {}
```

### `trace.watch`

Adds rules in front of the configured ones, newest first, until they end.

| Field | Type | Default |
|---|---|---|
| `rules` | list of rules, at least one | required |
| `seconds` | number ≥ 0 on the host's clock, or `null` for no end | `900` (15 minutes) |

Reply: `{"id": uint}`. When the watch ends is a deadline of `ta_host_run`.
Errors: as `trace.configure`; `missing` without rules.

```
{"op": "trace.watch", "rules": [{"chart": "checkout", "sample": "sample",
 "detail": "full"}], "seconds": 60}
→ {"id": 1}
```

### `trace.unwatch`

Ends a watch early. Field: `id` (uint, required). Reply: `{}`. Errors:
`missing`, `stale` (it ended, or never was).

```
{"op": "trace.unwatch", "id": 1}
→ {}
```

### `stats`

The host's counters (`ta_host_stats`) and each chart's
(`ta_chart_stats`), summed by chart name over every segment, retired
versions included. No fields.

Reply: `{"host": {...}, "charts": [...]}`:
- `host`: `segments`, `actors`, `hibernated`, `runnable`, `jobs`,
  `regionBytes`, `delivered`, `routed`, `migrated`, `retired`, `fuelClock`.
- each chart: `name`, `actors`, `hibernated`, `spawned`, `ended`, `turns`,
  `macrosteps`, `fuel`, `errorsExecution`, `errorsCommunication`,
  `errorsPlatform`, `faults`.

All the numbers are unsigned integers.

```
{"op": "stats"}
→ {"host": {"segments": 1, "actors": 1, "hibernated": 0, "runnable": 0, "jobs": 0,
   "regionBytes": 131072, "delivered": 0, "routed": 0, "migrated": 0, "retired": 0,
   "fuelClock": 3},
   "charts": [{"name": "pinged", "actors": 1, "hibernated": 0, "spawned": 1,
   "ended": 0, "turns": 1, "macrosteps": 1, "fuel": 3, "errorsExecution": 0,
   "errorsCommunication": 0, "errorsPlatform": 0, "faults": 0}]}
```

### `actors`

Lists the host's sessions, one page at a time.

| Field | Type | Default |
|---|---|---|
| `cursor` | uint, opaque, from the previous reply | `0` (the start) |
| `limit` | uint 1–512 | `256` |

Reply: `{"sessions": [uint, ...], "cursor": uint or null}`. `null` means
every session was listed. Errors: `type`, `range`.

```
{"op": "actors", "limit": 2}
→ {"sessions": [1, 2], "cursor": 2}          (opaque: send it back as it is)
```

### `actor`

One session. Field: `session` (uint, required).

Reply: `{"session", "segment", "chart", "parent", "mailbox", "invocations"}`:
- `chart`: the chart's name;
- `parent`: the session that invoked it, or `null` for a root actor;
- `mailbox`: events waiting for it;
- `invocations`: services it runs.

Errors: `missing`, `type`, `stale`.

```
{"op": "actor", "session": 1}
→ {"session": 1, "segment": 0, "chart": "pinged", "parent": null, "mailbox": 0,
   "invocations": 0}
```

### `state`

A session's configuration as a nested state value, in the shape of xstate's
`value`:
- an atomic state is its id: `"s"`;
- a compound state is `{"c": <its active child>}`;
- a parallel state is `{"p": {"r1": ..., "r2": {}}}`, one entry per region;
  an atomic region is `{}`;
- no active state is `{}`.

Field: `session` (uint, required). Reply: `{"value": <state value>}`.
Errors: `missing`, `type`, `stale`, `limit` (more than 1024 active states).

```
{"op": "state", "session": 1}
→ {"value": {"top": {"p": {"r1": "x", "r2": {}, "r3": {"k": "k1"}}}}}
```

### `spans.decode`

The spans of an ExportTraceServiceRequest, such as the bytes
`ta_host_traces` writes. It reads only the bytes given, not the host. Field:
`traces` (bytes, required).

Reply: `{"spans": [...]}`. Each span is
`{"trace", "span", "parent", "name", "kind", "start", "end", "status",
"statusMessage", "attributes", "events"}`:
- `trace` and `span` are lowercase hex text; `parent` is hex, or `null` for a
  root span;
- `kind` and `status` are OTLP's numbers;
- `start` and `end` are nanoseconds since the Unix epoch;
- `attributes` is a map of key to value. A list value becomes an array of
  values, a key-value list becomes a map, and anything nested deeper is
  `null`;
- each event is `{"name", "time", "attributes"}`.

The resource, the scope and links are not in the reply.

Errors: `missing`, `type`, `malformed` (not OTLP).

```
{"op": "spans.decode", "traces": h'0a8a...'}
→ {"spans": [{"trace": "4bf9...", "span": "00f0...", "parent": null,
   "name": "pinged", "kind": 1, "start": 1700000000000000000, "end": ...,
   "status": 0, "statusMessage": "", "attributes": {...}, "events": []}]}
```

## Not requests

These stay C calls:
- **The hot path:** spawn, send, send_many, terminate, deliver,
  advance_time, resume, run, and the effects and their ack.
- **Bytes that the host writes or takes:** `ta_host_traces`, `ta_host_logs`,
  `ta_host_metrics`, hibernate, wake, export_actor, adopt, migrate, snapshot,
  restore and `snapshot_pack`.
- **Calls that need no host:** `ta_host_snapshot_unpack`, `_image` and
  `_segments` read a snapshot before a host exists.
- **Lookups that spawning and routing use:** `ta_host_chart`,
  `ta_host_locate` and `ta_host_actor_by_name`.
- **The router's calls:** peers, routes and exports, which a transport makes.

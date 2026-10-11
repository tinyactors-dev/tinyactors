# Requests

`ta_host_request` (`include/tinyactors/host.h`) is the rarely called part of
the host's API ([ADR 0009](../../docs/adr/0009-one-host-two-queues.md),
decision 2). One CBOR map goes in and one CBOR map comes out, in the shape of
JSON-RPC 2.0 without its `"jsonrpc"` member, so that a JSON-RPC transport is
a thin mapping. This page lists every method. If a method is not listed
here, it does not exist. Examples are in CBOR diagnostic notation.

## The envelope

- **A request** is a map with these members and no others:
  - `method` (text, required): the method's name;
  - `params` (map, optional): the method's params, text keys; absent means
    none. A param that the method does not take is refused;
  - `id` (uint or text, optional): echoed in the reply.
- **A reply** is a map, its keys in this order:
  - `id`, when the request had a valid one;
  - then `result` (the method's result), or `error`:
    `{"code": int, "message": text, "data": {"reason": text, "path": text}}`.
  - `code` is from the table below; `reason` names the condition; `path`
    names the member or param: `"method"`, `"params.sample"`,
    `"params.rules[1].from"`, or `""` for the request as a whole.
  - `message` is for people. Do not match on it.
- **One difference from JSON-RPC:** a request without an `id` still gets a
  reply. There are no notifications; the caller asked, and the host answers
  in the caller's buffer either way.
- **Keys in replies** come in the order shown on this page, which is
  always the same (ADR 0009, decision 3). Byte strings stay bytes.
- **The status of `ta_host_request`:**
  - `TA_OK`: `reply` holds a result or an error reply.
  - `TA_ERROR_NO_MEMORY`: the reply did not fit. `reply->length` is what it
    needs and nothing was applied. Grow the buffer and send the request
    again.
  - `TA_ERROR_STATE`: the call was made during a run (from a datamodel).
    Nothing was written.
- **Limits:** a request is decoded into at most `TA_HOST_REQUEST_TOKENS`
  (256) items, on the stack. One more is the error `limit`.

| Code | Reason | Meaning |
|---|---|---|
| -32700 | `malformed` | not one well-formed CBOR item |
| -32600 | `type`, `missing`, `unknown` | not a request: not a map, no text `method`, `params` not a map, `id` neither uint nor text, or a member other than these three |
| -32601 | `unknown` | no such method |
| -32602 | `type`, `missing`, `unknown`, `range` | a param has the wrong type, is missing, is not one the method takes, or is out of range |
| -32001 | `stale` | the session or watch does not exist (any more) |
| -32002 | `state` | the host cannot do it now (tracing is off) |
| -32003 | `limit` | more than the request or the host holds |
| -32004 | `memory` | the host's allocator refused |
| -32005 | `invalid` | the host refused a rule as malformed |
| -32006 | `malformed` | bytes in params are not what the method reads (not OTLP) |

## Trace rules

`trace.configure` and `trace.watch` take a list of rules
(`ta_trace_rule`, docs/adr/0007), in `params.rules`. A request holds at most 32 of them. A rule
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

## Methods

### `trace.configure`

Sets what every segment traces, and what new segments start with. It replaces
the previous configuration; watches stay.

| Param | Type | Default |
|---|---|---|
| `detail` | `"summary"`, `"decisions"`, `"full"`, `"off"` | `"summary"` |
| `values` | bool | `false` |
| `sample` | number 0–1, the share of new traces sampled | `1` |
| `records` | bool: `false` writes no records (traceparents still flow) | `true` |
| `rules` | list of rules | `[]` |

Result: `{}`. Errors (by reason): `type`, `unknown`, `range`, `limit` (more than 32 rules;
or more different details or rules than the host's systems hold), `invalid`,
`state` (the host's systems do not trace).

```
{"method": "trace.configure", "params": {"detail": "summary", "sample": 0.01,
 "rules": [{"at": "start", "from": ["host"], "perSecond": 5}]}, "id": 1}
→ {"id": 1, "result": {}}
```

### `trace.watch`

Adds rules in front of the configured ones, newest first, until they end.

| Param | Type | Default |
|---|---|---|
| `rules` | list of rules, at least one | required |
| `seconds` | number ≥ 0 on the host's clock, or `null` for no end | `900` (15 minutes) |

Result: `{"id": uint}`. When the watch ends is a deadline of `ta_host_run`.
Errors: as `trace.configure`; `missing` without rules.

```
{"method": "trace.watch", "params": {"rules": [{"chart": "checkout",
 "sample": "sample", "detail": "full"}], "seconds": 60}}
→ {"result": {"id": 1}}
```

### `trace.unwatch`

Ends a watch early. Param: `id` (uint, required). Result: `{}`. Errors:
`missing`, `stale` (it ended, or never was).

```
{"method": "trace.unwatch", "params": {"id": 1}}
→ {"result": {}}
{"method": "trace.unwatch", "params": {"id": 1}}
→ {"error": {"code": -32001, "message": "no such watch: it ended or never was",
   "data": {"reason": "stale", "path": "params.id"}}}
```

### `stats`

The host's counters (`ta_host_stats`) and each chart's
(`ta_chart_stats`), summed by chart name over every segment, retired
versions included. No params.

Result: `{"host": {...}, "charts": [...]}`:
- `host`: `segments`, `actors`, `hibernated`, `runnable`, `jobs`,
  `regionBytes`, `delivered`, `routed`, `migrated`, `retired`, `fuelClock`.
- each chart: `name`, `actors`, `hibernated`, `spawned`, `ended`, `turns`,
  `macrosteps`, `fuel`, `errorsExecution`, `errorsCommunication`,
  `errorsPlatform`, `faults`.

All the numbers are unsigned integers.

```
{"method": "stats"}
→ {"result": {"host": {"segments": 1, "actors": 1, "hibernated": 0, "runnable": 0, "jobs": 0,
   "regionBytes": 131072, "delivered": 0, "routed": 0, "migrated": 0, "retired": 0,
   "fuelClock": 3},
   "charts": [{"name": "pinged", "actors": 1, "hibernated": 0, "spawned": 1,
   "ended": 0, "turns": 1, "macrosteps": 1, "fuel": 3, "errorsExecution": 0,
   "errorsCommunication": 0, "errorsPlatform": 0, "faults": 0}]}}
```

### `actors`

Lists the host's sessions, one page at a time.

| Param | Type | Default |
|---|---|---|
| `cursor` | uint, opaque, from the previous reply | `0` (the start) |
| `limit` | uint 1–512 | `256` |

Result: `{"sessions": [uint, ...], "cursor": uint or null}`. `null` means
every session was listed. Errors: `type`, `range`.

```
{"method": "actors", "params": {"limit": 2}}
→ {"result": {"sessions": [1, 2], "cursor": 2}}   (opaque: send it back as it is)
```

### `actor`

One session. Param: `session` (uint, required).

Result: `{"session", "segment", "chart", "parent", "mailbox", "invocations"}`:
- `chart`: the chart's name;
- `parent`: the session that invoked it, or `null` for a root actor;
- `mailbox`: events waiting for it;
- `invocations`: services it runs.

Errors: `missing`, `type`, `stale`.

```
{"method": "actor", "params": {"session": 1}}
→ {"result": {"session": 1, "segment": 0, "chart": "pinged", "parent": null,
   "mailbox": 0, "invocations": 0}}
```

### `actor.context`

A session's variables: each `<data>` of its chart, in the order the chart
declares them, as neutral values (the datamodel's `to_wire`;
docs/protocol.md, "Values"). Param: `session` (uint, required).

Result: `{"variables": {<data id>: <value>, ...}}`. A variable the datamodel
cannot read or write as a neutral value is `undefined` or `null`.

Errors: `missing`, `type`, `stale`; `state` (-32002) when the actor is
hibernated, or its datamodel does not show variables (no `variable` entry in
its vtable, as with a binding's own datamodel, which keeps its scope).

```
{"method": "actor.context", "params": {"session": 1}}
→ {"result": {"variables": {"n": 2, "who": "bob", "nothing": null}}}
```

### `actor.subscribe`, `actor.unsubscribe`

`actor.subscribe` asks for a `TA_EFFECT_CHANGED` effect each run in which
the session finished a macrostep: at most one per session per run, after
which the binding reads what it needs (`state`, `actor.context`). Other
sessions cost nothing. Subscribing twice is one subscription; a session that
ends leaves the list. `actor.unsubscribe` stops it. Param: `session` (uint,
required). Result: `{}`.

Errors: `missing`, `type`, `stale` (subscribe: no such session;
unsubscribe: not subscribed), `limit` (-32003: the host holds
`maxSubscriptions`, 64 by default).

```
{"method": "actor.subscribe", "params": {"session": 1}}
→ {"result": {}}
```

### `state`

A session's configuration as a nested state value, in the shape of xstate's
`value`:
- an atomic state is its id: `"s"`;
- a compound state is `{"c": <its active child>}`;
- a parallel state is `{"p": {"r1": ..., "r2": {}}}`, one entry per region;
  an atomic region is `{}`;
- no active state is `{}`.

Param: `session` (uint, required). Result: `{"value": <state value>}`.
Errors: `missing`, `type`, `stale`, `limit` (more than 1024 active states).

```
{"method": "state", "params": {"session": 1}}
→ {"result": {"value": {"top": {"p": {"r1": "x", "r2": {}, "r3": {"k": "k1"}}}}}}
```

### `trace.spans.decode`

The spans of an ExportTraceServiceRequest, such as the bytes
`ta_host_traces` writes. It reads only the bytes given, not the host. Param:
`traces` (bytes, required).

Result: `{"spans": [...]}`. Each span is
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

Errors: `missing`, `type`, `malformed` (-32006: not OTLP).

```
{"method": "trace.spans.decode", "params": {"traces": h'0a8a...'}}
→ {"result": {"spans": [{"trace": "4bf9...", "span": "00f0...", "parent": null,
   "name": "pinged", "kind": 1, "start": 1700000000000000000, "end": ...,
   "status": 0, "statusMessage": "", "attributes": {...}, "events": []}]}}
```

## Not methods

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

## Adding a method

### What belongs here

A method is a decision core can make without the language, called rarely:
configuration, inspection, counters, and decoding bytes that core produces.
Its defaults, its validation and its error text live in core, so every
binding gets the same answers.

### What does not

- the hot path: inputs (spawn, send, deliver, ...) and effects;
- bytes the host writes or takes: traces, logs, metrics, hibernation,
  export and snapshot blobs. These are C calls with a `ta_buffer`;
- calls that need no host;
- I/O, transport, and policy around I/O (retries, caps, posting);
- anything a binding can compute from other replies;
- values specific to a datamodel, other than neutral CBOR values.

### Steps

1. **Name it `area.verb`**, like `trace.watch` (`stats`, `actor`: one word
   when the area is the whole answer).
2. **Params:** name each (camelCase), its type, its default and its limits.
   Refuse params the method does not take.
3. **The result:** a map with its keys in a fixed order.
4. **Errors:** use a reason from the table above. A new reason needs its own
   code in the -32000 to -32099 range, added to the table.
5. **Atomic:** write the result first and apply the change only when it
   fits, so `TA_ERROR_NO_MEMORY` means nothing changed.
6. **Refused during a run:** `ta_host_request` does this for every method.
7. **Tests** in `tests/unit/test_host_request.c`: the result, each param's
   errors, and `TA_ERROR_NO_MEMORY` for a method that changes the host.
8. **The entry on this page**, with its params, result, errors and one
   example. Once request and reply vectors exist (ADR 0009 step 6), add one.

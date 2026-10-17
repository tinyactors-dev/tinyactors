# Requests: the control plane

`ta_host_post` (`include/tinyactors/host.h`) is the rarely called part of
the host's API ([ADR 0009](../../docs/adr/0009-one-host-two-queues.md),
decision 2; [ADR 0011](../../docs/adr/0011-the-control-plane-is-posted-messages.md)).
A message is one CBOR map in the shape of JSON-RPC 2.0 without its
`"jsonrpc"` member; its reply, and the host's notifications, are effects
the host appends, read like every other effect. Requests, replies and
notifications are one ordered stream, so that a JSON-RPC transport is a
pipe: frames to `ta_host_post`, the bytes of `TA_HOST_EFFECT_REPLY` and
`TA_HOST_EFFECT_NOTIFICATION` effects to the socket. This page lists every
method and notification. If one is not listed here, it does not exist.
Examples are in CBOR diagnostic notation.

**Every byte is CBOR; JSON-RPC names only the shape.** Core never reads
or writes JSON: a message, a reply and a notification are CBOR items whose
maps have JSON-RPC 2.0's members, so that no shape is invented and a
transport that speaks JSON on its wire converts at its edge (CBOR's value
world covers JSON's). A binding and its consumers deal with two systems,
the effect records read through accessors and CBOR, never a third.

## The envelope

- **A request** is a map with these members and no others:
  - `method` (text, required): the method's name;
  - `params` (map, optional): the method's params, text keys; absent means
    none. A param that the method does not take is refused;
  - `id` (an integer, text or null, optional): echoed in the reply.
- **A message without `id`** is a JSON-RPC notification. The host applies
  it and appends no reply: `ta_host_post` returns `TA_OK`, and its errors
  are not reported, including a method that does not exist and the host's
  allocator refusing. Send an `id` to learn the outcome. A message that is
  not a request (the errors with code -32700 and -32600 below, and too
  many items) is answered even without an `id`.
- **The reply** is the last effect the post appends,
  `TA_HOST_EFFECT_REPLY`, after the effects the method appends itself
  (`trace.end`'s `TRACES` and `LOGS`). Its `WIRE` field
  (`ta_host_effect_bytes`) is a map, its keys in this order:
  - `id`: the request's, the same item; `null` when it could not be read
    (the message is not one CBOR item, holds too many, is not a map, or its
    `id` is not an integer, text or null), as JSON-RPC 2.0 has it;
  - then `result` (the method's result), or `error`:
    `{"code": int, "message": text, "data": {"reason": text, "path": text}}`.
  - `code` is from the table below; `reason` names the condition; `path`
    names the member or param: `"method"`, `"params.sample"`,
    `"params.rules[1].from"`, or `""` for the request as a whole.
  - `message` is for people. Do not match on it.
- **Publication** ([ADR 0012](../../docs/adr/0012-lifetimes-follow-the-loop.md)):
  a reply is pending until a take (`ta_host_effects`) with no batch open
  hands it out, with or without a run; a post made while a batch is open
  is answered in the take after the ack; a take never widens the open
  batch. A batch holds, in order: the effects appended between
  runs in call order (earlier replies, a deliver's `PEER_ERROR`,
  `trace.end`'s `TRACES`), then a run's effects in drain order, then the
  run's notifications at its end. A reply and its bytes live until
  `ta_host_effects_ack` or `ta_host_free`. There is no synchronous
  request: a caller matches a reply to its message by `id`, in a later
  drain (embedding.md, section 5; a binding's typed methods, bindings.md
  section 10).
- **Batches** (a CBOR array of requests) are not supported: one is
  refused with -32600, as anything but a map is. A transport splits a
  JSON-RPC batch into messages and assembles the array from the replies.
- **Keys in replies and notifications** come in the order shown on this
  page, which is always the same (ADR 0009, decision 3). Byte strings stay
  bytes.
- **The status of `ta_host_post`:**
  - `TA_OK`: the message was read and, with an id, answered; a JSON-RPC
    error travels inside the reply.
  - `TA_ERROR_NO_MEMORY`: the host's allocator refused room for the reply
    (or the published batch holds as many replaced blocks as it can), and
    nothing was applied and no reply appended. Post the message again when
    it has room.
  - `TA_ERROR_STATE`: the call was made during a run, or while the host was
    inside a datamodel (from a datamodel's code). Nothing was appended.
- **Limits:** a message is decoded into at most `TA_HOST_REQUEST_TOKENS`
  (256) items, on the stack. One more is the error `limit`.

| Code | Reason | Meaning |
|---|---|---|
| -32700 | `malformed` | not one well-formed CBOR item |
| -32600 | `type`, `missing`, `unknown` | not a request: not a map (a batch too), no text `method`, `params` not a map, `id` not an integer, text or null, or a member other than these three |
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
{"id": 1, "method": "trace.configure", "params": {"detail": "summary", "sample": 0.01,
 "rules": [{"at": "start", "from": ["host"], "perSecond": 5}]}}
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
{"id": 1, "method": "trace.watch", "params": {"rules": [{"chart": "checkout",
 "sample": "sample", "detail": "full"}], "seconds": 60}}
→ {"id": 1, "result": {"id": 1}}
```

### `trace.unwatch`

Ends a watch early. Param: `id` (uint, required). Result: `{}`. Errors:
`missing`, `stale` (it ended, or never was).

```
{"id": 1, "method": "trace.unwatch", "params": {"id": 1}}
→ {"id": 1, "result": {}}
{"id": 1, "method": "trace.unwatch", "params": {"id": 1}}
→ {"id": 1, "error": {"code": -32001, "message": "no such watch: it ended or never was",
   "data": {"reason": "stale", "path": "params.id"}}}
```

### `trace.end`

The host is stopping: every open span ends now, at the wall time of the last
`ta_host_advance_time`, marked `scxml.incomplete`, and is staged for
`ta_host_traces`. Its `TRACES` effect (and `LOGS`, when logs are staged)
comes before the reply, in the same batch, without a run. When the staging cannot hold them all, the rest end as
takes make room: take until a take writes nothing. No params. Result:
`{}`. Errors: `state` (the host has no telemetry).

```
{"id": 1, "method": "trace.end"}
→ {"id": 1, "result": {}}
```

### `host.stats`

The host's counters (`ta_host_stats`) and each chart's
(`ta_chart_stats`), summed by chart name over every segment, retired
versions included. No params.

Result: `{"host": {...}, "pools": {...}, "charts": [...]}`:
- `host`: `segments`, `actors`, `hibernated`, `runnable`, `migrations`,
  `regionBytes`, `delivered`, `routed`, `migrated`, `retired`, `fuelClock`.
  `regionBytes` sums the live segments' regions; the host's copies of
  their images (`ta_host_add_image`) are not in it.
- `pools`: `actors`, `events`, `timers`, `workspaces`, `requests` and
  `slabBytes` (the slab of instances and strings, in bytes), each
  `{"capacity", "used", "highWater"}`: what the pool holds now, the most
  it held at once since its segment was made, and its size, summed over
  the live segments. A pool that fills grows its segment
  ([host-config.md](host-config.md)); `highWater` against `capacity` is
  what a config sized up front needs.
- each chart: `name`, `actors`, `hibernated`, `spawned`, `ended`, `turns`,
  `macrosteps`, `fuel`, `errorsExecution`, `errorsCommunication`,
  `errorsPlatform`, `faults`.

All the numbers are unsigned integers.

```
{"id": 1, "method": "host.stats"}
→ {"id": 1, "result": {"host": {"segments": 1, "actors": 1, "hibernated": 0, "runnable": 0, "migrations": 0,
   "regionBytes": 131072, "delivered": 0, "routed": 0, "migrated": 0, "retired": 0,
   "fuelClock": 3},
   "pools": {"actors": {"capacity": 64, "used": 1, "highWater": 1},
   "events": {"capacity": 256, "used": 0, "highWater": 1}, "timers": {"capacity": 64, "used": 0, "highWater": 0},
   "workspaces": {"capacity": 4, "used": 0, "highWater": 1}, "requests": {"capacity": 16, "used": 0, "highWater": 0},
   "slabBytes": {"capacity": 94208, "used": 4096, "highWater": 4096}},
   "charts": [{"name": "pinged", "actors": 1, "hibernated": 0, "spawned": 1,
   "ended": 0, "turns": 1, "macrosteps": 1, "fuel": 3, "errorsExecution": 0,
   "errorsCommunication": 0, "errorsPlatform": 0, "faults": 0}]}}
```

### `host.config`

The host's effective config: what `ta_host_create` was given, with every
default filled in ([host-config.md](host-config.md)), in the shape it takes.
A binding reads `system.ticksPerSecond` here once, after creating the host,
to convert its clock to the host's ticks. No params: absent or an empty
map; any param is refused (`unknown`, -32602).

Result: a map, its keys in this order:
- `systemID`;
- `system`: `maxActors`, `maxEvents`, `maxTimers`, `workspaces`,
  `maxRequests`, `outbox`, `quantum`, `instanceBytes`, `stringBytes`,
  `ticksPerSecond`, `traceBytes`, `traceContexts`, `traceRules`,
  `traceSeed` (unsigned integers), then `tracing` (bool).
  `instanceBytes` 0 means derived from each segment's image;
- `ioProcessors`: a list of `{"type", "location"}`, as given;
- `migrateQuantum`;
- `telemetry`, only when the host has an encoder: `stagingBytes`,
  `logBytes`, `openSpans`, `chunks`, `scopes`, `valueLimit`, `renderBytes`,
  `resourceBytes`, `metricBytes`, `spanMetricCharts`, `flushInterval`
  (unsigned integers), `linkParents`, `values`, `logs` (bools), and
  `resource` (a map of text to text).

Every member is present, with the value the host runs with: a host made
from the result answers the same result. The capacities are those of each
new segment and of the encoder when it was made; a segment or the encoder
that grew since has more.

```
{"id": 1, "method": "host.config"}
→ {"id": 1, "result": {"systemID": "h", "system": {"maxActors": 64, "maxEvents": 256,
   "maxTimers": 64, "workspaces": 4, "maxRequests": 16, "outbox": 64, "quantum": 1000,
   "instanceBytes": 0, "stringBytes": 17408, "ticksPerSecond": 1000000000,
   "traceBytes": 131072, "traceContexts": 256, "traceRules": 16, "traceSeed": 0,
   "tracing": true},
   "ioProcessors": [], "migrateQuantum": 256}}
```

### `actor.list`

Lists the host's sessions, one page at a time.

| Param | Type | Default |
|---|---|---|
| `cursor` | uint, opaque, from the previous reply | `0` (the start) |
| `limit` | uint 1–512 | `256` |

Result: `{"sessions": [uint, ...], "cursor": uint or null}`. `null` means
every session was listed. Errors: `type`, `range`.

```
{"id": 1, "method": "actor.list", "params": {"limit": 2}}
→ {"id": 1, "result": {"sessions": [1, 2], "cursor": 2}}   (opaque: send it back as it is)
```

### `actor.get`

One session. Param: `session` (uint, required).

Result: `{"session", "segment", "chart", "parent", "mailbox", "invocations"}`:
- `chart`: the chart's name;
- `parent`: the session that invoked it, or `null` for a root actor;
- `mailbox`: events waiting for it;
- `invocations`: services it runs.

Errors: `missing`, `type`, `stale`.

```
{"id": 1, "method": "actor.get", "params": {"session": 1}}
→ {"id": 1, "result": {"session": 1, "segment": 0, "chart": "pinged", "parent": null,
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
{"id": 1, "method": "actor.context", "params": {"session": 1}}
→ {"id": 1, "result": {"variables": {"n": 2, "who": "bob", "nothing": null}}}
```

### `actor.send`

An event for an actor, from a client that has no binding: a debugger, an
admin tool, a test driver. Params: the target, `session` (uint) or
`address` (text: a local address, `tinyactors://<this system>/#_scxml_<id>`
or `.../@<name>`), exactly one; `event` (text, required); `data` (a neutral
value, read by the target's datamodel, `from_wire`); `origin` and `sendid`
(text): `_event.origin` and `_event.sendid`, an origin getting the SCXML
event processor as its origintype. The event goes to the actor's mailbox
as `ta_host_send` puts it there; a hibernated actor's `WAKE` follows.

Result: `{}`, once the event is in the mailbox.

Errors: `missing`, `type`, `unknown`; `range` for both targets or for
another system's address; `stale` for a session or address that names no
actor; `malformed` (-32006) for data the datamodel cannot read; `state`
(-32002) when the datamodel reads no values (no `from_wire`) or the actor
ended.

```
{"id": 1, "method": "actor.send", "params": {"address": "tinyactors://h/@counter", "event": "set", "data": {"n": 7}}}
→ {"id": 1, "result": {}}
```

### `actor.state`

A session's configuration as a nested state value, in the shape of xstate's
`value`:
- an atomic state is its id: `"s"`;
- a compound state is `{"c": <its active child>}`;
- a parallel state is `{"p": {"r1": ..., "r2": {}}}`, one entry per region;
  an atomic region is `{}`;
- no active state is `{}`.

Param: `session` (uint, required). Result: `{"value": <state value>}`.
Errors: `missing`, `type`, `stale`, `limit` (-32003: the configuration
has more than 1024 active states, the room the host writes it in;
`HOST_STATE_VALUE_STATES`, `src/host/host_internal.h:334`).

```
{"id": 1, "method": "actor.state", "params": {"session": 1}}
→ {"id": 1, "result": {"value": {"top": {"p": {"r1": "x", "r2": {}, "r3": {"k": "k1"}}}}}}
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
{"id": 1, "method": "trace.spans.decode", "params": {"traces": h'0a8a...'}}
→ {"id": 1, "result": {"spans": [{"trace": "4bf9...", "span": "00f0...", "parent": null,
   "name": "pinged", "kind": 1, "start": 1700000000000000000, "end": ...,
   "status": 0, "statusMessage": "", "attributes": {...}, "events": []}]}}
```

## Notifications

The host's notifications are `TA_HOST_EFFECT_NOTIFICATION` effects: their
`WIRE` is a JSON-RPC notification, `{"method", "params"}`, their `EVENT`
the method's name, and their `SESSION` the session it is about (0: none).
They live until the ack, like every effect, and come with the next take after the run that appended them. A notification never stands
for an effect a binding must act on (`WAKE`, `SEND`, `TRACES`, ... stay
effects of their own).

No notification is defined. An application reads a session's state with
`actor.state`, and an actor's output reaches it through an I/O
processor. The kind stays: a notification added later is a method name in this
section, appended as an effect where the host learns of it, and a binding
that reads a method it does not know drops the effect.

## Not methods

These stay C calls:
- **The hot path:** spawn, send, send_many, terminate, advance_time,
  resume, run, and the effects and their ack. (`actor.send` is for a
  client without a binding; a program in the process calls
  `ta_host_send`.)
- **Images:** `ta_host_add_image` takes the image's bytes and keeps a copy.
- **Bytes that the host writes or takes:** `ta_host_traces`, `ta_host_logs`,
  `ta_host_metrics`, hibernate, wake, migrate, snapshot,
  restore and `snapshot_pack`.
- **Calls that need no host:** `ta_host_snapshot_unpack`, `_image` and
  `_segments` read a snapshot before a host exists.
- **Lookups that spawning and routing use:** `ta_host_chart`,
  `ta_host_locate`, `ta_host_actor_by_name` and `ta_host_resolve`.

## Adding a method

### What belongs here

A method is a decision core can make without the language, called rarely:
configuration, inspection, counters, and decoding bytes that core produces.
Its defaults, its validation and its error text live in core, so every
binding gets the same answers.

### What does not

- the hot path: inputs (spawn, send, deliver, ...) and effects;
- bytes the host writes or takes: traces, logs, metrics, hibernation,
  export and snapshot blobs. These are C calls that give a view of the
  host's memory (snapshots: the caller's `ta_buffer`);
- calls that need no host;
- I/O, transport, and policy around I/O (retries, caps, posting);
- anything a binding can compute from other replies;
- values specific to a datamodel, other than neutral CBOR values.

### Steps

1. **Name it `area.verb`**, like `trace.watch` or `actor.get`. A method
   about the whole host is in the area `host` (`host.stats`).
2. **Params:** name each (camelCase), its type, its default and its limits.
   Refuse params the method does not take.
3. **The result:** a map with its keys in a fixed order.
4. **Errors:** use a reason from the table above. A new reason needs its own
   code in the -32000 to -32099 range, added to the table.
5. **Atomic:** write the result first and apply the change only when
   `request_fits` says the host had room for it: the reply fits, and the
   effect pool holds room for the reply and three records (the method's own
   effects and the REPLY), so `TA_ERROR_NO_MEMORY` means nothing changed.
   A method that appends more than two effects of its own raises
   `POST_EFFECTS` (`src/host/request.c`). A method that only reads needs
   nothing: `ta_host_post` reserves room for its reply after it is written.
6. **Refused during a run:** `ta_host_post` does this for every method.
   **Messages without an id** need nothing either: a method runs the same
   with or without an `id`, and `ta_host_post` drops what it would write.
   A notification the host sends is a method name in the section above,
   appended as an effect where the host learns of it.
7. **Vectors** in `tinyactors-test/suites/protocol/requests.txt`: the result, its defaults,
   each param's error, and a `refused` case for a method that changes the
   host (the host's allocator refuses room for the reply: nothing applied). Unit tests (`tests/unit/test_host_request.c`) only for what a
   vector cannot show: a failing allocator, a call during a run.
8. **The entry on this page**, with its params, result, errors and one
   example.

# Host protocol

The host reads one UTF-8 JSON value per line from standard input and writes one
compact JSON response per line to standard output. Standard error is reserved
for diagnostics and must never contain protocol messages.

Requests and responses use the JSON-RPC 2.0 object shapes. The initial server
implements individual requests and notifications; JSON-RPC batch arrays are not
supported. Each input line is limited to 8 MiB.

```json
{"jsonrpc":"2.0","id":1,"method":"system.ping","params":{"value":42}}
{"id":1,"jsonrpc":"2.0","result":{"value":42}}
```

The process is stateful. Both peers may send notifications. A notification has
no `id`, and its receiver must not send a response. The host emits `host.ready`
after initialization; the Ruby driver accepts notifications interleaved with
ordinary responses and records them separately.

Methods generated from the public C API create core objects in the host and
refer to them through protocol handles (see [Generated methods](#generated-methods)).
`host.shutdown` performs an orderly shutdown. End-of-file also terminates the
process.

## Method rules

- Methods use `<domain>.<operation>` names.
- Tinyactors runtime IDs are serialized as JSON numbers only when their full
  value is representable; opaque protocol handles are preferred for long-lived
  objects.
- Methods that execute Tinyactors return observations only after the native API
  operation finishes.
- Synchronous adapter behavior is configured before execution so callbacks do
  not require nested RPC conversations.
- Timings and allocation counts are measured inside the C host. Ruby-side IPC
  time is not included in core metrics.
- Random choices are supplied explicitly and included in replay traces.

## Host methods

`rpc.discover` returns server metadata, every method name (`methods`), a schema
per method (`schemas`), the generated methods whose core function is not yet
implemented (`unimplemented`), and the registered adapters (`adapters`).

`system.ping` returns its parameters unchanged.

`json.transform` evaluates a jq filter against one JSON input. Its result is an
array because a jq filter can emit zero, one, or many values.

`metrics.snapshot` reports protocol counters. Core allocation and execution
metrics will be added with the corresponding Tinyactors commands.

`host.shutdown` returns `true` and then exits the request loop.

`handle.fixture` takes `{"type": "system"|"definition"|"session_image"}` and
returns a handle of that type that never reaches native code. It exists so
tests can exercise handle validation before the core implements the functions
that create real handles.

`system.actors` takes `{"system": <handle>, "runnable": bool}` and returns the
IDs of every resident actor, including children that invokers spawned. With
`"runnable": true` it lists only actors whose macrostep is in progress. It is
built on core's own enumeration, `ta_system_visit_actors`, walking
everything in one call. That function is also bound directly
(`ta.system.visit_actors`), with `from` and `limit` for one page
(ADR 0062); `next` comes back in `out` whatever the result. The callback it
takes is an `inspector.recording` instrument, so a scenario can assert on
the walk itself.

`instrument.list`, `instrument.journal`, and `instrument.control` read and
steer the instrumented adapters (see [Instruments](#instruments)):

- `instrument.list` returns `[{"name", "kind"}]` for every instrument.
- `instrument.journal` takes `{"name", "since"?}` and returns the journal
  entries whose `seq` is greater than `since`, oldest first. It never
  creates: reading the journal of an instrument core has never been handed
  is an error, so an unused instrument cannot look like a quiet one.
- `instrument.control` takes `{"name", "command", "args"?, "kind"?}`; the
  commands of each kind are listed below.

An instrument exists once core has been handed its adapter. `instrument.control`
with `kind` creates one that does not exist yet, so a scenario can arm a fault
or set a budget before the call that first passes the adapter to core. Only the
kinds whose adapter needs no system handle can be created this way:
`allocator.counting`, `directory.memory`, and `inspector.recording`.

### Hosted mode

The `hosted.*` methods run a system through the host kernel
(`tinyactors-host`, kernel ADR 0001 section 5) instead of through core calls
a scenario makes one by one. The kernel queues, schedules and routes; the
scenario runs turns and owns the clock, which starts at zero and moves only
when the scenario moves it, with a fixed random seed, so a hosted run is
reproducible. Scenarios about core's semantics keep using core directly.

- `hosted.create` takes `{"incarnation"?, "allocator"?}` and returns
  `{"hosted", "core"}`: a `hosted` handle, and a `system` handle for the
  kernel's core system, so the generated core methods (defining charts,
  inspecting actors) work on it. The core handle is retired when the hosted
  system closes. `allocator` is an allocator adapter, libc's when absent;
  with an `allocator.counting` instrument its budget and armed faults reach
  the kernel and core. When the kernel cannot create the system it returns
  the `ta_result` name instead (`"err_nomem"` under a small budget).
- `hosted.spawn` takes `{"system", "definition", "family"?, "name"?}`,
  spawns the actor in core and hands it to the kernel's scheduler, and
  returns its actor ID.
- `hosted.post` takes `{"system", "actor", "event", "data"?}` and puts the
  event in the actor's mailbox, with `data` as a jq value; it answers the
  `ta_result` name.
- `hosted.checkpoint` takes `{"system", "actor"}` and writes a checkpoint of
  an idle, logged, named actor with the kernel's encoder (kernel ADR 0002
  section 7): `{"result", "bytes"?}`, the bytes in hex. A checkpoint alone,
  or a checkpoint followed by later records, is a log `hosted.restore`
  reads.
- `hosted.turn` takes `{"system", "limit"?}`, runs turns until the system is
  quiet or `limit` turns ran (default 1), and returns `{"turns", "quiet"}`.
- `hosted.advance` takes `{"system", "time"}` and moves the scenario's
  clock, and core's time, to `time` milliseconds.
- `hosted.log` takes `{"system", "families"}` and turns on the kernel's
  input log (kernel ADR 0002) for the named actors of those families. Its
  values are written with the jq data model's codec, so hosted charts use
  the jq data model.
- `hosted.batches` takes `{"system"}` and returns the batches handed over
  since the last call, one per address per turn, in turn order:
  `[{"family", "name", "bytes"}]`, with the bytes in lowercase hex. A
  scenario stores them as an application would: each address's bytes
  appended in order.
- `hosted.evict` takes `{"system", "actor"}`: the kernel records `evicted`,
  and the actor is destroyed, so its log is its only state.
- `hosted.restore` takes `{"system", "definition", "family", "name", "log"}`,
  where `log` is the address's stored bytes in hex, and rebuilds the actor
  with the kernel's replay: it prepares it from the log's first record,
  applies the records, publishes it at the last record's time, and puts
  the messages the log still owes back in its mailbox. It returns the new
  actor's ID. It is called explicitly rather than from a loader: napi's
  loader orchestration is napi's, and this method tests the kernel's part.
- `hosted.close` takes `{"system": <hosted handle>}`, closes and frees the
  system, and returns `{"leaked": <bytes>}`: what the kernel allocated and
  did not free, which is always 0.

## Generated methods

`mise run bindings` generates one method per public function in
`tinyactors-core/include/tinyactors.api.json` (see
`tinyactors-core/docs/internal/api-ir.md`) and one codec method per bound
struct type. `bindings.rb` lists the functions and types that are skipped, with
a reason, or implemented by hand, and the layout decisions described below.

### Naming

`ta_<domain>_<rest>` becomes `ta.<domain>.<rest>`, split at the first underscore
after `ta_`: `ta_actor_spawn` is `ta.actor.spawn`,
`ta_system_register_definition` is `ta.system.register_definition`, and
`ta_event_name_intern` is `ta.event.name_intern`.

### Params

Params is an object keyed by the C parameter names. A parameter named `params`
that points at a struct is flattened: its input fields sit next to the other
parameters.

```json
{"jsonrpc":"2.0","id":7,"method":"ta.actor.spawn",
 "params":{"system":{"$handle":"system:1"},"definition":3,"address":{"family":{"name":"door"},"name":"d1"}}}
```

Output parameters and output fields (`out`, `status`, `needed`, ...) are never
accepted as input. Unknown members are errors. A pointer is required unless
the header marks it `TA_OPTIONAL` (`optional` in the API description); a
missing required pointer is a -32602 error. Direct by-value parameters are
required; by-value members of a params struct default to zero.

A caller buffer (`TA_BUFFER`, such as the `states` of `ta.actor.active_states`)
is an output, sized by its `capacity` member. `capacity` may be at most 65536
elements (`MAX_BUFFER_ELEMENTS` in `bindings.rb`); a larger one is a -32602
error, because the client would otherwise choose how much memory the host
allocates.

### Results

A function that returns `ta_result` answers `{"result": <name>, "out": {...}}`.
The result name is the constant without `TA_`, in lowercase: `"ok"`,
`"err_busy"`, `"err_not_found"`, and so on. A non-OK result is a normal
response, not a JSON-RPC error, so tests can assert on it. `out` holds every
output by its C name. An output is reported only after a result that makes it
valid (its `valid_on` in the API description) and is `null` otherwise. Most
outputs, new handles included, are valid only after `"ok"`. Counts and sizes
that a function reports on failure are valid after every result: the `count`
of `ta.actor.active_states` or the `needed` of `ta.event.name_format` after
`"err_nomem"`. A buffer output is an array of the reported count, at most
`capacity` elements, or a string for a text buffer.

Other functions answer `{"value": <return value>}`, plus `out` when they have
outputs; `void` functions answer `{}`.

JSON-RPC errors are reserved for the protocol itself:

| Code | Meaning |
|---|---|
| -32602 | Params do not decode. `data` is a path and a reason, e.g. `params.config.allocator: no ta_allocator adapter named "x"`. |
| -32001 | The core function is declared in `tinyactors.h` but not implemented yet. Params are fully decoded and validated first. |

### Value encoding

- Integers are JSON numbers. 64-bit values (`ta_actor_id`, `ta_definition_id`,
  `ta_time`, ...) are
  JSON numbers up to 2^53-1 and decimal strings above; decoders accept both
  forms.
- `ta_str` and `ta_bytes` are JSON strings. `ta_expression` is its source
  text, or `{"source": text, "span": n}` when it says where it was written
  (core ADR 0085); host expressions cannot cross the protocol.
- `ta_value` is any JSON value. It decodes into a value of the data model of
  the receiving actor, as the function's `value_model` in the API description
  names it: the `actor` of `ta.actor.deliver` and `ta.actor.report_invoke`, or
  the `definition` of `ta.actor.spawn` and `ta.actor.prepare_replacement`,
  or the definition of the `image` of `ta.actor.load` and
  `ta.actor.reconstruct` (`ta_session_image_definition`). Every other method
  decodes into jq values.
  The wire carries no model tag. On output each value is encoded by its own
  data model. An ECMAScript value that JSON cannot represent encodes as
  `{"$value": "undefined" | "function" | "symbol" | "unserializable"}` at the
  top level, and as `null` when nested. A value of no known data model
  encodes as `{"$value": "foreign"}`. Markers are output-only. See
  `docs/ecmascript-datamodel.md`.
  An owned output value (`TA_RELEASE`/`TA_RELEASE_IF` in the header, such as
  `ta_actor_completion.data` and the `out` of `ta_actor_datamodel_root`) is
  encoded, then released by the handler.
- Enums are their symbols in the API description: the constant name without
  the prefix its `TA_SYMBOLS` declares, in lowercase. `TA_RUN_PAUSED` is
  `"paused"`, and `ta_result` constants are `"ok"`, `"err_busy"`, and so on.
- A tagged union (`TA_TAGGED`) is one object with `"kind"`. A struct member of
  the union is flattened into that object when `FLATTEN_UNION_MEMBERS` in
  `bindings.rb` lists it: every action of `ta_scxml_action_spec` except
  `invoke`, whose own `kind` would collide. Other members are keyed by the
  member name. When `kind` is omitted and exactly one
  keyed member is present, that member selects the kind: `{"expr": ".x"}` is
  `{"kind": "expr", "expr": ".x"}`.
- A named `ta_actor_ref` is an interned address handle in C (ADR 0065) and
  `{"kind": "named", "family": {"name": ...}, "name": ...}` on the wire, through
  a hand-written codec. Decoding interns the address into the request's
  system (the one its `system` handle names) or, for the codec round trips,
  into the binding's own, and releases it when the request ends.
- The `ta_scxml_*` specification structs omit absent (zero) members when
  encoded. Their child pointers are arrays that the decoder terminates with a
  zero element; an element that would itself terminate the list is an error.
- Pointer and count pairs (`ta_atom_list`) are arrays.

### Handles

Opaque core objects (`ta_system`, `ta_definition`, `ta_session_image`) travel
as `{"$handle": "<type>:<id>"}`. A handle is created when a function writes a
`T **` output and returns `"ok"`. `ta_<type>_destroy` retires it, and
`ta_system_register_definition` retires the definition handle when it returns
`"ok"`, because the system then owns the definition. IDs are never reused, so
a retired handle stays invalid. A handle of the wrong type, an unknown handle,
or a retired handle is a -32602 error.

Borrowed opaque views (`ta_dm_host`, `ta_arena_block`) encode as
`{"$borrowed": "<type>"}` and cannot be passed back.

### Arenas and borrowed outputs

A `ta_arena *` a function fills with its outputs (`TA_ARENA` in the header) is
not a params member: the binding passes its
per-request arena, which lives until the response is encoded. A `const T **`
output (`view_out`) is encoded as a `T` after the call, or `null` when the
result is not `"ok"`. `ta.definition.export` uses both: it answers
`{"result": "ok", "out": {"out": <spec>}}`, where `<spec>` is the JSON form of
`ta_scxml_spec` described under [Chart spec](#chart-spec) and can be passed
back to `ta.definition.compile_spec`. Exported empty lists are omitted, except
that a handler's `actions` is `[]`; union kinds are always explicit.

### Adapters

A vtable-bearing value (`ta_allocator`, `ta_datamodel`, `ta_actor_directory`,
`ta_io`, `ta_invoker`, `ta_external_event_router`, `ta_actor_inspector`) is
`{"$adapter": "<name>", "config": {...}}`. Adapters are C factories registered
in `src/adapters.c`. They run synchronously, so callbacks never become nested
RPC conversations. The current adapters are `allocator.libc`,
`datamodel.jq`, `datamodel.ecmascript` (SCXML B.2 on quickjs-ng),
`datamodel.null` (SCXML B.1: `In(state)` conditions only, no data),
`router.direct`, and `invoker.scxml`, plus the instruments
`allocator.counting`, `router.recording`, `directory.memory`,
`invoker.scripted`, `io.recording`, and `log.recording` (see [Instruments](#instruments)). `router.direct` takes
`{"system": <system handle>}` and hands each external event to
`ta_actor_deliver()`; a busy actor makes it answer retry, so core keeps the
event for a later dispatch. A named destination is resolved first with
`ta_actor_find()`, through whatever actor directory the system has, so a chart
can address a peer as `#_actor_<family>/<name>` without the host installing a
routing instrument. Resolution answers unsupported when no directory is
installed, unloaded for an address the directory holds but has not loaded, and
not-found for one it does not know.

An adapter in an output, such as `ta_definition_inspection.datamodel`, encodes
as `{"$adapter": "<name>"}` when it matches a registered factory (same vtable)
and `{"$adapter": null}` otherwise. It identifies the adapter; it has no
config and cannot call it.

`invoker.scxml` takes `{"system": <system handle>, "charts": {name: spec}}`
and implements the SCXML invoker type (`http://www.w3.org/TR/scxml/`; core
normalizes `scxml` to it). Each invocation runs as a child actor spawned with
`ta_actor_spawn()` and the invoking actor as its `parent`: the machine is the
invoke's literal `<content>`, or, for `src="file:NAME.scxml"`, the chart
`irp_NAME` from `charts`. Core hands literal content to the invoker as bytes
with their media type (ADR 0053), and the invoker reads the media types it
has a reader for: `application/scxml+xml`, an SCXML document it imports with
the SCXML codec (`../tinyactors-scxml`), and
`application/vnd.tinyactors.scxml-spec+json`, a chart spec as JSON (the DSL's
`chart:`). Content of any other media type is refused. A `<content expr>`
value that is a string is SCXML text, and a `charts` entry may be a spec or
SCXML text. The config's `files` serve `<data src>` in imported documents.

`codec.scxml` is the SCXML codec as a `ta_chart_codec`, for
`ta.definition.import` and `ta.definition.encode`. Its config is null for the
default limits, or an object with any field of `ta_scxml_codec_params`
(`{"max_depth": 32}`). `resolver.map` is a `ta_chart_src_resolver` over
`{"files": {src: text}}`; an unknown `src` is not found.
`<param>`s whose names match the child's top-level `<data>` become its
data-model binding. Core routes `#_parent`, `#_<invokeid>`, autoforwarding,
and `done.invoke`; cancelling an invocation destroys the child. The child
compiles with the data model its spec's `datamodel`
names (`jq` or `ecmascript`), or with the parent's when the spec names none.
`src`, content, and params cross into the child as wire JSON: they are
encoded with the parent's model and decoded with the child's. The invoker
refuses a child whose data model differs from the parent's. The parent then
gets `error.execution`, and no child is spawned.

### Instruments

Instruments are adapters in `src/instruments.c` that record every call core
makes to them in a journal and answer with faults a test arms beforehand.
Each one is a named instance that lives as long as the host process. Its
config always has `"name"`, which must be unique in the process. A later
adapter argument of `{"name": ...}` alone refers to the same instance, so one
counting allocator can back a system and every definition it compiles.

A journal entry is an object with `seq`, `op`, and the operation's details.
`seq` is one counter shared by every instrument, so entries of different
instruments can be ordered against each other. Events are encoded as
`ta_event`, plus `name_text` with the dotted name. Actor references are
encoded as `ta_actor_ref`. Host values are encoded as wire JSON.

Every kind except the allocator arms faults with
`fail {"op", "count"?, "result"?, "status"?, "message"?}`. The next `count` calls of
`op` (default 1; 0 means every later call) fail without doing their work,
and their journal entries carry `"injected": true`. `clear_faults` disarms
them.

| Kind | Config | Journal ops | Commands |
|---|---|---|---|
| `allocator.counting` | `name` | `alloc_failed`, `free_unknown`, `free_mismatch` | `stats`, `fail {after, count}`, `budget {bytes}`, `clear_faults` |
| `router.recording` | `name`, `system`, `mode`?, `directory`? | `submit`, `release`, `drop` | `mode {mode}`, `held`, `release {id}`, `drop {id}`, `fail {op: "submit", status?}`, `clear_faults` |
| `directory.memory` | `name` | `claim`, `resolve`, `replace`, `remove` | `entries`, `put {family, name, kind, actor?}`, `fail`, `clear_faults` |
| `invoker.scripted` | `name`, `system` | `start`, `cancel`, `forward` | `services`, `fail`, `clear_faults` |
| `io.recording` | `name`, `system` | `send` | `fail`, `clear_faults` |
| `log.recording` | `name`, `system` | `log` | none |
| `inspector.recording` | `name` | `visit` | `stats`, `fail {after, result}`, `clear_faults` |

- **`allocator.counting`** wraps libc. It tracks every live allocation by
  address, and `stats` reports live allocations and bytes, the peak, and
  totals. A free of an address it did not hand out is journaled as
  `free_unknown` and never passed to libc. A free with a different size or
  alignment is journaled as `free_mismatch`. `fail {after: n, count: m}`
  lets `n` more allocations succeed, then fails `m` of them (`0`: every
  later one). `budget {bytes: n}` bounds what core may hold at once, as a
  fixed pool or an embedded heap does (ADR 0023): an allocation that would
  push live bytes over `n` fails, and its `alloc_failed` entry carries
  `"reason": "budget"` (an armed one carries `"armed"`). `bytes: 0` and
  `clear_faults` lift it again.
- **`router.recording`** journals every submitted delivery with its
  `status` (`accepted`, `retry`, `error`). With `directory` (the name of a
  `directory.memory` instrument) it resolves a named destination through
  that directory's `resolve` operation, the one core calls (ADR 0037), so
  the resolution is journaled by the directory and a fault armed on
  `resolve` reaches routing too. The submission records the entry as
  `resolved`: a resident entry is delivered, an unloaded one answers
  `err_unloaded` (this router cannot load an actor; a host would), and an
  unknown address answers what `resolve` reported, `err_not_found` by
  default. Without a directory, a named destination is unsupported.
  The host resolves a named destination because it owns location and
  residency: the actor may be unloaded, or live in another process. In mode `direct` it delivers like
  `router.direct`, and it also records the `ta_actor_deliver` result. In mode
  `hold` it clones the delivery and answers accepted. `held` lists the held
  deliveries. `release {id}` delivers one with `ta_actor_deliver`; a busy
  actor keeps it held. `drop {id}` discards one. An injected `submit` fault
  answers `status` `"retry"` or `"error"` (the default).
- **`directory.memory`** is a compare-and-swap directory. `claim` fails with
  `err_stale` when the address has an entry. `replace` and `remove` fail with
  `err_stale` unless the current entry equals `expected`. `resolve` of an
  absent address answers `err_not_found` and kind `none`. `put` changes an
  entry behind core's back, as another process would. Core does not call the
  directory yet.
- **`invoker.scripted`** is a service that is not an actor. `start`
  journals the request (`parent`, `invoke_id`, `type`, `src`, `data`,
  `params`, `autoforward`, and `content` and `content_type` when the invoke
  has literal `<content>`, ADR 0053), with `type` as the invoker type's name
  (ADR 0049), and answers a new `service` handle. The test then
  reports the service's events, completion, or loss with
  `ta.actor.report_invoke`. `cancel` and `forward` are journaled with
  `known`, which tells whether the service was still live.
- **`io.recording`** is an Event I/O Processor that journals each send
  (`source`, `send_id`, `event_text`, `io_type`, `target`, `data`, and
  `content` and `content_type` for literal `<content>`, ADR 0053) and
  delivers nothing. `io_type` is the type's name, read back from its atom
  with `ta_system_atom_text` (ADR 0049), not the handle.
- **`log.recording`** is the system's `<log>` sink (ADR 0045). It journals
  each record (`source`, `label`, `value`, `has_value`) and has no
  commands: writing a log cannot fail, so there is nothing to inject. A
  `<log>` whose expression fails writes no record; the failure is an
  ordinary execution error.
- **`inspector.recording`** is the `ta_actor_inspector` that
  `ta.system.visit_actors` walks the resident actors with. Each `visit`
  entry carries the visited `actor` and the whole `view` it was handed, in
  the encoding `ta.actor.inspect` answers with, so a scenario can compare
  the two. `fail {after: n, result?}` answers the visit after `n` successful
  ones with `result` (default `err_busy`), which stops the walk.

### Codec methods

`codec.roundtrip.<type>` takes `{"value": ...}`, decodes it into the C type, and
returns the encoding of the result. These methods exercise the codecs while
most core functions are still unimplemented.

### Schemas

`rpc.discover` returns a schema per method:

```json
{"function":"ta_actor_spawn","implemented":false,"result":"ta_result",
 "params":{"system":{"type":"handle:system","required":true},"definition":{"type":"ta_definition_id","required":false}},
 "out":{"out":"ta_actor_id"}}
```

The schema is derived from the same plan as the handler, so `required` matches
what the handler enforces. The Ruby driver builds a proxy from these schemas:
`driver.ta.actor.spawn(system:, definition:)` checks argument names and
required arguments before sending.

## Scenario runner

The scenario DSL (`ruby/tinyactors_test/scenario.rb`) drives the generated
`ta.*` methods directly; the host has no scenario-specific methods. Each step
maps onto the public C API:

| Step | Calls |
|---|---|
| first use of the system | `ta.system.create` with `{"allocator": {"$adapter": "allocator.counting", "config": {"name": "<run>.allocator"}}, "initial_time": 0}`, then `ta.system.set_external_event_router` with `router.direct`, and `ta.system.register_invoker` with `invoker.scxml` (every defined chart as `charts`) for the atom of `http://www.w3.org/TR/scxml/` |
| first `spawn` of a chart | `ta.definition.compile_spec` (spec plus `{"$adapter": "datamodel.jq"}` and the run's counting allocator), then `ta.system.register_definition`, which takes ownership |
| `spawn` | `ta.actor.spawn` |
| `advance_time` | `ta.system.set_time` with absolute logical time |
| `deliver` | `ta.system.intern_atom` per segment and `ta.event.name_intern` with `{"atoms": [...]}` (cached), then `ta.actor.deliver` with an external event whose `invoke_id` is `TA_INVOKE_ID_NONE` |
| after every step | run to quiescence: `system.actors` with `"runnable": true` lists every resident actor with a macrostep in progress, including invoked children; `ta.actor.resume` each until `stable` or `done` and list again until none is runnable; then `ta.system.dispatch_due`; repeat until nothing was dispatched and nothing more is due |
| end of the scenario | `ta.system.destroy`, then `instrument.control` `stats` on the counting allocator: live allocations or a bad free fail the scenario |
| `active` | `ta.actor.inspect` for the count, `ta.actor.active_states`, and `ta.definition.state` for each state's ID (cached) |
| `data` | `ta.actor.datamodel_root`, then `json.transform` for a filter |

A result other than `"ok"` fails the step with `TinyactorsTest::CoreResultError`
(`"ta_actor_spawn returned err_invalid"`). Until a core function is
implemented, its call answers RPC error -32001, which is what the scenarios'
`expect_failure` markers name. `ruby/runner_selftest.rb` runs the same steps
against a scripted fake host, with argument names checked against the real
host's schemas.

Event names travel as atom lists because the core has no dotted-string
parser. Scenarios observe only quiescent states.

### Chart spec

Charts compile from the JSON form of `ta_scxml_spec` (see
[Value encoding](#value-encoding)). A chart spec round-trips unchanged through
`codec.roundtrip.ta_scxml_spec`:

- The top level has `name`, `datamodel`, `binding` (optional: `"early"` or
  `"late"`), `data`, `initial`, and `states`. `datamodel` is the SCXML
  `<scxml datamodel>` attribute: the data model the document requires. The
  chart DSL defaults it to `"jq"`, the name of the `datamodel.jq` adapter, and
  the core rejects a mismatch with `err_unsupported`.
- `script` (optional) is the `<scxml>` element's `<script>`: jq source run
  once per session after early data initialization and before the initial
  transition. The chart DSL declares it with `script '...'` at the chart root.
- A state is `{"kind", "id", "states"?, "initial"?, "transitions"?, "onentry"?, "onexit"?, "data"?, "invokes"?, "done_data"?}`.
  `kind` is one of `atomic`, `compound`, `parallel`, `final`,
  `history_shallow`, or `history_deep`. A history state's default transition
  is its single entry in `transitions`.
- `onentry` and `onexit` are lists of handlers, one per `<onentry>` or
  `<onexit>` element, in document order. A handler is `{"actions": [...]}`,
  and `actions` is required. Each handler is a separate block of executable
  content: an error skips the rest of that handler, and the next handler
  still runs.
- `initial` is `{"targets": [id, ...], "actions"?}`; `actions` is the
  executable content of SCXML's `<initial><transition>`.
- A data declaration is `{"name", "initializer"?}`, where `initializer` is a
  value spec. `data` appears at the top level and in states.
- An invoke is `{"kind": "invoke", "id"?, "type"?, "src"?, "data"?, "autoforward"?, "params"?, "finalize"?}`,
  where `id` is an ID spec, `type` and `src` are string specs, `data` is the
  inline `<content>` as a value spec, and `finalize` is a list of actions.
- `done_data` is `{"content"}` or `{"params"}`.
- A param is `{"name", "value"}`, where `value` is a value spec. SCXML
  `namelist` entries are params with `location` values.
- A transition is `{"kind": "external"|"internal", "event"?, "condition"?, "targets"?, "actions"?}`.
- A value spec (`ta_scxml_value_spec`) is `{"expr": src}`, `{"location": src}`, or
  `{"kind": "content", "bytes": text, "media_type": type}`, where `media_type`
  is optional (ADR 0053).
- A string spec (`ta_scxml_string_spec`, SCXML's `x` / `xexpr` pairs) is
  `{"literal": text}` or `{"expr": src}`.
- An ID spec (`ta_scxml_id_spec`, SCXML's `id` / `idlocation`) is
  `{"literal": text}` or `{"location": src}`.
- The action kinds are:
  - `raise`: `{"event", "data"?}`
  - `send`: `{"event"?, "id"?, "type"?, "target"?, "delay"?, "data"?, "params"?}`,
    where `event` is `{"name"}` or `{"expr"}`, `id` is an ID spec, and
    `type`, `target`, and `delay` are string specs
  - `cancel`: `{"sendid"}`, where `sendid` is a string spec
  - `assign`: `{"location", "value"}`
  - `script`: `{"script"}`
  - `log`: `{"label"?, "expr"?}`
  - `if`: `{"condition", "then_actions"?, "else_actions"?}`; `<elseif>` is a
    nested `if` in `else_actions`
  - `foreach`: `{"array", "item", "index"?, "body"?}`, where `item` and `index`
    name the variables bound on each iteration
- Where elements were written (core ADR 0085): the top level may have
  `spans`, a list of `{"source", "offset"?, "length"?, "line", "column"?,
  "end_line"?, "end_column"?}`, and `sources`, a list of `{"name"}`. A
  state, transition, initial transition or action has `"span": n`, an index
  into `spans` counting from 1, and the top-level `<script>` has
  `script_span`. An expression with a span is `{"source", "span"}`. None of
  it changes the revision. The chart DSL records the file and line of every
  call that writes a state, transition, initial transition or action, with
  the Ruby file as the source, and leaves out spans from a chart it embeds
  as `chart:` content, whose bytes are part of the embedding chart's
  revision. `ta.definition.span` answers where element `id` of `kind` is.

#### Send targets

Core owns the target grammar and parses every `<send target>` itself
(ADR 0044), whether the author wrote `target=` or `targetexpr=`. The data
models only give core the text of an evaluated value:

| Target | Destination |
|---|---|
| (absent) | the actor itself, as an external event |
| `#_internal` | the actor's own internal queue |
| `#_parent` | the invoking parent |
| `#_scxml_<actor id>` | that runtime actor |
| `#_actor_<family>/<name>` | the named actor at that logical address |
| `#_<invoke id>` | that invoked child |

A session URI naming no session, such as `#_scxml_4x`, is inaccessible
rather than invalid, so the send reports `error.communication` (SCXML C.1).
Any other unknown target is invalid: `error.execution` (SCXML 6.2). There is
no `#_self`; an absent target already sends to the actor itself, and
`#_self` reads as the invocation named "self".

Core hands a named destination to the external-event router, because the
host owns location and residency: `router.recording` resolves it through
its `directory` instrument.

The spec carries only what an SCXML author writes, never runtime identifiers.
Send IDs are the author's strings. I/O processors are names, and an absent
`type` selects the SCXML Event I/O Processor. A literal `delay` is an SCXML
duration such as `"5s"` or `"500ms"`, which the core compiles to logical
milliseconds. A `delay` expression is jq that is evaluated when the send runs.

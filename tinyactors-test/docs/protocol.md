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
`"runnable": true` it lists only actors whose macrostep is in progress. Core's
own enumeration, `ta_system_visit_actors`, takes a callback and so cannot cross
the protocol.

## Generated methods

`mise run bindings` generates one method per public function in
`tinyactors-core/include/tinyactors.api.json` (see
`tinyactors-core/docs/internal/api-ir.md`) and one codec method per public
type. `bindings.rb` lists the functions and types that are skipped, with a
reason, or implemented by hand.

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
accepted as input. Unknown members are errors. Pointers to a single value are
optional; other direct parameters are required.

### Results

A function that returns `ta_result` answers `{"result": <name>, "out": {...}}`.
The result name is the constant without `TA_`, in lowercase: `"ok"`,
`"err_busy"`, `"err_not_found"`, and so on. A non-OK result is a normal
response, not a JSON-RPC error, so tests can assert on it. `out` holds every
output by its C name. Plain outputs are reported for every result, because
their storage starts zeroed and some functions report through them on failure
(`needed` of `ta_event_name_format`). New handles are reported only for `"ok"`.

Other functions answer `{"value": <return value>}`, plus `out` when they have
outputs; `void` functions answer `{}`.

JSON-RPC errors are reserved for the protocol itself:

| Code | Meaning |
|---|---|
| -32602 | Params do not decode. `data` is a path and a reason, e.g. `params.config.allocator: no ta_allocator adapter named "x"`. |
| -32001 | The core function is declared in `tinyactors.h` but not implemented yet. Params are fully decoded and validated first. |

### Value encoding

- Integers are JSON numbers. 64-bit values (`ta_actor_id`, `ta_time`, ...) are
  JSON numbers up to 2^53-1 and decimal strings above; decoders accept both
  forms.
- `ta_str` and `ta_bytes` are JSON strings. `ta_expression` is its source text;
  host expressions cannot cross the protocol.
- `ta_value` is any JSON value, held as a jq data-model value for the request.
- Enums are the constant names without their common prefix, in lowercase:
  `TA_RUN_PAUSED` is `"paused"`.
- A struct with a `kind` enum and an `as` union is one object with `"kind"`.
  A struct member of the union is flattened into that object, unless one of its
  fields collides with the parent's fields; then it is nested under the member
  name. Other members are keyed by the member name. When `kind` is omitted and
  exactly one keyed member is present, that member selects the kind:
  `{"expr": ".x"}` is `{"kind": "expr", "expr": ".x"}`.
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

### Adapters

A vtable-bearing value (`ta_allocator`, `ta_datamodel`, `ta_actor_directory`,
`ta_io`, `ta_invoker`, `ta_external_event_router`, `ta_actor_inspector`) is
`{"$adapter": "<name>", "config": {...}}`. Adapters are C factories registered
in `src/adapters.c`. They run synchronously, so callbacks never become nested
RPC conversations. The current adapters are `allocator.libc`,
`datamodel.jq`, `router.direct`, and `invoker.scxml`. `router.direct` takes
`{"system": <system handle>}` and hands each external event to
`ta_actor_deliver()`; a busy actor makes it answer retry, so core keeps the
event for a later dispatch.

`invoker.scxml` takes `{"system": <system handle>, "charts": {name: spec}}`
and implements the SCXML invoker type (`http://www.w3.org/TR/scxml/`; core
normalizes `scxml` to it). Each invocation runs as a child actor spawned with
`ta_actor_spawn()` and the invoking actor as its `parent`: the machine is the
chart spec JSON in the invoke's `<content>`, or, for `src="file:NAME.scxml"`,
the chart `irp_NAME` from `charts`. `<param>`s whose names match the child's
top-level `<data>` become its data-model binding. Core routes `#_parent`,
`#_<invokeid>`, autoforwarding, and `done.invoke`; cancelling an invocation
destroys the child. Markup content, such as an SCXML document as XML text, is
not supported.

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

The Ruby driver builds a proxy from these schemas:
`driver.ta.actor.spawn(system:, definition:)` checks argument names before
sending.

## Scenario runner

The scenario DSL (`ruby/tinyactors_test/scenario.rb`) drives the generated
`ta.*` methods directly; the host has no scenario-specific methods. Each step
maps onto the public C API:

| Step | Calls |
|---|---|
| first use of the system | `ta.system.create` with `{"allocator": {"$adapter": "allocator.libc"}, "initial_time": 0}`, then `ta.system.set_external_event_router` with `router.direct`, and `ta.system.register_invoker` with `invoker.scxml` (every defined chart as `charts`) for the atom of `http://www.w3.org/TR/scxml/` |
| first `spawn` of a chart | `ta.definition.compile_spec` (spec plus `{"$adapter": "datamodel.jq"}`), then `ta.system.register_definition`, which takes ownership |
| `spawn` | `ta.actor.spawn` |
| `advance_time` | `ta.system.set_time` with absolute logical time |
| `deliver` | `ta.system.intern_atom` per segment and `ta.event.name_intern` with `{"atoms": [...]}` (cached), then `ta.actor.deliver` with an external event whose `invoke_id` is `TA_INVOKE_ID_NONE` |
| after every step | run to quiescence: `system.actors` with `"runnable": true` lists every resident actor with a macrostep in progress, including invoked children; `ta.actor.resume` each until `stable` or `done` and list again until none is runnable; then `ta.system.dispatch_due`; repeat until nothing was dispatched and nothing more is due |
| `active` | `ta.actor.inspect` for the count, `ta.actor.active_states`, and `ta.definition.state` for each state's ID (cached) |
| `data` | `ta.actor.inspect`'s `datamodel_root`, then `json.transform` for a filter |

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
- A value spec (`ta_scxml_value_spec`) is `{"expr": src}`, `{"location": src}`, or `{"content": text}`.
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

The spec carries only what an SCXML author writes, never runtime identifiers.
Send IDs are the author's strings. I/O processors are names, and an absent
`type` selects the SCXML Event I/O Processor. A literal `delay` is an SCXML
duration such as `"5s"` or `"500ms"`, which the core compiles to logical
milliseconds. A `delay` expression is jq that is evaluated when the send runs.

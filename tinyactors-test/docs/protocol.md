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

Future definition, system, and actor methods will
create objects in the host and refer to them through stable protocol handles.
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

## Initial methods

`rpc.discover` returns server metadata and the currently available methods.

`system.ping` returns its parameters unchanged.

`json.transform` evaluates a jq filter against one JSON input. Its result is an
array because a jq filter can emit zero, one, or many values.

`metrics.snapshot` reports protocol counters. Core allocation and execution
metrics will be added with the corresponding Tinyactors commands.

`host.shutdown` returns `true` and then exits the request loop.

## Planned scenario methods

The Ruby scenarios in `ruby/scenarios` already drive these methods. Until the
host implements them, those scenarios are marked `expect_failure` with RPC
error -32601. Handles are opaque strings.

| Method | Params | Result |
|---|---|---|
| `definition.compile` | chart spec (below) | `{"definition": handle}` |
| `system.create` | `{}` | `{"system": handle}`; logical time starts at 0 |
| `actor.spawn` | `{"system", "definition"}` | `{"actor": handle}` |
| `actor.send` | `{"actor", "event": [atom, ...], "data"?}` | enqueues an external event |
| `actor.inspect` | `{"actor"}` | `{"configuration": [state id, ...], "data": root, "delayed_sends": n}` |
| `system.set_time` | `{"system", "time": ms}` | sets absolute, monotonic logical time |
| `system.run` | `{"system"}` | runs until no actor has runnable work |

Event names travel as atom lists (`"error.execution"` becomes
`["error", "execution"]`) because the core has no dotted-string parser. The
driver calls `system.run` after every spawn, send, and time change, so
scenarios observe only quiescent states.

### Chart spec

`definition.compile` takes a JSON form of `ta_scxml_spec` from
`tinyactors-core/src/builder.h`, so the host can build the spec with `ta_tree`
without interpretation:

- The top level has `name`, `datamodel` (`"jq"`), `binding` (optional:
  `"early"` or `"late"`), `data`, `initial`, and `states`.
- A state is `{"kind", "id", "states"?, "initial"?, "transitions"?, "onentry"?, "onexit"?}`.
  `kind` is one of `atomic`, `compound`, `parallel`, `final`,
  `history_shallow`, or `history_deep`. A history state's default transition
  is its single entry in `transitions`.
- `initial` is `{"targets": [id, ...]}`.
- A transition is `{"kind": "external"|"internal", "event"?, "condition"?, "targets"?, "actions"?}`.
- A value spec (`ta_scxml_value_spec`) is `{"expr": src}`, `{"location": src}`, or `{"content": text}`.
- A string spec (`ta_scxml_string_spec`, SCXML's `x` / `xexpr` pairs) is
  `{"literal": text}` or `{"expr": src}`.
- An ID spec (`ta_scxml_id_spec`, SCXML's `id` / `idlocation`) is
  `{"literal": text}` or `{"location": src}`.
- The action kinds are:
  - `raise`: `{"event", "data"?}`
  - `send`: `{"event": {"name"}, "id"?, "type"?, "target"?, "delay"?, "data"?}`,
    where `id` is an ID spec and `type`, `target`, and `delay` are string specs
  - `cancel`: `{"sendid"}`, where `sendid` is a string spec
  - `assign`: `{"location", "value"}`
  - `script`: `{"script"}`
  - `log`: `{"label"?, "expr"?}`

The spec carries only what an SCXML author writes, never runtime identifiers.
Send IDs are the author's strings. I/O processors are names, and an absent
`type` selects the SCXML Event I/O Processor. A literal `delay` is an SCXML
duration such as `"5s"` or `"500ms"`, which the core compiles to logical
milliseconds. A `delay` expression is jq that is evaluated when the send runs.

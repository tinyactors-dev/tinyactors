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

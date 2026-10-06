# tinyactors-otlp: core's trace records as OTLP

> **Parked.** This project builds against the earlier core (tag
> `ring-proto-final`), which [ADR 0004](../docs/adr/0004-replace-the-core-with-the-standalone-runtime.md)
> replaced. Nothing builds or tests it until it is ported to the current
> `tinyactors-core`; the [ROADMAP](../ROADMAP.md) says how.

A small C library that reads core's `tel` ring and encodes what it reads
as OTLP protobuf: spans in one `ExportTraceServiceRequest` (`/v1/traces`)
and log records in one `ExportLogsServiceRequest` (`/v1/logs`). It sends
nothing; where the bytes go is the host's business. The host kernel
(`tinyactors-host`) feeds it; the Node-API addon (`tinyactors-napi`)
compiles it in and installs it, and the test host (`tinyactors-test`)
links its archive with the kernel's.

It is libc-free C, like core, and builds for `wasm32-freestanding`. It
uses only core's public header (`tinyactors.h`), and is neither in core,
which has no use for protobuf, nor in a binding, because encoding is free
of any language's API.

The API is `include/tinyactors_otlp.h`; its header comment is the
reference.

## Input

`tel.trace` and `tel.dropped` records (core ADR 0098), read with core's
`ta_system_tel_peek`, `ta_trace_record_text` and `ta_system_tel_release`.
A host calls `tao_read_tel(encoder, core)` between core calls, never from
inside one.

- **Names, values and faults** are the record's own text; the encoder
  never calls into core for a trace record. A host log, a definition log
  or a `<log>` record names what it refers to by ID, and for those the
  encoder asks the core named in its config.
- **Times** are the `wall_ns` core stamped each record with. The host's
  `now` callback is read only for what carries no time: logs, and the
  spans `tao_end_all` ends.
- **Span IDs** are core's: `ta_step_key_span_id` of a step's key, so an
  ID a host computes for the same step (an exemplar, a `traceparent`)
  names the same span.
- **A drop** (`tel.dropped`) ends every open span as an error marked
  `tinyactors.telemetry.dropped` with the count; later records of a
  macrostep it ended are counted and skipped.

## Settings

`tao_config` holds them, fixed at `tao_create`:

| Setting | What it does |
|---|---|
| `staging_bytes` | The span staging, reserved whole at creation and shrunk back to after a burst; `tao_staging_full` says when the staged requests reach it. 0 is `TAO_STAGING_BYTES_DEFAULT` (1 MiB); a binding bounds it by `TAO_STAGING_BYTES_MIN` and `_MAX`. napi takes it from its `traceStagingBytes` parameter. |
| `wanted_kinds`, `wanted_watched_kinds`, `wanted_unsampled_kinds` | The record kinds that become spans; `tao_set_wanted` changes them. |
| `link_parents` | A macrostep is a child of its cause (false) or a root linked to it (true). |
| `values` | Values (event data, assigned values) go in attributes. |
| `log_limit` | Log records per definition and name per second; 0 writes no logs. |
| `version`, `allocator`, `host`, `core` | The scope's version, where memory comes from, the host's callbacks, and the system whose logs these are. |

## Output

A host asks for a request's size (`tao_traces_size`, `tao_logs_size`),
which is free, allocates exactly that many bytes in memory its own
language owns, and has the encoder write the request there in one pass
(`tao_put_traces`, `tao_put_logs`). At the end of each turn,
`tao_end_turn` lets a burst's buffers go.

## Building and testing

```sh
mise run build   # build/libtinyactors_otlp.a and the test programs
mise run test    # no recursion, wasm32-freestanding, ASan and UBSan, then the tests
```

`tests/otlp_test.c` runs a real core system and has the encoder read its
`tel` ring: requests are well-formed protobuf of the right shape, spans
stay open across turns, a drop ends them, every allocation can fail, and
the bytes of a small request are fixed.

# Host config

`ta_host_create(config, environment, &host)` (`include/tinyactors/host.h`)
takes the host in two parts: its config as one CBOR map, and a
`ta_host_environment` struct of pointers. The host reads the config during
the call; nothing points into it afterwards.

## Why two parts

What can be written as data is in the config. What cannot is in the
environment.

| Part | What | Why there |
|---|---|---|
| config (CBOR) | the system id, the segments' capacities, the I/O processors, the encoder, the host's limits | plain values: numbers, text, lists and maps. Every binding builds them from its own language's data with one CBOR module, as it builds requests. |
| environment (struct) | the allocator, the datamodels, a trace policy, the error buffer | pointers and callbacks: memory and functions in the caller's process. Bytes cannot carry them. |

**One shape for every binding.** A C struct of capacities and options
grows a field with every release, and each binding then needs code for
it: a converter from its language's object to the struct. A CBOR map is
the shape a binding already writes for requests. A new key needs no new
code in a binding that passes the application's map through.

**Core validates and fills defaults.** The host keeps the effective
config: at `ta_host_create` it fills in every default with the code a
system and an encoder fill theirs with, and the request `host.config`
([requests.md](requests.md)) answers it, every key present, in the shape
below. A binding reads what it needs from there (the clock's
`ticksPerSecond`) and never assumes a default. The host knows which keys it
takes, their types and their limits, and it reports a refusal the way it
answers a request: a code, a reason, a path such as `system.maxActors`,
and a message. A binding does not check the config, does not keep its own
defaults, and does not write its own error messages. Each binding then
behaves the same, and a default changes in one place.

**A failed creation answers like a request.** There is no host yet to ask,
so the reply goes into `environment->error`, a `ta_buffer` the caller
lends (see *Errors*), and not into the host's memory, as a request's reply
does. Without it, the caller still gets the status.

Keys are camelCase, as in [requests](requests.md). An absent key is its
default; a key the host does not take is refused. Examples are in CBOR
diagnostic notation.

```
{"systemID": "orders",
 "system": {"maxActors": 10000, "ticksPerSecond": 1000},
 "ioProcessors": [{"type": "http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor",
                   "location": "http://localhost:8080/"}],
 "telemetry": {"flushInterval": 5000, "resource": {"service.name": "orders"}}}
```

## Keys

| Key | Type | Default | Meaning |
|---|---|---|---|
| `systemID` | text | required | this system's id: its addresses are `tinyactors://<systemID>/...` (docs/protocol.md, Addresses) |
| `system` | map | `{}` | the capacities of each new segment, below |
| `ioProcessors` | list of maps, at most 16 | `[]` | the host's I/O processors: `type` (text, required) and `location` (text). Sends to other systems need none: they go to the host's transport (`TA_HOST_TRANSPORT`) |
| `migrateQuantum` | uint | `256` | actors a migration looks at per run |
| `telemetry` | map | absent: no encoder | the OTLP encoder, below |

Why these defaults:

- **`systemID` has none.** It is part of every address this system hands
  out and of every origin it gives another system, so two systems must
  never share one by accident. Only the application can choose it.
- **`system`: the system's own defaults.** A 0 or an absent key means
  `ta_system_config`'s default, so a host and a system created alone have
  the same capacities, and the numbers are kept in one place.
- **`ioProcessors`: none.** Core does no I/O. A processor is something the
  binding serves, so the binding names the ones it has.
- **`migrateQuantum`: 256.** A migration works between runs. 256 actors
  per run keep a run short while a large segment still moves in a few
  thousand runs.
- **`telemetry`: absent.** An encoder takes memory for its staging and
  needs a binding that takes its effects and posts the bytes. A host that
  does not export pays nothing (a null encoder).

`system` (each a uint; 0 or absent: the system's default, filled in by
`ta_system_config`'s code in `src/system/system.c`; larger values are
clamped to the limits in `tinyactors/system.h`):

| Key | Default |
|---|---|
| `maxActors` | 64 (at most 8388606) |
| `maxEvents` | 256 |
| `maxTimers` | 64 |
| `workspaces` | 4 |
| `maxRequests` | 16 |
| `outbox` | 64 |
| `quantum` | 1000 |
| `instanceBytes` | 0: derived from each segment's image and `maxActors` |
| `stringBytes` | 16384 + 16 × `maxActors` |
| `ticksPerSecond` | 1000000000 (the monotonic clock in nanoseconds; 1000 for milliseconds) |
| `traceBytes` | 512 × `maxEvents`, at least 65536 (0 when `tracing` is false) |
| `traceContexts` | `maxEvents` (0 when `tracing` is false) |
| `traceRules` | 16, at most 255 (0 when `tracing` is false) |
| `traceSeed` | 0 |

and `tracing` (bool, default `true`: the trace records are written, so a
binding can turn on export or a watch later without making the host
again).

**Every capacity is a segment's initial size, not a cap.** When a pool or
slab of a segment fills, a run answers `TA_RUN_EXHAUSTED` and the host
grows that segment, between runs, into a block twice as large
(`segment_grow` in `src/host/host.c`): `maxActors`, `maxEvents`,
`maxTimers`, `maxRequests`, `workspaces`, `stringBytes`, `traceContexts`
and `traceBytes` double, each clamped to its limit, and `instanceBytes`
is derived again. A value given here doubles like a default: `traceBytes`
included, so an explicit `traceBytes` is the journal's starting size, not
a bound on trace memory (accepted 2026-10-13; until then only a defaulted
journal grew, through `maxEvents`). Why the defaults are small:

- **`stringBytes`: 16384 + 16 × `maxActors`** (17408 at 64 actors), the
  system's default for every segment. Until 2026-10-13 the host gave each
  new segment its own default of 32768 instead; with one default in one
  place (`ta_system_config_normalize`), a segment starts with about half
  the string memory, and a workload with many or long strings grows the
  segment earlier (accepted 2026-10-13: memory per segment over fewer
  relocations).

`telemetry` (present: the host has an encoder and gives
`TA_HOST_EFFECT_TRACES` and `TA_HOST_EFFECT_LOGS`):
- uints, 0 or absent for the encoder's default (`ta_otlp_config` in
  `tinyactors/otlp.h`, filled in by the encoder's code, for the same reason
  as `system`): `stagingBytes` 1048576 (from 65536 to 1073741824), `logBytes` a
  quarter of `stagingBytes`, `openSpans` 1024, `chunks` 8 × `openSpans`,
  `scopes` 64, `valueLimit` 256, `renderBytes` 16384 (at least
  `valueLimit` + 4), `resourceBytes` 4096, `metricBytes` 65536,
  `spanMetricCharts` 64;
- `flushInterval` (uint, host clock): a request is ready this long after its
  first record was staged; 0 or absent: only when the staging is full. The
  host has no idea how often a backend wants data; a full staging is the
  one moment it must act;
- `linkParents` (bool, `false`): a macrostep is a root span linked to its
  cause, instead of its child. Off, one trace shows a whole conversation
  between actors, which is what most backends draw;
- `values` (bool, `false`): event data and assigned values in the spans.
  They can hold personal data, so an operator turns them on knowingly
  (ADR 0006 in the repository root);
- `logs` (bool, `true`): `<log>`, failed actions and definition logs
  ([architecture.md](architecture.md), 16.3) as log records. They are few
  and they are what an operator looks for first;
- `resource`: a map of text to text, at most 32 attributes, on every
  request (`service.name`, ...). None by default: only the application
  knows its name.

## Errors

`ta_host_create` returns `TA_ERROR_INVALID` for a config it does not take,
and for an environment without its size, an allocator, or with more than 8
datamodels. When `environment->error` is set, the reply of a refused
request is written there, with the codes and reasons of
[requests](requests.md) and a path from the config's root:

```
{"systemID": "h", "system": {"maxActors": "8"}}
→ {"error": {"code": -32602, "message": "expected an unsigned integer",
   "data": {"reason": "type", "path": "system.maxActors"}}}
```

A config is decoded into at most 256 items, on the stack.

`ta_host_restore` takes the same config, in `ta_host_snapshot_input.config`,
and the same environment.

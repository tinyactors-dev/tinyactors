# Host config

`ta_host_create(config, environment, &host)` (`include/tinyactors/host.h`)
takes the host's config as one CBOR map
([ADR 0009](../../docs/adr/0009-one-host-two-queues.md), decision 9). The
host reads it during the call; nothing points into it afterwards. What
cannot be data is in `ta_host_environment`: the allocator, the datamodels,
a trace policy, and an optional buffer for the error reply.

Keys are camelCase, as in [requests](requests.md). An absent key is its
default; a key the host does not take is refused. Examples are in CBOR
diagnostic notation.

```
{"systemID": "orders",
 "system": {"maxActors": 10000, "ticksPerSecond": 1000},
 "ioProcessors": [{"type": "http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor",
                   "location": "http://localhost:8080/", "sync": true}],
 "telemetry": {"flushInterval": 5000, "resource": {"service.name": "orders"}}}
```

## Keys

| Key | Type | Default | Meaning |
|---|---|---|---|
| `systemID` | text | required | this system's id: its addresses are `tinyactors://<systemID>/...` (docs/protocol.md, Addresses) |
| `system` | map | `{}` | the capacities of each new segment, below |
| `ioProcessors` | list of maps, at most 16 | `[]` | the host's I/O processors: `type` (text, required), `location` (text), `sync` (bool: the run stops after a send to it, `TA_IO_PROCESSOR_SYNC`) |
| `migrateQuantum` | uint | `256` | actors a migration job looks at per run |
| `maxSubscriptions` | uint, at most 2^20 | `64` | sessions `actor.subscribe` watches at once |
| `telemetry` | map | absent: no encoder | the OTLP encoder, below |

`system` (each a uint; 0 or absent: the system's default,
`ta_system_config` in `tinyactors/system.h`): `maxActors`, `maxEvents`,
`maxTimers`, `workspaces`, `maxRequests`, `outbox`, `quantum`,
`instanceBytes`, `stringBytes`, `ticksPerSecond`, `traceBytes`,
`traceContexts`, `traceRules`, `traceSeed`; and `tracing` (bool, default
`true`).

`telemetry` (present: the host has an encoder and gives
`TA_HOST_EFFECT_TRACES` and `TA_HOST_EFFECT_LOGS`):
- uints, 0 or absent for the encoder's default (`ta_otlp_config` in
  `tinyactors/otlp.h`): `stagingBytes`, `logBytes`, `openSpans`, `chunks`,
  `scopes`, `valueLimit`, `renderBytes`, `resourceBytes`, `metricBytes`,
  `spanMetricCharts`;
- `flushInterval` (uint, host clock): a request is ready this long after its
  first record was staged; 0 or absent: only when the staging is full;
- `linkParents` (bool, `false`), `values` (bool, `false`), `logs` (bool,
  `true`);
- `resource`: a map of text to text, at most 32 attributes, on every
  request (`service.name`, ...).

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

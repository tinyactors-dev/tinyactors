# System parameters

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

Every bound in this design is a system parameter under core ADR 0092's
rules:

- parameters are set once, at creation;
- every buffer is bounded by one;
- defaults are enumerators.

This document lists the parameters the design adds, the ones it keeps,
and the ones it removes from core, the kernel and napi.

**Every default below is a guess.** The real figures come from measuring
napi's suites, `scale.js` and the IRP suite, as ADR 0092 did for
`interpreterBytes`. They are recorded in the ADR that adopts them.

## New parameters

| Parameter | Meaning | Unit | Default (guess) | At the bound |
|---|---|---|---|---|
| `poolEnvelopes` | envelopes in the pool: admitted inputs, mail between local sessions, delayed sends of resident sessions, envelopes waiting for a load ([envelope.md](envelope.md)) | envelopes | 4,096 | a `<send>` to a local session or a delayed `<send>` fails as `error.execution.capacity`; admission from h2c has already stopped at the watermark |
| `nameAreaBytes` | the pool's byte area for text that has no ID: event names computed at run time, addresses of sessions that are not resident, origins ([names.md](names.md)) | bytes | 65,536 | the same as a full pool: the send fails as `error.execution.capacity`; admission stops at the watermark |
| `admissionWatermark` | the share of the pool (envelopes, and name area bytes) above which core stops moving records from h2c into the pool. The rest is headroom for mail between sessions. | percent | 50 | h2c fills up, and the host refuses at its edge ([BEHAVIORS.md](../BEHAVIORS.md)) |
| `fairnessRatio` | how many runnable envelopes core takes from the pool for each record it admits from h2c, while both are available ([envelope.md](envelope.md), "Fairness") | envelopes | 4 | not a bound: a ratio |
| `h2cBytes` | the h2c ring ([ring.md](ring.md)) | bytes | 65,536 | the host cannot post. It refuses at its edge, or keeps the input itself. |
| `c2hBytes` | the c2h ring | bytes | 262,144 | an action that writes a record that does not fit fails as `error.execution.capacity`. Core starts a macrostep only with room for its reserve. |
| `telBytes` | the tel ring ([telemetry.md](telemetry.md)) | bytes | 262,144 | the record is dropped and counted in [`tel.dropped`](../protocol/generated/MESSAGES.md#teldropped) |
| `maxRecordBytes` | the largest record on any ring, header and tail included. Part of the c2h reserve. | bytes | 4,096 | a `<send>` whose record would be larger (for example a very long computed event name or target) fails as `error.execution.capacity.record` |
| `internalQueueEvents` | entries in the internal queue of each running interpreter, one of them always kept free for `error.execution.capacity` ([envelope.md](envelope.md#the-internal-queue-is-not-in-the-pool)) | entries | 256 | a `<raise>` fails as `error.execution.capacity.internal` |
| `telemetryValueBytes` | text kept of a value in a [`tel.trace`](../protocol/generated/MESSAGES.md#teltrace) record | bytes | 1,024 | the text is cut, and the record carries the `truncated` flag |

### Constraints between them

Creation checks these and answers `TA_ERR_INVALID` with `rejected` set
(ADR 0092):

- `maxRecordBytes` ≤ `c2hBytes` / 4, so the c2h reserve (one `c2h.input`,
  one `c2h.fence` and one record of the largest size) never fills more
  than half the ring;
- `maxRecordBytes` ≤ `h2cBytes`, `telBytes`;
- `telemetryValueBytes` < `maxRecordBytes`;
- `admissionWatermark` from 1 to 99.

**Open:** whether `nameAreaBytes` should instead be a per-envelope inline
size plus overflow. A fixed inline size wastes space for short names. A
shared area needs an allocator for variable-size slices, which must not
fragment so badly that a free area cannot take a name. [names.md](names.md)
proposes the shared area.

### Memory per system, with the guessed defaults

| Part | Bytes |
|---|---|
| pool: 4,096 envelopes × about 72 bytes, plus 4 each for the deadline index (estimate, see [envelope.md](envelope.md#memory-as-estimates)) | 311,296 |
| name area | 65,536 |
| internal queue: 256 entries × about 48 bytes (estimate) | 12,288 |
| h2c + c2h + tel | 589,824 |
| **total** | **about 960 KiB** |

That is close to napi's current figure of about 1 MB RSS per system,
which ADR 0093 found too much for quiet systems and test suites that
create many. Two ways to bring it down:

- **Fill the pool on demand, up to its bound** (ADR 0093's shape). Growth
  happens only between macrosteps: at admission and after a fence. Before
  each macrostep, core keeps a reserve of free envelopes, so a send inside
  the macrostep only takes from what is already free. The macrostep
  invariant holds ([macrostep.md](macrostep.md)).
- **Small ring defaults** for hosts that are not busy. The rings must be
  fixed at creation, because the host holds pointers into them. A test
  host can create systems with 16 KiB rings.

**Open:** which defaults to choose. A busy napi server and a test suite
want different ones. ADR 0093 settled a similar question by defaulting
to the busy case, and filling on demand.

## Kept

| Parameter | What changes |
|---|---|
| `microstepLimit` (`TA_PARAM_MICROSTEP_LIMIT`) | nothing. A runaway macrostep ends with [`c2h.fence`](../protocol/generated/MESSAGES.md#c2hfence) status `runaway`. |
| `eventNameDepth` (`TA_PARAM_EVENT_NAME_DEPTH`) | now also bounds the scratch that resolves a run-time name's segments for matching ([names.md](names.md)). A deeper name is an invalid name: it fails as plain `error.execution` at the send, or is bounced (reason `invalid`) at admission. |
| `compileMessageBytes`, `compileTextBytes`, `compileNearestBytes` | nothing. Compiling is not a macrostep. |
| `interpreterBytes`, `interpreterNesting` | must be reserved at creation, not grown on first use, if nested interpreters can start inside a macrostep (cancelling an invocation). **Open:** check that this is the only path. |
| `renderBytes` | smaller: names and send IDs no longer render into it, because they are written into ring tails or the name area. It keeps `<param>` arrays and other temporaries. |
| `maxActors` | nothing. A quota on resident actors, checked at spawn and load, which are not macrosteps. |
| `maxPendingSends` | kept as a per-actor quota inside the shared pool, so one actor cannot take every envelope with timers. At the bound, the delayed `<send>` fails as `error.execution.capacity.pool`, as if the pool were full for that actor. |
| `maxDefinitions` | nothing. |

## Removed

| Parameter | Owner today | Replaced by |
|---|---|---|
| `eventBlockCache` (`TA_PARAM_EVENT_BLOCK_CACHE`, ADR 0069/0093) | core | `poolEnvelopes`. The taken event stays in its envelope until the fence, so there are no separate event blocks. |
| `valueBytes` (`TAH_PARAM_VALUE_BYTES`) | kernel | `telemetryValueBytes` |
| `traceStagingBytes` (`TAH_PARAM_TRACE_STAGING_BYTES`) | kernel | `telBytes` bounds the ring. The OTLP encoder's own staging becomes a setting of that library ([telemetry.md](telemetry.md)). |
| `resumeBudget`, `actionBudget` (`TAH_PARAM_RESUME_BUDGET`, `TAH_PARAM_ACTION_BUDGET`) | kernel | the budget arguments of `ta_run`: the host passes them per call. They are not system parameters, because they only shape one call. |
| `resumesPerTurn`, `dueBudget`, `messageBudget` | kernel | `ta_run`'s budget (envelopes per call), and `fairnessRatio` for the split between mail and admitted input |
| `maxQueuedMessages`, `maxRetainedBytes` | kernel | `poolEnvelopes`, `nameAreaBytes`, `admissionWatermark`. Retained data is the data model's memory, which core does not count ([data-model.md](data-model.md)). |
| napi's loader `maxWaiting` (per family) | napi | the pool. **Open:** a per-address cap ([unloading.md](unloading.md)). |

napi's own parameters (`registeredDatamodels`, `datamodelNameBytes`, and
the others in `tinyactors-napi/src/napi_params.c`) are unaffected.

## Where they are reported

`ta_param_info_of()` reports every core parameter with its name, unit,
default, minimum and maximum (ADR 0092 section 2). With the kernel gone,
napi's `parameterInfo()` lists only two owners, `core` and `napi`
(`tinyactors-napi/lib/params.js`).

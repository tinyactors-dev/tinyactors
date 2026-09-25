# ADR 0075: A macrostep that runs too many microsteps stops as a runaway

**Status:** Accepted; not yet implemented. Applies ADR 0067 (the step counters), ADR 0070 (commits and retries) and ADR 0072 (counters, logs as records); napi's part applies napi ADRs 0007, 0008 and 0017.
**Date:** 2026-09-25

## Context

A chart can run forever inside one macrostep. SCXML allows it: an eventless
transition that is always enabled, or two internal events that raise each
other. Nothing in the spec ends such a macrostep, and nothing should: the
interpreter is doing exactly what the chart says.

The host's budgets (napi ADR 0007: `resumeBudget` microsteps and
`actionBudget` actions per resume) keep such an actor from blocking the
pump: it pauses, other actors run, and it continues next turn. But it
never finishes its macrostep. It never takes its next message, so its
mailbox grows, and it takes a slice of every turn forever. From outside it
looks busy, not broken. At 3am that is the worst kind of fault: nothing
says what is wrong.

Pausing does not answer it. A long `<foreach>` also pauses across many
turns, and it is legitimate. The difference is what the two produce:

- a `<foreach>` runs inside one microstep, however many turns it pauses
  across, and its array is a snapshot, so it always ends;
- an endless loop produces new microsteps without end.

So the measure is **microsteps per macrostep**, not turns or time. Core
already keeps that count: `trace_microstep`, the microstep counter of the
step keys (ADR 0067 section 3), which moves only when a microstep commits
(ADR 0070) and restarts with each macrostep.

## Decision

### 1. Core stops the macrostep at a limit; the host decides what that means

- **The limit is a count of microsteps in one macrostep,** bootstrap
  included. A system has a default, `ta_system_config.microstep_limit`,
  and a definition may override it with
  `ta_definition_set_microstep_limit(system, definition, limit)`: 0 takes
  the system's, `TA_MICROSTEP_LIMIT_NONE` means no limit for that
  definition. Core's own default is 0: no limit. Core sets no policy; the
  host chooses.
- **The check sits between microsteps.** Before the run starts a new step
  of a macrostep that has begun, core compares the committed count with
  the limit. At the limit, `ta_actor_resume` returns **`TA_ERR_RUNAWAY`**,
  with status `TA_RUN_ERROR`. Nothing is open and nothing changed: the
  actor stays mid-macrostep, as after a budget pause, and deliver answers
  `TA_ERR_BUSY`. A later resume continues it.
- **`ta_actor_resume_params.no_microstep_limit`** lifts the limit for one
  call. A host sets it for an actor an operator released (napi, below).
- **It is deterministic.** The count moves only on commit, so a run that
  stopped for lack of memory and was retried (ADR 0070) counts each
  microstep once, and the same chart and events reach the limit at the
  same microstep on every run and every host.
- **It is not an interpreter semantic.** No `error.*` event is raised and
  the chart cannot observe the limit. It is a host policy, like the
  budgets, and a host that sets no limit gets SCXML's behaviour unchanged.
- **The boundary case.** The check runs before the next step is selected,
  so a macrostep that reaches exactly `limit` microsteps and would then
  have ended stable is also stopped. A chart that runs that many
  microsteps in one macrostep is already the case this limit exists for.

Why core and not the host: core has the counter, commits it, and knows
when a macrostep begins. A host reading the counter after each resume
would pay a call per resume, and every host would repeat the rule.

### 2. A warning at a tenth of the limit, and records that say where

- **At a tenth of the limit** (at least 1), when the microstep that
  reaches that count commits, core writes a `TA_TRACE_LONG_MACROSTEP`
  record and counts `macrosteps_long`. The run goes on. A chart that
  legitimately runs long macrosteps shows up here long before it would be
  stopped, so its owner can raise its limit.
- **At the limit,** when the microstep that reaches it commits, core
  writes a `TA_TRACE_RUNAWAY` record and counts `runaways`; the check
  before the next step then stops the run.
- **Both records** carry the macrostep's key, the transitions of the
  microstep that reached the count, in selection order, and the active
  configuration before it, so a sink can say where the actor was looping
  and through which transitions (named through ADR 0071's lookups). They
  are failure records in ADR 0072's sense: a host that turns failures into
  logs wants them even when the macrostep is not sampled
  (`unsampled_kinds`).
- **`ta_counters`** gains `macrosteps_long` and `runaways` (ADR 0072).

### 3. napi (napi ADRs 0007, 0008, 0017)

- **Configuration:** `createSystem({ scheduling: { microstepLimit } })`,
  default **100,000**, 0 for none; `system.define(chart, { microstepLimit
  })` overrides it for one definition, 0 for none. The system default is
  passed to core at creation, and an override when the definition is
  registered. The warning is therefore at 10,000.
  - Why 100,000: an ordinary macrostep runs a handful of microsteps, and
    long eventless chains run hundreds. 100,000 microsteps is well past
    any chart we know of, and at about 1 µs per microstep it is about a
    tenth of a second of one actor's work before it is stopped.
- **On `runaway`:** the existing fault path (napi ADR 0008). The actor is
  quarantined with fault code `runaway`: the fault hook receives
  `{ actor, operation: 'resume', code: 'runaway', message }`, the mailbox
  is kept, `tinyactors.faults` counts it by code and the quarantine gauge
  rises (napi ADR 0017).
- **Logs:** the two records become WARN logs through the encoder:
  `tinyactors.macrostep.long` and `tinyactors.actor.runaway`, with the
  state names and labelled transitions (ADR 0071), under the usual rate
  limit.
- **`release()` lifts the limit for the rest of that macrostep.** The
  operator decided the actor may go on, for example a known long
  computation, so napi resumes it with `no_microstep_limit` until the
  macrostep ends (stable or finished), and the next macrostep has the limit
  again. The alternative, keep counting, would stop it again at once; a
  fresh allowance would need a count per actor. The flag is one bit in the
  scheduling entry's `flags`, which has room: no memory per actor.

## Alternatives

- **Turns per macrostep.** A long `<foreach>` pauses across many turns, so
  it would be stopped although it always ends.
- **Wall-clock time per macrostep.** Operators think in time, but the same
  chart would pass under light load and fail under heavy load, which makes
  tests flaky and replay unreliable.
- **Detect only (a metric and a log, no stop).** The actor keeps burning
  CPU until a person reacts.
- **The limit in the host.** Every host repeats the rule, and each resume
  pays for reading the counter.

## Consequences

- An endless macrostep ends as a quarantined actor with a fault code, a
  WARN log that names the loop, and a metric, instead of a quietly busy
  actor.
- Core's API: `ta_system_config.microstep_limit`,
  `ta_definition_set_microstep_limit`, `TA_MICROSTEP_LIMIT_NONE`,
  `ta_actor_resume_params.no_microstep_limit`, `TA_ERR_RUNAWAY`, two record
  kinds, two counters.
- Cost: one comparison before each step and two in each committed
  microstep; 4 bytes per definition; nothing per actor.

## Testing

- **Core:** an eventless self-loop and an internal-event ping-pong stop
  with `TA_ERR_RUNAWAY` at the limit, with both records and both counters;
  a resume with `no_microstep_limit` continues them; a `<foreach>` over a
  large array that pauses on its action budget across many resumes is not
  stopped; the definition override and `TA_MICROSTEP_LIMIT_NONE`.
- **Retries:** the fault sweep over a looping chart reaches the limit at
  the same microstep as the run without failures.
- **napi, on Node and Bun:** quarantine with code `runaway`, the fault
  hook, the metric and the logs; the per-definition override; `release()`
  lets the macrostep finish and the next one has the limit again.

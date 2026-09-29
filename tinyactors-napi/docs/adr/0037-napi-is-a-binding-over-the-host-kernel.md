# ADR 0037: napi is a binding over the host kernel

**Status:** Proposed (2026-09-30). Applies kernel ADRs 0001 and 0002 and core ADR 0094. Code decided in napi ADRs 0003 to 0008, 0012, 0014, 0016 to 0018, 0020, 0022 and 0032 to 0036 moves to the kernel; those ADRs stay here as the record.
**Date:** 2026-09-30

## Context

napi holds two kinds of C:
- **code that uses Node-API:** `napi_*.c`, `values.c`, `runtime*.c`,
  `addon.c`, and `pump.c`'s event-loop glue;
- **code that doesn't:** scheduling, mailboxes, routing, the directory,
  loads, offloads, effects, the OTLP encoder, and the input log. Only
  `test-native`'s guard keeps it that way (`mise/lib/compile.rb`).

Kernel ADR 0001 moves the second kind into `tinyactors-host`, which the
test host also links. Kernel ADR 0002 moves the input log there and gives
messages an identity. Core ADR 0094 takes the log out of core, and
makes time an argument of every input.

## Decision

### 1. What napi keeps

- **Node-API glue:** the `System` wrapper, services, children, codecs,
  traces, and loads as JavaScript sees them.
- **Its data model:** JavaScript values as `ta_value`s (ADR 0011), named
  functions, registered classes, and the durable codec in `lib/durable.js`
  (ADR 0031's form, ADR 0034). A data model encodes its own values.
- **Its pump driver:** setImmediate for a turn, setTimeout for the next
  deadline, `hrtime` for the clock. It calls the kernel's turn.
- **The generated bindings and `lib/`:** the public API.

Everything else links from `tinyactors-host`. napi's `src/` loses the
moved files, and `NODE_API_SOURCES` and its guard go away: the kernel
cannot include Node-API.

### 2. The public API stays

`createSystem`, `setInputLog`, `setLoader` with `{ replay }`,
`checkpoint`, `evict`, `prepare`, `publish`, `PreparedActor.replay`,
`System.parameters` and the rest keep their shape. What changes:
- **`readInputLog()`** yields kernel ADR 0002's kinds: `accepted`,
  `delivered` and `dropped` in place of `queued` and `event`. An audit
  pairs a delivery with its message by `id`.
- **A logged message's data is written once,** so a batch's bytes shrink
  by the size of each message's data.
- **Nothing else is visible to applications.** Their store still keeps
  opaque bytes per address, and their loader still answers
  `{ replay: { spawn, inputs } }`.

### 3. Time on every input

napi passes the system's time on every `ta_actor_deliver()` and
`ta_actor_report_invoke()` (core ADR 0094 section 1). The kernel reads it
once per turn. Replays pass the logged time, which is the kernel's job.

## Consequences

- **napi becomes a binding in the README's sense:** values, a data model, a
  pump driver and an API, over the kernel.
- **Its native tests move with the code** to the kernel's tests. napi keeps
  the tests of its glue and its JavaScript suites, which now also exercise
  the kernel.
- **Performance must not change.** The code is the same, linked from
  another library. The migration runs `bench-compare` against the last
  commit before it, within 5%, and reports allocations per event.
- **Costs:** none per actor or per event from the move itself. Per logged
  message, one value write fewer (kernel ADR 0002).

## Implementation notes

None yet.

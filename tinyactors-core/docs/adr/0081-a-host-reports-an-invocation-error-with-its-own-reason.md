# ADR 0081: A host reports an invocation error with its own reason

**Status:** Accepted and implemented; see the implementation notes. Replaces `TA_INVOKE_REPORT_UNAVAILABLE` (ADR 0038's report of a lost service) with a general error report. Motivated by napi ADR 0025.
**Date:** 2026-09-26

## Context

A host tells an invoking session what became of an invocation through
`ta_actor_report_invoke()`, which has three kinds of report:

- `EVENT` delivers an event from the service.
- `DONE` delivers `done.invoke.<id>`.
- `UNAVAILABLE` retires the invocation and appends
  `error.communication.invoke.<id>.unavailable` to the internal queue.

napi ADR 0025 needs a fourth outcome. An invoked child that the host
quarantines has not ended, because an operator may release it. The parent
must still learn of the fault, as
`error.communication.invoke.<id>.quarantined`. The invocation stays, so a
later `done.invoke.<id>` or cancellation works as before.

Core knows nothing about quarantine, and must not. The difference between
the two error reports is only the last name token, and whether the
invocation ends.

SCXML 3.12.2 puts error events on the internal queue of the session where
the error is signalled. It allows a platform to extend a defined error
name with a suffix.

## Decision

**`TA_INVOKE_REPORT_UNAVAILABLE` becomes `TA_INVOKE_REPORT_ERROR`.** Its
parameters are:

- `reason`: the last name token, which is text the host chooses.
- `data`: optional, and becomes the event's data.
- `ends`: whether the report also retires the invocation.

Core appends `error.communication.invoke.<id>.<reason>` to the invoking
session's internal queue. The event's type is platform, and it runs under
the report's `trace`. Core counts and records it as raised outside a run
(ADR 0072), and starts a macrostep when the session is idle, as
`UNAVAILABLE` does today.

- **`ends` true** is today's `UNAVAILABLE`. The invocation is retired, and
  the service's later events are ignored (SCXML 6.4.3). A host reports a
  lost service with `reason` `unavailable` and `ends` true.
- **`ends` false** leaves the invocation active. Events from the service,
  `done.invoke.<id>`, autoforwarding and cancellation all behave as if no
  report was made.
- **The session may be mid-macrostep,** for example paused by a budget.
  The event joins the internal queue of that macrostep. A report never
  answers `TA_ERR_BUSY`.
- **`reason` must be one event-name token:** it is not empty and contains
  no `.` or space. Otherwise the report answers `TA_ERR_INVALID` and
  changes nothing.

Reconstruction's own report of a lost invocation (`ta_invoke_report_lost`,
ADR 0038) keeps its name and uses the same code path with `unavailable`.

## Alternatives

- **Add a fifth kind next to `UNAVAILABLE`.** That would give two ways to
  raise an invocation error that differ only in a token and a flag
  (AGENTS.md: one way to do each thing).
- **Deliver the error as an `EVENT` report.** An `EVENT` goes to the
  external queue as the service's own event, and runs `<finalize>`. SCXML
  3.12.2 puts errors on the internal queue, and `<finalize>` is for the
  service's data, not for faults the platform detects.

## Consequences

- napi's `reportInvoke` changes its call site from `UNAVAILABLE` to
  `ERROR` with `unavailable` and `ends` true. Nothing else calls the old
  kind.
- The generated bindings and `tinyactors.api.json` change with the enum
  and the parameter struct.

## Testing

- **Core unit tests:**
  - A report with `ends` false raises the named error in an idle session
    and in a paused session.
  - The invocation stays: a later `DONE` still delivers `done.invoke.<id>`,
    and leaving the state still cancels it.
  - A report with `ends` true behaves as `UNAVAILABLE` did.
  - An empty `reason`, or one with a `.`, answers `TA_ERR_INVALID`.
  - Out of memory while building the name changes nothing (the fault
    sweep).

## Implementation notes

Implemented 2026-09-26.

- **The interface:** `ta_invoke_report_params.as.error` points to a
  `ta_invoke_error { ta_str reason; const ta_value *data; bool ends; }`.
  `data` is optional and retained. The old `as.fault` is gone: core used
  only its `data`, never its `code` or `message`.
- **One path:** `ta_invoke_raise()` in `src/invoke.c` builds the name,
  queues the event, and counts and records it as raised outside a run.
  `ta_invoke_report_lost()` (reconstruction) and an `ERROR` report both
  use it. With `ends`, the invocation is removed only after the event is
  queued, so an out-of-memory report leaves the invocation active and
  queues nothing. Before this change, `UNAVAILABLE` removed the invocation
  first.
- **Never BUSY, verified:** the event goes to the session's internal
  queue, whether the session is idle, paused by a budget, or holding a
  delivered event. Core's queue marks that undo a step's events after an
  allocation fails are local to one call, so a report made between two
  resumes is never taken back. `ta_interpreter_start_macrostep()` does
  nothing for a session already inside a macrostep.
- **Addition: inspection names the parent and the current step.** A host
  that must report for a child needs its invocation. napi spawns SCXML
  children, but it did not keep their parent links, and core had no way to
  read them. `ta_actor_inspection` gains `has_parent` and `parent` (the
  `ta_actor_parent` the child was spawned with), and `trace`: the context
  of the actor's current macrostep and the key of its current step. napi
  runs the report under that `trace` (napi ADR 0025 point 6).
- **Callers migrated:** napi's `reportInvoke("unavailable")` (reason
  `unavailable`, `ends` true), and the test host's `report_invoke`, which
  takes `:error, reason:, ends:, data:`.
- **Tests:** `tinyactors-test/ruby/scenarios/invoke_errors.rb`:
  - `ends` false leaves the invocation active: events, then `DONE`, still
    arrive;
  - leaving the state still cancels;
  - `ends` true retires the invocation;
  - a report into a paused macrostep answers `ok` and is taken in that
    macrostep;
  - an empty reason, a dotted one, and one with a space are
    `err_invalid`;
  - an out-of-memory sweep refuses each allocation the report makes in
    turn, and each refusal changes nothing;
  - inspection names a child's parent, its invocation and its session's
    step.

  The existing unavailable scenarios (`tracing.rb`, `residency.rb`,
  `instruments_selftest.rb`) use the new kind.

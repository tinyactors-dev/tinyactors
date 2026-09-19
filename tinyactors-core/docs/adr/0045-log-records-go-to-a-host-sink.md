# ADR 0045: `<log>` records go to a host sink

**Status:** Accepted  
**Date:** 2026-09-20

## Context

SCXML `<log>` (SCXML 4.7) produces a label and an optional value, and says
the rendering is implementation-dependent, with one hard requirement: a
`<log>` must have no effect on document interpretation.

Core evaluated the expression, for its errors, and discarded the result:
there was nowhere to put it. That left `<log>` half-implemented and made a
chart's own diagnostics invisible to its host.

It also blocked conformance testing. Nine mandatory W3C IRP tests state
their result by logging it; the suite could only skip them as "manual
test: a person judges its log output", because nothing could read that
output.

## Decision

A system may have one log sink, installed with
`ta_system_set_log_sink()`. It is an ordinary `{self, vtable}` host
adapter, like the I/O processors and the external-event router. When a
`<log>` executes and a sink is installed, core writes one `ta_log_record`
to it: the logging actor, the label, and the evaluated value.

**The record carries the value, not text.** `ta_send` already hands a host
adapter the evaluated `ta_value` and lets the host render it with the data
model it owns. Converting to text in core would mean calling `value_text`
on a path where failure has no legal outcome, and would contradict ADR
0006 and ADR 0038, which keep host values host-owned and out of core's
serialization. A host that wants text asks its own data model for it.

**Writing a record cannot fail.** `write` returns `void`. SCXML 4.7
requires `<log>` to have no side-effects on interpretation, so a sink has
no way to fail the action, and core needs no error path for one. Failure
to evaluate the `expr` is different: that is an ordinary execution error,
raised as before, and no record is written for it.

**A sink is optional.** Without one, `<log>` still evaluates its
expression, so a chart's errors do not depend on whether anyone is
listening.

## Consequences

- Hosts can surface chart diagnostics without a side channel.
- The test host records them with a `log.recording` instrument, which
  turns the nine "manual" conformance tests into automated ones.
- The null data model (SCXML B.1) gains its only way to report anything:
  a `<log>` with a label and no expression needs no data model at all,
  which is how a null-model conformance test states its outcome.
- The sink sees every `<log>` of every actor in the system. Per-actor
  routing, filtering and severity are host policy, as with the router.

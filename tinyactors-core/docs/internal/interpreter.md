# `interpreter.h`

The interpreter advances exactly one resident actor.

It must implement:

- eventless transition selection;
- named-event prefix matching;
- condition evaluation and `error.execution` behavior;
- hierarchical transition lookup from every active atomic leaf;
- SCXML transition priority/conflict removal;
- history capture and restoration;
- ordered exits;
- transition executable content;
- ordered entries, initial/default entry, and parallel-region expansion;
- final/completion event generation;
- internal-event FIFO processing;
- invocation lifecycle and autoforward/finalize semantics;
- resumable budgeting without changing semantic ordering.

An operator transition (ADR 0082) is taken through the same engine. A
delivered event that carries one is taken as the first step of its
macrostep, before eventless transitions and before internal events queued
since the delivery, and without `<finalize>` or autoforwarding. No
selection runs: `enabled` holds the mark `TA_ENABLED_OPERATOR`, and the
transition's source and targets are read from the event block's copy while
the microstep begins. From there it is an ordinary microstep: exit set,
history, `<onexit>`, entry set, `<onentry>`, defaults, `done.state`, and
invocations when the macrostep ends. A transition's targets are read as a
`ta_state_list`, which a definition's span and an operator transition both
give.

`ta_interpreter_abandon()` ends a macrostep between two microsteps
(ADR 0082): the waiting internal events and the external event are released
as at the end of a macrostep, `ABANDONED` and `MACROSTEP_END` are written,
and the actor is idle with its stored configuration. An actor holding a
continuation, or one whose bootstrap microstep has not completed, is busy.

The continuation's ordered sets and executable-content frame stack are the
resumption boundary. A budget may stop between semantic operations, but never
inside a host callback or at a point not fully described by that continuation.

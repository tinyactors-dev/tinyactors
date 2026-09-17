# `continuation.h`

The continuation represents a macrostep that has not yet reached stability.

The continuation includes phase, program counter/cursors, the current external
event, candidate and selected transition sets, ordered exit/entry/invocation
sets, an executable-content frame stack, and any normalized external delivery
waiting for router capacity. Foreach frames retain the stable iteration snapshot
and current index. Together these allow safe resumption
within:

- transition selection/conflict resolution;
- ordered state exits;
- executable action blocks and loops;
- ordered state entry/default/history expansion;
- internal-event processing;
- state invocation.

A continuation is part of the session image when an actor is unloaded while
paused. Its arrays and frame-held values are copied and retained by the image.
`TA_CONT_IDLE` means no macrostep is in progress and has no side storage.

The implementation must never report a half-completed microstep as `TA_RUN_STABLE`.

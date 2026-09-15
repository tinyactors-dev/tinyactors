# `continuation.h`

The continuation represents a macrostep that has not yet reached stability.

The current skeleton includes phase, program counter/cursors, and the current external event. The final implementation may require additional definition-specialized scratch to resume safely within:

- transition selection/conflict resolution;
- ordered state exits;
- executable action blocks and loops;
- ordered state entry/default/history expansion;
- internal-event processing;
- state invocation.

A continuation is durable when an actor is unloaded while paused. `TA_CONT_IDLE` means no macrostep is in progress.

The implementation must never report a half-completed microstep as `TA_RUN_STABLE`.

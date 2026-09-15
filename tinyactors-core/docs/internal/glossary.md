# Glossary

**Actor** — one running statechart session.

**Definition** — immutable compiled statechart shared by actors.

**Configuration** — the set of active states. Physically TA generally stores active atomic leaves and derives active ancestors.

**Atomic state** — state with no child states.

**Compound state** — state with children where exactly one child branch is active.

**Parallel state** — state whose child regions are all active simultaneously.

**Microstep** — one selected conflict-free set of transitions plus exits/actions/entries.

**Macrostep** — processing beginning with one external event (or initialization) and continuing through eventless/internal transitions until a stable configuration is reached.

**Stable** — no enabled eventless transition and no internal event awaiting processing for the current macrostep.

**Continuation** — interpreter state required to resume a paused macrostep without changing semantics.

**Internal event** — event owned/queued by the running session and processed before another external event.

**External event** — event supplied by the host between macrosteps.

**Platform event** — event produced by the processor/platform, including `error.*`.

**Delayed send** — fully evaluated send record waiting for its logical due time.

**Invocation** — external or child service associated with a state; completion yields `done.invoke.<id>`.

**Residency** — whether the actor's execution state is currently materialized in RAM. Residency is not actor lifetime.

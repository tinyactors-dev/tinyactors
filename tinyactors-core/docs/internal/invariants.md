# Core invariants

1. **Single macrostep per actor.** An actor cannot process two external events concurrently.
2. **Pause is not stability.** `TA_RUN_PAUSED` does not permit another external event.
3. **Parallel completeness.** If a parallel state is active, each child region has an active descendant according to SCXML entry rules.
4. **Ancestor derivation.** Active atomic leaves imply all active ancestors; generic code must not assume exactly one leaf.
5. **Transition conflict rules.** One event may select multiple non-conflicting transitions across parallel regions; conflicts are resolved according to SCXML priority/document order semantics.
6. **Internal-before-external.** Internal events and eventless transitions complete before a new external event begins.
7. **Error events are queued.** `error.*` generated during execution enters the internal queue; it is not synchronously handled out of order.
8. **Fault consequence is contextual.** A plugin reports failure; the interpreter decides the SCXML-specific consequence for the operation being evaluated.
9. **Delayed-send arguments are frozen.** Event, target, type/data and send identity are evaluated when the send executes, not when it becomes due.
10. **Cancellation ownership.** An actor may cancel only delayed sends raised by the same actor/session.
11. **Unload is invisible.** Unloading changes residency only.
12. **Destroy is terminal.** Destroying removes name/state and cancels actor-owned delayed sends and invocations.
13. **Definition immutability.** Actor execution never mutates the compiled definition.
14. **No hidden wall clock.** Semantic time comes only from the supplied logical clock.
15. **No hidden concurrency.** The core starts no threads and invokes no callbacks concurrently unless the caller itself enters Tinyactors concurrently.
16. **Structured event names.** Hot-path matching never depends on reparsing a formatted event-name string.
17. **Population-independent actor cost.** Creating, replacing, or destroying one actor costs the same whether the system holds a hundred actors or four million: no per-actor operation walks the actor table, and freed slots are found through a free list (ADR 0018, ADR 0028, `memory-model.md`).

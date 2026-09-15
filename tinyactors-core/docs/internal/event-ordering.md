# Event and ordering rules

Within an actor, semantic ordering is SCXML-defined and deterministic given deterministic callbacks/data:

1. eventless transitions are considered at the required stable-point checks;
2. internal events are processed before another external event;
3. internal events are FIFO;
4. errors are internal events and therefore do not jump ahead of already queued internal events;
5. a parallel state's regions are simultaneously active but processed in defined serial/document order for exits/entries/actions as required by SCXML;
6. `done.state.*` is generated when final/completion rules are met;
7. `done.invoke.*` arrives through the invoking actor's external event flow on normal invoked-service completion.

Across actors, the host decides delivery/scheduling order except where causality and an I/O adapter impose additional guarantees.

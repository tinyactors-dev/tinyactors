# `actor.h`

`ta_actor_record` is directory/residency metadata. `ta_actor` is the resident session object.

The record's `id` is a runtime-local handle. Its `self` reference is either a
runtime reference for an ephemeral actor or a named logical address. The core
defines named-address lifecycle through
`ta_actor_directory`; the host supplies the backing index and routing/storage
implementation.

Residency states conceptually include:

- unloaded: logical actor exists, resident state absent;
- idle/stable: resident and ready for a new external event;
- running/paused: macrostep in progress;
- done/terminated: statechart reached terminal state; final removal policy belongs to the host/system API.

A resident actor owns configuration, history, continuation, data-model instance, internal events, and active invocation bookkeeping. Delayed sends may be physically stored in the system timer index but remain semantically part of the actor.

For memory density, future implementations may split uncommon state into sparse side tables rather than carrying vector pointers/capacities in every actor. The current header is intentionally a straightforward skeleton, not the final four-million-actor layout.

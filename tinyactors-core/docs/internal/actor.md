# `actor.h`

`ta_actor_record` is directory/residency metadata. `ta_actor` is the resident session object.

The record's `id` is a runtime-local handle. Its `self` reference is either a
runtime reference for an ephemeral actor or a named logical address. The core
defines named-address lifecycle through
`ta_actor_directory`; the host supplies the backing index and routing/storage
implementation.

Directory entries are tagged. A resident entry contains a runtime actor ID; an
unloaded entry reserves the logical address without pretending a runtime actor
still exists. Directory replacement and removal are compare-and-swap operations
against an expected entry, preventing an old actor from deleting or replacing a
new route.

Residency states conceptually include:

- unloaded: logical actor exists, resident state absent;
- idle/stable: resident and ready for a new external event;
- running/paused: macrostep in progress;
- done/terminated: statechart reached terminal state; final removal policy belongs to the host/system API.

A resident actor owns configuration, history, continuation, data-model instance, internal events, and active invocation bookkeeping. Its pending delayed sends are stored with it, in its sparse state, sorted by due time; the system's deadline index orders owners by their earliest send, not individual sends (ADR 0054). A send the router already refused outlives a destroyed or unloaded sender in a system-owned list that takes part in the index like an owner.

Each semantic mutation advances the actor's session epoch. A captured image
records that epoch. `ta_actor_unload()` accepts only a matching image, so a host
cannot publish an older checkpoint after the actor processes another event.

For memory density, uncommon state lives in sparse side storage, while
configuration and history use definition-sized storage. An unloaded actor has
no resident `ta_actor`; only the host directory/checkpoint record remains.

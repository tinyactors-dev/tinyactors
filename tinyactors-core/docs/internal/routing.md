# `routing.h`

The external-event router is the host scheduling boundary for fully normalized
events addressed to SCXML sessions. It is intentionally not a generic byte,
URI, socket, or message-broker transport.

Mail to a session resident in this system never reaches the router: core takes
an envelope from its pool and queues it itself, and a macrostep's mail joins
the run queue when that macrostep ends (`run.h`, ADR 0099). Core submits
`ta_external_delivery` values from the built-in SCXML Event I/O Processor and
from the standard parent/child paths only for a target that is not resident:
a session that is gone, unloaded, or held by another family, or an address
with no resident actor. The destination is already a runtime or named
`ta_actor_ref`; the event already has its SCXML name, type, origin, send ID,
invocation metadata, and retained data.

The callback may deliver synchronously when reentrancy and actor-busy rules
permit, but most hosts enqueue or marshal the delivery. Callback arguments are
borrowed for the call. A router retaining work must retain the delivery before
returning `TA_EXTERNAL_EVENT_ACCEPTED`. `ta_external_delivery_retain()` and
`ta_external_delivery_release()` implement that ownership: a named destination
and a named origin are interned address handles (ADR 0065), so retaining
increments their reference counts and the data's, allocates nothing and cannot
fail.

Typical implementations are:

- a fixed scheduler ring on embedded systems;
- a deterministic queue in a simulator;
- a JavaScript task or language-runtime thread handoff;
- a shard queue that loads `UNLOADED` actors;
- a durable remote inbox in a distributed host.

`TA_EXTERNAL_EVENT_RETRY` means the router accepted nothing. The send waits in
the deadline index, due now, submitted, and is offered again without
re-evaluating expressions. A pending invoked-child
completion likewise remains attached to the child. `TA_EXTERNAL_EVENT_ERROR`
with a fault causes an ordinary send to generate the required
`error.communication` event. Queue capacity, retries, deduplication, loading,
and physical transport remain host policy.

`ta_actor_deliver()` remains the low-level resident-only operation below this
boundary. Custom I/O processors and invokers do not pass arbitrary protocol work
through the external-event router.

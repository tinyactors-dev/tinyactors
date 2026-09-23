# `routing.h`

The external-event router is the host scheduling boundary for fully normalized
events addressed to SCXML sessions. It is intentionally not a generic byte,
URI, socket, or message-broker transport.

Core submits `ta_external_delivery` values from the built-in SCXML Event I/O
Processor and from other standard parent/child completion paths. The destination
is already a runtime or named `ta_actor_ref`; the event already has its SCXML
name, type, origin, send ID, invocation metadata, and retained data.

The callback may deliver synchronously when reentrancy and actor-busy rules
permit, but most hosts enqueue or marshal the delivery. Callback arguments are
borrowed for the call. A router retaining work must copy the actor reference and
retain the event before returning `TA_EXTERNAL_EVENT_ACCEPTED`.
`ta_external_delivery_clone()` and `ta_external_delivery_release()` implement
that ownership operation, including deep-copying a named logical address.

Typical implementations are:

- a fixed scheduler ring on embedded systems;
- a deterministic queue in a simulator;
- a JavaScript task or language-runtime thread handoff;
- a shard queue that loads `UNLOADED` actors;
- a durable remote inbox in a distributed host.

`TA_EXTERNAL_EVENT_RETRY` means the router accepted nothing. An executable
`<send>` retains the normalized delivery in its continuation and pauses; a later
resume retries without re-evaluating expressions. A pending invoked-child
completion likewise remains attached to the child. `TA_EXTERNAL_EVENT_ERROR`
with a fault causes an ordinary send to generate the required
`error.communication` event. Queue capacity, retries, deduplication, loading,
and physical transport remain host policy.

`ta_actor_deliver()` remains the low-level resident-only operation below this
boundary. Custom I/O processors and invokers do not pass arbitrary protocol work
through the external-event router.

# API gap closure

The public skeleton now represents every accepted API decision needed for
virtual actors. This page records how the former gaps were closed so later
implementation work does not reopen the semantic questions.

## Registration and definition construction

`include/tinyactors.h` exposes the complete embedding path:

1. construct a declarative `ta_scxml_spec`, directly or with `ta_tree`;
2. compile it with `ta_definition_compile_spec()`;
3. register the immutable result with `ta_system_register_definition()`;
4. register I/O processors and invokers by interned type atom; and
5. install the host-backed actor directory.

Compilation uses `ta_definition_compile_params`, keeping the ABI within the
three-argument rule. A compiled definition is destroyed with
`ta_definition_destroy()` if ownership has not been transferred to a system.
Successful registration transfers ownership to the system; definitions remain
alive until system destruction, so resident actors cannot outlive their IR. A
session image owns only copied definition identity and definition-local state;
it may outlive that system and later reconstruct against an exact registered
revision elsewhere.

## Session images

The private `ta_session_image` layout covers logical self identity, exact
definition identity, configuration storage, history storage, continuation and
current external event, queued internal events, active logical invocations,
resident delayed sends, invoked-child linkage, and terminal completion data.

Capture retains every `ta_value` held by those structures. It never walks or
serializes a host value or the data-model binding. Reconstruction requires an
exact family-and-revision match. It restores queued state in order, retires
captured invocations, and appends one
`error.communication.invoke.<id>.unavailable` event per lost invocation.

## Named actor replacement

Replacement is explicitly two phase:

- `ta_actor_prepare_replacement()` creates an unpublished actor with the
  incumbent's logical self-address. Its bootstrap may be resumed and inspected,
  but it cannot receive routed external events. Outbound I/O, invocation, and
  delayed-send publication is held behind its activation barrier.
- `ta_actor_activate_replacement()` first atomically swaps the directory entry,
  then releases the prepared actor's materialized effects in SCXML order. It
  succeeds only for a stable prepared actor and the matching incumbent.

Failure before the swap leaves the incumbent published and the replacement may
be destroyed. Failure after the swap is ordinary statechart-visible
communication failure; the directory never points at a partially bootstrapped
actor. Activation does not destroy the incumbent.

## Completion and destruction

Entering a top-level final state ends statechart processing and produces
`TA_RUN_DONE`; it does not reclaim the actor. The final state's SCXML
`<donedata>` is retained and cloned for the host by
`ta_actor_get_completion()`.

An actor spawned with `ta_actor_spawn_params.parent` is an invoked child. On
normal top-level completion it submits `done.invoke.<id>` with the final data
through the parent's external-event path. If the parent is busy, the child
retains the pending completion and the host retries with
`ta_actor_publish_completion()`. A child with pending completion cannot be
destroyed while the parent invocation is active. If the parent has already
cancelled or retired that invocation, publication returns `TA_ERR_NOT_FOUND`,
retires the pending completion, and permits reclamation. Cancellation or
explicit destruction never manufactures a normal completion event.

`ta_actor_destroy()` is therefore a distinct, explicit termination operation:
it cancels active invocations and delayed sends, removes a published name, and
releases resident state. A completed standalone actor stays inspectable until
the host reads its result and destroys it.

## Timer dispatch

`ta_system_next_deadline()` lets a host include resident delayed sends in its
own scheduler. `TA_ERR_NOT_FOUND` means no resident deadline exists.

After advancing logical time with `ta_system_set_time()`, the host calls
`ta_system_dispatch_due()` with a finite budget. Tinyactors dispatches eligible
sends in deadline order through the already-resolved I/O processor, reports the
count, and tells the host whether more work is immediately due. Dispatch errors
become SCXML communication-error events for the owner. No API call sleeps,
reads a physical clock, or runs an event loop. Durable deadline indexes for
non-resident actors remain outside core.

## Resident layout

The common `ta_actor` record now contains only identity, immutable-definition
reference, compact definition-sized session storage, small interpreter handles,
the data-model instance, and publication state. Configuration and history bytes
share one definition-sized allocation. Internal queues, invocation vectors,
completion/parent state, and replacement activation effects live in the
optional `ta_actor_sparse_state`. Timers remain in the system deadline index.

This is the intended density boundary: the immutable definition determines
compact configuration/history storage, while actors that never invoke, queue,
complete as children, or undergo replacement pay no per-feature vector cost.

## Virtual-actor residency

Named residency is an explicit public protocol. Directory entries distinguish
`RESIDENT(actor_id)` from `UNLOADED`; replacement and removal compare the
expected entry atomically. A host captures and durably stages an image, then
calls freshness-checked `ta_actor_unload()` to release resident state without
terminating the logical actor. `ta_actor_load()` reconstructs off-route and
publishes a new runtime ID only after success. `ta_actor_find()` reports
`TA_ERR_UNLOADED` so routing can trigger this load path.

The session image exposes only its next deadline. Portable encoding, binding
resolution, checkpoint transactions, and the non-resident deadline index stay
in the optional durability layer, preserving the core boundary.

## Standard inter-session delivery

Core provides and reserves the mandatory SCXML Event I/O Processor rather than
requiring each embedder to register an actor-router processor. The data-model
adapter decodes opaque target values into structured SCXML targets; core owns
special-target and event-envelope semantics. External deliveries, including
normal invoked-child completion, pass through one host-installed
`ta_external_event_router`. The router owns scheduling, queueing, residency, and
physical routing, but it never reinterprets SCXML semantics.

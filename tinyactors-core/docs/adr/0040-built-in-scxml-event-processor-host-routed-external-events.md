# ADR 0040: Built-in SCXML event processor; host-routed external events

**Status:** Superseded in part by ADR 0099 (2026-10-04, implementation pending): mail between local sessions stays in core, and the external-event router goes; core remains the SCXML Event I/O Processor. Accepted.  
**Date:** 2026-09-17

## Context

SCXML requires processors to support the SCXML Event I/O Processor for
communication between SCXML sessions. The standard defines target forms and
event-envelope mappings while deliberately leaving physical transport
platform-specific.

Treating actor delivery as an ordinary host-registered I/O processor would make
each embedder reimplement mandatory semantics such as `#_internal`, external
self-send, `#_parent`, `#_<invokeid>`, `#_scxml_<sessionid>`, origin, send ID,
data copying, `_ioprocessors`, and `error.communication`. Different embedders
would cease to be the same SCXML processor.

Core also deliberately has no per-actor external mailbox, thread, or event
loop. A normalized external event may need a fixed embedded queue, language
runtime handoff, shard scheduler, unloaded-actor wake-up, or durable remote
inbox. Those policies cannot be built into the processor.

## Decision

Tinyactors core provides the SCXML Event I/O Processor. Its standard URI is
interned when a system is created, returned by `ta_system_scxml_io_type()`, and
reserved from replacement through generic I/O registration. Omitted `type` and
`typeexpr` select it.

Core owns the target URI grammar and parses both spellings of a target
(ADR 0044, which replaces this paragraph's original rule that the data-model
adapter decoded an evaluated target). A literal target is the definition's
text; an evaluated one is the text of its value, which the adapter supplies
through `value_text`. Core then implements the target and envelope rules. An
omitted target is external self and needs no text; `#_internal` is appended
directly to the actor's internal queue.

Every external SCXML-session event becomes a normalized
`ta_external_delivery` containing a runtime or named destination and a complete
`ta_event`. Core submits it through one host-provided
`ta_external_event_router`, installed per system. Normal invoked-child
completion and other core-owned parent/child external-event paths use the same
router.

The router is intentionally actor-specific rather than a generic transport. It
may enqueue, marshal to another runtime thread, select a shard, load an unloaded
actor, or publish to a durable remote inbox. It ultimately schedules
`ta_actor_deliver()` for a resident actor. Callback arguments are borrowed; a
router retaining a delivery clones the event and copies its destination before
returning success. Core exposes clone/release helpers that deep-copy named
addresses as well as event data.

The router returns accepted, retry, or error. Retry transfers no ownership and
does not create a statechart error: executable content retains the materialized
delivery in its continuation and pauses, while invoked-child completion remains
pending on the child. Error carries a fault and becomes the appropriate
statechart-visible communication failure.

Custom I/O processors remain the extension point for non-SCXML destinations
such as HTTP, client streams, application storage, and brokers. Invokers remain
the lifecycle boundary for LLM streams, tools, and other cancellable services.

## Consequences

All embedders share one implementation of mandatory SCXML communication
semantics. Host implementations reuse a small addressed-event scheduling
contract across embedded, browser, language-runtime, server, and distributed
environments without forcing arbitrary protocols through it.

The router owns external queueing and residency races. In particular, it can
atomically observe an `UNLOADED` directory entry, retain the event by logical
address, load the actor, and deliver using its new runtime ID. Core still does
not allocate an unbounded mailbox or create an event loop.

An absent router, or one returning `TA_EXTERNAL_EVENT_ERROR`, makes an external
target inaccessible and causes the sending interpreter to generate
`error.communication`. Internal sends do not require the router.

# `io.h`

I/O processors are selected by a type atom/identifier when a send is ready to
dispatch. `ta_send.io_type` is the resolved SCXML `<send>` `type` or `typeexpr`
value. The `target`, `event`, and `data` fields are delivery arguments, not the
processor selector.

## Built-in SCXML Event I/O Processor

Tinyactors core owns the mandatory SCXML Event I/O Processor. It interns
`http://www.w3.org/TR/scxml/#SCXMLEventProcessor` during system creation;
`ta_system_scxml_io_type()` returns that atom. The atom is reserved and cannot
be replaced through `ta_system_register_io()`.

When SCXML omits `type` and `typeexpr`, Tinyactors selects this processor. It
implements the standard event-envelope mappings, `_ioprocessors` location,
default external self-send, `#_internal`, `#_parent`, `#_<invokeid>`, and
`#_scxml_<sessionid>` semantics. Inaccessible targets produce
`error.communication` in the sender.

A session ID is the runtime ID the session was spawned with, kept across
unload and load (ADR 0066). When a send is submitted, a runtime destination
that is not a resident actor's runtime ID is looked up as a session ID: a
reloaded session gives its current runtime ID, an unloaded named session its
address. A send with no target names the sending session the same way.

Core owns the target URI grammar in both directions (ADR 0044):
`ta_scxml_target_parse()` reads a target and `ta_scxml_target_format()` writes
one, so the URI a chart reads from `$_event.origin` parses back to its sender.
A literal target is the definition's text; an evaluated target stays a host
`ta_value`, and core asks the data model for its text with `value_text()` and
parses that. An omitted target is external self and needs no text. A named
address is interned (ADR 0065): the send record holds a reference to the
system's handle for it, not the text, and a router that keeps a delivery past
the call retains it with `ta_external_delivery_retain()`, which copies no text
and cannot fail.

`#_internal` is appended directly to the sender's internal queue. Every external
SCXML-session delivery becomes a normalized `ta_external_delivery` and is
submitted to the system's `ta_external_event_router`. The router decides when
and where to call `ta_actor_deliver()`, including queueing, thread marshalling,
shard routing, and loading an unloaded named actor. It does not reinterpret
SCXML target syntax or event metadata.

Normal invoked-child completion and SCXML parent/child external messages use
the same router boundary. This gives all core-generated external events one
scheduling and residency policy without creating a core mailbox or event loop.

A delayed send copies the resolved I/O type atom, so it dispatches through the
same built-in or custom processor after its delay, unload/reload, or persistence
round trip.

## Custom processors

Other processors are registered by the host and called synchronously:

The callback may:

- perform the operation synchronously;
- enqueue work into an event loop/RTOS queue;
- start asynchronous host work and return;
- record the send in a verifier/simulator;
- synchronously deliver to another actor if the embedder's scheduling policy permits it without violating actor reentrancy rules.

The callback returns `TA_OP_ERROR` plus `ta_fault` when dispatch fails. The interpreter converts this to a queued `error.communication...` event.

Custom processors serve non-SCXML destinations such as HTTP, client streams,
transcript stores, and brokers. They do not replace the standard inter-session
processor. `ta_send.source` still records the actor reference needed for replies
without putting routing information into application data.

Delayed-send waiting/cancellation is not an I/O processor responsibility.

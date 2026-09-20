# Values, data models, I/O, and invocation

Tinyactors deliberately permits rich host integration.

## Values

`ta_value` is a fat pointer: host object pointer plus vtable. A binding can wrap `PyObject *`, Ruby `VALUE`, N-API references, native C objects, or compact embedded values. The wrapper owns the host-specific lifetime, reference counting, cloning, and serialization rules.

## Data models

A `ta_datamodel` interprets conditions, value expressions, locations,
assignments, scripts, and the host representation of SCXML send targets. Target
decoding produces a structured internal/self/parent/invoke/actor target; core
then applies the standard semantics. The core does not impose an expression
language. Data models may perform arbitrary host work; disciplined embedders
should route externally visible effects through I/O processors when they want
replayable and testable systems.

## I/O processors

Core supplies the mandatory SCXML Event I/O Processor. External session events
from it go to the host's `ta_external_event_router`, which schedules resident
delivery or loads a virtual actor. Embedders do not register their own actor
router as an I/O processor.

A custom I/O processor callback is synchronous as a C call, but the operation
it initiates may be asynchronous. For example, a Node callback may start
`fetch()` and return; the promise completion later delivers another event to the
actor.

### Platform-specific executable content

SCXML 4.10 lets an implementation add executable content of its own, in a
foreign namespace. Tinyactors has no such action: a custom action is a
`<send>` to one of your I/O processors (ADR 0046), which is the lowering
SCXML's appendix G.7 describes and what implementations do in practice.

```text
<ccxml:accept connectionid="_event.data.connectionid"/>
```

becomes

```text
<send type="urn:example:telephony" event="accept" idlocation="requestid">
  <param name="connectionid" expr="_event.data.connectionid"/>
</send>
```

The element's namespace is the processor type you registered, its local name
is the event, and its attributes are `<param>`s the data model evaluates.
Report the outcome as an ordinary event: `<vocabulary>.<action>.done` on
success and `error.platform.<vocabulary>.<action>` on failure, which keeps a
vocabulary's errors inside the namespace ADR 0027 reserves for them, so a
chart can catch them with the plain `error` descriptor. Correlate a reply
with a token the chart passes as a `<param>`, or with the send's own ID:
`idlocation` gives the chart its text, your processor holds the same ID as
`ta_send.send_id`, and `ta_send_id_format()` renders one into the other
(ADR 0048).

Delayed-send timing belongs to Tinyactors statechart semantics. The I/O processor receives a fully materialized send only when it is due. The send's `io_type` is the resolved SCXML `<send>` type and selects the registered processor; `target`, event, and data are then interpreted by that processor.

## Invoked services

Invokers start, cancel, and forward events to invoked services. A native SCXML/statechart invoker may map invocation directly to spawning a child actor. Other invokers may wrap host services. Completion is represented by structured `done.invoke.<id>` events.

Invoker service handles are valid only in the runtime that created them and are
not part of a session image. If a session is reconstructed while an invocation
was active, Tinyactors does not restart or silently reconnect that service. It
queues `error.communication.invoke.<id>.unavailable`,
allowing the chart to define its recovery policy.

After `start()` returns, an invoker uses `ta_actor_report_invoke()` to report an
ordinary event, normal completion, or that the service is unavailable. Network
failure and reconstruction-discovered loss produce the same chart-visible
unavailable event.

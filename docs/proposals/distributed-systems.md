# Distributed actor systems

**Status:** Sketch (2026-09-24)
**Touches:** tinyactors-core (conformance gaps), tinyactors-napi and the future host kernel, the future browser build
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.
**Background:** [research on Orleans, Akka and Erlang/Elixir](../research/actor-runtime-distribution-hindsight.md)

## Goal

Actor systems that talk to each other across boundaries:
- **browser and server:** a system in the browser (the future WebAssembly
  build) and one on the server (napi);
- **main thread and worker threads:** one system per JavaScript environment,
  as napi ADR 0001 already requires;
- **one system per tenant** (thousands), driven by an **admin system**.

## Principles, from the research

1. **Remote must not look local.** Crossing a boundary is visible in the
   chart, or at least in the outcome of a send. The outcome is delivered,
   unreachable, or link down, never silent.
2. **Identity does not encode location.** A named address stays
   `(family, name)`. A tenant is a namespace carried in the name
   (`tenant42/cart-123`), as Orleans carries it in keys, not a node inside
   the address. An earlier idea to add a node to core's addresses is
   withdrawn.
3. **Mechanism is shared, and policy is pluggable.** Transports, envelopes,
   codecs and outcomes are shared code. Placement, directories, membership
   and topology belong to the application.
4. **At-most-once by default.** Stronger guarantees are an opt-in layer.
5. **Control traffic is kept apart from data traffic.**
6. **The wire format is explicit and versioned from the first release.**
7. **No supervision across boundaries.** Invoking a *child chart* in
   another system (with `#_parent` and `done.invoke` across the link) is out
   of scope. Invoking a *local connection* is fine (see style 2).

## How a chart crosses a boundary

Statecharts already have the parts distribution needs: I/O processors for
sending outside the session, invocations for services with a lifetime, and
events with an origin to reply to. There are three styles, from the most
explicit to the most implicit.

### Style 1: an explicit send to an I/O processor

The chart names the transport and the remote address:

```xml
<send type="ta.link" target="ta://tenant42/cart/cart-123" event="cart.add">
  <param name="sku" expr="sku"/>
</send>
```

- **This is standard SCXML.** `type` selects a processor and `target` is
  its URI, as with SCXML's Basic HTTP processor. Core already supports it
  (core ADRs 0040 and 0046).
- **The boundary is visible in the chart.** That is principle 1, at the
  source.
- **Options per send** travel as `<param>`s: a delivery guarantee, a
  timeout, or a request for an acknowledgement (answered as an event such
  as `ack.<sendid>`).
- **Failures are events.** A processor that refuses a send gives
  `error.execution` or `error.communication` at once. An asynchronous
  failure gives `error.communication` later, as napi step 13 already does.
- **Replies should use the SCXML idiom:**
  `<send typeexpr="_event.origintype" targetexpr="_event.origin">`. SCXML
  §5.10.1 says that for external events the processor SHOULD set `origin`
  and `origintype` to values that allow exactly this reply.
  **Gap G1:** an event that entered through an I/O processor has an opaque
  `uint64` origin, so `_event.origin` and `_event.origintype` are empty. Core
  renders them only for actor and invocation origins (`src/datamodel.c`).
  The fix is an I/O origin that carries the processor's type and a URI
  value.
- **`_ioprocessors` must list every processor.** SCXML §5.10 says the
  processor MUST bind `_ioprocessors` to one entry per Event I/O Processor
  it supports. **Gap G2:** core exposes only the SCXML processor's type and
  location (`TA_DM_TEXT_SCXML_IO_*`), and napi's `ioProcessors` lists only
  that entry, so processors registered with `registerIo` are missing. A
  processor needs a way to report its `location` for a session, the address
  others use to reach this session through it.

This is the style to recommend. It matches the research and works the
same in every host. Sending works today; replies need G1, and a session's
own address per processor needs G2.

### Style 2: a connection owned by a state (invoke)

A state owns a local connection for as long as the state is active:

```xml
<state id="online">
  <invoke id="conn" type="ws" src="wss://example/session">
    <finalize> <!-- check or transform inbound frames before they are processed --> </finalize>
  </invoke>
  <transition event="chat.say">
    <send type="ws" target="#_conn" event="say"><param name="text" expr="_event.data.text"/></send>
  </transition>
  <transition event="done.invoke.conn" target="reconnecting"/>
  <transition event="error.communication.invoke.conn.unavailable" target="reconnecting"/>
</state>
<state id="reconnecting">
  <onentry><send event="retry" delay="2s"/></onentry>
  <transition event="retry" target="online"/>
</state>
```

What the statechart gives for free:
- **Lifecycle.** Entering the state connects and leaving it closes the
  connection. `<invoke>` cancellation already does this.
- **Inbound frames are events** from the invocation, with
  `_event.invokeid = "conn"` and `_event.origin = "#_conn"`.
- **Sending to the service follows the spec.** SCXML §6.4 says a chart
  sends ad hoc messages to an invoked process with `<send>` whose `type`
  is the same value as the `<invoke>`'s `type`, and whose target is
  `#_<invokeid>`. So the invoke type `ws` is also an I/O processor type
  `ws`. The binding registers an invoker and a processor under the same
  type, and the processor maps `(sender, "#_conn")` to the service the
  invoker started. The SCXML processor's own `#_<invokeid>` target
  applies only to SCXML sessions created by `<invoke>` (C.1), which is
  what core does today, so core is correct there.
  Whether core passes `#_conn` to a custom processor unparsed needs a test.
- **Checking inbound traffic is in the chart:** `<finalize>` can validate
  or transform each frame before it is processed. That is where the
  server's "don't trust the browser" rules can live.
- **Reconnects, backoff and timeouts** are a few states and delayed sends.
  They are visible in `inspect()` and testable with the manual clock.
- **Persistence works:** after a load or a summon, the invocation is
  reported unavailable (core ADR 0038), and the chart reconnects.

**Gap G3:** for an event from an invoked service, core sets
`_event.origin` to `#_<invokeid>` but `_event.origintype` to the SCXML
processor's type, whatever the invocation's type was. For a non-SCXML
service the reply idiom then picks the SCXML processor, whose
`#_<invokeid>` target reaches only SCXML sessions, so the reply fails.
Per §5.10.1 (SHOULD), `origintype` should be the invocation's type (`ws`),
so that `<send typeexpr="_event.origintype" targetexpr="_event.origin">`
reaches the connection's processor.

**The link actor pattern.** One named actor per peer owns the connection,
and other actors send to it with ordinary local sends. Everything a link
needs is then chart logic, readable and replaceable per deployment:
- sequence numbers;
- an outbox in the actor's data, with acknowledgements and resend timers
  (at-least-once as an opt-in chart, not a runtime feature);
- flow control: the link actor's mailbox limit plus the router's `RETRY`
  pauses senders when the link is backed up;
- rules for inbound traffic.

It costs one extra local hop per message (about 0.3–1.3 µs in napi) and
encoding in the data model.

A server does not start a connection; it accepts one. There, the host
spawns the session actor with the accepted connection passed in its
parameters, and the actor invokes it (for example `type="ws.accepted"`), so
the lifecycle stays in the chart.

### Style 3: transparent names through a resolver (convenience)

A chart sends to a plain name (`#_actor_cart/tenant42/cart-123`), and the
host decides that the name lives elsewhere. This is a convenience built on
styles 1 and 2, not a separate mechanism:
- the host kernel's router asks a **resolver** hook:
  `resolve(address) → local | link | unknown`;
- a remote answer rewrites the send into style 1 (a processor send) or into
  a local send to a link actor (style 2);
- the outcome must still be explicit: unknown links and down links become
  `error.communication` to the sender. An unknown actor on a reachable
  system becomes a dead letter on the receiving side, as it does locally.

The resolver's *contents* are the application's policy: which families or
name prefixes live on which link. Examples:
- family `session-ui` → the browser link;
- a worker's families → its `MessagePort`;
- the name prefix `tenant42/` → the tenant42 link.

## Who owns what

| Layer | Owns |
|---|---|
| Core | nothing new beyond the conformance gaps G1–G4, which are SCXML behaviours core should have anyway. No membership, directory or placement. |
| Shared host kernel (C) | the resolver hook in the router; a versioned envelope and value-codec contract; explicit outcomes; at-most-once by default; the ingress path that posts remote events with their origin. |
| Binding | transports: I/O processors (style 1) and connection invokers (style 2) for its environment. For example a websocket in Node, `MessagePort` in browsers and workers, asyncio sockets in Python. It also converts its language's values for the codec. |
| Application | the resolver's contents, placement, membership, topology, and the link charts (style 2). |

## The three setups

| | Browser + server | Main thread + worker | Tenant systems + admin |
|---|---|---|---|
| Natural style | 2: the browser session chart owns the websocket; the server session actor owns the accepted connection | 1 or 3: `MessagePort` links, routed by family | 1: the admin sends `ta://tenant42/…` explicitly. Tenants reply with the origin idiom (needs G1). |
| Values | encoded (JSON, CBOR or `v8.serialize`) | structured clone | in one process: re-boxed, not encoded, because `napi_ref` belongs to the environment; across processes: encoded |
| Trust | the server does not trust the browser; checks in `<finalize>` or at ingress | trusted | the admin is trusted; each tenant is checked at ingress |
| Failure | reconnect states in the chart | the worker terminates → `error.communication` | a tenant is closed or crashes |
| Tenant lifecycle | — | — | the admin creates and closes systems through an I/O processor (for example `tenant.create`), because a chart cannot create systems |

A system per tenant gives isolation, per-tenant backpressure
(`maxQueuedMessages`), per-tenant persistence (see
[replay persistence](replay-persistence.md), layout 3 per tenant),
per-tenant chart revisions, and clean deletion. The fixed cost of one
system, multiplied by thousands, has not been measured yet.

## Semantics to fix once

- **Delivery:** at-most-once by default. At-least-once is opt-in, either as
  a link chart (style 2) or as a processor option. It shares the outbox and
  deduplication (by origin and send ID) with replay persistence.
- **Order:** FIFO per link while the link is up. Promising more across
  reconnects needs sequence numbers.
- **Copying: the spec requires it, and napi does not do it yet.** SCXML C.1
  says the `data` of an event sent with the SCXML processor MUST be a copy:
  changes in the receiving session must not affect the sending session, or
  the reverse. Core shares values by retain (core ADR 0050), and napi's
  JavaScript data model shares objects by reference (napi ADR 0011). The
  test host's ECMAScript data model records the same aliasing as a known
  limitation. So within one system, mutable JavaScript payloads are shared
  today, against a MUST (**gap G4**). Across a boundary, encoding copies
  naturally.
- **`send()` promises across a link:** completion mode needs the remote
  side to report back, so the link protocol needs completion messages.
- **Large messages:** fragment them, and keep control messages (acks,
  heartbeats) on a separate channel or with priority.

## Conformance gaps (checked against the SCXML spec, 2026-09-24)

These are gaps between the implementation and the W3C SCXML
Recommendation, not new features. Distribution makes them visible.
G1–G3 are addressed by core
[ADR 0068](../../tinyactors-core/docs/adr/0068-core-owns-the-io-processor-registry-and-every-origin-can-be-answered.md)
(Proposed). Core owns the processor registry with stable indices. An I/O
origin is a processor index plus one of three storage classes that the
processor declares:
- **computed:** a token rendered by a pure callback, for peers and
  formula-based URIs;
- **scoped:** a URI stored once per connection or session, where a stale
  scope renders empty;
- **per message:** a reference-counted copy freed with the last event copy.

Invoke origins carry the invoker's index, so `origintype` names the right
processor. `ta_event` stays 64 bytes. Locations are rendered on demand into
the interpreter's scratch arena, and images store origin text, which comes
back at load as per-message origins.

| Gap | Spec | Level | Needed by | Change |
|---|---|---|---|---|
| G1: origin of an event from an I/O processor | §5.10.1 | SHOULD | style 1 replies, style 3 ingress | an I/O origin with the processor's type and a URI value, rendered as `_event.origin` and `_event.origintype` |
| G2: `_ioprocessors` lists only the SCXML processor | §5.10 | MUST | style 1 (a session's own address per processor) | core lists every registered processor; a processor reports its `location` for a session; napi's `ioProcessors` shows them |
| G3: `origintype` of events from a non-SCXML invocation | §5.10.1, §6.4 | SHOULD | style 2 replies | `origintype` is the invocation's type, not the SCXML processor's |
| G4: event data is shared, not copied | C.1 | MUST | every setup, not only distribution | a data-model operation that core calls when a value leaves a session (send data to another session, done data to a parent, invoke parameters to a child). The JavaScript data model copies there (for example with `structuredClone`) or freezes. The cost per message with data needs measuring. |

Checked and conforming: the SCXML processor's `#_<invokeid>` target (C.1,
SCXML sessions only), and `origintype` for SCXML sends. The C.1 prose says
`"scxml"`, IRP test 253 accepts either `"scxml"` or the full URI, and core
writes the URI and accepts `scxml` as an alias.

## Open questions

- The envelope format and codec: JSON, CBOR or `v8.serialize`, and how
  versions are negotiated.
- Whether style 3 exists at all, or whether styles 1 and 2 are enough.
- The fixed cost of one system, and whether tenants share a process.
- How the admin enumerates and inspects tenant systems (an I/O processor
  that answers with pages, reusing core ADR 0062's cursor).
- Handing a whole actor to another system (network handoff) as an explicit
  processor send of a captured image. It needs the value codec.

# ADR 0014: Other systems are reached through an I/O processor the application serves

**Status:** Accepted (2026-10-15). Supersedes the router of
[ADR 0004](0004-replace-the-core-with-the-standalone-runtime.md) (peers,
envelopes, routes, relaying, export rules, dedup, queues) and
[ADR 0009](0009-one-host-two-queues.md) decision 5's envelope form.
**Date:** 2026-10-15

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the application developer who spreads systems over threads,
  processes and a browser.** How those parts talk (threads and a queue, a
  WebSocket, HTTP, a message bus) is a deployment decision, which
  tinyactors does not make. Today core makes it: the host's router owns
  peers, connection state, routes, relaying, trust, deduplication and an
  envelope format, and a review (session record of 2026-10-15) found that
  it fails in each of those (a restarted host's sends dropped as
  duplicates, forged `error.communication` from untrusted peers, policy
  and queues lost on a reconnect, a PEER effect that cannot be failed, an
  unversioned format) and that nothing outside tests and the conformance
  tools calls it.
- **Then: the binding author and the C embedder.** One mechanism for
  everything that leaves the system: a `TA_EFFECT_SEND` to an I/O
  processor, answered or failed, and inputs with `ta_host_send`.
- **Out of scope:** any transport, framing, value encoding, delivery
  guarantee, identity across restarts, trust between hosts and multi-hop
  routing: they are the application's. System-id uniqueness across a
  deployment is the application's too.
- **Who pays:**

  | Cost | What |
  |---|---|
  | per event inside a host | less: a send to a session of another segment is delivered as a struct, without the router's message ids, dedup key or envelope |
  | per event to another system | one `TA_EFFECT_SEND` (as for any I/O processor); the origin's text only when the handler asks for it |
  | memory | the router's state goes (peers, queues, the 32 KiB dedup window); one origin slot per effect of a batch, when asked |
  | API | 19 router functions, 7 structs, the `PEER`/`PEER_ERROR` effects and their fields, and the datamodel's `from_wire` go; `ioProcessors[].remote`, `TA_EFFECT_FIELD_ORIGIN` and `ta_host_resolve` come |
  | every binding | peer and envelope calls go; an I/O processor handler and `send` by address replace them |

## Decision

1. **Core decides one thing about an address: local or not.** A send
   whose target is a `tinyactors://` address (`<send>` with the default,
   SCXML type) is local when its system id is the host's: the host
   delivers it to the session (or the `@name`) in whichever segment holds
   it, with `origin` the sender's address and `origintype` the SCXML
   event processor. Otherwise it is a `TA_EFFECT_SEND` to the I/O
   processor the config marks `"remote": true` (at most one), with the
   full target address; without one, the sender gets
   `error.communication`. The decision is made for a send through the
   SCXML processor and for a send that names the remote processor's type
   alike, so a chart cannot send a local event over the wire. A remote
   processor marked `sync` makes the SCXML processor sync, since that is
   where the system makes the send.
2. **Charts stay deployment-agnostic.** A chart addresses another system
   with the plain SCXML target (`tinyactors://b/#_scxml_5`,
   `tinyactors://b/@name`), never with a transport's type. A reply to
   `_event.origin` takes the same path back.
3. **The effect carries the origin.** `TA_EFFECT_FIELD_ORIGIN` is the
   sender's address, formatted by core on the first ask (as
   `TRACEPARENT` is), so a transport hands it to the other side without
   knowing the address grammar.
4. **Inbound is an ordinary input.** `ta_host_resolve` turns a local
   address (`#_scxml_N`, `@name`) into its session, so no binding parses
   the grammar; an address of another system is `TA_ERROR_INVALID`, one
   that names nothing `TA_ERROR_STALE`. The transport then calls
   `ta_host_send` with that session, the event's name, `sendid` and
   `origin` (the remote sender's address): inputs stay structs with a
   session, and a binding knows the target's datamodel before it makes
   the payload. An input with an origin and no origintype gets the SCXML
   event processor's (SCXML C.1). (Decided first as an address field on
   `ta_event_in`; the refutation showed a binding must know the target
   session before it can make the payload in that session's datamodel,
   and that a text field would put parsing on the per-event path.)
5. **Success is silence, failure is a failed send.** A send the
   transport took needs no input. A transport that cannot deliver answers
   the effect with `TA_EVENT_IN_ERROR_COMMUNICATION` and its sendid, as
   for any I/O processor; the sender gets `error.communication`. A
   receiving side that cannot make a value of what arrived sends
   `TA_EVENT_IN_ERROR_COMMUNICATION` to the target (SCXML C.1) and may
   tell the sender over its transport.
6. **Values are the application's.** The handler gets the payload as its
   datamodel's value (a binding converts it to the language's), and
   chooses the encoding for the wire; the receiving side passes a value
   of its own. Core writes no bytes for another system.

## Consequences

- `src/host/router.c` goes, with `ta_host_peer_*`, `ta_host_route*`,
  `ta_host_export`, `ta_host_deliver`, `ta_host_envelope*`,
  `ta_host_peer_error*`, `ta_host_message_init`, `ta_host_error_*`, the
  `TA_ROUTE_*` constants, `ta_envelope`, `ta_peer_*`,
  `TA_HOST_EFFECT_PEER`/`PEER_ERROR` and the effect fields only they set
  (`peer`, `datamodel`, `msgid`, `origintype`, `deduplication_key`,
  `wire` for PEER). The snapshot no longer carries a dedup window.
- The datamodel vtable's `from_wire` loses its only caller (the router);
  it stays, as the reader of `actor.send`'s data ([ADR 0015](0015-values-cross-core-at-its-language-free-interfaces.md)).
- The torture chamber's multi-system transport moves onto the remote
  processor.
- `routed` counts sends delivered between segments of the host, as
  before; nothing in core counts remote sends (the transport can).
- A chart's `<send>` to an absolute target that is no `tinyactors://`
  address, with the SCXML type, stays `error.communication`.
- The conformance suite's envelope vectors and the driver's `peer.add`,
  `envelope.read` and `envelope.deliver` go; the value table keeps its
  vectors (the bindings' `encode`/`decode`).
- `docs/protocol.md` keeps the address grammar and the value table, and
  describes the remote I/O processor.
- What a later need for routing in core would have to settle first:
  identity across restarts, trust, delivery guarantees, versioning. None
  of them is core's while the application owns the transport.

# The tinyactors protocol

How actors in different systems talk to each other — in one process, across
threads, between a browser and a server, through a database, over a broker or
a serial line. **Protocol version 1.**

This document is the contract. Bindings and deployments implement it; none of
them defines it. A Python gateway, a Rust service, a browser tab and a
PostgreSQL database interoperate because they all follow these rules, not
because they share code (although most of them do: see *Conformance*).

```
┌──────────────────────────────────────────────────────────────────────────┐
│ SPEC: addresses · envelope · values · delivery contract · link protocol  │
├──────────────────────────────────────────────────────────────────────────┤
│ ONE IMPLEMENTATION (C): address parser (ta_address), envelope, router in    │
│ ta_host (peers, routes, relay, origin checks, policy, errors, dedup)     │
├──────────┬───────────┬────────────┬──────────┬──────────┬────────────────┤
│ in-proc  │ WebSocket │ store link │ broker   │ HTTP     │ serial / stdio │
│          │           │ (SQL)      │          │          │                │
└──────────┴───────────┴────────────┴──────────┴──────────┴────────────────┘
```

Words used throughout:

- **system** — a set of actors sharing one address authority (a `ta_host`:
  one or more `ta_system` segments), or a PostgreSQL database of actors, or
  anything else that owns actors and speaks this protocol.
- **peer** — another system, as seen through one transport connection. A peer
  may reach further systems behind it (it *relays*).
- **transport** — whatever carries envelopes between two peers.
- **router** — the part of a system that decides where an envelope goes:
  local delivery, a peer, or an error back to the sender.

## Addresses

```
address  = "tinyactors://" system "/" local
system   = label *( "." label )                 ; at most 253 characters
label    = alnum *( alnum / "-" ) ; 1-63 characters, lowercase a-z 0-9 "-",
                                   ; not starting or ending with "-"
local    = session / name
session  = "#_scxml_" 1*20DIGIT                 ; 1 .. 2^64-1, no leading zeros
name     = "@" 1*( unreserved / pct-encoded )   ; RFC 3986; the decoded name is UTF-8
```

The general form is **`tinyactors://<system>/<local>`**, where `<local>` is
either a session (`#_scxml_<id>`) or a name (`@<name>`).

- **Session ids** are unsigned 64-bit integers, written in decimal, from 1 to
  2^64-1 (18446744073709551615). Every implementation keeps them lossless: in
  JavaScript as `BigInt` or as strings, never as `Number` (which is exact only
  up to 2^53). Session ids are unique within a system for its lifetime and
  never reused.
- **Names** (`@<name>`) are resolved **by the system that owns them**, never
  by the sender. What a name means is the owner's business: a JS system's
  `systemID`, a PostgreSQL actor's `name` column, a boot actor of a system
  image. A name that resolves to nothing is an unknown target (*Errors*).
- **System ids** are configured explicitly in every runtime. Each runtime
  documents its default (a random id for a JS system; for PostgreSQL one
  derived from the database name — `tinyactors.system_id`); deployments that
  talk to each other set them on purpose. Dots give hierarchy (`eu.orders`), nothing more: the
  protocol does not interpret them.
- Within one system, SCXML's own targets keep working unchanged: `#_parent`,
  `#_<invokeid>`, `#_internal`, `#_scxml_<id>` (a session of this system).
  They never cross the wire; a router expands them to full addresses.

Comparison is exact after parsing: the system id is lowercase by grammar, a
name compares by its decoded bytes.

## Envelope

What travels. One envelope is one SCXML event on its way to one target.

| field | type | meaning |
|---|---|---|
| `target` | address | where it goes |
| `name` | string | the event name (`_event.name`) |
| `data` | value | the event data (`_event.data`), see *Values*; may be absent |
| `sendid` | string | the sender's `sendid` (`_event.sendid`), if any |
| `origin` | address | the sending session (`_event.origin`); replies go here |
| `origintype` | string | `http://www.w3.org/TR/scxml/#SCXMLEventProcessor` |
| `msgid` | string | unique per origin: `<origin>/<counter>` or the origin plus sendid; used for errors, acks and dedup |
| `hops` | integer | relays passed so far (starts at 0) |
| `dedup` | string | optional: a key the receiver uses instead of `msgid` to drop duplicates (e.g. an application's idempotency key) |
| `traceparent` | string | optional: the W3C `traceparent` of the step that sent it; the receiving macrostep runs under it, so one trace crosses systems (ADR 0006 in the repository root) |

Rules:

- **No times.** An envelope carries no timestamps or deadlines. `<send
  delay>` is scheduled by the sender (SCXML requires that); clocks of
  different systems are never compared.
- **Send after commit.** An envelope leaves a system only once the step that
  sent it is durable by that system's own definition: after the macrostep for
  an in-memory system, after `COMMIT` for a database.
- **One shape inbound.** Every implementation accepts envelopes whole —
  C: `ta_host_deliver(const ta_envelope *)`; JS: `system.deliver(envelope)`;
  SQL: `tinyactors.deliver(envelope jsonb)` — so a new field never breaks an
  argument list.

## Values

Event data on the wire is a **neutral value**: JSON (RFC 8259), plus two
extensions every language can represent:

| JSON | meaning |
|---|---|
| `{"$ref": "<address>"}` | an actor reference; the receiver turns it into its own reference type (JS `ActorRef`, a string elsewhere) |
| `{"$bytes": "<base64>"}` | a byte string (RFC 4648, standard alphabet, padded) |
| `{"$": {…}}` | an object that itself has a key starting with `$`: the inner object, taken literally |

Nothing else is special. Numbers are JSON numbers (integers beyond 2^53 are
written as strings by convention of the application). Datamodels convert
between their own values and neutral values (`to_wire` / `from_wire` in the
datamodel vtable): only the datamodel knows a `PyObject *`, a Ruby `VALUE`, a
napi value or a quickjs value.

**Profiles.** Two peers may agree on a richer encoding in `hello` (e.g. the
`js` profile: `undefined`, `Map`, `Set`, `Date`, shared references and cycles,
as the JS bindings encode them), or on none at all for an in-process route
between systems sharing one datamodel host. The default, and what every
implementation must support, is neutral.

## Delivery

Guarantees depend on the transport, so a route **declares** them; nothing
global is assumed.

| guarantee | meaning |
|---|---|
| `ordered` | envelopes from one origin to one target arrive in send order |
| `durable` | an accepted envelope survives a crash of either side |
| `at-least-once` | an envelope may arrive twice (after a retry); receivers deduplicate |
| `at-most-once` | an envelope may be lost (and then `error.communication` reaches the sender if the loss is detected) |

| transport | natural guarantees |
|---|---|
| in-process | ordered, exactly once |
| WebSocket link | ordered while connected; queued across reconnects up to a TTL; at-most-once across a crash |
| store link (SQL + outbox) | durable, at-least-once; ordered per peer when drained by one worker per peer |
| broker | what the broker provides |
| HTTP | one envelope per request; at-least-once with retries |

- **Within a system**, SCXML ordering holds (per sender and receiver FIFO).
- **Duplicates**: every envelope has a `msgid`, so any receiver *can* drop
  duplicates. A system keeps a **dedup window** (the last N message ids, or a
  time-bounded table) on routes that may repeat. The window is part of the
  system's state: a snapshot keeps it, and a durable system (a database)
  stores it with its actors.
- **No distributed transactions.** A cross-system send is never part of a
  transaction spanning two systems. Applications that need "my write and the
  event, atomically" use the local system's mechanism (in PostgreSQL:
  `tinyactors.send` inside their own transaction).

## Errors

A send that cannot be delivered is never dropped silently. The router that
finds out answers the sender's system with an error, and the sender's session
receives SCXML's `error.communication` (with the `sendid`):

| reason | when |
|---|---|
| `unknown-target` | no such session or name at the owning system (also: a session that ended) |
| `unreachable` | no route to the system |
| `denied` | the receiver's policy refuses it |
| `expired` | queued for an absent peer longer than the route's TTL |
| `hops` | the hop limit was exceeded (a routing loop) |
| `malformed` | the envelope or its value does not parse |

On a link, the error travels as an `err` message carrying the `msgid`, the
`sendid`, the `origin` and the reason; the router at the origin turns it into
`error.communication` for that session. In a store link the same happens
through `fail()`. Errors about errors are never sent.

## Routing

Each system has one router (in C: `ta_host`). For every outbound envelope:

1. **Local** — the target's system is this one: deliver to the session or
   resolve the name; unknown → `unknown-target`.
2. **Peer** — a route maps the target system to a peer: hand it over
   (`TA_HOST_EFFECT_PEER` effect with the peer handle; the transport sends it).
3. **None** — `unreachable`.

For every inbound envelope (from a peer): validate the origin (*Security*),
check the policy, drop duplicates, then deliver locally or **relay** to
another peer with `hops + 1`. Envelopes with `hops` above the limit (default
8) are answered with `hops`.

Routes come from configuration and from **announcements**: a peer lists the
systems it can reach in its `hello` (filtered by its own policy), and the
router installs routes for them. There is no implicit default route; a
deployment may configure one (e.g. a browser that sends everything unknown to
its server).

## Link protocol

The message protocol between two peers over any bidirectional transport.

Messages (JSON objects, field `t` is the type):

| `t` | fields | meaning |
|---|---|---|
| `hello` | `v` (protocol version, 1), `id` (system id), `reach` (system ids reachable through this peer, may be empty), `caps` (`profile`, `ordered`, `acks`, `max_frame`), `cred` (opaque credential, optional) | first message in each direction; nothing else before it |
| `ev` | the envelope fields | an envelope |
| `err` | `msgid`, `sendid`, `origin`, `reason` | delivery failed; becomes `error.communication` at the origin |
| `ack` | `msgid` | optional (only if both sides announced `acks`): the envelope was accepted durably |
| `ping` / `pong` | `at` (sender's clock, echoed) | liveness and round-trip time |

**Framing.**

- Message-oriented transports (WebSocket, `postMessage`, broker messages):
  one JSON text per message.
- Byte streams (TCP, stdio, serial): **length-prefixed** frames — a 4-byte
  big-endian length, then that many bytes of UTF-8 JSON, one message per
  frame. A frame may arrive split across reads; one read may hold several
  frames. Each side announces the largest frame it accepts in
  `caps.max_frame` (default 1 MiB, 1048576 bytes). Frames above the peer's
  `max_frame` are refused with `malformed`:
  - the **sender** does not send them: an envelope whose frame would not fit
    is refused where it is, and its origin gets `error.communication`;
  - the **receiver** skips a frame above its own `max_frame` (or one that is
    not UTF-8) as its bytes arrive, without buffering it, so the stream stays
    in step. It has not read the frame, so it cannot answer with an `err`.
    Between peers that follow the first rule, this does not happen.

  Codecs: `encodeFrame` / `FrameDecoder` in the JS bindings
  (`core.mjs` of each JavaScript binding, usable by any transport), and
  `ta_frame_header` / `ta_frame_read` for C hosts (`include/tinyactors/frame.h`,
  freestanding: no allocation, the caller's buffer).

**Hello again.** A peer may send `hello` again at any time with the same
`id`; the receiver replaces what it has as that peer's `reach`. This is how a
relay announces systems that connected after the link opened.

**Profiles.** `caps.profile` names the richest value encoding a peer
understands. Both sides use a profile only if both announced it; otherwise
`data` is a neutral value (*Values*). The JS bindings announce `js`.

**Versions.** A peer that receives a `hello` with an unknown major `v` closes
the connection. Unknown fields are ignored; unknown message types are
ignored.

An `ev` message is the envelope itself plus `t`:

```json
{"t": "ev", "target": "tinyactors://server/@lobby", "name": "join",
 "data": {"player": {"$ref": "tinyactors://tab-7f3/#_scxml_4"}},
 "origin": "tinyactors://tab-7f3/#_scxml_4", "msgid": "tinyactors://tab-7f3/#_scxml_4/12", "hops": 0}
```

**Implementations.** In C the router is `ta_host` and a transport is a loop
around three calls: `ta_host_peer_add` when a connection says hello,
`ta_host_deliver` / `ta_host_peer_error` for incoming `ev` / `err`, and the
`TA_HOST_EFFECT_PEER` / `TA_HOST_EFFECT_PEER_ERROR` effects for outgoing ones.
`ta_links` (`include/tinyactors/link.h`) speaks this link protocol over any
message transport: hellos and reach, pings, reconnects with backoff,
resumption and exports; its caller only moves text and says what its
transports did. The JS bindings run their transports on it: WebSockets (`createLink`,
`serveLinks` with `exports`, `trusted` / `trust(hello)`, `resumeMs`,
`queueTTLMs`, `announce`), and byte-stream transports with the same options:
TCP in Node (`tcp: { host, port }`), and any duplex byte stream, such as a
child process's stdio, through `transport` / `accept` (`maxFrame` sets
`caps.max_frame`). `system.deliver(envelope)` hands an envelope to the
router from host code.

**Resumption.** A peer that reconnects with the same system id takes over its
routes again; envelopes queued for it meanwhile (up to the route's TTL) are
delivered in order.

## Security

- **Origin validation is mandatory at every hop.** An envelope from a peer
  must have an `origin` inside a system that peer may speak for — its own id,
  its announced `reach`, or a system the receiver configured to route through
  that peer (replies come back the way sends went); otherwise it is refused
  (`denied`). A browser cannot forge events "from" a database actor.
- **Policy is declarative**, per peer: which local names or charts are
  exported to it, which event names it may send. The default for a peer
  without a credential the host trusts is to deny everything not explicitly
  exported. Rules live in the router (C, no callbacks); bindings may check
  more before handing an envelope over.
- **Credentials** in `hello` are opaque to the protocol; the host validates
  them (a session cookie, a token, mTLS identity of the transport) and picks
  the policy.

## Peers

- **Ephemeral peers** (browser tabs, devices) connect and disconnect. A route
  to an absent peer has a policy: queue up to a TTL (then `expired`), or fail
  at once.
- **Durable peers** (a database, a hibernating system) accept envelopes while
  their actors are not in memory; that is their business, not the
  protocol's.
- **Identity**: a peer is its system id. Two connections claiming one id: the
  newer one wins, the older one is closed.

## Transactions

The protocol has none. Each system commits its own steps; envelopes leave
after commit (*Envelope*), and arrive as new events in the receiver's own
steps. Exactly-once effects across systems are built from at-least-once
delivery plus deduplication (`msgid`, `dedup`), never from a global commit.

## Observability

Every route reports the same status, whatever the transport:

| field | meaning |
|---|---|
| `state` | `connecting`, `open`, `reconnecting`, `closed` |
| `queued` | envelopes waiting for the peer |
| `lag` | age of the oldest queued envelope (ms) |
| `errors` | errors sent back on this route (monotonic) |
| `rtt` | last round-trip time (ms), if the transport measures it |

C hosts read it with `ta_host_peer_status` (plus `rtt` the transport
reports with `ta_host_peer_round_trip_time`). The JS bindings expose a client link as an
actor (its snapshot is the connection status, its context the queue and
round trip) and every route through `link.status()` /
`server.status(systemID)`.

## Transports

A transport carries envelopes and link messages between two peers and nothing
else: no routing, no policy, no encoding decisions. Known transports:

- **in-process** — systems on threads or workers of one process.
- **WebSocket** — browser ↔ server, server ↔ server (`createLink`,
  `serveLinks`).
- **store link** — a database that holds actors (PostgreSQL today): envelopes
  in through SQL, out through an outbox per peer. See *Store links*.
- **broker** — NATS, Kafka, Redis streams: one subject/topic per system id.
- **HTTP** — one envelope per request (serverless, webhooks).
- **TCP, serial, stdio** — length-prefixed frames (servers, microcontrollers,
  child processes): `createLink` / `serveLinks` with `tcp`, or any byte
  stream through `transport` / `accept`; `ta_frame_read` in C.

Non-tinyactors SCXML processors interoperate through SCXML's BasicHTTP
Event I/O Processor, outside this protocol.

## Store links

A store link connects a system to a database of actors through SQL. Any
language with a SQL client can implement it; the database side is the
`tinyactors` extension.

The extension uses the C host router without resident segments. Its
`ta_host_route_begin` / `ta_host_route_step` driver answers lookup requests
from actor rows and dedup requests with an atomic insert into `seen`.
The router validates origins, applies policy and hop limits, chooses next
hops and dedup keys, and builds message identities and error events.
PostgreSQL owns privileges, locks and the transaction that commits the
dedup entry, inbox, actor state and outbox together. Store links are endpoints:
inbound targets outside the database return `unreachable`. Outbound sends
use the router's durable mode: an unknown system waits under its own id.
The peers table is read only when the router asks for it (`TA_ROUTE_PEERS`,
a send leaving the database): sends between the database's own actors cost
no peer lookup.

Inbound (envelopes for actors in the database):

```sql
SELECT tinyactors.deliver('{"target": "tinyactors://db/@order-1", "name": "pay",
                            "data": {...}, "origin": "tinyactors://web/#_scxml_7",
                            "msgid": "tinyactors://web/#_scxml_7/42"}'::jsonb, 'web');
-- returns 'ok', 'duplicate', or an error reason (unknown-target, denied, ...)
```

- The target may be a session (`#_scxml_<id>`, actors and invoked sessions)
  or a name (`@<name>`, the `name` column).
- The `origin` is kept: replies (`<send targetexpr="_event.origin">`) go
  back out through the outbox.
- `msgid` (or `dedup`) is checked against the dedup window table
  (`tinyactors.seen`, kept for `tinyactors.dedup_ttl`): a repeat returns
  `duplicate` and changes nothing.
- The call is its own transaction (or part of the caller's, if the caller
  wants that).
- The second argument names the peer the envelope came through. The role
  must be allowed to act as that peer (`tinyactors.grant_peer`), and the
  envelope's origin must be that peer or a system its `hello` announced —
  origin validation, as at every hop. Without it, the call is the
  database's own host code (the `app` privileges).

Outbound (envelopes from actors in the database to other systems): rows in
`tinyactors.outbox` with type `scxml`, the target's system in `system`, the
next hop in `peer` and the whole envelope in `envelope`.

```sql
SELECT tinyactors.hello('web', ARRAY['tab-1', 'tab-2']);     -- a store link: I am "web", I reach these
SELECT id, envelope FROM tinyactors.claim_for('web', 100, 'web/1');  -- envelopes for one peer, in order
SELECT tinyactors.ack(id);                                    -- delivered
SELECT tinyactors.deliver_error('{"origin": ..., "msgid": ..., "sendid": ..., "reason": "unknown-target"}');
                                                              -- an `err` back: error.communication at the sender
LISTEN tinyactors_outbox;                                     -- NOTIFY payload: the peer
```

- **hello** is the link protocol's `hello` in SQL: the peer's id and the
  systems it reaches (e.g. the browsers connected to a server). Only roles
  granted that peer id may say it, and a hello cannot announce another
  granted peer's system. An envelope
  for a system goes to the peer with that id, else to a peer that announced
  it, else it waits under the system's own id until some peer says hello for
  it (pending rows move to that peer). Say hello again when what you reach
  changes.
- `claim_for(peer, n, worker, lease)` leases the oldest undelivered
  envelopes for one peer, in order; one worker per peer keeps them ordered,
  several workers trade ordering for throughput. A lease that runs out is
  taken over (at-least-once).
- Each row carries a complete envelope, ready to send: `msgid` is
  `<origin>/<step>.<n>` (durable processing number and effect index), stable
  across rollback/retry and distinct even when SCXML reuses a `sendid`.
- `deliver_error(err)` finds the row by its msgid and fails it; the sending
  session gets `error.communication` (as `fail(id, reason)` does).
- `tinyactors.prune_seen()` drops dedup entries older than
  `tinyactors.dedup_ttl` (default 7 days); call it now and then.
- The database's system id is `tinyactors.system_id()` (the setting
  `tinyactors.system_id`, default derived from the database name);
  `tinyactors.address(text)` parses addresses with the same grammar as
  `ta_address`.
- Values in `data` are neutral values; between actors in the database too
  (`{"$bytes"}` arrives as a `Uint8Array`, `{"$ref"}` as the address).

A store link transport in another language is these few statements. The JS
bindings have one (`createStoreLink(system, { query, listen })`, driver
independent): it delivers in order and retries until the database takes an
envelope, claims on NOTIFY and on a poll timer, acks in batches, and says
hello with the browsers on the system's links.

## Deployments

| deployment | transport | notes |
|---|---|---|
| one process, one system | — | |
| one process, many systems (segments, threads, workers) | in-process | may negotiate no encoding |
| browser ↔ server | WebSocket | ephemeral peers, resumption, strict origin checks, default-deny policy |
| server ↔ server | WebSocket, TCP, broker | system ids as broker subjects |
| database of actors | store link | at-least-once + dedup; ordered per peer |
| serverless | HTTP | the router runs per request |
| embedded | serial | the C router is freestanding |

## Conformance

- `tests/conformance/addresses.json` — address parsing and formatting
  vectors; run by the C parser (`tests/unit/test_address.c`) and the JS
  bindings.
- `tests/conformance/values.json` — neutral value vectors; run in C and JS.
- `tests/conformance/links/*.json` — link transcripts (messages in, messages
  and deliveries out); replayed by the Node and web bindings, on a text
  transport and again on a byte stream in length-prefixed frames (split and
  coalesced at odd offsets).
- Framing: `tests/unit/test_frame.c` (C) and
  `bindings/node/test/frames.test.js` (JS codec; links over TCP and a child
  process's stdio).
- `tests/unit/test_router.c` — the router: routes, relaying, hop limit,
  origin validation, policy, errors, dedup, route status.

An implementation in a new language passes the vectors and transcripts.

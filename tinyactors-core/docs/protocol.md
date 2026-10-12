# The tinyactors protocol

How actors in different systems talk to each other — in one process, across
threads, between a browser and a server, through a database, over a broker or
a serial line. **Protocol version 1.**

This document is the contract. Bindings and deployments implement it; none of
them defines it. A Python gateway, a Rust service, a browser tab and a
PostgreSQL database interoperate because they all follow these rules, not
because they share code (although most of them do: see *Conformance*).

**Since [ADR 0009](../../docs/adr/0009-one-host-two-queues.md)** core speaks
no transport protocol: the link protocol, its framing and store links were
deleted, with the bindings that ran them. An envelope leaves a host as a
`TA_HOST_EFFECT_PEER` effect and arrives as `ta_host_deliver`; how it travels
is the application's. Where this document still names `hello`, `err`, links
or the JS bindings, it describes what they did.

```
┌──────────────────────────────────────────────────────────────────────────┐
│ SPEC: addresses · envelope · values · delivery contract                  │
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

| key | CBOR type | meaning |
|---|---|---|
| `target` | text, an address | where it goes (required) |
| `name` | text | the event name (`_event.name`, required) |
| `data` | tag 24 on a byte string | the event data (`_event.data`), one neutral value (see *Values*) encoded inside; absent: none |
| `sendid` | text | the sender's `sendid` (`_event.sendid`), if any |
| `origin` | text, an address | the sending session (`_event.origin`); replies go here |
| `origintype` | text | absent: `http://www.w3.org/TR/scxml/#SCXMLEventProcessor` |
| `msgid` | text | unique per origin: `<origin>/<counter>` or the origin plus sendid; used for errors, acks and dedup |
| `hops` | unsigned integer | relays passed so far (absent: 0) |
| `dedup` | text | optional: a key the receiver uses instead of `msgid` to drop duplicates (e.g. an application's idempotency key) |
| `traceparent` | text | optional: the W3C `traceparent` of the step that sent it; the receiving macrostep runs under it, so one trace crosses systems (ADR 0006 in the repository root) |

An envelope is a CBOR map with text keys. A writer puts the keys in the
order of this table and leaves out the empty ones. A reader takes them in
any order and refuses a map with an unknown key, a repeated key or a value
of the wrong type: such bytes are not an envelope, and nobody is answered.
An envelope without `target` or `name` is read, so that its sender can be
answered: delivery refuses it as `malformed` (see *Errors*).

The data is wrapped (RFC 8949, tag 24: an encoded CBOR data item) so that a
router reads the envelope without reading the value. Only the datamodel
that takes it decodes it:

- **Reading the bytes** checks that the data is one well-formed CBOR item
  and keeps it as bytes (`ta_envelope.wire`). Nothing is decoded yet,
  because the target is not known yet.
- **Local delivery** decodes it with the datamodel of the chart the target
  session runs (`from_wire`). A host with several datamodels never decodes
  a value with another one's reader, and never translates between them.
- **A relay** never decodes it: the `TA_HOST_EFFECT_PEER` effect carries the
  bytes as they arrived (`wire`), and so does a queue for a peer that is
  down, and `ta_host_envelope` writes them out unchanged.

Data that is not one well-formed item, or that the target's datamodel cannot
read as a neutral value, is `malformed`, and the sender is answered (see
*Errors*). Where that is found depends on the fault: bytes that are not CBOR
are refused by the first host that reads them; a value the target cannot
read is refused by the target's host, after any relays.

```
{"target": "tinyactors://b/@echo", "name": "ping", "data": 24(h'a1616e01'),
 "sendid": "p1", "origin": "tinyactors://a/#_scxml_1", "msgid": "m1", "hops": 1}
```

The data is `{"n": 1}`. An error envelope (see *Errors*) has the key
`error`, the reason, and only `sendid`, `origin` and `msgid` besides:

```
{"error": "unknown-target", "sendid": "p1", "origin": "tinyactors://a/#_scxml_1",
 "msgid": "m1"}
```

Rules:

- **No times.** An envelope carries no timestamps or deadlines. `<send
  delay>` is scheduled by the sender (SCXML requires that); clocks of
  different systems are never compared.
- **Send after commit.** An envelope leaves a system only once the step that
  sent it is durable by that system's own definition: after the macrostep for
  an in-memory system, after `COMMIT` for a database.
- **CBOR only between processes.** In C, an envelope is a `ta_envelope` and
  an error a `ta_peer_error`, and `ta_host_deliver` and
  `ta_host_peer_error` take them whole, so a new field never breaks an
  argument list. A transport between two hosts in one process fills them
  from the `TA_HOST_EFFECT_PEER` and `TA_HOST_EFFECT_PEER_ERROR` effects and
  never writes bytes (a relayed value it copies from the effect's `wire` to
  the envelope's `wire`, still undecoded). A transport between processes writes each effect with
  `ta_host_envelope` and, on the other side, reads the bytes with
  `ta_host_envelope_read` or `ta_host_peer_error_read` before delivering
  them: one encode and one decode per hop, none inside a process.

## Values

Event data between systems is a **neutral value**: one CBOR item (RFC 8949),
of these kinds:

| CBOR | meaning |
|---|---|
| unsigned and negative integers | integers, -2^64 … 2^64-1, exactly |
| floats (half, single, double) | numbers that are not integers, -0.0, NaN, ±Infinity |
| text strings | text (UTF-8) |
| byte strings | bytes |
| arrays | arrays |
| maps with text keys | objects; a key that is not text makes the value invalid |
| `false`, `true`, `null`, `undefined` | themselves |
| tag 32 on a text string | an actor reference: its address; the receiver turns it into its own reference type, or a string when it has none |

Nothing else is a neutral value: another tag, another simple value, or an
indefinite length is refused. A key starting with `$` is an ordinary key.
Writers use the shortest form of every integer, length and float, and write
NaN as `f9 7e00` (`ta_cbor_encode`); map keys stay in the order written.

Datamodels convert between their own values and neutral values (`to_wire` /
`from_wire` in the datamodel vtable): only the datamodel knows a `PyObject *`,
a Ruby `VALUE` or a quickjs value. The ECMAScript datamodel writes what
`JSON.stringify` keeps, with numbers, BigInts and bytes exact: an integer
within ±(2^53-1) as an integer and any other number as a float, a BigInt as
an integer (past 64 bits it is not a neutral value), and an `ArrayBuffer`, a
typed array or a `DataView` as bytes; it reads integers past 2^53 as BigInts,
bytes as a `Uint8Array` and a reference as its address. `ta_cbor_diagnostic`
writes a value as text, for logs and traces.

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

Between systems, the error travels as an error envelope carrying the
`msgid`, the `sendid`, the `origin` and the reason (see *Envelope*); the
router at the origin turns it into `error.communication` for that session.
Errors about errors are never sent.

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
reports with `ta_host_peer_round_trip_time`).

## Transports

A transport carries envelopes between two peers and nothing
else: no routing, no policy, no encoding decisions. Known transports:

- **in-process** — systems on threads or workers of one process.
- **WebSocket** — browser ↔ server, server ↔ server.
- **database** — a database that holds actors: envelopes in through SQL, out
  through an outbox per peer.
- **broker** — NATS, Kafka, Redis streams: one subject/topic per system id.
- **HTTP** — one envelope per request (serverless, webhooks).
- **TCP, serial, stdio** — servers, microcontrollers, child processes.

Non-tinyactors SCXML processors interoperate through SCXML's BasicHTTP
Event I/O Processor, outside this protocol.

## Deployments

| deployment | transport | notes |
|---|---|---|
| one process, one system | — | |
| one process, many systems (segments, threads, workers) | in-process | may negotiate no encoding |
| browser ↔ server | WebSocket | ephemeral peers, resumption, strict origin checks, default-deny policy |
| server ↔ server | WebSocket, TCP, broker | system ids as broker subjects |
| database of actors | SQL + outbox | at-least-once + dedup; ordered per peer |
| serverless | HTTP | the router runs per request |
| embedded | serial | the C router is freestanding |

## Conformance

- `tests/conformance/addresses.json` — address parsing and formatting
  vectors; run by the C parser (`tests/unit/test_address.c`).
- `tests/protocol/values.txt` — neutral value vectors, CBOR in hex with
  its diagnostic notation; run in C (`tests/unit/test_datamodel_ecmascript.c`).
- `tests/protocol/envelopes.txt` — envelopes and error envelopes as bytes:
  written, read, refused and relayed (`tests/protocol/run_protocol.c`; see
  `tests/protocol/README.md`).
- `tests/unit/test_router.c` — the router: routes, relaying, hop limit,
  origin validation, policy, errors, dedup, route status.

An implementation in a new language passes the vectors.

# Durable Streams: what the protocol offers a web app built on tinyactors

**Date:** 2026-09-26
**Used by:** nothing yet. Related: [replay persistence](../proposals/replay-persistence.md), [distributed systems](../proposals/distributed-systems.md), [napi ADR 0020](../../tinyactors-napi/docs/adr/0020-mail-for-an-unloaded-actor-waits-while-a-loader-brings-it-back.md) (the loader)
**Source:** [`PROTOCOL.md`](https://github.com/durable-streams/durable-streams/blob/f380bca/PROTOCOL.md) at commit `f380bca` (2026-06-02), the last commit that changed it. The document is marked **DRAFT**, version 1.0, by ElectricSQL. Section numbers below refer to that file.

The question: you deploy a web app. The server runs a tinyactors system
in napi; browsers talk to it over a network that drops connections; the
app is redeployed several times a day and may run in more than one
process. Where does the Durable Streams protocol fit in that picture,
and where does it not? This note records what the protocol says, what
its implementations are, and how its parts line up with tinyactors'
existing designs. It decides nothing.

## Summary

| Web-app need | Durable Streams part | Fit |
|---|---|---|
| Push an actor's updates to a browser, and resume after a dropped connection | catch-up read from an offset, then long-poll or SSE (§5.6–5.8) | **good** |
| Many viewers of the same actor (dashboards, a shared chat) | cacheable chunks and cursors for CDN collapsing (§10) | **good** |
| The browser posts to an actor exactly once over a flaky network | idempotent producers: `Producer-Id`, `-Epoch`, `-Seq` (§5.2.1) | **good** |
| Tell viewers an actor finished | stream closure, atomic with the last append (§4.1) | **good**: maps to a final state |
| Survive a redeploy: rebuild actors from their history | an actor's log as a stream, read from `-1` (§8) | partial: see "Persistence" |
| Several server processes: one owner per actor | producer epochs (403 fencing) and subscription leases (§5.2.1, §7.3) | partial: leases are per subscription, not per stream |
| One turn's writes to many actors commit together | none: every append is to one stream | **no** |
| 385k messages per second through the log | one HTTP request per append | **no**, not as the primary local log |

The best fit is the edge of the app: the connection between the server
and its browsers. The weakest fit is the primary log behind replay
persistence, where one transaction per turn across many actors is the
reason the storage cross-check chose one store per system.

## The protocol in brief

- **A stream is a URL** for an append-only byte sequence. `PUT` creates
  it (idempotent: 200 for the same configuration, 409 for a different
  one), `POST` appends, `GET ?offset=` reads, `HEAD` gives the tail and
  whether it is closed, `DELETE` removes it (§5.1–5.6).
- **Offsets are opaque tokens** that sort lexicographically and are
  strictly increasing. `-1` is the start, `now` the tail. A client must
  use the offsets the server returns and must not construct them (§8).
- **Reads have three modes:** catch-up, long-poll and SSE.
  `Stream-Up-To-Date: true` means the response reached the tail when it
  was generated. It is not an end-of-stream signal (§5.6).
- **Closure** is durable and final. A writer can append its last message
  and close in one request. Readers see `Stream-Closed: true` only when
  they have reached the final offset (§4.1, §5.6). An append to a closed
  stream gets 409 with `Stream-Closed: true` (§5.2).
- **Idempotent producers** (§5.2.1). All three headers go together.
  - Lower epoch than the server's: **403**, with the server's epoch in
    `Producer-Epoch` (zombie fencing).
  - Higher epoch: must start at seq 0 (otherwise 400); accepted, 200.
  - Same epoch: seq ≤ last is a duplicate, **204**; last + 1 is
    accepted, **200**; a gap is **409** with `Producer-Expected-Seq`.
  - The sequence counts **requests**, not messages.
  - Success returns the highest accepted seq, "to recover state after
    crashes". **Auto-claim:** a producer without a stored epoch can take
    the 403's epoch plus one. The spec says to use this with caution.
  - The server **SHOULD** commit producer state and the append
    atomically; a store that does not gives at-least-once across crashes
    and must say so.
  - Producer state **MAY** expire (7 days is suggested for in-memory
    stores). After that, a zombie can write again.
- **`Stream-Seq`** is a second, application-level sequence, compared as
  strings; the server chooses and documents its scope (§5.2).
- **JSON mode** (`application/json`) keeps message boundaries, flattens
  one level of a posted array into separate messages, rejects `[]`, and
  returns reads as one JSON array (§9.1). Other content types are bytes
  with no framing.
- **Forks** (§4.2): a `PUT` with `Stream-Forked-From` shares the source's
  data up to an offset the server issued. Same server only. A closed
  stream can be forked, and the fork starts open. A fork does not inherit
  producer state. Deleting a source with forks soft-deletes it (410).
- **Retention:** a server **MAY** drop old data while the stream stays
  open; reads before the retained range get 410 (§4, §5.6). A deleted
  stream's URL **SHOULD NOT** be used again (§4).
- **TTL:** sliding (`Stream-TTL`) or absolute (`Stream-Expires-At`).
  Live readers keep a stream alive (§5.1).
- **Read and write paths can be separate.** A server **MAY** implement
  only the read path and put data in through its own system (§3).
- **Subscriptions** (§6–7): a durable cursor (`acked_offset`) per linked
  stream, linked by glob (`actors/**`) or listed explicitly. When a
  linked stream has data past its cursor, the server wakes a worker by
  **webhook** (signed with Ed25519) or by writing to a **pull-wake**
  stream. A worker **claims** the subscription and holds a **lease**
  (1 second to 10 minutes, default 30 seconds) that each ack extends.
  Every wake has a `generation` and a `wake_id`; stale callbacks, acks
  and releases get 409 `FENCED`.

## Implementations

From the repository at `461b402` (2026-09-10), MIT licensed:

- `packages/server`: the Node.js reference server, in memory or on files.
- `packages/caddy-plugin`: a Go server as a Caddy plugin, in memory or on
  LMDB. The README calls it production ready.
- `packages/server-cloudflare`: one Durable Object per stream, on
  Durable Objects' SQLite.
- `packages/server-conformance-tests`: a test suite that runs against
  any server URL (`--run http://localhost:4437`).
- Clients in TypeScript, Python, Ruby, Go, Rust, Swift, Java, .NET,
  Elixir and PHP; `packages/state` (materialized state over a stream,
  with a TanStack DB layer); `packages/proxy` (makes streamed AI
  responses resumable); transports for the AI SDK and TanStack AI.

The conformance suite matters most to us: a server of our own that
speaks the protocol can be checked against it.

## How the parts line up with a tinyactors web app

### Server to browser: an actor's updates as a stream

A named actor whose state a browser shows (a cart, a chat, a job) gets a
stream, for example `/streams/cart/tenant42/cart-123`. What the actor
sends to its viewers is appended there. The browser reads from `-1` or
from an offset it stored, then follows with SSE.

- **Reconnects** need no protocol of our own: the browser resumes from
  the last `Stream-Next-Offset` (§8 says clients SHOULD store it).
- **Many viewers** are served from the CDN, not from napi. Catch-up
  chunks are immutable and cacheable (§10).
- **A final state** closes the stream with the done data as the last
  message, so viewers see the end of the actor (§4.1).
- In a chart, this is an I/O processor send
  (`<send type="ds" target="…">`, style 1 of the distributed-systems
  proposal). A browser-side chart that follows a stream is an invocation
  (`<invoke type="ds" src="…">`, style 2); `Stream-Closed` becomes
  `done.invoke.<id>`.
- Viewers must not trust the data (§12.3); nothing changes for the
  server, which does not read these streams.

### Browser to server: exactly-once posts

A browser that posts to an actor can use the producer headers even when
the server is not a Durable Streams server: `Producer-Id` = the browser
session, `Producer-Epoch` = the page load, `Producer-Seq` = a counter.
Retries after a timeout are then safe. The server keeps
`(producer, epoch, last seq)` per target and answers 204 for a
duplicate. This is the deduplication "by origin and send ID" that the
distributed-systems proposal leaves for later, with rules already
written down.

### Persistence: surviving a redeploy

Napi ADR 0020's loader rebuilds an actor after a restart from
`{ spawn, binding }` or from a replayed log. With an actor's history in
a stream:

- **Summon** is a catch-up read from `-1`. `Stream-Up-To-Date` marks the
  end of the history at that moment; mail after it is live. This is the
  replay proposal's "history as a prefix of the mailbox".
- **A finished actor** is a closed stream. Later mail gets 409 with
  `Stream-Closed`, which is the proposal's dead letter with reason
  `done`.
- **Forks** let a new chart revision be tried on a copy of an actor's
  history before it is deployed, and let a finished actor's history be
  continued (the fork starts open).
- **Fork offsets** must be offsets the server issued, so a log record
  would have to keep its offset to fork at that event.

Where it does not fit:

- **No transaction across streams.** One turn can deliver to up to 1024
  actors. The storage cross-check chose one store per system because it
  is the only layout where a turn's logs, the durable mailbox and the
  outbox commit together. With streams, each actor is a separate append.
  A way around it: a sender appends to the receiver's stream with its
  own producer ID and a counter that replay re-derives, so a retry after
  a crash is a 204 duplicate. Because `Producer-Seq` counts requests,
  this needs one message per request or batch boundaries that replay
  reproduces exactly.
- **Who orders an actor's events.** If other actors append to its
  stream, the stream decides the order. The owner's own events (fired
  timers, invoker reports, I/O results) must then be appended before
  they are delivered, which adds a round trip to every timer. The
  alternative is two streams: an inbox with many writers, and a history
  with one writer, committed once per turn, that points to inbox
  offsets.
- **Retention** conflicts with "history grows without bound, no
  snapshots": a server that drops old data breaks pure replay, and
  session images are tied to one definition revision (core ADRs 0036,
  0038) and to the process (core ADR 0074).
- **Throughput.** Napi measured about 385k messages per second for a
  websocket-shaped load (`tinyactors-napi/docs/scale.md`). One HTTP
  append per actor per turn does not keep up with that.

§3 allows a middle way: our own store (one per system, SQLite or
PostgreSQL) keeps its group commit for writes, and answers reads with
this protocol. Browsers, CDNs and the clients listed above can then read
actor histories, and the conformance suite can check the read path.

### Several processes: one owner per actor

- **Epoch fencing.** If an owner writes an actor's history with
  `Producer-Id` = the address and `Producer-Epoch` = its ownership
  generation, a process that lost ownership gets 403 on its next commit.
  Napi releases a turn's effects only after the commit, so a fenced
  process releases nothing. Two conditions: the epoch must come from an
  authority (a lease or the directory), not from auto-claim, which any
  process can do; and producer state must not expire while the actor
  exists.
- **Subscription leases** are close to the loader hook. A pull-wake
  subscription over `actors/**` wakes a worker when mail is appended for
  any actor, loaded or not. The worker claims, holds a lease, and acks
  the offset it has processed. The research on Orleans says leases are
  the only practical way to one activation per actor
  ([hindsight](actor-runtime-distribution-hindsight.md)). But a lease
  covers a whole **subscription**: owning actors one by one needs one
  subscription per actor, or one claim owns a group of actors.

## Deployment shapes

| | Reference or Caddy server beside napi | Cloudflare server | Our store, protocol read path |
|---|---|---|---|
| Runs | a second process (Caddy binary, LMDB files) | Workers and one Durable Object per stream | inside the napi deployment |
| Writes | HTTP per append | HTTP per append, one DO per stream | our group commit per turn |
| Atomic across actors | no | no | yes |
| Browser reads, CDN | yes | yes | yes, if we implement §5.6–5.8 and §10 |
| Subscriptions and leases | per server | yes (the spec's §7 speaks of Durable Objects) | only if we implement §6–7 |
| Best for | server-to-browser streams | server-to-browser streams at the edge | persistence and browser reads from one store |

## What tinyactors would need

These are observations, not plans.

- **Core:** nothing. The replay proposal's three core capabilities (a
  time override per resume, held sends during replay, withdrawal by time)
  are needed whether or not the log is a stream. Origins of events that
  arrive through a `ds` processor are covered by core ADR 0068.
- **Napi:** a `ds` I/O processor and a `ds` invoker registered under the
  same type (SCXML §6.4), and a loader that replays a stream.
- **A value codec.** JSON mode fixes JSON as the codec for a stream. A
  binary codec (`v8.serialize`, CBOR) needs framing of its own and is
  base64-encoded over SSE (§5.8). The codec is an open question in both
  proposals.
- **Actor names that come back.** A released name that is spawned again
  must not reuse its deleted stream's URL (§4), so the URL needs an
  incarnation, or the stream is closed and never deleted.

## Open questions

- Does the Caddy server commit producer state atomically with the append
  on LMDB? The exactly-once guarantee depends on it (§5.2.1).
- What throughput and latency does one server reach for small appends,
  and with how many streams? The repository has `packages/benchmarks`.
- Does a subscription over millions of streams (one per actor) work, or
  do subscriptions assume a few streams each?
- Can a lease be scoped to one stream without one subscription per
  actor?

## Reading note

A summary of the spec made by a fetch tool left out §3's separate read
path, all of §6–7 (subscriptions, leases, generation fencing), producer
state expiry, auto-claim, the request-based `Producer-Seq`, retention,
URL reuse, and the DRAFT status. Read the spec itself.

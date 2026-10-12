# ADR 0011: The control plane is posted messages; images are bytes the host keeps

**Status:** Accepted (2026-10-13). Amends [ADR 0009](0009-one-host-two-queues.md)
decision 2 (requests) and [ADR 0010](0010-outputs-are-views-values-stream.md)
decision 2 (a request's reply is no longer a view until the next call).
Reference: [tinyactors-core/docs/boundary.md](../../tinyactors-core/docs/boundary.md).
**Date:** 2026-10-13

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the operator and the observability backend, through a remote
  control plane.** A JSON-RPC transport (a debugger, the MCP-server
  proposal, an admin socket) needs requests, replies and notifications in
  one ordered stream. Today `ta_host_request` returns a reply view, and
  subscription updates arrive as `TA_EFFECT_CHANGED` effects that every
  transport would translate into notifications of its own making.
- **Then: the binding author.** One way to send a control message, read
  like every other effect; and images that need no rows, no
  `SEGMENT_FREED` bookkeeping and no "must not move".
- **Out of scope:** JSON-RPC batches in core (a transport splits and
  reassembles them); notifications for effects a binding must act on
  (WAKE, PEER, TRACES, ...: they stay effects); zero-copy images from
  flash or a mapped file (an embedder uses `ta_system` directly, whose
  `ta_system_setup.image` stays borrowed).
- **Who pays:**

  | Cost | What |
  |---|---|
  | per event | nothing |
  | per message | as a request today, plus one copy of the reply into the effect pool and one record (248 B `ta_host_effect` + 64 B strings) |
  | per run with subscriptions | at most one state value per subscribed session that finished a macrostep (at most `maxSubscriptions`, 64) |
  | per image | one copy at `ta_host_add_image` (charts are 1–10 KB; a segment's region is 117–285 KB); the open already hashes every byte |
  | memory | the effect pool holds replies until the ack; the largest reply is held twice at peak (`host->output` and the pool); 1 byte per subscription |
  | API | `ta_host_request` and the host-level `TA_EFFECT_CHANGED` go; `ta_host_post`, two effect kinds; `ta_host_add_image` takes bytes |
  | every binding | the request facade and the drain rule below; image rows go |

## Context

The maintainer wants notifications possible now. `ta_host_request`
answers with a view and nothing else; effects are the ordered stream, and
some requests already produce effects (`trace.end` appends TRACES and LOGS
at request time). A design for posted messages found that core already
has every mechanism it needs (the analysis and its refutation are in the
session record of 2026-10-13):

- `ta_host_effects` publishes everything appended since the last ack,
  whether a run happened or not (`src/host/host.c`, `effects_ready =
  effect_count`); a spike showed `trace.end` with no run giving one TRACES
  effect.
- An effect already carries CBOR bytes in its WIRE field (the PEER relay,
  `src/host/router.c`), read with `ta_host_effect_bytes`, valid until the
  ack.
- `request.c`'s parser and method table answer a message unchanged.

On the stated reason for dropping `ta_host_request` ("a notification can
arrive before the response"): a subscribe's first update never comes
before its reply (the watch is armed at the next run start). A
notification does come before a reply when it was produced before the
post (an `actor.unsubscribe` posted while a run's `actor.changed` is still
unread), and requests already produce effects. One ordered stream is the
reason; a synchronous view has no place for either.

For images, `ta_host_add_image` takes an opened `const ta_image *` whose
struct and bytes the caller must keep, unmoved, until the segment retires.
Core identifies images by content hash, never by address (snapshots,
hibernate and export blobs), so every pointer it keeps can point into a
copy the host owns.

## Decisions

### 1. `ta_host_post` is the only way to send a control message

```c
/* A JSON-RPC 2.0 message without "jsonrpc", in CBOR (docs/requests.md),
 * read during the call. A message with an id (or one that cannot be read)
 * is answered: the reply is the LAST effect this call appends,
 * TA_HOST_EFFECT_REPLY, after any effects the method itself appends. */
TA_API ta_status ta_host_post(ta_host *h, ta_bytes message);
```

`ta_host_request` is deleted. The reply is written into `host->output`
as today, then copied once into the effect pool (slot 7, WIRE, as the PEER
relay does). Room for the method's own effects and the reply is reserved
before a method applies, so `TA_ERROR_NO_MEMORY` still means nothing was
applied.

### 2. Two effect kinds

- `TA_HOST_EFFECT_REPLY` (23): WIRE is `{"id", "result"}` or
  `{"id", "error"}`.
- `TA_HOST_EFFECT_NOTIFICATION` (24): WIRE is `{"method", "params"}`;
  EVENT is the method name; SESSION is the session it is about, where it
  is about one.

Both are read with the existing accessors and live until
`ta_host_effects_ack`, the next `ta_host_run` or `ta_host_free`.

### 3. Publication and order (the code already does this; now it is a rule)

`ta_host_effects` publishes everything appended since the last ack, with
or without a run. In one batch: effects appended between runs in call
order (earlier REPLYs, a deliver's PEER_ERROR, `trace.end`'s TRACES), then
the run's effects in drain order, then the run's notifications at its end.
Within one post, the method's effects come before its REPLY.

### 4. One notification: `actor.changed`

`{"method": "actor.changed", "params": {"session": uint, "value": <state
value | null>}}`, emitted at the end of every run in which a subscribed
session finished a macrostep: at most one per session per run. `value` is
what `actor.state` would answer after the run (written at the end of the
run, since the watch is re-armed only at run start while a run may loop);
`null` means the session ended, and its subscription with it. It replaces
the host-level `TA_EFFECT_CHANGED` (the system's CHANGED and
`ta_actor_watch` stay, below the host). The context is not carried:
reading variables runs `to_wire`, user code, and its size is unbounded.
Nothing is paid per event without subscriptions. A notification the
allocator refuses at the end of a run keeps its mark and is emitted at the
next.

### 5. Errors

`ta_host_post` returns `TA_ERROR_STATE` during a run or inside the
datamodel guard (nothing appended), `TA_ERROR_NO_MEMORY` when the
allocator refuses room for the reply or the held-block limit is reached
(nothing applied, no REPLY), else `TA_OK`; JSON-RPC errors travel inside
the REPLY. A message without an id is applied with no REPLY; one that is
not a request is answered with `"id": null`. Batches stay refused
(-32600).

### 6. How a binding drains, and its synchronous `request`

A binding's drain re-reads `ta_host_effects` until the count is stable,
handles what it has not consumed, and acks only at the outermost level,
so an effect published by a nested call is never released unread. A
binding's synchronous `request(method, params)` posts with a fresh id,
calls `ta_host_effects`, finds its REPLY at the last index of that call's
array, decodes it (valid until the ack), marks it consumed, and drains
the rest unless it is already inside a drain. A facade that acks inside a
handler, or publishes and does not ack before the next run, loses
effects; both were reproduced in a spike, and a lost TRACES or LOGS stops
telemetry for good.

### 7. `ta_host_add_image` takes bytes, and the host keeps a copy

```c
/* A new segment running the image `image` (.tai bytes, any alignment). The
 * host reads `image` during the call only, before any datamodel call: it
 * copies the bytes into a block from environment->allocator, opens them
 * there (ta_image_open), and frees the block when the segment is freed or
 * with ta_host_free. The caller may change or free its bytes once the call
 * returns. TA_ERROR_INVALID: not an image; TA_ERROR_NO_MEMORY: the
 * allocator refused (nothing added); TA_ERROR_STATE during a run. */
TA_API ta_status ta_host_add_image(ta_host *h, ta_bytes image, uint32_t *segment);
```

One block per segment, `[ta_image | bytes]`, from the host's allocator
(8-aligned), since `host->segments` grows by copying. With it:
`ta_host_snapshot_input.images` becomes `ta_bytes`; each image block is
freed after `ta_system_deinit`, for every segment including one a failed
restore left; retiring clears the segment's image; the ECMAScript
datamodel's `chart_by_hash` compares stored hashes instead of reading an
image that may be gone; `ta_host_segment_info.image` is the host's copy,
NULL once freed. `TA_HOST_EFFECT_SEGMENT_FREED` stays as a retirement
signal; nothing needs it to free an image any more.

## What changes elsewhere

- `tinyactors-core/docs/requests.md`: the envelope is posted; replies and
  notifications are effects; the `actor.changed` notification.
- `tinyactors-core/docs/bindings.md` sections 3, 4 (REPLY and NOTIFICATION
  rows, no CHANGED row), 5 (the drain rule), 6 (images), 10.
- `tinyactors-test`: the driver's `host.request` becomes `host.post
  {message, refuse}` answering the REPLY's and NOTIFICATIONs' bytes;
  `host.run` returns its notifications; request vectors keep `send` and
  `reply`, with new lines for a subscribe exchange.
- The bindings: the request facade and drain rule; image rows,
  `SEGMENT_FREED` image handling and the 256-byte `ta_image` objects go.

## Rejected alternatives

- **Keeping `ta_host_request` beside `ta_host_post`:** two ways to do one
  thing (`AGENTS.md`), and a view that ends at the next call where an
  effect lasts until the ack.
- **A queue of posted messages answered at the next run:** inputs are
  applied at their call; answering at the call orders the reply as well
  and needs no queue (`boundary-design`).
- **The REPLY before the method's own effects:** atomicity reserves room
  by capacity before applying; the reply is written last.
- **The state value written when the macrostep ends:** it can be older
  than the state at the end of the run.
- **Context in the notification:** user code and unbounded size.
- **Batches in core:** JSON-RPC answers a batch with one array, which a
  transport assembles from the REPLYs.
- **`ta_compile_image` returning the `ta_image` the result holds (R2):**
  it kept the binding responsible for an image's lifetime; the host owning
  a copy removes the responsibility.
- **A borrow mode for zero-copy images:** the host already pays a region
  per segment far larger than an image; zero-copy embedders use
  `ta_system`.

## Order of work

1. `ta_host_post` with its tests, `tests/support/host_request.h`, and
   `ta-conform`, in one commit; `ta_host_request` deleted.
2. `actor.changed`, replacing the host-level CHANGED.
3. `ta_host_add_image` from bytes, with the five fixes above.
4. The driver protocol and the vectors.
5. The guide and `requests.md`.
6. The bindings, by their sessions, in one message after the maintainer
   has read this ADR.

# WebSocket connection actor

`websocket_connection.c` is one concrete, bounded actor for one accepted
WebSocket. A closed socket never reconnects. The host stops/releases that actor
and spawns another actor for the next socket.

Statechart:

```text
connecting --ws.open--> connected [onentry: schedule heartbeat]
connected
  initial: ready
  ready --server.update--> same [targetless; ta_context_send -> TA_IO_SEND]
  ready --ws.write_blocked--> blocked
  blocked --ws.drain--> ready
  heartbeat.timeout: check peer liveness; ping; reschedule
  ws.pong: record peer activity; no new timer
  onexit: cancel heartbeat
connecting/connected --ws.closed/ws.error/ws.timeout/error.*--> closing
closing --(final)--> terminal [terminal callback; close_once; release later]
```

`connection_chart` uses plain nested structs and C99 designated initializers,
without construction macros. State and transition arrays end with `{0}` instead
of manual counts; transitions refer to string IDs. `ta_definition_build` derives parents,
preorder indices, transition ranges and forward target references. It constructs
a copied, shared definition once, not a tree per connection. The flat table API
remains available for generated or ROM-backed definitions.

`connected` owns the heartbeat and failure transitions inherited by both child
states. Moving between `ready` and `blocked` does not exit/reenter that parent,
so it cannot restart the timer or reset the peer-activity timestamp.
`server.update` is admitted from a durable session/replay/coalescing store via
`post_update`. While blocked or disconnected, that store retains/coalesces the
update; the socket actor does not.

The byte actor state contains a `socket_token`, session generation, and the last
observed peer activity time. Write pressure is the
`blocked` state; the host owns the bounded transport queue. A host creates the initial image
for a new socket and keeps durable browser state separately (authentication,
subscription, acknowledged sequence, replay cursor, and backlog policy).
`spawn_connection` has three arguments; the caller supplies the initial byte
image in `ws_spawn_input_t`, overriding this definition's zero-filled default.
The image must contain a nonzero socket token and session generation;
`ACT_READY` rejects missing identities. Establish the
actor-to-socket mapping before running the system. This actor is created after
the host has accepted/upgraded the WebSocket; handshake/authentication timeouts
are host-owned. Post `ws.open` before publishing updates, or stop the actor if
that notification cannot be admitted.

`ACT_READY` schedules one delayed self-message with `ta_context_send` on entry
to `connected`. Every 15 seconds the timer checks for 30 seconds without peer
activity. If that deadline is reached, it raises `ws.timeout`; otherwise it
sends `ws.ping` through the processor and schedules the next timer. The host
encodes `ws.ping` as a WebSocket Ping and posts `ws.pong` only after receiving a
valid Pong for this socket. Only pongs update peer activity. Server publications
do not prove that the browser is alive. Pongs and drain transitions never start
extra timers, and heartbeat handling remains active while writes are blocked.
These deadlines use event-processing time and the host must keep driving `run`.

Closing cancels pending heartbeat sends; runtime termination also discards
delayed sends and queued events. A dispatched timer cannot be canceled, but it
is addressed to this exact actor incarnation and cannot revive or reach a new
connection. `closing` is deliberately final: this example requests immediate
socket shutdown through the host, rather than modeling a graceful close-frame
exchange. Host control notifications use `ta_post` with the exact actor ref and
an empty payload; stop the actor if critical close/drain notification admission
fails instead of leaving it stranded.

## Transport handoff and backpressure

Processor registration uses a `{0}`-terminated descriptor array, plain URI
strings and NULL-terminated alias arrays. This send-only adapter omits the
optional location callback. `connection_processors` has static storage because
the configuration helper returns before system creation. See `ioprocessors.c`
for two processors declared directly in a create call's enclosing scope.

`ws_dispatch_one` leases one request with `ta_io_next` and hands it to the
host's `queue_write(user, socket_token, request)` callback. Configure both host
callbacks and use the same `ws_host_ops_t` instance for configuration/dispatch.
The token is an incarnation-safe host capability, not a reusable raw file
descriptor. `/browser` is relative to that token; it is not a global destination.
The host must validate its actor/token mapping and retain that mapping while
taken requests remain. The dispatcher cancels requests for terminal/stopped
actors. On TA_OK or TA_AGAIN the host owns the request lease and must later
call `ws_complete_write(system, request, TA_OK)` only after the actual write is
accepted by its socket buffer. It reports `TA_COMMUNICATION_ERROR` on failure.
The core then queues `error.communication`, which closes the actor. A temporary
full socket buffer instead returns `TA_AGAIN` after retaining the request in a
bounded host retry queue. The dispatcher leaves that lease outstanding and
posts `ws.write_blocked`; if the notification cannot be queued it requests actor
stop. An exhausted retry queue is a fatal error, not TA_AGAIN. Once writable,
the host retries retained requests in order and completes them, then posts
`ws.drain` before publishing more updates. It must post drain after the blocked
notification, not from inside queue_write. Dispatcher TA_AGAIN means a request
was handed off under pressure, not that the core outbox is empty. A callback
returning any other error retains no lease; the
dispatcher completes it as a communication failure. No path silently reports
a successful write. Direct core admission failures propagate unchanged and
close the actor through the error transition.

The session publisher retains each update (or its coalesced replacement) until
an application acknowledgement from the browser, even when ta_post or a write
succeeds. That protects updates ignored while blocked and writes lost around a
disconnect. On reconnect, validate the browser's session/resume cursor and bind
it to a fresh actor/generation; replay from the last acknowledged sequence or
send a fresh snapshot if retention was exceeded. Acknowledgements carry sequence
and session/generation information and are validated by the durable session
store. Transport write completion must not advance that cursor. This example
specifies this host contract; it does not implement a database or replay broker.

`connection_terminal` calls the host's idempotent `close_once` hook. The host
tracks actor-to-socket generation and ignores duplicate close requests. A
completion for an old actor/request is passed through `ta_io_complete`; the API
returns `TA_STALE` for duplicate/unknown leases, and actor IDs are never reused.
No completion callback resolves a current actor by name: old requests are
completed with their original IDs even after actor release. This prevents a
reconnect from applying an old write completion to the replacement actor.

## Host-owned boundaries and API findings

The core supplies actor state, transitions, delayed sends/cancellation, queue
admission, request leases, serialized completion, and terminal notification.
The host still owns WebSocket parsing, socket readiness, actual writes,
backpressure thresholds, monotonic-clock driving, durable session/replay state,
reconnect policy, and the idempotent socket close implementation. These are
transport/session responsibilities, not missing actor semantics. There is no
core API for a durable browser session, a socket write buffer, or reconnect;
the example exposes those as host hooks. In particular, a temporarily blocked
send remains leased instead of relying on the insufficiently explicit
non-racing `TA_CANCELLED` completion semantics to discard it without an error.

The configured per-actor limits bound queues, pending sends and core I/O memory;
the host must also bound its socket buffer and durable replay/coalescing store.
It must drain ready work/request leases, wait on the core's next deadline, and
release terminal actors outside callbacks after initiating socket cleanup.
Socket cleanup also completes every retained lease, including pending retries.
The example closes a connection on core admission failure; it does not promise
lossless overload handling within the ephemeral actor's fixed memory budget.

The engine, including the new builder, is not implemented in this repository;
the header defines its public contract. Strict compilation validates C types
and initializers. Arity, list terminators and target references need separate
checks; none of these checks prove runtime transition selection or network
behavior.

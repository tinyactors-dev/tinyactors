# Ring protocol records

Generated from [`catalog.rb`](../catalog.rb) by `ruby render.rb all`. Do not edit.

- The data model is not on a ring. Core calls ta_datamodel_vtable synchronously, as today (reference/data-model.md). Only its results cross the rings, as value references.
- Defining a chart, creating a system and the operator calls (inspect, abandon, operator transitions) stay ordinary API calls: they are control, not traffic (ARCHITECTURE.md, "Two kinds of call").
- A vref in a c2h record is a reference core retained for the host. The host releases it when it has used the record. A vref in an h2c record is one the host retained for core; core releases it when the envelope holding it is freed.

## Rings

| Ring | Producer → consumer | Loss | When full |
|---|---|---|---|
| `h2c` | host → core | none | The host cannot post. It refuses at its edge (for example HTTP 503) or keeps the input itself. Core never waits on h2c. |
| `c2h` | core → host | none | A record an action would write does not fit: the action fails as error.execution.capacity. Core starts a macrostep only when c2h has room for its reserve (c2h.input, c2h.fence and one record of the largest size). A run never waits. |
| `tel` | core → host | drop | The record is dropped and counted. The next tel.dropped says how many. Telemetry never slows the system. |

## Index

| Code | Message | Summary | Fixed bytes |
|---|---|---|---|
| `0x0101` | [`h2c.time`](#h2ctime) | Move the system clock | 32 |
| `0x0102` | [`h2c.event`](#h2cevent) | An external event for a session | 88 |
| `0x0103` | [`h2c.spawn`](#h2cspawn) | Create a session | 64 |
| `0x0104` | [`h2c.publish`](#h2cpublish) | End a replay and put the session on the route | 48 |
| `0x0105` | [`h2c.load`](#h2cload) | Bring an unloaded session back | 48 |
| `0x0106` | [`h2c.load_failed`](#h2cload_failed) | The session cannot be loaded | 48 |
| `0x0107` | [`h2c.unload`](#h2cunload) | Page a stable session out | 40 |
| `0x0108` | [`h2c.checkpoint`](#h2ccheckpoint) | Ask for a recorded session's state | 40 |
| `0x0109` | [`h2c.invoke_report`](#h2cinvoke_report) | An invoked service finished or failed | 64 |
| `0x010A` | [`h2c.destroy`](#h2cdestroy) | End a session now | 40 |
| `0x0201` | [`c2h.spawned`](#c2hspawned) | A session exists | 64 |
| `0x0202` | [`c2h.input`](#c2hinput) | A recorded session took an input | 112 |
| `0x0203` | [`c2h.send`](#c2hsend) | A send for an I/O processor | 88 |
| `0x0204` | [`c2h.invoke`](#c2hinvoke) | Start an invocation | 80 |
| `0x0205` | [`c2h.invoke_cancel`](#c2hinvoke_cancel) | Cancel an invocation | 48 |
| `0x0206` | [`c2h.invoke_forward`](#c2hinvoke_forward) | Autoforward an event to an invocation | 88 |
| `0x0207` | [`c2h.log`](#c2hlog) | A <log> | 56 |
| `0x0208` | [`c2h.fence`](#c2hfence) | A macrostep committed | 56 |
| `0x0209` | [`c2h.wake`](#c2hwake) | Call core back at this time | 40 |
| `0x020A` | [`c2h.bounced`](#c2hbounced) | An h2c record was not run | 56 |
| `0x020B` | [`c2h.load_needed`](#c2hload_needed) | Envelopes wait for an unloaded address | 48 |
| `0x020C` | [`c2h.loaded`](#c2hloaded) | A session is resident again | 48 |
| `0x020D` | [`c2h.unloaded`](#c2hunloaded) | A session left memory | 56 |
| `0x020E` | [`c2h.state`](#c2hstate) | A recorded session's state at a point of its log | 56 |
| `0x020F` | [`c2h.published`](#c2hpublished) | A replayed session is live | 56 |
| `0x0210` | [`c2h.finished`](#c2hfinished) | A session reached a top-level final state | 48 |
| `0x0211` | [`c2h.destroyed`](#c2hdestroyed) | A session is gone | 48 |
| `0x0301` | [`tel.trace`](#teltrace) | One trace record (core ADR 0067) | 120 |
| `0x0302` | [`tel.dropped`](#teldropped) | Telemetry records were dropped | 48 |

## Common header (32 bytes)

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `kind` | u16 | the message's code |
| 2 | `flags` | u16 | the message's flags (bits 0..7) and the common flags (bits 8..15) |
| 4 | `size` | u32 | the whole record in bytes, header and tail included, a multiple of 8 |
| 8 | `seq` | seq | the sequence number in this ring, from 1, with no gaps on a lossless ring |
| 16 | `session` | session | the session the record is about, or 0 |
| 24 | `at` | time | the system clock when it was written (h2c: the input's time) |

Common flags: bit 8 `replaying` (part of a replay (reference/recording-and-replay.md). On c2h and tel: written by a replaying session. On h2c: an input of a replay; under a whole-system replay every h2c record carries it, sessions it spawns replay, mail between them is delivered as envelopes, and effects that would leave the process are suppressed until h2c.publish); bit 9 `recorded` (the session is recorded: its inputs appear as c2h.input).

## h2c.time

`0x0101` on `h2c`, 32 fixed bytes. Move the system clock.

The header `at` is the new time. It never moves back. Delayed sends due by then become runnable envelopes, in due order, then send order.

- may be answered by: [`c2h.wake`](#c2hwake)

## h2c.event

`0x0102` on `h2c`, 88 fixed bytes. An external event for a session.

Header `session` names a resident session, or is 0 and `target` gives an address. Admitted into the pool when the pool is below its watermark. An event for an unloaded address waits in the pool and core asks for the session with c2h.load_needed.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `target` | text | the address (family/name), when session is 0 |
| 40 | `name` | text | the dotted event name, as bytes (reference/names.md) |
| 48 | `sendid` | text | _event.sendid, or empty |
| 56 | `origin` | text | _event.origin, or empty |
| 64 | `origin_type` | text | _event.origintype, or empty |
| 72 | `data` | vref | _event.data |
| 80 | `type` | event_type (u8) | almost always external |

- may be answered by: [`c2h.input`](#c2hinput), [`c2h.bounced`](#c2hbounced), [`c2h.load_needed`](#c2hload_needed)

## h2c.spawn

`0x0103` on `h2c`, 64 fixed bytes. Create a session.

With the replaying flag the session is prepared to replay (core ADR 0089): its clock starts at `clock`, it crosses no border, and it is published with h2c.publish.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text | family/name |
| 40 | `revision` | bytes | the definition's revision hash |
| 48 | `data` | vref | the data model binding, or 0 |
| 56 | `clock` | time | with replaying_from_clock: the replayed run’s start |

- flag bit 0 `replaying_from_clock`: the session replays; `clock` is where its run began
- may be answered by: [`c2h.spawned`](#c2hspawned), [`c2h.bounced`](#c2hbounced)

## h2c.publish

`0x0104` on `h2c`, 48 fixed bytes. End a replay and put the session on the route.

Held delayed sends due by `at` are dropped; the rest stay in the pool under their due times (core ADR 0094). Active invocations are retired with error.communication.invoke.<id>.unavailable.

An empty `address` publishes every replaying session at once: the end of a whole-system replay (reference/recording-and-replay.md).

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text | family/name, or empty for every replaying session |
| 40 | `at` | time | the moment the replayed run stopped |

- may be answered by: [`c2h.published`](#c2hpublished), [`c2h.bounced`](#c2hbounced)

## h2c.load

`0x0105` on `h2c`, 48 fixed bytes. Bring an unloaded session back.

Usually the answer to c2h.load_needed. Its delayed sends rejoin the pool. Waiting envelopes for it become runnable.

With replaying, the image is a checkpoint (c2h.state): the session replays the c2h.input records after it and is published with h2c.publish, as a spawned replaying session is.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `image` | blob | the bytes c2h.unloaded gave |

- flag bit 0 `replaying`: restore a checkpoint as a replaying session
- may be answered by: [`c2h.loaded`](#c2hloaded), [`c2h.bounced`](#c2hbounced)

## h2c.load_failed

`0x0106` on `h2c`, 48 fixed bytes. The session cannot be loaded.

Every envelope waiting for the address is recycled: mail becomes error.communication for its sender, in the same envelope (reference/envelope.md, "Recycling"); external events are dropped and reported as tel.trace with a new trace kind, DEAD_LETTER, naming the event and the reason.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `reason` | text |  |


## h2c.unload

`0x0107` on `h2c`, 40 fixed bytes. Page a stable session out.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |

- may be answered by: [`c2h.unloaded`](#c2hunloaded), [`c2h.bounced`](#c2hbounced)

## h2c.checkpoint

`0x0108` on `h2c`, 40 fixed bytes. Ask for a recorded session's state.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |

- may be answered by: [`c2h.state`](#c2hstate), [`c2h.bounced`](#c2hbounced)

## h2c.invoke_report

`0x0109` on `h2c`, 64 fixed bytes. An invoked service finished or failed.

Header `session` is the invoking session.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `invokeid` | text |  |
| 40 | `reason` | text | for error: the reason segment |
| 48 | `data` | vref | done data or error data |
| 56 | `kind` | invoke_report_kind (u8) |  |

- may be answered by: [`c2h.input`](#c2hinput)

## h2c.destroy

`0x010A` on `h2c`, 40 fixed bytes. End a session now.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |

- may be answered by: [`c2h.destroyed`](#c2hdestroyed), [`c2h.bounced`](#c2hbounced)

## c2h.spawned

`0x0201` on `c2h`, 64 fixed bytes. A session exists.

For a recorded session this is the first record of its log.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `revision` | bytes |  |
| 48 | `parent` | session | the invoking session, or 0 |
| 56 | `cause_seq` | seq | the h2c.spawn, or 0 for an <invoke> |


## c2h.input

`0x0202` on `c2h`, 112 fixed bytes. A recorded session took an input.

Written when a recorded session takes an envelope, before any effect of the macrostep it starts. It is the replay log: these records, fed back as h2c.event and h2c.invoke_report under h2c.spawn(replaying), rebuild the session.

Not written for an input a replaying session takes: that input is already in the log being replayed. A session restored from c2h.state and replayed starts writing c2h.input again after h2c.publish.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep |  |
| 40 | `source_seq` | seq | source h2c: the h2c seq |
| 48 | `from_session` | session | source mail or timer: the sender |
| 56 | `name` | text |  |
| 64 | `sendid` | text |  |
| 72 | `origin` | text |  |
| 80 | `origin_type` | text |  |
| 88 | `invokeid` | text |  |
| 96 | `data` | vref |  |
| 104 | `source` | input_source (u8) |  |
| 105 | `type` | event_type (u8) |  |


## c2h.send

`0x0203` on `c2h`, 88 fixed bytes. A send for an I/O processor.

Mail to a local session never appears here: it is an envelope (reference/envelope.md). A delayed send to an I/O processor waits in the pool and is written when due. The host acts on it after the macrostep's c2h.fence.

A delayed send that becomes due on h2c.time is written outside any macrostep: its `macrostep` is 0, no fence follows, and the host may act on it at once. Why: the macrostep that scheduled it committed at its own fence, and firing it changes no session, so there is nothing to commit and nothing to roll back (reference/envelope.md, "A due send to an I/O processor has no fence").

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep | 0 for a delayed send that became due |
| 40 | `sendid` | text |  |
| 48 | `io_type` | text |  |
| 56 | `target` | text |  |
| 64 | `name` | text |  |
| 72 | `data` | vref | the namelist and params, aggregated by the data model |
| 80 | `content` | vref | <content>, or 0 |

- see: [`c2h.fence`](#c2hfence)

## c2h.invoke

`0x0204` on `c2h`, 80 fixed bytes. Start an invocation.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep |  |
| 40 | `invokeid` | text |  |
| 48 | `type` | text |  |
| 56 | `src` | text |  |
| 64 | `data` | vref |  |
| 72 | `autoforward` | bool |  |

- may be answered by: [`h2c.invoke_report`](#h2cinvoke_report)

## c2h.invoke_cancel

`0x0205` on `c2h`, 48 fixed bytes. Cancel an invocation.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep |  |
| 40 | `invokeid` | text |  |


## c2h.invoke_forward

`0x0206` on `c2h`, 88 fixed bytes. Autoforward an event to an invocation.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep |  |
| 40 | `invokeid` | text |  |
| 48 | `name` | text |  |
| 56 | `sendid` | text |  |
| 64 | `origin` | text |  |
| 72 | `data` | vref |  |
| 80 | `type` | event_type (u8) |  |


## c2h.log

`0x0207` on `c2h`, 56 fixed bytes. A <log>.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep |  |
| 40 | `label` | text |  |
| 48 | `value` | vref |  |


## c2h.fence

`0x0208` on `c2h`, 56 fixed bytes. A macrostep committed.

Everything the macrostep wrote to c2h comes before its fence. The host acts on none of it before the fence: a recorded session’s c2h.input is persisted first, then its effects are released (reference/macrostep.md, "The fence").

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep |  |
| 40 | `effects` | u32 | records this macrostep wrote to c2h, the fence excluded |
| 44 | `mail` | u32 | envelopes it put in the pool |
| 48 | `status` | fence_status (u8) |  |


## c2h.wake

`0x0209` on `c2h`, 40 fixed bytes. Call core back at this time.

The earliest due delayed send in the pool changed. One timer in the host's event loop is enough.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `due` | time |  |

- flag bit 0 `none`: nothing is due: cancel the timer
- may be answered by: [`h2c.time`](#h2ctime)

## c2h.bounced

`0x020A` on `c2h`, 56 fixed bytes. An h2c record was not run.

Core hands it back with a reason and keeps nothing of it. The host decides whether to post it again.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `source_seq` | seq |  |
| 40 | `detail` | text |  |
| 48 | `reason` | bounce_reason (u8) |  |


## c2h.load_needed

`0x020B` on `c2h`, 48 fixed bytes. Envelopes wait for an unloaded address.

Written once per address until h2c.load or h2c.load_failed answers it.

For a family whose first message creates the actor, h2c.spawn answers it.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `waiting` | u32 | envelopes waiting for it |

- may be answered by: [`h2c.load`](#h2cload), [`h2c.spawn`](#h2cspawn), [`h2c.load_failed`](#h2cload_failed)

## c2h.loaded

`0x020C` on `c2h`, 48 fixed bytes. A session is resident again.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `cause_seq` | seq | the h2c.load |


## c2h.unloaded

`0x020D` on `c2h`, 56 fixed bytes. A session left memory.

The image holds the session's state and its delayed sends. The host keeps it, and wake_at, in its own index of unloaded sessions, and posts h2c.load when wake_at comes.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `image` | blob |  |
| 48 | `wake_at` | time | its earliest delayed send |

- flag bit 0 `has_wake`: wake_at is meaningful

## c2h.state

`0x020E` on `c2h`, 56 fixed bytes. A recorded session's state at a point of its log.

A checkpoint (core ADR 0091). Its delayed sends are inside the image, so it is one artifact. Replaying from it needs only the c2h.input records after input_seq.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `image` | blob |  |
| 48 | `input_seq` | seq | the last c2h.input it includes |


## c2h.published

`0x020F` on `c2h`, 56 fixed bytes. A replayed session is live.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `replayed` | u32 | inputs it replayed |
| 44 | `dropped_timers` | u32 | held delayed sends due by `at` |
| 48 | `retired_invokes` | u32 |  |


## c2h.finished

`0x0210` on `c2h`, 48 fixed bytes. A session reached a top-level final state.

Mail to a local parent (done.invoke) is an envelope; this record is for the host's books.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep |  |
| 40 | `donedata` | vref |  |


## c2h.destroyed

`0x0211` on `c2h`, 48 fixed bytes. A session is gone.

Envelopes still waiting for it are recycled as for h2c.load_failed.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `reason` | destroy_reason (u8) |  |


## tel.trace

`0x0301` on `tel`, 120 fixed bytes. One trace record (core ADR 0067).

The fields of ta_trace_record that its kind names. Values are text the data model renders (value_text), cut at the parameter telemetryValueBytes; vrefs never go on a lossy ring.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep |  |
| 40 | `trace_id_high` | u64 |  |
| 48 | `trace_id_low` | u64 |  |
| 56 | `cause_session` | session |  |
| 64 | `cause_macrostep` | macrostep |  |
| 72 | `microstep` | u32 |  |
| 76 | `state` | u32 |  |
| 80 | `transition` | u32 |  |
| 84 | `action` | u32 |  |
| 88 | `iteration` | u32 |  |
| 92 | `trace_kind` | u16 | ta_trace_kind |
| 94 | `position` | u8 | ta_trace_at |
| 95 | `result` | bool |  |
| 96 | `name` | text |  |
| 104 | `value_text` | text |  |
| 112 | `states` | bytes | u32 state IDs: exited, entered, history, counted in the tail |

- flag bit 0 `truncated`: value_text was cut
- flag bit 1 `suppressed`: an effect a replaying session did not perform

## tel.dropped

`0x0302` on `tel`, 48 fixed bytes. Telemetry records were dropped.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `count` | u64 |  |
| 40 | `first_seq` | seq | the seq the first dropped record would have had |


## Enums

### `event_type` (u8)

What _event.type says (SCXML 5.10.1).

| Value | Name | Meaning |
|---|---|---|
| 0 | `platform` |  |
| 1 | `internal` |  |
| 2 | `external` |  |

### `input_source` (u8)

Where the envelope behind a c2h.input came from.

| Value | Name | Meaning |
|---|---|---|
| 0 | `h2c` | an h2c.event; source_seq is its h2c seq |
| 1 | `mail` | a <send> from another local session; from_session names it |
| 2 | `timer` | a delayed send that became due; from_session sent it |
| 3 | `platform` | core made it: error.communication for undeliverable mail, done.invoke, ... |
| 4 | `invoke_report` | an h2c.invoke_report |

### `fence_status` (u8)

How a macrostep ended.

| Value | Name | Meaning |
|---|---|---|
| 0 | `stable` | the session waits for its next input |
| 1 | `done` | the session reached a top-level final state; c2h.finished follows |
| 2 | `runaway` | the microstep limit stopped it (core ADR 0075); the session is stable |
| 3 | `abandoned` | an operator abandoned it (core ADR 0082) |
| 4 | `capacity` | a microstep's reserve could not be restored; the macrostep ended there (reference/envelope.md, "Running out"); a tel.trace says why |

### `bounce_reason` (u8)

Why core handed an h2c record back unrun.

| Value | Name | Meaning |
|---|---|---|
| 1 | `no_such_address` | no session has the address and its family has no loader |
| 2 | `no_such_definition` | the revision is not registered |
| 3 | `address_taken` | h2c.spawn for an address that is resident |
| 4 | `has_mail` | h2c.unload for a session with envelopes waiting |
| 5 | `not_replaying` | h2c.publish for a session that is not replaying |
| 6 | `invalid` | the record breaks the protocol; detail says how |

### `capacity_resource` (u8)

The last segment of error.execution.capacity.<resource>, the event an action raises when a bounded resource has no room for it. Its data names the action (reference/macrostep.md, "Two kinds of exhaustion").

| Value | Name | Meaning |
|---|---|---|
| 1 | `pool` | no free envelope for mail or a delayed send (poolEnvelopes) |
| 2 | `names` | no room in the name area for a run-time name (nameAreaBytes) |
| 3 | `c2h` | no room on c2h for the record the action writes (c2hBytes) |
| 4 | `internal` | the internal queue is full; <raise> failed |
| 5 | `record` | the record would be larger than maxRecordBytes |

### `invoke_report_kind` (u8)



| Value | Name | Meaning |
|---|---|---|
| 0 | `done` | the invoked service finished: done.invoke.<id> |
| 1 | `error` | it failed: error.communication.invoke.<id>.<reason> (core ADR 0081) |

### `destroy_reason` (u8)



| Value | Name | Meaning |
|---|---|---|
| 0 | `finished` |  |
| 1 | `operator` | h2c.destroy |
| 2 | `parent_exited` | its invoking state exited |

## Scalar types

| Type | Bytes | Meaning |
|---|---|---|
| `u8` | 1 | unsigned 8-bit integer |
| `u16` | 2 | unsigned 16-bit integer |
| `u32` | 4 | unsigned 32-bit integer |
| `u64` | 8 | unsigned 64-bit integer |
| `i32` | 4 | signed 32-bit integer |
| `bool` | 1 | 0 or 1 |
| `time` | 8 | nanoseconds on the system clock that h2c.time moves |
| `seq` | 8 | a record's sequence number in its ring |
| `session` | 8 | a session ID (core ADR 0066); 0 means the system itself |
| `macrostep` | 8 | a macrostep's number within its session |
| `vref` | 8 | a value reference: a slot in the binding's value table; 0 means none |
| `blob` | 8 | a host-owned byte buffer, read or written through a byte source or sink (core ADR 0090) |
| `span` | 8 | u32 offset from the record start, u32 length, into the tail |
| `text` | 8 | a span of UTF-8 text in the tail |
| `bytes` | 8 | a span of bytes in the tail |

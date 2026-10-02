# Ring protocol records

Generated from [`catalog.rb`](../catalog.rb) by `ruby render.rb all`. Do not edit.

- The data model is not on a ring. Core calls ta_datamodel_vtable synchronously, as today (reference/data-model.md). Only its results cross the rings, as value references.
- Definition management uses request/result records. Native calls create and destroy the system, expose its rings, drive execution and install data model/value integrations. An attached debugger takes exclusive ownership of the whole system from the production pump. Inspection, abandonment and operator transitions use native primitives outside the production rings; no operator ring is added (reference/debugging.md).
- A vref in a c2h record is a reference core retained for the host. The host releases it when it has used the record. A vref in an h2c record transfers one retained reference to core at commit. Core releases it on refusal, drop or completion; a bounce returns status, not ownership. Host retry originals belong in a bounded submission table.
- Replay seeds new execution: supplied inputs derive configuration and data model values, which may differ from an earlier run. External effects are suppressed while seeding. In live execution the host may execute effects on receipt, independently of recording, persistence or c2h.macrostep_ended.

## Rings

| Ring | Producer → consumer | Loss | When full |
|---|---|---|---|
| `h2c` | host → core | none | The host cannot post. It refuses at its edge (for example HTTP 503) or keeps the input itself. Core never waits on h2c. |
| `c2h` | core → host | none | A record an action would write does not fit: the action fails as error.execution.capacity. Core starts a macrostep only when c2h has room for all mandatory observations, cancellations, autoforwards, completion and end reports, including wrap padding. Startup and control phases can yield between macrosteps. |
| `tel` | core → host | drop | The record is dropped and counted. The next tel.dropped says how many. Telemetry never slows the system. |

## Index

| Code | Message | Summary | Fixed bytes |
|---|---|---|---|
| `0x0101` | [`h2c.time`](#h2ctime) | Move the system clock | 32 |
| `0x0102` | [`h2c.event`](#h2cevent) | An external event for a session | 104 |
| `0x0103` | [`h2c.spawn`](#h2cspawn) | Create a session | 80 |
| `0x0104` | [`h2c.publish`](#h2cpublish) | End a replay and put the session on the route | 48 |
| `0x0105` | [`h2c.load`](#h2cload) | Bring an unloaded session back | 56 |
| `0x0106` | [`h2c.load_failed`](#h2cload_failed) | The session cannot be loaded | 56 |
| `0x0107` | [`h2c.unload`](#h2cunload) | Page a stable session out | 48 |
| `0x0108` | [`h2c.checkpoint`](#h2ccheckpoint) | Ask for a recorded session's state | 48 |
| `0x0109` | [`h2c.invoke_report`](#h2cinvoke_report) | An invoked service finished or failed | 72 |
| `0x010A` | [`h2c.destroy`](#h2cdestroy) | End a session now | 40 |
| `0x010B` | [`h2c.definition_register`](#h2cdefinition_register) | Compile and register a complete definition | 56 |
| `0x010C` | [`h2c.definition_release`](#h2cdefinition_release) | Release one registration hold | 40 |
| `0x010D` | [`h2c.definition_retire`](#h2cdefinition_retire) | End a definition’s availability | 40 |
| `0x0201` | [`c2h.spawned`](#c2hspawned) | A session exists | 64 |
| `0x0202` | [`c2h.input`](#c2hinput) | A recorded session took an input | 128 |
| `0x0203` | [`c2h.send`](#c2hsend) | A send for an I/O processor | 88 |
| `0x0204` | [`c2h.invoke`](#c2hinvoke) | Start an invocation | 88 |
| `0x0205` | [`c2h.invoke_cancel`](#c2hinvoke_cancel) | Cancel an invocation | 56 |
| `0x0206` | [`c2h.invoke_forward`](#c2hinvoke_forward) | Autoforward an event to an invocation | 112 |
| `0x0207` | [`c2h.log`](#c2hlog) | A <log> | 56 |
| `0x0208` | [`c2h.macrostep_ended`](#c2hmacrostep_ended) | A macrostep ended | 72 |
| `0x0209` | [`c2h.wake`](#c2hwake) | Call core back at this time | 40 |
| `0x020A` | [`c2h.bounced`](#c2hbounced) | An h2c record was not run | 56 |
| `0x020B` | [`c2h.load_needed`](#c2hload_needed) | Envelopes wait for an unloaded address | 48 |
| `0x020C` | [`c2h.loaded`](#c2hloaded) | A session is resident again | 48 |
| `0x020D` | [`c2h.unloaded`](#c2hunloaded) | A session left memory | 72 |
| `0x020E` | [`c2h.state`](#c2hstate) | A recorded session's state at a point of its log | 72 |
| `0x020F` | [`c2h.published`](#c2hpublished) | A replayed session is live | 72 |
| `0x0210` | [`c2h.finished`](#c2hfinished) | A session reached a top-level final state | 48 |
| `0x0211` | [`c2h.destroyed`](#c2hdestroyed) | A session is gone | 56 |
| `0x0212` | [`c2h.definition_result`](#c2hdefinition_result) | A definition request completed | 56 |
| `0x0213` | [`c2h.input_refused`](#c2hinput_refused) | An admitted host input was not taken | 48 |
| `0x0214` | [`c2h.input_completed`](#c2hinput_completed) | A host input and its startup follow-up settled | 56 |
| `0x0215` | [`c2h.load_result`](#c2hload_result) | A load-failure response finished recycling waiting inputs | 48 |
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

The header `at` is the new time. It never moves back. Delayed sends due by then become runnable envelopes, in due order, then send order. Applied between macrosteps. Admission of an earlier event does not ensure that event has finished; the host awaits input_completed before posting time when it requires that execution order.

- may be answered by: [`c2h.wake`](#c2hwake)

## h2c.event

`0x0102` on `h2c`, 104 fixed bytes. An external event for a session.

Header `session` names a resident session, or is 0 and `target` gives an address. Admitted into the pool when the pool is below its watermark. An event for an unloaded address waits in the pool and core asks for the session with c2h.load_needed. A capacity refusal consumes and bounces the record, allowing later controls to progress.

A live input runs at system time when taken; its header at does not advance that clock. A replay input moves its prepared session clock at take. Complete event metadata must also fit the expanded c2h.input. Service events carry invocation_seq, checked again at take. Zero means ordinary input, or an application-supplied replay seed.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `target` | text | the address (family/name), when session is 0 |
| 40 | `name` | text | the dotted event name, as bytes (reference/names.md) |
| 48 | `sendid` | text | _event.sendid, or empty |
| 56 | `origin` | text | _event.origin, or empty |
| 64 | `origin_type` | text | _event.origintype, or empty |
| 72 | `invokeid` | text | _event.invokeid for an invoked service, or empty |
| 80 | `invocation_seq` | seq | c2h.invoke seq for a live service instance; 0 for ordinary input or replay seeds |
| 88 | `data` | vref | _event.data |
| 96 | `type` | event_type (u8) | almost always external |

- may be answered by: [`c2h.input`](#c2hinput), [`c2h.bounced`](#c2hbounced), [`c2h.load_needed`](#c2hload_needed), [`c2h.input_refused`](#c2hinput_refused), [`c2h.input_completed`](#c2hinput_completed)

## h2c.spawn

`0x0103` on `h2c`, 80 fixed bytes. Create a session.

With the replaying flag the session is prepared to replay (core ADR 0089): its clock starts at `clock`, it crosses no border, and it is published with h2c.publish. Inputs seed new configuration and data model values; exact restoration is not promised.

With `state`, the session is restored from a checkpoint (c2h.state, core ADR 0091) as a replaying session whose clock starts at the state's; the c2h.input records after the checkpoint follow. This is how a session survives a restart. h2c.load is not: it only pages a session back into memory within one process.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text | family/name |
| 40 | `revision` | bytes | the definition's revision hash |
| 48 | `data` | vref | the data model binding, or 0 |
| 56 | `clock` | time | with replaying_from_clock: the replayed run’s start |
| 64 | `state` | blob | a checkpoint c2h.state wrote, to restore; 0 for none |
| 72 | `request_seq` | seq | c2h.load_needed seq when answering a virtual-family load; 0 for an unsolicited spawn |

- flag bit 0 `replaying_from_clock`: the session replays; `clock` is where its run began
- may be answered by: [`c2h.spawned`](#c2hspawned), [`c2h.bounced`](#c2hbounced), [`c2h.input_completed`](#c2hinput_completed)

## h2c.publish

`0x0104` on `h2c`, 48 fixed bytes. End a replay and put the session on the route.

Held delayed sends due by `at` are dropped; the rest stay in the pool under their due times (core ADR 0094). Active invocations are retired with error.communication.invoke.<id>.unavailable.

An empty `address` publishes every replaying session at once: the end of a whole-system replay (reference/recording-and-replay.md).

Refuses without mutation while supplied seed inputs remain pending. Generated local mail need not drain: pending mail follows the selected publication cut into live execution. Publication does not execute effects suppressed during seeding.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text | family/name, or empty for every replaying session |
| 40 | `at` | time | the time chosen for the seeded session |

- may be answered by: [`c2h.published`](#c2hpublished), [`c2h.bounced`](#c2hbounced)

## h2c.load

`0x0105` on `h2c`, 56 fixed bytes. Bring an unloaded session back.

Usually the answer to c2h.load_needed. Its delayed sends rejoin the pool. Waiting envelopes for it become runnable.

Loading only bounds memory; it is not persistence. The image is one c2h.unloaded wrote in this process, and it is valid only here (core ADR 0074). To restore a session after a restart, use h2c.spawn with `state` (reference/unloading.md).

Stage the complete restore outside macrosteps. Capacity or decoding failure leaves residency, timers and waiting mail unchanged. The host pins image until c2h.loaded or c2h.bounced correlates this request. Zero request_seq is refused when an active load request exists for the address.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `image` | blob | the bytes c2h.unloaded gave, in this process |
| 48 | `request_seq` | seq | c2h.load_needed seq; 0 for an unsolicited timer-driven load |

- may be answered by: [`c2h.loaded`](#c2hloaded), [`c2h.bounced`](#c2hbounced)

## h2c.load_failed

`0x0106` on `h2c`, 56 fixed bytes. The session cannot be loaded.

Every envelope waiting for the address is recycled: mail becomes error.communication for its sender, in the same envelope (reference/envelope.md, "Recycling"); external inputs receive c2h.input_refused and DEAD_LETTER telemetry. Large waiting lists are processed incrementally between macrosteps as c2h space permits.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `reason` | text |  |
| 48 | `request_seq` | seq | the outstanding c2h.load_needed seq; a stale attempt changes nothing |

- may be answered by: [`c2h.load_result`](#c2hload_result), [`c2h.input_refused`](#c2hinput_refused), [`c2h.bounced`](#c2hbounced)

## h2c.unload

`0x0107` on `h2c`, 48 fixed bytes. Page a stable session out.

Refuses an owner of an active local invocation and an active locally invoked child. No lifetime envelope token moves into an image.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `image` | blob | host-provided writable sink, pinned until unloaded or bounced |

- may be answered by: [`c2h.unloaded`](#c2hunloaded), [`c2h.bounced`](#c2hbounced)

## h2c.checkpoint

`0x0108` on `h2c`, 48 fixed bytes. Ask for a recorded session's state.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `image` | blob | host-provided writable sink, pinned until state or bounced |

- may be answered by: [`c2h.state`](#c2hstate), [`c2h.bounced`](#c2hbounced)
- see: [`h2c.spawn`](#h2cspawn)

## h2c.invoke_report

`0x0109` on `h2c`, 72 fixed bytes. An invoked service finished or failed.

Header `session` is the invoking session.

Done enters the external queue with invoke metadata for finalize; errors are platform/internal events without event invokeid. A stale live instance cannot change a later invocation with the same authored ID.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `invokeid` | text |  |
| 40 | `invocation_seq` | seq | the c2h.invoke seq; checked at take, 0 only for replay seeds |
| 48 | `reason` | text | for error: the reason segment |
| 56 | `data` | vref | done data or error data |
| 64 | `kind` | invoke_report_kind (u8) |  |
| 65 | `ends` | bool | for error: retire the matching instance at take after validation and internal notification enqueue; false preserves it |

- may be answered by: [`c2h.input`](#c2hinput), [`c2h.bounced`](#c2hbounced), [`c2h.input_refused`](#c2hinput_refused), [`c2h.input_completed`](#c2hinput_completed)

## h2c.destroy

`0x010A` on `h2c`, 40 fixed bytes. End a session now.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |

- may be answered by: [`c2h.destroyed`](#c2hdestroyed), [`c2h.bounced`](#c2hbounced)

## h2c.definition_register

`0x010B` on `h2c`, 56 fixed bytes. Compile and register a complete definition.

Header session is 0. One atomic request, with no builder operations. Core validates a relocatable ta_scxml_spec, compiles it through the existing compiler and registers it between macrosteps. Native data model callbacks remain synchronous (reference/definitions.md).

The host keeps spec readable and diagnostic writable until the correlated c2h.definition_result. Failure registers nothing and retains nothing from the submitted spec.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `spec` | blob | complete declarative spec; offsets and counts, not native pointers |
| 40 | `datamodel` | u64 | process-local handle of a native-installed data model adapter |
| 48 | `diagnostic` | blob | host-owned bounded output buffer for a structured compile error |

- may be answered by: [`c2h.definition_result`](#c2hdefinition_result)

## h2c.definition_release

`0x010C` on `h2c`, 40 fixed bytes. Release one registration hold.

Core ADR 0077 semantics: an unused definition is freed when its last hold, actor and nesting reference goes. Header session is 0.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `definition` | u64 | the system-local definition ID returned by registration |

- may be answered by: [`c2h.definition_result`](#c2hdefinition_result)

## h2c.definition_retire

`0x010D` on `h2c`, 40 fixed bytes. End a definition’s availability.

Core ADR 0043 semantics: existing actors keep running; new spawns cannot use it. Retirement drops registration holds and is idempotent for an issued ID, including after it was freed. Header session is 0.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `definition` | u64 | the system-local definition ID |

- may be answered by: [`c2h.definition_result`](#c2hdefinition_result)

## c2h.spawned

`0x0201` on `c2h`, 64 fixed bytes. A session exists.

For a recorded session this is the first record of its log.

Creation does not imply bootstrap and invocation startup have settled. c2h.input_completed correlates that completion with the originating spawn.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `revision` | bytes |  |
| 48 | `parent` | session | the invoking session, or 0 |
| 56 | `cause_seq` | seq | the h2c.spawn, or 0 for an <invoke> |


## c2h.input

`0x0202` on `c2h`, 128 fixed bytes. A recorded session took an input.

Written when a recorded session takes an envelope, before any effect of the macrostep it starts. These observations, fed back as h2c.event and h2c.invoke_report under h2c.spawn(replaying), seed a new execution. Recording imposes no persistence-before-effects rule.

Not written for an input a replaying session takes: that input is already in the log being replayed. A session restored from c2h.state and replayed starts writing c2h.input again after h2c.publish.

Data is a copy_out snapshot created before binding or running the input, including finalize and autoforward. Later chart mutations cannot change it.

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
| 108 | `report_invokeid` | text | source invoke_report: original report invocation ID, separate from event invokeid |
| 116 | `report_reason` | text | source invoke_report/error: original reason token |
| 124 | `report_kind` | invoke_report_kind (u8) | source invoke_report only; otherwise ignored |
| 125 | `report_ends` | bool | source invoke_report/error: original retirement decision |


## c2h.send

`0x0203` on `c2h`, 88 fixed bytes. A send for an I/O processor.

Mail to a local session never appears here: it is an envelope (reference/envelope.md). A delayed send to an I/O processor waits in the pool and is written when due. The host may execute the send on receipt, without waiting for c2h.macrostep_ended or persistence.

A delayed send that becomes due on h2c.time is written outside any macrostep: its `macrostep` is 0, no end report follows, and the host may act on it at once. An end report would describe a macrostep that does not exist (reference/envelope.md, "A due send to an I/O processor has no macrostep end").

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep | 0 for a delayed send that became due |
| 40 | `sendid` | text |  |
| 48 | `io_type` | text |  |
| 56 | `target` | text |  |
| 64 | `name` | text |  |
| 72 | `data` | vref | the namelist and params, aggregated by the data model |
| 80 | `content` | vref | <content>, or 0 |

- see: [`c2h.macrostep_ended`](#c2hmacrostep_ended)

## c2h.invoke

`0x0204` on `c2h`, 88 fixed bytes. Start an invocation.

Written in the post-macrostep startup phase. This record seq is the live service-instance token; authored invokeid remains the SCXML identity.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep | the stabilization macrostep that enabled this startup |
| 40 | `invokeid` | text |  |
| 48 | `type` | text |  |
| 56 | `src` | text |  |
| 64 | `data` | vref |  |
| 72 | `content` | vref | evaluated content, or 0 |
| 80 | `autoforward` | bool |  |

- may be answered by: [`h2c.invoke_report`](#h2cinvoke_report), [`h2c.event`](#h2cevent)

## c2h.invoke_cancel

`0x0205` on `c2h`, 56 fixed bytes. Cancel an invocation.

Mandatory state-exit output, reserved before execution. The host invalidates this instance and ignores its late callbacks.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep |  |
| 40 | `invokeid` | text |  |
| 48 | `invocation_seq` | seq | the canceled c2h.invoke instance |


## c2h.invoke_forward

`0x0206` on `c2h`, 112 fixed bytes. Autoforward an event to an invocation.

Every event field is preserved at the autoforward point. Finalize runs first, as in the SCXML algorithm; each recipient then receives its own copy_out data before transition code runs. Record space is mandatory; a data-model copy failure remains error.execution.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep |  |
| 40 | `invokeid` | text | destination invocation, not the forwarded event invokeid |
| 48 | `invocation_seq` | seq | destination c2h.invoke instance |
| 56 | `name` | text |  |
| 64 | `sendid` | text |  |
| 72 | `origin` | text |  |
| 80 | `origin_type` | text | the original event origintype |
| 88 | `event_invokeid` | text | the original event invokeid |
| 96 | `data` | vref |  |
| 104 | `type` | event_type (u8) |  |


## c2h.log

`0x0207` on `c2h`, 56 fixed bytes. A <log>.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep |  |
| 40 | `label` | text |  |
| 48 | `value` | vref |  |


## c2h.macrostep_ended

`0x0208` on `c2h`, 72 fixed bytes. A macrostep ended.

Everything the macrostep wrote to c2h comes before this record. The host may already have executed effects on receipt. This record reports that execution ended and gives its status, including abandonment; invocation startup may follow. It is neither input completion nor a durability acknowledgement (reference/macrostep.md, "The macrostep end").

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep |  |
| 40 | `source` | input_source (u8) |  |
| 48 | `source_seq` | seq | originating h2c input, including internal startup follow-up; 0 for local work |
| 56 | `effects` | u32 | records this macrostep wrote to c2h, this end report excluded |
| 60 | `mail` | u32 | envelopes it put in the pool |
| 64 | `status` | macrostep_status (u8) |  |


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

Core consumes the record, releases transferred values and keeps nothing of it. The host decides whether to retry using its own retained original. Reserve the bounded reply before consuming the request. This is terminal before admission; an admitted input instead ends in refused or completed.

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

`0x020D` on `c2h`, 72 fixed bytes. A session left memory.

The image holds the session's state and its delayed sends. The host keeps it, and wake_at, in its own index of unloaded sessions, and posts h2c.load when wake_at comes.

The image bounds memory; it is not a checkpoint. It is valid only in the process that wrote it and ends with it. c2h.state is the checkpoint.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `cause_seq` | seq | the h2c.unload |
| 48 | `image` | blob |  |
| 56 | `image_bytes` | u64 | complete valid bytes written into the requested sink |
| 64 | `wake_at` | time | its earliest delayed send |

- flag bit 0 `has_wake`: wake_at is meaningful

## c2h.state

`0x020E` on `c2h`, 72 fixed bytes. A recorded session's state at a point of its log.

A checkpoint (core ADR 0091). Its delayed sends are inside the image, so it is one artifact. Replaying from it needs only the c2h.input records after input_seq.

Unlike the image c2h.unloaded writes, it is meant to be stored: its values are written with value_write durable=true (core ADR 0090). It is restored with h2c.spawn `state`.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `cause_seq` | seq | the h2c.checkpoint |
| 48 | `image` | blob |  |
| 56 | `image_bytes` | u64 | complete valid bytes written into the requested sink |
| 64 | `input_seq` | seq | the last c2h.input it includes |


## c2h.published

`0x020F` on `c2h`, 72 fixed bytes. A replayed session is live.

For an empty-address publication, one session-0 result summarizes the atomic mode change for all replaying sessions.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `cause_seq` | seq | the h2c.publish |
| 48 | `replayed` | u64 | inputs it replayed; total for all-session publication |
| 56 | `dropped_timers` | u64 | held delayed sends due by `at` |
| 64 | `retired_invokes` | u64 |  |


## c2h.finished

`0x0210` on `c2h`, 48 fixed bytes. A session reached a top-level final state.

Mail to a local parent (done.invoke) is an envelope; this record is for the host's books.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `macrostep` | macrostep |  |
| 40 | `donedata` | vref |  |


## c2h.destroyed

`0x0211` on `c2h`, 56 fixed bytes. A session is gone.

Envelopes still waiting for it are recycled as for h2c.load_failed.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `address` | text |  |
| 40 | `cause_seq` | seq | the h2c.destroy, or 0 for execution-driven destruction |
| 48 | `reason` | destroy_reason (u8) |  |


## c2h.definition_result

`0x0212` on `c2h`, 56 fixed bytes. A definition request completed.

Exactly one result per definition request. Header session is 0. Core reserves room for this fixed-size reply before consuming the request or changing definition state. No macrostep end report follows.

A compile_error diagnostic owns copies of the code, path, subject, subject range, related text and message of ta_compile_error (core ADR 0058); no borrowed pointers escape. The host owns the buffer before and after completion (reference/definitions.md).

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `cause_seq` | seq | the definition request’s h2c seq |
| 40 | `definition` | u64 | on successful register: the definition ID; otherwise 0 |
| 48 | `diagnostic_bytes` | u32 | bytes written to the request’s diagnostic buffer; 0 without a compile error |
| 52 | `status` | definition_status (u8) |  |


## c2h.input_refused

`0x0213` on `c2h`, 48 fixed bytes. An admitted host input was not taken.

Exactly one terminal refusal for an admitted h2c.event or invoke_report discarded by load failure, destruction, stale-instance validation or snapshot failure. No input observation or completion follows. Core releases its transferred values; host retry originals remain host-owned.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `source_seq` | seq | the originating h2c input |
| 40 | `reason` | input_refusal_reason (u8) |  |


## c2h.input_completed

`0x0214` on `c2h`, 56 fixed bytes. A host input and its startup follow-up settled.

Exactly one terminal result for a taken host input, including spawn bootstrap. Emitted after stabilization and invocation-startup internal follow-up settle, regardless of recording or replaying flags. Earlier macrostep ends do not settle the host operation. This does not hold effects, await child-service completion or acknowledge persistence.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `source_seq` | seq | the originating h2c input |
| 40 | `macrostep` | macrostep | the last macrostep in the input execution |
| 48 | `status` | input_completion_status (u8) |  |


## c2h.load_result

`0x0215` on `c2h`, 48 fixed bytes. A load-failure response finished recycling waiting inputs.

Acknowledges h2c.load_failed after all waiting envelopes were recycled or refused. Each external input refusal precedes this result. Recycling may yield between macrosteps to drain c2h; its cursor uses the existing waiting list. A stale attempt receives bounced instead.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 32 | `cause_seq` | seq | the h2c.load_failed |
| 40 | `request_seq` | seq | the answered c2h.load_needed |


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
| 0 | `h2c` | a host input; source_seq is its h2c seq |
| 1 | `mail` | a <send> from another local session; from_session names it |
| 2 | `timer` | a delayed send that became due; from_session sent it |
| 3 | `platform` | core made it: error.communication for undeliverable mail, done.invoke, ... |
| 4 | `invoke_report` | an h2c.invoke_report |

### `macrostep_status` (u8)

How a macrostep ended.

| Value | Name | Meaning |
|---|---|---|
| 0 | `stable` | stabilization ended; invocation startup and internal follow-up may still follow |
| 1 | `done` | the session reached a top-level final state; c2h.finished follows |
| 2 | `runaway` | the microstep limit stopped it (core ADR 0075); the session is stable |
| 3 | `abandoned` | an operator abandoned it (core ADR 0082) |

### `input_completion_status` (u8)

How an input and its invocation-startup follow-up settled.

| Value | Name | Meaning |
|---|---|---|
| 0 | `stable` | ready for another external input |
| 1 | `done` | the session finished |
| 2 | `runaway` | execution stopped at its configured limit |
| 3 | `abandoned` | an operator abandoned execution |

### `input_refusal_reason` (u8)

Why an admitted host input was discarded before take.

| Value | Name | Meaning |
|---|---|---|
| 1 | `load_failed` |  |
| 2 | `destroyed` |  |
| 3 | `snapshot_failed` | copy_out could not produce the recording snapshot |
| 4 | `stale_invocation` | the service instance is no longer active |
| 5 | `invalid` | validation at take failed, for example a backwards replay clock |

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
| 7 | `pool` | admission watermark, envelope quota or load restoration capacity |
| 8 | `names` | admission watermark or insufficient name-area storage |
| 9 | `allocation` | allocation outside macrosteps failed without committing the operation |
| 10 | `active_local_invoke` | unload of a local invocation owner or active invoked child |
| 11 | `pending_inputs` | publish attempted before supplied replay inputs settled |
| 12 | `stale_request` | a response does not match the outstanding load attempt |
| 13 | `record` | mandatory output can never fit the configured record or ring bounds |

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

### `definition_status` (u8)

The result of a definition request.

| Value | Name | Meaning |
|---|---|---|
| 0 | `ok` |  |
| 1 | `compile_error` | the complete spec was refused; diagnostic describes the first fault |
| 2 | `capacity` | allocation, a configured bound or diagnostic buffer capacity was exhausted |
| 3 | `mismatch` | an explicit identity names different content (core ADR 0077) |
| 4 | `not_found` | an unknown or stale definition ID |
| 5 | `invalid` | an invalid spec representation, adapter handle or release request |

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

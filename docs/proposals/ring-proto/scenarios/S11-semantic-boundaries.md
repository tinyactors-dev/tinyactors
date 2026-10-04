# S11: Semantic boundaries at capacity limits

**Status:** Accepted proposal refinements (2026-10-02), part of
[the ring protocol proposal](../README.md). These are implementation acceptance
scenarios, not a claim that the current runtime implements the protocol.

The host owns scheduling, and core and host take turns on one thread. Each
case fixes the host's posting and draining boundaries. Unless noted otherwise,
the definition's compiler-sized mandatory storage and mandatory c2h batch
fit the configured system. The remaining whole-execution error-storage bound
is explicit in [macrostep.md](../reference/macrostep.md#the-internal-queue-compiler-sized-mandatory-storage).

## Lifecycle and executable blocks

| Step | Setup and action | Required result |
|---|---|---|
| S11.1 | Enter a state with two separate `<onentry>` blocks. Each starts with a send that cannot acquire an envelope; a third block logs a marker. | Two errors are queued in block order. Each failing block stops, the third executes, and neither error uses the ordinary raise quota. SCXML §4.9 does not abandon the entire microstep. |
| S11.2 | An exit handler fills ordinary c2h capacity, then another send does not fit. The exited state owns an active external invocation. | The slot parks before that send; nothing of it is written, and other slots go on. After the host drains, the send runs again from the start, then the rest of the block. The ordinary sends never took the reserve, so the mandatory `c2h.invoke_cancel` (`0x0205`) and `c2h.macrostep_ended` (`0x0208`) still fit. The invocation is canceled after exit content; the host invalidates its instance. |
| S11.3 | Enter and exit an invoking state repeatedly through internal transitions, ending elsewhere. | The pending invoke never starts: no argument evaluation, lifetime token or `c2h.invoke` (`0x0204`) is issued for it. Only invocations surviving stabilization execute. |
| S11.4 | A surviving invoke fails during argument evaluation. Another external input is already queued. | Startup's internal error is processed before the external input. Any follow-up macrostep retains the original source sequence. Only after the chain settles does `c2h.input_completed` (`0x0214`) resolve the first input. |
| S11.5 | A local child finishes while every ordinary envelope is occupied. | Its lifetime token becomes external `done.invoke` mail for its parent. Finalization uses the matched invocation instance. No internal `done.invoke` is manufactured, and no new envelope allocation is needed. |
| S11.6 | Cancel a parent with a chain of invoked descendants. | Each child is logically canceled before further ordinary child execution, then executes exit cleanup in a separate reserved phase. Each runs its exit handlers once, cancels its own children, and emits no normal completion. |
| S11.7 | Try to unload the owner of an active local invocation, then try to unload its active child. | Both receive `c2h.bounced` (`0x020A`) with `active_local_invoke`. Residency, timers and token ownership do not change. Once the relationship and pending cleanup end, normal unload eligibility applies. |
| S11.8 | Schedule two pending delayed sends with the same authored send ID; execute one cancel in their owner. | Both are removed. Another session's timers and already delivered events are unaffected. |

## Recording and ownership

| Step | Setup and action | Required result |
|---|---|---|
| S11.9 | A recorded actor takes `{n: 1}`, changes its live `_event.data.n` to 2, and completes in one `ta_run`. The host then drains c2h. | `c2h.input` (`0x0202`) owns the independent pre-processing snapshot `{n: 1}`. The host can encode it after execution. The chart's original input is not frozen. |
| S11.10 | The adapter cannot isolate a recording snapshot. | No input is taken and no observation is written. External input receives `c2h.input_refused` (`0x0213`) with `snapshot_failed`; local mail is recycled into a data-free `error.communication` for its sender. No failed copy is silently shared. |
| S11.11 | Submit an external event to an unloaded actor; its load fails. | Core releases the transferred event reference and emits a terminal `input_refused(source_seq, load_failed)`. The host settles its bounded outstanding entry and releases its separately retained original. `c2h.load_result` (`0x0215`) follows all affected refusals. Telemetry may be dropped without preventing settlement. |
| S11.12 | An event fits h2c but its expanded observation exceeds `maxRecordBytes`, even though its target is unrecorded. | An `invalid` bounce occurs before admission. A chart-produced event with the same problem fails as `error.execution.capacity.record` before its send commits. Nothing becomes permanently unrecordable. |
| S11.13 | Destroy a system with values in unread h2c records, envelopes and unread c2h records. | Stop callbacks and submission; drain and release c2h while the adapter lives; core destruction releases its h2c/envelope references without generating records; host then releases originals and blob pins. Each transferred reference is released once. |

## Restoration and service instances

| Step | Setup and action | Required result |
|---|---|---|
| S11.14 | Unload an actor with ten pending timers. Other actors use its freed pool slots. Load its image. | Load stages restoration and bounces with `pool` if insufficient capacity remains. No actor is published, no subset of timers is attached, and waiting mail, directory, active request and clock remain unchanged. Host may free capacity and retry. |
| S11.15 | Loading decodes some values, then encounters invalid timer metadata or a failed value decode. | All staged values/resources are released. No partially restored actor or successful image result appears. The host's source blob remains pinned until the correlated bounce. |
| S11.16 | Resident A's delayed send targets unloaded B. It becomes due; B's load subsequently fails. | The due envelope first waits for B, then becomes A's `error.communication`. Timer ownership by A does not imply B was resident. |
| S11.17 | Load attempt A times out; attempt B starts at the same address. A's late response is already posted. | Its old `request_seq` cannot answer B. It receives `stale_request` without changing waiting mail or the new request. The host releases A's blob pin on the correlated result. |
| S11.18 | An older queued event cancels invocation A and re-enters its state, creating B with the same authored ID. A's reply was posted behind that event. | Core checks `invocation_seq` at take, refuses A's stale reply, and leaves B untouched. Its event is not finalized by B and cannot retire B. |
| S11.19 | Receive an external event containing origin, origintype, invokeid and mutable data. Its matching finalize handler updates the data; autoforward to two active services, then mutate the live data in transition code. | Finalize runs before autoforward, as in the SCXML algorithm. Each forwarded record preserves the event at that point and identifies its destination separately. Each service owns an independent data copy, unaffected by later transition changes. The recorder's earlier input snapshot remains unchanged. A nonterminal invocation error (`ends: false`) leaves the matched instance active. |
| S11.20 | Record a terminal invocation error, whose `_event.invokeid` is empty, then replay it in a new execution. | `c2h.input` retains the original report ID, kind, reason and `ends`. Replay reconstructs the report without parsing its dotted event name, uses `invocation_seq: 0`, and resolves its logical invocation in the seeded session. An old live token is never reused. |
| S11.21 | `maxConcurrentInputs` slots each fan out to an I/O processor, more than c2h holds. The host drains only when `ta_run` returns. | Each slot parks before the send that does not fit; none of them fails. `ta_run` answers `TA_RUN_C2H_FULL` once no slot can go on. After each drain at least one waiting send fits, because the slots' reserves leave room for one record of `maxRecordBytes`. Every input completes, and every send is written once. |

For admission and control progress with no runnable envelope, see
[S09](S09-edge-backpressure.md#no-runnable-work-is-required-for-control-progress).
For the host's explicit event-before-clock completion boundary, see
[S08](S08-cancel-and-second-click.md#order-a-the-click-before-the-clock).

## Who retries

External refusals settle the host's bounded outstanding submission. The host
either returns the refusal to its client or retries using its own original.
Local undeliverable mail becomes the sender's communication error. Lifecycle
notifications are mandatory reserved output. Neither telemetry nor an ordinary
macrostep end substitutes for a terminal input disposition.

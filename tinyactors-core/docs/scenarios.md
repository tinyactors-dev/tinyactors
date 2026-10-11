# Scenario walkthroughs

Notation: `instance.*` = instance header, `workspace.*` = workspace, `A.*` = actor record,
`pool[e]` = event record. Fuel numbers are illustrative. See
[architecture.md](architecture.md) for the structures.

---

## 1. Normal external event causing a transition

1. Host: `ta_system_send(system, &{target=A, name="go", payload=p})`.
   Name resolved once via `EVENT_TRIE` → `name_id`; `pool[e] = {go, p, refs=1}`;
   appended to `A.mailbox` (2 stores). A was `IDLE` → `RUNNABLE`, pushed on run queue.
2. Host: `ta_system_run(system, 10000)`. Scheduler pops A. `instance.phase ==
   PHASE_STABLE` → `ta_core_deliver(A, e)`: unpin previous `instance.current_event`
   (refs-- → maybe free), pin `e`, `datamodel->bind_event(view(e))`, phase
   `PHASE_EXTERNAL`.
3. Kernel acquires a workspace. Selection: scan `config` atomic states in
   document order; per state walk ancestors; per transition `event_lo <= id < event_hi`
   then `datamodel->eval_condition`. Matches go into `workspace.transition_set`; conflicts resolved by
   interval nesting.
4. Exit: `workspace.exit_set = config & range(domain)`; record history; walk bits
   descending, run `onexit` bytecode, clear config bits.
5. Transition content in document order (bytecode). Entry: OR static entry
   lists, walk ascending, set config bits, late-bind data, run `onentry`,
   mark `to_invoke`, generate `done.state.*` into the internal queue.
6. `S_END`: release workspace → `PHASE_EVENTLESS` → no eventless transitions
   enabled (`config & eventless_states == 0` fast path) → `PHASE_INTERNAL` →
   internal queue empty → start invokes in `to_invoke ∧ config` →
   `PHASE_STABLE`, mailbox empty → `A = IDLE`. `run` returns
   `{TA_RUN_IDLE, next_deadline}`.

Allocation: one event slot. No strings, no hashing, no host value conversion
other than the condition evaluations themselves.

## 2. Eventless transition chain reaching stability

After any microstep the kernel parks in `PHASE_EVENTLESS`. Selection with
`eventless=true` considers only transitions flagged eventless (state flag
`HAS_EVENTLESS_T` skips the rest). `A → B → C` with guards: three microsteps,
each acquiring and releasing a workspace. When selection finds nothing, phase
→ `PHASE_INTERNAL`; empty internal queue → macrostep end. `_event` is whatever
was bound before (or unbound at initialization).

## 3. Infinitely looping eventless actor rescheduled fairly

`A ⇄ B` with eventless transitions and no guards. Quantum = 500.

- Each microstep costs ~6 fuel (selection 2, exit 1, enter 1, ...).
- After ~83 microsteps fuel hits 0. The check happens at a chunk boundary:
  if inside a microstep, the continuation stays in `workspace` (`sub`, `cursor`,
  `instruction`) and the workspace stays held; if at the microstep boundary the
  workspace is released and `instance.phase = PHASE_EVENTLESS`.
- `TA_STEP_YIELD` → scheduler pushes A to the **tail** of the run queue. No event
  is required to wake it: it is still `RUNNABLE`.
- `run` returns `TA_RUN_YIELDED` when the host budget is consumed; calling
  `run` again continues A exactly where it stopped.

The actor never becomes `IDLE`; SCXML-wise it is a non-terminating macrostep,
which is legal. The host remains in control.

## 4. Several infinitely runnable actors sharing the thread

A, B, C all loop. Runq `[A B C]`, quantum 500, budget 1200.
A runs 500 → tail `[B C A]`; B runs 500 → `[C A B]`; C runs the remaining
200 and is pushed to the tail like any yielder → `[A B C]`; `run` returns
`YIELDED`. The next `run` starts with A. Round robin, deterministic. (A
fairness refinement — crediting C's unused quantum — is possible but not
needed for liveness.)
Mid-microstep yields hold at most 3 workspaces; with K=2, the third actor
gets `TA_STEP_NEED_WORKSPACE` only if it needs to *start* a microstep while both
are held mid-microstep; it waits on the `WAIT_WORKSPACE` FIFO and is woken as soon
as a holder finishes its (finite) microstep. Liveness argument: §0 C3.

## 5. Actor A sends to actor B internally

`<send event="ping" target="#_scxml_7"/>` (or `#_b` via invoke, or a static
boot-actor name resolved at link time).

1. A's bytecode `SEND d`: kernel evaluates dynamic attributes (none here),
   `datamodel->capture` for params/namelist → payload `p`, fills `core->out.send`,
   returns `TA_STEP_SEND`.
2. System (same `run` call, same native frame): route `T_SESSION(7)` →
   serial map → B handle. `pool[e] = {ping, origin=A, origintype=scxml, p}`;
   append to `B.mb`; B `IDLE → RUNNABLE` (tail of run queue).
3. System calls `ta_core_step(A)` again; A continues its block. B is **not**
   executed now; it will process `ping` in its own turn, as its own
   macrostep. No host round-trip, no string parsing (target was classified at
   compile time), no recursion.

If B does not exist: `error.communication` into A's internal queue.

## 6. Invoked SCXML child sending to / replying to its parent

Parent P in state `s` with `<invoke id="w" src="worker.scxml"/>`.

1. P enters `s` → `to_invoke` bit. At macrostep end the kernel returns
   `TA_STEP_INVOKE(def)`; system spawns child C (chart index from `LINKS`),
   `C.parent = P`, `C.parent_slot = slot(w)`, `P.invoke_child[slot] = C`.
   C is `RUNNABLE` (`PHASE_INIT`).
2. C: `<send event="progress" target="#_parent"/>` → `T_PARENT` →
   `P.mb` gets `{progress, origin=C, invokeid="w", invoke_slot=slot}`.
3. P dequeues it: origin `== invoke_child[slot]` ✓ (else dropped); bind
   `_event`; run `finalize` of `w`; then select transitions.
4. P replies `<send target="#_w" event="more"/>` → `T_INVOKE(slot)` →
   `C.mb`.
5. C reaches top-level final → `TA_STEP_DONE(donedata)` → system appends
   `done.invoke.w` (payload = donedata) to `P.mb` *after* `progress`
   (FIFO preserved), reclaims C, clears `invoke_child[slot]`.
6. If P instead exits `s` first: `TA_STEP_UNINVOKE(slot)` → C torn down; a
   `progress` already in `P.mb` is dropped at dequeue (origin mismatch).

## 7. Delayed cross-actor send

`<send event="tick" target="#_b" delay="2s" id="t1"/>` at time T.

1. Kernel returns `TA_STEP_SEND` with `delay = 2e9 ns` (static, precomputed).
2. System: payload captured **now**; `pool[e]` created; timer record
   `{e, route=T_INVOKE(slot), sendid="t1" (image string), owner=A}` linked
   into `A.timers`, pushed into `heap` with `(T+2s·tps, seq)`.
3. `run` returns `IDLE` with `next_deadline = T+2s`. Host arms its loop
   timer (libuv/asyncio/Handler/…).
4. Host later: `ta_system_advance_time(T+2s)`; `run`: pop due timer, resolve
   route now (B may be gone → `error.communication` to A), append to `B.mb`.
5. `<cancel sendid="t1"/>` before then: walk `A.timers` comparing tagged
   sendid (integer compare for image strings), `heap4_remove`, release event.

## 8. External asynchronous I/O that suspends an actor

`<invoke srcexpr="url"/>` (dynamic source) or an async custom action.

1. Kernel returns `TA_STEP_REQUEST {kind=RESOLVE_SRC, str=eval(url)}`.
   System allocates request id `r`, emits `TA_EFFECT_REQUEST(NEEDS_RESPONSE)`,
   `A = WAIT_EXTERNAL`, `A.pending_req = r`. A is not in the run queue; its mailbox
   keeps accepting events (not processed: RTC).
2. `run` returns; host drains effects, starts async I/O.
3. Later: `ta_system_resume(system, &(ta_resume){.request = request, .status = TA_OK, .result = payload_or_chart})` → validates the
   handle generation, stores the result in the kernel's output slot,
   `A = RUNNABLE`. The kernel continues the exact sub-phase.
4. Error result ⇒ the kernel raises the spec-appropriate error event
   (`error.execution`/`error.communication`) and continues.

Plain external `<send>` to e.g. BasicHTTP is **not** a request: it is a
fire-and-forget `TA_EFFECT_SEND`; failures come back later as
`ta_system_send(error.communication)` from the host.

## 9. Scheduler suspension during executable content

`onentry` with `<foreach array="items" item="x"> <assign/> <send/> </foreach>`
over 10 000 items, quantum 500.

Bytecode: `FOREACH_BEGIN items,x,-,L_end; L_body: ASSIGN; SEND; FOREACH_NEXT L_body; L_end: END`.
Fuel expires after instruction N: kernel returns `YIELD` with `workspace.instruction`
pointing at the next instruction, `workspace.foreach_frames[0] = {iteration, i, n}`, `workspace.subphase =
SUBPHASE_ONENTRY`, `workspace.cursor = state`. Workspace stays held (mid-microstep). On
resume the loop continues at item `i`. The iterator is the datamodel's
shallow copy (`foreach_begin`), so mutation of `items` during the pause is
not observable — exactly as without the pause. Other actors may run in
between; they can only enqueue into A's mailbox, which A does not read
until the macrostep ends.

## 10. Suspension during eventless processing while `_event` remains observable

1. A processes external `E` (`instance.current_event = e`, pinned, `bind_event(E)`).
2. Eventless transitions follow; their guards read `_event.name`.
3. Quantum expires at a microstep boundary: workspace released,
   `instance.phase = PHASE_EVENTLESS`, **`instance.current_event` still `e`**, refs still
   held. The datamodel binding is untouched.
4. Meanwhile B sends `F` to A → appended to `A.mailbox` only.
5. A resumes: guards still see `_event == E`. `F` becomes current only when
   A reaches `PHASE_STABLE` and the scheduler delivers it.

Yield does not touch `current_event`; only *semantic* transitions do (rows below).

| Transition | `current_event` action |
|---|---|
| scheduler yield / resume | none |
| initialization / eventless microsteps before first event | none (`_event` unbound) |
| internal event dequeued | release old, pin new, `bind_event` |
| external event delivered | release old, pin new, `bind_event` |
| stability reached | none (or `detach_event` + release if `TA_DATAMODEL_EVENT_DETACHABLE`) |
| actor done / terminated | release |

## 11. An internal event replacing the previous `_event`

During `E`'s transition content: `<raise event="X"/>` → `pool[x]` (refs 1)
appended to `instance.internal_queue`. Microstep ends; eventless selection finds nothing;
phase `PHASE_INTERNAL`: dequeue `x` (queue ref transfers to the pin),
release `e` (refs 0 → `datamodel->release(payload)`, slot freed), `current_event = x`,
`bind_event(X)`. Transitions on `X` see `_event.name == "X"`, type
`internal`. If a yield happens anywhere after this point, `X` stays current.

## 12. 100 000 mostly idle actors, a few workspaces

Config: `max_actors=100000`, `workspaces=4`, events 16 384, timers 4 096.

- Idle actor = 48 B record + ~64 B instance (no workspace, no queue objects,
  no continuation). Mailbox and timer list are two `u32` heads each.
- 100 actors receive events in a burst: they are queued `RUNNABLE`. Each
  takes a workspace only for the duration of one microstep, then parks in
  the instance header. 4 workspaces serve all of them; actors that try to
  start a microstep while all 4 are held mid-microstep wait in `WAIT_WORKSPACE`
  for a few hundred fuel units at most.
- Timers: one shared heap; a mostly-idle population with periodic delayed
  sends costs 24 B per pending timer + one event slot.
- `_event` pins: with a detachable datamodel, idle actors hold zero event
  slots; otherwise ≤ 1 each (40 B) — the explicit trade-off of §0 C9.
- Total ≈ 12 MB for the system region, regardless of how many actors were
  ever active, plus datamodel state.

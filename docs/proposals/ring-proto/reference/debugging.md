# Debugging: temporarily replace the production driver

**Status:** Sketch (2026-10-02), part of [the ring protocol proposal](../README.md)

**Accepted boundary:** debugger commands stay outside the production
protocol. Attachment suspends the production pump and transfers exclusive
ownership of one `ta_system` to the debugger. Intervention is limited to
operator transitions and the abandonment needed to enable them. The initial
design freezes the entire system; it adds no operator ring.

## Attachment and ownership

The host owns the debugger connection and its command/response transport.
An attach request takes effect after core yields to its caller, including
at a budget pause. It cannot interrupt a synchronous data model callback
that has not returned. Until ownership transfers, the debugger cannot touch
core or its rings.

Attachment stops automatic execution of every actor in the system. The
debugger makes native calls on the system's owning thread; a debugger client
on another thread or in another process goes through the host. This keeps
the existing rule that a system is not thread-safe. Other systems continue
running. The production pump and debugger never drive the same system at
the same time.

The debugger controls when core advances. It may inspect a paused actor or
explicitly drive its execution. Pending production inputs remain pending;
debug stepping must not accidentally admit them or run another actor.
The host may continue serving external I/O, including work already started.
Replies wait at the host under its normal bounded admission/refusal policy
until production resumes; attachment introduces no unbounded overflow queue.
Already executed effects remain executed.

Detachment returns the system, its pending work and any remaining
continuation to the production pump. The pump resumes from that point; it
does not roll back debug operations. The host must release ownership on
disconnect or failure of a debugger connection. Exact attachment API names
and disconnect policy remain to be specified.

## Native debugging primitives

These are debugging capabilities, not additional production request records.
Their exact signatures remain a sketch:

- **Inspect:** read configuration, data and execution position while the
  debugger owns the system, including at a budget pause (core ADR 0083).
  Inspection changes no execution state.
- **Reach a microstep boundary:** when paused inside a microstep, drive
  only that actor until its current microstep finishes, then return before
  another microstep or actor starts. A budget pause may require several
  calls. Ownership remains with the debugger between calls, so the pump
  cannot resume execution ahead of an abandon command.
- **Abandon:** apply core ADR 0082 between completed microsteps, after
  bootstrap has established a legal configuration. Inside a microstep it
  refuses without mutation. It releases the current input, internal queue
  and continuation, keeping the configuration after the last completed
  microstep. The macrostep's unpublished outgoing mail is freed, as
  [the envelope rules](envelope.md#mail-between-local-sessions) require.
  Data changes, history and effects already performed remain performed.
  A begun macrostep ends with `c2h.macrostep_ended(status=abandoned)`.
- **Transition:** on an actor available for delivery, supply an event,
  source and target states to the existing operator-transition semantics
  (core ADR 0082). Core validates the transition and takes it through its
  normal exit/history/entry and invocation machinery. Eventless and
  internal transitions then run normally. Debugger stepping may pause that
  macrostep; detachment may return its continuation to the pump.

Up to `maxConcurrentInputs` macrosteps can be in progress, one per slot
([macrostep.md](macrostep.md#macrostep-slots)). The debugger can inspect
and drive each of them. A transition on an actor that holds no slot needs
a free one; if every slot is busy, the debugger must finish a macrostep, or
explicitly abandon it at a legal boundary, first. An actor that holds a
slot must have that macrostep finished or abandoned before its
transition. Reaching
a boundary does not guarantee termination: a synchronous action that never
returns cannot be interrupted by these primitives.

The debugger owns values through the same native binding integrations as
the production driver. Transition data still obeys retain/release rules.
The debugger driver drains c2h and tel using the normal rules, releasing
values and delivering observations to the host's recording/effect handlers.
Effects may execute on receipt, and c2h capacity failures retain their
normal meaning. A debugger does not undo sends or bypass bounded memory.

The host returns debugger command results over its debugger connection,
without admission into h2c or a correlated debugger result on c2h. Macrostep ends
and effects caused by debug execution still use c2h. Exact input-observation
and replay representation for operator transitions remains an open question:
an ordinary event log alone does not describe the forced transition. The
host records the intervention in its debugger audit trail; a stable
post-intervention checkpoint can seed future execution without replaying
the intervention itself ([recording-and-replay.md](recording-and-replay.md)).

## A recovery session

This illustrative chart waits in `waiting_for_receipt` for an external
provider reply, with a `completed` state whose entry handlers finish the
work. It is not the campaign stress-test chart, which already has timeouts.

1. The host suspends the production pump after a core call returns, then
   attaches the debugger. All actors in this system stop automatic execution.
2. The debugger inspects the target. If it has a macrostep in progress,
   the debugger first reaches a legal microstep boundary and abandons it.
   It inspects again after doing so.
   A macrostep belonging to another actor must also finish or be explicitly
   abandoned before the target can run.
3. The operator checks the provider's delivery status outside core and
   confirms delivery. Core remains under debugger ownership during this
   check, so no actor changes the inspected configuration.
4. The debugger delivers an operator event identifying the incident, with
   source `waiting_for_receipt` and target `completed`. It drives that
   actor's macrostep through the normal engine and drains effects and its
   macrostep end report. It does not run other actors or admit normal inputs.
5. The host records the intervention and, if its recovery policy requires
   it, checkpoints the stable result. Detachment returns ownership to the
   pump, which resumes queued work and ordinary input admission.

## Scope and actor-only alternatives

No arbitrary memory writes, data edits, effect substitution or rewind is
part of this design. Transition handlers supply the changes a recovery
needs, and abandonment supplies a legal point from which to transition.

Freezing the system preserves every slot's continuation and internal
queue. Parking already keeps one actor's macrostep aside while others run
([macrostep.md](macrostep.md#parking-one-running-workspace-and-a-park-area)).
A debugger that holds one actor and lets the rest of the system run would
build on it, but it is not part of this design: the initial debugger
freezes everything. It is also how an operator looks at a live actor now
that quarantine is removed ([macrostep.md](macrostep.md#runaways-the-chain-limit)). An actor-only hold between macrosteps would need rules
for pending inputs and timers. Separate
systems can already be debugged independently.

# `invoke.h`

Each active invocation records:

- invoke ID;
- invocation type;
- host service handle;
- autoforward flag;
- active/cancelled state.

The invoker vtable starts, cancels, and forwards events to a logical external service.

The service handle is runtime-only. A session image records the logical active
invocation, including its invoke ID, type, and autoforward state, but it does not
retain, serialize, or rebind the service handle.

For a statechart child actor, the service handle can resolve to another
`ta_actor_id`. Parent/child communication still uses ordinary events and
origin/invoke metadata. Normal child completion sends `done.invoke.<id>` to the
parent's **external** event flow. Cancellation on parent-state exit runs the
child's active-state `onexit` handlers in exit order before calling the
invoker's cancellation hook, but does not manufacture a normal completion
event. Events the child emits after its parent has retired the invocation are
discarded.

Active invocation bookkeeping is persistent session state. Whether an external service itself can survive process restart is a contract of that invoker/store integration.

## Asynchronous reports

`start()` only reports whether the synchronous attempt to initiate a service
succeeded. It must not call `ta_actor_report_invoke()` reentrantly. After
`start()` returns, the invoker retains the system, parent actor, and invocation
IDs from `ta_invoke_request` and reports one of three outcomes through
`ta_actor_report_invoke()`:

- `TA_INVOKE_REPORT_EVENT` validates that the invocation is active, retains the
  supplied event, gives it external event type, marks it as originating from the
  invoked service, sets its envelope `invoke_id`, and submits it through the
  actor's external-event flow. The invocation remains active.
- `TA_INVOKE_REPORT_DONE` marks the runtime service inactive and submits
  `done.invoke.<id>` with optional final data through the external-event flow.
  The completion event carries the invocation ID because it represents a report
  from the invoked child. Enough invocation metadata remains until the event is
  processed so `<finalize>` can run; the record is then removed.
- `TA_INVOKE_REPORT_ERROR` (ADR 0081) appends
  `error.communication.invoke.<id>.<reason>` to the internal queue, where
  `reason` is one event-name token the host chooses. The error has
  `TA_EVENT_PLATFORM` type, `TA_ORIGIN_NONE` origin, and `TA_INVOKE_ID_NONE`
  in its event envelope. Optional `ta_invoke_error.data` becomes event data.
  With `ends`, the invocation is retired once the event is queued (a lost
  service is `unavailable` with `ends`); without it, the invocation stays
  active (napi reports a quarantined child as `quarantined`).

Ordinary-event and completion reports may return `TA_ERR_BUSY`, because the
core has no unbounded external mailbox; the host retains and retries them after
the current macrostep. An error report is appended even while an actor is inside a
macrostep, so it never returns `TA_ERR_BUSY`, subject to the normal
external-synchronization rule. A report for an
unknown, completed, cancelled, or retired invocation returns `TA_ERR_NOT_FOUND`
and generates no chart event.

All pointers in `ta_invoke_report_params` are borrowed for the duration of the
call. On any result other than `TA_OK`, the core has not accepted the report and
does not change the invocation record; a retry is therefore safe where the
result permits it.

The system pointer remains valid only for the invocation lifetime. System
destruction cancels active invocations before releasing the system; an invoker
must not report after cancellation or destruction completes.

Once an invocation becomes unavailable, late reports are ignored, state exit
does not call `cancel()`, and no `done.invoke.<id>` event is generated. A
synchronous `start()` communication failure performs the same unavailable
transition without first installing an active invocation record. Expression and
argument-evaluation failures remain `error.execution` errors.

## Reconstruction

Reconstruction never restarts or rebinds an active invoked service. For each
active invocation recorded in the image, it:

1. restores the actor without an active runtime invocation record for that
   service;
2. appends an `error.communication.invoke.<id>.unavailable` platform event to
   the internal queue after all events already present there;
3. leaves the event's envelope `invoke_id` set to `TA_INVOKE_ID_NONE`; and
4. ignores any later event attributed to that no-longer-active invocation.

Lost invocations are processed in their captured record order. They do not emit
`done.invoke.<id>`, and exiting the owning state does not attempt to cancel the
discarded runtime handle. If the chart does not handle the communication error,
it remains in the restored configuration without the invoked service until an
ordinary transition exits and later re-enters the invoking state.

This is a statechart-visible communication failure, not a failure of
`ta_actor_reconstruct()`. In particular, an active invocation does not cause
`TA_ERR_NOT_RECONSTRUCTIBLE`.

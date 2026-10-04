# ADR 0097: Event data leaving a session is a copy

**Status:** Accepted and implemented (2026-09-30); see the implementation notes. Refines ADR 0050: envelope data is still shared by retain, but a value leaving a session is first replaced by the data model's copy of it. Section 4 refined by ADR 0102 (2026-10-04, implementation pending): `TA_OP_NOMEM` goes; `copy_out` answers `TA_OP_OK` or `TA_OP_ERROR`.
**Date:** 2026-09-30

## Context

SCXML C.1 says of the SCXML Event I/O Processor that the data of an event
sent to another session MUST be a copy: a change the receiver makes to it
never shows in the sender, and the reverse. Section 6.2 says a `<send>`
evaluates its data when it runs, so what the receiver gets is the data as
it was then.

Core does not copy anything. ADR 0050 made retain the only way core shares
a host value and removed `ta_value_clone`, because no adapter's `clone`
ever copied, and because a value-level clone cannot know what a copy of a
data model's value is. It kept one rule: copying is a data-model
operation, as `<foreach>`'s shallow copy already was (ADR 0033,
`foreach_snapshot`).

So a data model with mutable values shares them between sessions. napi's
JavaScript data model shares objects by reference (napi ADR 0011), and so
does the test host's ECMAScript data model, whose design notes list the
aliasing as a known limitation. A chart that sends an object and then
changes it changes what the receiver holds, even while the event still
waits in the receiver's queue. This is gap G4 of the
[distributed-systems proposal](../../../docs/proposals/distributed-systems.md):
a MUST that every setup breaks, not only a distributed one. Across a
process boundary encoding copies anyway; inside one process nothing does.

Only the data model knows what a copy of its values is, and only core
knows when a value leaves a session.

## Decision

1. **A data-model operation, `copy_out`.** It gets a value about to leave
   the session and answers a value the session can no longer change: a
   copy, or the value itself, retained, when nothing the session does can
   change it. Core then sends that value in place of the original, which
   it releases.

   ```c
   typedef struct { void *shared; void *instance; const ta_value *value;
                    const ta_dm_host *host; ta_value *out; ta_fault *fault; }
       ta_datamodel_copy_out_params;
   TA_OPTIONAL ta_op_status (*copy_out)(void *self, const ta_datamodel_copy_out_params *params);
   ```

   It is optional. A data model without it says its values cannot change,
   so sharing one is a copy: core retains, as it did before.

2. **Where a value leaves a session.** Core calls `copy_out` on:
   - **the data of a `<send>` to any target but `#_internal`:** another
     session by URI, `#_parent`, `#_<invokeid>`, a named actor, the
     sender itself, and an I/O processor the host registered. It is
     called once the send's every attribute is evaluated (6.2's order),
     before the send is committed, whatever the delay. Data from
     `<content>`, `expr` or params and namelist alike;
   - **an `<invoke>`'s params (and namelist) and its content `expr`:** what
     the invoker receives in `ta_invoke_request.params` and `.data`, for a
     child session and a host service alike;
   - **an autoforwarded event's data,** once for each invocation it is
     forwarded to.

   A send to self is copied too. The event waits in the external queue,
   outside the macrostep, and the session may change the value before it
   is taken; 6.2 says the event carries the data as it was when the send
   ran. One rule for every external target is also simpler than one per
   target, and core does not always know a target is the sender itself
   before the router resolves it.

3. **What is not copied.**
   - **`#_internal` and `<raise>`.** The event stays in the session and
     is taken within the same macrostep. C.1 is about sessions; nothing
     leaves.
   - **Done data.** A session's `<donedata>` is evaluated as it ends, and
     it runs no code afterwards, so nothing can change the value on its
     side: sharing it with the parent is a copy. The same holds for the
     completion a host reads (`ta_actor_get_completion`).
   - **A `targetexpr` value** a host I/O processor receives: it is an
     address, not event data.
   - **Error events.** Their data is a fault the session itself receives.
   - **What a host puts in** (`ta_actor_deliver`, a service's events,
     `ta_actor_report_invoke`): a value that enters a session did not
     leave one, and whether the host keeps a reference is the host's
     business (ADR 0006).

4. **Failure.** `TA_OP_NOMEM` means nothing changed: the step runs again
   (ADR 0070). A `<send>` is still in its evaluation, so it runs again from
   the start; an `<invoke>` is started again; autoforwarding resumes at
   the invocation it stopped at. `TA_OP_ERROR` is `error.execution` with
   the data model's fault: a `<send>` is then not sent and the error
   carries its send ID (5.10.1), an `<invoke>` fails as any error in
   evaluating it does, and an autoforward skips that invocation, while
   the others still get the event.

5. **Replay copies too.** A replayed send goes nowhere (ADR 0089), but a
   copy can fail, and a failure changes what the chart does next
   (`error.execution`). So the replay makes the same calls as the run it
   repeats.

6. **The data models in this repository.**
   - Core's null data model makes no values, so it has no `copy_out`.
   - The test host's jq data model has none either: a `jv` is a persistent
     value, and assigning builds a new one when the old is shared, so
     sharing is already a copy.
   - The test host's ECMAScript data model copies with QuickJS's structured
     serialization (`JS_WriteObject`, then `JS_ReadObject`).
   - napi's JavaScript data model copies with a copier of its own; napi
     ADR 0039 says what it copies and what it shares.

## Consequences

- C.1's copy holds for every data model that implements `copy_out`, and
  across a process boundary nothing changes: encoding copied there before.
- The cost is where the data is: an event without data calls nothing, and
  a data model decides how cheap its copy is. For immutable values
  (primitives, strings, `jv`) a copy is a retain. Core allocates nothing
  for it, and no per-actor or per-event byte changes.
- A value that cannot be copied is now an error where it was shared
  before, if the data model says so. The data models here share what they
  cannot copy instead (a function, an object nothing says how to copy), so
  no chart that worked before fails.
- A data model with mutable values that does not implement `copy_out`
  still shares, and breaks C.1 as before.

## Implementation notes

- **Where:** `include/tinyactors.h` (`ta_datamodel_copy_out_params`, the
  `copy_out` member), `src/interpreter.c` (`ta_interpreter_copy_out`, and
  the call at the end of `ta_send_evaluate`), `src/invoke.c`
  (`ta_invoke_resolve_values` for params and content expr, and
  `ta_invoke_forward`, which now copies the event before either the child
  or the invoker's `forward` gets it).
- **Cost:** core adds one test per value leaving a session (absent data
  and a data model without `copy_out` call nothing) and allocates nothing.
  napi's copy is the cost, measured with `bench-compare d795ce9` and
  recorded in napi ADR 0039: sends without data or with a primitive, and
  the tracing and input-log benches, are within 1.5%; an actor-to-actor
  send whose data is `{ by: 1 }` is 14.2% slower on Node and 11.2% on Bun,
  one with three nested containers 39.9% and 35.6%. Bytes per actor and
  allocator calls per event do not change.
- **Tests:** `tests/copy_test.c` (a data model of mutable boxes): a send
  to self is copied and a mutation after it does not show, `#_internal`
  is not copied, a data model without `copy_out` shares, a refused copy is
  `error.execution` and nothing is sent, `TA_OP_NOMEM` runs the send again,
  an invocation's params and content and an autoforwarded event are
  copied, and a refused invoke copy fails the invocation. It runs under
  AddressSanitizer too. The test host's ECMAScript scenarios (a send to
  self; a child's `#_parent` send), napi's `test/copy.js`, and the W3C IRP
  suite on the addon (176 of 176 on Node and Bun).

### The cost is accepted, and a copy can be cheap (2026-09-30)

The owner accepted napi ADR 0039's cost for the JavaScript data model.
`copy_out` is the data model's to implement, and it need not duplicate
anything: a data model whose values cannot change after they are made
(the test host's jq values, a persistent structure, a value frozen for
good) satisfies C.1 by answering the same value with its reference count
raised. The copy the spec asks for is one the receiver cannot observe
changing, not a new allocation. A data model whose values are mutable pays
for a real copy, and may optimize it later (napi ADR 0039).

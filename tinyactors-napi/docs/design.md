# Tinyactors Node-API binding design

## Status and scope

This document records the design for the Node-API binding. The current module
is only a native loading and packaging stub: it exports its package `version`
and does not yet link Tinyactors core.

The binding targets Node.js 22 or newer and Bun. Its correctness boundary is
the stable Node-API C interface. It does not depend on V8, JavaScriptCore,
`node.h`, or direct access to a libuv event loop.

## Runtime ownership

JavaScript explicitly creates and closes each system. A system, its actors,
definitions, queues, and retained JavaScript values belong to the JavaScript
environment and thread that created it. No `ta_system` call may run
concurrently, and a system cannot move between Workers.

The native `System` wrapper will own:

- one `ta_system`;
- its in-memory actor directory;
- its external-message and runnable-actor queues;
- definition and actor handle tables;
- its data-model adapter and retained JavaScript values;
- pending message completions; and
- scheduler state and execution budgets.

There is no process-global system registry. Each Node.js or Bun Worker has its
own Node-API environment and creates independent systems.

## JavaScript definitions and data model

Definitions are authored as JavaScript object trees corresponding to the
Tinyactors SCXML specification structures. The binding normalizes each tree
and compiles it synchronously. Structural data and strings are copied during
compilation. JavaScript expression functions are represented as
`TA_EXPRESSION_HOST` values and retained for the lifetime of the compiled
definition.

Expressions are functions or explicit operation descriptors, not source text:

- values and conditions are functions;
- locations provide getter and setter functions;
- scripts are synchronous functions; and
- the call context provides the current event, session identity, `inState`,
  and the other SCXML system values.

Data-model functions must not return promises. A Tinyactors macrostep cannot
suspend inside an expression. Asynchronous work is initiated through an I/O
processor or invoker and reports completion as a later external event.

Definitions used for reconstruction have explicit family and revision values.
The binding does not derive a durable revision from `Function#toString`, since
function text does not identify captured closure state.

## JavaScript value lifetime

A `ta_value` is backed by a native box containing the owning system, one strong
`napi_ref`, and a native reference count. Retain increments that count. Release
deletes the Node-API reference when the count reaches zero. Clone shares the
same box instead of structured-cloning the JavaScript value.

The system therefore owns the value's lifetime, not exclusive mutation rights.
If application code mutates an object after sending it, a later actor may see
that mutation. An optional higher-level copy policy can call `structuredClone`
before submission, but copying is not the native binding's default semantic.

All reference operations and system destruction occur on the owning JavaScript
thread.

## Queues and scheduling

The host owns external scheduling. The binding uses two sparse native queue
layers:

1. External envelopes contain calls to `send`, normalized inter-actor
   deliveries, and asynchronous I/O or invocation completions. An envelope
   retains its JavaScript data.
2. The runnable queue contains actors whose macrostep has started but paused at
   an execution budget. A paused actor remains runnable until it becomes
   stable, done, or erroneous.

External mailboxes are allocated only for actors that have queued messages.
This avoids permanent per-actor mailbox overhead while also avoiding global
head-of-line blocking when one target is paused.

The external-event router copies every borrowed `ta_external_delivery` into the
host queue and returns accepted. It never recursively delivers to an actor.
Calls made from data-model, I/O, invoker, or logging callbacks may enqueue work
but may not re-enter the Tinyactors interpreter.

One bounded pump turn:

1. resumes runnable actors for a configured budget;
2. delivers queued external events only to stable actors;
3. dispatches due delayed sends with a configured callback budget; and
4. schedules another turn when work remains.

Message promises resolve according to an explicitly documented mode: queue
acceptance or completion of the message's macrostep. Queue depth and retained
data size limits provide backpressure. Closing a system rejects queued and
pending messages.

## MessageChannel and Workers

A private `MessageChannel` may provide the scheduler wake-up. The real
envelopes stay in the native retained-reference queues. Enqueueing work posts a
single coalesced token when no pump is already scheduled. The port callback
runs one bounded pump and posts another token if work remains.

The channel does not carry actor data. `postMessage` uses structured cloning,
which would lose object identity, reject functions, change prototypes and
accessors, and provide no portable queue-depth or backpressure control. Posting
only opaque IDs would still require a retained-value table and deterministic
cleanup, which is the native queue under another name.

The scheduler must not keep an otherwise idle process alive. Its ports are
unreferenced where the runtime supports that operation and are closed when the
system closes.

An optional Worker-hosted facade may use Worker `postMessage` as its ingress
transport. That is a distinct mode with structured-clone semantics: the real
system and definitions live inside the Worker, functions are registered there,
and message IDs plus acknowledgements provide completion and backpressure.
Local actor-to-actor traffic still uses the native queues.

## Logical time

The JavaScript layer supplies monotonic milliseconds, normally relative to
`performance.now()`. After a pump it reads the next Tinyactors deadline and
arms one JavaScript timer. The timer advances logical time, dispatches due
sends, and wakes the pump. A manual-clock mode supports deterministic tests.

This deliberately avoids `napi_get_uv_event_loop` and direct libuv timers so
the same design works under Node.js and Bun.

## I/O, invocation, and logs

The first implementation will provide an in-memory actor directory and an
SCXML invoker that creates child actors in the same system. Application I/O
processors and invokers are JavaScript callbacks. They may start asynchronous
work, but their promise completion re-enters the system only by enqueueing an
event or invocation report.

Log callbacks are buffered until the current core call returns. This prevents
logging code from re-entering the interpreter.

Durable virtual actors are a later layer. Checkpoint storage, binding locators,
unloaded-actor deadlines, and fencing are host infrastructure rather than
requirements of the initial in-process binding.

## Errors and teardown

Binding misuse and C API failures become thrown exceptions or rejected message
promises. Exceptions from chart expressions are captured as fault data so the
core can produce the corresponding `error.execution` event.

`System.close()` is explicit, idempotent, and authoritative. It stops new
messages, cancels the scheduler timer and wake-up channel, detaches external
services, rejects pending messages, destroys the `ta_system`, releases all
definition callbacks and JavaScript values, and invalidates subordinate
handles.

The JavaScript wrapper finalizer and Node-API environment cleanup hook are
safety nets. Cleanup is ordered through the owning system; independent actor,
definition, and value finalizers must not race environment teardown.

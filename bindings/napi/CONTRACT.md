# Binding implementation contract

The C API is unchanged. The addon uses raw stable Node-API (version 8), C++17
glue, and the C99 runtime. Host glue may use libc/STL; core remains freestanding.
Every function we define has at most three parameters (Node-API calls themselves
have their platform signatures). Do not add definition construction macros.

## Low level

The addon exports exact `ta_*` function names plus compiler-visible constants.
Struct fields retain C snake_case names. Arrays are JavaScript arrays: the bridge
derives counts and appends C sentinels. Text slices become strings (null/undefined
means absent). uint64 identities, revisions, time and code tokens use BigInt;
safe integer Numbers are also accepted as inputs. Size/index/status/enum values
use Numbers. Overflow, unsafe integers and wrong handle kinds throw TypeError or
RangeError before invoking the C runtime. Ordinary C failures return status.

Functions returning status and out parameters return `{status, <output>}`:
arena create/open -> `arena`; alloc -> `memory`; system create -> `system`;
definition prepare/build/lookup -> `definition` (build also `error`);
spawn/name lookup/session lookup -> `actor` (BigInt); post/context send ->
`message_id`; actor inspect -> `info`; run -> `result`; io_next -> `request`;
context_bytes -> `bytes`; context_arena -> `arena`; invocation_is_cancelled ->
`cancelled`. Functions with status but no output return a status Number. Void
query functions return their output directly. Void mutations return undefined.
`ta_system_config_init()` returns defaults. `ta_text_from_cstr` is a string helper.

Opaque arena/system/definition/context handles are branded JS objects, never
numbers or public pointers. Definitions belong to exactly one system. Context
handles expire when their callback returns. Arena allocations are safe opaque
memory handles, with copying `memoryRead(memory)` / `memoryWrite(memory, bytes)`
helpers; do not expose dangling external buffers. Owned handles have explicit
close/release and GC fallback; environment cleanup releases native resources.
Context bytes are copied Buffer views; an explicit context byte-write helper
commits changes. This avoids unsafe escaped pointers.

Native state/value roots wrap real JS values in strong napi_refs (boxing
primitives). Native payload is `{kind: TA_PAYLOAD_NATIVE, native: value}`; byte
payload is `{kind: TA_PAYLOAD_BYTES, bytes: Buffer/Uint8Array}`. Data views use
`{storage, native}` or `{storage, bytes}`. `_event` uses tagged `data` payload.
Body is `{kind, content?, parameters?: [{name,value}]}`. Target is
`{kind, instance?: BigInt, text?: string}`. Actor/request/invocation refs are
BigInts at the JS boundary, not `{id}` wrappers.

System config supplies `data` callbacks matching C names and argument order
except user/out parameters: `create(context,seed)` returns state;
`evaluate(context,code)` / `read_location` return a JS value;
`test(context,code)` returns boolean; `run(context,kind,code)` and other effect
callbacks return undefined or a status; `bind_system(context,values)` and
`bind_event(context,event)` are optional no-ops. `to_text(context,value)`,
`from_text(text)`, `to_delay(context,value)`, `evaluate_bytes(context,code)` return
their output. `iteration_open(context,code)` returns `{snapshot,count}`;
`iteration_bind(context,step)`, `pack_body(body)`, `finalize(context,call)`,
`log(record)` mirror the C callbacks. `create` defaults to cloning seed or `{}`.
`snapshot(value)` defaults to captured `structuredClone`, never mere retain.
Callback throws are caught/cleared at the C boundary, recorded, and become
TA_EXECUTION_ERROR. Promise returns from synchronous callbacks are errors.
`takeErrors(system)` drains recorded callback exceptions. Trace errors must not
change transitions. Do not invoke JS from GC/environment cleanup.

`ta_context_inspect(context)` returns the C view plus `code_owner` (the actual JS
object rooted by the current definition). This supports definition-local code
tables without leaking every old revision in a system-wide callback map.

Normalized descriptions include retained `inline_definition` handles alongside
diagnostic `inline_revision`/`inline_description` fields. They keep child
definitions usable in a subsequent prepare call; explicit release or GC drops
the returned caller references. No user-supplied sentinel objects are accepted
in JavaScript arrays. Input traversal and output description nesting are bounded.

## Ergonomic layer

CommonJS package root (with ESM named exports wrapper) exports `defineMachine`,
`createSystem`, and action helpers. `/lowlevel` exports the exact C mapping.
No eval/string-expression engine. Define machines as ordinary nested JS objects:

    const entity = defineMachine({
      id: 'entity', state: ({input}) => ({x: input.x ?? 0}), initial: 'idle',
      states: { idle: { on: { MOVE: { actions: ({state,event}) => {
        state.x += event.data.dx;
      } } } } }
    });
    const system = createSystem();
    const actor = system.spawn(entity, {name: 'player', input: {x: 10}});
    actor.send({type: 'MOVE', data: {dx: 2}});
    system.run();
    actor.snapshot();
    system.inspect(snapshot => { /* consume/copy one actor at a time */ });
    system.close();

Ergonomic definitions support nested/parallel/final/history states, on/always,
entry/exit blocks, guards, action functions, send/raise/cancel, invocation and
done data. Compile functions into a per-definition `code_owner` table and code
tokens; generic data hooks dispatch using context view `code_owner`.
State factory runs per actor. Posted native data is structured-cloned by the
bridge; snapshots clone mutable state before returning it to application code.
Callbacks receive `{state,event,self,now,in,send,raise,cancel,...}` with native
context lifetime checks. Keep default scheduling deterministic/manual;
offer explicit start/stop of bounded Node scheduling. Time inputs are ns BigInt.
Actor refs retain incarnation identity; named sends resolve current bindings.

The convenience layer may throw useful typed errors for failed C operations.
It must expose normal scheduler progress, limits, IO leases/drivers, upgrade,
publication and streaming inspection without hiding backpressure. Async I/O
drivers run outside native callbacks and complete leases exactly once.

Root owns native shared contracts, build glue, integration and SQLite. Agents
must not edit each other's files or the ledger. Root reviews actual assertions
and rebuilt binaries before accepting a task. No npm publishing/network service.

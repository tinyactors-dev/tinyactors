# AGENTS.md: tinyactors-cpython

The Python extension `tinyactors`: one C extension module, written against
the CPython C API, that drives `ta_host`. It is being rewritten from
scratch (`6426816a` removed the first one; [ROADMAP.md](ROADMAP.md) says
where the work is). This file is the rulebook the rewrite starts from: it
holds the binding's rules, true at every step. The repository root's
`AGENTS.md` applies too.

Read first, in this order:
[`tinyactors-core/docs/embedding.md`](../tinyactors-core/docs/embedding.md)
(how to use the core: the loop, lifetimes, memory, a complete loop in C),
[`tinyactors-core/docs/bindings.md`](../tinyactors-core/docs/bindings.md)
(the guide for a binding),
[`tinyactors-core/docs/boundary.md`](../tinyactors-core/docs/boundary.md)
(the contract of every host call), [ADR 0012](../docs/adr/0012-lifetimes-follow-the-loop.md),
and the public headers. Build from those, not from the removed binding or
the proposals' sketches.

## Whom it serves

First the **application developer writing Python**: chart logic in Python
(the native `"python"` datamodel), a `Host` driven from a script or from
asyncio, values that are Python objects. Then the **binding author**, held
to the guide by the conformance suite, and the **maintainers**, who run one
suite against every implementation. Every API item is a host input, a
typed method over a posted message, or the handling of an effect
(`docs/api-map.md`); what core does not have is not in the API.

## Names

Our names carry the prefix `ta_python_*`: every function this binding
defines or declares, static helpers included, and its types and globals.
A platform's name appears only where the platform requires it: the module
init function `PyInit_tinyactors` (CPython's import finds it by that name,
PEP 489), a slot such as `Py_tp_traverse` filled with our
`ta_python_host_traverse`. When you write a name in prose, say whose it
is. The lint rule `functions-carry-the-prefix` checks functions. Python
names follow the root's acronym rule: constants in capitals, methods
snake_case (`session_id`).

## The API

Decided by the maintainer (2026-10-13); every item is in
[`docs/api-map.md`](docs/api-map.md) with the core behind it. What core
does not have yet is marked [8.2] (root `ROADMAP.md`, step 8.2).

- **`tinyactors.Host(config, io_processors={type: handler}, log=handler,
  clock=None)`.** Everything is per host: the config (a `dict`, written
  as CBOR into the call's arena for `ta_host_create`), the I/O
  processors' handlers, the log handler and the clock. No global
  configuration (no `logging.basicConfig`, no module-level handler, no
  default logger). The extension reads `host.config` once at creation for
  `system.ticksPerSecond` (bindings.md section 3).
- **`with Host(...) as host`** (the synchronous driver) and **`async with
  Host(...) as host`** (asyncio) on the same class. Leaving either frees
  the host (`trace.end`, the last take of traces, `ta_host_free`); pending
  response handlers are dropped, never called.
- **`host.add_image(charts)`**: compiles the charts (`ta_compile_charts`),
  passes the image's bytes to `ta_host_add_image`, which copies them, and
  frees the compile result at once (`ta_compile_free`). The form of
  `charts` and the return value are settled at the API review.
- **`host.spawn(chart, name=None, params=None) -> int`**,
  **`host.send(session, event, data=None)`**,
  **`host.terminate(session)`**: applied at the call (`ta_host_chart` and
  `ta_host_spawn`, `ta_host_send`, `ta_host_terminate`), read during it;
  a payload moves to core on success and is released on refusal. Under
  asyncio an input made outside a turn schedules one turn.
- **I/O processors: `handler(host, send)`**, called once per
  `TA_EFFECT_SEND` whose `IO_PROCESSOR` is that type's index in the
  config's `ioProcessors`. Actor output to the application goes through
  them, and nothing else (ADR 0012, decision 6).
  - `send.answer(event, data=None)`: `ta_host_send` to the sending
    session (`SESSION`) with the send's `sendid` and `origintype` the
    processor's type.
  - `send.fail()`: `ta_host_send` with `TA_EVENT_IN_ERROR_COMMUNICATION`
    and the `sendid` (`tinyactors-core/include/tinyactors/system.h:155-157`:
    core places `error.communication` in the sender's queue); the event
    name is not repeated.
  - A plain function gets a **thin view** (below, "Thin views"); an
    **`async def`** handler is scheduled as a task and gets a
    **materialized record**, made before it is scheduled. A processor
    configured `sync` stops the run until its send is answered: its
    sender waits until then (`host.h:215-228`), so an `async def`
    handler may answer it later.
- **`log=handler(host, entry)`** gets this host's `<log>` lines
  (`TA_EFFECT_LOG`) as thin views with `chart`, `session`, `label` and
  `text`. Without one, the lines are discarded.
- **Typed methods over post**: `actor_state`, `actor_context`,
  `actor_list`, `actor_get`, `host_stats` and `trace_*` (one per method of
  `tinyactors-core/docs/requests.md`), each `(…, handler, on_error=None)`,
  returning `None`. The handler `(host, result)` runs when the method's
  `REPLY` is dispatched, in a later drain; `on_error(host, error)` gets a
  `TinyactorsError(code, reason, path, message)`; an error with no
  `on_error` is raised by the driver after the turn. A refusal at the
  call (`ta_host_post`: `TA_ERROR_NO_MEMORY`, `TA_ERROR_STATE`) raises at
  once. No synchronous request, no raw `post`, no reply object, no future.
- **The message id is the handle of a row in the pending table**
  (`ta_table` [8.2]) holding `(handler, on_error)`: the drain reads the
  `REPLY`'s `id` in C, finds the row with `ta_table_get` (O(1); the
  generation refuses a stale id), removes it and calls the handler. At
  `ta_host_free` the remaining rows are dropped and their handlers never
  called.
- **`DONE` and `ERROR` are internal** (the rows of the session are
  released); **`PEER` and `PEER_ERROR` are deferred**; no notification is
  defined (`actor.subscribe` and `actor.changed` are removed, ADR 0012
  decision 4 as amended again), and the extension drops a
  `TA_HOST_EFFECT_NOTIFICATION` whose method it does not know.

## The loop

The extension runs embedding.md's loop (section 2) in C: the guide's two
functions (bindings.md section 3) with our names. The comments carry the
rules:

```c
static void ta_python_drain(ta_python_host *host) {         /* one batch at a time [8.2] */
    const ta_host_effect *e; uint32_t n;
    while ((n = ta_host_effects(host->h, &e)) > 0) {
        for (uint32_t i = 0; i < n; i++) ta_python_dispatch(host, i);
                                                            /* handlers may call the host:
                                                               inputs apply now, effects come next take */
        ta_host_effects_ack(host->h);                       /* everything handed out for it ends */
        host->acks++;                                       /* this batch's thin views are stale from
                                                               here: core keeps no batch counter */
    }
}

/* one turn: returns what the driver does next */
static ta_python_next ta_python_turn(ta_python_host *host) {
    ta_python_drain(host);                                  /* what inputs and posts appended:
                                                               a pending reply needs no run */
    ta_host_advance_time(host->h, ta_python_now(host));     /* ta_host_time: ticks at host.config's
                                                               rate, wall ns */
    for (;;) {
        ta_run run = ta_host_run(host->h, TA_PYTHON_BUDGET); /* bounded: fuel per call */
        ta_python_drain(host);
        switch (run.status) {
        case TA_RUN_EFFECTS:                                /* a sync I/O processor's send: answered in
                                                               the drain, or its sender waits */
        case TA_RUN_EFFECTS_FULL: continue;                 /* run again now */
        case TA_RUN_YIELDED:      return TA_PYTHON_NEXT_YIELD; /* budget spent, actors still runnable */
        case TA_RUN_IDLE:         host->deadline = run.next_deadline; /* UINT64_MAX: none */
                                  return TA_PYTHON_NEXT_IDLE;
        case TA_RUN_EXHAUSTED:    host->pool = run.detail;  /* a TA_POOL_* */
                                  return TA_PYTHON_NEXT_EXHAUSTED;
        case TA_RUN_FAULT:        host->fault = (ta_status)run.detail; /* negative: ta_status_name */
                                  return TA_PYTHON_NEXT_FAULT;
        default:                  host->fault = TA_ERROR_UNSUPPORTED;  /* a status unknown here */
                                  return TA_PYTHON_NEXT_FAULT;
        }
    }
}
```

`ta_run` and the statuses are `tinyactors-core/include/tinyactors/system.h:196-218`,
`ta_host_time` is `host.h:205-208`. `ta_host_run` refuses while a batch
is open (`TA_RUN_FAULT`, detail `TA_ERROR_STATE`, ADR 0012 decision 2;
[8.2]), which the drain before each run rules out. There is no nested
drain: Python code a handler runs may call the `Host` to add inputs, post
or read views; it never takes or acks.

| `ta_python_turn` returns | Synchronous driver (`run_until_idle()`, `run()`, `SimulatedClock.increment(ms)`) | asyncio (`await host.idle()`) |
|---|---|---|
| `TA_PYTHON_NEXT_YIELD` | check signals (`PyErr_CheckSignals`; a `KeyboardInterrupt` is raised between turns), then turn again | yield to the loop: `loop.call_soon(turn)` |
| `TA_PYTHON_NEXT_IDLE` | `run_until_idle()` returns; `run()` sleeps to `deadline` (converted with `system.ticksPerSecond`) and turns again; `increment(ms)` advances the simulated clock (`ta_host_advance_time`, the simulated time as the wall clock) and turns until idle, arming nothing | one `loop.call_at` timer at `deadline` (none at `UINT64_MAX`), replacing the last; `idle()` completes; an input from outside a turn schedules one turn with `loop.call_soon` |
| `TA_PYTHON_NEXT_EXHAUSTED` | raise `MemoryError` naming the pool; the host stays usable and the next turn goes on where the run stopped | the same, from the turn |
| `TA_PYTHON_NEXT_FAULT` | raise `TinyactorsError` with `ta_status_name(fault)` (the detail); not retried | the same |
| a handler raised, or a reply's error had no `on_error` | finish the turn (the ack comes anyway), then raise the first | the same |

- **Both clocks before every run**, one timer (the sync driver's sleep or
  one asyncio `TimerHandle`) at `next_deadline`. A host created with a
  `SimulatedClock` arms no timer.

## Lifetimes and thin views

ADR 0012: **everything the host hands out about a batch lives until the
ack** (effect fields, `TRACEPARENT`, envelopes, traces, logs, metrics,
hibernation and export blobs), across any later call into the host. A
datamodel's outputs live until its next call. Snapshots and the refused
config's error fill a `ta_buffer` the extension lends.

**Thin views.** Wherever C values are exposed to Python, the extension
hands out a thin view where it can, not an eager conversion, as the
datamodel does with `_event.data` (the row's value, not a copy). A view
is a small object holding the `Host`, the ack count when it was made, and
the effect's index; it converts a field when Python reads it, with the
accessors (`ta_host_effect_number`, `_text`), and caches it. A handler
pays nothing for a field it does not read.

- **Staleness:** the `Host` counts acks (`host->acks`, bumped after
  `ta_host_effects_ack` in `ta_python_drain`). Reading a view's field
  when the count differs, or after the host was freed, raises
  `TinyactorsError` with status `TA_ERROR_STALE` (`ta_status_name`, the
  status core gives a stale handle, `types.h:108`), and so do a stale
  send view's `answer()` and `fail()`. Core has no batch counter and needs
  none: only the extension acks. What a handler already read out of a
  view is a Python object and outlives the ack.
- **`send.data`:** for a payload of the `"python"` datamodel, the row's
  value itself (the payload is retained by core until the ack,
  `host.c:1187-1191`, `:1044-1048`): no conversion. Capture copied it for
  this send, so no actor holds it any more. For a quickjs payload,
  converted on first read: `ta_datamodel_ecmascript_payload_to_cbor`
  (a view valid until quickjs's next call) read at once with
  `ta_cbor_reader` into Python objects.
- **Materialized, and when:** an `async def` processor's body runs after
  the ack (the task starts when the loop next runs it, after the drain
  returns), so before scheduling it the extension builds a record:
  `session` (`int`), `sendid` (`str`), the processor's type (the `str`
  the extension holds from the config), `event`, `target`, `data`. Its
  `answer()` and `fail()` work any time while the host lives
  (`ta_host_send`'s refusal, a session that ended, raises). Not chosen:
  materializing every live view at the ack (a list of views per batch,
  and conversions of fields nobody may read); copying `session` and
  `sendid` into every view (a `str` per send, read or not). A plain
  function that wants to answer later reads the fields it needs and is
  written as `async def`, or answers before it returns.
- **Converted once, not viewed:** a reply's `result` is decoded with
  `ta_cbor_reader` when its `REPLY` is dispatched, and the handler gets
  Python objects. The trace: its `WIRE` lives until the ack; its one
  reader is the handler, which reads it whole and often keeps it
  (`host_stats` stored for a dashboard); a view of a nested CBOR map
  would need random access into bytes that end at the ack. The `id` is
  read in C only.

| Object Python sees | Owner | Created | Converts | Valid until | Thread |
|---|---|---|---|---|---|
| `Host` | the application | `Host(...)` | the config, into the arena, for `ta_host_create` | `with` exit, `close`, or the collector (a fallback) | the creating one; others refused |
| `send` (thin view) | the handler, borrowed | dispatch of a `TA_EFFECT_SEND` to a plain function | `session`, `sendid`, `event`, `target`, `data` on first read | the ack; then stale | the host's |
| send record | the task | dispatch, before scheduling an `async def` processor | every field, at once | the collector | the host's |
| log `entry` (thin view) | the handler, borrowed | dispatch of a `TA_EFFECT_LOG` | `session`, `label` (`AUXILIARY`), `text` (`EVENT`), `chart` on first read | the ack; then stale | the host's |
| a reply's `result` | the handler | dispatch of a `TA_HOST_EFFECT_REPLY` | the whole result, once | the collector | the host's |
| `TinyactorsError` from a reply | `on_error`, or the driver | the same dispatch | `code`, `reason`, `path`, `message` | the collector | the host's |
| a session id | the application | `spawn` | — (`int`, 64 bits) | — | — |
| `_event` (in a chart) | the session's instance | `bind_event` | the strings during the call (`datamodel.h:35-37`); `data` is the row's value, the Event holds a reference on the row | the next `bind_event`, `detach_event` or `destroy` | the host's |
| system variables (in a chart) | the session's instance | `create` | once, during the call (`datamodel.h:62-63`); immutable, shared by reference | the session's end | the host's |

## The host's allocator

The host allocates through the `ta_allocator` its environment gives it
(`tinyactors-core/include/tinyactors/allocator.h`), and this binding gives
it `PyMem_RawMalloc` and `PyMem_RawFree`, which are in the limited API
from 3.13 (CPython's `Include/pymem.h`, 3.13: lines 90-98), the wheel's
floor. The raw domain needs no GIL ("Memory allocator which doesn't
require the GIL to be held", the same file, line 91), which a
free-threaded build has none of anyway. Python's own objects stay outside
the host's allocator; the README says so (bindings.md, section 8). The
two functions take core's parameter lists (`void *context, size_t size,
size_t align` and `void *context, void *ptr, size_t size`), which the
lint exempts by that signature. `malloc`, `calloc`, `realloc` and `free`
stay refused by the layer lint.

**Alignment: `align` is at most 8, and `PyMem_RawMalloc` gives 16.** The
host passes 8 on every allocation it makes
(`tinyactors-core/src/host/host.c`, `host_alloc`), the largest alignment
a region object needs is 8 (`src/base/config.h`, `TA_ALIGN_MAX`), and the
allocator's contract says "a power of two, at most 8 from this library"
(`include/tinyactors/allocator.h:9`). A spike on CPython 3.13.16 (macOS
arm64) called `PyMem_RawMalloc` 800 times, sizes 1 to 100000, under
`PYTHONMALLOC` default, `debug`, `malloc` and `pymalloc`: the smallest
alignment seen was 16 in every mode. So the binding's `alloc` need not
align by hand (an `assert(align <= 8)` states the contract). A raw
allocator an application installs with `PyMem_SetAllocator` is not
covered by the spike (unverified).

## The binding's memory

The extension allocates almost nothing (bindings.md, section 6), and
what it allocates comes from the host's allocator through core's base
APIs [8.2]:

- **One `ta_growable_arena` per host** (`tinyactors/arena.h`, ADR 0012
  decision 5 as amended again), created with the `Host` and freed with
  it; its chunks are kept. Each API call takes a mark at entry
  (`ta_growable_arena_position`) and releases to it at exit
  (`ta_growable_arena_release`), on every path, exceptions included. A
  call made while another is running on the same thread (a `__iter__`,
  a getter, a `__del__` that calls the `Host`) allocates above the outer
  mark and releases to its own: nothing moves and the outer call's
  memory is untouched. It holds the call's CBOR (the config, a typed
  method's params, a payload for quickjs, `tinyactors.encode`), written
  by a `ta_cbor_writer` whose allocator is the arena's `ta_allocator`
  view; converted text; and the `ta_event_in` structs of a `send_many`.
- **Strings go to core as the `str`'s own UTF-8**, a view for the call
  (`PyUnicode_AsUTF8AndSize`; CPython keeps the UTF-8 in the `str` object
  once made, so a non-ASCII `str` pays that once), bytes through the
  buffer protocol.
- **The value table accepts exact built-in types** (`PyDict_CheckExact`,
  `PyList_CheckExact`, ...), so a conversion runs no user code. Open:
  whether a `dict` or `list` subclass is refused or converted (refusing
  keeps conversion free of user code; listed in [ROADMAP.md](ROADMAP.md)).
- **Tables** (`ta_table`, `tinyactors/table.h`), grown through the host's
  allocator, freed with the host; keep handles, never row pointers
  across a `ta_table_add`:

  | Table | Row | Added | Removed |
  |---|---|---|---|
  | charts | the chart's class, its compiled functions, its slot names | `chart_attach` | `chart_detach` |
  | sessions | the instance; live or hibernated | `create`, `deserialize` | `destroy`, unless the session was just serialized (it stays, hibernated) |
  | payloads | `{value, refcount}` | `capture`, `from_wire`, an input (`send`, `spawn`'s params, `answer`'s data), `migrate`'s bindings | refcount 0: the last reference is dropped after the outer call into core returns (deferred: dropping it can run Python code) |
  | pending | `{handler, on_error}` of a typed method | the method's call | its `REPLY`'s dispatch; `ta_host_free` (dropped, not called) |

- **The datamodel's last output.** `to_wire`, `serialize` and
  `payload_serialize` write with a writer the datamodel keeps, overwritten
  by its next call; strings handed back to core (`eval_string`, `log`)
  are the UTF-8 of a `str` the datamodel holds until its next call.
- **Payloads are copied, not encoded,** between the API and the
  `"python"` datamodel; CBOR only toward quickjs and in envelopes. A
  value the application passes in becomes a payload of the target
  session's datamodel (its chart's `datamodel`, `ta_image_chart_datamodel`).
- **Statuses are named by core:** a bare status in a message is
  `ta_status_name(status)` (`"TA_ERROR_STATE"`), never a table of ours.
- **Where the allocator is called:** in the host's create, free and
  allocator, and through core (`ta_table`, `ta_growable_arena` while it
  grows, the compiler's options); nowhere else, nothing per event or per
  evaluation. The lint rule `allocation-in-tables-and-host` checks it; it
  names files of the removed binding (`src/datamodel/handles.c`) and
  changes with the first commit of the rewrite.
- **Images are bytes:** `ta_host_add_image` copies them; free a compile
  result at once. No image rows, no `SEGMENT_FREED` bookkeeping.

## The "python" datamodel

Registered beside quickjs, first; matched by the chart's
`datamodel="python"`.

- **Per chart, once, at `chart_attach`:** a class whose `__slots__` are
  the chart's `<data>` ids and `_event`, with read-only properties
  `_sessionid`, `_name` and `_ioprocessors` (reading the session's system
  variables object); the top-level `def`s of the chart-level `<script>`s
  become its methods. Every expression is compiled once, by a restrictive
  rewrite of its AST, into a function of the session: `tries < 3` becomes
  `s.tries < 3`. A name the chart does not declare, or a construct the
  rewrite does not allow, is a compile error, reported when the
  expression is evaluated as `error.execution`, as SCXML requires
  (W3C test 312: an illegal expression in `<assign>` raises
  `error.execution`). `In(state)` calls `ta_datamodel_environment_in`
  with the evaluation's environment.
- **Per session, one instance.** The system variables are made into
  Python objects once, at `create` (core's views are valid only during
  that call, `datamodel.h:62-63`), immutable, and shared by reference.
  Assigning to an undeclared name or to a system variable raises
  (`__slots__`, read-only properties), and the evaluation is
  `error.execution`.
- **`_event`:** one immutable Event per event, built at `bind_event`
  (its strings converted during the call, `datamodel.h:35-37`), taking a
  reference on its data's payload row; `_event.data` is that row's
  value. The datamodel sets `TA_DATAMODEL_EVENT_DETACHABLE`, so core calls
  `detach_event` at the stable point after the event's macrostep
  (`src/core/core.c:468-478`, `:1870`); it drops the Event and the row's
  reference there, so a stable actor holds no event. That nothing reads
  `_event` after it is verified in step 8.2 [8.2] (the exit handlers of a
  terminated actor, `core.c:1875-1878`; `serialize`).
- **Capture** (`<send>` data, `<donedata>`, invoke params) copies mutable
  values crossing to another actor and shares immutable ones; actors
  share no mutable value.
- **Hibernation keeps the handle.** `serialize` gives the session row's
  handle; core calls `destroy` right after it (`datamodel.h:173-174`),
  which keeps the row, marked hibernated; `deserialize` makes it live
  again. Hibernation frees the session's share of the region; its
  instance stays on the Python heap. Export of a Python actor is not
  supported: another host's datamodel refuses the handle. Migration
  within the host: `deserialize` with the new chart makes an instance of
  the new class and copies the slots both declare; `migrate` returns the
  old bindings as a payload.
- **Estimated cost per session** (unmeasured until step 8.4's
  measurement): the instance 32 B + 8 B per slot; the system variables
  ~100–250 B; a row in the sessions table ~16 B. Compiled code belongs to
  the chart, never to a session.

## C rules

- **At most three parameters** for every C function this binding defines
  or declares, static helpers and callback types included (root
  `AGENTS.md`, "At most three parameters"). A function whose signature a
  typedef fixes is exempt by that typedef, not by name: `PyCMethod`
  (`METH_METHOD | METH_FASTCALL | METH_KEYWORDS`, five parameters),
  `PyCFunctionFastWithKeywords` (`METH_FASTCALL | METH_KEYWORDS`, four),
  `ta_allocator`'s two. `tools/lint/utils/fixed-signature.yml` lists them
  by their parameter types; add a typedef there, with its header and line,
  before a function it fixes fails the rule.
- **Bytes are `ta_bytes` in and `ta_buffer *` out**, never a pointer and a
  length, in every function (`buffers-are-ta-buffer`).
- **No serialization on the hot path** (root `AGENTS.md`): spawn, send,
  send_many, terminate, deliver, advance_time, resume, run, the effects
  and their ack take and give C structs. CBOR is for `ta_host_post`, the
  config and bytes that leave the process.
- **Acronyms** are all capitals or all lowercase (root `AGENTS.md`,
  "Acronyms"): `ta_python_otlp_writer`, `TA_PYTHON_IO_...`.
- **Layers**, each an ast-grep rule:
  - `extension-calls-core-and-python`: `src/` calls core (`ta_*`, `TA_*`),
    CPython's API (`Py*`, never a private `_Py*`) and a short list of C
    library functions (memory and strings), nothing else. A C library
    function the binding needs joins the list with its reason, in the
    commit that first calls it.
  - `datamodel-calls-environment-and-python`: `src/datamodel/` calls of
    core only `ta_datamodel_environment_*`, `ta_image_*` (the chart's
    expressions, read at `chart_attach`), `ta_cbor_*` (`to_wire`,
    `from_wire`) and `ta_table_*`, and our own `ta_python_*`; never the
    host or the system (`datamodel.h`: "they must never call ta_system_*
    functions").
  - `one-timer-in-loop`: the loop's timer and wakes (`call_at`,
    `call_later`, `call_soon`, `sleep`) appear in `src/loop.c` only.
- **No mutable globals** (`no-mutable-globals`): no global and no static
  local the extension writes. The module is imported once per
  interpreter, and a host may be driven from any thread of the
  free-threaded build, so state lives in the module's state
  (`PyModule_GetState`) or on the `Host`. A constant is a `const` object
  (`static const char *const names[]`; `static const char *p` is a
  mutable pointer). CPython's tables, which its API takes by non-const
  pointer, are exempt by type: `PyMethodDef`, `PyMemberDef`,
  `PyGetSetDef`, `PyType_Slot`, `PyType_Spec`, `PyModuleDef`,
  `PyModuleDef_Slot`, `PyStructSequence_Desc`, `PyStructSequence_Field`.
- **One thread per host.** Record the creating thread and refuse calls
  from others (bindings.md section 5); the GIL is not a substitute.
- **The wheel is abi3 from 3.13** (`Py_LIMITED_API 0x030D0000`;
  `pyproject.toml`, `mise.toml`). A spike compiled `PyMem_RawMalloc`,
  `PyMem_RawFree`, `PyType_GetModuleByDef`, `PyType_GetModule`,
  `PyModule_GetState`, `PyType_FromModuleAndSpec`, `PyErr_SetInterruptEx`,
  `PyStructSequence_NewType`, `Py_CompileString`, `PyEval_EvalCode`,
  `PyDict_GetItemRef`, `PyList_GetItemRef`, `PyErr_GetRaisedException`,
  `PyErr_SetRaisedException`, `PyUnicode_AsUTF8AndSize`,
  `PyObject_GetAttrString`, `PyObject_CallMethod`, `PyErr_SetString`, the
  slots `Py_mod_exec`, `Py_mod_gil` (with `Py_MOD_GIL_NOT_USED`),
  `Py_tp_traverse`, `Py_tp_clear`, the flags `METH_METHOD | METH_FASTCALL
  | METH_KEYWORDS` and the typedefs `PyCMethod` and
  `PyCFunctionFastWithKeywords` with clang 22 against CPython 3.13.16's
  headers and `Py_LIMITED_API 0x030D0000`, with no error; at `0x030C0000`
  (3.12), `PyMem_Raw*`, `PyDict_GetItemRef`, `PyList_GetItemRef`,
  `PyType_GetModuleByDef` and `Py_mod_gil` are undeclared. Module state
  comes from `PyType_GetModuleByDef` (it searches the type's MRO, so a
  method called on a subclass finds the module) and `METH_METHOD`.
- **The free-threaded build is for tests only.** The limited API cannot
  be compiled for a free-threaded interpreter before 3.15 (`Python.h`
  refuses `Py_LIMITED_API` with `Py_GIL_DISABLED`: 3.13, line 51), so the
  test task builds the same sources again without `Py_LIMITED_API` for
  the free-threaded interpreter (`TA_PYTHON_FREE_THREADED` in
  `mise.toml`, installed by uv). The functions above are in the limited
  API from 3.13 and in the full API of 3.14t alike, so the sources need no
  compatibility macros. A free-threaded wheel waits for abi3t (3.15).

## Values

- **The value table** is `tinyactors-core/docs/protocol.md`, "Values",
  with Python's side in `docs/values.md`: `int` to 64 bits (exact),
  `float`, `str` (valid UTF-8: a lone surrogate is refused, never
  replaced), `bytes`, `list`, `dict` with `str` keys, `True`, `False`,
  `None`, the singleton `UNDEFINED`, and `Address` for tag 32 (the "not
  ECMAScript" vectors of `values.txt` hold it). Test `bool` before `int`.
- **`tinyactors.encode(value) -> bytes`** and **`tinyactors.decode(data)
  -> object`** expose the one conversion the API uses (`src/values.c`,
  streaming with `ta_cbor_writer` and `ta_cbor_reader`), each with its row
  in `docs/api-map.md`. What they cannot read or write is refused with an
  exception, never turned into a value. The adapter frames the driver's
  calls with them and imports nothing else; since the vectors then cross
  this codec both ways, the binding's own value tests (against
  `values.txt`'s `python` lines) catch a value mapped wrong both ways.
- **Nesting is limited to 64**, which stops cycles but not a value that
  shares subtrees; the README says charts are trusted code.

## Rules the suite cannot see, each with a test of its own

- **Dropping a payload's last reference can run Python code** (a
  finalizer, a weakref callback), and core calls `release` outside a run
  too (the ack, `ta_host_free`, posts, deliver), where it holds the run's
  rules. Defer the last drop to a list emptied after the outer call into
  core returns (bindings.md, section 8).
- **Handles are `ta_table` handles** with a generation: a stale one is
  refused, never a pointer; a reply whose id names no pending row is
  dropped.
- **A thin view read after its ack raises** `TA_ERROR_STALE`, and a
  record made for an `async def` processor answers after the ack.
- **Marks nest:** a conversion that runs Python code which calls the
  `Host` again leaves the outer call's arena memory intact, and an
  exception releases to the mark.
- **The session scope refuses** an assignment to an undeclared name or to
  a system variable (`error.execution`), and generator expressions and
  lambdas in an expression see session variables (through the rewrite).
- **A `BaseException` that is not an `Exception`** (`KeyboardInterrupt`,
  `SystemExit`) still gives `error.execution` to the chart, and the
  binding raises the same object again after the run.
- **Handler closures make cycles:** fill the CPython slots
  `Py_tp_traverse` and `Py_tp_clear` with our functions.
- **One drain at a time and an ack before every run:** a test that a
  handler's typed method is answered in the next take, and that a view's
  fields read in a handler stay valid after later calls in the same batch.
- **Pending handlers are dropped at `ta_host_free`**, never called.
- **The README says** that charts are trusted code, and that Python
  values lie outside the host's allocator (bindings.md, section 8).

## What not to build

- No synchronous request, no raw `post`, no request facade, no reply
  object or future: typed methods take response handlers.
- No re-reading `ta_host_effects` until stable, no consumed marks, no
  nested drain, no ack inside a handler.
- No eager conversion of an effect: a thin view, and a record only where
  a value must outlive the ack.
- No copy of a host output before the next call (it lives until the ack).
- No allocation of our own per call: the host's arena with a mark.
- No handle table, status table, arena or image rows of our own
  (`ta_table`, `ta_status_name`, `ta_growable_arena`,
  `ta_host_add_image` copies).
- No global configuration: handlers, logging and the clock are per host.
- No application callback for `DONE` or `ERROR`; no subscription.
- No defaults, limits, methods, events or messages of our own; nothing C
  does done again in Python (bindings.md section 2).
- No serialization on the hot path, no struct layout read from Python.

## Tasks

`mise.toml` pins python (3.13, and the free-threaded interpreter the tests
build for) and uv; the C toolchain is the root's.

- `mise run build`: core's amalgamation into `build/core`
  (`mise run //tinyactors-core:amalgamate -- --out build/core`), the only
  core task this project's tasks run, never core's build; the extension
  compiles against it with `Toolchain::Target` (root
  `mise/lib/toolchain.rb`; never a compiler from `PATH`, never `$CC`).
- `mise run lint`: the rules' tests, the rules over `src/`, the adapters'
  lint (`mise run //tinyactors-test:lint -- conform`: the adapter imports
  only `tinyactors` and the standard library), `docs/api-map.md` against
  core (`mise/lib/api_map.rb`), and the root's checks on this project's
  files: the build scripts' portability and acronyms (`.py` files
  included).
- `mise run conform [-- runner options]`: the suite against
  `python -m conform`, with `--kind binding`, on the native datamodel and
  on quickjs (both must pass); `-- --milestone <M>` runs one of the
  suite's sets (`tinyactors-test/suites/milestones.txt`).
- `mise run test`: lint, build, both interpreters, and the suite's set
  named by `MILESTONE` in `mise/cpython.rb` once the adapter exists. Its
  `#MISE sources` list this project's files, core's and the suite's. An
  exhaustive test goes behind `--full` (root `AGENTS.md`).
- `mise run wheel`: one abi3 wheel per platform; it aborts until
  packaging is built.

Put the built extension on the adapter's import path in the conform task
(`PYTHONPATH`), never in the adapter: its lint refuses `sys.path`.

## Commits and the changelog

Commit subjects are the changelog's input (root `AGENTS.md`, "Releases
and changelogs"): scope `cpython` (`feat(cpython): ...`), `!` for a
breaking change with a body that says what a caller does.

## Code search, static checks and design claims

- **ast-grep for code search and rewrites** (`ast-grep run -p ... -r
  ...`), `rg` only for plain text. Invoke `ast-grep`, never `sg`. A new
  static check is an ast-grep rule in `tools/lint/rules/` with its test in
  `tools/lint/tests/`.
- **Design claims are checked** with the reasoned-analysis skill: a
  scenario in this design, evidence (`path:line`, a primary source or a
  spike) or the word unverified, and a refutation by a fresh agent before
  a finding changes the design, the guide or a request to core.

## Gaps in core

This project never patches `tinyactors-core` or the suite
(`tinyactors-test`), and works around nothing in them. A gap is a request
to core in [ROADMAP.md](ROADMAP.md): what is missing, why, and which
suite case or test shows it. The binding waits for the change.

## Layout

The Node binding has the same files on purpose, and the two are compared
file by file.

```
README.md, CHANGELOG.md, mise.toml, pyproject.toml, sgconfig.yml
ROADMAP.md           the work in progress: plan, progress, how it runs, requests to core
docs/api-map.md      each API item → ta_* function, method or effect (checked)
docs/values.md       the Python side of the value table
src/                 extension.h, module.c, host.c (Host, inputs, the arena's marks),
                     loop.c (drain, turn, both drivers), effects.c (dispatch, thin views,
                     records), methods.c (typed methods, the pending table), values.c,
                     datamodel/{datamodel.h, datamodel.c, rows.c, expressions.c}
conform/             the suite's adapter, python -m conform; not in the wheel
tests/               what the suite cannot see: threads, GC, asyncio, refusals, stale views
tools/lint/          rules/, tests/, utils/ (ast-grep)
mise/cpython.rb      what the tasks share
mise/tasks/          build, test, conform, lint, wheel
build/               core's amalgamation and the extension (not in git)
```

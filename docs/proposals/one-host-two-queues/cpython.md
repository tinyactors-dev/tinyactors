# tinyactors-cpython, from the guide alone

**Status:** Proposed (2026-10-11). Part of [One host, two queues](README.md).

The second binding designed from [“Writing a binding”](bindings.md), after the Node addon. Python tests the guide where JavaScript could not: a datamodel in a language other than the charts’ ECMAScript, a garbage collector that finds cycles, threads without a GIL, programs without an event loop, and a high cost per call into C. Every effect, input and request of the host was mapped; where the guide had no answer, that is a finding.

No core changes: the host interface held, and Python gets a datamodel of its own without extending core. A python host runs charts that say `datamodel="python"`, whose expressions are Python over CPython objects held by handle; an ecmascript host runs the same binding over quickjs. A host never mixes the two. The other findings are the guide being too JavaScript-shaped, about values, threads, garbage collection, drivers and the cost of crossing into C.

## Where the guide was JavaScript-shaped

#### A Python datamodel needs no core change. *(guide)*

The guide said a native datamodel must implement the chart’s declared language, so Python could only use quickjs. That was wrong: core matches each registered vtable’s `name` against the chart’s `datamodel` attribute, and the compiler stores the attribute as written and takes a per-name expression checker.

**Fix:** The extension registers a `"python"` datamodel: charts that say `datamodel="python"` evaluate Python, and their values are CPython objects held by handle. A host runs one datamodel: `Host(datamodel="python")`, or `"ecmascript"` for charts written for quickjs, never both. The guide says a binding may define a datamodel of its own language under its own name.

#### The CBOR module belongs in C here. *(guide)*

The guide says to walk the token list “in the language”. In CPython that is a Python loop per token, far slower than building objects with the C API, which the extension can call directly.

**Fix:** Where the language’s objects are reachable from C, the CBOR module is written in the extension (`values.c`: tokens → `PyLong`, `PyBytes`, `PyUnicode`, `PyDict`, `PyList`); still one call per message.

#### `undefined` has no Python value. *(guide)*

CBOR keeps `null` and `undefined` apart and the guide says to keep them distinct. Python has only `None`.

**Fix:** `None` is `null`; `tinyactors.UNDEFINED`, a singleton, is `undefined`. The guide gets a per-language value table: integers, bytes, tag 32, null, undefined, floats.

#### Threads: free-threaded Python and subinterpreters. *(guide)*

The guide says a host is single-threaded. Python 3.13’s free-threaded build has no GIL to serialize calls, and subinterpreters (PEP 684) each have their own.

**Fix:** The extension records the creating thread and raises on a call from any other. Module state is per interpreter (multi-phase init, PEP 489), never in C globals.

#### Cycles through handlers. *(guide)*

The guide says to free deterministically. In Python the handlers (I/O processors, invokers, the store, `on.*`) often close over the host, so a forgotten `close()` leaves a cycle.

**Fix:** `close()` and a context manager free deterministically; the Host type supports the cycle collector (`tp_traverse`, `tp_clear`) so a forgotten host is still freed. The guide names cycle collectors.

#### An effect handler raises. *(guide)*

An I/O processor or `on.done` raises mid-turn. The guide does not say whether the turn acks, continues or stops. (The Node addon had the same gap; it kept the first exception and rethrew it after the turn.)

**Fix:** Ack the effects whatever happens, finish the turn, then raise the first exception from the call that ran the turn. The guide’s loop says so for every language.

#### Not every program has an event loop. *(guide)*

The guide’s loop arms “one timer”. A Python script or a test may have no event loop at all.

**Fix:** Two drivers over the same turn: synchronous (`host.run_until_idle()`, with the caller’s clock or sleeping to the next deadline) and asyncio (`host.attach(loop)`, in C: `loop.call_at` for the one timer). The guide allows a synchronous driver.

#### Accessors per field are expensive from Python. *(guide)*

Reading each effect field through `ta_host_effect_number` from Python costs a call into C per field.

**Fix:** The extension reads the fields in C and hands Python one record per effect (a `structseq`, `tinyactors.Effect`). The guide: where calls across are costly, the extension assembles records with the accessors.

#### An actor reference needs a Python type. *(api)*

CBOR tag 32 is an actor’s address. Python needs a type for it that is not just a string, or a reference and a string are indistinguishable when they come back.

**Fix:** `tinyactors.Address`, a `str` subclass: it prints and compares as the address and encodes as tag 32.

#### One wheel per platform needs the stable ABI. *(build)*

Building against every Python version multiplies wheels. The limited API has the buffer protocol (for zero-copy `ta_buffer`s) only from 3.11.

**Fix:** Build with `Py_LIMITED_API` 3.11: one `abi3` wheel per platform, built with the pinned zig like the release libraries.

#### Posting traces needs HTTP. *(api)*

The standard library’s HTTP client blocks, and an async one is a dependency.

**Fix:** Traces go to `on.traces(bytes)`; the README shows a ten-line handler that posts them with `urllib` in a thread. The binding ships no HTTP code and adds no dependency.

## Charts in Python, values as Python objects

The datamodel lives in the extension (`datamodel.c`) and is registered with the host under the name `python`. Core does not change: it already picks a chart’s datamodel by name, and the compiler already takes a checker per name. A host runs one datamodel, so an application with ecmascript charts as well runs a second host and the two exchange envelopes. A chart chooses it like any other:

```
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="python" initial="counting">
  <datamodel>
    <data id="count" expr="0"/>
    <data id="seen" expr="set()"/>                   <!-- any object: it stays in this actor -->
  </datamodel>
  <state id="counting">
    <transition event="hit" cond="_event.data['n'] > 0 and In('counting')">
      <assign location="count" expr="count + _event.data['n']"/>
      <script>seen.add(_event.origin)</script>
    </transition>
    <transition event="report">
      <send target="#_parent" event="total">
        <param name="count" expr="count"/>          <!-- copied: the parent gets its own int -->
        <param name="from" expr="sorted(seen)"/>   <!-- a list of str: copied -->
      </send>
    </transition>
  </state>
</scxml>
```

### Each vtable entry, in Python terms

| vtable entry                                            | what the python datamodel does                                                                                                                                                                                                                                                                                                                                             |
|---------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `name`                                                  | `"python"`: matched against the chart’s `datamodel` attribute                                                                                                                                                                                                                                                                                                              |
| `chart_attach / chart_detach`                           | compile every expression of the chart once with `Py_CompileString`, by its kind (`ta_image_expression`): condition, value, string and content in `eval` mode; script in `exec` mode; a location as the assignment `LOCATION = __value__`, so `x`, `x.y` and `x["k"]` are locations and anything else fails to compile. A location naming a system variable fails here too. |
| `create / destroy`                                      | a globals dict per session: the application’s `globals` (modules, helpers), the read-only `_sessionid`, `_name` and `_ioprocessors`, and `In`                                                                                                                                                                                                                              |
| `init_data`                                             | evaluate the `<data>` expression (or its content) into the session’s globals; early and late binding are core’s                                                                                                                                                                                                                                                            |
| `bind_event / detach_event`                             | `_event` becomes a read-only record; `data` is the payload’s object, borrowed through its handle                                                                                                                                                                                                                                                                           |
| `eval_condition, eval_string, log`                      | run the code object; truth by `PyObject_IsTrue`, text by `str()`, alive until the next call                                                                                                                                                                                                                                                                                |
| `assign, script`                                        | run the assignment or the script in the session’s globals                                                                                                                                                                                                                                                                                                                  |
| `foreach_begin / _item / _end`                          | `list(iterable)` at the start (SCXML iterates a shallow copy), item and index assigned as locations                                                                                                                                                                                                                                                                        |
| `capture`                                               | a dict of `<param>` and namelist values, or the content value, deep-copied (`ta_cpython_copy`) into a new slot                                                                                                                                                                                                                                                             |
| `retain / release`                                      | the slot’s reference count; at zero, `Py_DECREF` and the slot is free                                                                                                                                                                                                                                                                                                      |
| `custom`                                                | `TA_ERROR_UNSUPPORTED`: no custom executable content                                                                                                                                                                                                                                                                                                                       |
| `serialize / deserialize`                               | the session’s variables and `_event` as CBOR (values.c); a value outside the value table is `TA_ERROR_UNSUPPORTED`, so that actor is not hibernated and stays live. Never pickle: the same bytes go into export blobs, read by another host, and reading a blob back must never run code.                                                                                  |
| `migrate`                                               | the old `<datamodel>` bindings as one dict payload; names the new chart does not declare are dropped                                                                                                                                                                                                                                                                       |
| `to_wire / from_wire, payload_serialize / _deserialize` | CBOR through values.c, tag 32 as `Address`: envelopes to peers (an ecmascript host among them), export blobs                                                                                                                                                                                                                                                               |
| `compile-time checker`                                  | `ta_cpython_compiler()`: `compile()` each expression by kind and report a `SyntaxError` with its line and column, so a broken chart fails in `tinyactors.compile`, not at its first event                                                                                                                                                                                  |

### Rules

- **Values are objects; actors share none.** Inside a session a variable may hold any object: a set, a dataclass, a NumPy array. What crosses to another actor (`<send>` data, `<donedata>`, `Host.send`) is deep-copied through the value table in C, so no two actors hold the same mutable object. Immutable values (`int`, `float`, `str`, `bytes`, `Address`, tuples and frozensets of them) are shared without a copy. A mutable object outside the table fails the capture: `error.execution` in the sender.
- **A payload is a handle, never a pointer.** `ta_payload` is a slot and a generation in the datamodel’s table, so system memory holds no pointer (core’s rule) and a stale handle is refused. Within one host a handle is the whole cost of passing an event between python sessions.
- **Expressions do not drive the host.** Every call runs inside `ta_host_run`. An expression that reaches a `Host` (through the application’s globals) gets `RuntimeError` from every method but the four that core appends during a run.
- **Errors are `error.execution`.** An exception in an expression returns `TA_ERROR_EXECUTION`: core raises `error.execution` and skips the rest of the block, as SCXML requires. The traceback goes to `on.log`; there is no effect for it.
- **Conformance is the W3C suite in Python.** The W3C tests are written for ECMAScript. The datamodel ships the suite translated to Python expressions (the expressions only, the charts unchanged), and the conformance scenarios run on a python host and on an ecmascript host.

### Roles and costs

| role                                 | what it means for them                                                                                                                                                                                                                          |
|--------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| application developer (served first) | chart logic in the language and with the objects the rest of their program uses, without a JavaScript engine in between or CBOR on every event                                                                                                  |
| operator                             | a `<script>` that loops forever stops the host: CPython cannot be preempted mid-expression the way quickjs is by `max_ops`. Python charts are trusted code. Fuel is charged once per evaluation, so the scheduler’s fairness still counts them. |
| capacity planner                     | values live on the Python heap, outside `config.allocator`: the host’s memory bound covers kernel state, not the variables. Per actor, a dict of globals; per event, one `Event` record and one evaluation per expression.                      |
| C embedder, maintainers              | no code in core: the vtable, the compiler hook and the name match are already there                                                                                                                                                             |
| binding author                       | one more file in the extension (the vtable, about the size of the quickjs glue) and the translated W3C expressions; the guide’s datamodel section describes it                                                                                  |
| out of scope                         | python and ecmascript charts in one host, sandboxing Python, a memory limit for its values, sharing mutable objects between actors, ahead-of-time bytecode (marshal is per Python version)                                                      |

## One extension module

The extension links the core and does everything that needs the C API: the Host type, the python datamodel, values between CBOR and Python objects, effect records. The extension module is `tinyactors` itself, so `from tinyactors import Host` imports its type, and there is no package around it: no `__init__.py` re-exporting a `_tinyactors`, no Python files at all. Both drivers are Host methods in C (`run_until_idle`, `attach`), and trace posting is a handler the application writes. The wheel holds one abi3 shared library. The `tinyactors-cpython` project at the repository root, the counterpart of `tinyactors-napi`, holds it and pins Python and uv in its own `mise.toml`.

    Your Python: from tinyactors import Host · Host(...) · add_image · spawn · send · request · turn · attach · close
                                       |
    +----------------------------------v------------------------------------------------+
    | tinyactors-cpython/src: the extension module tinyactors (one abi3 library, no      |
    | Python files)                                                                      |
    |   extension.c (tinyactors.Host)   datamodel.c (the "python" datamodel)             |
    |   values.c (CBOR <-> Python objects)   effects.c (the turn, both drivers)          |
    +----------------------------------+----------------------------------------^-------+
                            C calls    |                     datamodel vtable    |
    +----------------------------------v----------------------------------------+-------+
    | core, linked in: ta_host, ta_system, ta_otlp, src/base/cbor,                       |
    | the quickjs datamodel (for an ecmascript host)                                     |
    +------------------------------------------------------------------------------------+

*Unlike JavaScript, Python has one runtime here (CPython), so there is one extension.*

## Structs and signatures

### The CPython extension · `tinyactors-cpython/src/extension.h`

```
/* tinyactors-cpython/src/extension.h — the CPython extension module tinyactors (PyInit_tinyactors) */
typedef struct ta_cpython_state {        /* per interpreter (multi-phase init, PEP 489) */
    PyTypeObject *host_type;
    PyTypeObject *effect_type;           /* structseq tinyactors.Effect */
    PyTypeObject *address_type;          /* str subclass tinyactors.Address (CBOR tag 32) */
    PyObject *error_type;                /* tinyactors.TinyactorsError(path, message) */
    PyObject *undefined;                 /* tinyactors.UNDEFINED */
} ta_cpython_state;

typedef struct ta_cpython_host {         /* one per ta_host: a Python Host */
    PyObject_HEAD
    ta_host *host;
    ta_cpython_datamodel *python;        /* the host's datamodel: "python" (datamodel.c), */
    ta_datamodel_ecmascript *quickjs;    /*   or "ecmascript" (quickjs); one is NULL */
    unsigned long thread;                /* the creating thread: other threads raise */
    PyObject *handlers;                  /* processors, invokers, store, on.*: traversed */
    PyObject *held;                      /* dict session -> bytes whose store put has not settled */
    ta_cbor_token *tokens;               /* decode scratch, grown on TA_ERROR_NO_MEMORY */
    uint32_t token_capacity;
    PyObject *loop, *timer;              /* attach(loop): the asyncio loop and its one TimerHandle */
    uint32_t in_turn, closing;           /* close() from a handler: free after the turn */
} ta_cpython_host;

/* datamodel.c: the "python" datamodel. Every call runs inside ta_host_run, on the
 * host's thread, with the thread attached to the interpreter. */
typedef struct ta_cpython_slot {         /* a payload: one strong reference to an object */
    PyObject *object;                    /* NULL: free; next free slot in `references` */
    uint32_t references;                 /* retain/release; Py_DECREF at zero */
    uint32_t generation;                 /* in the handle, so a stale handle is refused */
} ta_cpython_slot;

typedef struct ta_cpython_chart {        /* chart_attach: per chart, per image */
    PyObject **code;                     /* one code object per ta_expression, compiled once */
    uint32_t code_count;
} ta_cpython_chart;

typedef struct ta_cpython_instance {     /* create: one per session */
    PyObject *globals;                   /* dict: <data> ids, _sessionid, _name, _ioprocessors, In */
    PyObject *event;                     /* _event: a read-only Event record, or None */
    PyObject *text;                      /* eval_string/log result, alive until the next call */
} ta_cpython_instance;

typedef struct ta_cpython_datamodel {
    ta_cpython_state *state;
    ta_cpython_slot *slots;              /* ta_payload = generation << 32 | slot + 1 */
    uint32_t slot_count, free_slot;
    ta_datamodel_environment *environment;   /* during a call only: what In() asks */
} ta_cpython_datamodel;

const ta_datamodel_vtable *ta_cpython_vtable(void);        /* name "python" */
const ta_datamodel_compiler *ta_cpython_compiler(void);    /* check: compile() per expression */

/* values.c: neutral values between CBOR and Python objects, in C */
PyObject *ta_cpython_decode(ta_cpython_state *state, ta_bytes cbor);          /* tokens -> objects */
ta_status ta_cpython_encode(ta_cpython_state *state, PyObject *value, ta_buffer *out);
PyObject *ta_cpython_copy(ta_cpython_state *state, PyObject *value);  /* deep copy, same table */
```

### The public API: the extension’s `Host` type

```
# tinyactors.Host is the extension's type: from tinyactors import Host
Host(*, system_id: str, datamodel="python", capacities=None, io_processors=None,
     invokers=None, store=None, on=None, globals=None)
                                   # ta_host_create: keywords → CBOR config in C, handlers kept
Host.close() -> None / __enter__ / __exit__                   # ta_host_free
Host.turn(monotonic_ms=None) -> float                         # one loop turn, in C:
                                   # ta_host_advance_time, ta_host_run, ta_host_effects,
                                   # a handler call per effect, ta_host_effects_ack; the deadline
Host.run_until_idle(clock=None) -> None                       # the synchronous driver
Host.attach(loop=None) -> None     # the asyncio driver: loop.call_soon/call_at, async store

Host.add_image(image: bytes) -> int                           # ta_host_add_image
Host.boot(segment: int) -> None                               # ta_host_boot
Host.spawn(chart: str, name: str | None, input: object) -> int    # ta_host_chart, ta_host_spawn
Host.send(session: int, name: str, data: object) -> None      # handle or from_wire, ta_host_send
Host.send_many(sessions, names, datas) -> int                 # ta_host_send_many
Host.terminate(session: int) -> None                          # ta_host_terminate
Host.deliver(peer: int, envelope: bytes) -> int               # ta_host_deliver

Host.hibernate(session: int) -> bytes                         # ta_host_hibernate (bytearray, grown)
Host.wake(session: int, blob: bytes) -> None                  # ta_host_wake
Host.export_actor(session: int) -> bytes / adopt(blob: bytes) -> int
Host.migrate(chart: str | None, session: int, wake: bool, timeout_ms: int) -> int   # the job id
Host.snapshot() -> bytes                                      # ta_host_snapshot_pack
                                   # for inspection: not a backup, a save file or crash recovery
Host.request(op: str, fields: dict) -> object                 # values.c both ways, ta_host_request
Host.traces() -> bytes / metrics() -> bytes                   # ta_host_traces, ta_host_metrics

compile(sources: dict[str, str]) -> bytes        # ta_compile_*, with ta_cpython_compiler()
open_image(image: bytes) -> dict                              # ta_image_*: hash, charts, actors
Address · UNDEFINED · Effect · TinyactorsError                # types of the extension
```

Handlers are plain callables and objects: `on={"done": f, "log": g}`, `io_processors={type: obj with send(effect)}`, `invokers={type: obj with start and stop}`, `store=obj with get and put` (coroutine functions under `attach`), and `globals`, the names every python chart sees. Not here, because no core function is behind them: actor objects, per-actor callbacks, chart builders. Sessions are ints; addresses are `Address`.

## Six paths through the binding

### host.send(session, name, data)

```
Host.send(session, "OPEN", {"by": "ops"})          extension.c
   ├─ python host:     ta_cpython_copy(state, data) → slot → payload   no encoding
   │  ecmascript host: ta_cpython_encode(state, data, &buffer) → quickjs from_wire
   ├─ ta_host_send(host, session, &event)            (inside a run: appended)
   │    └─ refused: the payload is released, TinyactorsError raised
   └─ attached: loop.call_soon(turn) unless one is pending · else the next run_until_idle
```

### a turn

```
Host.turn()                                         effects.c, all in C
├─ ta_host_advance_time(host, {monotonic, wall_ns})   the caller's clock or the monotonic one
├─ first_error = None
└─ repeat
   ├─ status = ta_host_run(host, budget)
   ├─ for each of ta_host_effects(host): an Effect record, built with the accessors
   │    call the handler; an exception is kept, not raised
   │    ├─ SEND       → io_processors[type].send(effect)     (sync: answered before the next run)
   │    ├─ INVOKE     → invokers[type].start(effect)  ·  UNINVOKE → .stop(effect)
   │    ├─ DONE/ERROR → on.done(session, output) / on.error(session, code)
   │    ├─ CHANGED    → on.changed(session)                   (subscribed sessions only)
   │    ├─ LOG        → on.log(session, label, text)
   │    ├─ WAKE       → blob = held.get(session) or store.get(session) → Host.wake
   │    ├─ PEER       → on.peer(peer, ta_host_envelope(...) as bytes)
   │    ├─ TRACES     → on.traces(Host.traces())
   │    ├─ JOB_DONE   → migrations[job].set_result(...)
   │    └─ FREED      → images.pop(segment)
   │    the first exception is kept
   ├─ ta_host_effects_ack(host)                                always, even after an error
   └─ EFFECTS_FULL, EFFECTS → again · YIELDED → return 0 · IDLE → return deadline
                                       EXHAUSTED, FAULT → raise TinyactorsError
then: if first_error: raise first_error
```

### a transition in a python chart

```
ta_host_run                                         core, inside Host.turn()
└─ the event "hit" is current
   ├─ bind_event(dm, instance, environment, &view)   datamodel.c
   │    └─ instance.event = Event(name, type, sendid, origin, origintype, invokeid,
   │                              data = slots[view.data].object)    borrowed handle, no copy
   ├─ eval_condition(dm, instance, environment, e, &out)
   │    ├─ dm.environment = environment                for In()
   │    ├─ PyEval_EvalCode(chart.code[e], instance.globals, {"_event": instance.event})
   │    ├─ ta_datamodel_environment_charge(environment, 1)
   │    └─ raised → TA_ERROR_EXECUTION, the exception to on.log
   │                                                 core raises error.execution
   ├─ assign(dm, instance, environment, location, value)
   │    └─ chart.code[location] is "count = __value__", compiled at attach
   └─ capture(dm, instance, environment, &spec, &payload)    for <send>
        ├─ {name: ta_cpython_copy(value)} for <param> and namelist
        │    a mutable object outside the value table → TA_ERROR_EXECUTION
        └─ a new slot → payload                      the receiver gets its own objects
```

### the asyncio driver

```
host.attach(loop)                                   effects.c, in C
└─ self.loop = loop or asyncio.get_running_loop()
   wake:       loop.call_soon(step) unless one is pending
   step:       deadline = Host.turn()
               0 → loop.call_soon(step)              (yielded: give the loop a turn)
               d → self.timer.cancel(); self.timer = loop.call_at(when(d), step)   one timer
   WAKE with an async store (store.get is a coroutine function):
               task = loop.create_task(store.get(session))
               task.add_done_callback(woken)         woken: Host.wake(session, result), wake
```

### host.request("trace.configure", options)

```
Host.request("trace.configure", {"detail": "summary", "sample": 0.01})   extension.c
├─ ta_cpython_encode(state, {"op": op, **fields}, &request)
├─ ta_host_request(host, request, &reply)            TA_ERROR_NO_MEMORY: grow, again
├─ value = ta_cpython_decode(state, reply)           (nothing applied until TA_OK)
└─ "error" in value → raise TinyactorsError(path, message) : return value
```

### host.close()

```
Host.close()  /  with Host(...) as host:            extension.c
├─ in a turn → closing = True; the turn frees after the ack
├─ every running invocation: invokers[type].stop(...)
├─ on.traces(Host.traces())                          the last spans
└─ ta_host_free                                      python slots released, quickjs freed
     └─ handlers cleared (tp_clear): no cycle left
```

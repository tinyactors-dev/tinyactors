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

## The loop

The extension runs embedding.md's loop (sections 2 and 4), in C:

- **One drain at a time:** take (`ta_host_effects`), handle every effect
  of the batch, ack (`ta_host_effects_ack`), and take again until a take
  is empty; then, at most, a run. A take never widens the open batch, and
  `ta_host_run` refuses while one is open (ADR 0012, decision 2). There
  is no nested drain: Python code a handler runs (an I/O processor's
  callback) may call the `Host` to add inputs, post, or read outputs; it
  never takes or acks.
- **Every run status** of bindings.md section 3; one timer (asyncio's
  `TimerHandle`, or the synchronous driver's sleep) at `next_deadline`;
  both clocks before every run, converted with `system.ticksPerSecond`,
  which the extension reads from `host.config` at creation.
- **Two drivers:** a synchronous one (`run_until_idle`, or
  `SimulatedClock.increment(ms)`, xstate's shape and our meaning) and
  asyncio (`attach(loop)`).
- **Whatever a handler raised:** ack anyway, finish the turn, then raise
  the first error.

## Lifetimes

ADR 0012: **everything the host hands out about a batch lives until the
ack.** Effect fields (`TRACEPARENT` included), envelopes, traces, logs,
metrics, hibernation and export blobs read while handling stay valid
across any later call into the host, until the ack. Convert them into
Python objects when a handler builds its value; copy only what is kept
past the ack. A datamodel's outputs live until its next call. Snapshots
and the refused config's error fill a `ta_buffer` the binding lends.

## What reaches the application

| Python sees | Through | Core |
|---|---|---|
| an actor's output | an I/O processor the application configures, a callable (sync or async) | `TA_EFFECT_SEND` |
| services the charts invoke | a service registered by type | `TA_EFFECT_INVOKE`, `UNINVOKE`, sends with `IO_PROCESSOR` `TA_NIL` |
| an actor's state as it changes | `subscribe(session)`, an async iterator or callback | `actor.subscribe`, `actor.changed` |
| inspection and trace control | typed methods (`stats()`, `state(session)`, ...), each returning a result settled at a later drain | `ta_host_post`, `TA_HOST_EFFECT_REPLY` |
| `<log>` | the `logging` module | `TA_EFFECT_LOG` |
| telemetry | an exporter the application configures | `TRACES`, `LOGS`, `ta_host_metrics` |

- **`DONE` and `ERROR` are internal:** the extension releases its rows
  for the session and ends its subscriptions; no application callback.
- **No synchronous request and no raw `post`** (ADR 0012, decision 3):
  one typed method per method of `tinyactors-core/docs/requests.md`.
- **`PEER` versus `PEER_ERROR` in the API is deferred**: carrying
  envelopes is the application's, and its API shape waits for a decision.

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

The binding allocates almost nothing (bindings.md, section 6):

- **Rows in core's `ta_table`** (`tinyactors/table.h`, ADR 0012 decision
  5): a session (its globals dict), a chart (its code objects), a payload
  (with its reference count), a `<foreach>`. The tables grow through the
  host's allocator and are freed once with the host. Keep handles, never
  row pointers across a `ta_table_add`. The binding writes no table of
  its own.
- **Images are bytes:** `ta_host_add_image` copies them; free a compile
  result at once. No image rows, no `SEGMENT_FREED` bookkeeping.
- **One writer per call.** A config, a typed method's params, a payload
  for quickjs and `tinyactors.encode` are written with a `ta_cbor_writer`
  of that call, starting in a room on the C stack and growing through
  `PyMem_RawMalloc`, freed before the call returns. Python code running
  during a call (a getter, `__iter__`, `__del__`) may call the `Host`
  again: nothing is shared.
- **Views of what core and Python hold.** A string goes to core as the
  `str`'s own UTF-8 (`PyUnicode_AsUTF8AndSize`), bytes through the buffer
  protocol, for the call. What the host hands out is a view until the ack
  (above). Core's values are read with `ta_cbor_reader` as they stream.
- **The datamodel's last output.** `to_wire`, `serialize` and
  `payload_serialize` write with a writer the datamodel keeps, overwritten
  by its next call; strings handed back to core (`eval_string`, `log`)
  are the UTF-8 of a `str` the datamodel holds until its next call.
- **Payloads are copied, not encoded,** between the API and the
  `"python"` datamodel; CBOR only toward quickjs and in envelopes.
- **Statuses are named by core:** a bare status in a message is
  `ta_status_name(status)` (`"TA_ERROR_STATE"`), never a table of ours.
- **Where the allocator is called:** in the host's create, free and
  allocator, in a call's writer, and through core (`ta_table`, the
  compiler's options); nowhere else, nothing per event or per evaluation.
  The lint rule `allocation-in-tables-and-host` checks it; it names files
  of the removed binding (`src/datamodel/handles.c`) and changes with the
  first commit of the rewrite.

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
  refused, never a pointer.
- **One globals dict per session and no locals:** with separate locals,
  generator expressions and lambdas do not see session variables.
- **System variables stay as they were** (`_event`, `_sessionid`,
  `_name`, `_ioprocessors`, `_x`): after each evaluation a changed one is
  restored and the evaluation is `error.execution`; `assign` checks that
  its location is declared; `variable` and `serialize` skip
  `__builtins__`.
- **A `BaseException` that is not an `Exception`** (`KeyboardInterrupt`,
  `SystemExit`) still gives `error.execution` to the chart, and the
  binding raises the same object again after the run.
- **Handler closures make cycles:** fill the CPython slots
  `Py_tp_traverse` and `Py_tp_clear` with our functions.
- **One drain at a time and an ack before every run:** a test that a
  handler's post is answered in the next take, and that outputs read in a
  handler stay valid after later calls in the same batch.
- **The README says** that charts are trusted code, and that Python
  values lie outside the host's allocator (bindings.md, section 8).

## What not to build

- No synchronous request, no raw `post`, no request facade.
- No re-reading `ta_host_effects` until stable, no consumed marks, no
  nested drain, no ack inside a handler.
- No copy of a host output before the next call (it lives until the ack).
- No handle table, status table or image rows of our own (`ta_table`,
  `ta_status_name`, `ta_host_add_image` copies).
- No application callback for `DONE`, `ERROR` or `LOG`.
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
src/                 extension.h, module.c, host.c, loop.c, effects.c, methods.c, values.c,
                     datamodel/{datamodel.h, datamodel.c, rows.c, expressions.c}
conform/             the suite's adapter, python -m conform; not in the wheel
tests/               what the suite cannot see: threads, GC, asyncio, refusals
tools/lint/          rules/, tests/, utils/ (ast-grep)
mise/cpython.rb      what the tasks share
mise/tasks/          build, test, conform, lint, wheel
build/               core's amalgamation and the extension (not in git)
```

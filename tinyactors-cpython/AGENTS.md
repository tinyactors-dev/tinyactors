# AGENTS.md: tinyactors-cpython

The Python extension `tinyactors`: one C extension module, written against
the CPython C API, that drives `ta_host` (README.md). The repository root's
`AGENTS.md` applies too; this file adds the rules of this binding. Read
first: [`tinyactors-core/docs/bindings.md`](../tinyactors-core/docs/bindings.md)
(the guide) and the public headers it names. Build from the guide and the
headers, not from an older binding or the proposals' sketches. What is
being built now, how far it is and how the work runs are in this
project's [`ROADMAP.md`](ROADMAP.md).

## Whom it serves

First the **application developer writing Python**: chart logic in Python
(the native `"python"` datamodel), a `Host` driven from a script or from
asyncio, values that are Python objects. Then the **binding author**, held
to the guide by the conformance suite, and the **maintainers**, who run one
suite against every implementation. Every API item is a host input, a
request or the handling of an effect (`docs/api-map.md`); what core does
not have is not in the API.

## Names

Our names carry the prefix `ta_python_*`: every function this binding
defines or declares, static helpers included, and its types and globals.
A platform's name appears only where the platform requires it: the module
init function `PyInit_tinyactors` (CPython's import finds it by that name,
PEP 489), a slot such as `Py_tp_traverse` filled with our
`ta_python_host_traverse`. When you write a name in prose, say whose it
is. The lint rule `functions-carry-the-prefix` checks functions.

## The host's allocator

The host allocates through the `ta_allocator` its environment gives it
(`tinyactors-core/include/tinyactors/allocator.h`; bindings.md, section 6),
and this binding gives it `PyMem_RawMalloc` and `PyMem_RawFree`, which are
in the limited API from 3.13 (CPython's `Include/pymem.h`, 3.13: lines
90-98), the wheel's floor. The raw domain needs no GIL ("Memory allocator
which doesn't require the GIL to be held", the same file, line 91), which
a free-threaded build has none of anyway. Python's own objects stay
outside the host's allocator; the README says so (bindings.md, section 8).
The two functions take core's parameter lists (`void *context, size_t
size, size_t align` and `void *context, void *ptr, size_t size`), which
the lint exempts by that signature. `malloc`, `calloc`, `realloc` and
`free` stay refused by the layer lint.

**Alignment: `align` is at most 8, and `PyMem_RawMalloc` gives 16.** The
host passes 8 on every allocation it makes
(`tinyactors-core/src/host/host.c:25`, `host_alloc`, and `:287`, the host
itself; no other call in `src/host`, `src/system` or `src/base` reaches
the allocator), the largest alignment a region object needs is 8
(`src/base/config.h:46`, `TA_ALIGN_MAX`), and the allocator's contract
says "a power of two, at most 8 from this library"
(`include/tinyactors/allocator.h:9`). A spike on CPython 3.13.16
(macOS arm64) called `PyMem_RawMalloc` 800 times, sizes 1 to 100000,
under `PYTHONMALLOC` default, `debug`, `malloc` and `pymalloc`: the
smallest alignment seen was 16 in every mode. So `PyMem_RawMalloc`
meets what core asks, and the binding's `alloc` need not align by hand
(an `assert(align <= 8)` states the contract). A raw allocator an
application installs with `PyMem_SetAllocator` is not covered by the
spike (unverified).

## The binding's memory

The binding allocates almost nothing, so allocations and frees pair by
construction (bindings.md, section 6, "The binding allocates almost
nothing"):

- **Borrow.** A string goes to core as the `str` object's own UTF-8
  (`PyUnicode_AsUTF8AndSize`, valid while the object lives), bytes through
  the buffer protocol, for the call's duration: core copies what it
  keeps. A string the datamodel hands back to core stays in a Python
  object the session holds. System variables and a chart's states are
  read from core when it supplies them, never copied.
- **Hold, don't copy.** The image bytes the compiler writes stay in a
  Python `bytes` object the `Host` references until
  `TA_HOST_EFFECT_SEGMENT_FREED`.
- **Rows, not allocations.** A session (its globals dict), a chart (its
  code objects) and a payload are rows of the handle tables
  (`src/datamodel/handles.c`), grown by doubling, slots reused, freed
  once in `ta_host_free`'s path.
- **Where:** `PyMem_Raw*` is called only in `src/datamodel/handles.c` and
  in `src/host.c`'s create, free and the host's allocator; nothing per
  call, per event or per evaluation. The lint rule
  `allocation-in-tables-and-host` checks it.

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
  and their ack take and give C structs. CBOR is for `ta_host_request`,
  the config and bytes that leave the process.
- **Acronyms** are all capitals or all lowercase (root `AGENTS.md`,
  "Acronyms"): `ta_python_otlp_writer`, `TA_PYTHON_IO_...`; in Python,
  constants in capitals and methods snake_case.
- **Layers**, each an ast-grep rule:
  - `extension-calls-core-and-python`: `src/` calls core (`ta_*`, `TA_*`),
    CPython's API (`Py*`, never a private `_Py*`) and a short list of C
    library functions (memory and strings), nothing else. A C library
    function the binding needs joins the list with its reason, in the
    commit that first calls it.
  - `datamodel-calls-environment-and-python`: `src/datamodel/` calls of
    core only `ta_datamodel_environment_*`, `ta_image_*` (the chart's
    expressions, read at `chart_attach`) and `ta_cbor_*` (`to_wire`,
    `from_wire`), as the ECMAScript datamodel does, and our own
    `ta_python_*`; never the host or the system (`datamodel.h`: "they must
    never call ta_system_* functions").
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
- **The wheel is abi3 from 3.13** (`Py_LIMITED_API 0x030D0000`;
  `pyproject.toml`, `mise.toml`). Every CPython function this file names
  is in the 3.13 limited API: a spike compiled
  `PyMem_RawMalloc`, `PyMem_RawFree`, `PyType_GetModuleByDef`,
  `PyType_GetModule`, `PyModule_GetState`, `PyType_FromModuleAndSpec`,
  `PyErr_SetInterruptEx`, `PyStructSequence_NewType`, `Py_CompileString`,
  `PyEval_EvalCode`, `PyDict_GetItemRef`, `PyList_GetItemRef`,
  `PyErr_GetRaisedException`, `PyErr_SetRaisedException`,
  `PyUnicode_AsUTF8AndSize`, `PyObject_GetAttrString`,
  `PyObject_CallMethod`, `PyErr_SetString`, the slots `Py_mod_exec`,
  `Py_mod_gil` (with `Py_MOD_GIL_NOT_USED`), `Py_tp_traverse`,
  `Py_tp_clear`, the flags `METH_METHOD | METH_FASTCALL | METH_KEYWORDS`
  and the typedefs `PyCMethod` and `PyCFunctionFastWithKeywords` with
  clang 22 against CPython 3.13.16's headers and `Py_LIMITED_API
  0x030D0000`, with no error; at `0x030C0000` (3.12),
  `PyMem_Raw*`, `PyDict_GetItemRef`, `PyList_GetItemRef`,
  `PyType_GetModuleByDef` and `Py_mod_gil` are undeclared. Module state
  comes from `PyType_GetModuleByDef` (it searches the type's MRO, so a
  method called on a subclass finds the module: CPython's C API
  documentation of that function) and `METH_METHOD`.
- **The free-threaded build is for tests only.** The limited API cannot
  be compiled for a free-threaded interpreter before 3.15 (`Python.h`
  refuses `Py_LIMITED_API` with `Py_GIL_DISABLED`: 3.13, line 51), so the
  test task builds the same sources again without `Py_LIMITED_API` for
  the free-threaded interpreter (`TA_PYTHON_FREE_THREADED` in
  `mise.toml`, installed by uv: `CPython.free_threaded_python`). Every
  function above is in the limited API from 3.13 and in the full API of
  3.14t alike, so the sources need no compatibility macros: write
  `PyDict_GetItemRef`, `PyList_GetItemRef` and `Py_mod_gil` directly.
  A free-threaded wheel waits for abi3t (3.15).

## Rules the suite cannot see, each with a test of its own

The conformance suite cannot see these, so each has a test of its own in
`tests/`. They come from the guide and from the spikes and their review
(the reasoned-analysis pass of 2026-10-12: commit `2203218f` and the
review behind it).

- **Dropping a payload's last reference can run Python code** (a
  finalizer, a weakref callback), and core calls `release` outside a run
  too: at the start of `ta_host_run`, in the ack, in `ta_host_free`, in
  requests and in deliver, where reentry is not refused. Defer the last
  drop to a list emptied after the outer call into core returns
  (bindings.md, section 8, line 133).
- **Handles are slots in two tables** (`src/datamodel/handles.c`), one for
  payloads (with reference counts) and one for sessions, each a slot and a
  generation, never a pointer (bindings.md, section 8, line 131).
- **Text is valid UTF-8.** A lone surrogate is refused
  (`TA_ERROR_INVALID`), never replaced (bindings.md, section 9, line 142;
  `values.txt` "invalid a lone surrogate in text").
- **One globals dict per session and no locals:** with separate locals,
  generator expressions and lambdas do not see session variables.
- **System variables stay as they were** (`_event`, `_sessionid`,
  `_name`, `_ioprocessors`, `_x`): after each evaluation a changed one is
  restored and the evaluation is `error.execution`; `assign` checks that
  its location is declared; `variable` and `serialize` skip
  `__builtins__` (bindings.md, section 8, line 132: "Where the language's
  scope cannot refuse it, check after the evaluation and restore").
- **Test `bool` before `int`** when converting values: `bool` is a
  subclass of `int` (bindings.md, section 9).
- **A `BaseException` that is not an `Exception`** (`KeyboardInterrupt`,
  `SystemExit`) still gives `error.execution` to the chart, and the
  binding raises the same object again after the run (bindings.md,
  section 8, line 131).
- **Handler closures make cycles:** fill the CPython slots
  `Py_tp_traverse` and `Py_tp_clear` with our functions (bindings.md,
  section 6, line 117).
- **The README says** that nesting is limited to 64, which stops cycles
  but not a value that shares subtrees (`a = [a, a]` sixty times), so
  charts are trusted code; and that Python values lie outside the host's
  allocator (bindings.md, section 8, "Say what the language cannot
  bound").

## The module's CBOR: `tinyactors.encode` and `tinyactors.decode`

The module exposes its CBOR conversion as two top-level functions, shaped
like `json.dumps`/`json.loads` and `cbor2.dumps`/`cbor2.loads` but named
for what they do (bindings.md, section 9):

- `tinyactors.encode(value) -> bytes`: the value table of
  `docs/values.md` written with `ta_cbor_encode` (core's token lists,
  `include/tinyactors/cbor.h`) from the tokens `src/values.c` builds;
- `tinyactors.decode(data) -> object`: `ta_cbor_decode` into tokens, then
  the Python objects of the same table (`Address`, `UNDEFINED`, `bytes`,
  `int` to 64 bits). Malformed data, a value outside the table, a lone
  surrogate, nesting past 64 are refused with an exception, never turned
  into a value (`docs/values.md` names the exception).

They are the same conversion the API uses for requests, replies, config
and event data, not a second codec: one `values.c`, called by both. Each
has its row in `docs/api-map.md` (`ta_cbor_encode`, `ta_cbor_decode`).
The adapter (`conform/`) reads and writes the driver's frames with them,
and imports nothing but `tinyactors` and the standard library (the
adapters' lint refuses any other package), so no third-party codec stands
between the suite and the binding. The vectors then go through this
binding's codec both ways, so a value that is mapped wrong the same way
in `decode` and `encode` passes them: the binding's own value tests
(`tests/`, against `values.txt`'s `python` lines and `docs/values.md`)
are what catch it.

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
  files: the build scripts' portability (`mise/lib/build_scripts.rb`) and
  acronyms (`mise/lib/acronym_case.rb`, `.py` files included).
- `mise run conform [-- runner options]`: the suite against
  `python -m conform`, with `--kind binding`, on the native datamodel and
  on quickjs (the differential run: both must pass); `-- --milestone <M>`
  runs that milestone's set only (`tinyactors-test/suites/milestones.txt`).
  Without `--milestone`, the whole suite. Fails while there is no adapter.
- `mise run test`: lint, build, both interpreters, and the suite's set
  named by `MILESTONE` in `mise/cpython.rb` (`conform -- --milestone
  <MILESTONE>`; that constant is the one place the set is named) once the
  adapter exists (until then it prints `conform skipped: no adapter yet`).
  Its `#MISE sources` list this project's files, core's and the suite's,
  so the root's `mise run test` reruns it when any of them changed (root
  `AGENTS.md`, "Tests: what to run"). An exhaustive test goes behind a
  `--full` flag (root `AGENTS.md`, "Where a new test goes"), and the
  root's `test` step passes it then.
- `mise run wheel`: one abi3 wheel per platform; it aborts until
  packaging is built.

The adapter (`conform/`) runs on the pinned interpreter. Put the built
extension on its import path in the conform task (`PYTHONPATH`), never in
the adapter: its lint refuses `sys.path`.

## Done means

From bindings.md, section 13:

- The mapping table exists, and every API item has a core function or
  method behind it.
- The queue loop handles every run status and every effect in section 4.
- The suite passes against the binding's command: protocol vectors,
  scenarios and W3C, on every datamodel it offers.
- No defaults, limits, methods or messages of the binding's own.
- Nothing C does is done again in the language.

## Commits and the changelog

Commit subjects are the changelog's input (root `AGENTS.md`, "Releases
and changelogs"): scope `cpython` (`feat(cpython): ...`), `!` for a
breaking change with a body that says what a caller does.

## Code search, static checks and design claims

- **ast-grep for code search and rewrites** (`ast-grep run -p ... -r
  ...`), `rg` only for plain text. Invoke `ast-grep`, never `sg`. A new
  static check is an ast-grep rule in `tools/lint/rules/` with its test in
  `tools/lint/tests/` and its snapshot (`ast-grep test -U`, then read it).
- **Design claims are checked** with the reasoned-analysis skill
  (`.agents/skills/reasoned-analysis/SKILL.md`): a scenario in this
  design, evidence (`path:line`, a primary source or a spike) or the word
  unverified, and a refutation by a fresh agent before a finding changes
  the design, the guide or a request to core.

## Gaps in core

This project never patches `tinyactors-core` or the suite
(`tinyactors-test`), and works around nothing in them. A gap in either is
reported as a request to core: what is missing, why, and which suite case
or test shows it. The binding waits for the change in core.

## Layout

The Node binding has the same files on purpose, and the two are compared
file by file.

```
README.md, CHANGELOG.md, mise.toml, pyproject.toml, sgconfig.yml
ROADMAP.md           the work in progress: plan, progress, how it runs, requests to core
docs/api-map.md      each API item → ta_* function, request method or effect (checked)
docs/values.md       the Python side of the value table
docs/errors.md       TinyactorsError: what core refused, unchanged
docs/datamodel.md    the "python" datamodel: what a session sees, the rules it keeps
docs/api.md          the API's shape: the decisions the guide leaves open
src/                 extension.h, module.c, host.c, loop.c, effects.c, request.c, values.c,
                     datamodel/{datamodel.h, datamodel.c, handles.c, expressions.c}
conform/             the suite's adapter, python -m conform; not in the wheel
tests/               what the suite cannot see: threads, GC, asyncio, refusals
tools/lint/          rules/, tests/, utils/ (ast-grep)
mise/cpython.rb      what the tasks share
mise/tasks/          build, test, conform, lint, wheel
build/               core's amalgamation and the extension (not in git)
```

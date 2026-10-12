# AGENTS.md: tinyactors-cpython

The Python extension `tinyactors`: one C extension module, written against
the CPython C API, that drives `ta_host` (README.md). The repository root's
`AGENTS.md` applies too; this file adds what the Python session needs.
Read first: [`tinyactors-core/docs/bindings.md`](../tinyactors-core/docs/bindings.md)
(the guide), the public headers it names,
[`ROADMAP.md`](../ROADMAP.md), step 8 (the plan of both bindings), and
this project's [`ROADMAP.md`](ROADMAP.md) (this session's plan, progress
and requests to core). Build from the guide and the headers, not from an
older binding or the proposals' sketches.

## Whom it serves

First the **application developer writing Python**: chart logic in Python
(the native `"python"` datamodel), a `Host` driven from a script or from
asyncio, values that are Python objects. Then the **binding author**, held
to the guide by the conformance suite, and the **maintainers**, who run one
suite against every implementation (ROADMAP.md, step 8, "Rules for the
work"). Every API item is a host input, a request or the handling of an
effect (`docs/api-map.md`); what core does not have is not in the API.

## Session rules

- **This session owns `tinyactors-cpython/` and nothing else.** A Node
  session owns `tinyactors-napi/` at the same time, in the same checkout;
  a coordinating session owns `tinyactors-core` and `tinyactors-test`.
- **This project's `ROADMAP.md` is the session's.** It holds the plan
  and the progress of the milestone the session is on (`MILESTONE` in
  `mise/cpython.rb`), and its "Requests to core". The session keeps it up
  to date as it works, and never edits the root `ROADMAP.md`: the
  coordinator reads this file, moves a request into the root's
  "Requests to core", and marks a milestone done there.
- **Core is frozen.** A gap in core (or in the suite) is written under
  "Requests to core" in this project's `ROADMAP.md`: what is missing, why,
  and which suite case or test shows it. The binding waits for it or
  works around nothing; it never patches core or the suite. The
  coordinator makes the change and says when to go on. Commit the request
  alone (`docs(cpython): request to core: ...`).
- **One checkout, on `main`, commits of this project's paths only:**
  `git commit -- tinyactors-cpython/` (`git add` a new file by its path
  first). Never `git add -A`, `git add .` at the root, `git commit -a`,
  `git stash`, `git checkout -- .` or `git reset --hard`: they take or
  undo the other sessions' work. History stays linear: `git pull
  --rebase` when `main` moved.
- **`.git/index.lock` belongs to another session's git command.** When
  git says the index is locked, wait a few seconds and run the same
  command again, as often as it takes. Never delete the lock: the other
  command is still writing the index, and removing it can lose that
  session's staged work.
- **Commit subjects are the changelog's input** (root `AGENTS.md`,
  "Releases and changelogs"): scope `cpython` (`feat(cpython): ...`), `!`
  for a breaking change with a body that says what a caller does.
- **Run this project's `mise run test`** before every commit, and the
  suite against the adapter (`mise run conform`). Never the root
  `mise run test` (it runs every project for 20 minutes) and never
  `tinyactors-core`'s build or tests: `mise run build` here builds core's
  amalgamation into `build/core` through core's `amalgamate` task, and
  that is the only core task this session runs.
- **ast-grep for code search and rewrites** (`ast-grep run -p ... -r
  ...`), `rg` only for plain text. Invoke `ast-grep`, never `sg`. A new
  static check is an ast-grep rule in `tools/lint/rules/` with its test in
  `tools/lint/tests/` and its snapshot (`ast-grep test -U`, then read it).
- **Design claims are checked** with the reasoned-analysis skill
  (`.agents/skills/reasoned-analysis/SKILL.md`): a scenario in this
  design, evidence (`path:line`, a primary source or a spike) or the word
  unverified, and a refutation by a fresh agent before a finding changes
  the plan, the guide or "Requests to core".

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
and this binding gives it `PyMem_RawMalloc` and `PyMem_RawFree`
(ROADMAP.md, step 8, M0 item 4), which are in the limited API from 3.13
(CPython's `Include/pymem.h`, 3.13: lines 90-98), the wheel's floor. The
raw domain needs no GIL ("Memory allocator which doesn't require the GIL
to be held", the same file, line 91), which a free-threaded build has
none of anyway. Python's own objects stay outside the host's allocator;
the README says so (bindings.md, section 8). The two functions take
core's parameter lists (`void *context, size_t size, size_t align` and
`void *context, void *ptr, size_t size`), which the lint exempts by that
signature. `malloc`, `calloc`, `realloc` and `free` stay refused by the
layer lint.

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

## C rules

- **At most three parameters** for every C function this binding defines
  or declares, static helpers and callback types included (root
  `AGENTS.md`; ROADMAP.md, step 8). A function whose signature a typedef
  fixes is exempt by that typedef, not by name: `PyCMethod`
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
- **Layers** (ROADMAP.md, step 8, "Gates"), each an ast-grep rule:
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
  `pyproject.toml`, `mise.toml`). Every CPython function this file and
  ROADMAP.md name for M1 is in the 3.13 limited API: a spike compiled
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

## Pitfalls M1 handles, each with a test of its own

The suite cannot see these (ROADMAP.md, step 8, M1: "spikes and their
review: the reasoned-analysis pass of 2026-10-12").

- **Dropping a payload's last reference can run Python code** (a
  finalizer, a weakref callback). Core calls `release` outside a run too:
  at the start of `ta_host_run`, in the ack, in `ta_host_free`, in
  requests and in deliver, where reentry is not refused. Defer the last
  drop to a list emptied after the outer call into core returns
  (bindings.md, section 8, line 133).
- **Two slot tables** (`src/datamodel/handles.c`), for payloads (with
  reference counts) and for sessions, each a slot and a generation, never
  a pointer (bindings.md, section 8, line 131).
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
- **`bool` before `int`** when converting values (bindings.md, section 9).
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
for what they do (ROADMAP.md, step 8, M1; bindings.md, section 9):

- `tinyactors.encode(value) -> bytes`: the value table of
  `docs/values.md` written with `ta_cbor_encode` (core's token lists,
  `include/tinyactors/cbor.h`) from the tokens `src/values.c` builds;
- `tinyactors.decode(data) -> object`: `ta_cbor_decode` into tokens, then
  the Python objects of the same table (`Address`, `UNDEFINED`, `bytes`,
  `int` to 64 bits). Malformed data, a value outside the table, a lone
  surrogate, nesting past 64 are refused with an exception, never turned
  into a value (which exception is decided in M1, in `docs/values.md`).

They are the same conversion the API uses for requests, replies, config
and event data, not a second codec: one `values.c`, called by both. Each
has its row in `docs/api-map.md` (`ta_cbor_encode`, `ta_cbor_decode`).
The adapter (`conform/`) reads and writes the driver's frames with them,
and imports nothing but `tinyactors` and the standard library (the
adapters' lint refuses any other package). The vectors then go through
this binding's codec both ways, so a value that is mapped wrong the same
way in `decode` and `encode` passes them: the binding's own value tests
(`tests/`, against `values.txt`'s `python` lines and `docs/values.md`)
are what catch it (ROADMAP.md, step 8, M1, "Known limit").

## Tasks

- `mise run build`: core's amalgamation into `build/core`
  (`mise run //tinyactors-core:amalgamate -- --out build/core`); M1 adds
  the extension, compiled with `Toolchain::Target` (root
  `mise/lib/toolchain.rb`; never a compiler from `PATH`, never `$CC`).
- `mise run lint`: the rules' tests, the rules over `src/`, the adapters'
  lint (`mise run //tinyactors-test:lint -- conform`: the adapter imports
  only `tinyactors` and the standard library), `docs/api-map.md` against
  core (`mise/lib/api_map.rb`), and the root's checks on this project's
  files: the build scripts' portability (`mise/lib/build_scripts.rb`) and
  acronyms (`mise/lib/acronym_case.rb`, `.py` files included).
- `mise run conform [-- runner options]`: the suite against
  `python -m conform`, with `--kind binding`, on the native datamodel and
  on quickjs (the differential run); `-- --milestone M1` runs M1's set
  only (`tinyactors-test/suites/milestones.txt`). Without `--milestone`,
  the whole suite. Fails while there is no adapter.
- `mise run test`: lint, build, both interpreters, and the suite's set of
  the milestone the session is on (`conform -- --milestone M1`, from
  `MILESTONE` in `mise/cpython.rb`, the one place to move it) once the
  adapter exists (until then it prints `conform skipped: no adapter yet`).
  Its `#MISE sources` list this project's files, core's and the suite's.
  An exhaustive test goes behind a `--full` flag (root `AGENTS.md`,
  "Where a new test goes"), and the root's `test` step passes it then.
- `mise run wheel`: not yet (M5).

The adapter (`conform/`) runs on the pinned interpreter. Put the built
extension on its import path in the conform task (`PYTHONPATH`), never in
the adapter: its lint refuses `sys.path`.

## M1 done means

M1 of ROADMAP.md, step 8, for this binding; the plan and its progress are
in this project's [`ROADMAP.md`](ROADMAP.md).

- **Scope:** the `tinyactors` module and its `Host` type;
  `ta_host_create` from Python options; images, boot, spawn, send,
  terminate; the loop with every run status; `DONE`, `ERROR`, `LOG`;
  `request` with `TinyactorsError` (code, reason, path); the CBOR
  conversion with the value table (`Address`, `UNDEFINED`, big integers,
  bytes) and `tinyactors.encode`/`tinyactors.decode`; the `"python"`
  datamodel for `<data>`, `cond`, `assign`, `<script>`, `_event`, `In()`,
  `<send>` with params, `<foreach>`; quickjs beside it; the adapter
  (`conform/`); and each pitfall above with a test of its own.
- **`SimulatedClock`**, `increment(ms)` (xstate's shape, our meaning): a
  host created with it arms no timer; `increment(ms)` calls
  `ta_host_advance_time` and runs the loop until idle; inputs run the
  turn at the current time. It has its row in `docs/api-map.md`
  (`ta_host_advance_time`, `ta_host_run`), and the adapter uses it for
  the scenarios' `advance` steps.
- **Gates**, all in `mise run test`: the suite's M1 set
  (`--milestone M1`: every protocol vector file, the basics scenario and
  the outcome canaries) on the native datamodel and on quickjs; the
  mapping table; the layer lints, the prefix, `no-mutable-globals`; both
  interpreters (the abi3 build on 3.13, the free-threaded test build).
- **Then the API review**, by the coordinator, both bindings side by
  side: how a Python chart reads, how a Node program reads. M2 starts
  after it, when the coordinator marks M1 done in the root `ROADMAP.md`
  and this session moves `MILESTONE` on.

## Done means

From bindings.md, section 13:

- The mapping table exists, and every API item has a core function or
  method behind it.
- The queue loop handles every run status and every effect in section 4.
- The suite passes against the binding's command: protocol vectors,
  scenarios and W3C, on every datamodel it offers.
- No defaults, limits, methods or messages of the binding's own.
- Nothing C does is done again in the language.

## Layout

The file tree is ROADMAP.md's (step 8, "tinyactors-cpython"); the Node
binding has the same files on purpose, and the audit after each milestone
compares them file by file.

```
README.md, CHANGELOG.md, mise.toml, pyproject.toml, sgconfig.yml
ROADMAP.md           this session's plan, progress and requests to core
docs/api-map.md      each API item → ta_* function, request method or effect (checked)
docs/values.md       the Python side of the value table
src/                 extension.h, module.c, host.c, loop.c, effects.c, request.c, values.c,
                     datamodel/{datamodel.c, handles.c, expressions.c}   (M1)
conform/             the suite's adapter, python -m conform; not in the wheel   (M1)
tests/               what the suite cannot see: threads, GC, asyncio, refusals   (M1)
tools/lint/          rules/, tests/, utils/ (ast-grep)
mise/cpython.rb      what the tasks share
mise/tasks/          build, test, conform, lint, wheel
build/               core's amalgamation and the extension (not in git)
```

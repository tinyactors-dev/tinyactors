# AGENTS.md: tinyactors-cpython

The Python extension `tinyactors`: one C extension module, written against
the CPython C API, that drives `ta_host` (README.md). The repository root's
`AGENTS.md` applies too; this file adds what the Python session needs.
Read first: [`tinyactors-core/docs/bindings.md`](../tinyactors-core/docs/bindings.md)
(the guide), the public headers it names, and
[`ROADMAP.md`](../ROADMAP.md), step 8. Build from the guide and the
headers, not from an older binding or the proposals' sketches.

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
- **Core is frozen.** A gap in core (or in the suite) is written under
  "Requests to core" in `ROADMAP.md`: what is missing, why, and which
  suite case or test shows it. The binding waits for it or works around
  nothing; it never patches core. The coordinator makes the change and
  says when to go on. Commit the request alone (`git commit -- ROADMAP.md`,
  `docs: request to core: ...`).
- **One checkout, on `main`, commits scoped to this path:**
  `git commit -- tinyactors-cpython/`. Never `git add -A`, `git commit -a`,
  `git stash`, `git checkout -- .` or `git reset --hard`: they take or
  undo the other sessions' work. History stays linear.
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
(ROADMAP.md, step 8, M0 item 4): the raw domain needs no GIL ("the GIL is
not held when the allocator is called", CPython's
`Include/cpython/pymem.h`, 3.11: line 73), which a free-threaded build
has none of anyway. Python's own
objects stay outside the host's allocator; the README says so
(bindings.md, section 8). Core asks for an alignment (`align`): check
what `PyMem_RawMalloc` guarantees against what core asks before relying
on it (unverified). The two functions take core's parameter lists
(`void *context, size_t size, size_t align` and `void *context, void
*ptr, size_t size`), which the lint exempts by that signature.

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
- **The free-threaded build is for tests only.** The wheel is abi3 from
  3.11 (`Py_LIMITED_API`); the test task builds the same sources again
  without it for the free-threaded interpreter (`TA_PYTHON_FREE_THREADED`
  in `mise.toml`, installed by uv: `CPython.free_threaded_python`). The
  functions that differ (`PyDict_GetItemRef`, `PyList_GetItemRef`,
  `Py_mod_gil`, limited only from 3.13) sit behind `TA_PYTHON_*` macros
  in `src/extension.h`; module state comes from `PyType_GetModule` and
  `METH_METHOD` (ROADMAP.md, step 8, "The free-threaded build is for
  tests only").

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

## Tasks

- `mise run build`: core's amalgamation into `build/core`
  (`mise run //tinyactors-core:amalgamate -- --out build/core`); M1 adds
  the extension, compiled with `Toolchain::Target` (root
  `mise/lib/toolchain.rb`; never a compiler from `PATH`, never `$CC`).
- `mise run lint`: the rules' tests, the rules over `src/`, the adapters'
  lint (`mise run //tinyactors-test:lint -- conform`: the adapter imports
  only `tinyactors`), and `docs/api-map.md` against core
  (`mise/lib/api_map.rb`).
- `mise run conform [-- runner options]`: the suite against
  `python -m conform`, with `--kind binding`, on the native datamodel and
  on quickjs (the differential run). Fails while there is no adapter.
- `mise run test`: lint, build, both interpreters, and conform once the
  adapter exists (until then it prints `conform skipped: no adapter yet`).
  Its `#MISE sources` list this project's files, core's and the suite's.
  An exhaustive test goes behind a `--full` flag (root `AGENTS.md`,
  "Where a new test goes"), and the root's `test` step passes it then.
- `mise run wheel`: not yet (M5).

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

## Layout

The file tree is ROADMAP.md's (step 8, "tinyactors-cpython"); the Node
binding has the same files on purpose, and the audit after each milestone
compares them file by file.

```
README.md, CHANGELOG.md, mise.toml, pyproject.toml, sgconfig.yml
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

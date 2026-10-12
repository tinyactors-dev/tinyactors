# AGENTS.md: tinyactors-napi

`@tinyactors/node`: one Node-API addon, written in C, that drives
`ta_host` from Node.js and Bun (README.md). The repository root's
`AGENTS.md` applies too; this file adds what the Node session needs. Read
first: [`tinyactors-core/docs/bindings.md`](../tinyactors-core/docs/bindings.md)
(the guide), the public headers it names,
[`ROADMAP.md`](../ROADMAP.md), step 8 (the plan of both bindings), and
this project's [`ROADMAP.md`](ROADMAP.md) (this session's plan, progress
and requests to core). Build from the guide and the headers, not from an
older binding (the 0.2.0 package in git history) or the proposals'
sketches.

## Whom it serves

First the **application developer writing JavaScript** on Node.js or Bun:
chart logic in ECMAScript (the native `"ecmascript"` datamodel, in the
application's own engine), a `Host` on the runtime's event loop, values
that are JavaScript values. Then the **binding author**, held to the guide
by the conformance suite, and the **maintainers**, who run one suite
against every implementation (ROADMAP.md, step 8, "Rules for the work").
Every API item is a host input, a request or the handling of an effect
(`docs/api-map.md`); what core does not have is not in the API.

The addon is the binding: it defines the classes, runs the loop,
dispatches effects, converts values and implements the native datamodel
in C. `index.mjs` loads the platform's `.node` file and re-exports it,
nothing else (ROADMAP.md, step 8, "What changed": there is no JavaScript
above the addon).

## Session rules

- **This session owns `tinyactors-napi/` and nothing else.** A Python
  session owns `tinyactors-cpython/` at the same time, in the same
  checkout; a coordinating session owns `tinyactors-core` and
  `tinyactors-test`.
- **This project's `ROADMAP.md` is the session's.** It holds the plan
  and the progress of the milestone the session is on (`MILESTONE` in
  `mise/napi.rb`), and its "Requests to core". The session keeps it up to
  date as it works, and never edits the root `ROADMAP.md`: the
  coordinator reads this file, moves a request into the root's
  "Requests to core", and marks a milestone done there.
- **Core is frozen.** A gap in core (or in the suite) is written under
  "Requests to core" in this project's `ROADMAP.md`: what is missing, why,
  and which suite case or test shows it. The binding waits for it or
  works around nothing; it never patches core or the suite. The
  coordinator makes the change and says when to go on. Commit the request
  alone (`docs(napi): request to core: ...`).
- **One checkout, on `main`, commits of this project's paths only:**
  `git commit -- tinyactors-napi/` (`git add` a new file by its path
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
  "Releases and changelogs"): scope `napi` (`feat(napi): ...`), `!` for a
  breaking change with a body that says what a caller does. The package
  continues the release line of `@tinyactors/node` (tags
  `tinyactors-napi/v*`, last 0.2.0), so every `feat`, `fix` and
  `refactor` lands in its next changelog.
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
- **pnpm** manages the package's development dependencies, **node** and
  **bun** run it (`mise.toml`).
- **Design claims are checked** with the reasoned-analysis skill
  (`.agents/skills/reasoned-analysis/SKILL.md`): a scenario in this
  design, evidence (`path:line`, a primary source or a spike) or the word
  unverified, and a refutation by a fresh agent before a finding changes
  the plan, the guide or "Requests to core".

## Names

Our names carry the prefix `ta_napi_*`: every function this addon defines
or declares, static helpers included, and its types and globals. A
platform's name appears only where the platform requires it: the module's
entry points `napi_register_module_v1` and
`node_api_module_get_api_version_v1`, which `NAPI_MODULE_INIT()` defines
(Node.js's `include/node/node_api.h`). A callback given to Node-API (a
`napi_callback`, a `napi_finalize`) is our function, with our name. When
you write a name in prose, say whose it is. The lint rule
`functions-carry-the-prefix` checks functions. In JavaScript the API's
names follow the root's acronym rule (`sessionID`, `toOTLP`).

## The host's allocator

The host allocates through the `ta_allocator` its environment gives it
(`tinyactors-core/include/tinyactors/allocator.h`; bindings.md, section 6),
and this addon gives it `malloc` and `free`, reporting each block to the
engine with `napi_adjust_external_memory` (ROADMAP.md, step 8, M0 item 4),
so the garbage collector sees the host's memory. That call takes a
`node_api_basic_env` (Node.js 22's `include/node/js_native_api.h:493`),
so it may run where only basic-env functions may, such as a finalizer
that frees a host. The two functions take core's parameter lists (`void
*context, size_t size, size_t align` and `void *context, void *ptr,
size_t size`), which the lint exempts by that signature; the environment
they need comes through `context`.

**Alignment: `align` is at most 8, and `malloc` gives at least 8.** The
host passes 8 on every allocation it makes
(`tinyactors-core/src/host/host.c:25`, `host_alloc`, and `:287`, the host
itself; no other call in `src/host`, `src/system` or `src/base` reaches
the allocator), the largest alignment a region object needs is 8
(`src/base/config.h:46`, `TA_ALIGN_MAX`), and the allocator's contract
says "a power of two, at most 8 from this library"
(`include/tinyactors/allocator.h:9`). `malloc` returns memory aligned
for any object of fundamental alignment, that is `_Alignof(max_align_t)`
(C11, 7.22.3), and the pinned clang 22 gives `max_align_t` an alignment
of 16 on x86_64 and aarch64 Linux and on x86_64 macOS, and 8 on arm64
macOS: at least 8 on each. So `malloc` meets what core asks, and the
addon's `alloc` need not align by hand (an `assert(align <= 8)` states
the contract).

## C rules

- **At most three parameters** for every C function this addon defines or
  declares, static helpers and callback types included (root
  `AGENTS.md`; ROADMAP.md, step 8). A function whose signature a typedef
  fixes is exempt by that typedef, not by name:
  `napi_threadsafe_function_call_js` (four parameters), `ta_allocator`'s
  two, and, should a rule grow, `napi_callback` and `napi_finalize`.
  `tools/lint/utils/fixed-signature.yml` lists them by their parameter
  types; add a typedef there, with its header and line, before a function
  it fixes fails the rule.
- **Bytes are `ta_bytes` in and `ta_buffer *` out**, never a pointer and a
  length, in every function (`buffers-are-ta-buffer`).
- **No serialization on the hot path** (root `AGENTS.md`): spawn, send,
  send_many, terminate, deliver, advance_time, resume, run, the effects
  and their ack take and give C structs. CBOR is for `ta_host_request`,
  the config and bytes that leave the process.
- **Acronyms** are all capitals or all lowercase (root `AGENTS.md`,
  "Acronyms"): `ta_napi_otlp_writer`, `TA_NAPI_IO_...` in C;
  `sessionID`, `OTLPExporter`, `ioProcessors` in JavaScript and
  TypeScript.
- **Layers** (ROADMAP.md, step 8, "Gates"), each an ast-grep rule:
  - `extension-calls-core-and-node-api`: `src/` calls core (`ta_*`,
    `TA_*`), Node-API (`napi_*`, `node_api_*`, `NAPI_*`) and a short list
    of C library functions (memory, strings, and `malloc`, `calloc`,
    `realloc`, `free` for the host's allocator), nothing else: no libuv
    (`uv_*`; Bun has none) and no V8. A C library function the addon needs
    joins the list with its reason, in the commit that first calls it.
  - `datamodel-calls-environment-and-node-api`: `src/datamodel/` calls of
    core only `ta_datamodel_environment_*`, `ta_image_*` (the chart's
    expressions, read at `chart_attach`) and `ta_cbor_*` (`to_wire`,
    `from_wire`), as the ECMAScript datamodel does, and our own
    `ta_napi_*`; never the host or the system (`datamodel.h`: "they must
    never call ta_system_* functions").
  - `one-timer-in-loop`: the global timers (`setTimeout`, `setInterval`,
    `setImmediate` and their clear functions) appear in `src/loop.c`
    only.
- **No mutable globals** (`no-mutable-globals`): no global and no static
  local the addon writes. The module is initialized once per environment
  (the main thread and each worker), and a `napi_value` or `napi_ref` of
  one environment means nothing in another, so state lives in the
  environment's instance data (`napi_set_instance_data`) or on the
  `Host`. A constant is a `const` object (`static const char *const
  names[]`, `static const napi_type_tag`; `static const char *p` is a
  mutable pointer). Arrays of `napi_property_descriptor`, the tables
  `napi_define_class` takes, are exempt by type.

## Pitfalls M1 handles, each with a test of its own

The suite cannot see these (ROADMAP.md, step 8, M1: "spikes and their
review: the reasoned-analysis pass of 2026-10-12").

- **Dropping a payload's last reference can run JavaScript** (a
  finalizer). Core calls `release` outside a run too: at the start of
  `ta_host_run`, in the ack, in `ta_host_free`, in requests and in
  deliver, where reentry is not refused. Defer the last drop to a list
  emptied after the outer call into core returns (bindings.md, section 8,
  line 133).
- **Two slot tables** (`src/datamodel/handles.c`), for payloads (with
  reference counts) and for sessions, each a slot and a generation, never
  a pointer (bindings.md, section 8, line 131).
- **Text is valid UTF-8.** A lone surrogate is refused
  (`TA_ERROR_INVALID`); Node-API replaces it with U+FFFD silently, so the
  addon checks (`String.prototype.isWellFormed`) (bindings.md, section 9,
  line 142).
- **Expressions are compiled once as a strict-mode function inside
  `with (scope)`**, as the quickjs datamodel does, so an undeclared
  assignment throws (`tinyactors-core/src/datamodel_ecmascript/datamodel_ecmascript.c:224-227`);
  `assign` keeps quickjs's check of the location (the same file, 418).
- **One handle scope per vtable entry:** a run is one native call, so
  handles opened by the entries would pile up until it returns
  (`napi_open_handle_scope` around each entry).
- **Reference counts live in the slot table,** one `napi_ref` per slot,
  deleted at zero: a finalizer may call only basic-env functions, and
  `napi_reference_unref` is not one (it takes a `napi_env`;
  `napi_delete_reference` takes a `node_api_basic_env`: Node.js 22's
  `include/node/js_native_api.h:339, 355`).
- **`napi_get_value_int64` saturates silently:** ids are always `BigInt`,
  and a number in event data is checked with `Number.isSafeInteger`
  (bindings.md, section 9; `docs/values.md`).
- **`napi_type_tag_object` on every wrapped object,** so a method called
  on the wrong object is refused rather than misread.
- **The timer is the global `setTimeout`,** ref'd while a deadline is
  pending: Bun has no `uv_timer_*` (oven-sh/bun#18546). Whether a pending
  timer keeps the process alive is documented, not a policy (bindings.md,
  section 11).
- **The README says chart code is trusted:** it runs in the application's
  realm, and Node-API cannot make another.
- **An actor reference is a string** (decided 2026-10-12, ROADMAP.md,
  step 8, M1, with the analysis and its measurements): `values.c` reads
  tag 32 as its text and never writes it, as quickjs does
  (`docs/values.md`).
- **The README says** that nesting is limited to 64, which stops cycles
  but not a value that shares subtrees (`a = [a, a]` sixty times), so
  charts are trusted code.

## The package's CBOR: `encode` and `decode`

The addon exposes its CBOR conversion as two exports of the package,
`encode(value) -> Uint8Array` and `decode(bytes) -> value` (the names and
shapes of `cbor-x` and `cbor2`'s `encode`/`decode`; ROADMAP.md, step 8,
M1; bindings.md, section 9): `encode` builds core's tokens from the value
table of `docs/values.md` in `src/values.c` and writes them with
`ta_cbor_encode` (`include/tinyactors/cbor.h`); `decode` reads with
`ta_cbor_decode` and builds JavaScript values by the same table (`BigInt`
past 2^53, `Uint8Array`, an actor reference as its address text). What
it cannot read is refused with an exception, never turned into a value
(which exception is decided in M1, in `docs/values.md`).

They are the same conversion the API uses for requests, replies, config
and event data, not a second codec: one `values.c`, called by both. Each
has its row in `docs/api-map.md` (`ta_cbor_encode`, `ta_cbor_decode`),
and `index.d.ts` types them. The adapter (`conform.mjs`) reads and writes
the driver's frames with them, and imports nothing but
`@tinyactors/node` and `node:` built-ins (the adapters' lint refuses any
other package). The vectors then go through this binding's codec both
ways, so a value that is mapped wrong the same way in `decode` and
`encode` passes them: the binding's own value tests (`test/`, against
`docs/values.md` and `values.txt`'s `ecmascript` lines) are what catch
it (ROADMAP.md, step 8, M1, "Known limit").

## Tasks

- `mise run build`: core's amalgamation into `build/core`
  (`mise run //tinyactors-core:amalgamate -- --out build/core`); M1 adds
  the addon, compiled with `Toolchain::Target` (root
  `mise/lib/toolchain.rb`; never a compiler from `PATH`, never `$CC`).
  `Target#plugin` links a loadable module, and `Toolchain::ObjectFormat`
  already allows a Node-API addon's undefined `napi_*`.
- `mise run lint`: the rules' tests, the rules over `src/`, the adapters'
  lint (`mise run //tinyactors-test:lint -- conform.mjs`: the adapter
  imports only `@tinyactors/node` and `node:` built-ins),
  `docs/api-map.md` against core (`mise/lib/api_map.rb`), and the root's
  checks on this project's files: the build scripts' portability
  (`mise/lib/build_scripts.rb`) and acronyms (`mise/lib/acronym_case.rb`).
- `mise run conform [-- runner options]`: the suite against
  `conform.mjs`, with `--kind binding`, under node and under bun, each on
  the native datamodel and on quickjs (the differential run);
  `-- --milestone M1` runs M1's set only
  (`tinyactors-test/suites/milestones.txt`). Without `--milestone`, the
  whole suite. Fails while there is no adapter.
- `mise run test`: lint, build, both runtimes, and the suite's set of the
  milestone the session is on (`conform -- --milestone M1`, from
  `MILESTONE` in `mise/napi.rb`, the one place to move it) once the
  adapter exists (until then it prints `conform skipped: no adapter yet`).
  Its `#MISE sources` list this project's files, core's and the suite's.
  An exhaustive test goes behind a `--full` flag (root `AGENTS.md`,
  "Where a new test goes"), and the root's `test` step passes it then.
- `mise run pack`: not yet (M5).

The adapter imports the package by its name, `@tinyactors/node`, which
resolves to this directory through `package.json`'s `exports` (a package
may import itself by name): its lint refuses a path. Both runtimes do it:
a spike (2026-10-12) put this `package.json` beside an `index.mjs` that
exports `Host` and a `conform.mjs` that imports it from
`'@tinyactors/node'`, and node v22.23.2 and bun 1.4.1 (this project's
pins) each printed the export of `index.mjs`.

## M1 done means

M1 of ROADMAP.md, step 8, for this binding; the plan and its progress are
in this project's [`ROADMAP.md`](ROADMAP.md).

- **Scope:** the package and its `Host` class; `ta_host_create` from
  JavaScript options; images, boot, spawn, send, terminate; the loop on
  the runtime's event loop with every run status; `DONE`, `ERROR`, `LOG`;
  `request` with `TinyactorsError` (code, reason, path); the CBOR
  conversion with the value table (`BigInt`, `Uint8Array`, an actor
  reference as its address string) and the `encode`/`decode` exports;
  the native `"ecmascript"` datamodel for `<data>`, `cond`, `assign`,
  `<script>`, `_event`, `In()`, `<send>` with params, `<foreach>`;
  quickjs beside it; the adapter (`conform.mjs`); `index.mjs` and
  `index.d.ts`; and each pitfall above with a test of its own.
- **`SimulatedClock`**, `increment(ms)` (xstate's shape, our meaning): a
  host created with it arms no timer; `increment(ms)` calls
  `ta_host_advance_time` and runs the loop until idle; inputs run the
  turn at the current time. It has its row in `docs/api-map.md`
  (`ta_host_advance_time`, `ta_host_run`), and the adapter uses it for
  the scenarios' `advance` steps.
- **Gates**, all in `mise run test`: the suite's M1 set
  (`--milestone M1`: every protocol vector file, the basics scenario and
  the outcome canaries) under node and under bun, each on the native
  datamodel and on quickjs; the mapping table; the layer lints, the
  prefix, `no-mutable-globals`.
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

The file tree is ROADMAP.md's (step 8, "tinyactors-napi"); the Python
binding has the same files on purpose, and the audit after each milestone
compares them file by file.

```
README.md, CHANGELOG.md, mise.toml, package.json, sgconfig.yml
ROADMAP.md           this session's plan, progress and requests to core
index.mjs, index.d.ts   load the platform's .node and re-export it; the types   (M1)
docs/api-map.md      each API item → ta_* function, request method or effect (checked)
docs/values.md       the JavaScript side of the value table
src/                 addon.h, module.c, host.c, loop.c, effects.c, request.c, values.c,
                     datamodel/{datamodel.c, handles.c, expressions.c}   (M1)
conform.mjs          the suite's adapter, over the public API only   (M1)
test/                what the suite cannot see: liveness, workers, GC, refusals   (M1)
tools/lint/          rules/, tests/, utils/ (ast-grep)
mise/napi.rb         what the tasks share
mise/tasks/          build, test, conform, lint, pack
build/               core's amalgamation and the addon (not in git)
```

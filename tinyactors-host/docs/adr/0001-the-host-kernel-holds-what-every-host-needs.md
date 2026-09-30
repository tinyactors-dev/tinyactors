# ADR 0001: The host kernel holds what every host needs

**Status:** Accepted and implemented (2026-09-30); see the implementation notes. Turns `docs/proposals/host-kernel.md` into a decision, earlier than it planned, with the test host as the second consumer. Core ADR 0094 and kernel ADR 0002 depend on it.
**Date:** 2026-09-30

## Context

The repository has four layers (README, "Layers and responsibilities"):
1. core decides what inputs mean;
2. a host kernel queues, records and replays them;
3. a language binding owns values and their encoding;
4. the application owns storage and policy.

The kernel exists only as a convention today. napi keeps its code free of
Node-API in separate files, and `test-native` fails if Node-API reaches a
source outside `NODE_API_SOURCES` (`tinyactors-napi/mise/lib/compile.rb`).
What that code covers:
- scheduling, mailboxes, the ready and runnable queues, dead letters and
  `send()` waiters (`scheduler.c`);
- the router and the directory (`router.c`, `directory.c`);
- event-name caching, enumeration and effects (`events.c`, `enumerate.c`,
  `effects.c`);
- the clock and turn state (`system.c`), memory (`allocator.c`,
  `containers.c`) and hooks (`hooks.c`);
- adapters for core's callback interfaces (`adapters/`);
- loads and offloads (`loads.c`, `offloads.c`);
- the OTLP encoder (`otlp.c`);
- the input log (`inputs.c`).

The proposal planned to extract this "just before the second language host
starts, not before", so the interface would be shaped by a real second
user. Two things change that:
- **Core ADR 0094 moves the input log out of core.** It needs a home that
  every binding shares, or each binding reimplements the write-ahead log,
  which is the duplication that put it into core in the first place.
- **The test host can be the second user now.** It is a binding already: C
  with a Ruby driver, exercising core from scenarios. Today it drives core
  directly and implements no queueing. Giving it the kernel gives the
  kernel two consumers from the start, and gives the test host the hosted
  behaviour (mailboxes, routing, the log) that scenarios could not reach
  until now.

## Decision

### 1. A project: `tinyactors-host/`, prefix `tah_`

- **A C library,** built with the repository's toolchain (`Toolchain::Target`,
  AGENTS.md "Build scripts work the same on every host"). napi and the test
  host link it statically.
- **Its layout** is `include/tinyactors_host.h` (its public header, the
  host interface of section 3), `src/` and `tests/`, with its own
  `docs/adr/`, `mise.toml` and `mise/tasks/`.
- **Its tests run** natively, and under AddressSanitizer and UBSan.
- **Its `test` task** lists its sources in `#MISE sources`. The projects
  that build against it list it in theirs.
- **AGENTS.md's table of what reruns** gains a row: a change to the kernel
  reruns the kernel, napi, the test host and vscode.

### 2. Its rules are core's rules

- no I/O and no threads;
- memory only through the allocator it is given, so no libc allocation;
- no recursion (`check-recursion` covers it);
- at most three parameters per function;
- the acronym rules;
- no C standard library beyond the freestanding headers. The few memory
  helpers it needs are its own, as core's are, so it builds for
  `wasm32-freestanding` like core.

It includes core's public header and nothing of any language. A binding's
headers never enter it, so the Node-API guard in `compile.rb` becomes
unnecessary.

### 3. The host interface

A binding gives the kernel:
- **Values:** make, retain, release, and read (napi ADR 0011's model),
  through core's `ta_value` adapter.
- **The data model,** with its streaming codec (core ADR 0090 sections 1 and
  2). A data model encodes and decodes its own values.
- **A pump driver:** request a turn, arm a timer, and read a monotonic
  clock. napi uses setImmediate and setTimeout. The test host runs a turn
  when a scenario says so.
- **Hooks into its language:**
  - effects: I/O processor sends, invoker starts, cancels and forwards;
  - log records, trace requests, and input-log commits (awaitable);
  - loaders;
  - fault, done and dead-letter reports.
- **Error mapping** from the kernel's results to the language's errors.

The kernel gives the binding the system, actors, turns, mailboxes,
routing, the directory, loads, offloads, the OTLP encoder, parameters
that concern hosting (budgets, mailbox limits, trace sizes; napi ADR
0035), and the write-ahead log (kernel ADR 0002).

### 4. What moves, and what stays in each binding

| Moves to the kernel | Stays in napi |
|---|---|
| `scheduler`, `router`, `directory`, `events`, `enumerate`, `effects`, `system`, `allocator`, `containers`, `hooks`, `adapters/`, `loads`, `offloads`, `otlp`, `inputs` (becoming kernel ADR 0002's log), the host-generic part of `params`, and `pump.c`'s turn logic | `napi_*.c`, `values.c`, `runtime.c`, `runtime_codecs.c`, `addon.c`, `pump.c`'s setImmediate and setTimeout glue, the generated bindings, `lib/` |

The move is mechanical: the code already avoids Node-API. What changes is
its prefix (`tna_` to `tah_`) and where its tests live.

### 5. The test host uses the kernel

The test host keeps its reason for being: exact control of each core call.
Scenarios that test core's semantics go on driving core directly, with the
recording router, directory and instruments they use today.

It gains a **hosted mode** built on the kernel:
- a scenario can create a hosted system, whose actors are queued,
  scheduled and routed by the kernel;
- the scenario runs turns explicitly, since the test host's pump driver is
  "run a turn now";
- it inspects mailboxes, dead letters and the input log's batches.

Scenarios about hosting (mail between actors through mailboxes, loads,
dead letters, the input log, replay, checkpoints) use hosted mode. Its
persistence scenarios move there, because the log is the kernel's. So the
kernel is exercised by napi's suites and by the test host's scenarios,
from two languages, and the test host's scenarios can check the kernel
without a JavaScript runtime.

### 6. Where decisions are recorded

- **napi's ADRs about code that moves** stay in napi as the record of how
  it was decided (among them 0003, 0004, 0005, 0006, 0007, 0008, 0012,
  0014, 0016 to 0018, 0020, 0022 and 0032 to 0036). Each gains a
  Status-line pointer to this ADR.
- **New decisions about the kernel** go in `tinyactors-host/docs/adr/`,
  starting with this one.

## Consequences

- **The layers are enforced by the build.** The kernel cannot include a
  language's API, and core cannot see the kernel.
- **The build graph** becomes core ← kernel ← napi and the test host.
- **napi shrinks to Node-API glue, its data model, and `lib/`.** The test
  host gains a hosted mode.
- **A future binding (CPython, the browser build)** writes what section 3
  lists and nothing else.
- **Costs:**
  - one more project to build and test, and the renaming of about 2,500
    lines of C;
  - no change per actor, per event or per turn: the code is the same, only
    its location changes. The migration benchmarks napi before and after to
    show it.

## Implementation notes

### Step 1: an empty kernel that builds and tests (2026-09-30)

- **Where:**
  - `tinyactors-host/include/tinyactors_host.h`;
  - `src/memory.{c,h}` (the kernel's own copy, set and compare, since it calls
    no C library) and `src/version.c`;
  - `tests/smoke_test.c`;
  - `mise/kernel.rb` (what the tasks share) and `mise/tasks/{build,test,asan,check-recursion,check-freestanding}`;
  - `AGENTS.md` (with `CLAUDE.md` linking to it) and `README.md`.
- **The host interface is declared in outline only:** `tah_pump_driver`
  (request a turn, arm a timer, read a monotonic clock), `tah_hooks`
  (one operation so far, `effects_ready`) and `tah_host`, which bundles them
  with core's `ta_allocator`. Values and the data model are core's `ta_value`
  and `ta_datamodel`, and errors are core's `ta_result`, so the header
  declares no types for them and says so. Step 2 adds the hook operations as
  napi's code moves in.
- **libc-free is enforced twice:**
  - `src/` is compiled with `-ffreestanding -nostdlibinc` on every host, so a
    C library header fails to compile in the ordinary build;
  - `mise run check-freestanding` compiles every source with the release's zig
    for `wasm32-freestanding`, which has no C library. It fails on any
    undefined symbol other than core's API (`ta_*`), the kernel's own
    (`tah_*`), and the memory builtins the compiler emits (`memcpy`, `memset`,
    `memmove`, `memcmp`, `bcmp`; root ADR 0001, decision 16).

  Both were checked by hand to reject an `#include <string.h>` and a call to
  `puts`. The wasm object format now lists `__stack_pointer`, which the wasm
  linker defines, as a symbol an object may leave to the runtime
  (`mise/lib/toolchain/object_format.rb`).
- **`check-recursion`** compiles the kernel as one translation unit that
  includes each source, and reads its call graph. The cycle finder moved
  from core's task into `mise/lib/call_graph.rb`, which core's
  `check-recursion` now uses too.
- **The build:** each source is compiled into `build/objects/` (reused by
  content, `mise/lib/cache.rb`), then archived into
  `build/libtinyactors_host.a`. Tests link against it, and `asan` builds them
  with the sources under AddressSanitizer and UBSan.
- **Wiring:**
  - `tinyactors-host` joins the root `mise.toml`'s `config_roots`;
  - the root `build` builds it after core, and the root `test` runs it after
    core ("host kernel");
  - napi's and the test host's `test` tasks list the kernel's `include/` and
    `src/` in their `#MISE sources`.
- **AGENTS.md's table** says a kernel change reruns the kernel, the test host
  and napi. Section 1 also named vscode, but vscode's `#MISE sources` list
  napi's `lib/`, not its C, so a change to napi's C does not rerun vscode
  either. The table follows what actually reruns.

### Step 2a: the first modules move (2026-09-30)

- **What moved** (commit `435336a`), from `tinyactors-napi/src` to
  `tinyactors-host/src`, renamed `tna_` to `tah_`: `allocator`,
  `containers`, `events`, `effects`, `directory`, `definitions`,
  `host_metrics`, `watch` and `scheduler`, with the `allocator`,
  `containers`, `host_metrics` and `watch` native tests. The identifiers
  renamed in napi are exactly those declared in the moved headers,
  outside comments, so a `tna_` name that still belongs to napi was not
  touched.
- **What did not move, and why (differs from the plan).** The roadmap put
  `system`, `hooks`, `enumerate`, `router` and the adapters in this half.
  All of them include `system.h`, and `system.h` includes `loads`,
  `offloads`, `otlp`, `inputs` and `params`, the modules kept for step 2b.
  The kernel may not include napi's headers, so a module can only move
  with everything its header pulls in. These five move in step 2b, together
  with the modules `system.h` aggregates. Their tests (`scheduler`,
  `mailbox`, `router`, `send`, `quarantine`, `debugging`, `clock`,
  `events`, `directory`, `enumerate`, `system`) stay in napi until then,
  because each includes `system.h`.
- **libc removed:** `memcpy` and `memset` became `tah_mem_copy` and
  `tah_mem_set`. The counting allocator no longer calls `malloc`: it takes a
  **backing `ta_allocator`** in `tah_allocator_init(allocator, backing)`,
  which the binding provides. napi's is `tna_platform_allocator()` in the
  new `src/platform.{c,h}` (malloc and `aligned_alloc`, as before), and the
  kernel's tests use `check_platform_allocator()` from `tests/check.h`.
  This is the first platform interface the moved code needed: memory is the
  binding's, as core ADR 0023 has it for core.
- **How napi links the kernel:** napi compiles the kernel's `src/*.c` into
  the addon with its own compile command, into `build/obj/kernel/`, rather
  than linking `libtinyactors_host.a`. One command per target keeps release
  cross-compiles (zig, `mise/lib/toolchain/`) working without a second
  archive per target. The kernel's own tasks still check it freestanding
  and for wasm32. While napi's remaining code still uses the moved modules'
  structs, napi compiles with the kernel's `src/` on its include path. The
  public header is widened to what napi needs once the move is complete,
  so this is a transitional state recorded here.
- **Kernel tests link core:** the kernel's `build` compiles core's
  amalgamation (`tinyactors-core/include/tinyactors.c`) once with the test
  command and links every test program with it; `asan` compiles it in.
- **wasm objects may leave `__indirect_function_table` undefined** beside
  `__stack_pointer` (`mise/lib/toolchain/object_format.rb`): a call through a
  function pointer (every vtable) refers to it, and the linker defines it
  when it makes the module.
- **Benchmark** (`bench-compare 3a1eaa1`, 100,000 actors, 4 alternating
  runs): Node, nothing outside 5% (untraced ns/event unchanged). Bun:
  ns/event within ±3% on every variant, native B/actor within ±3%,
  allocations per event unchanged at 0.01. Two Bun RSS rows were flagged
  (+9.7% and +8.7% on traced variants); Bun's RSS varies by that much
  between identical runs (napi ADR 0035's notes), and native memory did not
  change, so it is noise.

### Step 2b: the rest of napi's portable C moves (2026-09-30)

- **What moved** (commit `cd14e2b`), renamed `tna_` to `tah_`: `system`,
  `hooks`, `enumerate`, `router`, `loads`, `offloads`, `otlp`, `inputs`,
  `params`, and the four adapters, flattened to `adapter_io`,
  `adapter_invoker`, `adapter_log` and `adapter_trace` because the kernel's
  and napi's builds take `src/*.c` without recursing. Every native test
  napi had moved with them: `clock`, `debugging`, `directory`,
  `enumerate`, `events`, `loads`, `mailbox`, `offloads`, `otlp`,
  `quarantine`, `router`, `scheduler`, `send` and `system`. The kernel now
  has 19 test programs, all passing natively and under ASan and UBSan, and
  it still builds for `wasm32-freestanding` with no C library and no
  recursion (443 functions).
- **The pump did not need splitting after all.** Its turn logic (budgets,
  turn order, quiet detection, deadlines, due sends) was already in
  `system.c` (`tah_system_turn`, `tah_system_end_turn`, `tah_system_quiet`,
  `tah_system_timer_fired`), which moved. What stays in napi's `pump.c` is
  Node-API: setImmediate and setTimeout, the hooks' JavaScript records, and
  flush promises. That is napi's pump driver.
- **`napi_replay.c` stays for step 4.** Its logic is interleaved with
  Node-API (feeds are JavaScript objects, records become JavaScript
  values). Step 4 replaces it with the kernel's replay driver (kernel ADR
  0002), so separating it now would be work thrown away.
- **The platform interface** (`include/tinyactors_host/platform.h`)
  replaces the libc calls the moved code made. It is one adapter the binding
  gives every system, in `tah_system_create_params.platform`:
  - `allocator`: the system record and the counting allocator's backing
    store (it replaces `malloc` and `free`, and step 2a's separate backing
    allocator argument);
  - `monotonic_ns`: `clock_gettime(CLOCK_MONOTONIC)` before; the realtime
    clock, load timeouts and turn durations read it through
    `tah_system_monotonic_ns()`;
  - `wall_ns`: `timespec_get` before, for exemplar and telemetry timestamps
    (`tah_system_wall_ns()`);
  - `random_seed`: `getrandom` or `arc4random_buf` before, to seed core's
    trace-ID generator (`tah_system_random_seed()`).

  The pump driver and the language hooks stay what they were: callbacks on
  the system (`wake`, `arm`, and the per-system callbacks in `system.h`),
  set by the binding after it creates a system. Step 1's outline of them as
  separate adapters (`tah_pump_driver`, `tah_hooks`, `tah_host`) is removed,
  since nothing used it. napi implements the platform in `src/platform.c`;
  the kernel's tests in `tests/check.h`, with a fixed random seed.
- **Other libc replaced by the kernel's own helpers** (`src/memory.h`):
  `memchr` (`tah_mem_find`), `strlen` (`tah_str_length`), and the one
  `snprintf` (the router's text for a session this system cannot name) by
  `tah_format_u64`. `memcmp` was only ever compared with 0, so it became
  `tah_mem_equal`. The router's temporary text comes from the system's
  counting allocator instead of `malloc`.
- **napi sees only the public headers** (commit `aafa919`). The kernel's
  module headers moved to `include/tinyactors_host/`, and
  `tinyactors_host.h` includes them all, with the platform in its own
  header so `system.h` can include it without a cycle. napi includes
  `tinyactors_host.h` and has no path into the kernel's `src/`, which keeps
  only `memory.h`, private. The kernel's own sources include their public
  headers as `tinyactors_host/<name>.h`, which also keeps them from
  colliding with a binding's headers of the same name (the test host has
  its own `allocator.h`). These headers are the kernel's API: a binding
  reads the system's plumbing (the scheduling table, mailboxes, effects,
  loads) directly, as napi did before the move.
- **Parameters split** (commit `aafa919`). The kernel's `params.h` keeps
  what concerns hosting: the turn budgets, mailbox limits, `valueBytes` and
  `traceStagingBytes`. The data-model registry's two limits
  (`registeredDatamodels`, `datamodelNameBytes`) are napi's, since the
  registry is, and went back to napi as `src/napi_params.{c,h}`.
  `tah_system_create_params.hosting` carries the kernel's.
  `parameterInfo()` reports core's, the kernel's and napi's, and labels the
  kernel's as napi's: napi embeds the kernel, so to napi's users they are
  napi's (the `owner` field stays `"core" | "napi"`).
- **napi's native test tasks and its Node-API guard are removed**
  (`test-native`, `test-native-asan`, `NODE_API_SOURCES`, and the unused
  sanitize build mode). No native test was left in napi, and the C left
  outside the Node-API files is napi's own: `platform.c` (libc by design)
  and `import.c` (the codec wrapper for `define()`), which napi's
  JavaScript suites exercise.
- **Copy, fill and compare use the compiler's builtins** (commit
  `b664b40`). The first benchmark of step 2b, with 2 runs, flagged Node's
  traced variants at +6 to +7% ns/event: the byte loops that replaced
  `memcpy` and friends in step 1 had moved into the OTLP encoder's hot
  path. `tah_mem_copy`, `tah_mem_set` and `tah_mem_equal` are now inline
  wrappers around `__builtin_memcpy`, `__builtin_memset` and
  `__builtin_memcmp`, which the compiler inlines when small and otherwise
  turns into calls to the symbols `check-freestanding` already allows. The
  kernel still links no C library: a release resolves them with zig's
  compiler-rt, a browser build with its own.
- **Benchmark** (`bench-compare 3a1eaa1`, 100,000 actors, 4 alternating
  runs, after `b664b40`): Node (a99e44a37e2e), nothing outside 5%
  (untraced 477 → 483 ns/event, every record 1,618 → 1,620). Bun
  (c989bd79a51f), ns/event within 5% on every variant (untraced 407 → 410),
  and one RSS row flagged (every record in 100 charts, +9.5%). Bun's RSS
  varies that much between identical runs (napi ADR 0035's notes), and
  native bytes per actor did not change. Allocations per event stay 0.01
  everywhere.
- **Later: the headers are opaque.** A binding no longer reads the
  plumbing's fields; see "Opaque headers" below.

### Step 7: the test host runs hosted systems on the kernel (2026-09-30)

Done by the coordinator directly, in small commits (`f8fee89`, `f11cce1`,
`5c83fcb`, `a36b727`, `7a54fda`), after several agents stalled on the
larger scope during an API outage.

- **The test host links the kernel.** Its build task runs the kernel's
  build and links `tinyactors-host/build/libtinyactors_host.a` into the host
  binary, with only the kernel's public headers on the include path.
- **`src/hosted.{c,h}` is the test host's binding.**
  - Its platform is libc memory, a clock the scenario owns (it starts at 0
    and moves only with `hosted.advance`) and a fixed random seed, so a
    hosted run is reproducible.
  - Its pump driver is "run a turn when the scenario asks": `wake` does
    nothing, and `hosted.turn` runs turns until the system is quiet.
  - It gives the kernel's input log the jq data model, whose codec streams
    both ways.
- **The hosted methods** are documented in `docs/protocol.md` ("Hosted
  mode"): `hosted.create` / `close` / `spawn` / `post` / `turn` / `advance`
  / `log` / `batches` / `evict` / `restore` / `checkpoint`.
  - `hosted.create` also answers the kernel's core system as an ordinary
    `system` handle, so charts are defined and actors inspected with the
    generated core methods, with no second path for them.
  - `hosted.restore` replays a stored log explicitly (prepare, run,
    publish, redeliver) rather than from a loader, because napi's loader
    orchestration is napi's and this tests the kernel's part.
- **Scenarios** (`ruby/scenarios/hosted.rb`, eight of them) cover:
  - mail between named actors through the kernel's mailboxes and router;
  - a delayed send on the scenario's clock;
  - an evicted actor rebuilt from its log with its timer;
  - a 1 MiB value logged and replayed;
  - a log compacted to its checkpoint;
  - a checkpoint of an unnamed actor refused;
  - a crash: another system, with another incarnation, restores the actor
    from the stored log, and its pending timer fires.
- **A kernel bug found and fixed** (`3cb4e60`): the checkpoint encoder wrote
  a delayed send to the actor itself as the old session's address, so after
  a restore the timeout went nowhere.
- **Scenarios about core's semantics** keep driving core directly, as
  section 5 decided.
- **The scenario runner does not rebuild the test host** when kernel sources
  change. `mise run test` does, through the build step, so a scenario run
  alone after a kernel edit needs `mise run build` first.

### Step 8: verification (2026-09-30)

- **`mise run test:full --all` passed** at `4933310`:
  - core;
  - the kernel: native tests, ASan and UBSan, `check-recursion`, and
    `check-freestanding` for `wasm32-freestanding` with no C library;
  - the test host: 537 of 537 scenarios, the exhaustive ones included;
  - napi: its suites, IRP 176/176 on Node and Bun, and `scale.js` to a
    million actors on Node and Bun, with zero steady-state calls into the
    addon and core.
- **The first full run failed 18 exhaustive scenarios,** the out-of-memory
  and tracing sweeps. Their probe delivery carried no time, which core ADR
  0094 made an argument of every input, so core answered `err_invalid`
  before the admission test. The fast pass does not run those sweeps, so
  it went unnoticed from step 3 until here. Fixed in `4933310`.
- **`bench-compare 3a1eaa1`** (the commit before step 2), 100,000 actors;
  Node on machine 84c362297e3d, Bun on bbe6bf531954:
  - the two-run pass flagged five rows: Node `decoded` heap +8.6% and
    `summary 10%` ns +5.2%, and Bun RSS on three traced variants;
  - rerun with four runs, **nothing is outside the 5% rule on either
    runtime**. Node untraced is 664 → 664 ns/event, Bun untraced 572 → 568;
  - allocations per event are unchanged at 0.01, and native bytes per actor
    move by at most +3% on traced variants;
  - the flags were run-to-run noise, as earlier steps' reruns also found.

### Opaque headers (2026-09-30)

The ROADMAP item that followed step 2b: `include/tinyactors_host/*.h`
declared the full structs, and napi read or wrote 32 of `tah_system`'s 43
fields and the test host 4. Done in six commits (`9782371`, `bc429db`,
`89280ee`, `cd77435`, `de0b1a1`, and the one that moved the structs).

- **The rule for what stays transparent.** The kernel's state is opaque:
  the public header keeps `typedef struct tah_x tah_x;` and the body is in
  `src/<module>_private.h`, which the kernel's sources and tests include
  and a binding never does. A struct body stays public only when it is
  data the binding fills or reads:
  - parameter records (`tah_system_create_params`, `tah_message`,
    `tah_load_finish`, `tah_otlp_config`, ...);
  - records and views handed out (`tah_effect`, `tah_turn_record`,
    `tah_settlement`, `tah_input_entry`, `tah_input_wake`,
    `tah_queued_message`, `tah_load_info`, `tah_load_request`, the
    write-ahead log's records);
  - snapshots of counters (`tah_sched_stats`, `tah_loads_stats`,
    `tah_definition_totals`, `tah_trace_watch_sizes`, `tah_histogram`,
    `tah_exemplar`, `tah_memory_counters`);
  - tables of callbacks (`tah_platform`, `tah_pump_driver`,
    `tah_system_callbacks`);
  - the containers a binding embeds in its own records (`tah_vec`,
    `tah_free_list`, and the rest of `containers.h`), and `tah_params`,
    which napi fills at creation.

  The root's `mise/ast-grep/rules/kernel-opaque-struct.yml` names them, and
  `mise run check-opaque` (in the kernel's test task) refuses any other
  struct body in `include/`. Its test runs with `check-portability`'s.
- **The accessors: 50 functions.**
  - The input log's batch (4): `tah_inputs_enabled`, `_touched_count`,
    `_entry_count`, `_entry_at`, an entry being a view of the log's
    buffer (`tah_input_buffer`, now private).
  - The scheduler (9): a stats snapshot, an actor's state, the turn's
    records, a mailbox walk as `tah_queued_message`, and a waiter's host
    (set and read), generation and the next taken waiter.
  - Loads, offloads, OTLP, definitions, metrics and the watch (11), two of
    them inline functions over fields before
    (`tah_loads_pending_requests`, `tah_otlp_set_wanted`).
  - The system (22): a getter for each part (core, allocator, scratch,
    scheduler, directory, loads, offloads, effects, OTLP, trace watch,
    definitions, metrics, input log, parameters), the two scratch arenas,
    `tah_system_enter_core` / `_leave_core` / `_inside_core`, and the
    tracing and log-sink flags.
  - Callbacks and the clock (4): `tah_system_set_callbacks`,
    `tah_system_set_driver`, `tah_system_turn_pending`,
    `tah_system_clock_kind`.
- **Callbacks are set, not created with.** The plan put them in the create
  parameters. napi's `after_core` may run only once its values exist,
  which is after the system is created, and a failed setup frees the
  system before that; its pump driver must be detached before a close
  that runs where JavaScript cannot (a finalizer). So the callbacks are
  one `tah_system_callbacks` table with one context, set when napi is
  ready, and none is called after close returns; the pump driver is its
  own table, detached with `tah_system_set_driver(system, NULL)`. This
  brings back step 1's separate pump driver, now as data rather than an
  adapter.
- **Wake and arm answer whether they worked.** napi's wake and arm wrote
  `turn_pending` and `clock.armed` back when setImmediate or setTimeout
  failed. `tah_wake_fn` and `tah_arm_fn` now return false instead, and the
  kernel forgets the request itself.
- **Removed:** the system's `trace_masks` (napi wrote it, nothing read
  it), and the inline functions over fields that only the kernel uses
  (`tah_loads_next_wake`, `tah_otlp_open_count`, `tah_trace_watch_empty`),
  which moved to the private headers.
- **Hot paths.** napi's generated entry points now call functions for
  what were field reads (the reentrancy flag, core, the call arena): three
  to five calls per core call. None of them showed:
  - `bench-compare 3a2b957` (the commit before this work), 100,000 actors,
    three alternating runs. Node (a99e44a37e2e): nothing outside 5%;
    untraced 487 → 483 ns/event, RSS 760 → 761 B/actor. Bun
    (c989bd79a51f): ns/event within 5% on every variant (untraced 406 →
    407), native and core bytes per actor unchanged, and four RSS rows
    flagged on traced variants (+5.5% to +9.8%).
  - Rerun on Bun with five runs: one row left (summary, RSS +6.3%), the
    others +4.5% or less, and `every record in 100 charts` at -0.1%.
  - An A/A run, the baseline against itself on the same five variants,
    showed the same bias: +3.0% to +4.6% RSS for the working tree's side
    on `sampled out`, `summary` and `summary without logs`, with nothing
    changed. What is left, 1% to 2%, is within Bun's run-to-run RSS
    noise (napi ADR 0035's notes). Allocations per event stay 0.01.
- **`mise run test:full` passed** before the last commit: the kernel's
  tests with ASan and UBSan, 537 of 537 test host scenarios with the
  exhaustive ones, `scale_test` to four million actors, and napi's suites
  with `scale.js` to a million actors.

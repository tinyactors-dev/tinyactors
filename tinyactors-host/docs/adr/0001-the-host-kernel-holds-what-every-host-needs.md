# ADR 0001: The host kernel holds what every host needs

**Status:** Proposed (2026-09-30). Turns `docs/proposals/host-kernel.md` into a decision, earlier than it planned, with the test host as the second consumer. Core ADR 0094 and kernel ADR 0002 depend on it.
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

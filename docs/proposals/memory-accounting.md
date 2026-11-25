# Memory accounting: core's costs from its definitions, the rest from counted allocations

**Status:** Accepted (2026-11-25): the maintainer decided it (section 7);
steps 1 to 6 of section 9 are done (section 10 records where the code
differs from this text); step 7, the published sweep, is not.
**Touches:** tinyactors-core (`tools/layout`, `mise/tasks/layout`, the
fast pass in `mise/tasks/test`, `src/host/host.c` at creation,
`include/tinyactors/stats.h`, `docs/requests.md` `host.stats`,
`src/otlp/otlp.c`, `src/datamodel_ecmascript`), both bindings
(`examples/memory`, the allocator they give their own tables, the
retired `measure` tasks and the per-session tables in their
`AGENTS.md`), the published tables (root `AGENTS.md`, "Published
numbers").

## Summary

The maintainer (2026-11-25): "our memory measurement approach is flawed,
and we should base the core metrics on the actual data structure
definitions and tracing allocator calls", and, deciding it: **"I just
need to be able to answer these questions based on the actual source
code and then runtime profiles through tracing allocators"**: how much
space an actor, a message, an event, a delayed send, a hibernated actor
and the rest take.

So every number this project publishes about its own memory comes from
one of two sources:

| Part | Owner | Source | Exact? |
|---|---|---|---|
| what the host reserves for N things | core region | the definitions, through `ta_system_layout` (`carve()` over `sizeof`), checked against the debug information in the fast pass | yes, at a given N |
| what the things use of it | core region | core's pool and slab counters, already traced: each pool's `used` and `highWater`, `slabBytes` (`host.stats` `pools`) | yes |
| everything the host allocates | core host | a counter around the host's allocator, wrapped once at creation; `host.stats` and OTLP report it | yes |
| the binding's tables, arenas and datamodel writers | binding | a counter around the allocator the binding gives them | yes |
| quickjs's heap | core's ECMAScript datamodel | its counting allocator, which will also count blocks | yes, once it counts blocks |
| the process | — | `RssAnon` + `VmSwap` on Linux | no: one informational column, never asserted |

What a chart's data costs in the application's own engine (Python
objects on the `"python"` datamodel, V8's and JavaScriptCore's heaps on
the native `"ecmascript"` one) is **not** measured: it is the chart's
data in the application's engine, not a cost of tinyactors, and no
interface of ours sees it. It is the main part of the gap between the
process column and the sum of the rest, and the tables say so.

Two findings shape the design:

- **The host's allocator calls give capacity, not a cost per thing.** A
  host sized up front allocates nothing while 100,000 actors are spawned
  (spikes 2 and R1). Core's cost per thing comes from the definitions
  (reservation) and from its own pool and slab counters (use), which are
  core's traced allocators inside its region.
- **The reservation is what a machine holds.** A host sized for a
  million actors is resident at creation: `RssAnon` grew 196,435,968 B
  for a `regionBytes` of 196,425,953 B, and 4,096 B over the million
  spawns (spike R2). Planning uses the reservation; use is a check and an
  operator's reading.

## 1. Whom it serves, who pays, what is out of scope

**First the capacity planner** (docs/roles.md: "How much memory per
actor? ... Needs: documented costs per actor and per event"). Today the
published "per actor, started" on node native is 1327 B at 10,000 actors
and 501 B at 4,000,000 (`tinyactors-napi/examples/memory/results/2026-11-20.csv`),
and on CPython 341 B at 10,000 and 408 B at 1,000,000
(`tinyactors-cpython/README.md`, "Memory"); no column explains either.
Afterwards the planner reads, per thing, what core reserves and uses and
what the binding adds, a fixed cost per host, and, separately, what the
process held in one run.

**Then the maintainers** ("Did this change cost memory?"): a struct that
grows changes `ta_system_layout`'s deltas, which the fast pass checks
against the debug information on the commit that grows it. Today nothing
does: `mise run layout` is not in `mise run test`
(`tinyactors-core/mise/tasks/test/_default:16-27`; `ta-layout` is in
`Core::TOOLS`, `tinyactors-core/mise/core.rb:332`).

**Then the operator and the observability backend** ("Is it growing?"):
the pools' use is in `host.stats` (`docs/requests.md:191-200`); the bytes
the host holds through its allocator join it, and join the OTLP metrics,
which carry only the region today (`src/otlp/otlp.c:2279`): one gauge
per host, bounded cardinality.

**Who pays:**

| Who | What | How much |
|---|---|---|
| every host, per allocation | the counted wrapper: one indirect call, three integer operations | nothing per event: the allocator is called at creation, growth, close and once per `host.stats` or `metrics` call (`src/host/request.c:550`, `telemetry.c:215`); 0 calls in 100,000 spawns of a sized host (spikes 2, R1) |
| core's API | three `uint64_t` in `ta_host_stats` (`stats.h:80-88`), three keys in `host.stats`, one OTLP gauge, a block count beside quickjs's heap bytes | minor-version changes; `ta_host_stats` has a `size` field |
| core's fast pass | the layout check: the layers built once more with debug information, `llvm-dwarfdump` read, `ta-layout` run | one more build of the layers per fast pass (decision D6) |
| each binding | a counted allocator for its own blocks, no public API; the memory example rewritten; `measure` retired | about 20 lines of C each, and less code overall |
| the C embedder | nothing unless it reads the new fields | — |

**Out of scope:** the chart's data in the application's engine (above);
glibc's per-block overhead and fragmentation; Node-API's own mallocs (a
payload's `napi_ref`, about 96 B on node: `tinyactors-napi/docs/allocation.md`,
finding 3); the wasm package's linear memory; the compiler's transient
memory.

## 2. What exists, and what was wrong with it

Each item checked on 2026-11-25.

**Core: `mise run layout`.** `tinyactors-core/mise/tasks/layout` builds
the layers with debug information (`:31-34`), reads struct sizes and
macros with the pinned `llvm-dwarfdump`, computes the cost of one more
of each thing as `carve()` lays it out (`COSTS`, `:61-74`), runs
`ta-layout` (`tools/layout/layout.c`), which asks `ta_system_layout`
(`include/tinyactors/system.h:132`) for a segment at 2^16 and 2^17 of
each capacity (`layout.c:58-67`), and fails on a difference
(`layout:156`). Run today:

```
actor            81 B  record 48, links 16, session index 8, name index 8, generation 1
                       up to 89 B: the two indexes round up to a power of two
                       in memory it also holds its chart's instance, and its name if it has one
                       in the string slab (the default config reserves 16 B of strings per actor)
message         106 B  event 56, trace context 48, generations 2  (57 B with tracing off)
delayed send    151 B  timer 24, heap entry 20, generation 1, its message 106  (102 B off)
request 41 B, outbox entry 256 B, trace rule 72 B, chart of the image 114 B,
slab page 4112 B, segment once 203704 B
Checked against ta_system_layout (tools/layout/layout.c): actor 81 B, event 106 B,
untraced-event 57 B, timer 45 B, request 41 B, outbox 256 B.
```

What was wrong: it is not in the fast pass; it has no hibernated-actor
row; it says nothing of the instance slab, which a sized host reserves
for the image's largest instance class for every actor
(`src/system/system.c:65-77`, so a cost per image and per N, not per
chart); and its numbers are copied by hand into both bindings' tests
(`tinyactors-cpython/examples/memory/test_memory.py:37-43`,
`tinyactors-napi/examples/memory/test.mjs:12-18`) and prose (the
capacity guide, the Python README, `website/content/guides/capacity.xml:131`).

**Core's own counters, already traced.** `ta_system_stats`
(`include/tinyactors/stats.h:20-56`) counts each pool's `used`, capacity
and high water, and the slab's pages; `host.stats` reports them per
segment (`docs/requests.md:191-200`). 100,000 spawns of a one-state
chart used 2,381 slab pages = ⌈100,000 / 42⌉; hibernating 50,000 of them
brought it to 1,191 (spike R1): a hibernated actor gives back its
instance and keeps its records.

**The bindings' memory examples** (`tinyactors-cpython/examples/memory/main.py`,
`tinyactors-napi/examples/memory/main.mjs`):

| Column | How | What was wrong |
|---|---|---|
| memory | CPython `RssAnon + VmSwap` (`main.py:61-73`, since `f34a0630`); node and bun `process.memoryUsage().rss` (`main.mjs:128, 136`) | different in each binding; it flaked (`f34a0630`); tests assert `resident > 0` (`test_memory.py:46-47`, `test.mjs:19-20`) |
| heap | CPython `tracemalloc` (`main.py:143-145`); node `heapUsed + external`, bun `heapSize + extraMemorySize` (`main.mjs:53-63`); quickjs `host.quickjsHeapBytes` (`main.mjs:131`) | mixes owners: `tracemalloc` also sees the binding's tables and the region (spike 4); node 22's `external` leaves out the addon's `napi_adjust_external_memory` reports and bun's includes them (spikes 1, R3); CPython has no quickjs reading |
| core reserves | `regionBytes` of a host sized for N less one sized for 0, over N (`main.py:86-92, 118-119`; `main.mjs:66-72, 114`) | sound and deterministic, but the started actor's 199, 195, 196 B (10,000, 100,000, 1,000,000) are 81 B of records, about 97.5 B of instance slab and 16 B of strings, and no column says so; tests assert `> 81` |
| blob | the store's bytes over the saves | sound |

**The `measure` tools** (`mise run measure` in both bindings) define
"region" a fourth way, `regionBytes` over N (`tinyactors-napi/tools/measure.mjs:23-25`);
their tables are the per-session costs in both `AGENTS.md`.

**The allocators.** The host allocates through `config.allocator` from
several sites, not only `host_alloc`/`host_free` (`src/host/host.c:29-35`):
the host struct (`host.c:426, 447, 628`), the outputs arena of blobs,
traces, logs and metrics (`host.c:436`), the blob scratch (`host.c:72`),
the session maps of moves and migrations (`host.c:137, 579, 606, 1619`).
50,000 hibernations made one 16 KB allocation of the outputs arena that
`host_alloc` never saw (spike R1). `ta_allocator.free` is given the size
(`include/tinyactors/allocator.h:12, 27`), so a counter needs no header.
quickjs counts requested bytes (`datamodel_ecmascript.c:3551-3602`), not
its 16-byte header per block. The bindings allocate their tables,
arenas and the datamodels' kept writers through one `ta_allocator` per
host (`tinyactors-napi/src/host.c:839-852, 60`, `src/datamodel/datamodel.c:126`,
`rows.c:104`; `tinyactors-cpython/src/host.c:616, 679-685`,
`src/datamodel/datamodel.c:731-742`).

## 3. The frame

- **One owning thread per host** (`tinyactors-core/docs/bindings.md`,
  section 5): counters are plain integers on that thread.
- **The host allocates only through `config.allocator`**; base through
  system never allocate.
- **A binding allocates only through the allocator it keeps per host**,
  enforced by the lint rule `allocation-in-tables-and-host`.
- **Allocations are not per event:** creation, `add_image`, growth of a
  pool or table, close, one per `host.stats` or `metrics` call
  (allocation.md, finding 1; spike R1).

## 4. The design

### 4.1 Core's costs: reservation from the definitions, use from the pools

**The definitions in the fast pass.** Core's `mise run test` runs the
layout check (`mise run layout`'s DWARF computation and its comparison
with `ta-layout`), so a struct that grows is explained and checked on
the commit that grows it. No checked-in copy of the numbers. The task
gains:

- a hibernated-actor line: its records, the instance given back;
- `--json` on stdout of what it already writes to
  `build/debuginfo/layout.json`, for other programs.

**`ta-layout` takes a configuration and an image.** Today it lays out its
own one-state chart at 2^16 and 2^17. It gains a mode that reads an image
file and the capacities of a measurement (`maxActors`, `maxEvents`,
`maxTimers`, `instanceBytes`, `stringBytes`, `tracing`) and prints
`ta_system_layout`'s region at N and at 0 per thing, the same difference
the examples take through two hosts. Its sources use only public calls
that the bindings' amalgamation holds (`ta_compile_charts` in
`tinyactors_runtime.c`, `ta_system_layout` in `tinyactors.c`; checked in
`tinyactors-napi/build/core`), so a binding builds it beside its
extension from core's `tools/layout/layout.c` (that it compiles cleanly
there is unverified), and runs no other core task.

**Use, from core's counters.** For any chart and N, with no new API: an
actor's records are the actors pool's `used` × the actor unit; its
instance and strings, the `slabBytes.used` delta over N; a waiting
message, the events pool's `used` × the event unit; a hibernated actor,
its records, and the fall of `slabBytes.used` when it hibernates.

### 4.2 Counted allocations

**In the host.** `ta_host_create` and `ta_host_restore` wrap
`config.allocator` once, in a counted allocator kept in the host, so
every call site in section 2 counts untouched; the host struct, allocated
before the wrapper exists, is added as `sizeof *host`. `ta_host_stats`
and `host.stats`'s `host` map gain `allocatedBytes`, `allocatedPeak` and
`allocations`; `regionBytes` beside them gives the host's blocks outside
the regions, and a relocation shows in the peak (spike R1: 44.5 MB peak,
22.5 MB live). The OTLP metrics gain `allocatedBytes` as a gauge beside
the region (`src/otlp/otlp.c:2279`). The counter type is internal.

`allocations` moves at every `host.stats` and `metrics` call, which
allocate and free, so "no allocation while spawning" is tested in C.
Always on: it adds no path and changes no outcome; a reading, like
`regionBytes`, not a test hook (`tinyactors-napi/AGENTS.md`, "Refusals
no input reaches", forbids an allocator that fails on demand).

**quickjs counts blocks.** A request to core:
`ta_datamodel_ecmascript_heap_bytes` (or a function beside it) also
reports the blocks, so the 16-byte header per block is in the account.
At a million started actors on node the process held 1041 B per actor
against 598 B of quickjs heap and 196 B of core reservation (the CSV);
how much of the 247 B the headers are is what this settles (unverified
until then).

**In each binding: its own blocks, no public API.** The binding gives
its tables, arenas and datamodel writers a counted allocator separate
from the one it gives the host. The writers keep the largest value ever
written, so the counter, not the tables' capacities, is the measurement.
It serves the binding's tests and the memory examples only. How the
examples, which import the public package, read it is open question O1.

### 4.3 Process memory

One informational column, never asserted, measured the same way in both
bindings: `RssAnon + VmSwap` from `/proc/self/status` on Linux (node
reads the file too), the peak elsewhere, as `f34a0630` does. The tables
show it beside the accounted sum and state what the gap is: **mostly the
application's engine holding the chart's data** (Python objects, V8's or
JavaScriptCore's heap), then malloc's overhead per block and the fixed
costs of a host and an engine spread over the population. The capacity
guide keeps the process row by population, since a machine must hold it.

### 4.4 Tables, programs and tests

**The memory examples are the one program** per binding that makes
these numbers (`mise run measure`, `tools/measure.*` and the per-session
tables in both `AGENTS.md` are retired). Each measurement sizes every
pool it fills for N, so no relocation runs during it (spike R1). Per row
(actor started, after an event, hibernated, message waiting, delayed
send pending) and N:

| core reserved | core used | host other | binding | quickjs | accounted | process | blob |
|---|---|---|---|---|---|---|---|

- *core reserved:* the `regionBytes` difference (planning);
- *core used:* from `host.stats` `pools` (4.1);
- *host other:* the delta of `allocatedBytes - regionBytes` over N;
- *binding:* its counter's delta over N (O1);
- *quickjs:* heap bytes and block headers over N, on quickjs only;
- *accounted:* the sum of the columns before it but "core used", which
  is a part of "core reserved";
- *process:* 4.3, with the gap's note.

**Tests:**

| Assertion | Where | Kind |
|---|---|---|
| the types' debug information gives `ta_system_layout`'s per-unit deltas | core fast pass, the layout check | equality |
| `allocatedBytes` equals an outside counting allocator's live bytes over a workload with hibernations, traces and a migration, and both are 0 after `ta_host_free` | core, `tests/unit/test_host_memory.c` | equality |
| a host with every pool sized makes no allocation while it spawns and runs N actors, read in C | core, the same test | equality |
| a hibernated actor gives back its instance: the slab's used pages fall by its pages | core test | equality |
| the OTLP metrics carry `allocatedBytes` | core, the metrics tests | equality |
| quickjs's block count returns to its start when its sessions go | core, `test_datamodel_ecmascript.c` | equality |
| every row's core-reserved column equals `ta-layout`'s for the example's image and capacities | both examples | equality, no copied numbers |
| the core-used column equals the pools' `used` × units plus the slab delta | both examples | equality |
| the binding's counter is 0 after close | both bindings' tests | equality |
| process | — | never asserted |

**The published tables** (root `AGENTS.md`, "Published numbers"): the
core row is `mise run layout`'s, checked in the fast pass; the memory
rows (both examples' READMEs, the Node capacity guide's "What an actor
costs", the Python README's "Memory", `website/content/guides/capacity.xml`)
show the columns above with process memory beside them and the gap's
note; the "Published numbers" table drops `measure`'s tables and says
what reruns them.

## 5. Not chosen

- **Per-thing core numbers from the allocator's calls**: a sized host
  makes none per thing (spikes 2, R1); a grown one reports its growth
  policy (255 B per actor grown from the default size, spike 2).
- **Counting in `host_alloc`/`host_free`**: misses the host struct, the
  outputs arena, the blob scratch and the session maps (spike R1).
- **A checked-in copy of the layout numbers** (`costs.txt`): only a
  change detector, and a copy; the layout check in the fast pass
  explains a change too, and the examples ask `ta-layout` at their own N.
- **Per-chart rows in core's table**: the reservation is per image and
  per N (`system.c:65-77`); use per chart comes from `slabBytes.used`.
- **`tracemalloc` and the engines' heaps**: the chart's data in the
  application's engine, which no interface of ours sees (the
  maintainer, 2026-11-25).
- **A public property for the binding's bytes** (for now): it would be
  an API item with no core behind it, implemented in node, Python and
  the wasm package (`tinyactors-napi/wasm/lib/host.mjs`,
  `tools/parity.mjs`).
- **A test-only build of the counter**: it costs nothing per event, and
  the operator would lose the numbers.
- **A header per block**: `free` is given the size.
- **glibc's `mallinfo2`**: per process, glibc only, outside the bindings'
  allowed C library calls.
- **Asserting process memory in a tolerance**: its variation is the
  machine's and the engine's, not the change's.

## 6. Spikes (2026-11-25, x86_64 Linux 6.18, 4 CPUs; node 22, bun 1.4.1, CPython 3.13)

Spikes 1, 2 and 4 are the author's; R1 to R3 the refuting agent's.

1. **`external` and the addon's reports.** A host sized for 200,000
   actors (`regionBytes` 39,619,089): node's `external` rose 308,021 B at
   creation (the region not in it); bun's rose 39,742,247 B (in it).
2. **A counting allocator over a host** (100,000 spawns, null
   datamodel): sized, +0 B and 0 allocations during the spawns, 5
   allocations in the host's life; from the default size, 255.38 B per
   actor in 7 allocations, peak 39,694,552 B.
4. **`tracemalloc` sees the region:** +19,792,954 B traced for a host
   whose `regionBytes` is 19,740,721 (why the old heap column mixed
   owners).
- **R1.** A host sized for 100,000 actors, events pool 4,096: create and
  image 5 calls, 22,064,200 B live; 100,000 spawns: 0 calls, slab pages
  2,381 of 3,071; 100,000 events sent 1,000 at a time: one relocation,
  peak 44,529,392 B; 50,000 hibernated: one call (the outputs arena,
  16 KB, outside `host_alloc`), slab pages 1,191; after free, live 0,
  7 calls and 7 frees.
- **R2.** A host sized for 1,000,000 actors: `regionBytes` 196,425,953,
  `RssAnon` +196,435,968 B at creation, +4,096 B over 1,000,000 spawns.
- **R3.** Bun's `external` includes `napi_adjust_external_memory`;
  node 22's does not.

## 7. Decisions (the maintainer, 2026-11-25)

- **Engine heaps:** no `tracemalloc`, no Python-object, V8 or
  JavaScriptCore column, in the design, the tables or the tests. Process
  memory stays one informational column with the gap's note.
- **D1:** core counts: the host wraps `config.allocator` once at
  creation, plus `sizeof *host`; `host.stats` gains `allocatedBytes`,
  `allocatedPeak`, `allocations`. **D1b:** `allocatedBytes` is exported
  over OTLP too.
- **D2:** `mise run measure` and the per-session cost tables in both
  bindings' `AGENTS.md` are retired; the memory examples make these
  numbers.
- **D3:** no public binding-bytes property for now; the binding's
  counters serve tests and examples only.
- **D4:** core's quickjs counter counts blocks.
- **D6:** no `costs.txt`; the layout check runs in the fast pass, and
  the bindings take their reference from `ta-layout`, not from copied
  numbers.

**Open questions** (for the implementation to bring back, not to decide
alone):

- **O1. How the memory examples read the binding's counter without public
  API.** The examples import the public package (`@tinyactors/node`,
  `tinyactors`), and napi's convenience layer takes the direct layer's
  helpers off its prototype so an application sees none
  (`tinyactors-napi/lib/convenience.mjs:32-34`). Options: (a) an
  undocumented accessor the convenience layer leaves in place, not in
  `docs/api-map.md` or `index.d.ts` (in practice public, and the parity
  check would see it); (b) the examples report the binding's bytes only
  in the bindings' own tests, which may reach the direct layer, and the
  published table leaves the column out; (c) the binding's bytes from
  its row definitions, unchecked, which misses the writers. (b) is the
  one that adds no API.
  **Decided (the maintainer, 2026-11-25): (b).** The binding's own byte
  count appears only in the bindings' own tests, not in the published
  table, and no public API is added for it.
- **O2. A binding builds `ta-layout`.** Each binding's `AGENTS.md` says
  its build compiles its extension against core's amalgamation and runs
  no core task but `amalgamate`. Compiling core's `tools/layout/layout.c`
  against the same amalgamation keeps that rule, but it is a second
  program in the binding's build; the rule's wording should say so.

## 8. Refutation

A fresh agent, given the code paths and the first version of this
proposal, tried to refute each claim with the repository and spikes of
its own (R1 to R3). What it found, and what changed:

| # | Its finding | Verdict | What changed |
|---|---|---|---|
| 1 | Not every host byte goes through `host_alloc`/`host_free`: the host struct, the outputs arena, the blob scratch and the session maps use `config.allocator` directly; 50,000 hibernations made a 16 KB allocation `host_alloc` never saw | confirmed (`host.c:72, 137, 426, 436, 447, 579, 606, 628, 1619`; R1) | the counter wraps the allocator once at creation, plus `sizeof *host`; no public base counter |
| 2 | Core's internal allocators (pools, slab) are already traced in `ta_system_stats` and `host.stats` | confirmed (`stats.h:20-56`, `requests.md:191-200`; R1) | "core used" comes from them for any chart, with no new API; it settles that a hibernated actor gives back its instance |
| 3 | Per-chart rows in a cost file are wrong: the reservation is the image's largest instance class for every actor; the file is only a change detector | confirmed (`system.c:65-77`) | per-chart rows dropped; the maintainer then dropped the file (D6) |
| 4 | Equality for a started actor's core column against fixed numbers fails: 199, 195, 196 B, since rounding depends on N | confirmed (napi CSV) | the examples ask `ta-layout` at their own N and image, so equality holds |
| 5 | Binding bytes are not `capacity × stride + arenas`: the datamodels' kept writers use the same allocator | confirmed (napi `src/datamodel/datamodel.c:126`, `rows.c:104`; cpython `src/datamodel/datamodel.c:731-742`) | the counter is the measurement; tests assert it returns to 0 |
| 6 | `host.stats` and `metrics` allocate per call; an unsized events pool relocates during a measurement | confirmed (`request.c:550`, `telemetry.c:215`; R1) | the no-allocation test reads the counter in C; the examples size every pool they fill |
| 7 | Holds: a sized host makes no allocation while spawning; the reservation is resident (R2); 196 B = 81 + about 97.5 + 16; node's `external` leaves out the addon's reports, bun's includes them; the counter is not a test hook | confirmed | planning uses the reservation; spike 1's bun reading corrected |
| 8 | OTLP exports `region_bytes` only; the wasm package is a third implementation of any binding property | confirmed (`otlp.c:2279`; `wasm/lib/host.mjs`, `tools/parity.mjs`) | D1b; D3 decided against a property for now |

## 9. Order of work

Each step is one commit, small and passing on its own, on `main` (root
`AGENTS.md`, "Version control"). Before each commit: `mise run test` in
each project the root table says the change reruns, and `mise run
check-acronyms` at the root.

1. **`test(core): the layout check runs in the fast pass`**: core's
   `test` runs `mise run layout`'s check; the task gains the
   hibernated-actor line and `--json`; `ta-layout` gains the image and
   capacities mode. Runs: core, napi, cpython.
2. **`feat(core): host.stats counts the bytes the host allocates`**: the
   counted wrapper at `ta_host_create` and `ta_host_restore`,
   `ta_host_stats` fields, `host.stats` keys, `docs/requests.md`, the
   generated API description; tests in `test_host_memory.c` (the outside
   counter, zero allocations in a sized host, the slab falling on
   hibernation). Runs: core, napi, cpython.
3. **`feat(core): the host's metrics export the bytes it allocates`**:
   the OTLP gauge beside the region, its test and its row in the metrics
   documentation. Runs: core, napi, cpython.
4. **`feat(core): quickjs counts its blocks`** (D4), with its test.
   Runs: core, napi, cpython.
5. **`feat(napi): the memory example accounts each part`**: the counted
   allocator for the binding's own blocks, `ta-layout` built beside the
   addon (O2), `examples/memory` rewritten (sized pools, the columns,
   `RssAnon + VmSwap`), its test by the equalities of 4.4; `mise run
   measure`, `tools/measure.mjs` and the per-session tables of
   `AGENTS.md` removed, and its "Tasks" and "Layout" entries. O1 decided
   before this step. Runs: napi.
6. **`feat(cpython): the memory example accounts each part`**: the same
   in the Python binding, `tools/measure.py` and its task removed. Runs:
   cpython, napi.
7. **`docs: memory tables from the accounted examples`**: the sweep of
   root `AGENTS.md` "Published numbers" ("How", steps 1 to 4) on one
   machine, new CSVs beside the old ones, every table replaced whole
   (both examples' READMEs, the Node capacity guide, the Python README,
   `website/content/guides/capacity.xml`), and the "Published numbers"
   table's rows updated (no `measure`). Runs: website (if its content
   changed), and the acronym check; the commit message names the
   programs and the machine.

Steps 2 and 4 change per-host costs, not the region's layout or
per-actor costs, so the fast pass suffices; step 7 is the release-time
rerun the root rules ask for.

## 10. As built

Where the code differs from the text above, the smallest sound choice,
recorded here.

- **The layout check (step 1).** Core's `mise run test` runs `mise run
  layout`, which gained a `hibernated` line (an actor's records; its
  instance goes back to the slab, and a name it was spawned with stays in
  the string slab) and `--json` (the
  document of `build/debuginfo/layout.json` on stdout, nothing else).
  `ta-layout FILE [NAME=VALUE ...]` is the other mode: FILE a compiled
  image, or an `.scxml` document it compiles; the capacities named as the
  host config's `system` map names them (`maxActors`, `maxEvents`,
  `maxTimers`, `workspaces`, `maxRequests`, `outbox`, `instanceBytes`,
  `stringBytes`, `traceBytes`, `traceContexts`, `traceRules`,
  `tracing=true|false`); it prints one line, `region <bytes>`, the
  region of a segment with that config, the others at their defaults.
  One config per run, not "at N and at 0 per thing": the examples size
  more than one pool per row (an actor's row also sizes `maxEvents`), so
  the program measuring runs it at N and at 0 and divides, as it does
  with two hosts. The task checks this mode too: an actor's cost from
  the regions of a file's chart at 2^16 and 2^17 actors. **O2:**
  `tools/layout/layout.c` includes core's headers, or the
  amalgamation's `tinyactors.h` and `tinyactors_runtime.h` when they are
  on the include path (`__has_include`), and calls only
  `ta_compile_charts`, `ta_compile_image`, `ta_compile_diagnostics`,
  `ta_compile_free`, `ta_image_open` and `ta_system_layout`. Compiled
  with the pinned clang against `mise run //tinyactors-core:amalgamate`'s
  output, `-Wall -Wextra -Wconversion -Werror`, with
  `tinyactors.c` and `tinyactors_runtime.c`, it builds and gives the same
  numbers. A binding builds it so beside its extension
  (`Toolchain::Target.host`, the C library allowed: a program, not the
  extension) and runs it with an image file it writes.
- **`host.stats` (step 2).** `allocatedBytes`, `allocatedPeak` and
  `allocations` are the host's own: `allocations` counts the blocks the
  allocator gave (refusals not counted), the host's own block included;
  the peak starts at `sizeof *host`. The counting allocator is
  `config.allocator` itself, set in `host_new` (which both
  `ta_host_create` and `ta_host_restore` call) over the environment's,
  kept in the host as `given`. The tests read `host_stats_get` from
  `host/host_internal.h`, since `host.stats` allocates. The protocol
  vectors of `host.stats` (tinyactors-test) carry the three keys as
  wildcards. The run protocol's JSON Schema
  (`tools/tinyactors/run.schema.json`, `host.stats.result`, which has
  `additionalProperties: false`) was left to the session building
  `tinyactors run`: it still lacks the three keys. No test checks a
  result against it (the server checks params only).
- **The OTLP metric (step 3).** `allocatedBytes` is exported as a gauge
  of its own, `tinyactors.memory.allocated` (unit `By`), one point per
  host, not as a point of `tinyactors.memory.usage` beside the region:
  the allocated bytes hold the regions, so a sum over that metric's
  points would count them twice. The encoder reads it only when the
  caller's `ta_host_stats.size` holds the field.
- **quickjs's blocks (step 4).** A function beside the bytes,
  `ta_datamodel_ecmascript_heap_blocks`, returns the count; the header
  of each block is two `size_t` (16 B on 64-bit targets, 8 B on wasm32),
  so the engine holds `heap_bytes + blocks × 2 × sizeof(size_t)` of the C
  library's allocator. `heap_bytes` is unchanged, so no binding changes
  with this step. The test: after a warm-up, 2,000 systems each spawning
  and freeing a session leave the block count where it was.
- **The Node binding (step 5).** The binding's counter is
  `ta_napi_counted` in the host (`src/addon.h`, `src/module.c`): the
  host's `allocator`, which its tables, arenas and the native datamodel's
  writer use, counts over the environment's allocator, which core's host
  and the host's own block use uncounted. It is read by the addon's
  `bindingMemory(host)`, which `lib/direct.mjs` does not re-export (O1:
  no public API); `test/memory.test.mjs` loads the addon to read it (0
  bytes after close, and `close()` allocates nothing), and the wasm
  package skips that file. `mise run build` links `build/ta-layout` from
  core's `tools/layout/layout.c` and the core objects the addon links,
  quickjs's left out (it needs `libm`, and layout.c calls nothing of it).
  The example (`examples/memory/main.mjs`) does not run `ta-layout`: an
  application runs it from an installed package, which has no
  `ta-layout`. It takes core reserved from two hosts' `regionBytes`, as
  before, and the record sizes for core used from two more hosts per
  pool at 2^16 and 2^17, as `ta-layout`'s check does; its test runs
  `build/ta-layout` (both modes) and requires each row's core reserved
  and the record sizes to equal it, so the equality the proposal asks
  holds without the example needing the program. The columns are core
  reserved, core used, host other, quickjs, accounted, process and blob
  (no binding column: O1). quickjs's column needs its blocks, so
  `host.quickjsHeapBlocks` joined the API (`ta_datamodel_ecmascript_heap_blocks`,
  in both packages, `index.d.ts` and `docs/api-map.md`); its header is
  taken as 16 B, the 64-bit C library's. The test found that quickjs keeps
  the event an actor handled last in core's events pool (its `_event`;
  the native datamodel detaches it), so after an event a quickjs actor
  uses one event's record more than a native one, and the delayed-send
  row's actor one; the test states it. Host other is 0 B per thing in
  every row of a sized host, as spike R1 found. `mise run measure`,
  `tools/measure.mjs` and the per-session table of `AGENTS.md` are gone;
  the example's README and the capacity guide say their tables predate
  the accounted columns and wait for step 7.
- **The Python binding (step 6).** The same as the Node binding's: the
  host's `allocator` counts over the raw one (`ta_python_counted`), core's
  host gets the raw one; `tests/test_memory.py` reads the count through
  the module's `_binding_memory(host)`. Python has no direct layer to
  hide it in, so O1's "no public API" is a private name: underscored,
  out of `docs/api-map.md`, the README and the docstring check (which
  skips private names). `host.quickjs_heap_bytes` and
  `host.quickjs_heap_blocks` joined the API, since the binding had no
  reading of quickjs's heap at all. `mise run build` links
  `build/ta-layout` as Node's does. `main.py` has the Node example's
  columns and drops tracemalloc and its second process per row (the
  "Python heap" column); `test_memory.py` the same equalities.
  `mise run measure`, `tools/measure.py` and the per-session table of
  `AGENTS.md` are removed; the README's tables say they predate the
  accounted columns. The root `AGENTS.md`'s "Published numbers" row
  names the columns, the website's capacity guide and the `ta-layout`
  check.
- **The tables (step 7), done 2026-11-25.** The sweep of root
  `AGENTS.md` "Published numbers" ran on one machine with nothing else
  running (Intel Xeon @ 2.10GHz, 4 CPUs, 16 GiB, Linux; node 22.23.5,
  bun 1.4.1, CPython 3.13.18): `main.mjs` under node and bun on both
  datamodels (quickjs at an 8 GiB heap) and `main.py` on both, at 10,000
  to 4,000,000, into `results/2026-11-25.csv` beside the 2026-11-20
  files. Every Node row completed. On Python's quickjs, limited to 1 GiB,
  the actors after an event failed at a million (`TA_ERROR_INVALID`) and
  both actor rows at two and four million; the latter print `exit 1`,
  since the job's bare `MemoryError` has no message for `main.py`'s
  pattern (`\w*Error: .*`) to match, and the README says so. The
  capacity guide holds three tables (the process at a million, what the
  parts account for at a million, the process by population), which the
  website's capacity guide and performance page transclude. A hibernated
  actor is said to use 81 B of records; 81 B to 83 B is the
  reservation's, its share of the indexes included.

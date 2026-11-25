# Memory accounting: core's costs from its definitions, the rest from counted allocations

**Status:** Proposed (2026-11-25; revised the same day after an
independent refutation, section 8).
**Touches:** tinyactors-core (`tools/layout`, `mise/tasks/layout`, the
fast pass in `mise/tasks/test`, `src/host/host.c` `host_new`,
`include/tinyactors/stats.h`, `docs/requests.md` `host.stats`, possibly
`src/otlp/otlp.c`), both bindings (`examples/memory`, `tools/measure.*`
and the per-session tables in their `AGENTS.md`, the allocator wrapper),
the wasm package if a binding property is added, the published tables
(root `AGENTS.md`, "Published numbers").
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Summary

The maintainer (2026-11-25): "our memory measurement approach is flawed,
and we should base the core metrics on the actual data structure
definitions and tracing allocator calls."

Today every published number per actor, message and delayed send is a
difference of process memory, beside columns whose definitions mix
owners. This proposal splits each row into parts with one owner each,
and takes each part from the most exact source that already exists or
can be added cheaply:

| Part | Owner | Source | Exact? |
|---|---|---|---|
| what the host reserves for N things | core region | the definitions, through `ta_system_layout` (`carve()` over `sizeof`): `regionBytes` of a host sized for N less one sized for 0 | yes, at a given N (rounding depends on N) |
| what the things use of it | core region | core's own pool and slab counters, already traced: `used` and `highWater` of each pool and `slabBytes` (`host.stats` `pools`) | yes |
| everything the host allocates | core host | a counter around the host's allocator, wrapped once at creation, reported by `host.stats` | yes |
| the binding's tables, arenas and datamodel writers | binding | a counter around the allocator the binding gives them | yes |
| Python objects | engine | `tracemalloc` less the two counters above | yes for one CPython and population (spike) |
| quickjs's heap | engine | `ta_datamodel_ecmascript_heap_bytes`, already a counting allocator | requested bytes; per-block overhead not counted |
| V8's and JavaScriptCore's heaps | engine | measured after a forced collection, not traced | no: within about 2 % (spike) |
| the process | — | `RssAnon` + `VmSwap` on Linux | no: informational, never asserted |

Two findings shape the design:

- **The host's allocator calls cannot give a cost per thing.** A host
  sized up front allocates nothing while 100,000 actors are spawned
  (spikes 2 and R1); its allocator reports capacity. Core's per-thing
  numbers come from the definitions (reservation) and from core's own
  pool and slab counters (use), which are already the "traced"
  allocators of core's inside.
- **The reservation is what a machine must hold.** A host sized for a
  million actors is resident at creation: `RssAnon` grew by 196,435,968 B
  for a `regionBytes` of 196,425,953 B, and by 4,096 B over the million
  spawns (spike R2). So planning uses the reservation; "in use" from the
  pools is a check and an operator's reading.

## 1. Whom it serves, who pays, what is out of scope

**First the capacity planner** (docs/roles.md: "How much memory per
actor? ... Needs: documented costs per actor and per event"). Today the
published row "per actor, started" on node native is 1327 B at 10,000
actors and 501 B at 4,000,000
(`tinyactors-napi/examples/memory/results/2026-11-20.csv`), and on
CPython 341 B at 10,000 and 408 B at 1,000,000 (`tinyactors-cpython/README.md`,
"Memory"): no column explains either direction. With the rows split,
the planner reads a reservation per thing from core, the binding's and
the engine's share per thing, and a fixed cost per host, and computes
the machine for N.

**Then the maintainers** ("Did this change cost memory?"): a struct that
grows changes `ta_system_layout`'s deltas, which the fast pass compares
on the commit that grows it. Today nothing compares them: `mise run
layout` is not in `mise run test` (`tinyactors-core/mise/tasks/test/_default:16-27`
runs `Core::PROGRAMS` and one torture scenario; `ta-layout` is in
`Core::TOOLS`, `tinyactors-core/mise/core.rb:332`).

**Then the operator** ("Is it growing? Is a pool about to fill?"): the
pools' use is in `host.stats` already (`docs/requests.md:191-200`); this
adds the bytes the host holds through its allocator and their peak, of
which `regionBytes` is a part. Over OTLP the operator and the
observability backend get only the region today
(`src/otlp/otlp.c:2279`, `tinyactors.memory`, "region"); a new metric is
decision D1b.

**Who pays:**

| Who | What | How much |
|---|---|---|
| every host, per allocation | the counted wrapper: one indirect call and three integer operations | nothing per event: the host's allocator is called at growth, at creation and close, and once per `host.stats` or `metrics` call (`src/host/request.c:550`, `telemetry.c:215`); 0 calls in 100,000 spawns of a sized host (spikes 2, R1) |
| core's API | three `uint64_t` in `ta_host_stats` (`stats.h:80-88`), three keys in `host.stats`; optionally one OTLP metric | a minor-version change; `ta_host_stats` has a `size` field |
| core's fast pass | `ta-layout` built in the dev build and run once | one compile and a few dozen `ta_system_layout` calls |
| each binding | wrap the allocator its tables, arenas and datamodel writers use; the memory example rewritten | about 20 lines of C each |
| each binding, if D3 | one read-only property, its `docs/api-map.md` row, `index.d.ts`, `docs.h`, and **the wasm package's** own implementation (`tinyactors-napi/wasm/lib/host.mjs`, checked by `tools/parity.mjs`) | three implementations, not two |
| the C embedder | nothing unless it reads the new stats fields | — |

**Out of scope:** what V8 and JavaScriptCore keep inside their heaps;
glibc's per-block overhead and fragmentation; Node-API's own mallocs (a
payload's `napi_ref`, about 96 B on node: `tinyactors-napi/docs/allocation.md`,
finding 3); the wasm package's linear memory; the compiler's transient
memory. These stay in the gap between accounted and process memory,
which the tables show and explain.

## 2. What exists, and what is wrong with it

Each item checked on 2026-11-25.

**Core: `mise run layout`.** `tinyactors-core/mise/tasks/layout` builds
the layers with debug information (`:31-34`), reads struct sizes and
macros with the pinned `llvm-dwarfdump`, and computes the cost of one
more of each thing as `carve()` lays it out (`COSTS`, `:61-74`). It then
runs `ta-layout` (`tools/layout/layout.c`), which asks `ta_system_layout`
(`include/tinyactors/system.h:132`) for a segment at 2^16 and 2^17 of
each capacity and prints the difference per unit (`layout.c:58-67`); a
mismatch fails the task (`layout:156`). Run today:

```
actor            81 B  record 48, links 16, session index 8, name index 8, generation 1
                       up to 89 B: the two indexes round up to a power of two
                       in memory it also holds its chart's instance and 16 B of strings
message         106 B  event 56, trace context 48, generations 2  (57 B with tracing off)
delayed send    151 B  timer 24, heap entry 20, generation 1, its message 106  (102 B off)
request 41 B, outbox entry 256 B, trace rule 72 B, chart of the image 114 B,
slab page 4112 B, segment once 203704 B
Checked against ta_system_layout (tools/layout/layout.c): actor 81 B, event 106 B,
untraced-event 57 B, timer 45 B, request 41 B, outbox 256 B.
```

This already is "core's numbers from the definitions". What is wrong:

- it is not in the fast pass;
- it has no row for a hibernated actor, and says nothing of the instance
  slab, which a sized host reserves **for the image's largest instance
  class, for every actor** (`src/system/system.c:65-77`): a cost per
  image, not per chart;
- its numbers are copied by hand into both bindings' tests
  (`tinyactors-cpython/examples/memory/test_memory.py:37-43`,
  `tinyactors-napi/examples/memory/test.mjs:12-18`) and into prose (the
  capacity guide, the Python README, `website/content/guides/capacity.xml:131`).

**Core's own counters, already traced.** `ta_system_stats`
(`include/tinyactors/stats.h:20-56`) counts each pool's `used`, capacity
and high water, and `slab_pages` used of `slab_pages_capacity`;
`host.stats` reports them per segment as `pools`, with `slabBytes`
(`docs/requests.md:191-200`). These are core's internal allocators, and
they are exact: 100,000 spawns of a one-state chart used 2,381 slab
pages = ⌈100,000 / 42⌉, and hibernating 50,000 of them fell to 1,191
(spike R1). That also shows a hibernated actor gives back its instance
and keeps its records only.

**The bindings' memory examples** (`tinyactors-cpython/examples/memory/main.py`,
`tinyactors-napi/examples/memory/main.mjs`):

| Column | How | What is wrong |
|---|---|---|
| memory | CPython: `RssAnon + VmSwap` (`main.py:61-73`, since `f34a0630`); node and bun: `process.memoryUsage().rss` (`main.mjs:128, 136`) | the bindings measure different things (`f34a0630` changed CPython only); it holds amortized fixed costs (node native started: 1327 B at 10,000, 501 B at 4,000,000) and behaviour no column explains; it flaked (`f34a0630`) and tests assert `resident > 0` (`test_memory.py:46-47`, `test.mjs:19-20`) |
| heap (Python) | `tracemalloc`, started after the host is made (`main.py:143-145`) | it also holds the binding's tables and the region's growth, since `tracemalloc` sees `PyMem_RawMalloc` (spike 4; `tinyactors-cpython/tools/measure.py:14-17`) |
| heap (node, bun) | node `heapUsed + external`, bun `heapSize + extraMemorySize` (`main.mjs:53-63`); quickjs `host.quickjsHeapBytes` (`main.mjs:131`) | node 22's `external` leaves out what the addon reports with `napi_adjust_external_memory` (spike 1; `tools/measure.mjs:18-19`), bun's includes it (spike 1, R3): the binding's tables are in no column on node and in a different one on bun; CPython has no quickjs heap reading |
| core reserves | `regionBytes` of a host sized for N less one sized for 0, over N (`main.py:86-92, 118-119`; `main.mjs:66-72, 114`) | sound and deterministic, but "per actor, started" (199, 195, 196 B at 10,000, 100,000, 1,000,000) is 81 B of records, about 97.5 B of instance slab and 16 B of strings reserved, and no column says so; tests assert only `> 81` |
| blob | the store's bytes over the saves | sound |

**The `measure` tools** (`mise run measure` in both bindings) report a
fourth definition, "region" as `regionBytes` over N
(`tinyactors-napi/tools/measure.mjs:23-25`), the segment's fixed cost
included. Their tables are the per-session costs in both `AGENTS.md`.

**The allocators.** Not every host byte goes through `host_alloc`/`host_free`
(`src/host/host.c:29-35`): the host struct itself (`host.c:426, 447,
628`), the outputs arena of blobs, traces, logs and metrics
(`host.c:436`, through `ta_growable_arena`), the blob scratch
(`host.c:72`) and the session maps of moves and migrations (`host.c:137,
579, 606, 1619`; `session_map.c`) use `host->config.allocator` directly.
50,000 hibernations made one 16 KB allocation of the outputs arena that
`host_alloc` never saw (spike R1). `ta_allocator.free` is given the size
(`include/tinyactors/allocator.h:12, 27`), so a counter needs no header.
quickjs counts its own heap (`datamodel_ecmascript.c:3551-3602`,
requested bytes, a 16-byte header per block not counted). Core's tests
already have counting allocators (`tests/support/counting_allocator.h`,
`tests/unit/test_host_memory.c:36-60`).

## 3. The frame

- **One owning thread per host** (`tinyactors-core/docs/bindings.md`,
  section 5): counters are plain integers on that thread.
- **The host allocates only through `config.allocator`**, from several
  call sites (section 2); base through system never allocate.
- **A binding allocates only in its tables, arenas, host struct, the
  datamodel's scratch arena and the datamodel's kept output writers**
  (`tinyactors-napi/src/datamodel/datamodel.c:126`, `rows.c:104`;
  `tinyactors-cpython/src/datamodel/datamodel.c:731-742`), through one
  `ta_allocator` per host, enforced by the lint rule
  `allocation-in-tables-and-host`.
- **Allocations are not per event.** A sized host allocates at creation,
  `add_image`, close, and per `host.stats` or `metrics` call; a grown
  one at each relocation, of any pool (100,000 sends, 1,000 at a time,
  relocated an events pool sized for 4,096: peak 44.5 MB for 22 MB live,
  spike R1); tables at each doubling (allocation.md, finding 1).
- **Engine heaps are outside the host's allocator.**

## 4. The design

### 4.1 Core's costs: reservation from the definitions, use from the pools

**Reservation per unit.** `ta-layout` moves to `Core::PROGRAMS`, so the
fast pass builds and runs it. It already proves the deltas divide evenly
(`layout.c:60`); what makes it a check of the definitions is a reference
to compare with. Two ways (decision D6):

- *(a)* `tinyactors-core/docs/costs.txt`, `ta-layout`'s output for the
  per-unit capacities (actor, event, untraced event, timer, request,
  outbox), and `ta-layout --check` against it. A change detector: a
  struct that grows fails the fast pass and the commit rewrites the file
  (`mise run layout -- --write`). Its value beyond detection is one
  source the bindings' tests read instead of copying 106, 57, 151, 102.
- *(b)* No file: `mise run layout`'s DWARF check (parts sum to
  `ta_system_layout`) runs in the fast pass. It explains a change, but
  builds every layer again with debug information, and the bindings'
  tests keep their own numbers.

No per-chart rows: the instance reservation is per image (the largest
class, `system.c:65-77`) and its page rounding depends on N, so only
`ta_system_layout` at N gives it. The bindings already ask it at N: the
`regionBytes` difference.

**Use, from core's counters.** For any chart and N, without new API:
an actor's records are the actors pool's `used` × the actor unit; its
instance and strings are the `slabBytes.used` delta over N; a message
waiting is the events pool's `used` × the event unit; a hibernated
actor's slab share is the fall of `slabBytes.used` when it hibernates.
`mise run layout` gains a hibernated-actor line (its records, with the
instance returned: spike R1).

### 4.2 Counted allocations

**In the host: wrap once, at creation.** `ta_host_create` (and the
restore) wraps `config.allocator` in an internal counted allocator kept
in the host, so every call site in section 2 counts without being
touched; the host struct, allocated before the wrapper exists, is added
as `sizeof *host`. `ta_host_stats` and `host.stats`'s `host` map gain
`allocatedBytes`, `allocatedPeak` and `allocations`. With `regionBytes`
beside them, the host's blocks outside the regions are the difference,
and a relocation shows in the peak (spike R1: 44.5 MB peak, 22.5 MB live).
The counter type stays internal; base gains no public API unless an
embedder needs it (the test fixtures can keep theirs).

`allocations` moves on every `host.stats` and `metrics` call, which
allocate and free (`request.c:550`, `telemetry.c:215`), so a test of "no
allocation while spawning" reads the counter in C, not through a post.

Cost: three integer operations and one indirect call per allocation;
always on, since it adds no path and changes no outcome (it is a
reading, like `regionBytes`; `tinyactors-napi/AGENTS.md`, "Refusals no
input reaches", forbids an allocator that fails on demand, not a count).

**Alternative (D1):** no core change; each binding counts the
allocator it gives the host's environment, as napi already reports each
block to the engine (`tinyactors-napi/src/module.c:22, 31`). The
examples get the number; the operator, the C embedder and OTLP do not.

**OTLP (D1b).** The host's metrics export `region_bytes` only
(`src/otlp/otlp.c:2279`). If the operator is to see the host's whole
footprint, `allocatedBytes` joins it as a gauge (one series per host,
bounded cardinality: docs/roles.md, the observability backend).

**In each binding: its own allocations.** The binding gives its tables,
arenas and datamodel writers a counted allocator separate from the one
it gives the host, so the two owners never mix. Those writers keep the
largest value ever written, so the binding's bytes are not a function
of the tables' capacities alone: the counter is the measurement. How it
is read is decision D3 (a property, with the wasm package as a third
implementation; or the examples only, through a test-only path that the
bindings' rules would have to allow).

**Engine heaps.**

- *Python's objects:* the `tracemalloc` delta less the deltas of the
  host's and the binding's counters, which `PyMem_RawMalloc` also serves
  (spike 4). Repeatable at one N (spike 3). Measurement only, as now;
  pymalloc's arenas and pools are not counted.
- *quickjs:* `ta_datamodel_ecmascript_heap_bytes`, as napi reads it now;
  CPython gains the reading with D3. At a million started actors on
  node, process memory is 1041 B against 598 B of quickjs heap and 196 B
  of core reservation (the CSV): 247 B unaccounted, plausibly per-block
  overhead (unverified; a block count in quickjs's counter would settle
  it: D4).
- *V8 and JavaScriptCore:* `heapUsed` after forced collections,
  labelled **measured, not traced**; stable to about 2 % (spike 5).

### 4.3 Process memory

Kept as a column, **informational, never asserted**, measured the same
way in both bindings: `RssAnon + VmSwap` from `/proc/self/status` on
Linux (node reads the file too), the peak elsewhere, as `f34a0630` does.
Shown beside the accounted sum with the gap and what fills it: malloc's
overhead per block, engine structures outside the measured heap,
Node-API's references, and a host's and an engine's fixed costs spread
over the population. The capacity guide keeps a process row by
population, since a machine must hold it.

### 4.4 Tables, programs and tests

**The memory examples** become the one program per binding (D2). Every
measurement sizes **every pool it fills** for N (actors, events, timers),
so no relocation runs during it (spike R1). For each row and N:

| core reserved | core used | binding | engine heap | accounted | process | gap | blob |
|---|---|---|---|---|---|---|---|

- *core reserved:* the `regionBytes` difference, as now (planning);
- *core used:* from `host.stats` `pools` (4.1);
- *binding:* the binding counter's delta over N;
- *engine heap:* traced, counted or measured, the source named;
- *accounted:* core reserved + binding + engine heap.

**Tests:**

| Assertion | Where | Kind |
|---|---|---|
| `ta_system_layout`'s per-unit deltas equal the reference (D6) | core fast pass, `ta-layout` | equality |
| the host's `allocatedBytes` equals an outside counting allocator's live bytes over a workload with hibernations, traces and a migration (the direct call sites included), and both are 0 after `ta_host_free` | core, `tests/unit/test_host_memory.c` | equality |
| a host with every pool sized makes no allocation while it spawns N actors and runs, read in C | core, the same test | equality |
| a hibernated actor returns its instance: `slab_pages` used falls by the hibernated actors' pages | core test | equality |
| the examples' message and delayed-send rows equal the event and timer units (`ta_system_layout` is linear in those pools) | both examples | equality |
| the started and hibernated actor's reserved core column lies between the actor unit and its most (index and page rounding depend on N) plus, for a started actor, the instance and string reservation | both examples | range |
| the core used column of a started actor equals actors used × unit + the `slabBytes.used` delta | both examples | equality |
| the binding counter is 0 after close | both bindings' tests | equality |
| engine heap, process, gap | — | not asserted (D5) |

**The published tables** (root `AGENTS.md`, "Published numbers"): the
core row's source stays `mise run layout`, checked in the fast pass; the
memory rows (both examples' READMEs, the Node capacity guide's "What an
actor costs", the Python README's "Memory",
`website/content/guides/capacity.xml`) show the split table with
process memory beside it; the per-session tables in both `AGENTS.md`
cite the examples or go with `measure` (D2). The CSV gains the columns;
earlier CSVs stay.

## 5. Not chosen

- **Per-thing core numbers from the allocator's calls**: a sized host
  makes none per thing (spikes 2, R1); a grown one reports its growth
  policy (255 B per actor grown from the default size, spike 2).
- **Counting in `host_alloc`/`host_free`**: it misses the host struct,
  the outputs arena, the blob scratch and the session maps (section 2,
  spike R1). Wrapping the allocator once catches every site.
- **Per-chart rows in core's table**: the reservation is per image and
  per N (`system.c:65-77`); use per chart comes from `slabBytes.used`.
- **A test-only build**: the counter costs nothing per event and the
  operator would lose the numbers.
- **A header per block**: `free` is given the size.
- **glibc's `mallinfo2`** as a column: per process, glibc only, outside
  the bindings' allowed C library calls.
- **Hooking V8's allocator**: Node-API and Bun expose none for the
  managed heap.
- **Asserting process memory in a tolerance**: its variation is the
  machine's and the engine's, not the change's.

## 6. Spikes (2026-11-25, x86_64 Linux 6.18, 4 CPUs; node 22, bun 1.4.1, CPython 3.13)

Spikes 1 to 5 are the author's; R1 to R3 the refuting agent's (programs
in the session's scratch directory, `refute-memory/`).

1. **`external` and the addon's reports.** A host sized for 200,000
   actors (`regionBytes` 39,619,089): node's `external` rose 308,021 B at
   creation (the region not in it); bun's rose 39,742,247 B (in it).
2. **A counting allocator over a host** (100,000 spawns, null
   datamodel): sized, +0 B and 0 allocations during the spawns, 5
   allocations in the host's life; from the default size, 255.38 B per
   actor in 7 allocations, peak 39,694,552 B.
3. **`tracemalloc` per actor**, `"python"` datamodel: 144.24 B at 10,000
   (twice, identical), 144.73 at 20,000, 145.03 at 40,000.
4. **`tracemalloc` sees the region:** +19,792,954 B traced for a host
   whose `regionBytes` is 19,740,721.
5. **V8's heap per actor**, two processes: 140.21 / 138.53 / 137.99 B
   and 140.21 / 138.58 / 138.09 B at 10,000 / 20,000 / 10,000.
- **R1.** A host sized for 100,000 actors, events pool 4,096: create and
  image 5 calls, 22,064,200 B live; 100,000 spawns: 0 calls, `slab_pages`
  2,381 of 3,071; 100,000 events sent 1,000 at a time: 1 relocation,
  peak 44,529,392 B; 50,000 hibernated: 1 call (the outputs arena,
  16 KB, outside `host_alloc`), `slab_pages` 1,191; after free, live 0,
  7 calls and 7 frees.
- **R2.** A host sized for 1,000,000 actors: `regionBytes` 196,425,953,
  `RssAnon` +196,435,968 B at creation, +4,096 B over 1,000,000 spawns.
- **R3.** Bun's `external` includes `napi_adjust_external_memory`;
  node 22's does not.

## 7. Decisions for the maintainer

- **D1.** Count in core (wrap the allocator in `ta_host_create`;
  `host.stats` gains `allocatedBytes`, `allocatedPeak`, `allocations`),
  or only in the bindings (no core API; the operator and C embedder get
  nothing). **D1b:** with D1, also export `allocatedBytes` over OTLP.
- **D2.** Retire `mise run measure` and the per-session tables in both
  `AGENTS.md` for the memory examples' split rows, or keep `measure`
  with the same columns.
- **D3.** How the binding's own bytes are read: a public property
  (`host.bindingBytes`, `host.binding_bytes`; Python also
  `quickjs_heap_bytes`), costing node, Python **and the wasm package**;
  or no API, the examples reading the counter through a path the
  bindings' rules would have to allow.
- **D4.** A request to core: quickjs's counter also counts blocks.
- **D5.** Whether the Python-object column is asserted against a value
  recorded for the pinned CPython (spike 3), updated when the pin moves.
- **D6.** Core's per-unit reference: a checked-in `costs.txt` that
  `ta-layout` compares in the fast pass and the bindings' tests read
  (a), or the DWARF task in the fast pass and no file (b).

## 8. Refutation

A fresh agent, given the code paths and the first version of this
proposal, tried to refute each claim with the repository and spikes of
its own (R1 to R3). What it found, and what changed:

| # | Its finding | Verdict | What changed |
|---|---|---|---|
| 1 | "Every host byte goes through `host_alloc`/`host_free`" is false: the host struct, the outputs arena, the blob scratch and the session maps use `config.allocator` directly; 50,000 hibernations made a 16 KB allocation `host_alloc` never saw | confirmed (`host.c:72, 137, 426, 436, 447, 579, 606, 628, 1619`; R1) | the counter wraps the allocator once at creation, plus `sizeof *host`; the public base counter is dropped; the binding-side alternative stays as D1 |
| 2 | The ask was misread: core's internal allocators (pools, slab) are already traced in `ta_system_stats` and `host.stats` | confirmed (`stats.h:20-56`, `requests.md:191-200`; R1: 2,381 pages = ⌈100,000/42⌉, 1,191 after 50,000 hibernations) | "core used" comes from those counters for any chart, with no new API; it also settles that a hibernated actor returns its instance (was "plausible") |
| 3 | Per-chart rows in `costs.txt` are wrong: the reservation is the image's largest instance class for every actor; and the file is only a change detector | confirmed (`system.c:65-77`) | per-chart rows dropped; the file is now an option (D6) beside running the DWARF check |
| 4 | Equality for a started actor's core column fails: 199, 195, 196 B at 10,000, 100,000, 1,000,000, since rounding depends on N and the bindings cannot call `carve()` | confirmed (napi CSV) | that row is a range; equality kept for message and delayed send, and for "core used" |
| 5 | `bindingBytes = capacity × stride + arenas` fails: the datamodels' kept writers also use the binding's allocator, sized by the largest value written | confirmed (napi `src/datamodel/datamodel.c:126`, `rows.c:104`; cpython `src/datamodel/datamodel.c:731-742`) | the counter is the measurement; tests assert only live 0 after close |
| 6 | `host.stats` and `metrics` allocate per call, so `allocations` moves on every read; an unsized events pool relocates during a measurement | confirmed (`request.c:550`, `telemetry.c:215`; R1) | the no-allocation test reads the counter in C; the examples size every pool they fill |
| 7 | Holds: a sized host makes no allocation while spawning; the reservation is what is resident (R2); 196 B = 81 + about 97.5 + 16; `tracemalloc` sees raw allocations; node's `external` leaves out the addon's reports, **bun's includes them**; the counter is not a test hook | confirmed | planning uses the reservation, "core used" is a check; spike 1's bun reading corrected from "unexplained" to "included" |
| 8 | Missing: OTLP exports `region_bytes` only, so the operator and the backend gain nothing; the wasm package is a third implementation of any binding property | confirmed (`otlp.c:2279`; `wasm/lib/host.mjs`, `tools/parity.mjs`) | D1b added; D3 costs the wasm package |

Still unverified: what fills quickjs's 247 B gap per actor (D4 would
settle it).

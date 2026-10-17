# Allocation by lifetime (2026-10-17)

The analysis step p2-decisions (7) asks for ([plan.xml](plan.xml),
log entries `l-coord-allocation`, `l-p1-measure-resident-engine`): every
allocation the addon makes and every block core asks of the host's
allocator, traced by lifetime, measured, with arenas over the
environment's allocator proposed where a lifetime is known instead of a
`malloc` and a `free` per block. It subsumes item (2), the spike of a
session row's `napi_ref` against scopes held in one JavaScript array per
host.

**Whom it serves:** first the capacity planner and the operator (bytes
per actor, calls into the allocator per event), then the application
developer (an event's cost), then the maintainers (one allocation story
for both bindings). **What answers them already:** `mise run measure`
(bytes per actor), the host's `regionBytes`, glibc's `malloc_stats`.
**Who pays for a change:** per event nothing, per actor nothing, per host
the scratch arena's high-water mark once a chart parses XML; code size a
few lines in `src/datamodel/dom.c`; API surface none; work in the Python
binding none (its datamodel has no XML parser). **Out of scope:**
quickjs's own `malloc` (core's sandbox allocates for itself), V8's and
JavaScriptCore's heaps, and core's growth policy.

## The frame

Facts the claims below depend on, each checked:

- One thread owns a host; every call comes from it
  (`tinyactors-core/docs/bindings.md`, section 5; AGENTS.md, "C rules").
- The host gets every byte from `environment->allocator` and gives every
  block back at `ta_host_free`; nothing grows during a run
  (`tinyactors-core/docs/embedding.md`, section 8). The addon's allocator
  is `malloc` and `free`, each block reported with
  `napi_adjust_external_memory` (`src/module.c:16-32`).
- `ta_growable_arena` keeps released chunks and reuses them: "an
  identical warmed sequence needs no backing allocator calls"
  (`tinyactors-core/include/tinyactors/arena.h`, `ta_growable_arena_alloc`).
- `ta_table` grows by doubling through the allocator it is given
  (`include/tinyactors/table.h`, `ta_table_add`); its handle is
  `generation << 32 | slot + 1` (same comment).
- What the host hands out lives until the ack; a datamodel's output until
  its next call (ADR 0012).

## How it was measured

On this machine (x86_64 Linux 6.18, Intel Xeon @ 2.10GHz, 4 CPUs, 16 GiB;
node 22.23.3, bun 1.4.1), 2026-10-17:

- **Calls of the host's allocator** by size and phase: the program
  `allocs.mjs` (in the step's scratch directory) spawns N actors of
  `tinyactors-cpython/tools/charts/fit.ecmascript.scxml`, sends each one
  event, then closes the host, raising `SIGUSR2` between the phases; gdb
  counts every call of `ta_napi_allocator_alloc` and `_free` (the addon is
  not stripped) with a Python breakpoint that never stops, and at each
  signal calls glibc's `malloc_stats()` for the bytes in use.
- **Bytes per actor:** `mise run measure` (10,000 actors, three runs each,
  since resident varies between runs).
- **Cost per event:** `perevent.mjs`, 10,000 actors of `fit`, 30 rounds
  of one `inc` to each (a `cond` and an `assign`), the least time per
  event of the rounds; `xml.mjs`, 1,000 actors whose `<data>` is XML
  content and an `<assign>` of XML content per event, the least of 20
  rounds. Builds compared side by side, alternating, in one session.

## The lifetime table

Every allocation the addon makes, and every block core asks of the
host's allocator. "Calls" are calls of the allocator.

| Object | Owner | Created | Read | Ends | Size, calls | Shaped by its lifetime? |
|---|---|---|---|---|---|---|
| the environment's instance (`ta_napi_instance`) | the environment | module init, `calloc` (`src/module.c:157`) | every call | the environment's teardown | one struct | yes: one block for the environment's life |
| the instance's arena | the environment | first module call (`encode`, `decode`, `Address`) | the call | chunks kept until teardown, a mark per call | 64 KB chunks | yes |
| a `ta_napi_host` | the wrapper | `new Host` (`src/host.c:750`) | every host call | the wrapper's finalizer | one struct | yes |
| the host's arena | the host | `new Host`, chunks on first use | the call (config CBOR, text, input structs, typed methods' params) | chunks kept until `close()`, a mark per call | 16 KB chunks; 2 at creation | yes |
| the host's tables (charts, sessions, payloads, loops, pending) | the host | first row | the datamodel's entries, the drain | `close()` | doubling: 10 calls for 10,000 sessions, the last 655,360 B (64 B a slot) | yes: one block per table size, the old one freed at the doubling |
| core's region of a segment | core, through the host's allocator | `ta_host_add_image` | runs | the next size (a relocation) or `close()` | sized up front by `maxActors`: one block of 2,868,303 B for 10,016 actors; grown from the default size: 10 relocations to 48,967,047 B at 40,000 actors | yes: one block per segment size; old and new coexist during the copy, so an arena could not hold less |
| core's other host blocks | core | `ta_host_create` | runs, takes | `close()` | about 20 blocks of 16 B to 64 KB | yes |
| the compile result | the `Compiled` | `compileCharts` (`ta_compile_options.alloc`) | `image`'s first read | `close()` or the finalizer (`ta_compile_free`) | about 40 blocks of 16 B to 1.5 KB, freed block by block by the compiler | the lifetime is known, the blocks freed one by one; not changed (below) |
| the native datamodel's last output (`native->output`) | the datamodel | its first output | core, until the datamodel's next call | grown, kept until `close()` | one block, doubling | yes |
| **an XML parse's tree** (txml) | the parse | `ta_napi_dom` (`src/datamodel/dom.c`) | the tree's cursors, while its JavaScript is made | the parse's end | **13 blocks, about 1.7 MB, for a small document, freed block by block** | **no, before this step; yes now** (finding 2) |
| a snapshot's pack | the call | `host.snapshot()` (`src/host.c:1565`, `:1608`) | `ta_host_snapshot_pack` | the call's end | the snapshot's size | the call's, but sized by the population: kept chunks would hold the largest snapshot for the host's life; not changed |
| `Host.restore`'s copy of an unaligned snapshot, and its system id | the call | `Host.restore` (`src/host.c:592`, `:636`) | the restore | the call's end | the snapshot's size, once | as above |
| a session's `napi_ref` | the session's row | `create`, `deserialize`, a restore | every entry (`ta_napi_evaluation_of`) | `destroy` | node's `v8impl::Reference` and its global handle, outside the host's allocator: about 96 B an actor of `malloc` (finding 3) | not the host's allocator; an alternative measured, not adopted |
| a payload's, a chart's and a loop's `napi_ref` | the row | `capture`, an input; `chart_attach`; `<foreach>` | the datamodel | the row's end | one per row | as above, not measured separately |

## Findings

Each claim with its scenario, its evidence and its verdict as this
analysis sees it; the refutation is below.

### 1. Core's blocks and the addon's own are already shaped by their lifetimes

- **Claim:** the host's allocator is called once per lifetime-sized
  block, never per actor or per event: per event nothing; per spawn
  nothing but the tables' doublings; a segment's region one block per
  size.
- **Scenario:** a host of 10,000 actors of `fit` sized up front, each sent
  one event; then one grown from the default size to 40,000.
- **Evidence:** gdb's counts (`allocs.mjs`): node, native, 10,000 actors:
  54 calls at creation (3,029,351 B, the region 2,868,303 B of them, the
  compile's temporaries the 39 small frees), **10 calls during the 10,000
  spawns** (the session table, 1,280 B to 655,360 B, each previous size
  freed), **0 calls during the 10,000 events**, 13 frees at `close()`.
  On quickjs: 0 calls during the spawns and the events (quickjs's state is
  its own `malloc`). Grown from the default size to 40,000 actors: 22
  calls on native (the region's 10 relocations, 190,511 B to 48,967,047
  B, and the session table's 12 doublings), 10 on quickjs, none during
  the events.
- **Verdict:** confirmed for these scenarios. A `malloc` per block is the
  right shape here: each block is one lifetime (a table size, a segment
  size), and the old and new blocks of a growth coexist during the copy,
  which an arena would not shorten.
- **Action:** none. Observation for core, not followed here: a region
  grown from the default size is 48,967,047 B at 40,000 actors (1224 B an
  actor), while one sized up front for 10,016 is 2,868,303 B (287 B an
  actor); why growth costs four times as much per actor is unverified.

### 2. An XML parse allocated and freed about 1.7 MB in 13 blocks, each time

- **Claim:** txml's blocks live for one parse, so they belong in an arena
  with a mark per parse; with its chunks kept, a warmed parse calls no
  allocator.
- **Scenario:** a chart whose `<data>` is XML content and whose
  transition assigns XML content: each spawn and each event parses once
  (`ta_napi_dom`), on the shared context of a host.
- **Evidence:** gdb's counts (`xml.mjs`, node, native): 1,000 spawns made
  **13,006 calls and 13,006 frees** (1,706,944,640 B: 65,672 B five
  times, 131,208 B and 262,280 B twice, 524,424 B, 65,736 B, 376 B and
  992 B per parse) and 20,000 events 260,001; with the arena, **6** (the
  session table's doublings) and **1**. Cost per event (least of 20
  rounds, three runs each): bun **447 to 503 us before, 14.6 to 15.4 us
  after**; node 20.9 to 22.9 us before, 21.1 to 22.5 us after. On bun each
  parse's blocks were reported to the engine
  (`napi_adjust_external_memory`, 1.7 MB up and down), which is the
  likely cause of bun's cost (unverified: not separated from glibc's
  `mmap` of the blocks over 128 KB).
- **Verdict:** confirmed (measured both ways).
- **Action:** built (commit `c5f8e1df`): the native datamodel keeps a
  `ta_growable_arena` (`native->scratch`, over the host's allocator,
  64 KB chunks, kept), txml allocates from it above a mark taken at the
  parse's start and released once the tree is read, and its frees are
  nothing. The cost moves to the host: the high-water mark (about 1.7 MB
  for txml's default limits) stays reserved for the host's life once a
  chart parses XML. AGENTS.md, "The binding's memory", says so.

### 3. A session's `napi_ref` costs about 96 B an actor; one array per host saves it and costs per event

- **Claim:** holding the sessions' scopes in one JavaScript array per
  host, at the slot of the row's handle (no `napi_ref` per session),
  saves about 96 B an actor of `malloc` on node and 80 to 90 B of
  resident, but makes every datamodel entry read the scope through
  `napi_get_element`, which costs about 0.4 us an event on node (8 %); so
  it is not adopted.
- **Scenario:** a host of 10,000 resident idle sessions of `fit`, then
  rounds of one event to each.
- **Evidence:** the spike (the step's scratch, `spike-scopes.diff`:
  `ta_napi_session_scope`, `_hold`, `_let_go` in `rows.c`, the row without
  `scope`, 24 B in place of 32). glibc's bytes in use per actor after the
  spawns: **160.4 B before, 63.9 B after** (`allocs.mjs`; the session
  table is about 52 to 65 B of either). `mise run measure`, resident per
  actor, three runs each: node native 1266, 1237, 1269 before, 1194,
  1184, 1180 after; after one event 1666, 1654, 1693 before, 1595, 1593,
  1615 after; heap 189 before, 197 after (the array's element); bun native
  2008, 2043, 1767 before, 1910, 1918, 1919 after (bun's resident varies
  most). Per event (`perevent.mjs`, three alternating runs): node **5.13,
  5.27, 5.16 us before, 5.54, 5.70, 5.84 us after**; bun 3.73, 3.90, 3.90
  before, 3.93, 3.89, 4.00 after; spawn and terminate node 7.49 to 7.63
  us before, 7.91 to 8.41 after.
- **Verdict:** confirmed for node; on bun the saving is within resident's
  variation and the cost within noise.
- **Action:** not adopted: the decision (`l-p1-measure-resident-engine`)
  adopts it only "if it saves memory per actor without a cost per event",
  and node pays about 8 % an event. The numbers are in AGENTS.md,
  "Measured cost per session", in place of the unverified attribution.

### 4. The compile result's blocks are freed one by one

- **Claim:** a compile result's about 40 small blocks share one lifetime
  (`compileCharts` to `close()`), so an arena per result would turn about
  40 calls into one or two.
- **Scenario:** an application compiles its image once at start, or once
  per deploy.
- **Evidence:** the 39 frees of 16 B to 1.5 KB in phase 0 of gdb's counts
  (the compile, then `close()`); the compiler frees through
  `ta_compile_options.alloc` (`src/compile.c:366`).
- **Verdict:** plausible saving, but nothing pays for it: about 40 calls
  per compile, rare. An arena would need one more object per result to
  own it.
- **Action:** none.

### 5. A snapshot's buffers are a call's, sized by the population

- **Claim:** `host.snapshot()`'s pack and `Host.restore`'s copy live for
  one call and are as large as the snapshot (MBs for a population), so a
  kept arena would hold the largest snapshot for the host's life; a
  `malloc` and a `free` per call is the right shape.
- **Evidence:** `src/host.c:1565-1584` (the pack, its probe's size),
  `:1608` (the writer through the host's allocator, "a snapshot's size is
  no call's scratch").
- **Verdict:** confirmed by reading; not measured.
- **Action:** none.

## Refutation

The step asks for a fresh agent to refute each finding against the
repository and the primary sources before any finding changes the design
(reasoned-analysis, step 8). This session cannot start one, so the claims
wait for the coordinator (log entry `l-p2-decisions-refute`). Finding 2
was built because its evidence is a measurement both ways; the others
change nothing.

| # | Claim to refute | Status |
|---|---|---|
| 1 | The host's allocator is called once per lifetime-sized block (tables' doublings, a segment's region per size), never per actor or per event; an arena would not do better. | unrefuted |
| 1b | A region grown from the default size costs about four times as much per actor as one sized up front (1224 B against 287 B). | unrefuted (an observation; its cause unverified) |
| 2 | An XML parse made 13 blocks of about 1.7 MB through the host's allocator; an arena with a mark per parse removes every call after warm-up and the bun cost (447 to 503 us to 15 us an event), at the price of the high-water mark kept per host. | unrefuted |
| 3 | A session's `napi_ref` costs about 96 B an actor of `malloc` on node; one array per host saves it but costs about 8 % an event on node, so it is not adopted. | unrefuted |
| 4 | The compile result's blocks are not worth an arena. | unrefuted |
| 5 | A snapshot's buffers are rightly a `malloc` and a `free` per call. | unrefuted |

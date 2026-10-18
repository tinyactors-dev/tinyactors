# tinyactors (`ta`) — architecture

A portable, embeddable SCXML interpreter and cooperative actor-system VM in C.

> One sentence: **compile** an actor system into an immutable, relocatable
> image; give the library **one block of mutable memory** sized from that
> image; plug in a **datamodel vtable**; then drive it with
> `send / run(budget) / advance_time / resume` and drain **tagged effects**.

Companion documents:

- [scenarios.md](scenarios.md) — the twelve required walkthroughs.
- [layout.md](layout.md) — file tree, build, mise tasks, binding layout.

---

## 0. Requirement conflicts and how they are resolved

These are the places where the brief contradicts itself (or SCXML) and a
decision has to be made. Everything else in this document follows from them.

| # | Conflict | Resolution |
|---|----------|------------|
| C1 | "Full SCXML" (unbounded `<raise>`/`<send>`, unbounded sessions) vs. "no malloc, bounded memory". | All pools are **bounded and sized at system creation**. Exhaustion is a *defined* outcome, never UB: the actor faults with `error.platform`-class semantics only if a reserved emergency slot cannot be used, and the host sees `TA_RUN_EXHAUSTED` with the pool id. The runtime layer can **grow by relocation** (§13) so malloc-capable hosts never notice. |
| C2 | Run-to-completion vs. fairness (`A→B→A…` must not starve the thread). | RTC is a property **observable only through messages**. Actors share nothing, so a yield mid-macrostep is invisible: incoming events go to the *external* mailbox and are not looked at until the macrostep ends. Fuel is deterministic, not wall-clock. |
| C3 | Bounded pool of continuation workspaces vs. liveness (K workspaces held by K infinite loopers would starve everyone else). | **Workspaces are only held *within* a microstep.** Every inter-microstep point (the place an infinite eventless loop spins) is a *parked* continuation that fits in the instance header and releases its workspace. Executable content always terminates (SCXML has no unbounded loop construct; `<foreach>` iterates a shallow copy), so a workspace holder always finishes its microstep in finite fuel ⇒ no starvation. Only "waiting for an external response mid-microstep" can pin a workspace: the host adds workspaces when a run leaves actors waiting for one (`segment_grow` in `src/host/host.c`), up to one per actor. |
| C4 | Per-instance memory minimal vs. fast exit/entry computation. | All derivable structure lives in the image (shared). Transient sets live in the pooled workspace. Instances hold only configuration, history, late-binding flags, invoke slots, queue heads and the `_event` pin. |
| C5 | Datamodel-agnostic vs. cross-actor event payloads without host round-trips. | Payloads are opaque `uint64_t` handles owned by the datamodel (`capture/retain/release`). Same-datamodel actors pass handles; the core never inspects them. Cross-datamodel systems must register a codec — rejected at **link time** otherwise. No universal value type is introduced. |
| C6 | "No arbitrary callbacks" vs. hot datamodel operations. | Exactly two synchronous callback surfaces exist: the **datamodel vtable** and the optional **allocator** (compile/runtime layers only). Effects never use callbacks. Datamodel callbacks may only call back into a small **read-only** query API (`In()`, event view, state names); re-entering `send/run` is forbidden and detected in debug builds. |
| C7 | Position-independent image vs. pointer-speed access. | The blob contains only offsets. `ta_image_open()` validates it once and fills a small *host-side* `ta_image` view (≈ 20 section base pointers). Hot paths use `base[index]`. |
| C8 | Immutable precompiled chart vs. datamodel-specific expression compilation. | The image carries an **expression table** (kind + source text + source position) keyed by `ta_expression` id. A datamodel compiles it once per chart at `chart_attach` (shared by all instances), *or* the image carries an opaque datamodel-tagged AOT section (e.g. generated C switch, JS bytecode). Both use the same ids. |
| C9 | Keep the current `_event` pinned (correctness) vs. 100 k idle actors each pinning a pool slot. | Pin until replaced by default. Datamodels advertising `TA_DATAMODEL_EVENT_DETACHABLE` materialize `_event` on `detach_event()`; the core then releases the slot when the actor reaches stability. |
| C10 | Precomputed entry sets (speed) vs. image size on flash. | Entry sets are stored as **sparse sorted state lists**, not per-transition bitsets (transition_count × state_count bits explodes). History-dependent targets fall back to a bounded iterative algorithm. Exit sets and conflicts need **no storage at all** (§3.6). |
| C11 | A separate, independently usable kernel (`ta_core`) vs. not duplicating queue/event code in two public APIs. | `ta_core` is a **pure resumable step function** that *returns* sends/invokes/cancels as step results instead of performing them. `ta_system` is the only public runtime API; a single interpreter is a system with `max_actors = 1` (≈ 200 bytes overhead). `tinyactors/core.h` is public but marked "advanced". |
| C12 | Small ABI vs. batching & zero-copy views. | ~25 functions. Batch entry points take arrays of fixed-layout structs (`ta_event_in[]`, `ta_effect[]`), each beginning with `uint32_t size` for forward compatibility. Views are borrowed `{pointer, length}` with one documented lifetime rule (§11.3). |
| C13 | Determinism vs. host time. | Time only moves when the host calls `ta_system_advance_time(now)`. Timer ties break by a monotonic sequence number. Same image + same inputs + same datamodel ⇒ same trace. |
| C14 | `<invoke src>`/`<data src>`/`<script src>` vs. "no filesystem, no blocking". | Static `src` is resolved by the **compiler** (inlined into the image, child charts linked by index). Dynamic `srcexpr` becomes an explicit **request/response** effect. |

---

## 1. Layering (challenged)

```diagram
┌──────────────────────────────────────────────────────────────────────┐
│ bindings          thin, hand-written; own nothing SCXML-specific     │
├──────────────────────────────────────────────────────────────────────┤
│ runtime     (optional, libc/OS)  malloc allocator, OS clock, file    │
│             loading, grow-by-relocation, loop adapters               │
├───────────────────────────────┬──────────────────────────────────────┤
│ system      (public ABI)      │ compile     (offline or in-process)  │
│ actors, mailboxes, scheduler, │ XML → SCXML AST → validate → analyze │
│ timers, SCXML I/O processor,  │ → bytecode → link system → image     │
│ invoke, effects, inspection,  │ (uses ta_allocator; may malloc)      │
│ trace contexts and sampling,  ├──────────────────────────────────────┤
│ lifecycle records, draining   │ otlp        trace records + image →  │
├───────────────────────────────┤ OTLP protobuf; OTLP → span views     │
│ core        (kernel)          │ (a codec: never inside a run)        │
│ phase machine, selection,     ├──────────────────────────────────────┤
│ exit/entry, bytecode VM,      │                                      │
│ null datamodel, step records  │                                      │
├───────────────────────────────┴──┬───────────────────────────────────┤
│ image       verify + view of the │ trace   IDs, step keys, span IDs, │
│ immutable image (shared by all)  │ traceparent, sampling, records    │
├──────────────────────────────────┴───────────────────────────────────┤
│ base        building blocks: arena, pool, slab, bitset, index queue, │
│             4-ary heap, handles, journal, freestanding mem ops       │
└──────────────────────────────────────────────────────────────────────┘
          freestanding: no libc, no malloc, no threads, no clock
          ───────────────────────────────────────────────────────
          below runtime / compile
```

Tracing ([ADR 0006](../../docs/adr/0006-tracing-is-a-core-feature.md))
adds two freestanding layers. `trace` (on `base`) holds the values every
layer and binding shares, trace and span IDs, step keys, the traceparent
parser, the seeded generator and ratio sampling, and the schema of the
records; it keeps no state. `otlp` (on `trace` and `image`, never on
`system`) is a codec, like `basic_http`: the host owns one, drains its
segments' records into it inside `ta_host_run`, and says with an effect
when a request is ready (`ta_host_traces`, ADR 0009). The kernel writes
the step records and stamps every event it creates with its cause;
`system` holds the policy (the context pool, sampling, lifecycle records,
the drain API); `host` carries the traceparent across systems.

The host layer (`src/host`, `ta_host`: many systems as one, §15) sits on
`system` and `otlp`, beside `compile`. It is freestanding too, but it allocates: every
block it owns, the segments' regions included, comes from the
`ta_allocator` in its configuration, so it needs no C library either
([ADR 0005](../../docs/adr/0005-freestanding-host.md)). `libtinyactors`
ships base through system, trace, otlp and the host, and promises: **no C library;
`base` through `system` never allocate; `host` allocates only through the
`ta_allocator` it was given.**

Each layer is one static library, `libtinyactors_<layer>.a`, and links only
the layers below it (`mise/core.rb` lists each layer's sources and what it
uses). Changes relative to the proposed four layers:

1. **The base and image layers are split out.** Both the compiler (writer) and
   the kernel (reader) need the image format; both runtime layers need the
   same pools/bitsets. Without this split the compiler would depend on
   core internals or duplicate them.
2. **The core performs no I/O-processor work at all.** It returns
   `TA_STEP_SEND`, `TA_STEP_INVOKE`, … and is resumed. This keeps the kernel
   independent *and* means there is exactly one routing implementation.
3. **`ta_system` (the system layer) is the normal embedding unit**, even for one chart. There is
   no second "single interpreter" public API to keep consistent.
4. **The runtime layer contains zero SCXML semantics.** The SCXML I/O processor is
   in `ta_system` (it is SCXML semantics). BasicHTTP etc. are *external* I/O
   and live in `ext/` as optional host-side effect handlers.

---

## 2. Core building blocks (the base layer)

Everything in the runtime is assembled from these. They are small enough to
read in one sitting and have no dependencies.

| Block | What | Used by |
|-------|------|---------|
| `ta_arena` | Bump allocator over a caller buffer with *measure mode* (NULL base): the **same carving code** computes `ta_system_layout()` and performs `ta_system_init()`, so sizing and placement can never disagree. Mark/reset for scratch. | system init, compiler scratch (chunked variant via `ta_allocator`), image verifier |
| `ta_pool` | Fixed-stride slot pool, `u32` indices, intrusive free list, 8-bit generation per slot. Self-relative offsets ⇒ relocatable with `memcpy`. | actors, events, timers, workspaces, requests, invoke records |
| `ta_handle` | `u32` = `generation:8 | index+1:24`; `0` is invalid. Stale handles fail cheaply. Index width is a compile-time constant. | every ABI-visible id (actor, request, invoke token) |
| `ta_slab` | Page-based size-class allocator over a fixed region (pages assigned to a class on demand, returned when empty). | variable-size instances (chart-dependent size), refcounted dynamic strings (dynamic event names, `idlocation` sendids, external origins) |
| `ta_bitset` | Inline ops over `u32` words: test/set/clear, and/or/andnot, range-mask, find-next/prev set bit, popcount. `u32` words keep snapshots portable across 32/64-bit hosts. | configuration, exit/entry sets, history, enabled-transition sets, invoke-pending |
| `ta_index_queue` | Intrusive FIFO of `u32` indices: `{head, tail}` + a `next` field at a known offset inside each record. Zero allocation per enqueue. | internal queues, mailboxes, run queue, per-actor timer lists, free lists |
| `ta_heap` | 4-ary min-heap of `{due:u64, seq:u32, item:u32}` + back-index for O(log n) removal (`<cancel>`). | timer/delayed-send queue |
| `ta_memory` | `ta_memcpy/memset/memmove/memcmp` mapping to builtins; freestanding fallback. | everything |
| `ta_journal` | Lossy ring of variable-size records: reserve/commit, peek/release, a refused reservation is counted (`lost`), never waits and never overwrites. Self-relative storage ⇒ relocatable with `memcpy`; `ta_journal_copy` moves the records into storage of another size. Knows nothing about what the records mean. | the system's trace records (ADR 0006) |
| `ta_format` | `ta_format_u64` (decimal) and `ta_text`, a bounded writer of views and numbers that records an overflow instead of truncating: what `snprintf` would do, without the C library. | session ids and addresses, generated invoke ids, the host's message ids |
| `ta_allocator` | `{context, alloc(context, size, align), free(context, ptr, size)}` vtable (`tinyactors/allocator.h`). **Never used by the core or the system during execution.** | the host (all of its memory), compiler, runtime convenience constructors |

Design rule: **no pointer is ever stored in mutable system memory**, except
opaque datamodel handles (which are `uint64_t` and may be indices anyway).
Consequences: snapshots are `memcpy` and relocation/growth is `memcpy` + header
fix-up.

---

## 3. Image format (Chart IR + System IR)

### 3.1 One container, not two

A "chart blob" is just a **system image with one chart and no boot actors**.
There is one format (`.tai`), one verifier, one loader. Reason: event-name ids
must be **system-wide** so that a cross-actor send never re-resolves names;
that forces a system-level symbol space anyway.

### 3.2 Container

```
ta_image_header (64 B, 8-aligned)
  magic "TAI\0", version, flags (endianness=LE always, index width)
  total_size, content_hash (64-bit; snapshots record it)
  section_count, section_table_offset
section_table[n]: { kind:u32, off:u32, size:u32, count:u32 }
sections...      (each 8-aligned, all references are u32 offsets/indices)
```

All integers little-endian. `ta_image_open((ta_bytes){bytes, length}, &view)` bounds-checks
every offset/index/range once (fuzzed), so untrusted images are safe and the
hot path does no checks. The view is a stack/host struct of section base
pointers; the blob itself is never written (ROM/flash/mmap/`PROGMEM`-able,
and the *same* blob may be shared by many systems and threads).

### 3.3 System-level sections

| Section | Content |
|---------|---------|
| `STRINGS` | All strings, NUL-terminated, deduplicated. Referenced by `u32` offset. |
| `EVENT_TRIE` | Global event-name trie (§5) in preorder: `end:u32`, `name_str:u32`, child lookup table. |
| `CHARTS` | Chart table: `{sections of that chart, instance_size, workspace_size, datamodel_id, name}` |
| `DATAMODELS` | Names of datamodels used (`"ecmascript"`, `"null"`, `"python"`, …) + optional AOT section per (datamodel, chart). |
| `IOPROCS` | Known I/O processor types (`scxml` built in; host-provided ones by name/URI). |
| `ACTORS` | Boot actor definitions: `{chart, name, params}` spawned by `ta_system_boot()`. |
| `LINKS` | Static invoke graph: `(chart, invoke def) → child chart`. Inline `<content><scxml>` children are compiled as separate charts. |

### 3.4 Per-chart sections

States are numbered in **document order** (preorder), so `descendants(s)` is
the contiguous interval `(s, s.end)`. This single fact removes most runtime
hierarchy traversal.

```c
/* 24 bytes */
typedef struct ta_state_record {
  uint32_t parent;        /* TA_NIL for <scxml> root */
  uint32_t end;           /* one past last descendant (preorder interval) */
  uint8_t  kind;          /* atomic, compound, parallel, final, history_shallow, history_deep */
  uint8_t  depth;
  uint16_t flags;         /* HAS_EVENT_T, HAS_EVENTLESS_T, HAS_INVOKE, HAS_DATA, HAS_HISTORY, ... */
  uint32_t transitions;         /* first transition (document order); count via next state's */
  uint32_t onentry_instruction;    /* bytecode block, TA_NIL if none; onexit follows the same pattern */
  uint32_t aux;           /* index into STATE_AUXILIARY: initial/default entry list, history slot,
                             invokes, data declarations, donedata, name string */
} ta_state_record;

/* 24 bytes */
typedef struct ta_transition_record {
  uint32_t source;
  uint32_t domain;        /* precomputed transition domain; TA_NIL if targetless */
  uint32_t event_lo, event_hi;  /* single descriptor as trie preorder interval; multi-
                             descriptor transitions point into EVENT_RANGES (flag) */
  uint32_t cond;          /* ta_expression or TA_NIL */
  uint32_t entry;         /* offset of sparse sorted entry list (static part) */
  /* flags (internal, eventless, multi_descriptor, history_targets) are packed into
     the top bits of `entry`; action_instruction and target list live in TRANSITIONS_AUX */
} ta_transition_record;
```

Other per-chart sections: `TRANSITIONS_AUX` (targets, action instruction), `EVENT_RANGES`,
`ENTRY_LISTS` (sparse sorted `u32` state lists), `HISTORY` (slot offsets,
default transitions), `CODE` (bytecode, §8), `EXPRS` (expression table,
§9.2), `SENDS`, `INVOKES`, `DATA`, `PARAMS`, `DONEDATA`, `STATE_NAMES`
(sorted for `In('name')` resolution by datamodels at attach time), `DEBUG`
(optional line/column map; strippable).

### 3.5 Precomputation done by the compiler

- Document order, preorder intervals, depth, max depth.
- **Transition domain** for every targeted transition (targets are static in
  SCXML, so this is always possible).
- **Static entry list** per transition and per default initial completion
  (compound initial, parallel children) — the closure of `addDescendantStatesToEnter`
  / `addAncestorStatesToEnter` that does not pass through a history state.
  Transitions whose closure reaches a history pseudo-state are flagged; their
  entry set is completed at runtime from recorded history (bounded explicit
  stack, depth ≤ max depth, in the workspace).
- Event descriptors → trie intervals.
- Chart-level masks: `eventless_states` bitset (skip eventless selection with
  one AND), `invoking_states`, `history_states`.
- `max_foreach_depth`, `max_entry_list`, workspace size, instance size.
- Static `<send>` targets classified (§10), static delays parsed to ticks,
  static `src` inlined, `#_<invokeid>` resolved to invoke slot.

### 3.6 Exit sets and conflicts need no stored tables

- `exitSet(t) = config ∩ (domain(t), domain(t).end)` — a range-masked AND on
  the configuration bitset. Targetless transitions exit nothing.
- Preorder intervals are either nested or disjoint. For two *targeted*
  transitions the exit sets are both non-empty (each domain strictly contains
  an active state), so **they intersect iff the domain intervals are nested**:
  an O(1) integer test. `removeConflictingTransitions` therefore runs on
  integers, applying the spec's preemption rule (a transition whose source
  is a descendant of the other's source wins; otherwise the earlier one in
  document order is kept).
- Entry: OR the static entry list into `entry_set`, then complete history
  targets from `instance.history` if flagged.

### 3.7 Memory cost (image)

For a 100-state / 200-transition chart: states 2.4 KB, transitions 4.8 KB,
entry lists ≈ 1–3 KB, bytecode typically 1–4 KB, plus strings. ≈ 10–15 KB,
shared by any number of instances.

---

## 4. Mutable state: instance, actor, workspace

### 4.1 Instance (owned by the core, chart-dependent size)

```c
typedef struct ta_instance_header {   /* 32 bytes */
  uint32_t chart;
  uint8_t  phase;              /* parked phase (§7) */
  uint8_t  flags;              /* STARTED, IN_MACROSTEP, DONE, EVENT_BOUND, ... */
  uint16_t chart_cursor;       /* small cursor for chunked init/teardown */
  uint32_t current_event;          /* _event pin: event-pool index or TA_NIL */
  uint32_t internal_queue_head, internal_queue_tail;   /* internal queue (links live in event records) */
  uint32_t workspace;                 /* workspace index or TA_NIL */
  uint64_t datamodel;                 /* datamodel instance handle (opaque) */
} ta_instance_header;
/* followed by, at offsets from the chart's layout record:
   u32 config[ceil(state_count/32)]
   u32 to_invoke[ceil(invoking_states_count/32)]       states entered this macrostep
   u32 data_init[ceil(states_with_data_count/32)]      late binding only
   u32 history[sum over history states of ceil(subtree/32)]
   u32 invoke_child[invoke_definition_count]                  child actor handle or 0 */
```

`ta_instance_size(chart)` is a field of the chart record. A 64-state chart
with 2 history states and 2 invokes is **≈ 64 bytes**.

### 4.2 Actor record (owned by the system, fixed 48 bytes)

```c
typedef struct ta_actor {
  uint32_t next;          /* run-queue / free-list link */
  uint8_t  state;         /* FREE, IDLE, RUNNABLE, WAIT_WORKSPACE, WAIT_EXTERNAL, DONE */
  uint8_t  generation;
  uint16_t flags;         /* CANCELLED, HAS_PARENT, ... */
  uint32_t instance;          /* slab offset of the instance */
  uint32_t mailbox_head, mailbox_tail;   /* external mailbox (event-pool links) */
  uint32_t parent;        /* parent actor handle or 0 */
  uint32_t parent_slot;   /* parent's invoke slot index */
  uint32_t timers;        /* head of this actor's delayed-send list */
  uint32_t pending_request;   /* outstanding request or 0 */
  uint32_t name;          /* _name (image string or slab string) */
  uint64_t serial;        /* monotonic; _sessionid = decimal(serial) */
} ta_actor;
```

Actor (48 B) + instance (~64 B) ≈ **112 bytes per idle actor**, plus
whatever the datamodel keeps. Beside the actor pool, per actor of
`max_actors`: its links (16 B: its place in the ring of actors of its
name, how many of its requests are live, a word that holds the actor
before it in the wake queue while it is hibernated there, and which
hibernation its latest blob is, so a wake refuses an older one) and the index of names (a
slot per name, 4 B, at most 4/5 full: 5–10 B). The index answers
`ta_system_actor_by_name` with the oldest live actor of a name in O(1)
expected; the count of requests lets hibernation and export check a stable
point without walking the requests.

### 4.3 Workspace (pooled, held only inside a microstep)

```c
typedef struct ta_workspace_header {
  uint32_t instruction;            /* bytecode PC in current block */
  uint32_t cursor;        /* bit index in the set being walked */
  uint16_t foreach_top;         /* foreach stack depth */
  uint8_t  sub;           /* sub-phase within a microstep */
  uint8_t  pad;
  uint32_t request;           /* outstanding external request, if any */
} ta_workspace_header;
/* followed by:
   u32 transition_set[W_t]        enabled transitions of this microstep
   u32 exit_set[W_s], entry_set[W_s], default_entry[W_s]
   u32 history_default[W_h]     history default content to run
   u32 dynamic_stack[max_depth]  history-entry completion
   ta_foreach_frame foreach_frames[max_foreach_depth]  {u64 iteration; u32 i, n; u32 body_instruction;} */
```

One size per system (max over charts), K slots, K from config (default 4).
100-state chart: ≈ 120 bytes per workspace.

### 4.4 What lives where

| Lifetime | Storage |
|----------|---------|
| immutable, shared by all instances/systems | image |
| persists across macrosteps (configuration, history, data-init, invokes) | instance |
| persists across microsteps inside one macrostep (`_event` pin, internal queue, `to_invoke`, parked phase) | instance header + event pool |
| lives only inside one microstep (sets, cursors, PC, foreach frames) | pooled workspace |
| actor identity, mailbox, scheduling, timers | actor record + system pools |
| values | datamodel (host) |

---

## 5. Event names and matching

**Representation.** The compiler collects every event *descriptor* and every
*static* event name (raise/send `event=`, platform names such as
`done.state.<id>`, `done.invoke.<id>`, `done.invoke.<stateid>` prefixes for
generated ids, `error.execution`, `error.communication`, `error.platform`)
into one **token trie** (`.`-separated tokens; `foo.*` and `foo.` normalize
to `foo`; `*` is the root). Nodes are numbered in preorder and store `end`.

- A **descriptor** is the interval `[node, node.end)`.
- An **event** carries `name_id` = preorder number of the *deepest trie node
  that is a token-prefix of its name*, plus `name_str` (`TA_NIL` when the
  name equals that node exactly, else a slab string for `_event.name`).
- **Match** = `lo <= name_id && name_id < hi`. One compare pair. This is
  exactly SCXML token-prefix semantics (`foo` matches `foo.bar`, never `foobar`).

**Dynamic names** (`eventexpr`, host-injected events) are resolved once when
the event is *created* (walk tokens down the trie; child lookup by binary
search over sorted token strings). Cost is paid once per event, not per actor
or per transition. Names unknown to the trie map to the deepest known prefix
(often the root) and still match `*`/prefix descriptors correctly.

**Hot path.** For each active atomic state in document order (bitset scan),
walk `self → ancestors` (depth ≤ max_depth, parent array), scan the state's
contiguous transition range: interval test, then `cond` via the datamodel.
Already-selected transitions are deduplicated via `trans_set`. States
without `HAS_EVENT_T` are skipped by flag.

---

## 6. Event representation and event pool

One system-wide `ta_pool` of 40-byte records:

```c
typedef struct ta_event {
  uint32_t next;          /* intrusive link: mailbox, internal queue, timer, free */
  uint16_t refs;          /* queue/timer membership + _event pin */
  uint8_t  type;          /* platform | internal | external */
  uint8_t  flags;         /* NAME_DYN, FROM_INVOKE, ... */
  uint32_t name_id;       /* trie preorder id */
  uint32_t name_string;      /* TA_NIL or slab string */
  uint32_t sendid;        /* tagged: TA_NIL | image string | slab string */
  uint32_t origin;        /* tagged: actor handle | slab string (external) */
  uint32_t invokeid;      /* tagged: image string | slab string */
  uint16_t origintype;    /* io_processor index */
  uint16_t invoke_slot;   /* receiver-side invoke slot (finalize/autoforward/filter) */
  uint64_t payload;       /* datamodel payload handle or 0 */
} ta_event;
```

- An event is in **at most one queue** at a time (SCXML sends are unicast), so
  one intrusive link suffices. Autoforward creates a *new* record sharing the
  payload (`retain`).
- `refs` counts queue/timer membership plus the `_event` pin. On zero:
  `payload_release`, slab strings released, slot freed.
- **Reserved slots**: R slots (config, default 2 per workspace) are reserved
  for platform error events, so `error.*` can always be delivered even when
  the pool is exhausted by the very condition being reported.
- Memory: 40 B × pool size; 4 096 events = 160 KB, independent of actor count.

---

## 7. Continuation machine (the core)

### 7.1 Phases

The parked phase (instance header, no workspace needed) is one of:

| Parked phase | Meaning |
|---|---|
| `PHASE_INIT` | not started; next run binds data (early: all `<data>`; late: the top-level `<data>`, as the document is entered — nested `<data>` on its state's first entry) + initial entry |
| `PHASE_EVENTLESS` | between microsteps: try eventless transitions next |
| `PHASE_INTERNAL` | eventless set empty: dequeue next internal event next |
| `PHASE_STABLE` | macrostep finished; waiting for an external event (actor IDLE) |
| `PHASE_EXTERNAL` | an external event has been bound, select its transitions next |
| `PHASE_DONE` | top-level final reached; teardown complete |

Active sub-phases (inside a microstep, workspace held, `workspace.subphase`):

`SUBPHASE_SELECTED → SUBPHASE_HISTORY_RECORD → SUBPHASE_EXIT(cursor) → SUBPHASE_ONEXIT(instruction) →
SUBPHASE_UNINVOKE(cursor) → SUBPHASE_TRANSITION_CONTENT(cursor, instruction) → SUBPHASE_ENTER(cursor) →
SUBPHASE_DATA_INIT(cursor) → SUBPHASE_ONENTRY(instruction) → SUBPHASE_HISTORY_DEFAULT(instruction) → SUBPHASE_DONE_EVENTS → SUBPHASE_END`

`S_END` releases the workspace and parks in `PHASE_EVENTLESS`.

### 7.2 Step contract

```c
ta_step ta_core_step(ta_core_context *core, ta_instance_header *instance, ta_fuel *fuel);
/* returns one of:
   TA_STEP_YIELD        fuel exhausted; call again later (state preserved)
   TA_STEP_STABLE       macrostep complete; deliver next external event
   TA_STEP_SEND         core->out.send describes a send; caller routes, calls again
   TA_STEP_CANCEL       cancel delayed send (sendid)
   TA_STEP_INVOKE       start invoke (def, params payload); caller fills child handle
   TA_STEP_UNINVOKE     cancel invoke slot
   TA_STEP_REQUEST      external response needed (srcexpr, custom action)
   TA_STEP_NEED_WORKSPACE      no free workspace; caller parks actor in WAIT_WORKSPACE
   TA_STEP_DONE         top-level final; donedata payload in core->out
   TA_STEP_FAULT        unrecoverable (corrupt image, pool exhaustion w/o reserve) */
```

`ta_core_deliver(core, instance, event_index)` binds an external event (only legal in
`PHASE_STABLE`). Because sends are *returned*, the kernel never touches mailboxes,
routing, timers or other actors: it is genuinely standalone, and testable
with a scripted driver.

### 7.3 Fuel

Deterministic units charged by the kernel: 1 per transition candidate test,
1 per state exited/entered, 1 per bytecode instruction, plus whatever the
datamodel reports through `ta_datamodel_environment_charge()` for expensive evaluations.
Checks happen at chunk boundaries (one state, one instruction), never inside
a datamodel call: a datamodel call is the unit of atomicity. A datamodel that
itself loops forever is a host bug outside SCXML's model (bindings can impose
their own interpreter budgets, e.g. QuickJS interrupt handler).

### 7.4 Native stack

No recursion anywhere in core/system: all hierarchy walks are interval/bitset
scans or bounded explicit stacks in the workspace. Max native stack is a
small constant (target < 1 KB on 32-bit), verified by `-fstack-usage` in CI.

---

## 8. Executable-content bytecode

32-bit words. `opcode:8 | argument:24`, followed by 0–3 operand words. Blocks (onentry,
onexit, transition content, finalize, history default, each `<data>` init)
are contiguous and end in `END`. Structured control flow is flattened at
compile time:

| Op | Operands | Semantics |
|----|----------|-----------|
| `END` | | block finished |
| `RAISE` | name_id | enqueue internal event |
| `SEND` | send_def | evaluate attributes via datamodel, capture payload, return `TA_STEP_SEND` |
| `CANCEL` / `CANCEL_EXPRESSION` | sendid string / expr | return `TA_STEP_CANCEL` |
| `ASSIGN` | loc, expr | `datamodel->assign` |
| `ASSIGNC` | loc, content | assign inline content |
| `SCRIPT` | expr | `datamodel->script` |
| `LOG` | label, expr | `datamodel->log` (datamodel formats; system emits `TA_EFFECT_LOG`) |
| `JF` | cond, target | `if`/`elseif`: jump if false |
| `JMP` | target | end of an `if` arm |
| `FOREACH_BEGIN` | arr, item, index, exit | push frame `{iter, i=0, n}`; jump to exit if `n==0` |
| `FOREACH_NEXT` | body | `i++`; if `i<n` assign item/index, jump body; else pop |
| `CUSTOM` | custom_def | host-defined executable content (sync via datamodel, or `TA_STEP_REQUEST`) |

**Error rule:** any datamodel error ⇒ enqueue `error.execution`, set
`foreach_top = 0`, jump to the block's `END` (SCXML: remaining elements of the
block are skipped). **Continuation = `instruction` + foreach frames**; nested `if`
adds nothing. Yield is possible between any two instructions.

---

## 9. Datamodel interface

### 9.1 Principles

The datamodel owns **values and expression semantics**; the core owns **when**
each operation happens and **what SCXML state** it may observe. The interface
is the minimum set of semantic operations SCXML requires — not a value API.

### 9.2 Expression table

`ta_expression` (u32) indexes the chart's `EXPRS` section:
`{kind, src_off, src_len, line, col}` with kinds `COND`, `VALUE`, `LOCATION`,
`SCRIPT`, `STRING` (for `*expr` attributes), `CONTENT`. The datamodel decides
what an id means (bytecode, closure, switch case, …).

### 9.3 vtable (synchronous, hot)

```c
typedef struct ta_datamodel_vtable {
  uint32_t size; uint32_t flags;      /* TA_DATAMODEL_EVENT_DETACHABLE, ... */
  const char *name;                   /* matched against the image's datamodel name */
  /* chart / instance lifecycle */
  ta_status (*chart_attach)(void *datamodel, const ta_datamodel_chart *chart, void **chart_context);
  void      (*chart_detach)(void *datamodel, void *chart_context);
  ta_status (*create)(void *datamodel, void *chart_context, const ta_datamodel_system_variables *variables,
                      uint64_t init_payload, uint64_t *instance);   /* invoke params override <data> */
  void      (*destroy)(void *datamodel, uint64_t instance);
  /* semantic operations (environment gives read-only access: In(), event view, names) */
  ta_status (*init_data)(void *datamodel, uint64_t instance, ta_datamodel_environment *environment, uint32_t data_declaration);
  ta_status (*bind_event)(void *datamodel, uint64_t instance, ta_datamodel_environment *environment, const ta_event_view *event);
  void      (*detach_event)(void *datamodel, uint64_t instance);
  ta_status (*eval_condition)(void *datamodel, uint64_t instance, ta_datamodel_environment *environment, ta_expression e, int *out);
  ta_status (*eval_string)(void *datamodel, uint64_t instance, ta_datamodel_environment *environment, ta_expression e, ta_string_view *out);
  ta_status (*assign)(void *datamodel, uint64_t instance, ta_datamodel_environment *environment, ta_expression location, ta_expression value);
  ta_status (*assign_string)(void *datamodel, uint64_t instance, ta_expression location, ta_string_view v); /* idlocation */
  ta_status (*script)(void *datamodel, uint64_t instance, ta_datamodel_environment *environment, ta_expression e);
  ta_status (*log)(void *datamodel, uint64_t instance, ta_datamodel_environment *environment, ta_expression e, ta_string_view *out);
  ta_status (*foreach_begin)(void *datamodel, uint64_t instance, ta_datamodel_environment *environment, ta_expression array,
                             uint64_t *iteration, uint32_t *n);
  ta_status (*foreach_item)(void *datamodel, uint64_t instance, uint64_t iteration, uint32_t i,
                            ta_expression item, ta_expression index);
  void      (*foreach_end)(void *datamodel, uint64_t instance, uint64_t iteration);
  /* payloads: namelist + <param> + <content>, also donedata and invoke params */
  ta_status (*capture)(void *datamodel, uint64_t instance, ta_datamodel_environment *environment,
                       const ta_datamodel_capture *spec, uint64_t *payload);
  void      (*retain)(void *datamodel, uint64_t payload);
  void      (*release)(void *datamodel, uint64_t payload);
  ta_status (*custom)(void *datamodel, uint64_t instance, ta_datamodel_environment *environment, uint32_t custom_definition);
} ta_datamodel_vtable;
```

`ta_status` is `TA_OK`, `TA_ERROR_EXECUTION` (core raises `error.execution`),
`TA_ERROR_FATAL`. View lifetimes: strings returned by `eval_string`/`log` are
valid until the next call on that datamodel instance.

### 9.4 System variables stay under SCXML control

- `_sessionid`, `_name`, `_ioprocessors` are passed **once** at `create` as
  views (`ta_datamodel_system_variables`); the datamodel must expose them read-only and
  report assignments to them as `TA_ERROR_EXECUTION` (covered by the W3C IRP
  system-variable tests).
- `_event` is *only* changed by `bind_event`, called by the core exactly when
  SCXML says the current event changes (§7, scenarios 10–11). Before the first
  event `_event` is unbound (the datamodel never received `bind_event`).
- `In(state)` resolves through `ta_datamodel_environment_in(environment, state_id)`; names → ids are
  resolved at `chart_attach` from `STATE_NAMES`.
- The `null` datamodel (SCXML §B.1) is built into the core.

---

## 10. Built-in SCXML I/O processor (in the system)

Targets are classified **at compile time** whenever static:

| Target class | Representation | Route |
|---|---|---|
| none | `T_SELF` | own external mailbox |
| `#_internal` | `T_INTERNAL` | own internal queue (kernel handles directly, never leaves core) |
| `#_parent` | `T_PARENT` | `actor.parent` handle |
| `#_<invokeid>` (static id) | `T_INVOKE(slot)` | `instance.invoke_child[slot]` |
| `#_scxml_<sessionid>` | `T_SESSION` | serial → actor (open-addressing map, only used for this form) |
| `targetexpr` | `T_EXPR(expr)` | evaluated once, parsed once into one of the above |
| anything else / non-SCXML `type` | `T_EXTERNAL(io_processor)` | `TA_EFFECT_SEND` effect to host |

Errors per spec: unsupported `type` or malformed target ⇒ `error.execution`;
unreachable SCXML session ⇒ `error.communication` (to the sender's internal
queue, using a reserved event slot). `#_internal` with delay is rejected at
compile time when static. Local routes **enqueue** into the target's mailbox
and mark it runnable; they never execute the target.

---

## 11. Host ↔ system ABI

### 11.1 Three channels, kept separate

```diagram
         host                                      ta_system
┌──────────────────┐  commands (sync C calls)  ┌──────────────────┐
│ event loop       │ ────────────────────────▶ │ send / send_many │
│                  │                           │ run(budget)      │
│                  │ ◀──────────────────────── │ advance_time     │
│                  │  tagged outcome + effects │ resume(req,res)  │
├──────────────────┤                           ├──────────────────┤
│ datamodel impl   │ ◀── vtable (hot, sync) ── │ ta_core          │
│                  │ ── env queries (r/o) ───▶ │                  │
└──────────────────┘                           └──────────────────┘
```

### 11.2 Function set (≈25)

```c
/* image */
ta_status ta_image_open(ta_bytes bytes, ta_image *out);
uint32_t  ta_image_chart_by_name(const ta_image *, ta_string_view name);
/* system lifetime: one caller-owned block */
ta_status ta_system_layout(const ta_image *, const ta_system_config *, size_t *bytes);
ta_status ta_system_init(const ta_system_setup *, ta_system **out); /* block, image, config, datamodels */
ta_status ta_system_relocate(ta_system **, ta_memory new_memory, const ta_system_config *new_config);
void      ta_system_deinit(ta_system *);       /* releases datamodel instances/payloads */
/* actors */
ta_status ta_system_boot(ta_system *);       /* spawn image-defined actors */
ta_status ta_system_spawn(ta_system *, const ta_spawn *, uint32_t *actor); /* chart, name, params */
ta_status ta_system_terminate(ta_system *, uint32_t actor);
/* inbound */
ta_status ta_system_send(ta_system *, const ta_event_in *event);
uint32_t  ta_system_send_many(ta_system *, const ta_event_in *events, uint32_t n);
ta_status ta_system_resume(ta_system *, const ta_resume *); /* request, status, result */
/* execution */
ta_run    ta_system_run(ta_system *, uint32_t budget);      /* tagged outcome */
void      ta_system_advance_time(ta_system *, uint64_t now);
uint64_t  ta_system_next_deadline(const ta_system *);         /* UINT64_MAX = none */
/* outbound (batched, borrowed) */
uint32_t  ta_system_effects(ta_system *, const ta_effect **out);
void      ta_system_effects_ack(ta_system *, uint32_t n);
/* trace records (ADR 0006): drained like effects, same lifetime rules */
uint32_t  ta_system_trace(ta_system *, ta_trace_view *view);     /* bytes of whole records */
void      ta_system_trace_ack(ta_system *, uint32_t bytes);
void      ta_system_trace_clock(ta_system *, uint64_t at_least);  /* one clock for many systems */
ta_status ta_system_trace_set(ta_system *, const ta_trace_policy *policy); /* classes, ratio, rules (ADR 0007) */
/* per-chart counts (ADR 0007): turns, macrosteps, fuel, errors, faults, actors */
ta_status ta_system_chart_stats_get(const ta_system *, uint32_t chart, ta_chart_stats *out);
/* inspection / snapshots */
ta_status ta_actor_info(const ta_system *, uint32_t actor, ta_actor_info *out);
uint32_t  ta_actor_config(const ta_system *, uint32_t actor, ta_uint32_array states);
ta_string_view    ta_image_state_name(const ta_image *, uint32_t chart, uint32_t state);
ta_status ta_system_snapshot(const ta_system *, ta_buffer *out);
ta_status ta_system_restore(const ta_system_setup *, ta_bytes snapshot, ta_system **out);
```

### 11.3 Lifetime rules (the only ones)

1. The image bytes must outlive every system using them (`ta_system_setup.image`
   is borrowed, so an embedder can run an image from flash or a mapped file
   without a copy). The host layer keeps its own copy of each segment's
   image instead (`ta_host_add_image`, ADR 0011).
2. `ta_string_view`/payload views **into the system** (effects, event views, actor
   info) are valid until the next *mutating* call on that system (`send*`,
   `run`, `resume`, `advance_time`, `effects_ack`, `spawn`, `terminate`).
3. Views **from the host** (`ta_event_in` strings) are copied (into the slab)
   or interned before the call returns. Payload handles passed in are
   **moved** into the system.
4. Payloads in effects are **borrowed**; the host calls the datamodel's
   `retain` if it keeps one.

### 11.4 Outcomes and effects

```c
typedef struct ta_run {
  uint32_t status;        /* TA_RUN_IDLE, TA_RUN_YIELDED, TA_RUN_EFFECTS_FULL,
                             TA_RUN_EXHAUSTED, TA_RUN_FAULT */
  uint32_t effect_count;     /* pending in outbox */
  uint32_t used;          /* fuel consumed */
  uint32_t detail;        /* pool id / fault code */
  uint64_t next_deadline;
} ta_run;

typedef struct ta_effect {   /* fixed layout, size-prefixed */
  uint32_t size, kind;       /* TA_EFFECT_SEND, TA_EFFECT_REQUEST, TA_EFFECT_INVOKE, TA_EFFECT_UNINVOKE,
                                TA_EFFECT_LOG, TA_EFFECT_DONE, TA_EFFECT_ERROR */
  uint32_t actor, token;     /* token: request id / invoke token / send token */
  uint32_t io_processor, flags;    /* NEEDS_RESPONSE, ... */
  ta_string_view   target, event, sendid, aux;
  uint64_t payload;
} ta_effect;
```

Effects accumulate in a bounded outbox ring during `run`; `run` returns early
with `TA_RUN_EFFECTS_FULL` only when the ring is full (natural backpressure).
One FFI crossing drains the whole batch.

Requests (`NEEDS_RESPONSE`): dynamic `srcexpr` for invoke/data, custom actions
declared asynchronous. Fire-and-forget: external sends, external invoke
start/cancel, log, done. External invokes report back via
`ta_system_send` with `ta_event_in.invoke_token` set (and a
`TA_EVENT_IN_INVOKE_DONE` flag), so the system can generate
`done.invoke.<id>`, apply `finalize`, and drop events from cancelled invokes.

### 11.5 FFI notes

Only `u32`/`u64`/pointer/`{pointer, length}` structs cross the boundary; no
varargs, no bitfields, no enums in structs (enums are `uint32_t`), no
callbacks except the datamodel vtable — which JNI/.NET/Go/Swift bindings
typically implement *in C* (e.g. a QuickJS or Lua datamodel) or as a thin
trampoline. WebAssembly exports the same functions; effects arrays are read
straight out of linear memory.

---

## 12. Scheduler, mailboxes, timers (in the system)

**Actor states:** `IDLE` (stable, empty mailbox, not queued) · `RUNNABLE` (in
run queue) · `WAIT_WORKSPACE` (needs a workspace) · `WAIT_EXTERNAL` (outstanding
request) · `DONE` (awaiting reclamation) · `FREE`.

**Run queue:** intrusive FIFO through `actor.next`. `WAIT_WORKSPACE` actors queue on
a second intrusive FIFO and are woken (FIFO) when a workspace is released.

**`ta_system_run(budget)`:**

```
fire due timers (deliver into mailboxes, wake targets)
while budget > 0 and run queue not empty:
    a = pop(run queue); q = min(QUANTUM, budget)
    loop:
        r = ta_core_step(a, &q)
        SEND/CANCEL/INVOKE/UNINVOKE → handle locally (route/enqueue/spawn), continue
        REQUEST   → emit effect, a.state = WAIT_EXTERNAL, break
        NEED_WS   → a.state = WAIT_WORKSPACE, break
        STABLE    → if mailbox nonempty: deliver head (+autoforward, +invoke filter), continue
                    else a.state = IDLE, break
        YIELD     → push_tail(run queue), break        (no event needed to wake it)
        DONE      → done.invoke to parent / TA_EFFECT_DONE, reclaim, break
    budget -= used
return IDLE (with next deadline) | YIELDED | EFFECTS_FULL | ...
```

Two budgets: `QUANTUM` (config, fairness between actors) and the `budget`
argument (fairness between the system and the host). `run` *always* returns
within `budget + max_chunk` fuel.

**Mailboxes:** each is `{head, tail}` in the actor record; events live in the
shared pool. Enqueue = 2 stores. No per-actor queue capacity; the only bound
is the shared pool (fairness against a flooding sender can be added with an
optional per-actor cap in config).

**Timers:** `ta_pool` of 24-byte records `{event, target route, sendid,
owner, owner_next, heap_pos}` + `ta_heap` keyed `(due, seq)`. `<cancel>`
walks the **owner's** intrusive timer list (usually 0–3 entries) comparing
tagged sendids, then removes from the heap in O(log n). Delays are host ticks;
the image stores static delays in ns and the system converts with
`ticks_per_second` from config. Payload is captured **at send time**; the
route is resolved **at fire time** (target may have terminated ⇒
`error.communication` to the owner if alive).

---

## 13. Memory allocation strategy

1. **Image:** caller bytes; never copied, never written.
2. **System region:** exactly one caller block. `ta_system_layout()` runs the
   carving code in measure mode; `ta_system_init()` runs it for real. Region
   contents: header, actor pool, instance slab, event pool, timer pool + heap,
   workspace pool, string slab, request pool, outbox ring, session map. No
   allocation ever happens after init.
3. **Datamodel:** host-owned values, host-owned allocation.
4. **Compiler:** `ta_allocator` + chunked arenas; frees everything at once.
5. **Growth:** because the region holds only indices and self-relative
   offsets, `ta_system_relocate()` copies it into a larger block with larger
   capacities. The runtime layer does this automatically on `TA_RUN_EXHAUSTED`.
6. **Host:** a nonempty `ta_host_effects` take freezes the published
   pointer and count until the ack. One private pointer per host tracks
   the published array; replaced arrays and pools are held until the ack.
   Effects appended during a drain wait for the next batch. The ack closes
   publication before freeing blocks or calling payload release hooks.
   `ta_host_run` refuses an open batch without doing work or releasing it.
   A `ta_host` gets every block — its segments' regions, its
   tables, its effect copies — from the `ta_allocator` in its
   configuration, and gives each back with its size. When the allocator
   refuses, a call returns `TA_ERROR_NO_MEMORY` with the host unchanged and
   `ta_host_run` returns `TA_RUN_EXHAUSTED`; the next run resumes where it
   stopped, so a bounded allocator (a fixed arena, a database's memory
   context) is backpressure, not failure. What the host writes for its
   caller (telemetry, a hibernation blob) goes into
   memory it grows the same way; the caller gets a view of it, valid until
   the next `ta_host_effects_ack` (ADR 0012; until that change lands, until
   its next call into the host, ADR 0010). Replies are effects (ADR 0011).

Sizing example — 100 000 actors of a 64-state chart, 4 workspaces,
16 384 events, 4 096 timers:
actors 4.8 MB + instances 6.4 MB + events 640 KB + timers ≈ 160 KB +
workspaces < 1 KB + slab (configurable) ≈ **12 MB** total, no per-actor
heap objects.

---

## 14. Invocation machinery

- `<invoke>` defs per state in `INVOKES`: `{type, src chart | srcexpr, id |
  idlocation, autoforward, params/namelist, content, finalize_pc}`.
- On entry the state's bit is set in `instance.to_invoke`; at macrostep end
  (`PHASE_STABLE` reached) the kernel walks `to_invoke ∧ config` in document order
  and returns `TA_STEP_INVOKE` per def (SCXML: invokes start only for states
  entered and not exited during the macrostep).
- SCXML-type invoke: the system spawns the child chart (static index from
  `LINKS`, or after a `srcexpr` request), with `parent`/`parent_slot`, and
  stores the child handle in `invoke_child[slot]`. Params become the child's
  `init_payload` (overrides matching `<data>` ids).
- Invoke ids: static strings from the image; generated ids are
  `"<stateid>.<serial>"` stored once in the slab (and in `idlocation`).
- On exit of the invoking state: `TA_STEP_UNINVOKE`; system terminates the
  child through the kernel's bounded teardown phase (resumable like any other
  phase; **no** `done.invoke` is generated), cancels its timers, drains its
  mailbox, and sets `invoke_child[slot] = 0`.
- Events from children carry `invoke_slot` + origin handle; at delivery the
  system drops those whose origin no longer equals `invoke_child[slot]`
  (events from cancelled invocations must be ignored).
- Delivery from an invoked child runs `finalize` (kernel sub-phase before
  selection, `_event` already bound); `autoforward` copies the external event
  into the child mailbox at dequeue time.
- Child reaching top-level final: `done.invoke.<id>` with donedata payload
  enqueued in the parent mailbox **after** any events the child sent earlier
  (same FIFO ⇒ ordering preserved).

---

## 15. Implementation status and deviations from this design

All six layers are implemented (`src/{base,image,core,system,compile,runtime}`)
and tested (`mise run test`). With the quickjs-ng ECMAScript datamodel
(`src/datamodel_ecmascript`) **all 191 automatic W3C IRP tests pass** — 158
mandatory and 33 optional, including BasicHTTP via `src/basic_http` and a
loopback host (`ta-conform`'s `w3c.run`, `tools/conform/w3c.c`; `mise run conform`). Where the implementation differs
from the sections above, this list is authoritative:

| Topic | As built |
|---|---|
| Image tables (§3.4) | Per-chart records live in *global* sections; each chart record holds base/count per table and records use chart-local indices. Same properties (one blob, offsets only, verified once). |
| Entry sets (§3.5, C10) | Computed at runtime by an iterative `addDescendant/addAncestor` with an explicit stack in the workspace (`4·state_count+16` words). Static entry-list precomputation is a future optimization; exit sets and conflict detection are interval-based as described. |
| Workspace liveness (C3) | Two rules make the bounded pool live *and* fair: (1) **fuel overrun** — when fuel runs out inside a microstep the kernel may finish it on up to `quantum/2+16` units of debt (charged to the run) and yields at the boundary with the workspace released; (2) an instance resumed while holding a workspace yields again at its next microstep boundary. Long executable content still suspends mid-microstep (scenario 9). Measured: 3 loopers on 2 workspaces progress 2310/2310/2310. |
| Forced termination | `ta_system_terminate` and parent cancellation take effect at the next **microstep** boundary (not only at stability), so a runaway eventless loop can be stopped. A cancelled child runs `exitInterpreter` (onexit) and produces no `done.invoke`. |
| Cancelled invocations | Events of a still-running child are purged from the parent's mailbox when the invocation is cancelled; a child that already finished keeps its queued events (incl. `done.invoke`) deliverable (W3C test 236). |
| `_event` view lifetime | Strings in `ta_event_view` are valid only during `bind_event`; `data` stays valid until the next bind. `origin` of SCXML sessions is formatted on demand (`#_scxml_<serial>`); internal events have no origin. |
| Requests | `TA_EFFECT_REQUEST` is used for dynamic `<invoke srcexpr>` that names no chart in the image; answer with `ta_system_resume` and `{.request = token, .status = TA_OK, .result = chart_index + 1}`. The actor is **not** blocked meanwhile (invoke is asynchronous). `srcexpr` values `file:x.scxml` / `./x.scxml` resolve to linked charts named `x.scxml`. |
| Dynamic documents | Images are immutable, so `<invoke><content expr>` resolves the document *text* against SCXML documents that appear literally in the image (e.g. inside `<assign>`/`<data>`), which the compiler precompiles under their whitespace-normalized text; anything else becomes a host request. |
| BasicHTTP / I/O | a send that leaves the system parks its sender (`TA_ACTOR_WAIT_EXTERNAL`, `TA_EFFECT_FLAG_WAITING`) until the effect is acknowledged, so the host can answer within the sender's macrostep while the other actors run (`ta_host_run` reports `TA_RUN_EFFECTS` while a sender waits); `TA_EVENT_IN_ERROR_COMMUNICATION` places `error.communication` in the sender's **internal** queue; `ta_event_in.raw` becomes `_event.raw`. |
| Effect field mapping | `LOG`: `auxiliary`=label, `event`=text. `SEND`: `target`, `event`, `sendid`, `payload`, `io_processor`. `INVOKE`: `event`=type, `target`=src, `sendid`=invokeid, `payload`=params, `token`. `DONE`: `target`=actor name, `payload`=donedata, `auxiliary`=id of the top-level `<final>` that ended the session. `REQUEST`: `auxiliary`=src, `token`. Views are materialized when `ta_system_effects` is called. |
| Exhaustion | Event/slab **and timer** exhaustion makes the kernel return without executing the instruction (retried after growth) — a full timer pool is backpressure (`TA_RUN_EXHAUSTED`, `detail = TA_POOL_TIMERS`), never a dropped `<send delay>`. The `TA_POOL_*` codes are public (`tinyactors/system.h`). `ta_system_deinit` drops the run queues before reclaiming, so tearing down hundreds of thousands of runnable actors is linear. Actor/slab exhaustion while spawning an invoked child parks the spawn as a pending request; `run` returns `TA_RUN_EXHAUSTED` and retries it first next time. `ta_runtime_run` grows by relocation automatically. |
| Memory | Actor record 48 B; instance = 64 B header (with the step counters and the current macrostep's trace context, ADR 0006) + configuration and to-invoke bitsets (+ late-binding / history bitsets only when the chart needs them) + history storage + 2 words per invoke — 56 B for a 2-state chart. Slab classes are half-steps (16, 24, 32, 48, …). Measured: 100 000 actors ⇒ 14.3 MB region (143 B/actor including all pools). |
| Cross-system SCXML | The system's base URI (`ta_system_config.base`, e.g. `tinyactors://a/`) makes sessions advertise `<base>#_scxml_<id>`; SCXML sends to the own base are delivered locally, other non-`#` SCXML targets become `TA_EFFECT_SEND` for the SCXML processor (`io_processor` 0); the host delivers them with `ta_system_actor_by_session()` and an `origin` so replies via `_event.origin` work. Used by the Node binding's segments and the torture chamber. |
| Datamodel `bind_event` | Receives a `ta_datamodel_environment*` so the datamodel can observe the configuration when an event becomes current (e.g. which finals produced `done.state.*`). |
| Metrics | [ADR 0007](../../docs/adr/0007-metrics-and-trace-policy.md). Each system keeps a `ta_chart_stats` per chart in the region (104 B on 64-bit hosts): actors resident and hibernated, spawned, ended, turns, macrosteps, fuel, errors by type, faults. Turns, macrosteps and fuel move once per scheduling turn in `run_actor`, errors where the kernel raises them, the rest on lifecycle changes: nothing per event or per actor. The host sums them by chart name, keeping retired segments' totals (the `host.stats` method, docs/requests.md). The otlp codec writes ExportMetricsServiceRequests from stats (`ta_otlp_add_stats`) or plain metrics (`ta_otlp_add_metric`), and, with `TA_OTLP_SPAN_METRICS`, per-chart histograms of the macrostep spans it writes, with exemplars (`ta_otlp_add_span_metrics`). `ta_host_metrics` writes a host's counters as one request on demand, with no timer or effect. |
| Inspection counters | `ta_system_stats_get()` reports pool occupancy (actors, events, timers, workspaces, slab pages, outbox), runnable count and monotonic counters (delivered events, fuel, turns, timers fired, sessions) in O(1) (the run queue keeps its length). `ta_runtime_system_grow()` doubles the pools that are at least half full (all if none is), saturating at the hard per-system limits `TA_SYSTEM_MAX_ACTORS` (2^23 − 2: 24-bit handles, one bit lent to external invoke tokens) and `TA_SYSTEM_MAX_POOL` (2^24 − 2); it fails instead of "growing" without adding capacity. |
| ECMAScript heap metric | `ta_datamodel_ecmascript_heap_bytes()` is maintained by the datamodel's counting allocator (O(1)); `JS_ComputeMemoryUsage` walks every object and took ~1 s per call at 80k sessions. |
| ECMAScript realms | By default (`TA_DATAMODEL_ECMASCRIPT_REALM_AUTO`) all sessions of a datamodel share one JS context whose global object is frozen; a session is a null-prototype scope object (<data>, `_event`, read-only system variables), and expressions compile once per chart to strict closures inside `with (scope)`. Assignments must start at a session variable (else `error.execution`); `In()` resolves through the evaluating session. Charts whose `<script>`s declare top-level names stay isolated (one context per session), as does everything under `TA_DATAMODEL_ECMASCRIPT_REALM_ISOLATED`. What a script declares is asked of the engine when the chart is attached: in a scratch context the script is instantiated without running (`throw 0;` ahead of it — hoisting happens, nothing executes); its var/function names are then on the scratch global object and its let/const/class names in their temporal dead zone (the source only proposes candidate identifiers). Blocks, nested functions and IIFEs therefore stay shared. A context costs ~54 KB (QuickJS has no heap snapshot/clone to share it), a scope ~0.5 KB: the `fit` session actor went from 60.7 KB to 2.9 KB. W3C: 191/191 in both modes (234 of 238 charts shared). |
| Dynamic spawn | Not in the library: SCXML's processor only reaches existing sessions (§6.2.4, App. C.1); sessions come from `<invoke>` or the host. The torture bench provides a `system` I/O processor (§6.2.5 allows extra processors) that spawns sessions on a chart's request. |
| Compiler cost | Linear in the documents' total size, with bounded native stack: XML namespaces are scoped with a prefix map (no ancestor walks), states are collected with an explicit stack, event-trie children are found by their dotted prefix in a hash map, generated ids resume their search, and the image verifier checks jumps in one pass (pending forward targets, a `<foreach>` body stack) and entry points with ascending cursors. Limits: 200 state levels, `TA_COMPILE_MAX_CONTENT_NESTING` (64) `<if>`/`<foreach>` levels (the only recursion), 65,535 descriptors or targets per transition, 64 dot-separated parts per event name (the image stores every prefix's full name), 200 kept diagnostics. `ta_compile_options.abort_flag` (polled per token, state, transition and executable element) stops a compile with `TA_ERROR_ABORTED`; the PostgreSQL binding passes `&InterruptPending`. `tests/unit/test_compile_scale.c` fails if any pathological family stops scaling linearly. |
| Null datamodel `_event` | The null datamodel declares `TA_DATAMODEL_EVENT_DETACHABLE`: `_event` is unobservable there, so stable actors pin no pooled event (100k idle actors hold no events). |
| Null datamodel | Supports `In('id')` conditions and quoted string literals in `<log expr>`. |
| Durable state | One inventory of a session's durable state and what each path out of a running system and back (hibernate/wake, a migration's export → `ta_blob_migrate` → adopt, system and host snapshots, the datamodels' serialize/snapshot) keeps of it; none of them is durability, and snapshots are for inspection and for taking actors out of memory for a while: [durability.md](durability.md). `tests/unit/test_durable.c` checks every path against a run that never left memory. Generated ids (`idlocation`) count per session (`ta_instance.ids`), not per system. |
| Hibernation | Host-driven (§16.1): `ta_actor_hibernate` / `TA_EFFECT_WAKE` / `ta_actor_wake` / `ta_system_cold_actors`; the system never does I/O. Torture `hibernate`: 2 M ECMAScript sessions with a few thousand resident (see tools/torture/README.md). |
| Bindings | None: [ADR 0009](../../docs/adr/0009-one-host-two-queues.md) deleted the Node.js, web and PostgreSQL bindings, the link protocol and the examples; each binding is regenerated as a queue loop over `ta_host`, a datamodel or none, and a CBOR module. The W3C suite (all 191) and the scenarios of the conformance suite (`tinyactors-test`) run through the C host, in `ta-conform` (`tools/conform`). |
| Snapshot/restore | `ta_system_snapshot` requires a drained outbox; `ta_system_restore` takes an optional config whose host pointers (I/O processors) are re-bound; capacities come from the snapshot. `ta_host_snapshot`/`ta_host_restore` do the same for a whole host (every segment, the moved-session map, the clock; quiescent: effects acknowledged, no migration). Datamodel state stays the datamodel's: regions refer to instances and payloads by handle, so a datamodel restores its tables at the same handles before the first run — `ta_datamodel_ecmascript_snapshot/_restore` (one `JS_WriteObject` graph: sessions, payloads, `<foreach>` items in progress), the JS bindings' value tables (`system.snapshot()` → bytes, `createSystem({ restore })`). Tested as a real restart (fresh host and datamodel) in `test_host` and the conformance suite's `snapshot` scenario. |
| Host layer | `ta_host` (`src/host`, freestanding: memory only through the `ta_allocator` of its config, ADR 0005) is the policy the bindings and the torture bench used to duplicate: segments (one `ta_system` per image, run from a copy of the image's bytes the host owns and frees with the segment) sharing a base URI, segment k owning sessions k·2³²+1…; cross-segment SCXML routing inside the library; versions = the newest segment per chart name (CGI-style reload, retirement → `TA_HOST_EFFECT_SEGMENT_FREED`); the control plane (`ta_host_post`: CBOR messages in JSON-RPC 2.0's shape, answered by `REPLY` effects, and `NOTIFICATION` effects, none defined yet, docs/requests.md); migrations advanced one quantum per `ta_host_run` (busy actors retried, hibernated ones woken or waited for, a timeout); the CLOCK sweep across segments; growth by relocation; boot of system images; snapshot/restore; a migrated session is routed to in the segment that took it, and a segment added later at its home index starts past its id. Each run shares its fuel fairly among runnable segments (a runaway segment cannot starve the others). |
| Remote targets | With a base, only absolute URIs (RFC 3986 scheme) leave the system; any other target the SCXML processor does not know is invalid → `error.execution` (W3C 159, 194). A malformed session id (`#_scxml_foo`) names no session anywhere → `error.communication` at once, in the internal queue (W3C 496, 521); a well-formed unknown one may live in another segment and is handed to the host (whose answer arrives as an external event). |
| Tracing | [ADR 0006](../../docs/adr/0006-tracing-is-a-core-feature.md). The event record stays 56 B: `invoke_slot` and `flags` are 16 bits, and the 4 bytes they gave up hold `trace`, a reference into the context pool (48 B entries, `max_events` of them by default; a full pool leaves the new event untraced and counts it). The instance header holds `macrostep`, `microstep` and the macrostep's context. Records are an 88 B header and a body in the system's journal (`ta_journal`, 128 KiB by default whatever `max_events`, at least 64 KiB; lossy: a refused record is counted and a `dropped` record precedes the next one). Strings and payloads in records are retained until `ta_system_trace_ack`. Every record carries the system's fuel clock; a run moves it up by one before it works, and the host maps fuel to wall time by interpolating between the wall times given to `ta_host_advance_time` (`ta_otlp_mark`). `TA_TRACING_DISABLED` carves neither journal nor pool. The host keeps one fuel clock for all its segments (`ta_system_trace_clock`) and hands a send's traceparent to other systems (`TA_EFFECT_FIELD_TRACEPARENT`); the `trace.configure` request changes the policy everywhere. A context's flags hold a detail class above the W3C byte (bits 8–10); the policy (`ta_trace_policy`, ADR 0007) gives each of 8 classes its kinds, and rules compiled into the region decide again where a trace starts and where an actor takes an external event (one branch each with no rules); the kernel tests the union of the classes' kinds, then the step's class. Unsampled traces keep a context in class 0 (their traceparent goes out with flag 00). |

## 16. Inspection and reconstruction

- `ta_actor_info`: state, chart, phase, `in_macrostep`, parent, mailbox depth,
  pending request, current event name id.
- `ta_actor_config`: active state ids in document order (or raw bitset).
- Names via image string tables; no strings are stored per actor.
- **Snapshots**: whole-system bytes = region copy + image hash
  (`ta_system_snapshot`), or a whole host (`ta_host_snapshot`: every
  segment, the moved-session map, the clock). Datamodel state is snapshotted
  by its owner and restored at the same handles (instances and payloads are
  referenced by handle): `ta_datamodel_ecmascript_snapshot/_restore` for the C
  datamodel, the value tables for the JS bindings. Snapshots taken
  mid-macrostep are valid (continuations are data — a suspended `<foreach>`
  keeps its item list in the datamodel snapshot), which is what makes
  database-resident actors practical.

### 16.1 Hibernation (host-driven unloading)

Idle actors can leave RAM and come back, without the library doing I/O and
without changing SCXML semantics. The host owns the policy and the storage.

```
 resident actor (~2.9 KB, ECMAScript)          hibernated actor (~150 B)
 ┌──────────────────────────────────┐          ┌───────────────────────────┐
 │ actor record 48 B, session entry │          │ actor record (state       │
 │ instance (bitsets, invoke words) │ ───────▶ │ HIBERNATED, inst = chart) │
 │ datamodel state (JS scope)       │   blob   │ session entry, timers,    │
 │ pinned _event                    │ ───────▶ │ mailbox                   │
 └──────────────────────────────────┘  to host └───────────────────────────┘
```

- `ta_actor_hibernate(system, actor, &buffer)` (the host's `ta_host_hibernate`
  writes the same blob in one pass into memory it grows): only at a stable point
  (idle, empty mailbox and internal queue, no workspace, no invoked children —
  they read the parent's invoke slots — no pending request). Writes
  `header | instance words | invoke ids (text) | datamodel bytes` and frees
  the instance, the pinned `_event` and the datamodel state. Region references
  are never part of a blob (invoke ids are re-interned on wake).
- Datamodels opt in with `serialize`/`deserialize` (appended to the vtable,
  `size`-checked). System variables are not serialized: `deserialize` gets
  them from the system again. The null datamodel stores nothing; the
  ECMAScript datamodel writes a shared-realm session's scope (its `<data>`
  and `_event`) with `JS_WriteObject` (object graphs, cycles, Map/Set/Date).
  An isolated session writes its global state: the global object properties
  the chart made (`<data>`, `var`, implicit globals, with their attributes),
  its global `let`/`const`/`class` bindings (value, const-ness, or still
  uninitialized — found by probing the analysed names: `delete name` is false
  only for a lexical binding) and the names of its top-level function
  declarations. A function is stored only as such a declaration — identified
  by its source position (`<script:N>`, line, column) or as re-created
  before — and unchanged (no added properties); on restore the declaration is
  re-run (strict if its script is) and the bindings re-declared with their
  values. Other functions, classes, Errors, Promises… → `TA_ERROR_UNSUPPORTED`,
  and `ta_datamodel_ecmascript_last_error()` names the variable, the path inside it
  and the document line of a function. A blob written for another chart
  version restores lexical bindings as plain properties (the migration drops
  what the new chart does not declare) and re-runs the new chart's
  declarations of the stored function names.
- Anything that needs a hibernated actor — an event from an actor, a timer, the
  host, a `<cancel>`led invocation, `ta_system_terminate` — leaves the work where
  it already lives (mailbox, flags) and puts the actor on an intrusive **wake
  queue**, linked both ways (an actor woken or reclaimed before its effect
  leaves it in O(1)); `ta_system_run` turns it into one `TA_EFFECT_WAKE` effect (one per
  hibernation; the queue survives a full outbox). The host loads the blob and
  calls `ta_actor_wake`; the actor then processes its mailbox in order. A
  host-reported `error.communication` (`TA_EVENT_IN_ERROR_COMMUNICATION`) for a hibernated
  actor is parked in the mailbox and moved to the internal queue on wake, so it
  is still handled before external events.
- `ta_system_cold_actors(system, &cursor, max_scan, out, capacity)` is a CLOCK sweep for
  the host's eviction policy: one "ran since the last sweep" bit per actor
  (set when it runs, spawns or wakes), cleared by the sweep; actors found
  without it, and hibernatable, are reported. A sweep never laps the ring.
- Hibernated actors keep 48 B of record + session-table entry + their timers
  and mailbox; `stats.hibernated/wakes_pending/hibernations/wakes` count them.
  Snapshots and relocation carry them unchanged (the blobs stay with the host).

### 16.2 Hot reload and live migration (host-driven)

A chart version is a new image, so a new version of a system is a new
`ta_system`. The library makes versions cooperate; the host decides when.

```
 tinyactors://w/  (one base URI, TA_SYSTEM_SHARED_BASE)
 ┌───────────────────────┐  export   ┌──────────────┐  adopt   ┌───────────────────────┐
 │ version 1             │ ────────▶ │ export blob  │ ───────▶ │ version 2             │
 │ sessions 1, 2, 3 ...  │           │ + timers     │ migrate  │ sessions 2^32+1 ...   │
 │ (first_serial = 0)    │           │ + name       │ ───────▶ │ + adopted session 2   │
 └───────────────────────┘           └──────────────┘          └───────────────────────┘
        ▲  send to #_scxml_2: not here → TA_EFFECT_SEND → host → version 2
```

- **Addresses survive versions.** `ta_system_config.first_serial` gives each
  version its own session range; with `TA_SYSTEM_SHARED_BASE` a send to an own-base session that is not here becomes a
  `TA_EFFECT_SEND` (full URI) instead of `error.communication`. The host knows
  which version has it, or reports `TA_EVENT_IN_ERROR_COMMUNICATION` — SCXML semantics are
  unchanged.
- **CGI style**: new actors start in the newest version, running ones finish
  on theirs, a version without actors is freed (`ta_system_actors` and
  `stats.actors` tell). Nothing is migrated unless the host asks.
- **`ta_actor_export`** = hibernate + leave, for a root actor and the
  sessions it invoked (its session tree): every session idle at a stable
  point with an empty mailbox and no pending request; host-run (external)
  invocations cannot move. The export blob is a header and one record per
  session in preorder: its state (as hibernation writes it), its pending
  delayed sends (absolute due ticks on the shared clock, their rank in the
  tree's firing order, route, target/name/sendid as text, the payload as a
  value written by the datamodel's `payload_serialize` — a handle only for
  datamodels without it, valid in the writing process), its name, and its
  parent's session id and invoke slot — never handles (a delayed send to
  `#_<invokeid>` stores the owner's slot). What a session's durable state is
  and which path keeps which part: [durability.md](durability.md).
  The records are freed silently. The tree is
  walked without a stack through the parent links (preorder to write,
  post-order to remove). `ta_system_adopt` validates the whole tree first,
  adopts parents before children, re-links slots and timers, and rolls back
  children-first on failure. `ta_blob_migrate` translates a blob of one
  session and refuses larger trees (`TA_ERROR_UNSUPPORTED`); `ta_host`
  migrates such an actor once its invocations have finished
  (`ta_actor_info.invocations`).
- **`ta_blob_migrate`** (pure) translates the blob to another chart: states by
  id; the mapped set plus ancestors is completed top-down to a legal
  configuration (compound without an active child → its initial targets,
  history defaults resolved; parallel → all regions; extra children dropped);
  history recordings kept only if every stored state still exists under the
  same parent; late-binding flags follow their states; states entered by the
  translation get their `<invoke>`s started (`to_invoke`); `<data>` ids the old
  chart did not declare are listed for initialization (late binding: only of
  active states).
- **`ta_system_adopt`** recreates the actor with the blob's session id (refused
  if that session exists), re-arms the timers, and for a migrated blob calls
  the datamodel's optional `migrate` hook (returns the previous bindings as a
  payload and drops variables the new chart does not declare), initializes
  the new `<data>` (`error.execution` on failure) and raises
  **`error.platform.upgrade`** — `error.platform` is the family SCXML §3.12.2
  reserves for platform events — whose `_event.data` are the previous
  `<datamodel>` bindings, then makes the actor runnable: the upgrade is
  processed before any mailbox event. A failed adoption changes nothing, so
  the host can adopt the original blob back where it came from.
- The ECMAScript datamodel's bindings are an object `{ id: value }` of the old
  scope, in the C datamodel and in the JS bindings' native one. The torture scenario `reload` exercises all of it under
  traffic (no lost or duplicated jobs, counts continuous across versions,
  timers kept, old versions freed); `tests/unit/test_migrate.c` covers the
  translation cases.


### 16.3 Definition logs

The host writes one OTLP log record when a chart version changes, through
its encoder like every other log (`TA_HOST_EFFECT_LOGS`, `ta_host_logs`;
`src/host/definitions.c`). They come only from `ta_host_add_image` and from
the run that frees a segment, never per event; a host without telemetry
(the null encoder) writes none, and neither does one whose config says
`"logs": false`.

| Event name | Severity | When |
|---|---|---|
| `tinyactors.definition.linked` | DEBUG | `ta_host_add_image`: once per chart of the new image |
| `tinyactors.definition.retired` | INFO | the same call, for the chart of the same name it replaces: the newest older segment that has it |
| `tinyactors.definition.freed` | DEBUG | the run that frees a segment no longer current and without actors: once per chart of its image |

Each record's body reads `linked pinged (segment 1)`,
`retired pinged (segment 0, 3 actors)` or `freed pinged (segment 0)`, its
time is the wall time of the last `ta_host_advance_time`, and its
attributes are:

| Attribute | Type | Value |
|---|---|---|
| `tinyactors.chart.name` | text | the chart's name (its source when it has none) |
| `tinyactors.image.hash` | text | the image's content hash, 16 hex digits: tells two versions apart |
| `tinyactors.segment` | int | the segment the version lives in |
| `tinyactors.chart.actors` | int | retired only: the actors still on the old version, which finish there or migrate |

A staging with no room drops a definition log, as it drops any record it
cannot hold, and makes the logs due at once.

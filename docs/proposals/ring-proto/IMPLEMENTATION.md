# Implementation: replacing core's plumbing with building blocks

**Status:** In progress (2026-10-06), part of [the ring protocol proposal](README.md),
adopted as core ADRs 0098 to 0105, repository ADR 0003 and napi ADR 0041.
The work is organised as four [milestones](#milestones). Phases 0 to 3
and step 4.1 to 4.3 are done; the root [ROADMAP](../../../ROADMAP.md#the-ring-protocol)
tracks the milestones.

This plan replaces [migration.md's "rough order of
work"](reference/migration.md#a-rough-order-of-work). Its block names
(A5, B1, C3, ...) come from the catalog in
[reference/building-blocks.md](reference/building-blocks.md).

## The approach: keep the code base, replace the plumbing

Core is **not** rewritten from scratch. It keeps its code, and the parts
that change are replaced module by module, as building blocks. The reasons:

- **About half of core is SCXML semantics that the proposal does not
  change.** That half is the compiler, definitions and the matcher, the
  data model glue, session images, `scxml_uri`, the atom table and the
  trie, addresses and sessions, and export and codec. It encodes the
  conformance edge cases that the W3C IRP tests and the test host's
  scenarios check.
- **What changes is concentrated.** Timers, event blocks, I/O origins,
  the vtables and the out-of-memory stop are replaced or deleted as
  whole modules.
- **The hard work is the same either way.** That work is in the
  interpreter: slots and parking, the chain limit, startup after the
  macrostep, mandatory storage, and the rule that a macrostep never
  allocates. It builds on the resumable phases and `ta_continuation` that
  exist today for pause.
- **Every commit stays green.** AGENTS.md asks for small commits on
  `main`, each passing `mise run test`. Only an incremental replacement
  can keep the existing suites as the reference throughout.

## Milestones

The plan began as seven phases built around the kernel: core gained its
queue while the kernel kept napi working, the rings replaced the vtables
while the kernel followed, and the kernel went last. On 2026-10-06 the
kernel (`tinyactors-host`), the test host's hosted mode and napi's native
layer (`tinyactors-napi/src`) were deleted, and napi's tests disabled,
because adapting them at every step cost more than rewriting napi on the
rings at the end. What is left is a dependency order in core, a napi
rewrite and a finale, as four milestones:

| Milestone | Contents | Done when |
|---|---|---|
| **M1: core runs inputs safely** | The rest of [Phase 4](#phase-4-core-gains-its-queue): 4.3c, 4.4, 4.5 | No allocation inside a macrostep; slots and parking work; core's tests and the test host's scenarios pass, IRP 183/183 |
| **M2: core speaks only rings** | [Rings, effects, definitions, loading, recording](#m2-core-speaks-only-rings) (the former 5.1 to 5.5) | No vtable left in core but the data model's, the value vtable, byte sinks and sources and the chart codec; the test host drives core only through the rings; IRP 183/183 and S01 to S13 pass as ring scripts |
| **M3: napi on the rings** | [A rewrite from scratch](#m3-napi-on-the-rings) of napi's native layer | `lib/` works; napi's suites and IRP 176/176 pass on Node and Bun, with `// status:` markers where it lags |
| **M4: finale** | [Verification, size, benchmarks, tuning](#m4-finale) | The figures are recorded and the regressions accepted are named |

Phases 0 to 3 and the Phase 4 table below are kept as the record of what
was done and why. Until M3, **the test host is the only host**: core's
tests and the test host's scenarios are the oracle, and napi's suites,
`lib/`, `test/` and `index.d.ts` wait unchanged as the rewrite's
specification.

## Desired source tree

This is `tinyactors-core` after Phase 7. `src/` stays flat, because the
amalgamation joins `src/*.c` in the order `src/internal.h` lists them;
`internal.h` changes with every row marked `+` or `-`. The tree is grouped
by role here, not alphabetically. Line counts are today's (`.c` + `.h`).
Block IDs refer to [building-blocks.md](reference/building-blocks.md), and
`step` refers to the phases below.

Every unit includes the public header itself and the units it uses. No
catch-all header such as `types.h` remains: each unit is named after the
one object it defines.

Legend:

- `+` new
- `-` removed
- `~` changes
- `=` unchanged, or only follows renames
- `→` renamed or moved into

```
tinyactors-core/
├── include/
│   ├── tinyactors.h            ~  4060  ta_run, h2c/c2h/tel access and the control block; loses vtables
│   │                                    (router, I/O, invoker, directory, trace, log), deliver/resume/
│   │                                    dispatch_due, register_io/_invoker, origins and scopes, TA_OP_NOMEM,
│                                        TA_ERR_BUSY, TA_DM_TEXT_IO_*, run-time intern entry points;
│                                        gains the ring records, so a host includes one header, the clock
│                                        function, ta_system_trim; ta_actor_inspect marks stable or active
├── protocol/                   +        moved from docs/proposals/ring-proto/protocol (2.9)
│   ├── dsl.rb                  +        the DSL
│   ├── catalog.rb              +        every record, defined once
│   └── render.rb               +        writes everything in src/generated/
└── src/
    │
    │   ── foundation: generic, no knowledge of SCXML ──────────────────────────────────────
    ├── types.c / types.h       -   282  a catch-all; split into one unit per object (1.0):
    ├── memory.c / memory.h     +        A1 helpers: ta_mem_alloc/_free/_copy/_zero, alignment, TA_ARRAY_COUNT
    ├── str.c / str.h           +        ta_str equality and construction from a C string
    ├── vec.c / vec.h           +        B6 ta_vec and TA_VEC_LAYOUT
    ├── bitset.h                +        B5 ta_bits_*, inline
    ├── span.h                  +        ta_span32: offset and length into an array or a record tail
    │                                    (compiled definitions, record spans)
    ├── text_writer.c / .h      +        D3 the bounded writer and render; D6 decimal, hex, percent-encoding;
    │                                    targets for a chunk chain and a record tail
    ├── version.c               +        ta_version
    ├── hash.h                  +        D5 FNV-1a and the 64-bit mixer, once (1.1)
    ├── handle.h                +        E1 generation handles: make, slot, generation, valid (1.2)
    ├── layout.c / layout.h     +        A2 layout builder with checked arithmetic (1.4)
    ├── arena.c / arena.h       =   199  A3; optional A4 mark/rewind
    ├── index_pool.c / .h       +        A5 fixed items in pages, safe-point refill, ceiling, trim (1.5)
    ├── chunks.c / chunks.h     +        A6 chunk chains over an index pool (2.4)
    ├── list.h                  +        B1 intrusive index lists, inline (2.1)
    ├── paged_array.c / .h      +        B8 dense array in power-of-two pages; grow and shrink at safe points (2.2)
    ├── heap.c / heap.h         +        B2 indexed min-heap, its positions in a B8 paged array (2.2)
    ├── table.c / table.h       +        B3 hash table, fixed or growing on an explicit call (1.6)
    ├── fifo.c / fifo.h         +        B4 fixed-entry FIFO (2.3)
    ├── quota.h                 +        C3 quota with reservations, inline (2.3)
    ├── claim.c / claim.h       +        C4 claim set, all or nothing (2.10, optional)
    ├── bytes.c / bytes.h       +        D1 byte sinks and sources: memory, arena, chunk chain (2.5)
    ├── serial.c / serial.h     +        D2 typed binary writer and reader, extracted from session_image (2.5)
    ├── ring.c / ring.h         +        C1 SPSC byte ring: reserve, pad, commit, release, lossy mode (2.8)
    ├── generated/              +        everything the protocol renderer writes; never edited by hand (2.9)
    │   ├── ring_records.h      +        record structs, kinds, enums, flags, size asserts
    │   ├── ring_codec.c        +        C2 writers and validating readers
    │   ├── messages.json       +        the catalog as data, for the test host's decoder
    │   └── MESSAGES.md         +        the catalog as a reference document
    ├── tree.c / tree.h         =   217  spec tree builder
    ├── ta_allocator_wasm.c     =   117  wasm allocator (platform file)
    │
    │   ── names and types ───────────────────────────────────────────────────────────────
    ├── atom.c / atom.h         ~   157  on B3; interning only at registration and creation
    ├── event_name.c / .h       →        trie.c / trie.h
    ├── trie.c / trie.h         ~   263  D7 trie of atoms, on B3; event names are one user (1.6)
    ├── names.c / names.h       +        D4 resolver (bytes → atoms, UNKNOWN) (2.6, 4.2)
    ├── type_registry.c / .h    +        B7, created twice: I/O processor and invoker types (2.7, 5.2)
    ├── address.c / address.h   ~   296  keeps its linear hash table (ADR 0065 decision 6); no run-time
    │                                    interning from targetexpr
    ├── session.c / session.h   =   270  keeps its linear hash table (ADR 0065 decision 6)
    ├── scxml_uri.c / .h        ~   414  parses and formats; looks up, never interns at run time
    ├── routing.c / routing.h   ~    77  keeps the actor_ref helpers; the router comment and boundary go
    │
    │   ── the queue: inputs core has not run yet ───────────────────────────────────────
    ├── envelope.c / .h         +        the pool (A5), its lists (B1), recycling, withdrawal; the trace parent
    │                                    in the envelope's text when traced (4.2); waiting entries (A5), one per
    │                                    address with the address once, found through a fixed B3 table (5.3)
    ├── name_area.c / .h        +        an A5 + A6 instance with 64-byte chunks, plus its quota; the text layout (4.2)
    ├── admission.c / .h        +        reads h2c: admit, bounce, apply controls, fairness (4.2, 5.1)
    ├── deadline.c / .h         +        delayed sends: B2 over envelopes, owner lists, exact <cancel> (4.3)
    ├── timer.c / timer.h       -   634  replaced by deadline.c (4.3)
    │
    │   ── execution ──────────────────────────────────────────────────────────────────────
    ├── run.c / run.h           +        ta_run: fill slots, take turns, drain phases, return status (4.2, 4.5)
    ├── slots.c / slots.h       +        slot records (A5), deferred lists, slotSlice, the chain limit (4.5)
    ├── park.c / park.h         +        the park area: A5 + A6 with 256-byte chunks, D2 continuations (4.5)
    ├── interpreter.c / .h      ~  4858  the SCXML algorithm stays; loses event blocks, the out-of-memory stop,
    │                                    queue mark/truncate, two-part reservations; the internal queue becomes
    │                                    B4 plus ordinary and mandatory quotas; capacity errors (4.2–4.5)
    ├── continuation.c / .h     ~   227  stays for pause; written to the park area through D2 (4.5)
    ├── configuration.c / .h    ~    56  one layout, words of bits; the kind enum goes (1.8)
    ├── history.c / history.h   =    32
    ├── invoke.c / invoke.h     ~  1294  startup after the macrostep, lifetime tokens, invocation_seq;
    │                                    c2h.invoke/_cancel/_forward instead of the invoker vtable (4.4, 5.2)
    ├── event.c / event.h       ~    42  name and origin as text, not an event-name ID or an io_origin
    ├── fault.c / fault.h       -    33  declarations never defined, and what only they used (1.8)
    ├── clock.c / clock.h       =    11
    │
    │   ── the host boundary ──────────────────────────────────────────────────────────────
    ├── c2h.c / c2h.h           +        c2h writer: mandatory reserves (C3), one pad per reserved record, the
    │                                    2 × maxRecordBytes headroom, and every core → host record (5.1, 5.2)
    ├── trace.c / trace.h       ~   464  writes self-contained tel.trace records stamped by the host clock
    │                                    instead of calling a sink (4.1)
    ├── io.c / io.h             -   232  processors live in the binding; origins are text (5.2)
    ├── io_origin.c / .h        -   262  COMPUTED, SCOPED and MESSAGE storage (5.2)
    ├── store.c / store.h       -    10  empty compatibility module (1.8)
    │
    │   ── sessions, definitions, the system ───────────────────────────────────────────────
    ├── actor.c / actor.h       ~  2000  loses event-block types, held effects, deliver and BUSY; gains list heads,
    │                                    the recorded bit and the deferred list (4.2–5.5)
    ├── session_image.c / .h    ~  1600  on D1 and D2; no event blocks; pending timers from the owner list (4.3, 5.3)
    ├── definition.c / .h       ~   960  descriptors as trie paths, mandatory-event bounds, per-definition
    │                                    chain limit (4.4)
    ├── builder.c / builder.h   ~  2746  sizes mandatory storage; checks platform events are representable (4.4)
    ├── export.c / export.h     =   636
    ├── codec.c / codec.h       =   531
    ├── datamodel.c / .h        ~   249  _event text from envelopes; TA_DM_TEXT_IO_* go; no TA_OP_NOMEM (4.4, 5.2)
    ├── null_datamodel.c / .h   ~   297  answers TA_OP_ERROR instead of TA_OP_NOMEM (4.4)
    ├── system.c / system.h     ~  1140  creates rings, the control block, pool, name area, park area and workspace
    │                                    through A2; ta_system_trim; loses the registries, sinks and the
    │                                    event-block cache
    └── internal.h              ~    33  module order follows the groups above
```

Annotations welcome directly in the tree. Names marked `+` are
suggestions.

## Where the documentation goes

The proposal's documents are good enough to be the reference for the
implementation, so they move into the packages, one by one, as the code
they describe lands. The proposal directory itself stays as history, with
status *Adopted → links*, like the [replay persistence
proposal](../replay-persistence.md).

Each document goes to the place its readers already look:

- **The source** takes everything that explains one unit: its contract,
  invariants, lifetimes, safe points and the reasons for its shape. It
  goes into the unit's header as a comment
  ([below](#documentation-lives-with-the-code)). Most of
  `building-blocks.md` and much of `envelope.md`, `ring.md` and
  `macrostep.md` land there.
- **Core's internal reference** (`tinyactors-core/docs/internal/`) keeps
  what crosses units: the memory model, the invariants, event ordering,
  the macrostep, and the building-block catalog as an index of links to
  headers. Where a row below sends a document to `docs/internal/`, the
  part about one unit goes to that unit's header instead.
- **Core's manual** (`tinyactors-core/docs/manual/`) is for embedders. It
  takes what a host author needs: the host loop, capacity and failure,
  parameters, and the walkthroughs.
- **The test host** proves behaviour (core's `docs/README.md`). The
  scenarios become executable ring scripts there, and their charts become
  its fixtures.
- **ADRs** take every decision, and the **root `ROADMAP.md`** takes the
  steps of this plan.

### Documentation lives with the code

The reference documents carry knowledge the code needs, and a reader of
the code should find it beside what it explains. Every step writes it as
part of the step.

- **Each unit's header opens with a doxygen-style file comment**
  (`/** @file ... */`, Markdown prose): the one object the unit defines,
  its contract, its invariants, who owns its memory and for how long,
  when it may allocate (only at a safe point, or never in a macrostep),
  its complexity, how it fails, and the ADRs behind it.
- **Each function a unit exports has a `/** ... */` comment:** what it
  does, what it borrows or takes over, whether it may run in a macrostep,
  and what it answers on failure. Tags only where they help (`@param` for
  a parameter whose meaning is not obvious). The existing `/* */` prose
  style stays; the comments are doxygen-compatible so that a generated
  index is possible, not required.
- **The comment is written in the commit that adds or changes the unit,**
  from the matching proposal text: a block's row and notes in
  `building-blocks.md`, the envelope fields in `envelope.md`, the reserve
  rules in `ring.md`. A step is not done until its units say what the
  proposal said about them.
- **Existing per-module documents fold in as steps touch them.**
  `docs/internal/` has one document per module today (`arena.md`,
  `atom.md`, `timer.md`, ...). The first step that changes a unit moves
  that document's description of the unit into its header, and the
  document is deleted or reduced to what crosses units. `module-map.md`
  becomes an index of headers.
- **A check keeps it true.** Step 1.0 adds an ast-grep rule, with a test
  (`mise/ast-grep/rules/`), that every header in `tinyactors-core/src/`
  opens with a file comment. Headers that predate it are on an allow-list
  that shrinks as steps touch them; `mise run test` runs the rule.
- **Rendering is optional.** A `docs:api` task may render the headers
  with a pinned doxygen; it is not a gate.

### From the proposal

Legend:

- `→` moves to, rewritten as reference
- `⇢` split between several homes
- `▶` becomes code or tests, and the prose follows it
- `◼` stays here as history

```
docs/proposals/ring-proto/
├── README.md                   ◼        status → "Adopted → links"; the index of where everything went
├── IMPLEMENTATION.md           ⇢        steps → root ROADMAP.md (Phase 0); rules → AGENTS.md where general;
│                                        this file stays as history
├── ARCHITECTURE.md             ⇢        layers → root README.md "Layers and responsibilities" (6.3);
│                                        the host loop, the life of an input and of a send, time
│                                        → core docs/manual/hosting.md (5.1)
├── BEHAVIORS.md                ⇢        who drops what, full resources, what a chart must do
│                                        → core docs/manual/capacity-and-failure.md (4.4);
│                                        crashes → core docs/manual/persistence.md (5.5);
│                                        parameters → core docs/manual/parameters.md (4.2, grows per step)
├── charts/*.scxml              ▶        tinyactors-test/ruby/charts/ as fixtures (5.1)
│   └── README.md               →        tinyactors-test/ruby/charts/README.md (5.1)
├── scenarios/S01–S13           ▶        tinyactors-test/ruby/scenarios/ring_*.rb, one per scenario,
│                                        test names carrying the step IDs (S04.3) (5.1, then per step)
│   ├── S*.md                   →        core docs/manual/scenarios/S*.md: the walkthrough prose, each
│   │                                    linking to its executable twin (with its scenario)
│   └── README.md               →        core docs/manual/scenarios/README.md
├── protocol/
│   ├── dsl.rb, catalog.rb,     ▶        tinyactors-core/protocol/ (2.9)
│   │   render.rb
│   ├── README.md               →        tinyactors-core/protocol/README.md (2.9)
│   └── generated/              ▶        tinyactors-core/src/generated/ (2.9)
└── reference/
    ├── building-blocks.md      ⇢        each block's contract, notes and duplication record → the file
    │                                    comment of its unit, in the step that adds it (Phases 1, 2);
    │                                    the catalog → core docs/internal/building-blocks.md, an index of headers;
    │                                    the removal table and tree → this file, as history
    ├── ring.md                 →        core docs/internal/ring.md (2.8, 5.1)
    ├── envelope.md             ⇢        core docs/internal/envelope.md (4.2); delayed sends
    │                                    → core docs/internal/deadline.md (4.3)
    ├── names.md                ⇢        core docs/internal/names.md (3.1, 4.2); trie and atoms
    │                                    → core docs/internal/trie.md, atom.md (1.6)
    ├── macrostep.md            ⇢        core docs/internal/macrostep.md (4.4); slots, the chain limit
    │                                    and parking → core docs/internal/slots.md, park.md (4.5)
    ├── data-model.md           ⇢        merged into core docs/internal/datamodel-contract.md,
    │                                    value.md and datamodel-system-variables.md (4.4, 5.2)
    ├── definitions.md          ⇢        merged into core docs/internal/definition-lifecycle.md (5.4)
    ├── debugging.md            →        core docs/internal/debugging.md (5.1)
    ├── recording-and-replay.md ⇢        core docs/manual/persistence.md and
    │                                    docs/internal/durability.md (5.5)
    ├── telemetry.md            →        core docs/internal/telemetry.md (4.1)
    ├── unloading.md            ⇢        core docs/manual/virtual-actors.md and
    │                                    docs/internal/session-image.md (5.3)
    └── migration.md            ◼        history; its ADR tables become ADRs (Phase 0)
```

### What changes where they land

Legend as in the source tree: `+` new, `-` removed, `~` changes.

```
tinyactors-core/docs/
├── README.md                   ~        a short "how core talks to its host" paragraph; links to hosting.md
├── manual/
│   ├── hosting.md              +        from ARCHITECTURE.md: the host loop, an input's life, a send's life, time
│   ├── capacity-and-failure.md +        from BEHAVIORS.md
│   ├── parameters.md           +        from BEHAVIORS.md "Parameters"
│   ├── scenarios/              +        S01–S13 walkthroughs and their README
│   ├── persistence.md          ~        replay seeds new execution; c2h.input; checkpoints; crashes
│   ├── virtual-actors.md       ~        unloading bounds memory and is not persistence; load_needed
│   ├── time-and-events.md      ~        h2c.time, c2h.wake, delayed sends as envelopes
│   ├── embedding.md            ~        ta_run and the rings instead of deliver, resume and the vtables
│   └── extensions.md           ~        I/O processors and invokers as declared types and records
└── internal/
    ├── building-blocks.md      +        the catalog of blocks and their lifetimes, linking to each header
    ├── ring.md                 +
    ├── envelope.md             +
    ├── deadline.md             +        replaces timer.md and timer-lifecycle.md
    ├── names.md                +        replaces name-resolution.md
    ├── trie.md                 +        replaces event-name.md
    ├── macrostep.md            +
    ├── slots.md, park.md       +
    ├── telemetry.md            +
    ├── debugging.md            +
    ├── timer.md                -
    ├── timer-lifecycle.md      -
    ├── event-name.md           -
    ├── name-resolution.md      -
    ├── io.md                   -        I/O processors are declared types and records (5.2)
    ├── store.md                -        with store.c (1.8)
    ├── fault.md                -        folded into errors.md (1.8)
    ├── configuration.md        -        folded into configuration.h (1.8)
    ├── internal-header.md      -        folded into internal.h (1.8)
    ├── types.md                -        replaced by the units' header comments (1.0)
    ├── routing.md              ~        local mail is envelopes; the router boundary is gone
    ├── event-ordering.md       ~        one run queue in arrival order; deferred lists
    ├── interpreter.md,         ~        no allocation in a macrostep; startup after it; capacity errors
    │   interpreter-semantics.md
    ├── continuation.md         ~        written to the park area
    ├── errors.md               ~        error.execution.capacity.<resource>
    ├── memory-model.md         ~        the lifetimes table; per-actor and per-envelope figures
    ├── invariants.md           ~        the proposal's invariants
    ├── invoke.md               ~        lifetime tokens, invocation_seq, records instead of the vtable
    ├── datamodel-*.md, value.md ~       no TA_OP_NOMEM; _ioprocessors from the binding; vrefs
    ├── module-map.md           ~        follows the source tree above; an index of unit headers
    ├── <module>.md             -        each folds into its unit's header comment when a step first
    │                                    changes the unit (arena, atom, event, configuration, ...)
    └── README.md               ~        the new cross-cutting references

tinyactors-test/docs/protocol.md ~       hosted.post, hosted.turn and hosted.batches as a ring driver (5.1)
tinyactors-napi/docs/*.md        ~       design.md and tracing.md follow the rings (6.1)
README.md (root)                 ~       "Layers and responsibilities": no kernel; core holds the queue (6.3)
AGENTS.md (root)                 ~       the test table loses the kernel, gains tinyactors-otlp (6.2, 6.3)
tinyactors-otlp/README.md        +       the `tel` reader and OTLP encoder, its input and settings (6.2)
tinyactors-host/                 -       with the package; its ADRs stay as history (6.3)
```

### Rules for moving a document

- **A document moves in the commit that makes it true.** `envelope.md`
  lands with 4.2, not before, so the reference never describes code that
  does not exist. Until then the proposal's copy is the reference.
- **It is rewritten as reference.** No "would", "Sketch" or "the
  proposal"; status lines go. Open questions are answered, or become
  roadmap items or ADRs. Estimates become measurements once they exist,
  such as the bytes per envelope and per actor.
- **Every link is updated.** The proposal's files then link to their new
  homes, and the new homes link to ADRs, not to the proposal. Record IDs
  (`h2c.event`) and step IDs (`S04.3`) stay stable, because tests and
  documents cite them.
- **Each scenario's prose and its test move together.** A walkthrough
  that no test runs is not moved.

## Rules for every step

- **Each commit builds and passes `mise run test`.** Run `mise run test:full`
  before committing changes to allocation, pause and resume, paging,
  trace writing or per-actor costs, and say in the commit message that it
  passed. Phases 4 and 5 touch these paths in almost every commit.
- **Change APIs in place, and migrate every caller in the same commit.**
  The callers are the kernel, the test host and napi. While the kernel
  still exists (until [Phase 6](#phase-6-delete-the-kernel)), it is a
  caller like any other. It is changed in the same commit, never bridged
  by a shim, a `_v2` function or a compatibility wrapper.
- **Delete what a step replaces, in that step.** The removal table in
  [building-blocks.md](reference/building-blocks.md#what-core-can-remove)
  is the checklist. A step is done when its rows are gone.
- **Documentation is part of the step.** The commit that adds or changes
  a unit writes or updates its file comment and its functions' comments
  ([above](#documentation-lives-with-the-code)), and folds that unit's
  `docs/internal/` document in. A reviewer reads the header first.
- **One unit per object.** Each new block is its own unit (`vec.c` /
  `vec.h`), named after the object it defines. Nothing is added to a
  catch-all header, and `types.h` is split up in 1.0.
- **Generated code lives only in `src/generated/`.** Files there are
  rendered from `protocol/` and never edited by hand.
- **The public API is organised by what a host uses, not by how it is
  built.** A host includes `tinyactors.h` and gets everything, the ring
  records included. No public file is named after a build step or a
  generator.
- **Do not refactor code that a later phase deletes.** This covers
  `timer.c`, the event blocks, `io.c` / `io_origin.c` and the kernel's
  containers. Phase 1 leaves them alone.
- **Scope commits by component:** `refactor(core)`, `feat(core,host,napi)`,
  `refactor(core,napi)!`. A breaking change's body says what a caller does.
  The changelogs are written from these subjects at release time,
  not between releases.
- **Benchmarks at phase boundaries.** Run `mise run bench-compare <commit>`
  in `tinyactors-napi` against the commit before the phase. Report
  throughput and memory together, with the machine signature.
- **napi is being rewritten in another session.** Change only the call
  sites there, and say in the commit and the report which of its files
  were touched.

## Phase 0: adopt

Nothing below starts until these exist:

1. **The core ADRs** listed in
   [migration.md](reference/migration.md#adrs-to-write), numbered from
   0098. They are three rings; no allocation in a macrostep; the envelope
   pool; delayed sends as envelopes; only authored names interned; input
   observations on c2h; telemetry as a lossy ring; macrostep slots. Add
   one more: **the I/O and invoker type registries are declared through
   h2c** (refines ADRs 0016 and 0068). The list says which ADR records
   which of the proposal's design questions: the mandatory internal-event
   floor, fairness, ring and pool sizes, definition specs, trace context on
   records, inspection without attaching. Each ADR can be
   written just before the phase that needs it, not all up front.
2. **The repository ADR** that removes `tinyactors-host`, and the **napi
   ADR** that makes napi a binding over rings, before Phase 6.
3. **The proposal's status** changes to *Adopted → links*. The decisions
   then live in the ADRs, and the steps below move into the root
   `ROADMAP.md`.

## Phase 1: turn the duplicates into blocks

**No behaviour changes**, so no ADR is needed. Each step is a refactor
that the existing tests cover, and each makes core smaller. Every block
gets its own test program in `tinyactors-core/tests/`.

| Step | Block | Replaces | Notes |
|---|---|---|---|
| 1.0 | – | `types.c` / `types.h` | Split into `memory`, `str`, `vec`, `bitset.h`, `span.h`, `text_writer` and `version.c`. Delete `ta_actor_key`, which nothing uses. Every module includes what it uses instead of `types.h`. Add the ast-grep rule that a header opens with a file comment, with today's headers on its allow-list; each new unit from the split gets its comment. |
| 1.1 | D5 hash functions, D6 formatters | The Murmur finalizer duplicated in `session.c:4` and `trace.c:34`; FNV-1a in 32-bit and 64-bit forms; three decimal formatters, four hex helpers, two `cstr` helpers | One header. Trace and span IDs must stay identical: `span_test` covers them. |
| 1.2 | E1 generation handles | The hand-packed definition IDs (`system.c:456`), and `ta_actor_id_make` as the only shared helper | |
| 1.3 | E2 failing, counting test allocator | The hand-rolled copies in core's tests (36) | It lives with the tests and is never amalgamated into the library. |
| 1.4 | A2 layout builder | Six layout calculators | Checked arithmetic. Phase 4's workspace sizing depends on it. |
| 1.5 | A5 index pool | The actor table's paged directory (the address and session tables keep their bucket pages: they are linear hash tables, which stay, see 1.6) | Not the timer heap or `io_origin`: they are deleted later. A5 needs `refill(min_free)` and a ceiling from the start, even though only the pool uses the ceiling. |
| 1.6 | B3 hash table, D7 trie of atoms | The atom table and the event-name trie | Done. Two modes: fixed, and growing on an explicit call. The trie becomes a named block on B3 (`trie.c`; `event_name.c` renamed). The address and session tables keep linear hashing: B3 grows only in an explicit reserve that rehashes the whole table at once, and ADR 0065 decision 6 requires them to grow without copying the whole table in one step ([building blocks](reference/building-blocks.md#b-containers-over-indexes)). |
| 1.7 | B6 | Hand-rolled doubling arrays that survive | Done, with nothing to fold: the invoke table grows inside a macrostep and becomes a definition-sized table in 4.4; the export queue is a fixed stack in an arena; the timer lists and the event-block queue go with their phases. |
| 1.8 | – | Dead code: `fault.h`'s undefined declarations, unused `ta_configuration` kinds, stale comments (`interpreter.h:143`, `actor.h:287`) | |

**Exit:** the duplication summary in the catalog is resolved, except for
code that later phases delete and the address and session tables, which
keep linear hashing (ADR 0065). `mise run test:full` passes. Every unit the
phase touched opens with a file comment, and its `docs/internal/` document
is folded in. `bench-compare` against the commit before Phase 1 is
recorded. Core's size is measured at the end (Phase 7), not here: the
blocks carry what Phase 4 needs (refill, ceilings, trim, checked growth),
and the large deletions (`timer.c`, `io.c`, `io_origin.c`, the event
blocks, the out-of-memory stop) come in Phases 4 and 5.

**Recorded (Phase 1, 2026-10-05):** `bench-compare 02e431c` (the commit
before Phase 1) on Node and Bun, 2 alternating runs each. Core and native
bytes per actor are unchanged in every scenario (within 0.1%; 332.8
native B/actor in `scale.js`). Throughput moved within ±2.7% except one
figure at +5.0% (Bun tracing, sampled out). Eight figures crossed 5% in
the worse direction: seven are process RSS, which moved as much the other
way elsewhere in the same run (−8.9%), and one is the JavaScript heap in
the decoded-tracing scenario (+8.4%), which Phase 1 does not touch.

## Phase 2: add the new blocks, unused

Each block comes with its own tests and no caller yet. None of them needs
the interpreter. A block may be committed before anything uses it,
because its user follows within the same phase plan. That is the
exception to "remove what nothing uses".

| Step | Block | Notes |
|---|---|---|
| 2.1 | B1 intrusive index list | Two link fields per item (`next`, `owner_next`). Has `insert_after`, `splice`, `remove_after` and `unlink`. |
| 2.2 | B8 paged array, B2 indexed min-heap | B8: dense positions in power-of-two pages, `grow` and `shrink` at safe points. B2 on B8: keyed by `(due, order)`, with a back-index stored in the item, and `remove(i)`. The pool's refill grows it and the pool's `trim` shrinks it. |
| 2.3 | B4 fixed-entry FIFO, C3 quota with reservations | The internal queue is B4 plus two C3 quotas. |
| 2.4 | A6 chunk chain, over A5 | Parametrized by chunk size (64 bytes for names, 256 for parking). Has `need(n)` for preflight. |
| 2.5 | D1 byte sinks and sources | Moves `ta_arena_sink` and `ta_memory_source` out of `session_image.c`, and adds a sink and a source over a chunk chain. |
| 2.6 | D4 name resolver | Splits a name at the dots and looks each segment up with `ta_atom_find`; scratch holds `eventNameDepth` segments. |
| 2.7 | B7 type registry | Built-in entries and their aliases, `declare`, `declared`. |
| 2.8 | C1 SPSC byte ring, and the control block | Lossless and lossy modes, pads, seq. Tests cover a wrap on every offset, a ring of `maxRecordBytes`, and lossy drop counting. |
| 2.9 | C2 record codec generator | Move `protocol/` (the DSL, the catalog, the renderer) from this proposal into `tinyactors-core/protocol/`. Add a mise file task (`protocol`) that renders into `src/generated/`: the record header, the C writers and validating readers, `MESSAGES.md` and `messages.json`, with a `--check` that `mise run test` runs. The generated code stays private to core here; the records reach hosts through `tinyactors.h` in 5.1. The reader tests feed malformed records: a span outside the record, a wrong size, an unknown kind, a seq gap. |
| 2.10 | C4 claim set (optional) | Reserves across C3 quotas, all or nothing. |

**Exit:** every new block has its own test program, and the core `test`
task's sources include them. Each block's header carries its contract from
`building-blocks.md`; the catalog's row for it links to the header.

## Phase 3: match names by lookup

[names.md](reference/names.md) splits names into authored names, interned
at registration, and run-time names, kept as bytes. **Matching** can
change on its own. **Storing** run-time names as bytes needs the name
area, so it lands in 4.2.

| Step | Change |
|---|---|
| 3.1 | When an event is taken, resolve its name with D4 into atoms, where a segment that is not an atom is UNKNOWN. Run the existing matcher over the result. Add the generated prefixes (`done.state`, `error.execution`, ..., and the `invoke` segment) as atoms at system creation. |
| 3.2 | Tests: the worked examples in names.md as a core test, plus a test that random names leave the atom table and the trie unchanged. |

**Exit:** matching gives the same answers through lookup. The IRP tests in
the test host and napi still pass.

**Done (Phase 3).** The matcher runs over a name's atoms, built once per
selection; `<send typeexpr>` and `<invoke typeexpr>` look types up
without interning; the `invoke` segment is an atom from system creation.
Events still carry a trie ID until 4.2, so at take the atoms come from
the event's trie path, which gives the same list `ta_names_resolve`
gives over the name's bytes (proved in `tests/name_matching_test.c`).
Step 4.2 replaces that path with `ta_names_resolve` over the envelope's
bytes, and stops interning at the sites that still do: the `<send
eventexpr>` value, the generated invocation ID (`<stateid>.<platformid>`,
to become a state ID and a counter), `done.invoke.<id>`,
`error.communication.invoke.<id>.<reason>`, `#_<invokeid>` and
session-URI families in `targetexpr`, and the host calls
`ta_send_id_intern`, `ta_system_intern_invoke_id`,
`ta_event_name_intern` and `ta_system_intern_atom`.

## Phase 4: core gains its queue

The kernel stays and keeps napi and the test host working. It is changed
in each commit to call core's new API. Steps 4.2 to 4.5 are the expensive
ones; run `mise run test:full` before each commit.

| Step | Change | Deletes | ADR |
|---|---|---|---|
| 4.1 | **`tel`.** Allocate the control block and the `tel` ring at system creation; with `telBytes` 0 there is no `tel` ring. Trace records become self-contained `tel.trace` records, names and lists as text, and core stamps each with the clock the host installs at creation. The kernel's OTLP encoder drains `tel`; it stops calling back into core and reading the host's clock. This is the smallest real user of a ring. | The trace sink vtable and `ta_null_trace_sink` | telemetry is a lossy ring |
| 4.2 | **The pool and `ta_run`, with one slot.** Envelopes (A5), the run queue and the outgoing list (B1), the name area (A5 + A6), the admission watermark and the macrostep reserve (C3). Local mail becomes envelopes. `ta_actor_deliver` becomes admission into the pool, changed in place. `ta_run` replaces `ta_actor_resume`. Run-time names become bytes in the name area, completing Phase 3. Recycling turns undeliverable mail into `error.communication`. Local mail carries the sender's trace parent in the envelope's name-area text, only when traced, so traces stay linked across sessions as they are today ([telemetry](reference/telemetry.md#trace-context-across-the-rings)). Measure the h2c and `tel` defaults (16 KiB each) and check the derived `c2hBytes`, `(3 × maxConcurrentInputs + 2) × maxRecordBytes`, with napi's suites, `scale.js` and the IRP suite ([BEHAVIORS.md](BEHAVIORS.md#parameters)). | The router vtable, `TA_ERR_BUSY`, the event blocks and their cache (`TA_PARAM_EVENT_BLOCK_CACHE`), run-time interning (`ta_send_id_intern`, `ta_system_intern_invoke_id`, the `eventexpr` and `typeexpr` sites, `invoke.c`'s error names) | pool; only authored names interned |
| 4.3 | **Delayed sends as envelopes.** The deadline index (B2 on B8, grown in the pool's refill) and the owner lists (B1). A delayed send keeps its sender's trace parent, in the pool and in session images. `ta_run` reports the next due time; that value becomes `c2h.wake` in 5.1. `<cancel>` walks the owner list. | `timer.c` / `timer.h` (634 lines), sparse state, `ta_system_dispatch_due` | delayed sends as envelopes |
| 4.4 | **No allocation in a macrostep.** The internal queue becomes B4 with an ordinary and a mandatory quota (C3). The compiler sizes the mandatory floor for one round of every phase; a mandatory event beyond it takes a free ordinary entry, and an overflow abandons the input as a runaway, with `runaway_cause` `internal_queue` on the `RUNAWAY` record. Invocation startup moves after the macrostep, with the local child's lifetime token. An actor's invoke table becomes a fixed table sized from its definition at spawn, and its activation effects and pending invokes stop growing a `ta_vec` inside a macrostep. Capacity failures become `error.execution.capacity.<resource>`. Data models answer `TA_OP_ERROR` instead of `TA_OP_NOMEM`, which changes the test host's jq and quickjs data models and napi's data model. ADR 0097's `copy_out`, which relies on `TA_OP_NOMEM` today, answers `TA_OP_ERROR` when it runs out, and the chart sees `error.execution` like every data-model failure (ADR 0102). Nested interpreter workspaces are sized and allocated at registration, spawn or load, not on first use inside a macrostep, where ADR 0092 allocates them today (ADR 0102). A core test asserts that an allocator which refuses every request after a macrostep starts never sees a call. | The out-of-memory stop machinery: the NOMEM branch, `ta_reserve_stop` / `ta_stop*` where only out-of-memory uses them, the spare continuation and spare sparse state, queue mark/truncate, `TA_OP_NOMEM` | no allocation in a macrostep |
| 4.5 | **Macrostep slots.** Slot records (A5), deferred lists (B1), `slotSlice`, the chain limit, and parking into the park area (A5 + A6 + D1 + D2). Start with parking disabled (`parkAreaBytes` 0, plain run to completion), then enable it in its own commit. `ta_actor_inspect` marks a session stable or active and gives an active one's position, so a host reads a live system between `ta_run` calls without attaching ([debugging](reference/debugging.md#inspection-without-attaching)). `ta_system_trim` gives back free pages of the pool, the name area, the park area and the deadline index; core never trims on its own. | napi's quarantine, `release()` and the `.quarantined` event (napi call sites only) | macrostep slots |

**Step 4.2 in commits.** 4.2 lands as eight commits, each green, in
this order; the baseline for 4.5's `bench-compare` is `ccd6d632`, the
commit before the first.

1. `envelope.c/.h` and `name_area.c/.h`, with their test, and no callers.
2. Admission and `ta_run` with one slot. `ta_actor_deliver` admits or
   bounces; `ta_run` replaces `ta_actor_resume` and reports each input's
   outcome (completed, ignored, finished, faulted, refused) in a buffer the
   caller passes, previewing `c2h.input_completed` and
   `c2h.input_refused`. The kernel settles waiters, quarantine, hooks and
   the recorder from those outcomes; the recorder journals an input when it
   is taken. Spawn and load admit their bootstrap input, outside any
   macrostep. A session stopped by a fault leaves the slot and keeps its
   continuation; its mail waits on its deferred list, so the deferred list
   arrives here with one slot. napi's `release()` calls a native release
   until 4.5 removes quarantine.
3. The taken envelope stays put while its macrostep runs; the event blocks
   and their cache go.
4. Mail to resident sessions travels as envelopes on the outgoing list,
   with the trace parent in its text when traced. A failed fanout fails
   once, at the first send that does not fit.
5. Waiting entries, `load_needed` and a native "load failed" call, the
   native withdraw, by a scan of the pool as ADR 0099 decision 11 says
   (napi's `send()` cancellation needs it before `h2c.withdraw` exists;
   the B3 index was tried and removed, ADR 0099's implementation notes),
   and
   `error.communication` for undeliverable mail. The router vtable and the
   kernel's router and mailboxes go; the kernel's `loads.c` keeps the
   loader policy until 5.1.
6. Names resolved from the envelope's bytes at take; core stops interning
   run-time names, except the `eventexpr` of delayed sends, whose names
   `timer.c` and `ta_delayed_send_info` hold as IDs until 4.3.
7. Hosts pass names as bytes; `ta_send_id_intern`,
   `ta_system_intern_invoke_id` and the host calls of
   `ta_event_name_intern` go.
8. The defaults measured, recorded in ADR 0099's notes.

Decisions for these commits:

- **`TA_ERR_BUSY` stays where it means "not idle"** (offload, capture,
  export, abandon). Its admission uses go; a bounce answers `TA_ERR_FULL`,
  which napi maps to its existing queue-full code.
- **`fairnessRatio` arrives with h2c in 5.1**: until then admission
  happens at call time, and nothing alternates with the run queue.
- **One session at a time.** Replay and the debugger run one session
  through a session filter in `ta_run`'s parameters, the debugging
  exception of [macrostep.md](reference/macrostep.md#macrostep-slots).
- **Invocation tokens** landed in 4.3a, not 4.2: a local invoked child
  takes one envelope from the pool when it is spawned, a free one inside
  the parent's macrostep (a spawn that finds none answers `TA_ERR_FULL`;
  an invoker that passes that code on fails the `<invoke>` as
  `error.execution.capacity.pool`), with room in the name area for a timer's
  record. Publishing its completion gives the token back first, so the
  completion's mail, or the timer that keeps it, always finds room; a
  completion is never dropped. 4.4 moves startup after the macrostep.
- **The defaults are measured by proxy** in 4.2, since h2c and c2h
  arrive in 5.1: `tel` by `tel.dropped` directly; h2c by the peak of
  native admissions between two `ta_run` calls times the size of
  `h2c.event`; `c2hBytes` by the largest per-input batch of the records
  an input would write, against the derived 794,624 bytes. The counters
  live in the test host and the benchmarks, not in core. 5.1 checks the
  figures again on real rings.

**Done (4.2).** The pool and `ta_run` with one slot landed as 4ca67840
(the envelope pool and the name area), fe01185e (tests drive actors through
one helper), 93c3d46a (admission and `ta_run`), 904b3353 (the taken input
stays in its envelope), 19cd1813 (mail to resident sessions as envelopes),
482417e2 and 6a6a1fac (mail for targets that are not resident waits in
core; hosts post by address; the router goes), 3c7ebc7f (an immediate
send's `eventexpr` name as bytes), 0bc6cfa9 and bb87794b (hosts pass and
read names as text), 1d4f4588 (internal events and invocation names as
text), 17ee9a94 (random target expressions) and the commit that measured
the defaults. Commits 5 to 7 of the list above were split into those
parts, and 7 landed before the rest of 6. The defaults, measured by proxy,
are in ADR 0099's notes: h2c's 16 KiB may be small for a host that admits
a whole turn's messages at once (5.1 measures it on the real ring);
`tel`'s 16 KiB and the derived `c2hBytes` hold. A session-URI family no
definition has is still interned (ADR 0100's notes), until waiting entries
keep the address's bytes.

Carried forward from commit 4 (19cd1813):

- **The macrostep reserve is a fixed 64 envelopes**, and a macrostep
  never refills, so on a cold pool one macrostep can send about 64
  messages before `error.execution.capacity.pool`, however large
  `poolEnvelopes` is. 4.5 sizes the reserve per slot from the pool's
  headroom (`poolEnvelopes` minus the admission watermark) and refills
  each slot's claim before it takes an input.
- **`_event.data` of `error.execution.capacity.*` is empty.** 4.4 fills
  it with the action and the `<foreach>` iteration (ADR 0102).
- **The trace watch no longer marks mail between resident actors**,
  because that mail no longer passes through the kernel. The per-actor
  watch bit in core ([telemetry](reference/telemetry.md)) restores it; it
  lands with 6.1, when napi drives core directly.
- Mail to a resident actor that finishes before taking it is refused by
  core; commit 5 turns it into `error.communication`.
- **Holding an `#_internal` name may grow the run's name buffer inside a
  macrostep** (commit 6b), where interning allocated before. 4.4 removes
  it: the buffer is sized for one name-area field (65,535 bytes) at
  system creation, or names are matched without a contiguous copy.
- **napi does not recover an actor whose data model ran out of memory**
  under `refuseAllocationsFrom`: its macrostep stays stopped after the
  refusal is lifted. It predates 4.2; 4.4 removes the out-of-memory stop
  machinery and checks the case.
- **A session URI's family is still interned** when no definition of the
  system has it (commit 6c), because a waiting entry names its
  destination by the family atom. Waiting entries keep the address's
  bytes with 5.1's records and 5.3's images; the interning goes then.
- **`mise run build` can keep a stale amalgamation** after `git checkout`
  restores a source ("amalgamate: cached"). A check that reverts a source
  to prove a test catches something must run `mise run amalgamate`
  itself. Fix the amalgamate task's cache key when a step next touches
  the build.
- **The kernel's overflow machinery is idle after commit 5**: no message
  is marked `may_overflow`, so napi's `maxQueuedMessages` and
  `maxRetainedBytes` act on nothing. They go with napi's parameter
  changes in 6.1 (migration.md), not before.

Carried forward from the step's review (the 4.2 fix commits):

- **The kernel still keeps per-actor mailboxes** that shadow core's pool,
  for `send()` waiters, held mail and quarantine. Phase 4's exit "the
  kernel's scheduler holds no mailboxes" is met by 4.5, which removes
  quarantine and `release()`; what remains after that (waiters keyed by
  correlation, held mail of the application) goes with 5.1, when outcomes
  arrive on c2h and the kernel's admission code is deleted.
- **The random paging runs no longer cause refused due sends.** Resolved
  by 95c4228c (4.3a), which replaced those scenarios with ADR 0101's
  property.
- **A kernel load answers only the last core request that joined it**
  (`tah_loads_ask`): two runtime IDs of one gone session are two waiting
  entries that share one load, and the first request is replaced, so its
  entry stays owed. A load keeps every core request it answers when 5.1
  moves load requests onto c2h.
- **The kernel keeps names only where an observer needs an ID**: mail core
  queued reaches the kernel's observer as text, and only the recorder
  interns it. JavaScript's own name cache (`lib/events.js`) stays until
  6.1.

**Done (4.3).** Delayed sends are envelopes in a deadline index
(95c4228c), setting the time fires due sends and `ta_run` reports the
next wake (caf78dfe), and delayed and I/O-processor sends keep an
eventexpr name as text (4.3c): a timer envelope keeps the name as the
second field of its text, inspection and export copy it next to their
records (`ta_delayed_send_info.event_text`), images and offload bytes
carry it, and `ta_send.event_text` gives it to an I/O processor. Two
places still intern a run-time name, both recorded in ADR 0100's notes:
a send held for an unpublished session, and a child's completion kept as
a stored send when its parent's pool is full. `ta_event_name_intern` and
`ta_system_intern_atom` stay public: the test host interns authored names
and registers I/O and invoker types with them until 5.2's type
registries; core interns no run-time name through them.

**4.4 so far.** What a macrostep needs is allocated when it takes its
input, a safe point (9183bd73, 78c7d71b, ce1535fe, 5cd2d33e): the storage
to pause or stop, an internal queue of `internalQueueEvents` ordinary
entries plus a floor the definition's shape sizes, the invoke table (a
record per `<invoke>`), pending invocations, the nested interpreter
regions and their exit frames, and a name buffer for the longest name.
A `<raise>` or `#_internal` send beyond the ordinary quota fails as
`error.execution.capacity.internal`. A data model, `copy_out` or an
invoker that runs out of memory fails in the chart as `error.execution`
(1c334601); `TA_OP_NOMEM` remains only for an I/O processor's send and
for image bytes. `tests/macrostep_alloc_test.c` checks, with an
allocator that refuses whatever is asked while a macrostep runs, that
raises, sends, delayed sends, cancels and host invocations make no
request. Choices where the design left one open: the floor is
`1 + data + 3 × states + 2 × transitions + invokes`, an overestimate; the
system keeps up to 64 spare queues; the name buffer costs 64 KiB per
system. **Left for 4.4:** a queue full of mandatory events still stops
the session as running out of memory did, where ADR 0102 abandons the
input as a runaway with `runaway_cause` `internal_queue`; invocation
startup still runs inside the macrostep, so a local child's spawn, its
queue when it is exited, and an unpublished session's held effects
(`ta_activation_effects`) still allocate there; the out-of-memory stop
machinery (`ta_stop*`, the spare continuation and sparse state, queue
mark and truncate) stays until those go; `_event.data` of capacity
errors is still empty; the test host's data models still answer
`TA_OP_NOMEM`, which core now reads as an error.

**Startup after the macrostep, and no allocator call inside one.**
Invocation startup runs after the macrostep (macrostep.md, "Invocation
startup and input completion"): it may allocate, and an invocation it
cannot start, out of memory included, starts nothing and raises its
error.execution; it is a cut of `check-macrostep-alloc`, since
`system->run.running` is false while it runs. Inside a macrostep nothing
frees either: a session the macrostep's host destroys is gone at once
and its storage is given back when the macrostep returns
(`ta_actor_free_doomed`, the design's "child exit cleanup"); the sparse
state and the internal queue are compacted there too
(`ta_interpreter_settle`). Choices the design left open, after checking
macrostep.md, BEHAVIORS.md and ADRs 0102 and 0103: a finished session's
done data and final state live in a finish record in its actor block,
made at spawn for a definition with a top-level `<final>` (the design
names only the floor's `F` entries and the child's token for a parent's
completion); generated invocation names are written into one buffer
made at system creation for the longest name a field holds (ADR 0102:
"generated error names ... have pre-sized storage").

**Exit:**
- The kernel's scheduler holds no mailboxes, only the code that admits
  input.
- `mise run test:full` passes.
- `bench-compare` against the commit before 4.2 is recorded in the 4.5
  commit or its ADR, including the per-actor bytes, before and after.
- The pool ADR records the measured h2c and `tel` defaults and the check
  of the derived `c2hBytes` (4.2).

## M2: core speaks only rings

The rings replace every host-facing call and vtable. **The test host is
the reference host:** its Ruby driver becomes the pump loop of
[ARCHITECTURE.md](ARCHITECTURE.md#the-host-loop), written once and
plainly: write h2c, run, drain c2h, act on each record. Policy that the
kernel used to hold (answering load requests, keeping a recording
journal, running effects) lives there, as small as the scenarios need.
M3 translates this host into napi, so it is written to be read.

The test host reaches the rings through its existing RPC server, which
exposes them as bytes; Ruby decodes them with `messages.json`, so the
generated codec is checked from a second language.

| Step | Change | Deletes | ADR |
|---|---|---|---|
| 5.1 | **Rings for inputs.** `h2c.event`, `h2c.time`, `h2c.spawn`, `h2c.destroy`, `h2c.withdraw`; `c2h.bounced`, `c2h.input_refused`, `c2h.input_completed` (with `ignored`), `c2h.spawned`, `c2h.macrostep_ended`, `c2h.wake`, `c2h.finished`, `c2h.destroyed`. `h2c.event` and `h2c.spawn` carry a trace parent. Core reserves room for mandatory records, one pad per record, sized from the slot count, before taking an input, and takes it only if 2 × `maxRecordBytes` of headroom stays unreserved ([ring](reference/ring.md#the-c2h-reserve)). The ring records reach hosts through `tinyactors.h`, as `docs/build/` describes. The test host becomes a ring driver, and its scenarios assert on decoded records. Core's tests gain the ring-script driver (E3). The ring defaults are measured on real rings in the test host, replacing 4.2's proxies. | `ta_actor_deliver`; the native spawn, time and destroy entry points; `ta_run`'s outcome and names buffers, which previewed c2h | three rings |
| 5.2 | **Effects on c2h.** `c2h.send`, `c2h.invoke`, `c2h.invoke_cancel`, `c2h.invoke_forward`, `c2h.log`, and `h2c.invoke_report` back. An action that may write c2h checks for room before it evaluates anything; without room its slot parks before the action, which runs once, after a drain; `ta_run` answers `TA_RUN_C2H_FULL` when nothing else can run ([macrostep](reference/macrostep.md#a-full-c2h-pauses-the-slot)). Effects carry the step's trace ID, flags and span ID. Two B7 registries filled by `h2c.type_declare`; bindings render `_ioprocessors` themselves; origins are text. The test host's I/O and invoker instruments become effect handlers in the reference host. | The I/O, invoker and log vtables; `ta_system_register_io`, `ta_system_find_io`, `ta_system_register_invoker`; `io.c`, `io_origin.c`, the scope API, `TA_DM_TEXT_IO_*` | type registries through h2c |
| 5.4 | **Definitions through h2c**, beside 5.2. `h2c.definition_register`, `h2c.definition_release`, `h2c.definition_retire`, `c2h.definition_result`, with bounded diagnostics. The request carries a native spec handle built with `tree.c`'s builder ([definitions](reference/definitions.md#a-native-spec-handle-not-a-byte-layout)). | Native definition-management entry points a host does not need; `ta_definition_sink` if nothing uses it | – (refines 0058, 0077, 0079) |
| 5.3 | **Unloading and loading.** Waiting entries keep the address's text once, which ends the last run-time interning (a session URI's family); `c2h.load_needed`, `h2c.load`, `h2c.load_failed`, `c2h.load_result`, `h2c.unload`, `c2h.unloaded`, `c2h.loaded`. Images go through D1 and D2 into host blobs. The reference host gains a small loader. | The directory vtable; the native load-request calls of 4.2 | – (refines 0037) |
| 5.5 | **Recording and replay.** The `recorded` bit, `c2h.input` with the input's trace parent, `h2c.spawn` with the replaying flag, `h2c.publish`, `h2c.checkpoint`, `c2h.state`. The reference host gains a small journal; its scenarios bring back the replay coverage napi's suites held. | Held effects (`ta_activation_effects`); the unused `durable` field of `ta_datamodel_value_write_params` | input observations on c2h |
| S01–S13 | **The scenarios as ring scripts**, one commit each, beside 5.2 to 5.5. They become M3's acceptance tests. | – | – |

Order: 5.1, then 5.2 and 5.4 side by side, then 5.3, then 5.5; the ring
scripts run beside them once 5.1 has landed.

**Done when:** no vtable remains in core but the data model's, the value
vtable, byte sinks and sources and the chart codec; the test host drives
core only through the rings and covers every c2h record kind; IRP
183/183; S01 to S13 pass as ring scripts; `mise run test:full` passes.

## M3: napi on the rings

napi's native layer is written again from scratch, against the rings.
`lib/`, `test/` and `index.d.ts` are its specification; the reference
host of M2 is its model.

1. **A design note first,** in `tinyactors-napi/docs/`: the pump loop;
   ring sizing (`tel` only when the system traces, large enough for one
   turn's records); value bindings; where each piece of policy lives,
   native or JavaScript (the loader, the recorder and replay, metrics,
   offload, the trace watch, containers, the counting allocator);
   linking `tinyactors-otlp` for OTLP. Span IDs come from core's
   `ta_step_key_span_id`.
2. **Then the code, until `lib/` works:** napi's suites and its IRP run
   pass on Node and Bun. `index.d.ts` changes as
   [migration.md](reference/migration.md#napi) lists; what the design
   removes goes (quarantine and `release()`, the overflow parameters,
   `waiting-full`, `maxWaiting`), and napi ADR 0040's status line says
   so. Where the implementation lags, `// status:` markers say what is
   missing.

**Done when:** napi's tests are enabled again and pass, IRP 176/176 on
Node and Bun, and `mise run check-api-status` is clean.

## M4: finale

- **Verification:** `mise run test:full`; one review of the whole range
  against `ccd6d632` by a reader who did not write it; the IRP results
  of the test host and napi against `ccd6d632`.
- **Removals:** walk the removal table and the duplication summary in
  [building-blocks.md](reference/building-blocks.md): every row is gone
  or deliberately kept.
- **Out-of-memory sweep** over creation, registration, spawn, load and
  refill only; a macrostep has nothing left to fail.
- **Core is smaller than before Phase 1:** count `tinyactors-core/src/`
  without `src/generated/`, code and comments separately, against the
  commit before Phase 1. At the end of Phase 1, code was 18,424 lines
  against 17,588 before it, and comments 5,975 against 4,070; comments
  grow on purpose, as documentation moves into headers.
- **Benchmarks and tuning:** a `bench-compare` with throughput, memory
  and the machine signature against the commit before Phase 1. Known
  debt to tune or accept: traced throughput (13 to 22% slower since
  4.1), bytes per actor (264 before 4.2, 348 after 4.3a), the names
  buffer.
- Changelogs come at release time, through the `release-changelog` skill.

## Running the work

### Order

```
M1: 4.3c ─▶ 4.4 ─▶ 4.5
M2: 5.1 ─▶ 5.2 ┬─▶ 5.3 ─▶ 5.5
           5.4 ┘
           S01 … S13 beside 5.2 to 5.5
M3: design note ─▶ napi native layer ─▶ napi tests enabled
M4: finale
```

One implementer works the main line at a time; the ring scripts, and
folding `docs/internal/` into headers, are the side lanes. A side lane
works in its own worktree and lands by rebase when the main tree is
clean.

### How a step runs

- **One implementer per step,** forked with the design in its context.
  It commits each green commit itself, small and focused, each passing
  `mise run test`, and reports at the end of the step, at a decision the
  design does not settle, or near its turn limit.
- **`mise run test:full` and core's `mise run asan` run once per step,**
  and the step's last commit says they passed.
- **Implementers iterate narrowly** (one test program, one scenario
  pattern) and run the fragile suites early: `scale_test` and
  `params_test`.
- **No per-step review.** Each step ends with the implementer's own
  check of lifetimes, allocation inside a macrostep, ordering and
  weakened tests, and each new mechanism gets a randomized property test
  where one fits. One independent review runs at the end of M2, where
  the test oracle moves to the rings, and one at the finale.
- **No step waits for a person.** Where the ADRs and reference documents
  leave something open, the implementer chooses what fits them and
  records the choice in the commit message and here.
- **Benchmarks record and do not block;** tuning waits for M4. The scale
  tests stay in the suites, because they catch complexity bugs.

### Where a person or an independent reader is needed

| Gate | Who | What they check |
|---|---|---|
| Public API changes in `tinyactors.h` | the maintainer, when they look | names, ownership, failure codes |
| End of M2, the test oracle on the rings | a reader who did not write it | every property the old driver checked is still checked through the rings |
| M3's design note | the maintainer | where policy lives, and what `index.d.ts` keeps |
| M4's benchmarks | the maintainer | which regressions to tune away and which to accept |
| The finale's review | a reader who did not write it | the whole range against `ccd6d632` |

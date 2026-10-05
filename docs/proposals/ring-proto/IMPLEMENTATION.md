# Implementation: replacing core's plumbing with building blocks

**Status:** Sketch (2026-10-04), part of [the ring protocol proposal](README.md).
This describes how the replacement would proceed *if* the proposal is
adopted. It is not an instruction to start. Adoption comes first: see
[Phase 0](#phase-0-adopt).

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
    ├── names.c / names.h       +        D4 resolver (bytes → atoms, UNKNOWN); envelope text layout (2.6, 4.2)
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
    ├── name_area.c / .h        +        an A5 + A6 instance with 64-byte chunks, plus its quota (4.2)
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

## Phase 4: core gains its queue

The kernel stays and keeps napi and the test host working. It is changed
in each commit to call core's new API. Steps 4.2 to 4.5 are the expensive
ones; run `mise run test:full` before each commit.

| Step | Change | Deletes | ADR |
|---|---|---|---|
| 4.1 | **`tel`.** Allocate the control block and the `tel` ring at system creation; with `telBytes` 0 there is no `tel` ring. Trace records become self-contained `tel.trace` records, names and lists as text, and core stamps each with the clock the host installs at creation. The kernel's OTLP encoder drains `tel`; it stops calling back into core and reading the host's clock. This is the smallest real user of a ring. | The trace sink vtable and `ta_null_trace_sink` | telemetry is a lossy ring |
| 4.2 | **The pool and `ta_run`, with one slot.** Envelopes (A5), the run queue and the outgoing list (B1), the name area (A5 + A6), the admission watermark and the macrostep reserve (C3). Local mail becomes envelopes. `ta_actor_deliver` becomes admission into the pool, changed in place. `ta_run` replaces `ta_actor_resume`. Run-time names become bytes in the name area, completing Phase 3. Recycling turns undeliverable mail into `error.communication`. Local mail carries the sender's trace parent in the envelope's name-area text, only when traced, so traces stay linked across sessions as they are today ([telemetry](reference/telemetry.md#trace-context-across-the-rings)). Measure the h2c and `tel` defaults (16 KiB each) and check the derived `c2hBytes`, `(3 × maxConcurrentInputs + 2) × maxRecordBytes`, with napi's suites, `scale.js` and the IRP suite ([BEHAVIORS.md](BEHAVIORS.md#parameters)). | The router vtable, `TA_ERR_BUSY`, the event blocks and their cache (`TA_PARAM_EVENT_BLOCK_CACHE`), run-time interning (`ta_send_id_intern`, `ta_system_intern_invoke_id`, the `eventexpr` and `typeexpr` sites, `invoke.c`'s error names) | pool; only authored names interned |
| 4.3 | **Delayed sends as envelopes.** The deadline index (B2 on B8, grown in the pool's refill) and the owner lists (B1). A delayed send keeps its sender's trace parent, in the pool and in session images. `ta_run` reports the next due time; that value becomes `c2h.wake` in 5.1. `<cancel>` walks the owner list. | `timer.c` / `timer.h` (634 lines), sparse state, `ta_system_dispatch_due` | delayed sends as envelopes |
| 4.4 | **No allocation in a macrostep.** The internal queue becomes B4 with an ordinary and a mandatory quota (C3). The compiler sizes the mandatory floor for one round of every phase; a mandatory event beyond it takes a free ordinary entry, and an overflow abandons the input as a runaway, with `runaway_cause` `internal_queue` on the `RUNAWAY` record. Invocation startup moves after the macrostep, with the local child's lifetime token. An actor's invoke table becomes a fixed table sized from its definition at spawn, and its activation effects and pending invokes stop growing a `ta_vec` inside a macrostep. Capacity failures become `error.execution.capacity.<resource>`. Data models answer `TA_OP_ERROR` instead of `TA_OP_NOMEM`, which changes the test host's jq and quickjs data models and napi's data model. A core test asserts that an allocator which refuses every request after a macrostep starts never sees a call. | The out-of-memory stop machinery: the NOMEM branch, `ta_reserve_stop` / `ta_stop*` where only out-of-memory uses them, the spare continuation and spare sparse state, queue mark/truncate, `TA_OP_NOMEM` | no allocation in a macrostep |
| 4.5 | **Macrostep slots.** Slot records (A5), deferred lists (B1), `slotSlice`, the chain limit, and parking into the park area (A5 + A6 + D1 + D2). Start with parking disabled (`parkAreaBytes` 0, plain run to completion), then enable it in its own commit. `ta_actor_inspect` marks a session stable or active and gives an active one's position, so a host reads a live system between `ta_run` calls without attaching ([debugging](reference/debugging.md#inspection-without-attaching)). `ta_system_trim` gives back free pages of the pool, the name area, the park area and the deadline index; core never trims on its own. | napi's quarantine, `release()` and the `.quarantined` event (napi call sites only) | macrostep slots |

**Exit:**
- The kernel's scheduler holds no mailboxes, only the code that admits
  input.
- `mise run test:full` passes.
- `bench-compare` against the commit before 4.2 is recorded in the 4.5
  commit or its ADR, including the per-actor bytes, before and after.
- The pool ADR records the measured h2c and `tel` defaults and the check
  of the derived `c2hBytes` (4.2).

## Phase 5: rings replace the vtables

`h2c` and `c2h` arrive together, because inputs need their dispositions.
The test host becomes a ring driver here rather than at the end. From then
on its scenarios check the protocol directly.

| Step | Change | Deletes | ADR |
|---|---|---|---|
| 5.1 | **h2c and c2h for inputs.** The ring records reach hosts through `tinyactors.h`, a public API change the maintainer reviews: napi's generated bindings follow. How the build puts them there is described in `docs/build/`. `h2c.event`, `h2c.time`, `h2c.spawn`, `h2c.destroy` and `h2c.withdraw`. On c2h, `c2h.bounced`, `c2h.input_refused`, `c2h.input_completed` (with `ignored`), `c2h.spawned`, `c2h.macrostep_ended`, `c2h.wake`, `c2h.finished` and `c2h.destroyed`. `h2c.event`, `h2c.spawn` and `h2c.invoke_report` carry a trace parent that a host fills from an incoming `traceparent`. Core reserves room for mandatory records, one pad per record, before taking an input, and takes it only if 2 × `maxRecordBytes` of headroom stays unreserved ([ring](reference/ring.md#the-c2h-reserve)). **The test host becomes a ring driver**: `hosted.post`, `hosted.turn`, `hosted.batches`, decoded with `messages.json`. Add the E3 ring script driver to core's tests. | `ta_actor_deliver`, the native spawn, time and destroy entry points; the kernel's admission code | three rings |
| 5.2 | **Effects on c2h.** `c2h.send`, `c2h.invoke`, `c2h.invoke_cancel`, `c2h.invoke_forward`, `c2h.log`, and `h2c.invoke_report` coming back. An action that may write c2h checks for room before it evaluates anything; without room its slot parks before the action, and the action runs once, after a drain; `ta_run` answers `TA_RUN_C2H_FULL` when nothing else can run ([macrostep](reference/macrostep.md#a-full-c2h-pauses-the-slot)). `c2h.send`, `c2h.invoke`, `c2h.invoke_forward` and `c2h.log` carry the step's trace ID, flags and span ID, so a processor writes `traceparent` without calling into core. Two B7 registries, filled by `h2c.type_declare`. Bindings render `_ioprocessors` locations themselves, and origins are text. | The I/O, invoker and log vtables; `ta_system_register_io`, `ta_system_find_io`, `ta_system_register_invoker`; `io.c`, `io_origin.c`, the scope API, `TA_DM_TEXT_IO_*`; the kernel's adapters and effects queue | type registries through h2c |
| 5.3 | **Unloading and loading.** Waiting entries (A5), one per waiting address, each holding the address's text once and its waiting list (B1), found through a fixed B3 table keyed by the address's hash ([envelope](reference/envelope.md#the-waiting-entry-one-copy-of-the-address)), `c2h.load_needed`, `h2c.load`, `h2c.load_failed`, `c2h.load_result`, `h2c.unload`, `c2h.unloaded`, `c2h.loaded`. Images go through D1 and D2 into host blobs. | The directory vtable; the kernel's loads, offloads and directory | – (refines 0037) |
| 5.4 | **Definitions through h2c.** `h2c.definition_register`, `h2c.definition_release`, `h2c.definition_retire`, `c2h.definition_result`, with bounded diagnostics. The request carries a native spec handle built with `tree.c`'s builder; core writes the native compile-error struct into host memory, or answers `capacity` if it does not fit ([definitions.md](reference/definitions.md#a-native-spec-handle-not-a-byte-layout)). | The native definition-management entry points that bindings do not need; `ta_definition_sink` if nothing uses it | – (refines 0058, 0077, 0079) |
| 5.5 | **Recording and replay.** The `recorded` bit, `c2h.input` with the input's trace parent, `h2c.spawn` with the replaying flag, `h2c.publish`, `h2c.checkpoint`, `c2h.state`. | Held effects (`ta_activation_effects`, about 300 lines); the kernel's recorder and replay | input observations on c2h |

**Exit:**
- No vtable remains in core except the data model's, the value vtable,
  byte sinks and sources, and the chart codec.
- The test host's scenarios include S01 to S13 as ring scripts.
- `mise run test:full` passes.

## Phase 6: delete the kernel

| Step | Change |
|---|---|
| 6.1 | napi drives core's rings directly: the pump loop in [ARCHITECTURE.md](ARCHITECTURE.md#the-host-loop). Its `index.d.ts` changes as [migration.md](reference/migration.md#napi) lists, with `// status:` markers where the implementation lags. Coordinate with the napi rewrite. |
| 6.2 | Move OTLP into `tinyactors-otlp`, a small C library that reads `tel` ([telemetry.md](reference/telemetry.md#otlp-tinyactors-otlp)). napi and the test host link it, and `TAH_PARAM_TRACE_STAGING_BYTES` becomes its own setting. Every span ID it or napi's `lib/tracing.js` computes comes from core's `ta_step_key_span_id`, or is tested against the values `tests/hash_test.c` pins, so IDs never drift from core's. Move the counting allocator, metrics and definitions helpers into napi. Delete what napi does not use. |
| 6.3 | Delete `tinyactors-host`: its sources, tests, mise tasks, the root `mise.toml` entries, and the rows in AGENTS.md's test table. Mark kernel ADRs 0001 to 0006 superseded. The MCP server is not part of this plan. It is an experimental feature with its own proposal ([mcp-server.md](../mcp-server.md)): a `tinyactors-mcp` library beside `tinyactors-otlp`, built on the rings once Phase 6 lands. |

**Exit:** `mise run test --all` and `mise run test:full` pass with no
kernel. The napi suites listed in migration.md are rewritten against the
rings.

## Phase 7: close

- Walk the removal table and the duplication summary in
  [building-blocks.md](reference/building-blocks.md) once more: every row
  is gone or deliberately kept.
- `mise run check-api-status` has no stale markers.
- Run the out-of-memory sweep over creation, registration, spawn, load
  and refill only. A sweep over macrosteps has nothing left to fail.
- Final `bench-compare` against the commit before Phase 1.
- **Core is smaller than before Phase 1:** count `tinyactors-core/src/`
  without `src/generated/`, code and comments separately, against the
  commit before Phase 1. At the end of Phase 1, code was 18,424 lines
  against 17,588 before it, and comments 5,975 against 4,070; the
  comments grow on purpose, as documentation moves into headers.
- Changelogs come at release time, through the `release-changelog` skill,
  not as part of this work.

## What runs where, by phase

| Phase | Core tests | Test host | napi | Kernel |
|---|---|---|---|---|
| 1, 2, 3 | new block tests; existing suite | unchanged | unchanged | unchanged |
| 4 | pool, timer, capacity and slot tests; out-of-memory sweep shrinks | through the kernel | through the kernel; quarantine removed | changed in place each commit |
| 5 | ring script tests (E3) | **ring driver**; S01 to S13 | through the kernel, which drives the rings | shrinks each step |
| 6 | – | – | **drives the rings** | deleted |

## Running the work

### The critical path

Most of the plan can run side by side. One chain cannot, because each link
changes the interpreter, the actor record, the system and the kernel that
the next link builds on:

```
1.0 ─▶ 1.5 ─▶ 1.6 ─▶ 4.2 ─▶ 4.3 ─▶ 4.4 ─▶ 4.5 ─▶ 5.1 ─▶ 5.3 ─▶ 5.5 ─▶ 6.1 ─▶ 6.3
```

4.2 builds the pool on A5 (1.5) and the `h2c.withdraw` index on B3 (1.6).
Everything else is a side lane that must land before the link that needs
it: 4.2 also needs A6 (2.4), B1, C3, the name resolver and Phase 3. The rule for scheduling is: keep the critical path busy, and use the
other lanes to have its inputs ready before it needs them.

### Lanes

| When | Lane | Steps | Waits for | Touches |
|---|---|---|---|---|
| first, alone | foundation | 1.0 | – | every module's includes, `internal.h`; nothing else can land cleanly beside it |
| Phase 1 | tables | 1.5 → 1.6 | 1.0 | actor, atom, trie; the address and session tables keep linear hashing |
| Phase 1 | helpers | 1.1, 1.2, 1.4, then 1.7 | 1.0 | hash and format helpers, handles, layouts, invoke and export arrays; 1.1 also edits `session.c`, so it lands before 1.5 or rebases onto it |
| Phase 1 | tests and dead code | 1.3, 1.8 | 1.0 | core's tests, `fault.h`, `configuration` |
| with Phase 1 | free blocks | 2.1, 2.2, 2.3, 2.8, 2.10 | 1.0 | new units only |
| with Phase 1 | generator | 2.9 | 1.0 | `protocol/`, a mise task, `src/generated/`, `docs/build/` |
| after its inputs | dependent blocks | 2.4 (needs A5), 2.7 (needs B3) | 1.5, 1.6 | new units |
| with Phase 1 | names and images | 2.5, 2.6 | 1.0 | new units; 2.5 moves sinks out of `session_image.c`; 2.6 uses today's `ta_atom_find`, whose storage 1.6 changes underneath it |
| beside Phase 2 | matching | 3.1, 3.2 | 2.6 | the matcher's input; lands before 4.2 |
| beside Phase 3 | telemetry | 4.1 | 2.8, 2.9 | `trace.c`, the kernel's OTLP adapter |
| right after 4.1 | OTLP library | 6.2's `tinyactors-otlp` | 4.1 | a new package; the kernel links it until Phase 6 |
| Phase 4 | core | 4.2 → 4.3 → 4.4 → 4.5 | Phase 3, Phase 2 blocks | interpreter, actor, system, the kernel; one lane, serial |
| Phase 5 | driver | 5.1 | 4.5 | the test host becomes a ring driver |
| after 5.1 | scenarios | S01 … S13 as ring scripts, one per lane | 5.1 | one test file and one walkthrough each |
| after 5.1 | effects, definitions | 5.2 and 5.4, side by side | 5.1 | I/O and invoke code; builder and definition code |
| after 5.1 | images | 5.3 → 5.5 | 5.1 | session images, actor records, the recorder; serial because both change images |
| Phase 6 | napi | 6.1 | Phase 5 | napi, with the session rewriting it |
| last | removal | 6.3 | 6.1, 6.2 | deletes the kernel |

Two moves shorten the critical path most. **4.1 and `tinyactors-otlp`
run before Phase 4**, so 2,300 lines of encoder move while the
interpreter work has not started. **The free blocks and the generator run
during Phase 1**, so Phase 2 is mostly done when the tables land.

Work that lands in the same commit as a critical-path step is prepared
beside it but never lands alone: the jq, quickjs and napi data models'
change from `TA_OP_NOMEM` to `TA_OP_ERROR` lands with 4.4, as every API
change lands with its callers.

### How parallel lanes land

The repository has no feature branches: every lane commits to `main`. A
lane works in its own worktree, rebases onto `main` before each commit,
runs `mise run test`, and lands small commits. A lane owns its files;
two lanes that need the same module run one after the other.

Three places conflict often and are handled the same way each time:

- **`src/internal.h`** lists units in amalgamation order. A rebase
  re-applies the lane's lines in the group order of the source tree.
- **`protocol/catalog.rb` and `src/generated/`.** Only the catalog is
  merged. The generated files are rendered again after the rebase, never
  merged by hand.
- **The kernel during Phases 4 and 5.** Every critical-path step changes
  it in place. Side lanes do not touch it.

### Where oversight is needed

The automated checks cover most of the risk: `mise run test`,
`mise run test:full`, the sanitizer builds, the acronym and portability
checks, `check-api-status`, the header-comment rule, the core test that an
allocator refusing every request after a macrostep starts never sees a
call, and the IRP suites. These gates need a person as well:

| Gate | Who | What they check |
|---|---|---|
| Each ADR (Phase 0, just before the phase that needs it) | the maintainer | the decision and its figures; the ADR is the decision, not the proposal |
| Public API changes in `tinyactors.h` | the maintainer | names, ownership, failure codes; the `// status:` markers napi needs |
| 2.9, the generator and its build task | the maintainer | the rules in `docs/build/`; nothing host-specific outside `mise/lib/toolchain/` |
| Each commit of 4.2 to 4.5 | the maintainer, reading the header comments first | the macrostep invariants hold; `test:full` passed and the message says so; IRP results unchanged |
| Phase 4's and Phase 7's benchmarks | the maintainer | `bench-compare` with throughput, memory and the machine signature; per-actor bytes before and after; whether a regression is accepted |
| 5.1, the test host becoming a ring driver | a reviewer who did not write it | every scenario property the old driver checked is still checked; nothing is weakened or dropped to make a scenario pass |
| Removals that supersede ADRs (5.x, 6.3) | the maintainer | the ADR's status line, and that nothing still depends on what goes |
| 4.5 and 6.1 in napi | the session rewriting napi | which napi files change, and when |

### Who does which work

- **Suited to agents, with an ordinary review:** the Phase 2 blocks (each
  has a contract in `building-blocks.md`, no callers and its own tests),
  the Phase 1 refactors (the existing tests are the oracle), folding
  `docs/internal/` documents into headers, the scenario ring scripts (the
  step IDs name the tests), and moving the OTLP encoder.
- **Agents with close review:** 1.0 (wide but mechanical), 2.9, Phase 3,
  and 5.2 to 5.5.
- **Led by a person, agents assisting:** the Phase 0 ADRs, 4.2 to 4.5,
  5.1's change of oracle, and 6.1 with the napi session.

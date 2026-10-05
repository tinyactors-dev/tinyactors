# Implementation: replacing core's plumbing with building blocks

**Status:** Sketch (2026-10-03), part of [the ring protocol proposal](README.md).
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
- **Do not refactor code that a later phase deletes.** This covers
  `timer.c`, the event blocks, `io.c` / `io_origin.c` and the kernel's
  containers. Phase 1 leaves them alone.
- **Scope commits by component:** `refactor(core)`, `feat(core,host,napi)`,
  `refactor(core,napi)!`. A breaking change's body says what a caller does
  now. The changelogs are written from these subjects at release time,
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
   h2c** (refines ADRs 0016 and 0068). Each ADR can be written just
   before the phase that needs it, not all up front.
2. **The repository ADR** that removes `tinyactors-host`, and the **napi
   ADR** that makes napi a binding over rings, before Phase 6.
3. **The proposal's status** changes to *Adopted → links*. The decisions
   then live in the ADRs, and the steps below move into the root
   `ROADMAP.md`.

Questions that must be answered before the phase that depends on them:

| Open question | Blocks |
|---|---|
| Size of the mandatory internal-event arena ([macrostep.md](reference/macrostep.md#sizing-mandatory-storage)) | 4.4 |
| Fairness: is a ratio enough? ([migration.md](reference/migration.md#open-questions)) | 4.2 |
| Default sizes for the pool, the name area and the rings | 4.2, 5.1 |
| The exact encoding of definition specs and diagnostics ([definitions.md](reference/definitions.md)) | 5.4 |
| A home for the MCP server mapping, now that the kernel goes | 6 |

## Phase 1: turn the duplicates into blocks

**No behaviour changes**, so no ADR is needed. Each step is a refactor
that the existing tests cover, and each makes core smaller. Every block
gets its own test program in `tinyactors-core/tests/`.

| Step | Block | Replaces | Notes |
|---|---|---|---|
| 1.1 | D5 hash functions, D6 formatters | The Murmur finalizer duplicated in `session.c:4` and `trace.c:34`; FNV-1a in 32-bit and 64-bit forms; three decimal formatters, four hex helpers, two `cstr` helpers | One header. Trace and span IDs must stay identical: `span_test` covers them. |
| 1.2 | E1 generation handles | The hand-packed definition IDs (`system.c:456`), and `ta_actor_id_make` as the only shared helper | |
| 1.3 | E2 failing, counting test allocator | Nine hand-rolled copies in core's tests | It lives with the tests and is never amalgamated into the library. |
| 1.4 | A2 layout builder | Six layout calculators | Checked arithmetic. Phase 4's workspace sizing depends on it. |
| 1.5 | A5 index pool | The paged directories of the actor, address and session tables | Not the timer heap or `io_origin`: they are deleted later. A5 needs `refill(min_free)` and a ceiling from the start, even though only the pool uses the ceiling. |
| 1.6 | B3 hash table, D7 trie of atoms | The atom table, the event-name trie, and the address and session tables | Two modes: fixed, and growing on an explicit call. The trie becomes a named block on B3. **Done** for all four tables: atoms and the trie (1.6), then addresses and sessions on B3 and the A5 pool (1.6b), which also adds `ta_table_prepare` (an insert that finds its home page full grows the page at the safe point instead of answering `TA_ERR_NOMEM`, so crafted names cannot fail a spawn or a load) and drops `ta_actor.session_node` and the address record's hash copy to pay for the directory. |
| 1.7 | B6 | Hand-rolled doubling arrays that survive: the invoke table and export blocks | Skip the timer lists and the event-block queue. |
| 1.8 | – | Dead code: `fault.h`'s undefined declarations, unused `ta_configuration` kinds, stale comments (`interpreter.h:143`, `actor.h:287`) | |

**Exit:** the duplication summary in the catalog is resolved, except for
code that later phases delete. `mise run test:full` passes. Core has fewer
lines than before.

## Phase 2: add the new blocks, unused

Each block comes with its own tests and no caller yet. None of them needs
the interpreter. A block may be committed before anything uses it,
because its user follows within the same phase plan. That is the
exception to "remove what nothing uses".

| Step | Block | Notes |
|---|---|---|
| 2.1 | B1 intrusive index list | Two link fields per item (`next`, `owner_next`). Has `splice` and `unlink`. |
| 2.2 | B2 indexed min-heap | Keyed by `(due, order)`, with a back-index stored in the item, and `remove(i)`. |
| 2.3 | B4 fixed-entry FIFO, C3 quota with reservations | The internal queue is B4 plus two C3 quotas. |
| 2.4 | A6 chunk chain, over A5 | Parametrized by chunk size (64 bytes for names, 256 for parking). Has `need(n)` for preflight. |
| 2.5 | D1 byte sinks and sources | Moves `ta_arena_sink` and `ta_memory_source` out of `session_image.c`, and adds a sink and a source over a chunk chain. |
| 2.6 | D4 name resolver | Splits a name at the dots and looks each segment up with `ta_atom_find`; scratch holds `eventNameDepth` segments. |
| 2.7 | B7 type registry | Built-in entries and their aliases, `declare`, `declared`. |
| 2.8 | C1 SPSC byte ring, and the control block | Lossless and lossy modes, pads, seq. Tests cover a wrap on every offset, a ring of `maxRecordBytes`, and lossy drop counting. |
| 2.9 | C2 record codec generator | Move `protocol/` (the DSL, the catalog, the renderer) from this proposal into `tinyactors-core/protocol/`. Add a mise file task that generates C writers and validating readers into `src/`, and keep generating `MESSAGES.md` and `messages.json`. The reader tests feed malformed records: a span outside the record, a wrong size, an unknown kind, a seq gap. **Done**: the generator is `tinyactors-core/protocol/codec.rb`, run by `mise run protocol` in `tinyactors-core` (`--check` verifies the committed output). It emits `src/ring_proto.{h,c}` (per message: the wire struct, a data struct, `size`, `write`, `get`, and one static descriptor table) and `tests/ring_proto_cases.h`. The one place hostile bytes are judged is hand-written: `src/record.{h,c}`, `ta_record_check`, driven by the descriptor table, which copies the fixed part into a snapshot before validating it. Tests: `record_test.c` (the checker against a hand-built table) and `ring_proto_test.c` (every message round-trips through a real ring, every single-field corruption is refused with its reason, and a fuzz loop of 20,000 inputs in the fast pass and 5,000,000 in the exhaustive one runs under AddressSanitizer). Nothing uses the codec yet; step 4.1 writes `tel.trace` with it. |
| 2.10 | C4 claim set (optional) | Reserves across C3 quotas, all or nothing. |

**Exit:** every new block has its own test program, and the core `test`
task's sources include them.

## Phase 3: match names by lookup

[names.md](reference/names.md) splits names into authored names, interned
at registration, and run-time names, kept as bytes. **Matching** can
change on its own now. **Storing** run-time names as bytes needs the name
area, so it lands in 4.2.

| Step | Change |
|---|---|
| 3.1 | When an event is taken, resolve its name with D4 into atoms, where a segment that is not an atom is UNKNOWN. Run the existing matcher over the result. Add the generated prefixes (`done.state`, `error.execution`, ..., and the `invoke` segment) as atoms at system creation. |
| 3.2 | Tests: the worked examples in names.md as a core test, plus a test that random names leave the atom table and the trie unchanged. |

**Exit:** matching gives the same answers through lookup. The IRP tests in
the test host and napi still pass.

**Done in 3.1 and 3.2 (branch `wip/ring-proto/3.1`).**

- The generated prefixes were already atoms: `ta_event_name_table_init`
  interns `error`, `execution`, `communication`, `platform`, `done`, `state`
  and `invoke`. The table now keeps the `invoke` atom (`invoke_segment`) and
  `invoke.c` reads it instead of interning the literal. No other prefix is
  generated (searched `src/`).
- `ta_event_pattern_matches` takes the resolved atoms (`ta_event_atoms`). The
  interpreter resolves once per taken event in `ta_interpreter_resolve_event`
  (the one function 4.2 changes). Until the name area exists there are no
  name bytes, so it takes the atoms from the trie node's own path
  (`ta_trie_segments`) and does not call D4: **core does not exercise
  `ta_name_resolve` in 3.1** (only `name_resolver_test` and
  `event_match_test` do). Rendering and re-splitting would break invoke IDs
  such as `s3.7`, one atom that contains a dot. 4.2 swaps the source for the
  envelope's bytes and `ta_name_resolve`. What 3.1 saves today is one trie
  walk per descriptor per candidate transition.
- Scratch: `ta_interpreter.event_atoms` holds eventNameDepth atoms, taken in
  `ta_interpreter_init` and counted by `ta_interpreter_scratch_need` in the
  same position. Every chart's scratch need grows by 4 * eventNameDepth bytes
  (256 at the default); `layout_test`'s golden figures moved by that.
- Tests: `tests/event_match_test.c` (new), `event_pattern_test.c` migrated.
  `mise run asan` in tinyactors-core: 39 programs pass. Test host scenarios
  (`--fast`): 449 passed. napi IRP on Node: 176 run, 176 passed. Bun does not
  get the addon's native surface in this container (`native.parameterTable` is
  undefined in `lib/params.js`), before any core code runs.
- Core size (`cat tinyactors-core/src/*.c tinyactors-core/src/*.h | wc -l`):
  26672 before, 26771 after.
- Per-event cost, `mise run bench-compare ring-proto --runtimes=node --runs=3`
  (machine 61a66aecf132, medians of 3 alternating runs). The A/A run on one
  commit moved between -10.9 and +6.9 percent. Untraced 1,413 to 1,479 ns
  (+4.6), summary 2,490 to 2,587 (+3.9), send data without data 2,608 to
  2,894 (+11.0; the A/A runs of that bench read 2,902 and 2,837, so the old
  run was the fast one), number +5.5, object -1.2. Per-actor memory (core,
  native, heap B/actor) is unchanged in every variant. Nothing here is
  outside the noise.

## Phase 4: core gains its queue

The kernel stays and keeps napi and the test host working. It is changed
in each commit to call core's new API. Steps 4.2 to 4.5 are the expensive
ones; run `mise run test:full` before each commit.

| Step | Change | Deletes | ADR |
|---|---|---|---|
| 4.1 | **`tel`.** Allocate the control block and the `tel` ring at system creation. Trace records become `tel.trace`, and the kernel's OTLP encoder drains `tel`. This is the smallest real user of a ring. | The trace sink vtable and `ta_null_trace_sink` | telemetry is a lossy ring |
| 4.2 | **The pool and `ta_run`, with one slot.** Envelopes (A5), the run queue and the outgoing list (B1), the name area (A5 + A6), the admission watermark and the macrostep reserve (C3). Local mail becomes envelopes. `ta_actor_deliver` becomes admission into the pool, changed in place. `ta_run` replaces `ta_actor_resume`. Run-time names become bytes in the name area, completing Phase 3. Recycling turns undeliverable mail into `error.communication`. | The router vtable, `TA_ERR_BUSY`, the event blocks and their cache (`TA_PARAM_EVENT_BLOCK_CACHE`), run-time interning (`ta_send_id_intern`, `ta_system_intern_invoke_id`, the `eventexpr` and `typeexpr` sites, `invoke.c`'s error names) | pool; only authored names interned |
| 4.3 | **Delayed sends as envelopes.** The deadline index (B2) and the owner lists (B1). `ta_run` reports the next due time; that value becomes `c2h.wake` in 5.1. `<cancel>` walks the owner list. | `timer.c` / `timer.h` (634 lines), sparse state, `ta_system_dispatch_due` | delayed sends as envelopes |
| 4.4 | **No allocation in a macrostep.** The internal queue becomes B4 with an ordinary and a mandatory quota (C3). The compiler sizes mandatory storage. Invocation startup moves after the macrostep, with the local child's lifetime token. Capacity failures become `error.execution.capacity.<resource>`. Data models answer `TA_OP_ERROR` instead of `TA_OP_NOMEM`, which changes the test host's jq and quickjs data models and napi's data model. A core test asserts that an allocator which refuses every request after a macrostep starts never sees a call. | The out-of-memory stop machinery: the NOMEM branch, `ta_reserve_stop` / `ta_stop*` where only out-of-memory uses them, the spare continuation and spare sparse state, queue mark/truncate, `TA_OP_NOMEM` | no allocation in a macrostep |
| 4.5 | **Macrostep slots.** Slot records (A5), deferred lists (B1), `slotSlice`, the chain limit, and parking into the park area (A5 + A6 + D1 + D2). Start with parking disabled (`parkAreaBytes` 0, plain run to completion), then enable it in its own commit. | napi's quarantine, `release()` and the `.quarantined` event (napi call sites only) | macrostep slots |

**Done in 4.2a (branch `wip/ring-proto/4.2a`).** Step 4.2 is split in three:
4.2a (this note), 4.2b (the name area and the end of run-time interning) and
4.2c (recycling, local mail as envelopes, and the removal of the router
vtable and the event blocks).

- Core: `src/envelope.{h,c}` (the pool of 168-byte envelopes, the run queue,
  the quotas and the waiting table) and `src/run.{h,c}` (`ta_run`, admission,
  the started queue). `ta_actor_deliver` is admission; `ta_actor_transition`
  splits out the operator's event; `ta_run` replaces `ta_actor_resume`;
  `TA_ERR_FULL` is new; `TA_ERR_RUNAWAY` and `ta_actor_resume_params` are
  gone; a runaway input is abandoned with status `runaway` (ADR 0103
  section 4). Parameters `poolEnvelopes`, `admissionWatermark` and
  `macrostepReserve`. Tests: `envelope_test`, `run_test`, and the other
  core programs through `tests/run_support.h`.
- Callers changed in the same commits: the kernel (`tah_sched_run`, the
  notices, the turn bounded by inputs), the test host (Ruby scheduler,
  scenarios), and napi's call sites. With one slot a paused input holds the
  slot and other actors' inputs wait behind it, so two napi tests and the
  test host scenarios that assumed interleaving were rewritten for it;
  interleaving returns with slots in 4.5.
- Not done in 4.2a: local mail as envelopes (the mail of a macrostep still
  reaches `ta_actor_deliver` through the external event router), recycling
  and `ta_address_load_failed`, the name area, the event blocks and their
  cache, the router vtable, and `bench-compare` against the commit before
  4.2 (it belongs in the 4.5 commit).

**Exit:**
- The kernel's scheduler holds no mailboxes, only the code that admits
  input.
- `mise run test:full` passes.
- `bench-compare` against the commit before 4.2 is recorded in the 4.5
  commit or its ADR, including the per-actor bytes, before and after.

## Phase 5: rings replace the vtables

`h2c` and `c2h` arrive together, because inputs need their dispositions.
The test host becomes a ring driver here rather than at the end. From then
on its scenarios check the protocol directly.

| Step | Change | Deletes | ADR |
|---|---|---|---|
| 5.1 | **h2c and c2h for inputs.** `h2c.event`, `h2c.time`, `h2c.spawn`, `h2c.destroy` and `h2c.withdraw`. On c2h, `c2h.bounced`, `c2h.input_refused`, `c2h.input_completed` (with `ignored`), `c2h.spawned`, `c2h.macrostep_ended`, `c2h.wake`, `c2h.finished` and `c2h.destroyed`. Core reserves room for mandatory records before taking an input. **The test host becomes a ring driver**: `hosted.post`, `hosted.turn`, `hosted.batches`, decoded with `messages.json`. Add the E3 ring script driver to core's tests. | `ta_actor_deliver`, the native spawn, time and destroy entry points; the kernel's admission code | three rings |
| 5.2 | **Effects on c2h.** `c2h.send`, `c2h.invoke`, `c2h.invoke_cancel`, `c2h.invoke_forward`, `c2h.log`, and `h2c.invoke_report` coming back. Two B7 registries, filled by `h2c.type_declare`. Bindings render `_ioprocessors` locations themselves, and origins are text. | The I/O, invoker and log vtables; `ta_system_register_io`, `ta_system_find_io`, `ta_system_register_invoker`; `io.c`, `io_origin.c`, the scope API, `TA_DM_TEXT_IO_*`; the kernel's adapters and effects queue | type registries through h2c |
| 5.3 | **Unloading and loading.** Waiting lists keyed by address (B3 fixed → B1), `c2h.load_needed`, `h2c.load`, `h2c.load_failed`, `c2h.load_result`, `h2c.unload`, `c2h.unloaded`, `c2h.loaded`. Images go through D1 and D2 into host blobs. | The directory vtable; the kernel's loads, offloads and directory | – (refines 0037) |
| 5.4 | **Definitions through h2c.** `h2c.definition_register`, `h2c.definition_release`, `h2c.definition_retire`, `c2h.definition_result`, with bounded diagnostics. | The native definition-management entry points that bindings no longer need; `ta_definition_sink` if nothing uses it | – (refines 0058, 0077, 0079) |
| 5.5 | **Recording and replay.** The `recorded` bit, `c2h.input`, `h2c.spawn` with the replaying flag, `h2c.publish`, `h2c.checkpoint`, `c2h.state`. | Held effects (`ta_activation_effects`, about 300 lines); the kernel's recorder and replay | input observations on c2h |

**Exit:**
- No vtable remains in core except the data model's, the value vtable,
  byte sinks and sources, and the chart codec.
- The test host's scenarios include S01 to S13 as ring scripts.
- `mise run test:full` passes.

## Phase 6: delete the kernel

| Step | Change |
|---|---|
| 6.1 | napi drives core's rings directly: the pump loop in [ARCHITECTURE.md](ARCHITECTURE.md#the-host-loop). Its `index.d.ts` changes as [migration.md](reference/migration.md#napi) lists, with `// status:` markers where the implementation lags. Coordinate with the napi rewrite. |
| 6.2 | Move OTLP into a small library that reads `tel`, or into each binding, as [telemetry.md](reference/telemetry.md) leaves open. Move the counting allocator, metrics and definitions helpers into napi. Delete what napi does not use. |
| 6.3 | Delete `tinyactors-host`: its sources, tests, mise tasks, the root `mise.toml` entries, and the rows in AGENTS.md's test table. Mark kernel ADRs 0001 to 0006 superseded. |

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
- Changelogs come at release time, through the `release-changelog` skill,
  not as part of this work.

## What runs where, by phase

| Phase | Core tests | Test host | napi | Kernel |
|---|---|---|---|---|
| 1, 2, 3 | new block tests; existing suite | unchanged | unchanged | unchanged |
| 4 | pool, timer, capacity and slot tests; out-of-memory sweep shrinks | through the kernel | through the kernel; quarantine removed | changed in place each commit |
| 5 | ring script tests (E3) | **ring driver**; S01 to S13 | through the kernel, which drives the rings | shrinks each step |
| 6 | – | – | **drives the rings** | deleted |

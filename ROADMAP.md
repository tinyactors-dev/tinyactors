# Roadmap

Work across packages, in the order it is meant to happen. Each package's
own `ROADMAP.md` holds its detailed steps; ideas that are not decided yet
are in [`docs/proposals/`](docs/proposals/README.md). How the layers divide
the work is in the [README](README.md#layers-and-responsibilities).

## The ring protocol

Adopted 2026-10-05: core talks to its host through three rings, core
holds its pending inputs in one pool, and the host kernel goes. The
decisions are core ADRs 0098 to 0105, repository ADR 0003 and napi ADR
0041; the plan, its rules and what runs where are in
[IMPLEMENTATION.md](docs/proposals/ring-proto/IMPLEMENTATION.md).

- [x] **Phase 1, the duplicates become blocks:** one unit per object
  (1.0), hashes and formatters (1.1), generation handles (1.2), the test
  allocator (1.3), the layout builder (1.4), the index pool under the
  actor table (1.5), the hash table under atoms and the trie (1.6); 1.7
  found nothing to fold, 1.8 removed dead code. Core and native bytes per
  actor are unchanged (`bench-compare`, recorded in the plan).
- [x] **Phase 2, the new blocks, no callers yet:** the intrusive list,
  paged array, heap, FIFO, quota, byte ring, record codec and its
  generator, byte sinks and sources, name resolver, chunk chain and type
  registry. The claim set (2.10) waits for a user.
- [x] **Phase 3, names are matched by lookup:** the matcher runs over a
  name's atoms; `typeexpr` lookups no longer intern.
- [ ] **M1, core runs inputs safely (Phase 4):** `tel` and the host clock
  (4.1, done: untraced cost unchanged, tracing +13 to 22% per event, ADR
  0098's notes), the pool and `ta_run` with one slot (4.2, done: hosts
  pass and read names as text, the router is gone, the ring defaults
  measured by proxy in ADR 0099's notes), delayed sends as envelopes
  (4.3, done: names of delayed and I/O sends stay text), no allocation in
  a macrostep (4.4, in progress: what a macrostep needs is allocated at the
  take; the stop machinery, invocation startup after the macrostep and the
  internal-queue runaway remain), macrostep slots (4.5). The kernel, the test host's
  hosted mode and napi's native layer were deleted on 2026-10-06; until
  M3 the test host is the only host.
- [ ] **M2, core speaks only rings:** inputs (5.1, the test host becomes
  the reference host and drives core through the rings), effects (5.2),
  definitions (5.4), unloading and loading (5.3), recording and replay
  (5.5), and S01 to S13 as ring scripts.
- [x] **`tinyactors-otlp`:** the OTLP encoder reads `tel` in its own
  package.
- [ ] **M3, napi on the rings:** napi's native layer rewritten from
  scratch until `lib/` works and its suites and IRP pass again.
- [ ] **M4, finale:** `test:full`, a review of the whole range, the IRP
  diff, core smaller than before Phase 1, benchmarks and tuning.

## Done recently

- **Actors record their events through the application (2026-10-01)**,
  [`PLAN.md`](PLAN.md) phase K, kernel ADR 0006: the runtime records the
  events an actor takes (`setRecorder()`, once per turn, as `ActorEvent`s
  with their times, after their macrosteps end) and the application
  journals them; `prepare({ at })`, `replay(event, at)` and `publish()`
  rebuild an actor with nothing crossing until it is published. The input
  log is gone from the kernel, napi and the test host (about 11,000
  lines), with the write-ahead hold, checkpoints and `send()`'s `until`.
  Delivery is at least once where someone retries; a delayed send to
  another target may repeat once on a rebuild, and its firing moves the
  sender's wake. `test:full` passed; `bench-compare 5164e0f`: a recorded
  message costs 3,039 ns on Node and 1,916 on Bun against the input log's
  5,718 and 4,537, with 30% less native memory per actor.

- **A macrostep's first record commits with what it sent (2026-10-01)**,
  [`PLAN.md`](PLAN.md) phase J, kernel ADR 0004: `delivered`, `jump`,
  `released`, and `spawned` for a bootstrap, are held and written in the
  batch of the turn the macrostep ends in, so a crash between two commits
  no longer loses an immediate send between persisted actors, from a
  delivery or from a bootstrap. `evict()`, `withdrawSend()` and a
  checkpoint answer `busy` inside a logged actor's macrostep, and a replay
  publishes at the latest record time.
  - **Decided: a macrostep paused across turns may repeat what it sent
    before the pause, after a crash** (at least once for those sends and
    effects, once per crash, in order; nothing lost, and macrosteps within
    the budgets stay exactly once). A repeat carries the same origin, and
    the same send ID when the `<send>` has an `id`. Holding such sends
    until the macrostep ends was built and dropped: it would stop a long
    `<foreach>`'s messages from streaming. napi ADR 0033's section 7 is
    reworded, and napi's README has "Crashes and repeated messages".
  - `test:full` passed; `bench-compare a42f5f6` is within 5% for
    throughput on Node and Bun, and a logged actor's native memory is
    +109 B per actor in the input-log bench (the held records at the peak
    of open macrosteps; ADR 0004's notes).
- **Mail between actors is never refused for a full mailbox (2026-10-01)**,
  [`PLAN.md`](PLAN.md) phase I, kernel ADR 0003: past `maxQueuedMessages`
  or `maxRetainedBytes` the router accepts it into its receiver's
  overflow and the receiver's log records it, so a replay from `spawned`,
  an eviction and exit, or a crash while the sender is evicted no longer
  loses it. The overflow is counted (`stats().messages.overflow`, the
  `tinyactors.messages.overflow` gauge, `hosted.mailboxes`). Core ADR
  0095's restore of refused sends is removed, since only out of memory
  makes one now; the fourth replay timeline crashes both actors and
  delivers once.
- **The ROADMAP's follow-ups, closed (2026-09-30)**, commit by commit as
  [`PLAN.md`](PLAN.md) lays out:
  - **Replay is tested natively in the kernel**
    (`tinyactors-host/tests/replay_test.c`): prepare, run and redelivery; a
    checkpoint round trip and life boundaries; core ADR 0089's four
    timelines; an out-of-memory sweep over replay (every allocation in
    `test:full`, sampled in the fast pass). It found four kernel bugs,
    fixed on the way: a checkpoint cut after its first record, a prepared
    actor orphaned by a failed bootstrap, a message queued without its ID,
    and an actor left with its mail when its requeue failed (kernel ADR
    0002's notes).
  - **Checkpoints restore the sends the router refused** (core ADR 0095);
    invocations still carry no values, since nothing reads them.
  - **The kernel's headers are opaque:** 50 accessors, the state in
    `src/<module>_private.h`, and `mise run check-opaque` (kernel ADR 0001,
    "Opaque headers"). `bench-compare` within 5% on Node and Bun.
  - **The test host's hosted mode is finished:** allocators, mailboxes and
    dead letters, loaders, a log that cannot grow, values written in pieces
    (kernel ADR 0001, "Hosted mode finished").
  - **Decisions recorded:** a pending `send()` does not hold the process
    (napi ADR 0007); the trace staging bound is the bound plus one call
    (napi ADR 0036); a logged input costs +3.2 µs on Node and +2.5 µs on
    Bun at log version 2, and no native durable writer is built (napi ADR
    0034).
  - **Smaller fixes:** `parameterInfo()` gives the kernel's parameters
    `owner: "kernel"`; the moved napi ADRs point to the kernel; the
    scenario runner builds first; `test:full` reruns every exhaustive pass.
- **Granary's feedback (2026-09-30):** typed event maps,
  `statechart<Data, Events>()`; a reply-to-sender target, `toOrigin()`;
  percent-encoded names in target URIs (core ADR 0096) and `actorURI()`.
- **The rest of napi's JavaScript API (2026-09-30):** event data leaving a
  session is a copy, as SCXML C.1 requires (core ADR 0097, napi ADR 0039;
  the cost is accepted, to be optimized later), and `effect()` names an
  effect sent through the `effects` I/O processor (napi ADR 0038).
- **The DAP adapters are removed (2026-09-30),** ahead of
  [the MCP server proposal](docs/proposals/mcp-server.md): napi's
  `createDebugSession`, the test host's `bin/dap`, the shared DAP
  transcripts and tinyactors-vscode. The core features they needed stay
  (core ADRs 0082 to 0086).
- **The host kernel (2026-09-30).** `tinyactors-host/` holds what every
  host needs: scheduling, mailboxes, the router, the directory, loads,
  offloads, the OTLP encoder, and the write-ahead log.
  - It is libc-free C: `check-freestanding` builds it for
    `wasm32-freestanding`, and platform needs are one interface
    (`tah_platform`) each binding implements.
  - napi and the test host (hosted mode) both run on it.
  - Core no longer contains an input log (core ADR 0094), and every input
    carries its time.
  - The log is at version 2, where a message is logged once (`accepted`,
    then `delivered` or `dropped`).
  - `test:full --all` passed, and `bench-compare 3a1eaa1` is within 5% on
    Node and Bun.

  Decisions: kernel ADRs 0001 and 0002, core ADR 0094, napi ADR 0037, with
  every step in their implementation notes.
- **Earlier on the same path:**
  - session IDs that name their family (core ADR 0088);
  - replaying actors (core ADR 0089);
  - the input log and checkpoints (core ADRs 0090 and 0091, napi ADRs 0032
    to 0034 and 0036);
  - system parameters (core ADRs 0092 and 0093, napi ADR 0035).

## Follow-ups

Gaps found while closing the ones above. None is a regression.

- [ ] **Out of memory can still lose a send between actors:** a router
  RETRY leaves it submitted in the sender, recorded nowhere (kernel ADR
  0003, section 3, accepted for now).
- [ ] **Events core queues because of the outside world are not
  recorded:** an `error.communication` for a send the router refuses for
  good inside a macrostep, or for a delayed send that fails when it
  fires. Accepted for now under fire and forget (kernel ADR 0006's notes);
  open for the owner.
- [ ] **Operator actions are not in the journal** (a jump, release,
  abandon, withdrawal): an application snapshots an actor it operated on.
  The snapshot in the recorder's batch is not built (kernel ADR 0006,
  section 8).
- [ ] **Core's `durable` value flag has no caller:** remove it from core's
  data-model contract and the test host's jq model (kernel ADR 0006's
  notes).
- [ ] **The recorder costs +8% on Node for messages without data**
  against the old input log (an `ActorEvent` object per record); with
  data it costs half (kernel ADR 0006's notes).
- [ ] **Overflow has no cap,** and `maxWaiting` still dead-letters mail
  between actors for a loading address (`waiting-full`), and the SCXML
  invoker's forward to a child it started is still refused at the limits
  (kernel ADR 0003, sections 3 and 4).
- [ ] **Make napi's copy of sent data cheaper:** +11 to 14% for `{ by: 1 }`
  and +36 to 40% for nested containers (napi ADR 0039's notes).
- [ ] **A chart that uses `toOrigin()` cannot be written out as SCXML:**
  napi's source-text expressions do not read `_event.origin` and
  `_event.originType`.
- [ ] **The kernel's public headers still declare functions a binding
  cannot use** without the private layout (`tah_sched_init`,
  `tah_loads_find`, ...); move them to the private headers.

## Open

- [ ] **Bootstrap a browser build.** Core and the host kernel compiled to
  WebAssembly, with a JavaScript surface shared with napi. Both already
  build for `wasm32-freestanding`; no build task or test runs them in a
  browser yet. The binding implements `tah_platform` and a pump driver on
  `MessageChannel` and `setTimeout`.

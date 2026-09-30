# Roadmap

Work across packages, in the order it is meant to happen. Each package's
own `ROADMAP.md` holds its detailed steps; ideas that are not decided yet
are in [`docs/proposals/`](docs/proposals/README.md).

- [x] **Decide the event memory optimization.** Accept, change or drop
  [An idle actor carries no event](docs/proposals/lean-actor-events.md):
  whether steps 1 and 2 land first, how the block cache is sized, and
  what `_event` is between macrosteps (keep the last event for invoked
  sessions, or declare the deviation). The experiment is on branch
  `worktree-agent-ac40da141657c3d16` (deleted 2026-09-25). Decide this before the tracing ADR,
  because it moves where the trace parent is stored (ADR 0067 section 8).
  Done: [core ADR 0069](tinyactors-core/docs/adr/0069-an-idle-actor-carries-no-event.md).
- [x] **Drive the tracing ADR to accepted and implemented.**
  [Core ADR 0067](tinyactors-core/docs/adr/0067-core-traces-its-decisions-and-data-to-a-host-sink.md):
  the trace sink, the null sink, step keys, and the W3C trace context and
  cause carried on events. Then a `trace.recording` instrument in
  tinyactors-test and an OTEL exporter in napi.
  Done: core ADR 0067 (implementation notes), the `trace.recording`
  instrument, and napi ADRs 0015 to 0018: traces, logs and metrics leave
  the addon as OTLP protobuf, with no telemetry dependency. Core ADR 0071
  names transitions, actions and expressions; core ADR 0072 adds
  counters, occupancy and failure logs; core ADR 0073 builds, generates
  and samples trace contexts. Throughput is accepted as the baseline in
  `tinyactors-napi/docs/scale.md`; open items are in the napi roadmap's
  "Tracing" follow-ups.
- [x] **Close the core API gaps for log-based durability.** Done:
  replaying actors, prepare and publish (core ADR 0089), session IDs that
  name their family (core ADR 0088), the input log and checkpoints (core
  ADRs 0090 and 0091, napi ADRs 0032 to 0034 and 0036), and system
  parameters (core ADRs 0092 and 0093, napi ADR 0035). Its shape is being
  revised by the next item.
- [ ] **Build out the rest of the napi JavaScript API.** The remaining
  steps of [the napi roadmap](tinyactors-napi/ROADMAP.md#steps).
- [ ] **Migrate to the host kernel, with the write-ahead log in it.**
  Decided in [kernel ADR 0001](tinyactors-host/docs/adr/0001-the-host-kernel-holds-what-every-host-needs.md),
  [kernel ADR 0002](tinyactors-host/docs/adr/0002-the-kernel-keeps-the-write-ahead-log.md),
  [core ADR 0094](tinyactors-core/docs/adr/0094-every-input-carries-its-time-and-the-input-log-leaves-core.md)
  and [napi ADR 0037](tinyactors-napi/docs/adr/0037-napi-is-a-binding-over-the-host-kernel.md);
  the layers are in the [README](README.md#layers-and-responsibilities).
  The kernel is extracted now, not with the browser build, because the log
  needs its home and the test host is a second consumer. Steps, in order;
  each ends with `mise run test` passing, and the last with
  `mise run test:full --all`:
  1. **Done:** [kernel ADR 0001, step 1](tinyactors-host/docs/adr/0001-the-host-kernel-holds-what-every-host-needs.md#step-1-an-empty-kernel-that-builds-and-tests-2026-09-30).
     **Create `tinyactors-host/`:** its `mise.toml`, build and test tasks on
     the root toolchain, `include/tinyactors_host.h`, native tests under
     ASan and UBSan, `check-recursion`, its `#MISE sources`, and its row in
     AGENTS.md's table of what reruns. An empty library that builds and
     tests.
  2. **Done:** [kernel ADR 0001, steps 2a and 2b](tinyactors-host/docs/adr/0001-the-host-kernel-holds-what-every-host-needs.md#step-2b-the-rest-of-napis-portable-c-moves-2026-09-30).
     **Move napi's Node-API-free C into the kernel,** renamed from `tna_` to
     `tah_`, with its native tests: scheduler, router, directory, events,
     enumerate, effects, system, allocator, containers, hooks, adapters,
     loads, offloads, the OTLP encoder, and the host-generic parameters.
     Split `pump.c` into the kernel's turn logic and napi's event-loop
     glue. napi links the kernel, and its guard over `NODE_API_SOURCES`
     goes. `bench-compare` within 5%.
  3. [x] **Core ADR 0094, the time on every input:** a time on
     `ta_actor_deliver()` and `ta_actor_report_invoke()`, `at` on publish,
     `ta_actor_set_time()` removed. `ta_actor_export_state()` and
     `ta_actor_prepare_params.state` added. The kernel and the test host
     migrate. Done: core ADR 0094's notes ("Sections 1 and 3").
  4. **4a done:** [kernel ADR 0002, step 4a](tinyactors-host/docs/adr/0002-the-kernel-keeps-the-write-ahead-log.md#step-4a-the-kernel-writes-and-replays-the-log-2026-09-30)
     (the kernel writes, replays and checkpoints the log at version 2;
     napi runs on it). 4b: the kernel's own replay tests, among them core
     ADR 0089's four timelines; see the notes.
     **Kernel ADR 0002, the log moves:** core's `src/input_log.c` layout,
     writer and reader become the kernel's, at version 2 with `accepted`,
     `delivered` and `dropped`. The kernel records at its own call sites,
     replays by calling core, and encodes checkpoints from core's exported
     state. napi's `inputs.c` and `napi_replay.c` become kernel code. The
     tests move with them, among them the four timelines of core ADR 0089,
     the out-of-memory sweeps, and the memory-bound tests.
  5. [x] **Core ADR 0094, the log leaves core:** remove the input sink, host
     records, `ta_actor_replay()`, `ta_actor_prepare_params.log`,
     `host_seq`, and checkpoint bytes from core. Done: `e4b6974`; see core
     ADR 0094's notes, "Section 2".
  6. **napi ADR 0037:** napi keeps its glue, data model, pump driver and
     `lib/`. `readInputLog()` yields the new kinds, and the rest of the
     public API is unchanged.
  7. **The test host's hosted mode (kernel ADR 0001 section 5):** it links the
     kernel. A scenario can create a hosted system, run turns explicitly,
     and inspect mailboxes, dead letters and batches. Its persistence
     scenarios (input log, replay, checkpoints, compaction) move to hosted
     mode. Scenarios about core's semantics keep driving core directly.
  8. **Verify:** `mise run test:full --all`, `bench-compare` against the
     commit before step 2, and allocations per event in napi. Update the
     kernel ADRs' implementation notes.
- [ ] **Bootstrap a browser build.** Core and the host kernel compiled to
  WebAssembly, with a JavaScript surface shared with napi. Core already
  compiles for `wasm32-freestanding`, but no build task or test covers
  it yet.
- [ ] **Package and release.** One command (`npm install tinyactors`,
  `bun add tinyactors`) installs a prebuilt addon in a Node or Bun
  project; releases are built with one command on a Mac or a Linux VM and
  published to npm and GitHub Releases:
  [Packaging and releases](docs/proposals/packaging.md).
- [ ] **Demo application.** A new project, tinyactors-open-stage uses tinyactors-napi for building a web-based val.town clone where people can deploy their actors, hot-reload them, and connect and listen to events. 
- [ ] **Build the debug adapter.** A Debug Adapter Protocol server in both
  hosts: Ruby in tinyactors-test and JavaScript in napi, with the core
  prerequisites first (operator transitions, the paused position,
  withdrawing a delayed send, source spans, a stop before a microstep's
  first unit): core ADRs 0082 to 0086 and napi ADR 0026, on top of core
  ADRs 0074 to 0080. napi's adapter watches and pokes a production actor
  and never stops or forks it; the test host's steps a developer's
  workbench. Forks, holds and images across systems were built and
  dropped (napi ADR 0026). Both adapters run; what is left is in the
  status map.
  [The design](docs/proposals/debug-adapter.md) and
  [the status of every DAP message](docs/debug-adapter.md).

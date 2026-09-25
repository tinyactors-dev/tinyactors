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
- [ ] **Close the core API gaps for log-based durability.** The core
  capabilities that
  [replay-based persistence](docs/proposals/replay-persistence.md#core-support-an-actor-can-be-replayed-in-place)
  needs, as a core ADR: a time override per resume, holding an actor's
  delayed sends out of dispatch while it replays, and withdrawing sends
  due at or before a given time when replay ends.
- [ ] **Build out the rest of the napi JavaScript API.** The remaining
  steps of [the napi roadmap](tinyactors-napi/ROADMAP.md#steps).
- [ ] **Extract the host kernel.** Move napi's host C that never touches
  Node-API (scheduling, mailboxes, routing, the directory, timers,
  enumeration, effects) into a shared library:
  [Host kernel](docs/proposals/host-kernel.md). When: as the first step
  of the browser build, which is the second host and so shapes the host
  interface; not earlier. Until then, `test-native` fails if Node-API
  reaches a source outside `NODE_API_SOURCES` in
  `tinyactors-napi/mise/lib/compile.rb`, so the portable part stays
  portable.
- [ ] **Bootstrap a browser build.** Core compiled to WebAssembly on the
  host kernel, with a JavaScript surface shared with napi. Core already
  compiles for `wasm32-freestanding`, but no build task or test covers
  it yet.
- [ ] **Package and release.** One command (`npm install tinyactors`,
  `bun add tinyactors`) installs a prebuilt addon in a Node or Bun
  project; releases are built with one command on a Mac or a Linux VM and
  published to npm and GitHub Releases:
  [Packaging and releases](docs/proposals/packaging.md).
- [ ] **Demo application.** A new project, tinyactors-open-stage uses tinyactors-napi for building a web-based val.town clone where people can deploy their actors, hot-reload them, and connect and listen to events. 

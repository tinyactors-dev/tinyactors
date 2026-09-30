# Roadmap

Work across packages, in the order it is meant to happen. Each package's
own `ROADMAP.md` holds its detailed steps; ideas that are not decided yet
are in [`docs/proposals/`](docs/proposals/README.md). How the layers divide
the work is in the [README](README.md#layers-and-responsibilities).

## Done recently

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

- [ ] **A refused send is lost when an actor is replayed from `spawned`,**
  with no checkpoint: replay drops immediate sends, and delayed sends once
  the clock passes them, without knowing the router refused them. Needs a
  log record for the refusal. (Core ADR 0095, Consequences.)
- [ ] **Two narrow windows around orphans:** a crash after an eviction but
  before the orphan is delivered loses the send; an orphan delivered after
  its address was loaded again writes its `fired` into the new actor's
  log. (Core ADR 0095.)
- [ ] **A partly failed redelivery can lose mail later:** the binding
  destroys the published actor, and the messages already put back are
  logged as `dropped`, so a later restore forgets them.
- [ ] **Make napi's copy of sent data cheaper:** +11 to 14% for `{ by: 1 }`
  and +36 to 40% for nested containers (napi ADR 0039's notes).
- [ ] **A chart that uses `toOrigin()` cannot be written out as SCXML:**
  napi's source-text expressions do not read `_event.origin` and
  `_event.originType`.
- [ ] **The kernel's public headers still declare functions a binding
  cannot use** without the private layout (`tah_sched_init`,
  `tah_loads_find`, ...); move them to the private headers.
- [ ] **The kernel holds two copies of a large value while logging it:** a
  4 MiB value peaks at about 17 MB in the log and replay buffers.

## Open

- [ ] **Bootstrap a browser build.** Core and the host kernel compiled to
  WebAssembly, with a JavaScript surface shared with napi. Both already
  build for `wasm32-freestanding`; no build task or test runs them in a
  browser yet. The binding implements `tah_platform` and a pump driver on
  `MessageChannel` and `setTimeout`.

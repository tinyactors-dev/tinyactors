# Roadmap

Work across packages, in the order it is meant to happen. Each package's
own `ROADMAP.md` holds its detailed steps; ideas that are not decided yet
are in [`docs/proposals/`](docs/proposals/README.md). How the layers divide
the work is in the [README](README.md#layers-and-responsibilities).

## Done recently

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

## Follow-ups from the kernel work

Each names where it was recorded. None is a known regression; they are
gaps left or found on the way.

- [ ] **Native replay tests in the kernel.** `tinyactors-host/tests/replay_test.c`
  was never written: three attempts stalled during an API outage. It
  should cover:
  - prepare, run and redelivery;
  - a checkpoint round trip, and life boundaries;
  - the four timelines of core ADR 0089 section 2;
  - an out-of-memory sweep over replay.

  Replay is covered today only through the test host's hosted scenarios
  and napi's `replay.js`. (Kernel ADR 0002, "Step 4b".)
- [ ] **Finish the test host's hosted mode.** Kernel ADR 0001 section 5
  promised inspecting mailboxes and dead letters, which has no method yet.
  Also missing:
  - two of core ADR 0089's four timelines as hosted scenarios: the send to
    another actor that fired before the stop, and the send still pending
    at the stop. The pending timer and the crash are covered;
  - a summons through a loader, rather than an explicit `hosted.restore`;
  - an input refused because the log could not grow. The core-log version
    of that scenario was removed in step 5;
  - the "largest write" check on the jq writer, lost with the
    `input.recording` instrument. Only the unit test covers it now.

  (Kernel ADR 0001, "Step 7"; core ADR 0094, "Section 2".)
- [ ] **The scenario runner does not rebuild the test host** when kernel
  sources change, so `mise run scenarios` after a kernel edit runs a stale
  binary until `mise run build`. Make `scenarios` depend on the build, or
  list the kernel in its sources. (Kernel ADR 0001, "Step 7".)
- [ ] **The cost of a logged input:** +5.5 µs on Node and +3.8 µs on Bun per
  message, measured before the value was written once. Measure it again at
  version 2. If it still matters, take option B from the review: a native
  durable writer for plain JSON values in napi's data model, byte-exact
  with ADR 0031's printer. (napi ADR 0034's notes.)
- [ ] **The kernel's public headers expose its internals.**
  `include/tinyactors_host/*.h` declare the full structs (`tah_system`,
  `tah_sched`, `tah_inputs` and more), because napi and the test host
  reach into their fields. Make the types opaque behind functions, so a
  binding cannot depend on the kernel's layout. (Kernel ADR 0001, "Step
  2b".)
- [x] **Status-line pointers on the napi ADRs whose code moved.** Kernel ADR
  0001 section 6 lists them (0003 to 0008, 0012, 0014, 0016 to 0018, 0020,
  0022, 0032 to 0036), and only one carries a pointer to it.
- [x] **`parameterInfo()` labels the kernel's parameters as napi's.** Give
  them `owner: "kernel"`, since the test host has them too. (Kernel ADR
  0001, "Step 2b".)
- [x] **Checkpoints leave out two things:** invocations carry no values, and
  sends already submitted to the router are not restored. Decided
  2026-09-30 in core ADR 0095: submitted sends are restored, since nothing
  else recorded them; invocations still carry no values, since nothing
  reads them. (Core ADR 0091's notes; kernel ADR 0002, "Step 4a".)
- [x] **The trace staging bound is the bound plus one call,** decided
  2026-09-30 and recorded in napi ADR 0036's follow-up: a per-record check
  would call the sink from inside core.
- [x] **A `send()` with a timeout does not keep the process alive,** and
  neither does any pending `send()`: decided 2026-09-30 and recorded in
  napi ADR 0007 (Consequences). Tests that wait for a timeout keep the
  process alive themselves.
- [x] **Run the test host's exhaustive pass after core changes.** Some
  full runs showed the test host's step cached after a core change
  (`scale_test` to four million actors; core itself has no exhaustive
  mode). Two causes are fixed: the root runner took a subtask's skip line
  (`bindings`) for the step's own, and each project's `test:full` shared
  its fast pass's stamp without forcing. Confirmed: `test:full` after
  core ADR 0095's change ran `scale_test` to four million actors.

## From granary's feedback, still open

- [ ] **Typed event maps on the chart builder:** `statechart<Data, Events>()`,
  so a transition's `event.data` is typed and event names are checked.
- [ ] **A reply-to-sender target,** instead of
  `.to(({ event }) => String(event?.origin ?? ''))`.
- [ ] **Names that contain `/`:** core's target grammar refuses `/` inside
  an actor's name (`src/scxml_uri.c`). Percent-encoding in the grammar
  (core ADR 0044), and a napi helper that builds addresses.

## Open

- [ ] **Build out the rest of the napi JavaScript API.** The remaining
  steps of [the napi roadmap](tinyactors-napi/ROADMAP.md#steps).
- [ ] **Bootstrap a browser build.** Core and the host kernel compiled to
  WebAssembly, with a JavaScript surface shared with napi. Both already
  build for `wasm32-freestanding`; no build task or test runs them in a
  browser yet. The binding implements `tah_platform` and a pump driver on
  `MessageChannel` and `setTimeout`.

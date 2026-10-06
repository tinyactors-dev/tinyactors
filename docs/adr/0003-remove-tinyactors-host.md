# ADR 0003: Remove tinyactors-host

**Status:** Superseded by [ADR 0004](0004-replace-the-core-with-the-standalone-runtime.md) (2026-10-06); the kernel was removed, the rings were not built. Was: accepted (2026-10-05). Supersedes kernel ADRs 0001 to 0006 (`tinyactors-host/docs/adr/`).
**Date:** 2026-10-05

## Context

The repository has four layers (README, "Layers and responsibilities"):
core decides what inputs mean, the host kernel queues, records and replays
them, a binding owns values, and the application owns storage and policy.

The kernel exists because of one decision: the queue of pending inputs
lives outside core (core ADRs 0011 and 0029). Core has no mailboxes, so
someone else holds mail between actors, schedules it and routes it. Every
binding needs the same router, scheduler, effects queue and recorder.
Kernel ADR 0001 moved that shared code out of napi into a C library,
`tinyactors-host` (prefix `tah_`), with the test host as its second user.

The queue outside core has costs:

- a send cannot be refused in the macrostep that makes it;
- overflow has to be held somewhere (kernel ADR 0003);
- the write-ahead log and session replay grew around it (kernel ADRs 0002
  to 0005), and kernel ADR 0006 removed them again.

The ring protocol
([proposal](../proposals/ring-proto/README.md),
[architecture](../proposals/ring-proto/ARCHITECTURE.md#layers-before-and-after))
puts the queue in core. Core holds every pending input in one fixed pool
of envelopes and talks to its host over three rings: `h2c`, `c2h` and
`tel`. A full pool refuses, as `error.execution.capacity` in the sender.
Mail between local sessions never leaves core. Effects are `c2h` records
the host executes on receipt.

With that, the kernel has nothing of its own left to do. Its scheduler is
the pool. Its router is an envelope in core, or a `c2h.send`. Its recorder
and replay are `c2h.input` and the `h2c` records core already replays. What
remains of host work is a short loop in each binding: post `h2c`, call
`ta_run`, read `c2h`, drain `tel`, set one timer from `c2h.wake`.

## Decisions

### 1. `tinyactors-host` is deleted

The package goes: `include/tinyactors_host/*.h`, `src/*.c` (8,137 lines),
`tests/` (5,360 lines), its mise tasks and its `mise.toml`. Line counts are
from [migration.md](../proposals/ring-proto/reference/migration.md#tinyactors-host-is-deleted)
(2026-10-01).

About 4,600 lines of `src` are deleted, because core or the protocol does
their work:

| Module | Replaced by |
|---|---|
| `scheduler` | core's pool and run queue, macrostep slots, `ta_run`'s budget. Quarantine is removed: the chain limit ends a runaway's input. |
| `router` | an envelope in core for local mail; `c2h.send` for the rest |
| `effects`, `adapter_io`, `adapter_invoker`, `adapter_log` | `c2h.send`, `c2h.invoke` and its kin, `h2c.invoke_report`, `c2h.log` |
| `recorder`, `replay` | `c2h.input` and the `recorded` bit; `h2c.spawn` (replaying), `h2c.event`, `h2c.publish` |
| `loads`, `offloads`, `directory` | waiting lists in the pool, the load records, the `blob` of `c2h.unloaded`, and the host's own index of unloaded addresses |
| `events` | nothing: run-time names are bytes |
| `hooks` | `c2h.spawned`, `c2h.destroyed` |
| `params`, `system`, `version` | removed parameters; what is left of `system` is napi's ring setup and pump |

### 2. What is still needed moves

| Module | Lines (src) | Goes to |
|---|---|---|
| `otlp`, `adapter_trace` | 2,349 | `tinyactors-otlp`, a new small C library that reads `tel` and encodes OTLP traces and logs. napi and the test host link it. Its span IDs come from core's `ta_step_key_span_id`, or are tested against core's pinned values. |
| `allocator`, `memory`, `containers`, `platform` | about 650 | napi (napi ADRs 0009 and 0014). Whatever napi does not use is deleted. |
| `host_metrics`, `definitions`, `enumerate` | 214 | napi: binding work over core's API. |
| `watch` | 321 | mostly deleted. Watched sessions become a per-actor bit in core; the parts an operator sees move to napi. |

### 3. Tests go with their modules; their properties stay

Each property the kernel's tests check becomes a core property, tested in
core, or a protocol property, tested on the rings: order between each
sender and receiver, admission, recycling, load requests, replay with
`h2c.spawn` then `h2c.publish`. The out-of-memory sweep over replay
(`replay_test` with `TAH_FULL=1`) is not needed for macrosteps, which
cannot fail to allocate. A sweep stays over spawn, load and definition
registration, which still allocate. Allocator and container tests move to
napi with their code.

### 4. The order: Phase 6, after napi drives the rings

The kernel is deleted in Phase 6 of the
[implementation plan](../proposals/ring-proto/IMPLEMENTATION.md#phase-6-delete-the-kernel),
after Phase 5 replaces core's vtables with rings. Every commit passes
`mise run test`.

1. **6.1:** napi drives core's rings directly, with the pump loop above.
   Its `index.d.ts` changes as migration.md lists, with `// status:`
   markers where the implementation lags.
2. **6.2:** OTLP moves into `tinyactors-otlp`;
   `TAH_PARAM_TRACE_STAGING_BYTES` becomes its own setting. The counting
   allocator, containers, metrics and definitions helpers move into napi.
3. **6.3:** `tinyactors-host` is deleted.

Phase 6 is done when `mise run test --all` and `mise run test:full` pass
with no kernel.

### 5. The repository changes with it

In the same change as the deletion:

- the kernel's mise tasks go, and `"tinyactors-host"` leaves
  `config_roots` in the root `mise.toml`;
- `tinyactors-otlp` joins `config_roots`, and the `#MISE sources` of napi
  and the test host name it instead of the kernel;
- AGENTS.md loses the kernel's row in "Tests: what to run" and gains one
  for `tinyactors-otlp`; its "Layers" paragraph and its list of components
  with a changelog follow;
- README's "Layers and responsibilities" has three layers in this
  repository: core, the binding, and the application. Core's row gains the
  pool and the rings; the binding's row gains the ring loop.

### 6. The kernel's ADRs stay as history

Kernel ADRs 0001 to 0006 and their README index stay in
`tinyactors-host/docs/adr/` until the directory is deleted, and in git
history after. Each gets a status line that names this ADR:

| Kernel ADR | Replaced by |
|---|---|
| [0001](../../tinyactors-host/docs/adr/0001-the-host-kernel-holds-what-every-host-needs.md) The kernel holds what every host needs | core holds it, as rings and a pool |
| [0002](../../tinyactors-host/docs/adr/0002-the-kernel-keeps-the-write-ahead-log.md) The kernel keeps the write-ahead log | already superseded by 0006; no WAL returns, `c2h.input` observes inputs |
| [0003](../../tinyactors-host/docs/adr/0003-a-full-mailbox-overflows-instead-of-refusing-mail-between-actors.md) A full mailbox overflows | the opposite: a full pool refuses, as `error.execution.capacity` in the sender |
| [0004](../../tinyactors-host/docs/adr/0004-a-macrosteps-first-record-commits-with-what-it-sent.md) A macrostep's first record commits with what it sent | already superseded by 0006; `c2h.macrostep_ended` reports completion and holds no effects |
| [0005](../../tinyactors-host/docs/adr/0005-an-actors-log-is-an-inbox-with-acknowledgements.md) An actor's log is an inbox with acknowledgements | already superseded by 0006; no write-ahead hold or acknowledgement returns |
| [0006](../../tinyactors-host/docs/adr/0006-actors-record-their-events-through-the-application-and-are-rebuilt-by-replaying-them.md) Actors record their events through the application | the `recorded` bit and `c2h.input`; journaling stays the application's, and replay seeds new execution |

## Consequences

- **Lines.** About 4,600 lines of kernel `src` and 5,360 of tests are
  deleted. About 3,500 lines move: 2,349 to `tinyactors-otlp`, the rest to
  napi, where the scheduler's absence leaves fewer containers to keep. napi
  gains ring setup and loses everything that wraps `tah_system_*`.
- **Packages.** Before: core, the kernel, scxml, tinyxml, napi and the
  test host. After: core, `tinyactors-otlp`, scxml, tinyxml, napi and the
  test host. The count stays the same; `tinyactors-otlp` is small and
  optional for a binding that does not export telemetry.
- **One fewer layer per binding.** A binding links core, and
  `tinyactors-otlp` if it exports OTLP. It drives rings instead of calling
  a kernel that calls core. A second binding writes the loop, not the
  router and scheduler, because those are in core.
- **A change to core reruns fewer layers.** Nothing sits between core and
  the bindings, so AGENTS.md's test table has one dependency less.
- **The kernel was never released.** It has no tag and no changelog, so
  nothing outside the repository changes.

## What this does not decide

- **The ring protocol itself.** The rings, the envelope pool, macrostep
  slots and telemetry are core ADRs, listed in
  [migration.md](../proposals/ring-proto/reference/migration.md#adrs-to-write).
- **napi's design over the rings.** A napi ADR, "napi is a binding over
  core's rings", supersedes napi ADR 0037 and the napi ADRs that depend on
  the kernel.
- **The MCP server.** It is an experimental feature with its own
  [proposal](../proposals/mcp-server.md), revised after Phase 5. Deleting
  the kernel breaks nothing there, because nothing implements it.
- **`tinyactors-otlp`'s API and parameters.** Step 6.2 settles them.
- **The SQLite and PostgreSQL host sketches.** They need updating to the
  ring protocol; they are applications, not a layer.

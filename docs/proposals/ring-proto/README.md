# The ring protocol: core and host talk through shared memory

**Status:** Sketch (2026-10-04)
**Touches:** tinyactors-core (gains the rings, the envelope pool and run-time names), tinyactors-host (deleted), tinyactors-napi and tinyactors-test (become ring drivers)
**Not an implementation plan:** implementation is not scheduled. ADRs and roadmaps win.

## The idea in one paragraph

Core and its host exchange **records over three rings in memory that core
provides**, the way core already hands trace records to a sink:

- **`h2c`** carries every input, from the host to core.
- **`c2h`** carries everything that leaves the process or must not be lost.
- **`tel`** carries telemetry, which may be dropped.

Core keeps **one fixed pool of envelopes**. It holds every pending input:
external events admitted from `h2c`, mail between actors, and delayed sends.
A macrostep **allocates nothing**. When something is full, the result is a
**refusal to someone who can retry**:

- the external client, at the host's edge;
- or the sending chart, as an SCXML error event.

Inputs that cannot be admitted are consumed and bounced, so they cannot
block the load response or clock update needed to free capacity. The host
owns scheduling boundaries and retains retry originals only in a bounded
submission table. Nobody keeps an unbounded overflow queue. The host kernel (`tinyactors-host`) is not
needed. Its scheduler becomes the pool. Its recorder and replay
become `c2h.input` observations and the replay core already has; its WAL was
already removed by kernel ADR 0006. Its effects queue becomes `c2h`, which
the host may execute as it reads. A macrostep end reports
macrostep completion and does not hold effects.

**Replay is for seeding new execution.** Feeding inputs into a prepared
session derives a chart configuration and data model values, then publish
starts live execution from that result. Different resource conditions,
budgets or compatible definitions may produce a different result. Recording
does not require persistence before effects, and replay does not promise
restoration of the old execution or its in-flight messages.

The idea follows io_uring (submission and completion rings shared between two
parties) and sans-I/O protocol libraries, such as Rust's raft implementations
that leave all I/O to their caller. The test host's JSON-RPC protocol was an
early, slower version of the same thing.

## Why

Extracting the kernel made things more complex, and the WAL and session replay
caused a lot of confusion. Most of that came from one decision: the queue of
pending inputs lived outside core (core ADRs 0011, 0029). Because of that:

- a send could not be refused in the macrostep that made it;
- overflow had to be held somewhere;
- every binding needed the same router and scheduler, so they moved into a
  third layer.

Put the queue in core, give it a fixed size, and those problems go away. See
[ARCHITECTURE.md](ARCHITECTURE.md) for how the pieces fit together.

## Invariants

1. **A macrostep never allocates in core.** Core allocates when a system is
   created, when a definition is registered, and when an actor is created or
   loaded. Nothing else allocates.
   ([reference/macrostep.md](reference/macrostep.md))
2. **Core storage is bounded by parameters and compiled definition requirements.**
   ([BEHAVIORS.md](BEHAVIORS.md#parameters))
3. **Every overflow is a refusal to someone who can retry:**
   - the external client (`h2c` is full, so the host refuses at its edge);
   - the sending chart (`error.execution.capacity`);
   - nobody, for an asynchronous reply. It is dropped, and the chart's timeout
     covers it.

   ([BEHAVIORS.md](BEHAVIORS.md))
4. **A macrostep never waits for ring capacity in a way that can
   deadlock.** Core reserves mandatory observations, cancellations,
   forwarding and end reports before taking input. An ordinary action that
   may write c2h first checks for room for the largest record. Without it,
   the slot parks before the action, which runs once after the host drains
   c2h. The host drains c2h without
   any session making progress, so the wait always ends. Other slots go on.
   The pool and the name area fail the action instead: only sessions free
   them, so a wait there could deadlock. Startup and control work can return
   for the host to drain c2h between macrosteps; a budget can pause execution
   ([reference/macrostep.md](reference/macrostep.md#a-full-c2h-pauses-the-slot)).
5. **One session never stops the others.** Each session runs one input at
   a time, but up to `maxConcurrentInputs` sessions run at once, taking turns
   between units of executable content. A runaway holds one slot, not the
   system ([reference/macrostep.md](reference/macrostep.md#macrostep-slots)).
6. **Core decides meaning; the application decides policy.** Reliability is
   built into charts (timeouts, retries, resync) and I/O processors
   (idempotency keys), not into the protocol. SCXML sends stay fire and forget.

## Reading order

| Document | What it covers |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the pieces fit together, with links to each reference |
| [BEHAVIORS.md](BEHAVIORS.md) | At a glance: who drops what, who is told, who retries, and the crash matrix |
| [protocol/](../../../tinyactors-core/protocol/README.md) | Every record, defined once in a Ruby DSL. [MESSAGES.md](../../../tinyactors-core/src/generated/MESSAGES.md) is generated from it. |
| [charts/](charts/README.md) | The stress-test charts: a notification campaign and its progress tracker |
| [scenarios/](scenarios/README.md) | Step-by-step walkthroughs that cite messages by stable ID |
| [IMPLEMENTATION.md](IMPLEMENTATION.md) | If adopted: how core's plumbing is replaced by building blocks, phase by phase, with every commit green |

Reference documents, one for each building block:

| Building block | Reference |
|---|---|
| A ring: framing, reserve and commit, lossless or lossy | [reference/ring.md](reference/ring.md) |
| The envelope and the pool: admission, fairness, delayed sends, recycling | [reference/envelope.md](reference/envelope.md) |
| Names: authored names are interned, run-time names are bytes | [reference/names.md](reference/names.md) |
| The macrostep: invariants, the macrostep end, two kinds of exhaustion | [reference/macrostep.md](reference/macrostep.md) |
| The data model: a synchronous vtable and value references | [reference/data-model.md](reference/data-model.md) |
| Definitions: complete declarative requests and structured results | [reference/definitions.md](reference/definitions.md) |
| Debugging: exclusive system ownership, inspection and transitions | [reference/debugging.md](reference/debugging.md) |
| Recording and replay: `c2h.input`, checkpoints, replaying a whole system | [reference/recording-and-replay.md](reference/recording-and-replay.md) |
| Telemetry: the lossy ring, OTLP | [reference/telemetry.md](reference/telemetry.md) |
| Unloading and loading sessions, which only bounds RAM: images end with their process | [reference/unloading.md](reference/unloading.md) |
| System parameters: defaults, bounds, what they drive | [BEHAVIORS.md](BEHAVIORS.md#parameters) |
| What is deleted, which ADRs change, and open questions | [reference/migration.md](reference/migration.md) |
| The building blocks it is assembled from: what core has, needs, and can drop | [reference/building-blocks.md](reference/building-blocks.md) |

## The worked example

A browser posts "send this campaign". The `campaign` chart sends each
notification to a notify I/O processor and reports progress to a `progress`
actor, which pushes updates to the browser. It uses a delayed send to itself
as its timeout. The notify processor keeps notification IDs for a bounded time
as idempotency keys, which the chart never sees.

The scenarios run this example through:

- the normal case;
- a full pool;
- a crash with every combination of recorded and unrecorded actors;
- checkpoints;
- an unloaded tracker;
- `<cancel>` racing a second click;
- backpressure at the edge;
- an HTTP client posting random event names.

## What this changes, in short

- **Deleted:** `tinyactors-host`, and napi's and the test host's code that
  wraps it ([reference/migration.md](reference/migration.md)).
- **Core gains:**
  - three rings: `h2c`, `c2h`, and an optional `tel` (`telBytes` may be 0)
    ([reference/ring.md](reference/ring.md));
  - the envelope pool and its deadline index, with one run queue in arrival
    order and `fairnessRatio` as its only fairness policy
    ([reference/envelope.md](reference/envelope.md#fairness));
  - macrostep slots, taken in turn, and a chain limit that ends a runaway
    input ([reference/macrostep.md](reference/macrostep.md#macrostep-slots));
  - a park area for slots that are not running
    ([parking](reference/macrostep.md#parking-one-running-workspace-and-a-park-area));
  - compiler-sized storage for mandatory internal events
    ([reference/macrostep.md](reference/macrostep.md#sizing-mandatory-storage));
  - a name area for names made at run time
    ([reference/names.md](reference/names.md#the-name-area));
  - capacity errors that charts can see.
- **Core loses:**
  - the out-of-memory stop machinery that runs mid-step (ADR 0070);
  - per-actor event blocks and their cache (ADRs 0069, 0093);
  - the input log format (ADR 0090);
  - interning at run time. Today core interns every event name computed at
    run time into tables that only grow, so a long-running system that
    computes event names leaks memory, and an HTTP endpoint that maps
    request fields to event names lets a client exhaust memory on purpose.
    Authored names are interned at registration, and atoms are never freed
    ([reference/names.md](reference/names.md#atoms-are-never-freed));
  - the directory vtable. Core keeps only resident sessions and asks for a
    non-resident address with `c2h.load_needed`
    ([reference/unloading.md](reference/unloading.md#no-directory-vtable-not-resident-means-ask)).
- **napi loses** quarantine, `release()` and the parent's `.quarantined`
  event. Quarantine existed so that one bad actor could not take the loop's
  turns; slots do that, and a runaway's input is abandoned. The host decides
  what to do about an actor that keeps running away
  ([reference/macrostep.md](reference/macrostep.md#runaways-the-chain-limit)).
- **Unchanged, by decision:**
  - the data model stays a synchronous vtable
    ([reference/data-model.md](reference/data-model.md#why-the-data-model-is-not-on-a-ring));
  - native integrations own data model adapters and values;
  - a `ta_system` is not thread-safe. A host with other threads puts its
    own queue in front of it ([reference/ring.md](reference/ring.md#one-thread)).

**Boundaries:**

- **Definitions.** Hosts submit complete declarative definitions and
  receive success or structured compile-error results; registration holds
  and retirement also use requests. A request carries a handle to a native
  `ta_scxml_spec` the binding built, and diagnostics are ADR 0058's native
  struct. A spec has no byte layout
  ([reference/definitions.md](reference/definitions.md#a-native-spec-handle-not-a-byte-layout)).
- **Debugging.** An attached debugger suspends the production pump and owns
  the whole system. Commands stay outside the three rings; intervention
  uses transitions and abandonment at legal boundaries. Detaching returns
  ownership to the pump ([reference/debugging.md](reference/debugging.md)).
- **Execution.** Input admission is not completion. `c2h.input_completed`
  follows stabilization and invocation-startup follow-up, independently of
  recording. The host awaits it when ordering input execution before time
  changes or replay publication. Recorded inputs use synchronous snapshots
  taken before chart execution
  ([reference/macrostep.md](reference/macrostep.md#invocation-startup-and-input-completion)).
- **Withdrawal.** Commit is final, and records are never edited. A host
  takes back an admitted input that has not been taken with `h2c.withdraw`,
  which core answers with `c2h.input_refused` (`withdrawn`); an input
  already taken keeps its own terminal record
  ([reference/envelope.md](reference/envelope.md#withdrawing-an-admitted-input)).
- **Macrostep slots.** Core runs up to `maxConcurrentInputs` sessions'
  inputs at once, in turn, `slotSlice` steps each; mail for an active
  session waits on its deferred list. `microstepLimit` counts one input's
  whole chain (microsteps, selection passes and startup passes) and cannot
  be turned off. At the limit, core abandons the input with status
  `runaway`; the session lives on and takes its next input. With one slot
  this is plain run to completion
  ([reference/macrostep.md](reference/macrostep.md#macrostep-slots)). Only
  the running slot uses the system's one workspace; the others are parked
  in chained chunks of a park area, about 1 KiB each for a chart of 100
  states. The area fills on demand up to a ceiling core derives, so no host
  has to size it, and a slot that cannot park keeps the thread
  ([parking](reference/macrostep.md#parking-one-running-workspace-and-a-park-area)).
  The host gives memory back with `ta_system_trim`; core never trims.
- **Internal events.** The compiler sizes mandatory error and completion
  storage for one round of every phase, allocated outside macrosteps in
  addition to the chart's `internalQueueEvents` quota. Mandatory events
  beyond the floor use free ordinary entries; one that fits in neither ends
  the input as a runaway, with `runaway_cause` `internal_queue` on the `TA_TRACE_RUNAWAY`
  telemetry record ([reference/macrostep.md](reference/macrostep.md#sizing-mandatory-storage)).
- **c2h backpressure.** An ordinary action whose c2h record does not fit
  pauses its slot, not its chart. Core checks for room before it evaluates
  the action, so the action runs once, after the host drains. A full pool
  or name area fails the action, because only sessions free them. So a
  ring's size is a throughput setting, not a correctness one. h2c and tel
  are 16 KiB each by default; c2h is derived as
  (3 × `maxConcurrentInputs` + 2) × `maxRecordBytes`, 776 KiB with the
  defaults
  ([reference/macrostep.md](reference/macrostep.md#a-full-c2h-pauses-the-slot),
  [reference/ring.md](reference/ring.md#the-c2h-reserve),
  [BEHAVIORS.md](BEHAVIORS.md#parameters)).
- **Invocation lifetime.** A local invocation holds an envelope token for
  completion or cancellation. Unload refuses both owners of active local
  invocations and active locally invoked children. Live service replies
  carry instance tokens; authored invocation IDs alone are insufficient
  ([reference/envelope.md](reference/envelope.md#local-invocation-tokens)).
- **Telemetry.** `tel.trace` records mirror today's trace kinds, are
  self-contained, and carry stamps from a host clock installed natively.
  `tinyactors-otlp` encodes OTLP. A drop ends the open spans with error
  status ([reference/telemetry.md](reference/telemetry.md)).
- **MCP.** The MCP mapping is outside this proposal;
  [mcp-server.md](../mcp-server.md) is revised after Phase 5.

The details, and the ADRs this would supersede, are in
[reference/migration.md](reference/migration.md). The one open item is
measuring the h2c and tel defaults and checking the derived c2h, in step 4.2
of [IMPLEMENTATION.md](IMPLEMENTATION.md).

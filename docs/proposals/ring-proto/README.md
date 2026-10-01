# The ring protocol: core and host talk through shared memory

**Status:** Sketch (2026-10-01)
**Touches:** tinyactors-core (gains the rings, the envelope pool and run-time names), tinyactors-host (deleted), tinyactors-napi and tinyactors-test (become ring drivers)
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

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

Nobody keeps an overflow queue. The host kernel (`tinyactors-host`) is no
longer needed. Its scheduler becomes the pool. Its recorder and replay
become `c2h.input` observations and the replay core already has; its WAL was
already removed by kernel ADR 0006. Its effects queue becomes `c2h`, which
the host may execute as it reads. A fence reports
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
2. **All memory is bounded by parameters.**
   ([BEHAVIORS.md](BEHAVIORS.md#parameters))
3. **Every overflow is a refusal to someone who can retry:**
   - the external client (`h2c` is full, so the host refuses at its edge);
   - the sending chart (`error.execution.capacity`);
   - nobody, for an asynchronous reply. It is dropped, and the chart's timeout
     covers it.

   ([BEHAVIORS.md](BEHAVIORS.md))
4. **A run never waits.** Core runs each macrostep to completion, in pool
   order. The only pause is the budget.
5. **Core decides meaning; the application decides policy.** Reliability is
   built into charts (timeouts, retries, resync) and I/O processors
   (idempotency keys), not into the protocol. SCXML sends stay fire and forget.

## Reading order

| Document | What it covers |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the pieces fit together, with links to each reference |
| [BEHAVIORS.md](BEHAVIORS.md) | At a glance: who drops what, who is told, who retries, and the crash matrix |
| [protocol/](protocol/README.md) | Every record, defined once in a Ruby DSL. [MESSAGES.md](protocol/generated/MESSAGES.md) is generated from it. |
| [charts/](charts/README.md) | The stress-test charts: a notification campaign and its progress tracker |
| [scenarios/](scenarios/README.md) | Step-by-step walkthroughs that cite messages by stable ID |

Reference documents, one for each building block:

| Building block | Reference |
|---|---|
| A ring: framing, reserve and commit, lossless or lossy | [reference/ring.md](reference/ring.md) |
| The envelope and the pool: admission, fairness, delayed sends, recycling | [reference/envelope.md](reference/envelope.md) |
| Names: authored names are interned, run-time names are bytes | [reference/names.md](reference/names.md) |
| The macrostep: invariants, the fence, two kinds of exhaustion | [reference/macrostep.md](reference/macrostep.md) |
| The data model: a synchronous vtable and value references | [reference/data-model.md](reference/data-model.md) |
| Recording and replay: `c2h.input`, checkpoints, replaying a whole system | [reference/recording-and-replay.md](reference/recording-and-replay.md) |
| Telemetry: the lossy ring, OTLP | [reference/telemetry.md](reference/telemetry.md) |
| Unloading and loading sessions, which only bounds RAM: images end with their process | [reference/unloading.md](reference/unloading.md) |
| System parameters: defaults, bounds, what they drive | [BEHAVIORS.md](BEHAVIORS.md#parameters) |
| What is deleted, which ADRs change, and open questions | [reference/migration.md](reference/migration.md) |

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
  wraps it.
- **Core gains:**
  - three rings;
  - the envelope pool and its deadline index;
  - a name area for names made at run time;
  - capacity errors that charts can see.
- **Core loses:**
  - the out-of-memory stop machinery that runs mid-step (ADR 0070);
  - per-actor event blocks and their cache (ADRs 0069, 0093);
  - the input log format (ADR 0090);
  - interning at run time.
- **Unchanged, by decision:**
  - the data model stays a synchronous vtable
    ([reference/data-model.md](reference/data-model.md#decided-the-data-model-stays-a-synchronous-vtable));
  - a `ta_system` is not thread-safe. A host with other threads puts its
    own queue in front of it ([reference/ring.md](reference/ring.md#one-thread)).
- **Found on the way:** core interns every event name computed at run time
  into tables that only grow. Any long-running system that computes event
  names leaks memory. An HTTP endpoint that maps request fields to event names
  lets a client exhaust memory on purpose.
  ([reference/names.md](reference/names.md))

The details, and the ADRs this would supersede, are in
[reference/migration.md](reference/migration.md).

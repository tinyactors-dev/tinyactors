# ADR 0104: Core talks to its host through three rings

**Status:** Accepted (2026-10-05); not yet implemented. Supersedes ADRs 0015 and 0040; refines ADRs 0016, 0037, 0045, 0058, 0068, 0077 and 0079.
**Date:** 2026-10-05

## Context

Core reaches its host through a call per fact. It calls a vtable for each
of these:

- the router, for mail between sessions (`ta_external_event_router`, ADR
  0040);
- I/O processors, inside the macrostep (`ta_io`, ADRs 0015 and 0068);
- invokers (`ta_invoker`, ADR 0016);
- `<log>` records (`ta_log_sink`, ADR 0045);
- trace records (`ta_trace_sink`, ADR 0067);
- the directory of actors that are not resident (`ta_actor_directory`,
  ADR 0037).

A host feeds core through more calls: `ta_actor_deliver`,
`ta_system_set_time`, spawns, loads and definition registration.

Each call out of a macrostep runs host code at a point the host did not
choose. JavaScript cannot run there, so the kernel (`tinyactors-host`)
copies every effect into a queue and runs it later. A router call can only
queue, so overflow has to be held somewhere outside core. Every binding
needs the same plumbing, which is why the kernel exists
([README, "Why"](../../../docs/proposals/ring-proto/README.md#why)).

One interface already points another way: the trace sink receives
self-contained records that a host may buffer and read later. The
[ring protocol proposal](../../../docs/proposals/ring-proto/README.md)
generalizes it. Core and its host exchange records over rings in memory
core provides, as io_uring's submission and completion rings do, and as
sans-I/O protocol libraries leave all I/O to their caller. The
[migration plan](../../../docs/proposals/ring-proto/reference/migration.md#adrs-to-write)
lists this as item 1. ADRs 0098 to 0103 and 0105 record the other items
and refer to "the rings" without deciding them. This ADR decides them.

The pieces already exist in core:

- `src/ring.h`: a single-producer, single-consumer byte ring with pads,
  gap-free sequence numbers, and a lossless and a lossy mode;
- `protocol/catalog.rb`: every record, defined once in a Ruby DSL, with
  `src/generated/ring_records.h`, `ring_codec.h`, `ring_codec.c`,
  `MESSAGES.md` and `messages.json` rendered from it;
- `src/type_registry.h`: a set of type atoms, one for I/O processors and
  one for invokers, with no behaviour attached.

Nothing uses them yet.

## Decision

1. **Three rings, split by delivery guarantee.** Core allocates them at
   system creation from the system's allocator (ADRs 0023, 0092), next to
   a control block the host reads. None changes size after creation.

   | Ring | Direction | Guarantee | Carries |
   |---|---|---|---|
   | `h2c` | host to core | lossless, gap-free; full means the host refuses at its edge | every input and host request: `h2c.event`, `h2c.time`, `h2c.spawn`, `h2c.load`, `h2c.invoke_report`, `h2c.type_declare`, `h2c.definition_register`, ... |
   | `c2h` | core to host | lossless, gap-free; full means the writing slot waits for a drain | everything that leaves the process or must not be lost: effects, macrostep ends, input results, bounces, load requests, images, definition results, input observations |
   | `tel` | core to host | lossy; full means drop and count | telemetry ([ADR 0098](0098-telemetry-is-a-lossy-ring.md)); `telBytes` 0 creates none |

   There are three because there are three guarantees, not three
   purposes. Recording on a ring of its own adds no guarantee c2h lacks.
   Recording on tel loses inputs. Telemetry on c2h lets a slow exporter
   stall a chart's sends. Mail between local sessions on a ring deadlocks
   when the running session writes the ring only its own run drains; it
   lives in the envelope pool instead
   ([ADR 0099](0099-pending-inputs-are-envelopes-in-one-pool-per-system.md)).
   A fourth ring needs a fourth guarantee.

2. **How a ring works**
   ([ring.md](../../../docs/proposals/ring-proto/reference/ring.md)).
   - **One producer, one consumer, one thread.** A ring's state is two
     counters that only grow: `head` (bytes committed) and `tail` (bytes
     released). Writing a record does not run the reader.
   - **Bytes, not slots.** The producer reserves, fills in place, and
     commits. The reader looks, uses in place, and releases. Core commits
     c2h after every record, so a host can execute a long macrostep's
     effects during a budget pause.
   - **A record never splits.** Every record is 8-byte aligned and
     contiguous. When it does not fit before the end of the buffer, the
     producer writes a **pad**: an 8-byte prefix with `kind` 0 and `size`
     the bytes to the end, with no seq. It costs at most
     `maxRecordBytes - 8` bytes per wrap. A pad that cannot fit with its
     record even in an empty ring is committed alone, so a ring of
     exactly `maxRecordBytes` makes progress at any offset.
   - **Sequence numbers start at 1 on each ring.** On h2c and c2h a gap is
     corruption; core treats an h2c gap as `invalid` and stops reading
     h2c. On tel a dropped record still takes its number. Other records
     cite h2c seqs (`source_seq`, `cause_seq`), so the host numbers h2c
     and core checks it.
   - **The largest record is `maxRecordBytes`.** On c2h, an action whose
     record would be larger fails as `error.execution.capacity.record`.
     On h2c, core bounces it with `c2h.bounced` (`invalid`). On tel, a
     long `value_text` is cut and flagged `truncated`.

3. **Records are defined once, in the catalog, and generated.** Every
   record has the 32-byte common header (`kind`, `flags`, `size`, `seq`,
   `session`, `at`), then fixed fields at their natural alignment, then a
   tail that `text` and `bytes` spans point into.
   [`protocol/catalog.rb`](../../protocol/catalog.rb) is the only
   definition. `mise run protocol` renders the C structs and codec,
   [MESSAGES.md](../../src/generated/MESSAGES.md) and `messages.json`,
   and `mise run protocol --check` fails on any drift. A message is
   named `<ring>.<name>` with a permanent 16-bit code (`0x01xx` h2c,
   `0x02xx` c2h, `0x03xx` tel). The codec's reader validates every h2c
   record before core trusts it, because h2c is where a host's bytes
   enter core. An unknown kind is a protocol failure, not a record to
   skip: the control block's `version` says which kinds and ownership
   rules both sides understand.

   Values cross as references. A committed `vref` transfers one retained
   reference: to core on h2c, to the host on c2h. tel never carries one,
   because a lossy ring would leak what it drops. A `blob` names a
   host-owned buffer that core reads or writes through a byte source or
   sink; the host keeps it valid until the request's correlated result.

4. **These vtables go, and become records:**

   | Vtable | Becomes |
   |---|---|
   | router (`ta_external_event_router`) | an envelope in core for local mail ([ADR 0099](0099-pending-inputs-are-envelopes-in-one-pool-per-system.md)); `c2h.send` for anything else |
   | I/O processor (`ta_io`) | `c2h.send`, executable on receipt, including before the macrostep end |
   | invoker (`ta_invoker`) | `c2h.invoke`, `c2h.invoke_cancel`, `c2h.invoke_forward`, and `h2c.invoke_report` back |
   | log sink (`ta_log_sink`) | `c2h.log` |
   | trace sink (`ta_trace_sink`) | `tel.trace` and `tel.dropped` ([ADR 0098](0098-telemetry-is-a-lossy-ring.md)) |
   | directory (`ta_actor_directory`) | `c2h.load_needed` (decision 10) |

   **Native integration stays ordinary calls:**
   - creating and destroying a system, and accessing its rings and
     control block;
   - `ta_run(budget)`, which reads h2c, runs macrosteps and writes c2h
     and tel;
   - installing and calling the data model vtable, synchronously, inside
     the macrostep. In-process data models gain nothing from requests on
     a ring, and every action would have to become resumable in the
     middle
     ([data-model.md](../../../docs/proposals/ring-proto/reference/data-model.md#why-the-data-model-is-not-on-a-ring));
   - value ownership (the value vtable, host expressions) and byte
     sources and sinks;
   - the clock function core stamps `tel` records with;
   - `ta_system_trim`, which gives free pages back between `ta_run`
     calls;
   - `ta_actor_inspect`, a read whenever core has returned, which marks a
     session stable or active
     ([debugging.md](../../../docs/proposals/ring-proto/reference/debugging.md#inspection-without-attaching));
   - building a definition's spec with core's spec builder, and chart
     codecs (ADR 0053).

   An attached debugger takes the whole system from the production driver
   and uses native primitives. Its commands stay outside the rings; there
   is no operator ring
   ([debugging.md](../../../docs/proposals/ring-proto/reference/debugging.md)).

5. **A full c2h parks the slot**
   ([macrostep.md](../../../docs/proposals/ring-proto/reference/macrostep.md#a-full-c2h-pauses-the-slot)).
   An ordinary unit writes at most one c2h record. Before a unit that may
   write c2h (a `<log>`, or a `<send>` whose type or target is not a
   literal local session), and before it evaluates anything, counts a
   sendid or assigns `idlocation`, core checks that every slot's
   outstanding reserve leaves room for the largest record and its pad.
   Without that room, the slot parks before the unit. The unit runs once,
   after the host drains, so an expression with side effects is evaluated
   once. Other slots go on. `ta_run` returns `TA_RUN_C2H_FULL` when no
   slot can run and no input can be taken. A record is never refused for
   lack of room; `error.execution.capacity.record` is the only c2h
   failure an action has.

   **A full pool or name area fails the action** in the chart, as
   `error.execution.capacity.pool` or `.names`. The rule is: pause where
   the host can free the space, fail where only sessions can.
   - **The wait for c2h cannot deadlock.** The host frees c2h by draining,
     without any session making progress. A slot waiting for c2h waits
     for the host, never for another slot, so no cycle of waits forms.
     The headroom (decision 6) means that once the host drains, at least
     one waiting slot's record fits, and its reserve returns when its
     input completes.
   - **A wait for the pool could deadlock.** An envelope is freed only
     when its receiver takes it. Two slots that each wait for an envelope
     the other would free never go on.

6. **The c2h reserve and its headroom**
   ([ring.md](../../../docs/proposals/ring-proto/reference/ring.md#the-c2h-reserve)).
   - **Before it takes an input, core reserves the input's mandatory
     batch:** `c2h.input` when recorded, `c2h.macrostep_ended`, a
     `c2h.invoke_cancel` per active invocation, `c2h.finished`, every
     autoforward, and `c2h.input_completed` (or `c2h.input_refused`).
     Mandatory records write into the reservation and never wait. A
     paused slot keeps its reservation.
   - **Each reserved record also reserves one pad** of up to
     `maxRecordBytes - 8` bytes, on top of its aligned size. This bound
     holds however several slots' records interleave. One shared pad
     would need an argument about how many wraps occur before a drain,
     and an exact pad from the write position stops holding once another
     slot writes.
   - **Headroom.** Core takes an input only when all slots' reservations,
     the new one included, leave 2 × `maxRecordBytes` of the ring
     unreserved: one ordinary record and its pad. Without it, slots could
     reserve the whole ring, each waiting for room only another's
     completion frees.
   - **Sizing.** `c2hBytes` defaults to
     `(3 × maxConcurrentInputs + 2) × maxRecordBytes`. A slot's smallest
     batch (observation, macrostep end, completion) costs at most 3 ×
     `maxRecordBytes` with pads, and the headroom 2 ×, so every slot can
     hold a reservation at once. The formula uses parameters only,
     because the ring exists before any definition. A larger batch, or an
     explicit smaller `c2hBytes` (minimum 5 × `maxRecordBytes`), lets
     fewer slots hold reservations at once: that costs throughput, not
     correctness. Registration refuses a definition whose batch plus the
     headroom cannot fit an empty c2h.
   - **Work outside an input returns for a drain.** Startup records,
     `c2h.bounced`, `c2h.load_needed`, `c2h.wake` and
     `c2h.definition_result` are reserved before core consumes the h2c
     record or changes state. Without room, `ta_run` returns.

7. **A host never makes draining c2h wait for room in h2c.** If it did,
   core would wait for the host and the host for core. The drain consumes
   every record it reads: it takes the record's references, calls the
   handler and releases the ring space, and never awaits a handler first.
   A result a handler cannot post to h2c goes into the host's own bounded
   buffer, or is dropped by the host's policy; the chart's timeout covers
   it ([ARCHITECTURE.md, "The host loop"](../../../docs/proposals/ring-proto/ARCHITECTURE.md#the-host-loop)).
   Core never waits on h2c either: a full h2c is the host's refusal at
   its edge.

8. **Type registries are declared through h2c.** The I/O processor types
   and invoker types are two `ta_type_registry` sets of atoms, filled by
   `h2c.type_declare` (registry `io` or `invoker`). It is a control
   applied when read, so every later record, in a replay too, sees the
   same declarations. Core checks a `<send type>` or `<invoke type>`
   when the action runs; an undeclared type still raises
   `error.execution` in the chart, which the host could not raise later
   from c2h. Core keeps only the name. `ta_system_register_io`,
   `ta_system_find_io` and `ta_system_register_invoker` go. The binding
   renders `_ioprocessors.<type>.location`, and an origin arrives as text
   in `h2c.event`, so the I/O vtable's `text` callback goes with it.

9. **Definitions are requests and results**
   ([definitions.md](../../../docs/proposals/ring-proto/reference/definitions.md)).
   `h2c.definition_register` carries a handle to a complete native
   `ta_scxml_spec` that the binding built with core's spec builder, a
   data model adapter handle and a diagnostic buffer. Core compiles and
   registers between macrosteps, and answers with `c2h.definition_result`,
   correlated by `cause_seq`. A compile error is ADR 0058's
   `ta_compile_error`, written whole into the host's buffer; if it does
   not fit, the status is `capacity` and nothing partial is written.
   `h2c.definition_release` and `h2c.definition_retire` get the same
   result. Registration keeps ADR 0077's identity, mismatch, adapter and
   host-expression semantics.

   **A spec has no byte layout.** The handle is process-local, like a
   `vref`. Every host builds specs natively, nothing leaves the process,
   and a layout would duplicate `ta_scxml_spec` field for field. A host
   with a chart as text uses a chart codec (ADR 0053) on its side.

10. **No directory vtable**
    ([unloading.md](../../../docs/proposals/ring-proto/reference/unloading.md#no-directory-vtable-not-resident-means-ask)).
    Core knows the resident sessions and nothing else. An envelope for an
    address that is not resident waits on that address's list, and core
    writes `c2h.load_needed`. The host answers from its own index with
    `h2c.load`, `h2c.spawn` or `h2c.load_failed`. Core never calls the
    host in the middle of a `<send>`. A send to an address that never
    existed costs a round trip, then becomes `error.communication`.

11. **One thread, no atomics**
    ([ring.md](../../../docs/proposals/ring-proto/reference/ring.md#one-thread)).
    A `ta_system` and its rings are not thread-safe, as ADR 0017 already
    assumes. Core and the host take turns on the system's thread: the
    host writes h2c, calls `ta_run`, and reads c2h and tel when it
    returns. A host that works on other threads puts its own queue or
    channel in front of the system. So the counters are plain loads and
    stores, and the same code runs on an MCU, in wasm without threads and
    in Node. Reading a ring from another process is not offered: `vref`s
    and blobs name tables in core's process.

12. **The test host is a ring driver.** `hosted.post` writes h2c,
    `hosted.turn` calls `ta_run`, and `hosted.batches` returns the
    available c2h records, decoded with `messages.json`. A scenario reads
    as an h2c script with expected c2h records, which is how the
    proposal's [scenarios](../../../docs/proposals/ring-proto/scenarios/)
    are written. The test host maps process-local handles (values, specs,
    buffers) in its own process.

**What this ADR does not decide:**

- the envelope pool, admission and fairness
  ([ADR 0099](0099-pending-inputs-are-envelopes-in-one-pool-per-system.md));
  delayed sends ([ADR 0101](0101-delayed-sends-are-envelopes.md)); run-time
  names ([ADR 0100](0100-only-authored-names-are-interned.md)); the content
  of `tel` records and OTLP ([ADR 0098](0098-telemetry-is-a-lossy-ring.md));
- that a macrostep never allocates
  ([ADR 0102](0102-a-macrostep-never-allocates-core-memory.md)), macrostep
  slots, parking and the chain limit
  ([ADR 0103](0103-macrostep-slots-run-inputs-in-turn.md)), and input
  observations on c2h
  ([ADR 0105](0105-recorded-sessions-expose-input-observations-on-c2h.md));
- removing `tinyactors-host`
  ([repository ADR 0003](../../../docs/adr/0003-remove-tinyactors-host.md)),
  and napi as a binding over the rings
  ([napi ADR 0041](../../../tinyactors-napi/docs/adr/0041-napi-is-a-binding-over-cores-rings.md));
- the debugger's attachment API, and the record shapes for reading,
  exporting and enumerating definitions;
- whether `ta_definition_sink` (ADR 0079) stays: the plan removes it only
  if nothing uses it once definitions are requests;
- the measured h2c and tel defaults (step 4.2).

## Consequences

- **What earlier ADRs become:**
  - **0015, superseded:** an I/O processor is no longer called inside the
    macrostep. `c2h.send` is a record the host executes on receipt.
  - **0040, superseded:** core no longer submits to a host router. Mail
    between local sessions stays in core, in the pool of ADR 0099, which
    also supersedes 0040.
  - **0016, refined:** the invocation boundary is records, and the
    invoker registry is a set of declared types. The invoker vtable goes.
  - **0037, refined:** core keeps the resident sessions; the unloaded
    ones are the host's index. The directory vtable goes.
  - **0045, refined:** `<log>` becomes `c2h.log`.
  - **0058, refined:** the same compile-error struct, written into
    host-provided memory, complete or not at all.
  - **0068, refined:** the I/O registry is a set of type atoms declared
    by `h2c.type_declare`, and an unsupported `<send type>` still raises
    `error.execution` in the sender. The adapter and its `text` callback
    go; origins are text.
  - **0077, refined:** registration, release and retirement are h2c
    requests with one correlated result each; identity and holds are
    unchanged.
  - **0079, refined:** registration and retirement that the host asks
    for are answered on c2h.
- **A send is never refused for lack of ring space.** A full c2h is
  latency for one slot, not an error in the chart. A ring's size is a
  throughput setting, not a correctness one.
- **The chart does not feel a slow I/O processor.** The host either
  buffers what it drains, within its own bound, or stops draining, and
  the slot waits. A chart that wants to bound work in flight clocks it by
  acknowledgements.
- **Every binding's host loop is the same short loop:** post h2c, call
  `ta_run`, handle each c2h record on receipt, drain tel, arm one timer
  from `c2h.wake`. No binding routes mail, orders inputs for a busy actor
  or holds a macrostep's effects.
- **The protocol is testable as data.** Scenarios are h2c scripts with
  expected c2h records, and the catalog's JSON form decodes them in any
  language.
- **Callers change:** napi's pump and services, and the test host's
  `hosted.*` methods, move to the rings. The kernel's router, I/O,
  invoker and log adapters and its effects queue have nothing left to do.

### Costs

These figures are the proposal's, on a 64-bit host with the defaults
(`maxRecordBytes` 4,096, `maxConcurrentInputs` 64). Step 4.2 of the
[implementation plan](../../../docs/proposals/ring-proto/IMPLEMENTATION.md)
measures the h2c and tel defaults, and checks the derived `c2hBytes`
against napi's suites, `scale.js` and the IRP suite.

| | Before | After |
|---|---|---|
| h2c | none | 16,384 bytes (16 KiB), fixed at creation; about 107 `h2c.event`s |
| tel | none; the trace sink is a call per record | 16,384 bytes, fixed at creation; 0 with `telBytes` 0 |
| c2h | none | derived: 794,624 bytes (776 KiB) at 64 slots; 20,480 bytes (20 KiB) with one slot; fixed at creation |
| Control block | none | a few hundred bytes per system |
| An external event | a `ta_actor_deliver` call with a borrowed event | `h2c.event`: 128 fixed bytes, about 152 with name and origin, until core admits it |
| A send to an I/O processor | a vtable call; the kernel copies it into its effects queue | `c2h.send`: 120 fixed bytes, about 184 with sendid, type, target and name, until the host releases it |
| Reserve per input in a slot | none | its mandatory batch, each record with one pad: at most 12 KiB for the smallest batch |
| Per actor, per envelope | | nothing from this ADR |
| Allocations in a macrostep | vtable calls into the host, which may allocate | none for ring writes |

**The trade-off against ADR 0093.** ADR 0093 found napi's roughly 1 MB
per system (1.05 MB RSS) too much for quiet systems and for test suites
that create many, and brought a default napi system to 97,464 native
bytes (124 KB RSS). The rings are fixed at creation, because the host
holds positions in them, so a default system now reserves 827,392 bytes
(808 KiB) of rings before it runs anything. With the pool and the name
area at their bounds, a system's defaults come to about 1.5 MiB
(BEHAVIORS.md's 1,512 KiB before the mandatory floor). Most of it is c2h.
Two things keep what a system pays smaller:

- the pool and the name area fill on demand up to their bounds (ADR
  0093's shape, ADR 0099), so a quiet system pays for its peak;
- c2h follows `maxConcurrentInputs` and `maxRecordBytes`. A test suite or
  a quiet system that sets one slot gets 20 KiB of c2h without choosing a
  number; 1 KiB records give 194 KiB at 64 slots. A busy server raises
  h2c and tel.

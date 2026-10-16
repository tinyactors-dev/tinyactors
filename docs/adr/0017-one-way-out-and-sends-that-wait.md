# ADR 0017: A host has one way out to other systems, and a sender waits for its send

**Status:** Proposed (2026-10-16). Decisions 1 and 2 were decided by the
maintainer on 2026-10-16; decisions 3 to 5 wait for the maintainer.
Amends [ADR 0014](0014-other-systems-are-an-io-processor.md) (decision 1's
`"remote": true` processor) and replaces `TA_IO_PROCESSOR_SYNC`.
**Date:** 2026-10-16

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the application developer who connects systems** (processes, a
  browser tab, a mobile app). Today they declare an I/O processor with a
  type they invent (`urn:app:mesh`), mark it `"remote": true`, and copy a
  send's fields by hand into the other host's input, losing some of them.
- **Then: the chart author.** A `<send>` is dispatched, or has failed, when
  the next element of the chart runs (SCXML 6.2, `docs/SCXML.md:1815`; the
  internal queue, `:851`). Today that holds for an I/O processor outside
  core only if the config says `"sync": true`; without it the order of
  events changes without an error (W3C tests 201 and 577 fail, below).
- **Then: the binding author and the C embedder.** One way out of the host,
  one meaning of a SEND, no flag to explain.
- **Out of scope:** routing in core (a table of systems, peers, links), any
  transport, delivery guarantees, trust between hosts, identity across
  restarts, and the encoding of values on the wire. They stay the
  application's, as ADR 0014 decided.
- **Who pays:**

  | Cost | What |
  |---|---|
  | per event inside a segment | nothing |
  | per send to another segment of the host | the sender waits until the host has routed it, in the same `ta_host_run` |
  | per send to an I/O processor, a service or another system | the sender waits until the batch with its SEND is acknowledged; the other actors go on |
  | memory | one parked-sender entry per waiting actor (an index queue in the segment) |
  | API | `ioProcessors[].remote`, `ioProcessors[].sync`, `TA_IO_PROCESSOR_REMOTE`, `TA_IO_PROCESSOR_SYNC` and `TA_RUN_EFFECTS`'s "a sync send" meaning go; the transport, a host input to fail a send, the message shape and the address's parts come |
  | every binding | a `transport` handler; the dispatch reads SEND indices from 1; the message's two halves |

## Context

`"remote": true` marks one entry of `ioProcessors` as the processor for
sends to other systems. That entry is not a processor of its own: core
copies its `sync` onto the host's SCXML processor
(`tinyactors-core/src/host/host.c:380-386`), a chart that names its type
gets the same local-or-not decision as an SCXML send (`host.c:1337-1340`),
two such entries are refused only by the config parser
(`src/host/request.c:1319-1322`), and its invented type and `location` are
advertised to every chart in `_ioprocessors`
(`src/system/system.c:474-477`). The host's own processor `"tinyactors"`
(`host.c:351`) is advertised there too, by mistake, and works as a type.

`sync` makes the run stop right after a send to that processor, so the
outside can dispatch or fail it before the sender goes on
(`src/system/schedule.c:127-136`). It exists for W3C conformance: with
`"sync": false` on the BasicHTTP processor of `tools/conform/w3c.c`, W3C
tests 201 (`event1` over BasicHTTP must come before a later `timeout`) and
577 (`error.communication` from a BasicHTTP send without a target must
come before an earlier external `event1`) fail on every datamodel (run on
2026-10-16; with `true`, all 191 pass). Nothing sets `sync` on the remote
processor but its own unit test (`tests/unit/test_remote.c:43`, `:164`).

## Decision

1. **A host has one way out to other systems: its transport.** Every send
   whose target address names another system is a `TA_EFFECT_SEND` for the
   transport. It needs no config: the config's `remote` and the `"remote"`
   entry go. A binding gives the transport to the application as a host
   handler (`transport=` in Python); a host without one fails each such
   send (`error.communication`), as a binding does for an invoked service
   with no handler. Core decides only local or not (ADR 0014, decision 1);
   which system goes over which connection is the application's, in the
   handler. There is no routing table in core: the peers of a server
   (browser tabs, mobile installs) get their system ids when they start, so
   a config cannot list them, and a table kept at run time is the peer
   state ADR 0014 removed. Routes would come back only for a sender that
   cannot run code per send, or for routes core needs for its own work
   (moving actors between hosts).
2. **A sender waits for its send.** Every send that leaves the segment
   without a delay (to an I/O processor, an invoked service, another
   segment or the transport) parks its sender right after the `<send>`;
   the other actors go on running. The host resumes the sender when it has
   decided the send: at once for a send it delivers itself (another
   segment), at the ack of the batch for a send it hands out. A failure
   given before that ack (`TA_EVENT_IN_ERROR_COMMUNICATION`) is in the
   sender's internal queue when it goes on, so every chart sees `<send>` as
   SCXML describes it. `ioProcessors[].sync` and `TA_IO_PROCESSOR_SYNC` go;
   `TA_RUN_EFFECTS` means "senders wait for this batch: handle it and run
   again". A parked sender is not at rest, so hibernation and migration
   wait for it as for any busy actor, and a snapshot is refused while its
   batch is open, as today; if the scheduler needs it, the snapshot and
   hibernation contracts may change instead.
3. **Effects number processors as the system does** (proposed): 0 is the
   SCXML processor, so the transport; config entry `i` is `i + 1`;
   `TA_NIL` stays an invoked service. The `"tinyactors"` entry leaves
   `_ioprocessors` and stops working as a type.
4. **A message between systems has one shape in every binding**
   (proposed). The outgoing half, `send.message`, is a value of the value
   table with the fields of `ta_event_in` (target, event, data, sendid,
   origin, traceparent); the incoming half, `host.send(message)`, takes
   that value and ignores keys it does not know, so keys are only ever
   added. A failure is never a key of a message: a binding offers an
   explicit input (`host.fail(address, sendid)`) that the application calls
   when it decides to believe one, so a generic receive loop cannot forge
   `error.communication`. The shape is a value, not bytes: core writes
   nothing for another system (ADR 0014, decision 6), and the encoding is
   the application's.
5. **An address has parts** (proposed): a binding's `Address` gives
   `.system`, `.session` and `.name`, read by `ta_address_parse`
   (`include/tinyactors/address.h`), so neither a binding nor an
   application parses the grammar; `send.target` and `send.origin` are
   addresses.

## Consequences

- `include/tinyactors/system.h`, `host.h`, `tinyactors.api.json`,
  `src/system/schedule.c`, `src/host/host.c`, `src/host/request.c` (the
  config, and `host.config`'s reply), `docs/host-config.md`,
  `docs/protocol.md`, `docs/bindings.md`, `docs/architecture.md`,
  `GLOSSARY.md`; the tests, `tools/conform` and the torture chamber's
  configs; `tinyactors-test`'s config vectors and runner; the Python
  binding's API, README and `AGENTS.md`.
- Python's `Host.send` passes `sendid` and `traceparent`, which also fixes
  `_event.sendid` across systems (SCXML 5.10.1, `docs/SCXML.md:1609`).
- What a JavaScript chart sends keeps an address in its data as text: the
  ECMAScript datamodel writes no tag 32.

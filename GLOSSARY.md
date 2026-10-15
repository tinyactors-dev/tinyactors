# Glossary

The words this repository uses, each in a sentence or two, with where to
read more. Grouped by layer, from the chart to the application; within a
group, the words build on each other.

## Charts and the language

- **Chart.** A state machine written in SCXML ([docs/SCXML.md](docs/SCXML.md)):
  states, transitions, executable content, a datamodel. Each `<scxml>`
  document is one chart, named by its `name` attribute.
- **Datamodel.** The language a chart's expressions and data are written
  in, named by its `datamodel` attribute: `ecmascript` (quickjs, in core)
  or `python` (the CPython binding's). It holds each session's variables
  and `_event`, and it owns payloads.
- **Image.** The compiled form of one or more charts (a `.tai` file):
  `ta_compile_charts` writes it, a host runs it. One image becomes one
  segment.
- **Event.** A named message with optional data. *External* events come
  from outside the actor (sends, the application) and wait in its
  mailbox; *internal* events are raised by the actor itself and are
  handled first.
- **Macrostep, microstep.** SCXML's run-to-completion unit: a microstep
  takes one set of transitions; a macrostep runs microsteps until no
  transition is enabled without a new external event. Another actor never
  sees a macrostep half done.
- **Stable point.** An actor between macrosteps, with its internal queue
  empty. Hibernation, export and migration happen only there.
- **Invoke, invocation.** `<invoke>` starts a child while its state is
  active, and cancels it when the state is left. The child is another
  chart (an *internal* invocation, run by core) or a service of the
  application (an *external* invocation, below). The child reports back
  with events, and with `done.invoke.<id>` when it ends.
- **Upgrade event.** `error.platform.upgrade`: the first event an actor
  gets after a migration, carrying its old version's `<datamodel>`
  bindings.
- **`error.communication`, `error.execution`.** SCXML's two error events:
  the first when talking to something outside the actor failed (a send,
  an invoked service), the second when the actor's own content did (an
  expression, a location). Names starting `error.platform` are the
  platform's own.

## Actors and systems (core)

- **Actor.** One running instance of a chart: its state configuration,
  its datamodel variables, its mailbox, its timers.
- **Session, session id.** An actor's identity, a 64-bit number unique in
  its host (`_sessionid` in the chart). It survives hibernation,
  migration, snapshots and a move to another host.
- **System.** Core's unit of execution (`ta_system`): actors of the
  charts of one image, in one block of memory, set up once and never
  allocating after that. It routes events between its actors and keeps
  their timers.
- **Region.** A system's block of memory. It holds only indices and
  offsets, no pointers, so it can be moved, grown by copying, or
  written out as bytes.
- **Instance.** The part of the region one actor's chart state lives in.
- **Payload.** The data of an event, a send, a `<donedata>` or an
  invoke's params, as a handle the datamodel that made it owns; core
  retains and releases it, and never reads it.
- **Effect.** Something an actor asks the outside world to do, queued for
  the embedder: a send to an I/O processor, a log line, an invoke of a
  service, a wake, a reply. The embedder handles effects; core never does
  I/O.
- **Fuel, budget.** Work counted in steps, not time: a run is given a
  budget, and the host shares it fairly among segments, so one busy
  actor cannot starve the rest.
- **Delayed send, timer.** `<send delay=...>`: an event due later, kept
  by the system on the host's clock.
- **Request.** A pending piece of work a system keeps in its request
  pool: an external invocation while it runs, a fetch of a chart not in
  the image, a spawn parked until a slot is free.

## The host

- **Host.** Many systems run as one actor system (`ta_host`): one
  segment per image, routing between them, chart versions, migration
  jobs, hibernation, snapshots, the control plane and telemetry. A
  binding's API sits on the host.
- **Segment.** One system inside a host, made from one image by
  `ta_host_add_image`. Segment k numbers its sessions k·2³²+1 and up, so
  a session id is unique across the host. A segment whose charts all
  have newer versions and no actors left is freed (`SEGMENT_FREED`).
- **Version.** The newest segment that has a chart of a given name holds
  that chart's current version: new spawns run it. Adding an image with
  a chart of the same name is a deploy.
- **Chart reference.** What `ta_host_chart(name)` returns: a segment and
  a chart index, the current version of that chart.
- **Migration.** Moving running actors from an old version of their
  chart to the current one, each at its stable point, keeping its session
  id, its state configuration (states match by id) and its delayed sends;
  it gets the upgrade event first.
- **Job.** A migration in progress (`ta_host_migrate` returns its id). It
  moves actors a few per run, retries the busy ones, asks for the
  hibernated ones with a WAKE when told to wake them, waits for them
  otherwise, and can time out. It ends with a `JOB_DONE` effect that says
  how many moved, how many failed and which were left behind.
- **Router.** The host's part that delivers a send to a session in
  another segment, or out to a peer.
- **Effects, take, ack.** The host's loop: *take* a batch of effects
  (`ta_host_effects`), *handle* them, *ack* the batch
  (`ta_host_effects_ack`), *run* again (`ta_host_run`). Everything the
  host hands out with a batch stays valid until its ack; a run refuses
  while a batch is open.
- **Turn, drain.** A binding's names for one pass of that loop (drain
  the effects, advance the clocks, run, drain again) and for taking and
  handling every batch until none is left.
- **I/O processor.** A transport an actor sends through
  (`<send type=...>`), declared in the host's config and run by the
  application: HTTP, a queue, an application callback.
- **External invocation, service.** An `<invoke>` of a type the
  application runs (an HTTP client, a database, a model). The host
  hands it out as an `INVOKE` effect, sends to it as `SEND` effects with
  no I/O processor, its cancel as `UNINVOKE`; the application answers
  with events that carry its *token*.
- **Token.** The handle of an external invocation in its segment's
  request pool. A service is keyed by (segment, token), never by invoke
  id.
- **Control plane.** Questions to the host about itself (an actor's
  state, the host's stats, traces): a message posted with `ta_host_post`
  and answered by a `REPLY` effect in a later batch. A binding offers a
  *typed method* per message, which takes a response handler.

## Taking actors out of memory, and moving them

- **Hibernate, hibernation.** An actor at a stable point written to a
  *blob* and removed from memory; its record (session id, mailbox,
  timers) stays in the host, so events and timers for it still arrive.
  The application stores the blob. Only an actor at a stable point with
  an empty mailbox, no children, no external invocations and no pending
  request can hibernate.
- **Wake.** Bringing a hibernated actor back: the host asks with a
  `WAKE` effect when an event or a timer arrives for it, or a job wants
  it, and the application answers with `ta_host_wake` and the blob. A
  blob wakes only in the host that wrote it, or in a host restored from a
  snapshot of that host taken after it was written.
- **Sweep.** The host's CLOCK sweep (`ta_host_sweep`): the sessions that
  did not run since the last sweep and can hibernate now, so the
  application can keep at most so many resident.
- **Snapshot, restore.** The whole host as bytes (every segment's region,
  the datamodels' state, the images) and a new host made from them. Same
  build of the library only, never a backup. It is a copy of the whole
  application: rehearse a deploy on it, capture a host for a developer,
  start tests from one prepared world, or take a host out of the process
  for a while. A restored host announces each external invocation that
  was running again, so the application starts its service again, a
  stand-in, or tells the invoker it cannot.
- **Pack.** The one-blob form of a snapshot (`ta_host_snapshot_pack`):
  the host's snapshot, the datamodels' state, the images and the system
  id.
- **Export, adopt.** Moving an actor to another host: export writes it to
  a blob and removes it, adopt makes it run in the other host with the
  same session id.
- **Peer, envelope.** Another host this one exchanges events with, and
  the CBOR message that carries an event between them. Within one
  process the struct form is used and nothing is encoded.
- **Durability.** None of the above is durability. Keeping data is the
  application's database.

## Observability

- **Trace, span, traceparent.** Core records what actors did as spans of
  traces; a W3C `traceparent` carries a trace across sends, the
  application's requests and other hosts.
- **Telemetry, OTLP.** Traces, logs and metrics, encoded by core as OTLP
  and handed to the application's exporter.

## Bindings

- **Binding.** A language's API over the host (`tinyactors-cpython`,
  `tinyactors-napi`): it exposes the core and nothing else, owns the
  language's values and their encoding, and runs the host's loop.
- **Embedder.** Whoever drives the host: a binding, or a C program.
- **Thin view.** An object a binding hands out for an effect, reading the
  effect's fields when asked; stale after the batch's ack.
- **Record.** The same fields copied out, for code that answers after the
  ack (an `async` handler, an invocation's service).
- **Value table.** The values that cross between languages and hosts
  (integers, floats, strings, bytes, lists, maps, booleans, null,
  undefined, addresses) and their CBOR form
  ([tinyactors-core/docs/protocol.md](tinyactors-core/docs/protocol.md)).
- **Direct layer, convenience layer.** A binding's API that mirrors the
  C calls one to one, and a later layer in the language on top that adds
  shapes, never behaviour.

## Working in the repository

- **Conformance suite, scenario, milestone.** One suite every
  implementation runs (`tinyactors-test`): protocol vectors, the W3C
  tests, and scenarios (charts that check themselves while the host is
  driven through steps). A milestone (M1 to M5) is the part of it a
  binding is held to.
- **Gate.** A stress test a feature must pass at scale before it is
  done, with throughput and memory measured (`tools/*.py` in a binding,
  the torture chamber in core).
- **Coordinator.** The session that owns core, the suite and the root
  documents while binding sessions own their directories.
- **Proposal, ADR.** An outline of possible work
  ([docs/proposals](docs/proposals)), not an instruction; a decision
  record ([docs/adr](docs/adr)), which wins over a proposal.

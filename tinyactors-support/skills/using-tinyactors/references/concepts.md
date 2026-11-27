# Concepts and words

The words tinyactors uses, as its
[GLOSSARY.md](https://github.com/tinyactors-dev/tinyactors/blob/main/GLOSSARY.md)
defines them, grouped from the chart outwards to your application. Use
them as written here: the documentation, the error messages and the
maintainers use them so.

## The layers

| Layer | It decides | You meet it as |
|---|---|---|
| **Core** (C) | what inputs mean, and runs actors in one block of memory | `CompileError`, `error.execution`, statuses such as `TA_ERROR_STALE` |
| **Host** (C) | many systems run as one: versions, routing, hibernation, migration, snapshots | `Host`, segments, `host.migrate`, `host.snapshot` |
| **Binding** (Node, Python) | values in your language, and the host's loop on your event loop | `@tinyactors/node`, `tinyactors` |
| **Your application** | storage, policy, I/O, who may do what | your I/O processors, your store, your database |

The core never does I/O and never decides policy. Whatever your program
must keep (a database row, a file, a message on a queue) is your code's
job, reached through an I/O processor.

## Charts

- **Chart.** A state machine written in SCXML: states, transitions,
  executable content, a datamodel. Each `<scxml>` document is one chart,
  named by its `name` attribute; you spawn actors by that name, not by
  the file's.
- **Datamodel.** The language of a chart's expressions and data:
  `ecmascript` (JavaScript) or `python` (with the Python package). With
  `@tinyactors/node`, `ecmascript` runs either *native* (your own engine,
  for trusted charts, the default) or on *quickjs* (a sandbox, for charts
  you do not trust).
- **Image.** The compiled form of one or more charts. `compileCharts`,
  `compileDir` (Python: `compile_charts`, `compile_dir`) make one; a host
  runs it.
- **Event.** A named message with optional data. *External* events come
  from outside the actor and wait in its queue; *internal* events
  (`<raise>`) are the actor's own and are handled first.
- **Macrostep.** SCXML's run-to-completion unit: an actor handles one
  external event and everything it raises before it takes the next. No
  other actor sees a macrostep half done.
- **Stable point.** An actor between macrosteps. Hibernation, migration
  and snapshots happen only there.
- **Invoke.** `<invoke>` starts a child while its state is active and
  cancels it when the state is left: another chart, or a *service* your
  program runs.
- **`error.communication`, `error.execution`.** SCXML's two error events:
  the first when talking to something outside the actor failed (a send,
  a service), the second when the actor's own code did. They are events,
  not exceptions: handle them with `<transition event="error...">`.

## Actors and systems

- **Actor.** One running instance of a chart: its state, its variables,
  its queue, its timers.
- **Session.** An actor's identity, a 64-bit number unique in its host
  (`actor.id`, a `bigint` in JavaScript; `_sessionid` in the chart). It
  survives hibernation, migration and snapshots.
- **Name.** An optional name given at spawn (`{ name: 'order.17' }`),
  which `host.actor(name)` and addresses find. The host does not refuse
  a second actor under a name already taken: within one version the
  oldest holder keeps it, and a newer version's holder wins. So give
  each name one owner, the actor that spawns it (below, "Collection
  actor"). Use only letters, digits, `-`, `.`, `_` and `~`, so an
  address holds the name as it is.
- **Address.** How actors find each other:
  `tinyactors://<system>/@<name>` for a named actor,
  `tinyactors://<system>/#_scxml_<session>` for any. A chart sends with
  `<send target="tinyactors://shop/@order.17" event="...">`.
- **System.** Core's unit of execution: the actors of one image, in one
  block of memory, routing events between themselves. Its **system id**
  is the host's `system` option and the first part of every address.
- **Effect.** Something an actor asks the outside world to do: a send to
  an I/O processor, a `<log>` line, an invoke of a service, a wake, an
  actor's end. The host hands effects to your handlers; the core never
  does I/O.
- **Delayed send, timer.** `<send delay="30s">`: an event due later, kept
  by the host on its clock, cancelled with `<cancel sendid>`.

## The host

- **Host.** Many systems run as one actor system: one **segment** per
  image, routing between them, chart versions, migration, hibernation,
  snapshots, telemetry. Your program makes one `Host` per system it runs.
- **Segment.** One system inside a host, made from one image. The charts
  you give `new Host` are segments 0, 1, ...; `host.addImage` adds one.
- **Version.** A chart's newest segment holds its current version: new
  spawns run it. Adding an image with a chart of the same name is a
  deploy; running actors finish on their version unless you migrate them.
- **Migration.** Moving running actors from an old version of their
  chart to the current one, each at its stable point, keeping its
  session, its state configuration (states match by id), its delayed
  sends and its services. Each actor first gets the event
  `error.platform.upgrade` with its old variables.
- **The loop: take, handle, ack, run.** The host hands out effects in
  batches, your handlers handle them, the binding acknowledges the batch,
  and the host runs again. The bindings do this for you; what you see is
  that **a handler's argument is valid only while the handler runs** (a
  *thin view*), unless the handler returns a promise (Node) or is an
  `async def` (Python), which gets a *record* it may keep.
- **I/O processor.** A transport an actor sends through
  (`<send type="db">`), given to the host by your program: an object
  whose `receive(send)` does the I/O and answers (`send.answer(event,
  data)`) or fails (`send.fail()`, which gives the sender
  `error.communication`).
- **Service (external invocation).** An `<invoke type="...">` your
  program runs: a connection, a subprocess, a long computation. It lives
  while its state is active; it sends back with `invocation.sendBack`
  (`send_back` in Python) and ends with `invocation.done`.
- **Control plane, typed method.** Questions to the host about itself
  (an actor's state and variables, the host's counters, traces): typed
  methods such as `actor.state()`, `actor.context()`, `host.hostStats()`,
  answered in a later turn, as promises in Node and awaitables under
  asyncio.

## Taking actors out of memory, and moving them

- **Hibernate, wake.** An actor at a stable point written to a *blob* and
  removed from memory; its record (session, queue, timers) stays in the
  host, so events and timers for it still arrive, and the host asks to
  **wake** it. A blob wakes only in the host that wrote it, or in one
  restored from a later snapshot of that host.
- **Store, resident.** Your place for blobs (`save`, `load`, `delete`;
  `MemoryStore` and `SQLiteStore` ship with the packages) and the most
  actors a host keeps in memory. The binding hibernates the rest after a
  turn and loads one back when the host asks.
- **Snapshot, restore.** The whole host as bytes and a new host made
  from them: a copy of the application at one moment. Restore it to
  rehearse a deploy, start tests from a prepared world, or survive a
  restart. A restored host announces each service that was running, so
  your program starts it again.
- **Other system, transport.** A system with another system id: another
  process, another host, a browser. A send to its address goes to your
  host's `transport` handler; your transport carries `send.message`, and
  the other side gives it to its `host.send(message)`. Delivery and
  encoding are yours.
- **Durability.** None of the above is durability. Data you must not lose
  belongs in your database, written through an I/O processor.

## The application API

- **Actor (the handle).** What `host.spawn` and `host.actor` return: `id`,
  `address`, `send`, `terminate`, `ask`, `state()`, `context()`, `done`.
  A small value; keeping one keeps nothing alive.
- **Spawner.** The I/O processor a binding ships for spawning from a
  chart: a send of `{chart, name, params}`, answered with `spawned` and
  `{name, address}`.
- **Collection actor.** An actor of yours that decides which actors of a
  kind exist (a workspace for its rooms, a project for its issues): the
  only one that spawns those names. It never ends, and hibernates when
  idle.
- **ask.** `await actor.ask(event, data)`: a request and its reply,
  `[event, data]`, answered by the chart with
  `<send targetexpr="_event.origin" event="...">`.

## Observability

- **Trace, span, traceparent.** The core records what actors did as
  spans; a W3C `traceparent` carries a trace across sends, your requests
  and other hosts.
- **Telemetry, OTLP.** Traces, logs and metrics encoded by the core as
  OpenTelemetry (OTLP) and handed to your exporter (the `telemetry`
  handler, `host.metrics()`).

# tinyactors for Python

SCXML actors in a host, driven from Python: charts written in Python (the
`"python"` datamodel) or in ECMAScript (quickjs), a `Host` driven from a
script or from asyncio, and values that are Python objects. The extension
is the direct layer: each item mirrors a call of the C library
([docs/api-map.md](docs/api-map.md) names the core behind every one).

```python
import tinyactors

CHART = """<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
               datamodel="python" name="greeter" initial="idle">
  <datamodel><data id="greeted" expr="0"/></datamodel>
  <state id="idle">
    <transition event="hello">
      <assign location="greeted" expr="greeted + 1"/>
      <log label="greeter" expr="f'hello, {_event.data[&quot;who&quot;]} ({greeted})'"/>
      <send type="urn:example:app" target="out" event="greeted">
        <content expr="{'count': greeted}"/>
      </send>
    </transition>
  </state>
</scxml>"""

def app(host, send):                      # an I/O processor: the actor's output
    print("app got", send.event, send.data)
    send.answer("thanks")                 # an event back to the sender

config = {"systemID": "demo", "ioProcessors": [{"type": "urn:example:app"}]}
with tinyactors.Host(config, {"urn:example:app": app},
                     log=lambda host, entry: print("log:", entry.text)) as host:
    with tinyactors.compile_charts([("greeter.scxml", CHART)]) as compiled:
        host.add_image(compiled.image)    # the host copies the image
    session = host.spawn(host.chart("greeter"))
    host.send(session, "hello", {"who": "world"})
    host.run_until_idle()
    host.actor_context(lambda host, result: print(result["variables"]), session=session)
    host.run_until_idle()                 # the reply's handler runs at a later drain
```

## The API

- `compile_charts(sources, resolver=None) -> Compiled`: `(name, text)`
  pairs into one image. A resolver is an object with `resolve(base,
  source) -> str` that reads a static `src=` at compile time; it refuses
  with `ResolveError(message, line=None, column=None)`. A compilation with
  an error raises `CompileError` with every diagnostic. `Compiled.image`
  is a read-only buffer, valid until `close()` (a context manager).
- `Host(config, io_processors=None, *, log=None, clock=None, peer=None,
  telemetry=None, wake=None, migrated=None)`:
  `config` is the host's config as a dict
  ([host-config.md](../tinyactors-core/docs/host-config.md));
  `io_processors` maps an I/O processor's type to `handler(host, send)`;
  `log` gets each `<log>` line as `handler(host, entry)`; `clock` is a
  `SimulatedClock` or `None` for the real clocks; `peer` gets each
  envelope for another system as `handler(host, envelope)`; `telemetry`
  gets each OTLP request as `handler(host, signal, data)`, `wake` each
  hibernated actor an event or a migration asks for as `handler(host,
  session, job)`, and `migrated` each actor a migration moved as
  `handler(host, migration)` (all below).
- Inputs, applied at the call: `add_image(image) -> segment`,
  `chart(name) -> ChartRef`, `spawn(ref, name=None, params=None,
  traceparent=None) -> session`, `send(session, event, data=None, *,
  traceparent=None)`, `send_many([(session, event, data, traceparent),
  ...]) -> accepted`, `terminate(session)`, `peer_add(id, *,
  trusted=False) -> peer`, `deliver(peer, envelope_bytes) -> route`.
- Drivers: `run_until_idle()`; `run()`, which sleeps to each deadline
  while there is one; `SimulatedClock.increment(ms)`; and under asyncio
  (`async with Host(...)`), `await host.idle()`.
- Typed methods, one per method of
  [requests.md](../tinyactors-core/docs/requests.md):
  `actor_state`, `actor_context`, `actor_list`, `actor_get`, `host_stats`,
  `host_config`, `trace_configure`, `trace_watch`, `trace_unwatch`,
  `trace_end`, `trace_spans_decode`, each `(handler, on_error=None, /,
  **params)`, returning `None`. The method's params are keywords, passed
  to core as given; `handler(host, result)` or `on_error(host, error)`
  runs when the reply is drained, later. An error without `on_error` is
  raised by the driver after the turn.
- `send` and `entry` are thin views: their fields are read when you read
  them, and a read after the batch's ack raises `TinyactorsError` with
  `status == "TA_ERROR_STALE"`. Read what you need inside the handler. An
  `async def` I/O processor (under asyncio only) gets a record instead,
  which it can answer later. A handler that raises before answering has
  its send failed (the sender gets `error.communication`), and the
  exception is raised from the driver after the turn.
- `encode(value) -> bytes`, `decode(data) -> value`: the value table
  ([docs/values.md](docs/values.md)) as CBOR.

A `Host` is used from the thread that made it; other threads are refused.
Several hosts, one per thread, exchange envelopes.

## Telemetry

With `"telemetry"` in the config the host traces its actors and writes
OTLP requests (protobuf): spans of every macrostep, `<log>` lines and
failed actions as log records, and counters as metrics. The host does no
I/O: the binding hands each request to your handler as `bytes` you keep,
`signal` being `"traces"` or `"logs"`, and you post it. Metrics are
pulled with `host.metrics()`. Leaving the `Host` (`with`, `async with`,
`close()`) ends the spans still open (`trace.end`) and hands over the
last requests; a `Host` the collector frees does not, so leave it. Posting,
retrying and capping what waits are yours, and the handler runs in the
turn: a slow post holds the loop (and, under asyncio, the event loop),
so give it a timeout, or make it an `async def` handler, which runs as a
task (`async with` awaits the last take's). A handler's exception is
raised by the driver after the turn, the first of a turn; every request
whose handler raised is lost.

```python
import urllib.request

def export(host, signal, data):           # OTLP/HTTP, to a collector or otel-tui
    request = urllib.request.Request("http://localhost:4318/v1/" + signal, data,
                                     {"Content-Type": "application/x-protobuf"})
    urllib.request.urlopen(request, timeout=2).close()

config = {"systemID": "shop",
          "telemetry": {"flushInterval": 5_000_000_000,       # host clock: ns by default
                        "resource": {"service.name": "shop"}}}
with tinyactors.Host(config, telemetry=export) as host:
    ...
    export(host, "metrics", host.metrics())
```

**Application spans above actors.** A `traceparent` given to `send` or
`spawn` makes the macrostep it starts a child of the application's span,
in the application's trace; sends between actors keep the trace, and an
I/O processor gets the sending step's context in `send.traceparent`.
With the OpenTelemetry SDK:

```python
from opentelemetry import propagate, trace

tracer = trace.get_tracer("shop")

def warehouse(host, send):                # continue the actor's trace in the application
    with tracer.start_as_current_span("warehouse.reserve",
                                      context=propagate.extract({"traceparent": send.traceparent})):
        carrier = {}
        propagate.inject(carrier)
        send.answer("reserved", traceparent=carrier["traceparent"])

with tracer.start_as_current_span("POST /orders"):
    carrier = {}
    propagate.inject(carrier)
    host.send(session, "order", order, traceparent=carrier["traceparent"])
    host.run_until_idle()
```

A backend then shows one trace: `POST /orders`, the actors' macrosteps
and microsteps below it, `warehouse.reserve` below the step that sent to
the warehouse, and the macrostep its answer started below that.

`flushInterval` is in the host's clock (`system.ticksPerSecond`); without
it a request is ready only when the staging is full, or at the stop.
`host.trace_spans_decode(handler, traces=data)` decodes a traces request
back into spans, and `trace_configure` and `trace_watch` choose what is
traced ([requests.md](../tinyactors-core/docs/requests.md)). A watch adds
rules for a while, on the host's clock, in front of the configured ones:
with tracing sampled down, it traces one chart in full until it ends.

```python
host.trace_configure(print, sample=0.01)                 # 1% of new traces
host.trace_watch(lambda host, result: print("watch", result["id"]),
                 rules=[{"chart": "checkout", "sample": "sample", "detail": "full"}],
                 seconds=60)                             # every checkout, for a minute
```

## Hibernation

A host keeps millions of actors when only some are in memory: the rest
are blobs in the application's store. The host does no I/O; the binding
hands each blob over as `bytes`, and asks for it back.

- `host.sweep(max_scan, limit) -> [session, ...]`: the CLOCK sweep, at
  most `limit` actors that did not run since the previous sweep and can
  be hibernated now.
- `host.hibernate(session) -> bytes`: the actor leaves memory, and its
  blob is yours to store. An actor that is not at rest (events waiting,
  in a macrostep, an invoked child) raises `TinyactorsError`, and so does
  a `"python"` actor holding a value outside the value table (a `set`, a
  `tuple`, an object): it stays resident. A pending delayed send stays
  in the host, and when it falls due it wakes the actor.
- `wake=handler(host, session, job)`: an event (or a due delayed send)
  came for a hibernated actor (`job` is `None`), or a migration asks for
  it (`job` is its id, below); it waits in the mailbox while your handler
  loads the blob and calls `host.wake(session, blob)`, now or later (an
  `async def` handler runs as a task, under `async with`). One call per
  hibernation, however many events come. Without a `wake` handler the
  call is dropped: the actor stays hibernated, and its events wait, until
  you call `host.wake`.

```python
import sqlite3

db = sqlite3.connect("actors.db", isolation_level=None)
db.execute("CREATE TABLE IF NOT EXISTS blobs (session INTEGER PRIMARY KEY, blob BLOB)")

def wake(host, session, job):
    (blob,) = db.execute("SELECT blob FROM blobs WHERE session = ?", (session,)).fetchone()
    host.wake(session, blob)

def evict(host):                          # after a turn: the cold actors leave
    for session in host.sweep(1 << 20, 1024):
        try:
            blob = host.hibernate(session)
        except tinyactors.TinyactorsError:    # not at rest, or a value outside the table
            continue
        db.execute("INSERT OR REPLACE INTO blobs VALUES (?, ?)", (session, blob))
```

**Plan the capacity up front.** A hibernated actor keeps its slot and its
session's record in the host (about 60 B), so `system.maxActors` counts
every actor; instance and string memory are needed for the resident ones
only. Left to grow, a segment doubles all its pools together and sizes
instance memory from `maxActors`, about 31 KB per actor at two million.
For `R` actors resident out of `N`:

```python
config = {"systemID": "big", "system": {
    "maxActors": N + room, "maxEvents": 4096,
    "instanceBytes": 2048 * (R + room), "stringBytes": 16384 + 64 * (R + room)}}
```

`mise run stress` is the pattern at scale (`tools/stress.py`): two
million actors of a counter chart, at most 1000 resident, the rest in
SQLite; 200,000 events to actors drawn at random, so nearly every event
wakes one from the store and the sweep sends another back. Measured on
x86_64 Linux, 4 CPUs, CPython 3.13.16 with the GIL, one run:

| datamodel | populate (spawn and hibernate 2,000,000) | 200,000 events (woken, swept back) | peak resident | core's region | SQLite file |
|---|---|---|---|---|---|
| `"python"` | 8.1 s (246,000 a second) | 9.3 s (21,400 a second; 199,869 wakes) | 170 MB (85 B per actor) | 121 MB (60 B per actor) | 355 MB |
| quickjs | 14.2 s (141,000 a second) | 11.9 s (16,800 a second; 199,869 wakes) | 172 MB (86 B per actor) | 121 MB (60 B per actor) | 409 MB |

The region is core's (about 60 B per actor, hibernated or not); the
store is SQLite's file. Every actor's count matched the events it was
sent, and after every eviction at most 1000 actors were resident.

## Migration

A new version of a chart (`add_image` of an image with a chart of the
same `name`) runs the actors spawned from then on; `host.migrate` moves
the running ones to it, at their stable points, in the next runs. Each
moved actor keeps its session id, its state configuration (states match
by id) and its delayed sends, and gets `error.platform.upgrade` first,
whose data is version 1's `<datamodel>` bindings: version 2 takes from
there what it keeps.

```xml
<transition event="error.platform.upgrade">
  <assign location="n" expr="_event.data['n']"/>
</transition>
```

- `host.migrate(handler, /, *, chart=None, session=None, wake=False,
  timeout=0) -> job`: every chart, one `chart` by name, or one `session`.
  With `wake`, a hibernated actor is asked for (`wake=handler(host,
  session, job)` with this job's id); without it, it waits, and the job
  ends with it in `pending` at `timeout` (host ticks; 0: none).
  `handler(host, result)` runs when the job ends, with `{"job",
  "migrated", "failed", "pending": [session, ...]}`.
- `migrated=handler(host, migration)`: each actor moved, a thin view of
  `session`, `segment`, `chart` (its name) and `job`.
- **A job ends when every actor it looks at moved, or at its
  `timeout`.** A hibernated actor moves once it is woken, by the job's
  WAKE or by an event's; with `wake`, the job asks again when it finds an
  actor hibernated after a wake. Without `wake`, an actor nobody wakes
  holds the job until its timeout, or for good without one, and its
  handler never runs: give such a job a `timeout`. A run costs the job a
  quantum of actors (`migrateQuantum`), however many it waits for.
- An actor that cannot be written out stays on its version and counts as
  failed: on the `"python"` datamodel, one whose variables or pending
  delayed sends hold a value outside the value table. One that is running
  an invocation waits until it ends.

**With most actors hibernated, wake the job's actors a batch at a
time.** The job asks for every hibernated actor at once, early; wake them
all and they are all resident. Queue the job's WAKEs and wake a batch
per round: the job takes each woken actor in its next runs. An event's
WAKE (`job` is `None`) wakes its actor at once, and the job takes that
one too. Put actors back to sleep once they moved (one put back before
is asked for again: work, not a stall).

```python
asked = collections.deque()

def wake(host, session, job):
    if job is not None:
        asked.append(session)             # the job's: a batch at a time
    else:
        host.wake(session, store.get(session))   # an event's: now

job = host.migrate(on_done, wake=True)
while not done:
    for _ in range(min(256, len(asked))):
        session = asked.popleft()
        host.wake(session, store.get(session))
    host.run_until_idle()
    evict_moved(host)                     # the moved actors at rest go back to the store
```

`mise run migration` is the pattern at scale (`tools/migration.py`), on
SQLite: 2,000,000 actors of a counter chart, at most 1000 resident, moved
to version 2 while 200,000 events go to actors drawn at random; it
checks every sampled actor's count and version, that the job left none
behind, that version 1's segment retired, and the resident bound after
every eviction. Measured on x86_64 Linux, 4 CPUs, CPython 3.13.16 with
the GIL, one run:

| datamodel | populate (2,000,000, 200,000 events) | migrate, live (200,000 events meanwhile) | woken from SQLite | peak resident | core's region (both versions, then after) | SQLite file |
|---|---|---|---|---|---|---|
| `"python"` | 37.9 s | 89.7 s (22,300 actors a second) | 2,218,199 | 487 MB (244 B per actor) | 243.6 MB, then 121.8 MB (61 B per actor) | 556 MB |
| quickjs | 39.2 s | 102.2 s (19,600 actors a second) | 2,218,199 | 488 MB (244 B per actor) | 243.6 MB, then 121.8 MB | 756 MB |

A run costs the job a quantum of actors however many it waits for, so
the time grows with the population: 13.6 s at 500,000 on the
`"python"` datamodel, against 74.9 s when the job went over everything
it waited for in every run. The peak is the migration's: both versions'
regions until version 1 retires, and the job's map of the sessions it
waits for (at most a quarter full after it is rebuilt).

## Snapshots

`host.snapshot() -> bytes` is the whole host in one blob: every
segment's region (actors, queues, timers, the actors' hibernation
records), both datamodels' sessions and payloads, the images, the system
id and the host's time. `tinyactors.Host.restore(snapshot, config, ...)`
makes a new host running it, with the handlers `Host(...)` takes; every
session id stays, and the clock goes on from the snapshot's time (a new
`SimulatedClock`, or the real clock in another process, is offset to it).

```python
blob = host.snapshot()                    # quiescent: not from a handler
host.close()
...
host = tinyactors.Host.restore(blob, config, log=log, wake=wake)
```

- **A snapshot is for the same build of the library, not a backup.** It
  holds raw region memory: take a host out of memory for a while, restart
  it, inspect it. Durability is your database's.
- **The hibernated actors stay in your store.** The snapshot holds their
  records, not their blobs; the restored host asks for them with the
  same WAKE.
- It is refused from a handler and while a migration job runs
  (`TA_ERROR_STATE`), and when a `"python"` session or payload holds a
  value outside the value table (`TA_ERROR_UNSUPPORTED`); the host goes
  on. Values are written through the value table: two variables of one
  session that share a list share it no more after a restore.

`mise run snapshot` is the pattern at scale (`tools/snapshot.py`): a
population on SQLite as in "Hibernation", 200,000 events, a delayed send
for every resident actor, the snapshot written to a file, the host
closed, a new one restored from the file (its own snapshot must be the
same bytes), 200,000 events more and the delayed sends due; every
sampled actor's count must hold both halves. Measured on x86_64 Linux, 4
CPUs, CPython 3.13.16 with the GIL, one run each:

| datamodel | actors | resident | snapshot | snapshot time | restore time | every count and delayed send |
|---|---|---|---|---|---|---|
| `"python"` | 2,000,000 | 1000 | 120.9 MB (60 B per actor) | 0.11 s | 0.07 s | right |
| quickjs | 2,000,000 | 1000 | 120.9 MB | 0.13 s | 0.07 s | right |
| `"python"` | 100,000 | all | 225.3 MB (2253 B per actor) | 0.59 s | 0.44 s | right |
| quickjs | 100,000 | all | 232.3 MB (2323 B per actor) | 1.50 s | 0.78 s | right |

A snapshot is mostly the region, as the config sized it; of the 225 MB of
100,000 resident `"python"` actors, about 11 MB is the datamodel's state.

## Charts in Python

A `datamodel="python"` chart's session is an instance of a class made for
the chart: its `<data>` ids are its slots, `_event`, `_sessionid`, `_name`
and `_ioprocessors` are read-only, and `In(state)` is SCXML's `In()`.
Expressions are compiled once per chart into functions of the session,
by a restrictive rewrite:

- *expressions:* literals, arithmetic, comparisons, boolean operations,
  conditional expressions, indexing and slicing, comprehensions and
  generator expressions, f-strings, calls to the chart's methods and to
  the allowed builtins, attribute access except dunder names;
- *statements*, in `<script>` and `.py` files a `src=` names: assignment,
  plain or augmented, to a `<data>` id or to its items or attributes;
  `if`, `for`, `while`, `pass` and expression statements; `def` at the top
  level of a script, which makes a method of the chart;
- *refused:* `import`, `global`, `nonlocal`, `lambda`, `await`, `yield`,
  the walrus (`:=`), `del`, `try` and `raise`, `with`, `class`;
- *builtins:* `len`, `min`, `max`, `sum`, `abs`, `round`, `sorted`,
  `reversed`, `enumerate`, `zip`, `range`, `any`, `all`, `isinstance`,
  `str`, `int`, `float`, `bool`, `list`, `dict`, `tuple`, `set`,
  `frozenset`, `map`, `filter`.

An undeclared name, a refused construct or an assignment to a system
variable is `error.execution` when the expression is evaluated, as SCXML
requires; a `.py` file's refused constructs are compile errors at their
line. What crosses to another actor (`<send>` data, `<donedata>`, invoke
params, data from the application) is copied through the value table:
actors share no mutable value.

## Limits

- **Charts are trusted code. The rewrite is not a sandbox:** it keeps a
  chart inside SCXML's scope rules, nothing more. A chart in Python runs
  in your interpreter, cannot be preempted in the middle of an
  expression, and is limited in memory by nothing (unlike quickjs, which
  has an operation limit and its own allocator).
- **Python values lie outside the host's allocator.** The host, its
  tables and its scratch allocate through `PyMem_RawMalloc`; the values
  of a Python chart's sessions and payloads are Python objects on
  Python's heap.
- **Nesting is limited to 64,** which stops cycles but not a value that
  shares subtrees (`a = [a, a]` sixty times): another reason charts are
  trusted code.
- A `"python"` actor hibernates and migrates (and exports, once that is
  in the API) only while its variables, and the data of its pending
  delayed sends, are values of the value table; a `<foreach>`'s `item` and `index` must be declared
  `<data>`.
- Not in the API yet: export and adopt, invoked services of the
  application.

## Memory

Measured with `mise run measure` (x86_64 Linux, 4 CPUs, CPython 3.13.16
with the GIL; 10,000 actors of one chart with three `<data>`, resident
and idle; then once more after one event to each actor, which keeps its
`_event` until the next):

| datamodel | resident per actor | traced per actor | core's region per actor | resident after one event | region after one event |
|---|---|---|---|---|---|
| `"python"` | 433 B | 201 B | 204 B | 814 B | 398 B |
| quickjs | 1104 B | — | 204 B | 13777 B | 12440 B |

"Traced" is tracemalloc's: the sessions' Python objects and the
binding's rows (it includes an 8 B array of the harness). After an event
a `"python"` session keeps one more object, its `_event` (about 100 B;
the event's name is a `str` the chart shares). The region's growth after
the events is core's (it grows its event pools by doubling and keeps
them), and much larger on quickjs.

## Building

`mise run build` builds the extension for CPython 3.13 (abi3) and for the
free-threaded test interpreter; `mise run test` lints, runs `tests/` on
both, and runs the conformance suite's milestone set against the adapter
(`python -m conform`) on both datamodels.

## License

MIT, as the repository ([LICENSE](../LICENSE)).

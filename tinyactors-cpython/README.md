# tinyactors for Python

Some programs keep track of many things that each move through steps of
their own: orders that are placed, paid and shipped, devices that connect
and drop, sign-ups that wait days for a confirmation. Written as plain
code, each of these becomes rows in a table, flags, timers and callbacks
spread through the program. Written as a statechart, each is one SCXML
document that says which states the thing can be in and which events move
it between them.

tinyactors runs those statecharts as actors. A **host** holds the actors,
up to millions of them, and runs each one's chart when events reach it.
The host does no I/O. When an actor sends a message out, logs a line or
starts a service, the host calls a Python function you gave it, and that
function does the work. Charts can be written in Python (the `"python"`
datamodel) or in ECMAScript (run by quickjs), and the values actors
exchange with your program are ordinary Python objects.

This package is a direct binding of the C library: each function and
method corresponds to one call of the library
([docs/api-map.md](docs/api-map.md) lists them).
[GLOSSARY.md](../GLOSSARY.md) defines the terms these pages use.

## Installing

    pip install tinyactors

The wheels are built for CPython 3.13 and later, using the stable ABI.
Wheels exist for these platforms:

- macOS 11 or later, on Apple silicon and Intel;
- Linux on x86-64 and arm64, with glibc 2.17 or later or musl 1.1 or
  later (one wheel serves both).

There is no source distribution, so other platforms and the free-threaded
interpreters (3.13t, 3.14t) cannot install it yet. Development builds of
`main` are published as prereleases; once a release is out,
`pip install --pre tinyactors` installs the newest of them.

## A first program

The chart below counts greetings. Each `hello` event increments a
variable, logs a line, and sends the count out to the application.

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

It prints:

```
log: hello, world (1)
app got greeted {'count': 1}
{'greeted': 1}
```

Step by step:

1. **The config** names the system (`systemID`) and declares one I/O
   processor, `urn:example:app`. An I/O processor is a type of `<send>`
   that your program handles. The dict passed after the config maps that
   type to the function `app`.
2. **`compile_charts`** turns the SCXML text into an **image**, the
   compiled form a host runs. `add_image` copies the image into the host,
   so the compile result can be closed as soon as it returns.
3. **`spawn`** starts an actor of the chart and returns its **session**,
   an `int` that identifies the actor from then on. **`send`** puts the
   `hello` event in the actor's mailbox.
4. **Nothing has run yet.** `spawn` and `send` only record their input.
   `run_until_idle()` runs the actors until none has anything left to do,
   and calls your handlers along the way: `log` for the `<log>`, and
   `app` for the `<send>`.
5. **`actor_context`** asks the host for the actor's variables. It returns
   `None` at once. The answer arrives as a reply, and the reply goes to
   its handler during a later run, which is why the program calls
   `run_until_idle()` a second time.

## How a host runs

A host never runs on its own. Your program drives it, and each run
follows the same cycle:

- **Inputs** are the calls that give the host work: `spawn`, `send`,
  `send_many`, `terminate`, `boot`, `wake`. Each is checked and applied
  when you call it, and raises at once when it is refused. The actors
  react later.
- **A turn** is one pass of the driver. It runs the actors that have
  work, for a bounded amount of computation, and collects what they ask
  of the outside world: sends to I/O processors, `<log>` lines,
  invocations, replies to typed methods. These are the host's
  **effects**.
- **Handlers** receive the effects in batches. When every handler in a
  batch has returned, the host **acknowledges** the batch and moves on. A
  handler may call the host again: an input it makes applies at once, and
  the effects that input causes arrive in a later batch.

A plain function handler receives a **thin view**, which reads each field
from the host the first time you access it. A view is valid until its
batch is acknowledged. Reading a field afterwards raises
`TinyactorsError` with `status == "TA_ERROR_STALE"`, so read what you need
inside the handler. What you have already read is a normal Python object
and stays valid. An `async def` handler receives a **record** instead:
every field is copied before the handler is scheduled, so it can answer
after the batch is gone.

When a handler raises, the host still finishes the turn and acknowledges
the batch, and then the driver raises the first exception. A host is
used from the thread that created it; calls from other threads are
refused.

### Images, segments and versions

`add_image` runs an image as a new **segment** of the host and returns
the segment's number. A segment is one actor system with its own memory.
A host can run many segments at once.

When an image has a chart whose name the host already runs, that image
holds a new **version** of the chart. `host.chart(name)` returns a
`ChartRef` for the current version. New actors start on it, and running
actors stay on their version until a migration moves them (below,
"Migration").

`compile_system` compiles a `<system>` manifest instead of a list of
charts. The manifest names the charts (`<chart name src>`) and the actors
the application starts with (`<actor name chart>`). `host.boot(segment)`
spawns those actors under their names, and `host.actor_by_name(name)`
finds one again. For an image from `compile_charts`, `boot` spawns the
first chart compiled.

### Drivers and time

| Driver | What it does | Use it for |
|---|---|---|
| `host.run_until_idle()` | turns until no actor can run at the current time | scripts, tests, programs that run the host between other work |
| `host.run()` | turns, and sleeps until each timer, until no timer is left | a program that is only the host |
| `SimulatedClock.increment(seconds)` | moves a simulated clock forward and turns every host made with it until idle | tests and simulations: a 30-day timeout takes no time |
| `await host.idle()` | under `async with`, the host turns on the event loop by itself; `idle()` completes when it is idle | programs that also serve other I/O |

Time in the API is in seconds, as an `int` or a `float`. Inside the host
time is counted in ticks, at the config's `system.ticksPerSecond`, and
the binding converts. `host.next_deadline` is the time in seconds until
the host's next timer, or `None` when there is none. With a
`SimulatedClock`, `clock.increment(host.next_deadline)` jumps straight to
the next thing that happens.

Under asyncio, an input made outside a turn schedules one, so you only
await `idle()` when you need the effects before going on:

```python
import asyncio

async def app(host, send):                # async def: runs as a task, gets a record
    await asyncio.sleep(0.01)             # your I/O
    send.answer("thanks")

async def main():
    async with tinyactors.Host(config, {"urn:example:app": app}) as host:
        with tinyactors.compile_charts([("greeter.scxml", CHART)]) as compiled:
            host.add_image(compiled.image)
        host.send(host.spawn(host.chart("greeter")), "hello", {"who": "world"})
        await host.idle()

asyncio.run(main())
```

An `async def` handler needs the asyncio driver. Entering a host that has
one with a plain `with` raises `TypeError` naming the handler.

### Typed methods

Questions about the host and its actors are **typed methods**:
`actor_state`, `actor_context`, `actor_list`, `actor_get`, `host_stats`,
`host_config`, `trace_configure`, `trace_watch`, `trace_unwatch`,
`trace_end` and `trace_spans_decode`. Each takes
`(handler, on_error=None, /, **params)`. The params are passed to the host
as given, and the host checks them.

A typed method returns `None`. `handler(host, result)` runs when the
reply is handled, in a later run, with the result as Python values. An
error goes to `on_error(host, error)` as a `TinyactorsError` whose `code`,
`reason` and `path` say what was wrong. Without `on_error`, the driver
raises it after the turn. There is no synchronous form: a host answers
questions in the same cycle as everything else.

The core's [requests.md](../tinyactors-core/docs/requests.md) describes
each method's params and result. Its `actor.send` has no typed method,
because `host.send` does the same.

### Leaving a host

`with`, `async with` and `close()` free the host. Before that, the host
ends the trace spans still open and hands the last telemetry to your
handler (below, "Telemetry"). Handlers of typed methods still waiting for
a reply are dropped without being called, and running services are
dropped without `stop`. The garbage collector frees a host you did not
close, but without the last telemetry, so close it.

## Charts in Python

A `datamodel="python"` chart gets a class of its own, and each actor of
the chart is an instance of it.

- **The variables** are the instance's slots: the chart's `<data>` ids,
  and the names a `<foreach>` (`item`, `index`) or a `<script>` outside
  its functions assigns. The chart declares these names too, as SCXML
  4.6 and a module's top level do. Each is `None` until set.
- **The system variables** `_event`, `_sessionid`, `_name` and
  `_ioprocessors` are read-only. `_event` is an `Event` and stays bound to
  the last event until the next one. `In(state)` is SCXML's `In()`.
- **Expressions** are compiled once per chart, by a restrictive rewrite,
  into functions of the instance.

The rewrite allows:

- *in expressions:* literals, arithmetic, comparisons, boolean operations,
  conditional expressions, indexing and slicing, comprehensions and
  generator expressions, f-strings, calls to the chart's methods and to
  the allowed builtins, and attribute access except to dunder names;
- *in statements* (in `<script>` and in `.py` files a `src=` names):
  assignment, plain or augmented, to a variable or to its items or
  attributes; `if`, `for`, `while`, `pass` and expression statements; and
  `def` at the top level of a script, which makes a method of the chart;
- *these builtins:* `len`, `min`, `max`, `sum`, `abs`, `round`, `sorted`,
  `reversed`, `enumerate`, `zip`, `range`, `any`, `all`, `isinstance`,
  `hasattr`, `str`, `int`, `float`, `bool`, `list`, `dict`, `tuple`,
  `set`, `frozenset`, `map`, `filter`.

It refuses `import`, `global`, `nonlocal`, `lambda`, `await`, `yield`, the
walrus (`:=`), `del`, `try`, `raise`, `with` and `class`.

An undeclared name, a refused construct, or an assignment to a system
variable raises `error.execution` in the chart when the expression is
evaluated, as SCXML requires. In a `.py` file, the same mistakes are
compile errors at their line.

**Write logic in `.py` files.** Inside a chart an expression is XML text,
so `<` is written `&lt;`, `&` is `&amp;`, and a quote inside a quoted
attribute is `&quot;`. Anything longer than a comparison reads better as
a method in a `.py` file that the chart names with `<script src>`. The
file is plain Python, checked by the same rewrite at compile time:

```python
# rules.py
def under_limit(tries, limit):
    return tries < limit
```

```xml
<script src="rules.py"/>
...
<transition event="retry" cond="under_limit(tries, 3)" target="trying"/>
```

The compiler reads `rules.py` through the resolver you give
`compile_charts`. A resolver is any callable `resolver(base, source) ->
str`, for example
`lambda base, source: pathlib.Path("charts", source).read_text()`. It is
called at compile time only, for each static `src=` that is not among
the sources. It refuses a file by raising
`ResolveError(message, line=None, column=None)`, which the compiler
reports at that line and column.

**Values do not leak between actors.** What crosses to another actor
(`<send>` data, `<donedata>`, invoke params, data from the application)
is copied through the value table (below, "Values"), so actors share no
mutable value.

## I/O processors

An I/O processor is how an actor's output reaches your program. The
config declares the types (`"ioProcessors": [{"type": ...}]`), and
`Host(config, io_processors)` maps each type to `handler(host, send)`.
Every `<send>` of that type reaches the handler once, as a `Send`:
`send.event`, `send.data`, `send.target`, `send.sendid`, `send.session`
(the sender) and `send.traceparent`.

The handler does the I/O and then does one of two things:

- **`send.answer(event, data=None, *, traceparent=send.traceparent,
  origin=None, raw=None)`** gives the sending actor an event in reply,
  carrying the send's `sendid`. `origin` and `raw` become `_event.origin`
  (where to reply) and `_event.raw` (the message as the transport read
  it).
- **`send.fail()`** reports that the send could not be delivered. The
  sending actor gets `error.communication`.

A handler may also do neither, when the send needs no reply. The sending
actor waits while the handler runs, so a plain function's `answer()` or
`fail()` reaches it before its next step, as SCXML has a `<send>`
behave; the other actors go on meanwhile. So hand the message to your
transport and return: SCXML needs the send dispatched, not delivered,
and a handler that waits for delivery (a request's response, a
broker's confirmation) holds its sender that long. An `async def`
handler answers after the sending actor has gone on. A handler that
raises before answering has its send failed for it, and the driver
raises the exception after the turn.

Read the fields inside the handler: a plain function gets a thin view
(above, "How a host runs"). To answer later, write the handler as
`async def`. Its record answers at any time while the host lives.

## Errors

| You see | When | What to do |
|---|---|---|
| `CompileError` | `compile_charts` or `compile_system` found an error | read `error.diagnostics`, each with `file`, `line`, `column`, `severity` and `message`; nothing was compiled |
| `TinyactorsError` | the host refused a call, a thin view was read after its batch, a run faulted, or a typed method's reply was an error | branch on `error.status`, the core's name for the status (`"TA_ERROR_STALE"`, `"TA_ERROR_STATE"`, `"TA_ERROR_INVALID"`, ...) |
| `MemoryError` naming a pool | the host ran out of a pool its config sized | the host stays usable, and the next run goes on where this one stopped; size the config (below, "Sizing the config") |
| your handler's exception | a handler raised | the driver raises the first one after the turn |
| `error.execution`, `error.communication` | inside a chart | handled by the chart, as SCXML defines; see the core's documentation |

`CompileError` and `ResolveError` are subclasses of `TinyactorsError`.
For example, `host.send(12345, "hello")` to a session that does not exist
raises `TinyactorsError` with `status == "TA_ERROR_STALE"`.

## Invoked services

A chart can `<invoke>` a service that your program runs, such as a
connection, a subprocess or a long computation. Register a handler per
type: `Host(..., services={type: handler})`. When an invocation starts,
the handler receives an `Invocation` and returns the service, any
object. The host keeps the service until the invocation ends.

```xml
<state id="talking">
  <invoke id="counter" type="urn:example:counter" src="counter://local">
    <param name="seed" expr="seed"/>
  </invoke>
  <transition event="go"><send target="#_counter" event="ping"/></transition>
  <transition event="pong">...</transition>
  <transition event="done.invoke.counter" target="finished"/>
</state>
```

```python
class Counter:
    def __init__(self, invocation):
        self.invocation, self.count = invocation, 0

    def receive(self, host, send):            # a <send target="#_counter">, or autoforwarded
        self.count += 1
        if send.event == "finish":
            self.invocation.done({"total": self.count})   # done.invoke.counter
        else:
            self.invocation.send_back("pong", {"n": self.count})

    def stop(self, host):                     # the invoker cancelled it (left the state, ended)
        ...

host = tinyactors.Host(config, services={"urn:example:counter": lambda host, invocation: Counter(invocation)})
```

- **The invocation** has `session` (the invoker), `invokeid`, `type`,
  `src`, `data` (the params, or the content), `traceparent`, `running`
  and `restored` (below, "Snapshots"). `send_back(event, data=None)`
  gives the invoker an event with its `invokeid` set. `done(data=None)`
  ends the service with `done.invoke.<invokeid>`. The invocation is a
  record, so you can keep it and answer while the service runs. After
  `done` or a cancel, these calls raise `TinyactorsError` with
  `TA_ERROR_STALE`.
- **The service** may have `receive(host, send)`, which gets each send to
  it as a thin view (`type` is `None`), and `stop(host)`, which gets the
  cancel. Both are optional. A service that ended with `done` is neither
  stopped nor kept, so a handler may call `done` and return at once.
- **A failure** of any kind gives the invoker `error.communication`: no
  handler for the type, a handler that raises, a `receive` that raises
  (the service crashed and is dropped, not stopped), or no handler for a
  restored invocation. Each later send to that invocation gets one too,
  until the invoker leaves the state. The invoker gets none when it
  cancelled the invocation in the same run. The driver raises the
  exception after the turn; a restored invocation with no handler raises
  nothing, since declining to start it again was your choice.
- **Leaving the host** drops the running services without calling `stop`.

An invocation uses one of the core's requests; `system.maxRequests` is
where the pool starts, and a segment grows it when it runs out.

## Telemetry

With `"telemetry"` in its config, the host traces its actors and writes
OTLP requests (protobuf): spans of every macrostep, `<log>` lines and
failed actions as log records, and counters as metrics. Traces and logs
are pushed: the host hands each request to
`telemetry=handler(host, signal, data)` as `bytes`, with `signal`
`"traces"` or `"logs"`, and your handler posts them. Metrics are pulled:
call `host.metrics()` when your exporter wants them.

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

The handler runs inside the turn, so a slow post holds up the host (and,
under asyncio, the event loop). Give it a timeout, or write it as
`async def`, which runs as a task; `async with` waits for the tasks of
the last requests. Retrying and limiting what waits are up to you. When
a handler raises, its request is lost and the driver raises the first
exception of the turn after it.

`flushInterval` is in the host's clock (`system.ticksPerSecond`). Without
it, a request is ready only when the staging buffer is full, or when the
host closes.

**Application spans above actors.** A `traceparent` given to `send` or
`spawn` makes the macrostep it starts a child of your span, in your
trace. Sends between actors keep the trace, and an I/O processor receives
the sending step's context in `send.traceparent`. With the OpenTelemetry
SDK:

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

A tracing backend then shows one trace. `POST /orders` is at the top, with
the actors' macrosteps and microsteps below it. `warehouse.reserve` sits
below the step that sent to the warehouse, and the macrostep its answer
started sits below that.

**Choosing what is traced.** `trace_configure` sets the policy, for
example sampling. `trace_watch` adds rules in front of it for a while, on
the host's clock: with tracing sampled down, it can trace one chart in
full for a minute. `trace_spans_decode(handler, traces=data)` decodes a
traces request back into spans, which is useful in tests.

```python
host.trace_configure(print, sample=0.01)                 # 1% of new traces
host.trace_watch(lambda host, result: print("watch", result["id"]),
                 rules=[{"chart": "checkout", "sample": "sample", "detail": "full"}],
                 seconds=60)                             # every checkout, for a minute
```

## Hibernation

Most actors spend most of their time waiting. Hibernation lets a host
keep millions of actors while only some of them are in memory: the rest
are blobs in your store, and the host asks for one back when it has work.
The host does no I/O, so storing and loading the blobs is your
program's job.

- **`host.sweep(max_scan, limit) -> [session, ...]`** returns at most
  `limit` actors that have not run since the previous sweep and can be
  hibernated now (a CLOCK sweep over at most `max_scan` slots). Calling
  it regularly is what makes "not run since the previous sweep" mean
  cold.
- **`host.hibernate(session) -> bytes`** takes the actor out of memory
  and returns its blob for you to store. It raises `TinyactorsError` when
  the actor is not at rest (events waiting, in a macrostep, an invoked
  child), or when a `"python"` actor holds a value outside the value
  table (a `set`, a `tuple`, an object). The actor stays resident then. A
  pending delayed send stays in the host and wakes the actor when it
  falls due.
- **`wake=handler(host, session, migration)`** is called when a
  hibernated actor has work: an event or a due delayed send (`migration`
  is `None`), or a migration that asks for it (`migration` is its id;
  below, "Migration"). The events wait in the mailbox while your handler
  loads the blob and calls **`host.wake(session, blob)`**, then or later.
  The handler is called once per hibernation, however many events come.
  Without a `wake` handler the host does not ask: the actor stays
  hibernated, and its events wait, until you call `host.wake`.

```python
import sqlite3

db = sqlite3.connect("actors.db", isolation_level=None)
db.execute("CREATE TABLE IF NOT EXISTS blobs (session INTEGER PRIMARY KEY, blob BLOB)")

def wake(host, session, migration):
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
every actor. Instance and string memory are needed for the resident ones
only. Left to grow, a segment grows the pool that ran out, by at least a
quarter of its region, and its instance memory with `maxActors`,
hibernated actors included. For `R` actors resident out of `N`:

```python
config = {"systemID": "big", "system": {
    "maxActors": N + room, "maxEvents": 4096,
    "instanceBytes": 2048 * (R + room), "stringBytes": 16384 + 64 * (R + room)}}
```

`tools/stress.py` (`mise run stress`) is this pattern at scale: two
million actors, at most 1000 resident, the rest in SQLite (below,
"Performance").

## Migration

A new version of a chart (an image added with a chart of the same `name`)
runs the actors spawned from then on. `host.migrate` moves the running
actors to it, each at a stable point, during the next runs. A moved
actor keeps its session id, its state configuration (states are matched
by id) and its delayed sends. It first gets `error.platform.upgrade`,
whose data is the old version's `<datamodel>` bindings, and the new
version takes what it keeps from there:

```xml
<transition event="error.platform.upgrade">
  <assign location="n" expr="_event.data['n']"/>
</transition>
```

- **`host.migrate(*, chart=None, session=None, wake=False, timeout=0) ->
  migration`** starts a migration of every chart, of one `chart` by name,
  or of one `session`, and returns its id. `timeout` is in seconds; 0
  means none.
- **`migration_done=handler(host, result)`** gets the end of each
  migration, with `{"migration", "migrated", "failed", "pending":
  [session, ...]}`. It is a handler of the host, not of the call, so it
  also gets the end of a migration that a restored host goes on with.
- **`migrated=handler(host, migrated)`** gets each actor moved, as a thin
  `Migrated` view of `session`, `segment`, `chart` (its name) and
  `migration`.

**When a migration ends.** A migration ends when every actor it looks at
has moved, or at its `timeout`. A hibernated actor moves once it is
woken. With `wake=True` the migration asks for it through the `wake`
handler, with the migration's id, and asks again if it finds the actor
hibernated after a wake. Without `wake`, a hibernated actor that nobody
wakes holds the migration until its timeout, or forever without one, and
`migration_done` is never called: give such a migration a `timeout`.
Each run costs the migration one quantum of actors (`migrateQuantum` in
the config), however many it waits for.

**Actors that do not move.** An actor that cannot be written out stays
on its version and counts as failed. On the `"python"` datamodel, that is
one whose variables or pending delayed sends hold a value outside the
value table. An actor that is running an invocation waits until the
invocation ends.

**With most actors hibernated, wake them a batch at a time.** A
migration with `wake=True` asks for every hibernated actor early. Wake
them all and they are all resident at once. Instead, queue the
migration's requests and wake a batch per round; the migration takes
each woken actor in its next runs. An event's request (`migration` is
`None`) still wakes its actor at once, and the migration takes that one
too. Put actors back to sleep once they have moved. One put back before
it moved is asked for again, which costs work but does not stall the
migration.

```python
asked = collections.deque()

def wake(host, session, migration):
    if migration is not None:
        asked.append(session)             # the migration's: a batch at a time
    else:
        host.wake(session, store.get(session))   # an event's: now

host = tinyactors.Host(config, wake=wake, migration_done=on_done)
...
migration = host.migrate(wake=True)
while not done:
    for _ in range(min(256, len(asked))):
        session = asked.popleft()
        host.wake(session, store.get(session))
    host.run_until_idle()
    evict_moved(host)                     # the moved actors at rest go back to the store
```

`tools/migration.py` (`mise run migration`) is this pattern at scale, on
SQLite (below, "Performance").

## Snapshots

`host.snapshot() -> bytes` copies the whole host into one blob: every
segment's memory (actors, queues, timers, the hibernated actors'
records), both datamodels' sessions and payloads, the images, the system
id and the host's time. `tinyactors.Host.restore(snapshot, config, ...)`
creates a new host running it. Pass the config the snapshot was taken
with (its system id is checked; its I/O processors and tick rate are not
in the snapshot) and the handlers `Host(...)` takes. Every session id
stays the same, and the host's clock goes on from the snapshot's time,
whatever clock reads it: a new `SimulatedClock`, or the real clock in
another process or on another machine.

```python
blob = host.snapshot()                    # quiescent: not from a handler
host.close()
...
host = tinyactors.Host.restore(blob, config, log=log, wake=wake)
```

- **A snapshot is valid only for the same build of the library.** It holds
  raw memory, so it is not a backup; durability is your database's job,
  and hibernation is how a running process keeps actors out of memory.
  A snapshot is a copy of the whole application, every chart version at
  once. Use one to rehearse a deploy: add the new image, migrate, run the
  clock ahead, look. Use one to capture a host when something goes wrong
  and step through it on the same build, to start every test from one
  prepared world, or to take a host out of the process for a while.
- **Invoked services are started again.** The services you run are not
  in the snapshot. The restored host's first run announces each
  invocation that was running to the `services=` handler you pass to
  `Host.restore`, with `invocation.restored` true and the same
  `invokeid`, `src` and params. Pass your real handlers to go on, or
  stand-ins for a copy that must not reach the real services. A type with
  no handler gives its invokers `error.communication`.
- **Hibernated actors stay in your store.** The snapshot holds their
  records, not their blobs, and the restored host asks for them through
  `wake` as before. A copy that hibernates actors needs its own copy of
  the store, since it writes blobs too.
- **The host must be quiescent.** A snapshot is refused with
  `TA_ERROR_STATE` from a handler, while an effect or a typed method's
  reply has not been handled (run the host first), and while a migration
  runs. It is refused with `TA_ERROR_UNSUPPORTED` when a `"python"`
  session or payload holds a value outside the value table. The host
  goes on either way.
- **Values are written through the value table**, so two variables of
  one session that share a list no longer share it after a restore.

## Other systems

How systems talk to each other (threads and a queue, a WebSocket to a
browser, HTTP) is your program's choice. A chart addresses another system
with an ordinary SCXML target, `tinyactors://<systemID>/#_scxml_<n>` or
`tinyactors://<systemID>/@<name>`. The host delivers a target in its own
system itself. Every other target goes to the host's transport, a
handler you pass as `transport=`; it needs nothing in the config.

A send between systems is one message in two halves. The sending host
hands your transport the outgoing half as `send.message`, a dict of the
target, event, data, sendid, origin and traceparent. The receiving host
takes the same dict as its incoming half, `host.send(message)`. Your code
only moves the dict:

```python
links = {}                                   # system id -> a connection of yours

def transport(host, send):                   # the outgoing half
    link = links.get(send.target.system)
    if link is None:
        send.fail()                          # no connection: error.communication
    else:
        link.write(tinyactors.encode(send.message))

host = tinyactors.Host({"systemID": "a"}, transport=transport)

def on_frame(link, frame):                   # the incoming half, on the other side
    message = tinyactors.decode(frame)
    if message["origin"].system != link.system:   # trust is yours: who may claim what
        return
    try:
        host.send(message)                   # every field arrives, the sendid included
    except tinyactors.TinyactorsError:       # no such actor here: tell the sender
        link.write(tinyactors.encode({"failed": message["origin"], "sendid": message["sendid"]}))
```

A chart's reply to `_event.origin` goes back the same way. `send.target`
and `send.origin` are `Address` values: `.system`, `.session` and `.name`
give their parts, so neither you nor the binding parses an address.
`host.send(message)` ignores keys it does not know, so a newer peer can
add one, and takes a `sendid`, `origin` or `traceparent` of `UNDEFINED`
(what another binding's absent field decodes as) as absent, as `None`.

A failure is never part of a message. When the other side reports one,
and you decide to believe it, call `host.fail(address, sendid)`: the
sending actor gets `error.communication` with that sendid. A peer whose
reports you accept without checking can give your actors any error, so
take failures only from the connection the send went out on. A send
the transport accepted needs no answer, and inside the handler
`send.fail()` reports a failure directly.

The data is a Python value; encode the message for the wire as you like.
`tinyactors.encode` gives the value table's CBOR, which every binding
reads, `Address` values included. Without a transport, each send to
another system fails, and its sender gets `error.communication`.

## Values

The values that cross between your program, the actors and other
processes are those of the value table: `int` (from -2\*\*64 to 2\*\*64-1), `float`, `str`,
`bytes`, `list`, `dict` with `str` keys, `True`, `False`, `None`,
`tinyactors.UNDEFINED` (ECMAScript's undefined, distinct from `None`) and
`tinyactors.Address` (an actor's address kept apart from plain text).
Only exact built-in types are accepted: a subclass of `dict` or `list`,
a tuple or a set is refused.

Inside one process, values cross as Python objects, with no encoding.
Data you give a `"python"` chart is copied (lists and dicts are copied,
immutable values shared); data for an ECMAScript chart is converted for
quickjs. `send.data` from a `"python"` chart is the value itself.
`tinyactors.encode(value) -> bytes` and `tinyactors.decode(data)` give
the value table's CBOR form, for values that leave the process.
[docs/values.md](docs/values.md) has the details.

## Limits

- **Charts are trusted code, and the rewrite is not a sandbox.** It keeps
  a chart inside SCXML's scope rules and nothing more. A chart in Python
  runs in your interpreter, cannot be preempted in the middle of an
  expression, and nothing limits its memory. quickjs, by contrast, has an
  operation limit and its own allocator.
- **Python values lie outside the host's allocator.** The host, its
  tables and its scratch memory are allocated with `PyMem_RawMalloc`. The
  values of a Python chart's sessions and payloads are Python objects on
  Python's heap.
- **Nesting is limited to 64 levels.** That stops cycles, but not a value
  that shares subtrees (`a = [a, a]` sixty times): another reason charts
  are trusted code.
- **A `"python"` actor hibernates and migrates** only while its
  variables, and the data of its pending delayed sends, are values of the
  value table.

## Memory

Measured with `mise run measure`: 10,000 actors of one chart with three
`<data>`, resident and idle, every pool sized for them and tracing off;
then again after one event to each actor, which keeps its `_event` until
the next one (2026-10-15; x86_64 Linux, 4 CPUs, CPython 3.13.16 with the
GIL):

| datamodel | resident per actor | traced per actor | core's region per actor | resident after one event | region after one event |
|---|---|---|---|---|---|
| `"python"` | 387 B | 188 B | 268 B | 571 B | 268 B |
| quickjs | 1066 B | — | 268 B | 1500 B | 268 B |

"Traced" is what tracemalloc sees: the sessions' Python objects and the
binding's rows (including an 8 B array of the measuring harness). After
an event, a `"python"` session keeps one more object, its `_event`
(about 100 B). Its name, invoke id, origin and origin type are strings
that the chart or the host holds once for every session. With the pools
sized, the region does not grow after the events. Left at its default
`maxEvents` (256), quickjs's region grows its event pool as the actors'
last events fill it: an event and a trace context per actor.

### Sizing the config

Most of a host's memory is what its config reserves, not what its actors
use: the core sets its pools up front and does not allocate after that.
The first configs of this package's benchmarks reserved four to sixteen
times what they used. Sized for the work, the 100,000 resident actors of
the snapshot benchmark went from 362 MB of region to 47 MB, and the
services benchmark's 200,000 actors from 269 MB to 50 MB. The core's
[host-config.md](../tinyactors-core/docs/host-config.md) lists every key;
these matter most:

- **`maxActors`:** every actor, resident or hibernated (a hibernated one
  costs about 58 B of region), with some room.
- **`instanceBytes` and `stringBytes`:** for the resident actors only, at
  what their charts use (128 B and 16 B per actor for the benchmarks'
  charts). Left out, they are derived from `maxActors`, which counts the
  hibernated actors too.
- **`maxEvents`:** the events in flight at once. On quickjs, add one per
  resident actor, because quickjs keeps each actor's last event in the
  core's event pool.
- **`maxRequests`:** the service invocations that run at once.
- **`tracing`:** on by default. It takes a journal of 128 KiB per
  segment and a trace context (48 B) per `maxEvents` slot; turn it off
  where nobody exports traces.
- **Size the pools; do not let them grow.** A segment that runs out of
  a pool grows that pool by at least a quarter of its region, copying the
  whole region into a new block, and never shrinks.

## Performance

Each benchmark below is a program in `tools/`, run with `mise run
<name>`, that checks its own results. Every run here was one run, on
2026-10-15, on x86_64 Linux with 4 CPUs and CPython 3.13.16 with the GIL,
with each config sized for its work ("Sizing the config").

**Hibernation** (`stress`): two million actors of a counter chart, at
most 1000 resident, the rest in SQLite. 200,000 events go to actors drawn
at random, so nearly every event wakes one from the store and the sweep
sends another back. Every actor's count matched the events it was sent,
and after every eviction at most 1000 actors were resident.

| datamodel | populate (spawn and hibernate 2,000,000) | 200,000 events (woken, swept back) | peak resident | core's region | SQLite file |
|---|---|---|---|---|---|
| `"python"` | 15.8 s (126,000 a second) | 14.7 s (13,600 a second; 199,869 wakes) | 164 MB (82 B per actor) | 115 MB (58 B per actor) | 355 MB |
| quickjs | 25.9 s (77,000 a second) | 17.3 s (11,500 a second; 199,869 wakes) | 165 MB (82 B per actor) | 115 MB (58 B per actor) | 409 MB |

**Migration** (`migration`): 2,000,000 actors of a counter chart on
SQLite, at most 1000 resident, moved to version 2 while 200,000 events
go to actors drawn at random. It checks every sampled actor's count and
version, that the migration left no actor behind, that version 1's
segment retired, and the resident bound after every eviction.

| datamodel | populate (2,000,000, 200,000 events) | migrate, live (200,000 events meanwhile) | woken from SQLite | peak resident | core's region (both versions, then after) | SQLite file |
|---|---|---|---|---|---|---|
| `"python"` | 59.9 s | 129.7 s (15,400 actors a second) | 2,218,199 | 473 MB (237 B per actor) | 230.9 MB, then 115.5 MB (58 B per actor) | 556 MB |
| quickjs | 71.1 s | 153.3 s (13,000 actors a second) | 2,218,199 | 475 MB (238 B per actor) | 230.9 MB, then 115.5 MB | 756 MB |

A run costs the migration one quantum of actors however many it waits
for, so its time grows with the population. At 500,000 actors on the
`"python"` datamodel it took 13.6 s, against 74.9 s when the migration
went over everything it waited for in every run. The peak is the migration's: both
versions' regions until version 1 retires, plus the migration's map of
the sessions it waits for (at most a quarter full after it is rebuilt).

**Snapshots** (`snapshot`): a deploy rehearsed on a copy of a live host.
The live host has its population on SQLite as in the hibernation
benchmark, 200,000 events, a delayed send for every resident actor, and
half of those paying through an invoked payment service that has not
answered. Its snapshot is written to a file, its store copied, and a copy
restored from the file with a stand-in payment service, which gets every
payment in flight again (`restored`, with its amount) and declines it.
The copy must snapshot to the same bytes before it runs. Then it adds
version 2 and migrates every actor, waking the hibernated ones a batch at
a time, with 200,000 events meanwhile, while the live host goes on with
200,000 events of its own. Both hosts' sampled actors must hold every
count, delayed send and payment: the copy's on version 2 and declined,
the live host's on version 1 and still paying. The copy must never reach
the real service. Both hosts ran in one process.

| datamodel | actors | resident | snapshot | snapshot time | restore time | migrating the copy | peak resident, both hosts | every actor right |
|---|---|---|---|---|---|---|---|---|
| `"python"` | 2,000,000 | 1000 | 116.1 MB (58 B per actor) | 0.14 s | 0.12 s | 145 s | 652 MB | yes |
| quickjs | 2,000,000 | 1000 | 116.2 MB | 0.14 s | 0.13 s | 172 s | 654 MB | yes |
| `"python"` | 100,000 | all | 56.6 MB (566 B per actor) | 0.66 s | 0.53 s | 22.3 s | 427 MB | yes |
| quickjs | 100,000 | all | 64.0 MB (640 B per actor) | 3.43 s | 1.29 s | 21.6 s | 684 MB | yes |

A snapshot is mostly the region, as the config sized it: before the
benchmark's config was sized for the work, the same 100,000 resident
actors made a 372 MB snapshot and a 1761 MB peak. Migrating 2,000,000
mostly hibernated actors is the store's work: each is woken, moved and
hibernated again (2,018,332 wakes, events included).

**Invoked services** (`services`): 200,000 actors, each with a counter
service, make 1,000,000 round trips (actor, service, actor). Then a third
finish with done data, a third are cancelled, and the rest keep their
service. It checks every pong and every done datum, that every cancelled
service was stopped exactly once and no other was, and that the host
holds only the services still running.

| datamodel | start 200,000 invocations | 1,000,000 round trips | end (done, cancel) | peak resident | core's region |
|---|---|---|---|---|---|
| `"python"` | 3.1 s (64,000 a second) | 38.3 s (26,000 a second) | 4.5 s | 408 MB (2041 B per actor and service) | 50 MB (251 B per actor) |
| quickjs | 5.0 s (40,000 a second) | 62.6 s (16,000 a second) | 5.9 s | 711 MB (3553 B per actor and service) | 62 MB (309 B per actor) |

## The API at a glance

Every item has a docstring: `help(tinyactors.Host.migrate)` says when and
how to use it.

| Item | What it does |
|---|---|
| `compile_charts(sources, resolver=None) -> Compiled` | compiles `(name, text)` pairs into one image; raises `CompileError` |
| `compile_system(manifest, resolver=None) -> Compiled` | compiles a `<system>` manifest of charts and boot actors |
| `Compiled.image`, `.diagnostics`, `.close()` | the image as a read-only buffer, valid until `close()`; the warnings and notes; a context manager |
| `Host(config, io_processors=None, *, log, clock, telemetry, wake, migrated, services, migration_done, transport)` | a host, with its handlers (`transport`: every send to another system); a context manager for `with` and `async with` |
| `add_image(image) -> segment`, `chart(name) -> ChartRef` | runs an image; the current version of a chart |
| `spawn(ref, name=None, params=None, traceparent=None) -> session` | starts an actor |
| `send(target, event, data=None, *, sendid=None, traceparent=None, origin=None)`, `send(message)` | gives an actor an event; `target` is a session or a local address; a message is another system's `send.message` |
| `fail(target, sendid=None)` | reports a send that could not be delivered: `error.communication` for its sender |
| `send_many([(target, event, data, traceparent), ...]) -> accepted` | many sends in one call; returns how many were accepted |
| `terminate(session)`, `boot(segment)`, `actor_by_name(name)` | ends an actor; spawns an image's boot actors; finds a named actor (`None` when there is none) |
| `run_until_idle()`, `run()`, `await idle()`, `next_deadline` | the drivers (above, "Drivers and time") |
| `actor_state`, `actor_context`, `actor_list`, `actor_get`, `host_stats`, `host_config`, `trace_*` | typed methods (above, "Typed methods") |
| `metrics() -> bytes` | the host's counters as OTLP |
| `sweep`, `hibernate`, `wake` | hibernation |
| `migrate(...) -> migration` | migration |
| `snapshot() -> bytes`, `Host.restore(snapshot, config, ...)` | snapshots |
| `close()`, `closed` | frees the host |
| `SimulatedClock()`, `.increment(seconds)`, `.now` | a clock for tests |
| `encode(value) -> bytes`, `decode(data)`, `Address`, `UNDEFINED` | the value table |
| `TinyactorsError`, `CompileError`, `ResolveError`, `Diagnostic` | errors |

## Building from source

`mise run build` builds the extension for CPython 3.13 (abi3) and for
the free-threaded test interpreter. `mise run test` lints, runs `tests/`
on both, and runs the conformance suite's milestone set against the
adapter (`python -m conform`) on both datamodels.

`mise run wheel` builds the wheels, one per platform, with zig from any
machine, into `build/wheels/`, and smoke-tests this machine's wheel in
fresh environments for 3.13 and 3.14. `mise run release -- <version>`
checks, builds, verifies and smoke-tests a release, in Linux containers
too (Docker; `--skip-containers` leaves them out). `--publish` tags it,
uploads it to PyPI and creates the GitHub release, and `--dev --publish`
uploads `main` as a prerelease.

## License

MIT, as the repository ([LICENSE](../LICENSE)).

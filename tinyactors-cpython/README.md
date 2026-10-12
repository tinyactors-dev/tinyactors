# tinyactors for Python

SCXML statecharts as actors, from Python: the `tinyactors` extension
module drives the tinyactors host, and chart logic is written in Python
(the `"python"` datamodel) or in ECMAScript (quickjs), each chart naming
its own.

**Status: milestone M1, a walking skeleton.** Charts, actors, requests,
values, peers and a simulated clock work; invocations, I/O processors,
hibernation, migration, snapshots and telemetry come in later milestones,
and there is no wheel yet ([ROADMAP.md](ROADMAP.md)). What each item of
the API calls in core: [docs/api-map.md](docs/api-map.md).

```python
import tinyactors

chart = """<scxml xmlns="http://www.w3.org/2005/07/scxml" datamodel="python" name="counter" initial="idle">
  <datamodel><data id="count" expr="0"/></datamodel>
  <state id="idle">
    <transition event="inc"><assign location="count" expr="count + _event.data['by']"/></transition>
    <transition event="stop" target="done"/>
  </state>
  <final id="done"><donedata><param name="total" expr="count"/></donedata></final>
</scxml>"""

clock = tinyactors.SimulatedClock()
with tinyactors.Host({"systemID": "app", "system": {"ticksPerSecond": 1000}}, clock=clock,
                     on_done=lambda session, data: print(session, data)) as host:
    host.add_image({"counter.scxml": chart})
    session = host.spawn("counter")
    host.send(session, "inc", {"by": 2})
    print(host.request("actor.context", {"session": session}))
    host.send(session, "stop")            # prints: 1 {'total': 2}
```

- **`Host(config, *, clock=None, on_log=..., on_done=..., on_error=..., on_peer=...)`**:
  the config is [core's](../tinyactors-core/docs/host-config.md), as
  Python values; a refused one raises core's error unchanged. Each effect
  goes to its handler; a handler's exception is raised after the turn.
- **Inputs run the turn.** `spawn`, `send`, `send_many`, `terminate`,
  `boot`, `add_peer` and `deliver` give core the input and run the host
  until it is idle at the current time. `run()` drives the real clock
  until no deadline is left; with a `SimulatedClock`, time moves only with
  `increment(ms)`.
- **`request(method, params)`** is any method of
  [requests.md](../tinyactors-core/docs/requests.md): its `result`, or a
  `TinyactorsError` with core's `code`, `reason` and `path`
  ([docs/errors.md](docs/errors.md)).
- **Values** cross as Python objects by one table
  ([docs/values.md](docs/values.md)), also exposed as
  `tinyactors.encode(value) -> bytes` and `tinyactors.decode(data) ->
  object`: integers to 64 bits, `bytes`, `tinyactors.Address` (an actor
  reference) and `tinyactors.UNDEFINED` beside `None`.
- **The `"python"` datamodel** ([docs/datamodel.md](docs/datamodel.md)):
  each session's variables live in one globals dict; `_event` is read by
  attribute (`_event.data['by']`); `In('state')` is a function;
  `<content>` is JSON if it parses.
- **One thread.** A host is called only from the thread that made it.

## Limits

- **Chart code is trusted.** CPython cannot preempt an expression in the
  middle or limit its memory: a chart's Python runs in the application's
  interpreter, with its builtins, and a loop that never ends stops the
  host with it. (quickjs charts are bounded: its own allocator and
  operation count.)
- **Python values lie outside the host's allocator.** The host's memory
  comes from `PyMem_RawMalloc`, sized by its config; what a chart keeps
  in its variables is ordinary Python memory, which no host limit bounds.
- **Values nest at most 64 deep**, which stops cycles from crossing
  between actors, but not a value that shares subtrees (`a = [a, a]`
  sixty times is 2^60 items when copied): charts are trusted code.
- **What crosses between actors is copied** through the value table; an
  object outside it (a `set`, a `datetime`, a class instance) fails the
  `<send>` with `error.execution`.

## License

MIT, as the repository ([LICENSE](../LICENSE)).

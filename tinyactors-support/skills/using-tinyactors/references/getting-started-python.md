# Getting started: Python

The `tinyactors` package runs on CPython 3.13 and later (the stable ABI),
on macOS and Linux (x86-64 and arm64; glibc or musl). This page is
written against **0.4.0**, the release on PyPI. Charts may be written in
Python (`datamodel="python"`) or in JavaScript (`datamodel="ecmascript"`,
run by quickjs, the sandbox).

## Install

```sh
pip install tinyactors          # or: uv add tinyactors
```

There is no source distribution: other platforms and the free-threaded
interpreters cannot install it yet.

## A project

Keep one chart per kind of actor in a `charts/` directory and compile it
once with `compile_dir`. Logic longer than a comparison goes in a `.py`
file the chart names with `<script src>`: plain Python, checked at compile
time, free of XML's escaping. Its top-level `def`s become the chart's
methods.

```
my-app/
  charts/
    ticket.scxml      <scxml name="ticket">
    rules.py
  main.py
```

```py charts/rules.py
def summary(replies, customer):
    return f"{replies} replies to {customer}"
```

```xml charts/ticket.scxml
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="python" name="ticket" initial="open">
  <script src="rules.py"/>
  <datamodel>
    <data id="customer"/>
    <data id="replies" expr="0"/>
  </datamodel>
  <state id="open">
    <transition event="reply">
      <assign location="replies" expr="replies + 1"/>
      <send type="mail" event="notify" targetexpr="customer">
        <param name="text" expr="_event.data['text']"/>
      </send>
    </transition>
    <transition event="status">
      <send targetexpr="_event.origin" event="current">
        <content expr="{'summary': summary(replies, customer)}"/>
      </send>
    </transition>
    <transition event="resolve" target="resolved"/>
  </state>
  <final id="resolved">
    <donedata><param name="replies" expr="replies"/></donedata>
  </final>
</scxml>
```

The synchronous driver: your program turns the host with
`run_until_idle()`.

```python main.py
import tinyactors

class Mail:                                   # an I/O processor: an object with receive
    def receive(self, send):
        print(f"mail to {send.target}: {send.data['text']}")
        send.answer("mailed")                 # an event back to the ticket

with tinyactors.compile_dir("charts") as compiled:
    host = tinyactors.Host(compiled, system="support", io_processors={"mail": Mail()},
                           on_done=lambda actor, donedata: print(actor.id, "ended with", donedata))
with host:
    ticket = host.spawn("ticket", name="ticket.4711", params={"customer": "ada@example.com"})
    ticket.send("reply", {"text": "We are on it."})
    host.run_until_idle()
    ticket.send("resolve")
    host.run_until_idle()
```

```output
mail to ada@example.com: We are on it.
1 ended with {'replies': 1}
```

Under asyncio the host turns on the event loop by itself, and `ask` and
the typed methods are awaitable. An `async def receive` runs as a task
and may answer after its I/O.

```python ask.py
import asyncio
import tinyactors

class Mail:
    async def receive(self, send):
        await asyncio.sleep(0.01)             # your I/O
        send.answer("mailed")

async def main():
    with tinyactors.compile_dir("charts") as compiled:
        image = compiled.image
    async with tinyactors.Host(image, system="support", io_processors={"mail": Mail()}) as host:
        ticket = host.spawn("ticket", name="ticket.4711", params={"customer": "ada@example.com"})
        ticket.send("reply", {"text": "We are on it."})
        print(await ticket.ask("status"))     # (event, data) of the chart's answer to _event.origin
        ticket.send("resolve")
        print(await ticket.done)

asyncio.run(main())
```

```output
('current', {'summary': '1 replies to ada@example.com'})
{'replies': 1}
```

What it shows:

- **`compile_dir(path)`** compiles every `*.scxml` under the directory;
  `src` files are read from under it. An error raises `CompileError` with
  `error.diagnostics`.
- **`Host(charts, *, system, io_processors, ...)`**: `system` is the
  system id; each I/O processor is an object with `receive(send)` (a
  plain function is refused).
- **`host.spawn(chart, name=None, params=None)`** returns an `Actor`;
  `params` set the chart's top-level `<data>`.
- **`await actor.ask(event, data)`** (asyncio only) resolves with the
  chart's answer as `(event, data)`; it has no timeout of its own, so
  wrap it in `asyncio.wait_for`.
- **`on_done(actor, donedata)`** and `await actor.done` report an actor
  that reached a top-level `<final>`; `on_error(actor, error)` one that
  faulted; `on_handler_error(error, effect)` an exception one of your
  handlers raised.
- **Nothing runs at a call.** `spawn` and `send` record the input; the
  driver (`run_until_idle()`, or the event loop under `async with`) runs
  the actors and calls your handlers.

## The "python" datamodel

A chart's expressions are compiled by a restrictive rewrite of Python:
literals, arithmetic, comparisons, comprehensions, f-strings, calls to the
chart's methods and to allowed builtins (`len`, `min`, `max`, `sorted`,
`range`, `str`, `int`, `dict`, `list`, ...). It refuses `import`,
`lambda`, `try`, `with`, `class`, the walrus and `global`. An undeclared
name or a refused construct is `error.execution` in the chart when it is
evaluated, and a compile error at its line in a `.py` file. It keeps a
chart inside SCXML's rules; it is not a sandbox: charts in Python are
trusted code.

Values crossing between your program and a chart are copied through the
value table: `int` (to 64 bits), `float`, `str`, `bytes`, `list`, `dict`
with `str` keys, `True`, `False`, `None`, `tinyactors.UNDEFINED` and
`Address`. A `set`, a `tuple` or an object in a variable keeps the actor
from hibernating.

## Time

Time in the Python API is seconds (an `int` or a `float`). A test gives
the host a `tinyactors.SimulatedClock()`; `clock.increment(seconds)`
moves it and turns every host made with it until idle
([testing.md](testing.md)).

## Where next

- [designing-actors.md](designing-actors.md) before the second chart.
- [choices.md](choices.md) for persistence, services and other systems.
- The package's README, from "How a host runs" on:
  [tinyactors-cpython/README.md](https://github.com/tinyactors-dev/tinyactors/blob/main/tinyactors-cpython/README.md),
  and its guide to a send to an actor that is not there
  (`docs/guide/missing-actors.md`).

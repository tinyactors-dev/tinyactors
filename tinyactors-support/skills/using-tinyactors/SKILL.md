---
name: using-tinyactors
description: How to build an application with tinyactors (statecharts as actors, written in SCXML, run by @tinyactors/node, @tinyactors/wasm or the tinyactors Python package). Use it whenever you add tinyactors to a project, write or change an SCXML chart, design which actors an application has and how they talk, choose between hibernation, snapshots and a database, wire I/O processors, services or other systems, test charts on a simulated clock, or meet tinyactors' words (host, system, segment, image, session, effect, I/O processor, transport, hibernation, migration, snapshot, Spawner, collection actor) - even when the user only says "statechart", "actor", "SCXML" or names one of the packages.
---

# Using tinyactors

tinyactors runs statecharts as actors. A **chart** is an SCXML document: the
states a thing can be in, the events that move it between them, and what
happens on the way. Each running chart is an **actor** with its own
variables and a queue of events, and it talks to the world only by sending
and receiving events. A **host** runs many actors (a handful to millions) in
one block of memory and does no I/O of its own: when an actor sends
something out, your code gets it, does the I/O, and answers with an event.
The core is a C library; the packages that bind it are
`@tinyactors/node` (Node.js 22.3+ and Bun), `@tinyactors/wasm` (browsers)
and `tinyactors` on PyPI (CPython 3.13+).

## When to reach for it

Use it when a program keeps many long-lived things that each have a
lifecycle (orders, payments, subscriptions, sessions, support tickets,
jobs), when timeouts, retries and cancellations should be states rather
than scattered timers and flags, or when the same logic must run in tests
without the network and without waiting.

Do not reach for it for a single request handler with no state between
requests, for data you query across many records (that is a database's
job), or for a pipeline that only transforms data. A chart is worth its
cost when the states and their transitions are the hard part.

## Sibling projects

Two projects of the same family, each useful on its own:

- **[staticjs](https://github.com/tinyactors-dev/staticjs)**: a pure,
  total subset of JavaScript, as a freestanding C library and as
  WebAssembly for Node.js and browsers (`npm install @tinyactors/staticjs`).
  Reach for it to run programs someone other than your host wrote (a
  user's plugin, a filter, a computed field, or a program an agent writes
  to call your tools in code) that must give the same answer every time,
  end, and touch nothing but what they are given. Every evaluation runs
  on fuel you give it. It is not a datamodel of tinyactors' charts today;
  using it as one is only a proposal in the monorepo
  (`docs/proposals/staticjs.md`).
- **[sxml](https://github.com/tinyactors-dev/sxml)**: an XML toolchain in
  portable C: XML and HTML parsing, XSD 1.1 and DTD validation, XPath
  4.0, XQuery 4.0 and XSLT 4.0, from C, JavaScript
  (`npm install @tinyactors/sxml`), Swift and Kotlin on Linux, or the
  `sxml` command (`mise use -g github:tinyactors-dev/sxml`). Reach for it
  to query, check or transform XML documents: your charts (they are XML),
  or the XML data your application and its charts handle. tinyactors does
  not depend on it: its compiler reads SCXML with its own reader.

## The workflow

1. **Model the entities.** Decide which things are actors (those with a
   lifecycle), who creates each kind, how they are named, and how they
   end. Read [designing-actors.md](references/designing-actors.md) first:
   most mistakes are made here, not in the XML.
2. **Write the charts.** One `.scxml` file per kind of actor, in a
   `charts/` directory, compiled with `compileDir` (`compile_dir` in
   Python). Keep charts pure: every side effect is a `<send>` to an I/O
   processor or an `<invoke>` of a service your program runs.
3. **Make the host and its processors.** `new Host(charts, { system,
   ioProcessors, ... })`: one I/O processor object per external system
   (a database, an HTTP client, a queue), each with `receive(send)`.
   [getting-started-node.md](references/getting-started-node.md),
   [getting-started-python.md](references/getting-started-python.md).
4. **Test on a simulated clock.** A `SimulatedClock`, a stand-in for every
   I/O processor, and `await host.idle()` (`run_until_idle()` in Python).
   [testing.md](references/testing.md).
5. **Decide what survives a restart.** The host keeps nothing on disk by
   itself. Choose per kind of actor between a store with hibernation,
   snapshots of the whole host, and your own database as the truth.
   [choices.md](references/choices.md).

## Which reference to read

| You are about to | Read |
|---|---|
| learn what a word means (host, segment, effect, transport, ...) | [concepts.md](references/concepts.md) |
| add tinyactors to a Node.js, Bun or browser project | [getting-started-node.md](references/getting-started-node.md) |
| add tinyactors to a Python project | [getting-started-python.md](references/getting-started-python.md) |
| decide which actors exist, who owns them, what they are called | [designing-actors.md](references/designing-actors.md) |
| pick a datamodel, a persistence strategy, a processor or a service | [choices.md](references/choices.md) |
| write tests for charts | [testing.md](references/testing.md) |
| debug a chart that does nothing, or an event that goes nowhere | [pitfalls.md](references/pitfalls.md) |

## Rules that save the most time

- **Write `datamodel="ecmascript"` on every chart** (or `"python"` with the
  Python package). Without it a chart gets SCXML's null datamodel, which
  evaluates nothing and says nothing.
- **Event names match by prefix.** `event="join"` also takes `join.refused`.
  Name an answer so that it does not extend the request
  (`refused.join`, `listed` for `list`).
- **Every wait has a timeout**: `<send event="timeout" delay="30s"
  id="t"/>` on entry, `<cancel sendid="t"/>` when the answer comes. An
  actor that waits on an answer that never comes waits for ever.
- **Answer to `_event.origin`.** A chart that replies sends
  `<send targetexpr="_event.origin" event="...">`, so the same chart
  answers another actor, `actor.ask` and another system alike.
- **Handlers get views.** What an I/O processor's `receive(send)` gets is
  valid while it runs. Copy out what you keep, or write `receive` as
  `async` to answer later.
- **Treat compile warnings as errors** in tests: a JavaScript syntax
  error or a top-level `let` in a `<script>` is only a warning until it
  runs. `compileCharts(sources, { warningsAsErrors: true })` refuses them;
  after `compileDir`, assert that `compiled.diagnostics` is empty.
- **Run `tinyactors lint charts/*.scxml`**: it finds transitions that can
  never be taken, such as one shadowed by a prefix match. The
  `tinyactors` command comes with each tinyactors-core release
  (`mise use github:tinyactors-dev/tinyactors-core`).

## When tinyactors gets in your way

The tinyactors MCP server at `https://mcp.tinyactors.dev/mcp` is its
support channel. Call `known_issues` before working around a problem: it
may already have a workaround. If it is not there, `report_issue` with
the minutes it cost you (the `reporting-tinyactors-issues` skill says
how). The maintainers fix the costliest issues first.

## Where the documentation is

The packages' guides are the reference; this skill routes to them.

- Node.js and Bun: the README and `docs/guide/` of
  [`@tinyactors/node`](https://github.com/tinyactors-dev/tinyactors/tree/main/tinyactors-napi),
  also in `node_modules/@tinyactors/node/docs/guide/` after an install.
- Python: the README of
  [`tinyactors`](https://github.com/tinyactors-dev/tinyactors/tree/main/tinyactors-cpython).
- The words: [GLOSSARY.md](https://github.com/tinyactors-dev/tinyactors/blob/main/GLOSSARY.md).
- Example applications:
  [`tinyactors-napi/examples/`](https://github.com/tinyactors-dev/tinyactors/tree/main/tinyactors-napi/examples)
  (chat rooms, order fulfillment, subscription billing, an HTTP gateway, a
  job scheduler, ...).
- On mcp.tinyactors.dev: `search_docs` and `get_doc` search and read the
  same guides.

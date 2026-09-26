# tinyactors-napi

`tinyactors-napi` is the Node-API binding for Tinyactors. It targets Node.js
22 or newer and Bun.

The binding is currently a bootstrap stub. It exports only `version`, which is
the package version:

```js
const tinyactors = require('tinyactors-napi');

console.log(tinyactors.version);
```

Build and test it through the repository task interface:

```text
mise run build
mise run test
```

The test loads the same native addon with Node.js and Bun. See
[`docs/design.md`](docs/design.md) for the planned binding architecture.

## `index.d.ts` is the desired API

`index.d.ts` is the design target, not a description of what `lib/`
implements. A declaration that is not implemented yet carries
`// status: unimplemented`, or `// status: partial: <what is missing>`, on
the line directly above it. Don't delete a declaration because `lib/` lacks
it: implement it, or leave it marked. `grep -n "^ *// status:" index.d.ts`
lists what is outstanding, and `mise run check-api-status` (part of
`mise run test`) fails when a marker is stale or a missing declaration is
unmarked, for the exports and for the objects it can probe at runtime.

## Benchmarks

`mise run bench [population ...]` runs the tracing bench on Node and Bun and
prints throughput and memory side by side; `mise run bench-compare <commit>`
builds another commit beside the working tree and runs both alternately on
this machine, flagging changes past 5%. Results are stored under
`bench/results/<machine signature>/` (see its README).

**Reporting convention.** A benchmark report always gives throughput and
memory together, names the machine signature it ran under, and compares
builds side by side with `bench-compare`, never against numbers from another
day or another machine: the same build measured 30% apart on two days on
one machine.

## Loading actors on demand

`System.setLoader(family, loader)` brings actors of a family back when mail
arrives for them. Mail for a name with no resident actor waits while the
loader runs, then is delivered first, in order. The loader returns a
`SessionImage` (process-local: for actors offloaded from memory in this
process), `{ spawn: definition, binding }` to rebuild an actor from data
the application stored (after a restart, for example), `"not-found"`, or
`"reject"` for "not now". An unloaded actor's own timers wake it through
its loader. See [ADR 0020](docs/adr/0020-mail-for-an-unloaded-actor-waits-while-a-loader-brings-it-back.md).

## Tracing

`System.setTraceSink()` hands a function you register the spans of each
pump turn as OTLP protobuf bytes, which any OTLP/HTTP collector takes as
they are; `decodeTraces()` reads them back into objects. There is no
telemetry dependency. See
[`docs/tracing.md`](docs/tracing.md), and `mise run otel-demo` to look at
a trace in otel-tui.

SCXML documents import with `define(xml, { format: 'scxml' })`, and
`registerDatamodel()` plugs in a compiler for their expression text. See
[`docs/scxml.md`](docs/scxml.md).

## Debugging an actor

`createDebugSession(system, { mode, allow, audit, send })` is a Debug
Adapter Protocol session over one actor of a running system: give it each
incoming message with `receive()`, and it hands each outgoing one to
`send`. It opens no socket and checks no credentials; put it behind your
own transport and authentication, and decide each user's mode and
permissions before you create it. `DAPReader` and `encodeDAPMessage` frame
messages on a byte stream (stdio, a TCP socket); a WebSocket needs neither.

A client attaches with `{ "address": "family/name" }` (or `"sessionID"`)
and a mode no higher than the session's:

- `watch` never stops or changes the actor. Breakpoints report where they
  would have stopped; each macrostep shows in the console.
- `fork` copies the actor into a debug system of its own, with a manual
  clock (`clock +3d`), a copy of its data and I/O processors that only
  record. It steps one action at a time and steps back to earlier stops.
  The console says how the copy differs from the live actor.
- `live` works on the actor itself, as `allow` permits: `send`, `set`,
  `goto` (with `abandon`, `cancel` and `replace`), and `hold` (pause,
  stepping and stopping breakpoints). A live actor stops only between
  macrosteps, and a hold ends by itself after `holdTimeout` (30 s).

**`evaluate` runs arbitrary code.** Watch, hover and console expressions
and breakpoint conditions run JavaScript in your process, a fork's
included. They need the `evaluate` permission, which is off unless
granted; without it the Variables pane and the console commands still
work. Grant it only to someone who may run code on the host.

`audit(entry)` hears every attach, detach, breakpoint change, console
command, evaluation and mutation before it happens; a hook that throws
refuses it.

`mise run dap-server` serves sessions for a small example system over
stdio, or on a local port with `-- --port 4711`, to try a DAP client
against. It grants every permission; it is a development tool, not part
of the package. In VS Code, install
[tinyactors-vscode](../tinyactors-vscode/README.md): an `attach`
configuration with the `port` and the actor's `address` connects to such a
server. See
[ADR 0027](docs/adr/0027-a-debugger-holds-steps-and-redirects-one-actor.md)
and [the status of every DAP message](../docs/debug-adapter.md).

`test/dap-transcripts.js` plays the DAP transcripts napi shares with the
test host's adapter (`../tinyactors-test/dap/transcripts/`), under Node
and Bun, so the two adapters stay in step. To add one, write the
transcript, and the chart in both forms (`dap/charts/<chart>.js` and
`.rb`) if it needs a new one, then play it in both hosts: `node
test/dap-transcripts.js <name>` here (`DAP_TRACE=1` prints every message)
and `ruby ruby/dap_transcripts.rb <name>` in tinyactors-test. The format
and where the adapters differ are in
[its README](../tinyactors-test/dap/README.md).

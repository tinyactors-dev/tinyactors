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

`createDebugSession(system, { allow, audit, send })` is a Debug Adapter
Protocol session over one actor of a running system: give it each incoming
message with `receive()`, and it hands each outgoing one to `send`. It
opens no socket and checks no credentials; put it behind your own
transport and authentication, and decide each user's permissions before
you create it. `DAPReader` and `encodeDAPMessage` frame messages on a byte
stream (stdio, a TCP socket); a WebSocket needs neither.

A client attaches with `{ "address": "family/name" }` (or `"sessionID"`),
and may ask for fewer permissions with `"allow"`. The session watches the
actor live and never stops it: a production actor is never held by a
debugger. The client hears the actor stopped on entry, so it shows the
stack and the variables, and after each macrostep the summary in the
console and a refresh of both that leaves the keyboard focus where it is,
so typing in the Debug Console is never interrupted. `pause` and the
steps are refused, and `continue` changes nothing. Breakpoints are
tracepoints: the console says where one would have stopped, and a logpoint
writes its message.

What `allow` grants are pokes, each a transition the watcher sees as a new
macrostep: `send` an event through the mailbox, `set` a data-model
location, `goto` (an operator transition, with `abandon`, `cancel` and
`replace`), and `evaluate`. With none, the session only watches.

**`evaluate` runs arbitrary code.** Watch, hover and console expressions
and breakpoint conditions run JavaScript in your process. They need the `evaluate` permission, which is off unless
granted; without it the Variables pane and the console commands still
work. Grant it only to someone who may run code on the host.

Frames and line breakpoints are on the lines a chart was written on:
the builder calls of statechart(), or the lines of an SCXML document you
name with `define(text, { format: 'scxml', name: path })`. A document
defined without a name, or a child chart inline in `<content>`, is shown
as SCXML the session writes from the definition, which the client asks
for by `sourceReference` and sets breakpoints in.

`audit(entry)` hears every attach, detach, breakpoint change, console
command, evaluation and mutation before it happens; a hook that throws
refuses it.

`mise run dap-server` serves sessions for a small example system over
stdio, or on a local port with `-- --port 4711`, to try a DAP client
against. It grants every permission; it is a development tool, not part
of the package. See
[ADR 0026](docs/adr/0026-a-debugger-watches-and-pokes-one-live-actor.md).

`test/dap-transcripts.js` plays the DAP transcripts napi shares with the
test host's adapter (`../tinyactors-test/dap/transcripts/`), under Node
and Bun, so the two adapters stay in step. To add one, write the
transcript, and the chart in both forms (`dap/charts/<chart>.js` and
`.rb`) if it needs a new one, then play it in both hosts: `node
test/dap-transcripts.js <name>` here (`DAP_TRACE=1` prints every message)
and `ruby ruby/dap_transcripts.rb <name>` in tinyactors-test. The format
and where the adapters differ are in
[its README](../tinyactors-test/dap/README.md).

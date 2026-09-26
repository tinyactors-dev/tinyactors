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

# Developing @tinyactors/node

This is for working on the package in the tinyactors repository. Using the
package is in [README.md](README.md). The repository's rules for every
project (tasks, tests, commits, acronyms) are in the root
[AGENTS.md](../AGENTS.md).

## Building and testing

The package is built and tested through the task interface; the addon it
loads in the repository is the one `mise run build` writes to
`build/tinyactors_napi.node`:

```text
mise run build
mise run test
```

The tests load the same native addon with Node.js and Bun. While iterating,
run one suite at a time (`node test/<suite>.js`, then `bun`); `mise run
test` is the fast pass for a commit. See [`docs/design.md`](docs/design.md)
for the binding architecture, and the ADRs in [`docs/adr/`](docs/adr/README.md)
for the decisions behind it.

## Packages and releases

[Root ADR 0001](../docs/adr/0001-packaging-and-releases.md) says how the
packages are built and released:

- `mise run build --target <t>` builds a release target's addon with zig
  (`x86_64-linux`, `aarch64-linux`, `aarch64-macos`, `x86_64-macos`);
- `mise run symbols` checks what each built addon imports;
- `mise run package` stages and packs `@tinyactors/node` and its four
  platform packages into `build/npm/tarballs/`;
- `mise run smoke-install` and `mise run smoke-packages` install the packed
  tarballs and run a smoke test, on this host and in glibc, musl and arm64
  containers.

To try the current commit in another project without publishing, pack it
under a development version and install the main tarball with your
platform's:

```text
mise run package -- --dev
npm install <repo>/tinyactors-napi/build/npm/tarballs/tinyactors-node-<version>.tgz \
            <repo>/tinyactors-napi/build/npm/tarballs/tinyactors-node-linux-x64-<version>.tgz
```

The maintainer releases with `mise run release -- <version> --publish`,
and publishes a development build of `main` with `mise run release -- --dev
--publish` (npm only, under `next`: no commit, tag or GitHub release). Both
run the suite, build every target, check their symbols and smoke test the
packages first; without `--publish` they are dry runs.

A development version is `<next minor>-dev.<date>.<n>.g<commit>`, for
example `0.2.0-dev.20260928.4.g1a2b3c4`: the commit's date, the number of
commits since the last release, and the commit. Later commits sort later,
and semver ranges such as `^0.1.0` never match a development build.

## `index.d.ts` is the desired API

`index.d.ts` is the design target, not a description of what `lib/`
implements. A declaration that is not implemented yet carries
`// status: unimplemented`, or `// status: partial: <what is missing>`, on
the line directly above it. Don't delete a declaration because `lib/` lacks
it: implement it, or leave it marked. `grep -n "// status:" index.d.ts`
lists what is outstanding, and `mise run check-api-status` (part of
`mise run test`) fails when a marker is stale or a missing declaration is
unmarked, for the exports and for the objects it can probe at runtime.

`index.d.ts` needs only Node's types, which it brings in itself with
`/// <reference types="node" />`; `package.json` names `@types/node` as an
optional peer dependency, so TypeScript users install it and JavaScript
users need not. It must not need the DOM: `mise run check-types` (also
part of `mise run test`) compiles it strictly against ES2022 and Node's
types alone, with `test/types/usage.ts`, a consumer file that is compiled
and never run.

## Benchmarks

`mise run bench [population ...]` runs the tracing bench
(`test/bench/tracing.js`) and the input-log bench (`test/bench/input_log.js`,
a logged input against an unlogged one, napi ADR 0034) on Node and Bun and
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

## Tracing and SCXML

`System.setTraceSink()` hands a function you register the spans of each
pump turn as OTLP protobuf bytes, which any OTLP/HTTP collector takes as
they are; `decodeTraces()` reads them back into objects. There is no
telemetry dependency. See [`docs/tracing.md`](docs/tracing.md), and
`mise run otel-demo` to look at a trace in otel-tui.

SCXML documents import with `define(xml, { format: 'scxml' })`, and
`registerDatamodel()` plugs in a compiler for their expression text. See
[`docs/scxml.md`](docs/scxml.md).

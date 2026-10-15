# AGENTS.md: staticjs-js

`@tinyactors/staticjs`: staticjs for JavaScript, in Node and browsers, as
a WebAssembly module of its own (staticjs's amalgamation and `src/js.c`)
and `index.mjs` above it. The decisions are
[ADR 0013](../docs/adr/0013-staticjs-javascript-package.md); the design
and its review are the proposal it adopted
([`docs/proposals/staticjs-js-api.md`](../docs/proposals/staticjs-js-api.md)).
The repository root's `AGENTS.md` applies too.

## Whom it serves

First the application developer writing JavaScript who evaluates programs
written elsewhere (a user's plugin, an agent's code), in a browser, an
edge runtime or Node. Then the maintainers. Every API item has a staticjs
call behind it (`docs/api-map.md`), except the conversion budget, which
ADR 0013 makes the package's.

## Layers

- **`src/js.c` is the C layer:** it calls staticjs's public API and
  nothing else, and its exports take and return numbers. JavaScript reads
  no struct of staticjs's: values cross as handles on one stack of slots,
  released to a mark at the end of each call; a value kept past a call is
  a row with a generation. The few records JavaScript reads (an error, a
  frame, a function, a time) are this file's own arrays of words, in the
  order written beside each.
- **`index.mjs` is the API:** classes over the C layer, the conversions
  both ways (iterative, with an explicit stack, counted against
  `maxItems`), natives (the import `staticjs_js.native`, released through
  `staticjs_js.release` when staticjs frees the function), and errors.
- **What staticjs lacks is a change to staticjs** (ADR 0013, decision 1:
  library first), with its tests and documentation there, never a
  workaround here.

## Rules

- **At most three parameters** for every C function (`mise run test`
  checks `src/js.c`); group the rest in the scratch or a record.
- **No defaults of the package's own** beyond what ADR 0013 decides: no
  clock, no seed, no fuel.
- **Refuse, never convert into something else:** a value staticjs has no
  kind for is a `TypeError`.
- **One evaluator per context, C's rule:** a `start` abandons a paused
  run; the abandoned run's next call throws.
- **The running flag:** while a step, a conversion or a native runs, every
  call on the module's contexts throws `TypeError` ("the context is
  running"), reading a `Value` included.
- **Intrinsics captured at load:** a conversion never calls a method
  looked up on a value (a patched `Map.prototype` runs nothing).

## Tasks

- `mise run build`: `staticjs.wasm` from staticjs's checked-in
  amalgamation and `src/js.c`, with `Toolchain::Target.release`; it checks
  the module imports exactly `staticjs_js.native` and `staticjs_js.release`.
- `mise run test`: the parameter check, the build, `node --test` over
  `test/` (the Chromium test runs where Playwright is installed), and the
  pack.
- `mise run pack`: the tarball into `build/pack/`, installed into an empty
  project and imported by name. Publishing (`npm publish`) is the
  maintainer's.

## Commits and the changelog

Scope `staticjs-js` (`feat(staticjs-js): ...`); a change to staticjs the
package needs is its own commit, scoped `staticjs`. The changelog follows
ADR 0002 (`mise run changelog:check staticjs-js`), written at release
time.

## Layout

```
README.md, CHANGELOG.md, package.json, mise.toml
index.mjs, index.d.ts   the API and its types
src/js.c                the C layer
docs/api-map.md         each API item → the C layer → staticjs
test/                   package.test.mjs (Node), browser.test.mjs (Chromium)
mise/staticjs_js.rb     what the tasks share
mise/tasks/             build, test, pack
staticjs.wasm, build/   built (not in git)
```

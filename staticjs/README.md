# staticjs

staticjs is a pure, total subset of JavaScript, as a freestanding C library.
Give it a source and some fuel, and it gives back a value: from C, or from
JavaScript through a WebAssembly module. It is meant for programs written
by someone other than the host: a plugin a user writes to transform their
records, a filter, a computed field, or a program an agent writes to call
the host's tools in code instead of one tool call at a time. These must
give the same answer every time, end, and touch nothing but what they are
given.

```js
orders
  .filter((o) => o.status === "open" && Temporal.PlainDate.compare(o.due, today) < 0)
  .toSorted((a, b) => b.total - a.total)
  .map((o) => ({ id: o.id, overdue: o.due.until(today).days }))
```

## The rule

> A program staticjs accepts is a valid JavaScript program, and evaluating
> it gives the same result as JavaScript does, or a fault. Never a
> different value.

Where JavaScript would coerce a value, compare identities or look on a
prototype, staticjs stops with an error that says why, rather than guess.
The test suite holds it to the rule against Node: every spec case, and
hundreds of thousands of numbers, BigInts, JSON documents, regular
expressions and dates.

## What it guarantees

- **No side effects.** A program reaches only what the host passes in: its
  globals, its native functions and the modules it imports. It has no I/O
  and no global state. A value, once shared, never changes.
- **Every evaluation ends.** There are no loops but the built-ins over
  collections, and no recursion. Evaluation runs on fuel the host pushes,
  pauses where the fuel ends, and resumes where it paused.
- **Determinism.** The same program on the same inputs gives the same
  value and uses the same fuel on every machine. Even the clock and
  `Math.random`'s seed are values the host sets.
- **Bounded memory.** Memory comes only from the host's allocator, and
  every construct a program controls has a limit the host sets. Nothing
  recurses, so a deep program cannot overflow the C stack.
- **No C library.** The library is freestanding: it builds for every
  target, a browser's WebAssembly included, and calls nothing outside
  itself.

## Getting it

| You want | Use | Start with |
|---|---|---|
| staticjs in a C program, any compiler | a prebuilt `libstaticjs.a` or shared library (Linux and macOS, x86_64 and arm64) | [Getting started](docs/guide/getting-started.md) |
| staticjs compiled into your build | the amalgamation, one `staticjs.c` beside `staticjs.h`, built with clang | [Getting started](docs/guide/getting-started.md) |
| staticjs in a browser or in Node | `staticjs.wasm`, which imports nothing, and a small JavaScript wrapper | [In the browser](docs/guide/wasm.md) |

Releases are tagged `staticjs/v<version>` on the repository's GitHub
releases page. [CHANGELOG.md](CHANGELOG.md) says what changed in each.

## Documentation

**Using staticjs** (`docs/guide/`):

- [Getting started](docs/guide/getting-started.md): from a download to a
  program that evaluates JavaScript.
- [Embedding](docs/guide/embedding.md): the C API. Contexts, limits and
  fuel; values in and out; globals, native functions and modules; the
  clock and the seed; streaming.
- [The language](docs/guide/language.md): what a program can say, and
  what every built-in gives.
- [Faults and refusals](docs/guide/faults.md): where staticjs stops instead
  of following JavaScript, and what it refuses when compiled.
- [Performance](docs/guide/performance.md): fuel, memory, the fast paths,
  and a comparison with Node and QuickJS.
- [In the browser](docs/guide/wasm.md): `staticjs.wasm` from JavaScript.
- [Examples](examples/README.md): whole programs, built and checked by the
  test suite.
- `include/staticjs.h`: the API reference. Every declaration has a comment.

**Working on staticjs** (`docs/internals/`): [AGENTS.md](AGENTS.md) for the
rules, then [docs/internals/](docs/internals/README.md) for testing,
benchmarks, how to extend staticjs, and design notes on each part.

Reading this in a release tarball? The documentation is in the repository,
under `staticjs/` at <https://github.com/tinyactors-dev/tinyactors>.

## Status

staticjs 0.9 is the first release: the language and its runtime. Still to
come: a statechart data model built on it, `tinyactors-staticjs` (the
[proposal](../docs/proposals/staticjs.md) says why staticjs exists and how a
statechart will use it); a hash index for large objects and sets; and a
static cost estimate.

## License

The repository's `LICENSE`. `src/math.c` and `src/number.c` port code from
fdlibm and V8, whose notices are `LICENSE.fdlibm` and `LICENSE.v8`. Every
release tarball carries all three.

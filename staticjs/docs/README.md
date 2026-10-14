# staticjs documentation

Two sets, for two readers.

## Using staticjs: `guide/`

For anyone who embeds staticjs or writes programs for it. These pages say
what you can rely on.

| Page | Read it to |
|---|---|
| [getting-started.md](guide/getting-started.md) | get a release, build a first program with a prebuilt library or the amalgamation, and try programs |
| [embedding.md](guide/embedding.md) | use the C API: contexts, allocators, limits, fuel, values and who owns them, globals, native functions, modules, the clock and the seed, streaming |
| [language.md](guide/language.md) | see what a program can say: syntax, modules, every built-in, BigInts, Maps, JSON, Temporal, Date, and changing a value a function owns |
| [faults.md](guide/faults.md) | see where staticjs stops instead of following JavaScript, and what it refuses when compiled |
| [performance.md](guide/performance.md) | see what fuel and memory an evaluation takes, which callbacks run fast, and how staticjs compares with Node and QuickJS |
| [wasm.md](guide/wasm.md) | run staticjs in a browser or in Node through `staticjs.wasm` |

The examples in [`../examples/`](../examples/README.md) are whole programs
for each of these. The reference for every function is
`../include/staticjs.h`.

## Working on staticjs: `internals/`

For contributors: how staticjs is tested, measured and extended, and design
notes on each part. Start with [`../AGENTS.md`](../AGENTS.md), then
[internals/README.md](internals/README.md).

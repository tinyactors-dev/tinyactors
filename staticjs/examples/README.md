# Examples

Whole programs that use staticjs. `mise run test` builds each one as
[getting started](../docs/guide/getting-started.md) tells you to (the
example, `staticjs.c` and `staticjs.h`, nothing else), under
AddressSanitizer and UBSan, and checks that it prints what
`expected/<name>.txt` says. They stay correct as the library changes.

| Example | Shows | Run |
|---|---|---|
| [hello.c](hello.c) | the smallest whole program: a context, an expression compiled, evaluated on fuel, its value written | `./hello` |
| [eval.c](eval.c) | a command-line evaluator: expressions and modules, error reports with positions, a fuel budget, the clock from the host, `Math.random`'s seed | `./eval '[3, 1, 2].toSorted((a, b) => a - b)'` |
| [host.c](host.c) | a host that runs a plugin its users wrote: a library module, the plugin compiled once and evaluated per record, globals built through the API, a native `rate()`, a module imported by name, the clock pushed by the host | `./host` |
| [stream.c](stream.c) | every stage pushed: source in 7-byte chunks, a sink of its own before the compiler, evaluation in slices of fuel, output through a 16-byte buffer | `./stream` |

## Building them

From a source release, with clang:

```sh
clang -std=c11 -O2 -I. hello.c staticjs.c -o hello
```

With a prebuilt library, with any C11 compiler:

```sh
cc -std=c11 -Iinclude hello.c lib/libstaticjs.a -o hello
```

In this repository, `mise run examples` builds them all into
`build/examples/` and checks their output. From JavaScript, staticjs is the
package `@tinyactors/staticjs` ([wasm.md](../docs/guide/wasm.md)).

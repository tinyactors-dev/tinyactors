# Working on staticjs

staticjs is a sibling library of `tinyactors-core`, like `tinyxml`: the
language runtime of a future data model, `tinyactors-staticjs`. It follows
core's C rules, because it must build wherever core does, the browser
included. The design is in `../docs/proposals/staticjs.md`.

## The rule comes first

A program staticjs accepts is valid JavaScript and gives the same result as
JavaScript does, or a fault. Never a different value.

- Before adding syntax or a built-in, write its spec cases in
  `tests/spec/`, and run `mise run diff-node`: it evaluates them in Node.
- Where JavaScript coerces, compares identities or reaches a prototype,
  fault (`sjs_vm_fault`) instead of following it.
- A refusal at compile time is `syntax` only when JavaScript refuses it too;
  JavaScript that staticjs leaves out is `unsupported`. `diff-node` checks
  both.

## C rules

- **No C library.** `stdint.h`, `stddef.h` and `stdbool.h` only. The build
  compiles `src/` with `-ffreestanding -nostdlibinc`, and
  `mise run check-freestanding` builds it for `wasm32-freestanding`.
- **Memory only through the embedder's allocator**, and through the
  context for anything a program can reach.
- **No recursion.** Keep an explicit stack. `mise run check-recursion`
  fails on any cycle in the call graph.
- **At most three parameters per function;** introduce a struct for more.
  `mise run check-parameters` enforces it.
- **A file-scope name means one thing across `src/`.** check-recursion
  compiles every source as one translation unit, as an amalgamation would.
- **One public header:** `include/staticjs.h`, prefix `sjs_`. `src/*.h`
  are private.
- **No global mutable state.** The built-in function values are immortal
  constants that nothing writes.
- **Push, never pull.** Every stage is a state machine driven by its
  caller: bytes into the parser, events into the compiler, fuel into the
  evaluator, buffer space into the writer. None blocks or calls back for
  more input.

## Tests

- `tests/spec/*.spec`: the language, case by case (format at the top of
  `tests/spec_test.c`). Each case runs with the source pushed whole, a byte
  at a time and in chunks, and must free everything it allocated.
- `tests/api_test.c`: the public API, natives, modules, sinks, fuel,
  limits, and running out of memory.
- `tests/number_test.c`: number formatting, remainder and rounding.
- `mise run test --full` also fails every allocation of every spec case
  (`spec_test --full`), and of a larger program (`api_test --full`).

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
- Adding a built-in function or method: follow `docs/builtins.md`, which
  says how to keep it from slowing evaluation down.
- Any other extension (syntax, a value kind, a shape or rewrite, mutation,
  numeric code, measuring, delegating to agents): `docs/extending.md` gives
  the steps for each goal.
- A refusal at compile time is `syntax` only when JavaScript refuses it too;
  JavaScript that staticjs leaves out is `unsupported`. `diff-node` checks
  both.
- Values are never changed once shared. A function may change a value only
  through a variable of its own that holds it alone (`docs/mutation.md`);
  everything that exists for that is tagged `MUTATION`, its spec cases are
  in `tests/spec/mutation.spec`, and its commits end with `Mutation: yes`.
  Keep new mutation code tagged the same way.

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
- `tests/matcher_test.c`: the shape matcher (`docs/matcher.md`), on every
  spec source. `@shapes` in a spec case says which functions it tags.
  Changing `src/shapes.rules`: run `mise run shapes` and commit the
  headers it writes; `mise run test` fails while they are stale, and runs
  the generator's tests (`mise run shapes-test`).
- `tests/number_test.c`: number formatting, remainder and rounding.
- `tests/json_test.c`: `JSON.parse` and `JSON.stringify` on random inputs;
  `--print` gives `diff-node` the same inputs to compare with Node.
- `tests/temporal_test.c`: Temporal values on fixed lists of inputs;
  `--print` gives `diff-node` expressions to compare with the vendored
  temporal-polyfill (`docs/temporal.md`).
- `mise run test --full` also fails every allocation of every spec case
  (`spec_test --full`), and of a larger program (`api_test --full`).

## Benchmarks

`mise run bench` and `mise run bench-compare <commit>` (README.md,
"Benchmarks"); `mise run bench-engines` compares staticjs with Node and
quickjs-wasi.

- Before committing a change to `vm.c`, `value.c`, `builtins.c`,
  `memory.c`, or the hot paths of the parser and the compiler, run
  `mise run bench-compare <commit before it>` and quote its table, with
  the machine hash, in the commit body. Claim only what it flags: a
  change within the runs' noise, or marked `?`, is not evidence either
  way (README.md, "Benchmarks", says how far a build compared with itself
  moves).
- A new case in `bench/bench.c` gets a name (`group.subject[.variant]`),
  a unit, its sizes (L for linear work; Q for work that is quadratic today,
  capped at 10,000, or smaller when it is worse) and the value it gives at
  its smoke size, checked against Node.
- A timing assertion never goes into `mise run test`: it runs only
  `bench --smoke`, which checks each case's value and that it frees what it
  allocates, so a leak there fails the tests.
- `mise run bench-engines` reads its cases from bench.c (`bench/engines/`):
  after changing a fixture there, change `bench/engines/guest.js` to match,
  or its verification stops the run. Commit the report a full run writes.

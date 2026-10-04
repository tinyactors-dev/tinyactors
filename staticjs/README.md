# staticjs

staticjs is a pure, total subset of JavaScript, as a freestanding C library.
It is the language runtime of the staticjs data model; see the
[proposal](../docs/proposals/staticjs.md) for why it exists and how a
statechart uses it. The data model itself will be a separate project,
`tinyactors-staticjs`, that depends on this one, as a tree-sitter grammar
depends on the tree-sitter runtime.

## The rule

> A program staticjs accepts is a valid JavaScript program, and evaluating
> it gives the same result as JavaScript does, or a fault. Never a
> different value.

`mise run diff-node` checks it: every case in `tests/spec/` that expects a
value is evaluated in Node and must give the same value; every case staticjs
refuses as a syntax error must be one JavaScript refuses too; every case it
refuses as unsupported must be valid JavaScript; and 100,000 doubles must
format as `String(x)` does.

## What it guarantees

- **No side effects.** There is no assignment, no mutating method, no
  statement but `const` and `return`, and no way to reach anything that is
  not passed in. Values cannot change once they are shared.
- **Every evaluation ends.** Loops are built-ins over collections that
  already exist. A function cannot refer to itself or to a later `const`
  (refused when compiled), and a function that is already running cannot
  be entered again (refused when called, as Starlark does). Evaluation runs
  on fuel the caller pushes, and stops where the fuel ends.
- **Nothing recurses.** The parser, the compiler, the evaluator, the writer
  and the release of nested values all keep explicit stacks. A callback of
  `map` or `reduce` is a frame on the evaluator's own stack, not a C call.
- **No C library, no I/O.** Memory comes from the embedder's allocator.
  Every unbounded construct has a limit in `sjs_limits`.

## Push and stream

```c
/* Bytes in, events out: the compiler is a sink of the parser's events. */
sjs_compiler *compiler;
sjs_compiler_config cc = { context, SJS_GOAL_EXPRESSION, globals, global_count, modules };
sjs_compiler_init(&compiler, &cc);
sjs_parser *parser;
sjs_parser_config pc = { allocator, SJS_LIMITS_DEFAULT, SJS_GOAL_EXPRESSION, sjs_compiler_sink(compiler) };
sjs_parser_init(&parser, &pc);
while (have_bytes) sjs_parser_push(parser, next_chunk());   /* any size, down to one byte */
sjs_parser_finish(parser);
sjs_program *program;
sjs_compiler_finish(compiler, &program);

/* Fuel in, a value out. */
sjs_vm *vm;
sjs_vm_init(&vm, &(sjs_vm_config){ context });
sjs_vm_start(vm, &(sjs_run_params){ program, values, global_count });
while (sjs_vm_run(vm, 1000) == SJS_RUN_PAUSED) { /* do other work */ }

/* A value out as text, into a buffer of any size. */
sjs_value result;
sjs_vm_take_result(vm, &result);
sjs_writer *writer;
sjs_writer_init(&writer, &(sjs_writer_config){ context, result });
while (sjs_writer_write(writer, &buffer) == SJS_NEED_SPACE) drain(&buffer);
```

`sjs_compile()` does the first half for a source held in memory. A sink
other than the compiler (a formatter, a linter) can take the same events.

## The language

Expressions: numbers, strings, template literals, `true`, `false`, `null`,
`undefined`, `NaN`, `Infinity`, arrays and objects (spread, computed and
shorthand keys), `new Set(...)`; `+ - * / %`, `< <= > >=`, `=== !==`,
`&& || ??`, `?:`, `!`, unary `-` and `+`, `typeof`, `in`; members, calls
and spread arguments, with `?.`; arrow functions with an expression body,
or a block of `const` declarations ending in one `return`; array and object
patterns in parameters and `const`.

Modules: `const` declarations, `import x from "m"`, `import { a, b as c }
from "m"`, `import * as ns from "m"`, `import x, { y }` and `import x, *
as ns`, `import "m"`; `export const`, `export default` of an expression,
`export { a, b as c }` (also of names declared later, and `a as default`),
and the re-exports `export { a, b as c } from "m"`, `export * from "m"`
and `export * as ns from "m"`. A line break ends a statement where
JavaScript inserts a semicolon. Imports may come anywhere at the top level,
but a name is bound only from its import on. A module's value is its
namespace: an object of every export, with keys sorted by UTF-16 code units
as JavaScript sorts a namespace's. A later program imports it by name
through an `sjs_modules` adapter, and only a module already evaluated can
be imported, so imports cannot form a cycle.

Built-ins:

| | |
|---|---|
| arrays | `at` `concat` `every` `filter` `find` `findIndex` `findLast` `flat` `flatMap` `includes` `indexOf` `join` `map` `reduce` `slice` `some` `toReversed` `toSorted` `with`, `length` |
| strings | `endsWith` `includes` `indexOf` `slice` `split` `startsWith` `toLowerCase` `toUpperCase` `trim`, `length`, indexing |
| sets | `has` `union` `intersection` `difference` `symmetricDifference` `isSubsetOf`, `size` |
| functions | `String` `Boolean` `Array.isArray` `Object.keys` `Object.values` `Object.entries` `Object.fromEntries` `Number.isInteger` `Number.isSafeInteger` `Number.isFinite` `Number.isNaN` |
| `Math` | `abs` `min` `max` `floor` `ceil` `round` `trunc` `sign`, and its constants |

The embedder can add functions (`sjs_function_native`), such as SCXML's
`In()`. They are the one way into a program from outside.

## Where it faults instead of following JavaScript

These follow from the rule: staticjs stops where JavaScript would coerce,
compare identities, or find something on a prototype.

- `===` on two objects (identity); `+` of a string and an object; arithmetic
  or `<` on values that are not both numbers (or, for `<`, both strings).
- A property JavaScript finds on `Object.prototype` (`toString`,
  `__proto__`); reading a method without calling it. A namespace
  (`import * as ns`) is an object too: `ns.toString` and `"valueOf" in ns`
  fault where JavaScript finds nothing, and so does turning a namespace into
  a string.
- `toSorted()` without a compare function; `toLowerCase()` of a string
  that is not ASCII; slicing a string through the middle of a surrogate pair.
- A number literal outside Clinger's fast path (digits worth more than
  2^53, or a power of ten past 10^22) is refused when compiled.

## Refused when compiled

JavaScript staticjs leaves out is refused as `unsupported`, never given a
different meaning. Among the module forms: `export let`, `export function`,
`export class` and `export default function`; a string as an exported or
imported name (`export { a as "x-y" }`), as an object cannot keep the order
a namespace gives it; `import()` and `import.meta`; two `export *` that
give one name from different modules, unless an explicit export settles it,
as staticjs cannot tell whether they are one binding; and a name used
before its import or declaration, even in an earlier function.

## Status

The first stage of the proposal: the language. Not done yet, in the
proposal's order: the data model (`tinyactors-staticjs`); updating a value in
place when it has one reference; a hash index for large objects and sets;
a static cost estimate; `Map`; number literals beyond the fast path.

## Tasks

```sh
mise run build              # the library (freestanding) and the test programs
mise run test               # everything below, the way `mise run test` at the root runs it
mise run test --full        # also fail every allocation of every spec case
mise run check-parameters   # at most three parameters per function
mise run check-recursion    # no cycle in the call graph
mise run check-freestanding # builds for wasm32-freestanding with no C library
mise run asan               # the tests under AddressSanitizer and UBSan
mise run diff-node          # the spec against JavaScript
```

The tests are `tests/spec/*.spec` (read by `tests/spec_test.c`, which runs
each case with the source pushed whole, a byte at a time and in chunks, on
different amounts of fuel), `tests/api_test.c` and `tests/number_test.c`.

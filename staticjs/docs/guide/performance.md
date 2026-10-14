# Performance: fuel, memory and speed

What an evaluation costs, and what staticjs does to keep the common idioms
cheap. None of it changes a value or a fault: a program cannot tell
whether an optimization ran, only how much fuel it used.

## Fuel

Fuel measures an evaluation's work: one unit per instruction of the
evaluator, and per element for the built-ins that loop. The same program
on the same inputs uses the same fuel on every machine and every target,
so fuel is a budget you can set per program and rely on. Time is not.

Some figures, from `examples/eval.c --stats` over an array of 100 objects:

| Program | Fuel |
|---|---:|
| `event.f > 25` (a guard) | 7 |
| `xs.map((x) => x.k)` | 205 |
| `xs.map((x) => x.k + 0)` | 605 |
| `xs.filter((x) => x.k > 3)` | 205 |
| `xs.reduce((s, x) => s + x.v, 0)` | 206 |
| `xs.toSorted((a, b) => a.k - b.k)` | 1,646 |
| `xs.toSorted((a, b) => a.k - b.k + 0)` | 4,928 |

The differences between the pairs come from shapes (below). A string
method charges a unit per 16 bytes it reads or writes, `JSON.parse` and
`JSON.stringify` a unit per 16 bytes and per value, and BigInt arithmetic a
unit per 16 products of 32-bit limbs.

## Shapes: callbacks run in C

The compiler recognises a few callback shapes and tags the function. A
built-in that receives a tagged callback runs its step in C rather than
calling the callback's bytecode:

- a key read, `x => x.key`, in `map`, `Array.from`, `Object.groupBy` and
  `Map.groupBy`;
- a comparator by a key or by value, `(a, b) => a.key - b.key` and
  `(a, b) => a - b` (or reversed), in `toSorted` and `sort`;
- a comparison of an element, or of its key, with a literal
  (`x => x.k > 3`, `x => x.s === "a"`), in `filter`, `find`, `findIndex`,
  `findLast`, `findLastIndex`, `some` and `every`;
- a sum, `(s, x) => s + x` and `(s, x) => s + x.key`, in `reduce`.

An iterator's helpers of the same names take the same shapes.

The C step runs only while its preconditions hold: the element is an object
that holds the key, the operands are numbers. Any other element goes
through the callback's bytecode, which gives its value or faults where it
faults. A tagged callback costs about 2 units per element where its
bytecode costs 6 to 8. In time, a pluck by `map` took 15 ns per element
in the engine comparison below, where `map` with a callback that computes
took 40. The test suite runs every spec case with shapes and without, and
both must agree. `sjs_compiler_config.no_shapes` turns them off.

Write a callback in one of these shapes when you can: `x => x.total`, not
`({ total }) => total`.

## A value nothing else holds is updated in place

Values are counted references without cycles. A value with one reference
is visible only to what holds it, so staticjs changes it in place where
JavaScript would make a copy:

- the last read of a local moves its value instead of copying it;
- `reduce` and `reduceRight` move their accumulator into the callback and
  back;
- a literal that begins with a spread (`{...acc, [k]: v}`, `[...xs, x]`)
  takes over a source nothing else holds, and `concat`, `with` and
  `toSorted` take over such a receiver.

So `xs.reduce((acc, x) => ({ ...acc, [x.id]: x }), {})` is linear here,
where it copies the accumulator at each step in other engines. With 1,000
elements it ran several hundred times faster than Node in the engine
comparison below. A closure, a global, a module's export or a value a
native keeps is always another holder, so it is copied. Owned mutation
rests on the same count: `acc.push(x)` in a `reduce` changes the
accumulator the callback holds alone ([language.md](language.md#changing-a-value)).

**Strings.** `+` and a template extend a string nothing else holds: in
`reduce((acc, x) => acc + x, "")`, `` (acc, x) => `${acc}${x},` `` or
`s += x` on a `let`, the string's block grows to twice its length when
full, and the new text is appended. Building a string of n bytes copies
O(n) bytes in O(log n) allocations, and fuel is a unit per 64 bytes
appended rather than a charge for the whole string each time. A string
holds at most twice its bytes. `acc.concat(x)` extends its receiver the
same way.

**BigInts.** `+`, `-` and unary `-` of a BigInt nothing else holds (a
reduce's accumulator, a `let` whose last read moved it) write the result
into its block when it has room, and so does `*` by a small BigInt. Adding
small BigInts in a `reduce` then allocates nothing per element.

## Memory

Memory comes only from your allocator. `sjs_context_live_bytes` gives what
a context holds now. An evaluator with the default limits holds about
101 KiB, and a compiled guard such as `event.f > 25` about 0.3 KiB more.
About 60 KiB of the evaluator is its stack of `max_stack` slots, so
`max_stack = 256` makes an evaluator of about 41 KiB. Keep one evaluator
and start each evaluation on it, rather than making one per evaluation.
`max_value_items`, `max_string_bytes` and the other limits bound what one
evaluation can allocate ([embedding.md](embedding.md#limits)).

## Compared with other engines

`bench/engines/REPORT.md` compares staticjs with Node 22 (with its JIT and
`--jitless`) and with QuickJS-NG compiled to WebAssembly, on the same
programs and inputs, run as the C API runs them. The figures below are from
one run, on one 4-core x86_64 machine (`f9d38355aeb8`, October 2026). They
show proportions, not a promise. Over its 33 evaluation cases, the
geometric mean of the other engine's time over staticjs's was:

| Engine | Ratio | Reads as |
|---|---:|---|
| Node 22, JIT | 0.43 | Node takes 0.43 of staticjs's time |
| Node 22, `--jitless` | 1.10 | about the same as staticjs |
| QuickJS-NG in wasm | 4.81 | staticjs is about 5 times faster |

Evaluating a compiled guard from C took 0.14 µs; compiling one took
6.4 µs. Node's `vm.Script.runInContext` of the same guard took 1.4 µs, and
a fresh V8 context 330 µs. staticjs suits many short evaluations, each
started by a host. The report gives every case, the memory each engine
held, the method and its caveats.

To measure a change to staticjs itself, see
[../internals/benchmarks.md](../internals/benchmarks.md).

# staticjs: a pure, total subset of JavaScript as a data model

**Status:** Proposed (2026-10-04). The first stage, the language library,
is in [`staticjs/`](../../staticjs/README.md).
**Touches:** a new `staticjs/` library (the language), and later a new
`tinyactors-staticjs/` (the data model over it; see [Stages](#stages)).
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

A data model for users who use actors as the control layer. Their charts
hold little data: a few counters, a set of IDs, the request to answer. They
talk to the data plane through I/O processors. What they need from a data
model is:

- **guaranteed free of side effects**: an expression cannot do anything
  outside the session, and a guard cannot change the session either;
- **modules**: functions shared between charts, imported by name;
- **sandboxed**: an expression reaches only what it is given, and every
  evaluation ends;
- **fast**: a guard costs tens of nanoseconds, not a call into another
  runtime.

The ECMAScript data model (SCXML B.2) gives none of these by construction,
and the null data model (core ADR 0057) has no data at all. CEL has the
guarantees but no modules, no fold and an unfamiliar syntax; Starlark has
modules, but statements and loops whose cost is only known at run time.
staticjs takes the guarantees of the first and the familiarity of
JavaScript.

## The rule

> **A program staticjs accepts is a valid JavaScript program, and evaluating
> it gives the same result as JavaScript does, or a fault. Never a different
> value.**

An expression is evaluated as JavaScript would evaluate `(` source `)`. A
module is a JavaScript module. A fault is where staticjs declines to follow
JavaScript (an object compared with `===`, a string added to an array): the
evaluation stops, and a data model turns it into `error.execution`.

The rule has three consequences:

- **No new syntax.** Authors bring what they know. Nothing looks familiar
  and means something else.
- **Differential testing.** Every test case runs in Node too, and the
  results must agree (`mise run diff-node` in `staticjs/`).
- **A way up.** A chart that outgrows staticjs changes
  `datamodel="staticjs"` to `"ecmascript"` and runs unchanged.

## The language

### What is in

- Literals: numbers, strings, template literals, `true`, `false`, `null`,
  arrays, objects (with shorthand, computed keys and spread).
- Operators: `+ - * / %`, unary `- + !`, `typeof`, `< <= > >=`, `=== !==`,
  `&& || ??`, `?:`, `in`, and optional chaining `?.`, `?.[]`, `?.()`.
- Arrow functions, with an expression body or a block of statements:
  `const`, `if`/`else`, blocks and `return`. Parameters and `const` take
  identifiers and array and object patterns, with default values and rest
  elements; a function can take rest parameters.
- Calls, with spread arguments. `new Set(...)`.
- Modules: `const`, and the static module syntax around it: default,
  named and namespace imports (`import x, { a, b as c } from "name"`,
  `import * as ns from "name"`), `export const`, `export default`, export
  lists (`export { a, b as c }`) and re-exports (`export { a } from`,
  `export * from`, `export * as ns from`). A module's value is its
  namespace, keys sorted as JavaScript sorts them.
- The built-ins listed in [`staticjs/docs/guide/language.md`](../../staticjs/docs/guide/language.md):
  the methods of arrays that do not change them (`map`, `filter`, `reduce`,
  `toSorted`, `with`, ...), of strings, of `Set` (including `union` and
  `difference`), and parts of `Math`, `Object`, `Number` and `Array`.

### What is out, and which guarantee each one protects

| Left out | Protects |
|---|---|
| `let`, `var`, assignment, `++`, `delete` | no side effects |
| `export let`, `export function`, `export class` | no side effects |
| `push`, `sort`, `splice`, `Set.prototype.add` and every other mutating method | no side effects |
| `for`, `while`, `do` | every evaluation ends |
| `switch`, `try`, `throw`, labels, `break`, `continue`, expression statements | a small language: `if` and `?:` choose, a fault stops |
| a function that refers to itself, or to a `const` declared after it | every evaluation ends |
| `this`, `class`, `function`, getters, `new` other than `new Set` | a small, closed set of values |
| `async`, `await`, generators, `import()`, `eval` | evaluation is a step function |
| `==` and `!=` | no coercions to remember |
| strings as export names (`export { a as "x-y" }`), and two `export *` that give one name | never a different value: an object cannot keep a namespace's order for every string, nor tell whether two exports are one binding |
| every host object (`globalThis`, `Date`, `Math.random`, `console`) | sandboxed and deterministic |

### Values

- **Numbers are float64**, as in JavaScript. IDs are strings.
- **Strings** are stored as UTF-8 and behave as JavaScript's UTF-16 strings:
  `length`, indices and comparisons count UTF-16 code units.
- **Arrays, objects and sets cannot change after they are made.** Nothing
  in the language could change them, so a value is shared by reference
  count, a `<foreach>` snapshot is free, and nothing can form a cycle.
  Reference counting is therefore complete: there is no garbage collector.
- **Object keys keep JavaScript's order**: keys that are array indices in
  ascending order, then the others in the order they were added. Iteration
  is deterministic, which replay needs.
- **Reading a property an object does not have** is `undefined`, unless
  JavaScript would find it on `Object.prototype` (`toString`, `__proto__`,
  ...). That is a fault, because staticjs objects have no prototype.
- **`===` on two objects is a fault.** In JavaScript it compares identity,
  which a pure language should not expose.

### Every evaluation ends

There is no loop statement, and the built-ins that loop do so over a
collection that already exists; `if` and `?:` only jump forward. A function
cannot call itself by name: a `const` cannot refer to itself or to a later
`const`, and modules cannot import each other in a cycle. A function could
still reach itself through a parameter (`(g => g(g))(g => g(g))`), so the
evaluator refuses to enter a function that is already running, as Starlark
does. Call depth is therefore bounded by the number of functions in the
program, and every evaluation ends.

Each evaluation also runs on **fuel** that the caller gives it: one unit
per instruction, and per element for the built-ins that loop. An
evaluation that runs out pauses, and the caller decides whether to give it
more.

## Push and stream: the pipeline

Every stage is a state machine that the caller drives by pushing work into
it. No stage pulls input, blocks, or recurses.

```
bytes ──push──▶ parser ──events──▶ compiler ──▶ program
                  │                                │
                  └──events──▶ any other sink      ▼
                                         fuel ──push──▶ evaluator ──▶ value ──▶ writer ──▶ bytes
```

1. **The parser** is fed the source in chunks of any size, down to one
   byte. A token can span chunks. It emits a stream of syntax events in
   evaluation order (operands before their operator, with markers where
   `&&`, `?:` and `?.` branch) to a sink. It keeps an explicit stack, never
   the C stack.
2. **The compiler is a sink.** It turns each event into bytecode as it
   arrives, resolves names, and checks the subset. A program exists when
   the last byte has been pushed. Another sink (a formatter, a linter, an
   editor) can take the same events.
3. **The evaluator** runs a program in steps. The caller pushes fuel; the
   evaluator stops when the result is ready, when the fuel is spent, or at
   a fault. Built-ins that call functions (`map`, `reduce`, `toSorted`)
   are frames on the evaluator's own stack, so a callback is not a C call,
   and an evaluation can pause inside one.
4. **The writer** turns a value into text in a caller's buffer, and
   resumes when the buffer is full. It serves `value_text`, logging, and
   writing a value for offload.

## As an SCXML data model

| SCXML | staticjs |
|---|---|
| `cond` | an expression, true if JavaScript would find it truthy |
| `expr` | an expression |
| `location` | a path of names, `.name` and `[expr]`. `<assign>` writes a new version of the value along the path: the same result as JavaScript's assignment, without changing anything shared |
| `<script>` | only at the top of the document, and only a module: imports, `const` and exports. A `<script>` in executable content is refused when the chart is compiled (SCXML A.1.4, 5.9.4) |
| `<foreach>` | over an array or a set; the snapshot is a reference count |
| `_event`, `_sessionid`, `_name`, `_ioprocessors` | names bound for each evaluation; they cannot be assigned |
| `In(id)` | a function the data model provides |
| `_x.now` | the macrostep's logical time (`ta_dm_host_time`) |
| `value_write`, `value_read` | the writer, and a reader of the same text |

The condition language must have no side effects (SCXML B); staticjs has
none anywhere. SCXML 5.3.2 says the data model changes only through
`<assign>` and `<finalize>`; under staticjs that is literally true.

A chart from the ring protocol proposal's
[`progress.scxml`](ring-proto/charts/progress.scxml) needs one change per
counter:

```xml
<transition event="progress.acked">
  <assign location="counts.acked" expr="counts.acked + 1"/>
  <raise event="changed"/>
</transition>
```

and `campaign-windowed.scxml`'s window becomes:

```xml
<assign location="window" expr="queue.slice(0, windowSize)"/>
<assign location="queue" expr="queue.slice(windowSize)"/>
...
<assign location="pending" expr="pending.difference(new Set([_event.data.id]))"/>
```

## Stages

1. **The language library** (`staticjs/`, done in its first form): the
   push parser, the streaming compiler, the evaluator with fuel, values,
   built-ins, modules and the writer, freestanding and without recursion,
   tested against Node.
2. **The data model**, in a new `tinyactors-staticjs/`: a
   `ta_datamodel` adapter with locations, `In()`, the system variables and
   `value_write`. The split follows tree-sitter's: the runtime
   (`tree-sitter`) knows nothing about any grammar, and a grammar package
   depends on it. Here `staticjs/` is the language and knows nothing about
   statecharts; `tinyactors-staticjs/` depends on it and on core's data
   model interface, and every binding links it as it links core's null
   data model. It is free of any host language, so it is not in a binding
   (README, "Layers and responsibilities").
3. **Speed and cost**, each when a measurement asks for it:
   - updating a value in place when it has one reference (Koka's Perceus),
     so `{...acc, [k]: v}` in a `reduce` does not copy (done: the last
     read of a local moves its value, reduce moves its accumulator, and
     spreads into a literal, `concat`, `with` and `toSorted` take over a
     value nothing else holds);
   - a hash index for large objects and sets, which are searched in order
     today;
   - a static estimate of an expression's cost from declared input sizes,
     so a rule uploaded by a third party can be refused before it runs.
4. **More of JavaScript, where it keeps the rule**: `Map`, number literals
   that need more than Clinger's fast path, `**`, string methods outside
   ASCII.

## Open questions

- Should the subset have an optional type layer for event payloads (JSDoc
  or a sidecar schema), so `_event.data.kwh` typos fail at compile time?
  It must not change the source, or the rule breaks.
- Is runtime refusal of re-entry enough, or should passing a function to
  itself be refused statically for the common shapes?

## Background

The comparison that led here (CEL, Starlark, Dhall, Lua, Jsonnet, Wasm, and
the SCXML standard's own rules) was a design discussion, not a research
note. Its sources: the [CEL
specification](https://github.com/google/cel-spec), the [Starlark
specification](https://github.com/bazelbuild/starlark/blob/master/spec.md)
(recursion and freezing), Reinking, Xie, de Moura and Leijen, "Perceus:
Garbage Free Reference Counting with Reuse" (PLDI 2021), and the SCXML
recommendation in `tinyactors-core/docs/internal/scxml.md`.

# ADR 0031: Values describe themselves, and data models print them

**Status:** Accepted and implemented (2026-09-28); see the implementation notes for where the code differs from the text. Refines ADR 0019 (a registered data model also prints), ADR 0027 (`encode()` can write a chart for another data model), ADR 0029 (the built-in data model resolves names only) and ADR 0030 (how an encode writes a `literal()`).
**Date:** 2026-09-28

## Context

A builder chart holds JavaScript values and functions as handles (ADR
0030). To write it as text (ADR 0027), each handle has to become source
text that a data model can read back. Today napi's encode printer writes a
`literal()` as JSON when JSON says its value exactly, and refuses
everything else: a function, a `Date`, a `Map`, a class instance.

Two observations settle how to do better:

- **Printing is the inverse of compiling, so it belongs to the data
  model.** An SCXML chart names its data model, and that data model is
  what reads its source text. On an ECMAScript data model,
  `new Date(1700000000000)` and a function expression are ordinary
  expressions. What a registered data model (ADR 0019) lacks is the other
  direction.
- **napi's built-in data model is JavaScript without evaluation.** It runs
  JavaScript functions and values as handles, and interprets a short list
  of source text without evaluating any (ADR 0011, ADR 0019, ADR 0029):
  `In(state)`, data names, `name()`, and JSON values. It stays that way:
  it works with *named references*, looked up in what the system
  registered, never with code in text.

A class instance cannot be printed without its class's help: an instance
does not record the arguments that built it.

## Decision

1. **Values describe themselves with a symbol.** The package exports
   `serialize`, which is `Symbol.for('tinyactors.serialize')`, so a
   library's classes can implement it without depending on the package:

   ```js
   class Money {
     constructor(amount, currency) { this.amount = amount; this.currency = currency; }
     [Symbol.for('tinyactors.serialize')]() {
       return { name: 'Money', arguments: [this.amount, this.currency] };
     }
   }
   ```

   The method returns the name its constructor is registered under and the
   arguments that rebuild it. It says nothing about syntax: each data
   model prints the description in its own language, and prints the
   arguments the same way, so they may be anything printable, including
   other self-describing values. A value that describes itself is printed
   from its description whatever else it is.

2. **Classes are registered by name.** `system.registerClass(name,
   constructor)` registers a constructor, as `registerFunction()` registers
   a function. Functions and classes share one name space per system: a
   name is registered once, whichever it names. The standard constructors
   `Date`, `Map`, `Set`, `RegExp` and the typed arrays are registered in
   every system and cannot be replaced, so data models print and read
   them without the values implementing anything.

3. **The built-in data model reads `new Name(arguments)`.** A named
   reference to a registered class, beside `name()` for a registered
   function. Its arguments are JSON values, in any position of which
   `new Name(arguments)` may appear again. The arguments are parsed when
   the chart is defined, an unregistered name refuses the chart there
   (core ADR 0058), and the constructor runs each time the expression is
   evaluated, as it would on an ECMAScript data model: text is a
   recipe, not a shared object. Nothing else is added to its source text,
   and it evaluates nothing.

4. **A data model prints.** `DatamodelCompiler` gains an optional
   `print(expression, role)`, the inverse of `compile`: it gets a host
   expression (`{ kind: 'function', function }`, `{ kind: 'literal',
   value }` or `{ kind: 'location', location }`) and returns source text
   in its language, or throws a `ChartCodecError` with code
   `unrepresentable`. A data model without `print` cannot be written to.
   napi's built-in data model prints what it reads:
   - JSON values, as today;
   - a self-describing value, a `Date`, a `Map`, a `Set`, a `RegExp` or a
     typed array as `new Name(arguments)`;
   - and refuses the rest: a function (write it as a named function), a
     location, and values JSON cannot say and nothing describes (`NaN`,
     `-0`, `undefined`, a `BigInt`, a plain object holding one, a class
     instance without `serialize`), and a value with a cycle.

5. **`encode()` can write a chart for another data model.**
   `definition.encode(format, { datamodel })` writes the chart with its
   `datamodel` set to that name and every host expression printed by that
   data model's `print`. Without `datamodel`, the chart's own data model
   prints. The chart's source text (`In(state)`, data names, `name()`,
   JSON) is written as it is: each is also an ECMAScript expression. A
   data model that reads it must give it that meaning; the example
   ECMAScript data model puts the system's registered functions and
   classes in scope, and calls a registered function with `this` bound to
   the data and the context as its argument, as the built-in one does.

6. **The example ECMAScript data model prints** (`tools/ecmascript-datamodel.js`,
   not part of the package): JavaScript literals for primitives
   (`NaN`, `-0`, `undefined`, `1n` included), object and array literals,
   `new Name(arguments)` for described values and standard constructors,
   and a function as its source text. A printed function is faithful only
   when it uses nothing but `this` and the context: its source does not
   carry what it closes over. The documentation says so, and recommends
   named functions whenever that cannot be promised.

## Consequences

- A chart holding class instances round-trips through text wherever its
  classes are registered, on the built-in data model as well as on an
  evaluating one, and `lib/` still evaluates no text.
- Dates, Maps, Sets and typed arrays round-trip without anything to
  implement.
- Text is a copy. A chart printed from handles creates new values when its
  expressions run, where the original shared one value between actors, so
  it is another chart: defining its text beside the live original under
  the same identity is `definition-mismatch` (ADR 0030's notes), and
  another system defines it under that identity. A chart printed for
  another data model is another chart too; its explicit identity holds
  only for text written for the chart's own data model.
- Shared references inside a value are not described: each occurrence is
  printed separately, and a cycle is refused.
- A constructor used in a chart must be registered where the chart is
  read, as a named function must.

## Implementation notes

- **Where:** `lib/values.js` (the `serialize` symbol, the standard classes,
  the `new Name(...)` parser and builder, the built-in printer, the print
  wrapper, and the printed spec a JavaScript codec gets), `lib/system.js`
  (`registerClass`, the names kept for the compile scope, the data models
  kept for their `print`, choosing the printer in `encode()`),
  `src/napi_datamodel.{c,h}` (one registered-name table per kind, checked
  together; `new Name(...)` in `tna_source` and its binding when a chart is
  defined; the per-encode printer), `src/import.{c,h}` (the codec wrapper
  that renames the data model), `src/napi_system.c` (`registerClass`,
  `encodeDefinition`'s print and data model), `src/addon.c`
  (`setupLiteral(brand, construct)`), `tools/ecmascript-datamodel.js`.
  Commits: `06f5157`, `484cef7`, `a2c36b7`, `e4f146e`, `b87baea`, and the
  one that adds these notes.
- **`print` gets no role (point 4 said `print(expression, role)`).** Core's
  expression printer (`ta_chart_expression_printer`) is handed a host
  expression only, so napi cannot pass one. A printed function therefore
  has to serve as a value, a condition and a script; the example
  ECMAScript printer writes `(source).call(this, __context)`, which does.
- **`compile` gets a third argument (implied by point 5, not declared
  there):** `scope`, `{ functions, classes }`, the names registered when
  the chart is defined, frozen and cached until the next registration.
  Point 5's "puts the system's registered functions and classes in scope"
  needs it: a data model has no other way to see them.
- **Writing another data model's name needed no core change.** napi wraps
  the codec for one encode: the wrapper's `encode` hands the codec it wraps
  a copy of the spec whose `datamodel` is the name asked for. Core ADR 0053
  lets a codec be any adapter, so this goes through
  `ta_definition_encode()` unchanged, source map included.
- **A JavaScript codec gets a printed chart only when `encode()` names a
  data model.** The printer runs inside a native codec's encode; a
  JavaScript codec gets the export as values instead (ADR 0027). With
  `{ datamodel }`, lib replaces every host expression in that spec with
  the data model's text and sets `datamodel`; without it, the codec gets
  the chart's values as before.
- **Encoding for the chart's own data model** uses its `print` when it has
  one. A registered data model without `print` still encodes a chart of
  source text alone (every imported chart, whose expressions are text), so
  the IRP round trip is unchanged; only naming such a data model
  explicitly throws `unsupported`.
- **The built-in data model also reads an array or object with
  `new Name(...)` inside** (`[new Date(1), "x"]`), which its printer writes
  for such a value. Source text that starts as JSON does and does not
  parse as JSON goes to the same parser as `new Name(...)`. Pure JSON is
  still parsed once into a literal; text with a construction in it becomes
  a function that builds its value at each evaluation.
- **Printed forms.** A `Date` is `new Date(<milliseconds>)`; an invalid one
  is refused by the built-in printer and written `new Date(NaN)` by the
  ECMAScript one. Only exact instances of the standard classes print as
  them: a subclass needs `[serialize]()`. The ECMAScript printer writes
  object keys as computed keys (`{["__proto__"]: 1}`), so `__proto__`
  stays an own property, and ends a function's source with a newline
  before `).call(...)`, so a trailing line comment cannot swallow the call
  (SCXML writes it as `&#10;`).
- **The data model's reason reaches the caller.** A printer's refusal
  replaces the codec's phrase in the `encode()` error, with code
  `unrepresentable`; an exception other than a `ChartCodecError` from a
  data model's `print` propagates from `encode()` as it is.
- **The built-in printer moved from `lib/statechart.js` to
  `lib/values.js`,** and the addon no longer holds a literal-text function:
  `setupLiteral` takes the literal brand and the construction parser.
- **Shared names are checked in C.** `registerFunction` and
  `registerClass` go through one native method; each table is a plain
  object whose properties are defined, not set, and a name in either table
  refuses the other. The refusal now reads "a function or class is already
  registered under this name".
- **Refusal messages** fit core's compile-error bound (about 90 bytes):
  the built-in data model's refusal of other source text now reads
  "expected a function or source text: In(), a data name, name(), new
  Name() or JSON".
- **Tests:** `test/values.js`: registration (one name space, the standard
  classes, `In`, `__proto__`, per system, after close); `new Name(...)`
  read as values and a condition, nested in maps, arrays and objects, with
  a new value per actor and per action; refusals at the expression
  (unknown class, a function name, other text, deep nesting, as a script);
  a builder chart of described values, standard classes and an array with
  a construction written as SCXML and read back by another system that
  registers the class, and refused by one that does not; the built-in
  printer's refusals with their reasons; `encode()` for a registered data
  model with `print` (its text, its refusal, an exception it throws, a
  non-string), a JavaScript codec getting the printed spec, and
  `unsupported` for a data model without `print`; the ECMAScript data
  model writing inline functions, literals and named calls, and another
  system reading them back and running them, and refusing a method and a
  native function. `test/handles.js`: a `Date` now has text; a class
  instance without `[serialize]()` has none. W3C IRP 176/176 on Node and
  Bun.

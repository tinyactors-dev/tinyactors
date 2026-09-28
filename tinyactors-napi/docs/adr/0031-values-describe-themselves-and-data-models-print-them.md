# ADR 0031: Values describe themselves, and data models print them

**Status:** Accepted (2026-09-28); not implemented. The target API is in `index.d.ts`, marked `// status: unimplemented` or `// status: partial`. Refines ADR 0019 (a registered data model also prints), ADR 0027 (`encode()` can write a chart for another data model), ADR 0029 (the built-in data model resolves names only) and ADR 0030 (how an encode writes a `literal()`).
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

- **Where:** `lib/system.js` (`registerClass`, the shared name space, the
  standard constructors), `index.js` (`serialize`), `src/napi_datamodel.c`
  (reading `new Name(arguments)`: parsing at define time, constructing at
  evaluation), napi's encode printer (`src/import.c` or wherever the last
  change put it: delegating to the chosen data model's `print`, and the
  built-in printer of point 4), `lib/codecs.js` or `lib/definition.js`
  (`encode()`'s `datamodel` option), `tools/ecmascript-datamodel.js`
  (`print`, and registered names in scope). Writing another data model's
  name into the document may need the export to be encoded by napi
  rather than through `ta_definition_encode` (core ADR 0053 lets an
  embedder call a codec itself); if it needs a core change, that is a
  core ADR of its own.
- **Tests:** a class with `serialize` round-trips through SCXML on the
  built-in data model and on the example ECMAScript one; `Date`, `Map`,
  `Set` and a typed array round-trip on both; an unregistered class name
  refuses the chart at the expression; a class name already taken by a
  function, and a standard name, are refused by `registerClass`;
  `new Name(...)` constructs a new value at each evaluation; a
  self-contained inline function round-trips through the ECMAScript data
  model and is refused by the built-in one; `NaN`, a `BigInt` and a cycle
  are refused by the built-in printer; a data model without `print` cannot
  be encoded to.

# ADR 0029: A chart calls a host function by name

**Status:** Accepted and implemented (2026-09-28); see the implementation notes, which add a fourth source form (JSON values) that point 5 needs. Refines ADR 0011 (the JavaScript data model's source text) and ADR 0027 (what round-trips). Leaves core ADR 0077's exemption for anonymous functions as it is.
**Date:** 2026-09-28

## Context

A builder chart's expressions are JavaScript functions. Core receives each
as a `ta_expression_host`, which it can retain and release and nothing
else (`tinyactors.h`, `ta_expression_host_vtable`): no name, no key, no
equality. So core cannot put a function into a chart's fingerprint, and
core ADR 0077 exempts such a chart from both registration rules. Defining
a builder chart twice, even from its own export with the same function
objects, registers two definitions under one identity (ADR 0027). A codec
cannot write a function either: the SCXML codec refuses it as
`unrepresentable`, so a builder chart has no text form.

SCXML's ECMAScript data model (SCXML 1.0 §5.8, B.2) puts a function in
the chart: a `<script>` runs in the session's global scope, so
`function isReady() {...}` becomes a variable that later expressions call
as `cond="isReady()"`. The body is source text in the document, so the
fingerprint covers it and every codec can write it. napi already offers
this to imported documents, through a data model registered with
`registerDatamodel()` (ADR 0019). It is not an answer for builder charts:
`lib/` ships no `eval` and no `new Function`, and a builder chart's point
is that its expressions are real functions, checked and debugged as
JavaScript.

SCXML has a second pattern for behaviour the chart does not contain: an
I/O processor or an invoker is named by its type, and the host resolves
the name. The chart's meaning includes the name; the host supplies the
implementation.

## Decision

1. **A system has named functions.**
   `system.registerFunction(name, run)` registers `run` under `name`, an
   identifier other than `In`. `run` is called as a builder function is:
   `this` is the actor's data and its argument the evaluation context
   (ADR 0011). A name is registered once per system: registering it again
   throws `invalid`, so every definition that names it means the same
   function for the system's life.
2. **Source text calls one as `name()`.** The built-in JavaScript data
   model reads `name()` (an identifier, then `()`, spaces allowed around
   it) as a call of the registered function, in every role but a
   location: a condition, a value, and a script. This joins its two other
   forms, `In(state)` and a bare data name, which are unchanged; a data
   name and a function name never collide, since only the call has
   parentheses. A call takes no arguments: the function reads what it
   needs from the context. A location stays a data name or a
   `{ get, set }` object.
3. **The name is resolved when the chart is defined.** The data model
   binds each call to its function while core prepares the definition,
   and keeps it with the definition's compiled expressions. A name no
   function is registered under refuses the chart, pointing at the
   expression (core ADR 0058), not at a run.
4. **The builder writes a call with `named(name)`.** `named("isReady")`
   is accepted wherever a function is: `when()`, `choose()`, a value
   (`assign`, `raise` data, a send's data or params), `log`, a script,
   `dataExpression`. It builds the source text `isReady()` into the spec.
   A condition may also be written as the string `"isReady()"`, since
   condition strings are source text already; a value cannot, because a
   string value is data. `ExpressionSpec` and `ScriptSpec` accept the
   text `name()`.
5. **A chart whose functions are all named is content core sees whole.**
   Its expressions are source text, so the fingerprint covers them and
   core ADR 0077's rules apply: defining it again answers the same
   definition, and the same explicit identity for different content is a
   mismatch. `export()` gives the calls back as text, every codec can
   write them (the SCXML codec as `cond="isReady()"`), and defining the
   encoded text answers the same definition. ADR 0027's round-trip
   contract is refined: "a chart without functions" means a chart without
   anonymous functions.
6. **A chart's revision names its functions, not their bodies.** Changing
   what `isReady` does does not change a revision, as changing an invoker
   does not. An explicit revision is already the author's promise that
   the chart has not changed (core ADR 0077); a host that changes a
   function's meaning registers it under a new name, or changes the
   revision.
7. **Anonymous functions stay.** They remain the quickest way to write a
   chart, exempt from core ADR 0077's rules and unrepresentable in a
   codec, as before. Nothing converts one into a named function
   implicitly: `Function.prototype.name` is not an identity (arrow
   functions are often anonymous, and two closures can share a name and
   capture different state).

A data model registered with `registerDatamodel()` compiles its own
source language and does not see these names; SCXML's `<script>`
functions are the answer there.

## Consequences

- A builder chart can be deduplicated, compared, exported, encoded to
  SCXML or another syntax, and defined again from the text, without an
  ECMAScript compiler.
- A document encoded from such a chart reads as ordinary SCXML: in an
  engine with an ECMAScript data model, `isReady()` is a call of a global
  function, which that platform supplies (a `<script>`, or its own
  environment).
- Another process that registers the same names can define the same
  chart from its text and get the same revision, which is what loading a
  session image there needs (core ADR 0028).
- Named functions live with the system, not in the actor's data, so they
  never reach an offloaded image, unlike the functions the ADR 0019
  example compiler copies into the data.
- A typo in a name is reported when the chart is defined.
- A chart mixing named and anonymous functions is still exempt from core
  ADR 0077 as a whole: one anonymous function is enough.

## Implementation notes

- **Where:** `src/napi_datamodel.{c,h}` (the `name()` and JSON forms in
  `tna_source`; `tna_bind_builtin`, which binds each call and parses each
  JSON value into the shared compiled table while core prepares the
  definition; the function table, `registerFunction`'s
  `tna_js_datamodel_register_function`), `src/napi_system.c` (the native
  `registerFunction`), `lib/statechart.js` (`named()`, the builder methods
  accepting it, plain data as JSON), `lib/system.js`
  (`registerFunction()`), `index.js`. Evaluation needed no change: a bound
  call is a host expression in the compiled table, run as a builder
  function is, and a parsed JSON value is one run as a `literal()` is.
- **Plain data is JSON text (not in the decision).** Point 5 was not true
  as written: the builder wrote every data value (`data('count', 0)`, an
  `assign` of a constant, a `raise`'s data) as a `literal()`, which is a
  host expression too, so a chart whose functions were all named was
  still exempt from core ADR 0077. The builder now writes a value JSON
  says exactly (finite numbers other than -0, strings, booleans, null,
  and arrays and plain objects of those, without holes, cycles, accessors
  or symbol keys) as JSON text, and the built-in data model reads source
  text that starts as a JSON value does (a digit, `-`, `"`, `[`, `{`, or
  `true`, `false`, `null`) as one, parsed once when the chart is defined
  into the same literal record a `literal()` gives. What JSON cannot say
  (a `Date`, `NaN`, `undefined`, a function in `data()`, a class
  instance) stays a `literal()`, and an explicit `literal()` stays one.
  An actor's value is the same object for every actor of the definition,
  as a `literal()`'s was. JSON text that does not parse, or used as a
  location or a script, refuses the chart.
- **Consequence of that:** a builder chart with plain data and no
  anonymous function is now answered by identity, so defining a different
  chart of that kind under an explicit revision already registered throws
  `definition-mismatch` where it used to register beside. No test relied
  on the old behaviour.
- **A named delay returns a duration.** The builder wraps a delay function
  so a number of milliseconds becomes an SCXML duration; nothing can wrap a
  named function without a function, so its result is the delay as it
  is, as a `delayexpr`'s is (`"500ms"`).
- **The refusal of other source text** now reads "expected a function, or
  source text: In(state), a data name, name() or JSON"; core cuts a
  compile error's message at about 90 bytes, which the longer wording
  passed.
- **Names:** an identifier of at most 254 bytes, other than `In`. The
  table is a plain object whose functions are defined, not set, as own
  properties, so `__proto__` is a name like any other and nothing on the
  prototype can be called.
- **Tests:** `test/functions.js`: a condition, a script, a value, a
  foreach array, a log value and a send event calling named functions,
  with `this` and the context; an unknown name refused at the expression
  (`states[0](a).transitions[0].condition`); a condition string
  `"ready()"`; a call as a location refused; registering a name twice,
  `In`, a non-identifier and a non-function refused, and after close;
  `__proto__`; a chart of named functions and plain data defined twice,
  and from its export, answers one definition, and a different call under
  the same identity is `definition-mismatch`; SCXML encode then define
  answers the same definition, and another system that registers the same
  names defines the same revision from the text; a chart with one
  anonymous function registers beside itself; plain data exported as JSON
  text, exotic values kept as literals, JSON refused as a script or when
  it does not parse; a registered data model gets `name()` as its own
  source text. `test/types/usage.ts` uses `registerFunction` and
  `named()`.

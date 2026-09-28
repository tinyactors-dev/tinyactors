# ADR 0030: A JavaScript value is one host expression, and charts call named functions

**Status:** Accepted and implemented (2026-09-28); see the implementation notes, which add an expression printer for encoding (point 5 of the notes) and record one open question. Applies core ADR 0087. Supersedes the "plain data is JSON text" part of ADR 0029's implementation notes; keeps the JSON source-text form it added, for document text. Refines ADR 0011 (how builder expressions reach core) and ADR 0027 point 1 (what round-trips within a process).
**Date:** 2026-09-28

## Context

Core exempted every chart with a host expression from registration by
identity (core ADR 0077). To get builder charts out of the exemption,
ADR 0029's implementation wrote each plain value the builder was given
(`data('count', 0)`, an assigned constant, a raised event's data) as JSON
text, which the built-in data model parsed back when the chart was
defined. That was a workaround, and a surprising one:

- A plain object the author passed became a copy parsed from its JSON. The
  chart no longer held the author's object.
- A class instance, a `Date` or a `Map` stayed a `literal()`, so whether a
  chart could be deduplicated depended on the type of a value in it.
- The built-in data model grew a JSON source form for it. That form is
  sound for document text (`<data expr="0">`): a JSON literal is an
  ECMAScript expression with the same meaning, and nothing the author wrote
  is converted. Only the builder's use of it was the workaround.

Core ADR 0087 removes the reason for it: core compares host expressions by
handle, a `(v, self, value)` triple, and answers a live definition whose
handles are all equal. napi's handle for an expression is a
`tna_expr_record`. Its `retain` already returns the same pointer. But
`tna_expression_make` allocates a new record each time a spec is
converted, so the same function in two specs is two handles, and nothing
ever matches.

## Decision

1. **One record per JavaScript value.** napi makes at most one
   `tna_expr_record` for each value and kind of expression, and reuses it
   while it lives:
   - **A function, a location object**: keyed by the object's identity.
   - **A `literal()`**: keyed by the value it holds, not by the box
     `literal()` returns, compared as `Object.is` compares: an object by
     identity; a primitive by value, so `literal(0)` is `literal(0)`,
     `NaN` is `NaN`, and `-0` is not `0`.
   - The table holds no strong reference: an object's entry goes when the
     object is collected or its record's last reference is released,
     whichever comes first, and a primitive's entry goes with its record's
     last reference.
   So the same values give the same handles, and core answers the live
   definition (core ADR 0087): defining the same builder chart again with
   the same functions and values, or defining a definition's own
   `export()`, answers that definition.

2. **Values stay the values the author passed.** The builder writes every
   value as a `literal()` again, and its JSON encoding (`jsonExact`,
   `jsonText` in `lib/statechart.js`) is removed. The built-in data
   model keeps reading source text that is a JSON value as that value, so
   a document on it can write `<data id="count" expr="0"/>`: its source
   text is `In(state)`, a data name, `name()` (ADR 0029), and a JSON
   value.

3. **Named functions are how charts are written.** The documentation
   (`README.md`, `docs/scxml.md`, and `statechart()`'s and
   `registerFunction()`'s comments in `index.d.ts`) teaches guards and
   actions as registered functions called by name, `when(named('full'))`
   and `script(named('audit'))`, with an inline function as the quick
   alternative. The reasons, in the order a reader meets them:
   - the chart can be written as SCXML or any other syntax and read back
     (ADR 0027), and another process that registers the same names
     defines the same revision from the text;
   - a typo in a name is reported when the chart is defined;
   - the same chart built twice is one definition, whatever code built it,
     while inline functions created anew each time register beside;
   - the functions live with the system, not in the chart, so they can be
     tested, reused and replaced by name.

4. **ADR 0027's round trip** within a process now holds for every chart:
   `define(d.export())` answers `d`, because the export hands back the
   same handles. "Without anonymous functions" remains the condition for
   a codec round trip and across processes.

## Consequences

- A value in a chart is the value the author passed, whatever its type.
- Builder charts are deduplicated by what they hold, not by the types of
  their values.
- A builder chart built by code that creates its functions anew (arrow
  functions written in the builder calls, run twice) is two definitions,
  as before; its structure changed under the same revision is
  `definition-mismatch` (core ADR 0087).
- Interning costs a table lookup per expression when a spec is converted,
  and a table entry per live record.

## Implementation notes

- **Where:** `src/napi_datamodel.{c,h}` (the records, `tna_expression_make`
  finding or adding one, `tna_expr_release` taking it out; the tables; the
  printer), `src/napi_system.c` (the printer passed to every encode; the
  tables freed in `tna_after_core`; `definition-mismatch` from the import
  path), `src/import.{c,h}` (`tna_encode_params.expressions`),
  `src/addon.c` and `src/runtime.{c,h}` (`setupLiteral(brand, text)`),
  `lib/statechart.js` (values as `literal()` again; `jsonExact` moved to
  the printer's renderer), `index.d.ts`, `test/handles.js` (new),
  `test/functions.js`, `test/definition.js`.
- **The tables.** A record is found again in one of two places, and
  neither keeps a value alive or needs JavaScript when a record goes:
  - an object (a function, a location, or a literal's object value) maps,
    in a per-system `WeakMap`, to an external naming a cell, which holds the
    object's record of each kind. The cell is `malloc` memory counted by
    the external and by each record in it, so the external's finalizer can
    run after the system is gone. A record holds its object strongly, so
    its entry cannot be collected while the record lives.
  - a primitive a literal holds is keyed in C, in an open-addressing table:
    a tag, then a number's bits (one NaN for all, `-0` apart from `0`), a
    string's UTF-16, or a bigint's sign and words. A record's key is freed
    with it, and it leaves the table then (backward-shift deletion).
    Records can be released while a system closes from a finalizer, where
    no JavaScript may run; this is why neither table is consulted from
    JavaScript on release.
  - A symbol held by a `literal()` is not shared: each is its own handle,
    so a chart with one registers beside. A lookup that fails for lack of
    memory makes an unshared record too; the chart works, core just never
    answers it by identity.
- **The primitive table is freed right after core's teardown**
  (`tna_after_core`), before the system counts what it leaked; freeing it
  when napi closes the data model came too late for that count.
- **A printer for encoding (not in the decision).** With values as
  `literal()`s again, a builder chart with plain data could no longer be
  written as SCXML (`a host expression and no printer`). napi now passes
  core an expression printer (`ta_chart_expression_printer`) for every
  encode: a `literal()` whose value JSON says exactly is written as that
  JSON (`<data id="count" expr="0"/>`), and a function, a location, or a
  value JSON cannot say (a `Date`, a class instance) is refused, which the
  codec reports as `unrepresentable`. The exactness rule is the one ADR
  0029's notes used for the builder (`jsonExact`), now used only to print.
  A JavaScript codec still gets `literal()` boxes in the spec it encodes.
- **Open question: text round trip in the same system.** Encoded text
  holds a copy of a literal's value as JSON source, which core hashes as
  source text, while the original holds a host expression. So defining the
  encoded text while the original is registered under the same explicit
  identity is another structure under that identity: `definition-mismatch`.
  Another system defines it under the identity. ADR 0029's test that
  encoded then defined a chart with `data('count', 0)` in the same system
  now does that with a chart whose values come from named functions
  (`dataExpression('count', named('zero'))`), which is all source text;
  `test/handles.js` asserts the mismatch for a literal-valued chart. If
  the same-system text round trip must hold for literal values too, core
  would need to treat a printable literal and its printed source as the
  same content; not decided here.
- **`definition-mismatch` from `define(text, { format })`.** The import
  path reported every result but out-of-memory as `invalid`; it now maps
  the result code, so a mismatch is `definition-mismatch` there too, as it
  is for `define(spec)`.
- **What `actor.data()` shows:** a `data()` value is still copied per actor
  where it was before (unchanged by this ADR); the definition, and its
  `export()`, hold the value itself, which is what the tests check.
- **Tests:** `test/handles.js`: the same functions and values answer one
  definition with a second hold; new closures register beside; another
  structure under the same revision is `definition-mismatch`; defining the
  export answers the definition, which holds the class instance and the
  plain object passed, and a value exports as its `literal()`;
  `literal(0)` and `0`, `NaN`, strings, bigints, `undefined` and `null`
  shared by value, `-0` apart from `0`, two objects apart, one object
  shared; a literal-valued chart's text is `definition-mismatch` in the
  same system and defines in another, and a `Date` is `unrepresentable`;
  a freed definition's function is collected (`FinalizationRegistry`,
  forced GC), and no system leaks. `test/functions.js`: values stay the
  values passed, a spec's JSON source text reads as its value, and a
  source-text chart round-trips through SCXML in the same system.
  `test/definition.js`: a builder chart's export answers the chart.
- **Benchmark:** not run. Interning costs a `WeakMap` lookup, or a hash of
  the primitive, per host expression when a spec is converted, that is
  per `define()`; no benchmark defines charts in its measured loop
  (`test/bench/tracing.js` defines its one or 100 charts before it
  measures).

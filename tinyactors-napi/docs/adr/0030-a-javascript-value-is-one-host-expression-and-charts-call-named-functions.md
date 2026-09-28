# ADR 0030: A JavaScript value is one host expression, and charts call named functions

**Status:** Accepted (2026-09-28); not implemented. Applies core ADR 0087. Supersedes the "plain data is JSON text" part of ADR 0029's implementation notes; keeps the JSON source-text form it added, for document text. Refines ADR 0011 (how builder expressions reach core) and ADR 0027 point 1 (what round-trips within a process).
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

- **Where:** `src/napi_datamodel.c` (`tna_expression_make` looks up or
  adds the record; release removes its entry), where the table lives (a
  weak map napi keeps per system, keyed as point 1 says), `lib/statechart.js`
  (values as `literal()` again; `jsonExact` and `jsonText` removed),
  `README.md`, `docs/scxml.md`, `index.d.ts`, ADR 0029's notes pointing
  here.
- **Tests:** the same builder chart built twice from the same functions
  and values answers one definition; built by code that makes new
  closures, it registers beside; `define(d.export())` answers `d` for a
  chart with inline functions, a class instance and a plain object as
  values, and the object in the definition is the object passed; `literal(0)`
  twice, `NaN`, and `-0` versus `0`; an interned record is released when
  the last definition using it is freed and its object can be collected
  (a `FinalizationRegistry` or `--expose-gc` check); a builder value is
  exported as the `literal()` it was, not as JSON text; a document's JSON
  source text still reads as its value; ADR 0029's named-function tests
  still pass.

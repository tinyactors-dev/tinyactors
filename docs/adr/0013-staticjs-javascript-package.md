# ADR 0013: staticjs's JavaScript package, and values from JSON and CBOR

**Status:** Accepted (2026-10-15). Adopts the proposal
[A JavaScript API for staticjs.wasm](../proposals/staticjs-js-api.md),
with the maintainer's answers to its decisions, and adds a request of its
own (R8).
**Date:** 2026-10-15

## Context

`staticjs/examples/wasm/staticjs.mjs` is a wrapper to copy: it reads the
module's struct layouts from `staticjs.api.json`, has no natives or
modules, and cannot be installed. JavaScript users need a package they
install and that is hard to misuse. The proposal sketched one, with seven
additions to staticjs's C API (R1 to R7) and six decisions for the
maintainer. The maintainer approved its direction on 2026-10-14 and
answered the decisions on 2026-10-15.

The maintainer also asked for a way to pass host values into staticjs
everywhere, not only from JavaScript: through handles (the value API), and
as JSON or CBOR bytes, with values written out the same ways. Hosts in C,
the package, and a future staticjs data model for tinyactors all have
values that arrive as bytes: an event's data is a CBOR neutral value
(`tinyactors-core/docs/protocol.md`, "Values"), and a web request's body is
JSON.

## Whom it serves first, and who pays

- **First: the application developer in JavaScript**, in Node and in
  browsers, who installs the package, evaluates programs, and passes values
  in and out.
- **Then: C embedders,** through R1 to R8: function values called, Dates
  and Temporal values made and read, iterators and regular expressions
  read, natives released, compile errors that keep their subject, frames
  that name their function, and values read from and written as JSON and
  CBOR.
- **Then: a staticjs data model for tinyactors,** whose `to_wire` and
  `from_wire` are R8's CBOR, checked by the conformance suite's value
  cases.
- **Who pays:** staticjs's library carries R1 to R8 (their code, tests and
  documentation); per evaluation, nothing unless a host calls them. The
  package costs its own module (the amalgamation and a C layer of the
  package's), its JavaScript, and a changelog. The maintainers keep a
  second wasm artifact, inside the npm package.
- **Out of scope:** a sandbox beyond what staticjs is; managing workers; a
  REPL; TypeScript types for programs; emnapi.

## Decisions

1. **Library first.** R1 to R8 land in staticjs, each with its tests and
   its documentation in `staticjs.h` and the embedding guide, before the
   package depends on them. The package ships no workaround that a
   request removes.

2. **Values from JSON and CBOR, and written as them (R8).** The value API
   stays the way to build values by handle. Beside it:

   ```c
   typedef enum {
       SJS_FORMAT_SOURCE = 0,  /* JavaScript source (the writer only) */
       SJS_FORMAT_JSON,        /* JSON text, as JSON.parse reads it and JSON.stringify writes it */
       SJS_FORMAT_CBOR         /* one CBOR neutral value */
   } sjs_format;

   typedef struct {
       sjs_format format;      /* SJS_FORMAT_JSON or SJS_FORMAT_CBOR; a zeroed struct is SJS_ERR_INVALID */
       sjs_bytes bytes;        /* the whole value; borrowed for the call */
       sjs_error *error;       /* optional: why and where the bytes were refused */
       sjs_buffer *subject;    /* optional: where the error's subject is copied, as sjs_compile does */
   } sjs_read_params;

   SJS_API sjs_result sjs_value_read(sjs_context *context, const sjs_read_params *params, sjs_value *out);
   ```

   `sjs_writer_config` gains `sjs_format format`, whose zero is today's
   JavaScript source, and an optional `sjs_error *error`, so the writer
   writes JSON and CBOR through the same buffer of any size.

   - **Reading** needs no evaluator, so a host can call it while a run is
     paused, and the package's conversions use it without running
     anything. That is why JSON is here, although `JSON.parse(text)` run
     as a program does the same: the program costs a compile, a copy of
     the text into a string, and the evaluator.
   - **JSON** reads as `JSON.parse` does in staticjs, from a reader shared
     with it that takes a context and an error instead of an evaluator,
     and writes as `JSON.stringify` does. A value `JSON.stringify` refuses
     or gives `undefined` for (a BigInt, `undefined`, a function, a
     callable `toJSON`) is refused.
   - **CBOR** is the neutral value of `tinyactors-core/docs/protocol.md`,
     "Values", read and written as the ECMAScript data model does. Read:
     an integer within ±(2^53 - 1) is a number and any other a BigInt;
     floats are numbers; a reference (tag 32) is its address; a byte
     string is refused, as staticjs has no bytes. Written: what
     `JSON.stringify` keeps, with numbers and BigInts exact, `undefined` at
     the top as `null`, and a BigInt outside -2^64 … 2^64 - 1 refused.
     Both ways, a value nested deeper than 64 is refused; a Date or a
     Temporal value counts as one level. CBOR has no indefinite lengths,
     so the writer counts the members it keeps when it opens a container.
   - **The writer's promise** to write a value that evaluates to an equal
     one is `SJS_FORMAT_SOURCE`'s. JSON and CBOR write what
     `JSON.stringify` keeps: a Date as its string, a Map as `{}`, `-0` as
     `0` in JSON. `function_text` is ignored in JSON and CBOR.
   - **Refusals.** Reading returns `SJS_ERR_PROGRAM`, whose meaning widens
     to "the program or the input is not staticjs", with `error` (when
     given) as for a program: `SJS_ERROR_SYNTAX` for bytes that are not a
     value of the format; `SJS_ERROR_UNSUPPORTED` for a value of the
     format that staticjs does not have (a `"__proto__"` key, a lone
     surrogate escape in JSON, bytes in CBOR); `SJS_ERROR_LIMIT` past a
     limit. `error->at.offset` is the byte offset; JSON gives the line and
     the column, CBOR line 1 and the column as the offset plus one. Bytes
     longer than `max_string_bytes` are `SJS_ERR_LIMIT` before anything is
     read, so one call's work is bounded. The text is checked to be UTF-8
     first. Writing refuses mid-stream, after bytes were handed out:
     `sjs_writer_write` returns `SJS_ERR_PROGRAM` with the writer's
     `error`, and the caller throws away what it wrote.
   - **The conformance suite's value cases**
     (`tinyactors-test/suites/protocol/values.txt`) run against staticjs's
     CBOR, with staticjs's own expressions where a case's ECMAScript uses
     what staticjs leaves out. Not applicable, each for its reason: the
     bytes cases (no bytes), "not ECMAScript" cases, cycles (values are
     immutable), lone surrogates (a staticjs string cannot hold one), and
     a method in an object literal (written as a property instead).

3. **The package ships its own module** (the proposal's decision 1): the
   amalgamation, the package's C layer and the two imports natives need.
   The released `staticjs.wasm` keeps importing nothing.

4. **Temporal out, where the runtime has no `Temporal`, is a `TypeError`**
   that names the type (decision 2). `toString()` still gives the
   writer's text, and the package reads it as JSON or CBOR on request.

5. **One evaluator per context, with C's rule** (decision 3): a `start`
   while a run is paused abandons it, and the abandoned run's next call
   throws.

6. **No emnapi** (decision 5).

7. **Every conversion is bounded, and nothing has to be configured for
   that** (decision 6, as the maintainer asked: an API that is hard to
   misuse). The bound is the context's own `max_value_items`, the limit
   staticjs already puts on a value; a context may give a smaller one. A
   conversion past it throws and releases what it made.

8. **Ready to publish.** The package is `@tinyactors/staticjs`, with what
   `npm publish` needs; a mise task builds, checks and packs it, and the
   maintainer publishes. Its changelog follows ADR 0002.

## Consequences

- staticjs's public API grows by R1 to R8; each is documented where its
  kind of call is.
- `staticjs/examples/wasm/` and `staticjs/docs/guide/wasm.md` give way to
  the package and its guide.
- The proposal is adopted; this ADR and the roadmap carry its decisions and
  steps. Where the proposal and this ADR differ (decision 7's default, R8),
  this ADR wins.

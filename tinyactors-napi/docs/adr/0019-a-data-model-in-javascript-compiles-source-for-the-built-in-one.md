# ADR 0019: A data model in JavaScript compiles source for the built-in one

**Status:** Proposed. Would refine ADR 0011 (the JavaScript data model) and ADR 0013 point 6 (specs, and turning on `ta_definition_import`). Research: [`docs/research/pluggable-datamodels.md`](../../../docs/research/pluggable-datamodels.md).
**Date:** 2026-09-25

## Context

napi has one data model, named "javascript" (ADR 0011). Its expressions
are JavaScript functions, `{ get, set }` locations and literals. Its only
source text is `In(state)` and a bare data name. So napi cannot run an
SCXML document whose expressions are ECMAScript source, such as
`<transition cond="count > 5">` in `datamodel="ecmascript"`, and
`ta_definition_import` (core ADR 0053) stays switched off. Existing SCXML
content, including the W3C IRP suite, cannot be loaded; the test host runs
it on its own ECMAScript data model, built on quickjs-ng.

The user asked to explore exposing core's data-model vtable
(`ta_datamodel_vtable`) through Node-API, so that users can write a data
model in JavaScript.

What the vtable asks of an adapter (`tinyactors-core/include/tinyactors.h`):

- **Lifecycle:** `prepare_definition` over the definition's expression
  table, with located rejections (ADR 0058); `destroy_definition`;
  instance size, alignment, `init_instance` with an optional binding;
  `destroy_instance`.
- **Data:** `declare` (with `bound` for invoke parameters), `set_named`,
  `read_location`, `assign`.
- **Evaluation:** `eval_condition`, `eval_value`, `execute_script`.
- **Executable content:** `make_params`, `foreach_snapshot` (SCXML takes a
  shallow copy) and `foreach_bind`.
- **Conversions:** `value_text` and `text_value` (wire JSON), and
  `import_content` (literal `<content>` bytes, ADR 0053).
- **Inspection:** `inspect`.
- **Contract:** each operation answers `TA_OP_OK`, `TA_OP_ERROR` with a
  fault, or `TA_OP_NOMEM`, which promises "ran out of memory and changed
  nothing" (ADR 0070). A run may call an operation again after it stops
  for lack of memory.
- **Host queries:** `In()`, the current event, the actor, and system
  variable text (ADR 0068).

napi's `src/napi_datamodel.c` already implements all of this over
Node-API: frames per call, values as reference-counted boxes (ADR 0011),
the reentrancy guard (ADR 0013), and system-variable failures that fail
the call (`text_failed`). Most of that work is not about expressions at
all.

**Measured** (research note): one call from C into JavaScript costs 89 to
96 ns per expression, on Node 22 and Bun. A vtable-shaped call with a
params object adds 2 to 5 ns. Compiled SCXML source that reads data as
variables through `with (this)` adds 27 to 58 ns per call, and through a
`Proxy` 115 to 226 ns.

## Decision (proposed)

**Users do not implement the vtable. They register a compiler for source
text, and napi's built-in data model runs what it returns.** The vtable
stays internal.

### 1. The JavaScript interface

```js
system.registerDatamodel('ecmascript', {
  // Called once per source expression when a chart is defined.
  compile(source, role, info) {
    // role: 'condition' | 'value' | 'location' | 'script'
    // info: { declared: string[] }  (the document's <data> IDs)
    // Returns what a builder chart would have passed for that role:
    //   condition, value, script: function (context) { ... } with `this` = the data
    //   location: a data name (string), or { get(context), set(value, context) }
    // Throws to refuse; an error with `offset` and `length` points into `source`.
  },
});

const definition = system.define(xmlText, { format: 'scxml' });
```

- **What `context` is:** the same object builder functions get today:
  `event`, `sessionID`, `name`, `ioProcessors` and `In(id)`, as ADR 0011
  and ADR 0068 define them. A compiler maps SCXML's `_event`,
  `_sessionid`, `_name` and `_ioprocessors` onto it, for example through
  a `with` scope.
- **Everything else stays native:**
  - storage (the data root);
  - `declare`, `set_named` and binding;
  - `foreach` snapshots;
  - `<param>` objects;
  - `<content>` import and wire JSON;
  - value boxes;
  - `In()`;
  - system-variable failures;
  - the reentrancy guard.

  A compiler writes no storage code and handles no values.
- **Names:** `registerDatamodel` takes the SCXML `datamodel` name. The
  names `javascript` and `null` are napi's own and cannot be registered.
  A registration belongs to one system, like `registerIO`, and must come
  before the first `define` that uses it.
- **Errors:** a `compile` that throws refuses the chart. `define` throws
  `TinyactorsError` with core's compile error (ADR 0058), naming the
  element and, when the thrown error has `offset` and `length`, the span
  in the source.

### 2. The trampoline

`compile` is called at define time, inside `prepare_definition`, once per
source expression. At run time there is no new trampoline. The compiled
functions run through the same call the built-in model already makes
(about 95 ns per expression), plus the compiler's own scoping (about
30 to 60 ns with `with`). That is acceptable for imported documents.
Builder charts do not change and pay nothing new.

### 3. Reentrancy, lifetime and retries

- **The same rules as builder functions.** Compiled functions run inside
  `ta_actor_resume`. They may read and change data, and `post` (ADR
  0006). Any call into core throws `reentrant` (ADR 0013).
- **`compile` runs inside `define`.** It is synchronous. It must not call
  `define` or anything else on the system.
- **Lifetime:** compiled functions are retained with the definition, and
  released when it is freed (ADR 0012). The compiler is released when the
  system closes.
- **Retries (ADR 0070):**
  - a condition or a value may be evaluated more than once when a unit
    stops for lack of memory, so compiled conditions and values must have
    no side effects;
  - scripts follow the built-in model's rules;
  - a user's code never promises "changed nothing": when memory runs out,
    the native model decides what to report (ADR 0070 open point 1).

### 4. How it serves import

- **napi turns on `ta_definition_import`,** linking the SCXML codec
  (`tinyactors-scxml`, on `tinyxml`) into the addon.
- **The adapter is picked by name.** `define(bytes, { format: 'scxml' })`
  chooses it by the document's `datamodel` attribute: `null` gives core's
  null data model, and any other name gives a registered one. Doing that
  needs one of:
  - **(a) A core change:** `ta_definition_import` takes the data models by
    name (a small list or a resolver), and picks one after decoding. This
    keeps located compile errors (ADR 0058).
  - **(b) Two steps in napi:** decode with the codec, read
    `spec.datamodel`, then `ta_definition_compile_spec`. This needs no
    core change, but loses the mapping from compile errors back to
    positions in the document.

  This ADR proposes (a).
- **An ECMAScript example in `tools/`,** not `lib/`.
  `tools/ecmascript-datamodel.js` compiles with `new Function` and a
  `with` scope, and maps `_event` and the other system variables onto
  `context`.
- **The W3C IRP suite then runs against the addon,** from
  `tinyactors-test/vendor/scxml-irp`, with an expected-failures file like
  the test host's.
- **Security:** `lib/` ships no `eval` and no `new Function`. Running
  expression text is the implementer's choice, with the process's
  rights. The docs say so, and point to a sandbox for untrusted
  documents: for example an interpreter compiled to WebAssembly. `node:vm`
  is not a security boundary.

### 5. The built-in data model is the only implementation

A builder chart is the case where `compile` is not needed: its
expressions are functions already. An imported chart becomes functions
through `compile`. So napi keeps one native data model and one set of
semantics. The built-in model gains a name per registration (its vtable
`name` is copied per registered model) and a call to `compile` in
`prepare_definition` for source expressions it doesn't recognise.

### 6. Other hosts

The host-kernel proposal lists the data model as something each host
writes. The compile hook is the natural shape for each of them:

- **The browser build** shares `lib/`, so it gets `registerDatamodel`
  unchanged.
- **A Python host** would take `compile(source, role) -> callable` over
  its native data model.

A full vtable in each host language would be a larger surface in every
host, with the same semantics to repeat each time.

## Alternatives

- **A. Expose the whole vtable to JavaScript.**
  `registerDatamodel(name, { createInstance, declare, setNamed,
  evalCondition, evalValue, readLocation, assign, executeScript,
  makeParams, foreachSnapshot, foreachBind, valueText, textValue,
  importContent, inspect })`, with values crossing as JavaScript values.
  - **For:**
    - it's the most general, and could host a data model whose storage
      isn't JavaScript objects, such as XPath over an XML document;
    - it matches Qt's `QScxmlDataModel` and uSCXML's plugins.
  - **Against:**
    - about 16 operations for a user to get right, including SCXML's
      foreach copy, parameter building, content import and wire JSON;
    - ADR 0070's "changed nothing" promise, which user code can't keep
      honestly;
    - an `assign` becomes two crossings (evaluate, then assign), each
      with value boxes;
    - it duplicates the native model's tested machinery in every user
      data model, and gives each host a large per-language surface.

    No data model with non-JavaScript storage has been asked for.
- **B. This ADR:** a compiler over the built-in model.
- **C. Both:** B now, and A later behind its own ADR if a data model with
  non-JavaScript storage is ever needed. B does not rule it out.
- **D. No import.** Document that ECMAScript charts go through the
  builder. Existing SCXML content and the IRP suite stay out of reach.

## Consequences

- SCXML documents with ECMAScript, or any language a user can compile to
  JavaScript functions, become loadable in napi.
- The public API grows by `registerDatamodel` and a `format` option on
  `define`. The vtable stays internal.
- The addon grows by the SCXML codec and `tinyxml`, to be measured.
- `ta_definition_import` needs the data models by name (option 4a), a
  core change with its own core ADR.

## Testing (when implemented)

- **A compiler test:** every role, compile errors with offsets, a
  refused chart, and `compile` called once per source expression.
- **Imported and builder charts behave the same:** an imported chart and
  the equivalent builder chart produce equal traces and data, on Node
  and Bun.
- **The IRP suite** runs against the addon with the `tools/` ECMAScript
  data model, with an expected-failures file.
- **Reentrancy:** a compiled function that calls core throws `reentrant`,
  and one that posts works.

## Questions for the user

1. **B over A?** A compiler over the built-in data model, with the vtable
   itself kept internal.
2. **Registration per system, or per process?** This ADR proposes per
   system, like `registerIO`.
3. **Picking the data model by name:** a core change to
   `ta_definition_import` (4a, keeps located errors), or two steps in
   napi (4b, no core change)?
4. **The IRP suite against the addon:** should it join `mise run test`
   once it passes, as the test host's does?
5. **The addon's size:** is linking the SCXML codec and `tinyxml` into
   the addon acceptable, or should import be an optional build?

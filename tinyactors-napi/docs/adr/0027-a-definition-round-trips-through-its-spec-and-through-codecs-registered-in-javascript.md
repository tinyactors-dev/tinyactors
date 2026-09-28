# ADR 0027: A definition round-trips through its spec, and through codecs registered in JavaScript

**Status:** Accepted (2026-09-28). Implemented (2026-09-28): points 1 to 5, see the implementation notes for where the code differs from the text. Applies core ADR 0042 (export), ADR 0053 (codecs), ADR 0071 (lookups by ID), ADR 0077 (identity) and ADR 0085 (spans) to the addon. Refines ADR 0019 (`define()` options, data models by name) and ADR 0024 (which charts are walked for inline children). Refined by ADR 0029: a function called by name is source text, so "a chart without functions" in point 1 means a chart without anonymous functions. Within a process, `define(d.export())` answers `d` for every chart since ADR 0030. Refined by ADR 0031: `encode(format, { datamodel })`, and host expressions printed by the data model a chart is written for.
**Date:** 2026-09-28

## Context

Consumers of the addon failed to render chart definitions. Core can
describe a definition completely: `ta_definition_export()` rebuilds the
spec it was compiled from, faithfully (core ADR 0042), with its identity,
spans and sources (core ADR 0085); `ta_definition_encode()` writes it in a
codec's syntax with a source map; and the lookups of core ADR 0071 answer
every ID a trace record or an inspection names. The addon calls all of
them, but only privately, for its debug adapter and its trace encoder.
Its public `Definition` answers `inspect()` (counts, of which only
`stateCount` is about structure), `sourceOf(kind, id)`, and nothing else.

What core offers and what napi exposes:

| Core API | Used internally by | Public in napi? |
|---|---|---|
| `ta_definition_export` → `DefinitionSpec` | `lib/debug.js:1350` | No. `index.d.ts` names "the shape core's definition export returns", but nothing returns it. |
| `ta_definition_encode` → SCXML text and source map | `lib/debug/sources.js` | No |
| `ta_definition_state` / `find_state` (id, parent, kind) | `lib/debug/labels.js`, `system.js` | No |
| `ta_definition_transition` (source, targets, event, guard, origin) | `labels.js` | No |
| `ta_definition_action` / `expression` / `content` / `invoke` | `labels.js`, `system.js` | Only the list of `nested` child definitions, built from the invokes |
| `transition_count`, `action_count`, `expression_count`, `content_count`, `invoke_count` | — | Not in `DefinitionInspection` |

Exposing the export is not enough on its own. A probe of today's private
export (`core.definition_export`, 2026-09-28) found where the round trip
`define(export(d))` falls short in napi, though core's export is
faithful:

- **An imported SCXML chart** round-trips: defining its export answers the
  same definition, because core fingerprints the whole chart and
  registration is idempotent (core ADR 0077).
- **A builder chart** registers a second definition under the same
  identity. That is core ADR 0077's exemption, not a defect: a function is
  retained, not hashed, so core cannot tell two builder charts apart.
  Round-tripping a builder chart can only mean an equal export, not the
  same definition. `index.d.ts`'s `findDefinition` comment claimed
  otherwise; this ADR corrects it.
- **A chart on a data model registered with `registerDatamodel()`**
  cannot be defined from its spec: `define(spec)` compiles every data
  model except `"null"` as `"javascript"` (ADR 0019), which refuses the
  chart's source text.
- **An imported chart with inline children** loses its nesting when
  defined from its spec: `define(spec)` does not walk `<invoke>`s
  (ADR 0024), so each child is compiled when its invoke runs, not with the
  parent.

Separately, SCXML is the only syntax napi reads, and it is hard-wired:
`define(text, { format: "scxml" })` calls `ta_scxml_codec` and refuses any
other format. Core's answer to "another syntax" is a `ta_chart_codec`
(core ADR 0053), and napi does not let JavaScript supply one.

## Decision

### 1. Round-tripping is the contract

A definition's meaning is its spec: its identity, data model, binding,
states, transitions, executable content, data, literal content, invokes
and spans (with the sources they name). `Definition.export()` returns it
in the shape `define()` takes, and these hold:

- **Spec round trip.** `system.define(d.export())` answers a definition
  whose `export()` is structurally equal to `d.export()`: functions and
  `literal()` values compare by identity, bytes by content, and a nested
  builder child by its `DefinitionID` (a system-local name, so this holds
  within one system). For a chart without anonymous functions (every
  imported chart, and a builder chart whose functions are all named,
  ADR 0029), it answers `d` itself (core ADR 0077).
- **Codec round trip.** For a codec that decodes and encodes,
  `define(e.bytes, { format, identity: e.identity })`, where
  `e = d.encode(format)`, answers a definition with `d`'s revision. It
  answers `d` itself when the chart has no anonymous functions. The codec's
  faithfulness is its own contract; the built-in SCXML codec is tested
  for it (see the implementation notes).
- **Lookups agree with the export.** Every ID a trace record, an
  inspection or `sourceOf()` names resolves through the lookups of point
  3, and what a lookup answers is what the export says about that
  element.

Two per-definition settings are the host's, not the chart's, and are not
in the spec: `microstepLimit` (an operational bound, core ADR 0075; core
has no getter for it) and the `resolve` function kept for children
compiled at invoke time (ADR 0024). A host passes both again to
`define()`.

### 2. `define(spec)` accepts everything the export can say

- **`datamodel` is any data model name.** `"javascript"` when absent,
  `"null"`, or a name registered with `registerDatamodel()`, chosen as an
  import chooses it (core ADR 0076). ADR 0019's "a registered data model
  is only for imported SCXML" is withdrawn.
- **A spec is walked for inline children** as an imported document is
  (ADR 0024 point 1): each SCXML `<invoke>` with a literal `<content>`,
  or a literal `src` when `define()` is given `resolve`, is decoded with
  the SCXML codec, compiled and nested. A builder chart's child is
  already a definition (a `DefinitionID` as content) and is not walked,
  as before.
- **`define(spec, options)` takes `resolve`,** as `define(text, options)`
  does.

### 3. Definitions answer core's lookups by ID

`Definition` gains `state(id)`, `findState(name)`, `transition(id)`,
`action(id)`, `expression(id)`, `content(id)` and `invoke(index)`: plain
objects read from core on each call, as `inspect()` is (ADR 0002). An ID
out of range answers `undefined`; a freed definition throws `not-found`.
`DefinitionInspection` gains `transitionCount`, `actionCount`,
`expressionCount`, `contentCount` and `invokeCount`, so a walk knows where
to stop.

An expression answers in the export's own `ExpressionSpec` form: its
source text, its function, or its `literal()` value.

The debug adapter's `lib/debug/labels.js` and `lib/system.js` move onto
these methods, so the addon has one way to read an element.

### 4. Chart syntaxes are codecs, and a codec can be written in JavaScript

```ts
system.registerCodec({
  name: "application/vnd.example.chart+yaml",
  extensions: ["chart.yaml"],
  decode(input, { name, resolve }) { return spec; },
  encode(spec, { definition }) { return { bytes, spans }; },
});
system.define(text, { format: "chart.yaml", name: "file:///charts/order.chart.yaml" });
definition.encode("chart.yaml");
```

- **A registered codec is a core codec.** napi wraps it in a
  `ta_chart_codec` whose `decode` and `encode` call the JavaScript
  functions synchronously, while `define()` or `encode()` runs, as a
  registered data model's `compile` runs (ADR 0019). `define(text)` and
  `encode()` go through `ta_definition_import()` and
  `ta_definition_encode()` for every codec, so one path serves SCXML and
  every other syntax. The flags come from which of `decode` and `encode`
  the object has.
- **`decode(input, context)`** gets the bytes (a string given to
  `define()` arrives as its UTF-8) and returns a `DefinitionSpec`. It
  fills `spans` and `sources` when it knows where it read each element,
  naming `context.name` as the first source (core ADR 0085). It fetches
  `src` references with `context.resolve`, the function given to
  `define()`.
- **`encode(spec, context)`** gets the definition's export and returns its
  bytes (or a string, written as UTF-8), and optionally `spans`: where it
  wrote each element, parallel to `spec.spans`, which napi turns into
  core's source map. `context.definition` is the definition, so a codec
  can export a nested child it wants to write inline. A function has no
  source form a codec can be trusted with: a codec that meets one throws
  `unrepresentable`, as the SCXML codec does, or writes a form that does
  not round-trip and documents it.
- **A codec refuses by throwing a `ChartCodecError`,** whose `code` is
  core's chart error code in lowercase and with dashes (`syntax`,
  `not-chart`, `unknown-construct`, ..., `unrepresentable`), with an
  optional place (line, column, offset) and the construct and attribute it
  is about. `define()` throws `invalid` with an `ImportRefusal` as
  `detail`, which now names the codec (`format`), the code, the construct
  and the attribute, and keeps its place. Any other exception from a
  codec propagates as it is and refuses the call.
- **`format` names a codec** by its `name` or by one of its `extensions`.
  The built-in SCXML codec is registered in every system as
  `application/scxml+xml` with the extension `scxml`, so
  `format: "scxml"` keeps working. Registering a name or an extension that
  is taken throws `invalid`; the built-in codec cannot be replaced.
  `system.codecs()` lists what is registered, as `ChartCodecInfo`.
- **`define()` takes `identity`** for any format: no chart syntax carries
  an identity (core ADR 0053), so a round trip that must keep an explicit
  one passes it beside the bytes. Without it, the SCXML codec keeps its
  rule (`<scxml name>` is the family, the revision a fingerprint).
- **`Definition.encode(format)`** returns
  `{ bytes, identity?, placeOf(kind, id) }`: the bytes, the explicit
  identity when the definition has one, and where the codec wrote each
  element, answered as `sourceOf()` answers it.
- **Children compiled when an `<invoke>` runs** are SCXML whatever codec
  defined the parent: the invoke's type chooses the syntax, not the
  parent's codec.

### 5. `index.d.ts` tells users how

The "Definitions" section of `index.d.ts` says to render a chart from
`export()`, to resolve trace IDs with the lookups, and to read or write a
syntax other than SCXML by registering a codec written in JavaScript. It
states the round-trip contract of point 1 on `export()`.

## Consequences

- Rendering a chart, resolving a trace's IDs, and converting a chart
  between syntaxes need no private binding, and the debug adapter uses the
  same public calls as everyone else.
- One definition-reading path: the private `core.definition_*` calls in
  `lib/debug/` and `lib/system.js` go away behind the public methods.
- A JavaScript codec pays a crossing per `define()` and per `encode()`,
  not per element: the spec crosses once, through the generated layer
  that already converts `define(spec)`.
- A spec on a registered data model now compiles, so the same data model
  serves imports and specs.
- `define(spec)` walks inline children, so a spec with many SCXML children
  is slower to define and reports a broken child when it is defined, as an
  import does (ADR 0024).
- A builder chart still registers beside itself (core ADR 0077). That is
  by design, and the contract says so rather than promising what core
  cannot tell.

## Implementation notes

- **Where:** `lib/definition.js` (`export`, `encode`, the lookups, the
  counts), `lib/system.js` (`registerCodec`, `codecs`, `define()`'s
  `format`, `identity` and `resolve`, the data model by name, walking a
  spec's children), `lib/errors.js` (`ChartCodecError`), a new
  `src/codecs.{c,h}` (the codec registry and the `ta_chart_codec` adapter
  over JavaScript), `src/import.{c,h}` (import and encode through a
  registered codec), `src/napi_children.c` (the walk, shared by both
  `define()` forms), `lib/debug/labels.js` and `lib/debug/sources.js`
  (onto the public calls).
- **Tests:** export then define, compared structurally, for every chart
  napi's test suites define, builder and imported; the W3C IRP charts
  encoded with the SCXML codec and defined again answer the same
  definition; a spec on a registered data model; a spec with inline SCXML
  children nests them; a JavaScript codec (a small JSON syntax in the
  tests) decodes with spans, encodes with a source map, refuses with a
  place, and round-trips; a codec name or extension registered twice; the
  lookups agree with the export for every ID up to each count.

### Points 1 and 3 (2026-09-28)

- **Where:** `lib/definition.js` holds one reader per lookup
  (`readState`, `findState`, `readTransition`, `readAction`,
  `readExpression`, `readContent`, `readInvoke`, `exportSpec`), in the
  public shapes. The `Definition` methods call them, and so do
  `lib/debug/labels.js`, `lib/debug.js` (the JSON source view) and
  `lib/system.js` (state names in inspection, the mailbox and metrics, a
  send's literal target, operator transitions). No other file reads a
  definition's elements through the private `core.definition_*` calls.
  `lib/debug/sources.js` still reads counts with `definition_inspect`.
- **Out of range and freed.** Core answers `not-found` for both. A reader
  that gets it asks `definition_inspect`, which throws `not-found` only
  when the definition is freed; otherwise the answer is `undefined`. An ID
  that is not an integer in range answers `undefined` without asking.
- **`expression(id)` can also answer a `{ get, set }` location,** the
  host value an assignment's location was built with. `index.d.ts` types
  it `ExpressionSpec | LocationSpec`; the text above says only
  `ExpressionSpec`.
- **Source text carries its span.** Found while testing: defining an
  imported chart's export answered the same definition but dropped its
  expression spans, because the export wrote each expression as a bare
  string and a registration that brings other spans replaces them (core
  ADR 0077). `src/runtime_codecs.c` now writes source text that has a span
  as `{ source, span }`, and reads that form back; `ExpressionSpec` gains
  it. A lookup (`expression(id)`) still answers the text, since the copies
  a definition keeps carry no span (core ADR 0085).
- **The export is core's, as it is.** Members core leaves empty read
  `undefined`, `binding` reads `"default"` for early binding, a literal()
  is the chart's own `LiteralValue`, a location the chart's own object.
  `define()` takes all of it. No normalization was needed for the round
  trip.
- **Tests:** `test/definition.js`: an imported chart's export answers the
  same definition and exports equal, sources and expression spans
  included; a builder chart's export registers beside it with an equal
  export, its functions, literal() values and locations the chart's own;
  every lookup agrees with the export up to each count (states by
  document order and parent, each `<transition>` with its source, event,
  targets and condition, each action at the place its block names, the
  nested children); out-of-range IDs answer `undefined`; a freed
  definition throws `not-found` from every lookup and the export. Not
  done: the round trip over every chart the other suites define.

### Point 2 (2026-09-28)

- **Where:** `src/napi_datamodel.{c,h}` (`tna_js_datamodel_select`: the
  compile's data model by name, from the same choices an import takes; it
  replaces the `select_javascript` flag), `src/napi_system.c`
  (`selectDatamodel` takes any of those names; `tna_adopt`, shared by
  `importDefinition` and the new `adoptDefinition`, nests the inline
  children with `tna_children_nest` and keeps `resolve`),
  `lib/system.js` (`define(spec)` selects `spec.datamodel`, takes
  `resolve`, adopts the registered definition, and throws a refusal as an
  import does).
- **Order.** `define(spec)` compiles and registers, then walks, as an
  import does: a spec that core answers with a live definition (core ADR
  0077) finds its children nested already and nests nothing.
- **A refusal is an import's.** A broken child refuses the spec with
  `invalid` and the `ImportRefusal` detail an import gives (`stage`,
  `message`), and the parent is retired, which frees what it nested.
  A name no data model has throws `invalid` before anything compiles.
- **Keeping `resolve` can fail for lack of memory.** Before, an import
  that could not keep it threw `out-of-memory` and left the definition
  registered with no `Definition` to retire it; both forms now retire it
  first.
- **The selection is cleared after every spec compile,** refused or not,
  so a spec refused before core reads it cannot pass its data model on to
  the next compile.
- **Tests:** `test/definition.js`: an imported chart on a registered data
  model defines again from its export in its own system (the same
  definition) and in another that registers the name (an equal export,
  and its source expressions run); a spec naming an unregistered data
  model is refused; another system defining a parent's export nests its
  inline child, and its `src` child only when given `resolve`; a spec
  with a broken inline child is refused and leaves no definition behind.

### Points 4 and 5 (2026-09-28)

- **Where:** `lib/codecs.js` (the registry, `ChartCodecError`, and the
  functions napi's adapter calls), `lib/system.js` (`registerCodec`,
  `codecs`, `define()`'s `format` and `identity`, the refusal's new
  fields, and the system side of `Definition.encode()`),
  `lib/definition.js` (`encode`, `EncodedChart`), `src/napi_codecs.{c,h}`
  (the `ta_chart_codec` adapter over a JavaScript codec, and the chart
  error names), `src/import.{c,h}` (one import and one encode for any
  codec: `tna_import`, `tna_encode`; `tna_import_scxml` stays for the
  children compiled at invoke time), `src/napi_system.c`
  (`importDefinition` and `encodeDefinition` take a codec and an
  identity), `mise/lib/napi/emit.rb` (the generated layer exports its
  spec and identity codecs: `tna_gen_decode_spec`, `tna_gen_encode_spec`,
  `tna_gen_decode_identity`, `tna_gen_encode_identity`),
  `lib/debug/sources.js` (the virtual SCXML source is
  `Definition.encode('scxml')` and `placeOf`).
- **The registry lives in lib/, not in C.** The text put it in
  `src/codecs.{c,h}`. A codec's functions are needed only during one
  define() or encode(), so lib/ passes the chosen one to the native
  method, which wraps it as a core codec for that call alone. C keeps no
  references and has no registry to tear down.
- **A codec's refusal crosses as data.** lib/ wraps `decode` and `encode`
  in functions that answer `{ spec }`, `{ bytes, spans }` or `{ refused }`
  and let every other exception propagate, so the C adapter reads plain
  objects and never inspects an exception. A thrown `RangeError` reaches
  the caller of define() or encode() as it was.
- **The spec lives in the call's arena, not the decode's.** The adapter
  converts the codec's spec under a `tna_call` that spans the whole
  import, so what the generated layer retains (functions, interned names)
  and allocates lives until core has compiled it; the call ends after.
- **An encode's refusal** is `invalid` with `detail` { message, format,
  code, construct?, attribute?, line?, column?, offset? }, and no
  `stage`. The message is formatted as a decode's: "no in <state>".
- **`ChartCodecError` owns its place.** On Bun, JavaScriptCore gives every
  `Error` a `line` and `column` of where it was thrown; the constructor
  always sets its own, `undefined` when the codec gave none, so a
  placeless refusal is not placed at the throw.
- **A decode refusal without a line is unlocated.** Core says a decode
  error always has a position; a JavaScript codec may give none, and a
  zero line then leaves `line`, `column` and `offset` out of the detail.
- **Formats share one name space.** A codec's name and its extensions are
  all names `format` may give, so a name that is another codec's
  extension is taken too.
- **Tests:** `test/codecs.js`: a JSON chart syntax decodes with spans
  naming define()'s `name`, encodes with a source map (`placeOf`),
  round-trips to the same definition, and carries an explicit identity;
  refusals with and without a place, with a construct and an attribute;
  a spec the generated layer refuses; core's compile error from a codec's
  spec; exceptions propagate from decode and encode; a codec calling back
  into the system is `reentrant`; names and extensions registered twice;
  the built-in codec cannot be replaced; decode-only and encode-only
  codecs; SCXML by extension and by name, with identity; a builder chart
  with a function is `unrepresentable`; a freed definition is
  `not-found`. `test/irp/run.js` encodes every W3C IRP chart as SCXML and
  defines it again: all 176 answer the same definition, on Node and Bun.

# ADR 0027: A definition round-trips through its spec, and through codecs registered in JavaScript

**Status:** Accepted (2026-09-28); not implemented. The target API is in `index.d.ts`, marked `// status: unimplemented` or `// status: partial`. Applies core ADR 0042 (export), ADR 0053 (codecs), ADR 0071 (lookups by ID), ADR 0077 (identity) and ADR 0085 (spans) to the addon. Refines ADR 0019 (`define()` options, data models by name) and ADR 0024 (which charts are walked for inline children). Refined by ADR 0029: a function called by name is source text, so "a chart without functions" in point 1 means a chart without anonymous functions.
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

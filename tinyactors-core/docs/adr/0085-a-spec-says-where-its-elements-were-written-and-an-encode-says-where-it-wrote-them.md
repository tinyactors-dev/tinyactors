# ADR 0085: A spec says where its elements were written, and an encode says where it wrote them

**Status:** Accepted and implemented (2026-09-26). Extends ADR 0053 (the codec interfaces carry spans) and ADR 0058 (positions beyond the error path); decides what ADR 0071 section 4 left out (source locations). Applies ADR 0018, ADR 0038, ADR 0042, ADR 0060 and ADR 0070; builds on ADR 0077 (a registration answered by identity hands over newer spans, section 3). See the implementation notes.
**Date:** 2026-09-26

## Context

A debugger speaks in places. A line breakpoint names a line of a file,
a stack frame shows where execution stands, `gotoTargets` asks which
states are at a line, and "jump to cursor" moves an actor to the state
under the cursor (`docs/proposals/debug-adapter.md`). Core speaks in
compiled IDs: a state, a transition, an action, an expression (ADR 0071).
The debug adapter needs a map from each ID to a place in a source, and it
builds the reverse, line to ID, from that map.

Core has positions only on the error path. ADR 0058 turns a compile
error's subject into a line and column inside `ta_definition_import()`,
by pointer arithmetic against the input. A chart that compiles keeps no
position at all, and ADR 0071 section 4 left source locations out as "a
separate decision, worth making only if jumping from a trace to a line in
a document is wanted". It is wanted now.

Three front ends build specs, and each knows a different kind of place:

- **The SCXML codec** (`tinyactors-scxml`, ADR 0053) reads bytes. txml
  knows every node's start and end (`txml_node_span`) and every
  attribute's name and value (`txml_attribute_span_read`), as offsets,
  lines and columns. A document may pull in other files through the
  `ta_chart_src_resolver` (`<script src>`).
- **The Ruby chart DSL** (`tinyactors-test`) has no bytes. It knows the
  file and line of each DSL call, from `caller_locations`.
- **napi's builder** (`tinyactors-napi/lib/statechart.js`) has no bytes
  either. It can know the file, line and column of each builder call from
  a V8 stack trace.

In production the chart's file is often not on the machine the debugger
attaches to. The adapter then shows the chart as text it gets from
`ta_definition_encode()`, and needs to know where in that text each ID
landed.

What must hold:

- **Spans never change a revision** (ADR 0036, ADR 0038). A session image
  restores only into its exact revision. If reformatting a file, or moving
  a chart down a line, made a new revision, a host could not restore its
  own sessions after a whitespace change.
- **Per definition, not per actor** (ADR 0018). A definition's spans cost
  its own storage once; an actor pays nothing.
- **A spec built without spans costs nothing and compiles as before.** The
  C compound-literal form, every existing test chart and every existing
  host keep working unchanged.
- **Export stays faithful** (ADR 0042): compiling an export gives the same
  revision, and now also the same spans.
- **No recursion in core** (ADR 0060), and the three-parameter rule.

## Decision

### 1. A span

```c
typedef struct {
    uint64_t offset;      /* the first byte, from the start of the source */
    uint32_t length;      /* bytes; 0 when the byte range is unknown */
    uint32_t source;      /* an index into ta_scxml_spec.sources */
    uint32_t line;        /* from 1; 0: the span is unknown */
    uint32_t column;      /* from 1, in bytes */
    uint32_t end_line;    /* one past the last byte, as txml's spans end */
    uint32_t end_column;
} ta_source_span TA_ZERO_IS_ABSENT;  /* 32 bytes */
```

- **Known when `line` is nonzero.** An all-zero span is unknown, so a spec
  built without spans, or a front end that knows nothing about one
  element, needs no flag. A front end that knows only lines (the Ruby DSL)
  sets `source` and `line` and leaves the rest zero.
- **Offset and length** are what a codec knows exactly and what an editor
  needs to select an element. The offset is 64-bit, as in
  `ta_chart_position`; the length is 32-bit, because no element is longer
  than the text core itself handles (`ta_str` and `ta_bytes` lengths are
  32-bit).
- **Lines and columns as well as offsets**, because the Ruby DSL and
  napi's builder have no offsets, and because DAP speaks lines. Columns
  count bytes from 1, as `ta_chart_position` and txml do. DAP's default
  column unit is UTF-16 code units; the adapter converts, since it has
  the text whenever it shows the source (for ASCII the two agree).
- **The end is kept.** DAP's `StackFrame`, `Breakpoint` and `GotoTarget`
  all carry `endLine` and `endColumn`, and the states at a line
  (`gotoTargets`) are the states whose span contains the line, which needs
  the end. A host could compute the end from offset and length, but only
  with the text in hand; an adapter that tells a client "line 12 of
  /srv/charts/order.scxml" often has no copy. Keeping the end costs 8 of
  the 32 bytes.

### 2. A spec holds a table of spans; elements index it

`ta_scxml_spec` gains the table and the sources it points into:

```c
typedef struct { ta_str name; } ta_scxml_source;

typedef struct {
    ...
    ta_expression script;
    uint32_t script_span;   /* the <script> element; script.span is its text */
    TA_COUNTED_BY(span_count) TA_OPTIONAL const ta_source_span *spans;
    uint32_t span_count;
    TA_COUNTED_BY(source_count) TA_OPTIONAL const ta_scxml_source *sources;
    uint32_t source_count;
} ta_scxml_spec;
```

and every element that becomes a compiled ID gains `uint32_t span`: an
index into `spans` counting from 1, 0 when unknown. Those are
`ta_scxml_state_spec`, `ta_scxml_transition_spec`,
`ta_scxml_initial_spec` (an initial transition is a transition ID),
`ta_scxml_action_spec`, and `ta_expression`. The root `<script>` is an
action with no action node of its own, so `ta_scxml_spec.script_span` is
its span.

**Why an index and not a span in each element:**

- **`ta_expression`.** It is a spec member in a dozen places, and it is
  also what a definition stores for each expression and hands to the data
  model (`prepare_definition()`, `ta_definition_expression()`). A 32-byte
  span inside it would cost every definition 32 bytes per expression
  whether its spec had spans or not, which breaks "costs nothing". A
  32-bit index fits in the padding after `kind` on 64-bit targets, so
  `ta_expression` stays 32 bytes there (on wasm32 it grows from 16 to 20).
  A definition's own copies carry 0.
- **Encode.** A codec that reports where it wrote an element needs a name
  for the element that the caller can map back to a compiled ID. The
  index is that name (section 6).
- **Elements written in one place can share one entry**, as a history
  state and its default transition do in the Ruby DSL.

The cost is that authoring is two-level: a front end appends to a table
and stores the index. The two front ends that fill spans (the DSL and
napi) do it in one helper each.

**Which elements.** Only those a compiled ID names, because only those
can be asked about (section 4). `<invoke>`, `<data>`, `<param>` and
`<donedata>` have no public ID and get no span; adding one later is adding
a member.

**Sources.** `sources[0]` is the input a codec decoded, then each file its
resolver fetched; for the DSL, the Ruby files the chart was written in. A
spec that lists no sources has one unnamed source, 0. The name is the
front end's: a path, a URI, or empty. `ta_chart_decode_params` and
`ta_definition_import_params` gain `ta_str name`, what the input is
called, which becomes `sources[0]`: a codec has no other way to learn it.

### 3. Compile copies known spans into a side table, and hashes none

`ta_definition_compile_spec()`:

- **Checks each span when it meets its element**, before the element's
  content: an index beyond `span_count`, or a known span whose `source` is
  not in `sources` (other than 0 when there are none), is
  `TA_COMPILE_ERROR_MALFORMED_SPEC`, with a path that ends in the new step
  `TA_SPEC_MEMBER_SPAN` (`states[0].states[1].span`). This adds checks to
  the compile order of ADR 0058 section 2 without reordering any: each
  comes first within its element. An unknown span is not checked further.
- **Hashes nothing about spans.** Neither the table, the indexes, nor the
  source names enter the fingerprint. Where an element was written is not
  what it means.
- **Keeps a side table only when the spec knew at least one span.** It
  records `(kind, ID, index)` for each known span during the walk, and
  after the walk, when every count is final, lays out one dense table in
  one allocation the definition owns, not its arena, and frees with it: a
  span per element, by kind and then by ID (the
  states, the transitions with the initial ones after the `<transition>`
  elements as ADR 0071 numbers them, the actions, the expressions),
  unknown ones zero. It copies the source names beside it. A definition
  compiled from a spec without known spans has no table and costs exactly
  what it did before.

The table is a property of the definition: every actor of it shares it,
and a session image does not mention it (ADR 0038 identifies a definition
by revision, which spans do not change).

**Registering the chart again hands over where it is written now.** ADR
0077 answers a registration with a live definition of the same identity,
and since spans are not in the fingerprint, a chart moved down a line is
answered by the definition compiled before the move. Kept as it was, that
definition would put every line breakpoint of a hot reload on the old
lines, for its actors and for the new ones alike, although the file on
disk now says otherwise. So:

- **a registration answered by identity that brings spans** gives them to
  the live definition, when they differ from what it has: the two tables
  are swapped, and the old one is freed with the new compile, which ADR
  0077 destroys. Nothing is allocated, so the reuse still cannot fail;
- **one that brings no spans**, or the same ones, changes nothing, and
  what a caller borrowed of the table (an export's `spans`, a
  `ta_source_location`'s `source`) stays valid;
- **what a caller borrowed** of a table that is handed over is freed with
  it. A host that keeps a line index per definition rebuilds it after it
  registers a chart again, which it knows it did. The debug adapters do so
  (their notes).

The alternative, spans in the fingerprint, would make a moved chart a new
revision, which section 1's first requirement rules out; registering it
beside the old one would break ADR 0077's reuse for every chart with
spans.

### 4. Asking where an element is

```c
typedef enum {
    TA_ELEMENT_STATE = 0,
    TA_ELEMENT_TRANSITION,
    TA_ELEMENT_ACTION,
    TA_ELEMENT_EXPRESSION
} ta_element_kind;

typedef struct { ta_source_span span; ta_str source; } ta_source_location;

typedef struct {
    ta_definition_id definition;
    ta_element_kind kind;
    uint32_t id;
    TA_OUT TA_LIFETIME(definition) ta_source_location *out;
} ta_definition_span_params;
TA_API ta_result ta_definition_span(const ta_system *system,
                                    TA_IN const ta_definition_span_params *params);
```

`TA_ERR_NOT_FOUND` when the definition is unknown or stale, the ID is out
of range, the definition keeps no spans, or it keeps none for this
element. It is one lookup per ID in the style of ADR 0071, and a host
builds its line-to-ID index by walking IDs 0, 1, 2, ... up to the counts
in `ta_definition_inspection`, once per definition. `source` is the
name, so a host needs no second call.

### 5. Export carries the spans back

A definition with a side table exports it: `spans` is the table itself,
borrowed like the rest of the export, every exported element whose span
is known has its place in it plus one as `span`, and `sources` names what
it points into. An element with no known span keeps `span` 0, so an
exported expression without one is still plain text over the test host's
RPC.
Compiling the export gives the same revision and the same spans, and
exporting that again gives the same table, so export is still a fixed
point. A definition without spans exports none, byte for byte as before.

### 6. Encode says where it wrote each element

`ta_chart_encode_params` gains a sink:

```c
typedef struct ta_chart_span_sink_vtable {
    void (*written)(void *self, uint32_t span, TA_IN const ta_source_span *where);
} ta_chart_span_sink_vtable TA_VTABLE;
typedef struct { void *self; TA_OPTIONAL const ta_chart_span_sink_vtable *v; } ta_chart_span_sink TA_ADAPTER;
```

For each element of the spec whose `span` is nonzero, a codec given a
sink calls `written` once with that index and the element's place in its
output (source 0). For an expression the place is its text: an
attribute's value, or an element's character data.

`ta_definition_encode_params` gains
`TA_OUT TA_OPTIONAL TA_LIFETIME(arena) ta_source_map *spans`: when asked,
core exports the definition with every element numbered by kind and ID,
even when the definition keeps no spans (the export's table is then all
zero), passes the codec a sink that files each report at that number, and
hands back the table as a `ta_source_map`, one array per kind indexed by
ID. An element the codec wrote nothing for has an all-zero span. Not
asked, core passes no sink and the codec does no work for spans.

Recording returns nothing: core's sink writes into a table it allocated
before the encode, and a sink that fails would leave the codec with an
error it could do nothing about.

Other shapes considered:

- **The codec writes the output spans into the spec.** The spec is
  `const`, and a spec made of compound literals is read-only memory.
- **The codec reports the element's address.** Core would need a map from
  addresses to IDs, a sort of every exported element, to file each
  report; the index is that map already.
- **A table the codec fills, indexed by span.** It puts an array and its
  size into the codec's contract; the sink leaves the storage to the
  caller.

### 7. What each front end records

- **The SCXML codec.** An element's span is its node, start tag through
  end tag. An `<elseif>` is the `<if>` it lowers to, so it gets the
  `<elseif>` element's span. An `initial` attribute is an initial
  transition, so it gets the attribute's span. An expression gets its
  attribute's value as written, between the quotes, so `x &gt; 1` is five
  bytes longer in the source than in the spec. A `<script>` body gets its
  text, inside any CDATA markers. A `<script src>` gets all of the fetched
  file, which is a source of its own named by `src`. A `namelist`'s
  locations get none: they are names, not authored expressions. Encode
  reports where it wrote each of these.
- **The Ruby DSL.** Every state, transition, initial transition and action
  gets the file and line of the DSL call that wrote it, and the chart's
  sources are those files. Expressions get no span of their own: they are
  arguments of the call that wrote their element, on its line.
- **napi's builder.** Every state, transition, initial transition and
  action gets the file, line and column of the builder call that wrote
  it, from a V8 call site, and the chart's sources are those files.
  Expressions are JavaScript functions, host expressions whose place the
  host can learn from the function itself, and get no span of their own.
  `Definition.sourceOf(kind, id)` reads a span back.

## Consequences

- A debugger can show where every state, transition, action and
  expression is, in the file it was written in or in the text
  `ta_definition_encode()` makes, and set line breakpoints and goto
  targets from the reverse index. A host needs no other source map.
- A chart moved down a line, reformatted, or renamed keeps its revision;
  its session images still restore. A test asserts this in core and in
  the codec.
- A definition compiled with spans costs 32 bytes per element (states,
  transitions, actions and expressions) plus its source names, in one
  allocation counted in its `definition_bytes`; one compiled without costs
  nothing more. Nothing is per actor.
- A hot reload that only moves lines keeps the definition (ADR 0077) and
  moves its spans; its actors keep running.
- The spec grows by a `span` in five element types and three members at
  the root. Building a spec without them is unchanged.
- ADR 0058's compile order gains span checks, one per element, each
  before the element's content. A chart with a bad span and another fault
  reports whichever comes first in that order.
- `ta_definition_import()` could now also place a compile error at the
  nearest element with a span when its subject is not a slice of the
  input. It does not yet; that is ADR 0058 section 5's rule to change, and
  it has not been needed.

## Implementation notes

Recorded when this ADR was implemented (2026-09-26).

- **Cost.** `ta_source_span` is 32 bytes. The side table is one
  allocation of exactly 32 bytes per element, the source records and the
  names' bytes, from the definition's allocator, freed by
  `ta_definition_destroy()` and counted in `definition_bytes`. The
  compiler's records are one growable vector in the compile allocator,
  freed with the compiler. `span_test` compiles a chart with spans under an
  allocator that fails at every allocation in turn: each attempt returns
  `TA_ERR_NOMEM` and gives back everything it took. `span_test` also
  registers the chart without spans, with them, moved down five lines and
  with its source unnamed, in one system: every registration is answered by
  the first definition, which then reports the latest spans, and a
  registration without spans keeps them (`ta_definition_take_spans()`).
  Nothing is per actor: `actor_fixed_bytes` does not change.
- **`ta_expression` did not grow** on x86-64: `span` sits in the padding
  after `kind`, and the type is 32 bytes as before. On wasm32 it grows
  from 16 to 20 bytes, and so does each entry of every definition's
  expression table there, spans or not: 4 bytes per expression per
  definition. The spec's elements grow too (on wasm32 an action spec from
  128 to 156 bytes, a state spec from 44 to 48), but a spec is the
  front end's and lives only until it is compiled.
- **The SCXML codec now indents what it encodes.** Before, the output was
  one line after the declaration, which would put every element on line 2.
  It writes a line break and two spaces per open element before each SCXML
  element and before the end tag of one with element children, as text,
  and nothing inside a `<content>`, `<script>`, `<data>` or `<assign>`
  body. Decoding ignores whitespace between SCXML elements, so the IRP
  documents and the codec's round trips keep their revisions and re-encode
  byte for byte. A test decodes the encoding of a chart and finds every
  element exactly where encode reported it: offsets, lines and columns
  agree.
- **Import has a name for its input.** The test host's SCXML invoker and
  the IRP harness pass none; their definitions' source 0 is unnamed.
- **The Ruby DSL** takes the first caller outside `chart.rb`, so a helper
  in a scenario file that calls the DSL is where its elements were
  written. It knows lines only. A branch of a `conditional` is an `<if>`,
  written where the branch is, and a history state's default transition
  is written with the history state.
- **napi's capture costs 5 to 7 µs per element** at definition time and
  nothing afterwards. Building a chart of 2000 states, each with a
  transition and an action (6000 elements), took 170 to 180 ns per element
  on Node 22 without spans and 5.4 to 5.7 µs with them; on Bun 1.3, 470 to
  600 ns without and about 7 µs with. The cost is V8 building call sites
  (`Error.captureStackTrace` with a structured `prepareStackTrace` and
  `stackTraceLimit` 6), so a chart of a thousand elements takes a few
  milliseconds longer to define. There is no switch to turn it off; one can
  be added if a host defines charts on a hot path.
- **Engines place a call differently.** A column is where the engine says
  the call is: V8 names the method's name, JavaScriptCore the dot before
  it. JavaScriptCore also places a call inside a chain that spans lines at
  an earlier line of the chain, so napi's test writes one call per
  statement. Columns count UTF-16 code units, as JavaScript does; for
  ASCII sources they are bytes.
- **`docs/internal/scxml.md` is the W3C recommendation**, rendered, and
  has no place for this; the codec's spans are documented in
  `tinyactors-scxml/README.md`.
- **The test host's RPC** carries spans as members like any other: `span`
  on elements, `spans`, `sources` and `script_span` at the root. An
  expression with a span crosses as `{"source": text, "span": n}`, text
  alone as before otherwise (`docs/protocol.md`). The RPC generator used
  to refuse a tagged union that is absent when zero and has other members;
  `ta_scxml_action_spec` now has `span`, so it accepts them, and an absent
  value writes none of them.
- **napi** binds `ta_definition_span()` through the generator;
  `ta_chart_span_sink` joins the codec adapters napi skips, as no codec is
  registered there yet. `lib/definition.js` wraps it as `sourceOf()`, and
  `index.d.ts` types the spec's span members.

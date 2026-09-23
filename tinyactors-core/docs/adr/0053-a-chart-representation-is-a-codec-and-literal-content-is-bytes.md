# ADR 0053: A chart representation is a codec, and literal content reaches adapters as bytes

**Status:** Accepted; amends ADR 0025, applies ADR 0040 and ADR 0046; §4 amended 2026-09-23 (with ADR 0058)
**Date:** 2026-09-22

## Context

### What core already has

`ta_scxml_spec` is a complete, representation-neutral description of a chart,
and `ta_tree` exists to build one dynamically: "so import code can report one
result at its outer boundary". `ta_definition_compile_spec()` turns a spec
into a definition and `ta_definition_export()` turns one back (ADR 0042).
Three front ends in this repository already use that surface — the Ruby chart
DSL and its JSON mirror of `ta_scxml_spec`, the C compound-literal form in the
tests, and `tinyactors-test/ruby/scxml_irp/document.rb`, a 751-line SCXML
reader over REXML. None of them needs anything from core that core does not
already have.

So the interface for getting a chart into the runtime exists. What does not
exist is a name for the thing that turns **bytes** into that spec and back.
Every byte representation — SCXML XML, a JSON encoding, a purpose-built
grammar — reinvents the boundary, and no tool can ask "what can you read?" of
anything.

### What is missing

ADR 0025 said an importer/exporter "is a separate library/tool" and that core
ships without XML. That has held, and this ADR does not disturb it: no parser
enters core. What it adds is the *shape* such a library implements, so that
codecs, tools and hosts agree, and so core can later grow functions expressed
over it without inventing a second vocabulary.

### The defect found while designing it

Writing the codec's contract for `<content>` exposed a live defect in
delivery, independent of codecs.

Literal `<content>` reaches core's three `import_content()` call sites —
`src/interpreter.c:222` (the generic `TA_VALUE_SOURCE_CONTENT` arm, serving
`<data>`, `<assign>` and `<send>`), `src/interpreter.c:1467` (`<donedata>`),
and `src/invoke.c:366` (`<invoke>`) — and is converted to a `ta_value` before
any adapter sees it. The two shipped data models parse the bytes as JSON and,
on failure, fall back to a whitespace-normalized string
(`tinyactors-test/src/json_datamodel.c:658`,
`tinyactors-test/src/ecmascript_datamodel.c:1234`); the null profile refuses
outright (`tinyactors-test/src/null_datamodel.c:227`).

That is correct for a data model and wrong for everyone else:

- SCXML 6.4.2 says that for invoke types other than the SCXML, CCXML and
  VoiceXML URIs, "the interpretation of `<param>` and `<content>` elements and
  the 'src' and 'namelist' attributes is platform-specific". The
  specification's own `<invoke type="x-clock" src="clock.pl">` example invokes
  a process; a `<content>` payload for such a service is bytes, and whitespace
  normalization corrupts anything indentation-sensitive or binary. Under the
  null data model it never arrives at all.
- SCXML G.6's worked example is a `<send>` to the Basic HTTP Event I/O
  Processor whose `<content>` becomes the message body, and which the
  recipient must see namespace-fixed-up: bytes on a wire, not an ECMAScript
  value.
- SCXML 6.4.2 requires services of type `http://www.w3.org/TR/scxml/` to
  interpret `<content>` "as markup to be executed" — a chart, which the
  invoker must decode.

In each case the consumer is a host adapter selected by an attribute
(`type`), not by the element, so the distinction is unavailable at the call
site and unavailable to a codec. It exists only at delivery.

`docs/internal/datamodel-literals-and-content.md` anticipated this. It
proposes `ta_literal { kind, media_type, bytes, frontend }` under a heading
"Why this is not `import_content`", motivated by supporting values "that did
not originate from an expression, without making XML part of the ABI", and
lists "future non-XML importers". What shipped is that design collapsed to
`ta_bytes` under the name the note rejected. This ADR implements the part the
defect requires and records why the rest is not needed.

## Decision

### 1. A chart representation is a `ta_chart_codec`

Core defines the adapter and the error vocabulary, and links no parser.

**Core calls a codec only from the two functions in §4.** Nothing else in
core decodes or encodes, and nothing in core links a parser. The vtable is in
the public header because those functions take it, and because codecs, tools
and hosts should share one shape.

```c
/* ========================================================================
 * Chart codecs
 *
 * A codec turns bytes into a ta_scxml_spec and back. Core defines the
 * adapter and the in-memory form and links no parser: SCXML XML, a JSON
 * encoding and a tree-sitter grammar are all codecs, and none is in core.
 *
 * Core calls a codec only from ta_definition_import() and
 * ta_definition_encode(). An embedder may also decode and then compile with
 * ta_definition_compile_spec() itself.
 */

typedef struct { uint64_t offset; uint32_t line; uint32_t column; } ta_chart_position;

typedef enum {
    TA_CHART_ERROR_NONE = 0,
    TA_CHART_ERROR_SYNTAX,                /* not well-formed in this codec's syntax */
    TA_CHART_ERROR_NOT_CHART,             /* well-formed, but not a chart */
    TA_CHART_ERROR_UNKNOWN_CONSTRUCT,
    TA_CHART_ERROR_MISPLACED_CONSTRUCT,   /* <else> before an <elseif>; <param> with <content> */
    TA_CHART_ERROR_UNKNOWN_ATTRIBUTE,
    TA_CHART_ERROR_MISSING_ATTRIBUTE,
    TA_CHART_ERROR_CONFLICTING_ATTRIBUTE, /* both `event` and `eventexpr` */
    TA_CHART_ERROR_BAD_VALUE,             /* a duration, a boolean, an ID list */
    TA_CHART_ERROR_FOREIGN_CONTENT,       /* SCXML 4.10 content, no lowering (ADR 0046) */
    TA_CHART_ERROR_SRC_UNRESOLVED,        /* a src= and no resolver */
    TA_CHART_ERROR_LIMIT,                 /* a bound this codec enforces was reached */
    TA_CHART_ERROR_UNREPRESENTABLE        /* encode: the spec has no form in this syntax */
} ta_chart_error_code TA_SYMBOLS("TA_CHART_ERROR_");

typedef struct {
    ta_chart_error_code code;
    ta_chart_position at;
    ta_str construct;  /* the element or production the error is about */
    ta_str attribute;  /* empty when it is not about one */
    ta_str message;    /* a fixed phrase owned by the codec */
} ta_chart_error;

enum TA_FLAG_SET(ta_chart_codec_flags, "TA_CHART_CODEC_") {
    TA_CHART_CODEC_DECODES = 1u << 0,
    TA_CHART_CODEC_ENCODES = 1u << 1
};

/*
 * What a codec is, for a host choosing one and a tool reporting what it did.
 * Every field is fixed for the codec's lifetime.
 */
typedef struct {
    /* The representation this codec reads and writes: the registered media
     * type where one exists ("application/scxml+xml"), otherwise a stable
     * identifier of the implementer's choosing. It is also what a decode
     * writes into a literal content's media_type. */
    ta_str name;
    /* Conventional file extensions without the dot, terminated by an empty
     * one; absent when the representation has none. */
    TA_TERMINATED_BY(ptr) TA_OPTIONAL const ta_str *extensions;
    /* One line, for a tool's --help. */
    ta_str description;
    uint32_t flags TA_FLAGS(ta_chart_codec_flags);
} ta_chart_codec_info;

/*
 * Resolves <data src>, <script src> and <invoke src> during a decode: the one
 * route to the outside world, and the embedder's, never core's and never the
 * codec's. For <script> the bytes are source text.
 */
typedef struct ta_chart_src_resolver_vtable {
    ta_result (*fetch)(void *self, ta_str src, TA_INOUT ta_arena *arena, TA_OUT ta_bytes *out);
} ta_chart_src_resolver_vtable TA_VTABLE;
typedef struct { void *self; TA_OPTIONAL const ta_chart_src_resolver_vtable *v; } ta_chart_src_resolver TA_ADAPTER;

/*
 * Lowers SCXML 4.10 platform-specific executable content to a <send> to a
 * registered I/O processor, the only form core has for it (ADR 0046).
 * Returning TA_ERR_NOT_FOUND refuses the document, which SCXML A.2 permits.
 * An absent adapter refuses every such element: skipping one would silently
 * drop behaviour the author wrote.
 */
typedef struct ta_chart_lowering_vtable {
    ta_result (*lower)(void *self,
                       TA_IN const ta_chart_position *at,
                       ta_bytes element,
                       TA_INOUT ta_arena *arena,
                       TA_OUT ta_scxml_send_spec *out);
} ta_chart_lowering_vtable TA_VTABLE;
typedef struct { void *self; TA_OPTIONAL const ta_chart_lowering_vtable *v; } ta_chart_lowering TA_ADAPTER;

/*
 * Renders TA_EXPRESSION_HOST back to source during an encode. A spec decoded
 * from a document carries only TA_EXPRESSION_SOURCE and needs none; a spec
 * built from host handles is TA_CHART_ERROR_UNREPRESENTABLE without it.
 */
typedef struct ta_chart_expression_printer_vtable {
    ta_result (*print)(void *self, TA_IN const ta_expression_host *host,
                       TA_INOUT ta_arena *arena, TA_OUT ta_str *out);
} ta_chart_expression_printer_vtable TA_VTABLE;
typedef struct { void *self; TA_OPTIONAL const ta_chart_expression_printer_vtable *v; } ta_chart_expression_printer TA_ADAPTER;

typedef struct {
    ta_bytes input;
    /*
     * The operation's arena. Everything the decode allocates lives here — the
     * codec's own parse, the spec, and the transients in between — and
     * nothing outlives it. Reset or destroy it once the spec is compiled.
     */
    TA_ARENA ta_arena *arena;
    ta_chart_src_resolver resolver;
    ta_chart_lowering lowering;
    /* No chart syntax has a form for a definition's identity, so it is
     * supplied here or not at all; absent means the compiler fingerprints. */
    TA_IN TA_OPTIONAL const ta_definition_identity *identity;
    TA_VIEW_OUT TA_LIFETIME(arena) const ta_scxml_spec **out;
    TA_OUT TA_OPTIONAL TA_VALID_ON(TA_ERR_INVALID) ta_chart_error *error;
} ta_chart_decode_params;

typedef struct {
    TA_IN const ta_scxml_spec *spec;
    /* The operation's arena: the output bytes and everything the encode
     * needed to produce them. */
    TA_ARENA ta_arena *arena;
    ta_chart_expression_printer expressions;
    TA_OUT TA_LIFETIME(arena) ta_bytes *out;
    TA_OUT TA_OPTIONAL TA_VALID_ON(TA_ERR_INVALID) ta_chart_error *error;
} ta_chart_encode_params;

typedef struct ta_chart_codec_vtable {
    ta_chart_codec_info info;
    /*
     * Decode `input` into a spec.
     *
     * **The spec may point into `input`.** A codec is not required to copy,
     * because a codec over a syntax tree keeps expression text as exact
     * slices of the source, which is what lets an embedder map a compiled
     * expression back to the construct it came from and run static checks on
     * it before compiling. So `input` must stay valid as long as the spec
     * does — until ta_definition_compile_spec() has copied it. Callers keep
     * the buffer; codecs that do copy make that harmless.
     *
     * A codec bounds its own input: nesting depth, document size, expansion.
     * Reaching one of its bounds is TA_CHART_ERROR_LIMIT. The interface sets
     * no bound of its own, so two codecs need not accept the same documents.
     *
     * A literal <content> is decoded to bytes plus this codec's `info.name`
     * as their media type. What those bytes are is the codec's rule: an XML
     * codec serializes the content subtree with namespaces resolved as SCXML
     * G.6 requires, rather than slicing the source, because what a recipient
     * must see is a re-serialization and not the author's bytes.
     *
     * TA_OK, TA_ERR_INVALID with `error` saying why, or TA_ERR_NOMEM. Null
     * when this codec only writes.
     */
    TA_OPTIONAL ta_result (*decode)(void *self, TA_IN const ta_chart_decode_params *params);
    /*
     * Write `spec` in this codec's syntax. `spec->identity` is not written:
     * identity has no chart syntax, so a round trip that must preserve an
     * explicit one carries it beside the document and feeds it back through
     * ta_chart_decode_params. A literal content whose media type is not this
     * codec's own is TA_CHART_ERROR_UNREPRESENTABLE unless the codec can
     * carry it verbatim. Null when this codec only reads.
     */
    TA_OPTIONAL ta_result (*encode)(void *self, TA_IN const ta_chart_encode_params *params);
} ta_chart_codec_vtable TA_VTABLE;

typedef struct { void *self; const ta_chart_codec_vtable *v; } ta_chart_codec TA_ADAPTER;
```

One supporting addition, because a decode is handed an arena while `ta_tree`
takes an allocator:

```c
/* An allocator that draws from `arena` and whose free() is a no-op, so an
 * arena can back anything that wants a ta_allocator. */
TA_API ta_allocator ta_arena_allocator(TA_INOUT ta_arena *arena);
```

### 2. Literal content reaches host adapters as bytes

A literal `<content>` carries its bytes and what they are:

```c
TA_WHEN(TA_SCXML_VALUE_CONTENT) struct {
    /* What these bytes are, as the codec that produced them knows it; the
     * decoding codec's info.name. Empty when nothing said. Opaque to core. */
    ta_str media_type;
    ta_bytes bytes;
} content;
```

`ta_content_def` gains the same field, the compiler hashes it into the
revision — omitting it would let two charts differing only in a media type
share a fingerprint, the flaw `TA_EXPRESSION_HOST` already has at
`src/builder.c:127` — and `ta_export_content()` emits it so a decoded chart
round-trips.

The two host-adapter requests carry it through:

```c
typedef struct { ta_system *system; ta_actor_id parent; ta_invoke_id invoke_id;
                 ta_atom type; ta_value src; ta_value data;
                 ta_str content_type; ta_bytes content;
                 ta_value params; bool autoforward; } ta_invoke_request;

typedef struct { ta_actor_ref source; ta_send_id send_id; ta_event_name_id event;
                 ta_atom io_type; ta_value target; ta_value data;
                 ta_str content_type; ta_bytes content; } ta_send;
```

`data` remains the evaluated `<content expr>`; `content` is the literal form.
SCXML 5.6.2 already makes those mutually exclusive, and the compiled IR
already distinguishes them as `TA_VALUE_SOURCE_EXPR` and
`TA_VALUE_SOURCE_CONTENT`. A delayed send stores the `ta_content_id`, not the
bytes, so a session image reconstructs it against its exact revision
(ADR 0038).

**Who imports.** `src/interpreter.c:501` already forks on whether the send's
type is the built-in SCXML Event I/O Processor, and that fork is the rule:

- **The SCXML Event I/O Processor** (core-owned, ADR 0040) keeps importing.
  `_event.data` in the receiving session must be a data-model value, and that
  semantics is core's.
- **Every `ta_io` processor** receives `content` and `content_type`, and core
  does not import. Interpretation is platform-specific (SCXML 6.4.2), the
  processor knows what it wants, and the media type tells it what it has.
- **Every invoker** receives `content` and `content_type`, and core does not
  import. Core owns no invoker, so it has no semantics to impose. This is
  what makes `<invoke type="x-clock">` with a payload work at all, and what
  removes the null data model's refusal from that path.

### 3. What does not change

`<data>`, `<assign>` and `<donedata>` keep `import_content()` exactly as it
is. Their only consumer is a data model — a named binding, a location, or a
chart's `_event.data` — so raw bytes would have nowhere to go, and SCXML
5.3.2, 5.4.2 and 5.5.2 all define them in data-model terms.

The `kind` and `frontend` fields of the `ta_literal` sketch are not adopted.
`kind`'s TEXT-versus-BYTES distinction is already carried by which spec field
holds the literal — `definition.h:212` notes that a literal `src` is also
`TA_VALUE_SOURCE_CONTENT`, and it goes to `text_value()` rather than
`import_content()` — and `frontend` is subsumed by the media type plus the
consuming `type` atom. `docs/internal/datamodel-literals-and-content.md` is
rewritten to record that, so the note and the code stop disagreeing.

### 4. Import and export over a codec

Core has two functions over a codec. Each one wraps a sequence that every
embedder would otherwise write by hand, and gets a detail right that
hand-written code tends to get wrong.

#### `ta_definition_import`: bytes to a registered definition

```c
typedef enum {
    TA_IMPORT_STAGE_NONE = 0,
    TA_IMPORT_STAGE_DECODE,
    TA_IMPORT_STAGE_COMPILE
} ta_import_stage TA_SYMBOLS("TA_IMPORT_STAGE_");

typedef struct {
    ta_import_stage stage;
    union {
        TA_WHEN(TA_IMPORT_STAGE_DECODE) ta_chart_error decode;
        TA_WHEN(TA_IMPORT_STAGE_COMPILE) ta_compile_error compile; /* ADR 0058 */
    } as;
    /* Where in `input` the error is. A decode error always has one. A compile
     * error has one when its subject, or the nearest element above it, is
     * text sliced from `input` (ADR 0058 §5). */
    bool located;
    ta_chart_position at;
} ta_import_error TA_TAGGED(stage, as);

typedef struct {
    ta_chart_codec codec;
    ta_bytes input;
    /* The operation's arena: the decode, its transients, the compile error's
     * path and message. Core does not reset it, because `error` borrows from
     * it and from `input`. Reset it once the error has been read. */
    TA_ARENA ta_arena *arena;
    ta_chart_src_resolver resolver;
    ta_chart_lowering lowering;
    TA_IN TA_OPTIONAL const ta_definition_identity *identity;
    ta_datamodel datamodel;
    ta_allocator allocator;
    TA_OUT ta_definition_id *out;
    TA_OUT TA_OPTIONAL TA_VALID_ON(TA_ERR_INVALID) ta_import_error *error;
} ta_definition_import_params;

TA_API ta_result ta_definition_import(ta_system *system,
                                      TA_IN const ta_definition_import_params *params);
```

It decodes, compiles and registers, and returns the new definition's ID. What
it takes care of:

- **The lifetime of `input`.** The spec may point into `input` until the
  compile has copied it (§1). Since import runs decode and compile in one
  call, that rule never reaches the caller. The caller only has to keep
  `input` until it has read `error`.
- **Ownership on failure.** `ta_system_register_definition()` takes the
  definition only on `TA_OK`, so if registration fails, import destroys the
  definition itself. The caller never holds an unregistered definition.
- **Codecs that only write.** If `decode` is null, import returns
  `TA_ERR_UNSUPPORTED` before touching `input`.
- **One error, with a position.** A decode error comes from the codec. A
  compile error comes from ADR 0058, and import gives it a position in
  `input`. Import reports at most one error, as each stage does.

An invoker for `http://www.w3.org/TR/scxml/` is the case §2 creates. It is
handed `request->content` and `request->content_type`, picks the codec whose
`info.name` equals that media type, and calls `ta_definition_import()`.
Choosing the codec stays the host's job.

The result is a registered ID, not a `ta_definition *`. A caller that needs
the unregistered object, for example to register it under a lock of its own,
calls the codec's `decode` and then `ta_definition_compile_spec()` itself.
Those remain public.

#### `ta_definition_encode`: a registered definition to bytes

```c
typedef struct {
    ta_definition_id definition;
    ta_chart_codec codec;
    /* Holds the exported spec, the output and the codec's transients. */
    TA_ARENA ta_arena *arena;
    ta_chart_expression_printer expressions;
    TA_OUT TA_LIFETIME(arena) ta_bytes *out;
    /* The identity the document cannot carry, or null when the definition was
     * fingerprinted. Feed it back as import's `identity` to round-trip. */
    TA_OUT TA_OPTIONAL TA_VIEW_OUT const ta_definition_identity **identity;
    TA_OUT TA_OPTIONAL TA_VALID_ON(TA_ERR_INVALID) ta_chart_error *error;
} ta_definition_encode_params;

TA_API ta_result ta_definition_encode(const ta_system *system,
                                      TA_IN const ta_definition_encode_params *params);
```

This is `ta_definition_export()` followed by the codec's `encode`, with two
additions. It returns `TA_ERR_UNSUPPORTED` when `encode` is null. And it hands
over the identity that `encode` does not write, so a caller does not have to
dig it out of the exported spec. It works on a retired definition until that
definition is freed, as export does (ADR 0043). `TA_ERR_NOT_FOUND` when no
definition has the ID.

Import and export round-trip: import the output of `ta_definition_encode()`
with the returned identity, and the result has the same revision. This follows
from ADR 0042's faithful export and from a rule every codec that both reads
and writes must follow: decoding what it encoded gives back an equal spec. A test asserts it for every codec in
the tree.

#### What core does not add

- **A codec registry or lookup by name or extension.** A codec is a value.
  `info.name` and `info.extensions` are enough for a host to build its own
  table.
- **Transcoding, bytes in one codec to bytes in another.** It is import
  followed by encode. And a literal content that is not in the target codec's
  own media type fails with `TA_CHART_ERROR_UNREPRESENTABLE`, so a core
  function would promise a conversion that does not exist.
- **A `ta_chart_decode()` wrapper.** It would only forward to `v->decode`.

#### Sequencing

Both functions land together with the first real codec and its tests, not
before. That codec is the XML codec that replaces `document.rb`. The
interface should be tested against one real codec before it gains callers.

## Consequences

- No parser enters core, so ADR 0025's substance survives; what moves in is a
  vtable, an error vocabulary and the two functions over them in §4.
- A codec is a value, not a registration. A host holding several passes the
  one it wants. Nothing in core resolves a codec by name, and §4 decides that
  nothing will.
- A file with a semantic error, such as an unknown target or a duplicate ID,
  reports a line and column through `ta_definition_import()`, as a syntax
  error does. This depends on ADR 0058 and on the codec slicing its input
  instead of copying it.
- `<invoke type="x-clock">` with a payload works, under every data model
  including the null profile, and an HTTP processor can set a `Content-Type`
  it was told rather than one it guessed.
- A literal content's bytes now affect a definition's revision, so two charts
  that differ only in a payload no longer share a fingerprint.
- The test host's SCXML invoker moves from `tat_value_encode(&request->data)`
  to `request->content`, and gains a case for an inline chart whose media type
  is not its own.
- Cross-representation conversion of a literal content is explicitly not
  solved: a codec that cannot carry another's bytes says
  `TA_CHART_ERROR_UNREPRESENTABLE` rather than emitting a slab of foreign
  syntax. An XML `<content>` holding namespaced markup has no faithful form in
  a syntax without namespaces, and that is a property of SCXML G.6, not of
  this interface.
- `document.rb` becomes replaceable: an XML codec in C would let the W3C IRP
  suite exercise the reader that ships instead of a test-only one written
  against REXML. That is the concrete test of whether this interface was worth
  defining, and it is not claimed until it is done.

## Implementation notes

Recorded when this ADR was implemented (2026-09-23), where the implementation
showed the text above to be wrong or silent.

- **The codec is `tinyactors-scxml`, over txml.** It is freestanding C over
  the repository's own XML library, not a vendored parser. It is a sibling
  module because it depends on core and on txml, and neither may depend on
  it. txml gained `TXML_WRITE_FRAGMENT` (txml ADR 0024) so the codec can
  write a `<content>` element's children, which are not a document.
- **A codec is configured.** Hard-coded bounds would not do for real
  embedders, so the codec is made from `ta_scxml_codec_params`: input bytes,
  parsed bytes, depth, attributes, text bytes, entity expansion, name length
  and distinct names, with zero meaning a documented default. The bounds txml
  also has are passed through to it, not enforced twice. A limit error's
  `construct` names the field.
- **What a literal content's bytes are.** Content with element children is
  re-serialized as the §1 comment says. Content with only character data is
  that character data, references resolved, not an escaped serialization:
  a data model imports `{"k": 1}` or `some text`, not `some &amp; text`. Both
  carry the codec's media type. To round-trip, encode writes bytes back as
  markup only if they parse as markup and re-serialize to themselves;
  otherwise it writes them as text.
- **`<invoke src>` is not fetched.** §1 listed it for the resolver, but it is
  a URI that the invoker interprets at run time (SCXML 6.4), so it stays in
  the spec. The resolver serves `<data src>` and `<script src>`, and a
  fetched `<data src>` becomes literal content in the codec's media type.
- **`<content expr>` of markup.** A `<content expr>` whose value is markup
  (test530 assigns an `<scxml>` document and invokes it) arrives as `data`, a
  value. The test host's SCXML invoker reads a string value as SCXML text.
  That is the invoker's rule, not core's.
- **Where decoded text points.** Text is a slice of the input wherever XML
  left it byte-for-byte unchanged. It points into txml's value pool, which is
  also in the decode arena, when XML changed it: a character or entity
  reference, attribute-value normalization, a CRLF, or text split across
  nodes (the codec joins these). Only the join and `<content>` markup are
  copies the codec makes.
- **The compile error is a placeholder.** `ta_compile_error` has only
  `TA_COMPILE_ERROR_NONE`, and an import that fails in the compiler reports
  `TA_IMPORT_STAGE_COMPILE` with `located = false`, pending ADR 0058.
- **Durations.** `ta_duration_parse` accepted only a whole part, which the
  chart DSL never exposed because it wrote milliseconds. A document hands
  the compiler the author's text, and SCXML durations are CSS2 times: `.5s`
  is now accepted (W3C test175).
- **The anonymous state ID** is `_ta_anonymous_N`, chosen to be distinct
  from every authored state ID in the document, inline machines included.
- **The consequence in the last bullet is claimed.** `document.rb` is
  deleted. The IRP harness rewrites the suite's `conf:` markup into SCXML and
  imports every document through the codec. The same 181 of 200 tests are
  imported, and all 183 IRP scenarios pass, as they did through
  `document.rb`.

## Sources

Primary, verified when this ADR was written:

- SCXML 1.0, 5.3.2 `<data>` Children — <https://www.w3.org/TR/scxml/#data>
- SCXML 1.0, 5.4.2 `<assign>` Children — <https://www.w3.org/TR/scxml/#assign>
- SCXML 1.0, 5.5.2 `<donedata>` Children — <https://www.w3.org/TR/scxml/#donedata>
- SCXML 1.0, 5.6 `<content>` — <https://www.w3.org/TR/scxml/#content>
- SCXML 1.0, 6.4.2 `<invoke>` Children — <https://www.w3.org/TR/scxml/#invoke>
- SCXML 1.0, A.2 Conforming Processors — <https://www.w3.org/TR/scxml/#ConformingProcessors>
- SCXML 1.0, G.6 Inline Content and Namespaces — <https://www.w3.org/TR/scxml/#content_and_namespaces>

In-tree, cited above by file and line: `src/interpreter.c:222`, `:501`,
`:1467`; `src/invoke.c:366`; `src/builder.c:127`; `src/export.c:92`;
`src/definition.h:210`, `:212`; `tinyactors-test/src/json_datamodel.c:658`;
`tinyactors-test/src/ecmascript_datamodel.c:1234`;
`tinyactors-test/src/null_datamodel.c:227`.

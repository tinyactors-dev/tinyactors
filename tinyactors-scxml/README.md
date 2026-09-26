# tinyactors-scxml

The SCXML 1.0 XML codec for tinyactors: a `ta_chart_codec` (core ADR 0053)
that reads and writes SCXML documents over [txml](../tinyxml), the
freestanding XML library. Core ships without XML (core ADR 0025); this module
is the reader and writer that does it, as a sibling of both.

```text
mise run build   # build the codec and its test against core's and txml's dist/
mise run test    # run tests/ta_scxml_test.c
```

## Why a module of its own

The codec depends on core (its types) and on txml (its parser and writer),
and neither may depend on it: core links no parser, and txml knows nothing of
SCXML. It is not test code either: it is the reader an embedder ships, and
the W3C IRP suite in `tinyactors-test` runs through it. The test host
compiles `src/ta_scxml.c` into its own binary and links txml's published
archive.

## What it does

- **Decode.** txml parses the input into the operation's arena, and the
  codec walks the tree into a `ta_scxml_spec`. Every SCXML element and
  attribute must be understood: anything else is a `ta_chart_error` with the
  element, the attribute and the position. Attributes in other namespaces are
  another vocabulary's and are ignored. A state without an `id` gets
  `_ta_anonymous_N`, unused by any authored id (SCXML 3.14).
- **Encode.** txml's writer writes a spec back, one element per line,
  indented two spaces per level. Decoding the output gives an equal spec
  (tested as the same revision and a byte-identical re-encoding).
- **`<content>`.** A body without element children is its character data. A
  body with markup is re-serialized as a fragment, every element carrying the
  namespace declarations it needs from outside (SCXML G.6). Both are labelled
  `application/scxml+xml`, the codec's name.
- **Outside world.** `<data src>` and `<script src>` go to the caller's
  `ta_chart_src_resolver`. Elements of another namespace in executable content
  go to the caller's `ta_chart_lowering` (SCXML 4.10, core ADR 0046); without
  one they are refused.

## Where decoded text points

Text in the decoded spec is a slice of the input wherever XML did not change
it: the attribute value or text node reads byte-for-byte as it was written.
Otherwise it points into txml's value pool, which txml filled while parsing
and which lives in the same arena. That happens for:

- a character or entity reference (`&lt;`, `&#10;`, an internal entity);
- attribute-value normalization of a tab or line end in an attribute;
- a CRLF line end, which XML reads as LF;
- text split across several nodes (text and CDATA), which the codec joins.

Only the last, and the re-serialization of `<content>` markup, are copies the
codec itself makes.

## Where elements are (core ADR 0086)

Decode fills the spec's spans: every state, transition, initial transition,
action and expression says where it was read, as an offset, a length, and
the lines and columns of its start and end. The decode params' `name` is
the first source; each file the resolver fetched is another, named by its
`src`.

| Element | Span |
|---|---|
| a state, `<transition>`, `<initial>`, action | the element, start tag through end tag |
| `<elseif>` (an `<if>` in the else branch) | the `<elseif>` element |
| an `initial` attribute | the attribute, name through closing quote |
| an expression attribute (`cond`, `expr`, `location`, `*expr`, `idlocation`, `array`) | its value between the quotes, as written: `x &gt; 1` is `x &gt; 1` |
| a `<script>` body | its text, inside any CDATA markers |
| a `<script src>` | all of the fetched file, in its own source |
| a `namelist` entry | none: a name, not an authored expression |

Encode reports the same places in the text it writes, through the encode
params' `spans` sink, so a document encoded and decoded again reads every
element exactly where encode said it wrote it. The codec indents by writing
whitespace between SCXML elements itself rather than with txml's
`TXML_WRITE_INDENT`: the writer would indent the markup of a `<content>`
too, and change its bytes and so the chart's revision.

## Limits

`ta_scxml_codec_params` holds every bound a decode enforces; zero is the
default beside each field in `include/ta_scxml.h`. Reaching one is
`TA_CHART_ERROR_LIMIT` with the field's name as `construct` and the position
in `at`.

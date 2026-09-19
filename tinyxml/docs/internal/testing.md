# Testing

The suite is written against `include/txml.h` and exists before the
implementation does. It is the executable half of the ADRs: where a decision
has an observable consequence, there is a fixture that pins it.

## What runs

| Binary | Source | Needs `src/`? | What it covers |
|---|---|---|---|
| `header_test` | `tests/header_test.c` | no | Layout, constants, flag sets, and the ergonomics of `TXML_STR` and `TXML_LIMITS_DEFAULT`. Calls no library function, so it links and runs today. |
| — | `tests/api_surface.c` | no (compile only) | One typed function pointer per `TXML_API` function. A missing, renamed, or retyped declaration is a compile error. Never linked: the symbols do not exist yet. |
| `arena_test` | `tests/arena_test.c` | yes | `txml_arena`: alignment, address stability across growth, contiguous oversized blocks, mark/release/reset/destroy, and refusal handling. |
| `writer_test` | `tests/writer_test.c` | yes | The writer, every case run with a 4096-byte and a 1-byte output buffer. |
| `spec_test` | `tests/spec_test.c` | yes | The data-driven corpus in `tests/spec`, plus the chunk sweep. |

```
mise run build                         # compile everything; link what can be linked
mise run test                          # run every linked binary
mise run spec                          # the whole corpus, verbosely
mise run spec tests/spec/elements.spec # one file
```

While `src/` has no modules, `mise run build` prints

```
implementation pending: linked 1 of 4 test binaries (src/ has no modules yet)
```

and `mise run test` runs `header_test`, reports the other three as pending, and
exits 0. The moment `src/*.c` appears they are linked and any failure fails the
task. Do not weaken a test to make the suite pass.

## The spec format

A `.spec` file is a sequence of cases. Outside a section, a line starting with
`#` is a comment and blank lines are ignored.

```
@case nested elements and text
@flags KEEP_COMMENTS SKIP_BLANK_TEXT
@limits max_depth=8 max_names=16
@chunk 3
@xml
<a><b>hi</b></a>
@nodes
0 document - end=4 parent=- depth=0
1 element {}a end=4 parent=0 depth=1 attributes=0
2 element {}b end=4 parent=1 depth=2 attributes=0
3 text "hi" end=4 parent=2 depth=3
@attributes
@names
0 {}a
1 {}b
@error
@end
```

- `@case <name>` opens a case, `@end` closes it. `@xml` and `@end` are required.
- `@flags` takes `txml_parser_flags` names without the `TXML_` prefix, separated
  by spaces.
- `@limits` takes `field=value` pairs using the `txml_limits` field names.
  Unlisted fields keep `TXML_LIMITS_DEFAULT`.
- `@chunk N` feeds the input `N` bytes at a time; `0` or absent feeds it whole.
- `@xml` is the input. Lines are joined with `\n` and there is no trailing
  newline. `\n`, `\r`, `\t`, `\"`, `\\`, and `\xNN` are decoded, which is how a
  case writes CRLF, a control byte, or invalid UTF-8. A line inside `@xml` that
  starts with `@` would be read as a directive; write `\x40` instead.
- `@nodes`, `@attributes`, `@names`, `@positions`, `@roundtrip`: **absent means
  not checked; present means exact match**, so a present-but-empty section
  asserts that the renderer produces nothing.
- `@error` is special: absent, or present and empty, asserts that the parse
  succeeds. One line asserts that it fails exactly that way.

### What the renderer prints

`tests/spec_test.c` is the single source of truth for this format. Change one,
change the other.

```
<index> <kind> <payload>[ prefix=<p>] end=<n|-> parent=<n|-> depth=<d>[ attributes=<n>][ flags=<f>[,<f>]]
```

- `<kind>` is `document`, `element`, `text`, `comment`, or `pi`.
- `<payload>` is `-` for the document node, `{namespace}local` for an element,
  `{namespace}local "data"` for a PI, and `"text"` for text and comments.
- `prefix=` appears on an element that was written with one.
- `end=-` means the node is still open (only reachable mid-parse).
- `attributes=` appears on elements only.
- `flags=` lists `EMPTY`, `CDATA`, `OPEN` and is omitted when there are none.

Attributes, numbered within their element:

```
<node-index>.<attribute-index> {namespace}local "value"[ flags=NS_DECL[,DEFAULTED]]
```

Names, in the order they were interned, and positions, one per node:

```
<name-id> {namespace}local
<node-index> <line>:<column> offset=<n>
```

An error, with `limit=` only for `LIMIT`:

```
<CODE> <line>:<column> offset=<n>[ limit=<KIND>]
```

`@roundtrip` holds the exact bytes `txml_write_subtree()` produces for the
document root, with the writer configured with `TXML_WRITE_EMPTY_TAGS` and
nothing else.

Text renders in double quotes with `\n`, `\r`, `\t`, `\"`, `\\` escapes and
`\xNN` for other control bytes; bytes above 0x7f pass through, so the fixtures
stay readable UTF-8.

## The chunk sweep

Every case is parsed again at chunk sizes 1, 2, 3, 7, and whole-input, and all
renderings must be byte-identical to the first. This is the property that
catches boundary bugs: a name split across two feeds, a CRLF pair straddling
the staging window, a partial UTF-8 sequence, an entity reference cut in half.
It costs nothing to write and it is the reason the fixtures are worth having.

`spec_test` also checks that `txml_parser_offset()` matches the number of bytes
actually fed, and that the parser never asks for input after the final chunk.

## Round-trip properties

Beyond the `@roundtrip` fixtures, two properties are worth running over any
corpus once the implementation exists:

1. **Model stability.** `parse(write(parse(x)))` equals `parse(x)` node by
   node. Writing and re-reading must not change the document.
2. **Byte fixed point.** `write(parse(write(parse(x))))` is byte-identical to
   `write(parse(x))`. After one round-trip the output stops changing.

Neither needs new API: both are `spec_test` machinery applied twice, and they
belong here as soon as there is something to run them against.

## Policies these fixtures pin

The header leaves some choices to the implementation. The suite fixes them, so
they are decisions now rather than accidents later.

**Parser**

- Name IDs are assigned in first-appearance order: an element's own name before
  its attributes, attributes in document order. An attribute whose expanded
  name equals an element's shares that name ID.
- A namespace declaration is stored as an attribute flagged `NS_DECL` whose
  expanded name is `{http://www.w3.org/2000/xmlns/}xmlns` for a default
  declaration and `{http://www.w3.org/2000/xmlns/}<prefix>` for a prefixed one.
- An unprefixed attribute has no namespace: a default `xmlns` never applies
  to it.
- Names of dropped nodes are not interned: with `TXML_KEEP_PIS` off, a PI
  target never reaches the name table.
- Whitespace outside the document element is never a node.
- The XML declaration is not a PI node.
- Under `TXML_KEEP_CDATA` a CDATA section is always its own text node, even
  when empty; without the flag it merges with the text around it.
- Character references are not subject to attribute-value whitespace
  normalization: `&#9;` stays a tab, a literal tab becomes a space.
- An error is reported at the first byte of the offending construct: the `<` of
  a tag, the first byte of a name, the `&` of a reference, the first byte of a
  bad character sequence.
- The document node's position is the start of the input.

**Writer**

- Text escapes `&`, `<`, and `>`. Attribute values escape those plus `"`, and
  write tab, LF, and CR as `&#9;`, `&#10;`, `&#13;`.
- A childless element is `<a></a>` unless `TXML_WRITE_EMPTY_TAGS` is set.
- A namespace declaration is emitted only when the binding a name needs is not
  already in scope when the start tag closes — so a document whose `xmlns`
  attributes are written back explicitly gains nothing extra, which is what
  makes round-trips exact.
- An element needing an unbound namespace with no prefix gets a default
  declaration; an attribute needing one gets a generated prefix `ns1`, `ns2`, …
  declared on its element.
- `]]>` inside CDATA is split across two sections.
- A comment containing `--` or ending in `-`, and PI data containing `?>`, are
  refused with `TXML_ERR_INVALID` rather than written as malformed XML.
- `TXML_WRITE_INDENT` puts each child on its own line, indented two spaces per
  level, with the end tag at the parent's indent and no trailing newline.

## Adding a case

Prefer a spec case to a C test: an input and an expected node table is
readable without knowing the API. Reach for C only when the thing under test is
not a document — allocator behaviour, the drain protocol, refusals.

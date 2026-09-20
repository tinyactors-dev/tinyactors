# Parser

The parser turns fed bytes into appended nodes. It is a byte-driven state
machine with no recursion: element nesting lives in an explicit stack bounded
by `limits.max_depth`, so C stack depth is constant. Every construct it scans
becomes a node — nothing is scanned and thrown away (ADR 0017).

## The loop the caller writes

```c
for (;;) {
    switch (txml_parse_step(parser)) {
    case TXML_STEP_PROGRESS:
        continue;                       /* budget spent; nothing is wrong */
    case TXML_STEP_NEED_INPUT:
        n = read(...);
        txml_parse_feed(parser, (txml_bytes){ buffer, n }, n == 0);
        continue;                       /* re-feed the remainder if short */
    case TXML_STEP_NEED_MEMORY:
        /* extend the budget and retry, or give up */
        continue;
    case TXML_STEP_FAILED:
        txml_parser_error(parser, &error);
        return;
    case TXML_STEP_DONE:
        txml_parser_finish(parser, &doc);
        return;
    }
}
```

Each outcome is precise:

| Outcome | Meaning | Parser state |
|---|---|---|
| `TXML_STEP_PROGRESS` | `step_budget` nodes were appended | consistent; call again |
| `TXML_STEP_NEED_INPUT` | staged bytes are exhausted (or only a holdback remains) and `final` has not been seen | consistent; feed, then call again |
| `TXML_STEP_NEED_MEMORY` | an arena allocation failed | consistent and **unchanged**: nothing half-written, retry after freeing memory |
| `TXML_STEP_DONE` | the document element closed, only whitespace, comments or PIs followed — each appended as a node — and `final` was seen | terminal |
| `TXML_STEP_FAILED` | a document error; read `txml_parser_error` | terminal; the partial document remains readable |

`TXML_STEP_DONE` and `TXML_STEP_FAILED` are sticky: further steps repeat them.

## Feeding

`txml_parse_feed` copies into the **staging window**, a parser-owned buffer of
`staging_length` bytes (default 64 KiB) followed by 64 bytes of padding that
is never filled with input. The padding exists so a scanner may read up to 64
bytes past the last valid byte without undefined behaviour — the property a
SIMD block scanner needs, and the reason `TXML_INPUT_PADDED` asks callers for
the same guarantee when they want the copy skipped.

Rules:

1. The call copies `min(chunk.length, free space)` bytes and returns that
   count. Free space is `staging_length` minus the bytes staged and not yet
   consumed.
2. **Partial accept is normal.** When the return value is less than
   `chunk.length`, the caller steps (which consumes staged bytes) and feeds
   the remainder.
3. `final` is recorded **only** when the whole offered chunk was accepted.
   Otherwise it is ignored, and the caller repeats it with the rest. A caller
   that never checks the return value and never re-feeds silently truncates
   its document into `TXML_DOC_UNEXPECTED_EOF`.
4. Feeding after `final` was recorded makes the next step return
   `TXML_STEP_FAILED` with `TXML_DOC_SYNTAX`; feeding into a terminal parser
   returns 0.
5. `staging_length` below 64 is `TXML_ERR_INVALID` at `txml_parser_init`.

**Compaction.** A step consumes a prefix of the window. What remains
unconsumed at `TXML_STEP_NEED_INPUT` is the **holdback**: at most 16 bytes, the
length of the longest fixed token the scanner must see whole (`<![CDATA[`,
`<!DOCTYPE`) plus a partial UTF-8 sequence. The holdback is moved to the front
of the window before the next feed. No construct larger than the holdback is
ever kept in staging: names, attribute values and text are pushed to their
destination as they are scanned, so a 10 MiB attribute value flows through a
64 KiB window without difficulty.

`txml_parser_offset` reports bytes accepted by feeds so far, which is also the
base for `txml_position.offset`.

## Scanning

The scanner is a state machine. Each state scans for a small set of delimiter
bytes and copies everything between them verbatim to the destination; this is
the loop that vectorizes.

| State | Delimiters | Destination |
|---|---|---|
| Prolog / misc | `<` | pool run of a text node under the document node; prolog and epilog whitespace is kept like any other text |
| Text content | `<` `&` `\r` | pool run of the pending text node |
| Start tag | whitespace `/` `>` `=` `'` `"` | parser scratch |
| Attribute value | the opening quote, `<` `&` `\r` `\t` `\n` | pool run of the pending attribute |
| Comment | `-` | pool run of the comment node |
| CDATA | `]` | pool run of the pending text node |
| PI | `?` | pool run of the PI node |
| DOCTYPE | `<` `>` `'` `"` `[` `]` | pool run of the doctype node, and the internal subset tables |
| Entity reference | `;` and the name characters | scratch, then expanded to the destination |

`\r` is a delimiter in text and attribute values because line-end
normalization has to see it. `<` is a delimiter in an attribute value because
it is forbidden there.

## Start tags

The **whole start tag is assembled in parser scratch** before any of it is
interned or appended. This is not an optimization; it is required:

```xml
<s:state xmlns:s="http://www.w3.org/2005/07/scxml" s:id="x"/>
```

`s:state` cannot be resolved until `xmlns:s` has been read, and a declaration
may appear after attributes that use it. So the parser:

1. collects the element's raw qualified name and each attribute's raw
   qualified name into scratch (records of `{prefix, local, value pool run}`),
   copying values to the pool as it goes;
2. applies every `xmlns` and `xmlns:*` attribute on this tag to the binding
   stack;
3. resolves the element name, then each attribute name, against the binding
   stack;
4. checks for duplicate **expanded** names among the attributes;
5. interns names, appends the element node, then appends the attribute
   entries.

Scratch is bounded by `max_attributes` records and `max_name_length` bytes per
name. Attribute values go straight to the pool, so a large value costs nothing
extra in scratch.

**The binding stack** is a vector of `{prefix run, namespace id, owning
depth}` entries. Declarations push; closing an element pops every entry whose
owning depth is the element's. Resolution scans the stack from the top, which
is correct because inner declarations shadow outer ones. `xmlns=""` pushes a
binding of the empty prefix to namespace 0. A prefix that resolves to nothing
is `TXML_DOC_UNDECLARED_PREFIX`; `xml` is pre-bound to
`http://www.w3.org/XML/1998/namespace` and `xmlns` to
`http://www.w3.org/2000/xmlns/`, and rebinding either is
`TXML_DOC_UNDECLARED_PREFIX` as well.

**Interning** hashes the expanded name (FNV-1a over the URI bytes, `0xFF`,
then the local bytes) and probes an open-addressed table of name IDs held in
the parser arena. A miss appends a `names` entry, copies the local part and
the prefix into the pool, and returns the new ID. The table doubles and is
rebuilt when it passes 70% load; it stores IDs only, no pointers, and nothing
outside the parser references it, so rebuilding it breaks no promise (see
`memory-architecture.md`). Exceeding `max_names` is `TXML_DOC_LIMIT` with
`TXML_LIMIT_NAMES`.

## Normalization

Everything written to the pool is already normalized; no consumer ever has to
post-process a `txml_str`.

**Line ends** (XML §2.11): `\r\n` and a lone `\r` both become `\n`, in text,
attribute values, comments, PI data and CDATA alike.

**Character references** `&#d;` and `&#xh;` are decoded to UTF-8. A reference
outside the XML `Char` production — including a surrogate, `0`, or a C0
control other than tab, LF and CR — is `TXML_DOC_ENCODING`.

**Entity references.** `&amp;` `&lt;` `&gt;` `&apos;` `&quot;` always resolve.
Any other name resolves against the internal-subset table, which every
document has because the subset is always read; a name that is not in it is
`TXML_DOC_UNDECLARED_ENTITY`.
A general entity's replacement text is scanned like any other input, and
nested references expand through the same path; every byte an expansion
produces is charged against `entity_expansion_budget`, so the billion-laughs
family terminates as `TXML_DOC_LIMIT` with `TXML_LIMIT_ENTITY_EXPANSION`. That
budget is now the only thing standing between a document and an expansion
bomb, and it applies to every parse, because there is no longer a
configuration in which the subset goes unread.
Recursion is a syntax error rather than a hang: an entity that is already on
the expansion stack is `TXML_DOC_SYNTAX`.

**Attribute values** (XML §3.3.3): after line-end normalization, every tab, LF
and space becomes a single space, each kept separately (no collapsing —
txml does not know attribute types, so it performs the non-tokenized
normalization only). Whitespace produced by a character reference is
**not** normalized, which is what makes `&#x9;` round trip as a tab.

**Text nodes.** Consecutive character data and resolved references between the
same pair of tags form **one** text node: the parser appends the node once and
keeps extending its pool run. A whitespace-only run is a text node like any
other, wherever it appears, including between the prolog's items and after the
document element.

A **CDATA section always breaks the run**. It is appended as its own text node
flagged `TXML_NODE_CDATA` and never merges with the text on either side, so
`<a>x<![CDATA[y]]>z</a>` is three text nodes rather than one. That is what
makes section boundaries survive a round trip.

## Appending and patching

- A start tag appends the element with `TXML_NODE_OPEN`, `end ==
  TXML_INDEX_NONE`, and pushes `{node index, binding stack height}` on the
  open-element stack.
- An empty-element tag appends the element with `TXML_NODE_EMPTY` and `end ==
  index + 1`; it is never open and nothing is pushed.
- A text, comment, PI or doctype node is appended with `end == index + 1`.
- The XML declaration appends a `TXML_NODE_DECLARATION` with `end == index +
  1`, followed by its pseudo-attributes as ordinary attribute entries, exactly
  as a start tag appends an element's. It is a node, not a PI, and it counts
  against the step budget like any other append.
- An end tag pops the open-element stack, checks that the raw qualified name
  matches the one the start tag was written with (`TXML_DOC_TAG_MISMATCH`
  otherwise — the comparison is on the written form, per XML §3.1), writes
  `end = txml_doc_node_count(doc)`, clears `TXML_NODE_OPEN`, and pops the
  binding stack to the saved height.
- Node 0 (`TXML_NODE_DOCUMENT`) is appended by `txml_parser_init`; its `end`
  is written when the document element closes and the input ends.

The **step budget** counts nodes appended. Patching `end` is not an append and
does not count, so closing a deep tree never stalls. A budget of 0 means the
step runs until it blocks, completes or fails.

## Failure catalogue

Every document error carries a position (the byte that made the parse
impossible) and a static message.

| Code | Raised when |
|---|---|
| `TXML_DOC_ENCODING` | ill-formed UTF-8; an overlong or surrogate encoding; a byte sequence or character reference outside the XML `Char` production; an XML declaration naming an encoding other than UTF-8 |
| `TXML_DOC_SYNTAX` | malformed markup: an unquoted attribute value, a missing `=`, `--` inside a comment, `]]>` in text, a bad PI, a malformed reference, recursive entity expansion, a second root element, a feed after `final` |
| `TXML_DOC_NAME` | a name that violates `NameStartChar`/`NameChar`, or a QName with more than one colon or an empty part |
| `TXML_DOC_TAG_MISMATCH` | an end tag whose written name differs from the open element's, or an end tag with no open element |
| `TXML_DOC_DUPLICATE_ATTRIBUTE` | two attributes on one tag with the same expanded name, including two prefixes bound to the same URI |
| `TXML_DOC_UNDECLARED_PREFIX` | a prefix with no binding in scope, or an attempt to rebind `xml` or `xmlns` |
| `TXML_DOC_UNDECLARED_ENTITY` | a general entity that is neither predefined nor declared in the internal subset |
| `TXML_DOC_UNEXPECTED_EOF` | `final` arrived inside markup, or with elements still open |
| `TXML_DOC_NO_ROOT` | `final` arrived with no document element |
| `TXML_DOC_TRAILING_CONTENT` | non-whitespace character data, or a second element, after the document element closed. Whitespace there is legal and becomes a text node. |
| `TXML_DOC_LIMIT` | a `txml_limits` value was reached; `limit` says which |

After `TXML_STEP_FAILED` the document is still readable: the nodes appended
before the failure are intact, and open elements stay open. This is what lets
an editor point at the error in a partial tree.

# Parser

The parser turns fed bytes into appended nodes. It is a table of resumable
states (ADR 0021): each construct in the grammar is a struct holding a `run`
function and an `at_eof` function, the parser holds a pointer to the current
one, and everything a state needs in order to continue after a chunk boundary
is carried in the parser, never in the input window. Nothing in it recurses:
element nesting lives in an explicit stack bounded by `limits.max_depth`, and
entity expansion in an explicit frame stack, so C stack depth is constant.
Every construct it scans becomes a node — nothing is scanned and thrown away
(ADR 0017).

No construct has to fit in the input window. A 10 MiB attribute value, a
5 MiB text node, or a comment as long as the document flows through a 64-byte
window.

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
is never filled with input. The window is the size of a *chunk*, not of a
construct: a state consumes everything it can read, so the window empties on
every step and a feed only ever has to hold what has not been stepped yet.

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

**Validation happens as bytes arrive.** The feed checks the new bytes as UTF-8
and as XML `Char`s and keeps three indices into the window: `start`, the first
unconsumed byte; `valid`, one past the last *whole valid character*; and `bad`,
the lead byte of the first invalid character, if any. States read only
`[start, limit)`, where `limit` is `bad` when there is one and `valid`
otherwise, so a state never sees half a character. A multi-byte character
cut off by the end of a chunk is the whole **holdback**: at most three bytes
stay unreadable until the rest arrives. When `start` reaches `bad` the step
fails with `TXML_DOC_ENCODING` at that byte; input that ends inside a character
fails the same way at its lead byte.

**Compaction.** Whatever is unconsumed (only ever a holdback) moves to the
front of the window at the next feed. No state keeps an index into the window
across a call, which is what makes that move safe.

`txml_parser_offset` reports bytes accepted by feeds so far, which is also the
base for `txml_position.offset`.

## The scanner

A state is

```c
typedef struct state {
    const char *name;
    scan (*run)(txml_parser *p);    /* consume what is readable */
    scan (*at_eof)(txml_parser *p); /* the end of input, while this state is current */
} state;
```

and `run` reports one of `SCAN_OK` (progress; run it again), `SCAN_YIELD` (the
step budget is spent), `SCAN_NOMEM`, `SCAN_FAIL` or `SCAN_DONE`. There is no
"incomplete": a state consumes every readable byte, and the step loop asks for
input when none is left. The loop is the whole of `txml_parse_step`: finish a
pending entity expansion; if nothing is readable, report the encoding error,
ask for input, or run `at_eof`; otherwise run the current state.

| State | Reads | Then |
|---|---|---|
| `content` | text up to `<`; whitespace only outside the document element | `lt` |
| `lt` | the byte after `<` | `end-tag`, `bang`, `pi-target`, or `start-tag` |
| `bang` | the byte after `<!` | `literal` |
| `literal` | the rest of `<!-`, `<![CDATA[` or `<!DOCTYPE`, a byte at a time | `comment`, `cdata`, `doctype` |
| `comment` | the body; a `-` is held until the next byte says whether it starts `-->` | `content` |
| `cdata` | the body; up to two `]` are held the same way | `content` |
| `pi-target` | the target name | `pi-space`, `pi-question`, or `declaration` |
| `pi-space`, `pi-question`, `pi-data` | whitespace, `?>` with no data, the data (a `?` is held) | `content` |
| `declaration` | the XML declaration, gathered whole in the small buffer | `content` |
| `doctype` | the declaration text and its internal subset | `content` |
| `start-tag` | name, attribute names, `=`, quoted values, `/>` or `>` | `content` |
| `end-tag` | the name, compared with the open element's as it arrives, then `>` | `content` |

**What a chunk boundary can split**, and where the parser keeps it:

| Split | Carried in |
|---|---|
| `-->`, `]]>`, `?>`, the `--` rule | a held-back count of `-`, `]` or `?` |
| `<!--`, `<![CDATA[`, `<!DOCTYPE` | the `literal` state's index into the expected text |
| `\r` followed by `\n` | `decoder.after_cr`: the `\r` is emitted as `\n` at once, and a following `\n` is dropped |
| `]]>` in text | `decoder.brackets` |
| `&name;` or `&#N;` | `decoder.in_ref` and a reference buffer of `max_name_length + 2` bytes; a longer one is `TXML_DOC_SYNTAX` |
| a name | the name buffer, `max_name_length` bytes |
| an entity expansion that ran out of memory | the frame stack, finished before anything else on the next step |
| a multi-byte character | not carried at all: it is held back at feed time (see Feeding) |

**Atomicity.** A state either consumes a byte and changes state for it, or
does neither. `SCAN_NOMEM` therefore leaves the parser exactly where it was
before the failing byte, and `TXML_STEP_NEED_MEMORY` is retryable. Where a
construct ends in several fallible steps (finishing a run, interning a name,
appending a node) each is idempotent, and `tests/stream_test.c` refuses every
allocation of a parse in turn to prove it.

**The step budget** counts nodes appended. It is checked before a construct
that would append one: at the start of a text run and after `<` for everything
but an end tag. An end tag only patches `end`, so it never ends a step; a
step therefore often returns `TXML_STEP_PROGRESS` with a record closed and the
next one not yet appended, which is the state in which `txml_next_sibling`
answers `TXML_PENDING`.

`TXML_STEP_DONE` and `TXML_STEP_FAILED` are sticky.

## Streaming values

Text, comments, CDATA sections, processing-instruction data, the DOCTYPE text
and attribute values are written into the pool as they arrive through a **run**
(`txml_doc_run_begin`, `_append`, `_finish` in `src/doc.h`). A run is
contiguous. When it outgrows its block it moves to a larger one, doubling,
copying what it holds and abandoning the old space, which is sound because a
pending run is invisible: no node or attribute refers to it until `finish`, so
nothing a reader can hold moves. Abandoned bytes stay in the values arena and
count against `max_document_bytes`, so a run costs at most about twice its
final size. At most one run is pending, and nothing else appends to the pool
while it is; a construct that needs to intern a name (a PI) finishes its run
first.

The decoder writes to a **sink**, a struct holding one function pointer: the
pending run, or the small buffer. The small buffer is a growable transient
buffer, bounded by `max_document_bytes`, for the constructs that are worth
holding whole: the XML declaration, an `<!ENTITY>` or `<!ATTLIST>` declaration
(which are kept anyway), and a namespace URI (which is interned).

## Start tags

The **whole start tag is resolved at its `>`**, not before, because

```xml
<s:state xmlns:s="http://www.w3.org/2005/07/scxml" s:id="x"/>
```

cannot resolve `s:state` until `xmlns:s` has been read, and a declaration may
appear after attributes that use it. Until then the state machine records:

1. the element's raw qualified name, copied into the stack arena (it lives
   until the end tag matches it);
2. for each attribute, a record `{prefix, local, value run, position}`, with
   the names copied into a per-tag arena that is released when the tag ends,
   and the value streaming into a pool run as it is decoded;
3. each `xmlns` and `xmlns:*` on the tag, applied to the binding stack as its
   value completes, the value assembled in the small buffer and interned.

At `>` the parser adds the `ATTLIST` defaults of this element that the tag did
not write, resolves the element name and then each attribute name against the
binding stack, checks for duplicate **expanded** names, interns in that order,
and appends the element and its attributes together. If an append runs out of
memory the counts are rolled back and the whole finish is retried.

Names are bounded by `max_name_length` and attributes by `max_attributes`
records, so a tag of any length costs what its names and values cost.

**The binding stack** is a linked list of `{prefix, namespace id}` entries in
the stack arena, released in LIFO order with the element that pushed them.
Resolution scans it from the top, which is correct because inner declarations
shadow outer ones. `xmlns=""` binds the empty prefix to namespace 0. A prefix
that resolves to nothing is `TXML_DOC_UNDECLARED_PREFIX`; `xml` is
pre-bound to `http://www.w3.org/XML/1998/namespace` and `xmlns` to
`http://www.w3.org/2000/xmlns/`, and rebinding either is
`TXML_DOC_UNDECLARED_PREFIX` as well.

**Interning** hashes the expanded name (FNV-1a over the URI bytes, `0xFF`,
then the local bytes) and probes an open-addressed table of name IDs held in
the document's values arena. A miss appends a `names` entry, copies the local
part and the prefix into the pool, and returns the new ID. Exceeding
`max_names` is `TXML_DOC_LIMIT` with `TXML_LIMIT_NAMES`. Namespaces are found
by scanning, so their number is bounded by `max_names` as well.

Each element and attribute also records the prefix it was written with when it
differs from its name's first one (ADR 0020).

## Normalization

Everything written to the pool is already normalized; no consumer ever has to
post-process a `txml_str`. The decoder does it as bytes stream through, in one
of three modes: **text**, **attribute** and **verbatim** (comments, CDATA, PI
data, the DOCTYPE text).

**Line ends** (XML §2.11): `\r\n` and a lone `\r` both become `\n`, in every
mode. In attribute values every `\r`, `\n` and tab then becomes one space, each
kept separately (no collapsing — txml does not know attribute types, so it
performs the non-tokenized normalization only).

**Character references** `&#d;` and `&#xh;` are decoded to UTF-8. A reference
outside the XML `Char` production — including a surrogate, `0`, or a C0
control other than tab, LF and CR — is `TXML_DOC_ENCODING`. Whitespace
produced by a reference is **not** normalized, which is what makes `&#x9;`
round trip as a tab.

**Entity references.** `&amp;` `&lt;` `&gt;` `&apos;` `&quot;` always resolve.
Any other name resolves against the internal-subset table, which every
document has because the subset is always read; a name that is not in it is
`TXML_DOC_UNDECLARED_ENTITY`. A general entity's replacement text is scanned
like any other input, and nested references expand through the same path on
the frame stack; every byte an expansion produces is charged against
`entity_expansion_budget`, so the billion-laughs family terminates as
`TXML_DOC_LIMIT` with `TXML_LIMIT_ENTITY_EXPANSION`. That budget is the only
thing standing between a document and an expansion bomb, and it applies to
every parse. An entity that is already on the frame stack is
`TXML_DOC_SYNTAX`.

**When references are read.** Character references in an entity's literal are
expanded when the entity is *declared* (XML 1.0 §4.5); `&name;` references are
kept. What is left is decoded again at each reference. So
`<!ENTITY e "&#38;#60;">` is stored as `&#60;` and reads as `<`, and
`<!ENTITY e "&#38;amp;">` reads as `&`.

**Text nodes.** Consecutive character data and resolved references between the
same pair of tags form **one** text node: the parser appends the node once,
when the `<` that ends the run arrives, and the run behind it may have been
built up over any number of feeds. A whitespace-only run is a text node like
any other, wherever it appears, including between the prolog's items and after
the document element.

A **CDATA section always breaks the run**. It is appended as its own text node
flagged `TXML_NODE_CDATA` and never merges with the text on either side, so
`<a>x<![CDATA[y]]>z</a>` is three text nodes rather than one. That is what
makes section boundaries survive a round trip.

## The DOCTYPE

`<!DOCTYPE` becomes a `TXML_NODE_DOCTYPE` whose text is everything between
`<!DOCTYPE` and its closing `>`, streamed into a run exactly as written (line
ends normalized). A second, byte-driven view of the same bytes finds what
matters in them: the internal subset between `[` and `]`, and inside it the
declarations. `<!ENTITY name "value">` (general, internal) and `<!ATTLIST>`
defaults (`"value"` and `#FIXED "value"`) are assembled whole in the small
buffer and read from there; comments, processing instructions, `<!ELEMENT>`,
`<!NOTATION>` and parameter-entity references stream past without being held.
The first declaration of an entity, or of a default for one element and
attribute, binds. External entities are never recorded and never fetched.

Both tables are hash tables in the parser arena (`src/tables.c`), charged
against `max_document_bytes`: entities by name, and defaults by element and by
element and attribute. A default is applied when a start tag does not write
the attribute, after the written attributes, and carries `TXML_ATTR_DEFAULTED`
(stored as a bit in the attribute entry, see `document-layout.md`). A default
for `xmlns` or `xmlns:p` declares a namespace like a written one.

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

## Status

Implemented in `src/`: elements, attributes, namespaces, text, character and
predefined entity references, comments, PIs, CDATA, the XML declaration, the
DOCTYPE and its internal subset, UTF-8 and `Char` validation, positions,
every limit, and the step budget — all streaming.

Known deviations, each a deviation rather than a design change:

- **Markup in an entity's replacement text** is `TXML_DOC_SYNTAX` in content
  (in an attribute value `<` is an error anyway). An entity that expands to
  elements is not supported, and is a non-goal.
- **The small constructs are bounded by `max_document_bytes`, not by a
  constant.** The XML declaration, an ENTITY or ATTLIST declaration and a
  namespace URI are gathered whole; a document may make one large, up to that
  limit.
- **`TXML_INPUT_PADDED`** is accepted and ignored: input is always copied.
- **Error priority follows byte order.** Validation is incremental, so when a
  construct has both an encoding error and an earlier syntax error the syntax
  error is the one reported; the whole-construct scanner reported the encoding
  error first.
- **Namespaces are bounded by `max_names`.**

# Writer

The writer takes events and fills a caller buffer. It knows nothing about
documents: anything that can call start/attribute/text/end can produce XML.
`txml_write_subtree` is the one bridge from a parsed document, and it is
written in terms of the same event calls.

Everything the writer refuses is **API misuse** (`TXML_ERR_INVALID`), never a
document error. The caller controls the events, so a malformed event is a bug
in the caller.

## State machine

| State | Entered | Accepts |
|---|---|---|
| `PROLOG` | at init | `txml_write_comment`, `txml_write_pi`, `txml_write_start`, `txml_write_subtree` |
| `TAG` | after `txml_write_start` | `txml_write_attribute`, and anything that ends the tag |
| `CONTENT` | after the first non-attribute call inside an element | every emit call |
| `EPILOG` | when the outermost element closes | `txml_write_comment`, `txml_write_pi`, `txml_writer_finish` |

The start tag's `>` is **deferred** while in `TAG`, because the writer does not
yet know whether the element is empty. It is emitted when content follows, or
resolved by `txml_write_end`: `/>` with `TXML_WRITE_EMPTY_TAGS`, `></local>`
without it.

With `TXML_WRITE_DECLARATION`, `<?xml version="1.0" encoding="UTF-8"?>` is
emitted before the first event. Writing text in `PROLOG` or `EPILOG`, or
calling `txml_write_end` with no open element, is `TXML_ERR_INVALID`.

## The element stack

Each open element pushes a frame into the writer's arena:

```text
{ qualified name bytes (prefix ':' local, copied), binding stack height,
  had_content, subtree end (txml_write_subtree only) }
```

The name bytes are **copied**. A `txml_write_name` is borrowed for the
duration of the call, but the end tag is written much later, so the writer
cannot keep the caller's pointer. Frames are bounded by `limits.max_depth`
(`TXML_ERR_LIMIT` beyond it) and the name bytes by `max_name_length`.

The namespace binding stack mirrors the parser's: `{prefix, URI, owning
depth}`, pushed by declarations, popped when the owning element closes.
URIs and prefixes in it are copied for the same reason.

## Prefix selection

`txml_write_name` has three cases:

| `ns` | `prefix` | Behaviour |
|---|---|---|
| empty | empty | unqualified name. If a default binding to a non-empty URI is in scope, the writer emits `xmlns=""` on this start tag to undeclare it. |
| non-empty | non-empty | use that prefix. If it is not already bound to that URI in scope, emit `xmlns:prefix="ns"` on this start tag. |
| non-empty | empty | reuse a prefix already bound to that URI in scope (the innermost one). If none exists, invent `ns1`, `ns2`, … avoiding every prefix in scope, and emit the declaration. For an **element** name the writer may instead use a default declaration `xmlns="ns"`; it does so only when no default binding is in scope. |
| empty | non-empty | `TXML_ERR_INVALID`: a prefix cannot name the empty namespace. |

**When a declaration is decided.** The writer does not emit a declaration
while writing the start tag's name. It records what bindings the name and each
attribute need and settles them when the start tag closes, so an `xmlns`
attribute the caller writes explicitly satisfies the need and no second
declaration is invented. This is what makes `txml_write_subtree` of a parsed
document byte-exact: the declarations stored as attributes are written back in
their original order and positions, and nothing is added. It is also why the
element stack copies the URIs it may have to declare.

An attribute is never put in the default namespace: an attribute with a
non-empty `ns` always gets an explicit prefix, because an unprefixed attribute
name has no namespace by definition.

Writing an attribute whose expanded name duplicates one already written on the
same tag is `TXML_ERR_INVALID`.

## Escaping

| Context | Escaped |
|---|---|
| Text | `&` → `&amp;`, `<` → `&lt;`, `>` → `&gt;` (always, so `]]>` cannot appear), `\r` → `&#13;` (so it survives line-end normalization on re-parse) |
| Attribute value | the above, plus `"` → `&quot;` (the writer always double-quotes), `\t` → `&#9;`, `\n` → `&#10;`, `\r` → `&#13;` (so attribute-value normalization cannot alter them) |
| Comment | nothing is escapable: `--` in the text, or a trailing `-`, is `TXML_ERR_INVALID` |
| CDATA | nothing is escaped; an occurrence of `]]>` is **split** into `]]]]><![CDATA[>`, which re-parses to the same characters |
| PI | nothing is escaped; `?>` in the data is `TXML_ERR_INVALID`, as is a target of `xml` in any case combination |

`txml_write_pi` takes the target and the data as two `txml_str` values. The
target must be an unqualified XML name; the data is written verbatim.

Every string the caller passes is validated as UTF-8 within the XML `Char`
production. A violation is `TXML_ERR_INVALID`; the writer never emits bytes it
could not read back.

`TXML_WRITE_INDENT` inserts a newline and two spaces per depth before a start
tag and before an end tag. It is suppressed for the whole of an element that
received any text, because indentation there would change the content.

## The output protocol

The writer fills `config.output` and never allocates for output. Every emit
call returns `TXML_OK` or `TXML_NEED_SPACE`:

```c
static txml_result pump(txml_writer *w, txml_result r, FILE *out) {
    while (r == TXML_NEED_SPACE) {
        txml_bytes b = txml_writer_output(w);
        fwrite(b.ptr, 1, b.length, out);
        r = txml_writer_drain(w);
    }
    return r;
}
```

- `txml_writer_output` reports the bytes produced and not yet consumed. They
  are valid until the next `txml_writer_drain`.
- `txml_writer_drain` marks them consumed, resets the buffer, and **continues
  the interrupted call**. It may return `TXML_NEED_SPACE` again, as many times
  as the input needs.
- Calling any emit function while an operation is pending is
  `TXML_ERR_INVALID`. The only legal calls between `TXML_NEED_SPACE` and
  `TXML_OK` are `txml_writer_output`, `txml_writer_drain` and
  `txml_writer_destroy`.

**The borrowed-input rule.** A `txml_str` handed to a call that returned
`TXML_NEED_SPACE` must stay valid until the drain sequence for that call
returns `TXML_OK`. The writer stores a pointer and an offset into the caller's
string rather than copying it — that is what makes writing a 100 MiB text node
through a 4 KiB buffer allocation-free.

The pending-operation record is:

```text
{ operation, phase, source pointer, source offset, pending escape bytes }
```

`pending escape bytes` is a tiny fixed buffer (at most 6 bytes) holding a
partially written entity such as `&quot;` when the output buffer filled
mid-escape.

`txml_writer_finish` returns `TXML_ERR_INVALID` when an element is still open
or an operation is pending. It emits nothing: bytes already produced stay in
the output for a final `txml_writer_output`.

## Resumable subtree walk

`txml_write_subtree(w, cursor)` walks `[index, end)` iteratively — the writer's
element stack is the only stack involved, so a 10 000-deep document costs no C
stack:

```text
i = index
while i < end:
    while stack top has subtree_end == i: write_end(); pop
    emit node i according to its kind
    i += 1
while stack deeper than at entry: write_end(); pop
```

Because each frame records the source node's `end`, closing happens by
comparing integers; no separate "pending end tags" structure exists.

The walk position is stored in the writer as:

```text
{ document, current node index, bound, phase, attribute k, text offset }
```

with `phase` one of `start tag`, `attribute k`, `text`, `end tag`. A drain
resumes at exactly that phase. So a subtree write can be interrupted between
any two bytes and continued later, and the caller's only obligation is to keep
the document alive, since every string the walk emits is borrowed from it.

`txml_write_subtree` on a `TXML_NODE_DOCUMENT` cursor writes the node's
children and no wrapper. On a node that is still `TXML_NODE_OPEN` it returns
`TXML_ERR_INVALID`: an incomplete subtree has no `end`, so the walk has no
bound.

## Status

Implemented in `src/writer.c`: the whole event vocabulary, escaping, prefix
selection and deferred declarations, indentation, the resumable output
protocol, and `txml_write_subtree` for every node kind (declaration, doctype,
comments, PIs, CDATA, text at document level); attribute prefixes come from
`txml_attribute_view.prefix`, the same record a consumer reads (ADR 0020). `writer_test` and the
`@roundtrip` half of the spec corpus pass; a mutation fuzz over parse, write
with 1-, 7- and 4096-byte buffers, re-parse and re-write finds the output
identical across buffer sizes and a fixed point after one round trip.

How it is built, where that differs from the sections above:

- **Operations, not re-entrant calls.** An emit call does all its state
  changes (frames, bindings, needs) when it is made and compiles the bytes it
  owes into a short segment queue; only byte production is deferred. A drain
  resumes at the exact segment, byte and half-written escape. The pending
  escape buffer is 16 bytes, not 6, because a split CDATA terminator
  (`]]]]><![CDATA[>`) is written through it.
- **Names are copied at the call.** A start tag's name is copied into the
  element frame, an attribute's name and prefix into the tag's storage, so
  only *values and text* obey the borrowed-input rule. The frame's storage is
  released when the operation that closes the element completes, not when the
  call returns, so a pending end tag never reads freed memory.
- **Document-driven names are exact.** `txml_write_subtree` passes each
  element's and attribute's own prefix (ADR 0020) and an empty prefix means
  unprefixed. Declarations then appear only where the document had them, and
  are added only when a subtree root's ancestors declared what it needs.
  Attributes flagged `TXML_ATTR_DEFAULTED` are skipped: they were not written.
- **`<a/>` versus `<a></a>`.** A document node flagged `TXML_NODE_EMPTY`
  is written self-closed and one that is not is written as a pair, whatever
  `TXML_WRITE_EMPTY_TAGS` says; the flag governs event producers only.
- **An invented attribute prefix is declared immediately** before the
  attribute (`<a xmlns:ns1="u" ns1:k="v"/>`, as `writer_test` requires), since
  the attribute's text needs the prefix now. Declarations needed by an
  element's name or by an explicitly prefixed attribute are settled when the
  tag closes, so a later explicit `xmlns` attribute satisfies them.
- **After `TXML_ERR_NOMEM` from a call that had already settled the previous
  tag, the writer is unusable** and later calls return `TXML_ERR_INVALID`;
  destroy it.
- `TXML_WRITE_INDENT` also suppresses indentation in every descendant of an
  element that received text, and writes a newline after a declaration it
  emits itself.

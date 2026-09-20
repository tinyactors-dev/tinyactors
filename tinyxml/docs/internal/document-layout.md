# Document layout

A document is columns of fixed-size entries plus one byte pool, paged over the
document's two arenas (`memory-architecture.md`). Nothing in a column is a
pointer: every reference is a `uint32_t` index or offset. That is what makes a
document relocatable in principle, serializable later, and safe to hand to a
binding as plain integers.

Which arena holds which column follows the lifetime split of ADR 0016:

| Arena | Holds |
|---|---|
| values | the pool, the names column, the namespaces column |
| tree | the nodes column, the attributes column, the positions column |

## Columns

### nodes — 24 bytes

| Field | Type | Meaning |
|---|---|---|
| `kind` | `uint8_t` | a `txml_node_kind` value |
| `flags` | `uint8_t` | `TXML_NODE_EMPTY`, `TXML_NODE_CDATA`, `TXML_NODE_OPEN` |
| `depth` | `uint16_t` | 0 for the document node, 1 for the document element |
| `parent` | `uint32_t` | parent index; `TXML_INDEX_NONE` for node 0 |
| `end` | `uint32_t` | one past the last descendant; `TXML_INDEX_NONE` while open |
| `name` | `uint32_t` | name ID for `TXML_NODE_ELEMENT` and `TXML_NODE_PI`; `TXML_NAME_NONE` otherwise |
| union `element` | `uint32_t first_attribute, attribute_count` | `TXML_NODE_ELEMENT` and `TXML_NODE_DECLARATION`; `TXML_NODE_DOCUMENT` (count 0) |
| union `text` | `uint32_t offset, length` | `TXML_NODE_TEXT`, `TXML_NODE_COMMENT`, `TXML_NODE_PI` (the PI's data), `TXML_NODE_DOCTYPE` |

`depth` is a `uint16_t`, so `limits.max_depth` above 65535 is rejected with
`TXML_ERR_INVALID` at `txml_parser_init`.

Every construct in the input is a node (ADR 0017), so the union each kind uses
is worth stating outright:

| Kind | `name` | Payload |
|---|---|---|
| `TXML_NODE_DOCUMENT` | none | `element`, with `attribute_count == 0` |
| `TXML_NODE_ELEMENT` | the expanded name | `element` |
| `TXML_NODE_TEXT` | none | `text`: the normalized run, whitespace included |
| `TXML_NODE_COMMENT` | none | `text`: the comment body |
| `TXML_NODE_PI` | the target | `text`: the PI data |
| `TXML_NODE_DECLARATION` | none | `element`: `version`, `encoding` and `standalone` are ordinary attributes in namespace 0 |
| `TXML_NODE_DOCTYPE` | none | `text`: the declaration as written |

The declaration borrows the attribute machinery rather than growing accessors
of its own, so `txml_first_attribute` and `txml_find_attribute` read it exactly
as they read an element.

**`end` is the only field ever rewritten.** It is written exactly once:

- at append time for a leaf (text, comment, PI, declaration, doctype) and for
  an empty-element tag (`<a/>`), which is therefore never `TXML_NODE_OPEN`;
- when the matching end tag is parsed, for an element written with a separate
  end tag. Between append and that moment the node carries `TXML_NODE_OPEN`
  and `end == TXML_INDEX_NONE`.

Every other field is final the moment the node is appended.

### attributes — 12 bytes

| Field | Type | Meaning |
|---|---|---|
| `name` | `uint32_t` | name ID |
| `value_offset` | `uint32_t` | pool offset of the normalized value |
| `value_length` | `uint32_t` | value length in bytes |

Attribute flags (`TXML_ATTR_NS_DECL`, `TXML_ATTR_DEFAULTED`) are not stored per
attribute. `TXML_ATTR_NS_DECL` is derived: the attribute's name lies in the
XMLNS namespace (see below). `TXML_ATTR_DEFAULTED` is derived from the
internal-subset default table, which every document keeps, because the subset
is always read (ADR 0017). `txml_attribute_read` computes both.

The attributes of one element are **contiguous**: `first_attribute` through
`first_attribute + attribute_count - 1`, in document order, declarations
included and in place. The same holds for the pseudo-attributes of a
`TXML_NODE_DECLARATION`.

A namespace declaration's value is not stored twice: its `value_offset` and
`value_length` point at the pool run that already holds the interned namespace
URI. Reading `xmlns="…"` as an attribute therefore yields exactly the URI
bytes in the namespaces column.

### names — 24 bytes

| Field | Type | Meaning |
|---|---|---|
| `namespace` | `uint32_t` | namespace ID; 0 means no namespace |
| `local_offset` | `uint32_t` | pool offset of the local part |
| `local_length` | `uint32_t` | local part length |
| `prefix_offset` | `uint32_t` | pool offset of the prefix as first written |
| `prefix_length` | `uint32_t` | prefix length; 0 when there was none |
| `hash` | `uint32_t` | FNV-1a over `namespace URI`, `0xFF`, `local`; stored so a hash-index rebuild never re-reads the pool |

Identity is `(namespace, local)` only. The prefix is recorded for round
tripping and for `txml_node_prefix`, and is the one the name was **first**
written with; a later element that spells the same expanded name with a
different prefix shares the name ID and does not change the recorded prefix.

Name IDs are dense from 0 in interning order. `txml_doc_name_count` is the
count; `txml_doc_name_at` resolves one; `txml_doc_find_name` hashes and
probes.

### namespaces — 8 bytes

| Field | Type | Meaning |
|---|---|---|
| `uri_offset` | `uint32_t` | pool offset of the URI |
| `uri_length` | `uint32_t` | URI length; 0 only for ID 0 |

**Namespace ID 0 is the empty URI** and always exists, whether or not the
document uses namespaces. An unprefixed element name in scope of no default
declaration has namespace 0.

**Unprefixed attributes have namespace 0 always**, even under a default
`xmlns`. This is Namespaces in XML §6.2, and it is why `txml_find_attribute`
on `id` uses a name whose `ns` is empty.

A **namespace declaration** is interned with the name exactly as written, in
the XMLNS namespace `http://www.w3.org/2000/xmlns/`:

| Written | Interned name |
|---|---|
| `xmlns="…"` | `ns = http://www.w3.org/2000/xmlns/`, `local = "xmlns"`, prefix empty |
| `xmlns:s="…"` | `ns = http://www.w3.org/2000/xmlns/`, `local = "s"`, prefix `"xmlns"` |

This gives declarations a stable identity, keeps them inside the ordinary
attribute machinery so they round trip in place, and lets a consumer skip them
with one flag test.

### positions — 16 bytes

`uint64_t offset`, `uint32_t line`, `uint32_t column`, parallel to `nodes`:
exactly one entry per node, always. The position is that of the node's first
byte — `<` for an element, comment, PI, declaration or doctype, and the first
character of the run for text. `txml_node_position` succeeds for every node of
every document (ADR 0017); it reports `TXML_ERR_INVALID` only for a cursor
that is not on a node at all.

The column is 16 bytes against the node's 24, so positions are two thirds
again on top of the node column. They are a separate column, not a wider node
record: the scan loops in `cursor.md` still read 24-byte nodes.

### pool

Raw bytes in the values arena: normalized text, normalized attribute values,
name local parts, prefixes, and namespace URIs. Pool bytes are never rewritten
after the run that produced them is closed.

## Worked example

Nothing is dropped, so the indentation in this document is part of it:

```xml
<scxml xmlns="http://www.w3.org/2005/07/scxml" initial="closed">
  <state id="closed">
    <transition event="open" target="opening"/>
  </state>
  <state id="opening">
    <transition event="socket.open" target="open"/>
    <transition event="error.communication" target="closed"/>
  </state>
</scxml>
```

303 bytes, including the newline after `</scxml>`.

### nodes

| index | kind | depth | parent | end | name | first_attribute | attribute_count | flags |
|---|---|---|---|---|---|---|---|---|
| 0 | DOCUMENT | 0 | NONE | 16 | NONE | 0 | 0 | — |
| 1 | ELEMENT `scxml` | 1 | 0 | 15 | 0 | 0 | 2 | — |
| 2 | TEXT `"\n  "` | 2 | 1 | 3 | NONE | — | 0 | — |
| 3 | ELEMENT `state` | 2 | 1 | 7 | 3 | 2 | 1 | — |
| 4 | TEXT `"\n    "` | 3 | 3 | 5 | NONE | — | 0 | — |
| 5 | ELEMENT `transition` | 3 | 3 | 6 | 5 | 3 | 2 | EMPTY |
| 6 | TEXT `"\n  "` | 3 | 3 | 7 | NONE | — | 0 | — |
| 7 | TEXT `"\n  "` | 2 | 1 | 8 | NONE | — | 0 | — |
| 8 | ELEMENT `state` | 2 | 1 | 14 | 3 | 5 | 1 | — |
| 9 | TEXT `"\n    "` | 3 | 8 | 10 | NONE | — | 0 | — |
| 10 | ELEMENT `transition` | 3 | 8 | 11 | 5 | 6 | 2 | EMPTY |
| 11 | TEXT `"\n    "` | 3 | 8 | 12 | NONE | — | 0 | — |
| 12 | ELEMENT `transition` | 3 | 8 | 13 | 5 | 8 | 2 | EMPTY |
| 13 | TEXT `"\n  "` | 3 | 8 | 14 | NONE | — | 0 | — |
| 14 | TEXT `"\n"` | 2 | 1 | 15 | NONE | — | 0 | — |
| 15 | TEXT `"\n"` | 1 | 0 | 16 | NONE | — | 0 | — |

Node 15 is the newline after `</scxml>`. Whitespace in the prolog and epilog
is a text child of the document node like any other text; the old rule that
made it disappear is gone with the retention flags.

Read off the table: node 3 owns `[3, 7)`, so it has `7 - 3 - 1 = 3`
descendants — a text node, a transition, and another text node. Node 8 owns
`[8, 14)`: five. Node 1 owns `[1, 15)`: thirteen. Node 5's `end` is `5 + 1`,
because an empty-element tag closes at append. Node 13's `end` (14) equals its
parent's `end` (14), which is exactly why `txml_next_sibling` reports
`TXML_NONE` for it.

Seven of these sixteen nodes are the structure an SCXML importer cares about;
the other nine are whitespace. That ratio is the cost ADR 0017 accepted in
exchange for one tree per document, and it is why an importer walks with
`txml_next_element` and `txml_find_next`, which skip every non-element in one
call.

### attributes

| index | name | value |
|---|---|---|
| 0 | 1 (`xmlns`) | `http://www.w3.org/2005/07/scxml` |
| 1 | 2 (`initial`) | `closed` |
| 2 | 4 (`id`) | `closed` |
| 3 | 6 (`event`) | `open` |
| 4 | 7 (`target`) | `opening` |
| 5 | 4 (`id`) | `opening` |
| 6 | 6 (`event`) | `socket.open` |
| 7 | 7 (`target`) | `open` |
| 8 | 6 (`event`) | `error.communication` |
| 9 | 7 (`target`) | `closed` |

### names

Interning order is: the element name first (declarations on its start tag are
resolved before it), then its attributes in document order. Text nodes intern
nothing, so the whitespace changes none of this.

| id | namespace | local | prefix |
|---|---|---|---|
| 0 | 1 | `scxml` | — |
| 1 | 2 | `xmlns` | — |
| 2 | 0 | `initial` | — |
| 3 | 1 | `state` | — |
| 4 | 0 | `id` | — |
| 5 | 1 | `transition` | — |
| 6 | 0 | `event` | — |
| 7 | 0 | `target` | — |

### namespaces

| id | URI |
|---|---|
| 0 | *(empty: no namespace)* |
| 1 | `http://www.w3.org/2005/07/scxml` |
| 2 | `http://www.w3.org/2000/xmlns/` |

### Size

Columns, by arena:

| Arena | Column | Bytes |
|---|---|---|
| tree | 16 nodes × 24 | 384 |
| tree | 10 attributes × 12 | 120 |
| tree | 16 positions × 16 | 256 |
| values | 8 names × 24 | 192 |
| values | 3 namespaces × 8 | 24 |

760 bytes in the tree arena and 216 in the values arena, plus **204 bytes** of
pool: 45 for the eight local parts, 60 for the two non-empty URIs, 70 for the
attribute values other than the declaration (which reuses the URI run), and 29
for the nine whitespace runs. The source document is 303 bytes. Pages and
chunks round all of this up; `txml_doc_arena(doc, which)->used` reports the
truth for each arena.

A consumer looking for every transition resolves `{scxml-ns, "transition"}` to
name 5 once, then scans the name column of `[1, 15)` for the value 5. It finds
nodes 5, 10 and 12 with three integer comparisons per candidate and no string
work at all — the nine whitespace nodes cost one `uint32_t` compare each and
nothing more.

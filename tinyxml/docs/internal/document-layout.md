# Document layout

A document is columns of fixed-size entries plus one byte pool, all paged over
the document arena (`memory-architecture.md`). Nothing in a column is a
pointer: every reference is a `uint32_t` index or offset. That is what makes a
document relocatable in principle, serializable later, and safe to hand to a
binding as plain integers.

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
| union `element` | `uint32_t first_attribute, attribute_count` | `TXML_NODE_ELEMENT` and `TXML_NODE_DOCUMENT` (count 0) |
| union `text` | `uint32_t offset, length` | `TXML_NODE_TEXT`, `TXML_NODE_COMMENT`, `TXML_NODE_PI` (the PI's data) |

`depth` is a `uint16_t`, so `limits.max_depth` above 65535 is rejected with
`TXML_ERR_INVALID` at `txml_parser_init`.

**`end` is the only field ever rewritten.** It is written exactly once:

- at append time for a leaf (text, comment, PI) and for an empty-element tag
  (`<a/>`), which is therefore never `TXML_NODE_OPEN`;
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
internal-subset default table, which the document keeps only when
`TXML_INTERNAL_SUBSET` is set. `txml_attribute_read` computes both.

The attributes of one element are **contiguous**: `first_attribute` through
`first_attribute + attribute_count - 1`, in document order, declarations
included and in place.

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

### positions — 16 bytes, optional

`uint64_t offset`, `uint32_t line`, `uint32_t column`, parallel to `nodes` and
present only when the document was parsed with `TXML_KEEP_POSITIONS`. The
position is that of the node's first byte: `<` for an element, comment or PI,
and the first character of the run for text. `txml_node_position` returns
`TXML_ERR_NOT_FOUND` when the column is absent.

### pool

Raw bytes: normalized text, normalized attribute values, name local parts,
prefixes, and namespace URIs. Pool bytes are never rewritten after the run
that produced them is closed.

## Worked example

Parsed with `TXML_SKIP_BLANK_TEXT` (the flag an SCXML importer would use):

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

### nodes

| index | kind | depth | parent | end | name | first_attribute | attribute_count | flags |
|---|---|---|---|---|---|---|---|---|
| 0 | DOCUMENT | 0 | NONE | 7 | NONE | 0 | 0 | — |
| 1 | ELEMENT `scxml` | 1 | 0 | 7 | 0 | 0 | 2 | — |
| 2 | ELEMENT `state` | 2 | 1 | 4 | 3 | 2 | 1 | — |
| 3 | ELEMENT `transition` | 3 | 2 | 4 | 5 | 3 | 2 | EMPTY |
| 4 | ELEMENT `state` | 2 | 1 | 7 | 3 | 5 | 1 | — |
| 5 | ELEMENT `transition` | 3 | 4 | 6 | 5 | 6 | 2 | EMPTY |
| 6 | ELEMENT `transition` | 3 | 4 | 7 | 5 | 8 | 2 | EMPTY |

Read off the table: node 2 owns `[2, 4)`, so it has `4 - 2 - 1 = 1`
descendant. Node 4 owns `[4, 7)`. Node 1 owns `[1, 7)`: five descendants.
Node 3's `end` is `3 + 1`, because an empty-element tag closes at append.
The last transition's `end` (7) equals its parent's `end` (7), which is
exactly why `txml_next_sibling` reports `TXML_NONE` for it.

Without `TXML_SKIP_BLANK_TEXT` the same document yields 15 nodes: eight
whitespace-only text nodes interleave with these, and every `end` and `parent`
shifts accordingly.

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
resolved before it), then its attributes in document order.

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

7 × 24 + 10 × 12 + 8 × 24 + 3 × 8 = 168 + 120 + 192 + 24 = **504 bytes** of
columns, plus **176 bytes** of pool: 45 for the eight local parts, 60 for the
two non-empty URIs, and 71 for the attribute values other than the declaration
(which reuses the URI run). The source document is 303 bytes. Pages and
chunks round those numbers up; `txml_doc_arena(doc)->used` reports the truth.

A consumer looking for every transition resolves `{scxml-ns, "transition"}` to
name 5 once, then scans the name column of `[1, 7)` for the value 5. It finds
nodes 3, 5 and 6 with three integer comparisons per candidate and no string
work at all.

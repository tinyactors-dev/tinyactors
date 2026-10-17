# Memory architecture

Everything in txml rests on one rule: **an allocation keeps its address until
its owner is destroyed.** No `realloc`, no compaction pass, no pointer
fix-ups. That rule is what lets a document hand out `txml_str` views into its
own storage and promise they stay valid, and it is why the allocator vtable
has only `alloc` and `free`.

## The arena

`txml_arena` is a bump allocator over a singly linked list of chunks. The
arena header is a plain struct the caller can embed; the chunks are opaque
(`txml_arena_chunk`).

```text
txml_arena
  backing ──▶ txml_allocator (embedder)
  head    ──▶ [chunk C] ──prev──▶ [chunk B] ──prev──▶ [chunk A] ──▶ NULL
              ^ bump pointer here
  chunk_size  minimum payload of the next chunk
  used        bytes handed to callers
  reserved    bytes obtained from the backing allocator
```

Each chunk carries its own payload size and bump offset. `txml_arena_alloc`
rounds the bump offset up to the requested alignment (a power of two) and
returns the space if it fits in `head`.

**Growth.** When the request does not fit, a new chunk is linked in front.
The new chunk's payload is `chunk_size`, and `chunk_size` then doubles, capped
at 1 MiB. A fresh arena starts at `TXML_ARENA_CHUNK_DEFAULT` (64 KiB) unless
`txml_arena_init` was given a size. Doubling keeps the chunk count
logarithmic in document size; the cap keeps a large document from asking the
backing allocator for one enormous block.

**Oversized requests.** A request larger than the chunk payload gets a chunk
of its own, sized to exactly that request plus the chunk header. It is linked
in *behind* `head`, so the current chunk's remaining space stays usable. This
is what makes every single allocation contiguous, including a text run longer
than a chunk.

**Waste.** The tail of a chunk that could not satisfy a request is abandoned.
Per-chunk waste is bounded by the largest alignment plus the failed request
size; total waste is bounded by the chunk count, which is logarithmic.

**Marks.** `txml_arena_position` captures `{head, bump offset}`.
`txml_arena_release(arena, mark)` frees every chunk newer than `mark.chunk`,
restores the bump offset inside `mark.chunk`, and makes `mark.chunk` the head
again. One freed chunk is retained as a spare and reused for the next growth,
so a mark/release loop in steady state performs no backing allocation at all.
Releasing to a mark whose chunk has already been freed is undefined; callers
release in stack order.

`txml_arena_reset` frees every chunk (keeping one spare) and returns the arena
to its initial state. `txml_arena_destroy` frees everything including the
spare.

## Paged columns

A document is seven columns (the last two, the prefix exceptions of ADR 0020, are sparse), each addressed by a dense `uint32_t` index. A
column is a **page directory** plus fixed-size pages allocated from whichever
of the document's two arenas owns it (ADR 0016) — the tree arena for the node,
attribute and position columns, the values arena for the name and namespace
columns:

```c
entry i  ──▶  directory[i >> page_shift][i & page_mask]
```

The directory is an array of pointers allocated **once**, at document
initialization, sized from `limits.max_document_bytes`:

```text
directory_capacity = max_document_bytes / (entries_per_page * entry_size) + 1
```

Because the directory covers the largest document the limits allow, it never
grows, so it never moves either. Pages are allocated lazily, one at a time, as
indices are used.

Default page sizes: 4096 entries for the node, attribute, name, namespace and
position columns. For the defaults in `TXML_LIMITS_DEFAULT` that is a 96 KiB
node page and a 683-pointer directory (5.5 KiB); for an MCU profile with
`max_document_bytes = 64 * 1024` the directory is a single pointer.

**The pool** lives in the values arena. It holds bytes rather than entries and
is paged the same way: a pool offset is a global byte offset, and the byte
lives at
`pool_directory[offset >> pool_shift] + (offset & pool_mask)`. The pool page
size is `chunk_size` clamped to `[1024, 65536]` and rounded down to a power of
two.

Unlike a column's, the pool's directory grows with the document. A small page
would make a directory sized for `max_document_bytes` large (16,386 pointers
for the default limits at 4096 B pages), and setting it up would cost every
parse. It starts at 16 slots and doubles when a page past its end is needed,
up to `max_document_bytes / page + 2`: the new array, in the values arena,
takes the page pointers, and the old one is abandoned there, counted in
`used`. The pages do not move, so no offset or `txml_str` changes; only the
index to them is copied.

Three rules keep pool references usable:

1. **A string never spans a page.** If a run does not fit in the remainder of
   the current page, that remainder is abandoned and the run starts a fresh
   page. Callers therefore always get one contiguous `txml_str`.
2. **An oversized run reserves consecutive directory slots.** A run larger
   than a page is allocated as a single arena block covering *n* whole pages,
   and slots *k..k+n-1* point at `base + k * page_size`. Offset arithmetic
   stays uniform and the run stays contiguous.
3. **A run whose length is not known moves as it grows.** Text that streams in
   (ADR 0021) is built through a *run*: it starts in the tail of the current
   page and, when it outgrows its block, moves to a fresh block of whole
   pages at least twice the size, copying what it holds. The old space is
   abandoned but stays counted in the values arena's `used`, so a run costs at
   most about twice its final size against `max_document_bytes`. This is not
   relocation in the sense of the rule above: a pending run is invisible, so
   no offset, index or `txml_str` refers to it until it is finished, and
   nothing a reader holds moves. There is at most one pending run, and nothing
   else appends to the pool while it is.

Pool offsets are `uint32_t` and address the values arena alone, so the pool is
limited to 4 GiB. A `max_document_bytes` above `UINT32_MAX` is rejected with
`TXML_ERR_INVALID` at `txml_parser_init`.

## Who owns what

| | Parser (transient) | Document (the product) |
|---|---|---|
| Storage | three arenas of its own, created over `config.allocator`: one for the window, buffers and DTD tables, one for open-element names and namespace bindings (released in LIFO order), one for the names of the start tag being read | **two** arenas, values and tree, both created over `config.allocator` |
| Holds | staging window, scanner state, open-element stack, start-tag records and names, namespace binding stack, the small buffer, the DTD tables | values: the pool and the name and namespace columns. tree: the node, attribute, position and prefix-exception columns |
| Sizing | fixed from `config` and `limits`, except the binding stack and hash index | grows with the document; the **sum** of the two arenas' `used` is capped by `max_document_bytes` |
| Freed by | `txml_parser_finish` (after handing the document over) or `txml_parser_destroy` | `txml_doc_destroy`, or `txml_parser_destroy` when the document was never detached — both arenas, unconditionally |

**Why the document has two.** The split is by lifetime, not by type. A name, a
prefix, a namespace URI and an attribute value are written once and live as
long as the document; node, attribute, position and prefix-exception entries are appended in
order and are, in principle, truncatable. One bump allocator tied the first
group's lifetime to the second's. The test for which arena something belongs
in is whether it could ever be discarded independently of the rest of the
document: if not, it is a value. `txml_doc_arena(doc, TXML_ARENA_VALUES)` and
`txml_doc_arena(doc, TXML_ARENA_TREE)` report each one, which separates what a
document spends on content from what it spends on structure — a document that
is mostly text and one that is mostly deeply nested empty elements are no
longer indistinguishable in a single number.

**The document owns both.** There is no borrowed arena and no conditional
ownership: the embedder supplies the allocator, the document creates its
arenas over it, and `txml_doc_destroy` destroys both. That is what lets every
`TXML_LIFETIME(document)` annotation in the header mean exactly one thing.
`txml_arena` remains public for the embedder's **own** data.

Keeping parser and document memory separate is what makes
`txml_doc_arena(doc, which)->used` mean "document bytes" and what makes
`max_document_bytes` a document limit rather than a process limit. Transient
parser state is bounded by the config alone:

```text
staging_length + 64          input window plus scanner padding
max_attributes * 16          pending attribute records for one start tag
max_name_length              name assembly buffer
max_depth * 8                open-element stack (node index, binding height)
```

The namespace binding stack and the name hash index grow with the document and
so are bounded by `max_names` and `max_document_bytes` rather than by a fixed
formula. The hash index is the one structure that is *rebuilt* on growth: it
holds name IDs, never pointers, nothing outside the parser references it, and
the superseded table is abandoned in the arena (total dead space stays below
the live table's size). Rebuilding an index is not relocation — no address
that was ever handed out changes.

## The path a byte takes

```text
caller chunk
   │  txml_parse_feed: copy, bounded by free staging space;
   │  validate as UTF-8 and Char, hold back a cut-off character
   ▼
staging window  [ unconsumed bytes ][ 64 bytes of padding ]
   │  the current state's run(): read what is readable
   ▼
normalize: expand entities and character references,
           CRLF and lone CR to LF, attribute-value whitespace to space
   │
   ▼
pool run in the values arena
   (text, attribute value, name local part, prefix, namespace URI)
   │
   ▼
name and namespace columns   (values arena)
node, attribute, position and prefix-exception columns   (tree arena)
```

The copy into the staging window is what frees the caller's chunk immediately
and what gives the scanner a padded buffer it may over-read by up to 64 bytes
without undefined behaviour — the property a future SIMD scanner needs. There
is no zero-copy path: the input is always copied (ADR 0022).

Names and attribute values are copied into the pool once, during the same
pass; nothing is copied a second time when a node is appended.

## Three memory profiles

**Server or desktop.** `txml_allocator` wraps `malloc`/`free`. Arena chunks
are allocated on demand; `TXML_STEP_NEED_MEMORY` appears only under real
memory pressure.

**Fixed budget (WASM, a game, a request quota).** The allocator is a bump
allocator over a static buffer, or one that enforces a quota. When it refuses,
`txml_parse_step` returns `TXML_STEP_NEED_MEMORY` and *the parse is not lost*:
the caller can extend the budget and call the step again, or destroy the
parser and reject the document. Nothing is half-written when a step returns
`TXML_STEP_NEED_MEMORY`.

**Long inputs (MCU).** A document that is a long sequence of records costs
what any other document costs: memory grows with the input, bounded by
`max_document_bytes`, and exceeding it ends the parse with `TXML_DOC_LIMIT`
and `TXML_LIMIT_DOCUMENT_BYTES`. ADR 0013 once offered mark-and-rewind to
reclaim finished records; ADR 0018 withdrew it for want of a consumer.

What remains is incremental **consumption** (ADR 0003), which is unchanged.
`txml_parser_doc` is readable from `txml_parser_init` onward, an element whose
end tag has not arrived carries `TXML_NODE_OPEN` with `txml_subtree_end ==
TXML_INDEX_NONE`, and a move that cannot yet be answered returns
`TXML_PENDING` rather than `TXML_NONE`. A consumer interleaves
`txml_parse_step` with cursor walks and processes each record as it closes; it
simply cannot drop the record afterwards. An embedder whose stream exceeds its
budget splits the input upstream and parses each record as its own document,
which costs a parser init per record and loses cross-record name interning.

Should reclamation ever return, the split above makes it sound in a way one
arena did not: releasing the tree arena to a mark cannot touch a byte that any
name, namespace or attribute value points at, because those bytes are not in
it.

## Lifetimes

These are the exact rules; the header states them per function.

| Thing | Valid from | Valid until |
|---|---|---|
| `txml_str` from any document call (`txml_node_text`, `txml_node_prefix`, `txml_doc_name_at`, `txml_attribute_read`, `txml_find_attribute`) | the call returns | `txml_doc_destroy` or `txml_parser_destroy` |
| `txml_cursor`, `txml_attr_cursor` | created | same as above; they are values and survive any amount of further parsing |
| Name IDs (`uint32_t`) | interned | the document is destroyed; never valid for another document |
| A chunk passed to `txml_parse_feed` | — | the call returns; the caller may reuse or free it immediately |
| `txml_str` passed to a writer call that returned `TXML_OK` | — | the call returns |
| `txml_str` passed to a writer call that returned `TXML_NEED_SPACE` | — | the drain sequence for that call finally returns `TXML_OK` |
| `txml_bytes` from `txml_writer_output` | the call returns | the next `txml_writer_drain` |
| `txml_document_error.message` | — | process lifetime; it is a static phrase |
| `txml_arena_mark` | `txml_arena_position` | the marked chunk is freed by a release to an older mark |

Continuing to parse **never** invalidates anything a document already handed
out. Only destruction does — there is no operation that shortens a document.

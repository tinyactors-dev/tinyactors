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

A document is five or six columns, each addressed by a dense `uint32_t` index.
A column is a **page directory** plus fixed-size pages allocated from the
arena:

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

**The pool** holds bytes rather than entries, and is paged the same way: a
pool offset is a global byte offset, and the byte lives at
`pool_directory[offset >> pool_shift] + (offset & pool_mask)`. The pool page
size is `chunk_size` clamped to `[1024, 65536]` and rounded down to a power of
two.

Two rules keep pool references usable:

1. **A string never spans a page.** If a run does not fit in the remainder of
   the current page, that remainder is abandoned and the run starts a fresh
   page. Callers therefore always get one contiguous `txml_str`.
2. **An oversized run reserves consecutive directory slots.** A run larger
   than a page is allocated as a single arena block covering *n* whole pages,
   and slots *k..k+n-1* point at `base + k * page_size`. Offset arithmetic
   stays uniform and the run stays contiguous.

Pool offsets are `uint32_t`, so a document arena is limited to 4 GiB. A
`max_document_bytes` above `UINT32_MAX` is rejected with `TXML_ERR_INVALID` at
`txml_parser_init`.

## Who owns what

| | Parser (transient) | Document (the product) |
|---|---|---|
| Storage | the parser's **own** arena, created over `config.allocator` | the document arena: `config.arena` when given, otherwise one the parser creates |
| Holds | staging window, lexer state, open-element stack, start-tag scratch, namespace binding stack, name hash index | node, attribute, name, namespace, position columns and the pool |
| Sizing | fixed from `config` and `limits`, except the binding stack and hash index | grows with the document, capped by `max_document_bytes` |
| Freed by | `txml_parser_finish` (after handing the document over) or `txml_parser_destroy` | `txml_doc_destroy`, or `txml_parser_destroy` when the document was never detached |

Keeping the two arenas separate is what makes `txml_doc_arena(doc)->used`
mean "document bytes" and what makes `max_document_bytes` a document limit
rather than a process limit. Transient parser state is bounded by the config
alone:

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

When `config.arena` is supplied, the document borrows it. `txml_doc_destroy`
releases the chunks the document allocated (via a mark taken at
initialization) but does not destroy the arena; the caller owns it before and
after.

## The path a byte takes

```text
caller chunk
   │  txml_parse_feed: copy, bounded by free staging space
   ▼
staging window  [ unconsumed bytes ][ 64 bytes of padding ]
   │  scan: find the next delimiter for the current lexer state
   ▼
normalize: expand entities and character references,
           CRLF and lone CR to LF, attribute-value whitespace to space
   │
   ▼
pool run (text, attribute value, name, namespace URI)
   │
   ▼
node / attribute / name / namespace columns
```

The copy into the staging window is what frees the caller's chunk immediately
and what gives the scanner a padded buffer it may over-read by up to 64 bytes
without undefined behaviour — the property a future SIMD scanner needs. A
caller that keeps its input alive and guarantees the same padding can set
`TXML_INPUT_PADDED`, and the scanner reads the caller's bytes in place.

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

**Streaming records (MCU).** For a document that is a long sequence of
records, the consumer processes one record subtree and then discards it:

```c
uint32_t mark = txml_doc_mark(doc);        /* before the record's start tag */
/* ... step until the record's subtree is closed, then consume it ... */
txml_doc_rewind(doc, mark);                /* nodes and arena memory reused */
```

`txml_doc_rewind` drops every node from `mark` on, truncates the attribute,
name, namespace and position columns to what those nodes used, and calls
`txml_arena_release` with the arena mark recorded for node `mark`. Peak memory
becomes the largest single record rather than the whole document. The rewind
is refused with `TXML_ERR_INVALID` while any element at or after `mark` is
still open, which means a consumer rewinds only to a point where the parser's
open-element stack has unwound to it.

Name IDs are *not* renumbered by a rewind: names interned by discarded nodes
keep their IDs and their pool bytes. This keeps the "dense from 0" invariant
and keeps every name ID a consumer captured still valid. It also means a
document of unique names is still bounded by `max_names`, as intended.

## Lifetimes

These are the exact rules; the header states them per function.

| Thing | Valid from | Valid until |
|---|---|---|
| `txml_str` from any document call (`txml_node_text`, `txml_node_prefix`, `txml_doc_name_at`, `txml_attribute_read`, `txml_find_attribute`) | the call returns | `txml_doc_destroy`, `txml_parser_destroy`, or a `txml_doc_rewind` to a mark at or before the owning node |
| `txml_cursor`, `txml_attr_cursor` | created | same as above; they are values and survive any amount of further parsing |
| Name IDs (`uint32_t`) | interned | the document is destroyed; never valid for another document |
| A chunk passed to `txml_parse_feed` | — | the call returns; the caller may reuse or free it immediately |
| A chunk passed to `txml_parse_feed` under `TXML_INPUT_PADDED` | — | the next `txml_parse_step` returns `TXML_STEP_NEED_INPUT` |
| `txml_str` passed to a writer call that returned `TXML_OK` | — | the call returns |
| `txml_str` passed to a writer call that returned `TXML_NEED_SPACE` | — | the drain sequence for that call finally returns `TXML_OK` |
| `txml_bytes` from `txml_writer_output` | the call returns | the next `txml_writer_drain` |
| `txml_document_error.message` | — | process lifetime; it is a static phrase |
| `txml_arena_mark` | `txml_arena_position` | the marked chunk is freed by a release to an older mark |

Continuing to parse **never** invalidates anything a document already handed
out. Only destruction and `txml_doc_rewind` do.

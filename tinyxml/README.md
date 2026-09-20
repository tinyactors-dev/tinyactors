# txml

txml is a freestanding XML reader and writer in C. It is the XML front end
that `tinyactors-core` deliberately does not have (core ADR 0001 and 0025):
small enough for an MCU, embeddable in the same hosts as the core, and usable
on its own.

It performs no I/O, uses no C library, allocates only through an
embedder-supplied allocator, and never moves what it has allocated.

## What it does

- **Parse** UTF-8 XML fed in byte chunks, with namespaces, into a concrete
  syntax tree: every construct in the input — comments, processing
  instructions, whitespace, the XML declaration, the DOCTYPE — is a node, held
  as index-addressed columns over the document's two arenas.
- **Navigate** that document with cursors, which are plain values. A subtree
  is the index range `[index, end)`, so an ancestor test is two comparisons
  and skipping a subtree is one jump.
- **Write** XML from events into a caller buffer, resumable when the buffer
  fills. Writing does not require a parsed document.
- **Read while parsing**: the document is navigable from the first step, an
  element whose end tag has not arrived is marked open, and a move that needs
  bytes nobody has fed yet says so instead of lying. A consumer can process
  each record as it closes.
- **Allocate** through the arena itself, which is public: the same
  linked-chunk bump allocator — with marks and release — is available for an
  embedder's own data. A document's own arenas belong to the document and go
  away with it.

## What it does not do

No validation, XPath, XInclude, or DTD beyond the internal subset, which is
always read. No external entities or external DTDs — they are never fetched,
which removes XXE by construction rather than defending against it. UTF-8
only; other encodings are transcoded by an adapter in front. Nothing reclaims
a parsed node, so a document costs what it contains, bounded by
`max_document_bytes`. See `docs/internal/conformance.md`.

## Shape of the API

```c
txml_parser *parser;
txml_parser_config config = { .allocator = heap, .limits = TXML_LIMITS_DEFAULT };
txml_parser_init(&parser, &config);

for (;;) {
    switch (txml_parse_step(parser)) {
    case TXML_STEP_NEED_INPUT: txml_parse_feed(parser, chunk, last); continue;
    case TXML_STEP_PROGRESS:   continue;
    case TXML_STEP_DONE:       goto done;
    default:                   goto failed;   /* NEED_MEMORY or FAILED */
    }
}
done:;
txml_doc *doc;
txml_parser_finish(parser, &doc);

uint32_t item = txml_doc_find_name(doc, &(txml_qname){ TXML_STR(""), TXML_STR("item") });
txml_find query = { .name = item, .bound = TXML_INDEX_NONE };
txml_cursor c = txml_doc_root(doc);
while (txml_find_next(&c, &query) == TXML_MOVED) {
    /* every <item> in document order */
}
```

## Status

Implemented: the arena, string helpers, document columns, the whole cursor read
side, a streaming parser (a table of resumable states, ADR 0021) including the
DOCTYPE and internal subset, and the writer. No construct has to fit in the
input window: text, attribute values, comments and DOCTYPEs of any size flow
through it. Known deviations are listed under "Status" in
`docs/internal/parser.md` and `docs/internal/writer.md`.

## Documentation

- `docs/adr/` — the design decisions, and the primary record of intent.
- `docs/internal/` — layout, invariants, and implementation reference.
- `AGENTS.md` — the constraints any change must respect.

## Tasks

```sh
mise run build   # compile the header checks and whatever can be linked
mise run test    # run the suite
mise run spec tests/spec/elements.spec
```

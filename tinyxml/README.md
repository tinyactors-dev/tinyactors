# txml

txml is a freestanding XML reader and writer in C. It is the XML front end
that `tinyactors-core` deliberately does not have (core ADR 0001 and 0025):
small enough for an MCU, embeddable in the same hosts as the core, and usable
on its own.

It performs no I/O, uses no C library, allocates only through an
embedder-supplied allocator, and never moves what it has allocated.

## What it does

- **Parse** UTF-8 XML fed in byte chunks, with namespaces, into a compact
  document: index-addressed columns in one arena.
- **Navigate** that document with cursors, which are plain values. A subtree
  is the index range `[index, end)`, so an ancestor test is two comparisons
  and skipping a subtree is one jump.
- **Write** XML from events into a caller buffer, resumable when the buffer
  fills. Writing does not require a parsed document.
- **Stream** a document of unbounded length in flat memory: mark a point,
  process the record that follows it, then rewind to the mark to drop those
  nodes and reuse their arena bytes. Peak memory is the largest record, not
  the whole input.
- **Allocate** through the arena itself, which is public: an embedder can
  hand the parser an arena it already owns, or use the same linked-chunk bump
  allocator — with marks and release — for its own data.

## What it does not do

No validation, XPath, XInclude, or DTD beyond the optional internal subset.
No external entities or external DTDs — they are never fetched, which removes
XXE by construction rather than defending against it. UTF-8 only; other
encodings are transcoded by an adapter in front. See
`docs/internal/conformance.md`.

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

The public header is authored; there is no implementation yet. `tests/` is the
specification an implementation will be written against: `mise run test`
reports the suite as pending until `src/` has modules, then runs it in full.

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

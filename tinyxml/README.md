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

## Command line

`cli/txml.c` is a small program (it uses libc; the library does not) that
parses files or standard input and prints the tree the way `tree-sitter parse
--cst` does: one line per node with its `row:col - row:col` range (0-based, byte
columns), its kind, and leaf text in backticks.

```sh
mise run txml -- [-S] [-s] [--color=WHEN] [FILE...]   # no FILE, or "-", reads stdin
echo '<a x="1">hi</a>' | mise run txml
# 0:0  - 1:0     document
# 0:0  - 0:15      element
# 0:1  - 0:2         name `a`
# 0:3  - 0:8         attribute
# 0:3  - 0:4           name `x`
# 0:6  - 0:7           value `1`
# 0:9  - 0:11        text `hi`
# 0:15 - 1:0       text `\n`
```

`-S` prints the S-expression form of plain `tree-sitter parse`, `-s` drops
whitespace-only text, and color is on for a terminal unless `NO_COLOR` is set.
Diagnostics go to stderr as `file:line:column: message`. Exit status is 0 on
success, 1 when a document is malformed, 2 for a usage or I/O error. `mise run
build:cli` builds `build/txml`.

Ranges come from the document itself: `txml_node_span` and
`txml_attribute_span_read` report where every node and attribute starts and ends
(ADR 0023), so the tool never re-reads its input and streams it through the
parser in chunks.

## Documentation

- `docs/adr/` — the design decisions, and the primary record of intent.
- `docs/internal/` — layout, invariants, and implementation reference.
- `AGENTS.md` — the constraints any change must respect.

## Tasks

```sh
mise run build          # compile and link the modules and the test binaries
mise run generate       # regenerate include/txml.c and txml.api.json after a change
mise run test           # the whole suite, including the checks below (seconds when little changed)
mise run spec tests/spec/elements.spec
mise run generated      # fail if a checked-in generated file is stale
mise run freestanding   # no libc, quick tier (src -O0 -O2 -Os, amalgamation -O2)
mise run freestanding:full  # both targets at all five levels, with a report; before a commit and in CI
mise run lint-src       # no recursion, at most three parameters per function
mise run amalgamate     # include/txml.c, the single-file library
mise run api            # include/txml.api.json, the machine-readable API
```

Checks skip by content, not modification time, so touching a file or editing a
comment costs a second, and only the objects and test binaries whose bytes
changed are rebuilt or rerun. `TXML_FORCE=1 mise run <task>` ignores every
cache; see AGENTS.md.

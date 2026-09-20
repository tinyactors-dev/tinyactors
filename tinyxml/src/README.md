Implementation modules. Headers here are private; the public API is
`../include/txml.h`.

- `arena.c`      linked-chunk arena and marks
- `text.{c,h}`   UTF-8 and XML `Char` checks (whole, and incremental for feeds), string helpers
- `doc.{c,h}`    document columns, the pool and its run builder, name interning, prefix exceptions
- `cursor.c`     navigation, ranges, and the read side of a node
- `parser.h`     the parser's state and what its modules share
- `parser.c`     construction, feeding (with validation), the step loop, positions, errors
- `decode.c`     sinks, and the reference / entity / line-end decoder
- `content.c`    text, `<` dispatch, comments, CDATA, PIs, the XML declaration
- `tag.c`        start tags and end tags
- `doctype.c`    the DOCTYPE and its internal subset: the state machine
- `subset.c`     the declarations of a DOCTYPE, read whole against the XML 1.0 grammar
- `tables.c`     the entity and ATTLIST tables
- `writer.c`     the event writer, escaping, resumable output

`internal.h` lists the private headers in dependency order for
`mise run amalgamate`, which inlines them and every `.c` into
`../include/txml.c`. That file is one translation unit, so a file-scope name
must mean one thing across all of `src/`.

The scanner is a table of resumable states (ADR 0021): see
`../docs/internal/parser.md`.

# ADR 0011: The writer takes events, not documents

**Status:** Accepted  
**Date:** 2026-09-20

## Context

An early sketch of this design gave the writer a document-shaped API: hand it a cursor and it serializes the subtree. That is the wrong dependency. Most XML that needs writing was never parsed. The SCXML exporter that motivates this library (core ADR 0042) emits XML from a compiled `ta_definition`; a writer that required a `txml_doc` would force it to build an entire tree and immediately discard it, spending memory to produce nothing.

The output side has its own problem: a writer that owns the destination needs I/O, which ADR 0001 forbids, and a writer that must fit its whole output in one buffer imposes an unknown allocation on the caller.

## Decision

The writer takes events. `txml_write_start`, `txml_write_attribute`, `txml_write_text`, `txml_write_cdata`, `txml_write_comment`, `txml_write_pi`, and `txml_write_end` are the whole vocabulary, and nothing in the writer's API mentions a document. It is a state machine over an element and namespace stack: it escapes text and attribute values, emits namespace declarations, and checks well-formedness — matching end tags, no duplicate attributes — as it goes.

Output goes into the caller's buffer, supplied in `txml_writer_config.output`. When that buffer fills, the emit call returns `TXML_NEED_SPACE`; the caller takes the bytes with `txml_writer_output`, disposes of them however it likes, and calls `txml_writer_drain`, which marks them consumed and *continues the interrupted call*, returning `TXML_OK` or `TXML_NEED_SPACE` again. Resuming without copying requires the writer to keep an offset into the caller's input, so text passed to a call that returned `TXML_NEED_SPACE` must stay valid until that call finally returns `TXML_OK`. `txml_writer_finish` verifies that every element was closed.

`txml_write_subtree` is the bridge from the reading side, not a foundation of it: it walks `[index, end)` (ADR 0005) and emits the same events a caller would, iteratively and with no extra memory beyond the writer's stack. It is resumable in the same way, because the writer remembers where in the walk it stopped.

Reader and writer nevertheless ship as one library and one compilation unit. The earlier argument for splitting them — a firmware target that only emits XML — does not hold: with `-ffunction-sections -fdata-sections` and `--gc-sections`, or with LTO, unused reader code is removed by the linker, and the eventual amalgamation (ADR 0014) gives private functions internal linkage. The separation that matters is in the API, not in the build.

## Consequences

Anything that can call four functions can produce XML: a statechart exporter, a config serializer, a test fixture generator. None of them needs a parsed document, and none of them allocates a tree.

Output is fully streamable with a fixed-size buffer — 4 KiB, or 1 byte — because the caller decides when and where bytes go. Combined with the parser's pull loop (ADR 0009), a full round trip runs in bounded memory with two small loops and no I/O inside the library.

Round-tripping preserves the XML Infoset rather than the original bytes. Names, prefixes, namespace declarations, attribute order, `<a/>` versus `<a></a>`, CDATA sections, and — with the keep flags — comments and PIs survive, because the document records them. Entity references, line endings, attribute whitespace, and quote style do not, because normalization already removed them (ADR 0008). The spec suite therefore tests two properties instead of byte equality: parsing the writer's output reproduces the same document, and writing that document again is byte-identical to the first output.

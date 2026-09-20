# ADR 0018: Streaming by mark and rewind is withdrawn

**Status:** Accepted  
**Date:** 2026-09-20  
**Supersedes:** [ADR 0013](0013-streaming-by-mark-and-rewind.md)

## Context

ADR 0013 added `txml_doc_mark` and `txml_doc_rewind` so a consumer of a record-shaped document could drop a finished record and parse the next one into the same memory. Writing the test suite against the header turned up three things.

It had no consumer. The library exists as an XML front end for an eventual SCXML importer (core ADR 0025), and a statechart is configuration-shaped: it is read once, whole, and then compiled. ADR 0013's own context named "a feed, an export, a log" as the audience, and nothing in this repository or the core asks for one.

Its documented loop could not be written. Both ADR 0013 and `memory-architecture.md` spell the mechanism as rewinding the document while the parser is still building it, but the only handle to that document is `txml_parser_doc`, which returns `const txml_doc *`, while `txml_doc_rewind` takes `txml_doc *`. `stream_test.c` had to cast the const away to exercise the feature as specified. Closing that gap meant either a second accessor or moving the rewind onto the parser — an API change carried entirely by a feature nobody had asked for.

And it did not agree with the memory layout. Under one arena, releasing to a mark freed the pool bytes of any name the discarded record had interned, contradicting ADR 0013's own promise that names survive a rewind and invariant 24 that repeated it. ADR 0016 fixes that by splitting the arenas, but it took a redesign of how a document stores everything to make two functions sound.

## Decision

`txml_doc_mark` and `txml_doc_rewind` are removed. ADR 0013 is superseded.

The other half of what "streaming" meant in this design is untouched. ADR 0003's incremental consumption stands exactly as written: `txml_parser_doc` is valid from `txml_parser_init` onward, an element whose end tag has not arrived carries `TXML_NODE_OPEN`, its `txml_subtree_end` is `TXML_INDEX_NONE`, and a move that cannot yet be answered returns `TXML_PENDING`. A consumer can still interleave `txml_parse_step` with cursor walks and process each subtree as it closes. What it cannot do is reclaim the memory afterwards.

## Consequences

The public surface is 58 functions and has no const gap. `txml_parser_doc` returning `const txml_doc *` is now simply true: nothing a caller may do to a document requires write access while the parser owns it.

The capability lost is real and worth stating plainly. A document that is a long sequence of records is now bounded by `max_document_bytes` like any other, and a log longer than that budget ends with a `TXML_DOC_LIMIT` document error instead of being consumed in flat memory. On a microcontroller that is the difference between processing a stream and refusing it. An embedder in that position today must split the input upstream and parse each record as its own document, which costs a parser init per record and loses cross-record name interning.

Restoring it later is additive: two functions, no change to anything already shipped, and no ADR to un-write beyond this one. ADR 0016's split arenas make it sound in a way it was not before — a rewind releases the tree arena to a mark and leaves the values arena untouched, so names and their bytes survive by construction rather than by a rule the implementation has to remember. The right time to add it back is when a consumer exists to say what the loop should look like, and that consumer can settle the const question in the same stroke.

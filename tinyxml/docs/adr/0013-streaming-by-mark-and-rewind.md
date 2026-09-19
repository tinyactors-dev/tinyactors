# ADR 0013: Streaming by mark and rewind

**Status:** Accepted  
**Date:** 2026-09-20

## Context

Because the parser's product is a document (ADR 0003), memory grows with the input. That is correct for a configuration file or an SCXML chart, and wrong for the other common shape: a long sequence of independent records — a feed, an export, a log — where the consumer handles one record and never looks at it again. A token API exists largely to serve that case, and ADR 0003 deliberately declined to add one.

The columns are append-only and the arena grows by linking chunks (ADR 0002), so the memory of a finished record is contiguous, at the end of everything, and trivially identifiable: it is every node from some index onward.

## Decision

`txml_doc_mark` returns the next node index — a point the caller can return to. After processing the subtree that followed it, the caller calls `txml_doc_rewind(doc, mark)`, which drops every node from `mark` on and releases their arena memory through `txml_arena_release`, resuming bump allocation at that point. Parsing then continues into the reclaimed space.

Two rules make this safe. Rewinding is refused with `TXML_ERR_INVALID` while any element at or after `mark` is still open, so a caller cannot discard a subtree the parser is still building. And rewinding invalidates every cursor and every `txml_str` at or after the mark: indices below it remain valid — that is the whole point — but anything the consumer kept from the discarded region is gone. This is the single operation in the library that can invalidate an index (ADR 0005), and it does so only when explicitly asked.

Name and namespace entries are not rewound. They are small, bounded by `max_names` (ADR 0010), and keeping them means a name ID resolved once stays valid for the whole stream.

## Consequences

A record-shaped document of unbounded length can be processed in flat memory, with the high-water mark set by the largest single record rather than by the total. On a microcontroller this is the difference between working and not working; the parser loop, the cursor API, and the consumer's code are otherwise unchanged.

The mechanism is explicit rather than automatic. The library never discards anything on its own, so a consumer that does not call `txml_doc_rewind` gets exactly the accumulate-everything behaviour it asked for, and one that does is stating precisely when the data stops being needed. There is no reachability analysis, no reference counting, and no way for the library to guess wrong.

The restriction to closed subtrees means a caller must drive the parse to the point where its record is complete before reclaiming, which the `TXML_NODE_OPEN` flag and `TXML_PENDING` moves (ADR 0003) make easy to detect.

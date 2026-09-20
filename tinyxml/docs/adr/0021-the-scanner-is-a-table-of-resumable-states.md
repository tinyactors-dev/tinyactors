# ADR 0021: The scanner is a table of resumable states

**Status:** Accepted  
**Date:** 2026-09-20  
**Amends:** [ADR 0008](0008-input-is-copied-into-a-padded-staging-window.md) (the window holds a chunk, not a construct) and [ADR 0009](0009-pausable-parsing-through-feed-step-and-budget.md) (what pausing has to save)

## Context

The first scanner looked for a construct's terminator inside the staging window and answered `TXML_STEP_NEED_INPUT` until it was there. That made every construct a whole-window problem: a tag, a comment, a text run or a DOCTYPE larger than `staging_length` could not be scanned, and the parser said so with `TXML_DOC_LIMIT` and no limit kind. Two documents claimed the opposite. `parser.md` promised that "names, attribute values and text are pushed to their destination as they are scanned, so a 10 MiB attribute value flows through a 64 KiB window without difficulty", and ADR 0008 sized the window for a chunk.

Nothing about the grammar needs the construct in memory. What it needs is to remember, across a boundary, the few things a boundary can cut: a partial delimiter, a `\r` waiting for its `\n`, an unfinished reference, a partial character. The obstacle was the destination, not the scanner. The pool promises that every `txml_str` is one contiguous run, so a run whose length is not known when it starts looked like it could not be built piecemeal.

## Decision

**The scanner is a table of resumable states.** A state is a struct holding two function pointers, `run` and `at_eof`. The parser points at the current state; `run` consumes whatever is readable, may change the pointer, and reports progress, a yielded budget, an allocation failure, a document error, or completion. There is no "incomplete" outcome: a state always consumes everything readable, and the step loop asks for input when nothing is left. Everything a state needs to continue lives in the parser — a held-back `-`, `]` or `?` count, an index into a fixed opener, the decoder's pending `\r`, `]` run or reference, the tag's phase — and no state keeps an index into the window across a call.

**Text streams into a pool run.** A run is contiguous and appended to piece by piece. When it outgrows its block it moves to a larger one, doubling, copying what it holds and abandoning the old space. That is sound because a pending run is invisible: no node or attribute refers to it before `finish`, so nothing a reader holds moves. This is a relocation of unpublished data, and ADR 0002's rule ("nothing relocates") is about published data. Abandoned bytes count against `max_document_bytes`, so the cost is bounded at roughly twice a run's final size.

**Validation moves to the feed.** The bytes a state may read are the whole, valid characters. `txml_parse_feed` validates as UTF-8 and as XML `Char`s, keeps `valid` and `bad` indices, and holds back a character cut off by the end of the chunk (at most three bytes) until its remainder arrives. A state therefore never sees half a character, and an invalid one is reported when the parser reaches it.

**Atomic per byte.** A state either consumes a byte and changes state for it, or does neither, and every multi-step ending (finish a run, intern a name, append a node) is idempotent. `TXML_STEP_NEED_MEMORY` is therefore retryable at every allocation, including in the middle of a construct.

**Small constructs are gathered whole.** The XML declaration, `<!ENTITY>` and `<!ATTLIST>` declarations, and a namespace URI are assembled in a growable transient buffer bounded by `max_document_bytes` and read from there, because they are small, structured, and (the last two) kept anyway. A start tag is a state machine over its own grammar; its names are bounded, and its values stream.

The step budget keeps its meaning: it is checked before a construct that appends, and an end tag never ends a step.

Two things changed with it. Character references in an entity's literal are now read at the declaration (XML 1.0 §4.5) and the rest at each reference. The DTD tables are hash tables, because a list made the cost of a parse the product of the entity count and the reference count, and of the `ATTLIST` count and the tag count.

## Consequences

`staging_length` sizes a chunk, not a construct. A document's constructs are limited only by `max_document_bytes`, and the failure that reported a construct larger than the window is gone. The window empties on every step, so the holdback is at most one character.

Pausing has to save more than ADR 0009 said. The document columns are still the only *result*, but the scanner's own state is now part of what a pause preserves. It is all in the parser, so a pause still costs nothing to save and nothing to restore.

The differential property that ADR 0009 named is now checked much harder: `tests/stream_test.c` parses every spec input and a corpus of awkward ones at feeds of 1, 2, 3 and 7 bytes and whole, through a 64-byte window and at several step budgets; splits each corpus input at every byte; refuses every allocation in turn; and pushes megabyte-scale text, attribute values, comments, CDATA sections, PIs and DOCTYPEs through a 64-byte window.

The costs are real. An error is now reported at the first byte that is wrong, so when a construct has both an encoding error and an earlier syntax error, the syntax error wins where the whole-construct scanner reported the encoding error. A run that grows by doubling leaves its old blocks behind. The small constructs are bounded by the document limit rather than by a constant. And an entity that expands to markup remains unsupported: the decoder emits text, not events, and the decision here does not change that.

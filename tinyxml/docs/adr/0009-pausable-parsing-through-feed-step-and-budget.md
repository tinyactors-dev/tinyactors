# ADR 0009: Pausable parsing through feed, step, and budget

**Status:** Accepted  
**Date:** 2026-09-20

## Context

A parser that owns its loop owns the caller's thread. Callback-based parsers such as Expat invert control: the embedder hands over a buffer and receives calls until it is consumed, with suspension as a bolted-on extra. That shape is painful in three of txml's target environments. Across an FFI boundary every callback is an expensive transition and a reentrancy hazard. In a cooperatively scheduled runtime — a game loop, an event loop, an RTOS task — a long parse must be interruptible at a known point. In WASM, callbacks into the host are the most awkward thing a library can require.

The core solved the analogous problem in ADR 0009 by making run-to-completion a semantic boundary rather than a C stack boundary, with `ta_actor_resume` returning `TA_RUN_PAUSED`.

## Decision

txml parses by pull. The caller stages bytes with `txml_parse_feed` (ADR 0008) and then calls `txml_parse_step`, which appends nodes and returns one of five outcomes:

- `TXML_STEP_PROGRESS` — the step budget was used up; call again.
- `TXML_STEP_NEED_INPUT` — feed more bytes, or feed with `final = true`.
- `TXML_STEP_NEED_MEMORY` — the allocator refused; extend it and call again (ADR 0002).
- `TXML_STEP_DONE` — the document is complete; `txml_parser_finish` detaches it.
- `TXML_STEP_FAILED` — not well-formed; read `txml_parser_error` (ADR 0007).

`txml_parser_config.step_budget` caps the nodes one step appends; 0 means "until the step blocks". A caller with a frame deadline sets a small budget and gets control back at a node boundary; a caller parsing a small configuration file sets 0 and loops twice.

No callback ever crosses the API. The caller supplies the CPU, exactly as it supplies the bytes and the memory.

## Consequences

The driving loop is a `switch` over five cases, and each of the three pause reasons has a distinct, obvious response. Nothing about the loop changes between a 200-byte document and a 200-megabyte one; only the number of iterations does.

Suspension is free rather than special: because a step appends whole nodes and the document columns are the only state, pausing costs nothing to save and nothing to restore. There is no continuation to encode, unlike the core's mid-macrostep case.

Bindings are straightforward — five enum values and no callbacks — and a WASM build needs no host function imports at all.

The spec suite exercises this directly: every case is run with several staging sizes and step budgets, including a one-byte window and a one-node budget, and must produce an identical document. That single property covers most of the boundary handling a chunked parser can get wrong.

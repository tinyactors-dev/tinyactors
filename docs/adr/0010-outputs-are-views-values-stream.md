# ADR 0010: Outputs are views; values stream

**Status:** Accepted (2026-10-12). Amends [ADR 0009](0009-one-host-two-queues.md),
decisions 3 (CBOR as token lists) and 8 (every output into a caller
`ta_buffer`).
**Date:** 2026-10-12

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the binding author.** Both bindings written in step 8 grew the
  same machinery around core's byte calls: stack rooms with token counts,
  temporaries past them, length hints per output, retries on
  `TA_ERROR_NO_MEMORY`, a text buffer per datamodel, and, in the Node
  addon, shared scratch that a nested call corrupted. About 200 lines per
  binding exist only for this; none of it is the language's.
- **Then: the application developer.** Core asks a datamodel to write the
  same value more than once for one output, and every write runs the
  user's code again (`toJSON`): `ta_host_envelope` calls `to_wire` twice
  per PEER effect (`src/host/router.c:1238`, `:1267`), export calls
  `payload_serialize` three times per delayed-send payload
  (`src/host/hibernate.c:363`, `:411`, `:449`), and a retried
  `ta_host_traces` encodes every traced value again (`src/otlp/otlp.c:246`).
  With the bindings' retries, one 1 KB envelope walked its value six times.
- **Then: every embedder, for safety.** Outside a run, nothing stops a
  datamodel's code from calling back into the host: a `toJSON` that
  terminates its own actor during `actor.context` leaves core calling
  `variable()` on a destroyed instance (`src/host/request.c:700-720`).
- **Out of scope:** Node-API's lack of a UTF-8 view (a platform cost that
  stays in that binding); trimming the host's output memory; snapshots,
  whose size is known up front.
- **Who pays:**

  | Cost | What |
  |---|---|
  | per event | nothing; one `to_wire` per envelope fewer |
  | per output | none; one write, no retry |
  | memory | the host holds the high-water mark of its largest output until `ta_host_free`; a datamodel holds its largest written value |
  | C embedder | an output is a view until the next call into the host: copy it to keep it (one memcpy for asynchronous I/O) |
  | API surface | the host's output calls and three vtable entries change shape; `cbor.h` gains a streaming writer and a reader, and loses `ta_cbor_encode` |
  | every binding | deletes its rooms, hints and retries; converts values with the streaming calls |

## Context

ADR 0009 made CBOR a token list in caller memory so that a binding "pays
one call across per message, not one per item", and made every output a
`ta_buffer` the caller sizes, retried with the length core answers. Both
were made when a binding was JavaScript over an addon. Since step 8 each
binding is its extension's C ([ROADMAP.md](../../ROADMAP.md), step 8,
decision 2), and the browser's options both keep the conversion in C or
read memory directly:

- **emnapi** (the Node-API addon compiled to wasm): core calls stay inside
  the module; the crossings are the `napi_*` imports, one per object made,
  the same with tokens or streaming.
- **a JavaScript twin**: a JS codec reading host-owned output as a view of
  wasm memory beat token lists in a spike (decode 35.1 µs against 38.1, a
  1 KB value; encode 7.9 µs against 18.2). A JS→wasm call costs 2.9 ns, so
  "one call per message" is worth about 1 µs per 326 items.

Measured natively (Apple M4 Max, clang -O2): streaming encodes a 1 KB value
in 2.5 µs against 3.7 with tokens, decodes and builds it in 6.7 µs against
8.5, needs no token memory (294 KB of tokens for a 64 KB value), and a 1 KB
envelope end to end drops from 15.4 µs to 2.5 µs with outputs as views.

Effects already work the way this ADR makes every output work: the host
keeps the records, the binding reads views until the ack, and no binding
sizes or retries anything there.

## Decisions

### 1. A datamodel's outputs are views

`to_wire`, `serialize` and `payload_serialize` give a `ta_bytes` view the
datamodel owns, valid until the next call on the same datamodel, the
contract `eval_string` and `log` already have (`datamodel.h`):

```c
ta_status (*to_wire)(void *datamodel, ta_payload p, ta_bytes *out);
ta_status (*serialize)(const ta_datamodel_call *call, ta_bytes *out);
ta_status (*payload_serialize)(void *datamodel, ta_payload p, ta_bytes *out);
```

Core calls each once per value and copies what it keeps before its next
call on that datamodel: export is written in one pass (its header filled
last), and `serialize`'s bytes are copied before `destroy`. A datamodel
writes them with the streaming writer (decision 4) into memory it keeps
for the next call.

### 2. The host's outputs are host-owned views

`ta_host_request`'s reply, `ta_host_envelope`, `ta_host_traces`,
`ta_host_logs`, `ta_host_metrics`, `ta_host_hibernate` and
`ta_host_export_actor` give a `ta_bytes` view of memory the host owns,
valid until the next call into the host, as effect fields are until the
ack. The host grows that memory through its allocator, between runs, and
keeps its high-water mark until `ta_host_free`. "Nothing changed on
`TA_ERROR_NO_MEMORY`" now means the allocator refused.

These keep `ta_buffer *`: `ta_host_snapshot` and `ta_host_snapshot_pack`
(their size is known before writing, and a host copy would hold three
copies at peak), `environment->error` (no host exists when creation
fails), and the freestanding layers (`ta_system_*`, `ta_otlp_*`, `cbor.h`
other than the writer's growth).

### 3. Calls that reach a datamodel outside a run are guarded

While the host calls into a datamodel outside a run (a request, an
envelope, hibernate, export, the telemetry takes, `release` in the ack,
deliver and free), the rules of a run apply to calls into the host
([bindings.md](../../tinyactors-core/docs/bindings.md), section 5):
`send`, `send_many`, `spawn` and `terminate` take their in-run paths, a
terminated actor exits at its next microstep boundary, lookups answer, and
everything else answers `TA_ERROR_STATE`. A datamodel's code can no longer
destroy what core is about to use.

### 4. Values stream

- **The writer writes bytes, item by item.** `ta_cbor_writer` keeps its
  calls (`ta_cbor_write_map`, `_array`, `_text`, ...) and writes CBOR
  directly, without tokens, into a block it starts in (a caller's room,
  which may be on the stack) and grows through an optional
  `ta_allocator`; `ta_cbor_writer_bytes` gives the result, and
  `ta_cbor_writer_free` returns what it grew. A container's count is fixed
  when it closes, so a map whose keys a language drops is still exact.
  Without an allocator, a full room is `TA_ERROR_NO_MEMORY` for that item,
  and nothing is written twice. Each call into a binding has its own
  writer, so a nested call cannot overwrite another's.
- **The reader is a cursor.** `ta_cbor_reader` walks a `ta_bytes` view one
  item at a time (type, value, length, a view of text and bytes),
  validating as it reads, with the same depth limit; it needs no memory.
- **Token lists stay as the random-access index** (`ta_cbor_decode`,
  `ta_cbor_next`, `ta_cbor_member`, `ta_cbor_string`) that request
  parsing and C embedders use. `ta_cbor_encode` goes; its callers move to
  the writer in the same change.
- The CBOR functions allocate only through the allocator a caller passes
  to the writer; the base layer still calls no allocator of its own.

## What changes elsewhere

- `AGENTS.md` (root), "At most three parameters": bytes come out as a
  `ta_buffer *` the caller fills, or as a `ta_bytes *` view whose owner and
  lifetime the declaration states; the lint rule `buffers-are-ta-buffer`
  allows the latter.
- `tinyactors-core/docs/bindings.md` sections 1, 5, 6, 8 and 9, and each
  binding's `AGENTS.md`, "The binding's memory": what the bindings keep is
  their handle tables, one writer per call (a stack room, grown through
  the environment's allocator), and, in a datamodel, the memory its last
  output view lives in.
- The protocol vectors do not change: the bytes are the same.

## Consequences

- A binding's memory is its handle tables and the language's objects; no
  sizes to tune, no retries, nothing shared between nested calls.
- Every output runs the datamodel's code once.
- A C embedder that keeps an output across calls copies it.
- Order of work: the guard (decision 3) first, since it is a fix on its
  own; then the host's outputs and the vtable (decisions 1 and 2) with the
  quickjs datamodel and `ta-conform`; then the writer and reader
  (decision 4) with every caller of `ta_cbor_encode`; then the bindings.

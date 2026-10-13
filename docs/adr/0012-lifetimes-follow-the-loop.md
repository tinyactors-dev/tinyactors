# ADR 0012: Lifetimes follow the loop

**Status:** Accepted (2026-10-13); amended 2026-10-13 (decision 4, see
"Amended (2026-10-13)" at the end); amended again 2026-10-13 (decision 4:
`actor.subscribe` and `actor.changed` are removed; decision 5:
`ta_growable_arena`; see "Amended again (2026-10-13)"); amended a third
time 2026-10-13 (decision 5: `ta_growable_arena` built in base, and the
compiler's `ta_chunk_arena` moved onto it; decision 7, new: the
resolver's errors and its scratch arena; see "Amended a third time
(2026-10-13)"); amended a fourth time 2026-10-13 (decision 7: the
resolver lives only at compile time; see "Amended a fourth time
(2026-10-13)"); amended a fifth time 2026-10-13 (decision 7:
`TA_EFFECT_REQUEST` stays for run-time fetching; see "Amended a fifth
time (2026-10-13)"). Amends [ADR 0010](0010-outputs-are-views-values-stream.md)
decision 2 (the host's outputs) and [ADR 0011](0011-the-control-plane-is-posted-messages.md)
decisions 3 (publication), 4 (`actor.changed`) and 6 (the drain and the
synchronous `request`).
Reference: [tinyactors-core/docs/boundary.md](../../tinyactors-core/docs/boundary.md)
(the contract, one row per call); guide:
[tinyactors-core/docs/embedding.md](../../tinyactors-core/docs/embedding.md)
and [bindings.md](../../tinyactors-core/docs/bindings.md).
**Date:** 2026-10-13

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the binding author.** Both bindings are being rewritten from
  scratch. Today a binding has to know four states of an effect and five
  lifetimes of what core hands it (below), re-read `ta_host_effects`
  until its count is stable, mark replies consumed, ack only at its
  outermost level, and copy every host output before its next call into
  the host. After this ADR it knows one rule: what core hands out about
  the current batch lives until `ta_host_effects_ack`.
- **Then: the C embedder.** The loop in
  [embedding.md](../../tinyactors-core/docs/embedding.md) is take, handle,
  ack, run, with nothing to copy while handling.
- **Then: the application developer.** No synchronous call into the
  control plane runs inside an effect handler; actor output arrives
  through the channel SCXML has for it (an I/O processor).
- **Out of scope:**
  - the API shape of `PEER` versus `PEER_ERROR` in a binding (deferred
    by the maintainer);
  - a JSON-RPC transport, and so a raw `post` in any language API;
  - handing large outputs over without a copy (boundary.md, "Not yet
    decided");
  - datamodel outputs, which keep "until the next call on that
    datamodel": core consumes them inside its own calls.
- **Who pays:**

  | Cost | What |
  |---|---|
  | per event | nothing. `TRACEPARENT`'s text is written on first ask, not per effect |
  | per batch | the host's outputs taken while handling it are held until the ack, not replaced by the next call: a batch of 64 `PEER` effects of 1 KB holds 64 KB of envelopes until the ack instead of 1 KB (arithmetic, not measured) |
  | per session | nothing: `actor.subscribe` and `actor.changed` are removed (decision 4, amended again), and with them the subscription table's mark byte per subscription (`host.c:731`) |
  | API | `ta_host_run` refuses while a batch is open; `ta_host_effects` returns the open batch instead of widening it; `actor.subscribe`, `actor.unsubscribe` and `actor.changed` removed, `actor.state` as it is, `limit` included (decision 4, amended again); new in base: `ta_table` (7 functions, ~83 lines in the spike), `ta_status_name` and `ta_growable_arena` (decision 5; a port of tinyxml's `txml_arena`, `tinyxml/src/arena.c`, 218 lines) |
  | per binding call | nothing from the allocator once the binding's arena has reached its high-water mark: a call takes a mark and releases to it, and released chunks are kept (decision 5) |
  | callers in this repository | the tests and `ta-conform` that run without acking migrate in the same change (`git grep ta_host_run`) |
  | every binding | less: no drain re-read, no consumed marks, no request facade, no copy before each call, no handle table and no status table of its own |

## Context

A maintainer review of 2026-10-13 read the boundary as a binding author
meets it (the headers, ADR 0010, ADR 0011, the guide) and found:

- **Four states of an effect:** appended but not published; published
  and unread; published and consumed by a facade (the binding's own
  mark, ADR 0011 decision 6); published again by a later
  `ta_host_effects` that widened the batch (`src/host/host.c:1200-1201`
  sets `effects_ready = effect_count` on every call). A drain had to
  re-read until the count was stable and ack only at its outermost
  level (bindings.md section 5); a facade that acked inside a handler,
  or published and did not ack before the next run, lost effects
  (ADR 0011 decision 6, reproduced in a spike).
- **Five lifetime classes** of what core hands out: effect records
  until the ack, the next run or `ta_host_free` (`host.h:55-59`);
  `TRACEPARENT`'s text until the next `ta_host_effect_text`
  (`host.h:297-298`; one buffer, `host.c:1265-1268`); the host's
  outputs until the next call into the host (`host.h:62-69`,
  ADR 0010 decision 2); a datamodel's outputs until its next call
  (`datamodel.h:9-13`); the caller's `ta_buffer` (snapshots,
  `environment->error`). A handler that posts, delivers or writes a
  second envelope ends the view it is still reading, so both bindings
  copied every output before any call (`tinyactors-cpython/AGENTS.md`
  and `tinyactors-napi/AGENTS.md`, "Views of what core ... hold", before
  this ADR).
- **The value divergence:** ADR 0011 decision 4 says `actor.changed`
  carries `"value": <state value | null>`; the guide says the same
  (bindings.md section 4); requests.md says `value` is left out past the
  state-value limit, and the code does that (`host.c:755-758`, patching
  the params map to one member). A subscriber had three shapes to read.
- **A synchronous `request` runs inside dispatch.** The facade of
  ADR 0011 decision 6 posts, re-reads `ta_host_effects` to find its
  reply in a widened batch, and drains the rest. With the drain's
  re-read, which picks up what handlers' calls append, it is why a batch
  widens at all.

The maintainer decided the model below; this ADR writes it down.

## Decisions

### 1. Lifetimes follow the loop

The loop is: take (`ta_host_effects`), handle, ack
(`ta_host_effects_ack`), run (`ta_host_run`). **The ack means "I have
fully handled everything up until now."** Everything core hands the
caller about the current batch lives until that ack (or `ta_host_free`):

- the effect records and every field read through the accessors
  (numbers, text, `WIRE` bytes, payload handles);
- `TRACEPARENT`'s text: written into host memory on first ask for that
  effect, valid until the ack like every other field (no shorter
  lifetime);
- the host's outputs taken while the batch is open: `ta_host_envelope`,
  `ta_host_traces`, `ta_host_logs`, `ta_host_metrics`,
  `ta_host_hibernate`, `ta_host_export_actor`. Each is a `ta_bytes` view
  valid until the ack, however many calls into the host come between.

A batch is open from the first thing the host hands out (a
`ta_host_effects` that returns at least one effect, or an output view)
until `ta_host_effects_ack`. An output taken with no effects published
opens a batch of its own, which the ack closes the same way.

What keeps its own lifetime:

- **snapshots** (`ta_host_snapshot`, `ta_host_snapshot_pack`) and
  **`environment->error`** write into the caller's `ta_buffer`, as
  ADR 0010 decision 2 left them;
- **a datamodel's outputs** (`to_wire`, `serialize`,
  `payload_serialize`, `eval_string`, `log`) stay views until the next
  call on that datamodel: core consumes them inside its own calls;
- **structural borrows** stay as `host.h` states them: a segment's
  image while the segment is live, `ta_host_system` until a call that
  can grow or free the segment, a peer's id until the peer is removed,
  views into a caller's bytes (`ta_host_envelope_read`) while the caller
  keeps them.

The host keeps a batch's outputs in memory it frees or reuses at the
ack; growing it never moves an output already handed out (a new block,
as the effect pool holds replaced blocks while a batch is open,
`host.c:972-981`).

### 2. One drain at a time

`ta_host_effects` never widens an open batch. While a batch is open it
returns the same batch; effects appended meanwhile (a reply to a
message posted from a handler, a deliver's `PEER_ERROR`, `trace.end`'s
`TRACES`) are pending and come with the next take, after the ack.

`ta_host_run` refuses while a batch is open: `TA_RUN_FAULT` with
`detail` `TA_ERROR_STATE`, and nothing runs. The run no longer releases
an unacknowledged batch (`host.c:1834` today), so forgetting the ack is
an error the caller sees, not effects lost in silence.

The drain is:

```
while (n = ta_host_effects(h, &effects)) > 0:
    for i in 0 ..< n: handle(i)        # calls into the host are fine here
    ta_host_effects_ack(h)             # everything handed out ends here
```

and then, at most, a run.

### 3. No synchronous request in any language API

The control plane is `ta_host_post` with `REPLY` and `NOTIFICATION`
effects (ADR 0011, done). A binding exposes **typed methods over post**
(`host.stats()`, `actor.state(session)`, ..., one per method of
`requests.md`), each of which posts with a fresh id and is settled when
its `REPLY` is handled in a later drain, by calling the response handler
the caller passed (amended again below; bindings.md section 10). **No raw `post`** in a
language API until a JSON-RPC transport exists. The binding's own C may
post and drain at creation (reading `host.config`), since no batch is
open then; that is the loop, not a synchronous API.

### 4. `actor.subscribe` and `actor.changed` are removed (amended twice)

*As first accepted, this decision made `actor.changed` carry the whole
state value after the run; the first amendment (2026-10-13) made it one
notification per macrostep with the states entered and exited, and left
five problems open (the capture without allocation, the outbox, a busy
actor's flood, the last macrostep, a refused notification). The second
amendment (2026-10-13, at the end) replaces both: this is the decision as
amended again.*

**`actor.subscribe`, `actor.unsubscribe` and `actor.changed` are
removed** from core, the vectors and the documents [8.2]. They were a
stress test of the protocol, and served it: they showed that a posted
message can start a stream of effects, and what such a stream costs in a
run. **Notifications stay possible:** `TA_HOST_EFFECT_NOTIFICATION`
(WIRE `{"method", "params"}`, EVENT the method, SESSION the session,
`host.h:250-251`) keeps its place in the effect list and in requests.md's
"Notifications" section, with no notification defined today. A binding
handles the kind (it reads the method and drops one it does not know)
and offers no API for it until a notification exists.

`actor.state` stays as it is: the state value written in the stack's
room for 1024 states (`host_state_value`, `src/host/host.c:806-816`;
`HOST_STATE_VALUE_STATES`, `src/host/host_internal.h:334`), refused with
`limit` past it (`src/host/request.c:657-660`).

What the removal takes out of core [8.2]: the subscription table and its
mark byte per subscription (`subscription_mark`, `host.c:731`;
`subscriptions_notify`, `host.c:777-797`), the host's use of
`ta_actor_watch` for it (`system.h:304-311`; the system's
`TA_EFFECT_CHANGED` and `ta_actor_watch` stay below the host, as
`ta_system`'s own API), the methods in `src/host/request.c`, the config
key `maxSubscriptions` (`docs/host-config.md:61`), the subscribe exchange
in `tinyactors-test/suites/protocol/requests.txt`, and the self-test's
check of a heading with two methods, which names `actor.unsubscribe`
(`mise/tasks/api-map-test:17`).

### 5. `ta_table`, `ta_status_name` and `ta_growable_arena` are public, in base

Every binding keeps handle tables for what core stores as integers
(sessions, payloads, charts, loops), names statuses in its errors, and
needs memory for what one call into it converts (a config, a request's
params, a payload for another datamodel, text and input structs). All
three are free of any language's API, so they belong in a C layer
(`AGENTS.md`, "Layers"). `ta_growable_arena` was added by the second
amendment (below).

`include/tinyactors/table.h`, implemented in `src/base/table.c`
(freestanding; it allocates only through the caller's allocator, as
the CBOR writer does, ADR 0010 decision 4):

```c
/* A table of rows of `row_size` bytes, each named by a 64-bit handle:
 * generation << 32 | (slot + 1), so 0 is never a handle. A slot's
 * generation is odd while its row is live (31 bits of generations); a
 * removed row's handle is stale for good, also after its slot is reused.
 * Rows grow by doubling through `allocator`, so a row pointer is valid
 * until the next ta_table_add; a handle is valid until ta_table_remove. */
typedef struct ta_table_config {
    uint32_t size;     /* sizeof(ta_table_config) */
    uint32_t row_size; /* bytes per row, rounded up to 8 */
    const ta_allocator *allocator;
} ta_table_config;
typedef struct ta_table {
    unsigned char *rows; /* `capacity` rows, then `capacity` slot records */
    const ta_allocator *allocator;
    uint32_t row_size, count, capacity;
    uint32_t free; /* the first free slot + 1; 0: none */
} ta_table;
TA_API void ta_table_init(ta_table *table, const ta_table_config *config);
TA_API void *ta_table_add(ta_table *table, uint64_t *handle);      /* zeroed row; NULL: refused */
TA_API void *ta_table_get(const ta_table *table, uint64_t handle);  /* NULL: stale or never was */
TA_API void *ta_table_at(const ta_table *table, uint32_t slot);     /* NULL: free slot */
TA_API uint64_t ta_table_handle(const ta_table *table, uint32_t slot); /* 0: free slot */
TA_API void ta_table_remove(ta_table *table, uint64_t handle);     /* stale: nothing */
TA_API void ta_table_free(ta_table *table);
```

`include/tinyactors/types.h`, beside the status codes, implemented in
`src/base/status.c`:

```c
/* The name of `status` as this header spells it ("TA_OK",
 * "TA_ERROR_STATE", ...); NULL for a status it does not define. */
TA_API const char *ta_status_name(ta_status status);
```

`include/tinyactors/arena.h`, implemented in `src/base/growable_arena.c`
(freestanding; it allocates only through the caller's allocator) [8.2]:
a port of tinyxml's `txml_arena` (`tinyxml/include/txml.h:209-252`,
`tinyxml/src/arena.c`) with one difference, **release keeps its
chunks**. The names below are a sketch; 8.2 settles them, the rules
are decided.

*Amended a third time (2026-10-13):* `ta_growable_arena` is built in
base, taking from `txml_arena` its linked chunks and marks
(`txml_arena_position`, `txml_arena_release`, `txml.h:221-249`), with the
two things this decision adds to it, a release that keeps the chunks
and the `ta_allocator` view; and **the compiler's private chunked arena,
`ta_chunk_arena`, moves onto it** in 8.2: one chunked arena in core, not
two. `ta_chunk_arena` (`src/compile/compile_support.h:14-26`, 28 call
sites of `_alloc` and `_strdup` in `src/compile/`) is the compiler's
per-compilation arena (`compile.c:37`, freed at `:87`: source texts,
`compile_internal.h:11-16`, `link.c:39-40`; the XML parser's nodes,
`xml.c:267`) and the compile result's (`compile.c:13`, `:24`, freed by
`ta_compile_free` at `:275`: the diagnostics' file and message strings,
`compile_support.c:226-228`). Both become `ta_growable_arena`s, and the
per-compilation one is the resolver's scratch (decision 7).

```c
/* A bump allocator over linked chunks from `allocator`. An allocation
 * never moves: growing links a new chunk, nothing is copied. A request
 * larger than the chunk size gets a chunk of its own, so one allocation
 * is always contiguous. A mark is a position; releasing to it ends every
 * allocation made after it and KEEPS the chunks for the next ones, so an
 * arena that has reached its high-water mark calls the allocator no more.
 * Only ta_growable_arena_free gives chunks back. Alignment is at most 8. */
typedef struct ta_growable_arena_chunk ta_growable_arena_chunk;
typedef struct ta_growable_arena_config {
    uint32_t size;      /* sizeof(ta_growable_arena_config) */
    uint32_t pad;
    size_t chunk_size;  /* the first chunk's payload; 0: a default */
    const ta_allocator *allocator;
} ta_growable_arena_config;
typedef struct ta_growable_arena {
    const ta_allocator *allocator;
    ta_growable_arena_chunk *head;  /* the chunk bumped now */
    ta_growable_arena_chunk *kept;  /* chunks released and kept, for reuse */
    size_t chunk_size, used, reserved;
} ta_growable_arena;
typedef struct ta_growable_arena_mark {
    ta_growable_arena_chunk *chunk;
    size_t offset;
} ta_growable_arena_mark;
TA_API void ta_growable_arena_init(ta_growable_arena *arena, const ta_growable_arena_config *config);
TA_API void *ta_growable_arena_alloc(ta_growable_arena *arena, size_t size, size_t align); /* NULL: refused */
TA_API ta_growable_arena_mark ta_growable_arena_position(const ta_growable_arena *arena);
TA_API void ta_growable_arena_release(ta_growable_arena *arena, ta_growable_arena_mark mark);
/* A ta_allocator over the arena, so a ta_cbor_writer grows into it: alloc
 * bumps; free gives nothing back (the space returns at the next release
 * to a mark taken before it). Valid while the arena lives. */
TA_API ta_allocator ta_growable_arena_allocator(ta_growable_arena *arena);
TA_API void ta_growable_arena_free(ta_growable_arena *arena);
```

The rules:

- **Allocations never move**, so a pointer into the arena is valid until
  a release to a mark taken before it, whatever is allocated meanwhile.
- **Marks nest.** A caller takes a mark at the entry of a call and
  releases to it at the exit, on every path. A call made while another is
  in progress on the same thread (Python code a conversion runs, which
  calls the binding again) allocates above the outer call's last
  allocation and releases back to its own mark, which is above it: the
  outer call's memory is untouched. Releasing to a mark older than one
  still in use is the caller's error.
- **Release keeps its chunks** (txml's keeps one spare and frees the
  rest, `arena.c:169-202`): the arena holds its high-water mark until
  `ta_growable_arena_free`, and after warm-up no call reaches the
  allocator. An oversized chunk is kept too.
- **A writer's growth costs up to twice its final size** until the
  release: `ta_cbor_writer` grows by allocating a block twice as big and
  freeing the old one (`cbor.h:124-131`), and the arena's `free` gives
  nothing back. Size the writer's first room from the arena when the
  size is known.
- **The internal `ta_arena`** (`src/base/arena.h`: carving a caller's
  region, offsets, measure mode, used by `ta_system_layout` and
  `ta_system_init`) keeps its name and its job; it never allocates.

### 6. DONE, ERROR and LOG are the binding's; output goes through I/O processors

- **`TA_EFFECT_DONE` and `TA_EFFECT_ERROR` are internal to the
  binding's C.** They are lifecycle: the binding releases the rows it
  keeps for the session and settles what waits on it. They are not
  application callbacks. The done data's
  payload is retained by core when the effect is copied and released at the ack (`host.c:1187-1191`, `:1044-1048`); a fault
  is counted in `host.stats` (`faults`, requests.md).
- **Actor output to the application goes through I/O processors:** a
  chart that has something for the application `<send>`s it to an I/O
  processor the application configured (`TA_EFFECT_SEND`, synchronous
  or not). That is SCXML's channel for output, and the one that carries
  a target, a name and data.
- **`TA_EFFECT_LOG` goes to a log handler configured per host** (the
  second amendment: Python's `Host(..., log=handler)`, no global
  configuration such as `logging.basicConfig`; without one, the lines
  are discarded), not to a callback per actor.
- **Inspection is by typed methods over post** (decision 3), state
  included (`actor.state`); there is no subscription (decision 4,
  amended again).
- **`PEER` versus `PEER_ERROR` in a language API is deferred.**

### 7. The resolver reports errors, and lends a scratch arena (added by the third amendment)

A static `src` (`<script>`, `<data>`, `<invoke>`) is resolved during
compilation through `ta_compile_options.resolve`
(`src/compile/scxml.c:201-216`, called at `:599`, `:860`, `:968`; a
manifest's `<chart src>` at `compile.c:191-194`). Today a refusal is one
bare diagnostic at the referring element, `cannot resolve src="..."`
(`scxml.c:212`), so a resolver that knows why (a Python `.py` file whose
rewrite refuses a construct at line 7) cannot say so; and the resolver
must keep the text it returns alive by itself until the compilation
ends (`ta_runtime_files` owns its texts, `runtime.h:32-37`). [8.2]
changes the callback in place, keeping its three parameters:

```c
typedef struct ta_resolve_request {
    ta_string_view base;           /* the referring document */
    ta_string_view source;         /* src as written */
    ta_growable_arena *scratch;    /* lives for this compilation */
} ta_resolve_request;
typedef struct ta_resolve_error {
    uint32_t line, column;         /* in the resolved src */
    ta_string_view message;
} ta_resolve_error;
typedef struct ta_resolve_result {
    ta_source source;              /* on TA_OK: the text (and the canonical name, if any) */
    ta_resolve_error error;        /* on a refusal: why */
} ta_resolve_result;
typedef ta_status (*ta_resolve_function)(void *context, const ta_resolve_request *request,
                                         ta_resolve_result *out);
```

- **A refusal becomes a diagnostic with the resolver's words:** file the
  resolved src, line and column `out->error`'s, message
  `out->error.message`, copied into the result's arena (as every
  diagnostic is, `compile_support.c:226-228`; a message is cut at 511
  bytes, `:208`), instead of the bare `cannot resolve src`.
- **The scratch arena is the compiler's per-compilation arena**
  (decision 5, third amendment), lent to the resolver: the resolved text
  and the message may live there, and the compiler copies what it keeps
  (a queued document's text, `link.c:40`; an expression's source,
  `scxml.c:116`).
- **The callers migrate in the same commits:** `fetch_source` and
  `ta_compile_system`; the implementers `ta_runtime_resolve`
  (`runtime_hosted.c:78`), `ta-conform`'s `document_resolve`
  (`tools/conform/datamodel.c:113`), the tests' `test_resolve`
  (`tests/support/compile_util.h:20`) and `abort_resolve`
  (`tests/unit/test_compile.c:432`), and the lint rule's test
  (`tools/lint/tests/at-most-three-parameters-test.yml:6`).

| Object | Owner | Created | Read | Ends |
|---|---|---|---|---|
| `request->scratch` | the compiler (chunks from `ta_compile_options.alloc`) | at `ta_compile_charts`' (`_system`'s) start | written by the resolver; read by the compiler | when `ta_compile_charts` returns |
| `out->source.text` | the scratch, or the resolver's own memory | in the resolver's call | by the compiler, which copies what it keeps | when `ta_compile_charts` returns, at the latest |
| `out->error.message` | the scratch, or the resolver's own memory | in the resolver's call | by the compiler, which copies it into a diagnostic | when `ta_compile_charts` returns, at the latest |
| a diagnostic (`file`, `message`) | the compile result's arena | when the compiler reports it | by the caller (`ta_compile_diagnostics`) | `ta_compile_free` |
| the image | the compile result | at the compilation's end | by the caller; `ta_host_add_image` copies it | `ta_compile_free` |

**The resolver lives only at compile time** (fourth amendment). A
static `<invoke src>` is fetched through it while compiling, and the
invoked chart is compiled into the same image (`scxml.c:893-896`,
`resolve_chart` at `:855-870`). Run-time fetching is a different
thing, `TA_EFFECT_REQUEST`, and it stays (fifth amendment):

**`TA_EFFECT_REQUEST` stays (2026-10-13).** It is how a chart fetches at
run time: an `<invoke srcexpr>` (or a `src` the compiler did not match)
whose chart is not in the image. Core normalizes
the name itself (`file://`, `file:`, `./`, an inline document) and looks
it up in the requesting segment's image (`route.c:439-489`); it gives the
REQUEST only when that lookup fails (`:491-497`). The fetching is built
out later. Until then a binding answers every REQUEST with
`ta_host_resume` and `TA_ERROR_INVALID`, and the parent gets
`error.execution` (`route.c:544-550`): a resume names only a chart of the
requesting segment's image (`:544-553`), and core has already looked
there. What the build-out needs from core is in `ROADMAP.md`, "Decided
later". A REQUEST is not the control plane: `ta_host_post`, `REPLY` and
`NOTIFICATION` ([ADR 0011](0011-the-control-plane-is-posted-messages.md))
are a different mechanism.

## Rejected alternatives

- **Not "until the next call into the host" for outputs**, because a
  handler that posts, delivers or writes a second envelope ends the view
  it is still reading, as both bindings' rules to copy every output
  before any call show (their `AGENTS.md`, "Views ..."); the ack is
  where the caller already says it is done.
- **Not widening the open batch**, because it forced a stable-count
  re-read, consumed marks and an outermost-only ack in every binding,
  and a facade that acked inside a handler lost effects, as ADR 0011's
  spike showed (decision 6).
- **Not letting `ta_host_run` release an unacknowledged batch**, because
  the effects are lost without a word ("the run releases a published
  batch, read or not", bindings.md section 5 before this ADR), and a
  lost `TRACES` or `LOGS` stops telemetry for good (no other comes
  until a take).
- **Not a synchronous `request`**, because finding its reply needs the
  open batch to widen (decision 2), and it runs control-plane calls
  inside an effect handler, as ADR 0011 decision 6's facade shows.
- **Not `TRACEPARENT` in one buffer per call** (`host.c:1265-1268`),
  because a handler reading two effects' traceparents keeps the first
  only until the second; **not formatted when the effect is made**,
  because that costs a format and 55 bytes per traced effect whether
  anyone asks or not (`TA_TRACEPARENT_LENGTH`, `trace.h:61`), a cost
  per event.
- **Not the whole state value in `actor.changed`** (decision 4 as first
  accepted; amended 2026-10-13), because past the stack's room for 1024
  states it needed new state-value machinery, a walk of the
  configuration in place or scratch from the host's allocator
  (`host.c:806-816`), both dropped by the maintainer; a subscriber gets
  the starting point from `actor.state` and the changes from each
  notification. The three alternatives below were weighed against that
  first decision.
- **Not a truncated state value with `"limit": true`**, because a
  truncated configuration of a parallel state lists fewer regions than
  its chart has, a value `actor.state` never answers, and every
  subscriber needs a second branch. **Not `null` past the limit**,
  because `null` means the session ended and its subscription went with
  it (`host.c:789-790`). **Not leaving `value` out** (today's code),
  because ADR 0011, the guide and requests.md disagreed on it, and a
  reader had three shapes.
- **Not `actor.changed` per macrostep** (the first amendment), because
  its five open problems (capture without allocation, the outbox, a
  busy actor's flood, the last macrostep, a refused notification) bought
  nothing the application needs: an actor's output reaches it through an
  I/O processor, and its state through `actor.state` (the second
  amendment).
- **Not txml's release, which frees chunks past one spare**
  (`tinyxml/src/arena.c:169-202`), because a binding takes and releases
  a mark on every call: freeing at release calls the allocator per call,
  where keeping the chunks calls it only while the high-water mark grows.
- **Not one writer per call on the C stack, grown through the host's
  allocator and freed at the call's end** (bindings.md section 6 before
  the second amendment), because a call that converts several values (a
  `send_many` of event structs, text and payloads) needs several blocks,
  each a pair of allocator calls per call; the arena serves them all from
  kept chunks.
- **Not the internal `ta_arena` made public**, because it carves one
  region given up front, by offset, with a measure mode
  (`src/base/arena.h:1-13`): it cannot grow, and its job (the system's
  layout) stays.
- **Not a handle table in each binding**, because the same ~80 lines
  were written twice (`src/datamodel/handles.c` in both bindings, before
  their removal in `6426816a` and `70434bee`), and the audit compared
  them file by file; the spike's public variant needs no header of its
  own in a row (`table_public.c`, the coordinator's spike of
  2026-10-13).
- **Not a status table in each binding**, because the CPython binding
  had one (`ta_python_status_names`, `src/module.c` at `6426816a^`) that
  every other binding would repeat.

## Order of work

Step 2 of [ROADMAP.md](../../ROADMAP.md), step 8, after these documents:

1. `ta_status_name`, `ta_table` and `ta_growable_arena` in base, with
   unit tests (`tests/unit/test_table.c`: add, get, remove, a stale
   handle after reuse, growth through a refusing allocator, free;
   `tests/unit/test_growable_arena.c`: an allocation that never moves,
   an oversized request, nested marks, a release that keeps its chunks
   and calls the allocator no more on the next round, a writer growing
   through its `ta_allocator`, a refusing allocator, free), and their
   entries in `tinyactors.api.json`. Then the compiler moves from
   `ta_chunk_arena` onto `ta_growable_arena`, and the resolver takes its
   scratch and reports its errors (decision 7, third amendment), with a
   test of a refusal's diagnostic (file, line, column, message) and of a
   text left in scratch.
2. One drain at a time: `ta_host_effects` returns the open batch;
   `ta_host_run` refuses while one is open; tests and `ta-conform`
   migrate (each takes, handles, acks before a run).
3. Outputs until the ack: the batch's output memory, the six output
   calls on it, `TRACEPARENT` on first ask; a test that takes two
   envelopes and a metrics view in one batch and reads all three after
   the second call (ASan).
4. `actor.subscribe`, `actor.unsubscribe` and `actor.changed` removed
   (decision 4, amended again): the code, the vectors, the config key
   `maxSubscriptions` and the self-test that names them, in one commit.
   `actor.state` does not change.
5. `host.h`'s comments, `requests.md` and the guide follow in the same
   commits (`tinyactors-core/AGENTS.md`).

Then step 3 (the full loop end to end in C) and step 4 (the minimal
Python binding) of the root roadmap.

## Consequences

- One lifetime for everything the host hands out: until the ack.
- The drain is a loop of take, handle, ack; no binding re-reads, marks
  or copies to survive its own calls.
- Memory held between take and ack grows with what a batch's handling
  takes; acking often bounds it.
- Language APIs have no synchronous control-plane call; their typed
  methods settle at a later drain.

## Amended (2026-10-13)

The maintainer amended decision 4 on the day it was accepted:

- **`actor.changed` carried changes, not a value** (superseded by the
  second amendment, below: removed).
- **No new state-value machinery.** The two versions considered for the
  whole value (walking the configuration in place, and scratch from the
  host's allocator past the stack's room) are both dropped, and so is
  the removal of the 1024-state limit. `actor.state` stays as it is,
  `limit` included.
- **Snapshots stay** (system, host and datamodel snapshots); nothing in
  this ADR changes them.
- **`ta_host_run` refusing while a batch is open** (`TA_RUN_FAULT`,
  `detail` `TA_ERROR_STATE`; decision 2) is accepted as decided.

What this replaces in the text above: the cost rows and decision 4 are
rewritten; the context's "value divergence" and the rejected
alternatives about a truncated value, `null` past the limit and a left
out `value` stay as the record of the first decision, since the shape
they argued over is gone.

## Amended again (2026-10-13)

The maintainer amended decisions 4 and 5 a second time, the same day:

- **`actor.subscribe` and `actor.changed` are removed; notifications
  stay possible** (decision 4, rewritten). They were a stress test of
  the protocol. `actor.unsubscribe` goes with them. Core, the vectors and
  the documents lose them in step 8.2 [8.2]; until then the code answers
  them as before.
- **`ta_growable_arena` is public, in base** (decision 5): linked chunks
  from a `ta_allocator`, allocations that never move, marks, a release
  that keeps its chunks, and a `ta_allocator` view for `ta_cbor_writer`.
  A binding keeps one per host and brackets each call with a mark
  (bindings.md section 6).
- **Typed methods take response handlers** (decision 3): in Python
  `method(..., handler, on_error=None)` returns `None`, and the drain
  that handles the `REPLY` calls the handler (bindings.md section 10).
  The rule holds for every binding; Node's shape of it is decided in
  step 8.5.
- **`<log>` lines go to a log handler per host** (decision 6).

What this replaces in the text above: decision 4, the cost rows for
subscriptions, decision 6's subscription bullets, and item 4 of the order
of work. The first amendment's text about the per-macrostep shape is
reduced to one line; its record is in git (this file at `30fd4ad3`).

## Amended a third time (2026-10-13)

The maintainer amended decision 5 and added decision 7, the same day:

- **`ta_growable_arena` is built in base, and `ta_chunk_arena` moves
  onto it** (decision 5): `txml_arena`'s linked chunks and marks, a
  release that keeps its chunks, and the `ta_allocator` view; the
  compiler's private `ta_chunk_arena` (`src/compile/compile_support.h:14-26`)
  is replaced by it in 8.2, so core has one chunked arena, not two.
- **The resolver reports why it refused, and gets a scratch arena**
  (decision 7, new): `ta_resolve_request` gains `scratch`, the out
  parameter becomes `ta_resolve_result` (`ta_source` and
  `ta_resolve_error`), the callback keeps three parameters, and a
  refusal becomes a diagnostic with the resolver's file, line, column and
  message. Scratch lives from the start of `ta_compile_charts` to its
  return; diagnostics until `ta_compile_free`.

Whom it serves first: the application developer whose chart names a
`src` the binding's resolver checks (a Python `.py` script, rewritten as
it is resolved: a refused construct is reported at compile time, at its
line). Who pays: nothing per event or per actor; one pointer in the
request; the compiler's code moves to one arena; every resolver in core
and its tests changes signature (decision 7's list).

## Amended a fourth time (2026-10-13)

The maintainer corrected decision 7, the same day:

- **The resolver lives only at compile time** (decision 7, the
  paragraph after its table). The guide, `boundary.md` section 12 and
  the bindings' documents had the compiler's resolver on the host. A
  static `src` is compiled into the same image by the compiler's
  resolver, and the `Host` has none. (This amendment also said a
  REQUEST is answered by an image lookup in C and gave `[8.2]` a
  normalization item; the fifth amendment replaces both.)

Whom it serves first: the binding author, who has one resolver (the
compile call's).
Who pays: nothing per event or per actor; every binding loses a host
parameter and a callback into the language.

## Amended a fifth time (2026-10-13)

The maintainer decided, the same day: **`TA_EFFECT_REQUEST` stays**, as
the mechanism for fetching at run time (an `<invoke srcexpr>` whose
chart is not in the image), which must stay
supported though the fetching is built out later. Decision 7's
statement "`TA_EFFECT_REQUEST` stays (2026-10-13)" replaces what the
fourth amendment said about a REQUEST. The `[8.2]` item
"`ta_image_chart_by_name` normalizes a `src` name" is withdrawn: the
normalization is core's, before its own lookup (`route.c:439-489`), and
nobody after it repeats it. Only what `docs/SCXML.md` mandates is
supported: SCXML 1.0 defines no `srcexpr` on `<data>` (only `src`,
`expr` or children, `docs/SCXML.md:1317`), so a `<data srcexpr>` is not
part of this design (decided 2026-10-13).

Whom it serves first: the application developer, whose chart may name a
document at run time. Who pays: nothing now; a binding answers a REQUEST
with one `ta_host_resume` call.

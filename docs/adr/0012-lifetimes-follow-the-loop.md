# ADR 0012: Lifetimes follow the loop

**Status:** Accepted (2026-10-13). Amends [ADR 0010](0010-outputs-are-views-values-stream.md)
decision 2 (the host's outputs) and [ADR 0011](0011-the-control-plane-is-posted-messages.md)
decisions 3 (publication), 4 (`actor.changed`) and 6 (the drain and the
synchronous `request`). Decided and revised the same day; "History", at
the end, lists what changed.
Reference: [tinyactors-core/docs/boundary.md](../../tinyactors-core/docs/boundary.md)
(the contract, one row per call); guide:
[tinyactors-core/docs/embedding.md](../../tinyactors-core/docs/embedding.md)
and [bindings.md](../../tinyactors-core/docs/bindings.md).
**Date:** 2026-10-13

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the binding author.** Both bindings are being rewritten from
  scratch. Before this ADR a binding had to know four states of an
  effect and five lifetimes of what core hands it (below), re-read
  `ta_host_effects` until its count was stable, mark replies consumed,
  ack only at its outermost level, copy every host output before its
  next call into the host, and keep a handle table, a status table and
  a resolver on its host of its own. After it a binding knows one rule:
  what core hands out about the current batch lives until
  `ta_host_effects_ack`; and it takes its tables, status names and
  per-call memory from base.
- **Then: the C embedder.** The loop in
  [embedding.md](../../tinyactors-core/docs/embedding.md) is take, handle,
  ack, run, with nothing to copy while handling.
- **Then: the application developer.** No synchronous call into the
  control plane runs inside an effect handler; actor output arrives
  through the channel SCXML has for it (an I/O processor); a `src` the
  resolver refuses is reported at compile time with the resolver's own
  file, line and message (a Python `.py` script whose rewrite refuses a
  construct at line 7); and a chart that names a document at run time
  keeps a mechanism for it (`TA_EFFECT_REQUEST`, decision 8).
- **Out of scope:**
  - the API shape of `PEER` versus `PEER_ERROR` in a binding (deferred
    by the maintainer);
  - a JSON-RPC transport, and so a raw `post` in any language API;
  - handing large outputs over without a copy (boundary.md, "Not yet
    decided");
  - datamodel outputs, which keep "until the next call on that
    datamodel": core consumes them inside its own calls;
  - how run-time fetching is built out (`ROADMAP.md`, "Decided later");
  - anything `docs/SCXML.md` does not mandate, such as a `<data srcexpr>`
    (decision 8).
- **Who pays:**

  | Cost | What |
  |---|---|
  | per event | nothing. `TRACEPARENT`'s text is written on first ask, not per effect |
  | per batch | the host's outputs taken while handling it are held until the ack, not replaced by the next call: a batch of 64 `PEER` effects of 1 KB holds 64 KB of envelopes until the ack instead of 1 KB (arithmetic, not measured) |
  | per session | nothing: `actor.subscribe` and `actor.changed` are removed (decision 4), and with them the subscription table's mark byte per subscription (`host.c:731`) |
  | per binding call | nothing from the allocator once the binding's arena has reached its high-water mark: a call takes a mark and releases to it, and released chunks are kept (decision 5) |
  | per compilation | one pointer in `ta_resolve_request`; a refusal's message copied into a diagnostic (decision 7); the compiler's two chunked arenas become `ta_growable_arena`s (decision 5) |
  | per `TA_EFFECT_REQUEST` | one `ta_host_resume` call in the binding's C, until run-time fetching is built (decision 8) |
  | API | `ta_host_run` refuses while a batch is open; `ta_host_effects` returns the open batch instead of widening it; `actor.subscribe`, `actor.unsubscribe` and `actor.changed` removed, `actor.state` as it is, `limit` included; new in base: `ta_table` (7 functions, ~83 lines in the spike), `ta_status_name` and `ta_growable_arena` (decision 5; built from tinyxml's `txml_arena`, `tinyxml/src/arena.c`, 218 lines); `ta_resolve_request` gains `scratch` and the resolver's out parameter becomes `ta_resolve_result` (decision 7) |
  | callers in this repository | the tests and `ta-conform` that run without acking (`git grep ta_host_run`); every resolver and its tests (decision 7's list); the compiler's 28 calls of `ta_chunk_arena_alloc` and `_strdup` (decision 5) — all migrate in the same change |
  | every binding | less: no drain re-read, no consumed marks, no request facade, no copy before each call, no handle table and no status table of its own, no resolver on its host |

## Context

A maintainer review of 2026-10-13 read the boundary as a binding author
meets it (the headers, ADR 0010, ADR 0011, the guide) and found:

- **Four states of an effect:** appended but not published; published
  and unread; published and consumed by a facade (the binding's own
  mark, ADR 0011 decision 6); published again by a later
  `ta_host_effects` that widened the batch (`src/host/host.c:1200-1201`
  sets `effects_ready = effect_count` on every call). A drain had to
  re-read until the count was stable and ack only at its outermost
  level (bindings.md section 5 before this ADR); a facade that acked
  inside a handler, or published and did not ack before the next run,
  lost effects (ADR 0011 decision 6, reproduced in a spike).
- **Five lifetime classes** of what core hands out: effect records
  until the ack, the next run or `ta_host_free` (`host.h:55-59`);
  `TRACEPARENT`'s text until the next `ta_host_effect_text`
  (`host.h:299-300`; one buffer, `host.c:1265-1268`); the host's
  outputs until the next call into the host (`host.h:62-69`,
  ADR 0010 decision 2); a datamodel's outputs until its next call
  (`datamodel.h:9-13`); the caller's `ta_buffer` (snapshots,
  `environment->error`). A handler that posts, delivers or writes a
  second envelope ends the view it is still reading, so both bindings
  copied every output before any call (`tinyactors-cpython/AGENTS.md`
  and `tinyactors-napi/AGENTS.md`, "Views of what core ... hold", before
  this ADR).
- **A value divergence:** ADR 0011 decision 4 says `actor.changed`
  carries `"value": <state value | null>`; the guide said the same;
  requests.md said `value` is left out past the state-value limit, and
  the code does that (`host.c:755-758`, patching the params map to one
  member). A subscriber had three shapes to read.
- **A synchronous `request` ran inside dispatch.** The facade of
  ADR 0011 decision 6 posts, re-reads `ta_host_effects` to find its
  reply in a widened batch, and drains the rest. With the drain's
  re-read, which picks up what handlers' calls append, it is why a batch
  widened at all.

Reading on, the same day, found:

- **Subscriptions bought nothing the application needs.** Every shape of
  `actor.changed` weighed (History) needed new machinery or left
  problems open, while an actor's output already reaches the
  application through an I/O processor and its state through
  `actor.state`. They had served as a stress test of the protocol: a
  posted message can start a stream of effects, and such a stream has a
  cost in a run.
- **Two chunked arenas, and the same code in every binding.** Each
  binding needs memory for what one call converts, and the compiler
  already has a private chunked arena (`ta_chunk_arena`,
  `src/compile/compile_support.h:14-25`) beside tinyxml's `txml_arena`
  (`tinyxml/include/txml.h:209-252`).
- **A resolver cannot say why it refused.** A refusal is one bare
  diagnostic at the referring element, `cannot resolve src="..."`
  (`src/compile/scxml.c:212`), and a resolver keeps the text it returns
  alive by itself until the compilation ends (`ta_runtime_files` owns
  its texts, `runtime.h:32-37`).
- **The documents put a resolver on the host.** The guide, boundary.md
  and the bindings' documents had the compile call's resolver on the
  `Host`, answering `TA_EFFECT_REQUEST` with a text that a resume has
  nowhere to put (`route.c:544-553`).

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
  ADR 0010 decision 2 left them; nothing in this ADR changes snapshots;
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
`host.c:972-980`).

### 2. One batch at a time

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

and then, at most, a run. There is no nested drain: language code that
runs during a drain (an I/O processor's handler) may add inputs, post or
read outputs; it never takes or acks.

### 3. No synchronous request and no raw post in a language API

The control plane is `ta_host_post` with `REPLY` and `NOTIFICATION`
effects (ADR 0011). A binding exposes **typed methods over post**
(`host_stats`, `actor_state(session)`, ..., one per method of
`requests.md`, named by the language's conventions). Each takes the
method's params, a **response handler** and an optional error handler,
returns nothing, and posts with a fresh id (the handle of a row in the
binding's pending `ta_table`). The drain that handles its `REPLY`, a
later one, calls the handler with the decoded `result`, or the error
handler with the `error` (bindings.md section 10). There is no reply
object, future or awaitable. In Python: `method(..., handler,
on_error=None)` returning `None`; Node's shape of the same rule is
decided in step 8.5.

**No raw `post`** in a language API until a JSON-RPC transport exists.
The binding's own C may post and drain at creation (reading
`host.config`), since no batch is open then; that is the loop, not a
synchronous API.

### 4. `actor.subscribe`, `actor.unsubscribe` and `actor.changed` are removed

They are removed from core, the vectors and the documents [8.2]. They
were a stress test of the protocol and served it. Until 8.2 lands the
code answers them as before, and a binding never subscribes.

**Notifications stay possible:** `TA_HOST_EFFECT_NOTIFICATION` (WIRE
`{"method", "params"}`, EVENT the method, SESSION the session,
`host.h:250-251`) keeps its place in the effect list and in requests.md's
"Notifications" section, with no notification defined. A binding
handles the kind (it reads the method and drops one it does not know)
and offers no API for it until a notification exists.

`actor.state` stays as it is: the state value written in the stack's
room for 1024 states (`host_state_value`, `src/host/host.c:806-816`;
`HOST_STATE_VALUE_STATES`, `src/host/host_internal.h:334`), refused with
`limit` past it (`src/host/request.c:657-660`). There is no new
state-value machinery.

What the removal takes out of core [8.2]: the subscription table and its
mark byte per subscription (`subscription_mark`, `host.c:731`;
`subscriptions_notify`, `host.c:777-797`), the host's use of
`ta_actor_watch` for it (the system's `TA_EFFECT_CHANGED` and
`ta_actor_watch`, `system.h:305-311`, stay below the host, as
`ta_system`'s own API), the methods in `src/host/request.c`, the config
key `maxSubscriptions` (`docs/host-config.md:61`), the subscribe exchange
in `tinyactors-test/suites/protocol/requests.txt`, `host.h`'s comment on
the notifications, and the self-test's check of a heading with two
methods, which names `actor.unsubscribe` (`mise/tasks/api-map-test:17`).

### 5. `ta_table`, `ta_status_name` and `ta_growable_arena` are public, in base

Every binding keeps handle tables for what core stores as integers
(sessions, payloads, charts, loops, pending typed methods), names
statuses in its errors, and needs memory for what one call into it
converts (a config, a typed method's params, a payload for another
datamodel, text and input structs). All three are free of any language's
API, so they belong in a C layer (`AGENTS.md`, "Layers"), and a binding
writes none of its own.

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
built from tinyxml's `txml_arena` (`tinyxml/include/txml.h:209-252`,
`tinyxml/src/arena.c`), taking its linked chunks and its marks
(`txml_arena_mark`, `txml.h:232`; `txml_arena_position` and
`txml_arena_release`, `:246-249`), with two additions: **a release that
keeps its chunks**, and a `ta_allocator` view. The names below are a
sketch; 8.2 settles them, the rules are decided.

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
- **A binding keeps one per host**, made with the host from the
  environment's allocator and freed with it, and brackets each API call
  with a mark (bindings.md section 6).

**One chunked arena in core.** The compiler's private `ta_chunk_arena`
(`src/compile/compile_support.h:14-25`, 28 call sites of `_alloc` and
`_strdup` in `src/compile/`) moves onto `ta_growable_arena` in 8.2 and
is deleted. It serves two arenas, and both become
`ta_growable_arena`s: the compiler's per-compilation arena
(`compile.c:37`, freed at `:87`: source texts,
`compile_internal.h:11-16`, `link.c:39-40`; the XML parser's nodes,
`xml.c:267`), which is the resolver's scratch (decision 7), and the
compile result's (`compile.c:13`, `:24`, freed by `ta_compile_free` at
`:275`: the diagnostics' file and message strings,
`compile_support.c:226-228`).

**The internal `ta_arena`** (`src/base/arena.h`: carving a caller's
region, offsets, measure mode, used by `ta_system_layout` and
`ta_system_init`) keeps its name and its job; it never allocates.

### 6. What reaches the application

- **`TA_EFFECT_DONE` and `TA_EFFECT_ERROR` are internal to the
  binding's C.** They are lifecycle: the binding releases the rows it
  keeps for the session and settles what waits on it. They are not
  application callbacks. The done data's payload is retained by core
  when the effect is copied and released at the ack
  (`host.c:1187-1191`, `:1044-1048`); a fault is counted in
  `host.stats` (`faults`, requests.md).
- **Actor output to the application goes through I/O processors:** a
  chart that has something for the application `<send>`s it to an I/O
  processor the application configured (`TA_EFFECT_SEND`, synchronous
  or not), whose handler may answer or fail it. That is SCXML's channel
  for output, and the one that carries a target, a name and data.
- **`TA_EFFECT_LOG` goes to a log handler configured per host**
  (Python's `Host(..., log=handler)`), not to a callback per actor and
  not to a global configuration such as `logging.basicConfig` or a
  console default; without a handler, the lines are discarded.
- **State and inspection are typed methods over post** (decision 3),
  state by `actor.state`; there is no subscription (decision 4).
- **`PEER` versus `PEER_ERROR` in a language API is deferred.**

### 7. The resolver lives at compile time, reports its errors and lends a scratch arena

A static `src` (`<script>`, `<data>`, `<invoke>`) is resolved during
compilation through `ta_compile_options.resolve`
(`src/compile/scxml.c:201-216`, called at `:599`, `:860`, `:968`; a
manifest's `<chart src>` at `compile.c:191-194`). **That is the only
resolver:** a static `<invoke src>` is fetched through it while
compiling, and the invoked chart is compiled into the same image
(`scxml.c:893-896`, `resolve_chart` at `:855-870`). A host has none; a
binding gives a resolver to its compile call only.

[8.2] changes the callback in place, keeping its three parameters:

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
  bytes, `:208`), instead of the bare `cannot resolve src="..."`
  (`scxml.c:212`).
- **The scratch arena is the compiler's per-compilation arena**
  (decision 5), lent to the resolver: the resolved text and the message
  may live there, and the compiler copies what it keeps (a queued
  document's text, `link.c:40`; an expression's source, `scxml.c:116`).
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

### 8. `TA_EFFECT_REQUEST` stays, for fetching at run time

`TA_EFFECT_REQUEST` is how a chart fetches at run time: an
`<invoke srcexpr>` (or a `src` the compiler did not match, one with a
`typeexpr`, `scxml.c:895`) whose chart is not in the image. It must stay
supported, though the fetching is built out later. Core normalizes the
name itself (`file://`, `file:`, `./`, an inline document) and looks it
up in the requesting segment's image (`route.c:439-489`); it gives the
REQUEST, the name in `AUXILIARY`, only when that lookup fails
(`:491-497`). Nobody after core repeats the normalization.

Until the fetching is built, a binding answers every REQUEST in its C,
never in the language, with `ta_host_resume` and `TA_ERROR_INVALID`, and
the parent gets `error.execution` (`route.c:544-550`): a resume names
only a chart of the requesting segment's image (`:544-553`), and core
has already looked there. What the build-out needs from core, and how a
binding is told to fetch, are in `ROADMAP.md`, "Decided later".

Only what [docs/SCXML.md](../SCXML.md) mandates is supported. SCXML 1.0
defines no `srcexpr` on `<data>` (only `src`, `expr` or children,
`docs/SCXML.md:1317`), so a `<data srcexpr>` is not part of this design.

A REQUEST is not the control plane: `ta_host_post`, `REPLY` and
`NOTIFICATION` ([ADR 0011](0011-the-control-plane-is-posted-messages.md))
are a different mechanism.

## What changes elsewhere

- `tinyactors-core/include/tinyactors/host.h`: the comments on effects'
  and outputs' lifetimes, `TRACEPARENT`, the run's refusal and the
  notifications, in the commits that land each decision [8.2].
- `tinyactors-core/docs/boundary.md` (a row per call; sections 2, 3, 6,
  7, 9 to 12), `embedding.md`, `bindings.md` (sections 3 to 6 and 10)
  and `requests.md` (publication; `actor.subscribe` and `actor.changed`
  removed): written for this ADR in step 8.1, with "(0012)" and "[8.2]"
  marking what the code does not do yet.
- `tinyactors-core/AGENTS.md`: what the host hands out lives until the
  ack; base holds the table and the status names.
- `docs/host-config.md` (`maxSubscriptions`),
  `tinyactors-test/suites/protocol/requests.txt` (the subscribe
  exchange) and `mise/tasks/api-map-test` lose the subscriptions [8.2].
- Each binding's `AGENTS.md` and `ROADMAP.md`: the loop, thin views valid
  until the ack, typed methods with response handlers, `ta_table`, one
  `ta_growable_arena` per host, the resolver on the compile call only,
  and REQUEST answered in C.
- `ROADMAP.md`, step 8: 8.2 implements this ADR.

## Rejected alternatives

- **Not "until the next call into the host" for outputs**, because a
  handler that posts, delivers or writes a second envelope ends the view
  it is still reading, as both bindings' rules to copy every output
  before any call show (their `AGENTS.md`, "Views ...", before this
  ADR); the ack is where the caller already says it is done.
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
- **Not a subscription to an actor's state, in any shape**, because
  each shape weighed needed new state-value machinery or left problems
  open (History), and it bought nothing the application needs: an
  actor's output reaches it through an I/O processor, and its state
  through `actor.state`.
- **Not txml's release, which frees chunks past one spare**
  (`tinyxml/src/arena.c:169-202`), because a binding takes and releases
  a mark on every call: freeing at release calls the allocator per call,
  where keeping the chunks calls it only while the high-water mark grows.
- **Not one writer per call on the C stack, grown through the host's
  allocator and freed at the call's end** (bindings.md section 6 before
  this ADR), because a call that converts several values (a `send_many`
  of event structs, text and payloads) needs several blocks, each a pair
  of allocator calls per call; the arena serves them all from kept
  chunks.
- **Not the internal `ta_arena` made public**, because it carves one
  region given up front, by offset, with a measure mode
  (`src/base/arena.h:1-13`): it cannot grow, and its job (the system's
  layout) stays.
- **Not two chunked arenas in core**, the compiler's `ta_chunk_arena`
  beside `ta_growable_arena`, because both are bump allocators over
  linked chunks from a `ta_allocator` (`compile_support.h:14-25`): the
  same job twice.
- **Not a handle table in each binding**, because the same ~80 lines
  were written twice (`src/datamodel/handles.c` in both bindings, before
  their removal in `6426816a` and `70434bee`), and the audit compared
  them file by file; the spike's public variant needs no header of its
  own in a row (`table_public.c`, the coordinator's spike of
  2026-10-13).
- **Not a status table in each binding**, because the CPython binding
  had one (`ta_python_status_names`, `src/module.c` at `6426816a^`) that
  every other binding would repeat.
- **Not a fourth parameter for the resolver's error**, because public
  callbacks take at most three (root `AGENTS.md`, "At most three
  parameters"); the out parameter becomes a struct instead.
- **Not a resolver that keeps every text alive by itself until the
  compilation ends** (as `ta_runtime_files` does, `runtime.h:32-37`),
  because the compiler already has an arena with exactly that lifetime.
- **Not the compile call's resolver kept on the host to answer a
  REQUEST with a text**, because a resume names only a chart of the
  requesting segment's image (`route.c:544-553`), so the text has
  nowhere to go.

## Order of work

Step 8.2 of [ROADMAP.md](../../ROADMAP.md), after these documents
(step 8.1):

1. **Base:** `ta_status_name`, `ta_table` and `ta_growable_arena`, with
   unit tests (`tests/unit/test_table.c`: add, get, remove, a stale
   handle after reuse, growth through a refusing allocator, free;
   `tests/unit/test_growable_arena.c`: an allocation that never moves,
   an oversized request, nested marks, a release that keeps its chunks
   and calls the allocator no more on the next round, a writer growing
   through its `ta_allocator`, a refusing allocator, free), and their
   entries in `tinyactors.api.json`.
2. **One chunked arena:** the compiler moves from `ta_chunk_arena` onto
   `ta_growable_arena`, and `ta_chunk_arena` is deleted (decision 5).
3. **The resolver** takes its scratch and reports its errors
   (decision 7), every caller and implementer migrating, with a test of
   a refusal's diagnostic (file, line, column, message) and of a text
   left in scratch.
4. **One batch at a time:** `ta_host_effects` returns the open batch;
   `ta_host_run` refuses while one is open; tests and `ta-conform`
   migrate (each takes, handles, acks before a run).
5. **Outputs until the ack:** the batch's output memory, the six output
   calls on it, `TRACEPARENT` on first ask; a test that takes two
   envelopes and a metrics view in one batch and reads all three after
   the second call (ASan).
6. **`actor.subscribe`, `actor.unsubscribe` and `actor.changed`
   removed** (decision 4): the code, the vectors, the config key
   `maxSubscriptions`, `host.h`'s comment and the self-test that names
   them, in one commit; `requests.md` then drops their heading.
   `actor.state` does not change.
7. `host.h`'s comments, `requests.md` and the guide follow in the same
   commits (`tinyactors-core/AGENTS.md`), and the "(0012)" and "[8.2]"
   marks leave the documents as each lands.

The rest of step 8.2 (boundary.md's findings, the contract table's
missing tests, `detach_event`) is the roadmap's, not this ADR's. Then
step 8.3 (the full loop end to end in C) and step 8.4 (the minimal
Python binding).

## Consequences

- One lifetime for everything the host hands out: until the ack.
- The drain is a loop of take, handle, ack; no binding re-reads, marks
  or copies to survive its own calls.
- Memory held between take and ack grows with what a batch's handling
  takes; acking often bounds it.
- Language APIs have no synchronous control-plane call; their typed
  methods settle at a later drain, through response handlers.
- The host has no notification defined and no subscription; the effect
  kind waits for one.
- A binding's tables, status names and per-call memory are core's; after
  warm-up its calls reach no allocator.
- Core has one chunked arena; the compiler's moves onto it.
- A resolver's refusal is a diagnostic at the resolved file's line.
- The host has no resolver. A chart that names at run time a document
  not in its image gets `error.execution` until run-time fetching is
  built.

## History

All on 2026-10-13, the day this ADR was accepted and amended five times.
Each line: what was first decided, and what replaced it. The text of
each version is in git (`2867861f`, `30fd4ad3`, `670d3088`, `7afb8ab8`,
`ccad425b`, `934ff50b`, `69a57ad4`).

- **Decision 4, accepted:** `actor.changed` always carried the whole
  state value after the run (`null` at the end), and the 1024-state
  limit went, with scratch from the host's allocator past the stack's
  room and `actor.state` losing `limit`. **First amendment:** one
  `actor.changed` per macrostep with the states entered and exited, no
  new state-value machinery, `actor.state` as it is; five problems left
  open (the capture without allocation, the outbox, a busy actor's
  flood, the last macrostep, a refused notification). **Second
  amendment:** `actor.subscribe`, `actor.unsubscribe` and
  `actor.changed` removed; notifications stay possible (decision 4).
- **Rejections of the first decision 4** (a truncated value with
  `"limit": true`; `null` past the limit, which meant the session ended;
  leaving `value` out, the code's behaviour) were dropped with the
  subscription they were about.
- **Decision 3, accepted:** a typed method returned the language's
  pending result (a promise, a future). **Second amendment:** it takes
  a response handler and an optional error handler and returns nothing.
- **Decision 6, accepted:** `<log>` lines went to the language's logging
  (Python's `logging`, the console); state reached the application by
  subscription. **Second amendment:** a log handler per host, no global
  configuration; state by `actor.state`.
- **Decision 5, accepted:** `ta_table` and `ta_status_name`, with a
  binding's per-call memory one writer on the C stack grown through the
  host's allocator. **Second amendment:** `ta_growable_arena` public,
  a port of `txml_arena` whose release keeps its chunks. **Third
  amendment:** built in base from `txml_arena`'s chunks and marks, and
  the compiler's `ta_chunk_arena` moves onto it.
- **Decision 7, added by the third amendment:** the resolver's errors
  and its scratch arena. **Fourth amendment:** the resolver lives only
  at compile time; the documents had put the compile call's resolver on
  the `Host` to answer `TA_EFFECT_REQUEST`.
- **REQUEST, fourth amendment:** answered in C by looking the name up
  with `ta_image_chart_by_name` in the requesting segment's image, with
  an [8.2] item making `ta_image_chart_by_name` normalize `file://`,
  `file:` and `./`. **Fifth amendment:** `TA_EFFECT_REQUEST` stays for
  run-time fetching and is answered with `TA_ERROR_INVALID` until that
  is built, since core has already looked; the normalization item was
  withdrawn, the normalization being core's before its own lookup
  (decision 8).
- **Fifth amendment, first version:** named `<data srcexpr>` among the
  REQUEST's sources. **Replaced:** only what `docs/SCXML.md` mandates is
  supported, and SCXML 1.0 defines no `<data srcexpr>` (decision 8).
- **Order of work, accepted:** item 4 was "`actor.changed` with `value`
  always", a state value without the 1024 limit. **Second amendment:**
  the removal of the subscriptions; the third added the arena's move
  and the resolver's change (items 2 and 3).

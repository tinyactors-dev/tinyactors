# ADR 0012: Lifetimes follow the loop

**Status:** Accepted (2026-10-13). Amends [ADR 0010](0010-outputs-are-views-values-stream.md)
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
  control plane runs inside an effect handler; a subscriber always gets
  a state value; actor output arrives through the channel SCXML has for
  it (an I/O processor).
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
  | per subscribed session with a configuration over 1024 states | scratch for the state value from the host's allocator at the end of the run (6 bytes per active state, `host.c:807-808`); nothing for other charts |
  | API | `ta_host_run` refuses while a batch is open; `ta_host_effects` returns the open batch instead of widening it; `actor.state` loses its `limit` error; new in base: `ta_table` (7 functions, ~83 lines in the spike) and `ta_status_name` |
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
(`host.stats()`, `actor.state(session)`, `actor.subscribe(session)`, ...,
one per method of `requests.md`), each of which posts with a fresh id
and returns the language's pending result (a promise, a future), settled
when its `REPLY` is handled in a later drain. **No raw `post`** in a
language API until a JSON-RPC transport exists. The binding's own C may
post and drain at creation (reading `host.config`), since no batch is
open then; that is the loop, not a synchronous API.

### 4. `actor.changed` always carries `value`

`{"method": "actor.changed", "params": {"session": uint, "value":
<state value | null>}}`. `value` is always present: the session's state
value after the run, whole, or `null` when the session ended or left the
host (its subscription with it).

**The state-value limit goes.** `host_state_value` writes any
configuration: when it holds more states than the 1024 the stack has
room for (`host.c:807-816`, `HOST_STATE_VALUE_STATES`), it takes its
scratch from the host's allocator, sized from the chart's state count
(`ta_image_state_count`, `image.h:45`). The notification is written at
the end of a run, outside the system's run, where the host already grows
its output and its effect pool (`host.c:742`, `:760`; `host_output`
asserts `!host->running`, `host.c:43`). When the allocator refuses, the
path that exists today holds: the session keeps its mark, the run
answers `TA_RUN_EXHAUSTED`, and the notification comes at the end of the
next run (`host.c:785-790`; requests.md, `actor.changed`). `actor.state`
loses its `limit` error (`request.c:657-660`) and answers `memory`
(-32004) when the allocator refuses.

### 5. `ta_table` and `ta_status_name` are public, in base

Every binding keeps handle tables for what core stores as integers
(sessions, payloads, charts, loops) and names statuses in its errors.
Both are free of any language's API, so they belong in a C layer
(`AGENTS.md`, "Layers").

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

### 6. DONE, ERROR and LOG are the binding's; output goes through I/O processors

- **`TA_EFFECT_DONE` and `TA_EFFECT_ERROR` are internal to the
  binding's C.** They are lifecycle: the binding releases the rows it
  keeps for the session and settles what waits on it (its
  subscriptions end; `actor.changed` with `null` says the same to a
  subscriber). They are not application callbacks. The done data's
  payload is retained by core when the effect is copied and released at the ack (`host.c:1187-1191`, `:1044-1048`); a fault
  is counted in `host.stats` (`faults`, requests.md).
- **Actor output to the application goes through I/O processors:** a
  chart that has something for the application `<send>`s it to an I/O
  processor the application configured (`TA_EFFECT_SEND`, synchronous
  or not). That is SCXML's channel for output, and the one that carries
  a target, a name and data.
- **`TA_EFFECT_LOG` goes to the language's logging** (Python's
  `logging`, the console or a logger in Node), not to an application
  callback.
- **State reaches the application by subscription** (`actor.subscribe`,
  `actor.changed`), and inspection by typed methods over post
  (decision 3).
- **`PEER` versus `PEER_ERROR` in a language API is deferred.**

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
- **Not a truncated state value with `"limit": true`**, because a
  truncated configuration of a parallel state lists fewer regions than
  its chart has, a value `actor.state` never answers, and every
  subscriber needs a second branch. **Not `null` past the limit**,
  because `null` means the session ended and its subscription went with
  it (`host.c:789-790`). **Not leaving `value` out** (today's code),
  because ADR 0011, the guide and requests.md disagreed on it, and a
  reader had three shapes.
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

1. `ta_status_name` and `ta_table` in base, with unit tests
   (`tests/unit/test_table.c`: add, get, remove, a stale handle after
   reuse, growth through a refusing allocator, free), and their entries
   in `tinyactors.api.json`.
2. One drain at a time: `ta_host_effects` returns the open batch;
   `ta_host_run` refuses while one is open; tests and `ta-conform`
   migrate (each takes, handles, acks before a run).
3. Outputs until the ack: the batch's output memory, the six output
   calls on it, `TRACEPARENT` on first ask; a test that takes two
   envelopes and a metrics view in one batch and reads all three after
   the second call (ASan).
4. `actor.changed` with `value` always: the state value without the
   1024 limit, `actor.state` without `limit`, a unit test with a
   configuration of more than 1024 states, the vectors.
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

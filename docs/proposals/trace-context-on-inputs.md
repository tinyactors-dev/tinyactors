# Trace context on inputs: a struct in, text only at the edges

**Status:** Proposed (2026-10-10).
**Touches:** tinyactors-core: `include/tinyactors/system.h` (`ta_event_in`,
`ta_system_spawn`), `include/tinyactors/host.h` (`ta_host_spawn`,
`ta_envelope`, `ta_host_message`, `ta_host_error_event`), `src/core`
(`event_pool.c`: the `_event` view), `src/system` (`schedule.c`,
`system.c`, `tracing.c`), `src/host` (`host.c`, `router.c`),
every caller (`bindings/shared/binding.c`, `bindings/postgres`,
`tools/torture/bench.c`, `examples/access-reconciler`, `tests/`).
Complements [A thin JavaScript binding](thin-js-binding.md), which does not
cover the reply path.
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Summary

A trace context leaves the system as a struct (`ta_effect.trace`,
`ta_host_effect.trace`) but comes back in as W3C text
(`ta_event_in.traceparent`), plus a `trace_flags` field for the bits the
text cannot hold. A host that answers a send has to format the struct into
text so core can parse it straight back, and if it forgets, the trace
breaks: the reply starts a new trace.

This proposal makes inputs take the same struct, keeps text for the two
edges that need it (an HTTP header, the link envelope's JSON), and lets an
input name the I/O processor it comes from by index, so a reply to a send
is three field copies from the send's effect (session, trace, processor)
and costs no strings. It is the smallest change that does this: two
fields on `ta_event_in`, one on `ta_envelope`, no new type and no new
function.

## Whom it serves, and who pays

Following [docs/roles.md](../roles.md):

- **Served first: the application developer and the observability
  backend.** The developer's request ends in one trace: a database call made
  through an I/O processor, and the step its answer starts, hang under the
  step that asked for it. The backend stops receiving orphan traces, one per
  reply.
- **The binding author:** answering a send is one rule, the same in every
  language: copy `session`, `trace` and `io_processor` from the send's
  effect. The binding does it once for every I/O processor, so application
  code never sees it. No formatting or parsing of trace text.
- **The C embedder and the maintainers:** no text round trip inside the
  process. Today every message between two segments of one host is
  formatted into W3C text and parsed back; after this it is a 32-byte copy.
  A reply interns no origin or origintype strings.
- **Who pays:** an in-place change to `ta_event_in`, `ta_envelope`,
  `ta_system_spawn`, `ta_host_spawn` and `ta_host_error_event`, and every
  caller migrated (about 25 files, listed under Touches). `ta_event_in`
  grows by 16 bytes (it is passed, not stored). Nothing changes per event
  inside a system; a queued envelope gets 23 bytes smaller (a 32-byte
  struct instead of 55 bytes of text). A C host that answers sends must do
  the three copies itself; a test makes forgetting the trace fail.
- **Out of scope:** a span for the time between a send and its reply, a
  request as a root span that ends when its work ends, and `tracestate` for
  carrying a detail class across processes. They build on this; see
  "Afterwards".

## Where things stand

Out:

- `ta_effect.trace` and `ta_host_effect.trace` are a `ta_trace_context`
  (`include/tinyactors/trace.h`): trace ID, the sending step's span ID as
  `parent`, and flags (the W3C byte, and the detail class above it, local to
  the process). A SEND to an I/O processor carries it.

In:

- `ta_event_in.traceparent` is `ta_string_view` W3C text, and
  `ta_event_in.trace_flags` carries the detail class beside it (added by
  [ADR 0007](../adr/0007-metrics-and-trace-policy.md) because the text has no
  room for it).
- `ta_system_spawn` and `ta_host_spawn` take `ta_string_view traceparent`.
- `ta_system_start_trace` (`src/system/tracing.c`) parses the text with
  `ta_traceparent_parse` for every input that has one.
- The router keeps text too: `ta_envelope.traceparent` plus
  `ta_envelope.trace_flags`; `ta_host_message` has a 55-byte `traceparent`
  buffer; `router.c` formats the effect's struct into it (`:698`) and
  `local_deliver` hands the text to `ta_event_in` (`:530`), which parses it
  again, even when both ends are segments of one host.

Replies:

- A host answers a send with `ta_host_send(h, session, &event)`. Nothing
  ties that event to the send it answers: the host must copy the sendid,
  set `origin` and `origintype` as strings, and format the effect's trace
  into `event.traceparent`. Core then interns the origin and origintype
  strings in the slab for every reply (`schedule.c:417`).
  `ta_host_error_event(sendid, &event)` fills an `error.communication` but
  not its trace. The JavaScript bindings do not answer sends to I/O
  processors at all today.
- Core already keeps the processor table it was configured with
  (`core->io_processors`, re-supplied on restore), and builds the `_event`
  view when an event is bound (`event_pool.c:161`).

## The change

### 1. Inputs take a `ta_trace_context`

```c
typedef struct ta_event_in {
    ...
    ta_string_view raw;
    /* The trace the macrostep this event starts runs under: the sender's
     * context as an effect gave it (`parent` is the sending step's span),
     * or one parsed from a traceparent with ta_traceparent_parse. A zero
     * trace ID: none (a new trace, or none if unsampled). Flags above the
     * W3C byte (the detail class) count only from this process. */
    ta_trace_context trace;
} ta_event_in;

TA_API ta_status ta_system_spawn(ta_system *system, uint32_t chart, ta_string_view name,
                                 ta_payload params, const ta_trace_context *trace,
                                 uint32_t *actor);   /* NULL: none */
TA_API ta_status ta_host_spawn(ta_host *h, uint32_t segment, uint32_t chart, ta_string_view name,
                               ta_payload params, const ta_trace_context *trace,
                               uint64_t *session);
```

- `traceparent` and `trace_flags` go. `ta_system_start_trace` takes a
  `const ta_trace_context *` and parses nothing.
- Copying an effect's `trace` into an input's `trace` is exactly right: the
  effect's `parent` is the sending step's span, which is what the new step's
  parent must be, and the detail class travels with it.
- Text stays at the edges, with the functions that exist:
  `ta_traceparent_parse` for an incoming HTTP header or a link envelope,
  `ta_traceparent_format` for an outgoing one.

### 2. Envelopes carry the struct; text only on the wire

```c
typedef struct ta_envelope {
    ...
    ta_trace_context trace; /* the sending step's trace (zero: none) */
} ta_envelope;
```

- `trace_flags` and `traceparent` go from `ta_envelope`, and the 55-byte
  buffer from `ta_host_message`. Queued envelopes keep the struct.
- `ta_host_envelope_json` formats `traceparent` when it writes the wire
  form, and only then. The receiving binding parses the wire field with
  `ta_traceparent_parse` before `ta_host_deliver` (in its C glue, not in
  its language).
- Between segments of one host, the struct passes through untouched, so
  the detail class needs no side channel. Across processes it is lost, as
  today (see `tracestate` under "Afterwards").

### 3. Replies name their I/O processor by index

```c
typedef struct ta_event_in {
    ...
    ta_trace_context trace;
    /* The I/O processor this event comes from: its index in the system's
     * io_processors plus one, 0 for none. `_event.origintype` is its type
     * and `_event.origin` its location; `origin` and `origintype` above
     * must then be empty. */
    uint32_t io_processor;
    uint32_t pad;
} ta_event_in;
```

- The event records the index, tagged in its `origintype` field as image
  and slab strings already are; the `_event` view resolves the type and
  location from `core->io_processors` when the datamodel binds the event.
  No string is interned per reply.
- `ta_effect.io_processor` and `ta_host_effect.io_processor` already hold
  the same index (without the plus one): a reply copies it, adding one.
- Indices must mean the same processors after a restore. The host
  re-supplies its table on restore already; a binding registers its
  processors in a fixed order (sorted by type).
- `ta_host_error_event` takes the send's effect instead of a sendid, and
  fills `error.communication` with the effect's sendid, trace and
  processor:

  ```c
  /* error.communication for a send that failed: its sendid, trace and processor. */
  TA_API void ta_host_error_event(const ta_host_effect *send, ta_event_in *event);
  ```

  A host that fails a send later than the effect's lifetime keeps the same
  three fields plus the sendid and fills the event itself.

So a reply is three copies: `session` (to address it), `trace`,
`io_processor`; plus the sendid when the send had one and the chart needs
it. Generated sendids are short, and charts often correlate by data
instead. What a host keeps per send in flight is about 48 bytes, plus the
sendid if it has one.

## What it looks like in a host

```c
/* the effect loop: remember what the reply needs */
case TA_EFFECT_SEND: {
    pending *p = pending_new();                         /* the host's own table */
    p->session = effect->session;
    p->trace = effect->trace;
    p->io_processor = effect->io_processor + 1;
    start_query(p, effect->payload);                    /* async: database, HTTP */
    break;
}

/* later, when the query is done */
ta_event_in event = {.size = sizeof event, .name = VIEW("db.result"), .data = rows,
                     .trace = p->trace, .io_processor = p->io_processor};
ta_host_send(host, p->session, &event);                 /* continues the step's trace */
```

An HTTP server bridging an incoming request:

```c
ta_event_in event = {.size = sizeof event, .name = VIEW("place")};
ta_traceparent_parse(header_value, &event.trace);    /* the request span is the parent */
ta_host_send(host, orders, &event);
```

## Increments

Each builds and passes `mise run test`, with the targeted tasks named.

1. **Inputs take the struct.** `ta_event_in.trace`, the spawn functions,
   `ta_system_start_trace`; migrate every caller (the JavaScript bindings'
   C glue parses their `traceparent` option there; postgres, the torture
   bench, the example, tests). Tests: `test_tracing` (an effect's trace
   copied into an input continues it, class included), `test_router`,
   `test_durable`, `conformance`, `node:test`, `web:test`.
2. **Envelopes carry the struct.** `ta_envelope`, queued envelopes,
   `ta_host_message`, `ta_host_envelope_json`; the bindings parse the wire
   field before `ta_host_deliver`. Tests: `test_router` (a trace crosses two
   segments and two hosts unbroken, the class only within the process),
   `node:test` (links).
3. **Replies by processor index.** `ta_event_in.io_processor`, the tagged
   origintype and its resolution in the `_event` view, `ta_host_error_event`
   taking the effect; the torture bench's I/O processors answer this way.
   Tests: `test_host` and `test_tracing` (a reply and a failure both
   continue the sending step's trace; a reply without its trace fails the
   test; `_event.origintype` and `_event.origin` are the processor's, also
   after a restore), `test_durable` (an event from a processor, hibernated
   and restored), `torture:test`, `w3c` (the BasicHTTP tests still pass).
4. **Measure.** Release builds, side by side with the commit before
   increment 1, throughput and memory together, machine named, in the
   `federation` scenario (messages between systems) and in a scenario with
   two segments of one host exchanging events, where today each message is
   formatted and parsed. Report per-message cost before and after, and
   region size (queued envelopes). This is the proposal's performance
   evidence; if it shows nothing, say so.
5. **Docs.** `architecture.md` (section 11), ADR 0006's input section,
   `docs/durability.md` if anything it lists changes, the bindings' READMEs
   where they explain `traceparent`.

The JavaScript rewrite ([thin-js-binding.md](thin-js-binding.md)) then
answers its I/O processors' sends with these copies, once, in its glue.

## Afterwards (not in this proposal)

- **A span for the wait.** With replies carrying the send's context and
  sendid, the OTLP encoder can pair a `sent` record with the step its reply
  starts and emit an "io <type>" span between them, child of the sending
  step and parent of the reply's step: database latency in span metrics
  with no instrumentation.
- **A request as a root that ends when its work ends.** A host-written root
  span, and per-trace counting of live work (events, timers, steps, sends
  in flight) so the host knows when a request is finished, rather than when
  the whole system is idle.
- **`tracestate`** (`ta=c<n>`) on link envelopes, so a detail class (a
  watch) crosses processes.

## Decisions

1. **The index on `ta_event_in`, or origin and origintype as strings?**
   Recommended: the index. It costs one field, no strings per reply, and
   the `_event` view gives the chart the processor's type and location as
   SCXML specifies. A host can still set `origin`/`origintype` strings for
   inputs that come from no registered processor.
2. **No new reply type or helper; three documented copies and a test?**
   Recommended: yes. A value type with helpers (an earlier draft) made
   forgetting impossible for C hosts, but cost a type, three functions and
   about 300 bytes per send in flight; the bindings do the copies once, and
   the test catches a C host that forgets.
3. **No text in the C input API at all?** Recommended: yes. Text exists
   only through `ta_traceparent_parse`/`_format`. The JavaScript public API
   keeps a `traceparent` string option for HTTP bridging; the binding
   parses it in its C glue.
4. **Keep the wire format (`"traceparent"` text in envelope JSON)?**
   Recommended: yes. It is the W3C form other systems read; only the
   in-process representation changes.

# ADR 0068: Core owns the I/O processor registry, and every origin can be answered

**Status:** Accepted; refines ADR 0015, ADR 0016, ADR 0040, ADR 0046 and ADR 0065. Revised in place while Proposed:
- 2026-09-24, first review: origins reference the registry instead of copying from it, and text is rendered on demand.
- 2026-09-24, second review: I/O origins use three storage classes (computed, scoped, per message) instead of processor tokens with an inverse callback.
**Date:** 2026-09-24

## Context

SCXML lets a chart answer whoever sent it an event. The idiom is:

```xml
<send typeexpr="_event.origintype" targetexpr="_event.origin" event="reply"/>
```

For this to work, `_event.origin` must be a target URI and
`_event.origintype` must name the processor that reaches it. SCXML also
tells a chart which processors exist and how to reach this session through
each of them: the `_ioprocessors` system variable. Three places in core do
not meet the specification. They were found while checking the proposal
`docs/proposals/distributed-systems.md` against the W3C text.

### G1: an event from an I/O processor has no origin a chart can use

SCXML 5.10.1 says that for external events the processor SHOULD set
`origin` to a value that, used as a `<send>` target, reaches the sender,
and `origintype` to the processor type that goes with it.

`ta_origin` has a `TA_ORIGIN_IO` variant, but it holds only an opaque
`uint64_t`. `ta_dm_host_text()` renders `_event.origin` and
`_event.origintype` only for actor and invoke origins (`src/datamodel.c`).
For anything else they are empty. No caller in this repository fills
`TA_ORIGIN_IO`: the napi addon and the test host deliver the events their
processors receive with no origin at all. So an actor cannot reply to a
browser, a worker, an HTTP client or another system, except through a
convention the chart and the host agree on outside SCXML.

### G2: `_ioprocessors` lists only the SCXML processor

SCXML 5.10 says the processor MUST bind `_ioprocessors` to a set of values,
one for each Event I/O Processor it supports. B.2 (the ECMAScript data
model) makes it an object with one property per processor, named like the
processor. C.1.1 and C.2.3 add that the SCXML and Basic HTTP entries MUST
have a `location` field: an address that outside parties can use to reach
this session through that processor. For other processors, SCXML leaves
the contents to the platform: "the nature of the values associated with
the individual Event I/O Processors depends on the Event I/O Processor in
question" (5.10).

Core exposes only the SCXML processor, through two text kinds:
`TA_DM_TEXT_SCXML_IO_TYPE` and `TA_DM_TEXT_SCXML_IO_LOCATION`. Processors
registered with `ta_system_register_io()` are invisible to charts. The test
host's ECMAScript and JSON data models, and the napi addon's JavaScript data
model, all build `_ioprocessors` from those two kinds, so all three have the
same gap.

### G3: an event from an invoked service names the wrong processor

SCXML 6.4 says a chart sends ad hoc messages to an invoked process with a
`<send>` whose `type` is the `<invoke>`'s type and whose target is
`#_<invokeid>`. The SCXML processor's own `#_<invokeid>` target reaches
only SCXML sessions that the sender created by `<invoke>` (C.1). Core does
that correctly: `ta_invoke_destination()` resolves the target only for an
invocation with a child actor.

But when an event has `TA_ORIGIN_INVOKE`, core writes `_event.origin` as
`#_<invokeid>` and `_event.origintype` as the SCXML processor's type,
whatever type the invocation has (`src/datamodel.c`). For a non-SCXML
service, for example a websocket invoked with `type="ws"`, the reply idiom
then picks the SCXML processor, which cannot reach the service. The reply
fails with `error.communication`.

A second problem hides behind G3. The invocation's type is known when it
starts (`ta_active_invoke.type`), but the origin does not carry it. And no
path fills `TA_ORIGIN_INVOKE` for a service's own events:
- `ta_actor_report_invoke()` copies the event the host gives it, origin
  included, and a `done` report has no origin;
- the napi addon posts invoker events into the parent's mailbox (napi
  roadmap step 13), so they reach `ta_actor_deliver()` with an invoke ID
  but no origin.

### The text is enough for the reply

A reply reaches the processor as target text:
`<send type="…" target="…">`. So the URI text alone must already be
enough for the processor to route the reply. Anything else an origin
could hold, such as a token, is only a cheaper way to store that text. The
real question is how long the text must live, and that depends on where
the event came from:

| Origin | Lifetime of its URI |
|---|---|
| a peer link, or another actor system | effectively static |
| a websocket, `MessagePort` or TCP connection | the connection |
| an authenticated user or session spanning connections | the session |
| an inbound HTTP request, webhook or RPC awaiting a response | one message, answered once |
| a message broker's reply-to plus a correlation ID | one message |
| a Basic HTTP callback URL | one message |
| a post from JavaScript, a timer, a cron job | no origin |
| an invoked service | an invoke origin, whose lifetime core already tracks |

### Constraints

- **Size.** `ta_origin` is 24 bytes and `ta_event` 64 bytes (measured with
  `sizeof` on a 64-bit host). Every queued event, held event, envelope and
  log record carries one. The fix should not grow them.
- **Cleanup without callbacks.** Core copies and drops events in many
  places (queues, held events, images). It must be able to end an origin's
  storage without calling back into the processor.
- **Determinism.** A chart must read the same `_event.origin` when its
  history is replayed (`docs/proposals/replay-persistence.md`).
- **Allocation.** No allocation per message in steady state.

### Designs considered

1. **The host supplies `_ioprocessors`, or the data model asks the host.**
   Rejected. The host would keep a second copy of a list core already has
   (napi ADR 0001, rule 1), and data models are shared between hosts.
2. **Core renders location templates.** Rejected. A URI's shape and
   escaping belong to the processor that defines the scheme.
3. **Each origin copies the processor type and a retained URI value.**
   Rejected. It grows `ta_origin` to 32 and `ta_event` to 72 bytes, and
   retains a host value per event.
4. **Every I/O origin is a processor token, rendered by a callback, with
   an inverse callback for images** (this ADR's first revision). Rejected.
   - Every processor would keep an unbounded table from tokens to URIs,
     with no signal for when an entry can go.
   - Images would need the inverse callback.
   - One-shot origins (an HTTP request) and static ones (a peer) would pay
     the same way.
5. **Three storage classes, declared by the processor, with core owning
   the storage.** Chosen.

## Decision

### 1. Core is the registry of I/O processors, with stable indices

- The processors a system supports are the SCXML processor plus every
  processor registered with `ta_system_register_io()`. Core is the only
  source of that set.
- Every processor has a **registry index** (`uint32_t`):
  - index 0 is always the SCXML processor;
  - registered processors get 1, 2, 3 and so on, in registration order.

  An index never changes while the system lives. Registering a type again
  replaces its adapter at the same index. Nothing is ever unregistered.
- Registration returns the index, so it takes a params struct (core allows
  at most three parameters):

  ```c
  typedef struct {
      ta_atom type;
      ta_io io;
      TA_OUT TA_OPTIONAL uint32_t *index;
  } ta_io_registration_params;
  TA_API ta_result ta_system_register_io(ta_system *system,
                                         TA_IN const ta_io_registration_params *params);
  /* The registry index of a processor type; TA_ERR_NOT_FOUND when none. */
  TA_API ta_result ta_system_find_io(const ta_system *system, ta_atom type, TA_OUT uint32_t *index);
  ```

  `ta_system_register_io()` changes in place (root `AGENTS.md`).
- Invokers keep their registry (`ta_system_register_invoker()`, ADR 0016).
  Their entries also get stable indices in registration order. Hosts never
  need these indices; core uses them in invoke origins (point 5).

### 2. An I/O origin has one of three storage classes

The processor chooses the class when it builds the origin, because only it
knows which of the lifetimes in the table above applies:

1. **Computed.** The origin holds a `uint64_t` token. The processor renders
   the URI from the token with a pure callback (point 4). Nothing is
   stored, and nothing needs cleaning up. For peer links, and for
   connections whose URI follows a formula.
2. **Scoped.** The processor opens a **scope** that stores the URI once,
   and closes it when the connection or session ends. Many events refer to
   the same scope. An event that outlives its scope renders an empty
   origin, so a late reply fails with `error.communication`, which is the
   correct outcome.
3. **Per message.** Core copies the URI into a reference-counted **origin
   entry**. The entry lives as long as any copy of the event that holds it,
   and is freed when the last copy is released, with no host call.

`ta_origin_kind` replaces `TA_ORIGIN_IO` with three kinds:
`TA_ORIGIN_IO_COMPUTED`, `TA_ORIGIN_IO_SCOPED` and `TA_ORIGIN_IO_MESSAGE`.
The kind encodes the class, so the union stays 16 bytes:

```c
typedef struct ta_io_origin_entry ta_io_origin_entry;        /* private */
typedef struct { uint32_t index; uint32_t generation; } ta_io_scope;
typedef struct {
    uint32_t processor;                                     /* registry index, not 0 */
    union {
        TA_WHEN(TA_ORIGIN_IO_COMPUTED) uint64_t token;
        TA_WHEN(TA_ORIGIN_IO_SCOPED) ta_io_scope scope;
        TA_WHEN(TA_ORIGIN_IO_MESSAGE) ta_io_origin_entry *entry;
    } as;
} ta_io_origin;
typedef struct { ta_invoke_id invoke; uint32_t invoker; } ta_invoke_origin;
typedef struct {
    ta_origin_kind kind;
    union {
        TA_WHEN(TA_ORIGIN_ACTOR) ta_actor_ref actor;
        TA_WHEN(TA_ORIGIN_INVOKE) ta_invoke_origin invoke;
        TA_WHEN(TA_ORIGIN_IO_COMPUTED, TA_ORIGIN_IO_SCOPED, TA_ORIGIN_IO_MESSAGE) ta_io_origin io;
    } as;
} ta_origin TA_TAGGED(kind, as);
```

**Why a scope is an index plus a generation, while a per-message entry is
a pointer:**
- Events hold no reference to a scope. The scope closes when its
  connection ends, whatever events still point at it. So a scope is looked
  up in the system's scope table at render time, and the generation turns
  a stale handle into an empty origin, never into another URI.
- Events do hold references to a per-message entry. `ta_event_retain()`
  and `ta_event_release()` take no system argument, so the entry must be
  reachable from the origin alone. It carries a pointer back to its table,
  as `ta_address` records do (ADR 0065). A held reference keeps it alive,
  so it cannot go stale.

**API:**

```c
typedef enum { TA_IO_ORIGIN_COMPUTED, TA_IO_ORIGIN_SCOPED, TA_IO_ORIGIN_MESSAGE } ta_io_origin_class TA_SYMBOLS("TA_IO_ORIGIN_");
typedef struct {
    uint32_t processor;
    ta_io_origin_class origin_class;
    uint64_t token;                /* COMPUTED */
    ta_io_scope scope;             /* SCOPED */
    ta_str uri;                    /* MESSAGE: copied */
    TA_OUT TA_RELEASE(ta_origin_release) ta_origin *out;
} ta_io_origin_make_params;
TA_API ta_result ta_io_origin_make(ta_system *system, TA_IN const ta_io_origin_make_params *params);
/* Releases a made origin that was never put into a delivered event; accepts any origin. */
TA_API void ta_origin_release(TA_INOUT ta_origin *origin);

typedef struct { uint32_t processor; ta_str uri; TA_OUT ta_io_scope *out; } ta_io_scope_open_params;
TA_API ta_result ta_io_scope_open(ta_system *system, TA_IN const ta_io_scope_open_params *params);
TA_API void ta_io_scope_close(ta_system *system, ta_io_scope scope);
```

- `ta_io_origin_make()` checks the processor index, and that a scoped
  origin names an open scope of that processor. A per-message origin comes
  back holding one reference. The host puts the origin in the event it
  delivers, then releases the event as usual (`ta_event_release()` also
  releases the origin). `ta_actor_deliver()` retains its own copy, as it
  does for `data`.
- `ta_event_retain()` and `ta_event_release()` retain and release a
  per-message origin together with the event's data and named origin
  (ADRs 0050 and 0065). Queued, current, held and paused copies each hold
  a reference.
- `ta_io_scope_close()` increments the scope's generation and frees its
  record. It is idempotent for a stale handle.
- **Reply-once is the processor's concern, not core's.** An HTTP request
  or a broker message is answered once. The processor refuses a second
  reply with `error.communication`. Core only ends the storage when the
  last copy of the event is released.

### 3. Storage: fixed-size records, pages and a free list

Scopes and per-message entries live in fixed-size records:
- the records are in paged tables whose pages never move (as the actor
  table in ADR 0063), with a free list;
- each record holds a reference count (per-message entries only), a
  generation, a pointer back to its table, the processor index, the URI's
  length, and a **48-byte inline buffer** for the URI;
- a longer URI gets one separate allocation, freed with the record;
- a record is 80 bytes (measured with `sizeof` on a 64-bit host).

After warm-up, a per-message origin whose URI fits in 48 bytes allocates
nothing: its record comes from the free list. Opening a scope allocates
nothing either, unless its URI is longer. Pages grow only with the largest
number of live entries and open scopes the system has had.

### 4. Processors render only what core does not store

`ta_io_vtable` gains one optional callback:

```c
typedef enum { TA_IO_TEXT_ORIGIN = 0, TA_IO_TEXT_LOCATION = 1 } ta_io_text_kind TA_SYMBOLS("TA_IO_TEXT_");
typedef struct {
    ta_io_text_kind kind;
    uint64_t session_id;                  /* the session the text is for (ADR 0066) */
    TA_OPTIONAL ta_address *address;      /* its named address, or null */
    uint64_t token;                       /* TA_IO_TEXT_ORIGIN: a computed origin's token */
    ta_allocator arena;                   /* where the text must be allocated */
    TA_OUT ta_str *out;
} ta_io_text_params;
typedef struct ta_io_vtable {
    ta_op_status (*send)(void *self, TA_IN const ta_send *send,
                         TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault);
    TA_OPTIONAL ta_result (*text)(void *self, TA_IN const ta_io_text_params *params);
} ta_io_vtable TA_VTABLE;
```

- **`TA_IO_TEXT_ORIGIN`** renders a *computed* origin's token. Scoped and
  per-message origins are rendered by core, from the stored text, with no
  call.
- **`TA_IO_TEXT_LOCATION`** renders this session's address through the
  processor (`_ioprocessors[type].location`). A location is derived, not
  stored, so it has no lifetime to manage.
- **The contract, stated in the header:**
  - **Pure.** The text is a function of the processor's registration, the
    session and the token, and nothing else, for the life of the system.
    This keeps replay deterministic.
  - **No calls into core.** The callback runs while a data model is
    evaluating an expression inside a macrostep (ADR 0017).
  - **Only the arena.** All memory for the text comes from `arena`. The
    callback keeps no pointer into it.
  - **Results.**
    - `TA_OK` with the text.
    - `TA_ERR_NOT_FOUND`: no such text. The field is then empty.
    - `TA_ERR_NOMEM`: the arena could not grow.
  - A processor with no `text` callback cannot use computed origins
    (`ta_io_origin_make()` refuses them) and has no location.
- A native adapter renders from its own C state, so a host whose language
  cannot run inside core (napi ADR 0001, rule 3) still answers.
- **A Basic HTTP processor must render a location** (C.2.3). Core cannot
  check this. The test host's scenarios do.

### 5. Invoke origins (G3)

- An invoke origin holds the invoke ID and the registry index of the
  invocation's invoker. It is 16 bytes. Core fills it, never the host:
  - `ta_actor_report_invoke()` sets it for `EVENT` and `DONE` reports,
    replacing whatever origin the host passed;
  - `ta_actor_deliver()` sets it for an external event whose `invoke_id`
    names an active invocation of the target and whose origin is
    `TA_ORIGIN_NONE`. This covers hosts that post invoker events through
    their own mailbox, as napi does.
- The invoker index is recorded when the event is accepted, so the origin
  stays meaningful after the invocation ends, which `done.invoke` needs.
- `_event.origin` is `#_<invokeid>`. `_event.origintype` is the processor
  that reaches that target:
  - the SCXML processor when the invoker is the SCXML invoker (the target is
    a child session);
  - otherwise the invoker's own type, which is the type SCXML 6.4 uses for
    ad hoc sends to a non-SCXML service.

### 6. System-variable text is a view, rendered into the run's arena

`ta_dm_host_text()` stops writing into a caller's buffer. It returns a view
for every text kind:

```c
typedef struct {
    ta_dm_text_kind kind;
    uint32_t io_index;                    /* TA_DM_TEXT_IO_TYPE and TA_DM_TEXT_IO_LOCATION */
    TA_OUT TA_LIFETIME(run) ta_str *out;
} ta_dm_text_params;
TA_API ta_result ta_dm_host_text(const ta_dm_host *host, TA_IN const ta_dm_text_params *params);
```

- **Origins.** `TA_DM_TEXT_EVENT_ORIGIN` and `TA_DM_TEXT_EVENT_ORIGINTYPE`
  render every origin kind:
  - actor and invoke origins are rendered by core into the arena;
  - scoped and per-message origins return a view of the stored URI, with no
    copy. The event holds the per-message entry for as long as the actor
    holds the event, and a host cannot close a scope during a run, because
    it cannot call core then;
  - computed origins are rendered through the processor's `text` callback;
  - `origintype` of an I/O origin is the type of its registry entry;
  - a stale scope gives `TA_ERR_NOT_FOUND`, so `_event.origin` is empty.
- **Processors.** `TA_DM_TEXT_IO_TYPE` and `TA_DM_TEXT_IO_LOCATION` take
  `io_index`. They answer `TA_ERR_NOT_FOUND` past the last index, which is
  how a data model enumerates `_ioprocessors` (G2):
  - it iterates from 0 until the type answers `TA_ERR_NOT_FOUND`, reading
    each type and, when one exists, its location. Every processor is
    listed, which meets 5.10's MUST;
  - index 0's location is rendered by core: the session's
    `#_actor_<family>/<name>` address when it has one, otherwise
    `#_scxml_<session>`.

  `TA_DM_TEXT_SCXML_IO_TYPE` and `TA_DM_TEXT_SCXML_IO_LOCATION` are removed.
  Whether a data model also offers a short key such as `scxml` is its own
  choice.
- **The arena is the running interpreter's scratch arena:**
  - `system->scratch` for an ordinary run;
  - the exit frame's own arena while a cancelled invoked child is being
    exited (ADR 0060).

  No new arena is needed, because `system->scratch` already has the right
  lifetime and use:
  - it is reset when each interpreter run starts (`ta_interpreter_init`);
  - a run ends when its macrostep ends or pauses;
  - core already allocates temporaries there while actions run, for example
    the evaluated `<param>`s of a send.
- **Lifetime of a view:** until the interpreter run that produced it ends,
  which is the end of the macrostep, or earlier if it pauses. Data models
  copy the text into their own values before returning to core, as they do
  today.
- **Allocation failure.** A failed allocation fails the data-model
  operation that asked for the text. It becomes `error.execution`, as a
  failed scratch allocation for `<param>` values already does. Rendering
  has no side effects, so ADR 0047 holds.

### 7. Session images

- **At capture,** every I/O origin, of every class, is rendered to the
  processor's type text plus the URI text:
  - a computed origin through the processor's `text` callback, into the
    image's own arena (capture runs outside a macrostep);
  - a scoped or per-message origin from its stored text;
  - a stale scope is stored as no origin.

  An invoke origin is stored as its invoke ID plus the invoker's type text.
- **At load,** every I/O origin becomes a **per-message** origin holding
  that text. The type text is found in the registry with
  `ta_system_find_io`. An unknown type becomes `TA_ORIGIN_NONE`; the event
  is still delivered, and only the reply is lost.
  - No inverse callback is needed: the processor routes from text anyway.
  - A scope cannot be revived, and does not need to be: its text travels
    in the per-message entry.
- This is an image format change. Old images exist only in tests, so no
  migration is needed (root `AGENTS.md`). The same rendering applies to any
  log that stores events, such as the replay proposal's.

### 8. Callers that move with the change

- **Core:**
  - `include/tinyactors.h` and the regenerated `tinyactors.api.json`:
    - `ta_origin` and the new kinds;
    - `ta_io_origin_make()`, `ta_io_scope_open()` / `_close()`;
    - `ta_io_vtable.text`;
    - the registration params and `ta_system_find_io()`;
    - the text API;
    - a new `TA_LIFETIME(run)` in the API vocabulary, and `TA_WHEN`
      accepting several tag constants for the shared I/O variant
      (`docs/internal/api-ir.md` and the extractor in `mise/lib/api`);
  - `src/system.c`: the registry, indices, the scope and entry tables;
  - `src/event.c`: retain and release of per-message entries;
  - `src/datamodel.c`: the text API returns views, all origin kinds,
    enumeration;
  - `src/invoke.c` and `src/actor.c`: filling invoke origins, checking I/O
    origins;
  - `src/session_image.c`: rendering at capture, per-message entries at
    load;
  - core tests, including one where a stale scope renders empty and one
    where per-message entries balance to zero.
- **Test host (`tinyactors-test`):**
  - `src/ecmascript_datamodel.c` and `src/json_datamodel.c`: views instead
    of buffers, `_ioprocessors` from the registry, `_event.origin` for every
    kind;
  - its I/O adapters build origins of each class, and gain `text` where
    they use computed origins or locations;
  - the generated RPC bindings, regenerated;
  - `ruby/tinyactors_test/scenario.rb` (`ta_system_register_io`) and
    `ruby/scenarios/scxml_processor.rb`;
  - new scenarios:
    - a reply through a custom processor, for each storage class;
    - a reply after its scope closed fails with `error.communication`;
    - `_ioprocessors` lists registered processors;
    - a reply to a non-SCXML invocation;
    - an origin survives capture and load as a per-message origin;
    - a Basic HTTP entry has a location.
- **napi addon (`tinyactors-napi`):**
  - `src/napi_datamodel.c`: views instead of buffers, `ioProcessors` lists
    every processor, `event.origin` for every kind;
  - `src/napi_services.c`:
    - `post()` gains `origin: { type, uri }` (per message) or
      `origin: { type, scope }`;
    - a new `openScope(type, uri)` / `closeScope(scope)` for connections;
    - `registerIo(type, processor, { location })` states how locations
      and computed origins are formed, and the native adapter renders them
      without running JavaScript;
  - the envelope releases its event as before, which now also releases a
    per-message origin;
  - regenerated `src/gen/`.

## Consequences

- The standard reply idiom works for every external event: from another
  session (as before), from an I/O processor, and from an invoked service
  of any type. Distribution across systems can be written in plain SCXML,
  with explicit sends to processors.
- `_ioprocessors` meets SCXML 5.10's MUST, and `ta_dm_host_text()` treats
  all origins alike.
- Core owns the registry and the storage of every origin, and ends that
  storage without calling a processor. Processors keep no token tables.
- **Sizes**, measured with `sizeof` on a 64-bit host:

  | | Before | After |
  |---|---|---|
  | `ta_origin` | 24 | 24 |
  | `ta_event` | 64 | 64 |
  | `ta_queued_event` | 72 | 72 |
  | `ta_send` | 96 | 96 |
  | `ta_active_invoke` | 32 | 32 |
  | origin record (scope or per-message entry) | — | 80, with a 48-byte inline URI |

  The I/O and invoke variants are 16 bytes each, the size of the existing
  `ta_actor_ref` variant, so the union does not grow.
- **Memory:**
  - **per actor:** nothing;
  - **per computed origin:** nothing;
  - **per open scope:** one 80-byte record, plus one allocation if its URI
    is longer than 48 bytes;
  - **per live per-message origin:** one 80-byte record, plus a spill
    allocation for a long URI, freed when the last copy of the event is
    released;
  - **steady state:** no allocation per message for URIs of up to 48 bytes.
- **Per read of a system variable:**
  - a view of stored text, with no copy, for scoped and per-message origins;
  - arena bytes plus one `text` call for computed origins and locations.
- The public ABI changes. Every caller is in this repository and moves with
  the change:
  - `ta_origin` (three I/O kinds replace one; the invoke variant gains the
    invoker index);
  - `ta_io_origin_make()`, `ta_origin_release()`, `ta_io_scope_open()` and
    `ta_io_scope_close()`;
  - `ta_io_vtable.text`;
  - `ta_system_register_io()` with params, and `ta_system_find_io()`;
  - `ta_dm_text_params` and the view contract of `ta_dm_host_text()`;
  - two text kinds removed and two added;
  - `ta_event_retain()` and `ta_event_release()` now also manage
    per-message origins.

# ADR 0068: Core owns the I/O processor registry, and every origin can be answered

**Status:** Proposed; refines ADR 0015, ADR 0016, ADR 0040, ADR 0046 and ADR 0065. Revised in place on 2026-09-24 after review (it had not been accepted): origins reference the registry instead of copying from it, and processors render text on demand.
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

### Constraints

- **Size.** `ta_origin` is 24 bytes and `ta_event` 64 bytes (measured with
  `sizeof` on a 64-bit host). Every queued event, held event, envelope and
  log record carries one. The fix should not grow them.
- **Determinism.** A chart must read the same `_event.origin` when its
  history is replayed (see `docs/proposals/replay-persistence.md`).
- **Allocation.** Text for system variables is needed only while a data
  model reads it. It should not outlive the interpreter run that needs it.

### Designs considered

1. **The host supplies `_ioprocessors` itself**, for example through the
   data-model binding. Rejected. The host would keep a second copy of a
   list that core already has (napi ADR 0001, rule 1). Data models are
   shared between hosts, and each would need host-specific plumbing.
2. **The data-model adapter asks the host directly.** Rejected for the same
   reason, and because a data model must not depend on which host it runs
   in.
3. **Core renders location templates** (`ta://tenant42/{family}/{name}`).
   Rejected. A URI's shape and escaping belong to the processor that
   defines the scheme, and a template cannot turn a delivery token back
   into an address.
4. **Each origin copies the processor type and a retained URI value.**
   Rejected. An I/O origin would grow to a type atom plus a `ta_value`,
   making `ta_origin` 32 and `ta_event` 72 bytes, and every event would
   retain a host value.
5. **Core owns the registry, origins reference it, and the processor
   renders text on demand.** Chosen.

## Decision

### 1. Core is the registry of I/O processors, with stable indices

- The processors a system supports are the SCXML processor plus every
  processor registered with `ta_system_register_io()`. Core is the only
  source of that set. Hosts and data models read it from core.
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
  Their entries also get stable indices in registration order, and
  re-registering a type keeps its index. Hosts never need these indices;
  core uses them in invoke origins (point 4).

### 2. Origins reference the registry

`ta_origin` changes in place. Its size stays 24 bytes:

```c
/* The processor's registry index, and the token it assigned to the sender. */
typedef struct { uint32_t processor; uint64_t token; } ta_io_origin;
/* The invocation, and its invoker's registry index. */
typedef struct { ta_invoke_id invoke; uint32_t invoker; } ta_invoke_origin;
typedef struct {
    ta_origin_kind kind;
    union {
        TA_WHEN(TA_ORIGIN_ACTOR) ta_actor_ref actor;
        TA_WHEN(TA_ORIGIN_INVOKE) ta_invoke_origin invoke;
        TA_WHEN(TA_ORIGIN_IO) ta_io_origin io;
    } as;
} ta_origin TA_TAGGED(kind, as);
```

- **I/O origin.** The host fills it when it delivers an event that one of
  its processors received (`ta_actor_deliver()`, or its router's ingress
  path).
  - `processor` must be a registry index other than 0, otherwise delivery is
    `TA_ERR_INVALID`.
  - `token` is chosen by that processor. It is opaque to core. It must
    identify the sender for as long as any event, image or log record may
    hold it (point 3).
  - Nothing is retained or released. The origin is plain data.
- **Invoke origin.** Core fills it, never the host:
  - `ta_actor_report_invoke()` sets it for `EVENT` and `DONE` reports,
    replacing whatever origin the host passed;
  - `ta_actor_deliver()` sets it for an external event whose `invoke_id`
    names an active invocation of the target and whose origin is
    `TA_ORIGIN_NONE`. This covers hosts that post invoker events through
    their own mailbox, as napi does.

  `invoker` is the registry index of the invoker whose type is
  `ta_active_invoke.type`. It is recorded when the event is accepted, so the
  origin stays meaningful after the invocation ends, which `done.invoke`
  needs.
- **Actor origin:** unchanged (ADR 0065).

### 3. Processors render their text on demand

`ta_io_vtable` gains one optional callback:

```c
typedef enum { TA_IO_TEXT_ORIGIN = 0, TA_IO_TEXT_LOCATION = 1 } ta_io_text_kind TA_SYMBOLS("TA_IO_TEXT_");
typedef struct {
    ta_io_text_kind kind;
    /* The session the text is for (ADR 0066), and its named address, or null. */
    uint64_t session_id;
    TA_OPTIONAL ta_address *address;
    /* TA_IO_TEXT_ORIGIN: the token from the origin. Zero otherwise. */
    uint64_t token;
    /* Where the text must be allocated. */
    ta_allocator arena;
    TA_OUT ta_str *out;
} ta_io_text_params;

typedef struct ta_io_vtable {
    ta_op_status (*send)(void *self, TA_IN const ta_send *send,
                         TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault);
    TA_OPTIONAL ta_result (*text)(void *self, TA_IN const ta_io_text_params *params);
    TA_OPTIONAL ta_result (*token)(void *self, TA_IN const ta_io_token_params *params); /* point 6 */
} ta_io_vtable TA_VTABLE;
```

- **`TA_IO_TEXT_ORIGIN`** turns a token into the URI a `<send>` of this
  processor's type accepts as a target.
- **`TA_IO_TEXT_LOCATION`** gives this session's address through the
  processor (`_ioprocessors[type].location`).
- **The contract, stated in the header:**
  - **Pure.** The text is a function of the processor's registration, the
    session (its ID and named address) and the token, and nothing else.
    Two calls with the same inputs return the same text for the life of the
    system. This keeps replay deterministic.
  - **No calls into core.** The callback runs while a data model is
    evaluating an expression, inside a macrostep. Core's reentrancy rules
    apply (ADR 0017).
  - **Only the arena.** All memory for the text comes from `arena`. The
    callback keeps no pointer into it.
  - **Results.**
    - `TA_OK` with the text.
    - `TA_ERR_NOT_FOUND`: there is no such text, for example an unknown
      token, or a processor without a location. The field is then empty.
    - `TA_ERR_NOMEM`: the arena could not grow.
  - A processor with no `text` callback has no location and no origin text.
- **A Basic HTTP processor must render a location** (C.2.3). Core cannot
  check this. The test host's scenarios do.
- This is a call into host code during a macrostep, for reading a system
  variable. It is allowed because the contract makes it a pure lookup. A
  native adapter renders from its own C state, so a host whose language
  cannot run inside core (napi ADR 0001, rule 3) still answers without
  running that language. For example, napi keeps a table from tokens to
  URIs, filled when the host delivers the result.

### 4. All system-variable text is rendered on demand into the run's arena

`ta_dm_host_text()` stops writing into a caller's buffer. It returns a view,
and every text kind is rendered the same way:

```c
typedef struct {
    ta_dm_text_kind kind;
    /* TA_DM_TEXT_IO_TYPE and TA_DM_TEXT_IO_LOCATION: the registry index. */
    uint32_t io_index;
    TA_OUT TA_LIFETIME(run) ta_str *out;
} ta_dm_text_params;
TA_API ta_result ta_dm_host_text(const ta_dm_host *host, TA_IN const ta_dm_text_params *params);
```

- **Text kinds:**
  - `TA_DM_TEXT_EVENT_ORIGIN` and `TA_DM_TEXT_EVENT_ORIGINTYPE` render every
    origin kind. Actor and invoke origins are rendered by core, and I/O
    origins through the processor's `text` callback. The earlier asymmetry,
    where some origins had no text, is gone.
  - For an I/O origin, `origintype` is the type of registry entry
    `processor`.
  - For an invoke origin, `origin` is `#_<invokeid>`. `origintype` is the
    processor that reaches that target: the SCXML processor when the
    invoker is the SCXML invoker (the target is a child session), and
    otherwise the invoker's own type. That is the type SCXML 6.4 uses for
    ad hoc sends to a non-SCXML service. The rule needs only the registry,
    so it works after the invocation has ended.
  - `TA_DM_TEXT_IO_TYPE` and `TA_DM_TEXT_IO_LOCATION` take `io_index`. They
    answer `TA_ERR_NOT_FOUND` past the last index, which is how a data
    model enumerates `_ioprocessors` (G2). Index 0's location is rendered by
    core: the session's `#_actor_<family>/<name>` address when it has one,
    otherwise `#_scxml_<session>`, as today.
  - `TA_DM_TEXT_SCXML_IO_TYPE` and `TA_DM_TEXT_SCXML_IO_LOCATION` are
    removed; index 0 replaces them.
  - Whether a data model also offers a short key such as `scxml` is its own
    choice. SCXML names only the full type URIs.
- **The arena is the running interpreter's scratch arena:**
  - `system->scratch` for an ordinary run;
  - the exit frame's own arena while a cancelled invoked child is being
    exited (ADR 0060).

  No new arena is added, because `system->scratch` already has the right
  lifetime and use:
  - it is reset when each interpreter run starts (`ta_interpreter_init`);
  - a run ends when its macrostep ends or pauses;
  - core already allocates temporaries there while actions run, for example
    the evaluated `<param>`s of a send.
- **Lifetime of a view:** until the interpreter run that produced it ends,
  which is the end of the macrostep, or earlier if the macrostep pauses. A
  data model copies the text into its own values (a JavaScript string, a jq
  value) before returning to core, as it already does.
- **Allocation failure.** A failed allocation, in core's rendering or in a
  processor's, fails the data-model operation that asked for the text. It
  becomes `error.execution`, as a failed scratch allocation for `<param>`
  values already does. Rendering has no side effects, so nothing is left
  half-done (ADR 0047).
- **Cost of the arena.** The arena's blocks are kept across resets, so the
  largest text rendered in one run raises its permanent size once.

### 5. G2: `_ioprocessors` from the registry

A data model builds `_ioprocessors` by iterating `io_index` from 0 until
`TA_DM_TEXT_IO_TYPE` answers `TA_ERR_NOT_FOUND`. For each entry it reads
the type, and the location when one exists. Every processor the system
supports is listed, which meets 5.10's MUST. Nothing is stored per session.

### 6. Session images

Most events in an image carry actor origins, which images already handle.
Delayed sends store the owner's own address. I/O and invoke origins appear
in queued external events, and in the events an actor holds while a
macrostep is paused. A token means something only to the processor that
issued it, and only in this system, so an image never stores one.

- **At capture,** core renders each I/O origin to text, the processor's
  type text plus the origin's URI text, into the image's own arena. Capture
  runs outside a macrostep, so the arena is the image's, not scratch. An
  invoke origin is stored as its invoke ID plus the invoker's type text.
- **At load,** core turns the text back into a reference:
  - the type text is found in the registry (`ta_system_find_io`);
  - the processor's second optional callback, `token`, turns the URI text
    into a token:

    ```c
    typedef struct { uint64_t session_id; ta_str uri; TA_OUT uint64_t *token; } ta_io_token_params;
    ```

    It is the inverse of `text` and may record the URI in the processor's
    own state. Load runs outside a macrostep, so this is an ordinary host
    call.
  - an invoke origin's invoker type is found in the invoker registry.
- **If a type is not registered,** or the processor has no `token`
  callback, the origin becomes `TA_ORIGIN_NONE`. The event is still loaded
  and delivered. Only the reply is lost, which is SCXML's outcome for an
  unreachable origin (`error.communication` if the chart tries).
- This is an image format change. Old images exist only in tests, so no
  migration is needed (root `AGENTS.md`). The same rendering applies to any
  log that stores events, such as the replay proposal's.

### 7. Callers that move with the change

- **Core:**
  - `include/tinyactors.h` and the regenerated `tinyactors.api.json`:
    `ta_origin`, `ta_io_vtable`, the registration params, the text API, and
    a new `TA_LIFETIME(run)` in the API vocabulary
    (`docs/internal/api-ir.md`);
  - `src/system.c`: the registry, indices, `ta_system_find_io`;
  - `src/datamodel.c`: the text API renders into the arena, all origin
    kinds, enumeration;
  - `src/invoke.c` and `src/actor.c`: filling invoke origins, checking I/O
    origins;
  - `src/session_image.c`: rendering and resolving origins;
  - core tests.
- **Test host (`tinyactors-test`):**
  - `src/ecmascript_datamodel.c` and `src/json_datamodel.c`: views instead
    of buffers, `_ioprocessors` from the registry, `_event.origin` for every
    kind;
  - its I/O adapters gain `text` and `token`;
  - the generated RPC bindings, regenerated;
  - `ruby/tinyactors_test/scenario.rb` (`ta_system_register_io`) and
    `ruby/scenarios/scxml_processor.rb`;
  - new scenarios: a reply through a custom processor, `_ioprocessors`
    listing registered processors, a reply to a non-SCXML invocation, an I/O
    origin surviving capture and load, and a Basic HTTP entry that has a
    location.
- **napi addon (`tinyactors-napi`):**
  - `src/napi_datamodel.c`: views instead of buffers, `ioProcessors` lists
    every processor, `event.origin` for every kind;
  - `src/napi_services.c`: the native I/O adapter implements `text` from a
    per-processor table of tokens and URIs, and `token` for loads.
    `registerIo(type, processor, { location })` takes how locations are
    formed; the native side renders them, never JavaScript;
  - `post()` gains `origin: { type, uri }`. The native side registers the
    URI in that processor's table and delivers the token;
  - regenerated `src/gen/`.

## Consequences

- The standard reply idiom works for every external event: from another
  session (as before), from an I/O processor, and from an invoked service
  of any type. Distribution across systems can be written in plain SCXML,
  with explicit sends to processors.
- `_ioprocessors` meets SCXML 5.10's MUST, and `ta_dm_host_text()` treats
  all origins alike.
- The registry stays in core, where registration already was. Hosts keep
  no copy, and events hold references, not copies.
- **Sizes**, measured with `sizeof` on a 64-bit host:

  | | Before | After |
  |---|---|---|
  | `ta_origin` | 24 | 24 |
  | `ta_event` | 64 | 64 |
  | `ta_queued_event` | 72 | 72 |
  | `ta_send` | 96 | 96 |
  | `ta_active_invoke` | 32 | 32 |
  | napi envelope | unchanged | unchanged |

  The I/O and invoke variants are 16 bytes each, the size of the existing
  `ta_actor_ref` variant, so the union does not grow.
- **Per actor:** nothing. **Per registered processor:** nothing beyond its
  existing entry. **Per event:** no retained host value (the rejected
  design 4 would have retained one per I/O-origin event).
- **Per read of a system variable:** arena bytes until the run ends, plus,
  for an I/O origin, one call to the processor's `text`.
- **Tokens are the processor's responsibility.** A processor that issues a
  token must be able to render it for the life of the system, or for as
  long as events, images or logs may hold it. For example, napi's table
  grows with the number of distinct senders. Bounding it (sender URIs that
  are stable per connection, or expiry) is the processor's policy.
- The public ABI changes:
  - `ta_origin`;
  - `ta_io_vtable` (two optional callbacks);
  - `ta_system_register_io()`, plus the new `ta_system_find_io()`;
  - `ta_dm_text_params` and the view contract of `ta_dm_host_text()`;
  - two text kinds removed and two added.

  Every caller is in this repository and moves with the change.

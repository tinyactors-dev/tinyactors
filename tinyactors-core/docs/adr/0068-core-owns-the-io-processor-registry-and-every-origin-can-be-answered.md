# ADR 0068: Core owns the I/O processor registry, and every origin can be answered

**Status:** Proposed; refines ADR 0015, ADR 0016, ADR 0040, ADR 0046 and ADR 0065
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
this session through that processor.

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

### Who should know which processors exist

Core already owns I/O processor registration (`ta_system_register_io()`,
ADR 0046) and invoker registration (ADR 0016), and it already routes each
`<send>` by its type atom. Three designs for `_ioprocessors` were
considered:

1. **The host supplies `_ioprocessors` itself**, for example through the
   data-model binding. Rejected. The host would keep a second copy of a
   list that core already has (napi ADR 0001, rule 1). Data models are
   shared between hosts: the test host has jq and ECMAScript models, and
   napi has a JavaScript model. Each would need host-specific plumbing to
   find the list.
2. **The data-model adapter asks the host through a callback.** Rejected
   for the same reason, and because a data model must not depend on which
   host it runs in.
3. **Each processor answers a `location` callback per session.** Rejected.
   Core would call host code while a macrostep runs, just to read a system
   variable. A host such as napi cannot run JavaScript there (napi ADR 0001,
   rule 3). A callback can also answer differently on each call, which
   breaks replaying a chart from its log.
4. **Core owns the registry, and each processor declares its location as a
   template when it registers.** Chosen. Core renders the template for a
   session from facts it already has, with no host call and no allocation.

## Decision

### 1. Core is the registry of I/O processors

- The set of processors a system supports is the SCXML processor plus
  every processor registered with `ta_system_register_io()`. Core is the
  only source of that set. Hosts and data models read it from core.
- Registration takes a params struct, because it gains a parameter and
  core allows at most three:

  ```c
  typedef struct {
      ta_atom type;
      ta_io io;
      /* Where this session can be reached through this processor, as a
       * template core renders per session. Empty: the processor has no
       * location, and its _ioprocessors entry carries none. Copied. */
      TA_OPTIONAL ta_str location;
  } ta_io_registration_params;
  TA_API ta_result ta_system_register_io(ta_system *system,
                                         TA_IN const ta_io_registration_params *params);
  ```

  The function changes in place, as the root `AGENTS.md` requires.
- A **location template** is text with placeholders that core fills per
  session:
  - `{session}`: the session ID in decimal (ADR 0066);
  - `{family}` and `{name}`: the parts of the session's named address,
    percent-encoded as core's target grammar already does (ADR 0044).

  For a session with no named address, a template that uses `{family}` or
  `{name}` has no location. Other text is copied as it is. Example:
  `ta://tenant42/{family}/{name}`.
- Registering the Basic HTTP type
  (`http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor`) without a location
  is `TA_ERR_INVALID`, because C.2.3 requires one. For other custom
  processors, SCXML leaves the entry's contents to the platform ("the nature
  of the values … depends on the Event I/O Processor in question", 5.10),
  so the location stays optional.
- The SCXML processor's location is the one core already writes: the
  session's `#_actor_<family>/<name>` address when it has one, otherwise
  `#_scxml_<session>`.

### 2. Data models enumerate the registry through the host interface

`_ioprocessors` is a map, so a data model needs to list its entries. The
interface follows `ta_dm_host_state_name()`: iterate indices until
`TA_ERR_NOT_FOUND`.

```c
typedef struct {
    uint32_t index;                              /* 0 is the SCXML processor */
    TA_OUT TA_LIFETIME(system) ta_str *type;     /* the processor's type text */
    TA_OUT bool *has_location;
} ta_dm_io_processor_params;
TA_API ta_result ta_dm_host_io_processor(const ta_dm_host *host,
                                         TA_IN const ta_dm_io_processor_params *params);
```

- Index 0 is always the SCXML processor. Registered processors follow in
  registration order.
- `ta_dm_text_params` gains `uint32_t io_index`. The new text kind
  `TA_DM_TEXT_IO_LOCATION` renders the location of processor `io_index` for
  this session, into the caller's buffer, with the usual list-output rules
  and no allocation. `TA_ERR_NOT_FOUND` means that processor has no location
  for this session.
- `TA_DM_TEXT_SCXML_IO_LOCATION` and `TA_DM_TEXT_SCXML_IO_TYPE` are removed.
  Index 0 replaces them. Aliases such as the short key `scxml` stay a choice
  of each data model: SCXML names only the full type URIs.

### 3. An I/O origin carries the processor type and a URI

`ta_origin` changes in place:

```c
typedef struct { ta_atom type; ta_value uri; } ta_io_origin;
typedef struct { ta_invoke_id invoke; ta_atom type; } ta_invoke_origin;
typedef struct {
    ta_origin_kind kind;
    union {
        TA_WHEN(TA_ORIGIN_ACTOR) ta_actor_ref actor;
        TA_WHEN(TA_ORIGIN_INVOKE) ta_invoke_origin invoke;
        TA_WHEN(TA_ORIGIN_IO) ta_io_origin io;
    } as;
} ta_origin TA_TAGGED(kind, as);
```

- The opaque `uint64_t` is removed. Nothing in this repository sets it.
- **Who fills it:** the host, when it delivers an event that one of its
  processors received (`ta_actor_deliver()` or the router's ingress path).
  `type` must be a registered processor, otherwise delivery is
  `TA_ERR_INVALID`. `uri` is a host value, like `ta_send.target`: a URI
  that the same processor accepts as a `<send>` target. It may be empty
  (null vtable) when the sender cannot be answered.
- **Lifetime:** `uri` is retained and released together with the event, in
  the same way as `data` (ADR 0050) and a named origin (ADR 0065).
  `ta_event_retain()` and `ta_event_release()` handle it.
- **Reading it:**
  - `TA_DM_TEXT_EVENT_ORIGINTYPE` renders the processor's type text.
  - `TA_DM_TEXT_EVENT_ORIGIN` does not render a host value. It answers
    `TA_ERR_UNSUPPORTED` for an I/O origin. A data model reads
    `ta_dm_host_event()->origin.as.io.uri` and exposes it with its own
    conversion (the value came from the host in the receiving actor's
    data-model representation, as event data does). This keeps
    `ta_dm_host_text()` free of calls back into the adapter.

### 4. An invoke origin carries the invocation's type

- `TA_ORIGIN_INVOKE` holds the invoke ID and the invocation's type atom
  (`ta_invoke_origin` above). `TA_DM_TEXT_EVENT_ORIGINTYPE` renders that
  type. `TA_DM_TEXT_EVENT_ORIGIN` still renders `#_<invokeid>`.
- For an SCXML child the type is the SCXML invocation type, so the reply
  idiom keeps reaching the child as it does today.
- **Core fills the origin** of every event that belongs to an active
  invocation of the receiving actor:
  - `ta_actor_report_invoke()` sets it for `EVENT` and `DONE` reports,
    replacing whatever origin the host passed;
  - `ta_actor_deliver()` sets it for an external event whose `invoke_id`
    names an active invocation of the target and whose origin is
    `TA_ORIGIN_NONE`. This covers hosts that post invoker events through
    their own mailbox, as napi does.

  The type is copied from `ta_active_invoke.type` when the event is
  accepted, so it stays correct after the invocation ends. This matters for
  `done.invoke`.

### 5. Session images

Queued events are captured with their origins. For an I/O origin, the
image carries the processor type by its text (ADR 0028: images resolve
atoms by logical identity) and the URI value in the same way as event data.
For an invoke origin, it carries the invoke ID and the type text. Delayed
sends store the sending actor's own address as their origin and are not
affected. This is an image format change; old images do not exist outside
tests, so no migration is needed (root `AGENTS.md`).

### 6. Callers that move with the change

- **Core:**
  - `src/system.c`: registration, the registry and template rendering;
  - `src/datamodel.c`: origins, the new text kind, enumeration;
  - `src/invoke.c`: filling invoke origins;
  - `src/actor.c`: filling in delivery;
  - `src/event.c`: retain and release of the URI;
  - `src/session_image.c`;
  - core tests;
  - `include/tinyactors.h` and the regenerated `tinyactors.api.json`.
- **Test host (`tinyactors-test`):**
  - `src/ecmascript_datamodel.c` and `src/json_datamodel.c`, which build
    `_ioprocessors` and `_event.origin`;
  - the generated RPC bindings (regenerated);
  - `ruby/tinyactors_test/scenario.rb` (`ta_system_register_io`) and
    `ruby/scenarios/scxml_processor.rb`;
  - new scenarios: a reply through a custom processor, `_ioprocessors`
    listing registered processors with rendered locations, and a reply to
    a non-SCXML invocation.
- **napi addon (`tinyactors-napi`):**
  - `src/napi_services.c` (`registerIo` gains a `location` option);
  - `src/napi_datamodel.c`: `ioProcessors` lists every processor, and
    `event.origin` reads an I/O origin's value;
  - `post()` gains an `origin` option (`{ type, uri }`) for events a
    processor received;
  - regenerated `src/gen/`.

## Consequences

- The standard reply idiom works for every external event: from another
  session (as before), from an I/O processor, and from an invoked service
  of any type. Distribution across systems can be written in plain SCXML,
  with explicit sends to processors.
- `_ioprocessors` meets SCXML 5.10's MUST. A chart can read its own address
  for each processor, for example to hand a reply URI to a remote system.
- The registry stays in core, where registration already was. Hosts and
  data models keep no copy of it. Locations are a pure function of the
  template and the session, so a replayed chart reads the same values.
- **Cost per event:** `ta_origin` grows from 24 to 32 bytes, because an I/O
  origin holds a type and a value. `ta_event` grows from 64 to 72 bytes, and
  `ta_queued_event` from 72 to 80 bytes (measured with `sizeof` on a 64-bit
  host). The napi addon's envelope, which embeds a `ta_event`, grows by the
  same 8 bytes.
- **Cost per actor:** none. Nothing is stored per actor. Locations are
  rendered when read.
- **Cost per system:** one copied template string per registered processor.
- The public ABI changes: `ta_origin`, `ta_system_register_io()`,
  `ta_dm_text_params`, and the removal of two text kinds. Every caller is in
  this repository and moves with the change.
- A host that cannot answer a sender still delivers the event with an I/O
  origin whose `uri` is empty. `_event.origintype` then says which
  processor the event came through, and `_event.origin` is empty, as SCXML
  allows.

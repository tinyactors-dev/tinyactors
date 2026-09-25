# ADR 0074: A session image crosses a process through a codec

**Status:** Proposed. It would refine ADR 0038 (images stay in-memory structures; a codec gives them a byte form outside core), and it serves napi ADR 0020 (restarts) and the replay-persistence proposal.
**Date:** 2026-09-25

## Context

A session image (`ta_session_image`, ADR 0038) is an owned, in-memory
structure. `ta_actor_capture()` makes one, and `ta_actor_load()` and
`ta_actor_reconstruct()` rebuild an actor from it. It has no byte form.
ADR 0038 decided that on purpose: "The image is an in-memory
representation of Tinyactors-owned interpreter state, not a wire format",
and it removed serialization from `ta_value` and from the data-model
vtable. Crossing a process was left to "a higher-level durability
mechanism".

That mechanism is now needed:

- **Restarts** (napi ADR 0020, section 6): drain, capture each actor,
  store its image, exit, restart, and let a loader bring actors back on
  demand. Steps 4 and 6 need an image as bytes.
- **Replay-based persistence** (`docs/proposals/replay-persistence.md`):
  a snapshot is an image, and every logged event's data needs a value
  codec.

The user decided: "For the serialization, define a codec interface
again, similar to how we load chart definitions. The core should not
prescribe the serialization format."

The precedent is ADR 0053. A chart representation is a `ta_chart_codec`:
core defines the adapter, the in-memory form (`ta_scxml_spec`) and the
error vocabulary, and links no parser. SCXML XML is one codec
(`tinyactors-scxml`), and nothing in core decodes or encodes.

What an image holds today (`src/session_image.c`):

- **Identity:** the session ID (ADR 0066), the definition identity
  (family and exact revision, ADR 0028), the actor's address, and local
  freshness fields (`source_actor`, `source_epoch`) that name the runtime
  actor it came from.
- **Configuration and history,** as the definition-sized session storage
  the actor holds.
- **Execution state:** bootstrapped, in a macrostep, finished and its
  final state (ADR 0056), the step counters and the lifecycle counter
  (ADRs 0067 and 0073), the event slots with the macrostep's trace parent
  and any kept event (ADR 0069), the next send ID, the internal queue,
  and a paused or stopped microstep's continuation (ADRs 0061 and 0070).
- **Delayed sends** (ADR 0054), each with its target, data, content ID,
  due time and trace parent.
- **Invocations,** as logical records; a service handle is dropped, and
  restoring raises `error.communication.invoke.<id>.unavailable` with a
  RAISED record.
- **A parent link** for an invoked session, and a pending completion.

Three kinds of content in that list cannot be written out as they are:

1. **Runtime identifiers:** actor IDs (`source_actor`, the parent link),
   interned atoms and event-name IDs, interned address handles, and
   scoped I/O origins (ADR 0068). They are only meaningful inside one
   system.
2. **Values** (`ta_value`, `{ self, vtable }`): event data, delayed-send
   targets and data, completion data, `<foreach>` snapshots. They are the
   data model's or the host's, and core cannot see inside them.
3. **The interpreter's own layout:** a continuation holds executor
   frames, bitsets and a `<foreach>` snapshot in a form that changes with
   the interpreter.

The data-model root is not in an image at all (ADR 0038): the host
supplies it as a binding.

## Decision

### 1. Core defines an image view; codecs turn it into bytes and back

As `ta_scxml_spec` is the form a chart codec reads and writes, **a
`ta_session_image_view` is the form an image codec reads and writes.** It
is a public, read-only, flat structure: arrays of plain records, no
nesting that needs recursion, and **no runtime identifiers**. Anything
interned appears as text.

```c
typedef struct {
    uint32_t version;                 /* the view's layout: core's, see section 4 */
    uint64_t session_id;
    ta_definition_identity definition; /* family and exact revision */
    ta_actor_address address;         /* family and name, as text */
    bool bootstrapped;
    bool finished;
    ta_state_id final_state;          /* TA_STATE_NONE unless finished */
    uint64_t macrostep_counter;
    uint32_t lifecycle_counter;
    uint64_t next_send_id;
    /* The active atomic states, and each history state's record,
     * by definition-local ID (valid for the exact revision only). */
    TA_COUNTED_BY(state_count) const ta_state_id *states;
    uint32_t state_count;
    TA_COUNTED_BY(history_count) const ta_image_history *history;
    uint32_t history_count;
    /* The event an invoked session keeps between macrosteps (ADR 0069). */
    TA_OPTIONAL const ta_image_event *kept_event;
    TA_COUNTED_BY(delayed_send_count) const ta_image_delayed_send *delayed_sends;
    uint32_t delayed_send_count;
    TA_COUNTED_BY(invocation_count) const ta_image_invocation *invocations;
    uint32_t invocation_count;
} ta_session_image_view;
```

`ta_image_event` holds an event's name as text, its type, its send ID
text, its origin as text (URI and type), its trace parent, and its data
as a value. `ta_image_delayed_send` holds the send ID text, the event,
the target (literal text, or a value), the content ID, the due time and
the trace parent. `ta_image_invocation` holds the invoke ID text and the
state that owns it. `ta_image_history` holds a history state and the
states it recorded.

Two functions in core connect a view to an image, and a codec sits
between them and the bytes:

- **`ta_session_image_view(image, arena, &view)`** renders an image as a
  view. The view's text and arrays live in `arena`. Values in it are
  retained references.
- **`ta_session_image_from_view(system, view, &image)`** builds an image
  from a view, re-interning text against `system` and validating the view
  against the definition (section 4).

The codec vtable follows `ta_chart_codec`:

```c
typedef struct ta_image_codec_vtable {
    ta_image_codec_info info;   /* name (a media type or stable ID), flags */
    TA_OPTIONAL ta_result (*decode)(void *self, TA_IN const ta_image_decode_params *params);
    TA_OPTIONAL ta_result (*encode)(void *self, TA_IN const ta_image_encode_params *params);
} ta_image_codec_vtable TA_VTABLE;
typedef struct { void *self; const ta_image_codec_vtable *v; } ta_image_codec TA_ADAPTER;
```

- **Encode** takes a view, a value codec (section 2) and an arena, and
  writes bytes into the arena.
- **Decode** takes bytes, a value codec and an arena, and writes a view
  into the arena, or fails with a `ta_image_error` naming what is wrong
  and where (section 4).
- **Core calls a codec only from `ta_session_image_encode()` and
  `ta_session_image_decode()`,** which wrap view, codec and from-view.
  An embedder may also call the view functions and a codec directly.
  Core links no codec.

### 2. Values go through a value codec the host provides

**A value codec is a separate adapter, passed with each encode and
decode.** It is not on the data-model vtable and not on `ta_value`.

```c
typedef struct ta_value_codec_vtable {
    ta_result (*encode)(void *self, TA_IN const ta_value *value,
                        TA_INOUT ta_arena *arena, TA_OUT ta_bytes *out);
    ta_result (*decode)(void *self, ta_bytes bytes,
                        TA_OUT TA_RELEASE(ta_value_release) ta_value *out);
} ta_value_codec_vtable TA_VTABLE;
typedef struct { void *self; const ta_value_codec_vtable *v; } ta_value_codec TA_ADAPTER;
```

- **Why not the data model.** ADR 0038 removed serialization from the
  data-model vtable and from `ta_value`, so that a data model never has
  to promise a byte form it may not have. Values in an image are also
  not only the data model's: a host posts event data it made itself.
  The host is the one party that knows every kind of value in its
  process.
- **An image codec treats a value as opaque bytes,** length-prefixed in
  its own format. The two formats are independent: a host can pair the
  reference image codec with any value codec.
- **Replay persistence uses the same adapter** for the data of every
  logged event, so a snapshot and its log agree on how values look.
- **napi:** a JavaScript value codec, with a default the host can
  replace. The candidates are JSON (portable, loses `undefined`, `Date`
  and cycles), `v8.serialize` on Node (structured clone, fast, Node-only
  bytes) and Bun's equivalent. Which is the default is napi's decision,
  in its own ADR.
- **jq and the test host's JSON data model:** values are JSON already,
  so the codec is their JSON text.
- **The data-model root** is not a value in the image (ADR 0038). The
  host stores it beside the image, with the same value codec. To keep
  one blob per actor, the encode and decode params take an **optional
  root value**: when given, the codec writes it after the view and
  decode returns it, so the host can pass it straight to
  `ta_actor_load()` as the binding. Core never holds it.

### 3. In this version, only stable images have a byte form

**Encode refuses an image captured inside a macrostep** (a paused
continuation, a stopped run under ADR 0070, a non-empty internal queue,
pending invocations or a pending completion) with
`TA_IMAGE_ERROR_UNREPRESENTABLE`.

- **Every place that needs bytes has a stable image.** napi's `unload()`
  refuses (`busy`) inside a macrostep, so ADR 0020's restart sequence
  only ever captures stable actors. ADR 0038 already expects durability
  to checkpoint stable boundaries.
- **A continuation is the interpreter's layout.** Putting it in a public
  view would freeze executor frames and bitsets into a format others
  depend on. Leaving it out keeps the interpreter free to change.
- **Serializing continuations later** is possible as its own decision:
  a core-owned, versioned opaque section inside the view. It is not
  needed by anything planned.

**Encode also refuses an invoked session** (one with a parent link). Its
parent is a runtime actor ID, and invoked sessions are not named, so
ADR 0020 never unloads them. The parent's image keeps its invocation as
a logical record, and restoring the parent raises
`error.communication.invoke.<id>.unavailable` as today.

### 4. What core guarantees, and what the codec owns

- **Two versions, two owners.**
  - The **view version** is core's. It changes when the view's fields
    change. A codec writes it into its output and hands it back on
    decode. `ta_session_image_from_view()` accepts the versions core
    still reads and refuses the rest with `TA_ERR_UNSUPPORTED`.
  - The **format version** is the codec's own, for its byte layout. Core
    never sees it.
- **Definitions match exactly.** A view names its definition's family
  and exact revision, and `from_view` refuses any other revision with
  `TA_ERR_DEFINITION_MISMATCH`, as `ta_actor_reconstruct()` does today
  (ADR 0038). Moving an actor to a new revision is replacement, not
  decoding. A host reads the identity from the decoded view to pick the
  definition first.
- **Validation happens twice.**
  - **The codec** validates the bytes: syntax, bounds on sizes and
    counts it enforces itself, and truncation. A failure is a
    `ta_image_error` with a code and a byte offset, as `ta_chart_error`
    has a position.
  - **Core** validates the view against the definition: state IDs in
    range, the states forming a legal configuration (one active child
    per active compound state, every region of an active parallel state,
    ADR 0010), history records naming history states and their own
    descendants, a final state only when finished, and consistent
    counts. A failure names the field, as ADR 0058 names a spec path
    (`delayed_sends[3].due`).
  - **Bytes from outside are untrusted.** A view that passes both is a
    state the chart could have reached structurally. Core does not claim
    it is one the chart did reach: a host that needs authenticity signs
    or stores the bytes where they cannot be changed.
- **Determinism is split.** Core renders a view deterministically:
  delayed sends in due order, then by send ID; history in state order;
  the configuration in state ID order. A codec that is a function of the
  view and the value codec's bytes is deterministic. Whether a value
  encodes the same way twice (key order, for example) is the value
  codec's property, and core cannot promise it.
- **Allocation.** Rendering a view and encoding use the arena passed in,
  which draws from the host's allocator. Decoding writes the view into
  the arena, and `from_view` allocates the image as `ta_actor_capture()`
  does today. `TA_ERR_NOMEM` leaves nothing behind.

### 5. The reference codec is CBOR, in a sibling package

**`tinyactors-image`** ships one image codec, as `tinyactors-scxml` ships
the SCXML chart codec. Core links none, and any host may use another.

The format is **CBOR (RFC 8949) with its core deterministic encoding
(section 4.2.1)**:

- a standard, self-describing binary format, readable with existing
  tools and CBOR's diagnostic notation, which serves debugging as the
  SCXML codec does for charts;
- a deterministic encoding defined by the standard, so determinism
  (section 4) needs no rules of our own;
- compact: small integers and short text cost one or two bytes of
  overhead, and a value codec's bytes go in as a byte string;
- a small encoder and decoder in freestanding C (no stdlib, no
  recursion: the view is flat, so the codec never nests deeper than a
  fixed number of levels).

The image is a CBOR map with small integer keys, tagged with the view
version and the codec's format version. **A rough size** for a small
chart (three active states, no history, one pending delayed send with a
short event name and small data) is 100 to 150 bytes before the
data-model root. The definition's revision fingerprint is the largest
fixed part.

### 6. Size: an arena, not size-then-write

A codec writes into an arena, as chart codecs do. napi ADR 0018's
size-then-write contract suits the OTLP encoder, where sizes accumulate
as spans finish. Here, knowing the size first would mean running the
value codec twice, once to measure and once to write, and values are
most of an image's bytes. Images are small and written once per unload,
so one copy from the arena into the host's memory is negligible.

## Options considered

| Option | Pros | Cons |
|---|---|---|
| **A view plus a codec adapter** (chosen) | the ADR 0053 pattern; core prescribes no format; codecs never see interpreter internals; one validation path for every codec | a public view to keep stable, and a second copy of the image in the view during encode |
| Core writes one fixed binary format | no codec to write; smallest possible code | core prescribes a format, which the user ruled out; every host is tied to it |
| A codec reads the in-memory image through accessors | no view struct | many accessors to keep stable, and each codec re-implements the walk and the validation |
| Values through the data-model vtable (`value_to_bytes`) | one adapter for charts and values | reverses ADR 0038; host-made event data has no data model to ask; forces every data model to have a byte form |
| Serialize continuations now | any image can cross, including one captured mid-macrostep | freezes interpreter internals into a public format, for a case nothing needs |
| A custom compact binary reference format | slightly smaller than CBOR | nobody else can read it, and determinism rules are ours to write |
| JSON as the reference format | readable as text | larger; binary values need base64; no standard deterministic form |

## Consequences

- ADR 0038 stands: the in-memory image and the data model gain no
  serialization. The byte form lives in codecs outside core.
- Core's API grows by the view, its record types, the image and value
  codec adapters, `ta_session_image_view()`,
  `ta_session_image_from_view()`, `ta_session_image_encode()` and
  `ta_session_image_decode()`.
- A new package, `tinyactors-image`, holds the CBOR codec and its tests.
- napi gains `SessionImage.encode()`, `System.decodeImage(bytes)` and a
  value codec option, in a napi ADR of its own.
- ADR 0020's restart sequence becomes possible across processes.
- Replay persistence gets its snapshot format and its value codec.

## Testing (when implemented)

- **Round trips:** for every chart in the test host's catalogue, capture
  a stable actor, encode, decode, load into a fresh system, and compare
  inspection and the next macrostep's trace with the original's.
- **Refusals:** images mid-macrostep and invoked sessions are refused
  with `TA_IMAGE_ERROR_UNREPRESENTABLE`; a view for another revision with
  `TA_ERR_DEFINITION_MISMATCH`.
- **Untrusted bytes:** truncated, oversized and bit-flipped inputs fail
  in the codec with an offset or in core with a field path, never
  crash, never leak (under AddressSanitizer), and leave no image.
- **Determinism:** the same image encodes to the same bytes twice, and
  across two processes.
- **Allocation failure:** each allocation of encode, decode and
  `from_view` fails in turn; each fails cleanly.
- **Restart:** napi's restart sequence from ADR 0020, across two
  processes, with pending delayed sends that fire after the restart in
  their original trace.

## Questions for the user

1. **Stable images only** (section 3): is refusing mid-macrostep images
   and invoked sessions acceptable for this version?
2. **The optional root value** (section 2): should an encoded image carry
   the data-model root when the host asks, so one blob restores one
   actor, or should the root always be stored separately?
3. **CBOR** as the reference format (section 5), in a new
   `tinyactors-image` package?
4. **An arena rather than size-then-write** (section 6), given that
   measuring would run the value codec twice?
5. **napi's default value codec:** JSON (portable across runtimes and
   languages) or `v8.serialize` (richer and faster, but Node-specific
   bytes)? This belongs to the napi ADR that follows, but it decides
   whether images move between Node and Bun.

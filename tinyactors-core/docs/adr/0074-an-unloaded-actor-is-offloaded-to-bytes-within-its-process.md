# ADR 0074: An unloaded actor is offloaded to bytes within its process

**Status:** Proposed. It would refine ADR 0038 (an image gains a byte form, valid only in the process that wrote it) and ADR 0051 (`ta_actor_load` also takes that byte form), and it serves napi ADR 0020 (in-process loads). Revised in place while Proposed, 2026-09-25: the first version made images cross processes through a codec; the user narrowed the purpose to offloading from memory within one process, and left durability to other mechanisms (see "Durability is not this ADR's job").
**Date:** 2026-09-25

## Context

A session image (`ta_session_image`, ADR 0038) is an owned, in-memory
structure. `ta_actor_capture()` makes one, and `ta_actor_load()` and
`ta_actor_reconstruct()` rebuild an actor from it. ADR 0038 decided it is
"an in-memory representation of Tinyactors-owned interpreter state, not a
wire format", and left crossing a process to "a higher-level durability
mechanism".

The first version of this ADR gave images a byte form for **restarts**:
napi ADR 0020's sequence captured every actor at shutdown, stored the
bytes, and loaded them in the next process. That needed a public image
view, a codec interface, a stable and versioned format, validation of
untrusted bytes, text in place of every runtime identifier, and a CBOR
reference codec.

The user questioned the purpose: "session image serialization was
conceived for graceful restarts and can be process specific - but at 4
million actors I'm not sure how that'd even work. Maybe their only purpose
is offloading from RAM within the lifetime of one process, and anything
beyond that needs to use external mechanisms like persisting the datamodel
and starting a new chart instance either via replay or by providing an
input, kind of how xstate does it."

That holds up:

- **Writing every actor at exit does not scale, and is not durability.**
  At 4 million actors and about 500 bytes each (below), a shutdown writes
  about 2 GB before the process may exit. A crash writes nothing. Systems
  that must not lose state persist as they go, to an event log or a state
  store, and a restart reads what is already there. That is what the
  replay-persistence proposal and application-owned storage do.
- **Within one process, most of the first design falls away.** No other
  build reads the bytes, so there is no format to keep stable, no version
  to negotiate, no untrusted input, and no need to replace runtime
  identifiers with text: interned atoms, event-name IDs and address
  handles stay valid for the life of the system.

### What unloading costs today

Measured on this commit's code with a throwaway script (napi, Node
22.23.2, 50,000 and 100,000 named actors of a one-state chart, after
forced GC; `system.stats()` for native memory and `process.memoryUsage()`
for the JavaScript heap). Bytes per actor:

| State | Native | JavaScript heap |
|---|---|---|
| Resident | 432 (core 338, directory 45, values 32, scheduling 17) | 154 for `{ count }`; 529 for a small entity (customer, email, a two-line cart, flags) |
| Unloaded, holding its `SessionImage` (today) | 870 (the core image is about 668) | about 430 more than resident: the `SessionImage` object, and the root it keeps alive |
| Unloaded, image destroyed, only the address kept | 208 (core 114, directory 45, values 32, scheduling 17) | none |

**Today's unload frees no memory: it roughly doubles it.** The in-memory
image is larger than the resident actor, and the JavaScript side keeps a
wrapper plus the data-model root. Unloading is only worth having if the
image and the data become compact bytes.

The data serializes small. `v8.serialize` of the root: 72 bytes for
`{ count }` and 218 for the small entity (JSON: 93 and 228), in 1.4 to
2.2 µs, and back in 0.8 to 1.7 µs.

## Decision

### 1. Purpose: offloading within one process, nothing more

**An offloaded image is a byte string that only the system that wrote it
can load.** It lets a host take idle actors out of memory and bring them
back when mail or a timer arrives (napi ADR 0020), during the life of one
process. It is not a persistence format, and nothing may depend on its
layout across builds, processes or systems.

- The bytes carry the writing system's identity and an epoch.
  `ta_actor_load()` refuses bytes from another system, or from before
  `ta_system` was destroyed, with `TA_ERR_STALE`.
- Anything that must survive the process uses the mechanisms in
  "Durability is not this ADR's job" below.

### 2. Core owns the encoding, and it is opaque

**Core encodes and decodes images itself, in a compact, private layout
that may change in any build.** There is no public view and no image
codec interface.

```c
typedef struct {
    ta_actor_id actor;
    ta_value_codec values;            /* section 3 */
    TA_OPTIONAL const ta_value *root; /* the data-model root to include */
    ta_arena *arena;                  /* where the bytes are written */
    TA_OUT ta_bytes *out;
} ta_actor_offload_params;
TA_API ta_result ta_actor_offload(ta_system *system, TA_IN const ta_actor_offload_params *params);
```

`ta_actor_offload` captures and unloads in one step: it writes the bytes
and releases the actor, as `ta_actor_unload` does after a capture. No
in-memory `ta_session_image` is created on the way. `ta_actor_load` takes
either an in-memory image, as today, or offloaded bytes and the value
codec, and returns the root it decoded, if the bytes carry one, for the
host to bind.

Why core owns it, weighed against the user's earlier rule that "the core
should not prescribe the serialization format":

- That rule was about a stable, cross-process format, where a host has
  good reasons to choose (CBOR, a database's native type, its language's
  tooling). A process-local encoding has no reader but core itself, so
  there is nothing for a host to choose, and nothing to prescribe to
  anyone.
- An opaque encoding can use what only core knows: definition-local
  state IDs, bitsets sized by the definition, atoms and event-name IDs as
  integers, and delta-coded due times. A public view would have to spell
  these out.
- It keeps the API small: two functions change, none are added for views
  or codecs.

The codec-interface design is kept below as option B, in case the user
prefers it.

### 3. Values go through a value codec the host provides

Core cannot see inside a `ta_value`, so **a value codec is a separate
adapter** passed to `ta_actor_offload` and `ta_actor_load`. This part of
the first version stands:

```c
typedef struct ta_value_codec_vtable {
    ta_result (*encode)(void *self, TA_IN const ta_value *value,
                        TA_INOUT ta_arena *arena, TA_OUT ta_bytes *out);
    ta_result (*decode)(void *self, ta_bytes bytes,
                        TA_OUT TA_RELEASE(ta_value_release) ta_value *out);
} ta_value_codec_vtable TA_VTABLE;
typedef struct { void *self; const ta_value_codec_vtable *v; } ta_value_codec TA_ADAPTER;
```

- **Not on the data model** (ADR 0038 stands): event data the host posts
  has no data model to ask, and no data model has to promise a byte form.
- **Every value in the image goes through it:** event data, delayed-send
  targets and data, completion data, and the optional root.
- **napi's default is `v8.serialize`** on Node and Bun's equivalent on
  Bun: fast, structured-clone rich (`Date`, `Map`, `undefined`, cycles),
  and portability does not matter within one process. This is napi's to
  confirm in its own ADR.
- **The root is included by default in napi,** so one byte string holds
  the whole actor and a load needs no second lookup. A host that keeps
  data elsewhere passes no root.
- **Replay persistence** may reuse the same adapter type for logged event
  data, but it needs a stable encoding, so its codec is its own choice.

### 4. Where the bytes live is the host's choice

Core writes the bytes into the arena it is given and never stores them.
The host decides where they go:

- **napi keeps them in a native offload store** owned by the system: one
  growable region, keyed by address, with a free list for bytes that
  were loaded again. That is one allocation per region, not one
  JavaScript object per actor. A `Buffer` per offloaded actor would cost
  about 190 bytes of JavaScript heap each (measured), close to the bytes
  it holds.
- **Other places are possible:** a temporary file, `mmap`, or compressed
  pages. napi may add them later; this ADR does not require any.
- **ADR 0020's loader** reads from the store for in-process loads, and
  only calls the application's loader when the address has no offloaded
  bytes.

### 5. An arena, not size-then-write

Core writes into the arena, as chart codecs do. Size-then-write (napi
ADR 0018) would have to encode every value once to measure and once to
write, or keep the value bytes somewhere in between, which is an arena.
Offloading happens once per unload, not once per event, so one copy from
the arena into the store is negligible.

### 6. What cannot be offloaded, and what process-local changes

| Case | First version | Now |
|---|---|---|
| Mid-macrostep (a paused or stopped run, queued internal events, a pending completion) | refused: a continuation is interpreter layout | **still refused.** It could be encoded now, since the layout never leaves the build, but napi's `unload()` refuses busy actors anyway, so nothing needs it. |
| Open invocations | the service handle is dropped; loading raises `error.communication.invoke.<id>.unavailable` | **unchanged in this version.** Within one process the handle stays valid, so a later decision could keep an invocation running while its actor is offloaded, with the invoked service's events loading the actor back. Not needed yet. |
| Invoked sessions (they have a parent) | refused: the parent link is a runtime ID | **still refused, but only by policy.** The parent's ID is valid in-process. They are unnamed, and ADR 0020 does not offload them. |
| Scoped I/O origins (ADR 0068) | could not cross | **fine:** a scope lives as long as its system. |
| Interned atoms, event-name IDs, address handles | had to become text | **fine:** stored as integers and retained handles. |

### 7. What an offloaded actor costs

Estimated from the measurements above, for the small chart:

| | Resident | Offloaded (this ADR) |
|---|---|---|
| Native | 432 | 208 (the address and session kept) + about 60 (core's image bytes) + the value bytes |
| Value bytes, `{ count }` | 154 in the JavaScript heap | 72 |
| Value bytes, small entity | 529 in the JavaScript heap | 218 |
| **Total, `{ count }`** | **about 590** | **about 340** |
| **Total, small entity** | **about 960** | **about 490** |

The 60-byte figure for core's part is an estimate for a stable actor with
no pending sends: a few bitset words, the counters and the session ID.
Implementation measures it.

**Offloading halves an actor's memory, and no more,** because of the
208 bytes that stay per unloaded address. Of those, 114 are core's and 94
are napi's directory, value box and scheduling entry. Shrinking them is
where the rest of the saving is, and belongs with the implementation (see
the questions).

## Durability is not this ADR's job

Anything that must survive the process uses one of two mechanisms, both
outside images:

1. **The application persists its data, and spawns with it.** The data
   model is the part of an actor that matters to the application: a
   customer's cart, an order's status. The application stores it as it
   changes, in its own database. After a restart, a loader (ADR 0020)
   returns `{ spawn: definition, binding: data }`, and the chart starts
   fresh with that data. A chart that must resume in the state its data
   implies starts in a routing state whose eventless transitions pick the
   target from the data. That is plain SCXML and needs no API.
2. **Replay persistence** (`docs/proposals/replay-persistence.md`) logs
   every delivered event and rebuilds an actor by replaying the log in
   place, possibly from a snapshot. It reproduces the exact configuration,
   history and pending delayed sends. Its snapshot is its own stable
   format, not this ADR's bytes.

**How XState does it, and where we differ.** XState's
`actor.getPersistedSnapshot()` returns a JSON-serializable snapshot (the
state value, context and child actors), and `createActor(machine,
{ snapshot })` restores it; `input` passes initial data to a new actor
([persistence](https://stately.ai/docs/persistence),
[input](https://stately.ai/docs/input)). So XState's snapshot is a
cross-process format owned by the library. We do not offer one:

- **`input` corresponds to our spawn binding,** and that part matches.
- **A persisted snapshot we leave to replay,** because it is the only way
  to also restore pending delayed sends and history exactly. A snapshot of
  configuration and data alone would silently drop a timer that was set
  before the restart.
- **The cost of that choice:** with approach 1, **pending delayed sends
  do not survive a restart.** A chart that must keep a timeout across
  restarts either re-arms it on entry from its routing state, or uses
  replay. The docs say so plainly.

What tinyactors should add to make approach 1 easy: nothing in core. The
binding already sets the whole data model. napi's docs gain the routing
pattern, a worked restart example with ADR 0020's loader and a stored
binding, and the note on timers.

## Options considered

| Option | Pros | Cons |
|---|---|---|
| **A. Core-owned opaque encoding, host value codec** (chosen) | smallest API; uses core's internal layout; nothing to keep stable | the format is core's, so a host cannot inspect or reuse the bytes |
| B. The first version: a public image view and an image codec interface, with a CBOR reference codec | a host could choose the format, and inspect it with standard tools | a public view and a codec API to keep stable for a byte format nobody else reads; text in place of runtime IDs costs size and time |
| C. Keep in-memory images only | nothing to build | unloading frees nothing, as measured |
| D. Stable, cross-process images for restarts | restarts without application storage | at millions of actors, writing everything at exit is slow and is not durability; replay does the same job properly |
| E. Values through the data model (`value_to_bytes`) | one adapter | reverses ADR 0038; host-made event data has no data model |

## Relation to other decisions

- **ADR 0038:** stands for in-memory images. Offloaded bytes are a second
  form, valid only in their process.
- **ADRs 0051 and 0052:** `ta_actor_load` accepts offloaded bytes as well
  as images. `ta_actor_reconstruct` stays in-memory only.
- **napi ADR 0020:** in-process loads read the offload store. Its restart
  sequence changes: step 4 no longer stores images, and after a restart a
  loader returns `{ spawn, binding }` from the application's data, or a
  replayed actor. napi's unload API changes to offload in one call (its own
  ADR).
- **Replay persistence:** unaffected; it owns durability, and its snapshot
  format is its own.
- **Core ADRs 0067 and 0073:** the step counters, the lifecycle counter and
  pending sends' trace parents are in the bytes, so a load continues the
  same traces.

## Consequences

- Unloading starts to save memory: about half of an idle actor.
- Core gains `ta_actor_offload` and the value codec adapter, and
  `ta_actor_load` gains a bytes form. No view, image codec or reference
  codec package is needed.
- napi gains a native offload store and a default value codec, in a napi
  ADR.
- Pending delayed sends survive offloading, but not a process restart
  unless the application uses replay.

## Testing (when implemented)

- **Round trips:** for every chart in the test host's catalogue, offload a
  stable actor and load it, then compare inspection, the next macrostep's
  trace, pending delayed sends and their firing times with an actor that
  was never offloaded.
- **Refusals:** mid-macrostep and invoked sessions refused; bytes from
  another system or a destroyed one refused with `TA_ERR_STALE`.
- **Allocation failure:** each allocation of offload and load fails in
  turn, cleanly, leaving the actor resident or the bytes intact.
- **Memory:** a napi bench offloads 1 million actors and reports native
  and JavaScript bytes per actor before and after, against section 7.
- **ADR 0020:** mail and timers load offloaded actors from the store.

## Questions for the user

1. **Core-owned opaque encoding (A) rather than the codec interface (B)?**
   The earlier rule against core prescribing a format was about a stable,
   cross-process format; within one process nobody else reads the bytes.
2. **Include the data-model root in the bytes by default in napi,** with
   `v8.serialize` as napi's default value codec?
3. **Shrink the 208 bytes left per unloaded address?** They cap the saving
   at about half. Of them, 114 are core's (the session and address
   records) and 94 napi's (directory entry, value box, scheduling entry).
   Worth its own measurement and change once offloading exists.
4. **Timers across restarts:** accept that approach 1 loses pending
   delayed sends unless a chart re-arms them, and point to replay for
   exact restarts?

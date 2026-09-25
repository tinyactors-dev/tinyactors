# ADR 0074: An unloaded actor is offloaded to bytes within its process

**Status:** Accepted; not yet implemented. Refines ADR 0038 (an image gains a byte form, valid only in the process that wrote it), ADR 0051 (`ta_actor_load` also takes that byte form) and ADR 0054 (a host captures an actor's pending delayed sends and re-arms them at spawn); extends the data-model contract (ADR 0038 kept values off it; this ADR adds a value encode and decode pair); serves napi ADR 0020. Revised in place while Proposed, 2026-09-25: the first version made images cross processes through a codec; the user narrowed the purpose to offloading within one process. Accepted 2026-09-25 with the user's answers (see "Answers"): core owns the encoding; values go through a pair of data-model operations, so each host decides what a value's bytes are; the 208 bytes left per unloaded address are a later, measured follow-up; pending delayed sends are captured as data and re-armed at spawn.
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

The user then added, on restarts: "we need to capture pending sends
somehow, otherwise you never know when you can unload an actor". An
application that restarts an actor from its stored data (napi ADR 0020's
`{ spawn, binding }`) lost every delayed send the actor had armed, so it
could never be sure an actor was safe to let go: a timeout set before the
restart would silently never fire.

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

- The bytes carry the writing system's identity (its address and a
  per-system token). `ta_actor_load()` refuses bytes written by another
  system with `TA_ERR_STALE`.
- Anything that must survive the process uses the mechanisms in
  "Durability is not this ADR's job" below, and section 8 for pending
  sends.

### 2. Core owns the encoding, and it is opaque

**Core encodes and decodes images itself, in a compact, private layout
that may change in any build.** There is no public view and no image
codec interface.

```c
typedef struct {
    ta_actor_id actor;
    TA_OPTIONAL const ta_value *root; /* the data-model root to include */
    TA_ARENA ta_arena *arena;         /* where the bytes are written */
    TA_OUT TA_LIFETIME(arena) ta_bytes *out;
} ta_actor_offload_params;
TA_API ta_result ta_actor_offload(ta_system *system, TA_IN const ta_actor_offload_params *params);
```

`ta_actor_offload` captures and unloads in one step: it writes the bytes
and releases the actor, as `ta_actor_unload` does after a capture.
`ta_actor_load` takes either an in-memory image, as today, or offloaded
bytes. With bytes, the root they carry becomes the data-model binding
when the caller passes none, and it is handed back through an optional
out-parameter for the host's own bookkeeping.

Why core owns it, weighed against the user's earlier rule that "the core
should not prescribe the serialization format":

- That rule was about a stable, cross-process format, where a host has
  good reasons to choose (CBOR, a database's native type, its language's
  tooling). A process-local encoding has no reader but core itself, so
  there is nothing for a host to choose, and nothing to prescribe to
  anyone.
- An opaque encoding can use what only core knows: definition-local
  state IDs, the session storage as it lies in memory, atoms and event-name
  IDs as integers. A public view would have to spell these out.
- It keeps the API small: one function added, one params struct grown.

### 3. Values go through the data model

Core cannot see inside a `ta_value`, so **the data-model contract gains a
pair of operations** that turn one value into bytes and back:

```c
typedef struct { void *shared; TA_IN const ta_value *value; TA_ARENA ta_arena *arena;
                 TA_OUT TA_LIFETIME(arena) ta_bytes *out; } ta_datamodel_value_encode_params;
typedef struct { void *shared; ta_bytes bytes;
                 TA_OUT TA_RELEASE(ta_value_release) ta_value *out; } ta_datamodel_value_decode_params;
ta_op_status (*value_encode)(void *self, TA_IN const ta_datamodel_value_encode_params *params);
ta_op_status (*value_decode)(void *self, TA_IN const ta_datamodel_value_decode_params *params);
```

- **What the bytes contain is the data model's choice.** The user's words:
  "each host can decide what to serialize then, e.g. napi could decide to
  just serialize the handle IDs....or do JSON if it ever needed that." A
  data model that keeps its values alive elsewhere may write a handle; one
  that wants the memory back writes the value itself. Core only stores
  the bytes and hands them back.
- **Every value in the image goes through the actor's data model:** the
  optional root, delayed-send targets and data, and completion data. A
  value that is absent (a null `v`) is stored as absent and never reaches
  the data model.
- **Results follow ADR 0070:** `TA_OP_NOMEM` means nothing changed and the
  offload or load answers `TA_ERR_NOMEM`, so the host may retry;
  `TA_OP_ERROR` means the data model cannot encode that value, and the
  offload answers `TA_ERR_UNSUPPORTED` with the actor still resident.
- **Core's null data model (ADR 0057) ships the trivial pair:** it owns no
  values, so both operations answer `TA_OP_ERROR`. A null-data-model actor
  carries no values in a stable state, so it offloads.
- **This replaces the separate host value codec** of the earlier text, and
  reverses ADR 0038's choice to keep values off the data model. Values a
  host made itself (event data it posted) are only in an image while a
  macrostep runs or events are queued, and section 6 refuses those, so
  every value an offloaded image holds was made by, or bound through, the
  actor's data model.
- **Each host picks its default** in its own ADR. napi's choice is
  measured in the implementation notes: writing the value box's handle
  keeps the JavaScript object alive and frees only core's part;
  `v8.serialize` frees the object too.

### 4. Where the bytes live is the host's choice

Core writes the bytes into the arena it is given and never stores them.
The host decides where they go:

- **napi keeps them in a native offload store** owned by the system, keyed
  by address, not in a JavaScript object per actor. A `Buffer` per
  offloaded actor would cost about 190 bytes of JavaScript heap each
  (measured), close to the bytes it holds.
- **Other places are possible:** a temporary file, `mmap`, or compressed
  pages. napi may add them later; this ADR does not require any.
- **ADR 0020's loads** read from the store for in-process loads, and only
  call the application's loader when the address has no offloaded bytes.

### 5. An arena, not size-then-write

Core writes into the arena, as chart codecs do. Size-then-write (napi
ADR 0018) would have to encode every value once to measure and once to
write, or keep the value bytes somewhere in between, which is an arena.
Offloading happens once per unload, not once per event, so one copy from
the arena into the store is negligible.

### 6. What cannot be offloaded, and what process-local changes

**Only a stable, named actor offloads.** `ta_actor_offload` answers
`TA_ERR_BUSY`, and changes nothing, for an actor that is inside a
macrostep, has a paused or stopped run (ADRs 0061 and 0070), has internal
events queued, has a completion pending, or was invoked by a parent; and
`TA_ERR_INVALID` for an anonymous actor, as `ta_actor_unload` does.

| Case | First version | Now |
|---|---|---|
| Mid-macrostep (a paused or stopped run, queued internal events, a pending completion) | refused: a continuation is interpreter layout | **still refused.** It could be encoded now, since the layout never leaves the build, but napi's `unload()` refuses busy actors anyway, so nothing needs it. |
| Open invocations | the service handle is dropped; loading raises `error.communication.invoke.<id>.unavailable` | **unchanged in this version.** Within one process the handle stays valid, so a later decision could keep an invocation running while its actor is offloaded. Not needed yet. |
| Invoked sessions (they have a parent) | refused: the parent link is a runtime ID | **still refused, but only by policy.** The parent's ID is valid in-process. They are unnamed, and ADR 0020 does not offload them. |
| Scoped I/O origins (ADR 0068) | could not cross | **fine:** a scope lives as long as its system. |
| Interned atoms, event-name IDs, address handles | had to become text | **fine:** stored as integers; addresses as their text, since an unloaded actor's address handle is the directory's, not the image's. |

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
no pending sends. The implementation measures it, and napi's value
choice, in the implementation notes.

**Offloading halves an actor's memory, and no more,** because of the
208 bytes that stay per unloaded address. Of those, 114 are core's and 94
are napi's directory, value box and scheduling entry. Shrinking them is a
separate, measured follow-up: "we need to learn more" (napi's roadmap).

### 8. Pending delayed sends are captured as data, and re-armed at spawn

An application that restarts an actor from its own stored data must be
able to keep the actor's timers. **Core lets a host read an actor's
pending delayed sends as plain records, and re-arm them when it spawns
the actor again.** The records stay the application's data; core defines
no byte format for them (section 1).

- **Capture is inspection.** `ta_actor_inspect` already lists pending sends
  as `ta_delayed_send_info` (ADR 0054). It gains what a re-arm needs and
  inspection lacked: `has_content` and `content` (a literal `<content>` for
  a host I/O processor, by content ID in the definition), and `trace` (the
  sending step's trace parent, ADR 0067).
- **Re-arm is a spawn parameter.** `ta_actor_spawn_params` gains
  `pending_sends` and `pending_send_count`, records of the same type. Core
  schedules them in the new actor's own session, before its bootstrap
  runs, keeping each send's SCXML send ID, due time, event, I/O type,
  destination, target, data, content and trace parent. A due time already
  past fires on the next dispatch.
- **Send IDs keep working.** A generated ID (`#_send.<n>`, ADR 0048) is the
  session's counter; core raises the new session's counter past every
  re-armed generated ID, so a later `<send>` never reuses one, and a
  `<cancel>` naming a re-armed send, by `sendid` or through a value stored
  with `idlocation`, finds it. An author's literal ID is kept as it is.
- **Validation:** a record's event name must be one this system issued, and
  its content IDs must exist in the spawned definition; otherwise the spawn
  answers `TA_ERR_INVALID` and creates nothing. Content IDs are stable
  within one revision of a definition, so an application re-arms against
  the revision it captured under. A destination that is a runtime actor ID
  from another process means nothing here: a host translates destinations
  through addresses, and a stale ID becomes a dead letter at dispatch, as
  it would for any stale target.
- **Submitted sends** (the router refused one and it waits to be offered
  again) are re-armed as ordinary pending sends.

**This answers "when can I unload an actor".** An actor may be let go
when it is stable (section 6), and what it needs to continue is its data
model plus its pending sends: both are the application's to store, and
together they bring the actor back with its timers. In-process, section 2
does this automatically; across a restart, the application stores the two
and spawns with them.

## Durability is not this ADR's job

Anything that must survive the process uses one of two mechanisms, both
outside offloaded bytes:

1. **The application persists its data and pending sends, and spawns
   with them.** The data model is the part of an actor that matters to the
   application: a customer's cart, an order's status. The application
   stores it as it changes, with the actor's pending sends (section 8), in
   its own database. After a restart, a loader (ADR 0020) returns
   `{ spawn: definition, binding: data, pendingSends }`, and the chart
   starts fresh with that data and its timers armed. A chart that must
   resume in the state its data implies starts in a routing state whose
   eventless transitions pick the target from the data. That is plain
   SCXML and needs no API.
2. **Replay persistence** (`docs/proposals/replay-persistence.md`) logs
   every delivered event and rebuilds an actor by replaying the log in
   place, possibly from a snapshot. It reproduces the exact configuration
   and history as well. Its snapshot is its own stable format, not this
   ADR's bytes.

**How XState does it, and where we differ.** XState's
`actor.getPersistedSnapshot()` returns a JSON-serializable snapshot (the
state value, context and child actors), and `createActor(machine,
{ snapshot })` restores it; `input` passes initial data to a new actor
([persistence](https://stately.ai/docs/persistence),
[input](https://stately.ai/docs/input)). So XState's snapshot is a
cross-process format owned by the library. We do not offer one:

- **`input` corresponds to our spawn binding,** and that part matches.
- **Delayed events are captured as data** (section 8), next to the
  binding, rather than inside a library-owned snapshot.
- **The configuration and history are not restored by approach 1.** A
  chart that needs them exactly uses replay, or routes on its data.

## Options considered

| Option | Pros | Cons |
|---|---|---|
| **A. Core-owned opaque encoding, values through the data model** (chosen) | smallest API; uses core's internal layout; nothing to keep stable; each host decides what a value's bytes are | the format is core's, so a host cannot inspect or reuse the bytes |
| B. The first version: a public image view and an image codec interface, with a CBOR reference codec | a host could choose the format, and inspect it with standard tools | a public view and a codec API to keep stable for a byte format nobody else reads; text in place of runtime IDs costs size and time |
| C. Keep in-memory images only | nothing to build | unloading frees nothing, as measured |
| D. Stable, cross-process images for restarts | restarts without application storage | at millions of actors, writing everything at exit is slow and is not durability; replay does the same job properly |
| E. A separate value codec the host passes (the earlier text) | keeps ADR 0038's line | a second adapter beside the data model that owns the values anyway; the user preferred extending the data model |
| F. Pending sends restored only by replay | no new spawn parameter | an application restarting from its data could never keep a timer, so it could never know when unloading is safe |

## Relation to other decisions

- **ADR 0038:** stands for in-memory images. Offloaded bytes are a second
  form, valid only in their process. Its choice to keep values off the
  data model is reversed by section 3.
- **ADRs 0051 and 0052:** `ta_actor_load` accepts offloaded bytes as well
  as images. `ta_actor_reconstruct` stays in-memory only.
- **ADR 0054:** `ta_delayed_send_info` gains content and trace, and a spawn
  can re-arm records of it.
- **ADR 0057:** the null data model implements the new pair trivially.
- **napi ADR 0020:** in-process loads read the offload store. Its restart
  sequence changes: step 4 no longer stores images; after a restart a
  loader returns `{ spawn, binding, pendingSends }` from the application's
  data, or a replayed actor.
- **Replay persistence:** unaffected; it owns exact restarts, and its
  snapshot format is its own.
- **Core ADRs 0067 and 0073:** the step counters, the lifecycle counter and
  pending sends' trace parents are in the bytes, so a load continues the
  same traces; re-armed sends keep their trace parents.

## Consequences

- Unloading starts to save memory: about half of an idle actor.
- Core gains `ta_actor_offload`; `ta_actor_load` gains a bytes form; the
  data-model vtable gains `value_encode` and `value_decode`, which every
  data model implements; `ta_delayed_send_info` gains content and trace;
  `ta_actor_spawn_params` gains pending sends.
- napi gains a native offload store and a default value choice.
- Pending delayed sends survive offloading, and a process restart when the
  application stores them with its data.

## Testing (when implemented)

- **Round trips:** offload a stable actor and load it, then compare
  inspection, the next macrostep's behaviour, and pending delayed sends and
  their firing times with an actor that was never offloaded.
- **Refusals:** mid-macrostep, queued internal events and invoked sessions
  refused with nothing changed; bytes from another system refused with
  `TA_ERR_STALE`.
- **Allocation failure:** each allocation of offload and load fails in
  turn, cleanly, leaving the actor resident or the bytes intact.
- **Re-arm:** pending sends captured by inspection and passed to a spawn
  fire at their due times, keep their send IDs, can be cancelled by ID,
  and a later generated ID does not collide.
- **Memory:** napi measures native and JavaScript bytes per actor before
  and after, for each value choice.
- **ADR 0020:** mail and timers load offloaded actors from the store; a
  restart through `{ spawn, binding, pendingSends }` keeps the timers.

## Answers

The questions of the Proposed version, answered by the user on
2026-09-25:

1. **Core-owned opaque encoding (A)** rather than the codec interface (B):
   yes.
2. **Values:** include the data-model root, but "extend the datamodel
   contract -- each host can decide what to serialize then, e.g. napi could
   decide to just serialize the handle IDs....or do JSON if it ever needed
   that." Section 3.
3. **The 208 bytes left per unloaded address:** a separate follow-up, "we
   need to learn more".
4. **Timers across restarts:** "we need to capture pending sends somehow,
   otherwise you never know when you can unload an actor." Section 8.

# ADR 0085: An image restores into any system that registers its revision

**Status:** Accepted and implemented (2026-09-26). Refines ADR 0038 and ADR 0052 (where an image may be restored), ADR 0066 (the session ID in another system) and ADR 0014 (unload refuses an image of another system); applies ADR 0028, ADR 0049, ADR 0065, ADR 0068 and ADR 0070. Builds on ADR 0074: an image names its system as offloaded bytes do, by address and token, and offloaded bytes still load only into the system that wrote them (section 8).
**Date:** 2026-09-26

## Context

Logical time belongs to a system (ADR 0012): `ta_system_set_time()` moves
every actor in it, and every delayed send goes through that system's I/O
processors. A debugger's fork of a production actor
(`docs/proposals/debug-adapter.md`, "A fork needs its own system") must be
able to run the actor's future now (`clock +3d`) and send into stub
processors, without moving production's clock or reaching production's
services. So the fork lives in a system of its own, in the same process,
restored from an image of the production actor.

`ta_actor_reconstruct()` and `ta_actor_load()` take any image, but an image
was only partly independent of the system that captured it. ADR 0065 made
its addresses detached text and ADR 0068 its I/O origins, and both are
interned again on restore. Everything else that a system interns stayed a
number of the capturing system:

| In the image | Identity | Whose |
|---|---|---|
| each held and queued event | `name` (`ta_event_name_id`) | interned per system (`event_names`) |
| | `send_id` of a literal ID (atom + 1) | interned per system (`atoms`) |
| | `invoke_id` (atom + 1) | interned per system |
| | a `TA_ORIGIN_INVOKE` origin: its invocation and its invoker's registry index | per system |
| | a `TA_ORIGIN_ACTOR` origin with a runtime ID | the capturing system's actor table |
| each delayed send | `event`, literal `send_id`, `io_type` (an atom) | per system |
| | `destination` with a runtime ID (another actor, or this session by its session ID, ADR 0066) | the capturing system's actor table |
| each invocation record | `id`, `type` | per system |
| the parent of an invoked child | `actor` (a runtime ID), `invoke_id` | per system |
| the session | `session_id` | a runtime ID of the capturing system (ADR 0066) |
| freshness | `source_actor`, `source_epoch` | the capturing system |

Another system that registers the same revision interns names in its own
order, so these numbers name other events, other sends and other
invocations there, or nothing at all. A runtime ID of the capturing system
may be a live actor of the other system: a restored send to "the ledger"
would reach whatever actor has that slot and generation there.

The rest of an image is independent of the system already, given the exact
revision (ADR 0038): the session storage (configuration, history and
late-binding bits), state IDs (the final state, pending invocations, an
operator transition's targets, ADR 0082), the continuation's sets, enabled
transitions and frames (ADR 0061), content IDs of literal content and
targets (ADR 0053), generated send IDs (`#_send.<n>` is the actor's own
counter), trace parents and step counters (ADR 0067), and times. A
definition compiled in another system from the same spec numbers its
states, transitions, actions and content the same way; that is what its
revision promises.

Host values are the last part. An image retains the values its events and
sends carry (ADR 0050), and restoring retains them again. A host whose
values belong to a system (napi's value boxes, napi ADR 0011) cannot let a
second system hold the first system's values past its close.

## Decision

### 1. An image carries every interned identity as text

Capture writes, into the image's arena, the text of each identity in the
table above that the capturing system interned:

- an event name as its segments, root first. Not dotted text: a segment
  may hold a dot (an invocation ID such as `s1.3` is one segment of
  `error.communication.invoke.s1.3.unavailable`, `src/invoke.c`);
- a literal send ID's author text. A generated ID and no ID are numbers
  that mean the same in every system, so they stay numbers;
- an invocation ID's text (ADR 0049 made it readable back), for held and
  queued events, invocation records and the parent link;
- the invoker type of a `TA_ORIGIN_INVOKE` origin;
- a delayed send's I/O type, except the SCXML processor's, which every
  system has.

Restoring into another system interns each text there
(`ta_atom_intern`, `ta_event_name_child`, `ta_invoke_id_from_text`,
`ta_send_id_intern_literal`), as addresses and I/O origins already are. An
invoked service's origin names its invoker by the restoring system's
registry index for that type; when no invoker of that type is registered
there, the event has no origin, as an unknown I/O processor type already
leaves none (ADR 0068). Restoring into the capturing system keeps the
numbers: its tables never forget what they interned, so it behaves, and
costs, exactly as before.

### 2. An image knows which system captured it

An image names the system that captured it the way ADR 0074's offloaded
bytes do: by the system's address and its token, a 64-bit number drawn
once from the system's ID generator (ADR 0073). ADR 0074 drew the token at
a system's first offload; it is now drawn at its first capture, which an
offload makes anyway, and kept for the system's life. A restore is into
another system when either differs. One mechanism names a system for both
forms of an unloaded actor.

- **The address alone is not enough.** A system created after another is
  destroyed may get its address. The token tells the two apart, and a
  system that has captured nothing yet has token 0, which no image
  carries, so it takes no image for its own.
- **What it does not promise.** Core keeps no global state, so it cannot
  number systems. Two systems at the same address, one after the other,
  that seed their generators alike (ADR 0073's default seed is 0) and draw
  the same number of IDs before their first capture draw the same token,
  and the later one would take the earlier one's image for its own. A host
  that keeps images past their system seeds each system differently: napi
  seeds every system from the operating system. ADR 0074 accepted the same
  limit for bytes.
- **Nothing to release.** The token is a number in the image, so
  `ta_session_image_destroy()` has nothing of the system's to give back
  beyond the values it already releases.

The first version of this ADR allocated a reference-counted token that the
system and its images held, so that no later system could have it. It was
replaced by ADR 0074's token when both were merged: two ways to name a
system would disagree about the same image, and the reference-counted one
cost an allocation and a rule that `ta_session_image_destroy()` be
synchronized with the capturing system for the token's sake.

### 3. A runtime reference to another actor names nothing elsewhere

In another system, a runtime reference in the image becomes:

- the restored actor, when it names the captured session: its runtime ID
  or its session ID (a send to itself, ADR 0066);
- runtime ID 0 otherwise, which no actor has (ADR 0059), and which the
  target URI parser already uses for a session URI that names no session
  (ADR 0044). A send to it fails as a send to a destroyed actor does: the
  router finds no actor, and the chart gets `error.communication`. The
  origin of a queued event from such an actor names no session either, so
  a reply to it fails the same way.
- An invoked child's parent is gone: its `parent.actor` is 0, so a send to
  `#_parent` fails and its completion finds no parent, as when the parent
  has ended.

A fork's view of the world is only itself: it cannot reach production's
actors, which is what a fork should do.

### 4. In another system the restored actor is a new session

A session ID is the runtime ID the session was spawned with (ADR 0066), a
number of the capturing system. The other system may already have an
actor with that runtime ID, or issue it later, and two live sessions must
never share an ID. So `ta_actor_reconstruct()` and `ta_actor_load()` into
another system start a new session whose ID is the restored actor's runtime
ID, as reconstruction does when a resident session holds the image's ID.
In the capturing system nothing changes: reconstruct continues the session
unless a resident session holds it, and load always continues it.

### 5. Unload refuses an image of another system

`ta_actor_unload()` compared the image's source actor ID and epoch with the
actor. Another system may have an actor with the same runtime ID at the
same epoch. An image of another system describes another actor, so unload
answers `TA_ERR_STALE` for it (ADR 0014).

### 6. An adopter gives the restoring system the image's values

`ta_actor_reconstruct_params` and `ta_actor_load_params` gain an adapter:

```c
typedef struct ta_value_adopter_vtable {
    ta_result (*adopt)(void *self, const ta_value *value, ta_value *out);
} ta_value_adopter_vtable;
typedef struct { void *self; const ta_value_adopter_vtable *v; } ta_value_adopter;
```

For every value the restored actor takes from the image (held and queued
event data, a delayed send's target and data, completion data, and
`<foreach>` snapshots of a paused continuation), core calls `adopt`, which
stores a value the restoring system's host may hold, owning one reference:
the same value retained, or a copy. Any result but `TA_OK` fails the
restore, which leaves nothing behind (ADR 0070). A zero adopter retains
each value, as restore always did. Core does not decide whether a value
must be copied: only the host knows whether its values belong to a system.
The test host's values belong to its data-model contexts, which outlive
every system, so it passes none; napi copies each box of another system
into the restoring system's values (napi ADR 0026).

The data-model binding needs no adopter: the host supplies it for the
restoring system, and a fork passes its own copy of the root.

**Why not ADR 0074's `value_encode` and `value_decode`.** They turn a
value into bytes and back through the actor's data model, and a restore
into another system could run every value through them. They do not fit:

- an in-memory image holds values no data model made: event data a host
  posted, in the held and queued events of an actor captured mid-macrostep
  (ADR 0074 section 6 refuses such actors for bytes; this ADR restores
  them);
- a round trip through bytes is a copy with the data model's rules, which
  a host may not want (napi's `v8.serialize` cannot copy a function, and
  its fallback, a handle, names a box of the capturing system); the
  adopter lets the host keep sharing the value and only move its box;
- the pair is optional in the data-model vtable, and the test host's
  models implement it, but a data model without it could never be forked.

So the adopter stays, and the pair stays what ADR 0074 made it: the byte
form of an offloaded actor.

### 7. Restoring into another system may allocate more

Interning names the restoring system has not seen allocates. A restore
that runs out of memory at any allocation answers `TA_ERR_NOMEM` and
leaves no actor, as before (ADR 0070); the names it interned stay in the
system's tables, which are not state of any actor. When the system has
registered the revision, its compiler has already interned every authored
event name and literal send ID, so the usual restore interns nothing new.

### 8. Offloaded bytes stay in their system

ADR 0074's rule for bytes is unchanged: `ta_actor_load()` refuses bytes of
another system with `TA_ERR_STALE`. Bytes store interned numbers and raw
records, and nothing but the writing system can read them; an image
carries the text this ADR adds, and bytes do not need it. A fork of an
offloaded actor loads it in its own system first and captures an image
there.

## Alternatives considered

- **Keep the capturing system alive and resolve names through it.** The
  image would hold a reference to the capturing system and read its
  tables on restore. A fork would then keep production's system alive,
  and an image could not outlive its system at all. Text is what ADR 0028
  asked of anything that crosses systems, and what addresses and I/O
  origins already do.
- **Text only, and always intern on restore.** One path instead of two.
  Measured, it more than doubled the cost of every restore into the
  capturing system, which the paging hosts do all the time, for no gain:
  the capturing system's numbers are correct there.
- **Refuse an image of another system that holds runtime references to
  other actors.** A fork of any actor that ever talked to another would be
  refused. The references are meaningless there, not dangerous, once they
  name nothing.
- **Clone values in core.** `ta_value_vtable` has no `clone`, deliberately
  (ADR 0050), and core cannot know which values need one. The adopter is
  the host's decision, per restore.
- **Let the host say whether an image is foreign.** A flag in the params
  would be one more thing to get wrong, and a wrong answer resolves
  runtime IDs in the wrong table. Core can know, so core decides.

## Consequences

- A debugger can fork an actor into a system of its own with its own
  clock and stub processors (`docs/proposals/debug-adapter.md`), and the
  fork cannot reach production's actors or services.
- An image restores after the system that captured it is destroyed.
- The public API changes: `ta_value_adopter` is new, and the load and
  reconstruct params gain `adopter`. Callers updated: the test host's
  generated bindings (the field is an optional adapter no scenario passes)
  and napi, which injects its adopter into both calls (napi ADR 0026).
- The image format changes. An image is an in-memory structure, not a
  wire format (ADR 0038), so nothing stored depends on it. Offloaded bytes
  (ADR 0074) do not change: they carry no text.

### Costs

Measured with `clang -O2` on the development machine, for an idle actor of
the null data model with three pending sends, each with a three-segment
event name and a literal ID to the SCXML processor (a scratch program that
counts the allocator's calls and times 200,000 iterations):

| | Before | After |
|---|---|---|
| `ta_session_image` struct | 488 bytes | 560 bytes |
| a capture: allocations | 2 | 2 |
| a capture: bytes | 1,173 | 1,565 |
| capture and destroy | 0.08 µs | 0.18 to 0.24 µs |
| a restore into the capturing system | 4 allocations, 1,128 bytes, 0.18 µs | the same |
| a restore into another system | not possible | 4 allocations, 1,128 bytes, 0.31 µs |

- **Image bytes.** The struct grows by 72 bytes: the system's address and
  token (16), and five pointers or texts for the text arrays and the
  parent's invocation. The
  arena reserves, per identity-carrying record, the text record and a
  64-byte estimate of its text: 112 bytes more per pending send (a 48-byte
  record), 144 per held or queued event (an 80-byte record), 80 per
  invocation. What a name takes beyond the estimate grows the arena.
- **Capture work.** Each name segment and each literal ID is copied into
  the arena: about 35 ns per pending send in the measurement above.
- **Restore work.** Into the capturing system: none. Into another system:
  one intern per name segment, literal send ID, invocation ID and I/O
  type, each a hash lookup that allocates only for text the system has
  not seen, and one `adopt` call per value.
- **Per system:** nothing new; ADR 0074's token is drawn at the first
  capture instead of the first offload, so a system's first capture takes
  one number from its ID generator, and trace IDs made after it differ
  from those of a run without the capture.
- The times and bytes in the table were measured with the first version's
  reference-counted token; the struct sizes and allocation counts are
  those of the merged code.

## Implementation notes

Implemented in `src/session_image.c`: the text at capture
(`ta_image_copy_event_text`, `ta_image_copy_send_text`), the restore
context and its runtime-ID rule (`ta_image_restore`,
`ta_image_map_runtime`), and the per-kind restores (`ta_image_restore_event`,
`ta_image_restore_send`, `ta_image_restore_sparse`,
`ta_image_restore_pause`). `src/interpreter.c` gains
`ta_send_id_literal()` and `ta_send_id_intern_literal()`.

- **Unsubmitted sends carry no envelope.** Only sends the router has
  refused (submitted, ADR 0054) have an `origin` and an `invoke_id`, and an
  image holds only unsubmitted ones. The restore therefore no longer
  detaches, localizes or rebinds a send's origin, and
  `ta_image_rebind_self()`, which only ever saw an empty origin since ADR
  0066, is removed.
- **Tests.** `tests/image_across_systems_test.c`: a copy in a second
  system whose names are numbered differently fires its send to itself on
  that system's clock, its send to another actor of the capturing system
  fails although that runtime ID is a live actor there, and the original
  is untouched; an image restores after its system is destroyed, and a
  later system with the same actors and IDs gets a new session; an
  adopter copies a held event's data, and a refusing one fails the restore
  and leaves nothing; unload refuses an image of another system and load
  restores it there; and every allocation of a restore into a fresh system
  is refused in turn. `tinyactors-test/ruby/scenarios/images_across_systems.rb`
  covers the same through the test host, with data, a lost invocation
  whose ID holds a dot, and a copy made at every pause of a macrostep with
  a `<foreach>` and queued events.

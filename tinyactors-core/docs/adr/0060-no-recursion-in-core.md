# ADR 0060: No recursion in core

**Status:** Accepted; applies ADR 0023, ADR 0033 and ADR 0047
**Date:** 2026-09-23

## Context

Core runs on hosts whose stack is small, fixed, and not core's to choose:
an MCU task, a WASM stack, a host thread (ADR 0002, ADR 0023). Several
walks in core recurse over structures whose depth the author of the input
chooses:

| Recursion | Where | Depth follows |
|---|---|---|
| `ta_compiler_collect` calls itself | `src/builder.c` | state nesting |
| `ta_compiler_block` → `ta_compiler_action` → block again, through `<if>` then/else, `<foreach>` body and `<invoke>` finalize | `src/builder.c` | nesting of executable content |
| `ta_add_descendants`, `ta_in_final_state`, `ta_effective_targets` | `src/interpreter.c` | state nesting, on every transition |
| `ta_run_block` ↔ `ta_exec_action` ↔ `ta_exec_foreach` | `src/interpreter.c` | nesting of executable content, as it runs |
| `ta_export_state` ↔ `ta_export_states`, `ta_export_action` ↔ `ta_export_actions` | `src/export.c` | both |
| `ta_pattern_match_from` | `src/definition.c` | segments in an event descriptor, with backtracking at each `*` |

Recursion turns input depth into C-stack depth. When the stack runs out,
the process crashes, and no `ta_result` can report it. The worst case is at
run time: a chart that compiles on one thread can overflow a smaller stack
later, in the middle of a transition.

Core has no limit on nesting. The SCXML codec bounds untrusted documents
(`max_depth`, 64 elements by default), but a chart built in C, or by
another front end, can nest as deep as its author likes.

The pattern matcher has a second problem besides depth. It backtracks at
each `*`, so a descriptor made of many `*` segments costs exponential time.
The event side is capped at 64 segments (`TA_EVENT_NAME_MAX_DEPTH`), but
the descriptor side is not capped.

A depth limit in core was considered first. It would make recursion safe
only when the limit is small enough for the smallest stack core runs on,
and core cannot know that size. The limit would become a guess that every
embedder has to tune. Removing the recursion removes the need to guess.

## Decision

### 1. The rule

No function in `tinyactors-core/src` may call itself, directly or through
other functions. The call graph of core is acyclic.

`mise run check-recursion` builds core's call graph from the sources and
fails on any cycle, naming the functions in it. It is part of
`mise run test`, so a change that brings recursion back cannot land.

The rule applies to core only. The SCXML codec and txml bound their own
input and have their own rules.

### 2. Nested walks use explicit stacks

A walk over nested data keeps a stack of frames. A frame holds the list
being walked, the index into it, and a `stage` when the walk does work
before, between or after its children:

- `<if>`: evaluate or compile the condition, then the then block, then the
  else block, then store the action;
- `<foreach>`: the array, then the item and index, then the body.

Where each stack lives:

| Walk | Stack | Why there |
|---|---|---|
| The compiler | A `ta_vec` of frames in `ta_compiler`, from the compile allocator. It replaces the ADR 0058 path stack: every frame names its list and index, so the frames are the path. | Compiling may fail with `TA_ERR_NOMEM` at any point. |
| Export | The exporter's arena. | Export already allocates everything in that arena. |
| Running executable content | One stack owned by the system, shared by every actor it runs. Its frames use the `ta_exec_frame` layout that `src/continuation.h` already declares (`ACTIONS` and `FOREACH`, with `cursor`, `iteration` and `iteration_snapshot`). | A system runs one macrostep at a time (`docs/internal/concurrency.md`), and an actor pauses only between microsteps, when the stack is empty. No actor needs frames of its own. |

### 3. The run-time stack is reserved at registration, never grown while actions run

The compiler records the deepest nesting of executable content in the
definition. It visits every block, so this costs nothing extra.
`ta_system_register_definition()` grows the system's frame stack to that
depth if it is not already that large. So the stack always has room for
every registered definition, and it never grows while actions run.

This is what ADR 0047 requires. A failed allocation must happen before any
part of the microstep is visible. With a stack that grows while actions
run, the allocation could fail after an earlier `<assign>` in the same
block had already changed the data model. Reserving at registration puts
the only possible failure in `ta_system_register_definition()`, which
returns `TA_ERR_NOMEM` and registers nothing. Running an actor never fails
for want of frames.

The recursive executor never failed an allocation for its frames either,
because the C stack never reports failure; it crashes instead. The reserved
stack turns that crash into a `TA_ERR_NOMEM` at registration.

The stack is not per actor, so a million idle actors cost nothing for it.
It is sized by the deepest registered definition, not by the number of
actors. Retiring a definition does not shrink it.

**Why not on the actor.** `ta_continuation.frames` was declared for pausing
in the middle of a block (ADR 0033). A paused actor would then need its own
frames, kept until it resumes. Core does not pause inside a block today
(see the last point under Consequences), so per-actor frames would cost memory on
every actor for nothing. If pausing inside a block is added, the frames of
a paused actor move to its continuation at that point, and that change
decides how they are captured in a session image.

### 4. State-tree queries use the compiled layout

The compiler numbers states in document pre-order. So a state's subtree is
a contiguous range of IDs, and every child has a higher ID than its parent.
The run-time queries use this instead of a stack:

- **Is a compound or parallel state in a final state?** One pass over the
  states in reverse ID order. Each child's answer is known before its
  parent is visited.
- **Adding the descendants to enter** walks child spans with a worklist.
- **History defaults** are resolved with a worklist bounded by the number
  of history states.

Any scratch space these need (a bitset, a worklist) is sized from the
definition and reserved at registration, like the frame stack.

### 5. Event descriptors: `*` anywhere, matched in polynomial time

A `*` segment may appear anywhere in an event descriptor, not only at the
end. It matches zero or more event segments:

- `*` matches every event;
- `a.*` matches `a` and every event below it (SCXML 3.12.1: a trailing `.*`
  adds nothing to prefix matching);
- `a.*.c` matches `a.c`, `a.b.c` and `a.b.x.c`, and, because descriptors
  match by prefix, `a.b.c.d` too;
- `*.error` matches `error`, `x.error` and `x.y.error.detail`.

Only a whole segment is a wildcard. `a*` and `*b` are ordinary segments.

A `*` in the middle of a descriptor goes beyond SCXML, which defines only
a bare `*` and a trailing `.*`. Core already accepted it: the compiler kept
middle wildcards, and the matcher gave them this meaning. This ADR makes
it deliberate and gives it a bounded cost.

The matcher is the two-pointer wildcard algorithm. It walks the event and
the descriptor together, remembers the last `*` it passed and the event
position to resume from, and on a mismatch retries one segment further
from that `*`. It ends with a match as soon as the descriptor is used up,
which is prefix matching. It uses O(1) space and O(event × descriptor)
time. With the event capped at 64 segments, the time is linear in the
descriptor.

### 6. Visit order does not change

The revision hash, the numbering of states and expressions, the layout of
nested action blocks after their parent block, and the ADR 0058 replay all
depend on the order in which the compiler visits the spec. Replacing the
recursion keeps that order exactly. The export round-trip check (every
chart keeps its revision) and the IRP suite must pass unchanged, and they
are the test that it did.

### 7. No depth limit in core

With no recursion, deep input costs heap memory proportional to its depth,
not stack. Core adds no nesting limit to protect the stack. Bounding
untrusted input is the front end's job, as the SCXML codec does with
`max_depth`. A host that wants a limit for charts built in C can check the
depth before compiling.

## Consequences

- A chart nested ten thousand levels deep compiles, exports and runs on a
  64 KiB stack. A test does this under an allocator that counts bytes.
- Running out of memory for frames is `TA_ERR_NOMEM` from
  `ta_system_register_definition()`. Running an actor never fails for want
  of frames, and never in the middle of a block.
- Each definition records its maximum action depth. The system keeps one
  frame stack, as deep as its deepest registered definition. Actors carry
  no frames, so the per-actor cost is unchanged.
- The compiler's walk becomes a frame loop with stages, which is longer than
  the recursion it replaces. Visit order is tested against every chart in
  the suite, so a mistake in it cannot go unnoticed.
- A descriptor full of `*` segments matches in linear time. A test with
  thousands of `*` segments checks this.
- **Not decided here: pausing inside a block.** Frames on the actor make it
  possible to pause in the middle of a block or a `<foreach>` iteration, as
  ADR 0033 describes. Today `ta_actor_resume`'s budget pauses only between
  microsteps, and this ADR does not change that. Pausing inside a block
  needs the frames in the session image (ADR 0038) and in inspection, and
  it needs its own decision. The frame layout chosen here is the one it
  would use.

## Sources

- SCXML 1.0, 3.12.1 Event Descriptors —
  <https://www.w3.org/TR/scxml/#EventDescriptors>
- In-tree: `src/builder.c` (`ta_compiler_collect`, `ta_compiler_block`,
  `ta_compiler_action`), `src/interpreter.c` (`ta_run_block`,
  `ta_exec_action`, `ta_exec_foreach`, `ta_add_descendants`,
  `ta_in_final_state`, `ta_effective_targets`), `src/export.c`,
  `src/definition.c` (`ta_pattern_match_from`), `src/continuation.h`.

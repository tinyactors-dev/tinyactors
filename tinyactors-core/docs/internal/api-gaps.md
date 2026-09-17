# Current header skeleton: gaps against intended design

This file is intentionally candid. The headers compile, but they do not yet expose every accepted design decision.

## Public registration/build surface

`include/tinyactors.h` currently exposes system creation and actor operations, but definition registration, builder operations, I/O registration, invoker registration, and actor-directory configuration still need final public-surface review.

## Session-image implementation

`ta_session_image` is intentionally opaque but not implemented yet. Capture/reconstruction must cover all Tinyactors-owned state in `semantic-state.md`, retain interpreter-held host values without traversing their object graphs, and verify exact definition identity on reconstruction.

## Named actor replacement activation

The directory has `replace`, but the public lifecycle still needs a clean way to prepare a replacement actor with the incumbent logical self-address without publishing it before bootstrap completes. Activation must make the directory swap atomic from the routing perspective.

## Completion and actor lifecycle

Clarify how reaching a top-level final state differs from explicit `ta_actor_destroy()`, when final data is returned to the host, and how a child actor generates `done.invoke.<id>` before eventual reclamation.

## Timer dispatch API

The public host-facing mechanism for observing resident delayed sends and routing due work needs a clear API that does not create an event loop inside Tinyactors. Durable deadline indexing for non-resident actors belongs to an optional durability layer.

## Resident layout

`ta_actor` currently contains generic vector pointers/counts for queues/invocations. This is suitable as a skeleton but not the final density target. Move uncommon data to sparse pools/side tables or definition-specialized slabs after semantics stabilize.

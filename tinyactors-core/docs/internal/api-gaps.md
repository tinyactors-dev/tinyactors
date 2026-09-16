# Current header skeleton: gaps against intended design

This file is intentionally candid. The headers compile, but they do not yet expose every accepted design decision.

## Public registration/build surface

`include/tinyactors.h` currently exposes system creation and actor operations, but definition registration, builder operations, I/O registration, invoker registration, and actor-directory configuration are internal declarations. Decide which are public embedder APIs and surface them without requiring `src/internal.h`.

## Full definition semantics

`ta_definition` / builder still need representation for:

- explicit initial transitions and initial executable content;
- transition type (`internal` / `external`);
- onentry/onexit action ranges;
- executable-content IR;
- raise/send/cancel actions;
- conditional/foreach structures;
- invocation declarations, finalize blocks, and autoforward;
- data declarations and early/late binding metadata;
- extension executable actions.

## Invocation restore

The invoker interface currently lacks explicit snapshot/rebind/restore semantics for live external services. Define how an invoker participates in unload/restart, or when it may refuse serialization.

## Completion and actor lifecycle

Clarify how reaching a top-level final state differs from explicit `ta_actor_destroy()`, when final data is returned to the host, and how a child actor generates `done.invoke.<id>` before eventual reclamation.

## Timer dispatch API

The host owns the durable deadline index for evicted actors. The public
host-facing mechanism for observing resident delayed sends and routing due work
needs a clear API that does not create an event loop inside Tinyactors.

## Resident layout

`ta_actor` currently contains generic vector pointers/counts for queues/invocations. This is suitable as a skeleton but not the final density target. Move uncommon data to sparse pools/side tables or definition-specialized slabs after semantics stabilize.

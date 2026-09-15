# Current header skeleton: gaps against intended design

This file is intentionally candid. The headers compile, but they do not yet expose every accepted design decision.

## Public registration/build surface

`include/tinyactors.h` currently exposes system creation and actor operations, but definition registration, builder operations, I/O registration, invoker registration, and store configuration are internal declarations. Decide which are public embedder APIs and surface them without requiring `src/internal.h`.

## Full definition semantics

`ta_definition` / builder still need representation for:

- explicit initial transitions and initial executable content;
- transition type (`internal` / `external`);
- multiple event descriptors and universal wildcard;
- onentry/onexit action ranges;
- executable-content IR;
- raise/send/cancel actions;
- conditional/foreach structures;
- invocation declarations, finalize blocks, and autoforward;
- data declarations and early/late binding metadata;
- extension executable actions.

## Data-model surface

The current vtable lacks some full-SCXML operations discussed earlier, including iteration/content import and explicit data initialization hooks. Add only the semantic operations required by the compiled action/data IR; do not copy XML-shaped APIs unnecessarily.

## Send representation

`ta_send` / `ta_delayed_send` need enough information to select the intended I/O processor/type. The final design should not depend on reconstructing this from transient interpreter context.

## Event patterns

`ta_transition_def.event_prefix` currently represents only one prefix. SCXML allows multiple descriptors and `*`. Compile a pattern set or equivalent efficient representation.

## Snapshot atomicity

The current API separately snapshots actor state and actor timers. The persistence contract must support one atomic logical snapshot/transaction even if a backend stores timers separately.

## Stable persisted identity

`ta_snapshot_header` currently contains runtime `ta_definition_id` and `ta_atom name`. Persistence must encode resolvable logical identity/revision rather than assume these table handles survive restart.

## Invocation restore

The invoker interface currently lacks explicit snapshot/rebind/restore semantics for live external services. Define how an invoker participates in unload/restart, or when it may refuse serialization.

## Completion and actor lifecycle

Clarify how reaching a top-level final state differs from explicit `ta_actor_destroy()`, when final data is returned to the host, and how a child actor generates `done.invoke.<id>` before eventual reclamation.

## Timer dispatch API

The timer heap primitives exist internally, but the public host-facing mechanism for observing/dispatching due work and querying the next deadline needs a clear API that does not create an event loop inside TA.

## Resident layout

`ta_actor` currently contains generic vector pointers/counts for queues/invocations. This is suitable as a skeleton but not the final density target. Move uncommon data to sparse pools/side tables or definition-specialized slabs after semantics stabilize.

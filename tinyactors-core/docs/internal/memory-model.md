# Memory model and four-million-actor target

The target is achieved by minimizing **per-actor** fixed cost, not by avoiding rich semantics.

## Share per definition

Never repeat these per actor:

- state/transition tables;
- strings and event patterns;
- ancestor/descendant relationships;
- configuration/history layout metadata;
- action/expression programs;
- data-model vtable/shared prepared program;
- I/O/invoker registries;
- allocator pointer.

## Specialize layouts

A sequential definition can store one active leaf. A small parallel definition can store a bitmap. History slots are sized only for history actually declared. Data-model instance size comes from the model.

## Keep uncommon features sparse

Actors with no active invocation should ideally pay no invocation-vector allocation. Idle actors do not need external mailboxes. Timers live in a shared deadline index. Large paused continuations may be side-allocated if the common continuation is small.

The resident representation enforces that boundary: one definition-sized slab
backs configuration and history, while internal queues, invocations, invoked-
child completion state, and replacement activation effects occupy an optional
`ta_actor_sparse_state`. An actor that does not use those features has no
vectors for them.

## Per-actor operations cost the same at every population

The target is a population, so no operation on one actor may walk the others
(invariant 17). Slots in the actor table are found through a free list, not by
scanning for a hole: a scan is invisible in a unit test and fatal in
production, where short-lived actors are spawned and destroyed against a large
resident population and would each pay for all of it.

`tinyactors-test/tests/scale_test.c` measures spawn, upgrade, and ephemeral
churn at two populations and fails when per-actor cost grows between them. It
also reports the target itself: four million resident named actors of a null
data-model chart cost 558 bytes each, 2.1 GB in all, and a rolling upgrade of
all of them takes 1.6 s of CPU time.

## Residency

Named durable actors may exist while not resident. The actor directory/name index is distinct from the resident execution cache. The exact directory representation is an implementation concern and may itself be store-backed for populations larger than RAM.

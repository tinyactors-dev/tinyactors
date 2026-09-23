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

The resident representation enforces that boundary: an actor is one
definition-sized allocation, laid out when the definition is compiled: the
actor record, then the session storage (configuration, late-binding bits and
history), then the data-model instance (ADR 0063). Invocations, invoked-child
completion state, replacement activation effects, pending delayed sends and a
paused continuation occupy an optional `ta_actor_sparse_state`. An actor that
does not use those features has no vectors for them.

The internal event queue is not per actor (ADR 0064). A run is lent one: the
actor's own if it kept one, else one from the system's cache of four empty
queues, else a new allocation, always before its first microstep is visible.
When the run ends with the queue empty and no microstep paused, the queue
goes back to the cache, or is freed when the cache is full or the queue grew
past 64 events. An actor keeps a queue only while events are queued in it or
a microstep is paused. So once a system is warm, an event that moves an
actor and returns it to stable makes no allocator call, and an actor that
has never run costs one null pointer for its queue.

## Per-actor operations cost the same at every population

The target is a population, so no operation on one actor may walk the others
(invariant 17). Slots in the actor table are found through a free list, not by
scanning for a hole: a scan is invisible in a unit test and fatal in
production, where short-lived actors are spawned and destroyed against a large
resident population and would each pay for all of it.

The table itself is paged (ADR 0063): slots live in fixed pages of 4,096,
found through a small directory of page pointers. A page is allocated only when
the last one is full and never moves, so no spawn copies the table. The largest
block a spawn allocates is one 64 KiB page at any population; before paging,
the spawn that crossed 2,097,152 actors copied a 32 MB table.

`tinyactors-test/tests/scale_test.c` measures spawn, upgrade, and ephemeral
churn at populations up to four million, sized to the memory available, and
fails when per-actor cost, or the largest block a spawn allocates, grows from
one population to the next. It
also reports the target itself: four million resident named actors of a null
data-model chart cost 558 bytes each, 2.1 GB in all, and a rolling upgrade of
all of them takes 1.6 s of CPU time.

## Residency

Named durable actors may exist while not resident. The actor directory/name index is distinct from the resident execution cache. The exact directory representation is an implementation concern and may itself be store-backed for populations larger than RAM.

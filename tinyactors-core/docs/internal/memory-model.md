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

## Residency

Named durable actors may exist while not resident. The actor directory/name index is distinct from the resident execution cache. The exact directory representation is an implementation concern and may itself be store-backed for populations larger than RAM.

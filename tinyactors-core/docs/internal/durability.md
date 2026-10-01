# Optional durability layer

Tinyactors core provides named actor/session identity, logical addressing, inspection, replay of recorded inputs, and checkpoints of actor state (ADRs 0089–0094). It deliberately does not impose one durable-execution protocol on every embedder.

Session images and unloading are not part of durability. They bound memory within one process, and an image is valid only in the process that wrote it (ADR 0074).

A reusable durability component may add:

- an encoding of checkpoints of actor state (`ta_actor_export_state`);
- stable resolution of host values referenced by events, timers, and continuations;
- storage transactions and checkpoint publication;
- durable input/command journals;
- deterministic replay of recorded inputs/results;
- stable I/O/effect identifiers and deduplication;
- deadline indexes for actors that are not resident;
- crash recovery and reconciliation;
- orchestration of actor-definition replacement/upgrades.

Passivation, unloading an idle actor to save memory, is not this layer's job:
the host keeps the image for the life of the process, and indexes
`ta_session_image_next_deadline()` in memory. What this layer stores for an
actor is its data, its recorded inputs, and checkpoints of its state.

## Huge data models

The durability layer should not assume checkpointing means serializing the host data-model graph. The application may already store a conversation transcript in a database, object graph, append-only log, or persistent structure. A checkpoint needs only enough information to re-resolve the binding and any host values directly referenced by interpreter state.

## I/O command/result journaling

For external effects, the useful durable record is typically the command identity and its observed completion/result, not an invisible transaction around the whole chart. For example:

```text
E17 db.persist(subscription)  -> committed
E18 queue.publish(changed)    -> pending
```

The chart can be in `DatabaseCommitted` while `E18` is pending. If a crash occurs after the DB commit but before its completion event is observed, the durability layer can retry/reconcile `E17` using the same stable ID and then deliver the completion. This makes the real partial-commit state explicit and testable.

## Replay boundary

Command sourcing/replay requires nondeterministic inputs—time, randomness, HTTP/LLM/database reads, external callbacks—to be recorded or supplied through deterministic boundaries. That requirement belongs to the durability component; plain Tinyactors core remains usable with ordinary synchronous host callbacks.

## Invoked services

Core reconstruction reports every formerly active invocation as
`error.communication.invoke.<id>.unavailable`; it does not rebind the old service
handle. A durability component may retain stable external operation identities
and results so the chart's error transition can reconcile or retry the service
without duplicating an already completed effect. This recovery is explicit
statechart protocol, not restoration of an opaque core handle.

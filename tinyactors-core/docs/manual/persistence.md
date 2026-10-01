# Session images and durability

Tinyactors core separates **reconstruction** from **durability**.

> **Unloading bounds memory; it is not persistence. An image is valid only in
> the process that wrote it.** Session images, `ta_actor_offload()`,
> `ta_actor_unload()` and `ta_actor_load()` exist so that one process can hold
> far more named actors (say, 100 million) than fit in memory, as long as the
> actors that are active at once do fit. Nothing written by them survives the
> process (ADR 0074). Surviving a restart is a different mechanism:
> [Surviving a restart](#surviving-a-restart).

`ta_actor_capture()` creates an in-memory session image containing Tinyactors-owned interpreter state. `ta_actor_reconstruct()` recreates that session against the exact same immutable definition revision and a host-supplied data-model binding. The image does not serialize the host object graph.

For a Python application, the binding may be an existing `Subscription` or `Conversation` object. A conversation containing hundreds of thousands of messages therefore remains where the application already keeps it; Tinyactors captures only its own interpreter state and any host references held directly by interpreter-owned events/timers/continuations.

Opaque handles for active invoked services are not rebound. Reconstruction
succeeds, retires each such invocation, and queues
`error.communication.invoke.<id>.unavailable` so the chart can retry,
compensate, or move to a failure state. Transitions listening to the broader
`error.communication` or `error` prefixes also match this event.

## Unloading named actors bounds memory

For a named virtual actor, capture and reconstruction are wrapped by the public
residency protocol:

1. fence the logical address against concurrent routing;
2. capture the resident actor (or call `ta_actor_offload()`, which captures
   and unloads in one step and writes the image as bytes in this process);
3. keep the image, the binding locator and the image's earliest deadline
   wherever the host likes, in memory or on local disk, for the life of the
   process;
4. call `ta_actor_unload()` with that fresh image;
5. on new input or a due deadline, resolve the binding;
6. call `ta_actor_load()` and then deliver or dispatch the triggering work.

The directory address remains claimed as `UNLOADED` between steps 4 and 6. The
old runtime actor ID is invalid after unload; load returns a new one. See
`virtual-actors.md` for the complete host call sequence.

When the process ends, every image ends with it, and so does every unloaded
actor that was not also made durable by the mechanisms below. Writing images
out at exit is not durability either: it does not scale, and a crash skips it
(ADR 0074, "Durability is not this ADR's job").

## Surviving a restart

What survives the process is the application's own storage, never an image:

- **The actor's data lives where the application keeps it,** and the binding
  locator names it. A new process spawns the actor again against that data.
- **Its inputs can be recorded and replayed.** The host keeps an actor's inputs
  in its own log, and rebuilds the actor by replaying them into an actor
  prepared with `at` before publishing it (ADRs 0089, 0094).
- **A checkpoint shortens the replay.** `ta_actor_export_state()` gives an idle
  actor's state at a record of its log, as structure the host encodes and
  stores; `ta_actor_prepare()` restores it (ADR 0091). A checkpoint in this
  sense is a persistence format. An unload image is not.

A durability module may add more above core: stable identities for host
references, command journals and stable IDs for external effects. Applications
that do not need these guarantees do not pay for them in the core ABI.

## External I/O and partial commits

Tinyactors does not pretend unrelated external effects are atomic. A chart can explicitly model:

```text
Persisting
  --db.committed--> DatabaseCommitted
  --publish-->      WaitingForPublish
  --published-->    Done
```

`DatabaseCommitted` is useful information: the database write happened while queue publication has not yet completed. An optional durability layer can record the I/O command and completion with a stable operation ID, so after a crash it can reconcile or retry without hiding the intermediate state from the chart.

## Definition upgrades

Reconstruction uses the same definition revision. Upgrading a named actor uses replacement instead:

1. fence the old named route;
2. create a new interpreter under the new definition, normally bound to the same host data-model object;
3. bootstrap it without externally visible effects;
4. atomically replace the directory entry;
5. retire the old interpreter.

Once no new actor should start on the old revision, retire it with
`ta_system_retire_definition()`: incumbents keep running, and the core frees
the old revision with its last resident actor (ADR 0043). Unloaded session
images do not keep a definition alive; they name its exact identity and need
it registered again to be reconstructed.

No translation of definition-local state IDs is required.

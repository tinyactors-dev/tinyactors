# Session images and durability

Tinyactors core separates **reconstruction** from **durability**.

`ta_actor_capture()` creates an in-memory session image containing Tinyactors-owned interpreter state. `ta_actor_reconstruct()` recreates that session against the exact same immutable definition revision and a host-supplied data-model binding. The image does not serialize the host object graph.

For a Python application, the binding may be an existing `Subscription` or `Conversation` object. A conversation containing hundreds of thousands of messages therefore remains where the application already keeps it; Tinyactors captures only its own interpreter state and any host references held directly by interpreter-owned events/timers/continuations.

Opaque handles for active invoked services are not rebound. Reconstruction
succeeds, retires each such invocation, and queues
`error.communication.invoke.<id>.unavailable` so the chart can retry,
compensate, or move to a failure state. Transitions listening to the broader
`error.communication` or `error` prefixes also match this event.

A process-independent checkpoint is a higher-level concern. A durability module can encode a session image, map host references to stable identities, store checkpoints, keep command/input journals, replay deterministic inputs, and assign stable IDs to external effects. Applications that do not need these guarantees do not pay for them in the core ABI.

## Restoring into another system

An image is not tied to the system that captured it. Any system in the same
process that registers the exact definition revision can reconstruct or load
it, even after the capturing system is destroyed. This is how a debugger forks
a production actor into a system of its own, whose clock it can move and whose
I/O processors are stubs:

```c
ta_session_image *image = NULL;
ta_actor_capture(production, &(ta_actor_capture_params){ .actor = actor, .out = &image });

ta_actor_id fork = 0;
ta_actor_reconstruct(debug, &(ta_actor_reconstruct_params){
    .image = image,
    .datamodel_binding = &copy_of_root, /* the fork's own data */
    .out = &fork,
});
ta_session_image_destroy(image);

ta_system_set_time(debug, ta_system_time(debug) + 3 * 24 * 3600 * 1000);
/* dispatch and resume in `debug`: the fork's timers fire, production's do not */
```

In the other system:

- event names, send IDs, invocation IDs and I/O types are interned there, so
  the copy reacts to the same events and `<cancel>` finds its sends;
- the copy is a new session, with its own session ID;
- a send the copy makes to another actor of the capturing system, by runtime
  or session ID, reaches nothing: it fails with `error.communication`, as a
  send to a destroyed actor does. Sends to named addresses resolve through
  the other system's directory, and an invoked child has no parent there;
- invocations that were active are lost, as on every restore.

The image's host values (event and send data) are handed to the other system
through the params' `adopter`. A host whose values belong to one system gives
an adopter that copies them; a host whose values outlive its systems leaves it
zero, and core retains them. Bind the copy to its own data-model root, or it
shares the original's.

`ta_actor_unload()` refuses an image captured in another system with
`TA_ERR_STALE`.

## Unloading named actors

For a named virtual actor, capture and reconstruction are wrapped by the public
residency protocol:

1. fence the logical address against concurrent routing;
2. capture the resident actor;
3. persist the image, binding locator, and earliest image deadline;
4. call `ta_actor_unload()` with that fresh image;
5. discard the in-memory image after the durable checkpoint is committed;
6. on new input or a due deadline, decode the image and resolve the binding;
7. call `ta_actor_load()` and then deliver or dispatch the triggering work.

The directory address remains claimed as `UNLOADED` between steps 4 and 7. The
old runtime actor ID is invalid after unload; load returns a new one. See
`virtual-actors.md` for the complete host call sequence.

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

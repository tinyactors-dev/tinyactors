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

No translation of definition-local state IDs is required.

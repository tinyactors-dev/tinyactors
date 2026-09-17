# ADR 0038: Session images separate interpreter state from host data

- Status: Accepted
- Date: 2026-09-16

## Context

Tinyactors needs three properties that were previously conflated under "snapshots":

1. operators must be able to inspect every resident interpreter instance;
2. the interpreter must be reconstructible from its own semantic state;
3. applications may optionally require crash-safe persistence, replay, command sourcing, and upgrade orchestration.

The previous snapshot design made the third requirement part of the core ABI. In particular it required data-model plugins and every `ta_value` to serialize. That is a poor fit for language bindings: a CPython host already owns the `PyObject *` graph, CRuby already owns its `VALUE`s, and Node already owns JavaScript objects. It is also pathological for very large models such as an LLM conversation transcript; capturing interpreter state must not copy gigabytes of application data.

At the same time, named actor identity and logical addressing are core statechart/session semantics. They cannot be moved into a persistence add-on merely because storage is optional.

## Decision

### Core owns interpreter sessions completely

Tinyactors owns and exposes the semantic state required to understand and continue an interpreter session: logical actor/session identity, exact definition revision, active configuration, history, paused continuation, current/internal events, active invocations, and delayed sends.

Every resident session is operationally inspectable. Inspection exposes the active state set and other Tinyactors-owned structures and asks the data-model adapter for a host-native root view of the bound data model.

### Session images are structural, not persistence blobs

`ta_actor_capture()` produces an owned `ta_session_image`. The image is an in-memory representation of Tinyactors-owned interpreter state, not a wire format. `ta_actor_reconstruct()` rebuilds that interpreter state from the image.

A session image does **not** contain or serialize the host data-model object graph. Reconstruction receives a host-supplied data-model binding. In CPython this can be the existing Python object; in CRuby the existing Ruby object; in JavaScript the existing host object/reference. A large conversation transcript therefore does not become part of every session image.

Interpreter-owned event/timer/continuation values may still reference host values. An in-process image retains those references using ordinary `ta_value` lifetime rules. Crossing a process boundary requires a higher-level durability mechanism to encode or resolve such references.

Opaque invoker service handles are runtime resources, not retained host values.
An image records the logical active invocation, but reconstruction retires it
and appends `error.communication.invoke.<id>.unavailable` to the internal event queue.
The chart therefore observes loss of the external service through normal SCXML
error control flow while the reconstruction API operation succeeds.

### Reconstruction is exact; upgrades are replacement

A session image is tied to one immutable definition identity: definition family plus exact opaque revision fingerprint. Core reconstruction never silently interprets definition-local IDs against a different revision.

Changing a chart definition is not reconstruction. To upgrade a named actor, create a new actor under the new definition, normally bound to the same host data-model object, bootstrap it, atomically replace the logical directory entry, and retire the old interpreter. The charts need not share state IDs, hierarchy, or data layout.

### Durability is a separate module

Portable checkpoint encoding, storage transactions, command/input journals, replay, effect IDs, deduplication, and crash recovery are optional durability concerns. Such a module consumes core session images and supplies stable encodings/rebinding rules for host references. It may choose to checkpoint only stable macrostep boundaries even though core capture/reconstruction can represent paused continuations.

The durability module may command-source chart/datamodel changes or I/O requests/results. This must not turn ordinary core execution into a mandatory event log or universal outbox protocol.

### I/O remains explicit statechart protocol

I/O processors remain the boundary for external side effects. A chart may deliberately model states such as `DatabaseCommitted` followed by `WaitingForPublish`. This makes real partial-commit conditions visible instead of pretending a database commit and queue publish are atomic.

A durability module can assign stable operation IDs and record I/O completions so a crash after the database commit but before the completion event can safely reconcile/retry. The statechart remains the business protocol; durability machinery supports it rather than hiding it.

## Consequences

- Remove mandatory `snapshot`/`restore` operations from the data-model vtable.
- Remove mandatory serialization from `ta_value`.
- Add a data-model inspection root and a host binding supplied when a data-model instance is initialized/reconstructed.
- Replace byte-oriented snapshot reader/writer APIs with opaque session-image capture/reconstruct APIs.
- Restore exact definition revision fingerprints; remove session-schema compatibility from core reconstruction.
- Keep named actors, logical addresses, directory replacement, inspection, and session reconstruction in the core.
- Very large data models do not cause session-image write amplification; their storage/lifetime remains the host application's concern.
- Portable durability remains reusable and standardized, but can evolve as a separate component for applications that need it.

## Supersedes/refines

This ADR supersedes ADR 0013, the serialization portions of ADRs 0005 and 0006, and the family/session-schema reconstruction rule introduced by ADR 0037. It reinstates the exact-revision identity principle from ADR 0036.

# tinyactors-core 0.5.0

### Summary

This release is about the services a chart invokes: an `<invoke>` of a type the application runs, such as an HTTP request in flight, a payment or a shipment that takes a week. Such an actor used to stay on its old version through a migration and in memory under a resident bound. Now it does neither.

A migration moves an actor whose service is running at once, and the service goes on (ff43f15). Each external invocation has an id of its own, unique in the host and kept when its actor moves to another segment; what the service sends reaches the new version, as do the delayed sends to it. A service goes on when its state is kept and still has an `<invoke>` of the same type and id; any other is stopped. Invoked charts still hold their actor until they end.

An actor hibernates with its services running (52f7f6b): the services go on while it sleeps, and what they send asks for its wake. A host restored from a snapshot reconciles a newer blob's services with its own. An actor that faults stops its services. A request costs 57 B of the region, 8 B of it the index of running services.

The `tinyactors` command's help is written in one voice, and a command lists the exit statuses it has beyond a usage error (1601ebe). Snapshots and hibernation blobs of earlier builds are refused.

### Upgrading

- `ta_event_in` replaces its `token` with `invocation`, a 64-bit id, and loses its `pad`: answer a service with `TA_EVENT_IN_FROM_INVOKE`, `TA_EVENT_IN_INVOKE_DONE` or a failure by setting `invocation` to the id of its INVOKE effect. `trace_flags` moved to where `token` was, so an initializer by position must be rewritten; one by name only renames the field (ff43f15).
- `ta_effect` and `ta_host_effect` gain `invocation`, read through the new `TA_EFFECT_FIELD_INVOCATION`, on an INVOKE, an UNINVOKE and a SEND to a service. Key a service by that id alone, no longer by its segment and token: the id stays the same when a migration moves its actor. `TA_EFFECT_FIELD_TOKEN` and `ta_effect.token` are now the token of a REQUEST only. Both structs are larger: recompile (ff43f15).
- `ta_actor_info` turns its `pad` into `services`, the actor's external invocations (ff43f15).
- `tinyactors run` takes and gives `invocation` as that id, up to 2^64-1 (ff43f15).
- An actor whose services run is now migrated at once instead of failing with `TA_ERROR_STATE` or waiting for the timeout. Expect an UNINVOKE after it moves for each service the new version does not keep, and new INVOKE effects when a kept state's services were all stopped (ff43f15).
- An actor whose services run can now be hibernated, and the events of its services ask for its wake. An actor that faults now stops its services with an UNINVOKE each. A host restored from a snapshot, given a blob of a later hibernation, stops a service the blob no longer names and gives the actor error.communication for one it names that the host lacks (52f7f6b).
- An external `<invoke>` gets error.execution once its segment has handed out every id of its range (ff43f15).
- The `tinyactors` command's help in JSON and XML gives an example as a caption and a command instead of a string, and a command an exit status list (help and version have none) (1601ebe).
- Snapshots and hibernation blobs of earlier builds are refused: take new ones with this build (ff43f15, 52f7f6b).

### Breaking changes

- a migration carries an actor's services along, each under an id of its own (ff43f15)

### API changes

**Added**

- constant `TA_EFFECT_FIELD_INVOCATION`

**Changed**

- struct `ta_actor_info`
- struct `ta_effect`
- struct `ta_event_in`
- struct `ta_host_effect`

### Changes

**Added**

- an actor hibernates with its services running (52f7f6b)
- tinyactors help in one voice: imperative summaries, captioned examples, exit statuses (1601ebe)

**Fixed**

- test_otlp's fixed request at 0.4.0's scope version (76f99af)

<details>
<summary>Internal (2)</summary>

- hibernate and wake rows say what they do with an actor's services (3b03ce8)
- a migration keeps a running service whose <invoke> the new version keeps (fad7314)

</details>

# Persistence format requirements

No concrete wire format is fixed yet, but any format must satisfy these requirements.

## Versioning

Each checkpoint has a format version. Its decoded header contains a
`ta_definition_identity`: UTF-8 definition family plus session schema. The
schema changes only when a saved session needs transformation; a logic/build
change may keep the schema compatible. It must not depend on pointers, runtime
handles, registration order, or callback addresses.

## Stable encoding

Do not persist raw pointers or native C structs. Do not assume runtime table
handles (`ta_atom`, `ta_event_name_id`, `ta_definition_id`, or `ta_actor_id`)
retain meaning after restart. Encode the definition family, session schema,
named self address, and event-name segments as logical data.

## Values

Value/data-model serializers own the encoding of host-native data. A snapshot is invalid if referenced values cannot be restored with equivalent semantics.

## Timers

Persist absolute logical due time plus fully materialized send semantics. Restoring an overdue timer makes it due; restoration itself should not unexpectedly call external I/O.

## Invocations

Persist invocation identity and interpreter-visible state. Host service handles are not assumed portable. Each invoker may need a restore/rebind contract or may explicitly declare that actors with live invocations cannot be unloaded across process restart.

## Definition upgrades

The system resolves a checkpoint's definition family before decoding
definition-local IDs and verifies that the target accepts its session schema.
A missing family returns `TA_ERR_NOT_FOUND`; an unsupported schema returns
`TA_ERR_SESSION_SCHEMA`. Applications migrate incompatible definitions by
replacing an actor with a newly bootstrapped actor under the target definition.

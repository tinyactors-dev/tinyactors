# Persistence format requirements

No concrete wire format is fixed yet, but any format must satisfy these requirements.

## Versioning

Each snapshot has a format version. Definition identity must include enough stable information to reject or deliberately migrate an incompatible definition revision.

## Stable encoding

Do not persist raw pointers. Do not assume runtime table handles (`ta_atom`, `ta_event_name_id`, `ta_definition_id`) retain meaning after restart. Encode logical names/segments or a versioned stable dictionary.

## Values

Value/data-model serializers own the encoding of host-native data. A snapshot is invalid if referenced values cannot be restored with equivalent semantics.

## Timers

Persist absolute logical due time plus fully materialized send semantics. Restoring an overdue timer makes it due; restoration itself should not unexpectedly call external I/O.

## Invocations

Persist invocation identity and interpreter-visible state. Host service handles are not assumed portable. Each invoker may need a restore/rebind contract or may explicitly declare that actors with live invocations cannot be unloaded across process restart.

## Definition upgrades

Automatic statechart migration is not designed yet. Stores should at least detect definition mismatch rather than restoring bytes into an incompatible layout.

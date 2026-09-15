# Public API intent

The current `include/tinyactors.h` is a compact public skeleton. This page records intended semantics of the exposed surface, not a promise that the signatures are final.

## Scalar handles

- `ta_atom` — interned ordinary string handle, process-local unless explicitly persisted through a logical encoding.
- `ta_event_name_id` — structured event-name handle.
- `ta_definition_id` — compact runtime definition handle.
- `ta_actor_id` — actor runtime identity; may internally encode slot + generation.
- `ta_invoke_id`, `ta_send_id`, `ta_timer_id` — identities scoped by their semantic subsystem.
- `ta_time` — caller-supplied monotonic logical time.

## Results

`ta_result` reports C/runtime/API success or failure. Statechart-visible execution/communication failures normally become queued `error.*` events instead.

## System

`ta_system_create` / `ta_system_destroy` create the shared in-process container. Creation does not start threads or I/O.

`ta_system_set_time` advances logical time. `ta_system_time` reads the current logical value.

The public API still needs registration calls for definitions, I/O processors, invokers, and persistence adapters.

## Event names

`ta_system_intern_atom` interns one reusable name segment. Its resulting atom
belongs to that system and must not be used with another system.

`ta_event_name_intern` interns an event name from one ordered `ta_atom_list`.
It is the only event-name construction API; string parsing and incremental
append APIs are deliberately not exposed. The list must be non-empty and every
atom must belong to the supplied system; otherwise it returns `TA_ERR_INVALID`.

`ta_event_name_matches` performs SCXML token-prefix matching.

`ta_event_name_format` materializes a dotted string for diagnostics/host boundaries.

## Actors

`ta_actor_spawn` creates a session from a definition. Empty name means anonymous; non-empty names are unique within the definition namespace.

`ta_actor_find` resolves a named actor.

`ta_actor_deliver` begins delivery of one external event to an actor that is ready for a new macrostep. It does not imply an unbounded external mailbox.

`ta_actor_resume` advances the in-progress macrostep for a caller-supplied budget and reports stable/paused/done/error execution status.

`ta_actor_unload` persists/evicts the actor without terminating it.

`ta_actor_load` materializes an unloaded actor.

`ta_actor_destroy` terminates/removes the actor and its actor-owned semantic resources.

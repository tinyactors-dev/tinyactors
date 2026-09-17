# Public API intent

The current `include/tinyactors.h` is a compact public skeleton. This page records intended semantics of the exposed surface, not a promise that the signatures are final.

## Scalar handles

- `ta_atom` — interned ordinary string handle, process-local unless resolved through logical text by a higher layer.
- `ta_event_name_id` — structured event-name handle.
- `ta_definition_id` — compact runtime definition handle.
- `ta_definition_identity` — logical definition family plus exact opaque revision fingerprint.
- `ta_actor_id` — runtime-local actor identity; may internally encode slot + generation.
- `ta_actor_address` — logical named actor address scoped by definition family.
- `ta_invoke_id`, `ta_send_id`, `ta_timer_id` — identities scoped by their semantic subsystem.
- `ta_time` — caller-supplied monotonic logical time.

## Results

`ta_result` reports C/runtime/API success or failure. Statechart-visible execution/communication failures normally become queued `error.*` events instead.

`TA_ERR_DEFINITION_MISMATCH` means reconstruction found the definition family but not the exact image revision. `TA_ERR_NOT_RECONSTRUCTIBLE` is reserved for non-invocation interpreter state that cannot be captured or recreated under the configured adapters. Lost active invocations are reported to the chart as `error.communication.invoke.<id>.unavailable`, not as a reconstruction API failure. The event does not expose why the service became unavailable.

## System

`ta_system_create` / `ta_system_destroy` create the shared in-process container. Creation does not start threads or I/O.

`ta_system_set_time` advances logical time. `ta_system_time` reads the current logical value.

`ta_system_set_actor_directory` installs host-provided operations for named actor claim, resolution, replacement, and removal. The named-address protocol belongs to Tinyactors; the host owns the backing directory implementation, physical routing, synchronization, and any durable storage.

The public API still needs registration calls for definitions, I/O processors, and invokers.

## Event names

`ta_system_intern_atom` interns one reusable name segment. Its resulting atom belongs to that system and must not be used with another system.

`ta_event_name_intern` interns an event name from one ordered `ta_atom_list`. It is the only event-name construction API; string parsing and incremental append APIs are deliberately not exposed. The list must be non-empty and every atom must belong to the supplied system; otherwise it returns `TA_ERR_INVALID`.

`ta_event_name_matches` performs SCXML token-prefix matching.

`ta_event_name_format` materializes a dotted string for diagnostics/host boundaries.

## Actors

`ta_actor_spawn` creates a session from an exact definition. `datamodel_binding` is a host-owned value passed to the selected data-model adapter; Tinyactors does not copy its object graph. A null address creates an ephemeral actor; a supplied logical address is claimed through the host directory.

`ta_actor_find` resolves a named actor.

`ta_actor_deliver` begins delivery of one external event to an actor that is ready for a new macrostep. It does not imply an unbounded external mailbox.

`ta_actor_report_invoke` is the asynchronous return path for an invoked service.
It reports an ordinary child event, normal completion, or terminal service
unavailability. Ordinary and completion reports use the actor's external-event
flow and may return `TA_ERR_BUSY`; an unavailable report retires the invocation
and appends a platform error to the internal queue. Reports for stale invocation
IDs return `TA_ERR_NOT_FOUND` without generating another event. The start
request supplies the owning system pointer as well as the system-local actor and
invocation IDs needed for later reports.

`ta_actor_resume` advances the in-progress macrostep for a caller-supplied budget and reports stable/paused/done/error execution status.

`ta_actor_inspect` returns a read-only operational view of one resident interpreter. `ta_system_visit_actors` enumerates resident interpreter instances. The data-model root remains a host value supplied by the adapter.

`ta_actor_capture` creates an opaque in-memory `ta_session_image`. `ta_actor_reconstruct` recreates that same interpreter session from the image, the exact definition revision, and a host-supplied data-model binding. Opaque invoker service handles are runtime-only: reconstruction retires each captured active invocation and appends `error.communication.invoke.<id>.unavailable` after the image's existing internal events. Neither operation defines a persistence wire format.

`ta_actor_destroy` terminates/removes the actor and its actor-owned semantic resources.

## Durability boundary

Portable checkpoint encoding, storage transactions, command/input journals, replay, effect IDs/deduplication, and durable deadline indexes are intentionally not core APIs. A reusable durability component may build them over session images and the core named-actor protocol.

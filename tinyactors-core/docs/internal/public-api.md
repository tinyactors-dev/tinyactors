# Public API intent

`include/tinyactors.h` is the complete, authored public API (ADR 0041). This page records intended semantics of the exposed surface, not a promise that the signatures are final.

## Scalar handles

- `ta_atom` — interned ordinary string handle, process-local unless resolved through logical text by a higher layer.
- `ta_event_name_id` — structured event-name handle.
- `ta_definition_id` — runtime definition handle: slot in the low 32 bits, the slot's generation in the high 32 bits (ADR 0043). A freed definition's ID goes stale and never names another definition; zero is never valid.
- `ta_definition_identity` — logical definition family plus exact opaque revision fingerprint.
- `ta_actor_id` — runtime-local actor identity; may internally encode slot + generation.
- `ta_actor_address` — logical named actor address scoped by definition family.
- `ta_invoke_id`, `ta_send_id`, `ta_timer_id` — identities scoped by their semantic subsystem.
- `ta_time` — caller-supplied monotonic logical time, in milliseconds.

## Results

`ta_result` reports C/runtime/API success or failure. Statechart-visible execution/communication failures normally become queued `error.*` events instead.

`TA_ERR_DEFINITION_MISMATCH` means reconstruction found the definition family but not the exact image revision. `TA_ERR_NOT_RECONSTRUCTIBLE` is reserved for non-invocation interpreter state that cannot be captured or recreated under the configured adapters. Lost active invocations are reported to the chart as `error.communication.invoke.<id>.unavailable`, not as a reconstruction API failure. The event does not expose why the service became unavailable.

`TA_ERR_UNLOADED` means a logical named actor exists but currently has no
resident runtime ID. `TA_ERR_STALE` means a compare-and-swap directory update or
an unload image no longer describes the expected actor state. Neither result
changes the actor.

## System

`ta_system_create` / `ta_system_destroy` create the shared in-process container. Creation does not start threads or I/O.

`ta_system_set_time` advances logical time. `ta_system_time` reads the current logical value.

`ta_system_set_actor_directory` installs host-provided operations for named actor claim, resolution, replacement, and removal. The named-address protocol belongs to Tinyactors; the host owns the backing directory implementation, physical routing, synchronization, and any durable storage.

`ta_scxml_target_parse` and `ta_scxml_target_format` are the SCXML target URI
grammar (ADR 0044). Core parses both spellings of a `<send>` target with the
first and writes every URI it gives a chart, such as `$_event.origin`, with the
second, so the two are inverses. A named address borrows its text from the
input and stays valid while that input does.

`ta_send_id_parse` and `ta_send_id_format` are the send ID text, for the same
reason (ADR 0048). A chart sees a send ID as text — `idlocation` stores it,
`_event.sendid` carries it, `<cancel sendidexpr>` reads it — while a host
holds `ta_send.send_id`, so core renders one into the other rather than
letting each host re-derive `#_send.<n>`. Parsing the generated form always
succeeds; a literal ID must already be interned, otherwise `TA_ERR_NOT_FOUND`,
and text that is no send ID is `TA_ERR_INVALID`. Formatting follows the buffer
convention and answers `TA_ERR_NOT_FOUND` for a send with no ID.

`ta_system_set_external_event_router` installs the host scheduling boundary for
normalized external SCXML-session events. Core provides the mandatory SCXML
Event I/O Processor; it resolves standard targets and submits external delivery
through this router. Accepted deliveries have been copied if retained; retry
pauses and preserves the materialized delivery; error becomes statechart-visible
communication failure. `ta_system_scxml_io_type` returns the processor's
reserved type atom.

`ta_system_register_definition`, `ta_system_register_io`, and
`ta_system_register_invoker` install compiled charts and host adapters. A
successful definition registration transfers ownership to the system. Adapter
registrations are borrowed and must remain valid through system destruction.
The standard SCXML processor atom cannot be replaced with
`ta_system_register_io`.

`ta_system_set_log_sink` installs where `<log>` goes (ADR 0045). The record
carries the logging actor, the label and the evaluated value, borrowed for
the call; writing it returns nothing, because SCXML 4.7 forbids `<log>`
from affecting interpretation. Without a sink a `<log>` still evaluates its
expression, so its errors do not depend on whether a host is listening.

## Event names

`ta_system_intern_atom` interns one reusable name segment. Its resulting atom belongs to that system and must not be used with another system. `ta_system_atom_text` reads one back, borrowed from the system, so a host handed `ta_send.io_type` or `ta_invoke_request.type` can say what it was given (ADR 0049); an atom of another system is `TA_ERR_NOT_FOUND`.

`ta_event_name_intern` interns an event name from one ordered `ta_atom_list`. It is the only event-name construction API; string parsing and incremental append APIs are deliberately not exposed. The list must be non-empty and every atom must belong to the supplied system; otherwise it returns `TA_ERR_INVALID`.

`ta_event_name_matches` performs SCXML token-prefix matching.

`ta_event_name_format` materializes a dotted string for diagnostics/host boundaries.

## Actors

`ta_actor_spawn` creates a session from an exact definition. `datamodel_binding` is a host-owned value passed to the selected data-model adapter; Tinyactors does not copy its object graph. A null address creates an ephemeral actor; a supplied logical address is claimed through the host directory.

Bootstrap follows SCXML `interpret()`: declare all data, apply early initializers, run the definition's top-level `<script>` if any, then take the root initial transition. Reconstruction and `ta_actor_load` restore a bootstrapped session and never repeat these steps.

An optional `parent` marks the actor as an invoked child. Normal terminal
completion then becomes `done.invoke.<id>` in the parent's external-event flow.

`ta_actor_prepare_replacement` creates an unpublished replacement using the
incumbent named address and holds bootstrap effects behind an activation
barrier. `ta_actor_activate_replacement` requires that actor to be stable,
atomically replaces the directory route, and only then releases its effects.

`ta_actor_find` resolves a named actor.

If the directory entry is unloaded, `ta_actor_find` returns
`TA_ERR_UNLOADED` and leaves its output unchanged. The host obtains the
checkpoint and binding and calls `ta_actor_load` before retrying delivery.

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

`TA_RUN_DONE` leaves the terminal actor resident. `ta_actor_get_completion`
clones its optional final data. For an invoked child,
`ta_actor_publish_completion` retries a `done.invoke.<id>` delivery that could
not be accepted while the parent was busy. Explicit destruction is not normal
completion and never generates that event.

`ta_system_invoke_id_text` says which invocation a `ta_invoke_id` is — the author's `<invoke id>` or the generated `<stateid>.<platformid>` — so a host reading `ta_active_invoke.id` can name it (ADR 0049).

`ta_actor_inspect` returns a read-only operational view of one resident interpreter: identity, definition, residency, whether a macrostep is paused (and its external event), internal queue, invocations, pending delayed sends (a borrowed array sorted by due time, ADR 0054), completion, the count of active states, and `allocated_bytes`. `ta_system_visit_actors` enumerates resident interpreter instances: every resident actor once, invoked children included, with the view `ta_actor_inspect` would give; the walk stops at the first visit that does not answer `TA_OK` and returns that result, and a null `visit` is `TA_ERR_INVALID`. Its order is not part of the contract. Neither calls the data-model adapter. `ta_actor_datamodel_root` asks the adapter's `inspect` operation for the data-model root, a host value the caller releases; it returns `TA_ERR_UNSUPPORTED` when the adapter has none.

`ta_definition_inspect` returns fixed-size facts about a registered definition (ADR 0042): its ID, exact identity, `<scxml name>`, the data-model adapter it was compiled with (to identify, never to call), its binding mode, state count, lifecycle facts (`retired`, `actor_count`), and memory measurements (`actor_fixed_bytes`, `definition_bytes`). `ta_system_find_definition` resolves an exact identity (family and revision compared bytewise) to the most recently registered matching definition that is not retired. See [definition inspection](definition-inspection.md).

`ta_system_retire_definition` ends a definition's availability (ADR 0043): spawns and replacements naming it fail with `TA_ERR_NOT_FOUND` and `ta_system_find_definition` skips it, while resident actors keep running and `ta_definition_inspect`, `ta_definition_state`, and `ta_definition_export` keep working. The core frees the definition when it is retired and no resident actor uses it: at once, or after the last actor is fully torn down. Retiring is idempotent, also after the definition is freed. Every function that takes a `ta_definition_id` returns `TA_ERR_NOT_FOUND` for an unknown, stale, or freed ID; `ta_actor_spawn` also for a retired one.

`ta_actor_active_states` and `ta_actor_history` list the active configuration (ancestors included, document order) and one history pseudo-state's recorded states. They fill caller-supplied arrays and always report the total count; a too-small array yields `TA_ERR_NOMEM` without writing, so a zero capacity is a size query. `ta_definition_state` resolves a state ID of a registered definition to its author ID, parent, and kind, and `ta_definition_find_state` is its inverse (ADR 0049). Inside a data-model callback, `ta_dm_host_find_state` does the same for the running definition, which is what `In()` needs: core owns that mapping, so an adapter does not scan for it.

`ta_actor_capture` creates an opaque in-memory `ta_session_image`. `ta_actor_reconstruct` recreates that same interpreter session from the image, the exact definition revision, and a host-supplied data-model binding. Opaque invoker service handles are runtime-only: reconstruction retires each captured active invocation and appends `error.communication.invoke.<id>.unavailable` after the image's existing internal events. Neither operation defines a persistence wire format.

`ta_actor_unload` verifies that its image is a fresh capture of the named actor,
atomically changes the directory entry from resident to unloaded, removes
delayed sends from the resident deadline index, cancels runtime service handles,
and releases resident interpreter memory. It does not consume the image. The old
runtime actor ID becomes stale while the logical name remains alive.

`ta_actor_load` reconstructs a named image against its exact registered
definition and a host-supplied binding, then atomically changes the directory
entry from unloaded to resident. It returns a new runtime actor ID. On failure,
the directory remains unloaded and the caller retains the image.

`ta_actor_remove_unloaded` terminates a non-resident logical actor by removing
the expected unloaded directory entry. Core owns no resident resources at that
point; after success the host deletes its checkpoint, binding record, and
non-resident deadline entry.

`ta_session_image_next_deadline` exposes the earliest actor-owned delayed-send
deadline so a durability layer can index unloaded actors without depending on
the image's private layout.

`ta_actor_destroy` terminates/removes the actor and its actor-owned semantic resources.

`ta_system_next_deadline` and bounded `ta_system_dispatch_due` integrate
resident delayed sends with the host scheduler. They do not wait or read a
physical clock.

## Durability boundary

Portable checkpoint encoding, storage transactions, command/input journals, replay, effect IDs/deduplication, and durable deadline indexes are intentionally not core APIs. A reusable durability component may build them over session images and the core named-actor protocol.

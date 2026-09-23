# Inspection

Inspection is a core operational capability, not a persistence feature.

`ta_actor_inspect()` exposes a read-only view of one resident interpreter instance. `ta_system_visit_actors()` walks the resident actors so an embedder can build an admin endpoint, debugger, REPL, metrics collector, or process inspector without reverse-engineering private memory. It visits every resident actor once, invoked children included, with the same view `ta_actor_inspect()` gives for it, and stops at the first visit that answers anything but `TA_OK`, returning that result — so an inspector that has seen enough, or that cannot go on, ends the walk. The order is not part of the contract.

The view reports the logical actor reference, definition ID and exact definition identity, residency, whether a macrostep is paused mid-way and the external event it is processing, the internal queue, invocations, pending delayed sends, completion, the number of active states, and `allocated_bytes`, the memory the core currently holds for the actor. Actor inspection never calls the data-model adapter, so it is cheap and has no adapter side effects. `ta_actor_datamodel_root()` asks the adapter's `inspect` operation for the data-model root; it returns `TA_ERR_UNSUPPORTED` when the adapter has none.

The internal queue, the invocations, and the delayed sends are borrowed arrays in the view. Delayed sends are stored with their owner (ADR 0054), so `delayed_sends` lists them by due time, then send order, at no extra cost: send ID, due time, event, I/O type, resolved destination, target, data, and whether the router already refused the send once and it waits to be offered again (`submitted`). Inspecting one actor costs the size of that actor, never the size of the system.

The active configuration and history are read with their own list functions, so the view never points into private layouts:

| Function | Lists |
|---|---|
| `ta_actor_active_states()` | the active configuration, ancestors included, in document order |
| `ta_actor_history()` | the states one history pseudo-state recorded (empty until recorded) |

Each fills a caller-supplied array and always reports the total count. A too-small array returns `TA_ERR_NOMEM` without writing anything, so a zero capacity is a size query; the counts in the inspection view let a host size the array up front. `ta_definition_state()` resolves a state ID to the author's state ID, its parent, and its kind; `ta_definition_find_state()` goes the other way, from the author's ID to the state.

```c
ta_actor_inspection view;
ta_actor_inspect(system, actor, &view);
ta_state_id states[16];
uint32_t count = 0;
ta_actor_active_states(system, &(ta_actor_active_states_params){
    .actor = actor, .states = states, .capacity = 16, .count = &count });
for (uint32_t i = 0; i < count; i += 1) {
    ta_state_info info;
    ta_definition_state(system, &(ta_definition_state_params){
        .definition = view.definition_id, .state = states[i], .out = &info });
    /* info.id is the author's state ID */
}
ta_value root;
if (ta_actor_datamodel_root(system, actor, &root) == TA_OK) {
    /* ... */
    ta_value_release(&root);
}
```

The data-model root is a host value. For CPython it may simply wrap the existing `PyObject *`; for CRuby a `VALUE`; for Node a host reference. Tinyactors does not traverse or copy it. This matters for large models such as LLM conversation transcripts: inspecting the actor does not imply materializing or serializing the full transcript.

## Definitions

`ta_definition_inspect()` reports fixed-size facts about a registered definition without an actor:

| Field | Meaning |
|---|---|
| `id`, `identity` | the runtime ID and the exact revision (family and fingerprint) |
| `document_name` | the `<scxml name>` attribute; empty when absent |
| `datamodel` | the adapter instance the definition was compiled with, to identify it (for example to pick a value codec); never call its operations |
| `binding` | `TA_SCXML_BINDING_EARLY` or `TA_SCXML_BINDING_LATE` |
| `state_count` | states readable with `ta_definition_state()` |
| `retired`, `actor_count` | lifecycle: whether the definition is retired, and how many resident actors use it |
| `actor_fixed_bytes` | what each actor costs before it processes events: actor record, session storage, data-model instance |
| `definition_bytes` | the definition's own storage |

The byte counts are measurements and may change between releases. `ta_system_find_definition()` resolves an exact identity, for example one read from a stored session, to the most recently registered definition with that identity that is not retired.

### Retiring definitions

`ta_system_retire_definition()` stops new use of a definition: spawning it fails with `TA_ERR_NOT_FOUND` and `ta_system_find_definition()` no longer returns it. Actors already running it keep running, and the definition stays inspectable (`retired` is true) and exportable while they exist. The core frees it once no resident actor uses it — immediately if none does. After that its ID is stale: definition IDs carry a generation, so every call that takes the old ID returns `TA_ERR_NOT_FOUND` even after the slot is reused. Retiring twice is harmless. A host that caches definition IDs (for example an invoker that compiles child charts) should treat `TA_ERR_NOT_FOUND` as "compile and register again".

```c
ta_definition_inspection facts;
ta_definition_inspect(system, view.definition_id, &facts);
if (facts.datamodel.self == my_adapter.self) { /* encode values for my_adapter */ }
```

Inspection views are read-only and require the same external synchronization as other access to a `ta_system`. Borrowed core pointers and borrowed values in listed delayed sends remain valid only while the actor cannot mutate. A returned data-model root follows ordinary `ta_value` lifetime rules.

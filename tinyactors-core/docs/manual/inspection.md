# Inspection

Inspection is a core operational capability, not a persistence feature.

`ta_actor_inspect()` exposes a read-only view of one resident interpreter instance. `ta_system_visit_actors()` walks the resident actors so an embedder can build an admin endpoint, debugger, REPL, metrics collector, or process inspector without reverse-engineering private memory.

The view reports the logical actor reference, definition ID and exact definition identity, residency, whether a macrostep is paused mid-way and the external event it is processing, the internal queue, invocations, completion, the number of active states and delayed sends, and a data-model root when the selected data-model adapter implements `inspect`.

Variable-length state has its own list functions, so the view never points into private layouts:

| Function | Lists |
|---|---|
| `ta_actor_active_states()` | the active configuration, ancestors included, in document order |
| `ta_actor_history()` | the states one history pseudo-state recorded (empty until recorded) |
| `ta_actor_delayed_sends()` | pending delayed sends: send ID, due time, event, I/O type, target, and data |

Each fills a caller-supplied array and always reports the total count. A too-small array returns `TA_ERR_NOMEM` without writing anything, so a zero capacity is a size query; the counts in the inspection view let a host size the array up front. `ta_definition_state()` resolves a state ID to the author's state ID, its parent, and its kind.

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
if (view.has_datamodel_root) ta_value_release(&view.datamodel_root);
```

The data-model root is a host value. For CPython it may simply wrap the existing `PyObject *`; for CRuby a `VALUE`; for Node a host reference. Tinyactors does not traverse or copy it. This matters for large models such as LLM conversation transcripts: inspecting the actor does not imply materializing or serializing the full transcript.

Inspection views are read-only and require the same external synchronization as other access to a `ta_system`. Borrowed core pointers and borrowed values in listed delayed sends remain valid only while the actor cannot mutate. A returned data-model root follows ordinary `ta_value` lifetime rules.

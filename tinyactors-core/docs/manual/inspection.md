# Inspection

Inspection is a core operational capability, not a persistence feature.

`ta_actor_inspect()` exposes a read-only view of one resident interpreter instance. `ta_system_visit_actors()` walks the resident actors so an embedder can build an admin endpoint, debugger, REPL, metrics collector, or process inspector without reverse-engineering private memory.

The view includes the logical actor reference, exact definition identity, residency/run state, active configuration, history, continuation, internal queue, invocations, delayed-send count, and a data-model root when the selected data-model adapter implements `inspect`.

The data-model root is a host value. For CPython it may simply wrap the existing `PyObject *`; for CRuby a `VALUE`; for Node a host reference. Tinyactors does not traverse or copy it. This matters for large models such as LLM conversation transcripts: inspecting the actor does not imply materializing or serializing the full transcript.

Inspection views are read-only and require the same external synchronization as other access to a `ta_system`. Borrowed core pointers remain valid only while the actor cannot mutate. A returned data-model root follows ordinary `ta_value` lifetime rules.

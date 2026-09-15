# `event.h`

`ta_event` is the SCXML-visible event envelope:

- structured name;
- event type: external, internal, or platform;
- send ID where applicable;
- origin metadata;
- invoke ID where applicable;
- arbitrary `ta_value` data.

The event name and envelope metadata are transport/control information and should not be duplicated into application payloads.

`ta_queued_event` adds ownership/implementation flags. Queueing an event that outlives the caller requires cloning/retaining its data according to the value contract.

When a macrostep is paused, both the current external event and internal queued events are persistent actor state.

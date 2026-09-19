# System variables and the data-model host view

System variables are engine-owned semantic state exposed through the data model.

## `ta_dm_host`

The host view should provide semantic getters rather than requiring the engine to allocate a data-model object for every field.

Conceptual operations:

```c
bool ta_dm_host_in_state(const ta_dm_host *, ta_state_id);
const ta_event *ta_dm_host_event(const ta_dm_host *);
ta_actor_id ta_dm_host_actor(const ta_dm_host *);
ta_str ta_dm_host_definition_name(const ta_dm_host *);

ta_result ta_dm_host_io_processor_info(
    const ta_dm_host *, uint32_t index, ta_io_info *out);

ta_result ta_dm_host_platform_value(
    const ta_dm_host *, ta_atom name, ta_value *out);
```

The public API implements this as `ta_dm_host_text()`, which formats one
field (`_event.name`, `type`, `sendid`, `origin`, `origintype`, `invokeid`,
`_sessionid`, `_name`, or the SCXML Event I/O Processor's type and location)
into a caller buffer, and `ta_dm_host_state_name()` supports `In()`.
`make_params()` receives parameter names as text, so adapters need no atom
lookup.

While a microstep runs, `In()` and `ta_dm_host_in_state()` see the
interpreter's live active set, so a state already exited in this microstep
is inactive in later onexit handlers.

## Send IDs as text

A literal send ID reads as the author's text. A generated one (for
`idlocation`) reads as `#_send.<n>`, unique per session. Both round-trip
through `sendidexpr`, and `_event.sendid` shows the same text. A send with
neither `id` nor `idlocation` has no ID, so its events have a blank sendid.

## Current event

Before the first processed event, current event is unbound.

When an internal or external event begins processing, the core updates the host view. That event remains current throughout transition conditions, exits, transition actions, entries, and subsequent eventless microsteps until another event is processed.

Incoming event data may require conversion to a value legal in the receiving data model. `import_event_data()` performs that conversion. If conversion fails, the event remains processable but its data is unavailable and `error.execution` is queued.

## `In(state)`

`In` is resolved against the engine configuration, not a plugin-maintained copy. It therefore works correctly for parallel configurations and ancestor states.

## Protection

System variables are read-only from application code. A plugin must prevent assignment through its location language.

## `_ioprocessors`

Core always exposes an entry for the built-in SCXML Event I/O Processor URI.
Its location denotes the current actor's externally usable runtime or logical
address. Custom registered processors may add their own profile-specific
entries. The data model presents these values in its native representation; it
does not invent the processor list.

## Platform extension data

Host-defined system values are conceptually children of `_x`; their concrete representation is data-model-specific.

# Time and events

## Logical time

Tinyactors never reads the wall clock. The embedder supplies monotonic logical time in milliseconds with `ta_system_set_time()`. Delayed sends persist an absolute logical due time, not a remaining duration.

Advancing time makes due sends eligible. Loading a snapshot does not itself perform external I/O.

A pending delayed send that should not fire can be withdrawn from outside the chart with `ta_actor_withdraw_send(system, actor, send_id)` (ADR 0084). It names the owning actor and the `send_id` its inspection lists in `delayed_sends` (`ta_send_id_parse()` turns the text a chart sees, such as `#_send.3`, into one), and removes what a `<cancel>` of that ID in the actor would: every pending send of the actor with that ID. It works while the actor idles or while a macrostep is paused, and allocates nothing. A send the router already refused once is `submitted`: it has been sent and belongs to its receiver, so it is not withdrawn and the call answers `TA_ERR_DONE`. `TA_ERR_NOT_FOUND` means no resident actor with that ID, or no pending send of it with that ID. The withdrawal changes the actor, so an image captured before it can no longer unload it (`TA_ERR_STALE`); capture again.

## Event names

Intern each event-name segment with `ta_system_intern_atom()`, then construct
the name from one ordered `ta_atom_list` with `ta_event_name_intern()`. The
core accepts no dotted-string event-name parser, so names are always structured
token paths. The list must be non-empty and its atoms must come from that same
system.

Prefix matching follows SCXML semantics:

- `error` matches `error`, `error.execution`, `error.execution.datamodel.condition`, ...
- `error.execution` does not match `error.communication`;
- transitions can hold several compiled atom-segment patterns; a wildcard
  segment can occur anywhere, so `error.*.retry` can match retry events below
  `error` without reparsing event strings;
- an eventless transition has no patterns, so it is checked only during
  stabilization rather than against named events.

Generated names such as `done.state.X`, `done.invoke.X`, and detailed `error.*` names should be built structurally, without formatting and reparsing strings.

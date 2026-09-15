# ADR 0035: System variables are protected data-model views

- Status: Accepted
- Date: 2026-09-15

## Context

SCXML defines protected system data including the current event, session identity, machine name, available I/O processors, and platform-specific values. Their concrete representation depends on the data model.

Duplicating complete host objects into every actor whenever these values change would waste memory and complicate lifetime management.

## Decision

The data model receives a `ta_dm_host` view from which it can obtain semantic system information. The plugin exposes that information to its expression language using its native representation.

The core does not require the plugin to physically store copies of system variables in actor data.

At minimum the host view supports:

- current event;
- actor/session identity;
- definition/machine name;
- `In(state)` against the full active configuration, including parallel regions;
- I/O processor metadata;
- platform-specific values under the equivalent of `_x`.

The core controls when the current-event binding changes. It is initially unbound, becomes bound when an event is taken for processing, and remains bound until another event is processed.

Attempts to assign to protected system variables fail as data-model operations and cause the appropriate `error.execution` control event.

## Consequences

Python, Ruby and JavaScript plugins can construct lightweight proxy objects rather than copying event/session metadata.

The Null data model may expose none of these variables to expressions while still using the host's `In(state)` operation internally.

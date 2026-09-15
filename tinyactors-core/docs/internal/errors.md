# Error model

Tinyactors has two orthogonal failure channels.

## `ta_result`

Used for API/runtime failures such as invalid arguments, allocation failure, corrupt snapshot, unsupported operation, busy actor, or failed persistence mechanics. A negative `ta_result` means the requested C operation itself did not complete normally.

## SCXML `error.*`

Used for statechart-visible runtime errors. These are ordinary internal platform events. The broad standard categories are:

- `error.execution` — failures internal to executing the machine, such as expression/assignment evaluation;
- `error.communication` — failures communicating with external entities;
- `error.platform` — reserved platform/application namespace.

Tinyactors may append structural detail suffixes while retaining those prefixes.

A callback should not directly enqueue the error. It reports a `ta_fault`; the interpreter maps operation context to the appropriate event and implements the correct local semantic consequence.

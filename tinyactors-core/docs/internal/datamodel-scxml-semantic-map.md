# SCXML semantic map

This file records how SCXML 1.0 data semantics map to the `ta` runtime without requiring XML in the core.

| SCXML concept | `ta` semantic mechanism |
|---|---|
| data model definition | `ta_datamodel` plugin + prepared definition state |
| `<data id>` | compiled data declaration -> `declare` |
| `<data expr>` | expression initializer -> `eval_value` + `set_named` |
| `<data>` inline content | literal initializer -> `import_literal` + `set_named` |
| `<data src>` | importer/resource layer resolves resource to literal; runtime imports literal |
| early binding | engine runs initializer at actor initialization |
| late binding | engine declares at initialization; runs initializer before first entry actions |
| environment top-level override | actor creation initial values -> `set_named` |
| `cond` | `eval_condition` |
| `In(state)` | `ta_dm_host_in_state` |
| value expression | `eval_value` |
| location expression read | `read_location` |
| `<assign>` | evaluate/import value then `assign` |
| `<script>` | `execute_script` |
| `<foreach>` | `foreach_snapshot`, `foreach_item`, `bind_iteration` + core continuation |
| `<param expr>` | `eval_value` -> named pair |
| `<param location>` | `read_location` -> named pair |
| `namelist` | location reads -> named pairs |
| parameter payload | `make_params` |
| `<content expr>` | `eval_value` |
| inline `<content>` | `import_literal` |
| `<donedata>` | normal content/parameter materialization attached to done event |
| `_event` | protected system-variable view of core current event |
| `_sessionid` | actor/session identity exposed by host view |
| `_name` | definition/machine name exposed by host view |
| `_ioprocessors` | host-view metadata materialized by plugin |
| `_x` | host platform values materialized by plugin |
| incoming event data | `import_event_data` |
| `error.execution` | core-generated structured internal platform event from callback fault |

## Relevant SCXML requirements

The mapping preserves these important requirements from SCXML 1.0:

- conditional evaluation errors behave as false and generate `error.execution`;
- all data models supporting conditions provide `In(state)`;
- late-bound bindings exist before their initializers run;
- foreach iterates as if over a shallow copy;
- foreach errors terminate the foreach and its containing executable block;
- content/param expression failures become execution errors with the prescribed local consequence;
- system variables are protected;
- event data is converted to the receiving data model when possible;
- delayed send arguments are materialized when the send action executes.

Reference: https://www.w3.org/TR/scxml/ sections 4.6, 5.2–5.10, 6.2 and Appendix B.

# SCXML semantic coverage map

Tinyactors intentionally separates SCXML semantics from SCXML XML syntax.

| SCXML concept | Tinyactors design location |
|---|---|
| hierarchical states | `definition`, `configuration`, `interpreter` |
| compound/default initial entry | `definition`, `interpreter` |
| parallel states | `configuration`, `interpreter` |
| final states | `interpreter`, structured `done.state.*` events |
| shallow/deep history | `history`, `interpreter` |
| event descriptors | `event_name`, transition patterns |
| transition conditions | `datamodel`, `interpreter` |
| transition conflict/priority | `interpreter` |
| onentry/onexit/executable content | compiled action IR + `interpreter` |
| raise/internal events | `event`, actor internal queue |
| assign/script/expressions | `datamodel` |
| system variables / `In()` | `ta_dm_host` |
| send | action IR, `timer`, built-in/custom `io` |
| SCXML Event I/O Processor / special targets | built-in `io`, `datamodel` target decoding, `routing` |
| cancel | `timer` ownership index |
| invoke/autoforward/finalize | `invoke`, `interpreter` |
| `done.invoke.*` | `invoke`, `event_name`, `routing` |
| `error.execution` | `fault`, `interpreter` |
| `error.communication` | `fault`, `io`, `invoke` |
| platform errors | `fault`, structured `error.platform.*` |
| SCXML event I/O processor | implementable as an I/O adapter/actor transport |
| XML syntax/namespaces | optional future importer/exporter, not core |
| ECMAScript data model | optional data-model plugin, not core |
| Null data model | trivial built-in/adapter candidate |

Semantic details should be checked against the W3C SCXML 1.0 Recommendation: <https://www.w3.org/TR/scxml/>.

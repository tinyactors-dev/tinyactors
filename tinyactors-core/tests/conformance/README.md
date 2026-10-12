# Host conformance scenarios

Binding-independent scenarios: SCXML documents (ECMAScript datamodel) plus
host operations and the outcomes every host must produce, run on simulated
time by `tests/conformance/run_c.c` (`ta_host` and the quickjs-ng
datamodel), the `conformance` program of `mise run test`.

## Format

```json
{
  "name": "basics",
  "description": "what it shows",
  "documents": { "counter.scxml": ["<scxml ...>", "...", "</scxml>"] },
  "steps": [ { "spawn": "counter.scxml", "as": "a" }, { "expect": { "states": { "a": ["idle"] } } } ]
}
```

Documents are arrays of lines (joined with newlines). After every step the
host runs until it is idle at the current simulated time (starting at 0).

| step | meaning |
|---|---|
| `{"spawn": doc, "as": name, "input"?: {...}, "documents"?: [doc...]}` | start an actor; `input` overrides `<data>`; `documents` are linked alongside (for `<invoke src>`) |
| `{"send": name, "event": e, "data"?: v}` | an external event; `"@name"` strings in `data` become that actor's `#_scxml_<session>` |
| `{"sendMany": [{"to", "event", "data"?}...]}` | one batch |
| `{"advance": ms}` | move the clock |
| `{"hibernate": name}` | unload the actor (the host keeps the blob, and wakes it when it has work) |
| `{"snapshot": id}` / `{"restore": id}` | the whole system to bytes / replace the running system with it (a restart) |
| `{"reload": doc, "documents"?: [...]}` | a new version of the chart with the same `<scxml name>` |
| `{"migrate": {"wake"?: bool, "timeoutMs"?: n}}` | move every actor on an older version to the current one (until the job ends) |
| `{"expect": {...}}` | check (below) |

Expectations (all optional):

- `logs`: the `<log>` messages since the previous `expect`, in order
- `logsUnordered`: the same, as a multiset (for interleavings hosts may order differently)
- `states`: `{name: [atomic state ids]}` (sorted) of live actors
- `done`: `{name: output}` — the actor finished with that `<donedata>` (`true`: no data)
- `hibernated` / `awake`: actor names in / not in hibernation

## Protocol vectors

Beside the scenarios, the cross-system protocol (`docs/protocol.md`) has its
own shared files, run by every implementation:

- `addresses.json` — address parsing/formatting (C: `tests/unit/test_address.c`).
- Neutral values, requests, host configs and envelopes, as bytes, are
  protocol vectors in `tinyactors-test/suites/protocol/` (its README.md).
- `span_ids.json` and `traceparent.json` — trace values: the span ID of a
  step key, and W3C `traceparent` text (C: `tests/unit/test_trace.c`).

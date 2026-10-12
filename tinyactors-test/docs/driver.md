# The driver protocol

A conforming implementation is one command that speaks this protocol on
its standard input and output. The runner (`lib/`) starts it and sends it
the suites' cases; `ta-conform` is the C reference
(`tinyactors-core/tools/conform/`), and each binding has an adapter written
over its own API (`conform.mjs`, `python -m conform`).

The suite follows the W3C IRP's approach: a scenario's charts check what
happened themselves and end in a final state that logs `Outcome` `pass` or
`fail`. The implementation runs them with its own loop and effect handlers,
in the test environment below, and answers only the outcome. What the suite
compares byte for byte is what core takes and gives at its API (the
protocol vectors): configs, requests and replies, envelopes, values.

Every method names the core calls behind it; header references are to
`tinyactors-core/include/tinyactors/`.

## Frames and messages

- **Frames.** Each message is a 4-byte unsigned length, big-endian, then
  that many bytes: one CBOR item (RFC 8949). Calls go to the command's
  standard input and replies come on its standard output, one per call, in
  order. Standard error is free text; the runner shows its end when a case
  fails. The command exits with status 0 when its input ends.
- **Calls and replies** take JSON-RPC 2.0's shape without its `"jsonrpc"`
  member, as `ta_host_request` does (`tinyactors-core/docs/requests.md`):
  `{"method", "params", "id"}` in, `{"id", "result"}` or
  `{"id", "error": {"code", "message", "data": {"reason", "path"}}}` out.
  Every call has an integer `id`; there are no notifications or batches.
- **Errors are the protocol's own**, with requests.md's codes and reasons:
  -32700 `malformed`, -32600 (not a call), -32601 `unknown` (no such
  method), -32602 `missing`, `type`, `unknown`, `range` (a param, named by
  `path`), -32002 `state` (no host, or a host already). An implementation
  that does not offer a method answers -32601; the runner skips what needs
  it and says so.
- **What core answers is a result.** A method that calls a function
  returning `ta_status` reports it as `"status"`, the integer of `types.h`
  (0 `TA_OK` … -9 `TA_ERROR_ABORTED`), and its other members only when it
  is 0, unless the method says otherwise.
- **Bytes core takes or gives** (a config, a request, a reply, an envelope,
  a value's CBOR) are byte strings, exactly as core takes or wrote them. An
  implementation whose API takes the language's values instead (a binding's
  `request(method, params)`) decodes the well-formed ones into those values
  and encodes them again on the way in, as its API does: that is what the
  protocol vectors check.
- **Event data and spawn params** in a scenario's steps are neutral values,
  CBOR items as `tinyactors-core/docs/protocol.md` ("Values") defines them,
  which the implementation converts as its API does.

## Methods

| Method | Params | Result | Core |
|---|---|---|---|
| `scenario.run` | `documents`: array of `{"name", "text"}`, SCXML; `steps` (below); `datamodel` (`"quickjs"` or `"native"`, default `"quickjs"`); `realm` (`"auto"` or `"isolated"`, quickjs only) | `outcome`: `"pass"`, `"fail"`, `"compile"` or `"timeout"` | a host of its own in the scenario environment (below): `ta_host_create`, then each step's calls, each followed by the loop until idle |
| `w3c.run` | `documents`: the test's start document first, then its dependencies; `datamodel`, `realm` | `outcome`, as `scenario.run` | one W3C IRP test in the W3C environment (below) |
| `host.create` | `config` (bytes); `datamodel`, `realm` | `status`; `datamodel`: the vtable's `name` (`datamodel.h`); `error` (bytes) when refused | `ta_host_create` (`host.h`) with `environment->error` set; quickjs: `ta_datamodel_ecmascript_new` with `realm` (`datamodel_ecmascript.h`); native: the implementation's own vtable, or -32602 `range` when it has none |
| `host.free` | — | `{}` | `ta_host_free` |
| `host.request` | `request` (bytes); `capacity` | `status`; `length`; `reply` (bytes) when `status` is 0 | `ta_host_request` into a `ta_buffer` of `capacity` bytes (default: enough); on `TA_ERROR_NO_MEMORY` `length` is what it needs |
| `image.add` | `charts`: array of `{"name", "text"}` | `status`; `segment` | `ta_compile_charts`, `ta_compile_image` (`compile.h`), `ta_image_open` (`image.h`), `ta_host_add_image` |
| `actor.spawn` | `chart` (its name); `params` (value) | `status`; `session` | `ta_host_chart`, `ta_host_spawn` |
| `actor.send` | `session`, `event`; `data` (value) | `status` | `ta_host_send` |
| `clock.advance` | `monotonic` (ticks), `wallNs` | `{}` | `ta_host_advance_time` |
| `host.run` | — | `status` of the last run; `envelopes`: array of bytes | `ta_host_run`, again while it answers `TA_RUN_YIELDED` or `TA_RUN_EFFECTS_FULL` (`system.h`), at most 1,000 times, acknowledging the effects after each run; for each `PEER` and `PEER_ERROR` effect, in order, the bytes `ta_host_envelope` writes. No other effect is handled |
| `peer.add` | `id`; `trusted` (bool) | `status`; `peer` | `ta_host_peer_add` (`TA_PEER_TRUSTED`) |
| `envelope.read` | `cbor` (bytes); `error` (bool: an error envelope) | `status`; `back` (bytes): what was read, written again | `ta_host_envelope_read` or `ta_host_peer_error_read`, then `ta_host_envelope` of a `PEER` (`PEER_ERROR`) effect holding it |
| `envelope.deliver` | `peer`, `cbor` (bytes) | `status`; `route` (`TA_ROUTE_*`) | `ta_host_envelope_read`, then `ta_host_deliver` |
| `value.roundtrip` | `cbor` (bytes) | `status`; `back` (bytes) | the host's datamodel's `from_wire`, then `to_wire` (`datamodel.h`) |
| `value.encode` | `expression` (text, ECMAScript) | `status`; `cbor` (bytes) | ECMAScript datamodels only: the expression's value, then `to_wire` (quickjs: `ta_datamodel_ecmascript_payload_eval`) |

The methods from `host.create` on serve the protocol vectors; they need a
host (`host.create` makes one and its datamodel, `host.free` frees both),
and `host.create` needs none (-32002 `state` otherwise). Optional params
left out mean none: empty text, 0, no value.

## Scenario steps

`steps` is an array of maps, each one host operation a chart cannot do
itself. A step carries no expectation: the charts check afterwards. They
are the steps of core's conformance scenarios
(`tinyactors-core/tests/conformance/README.md`, "Format") without `expect`,
with images named instead of a document per spawn: `image` replaces a
spawn's `documents` and `reload`, and `spawn` names a chart. A document is
named by its `name` in `documents` (`counter.scxml`); a chart by its
`<scxml name>`, else its document's name (`ta_host_chart`).

| Step | Calls |
|---|---|
| `{"image": [names]}` | the documents named, compiled into one image (the first is the default chart) and added as a segment: `ta_compile_charts`, `ta_image_open`, `ta_host_add_image`. A later image with a chart of the same `<scxml name>` is its new version (a reload) |
| `{"spawn": alias, "chart": name, "params"?: value}` | `ta_host_chart` (the current version of the chart named), `ta_host_spawn` with `params` (overriding `<data>` of those names) and `name` = `alias` |
| `{"send": alias, "event": name, "data"?: value}` | `ta_host_send` to the alias's session |
| `{"sendMany": [{"to": alias, "event", "data"?}]}` | `ta_host_send_many`, one batch |
| `{"advance": ticks}` | the monotonic clock moves on by `ticks`: `ta_host_advance_time` |
| `{"hibernate": alias}` | `ta_host_hibernate`; the blob goes to the implementation's store |
| `{"snapshot": name}` | `ta_host_snapshot_pack` with the datamodel's state, kept under `name` |
| `{"restore": name}` | `ta_host_snapshot_unpack`, `ta_host_restore` with its images and the datamodel's state: the restored host replaces the running one (a restart) |
| `{"migrate": {"wake"?: bool, "timeout"?: ticks}}` | `ta_host_migrate` of every chart (`TA_HOST_MIGRATE_WAKE` with `wake`), then the loop until its `JOB_DONE` |

In `params` and `data`, a text `"@alias"` (at any depth) stands for that
actor's address, `#_scxml_<session>`, as core's conformance runner
substitutes it (`tests/conformance/run_c.c`, `payload`). A step the host
refuses (a status other than `TA_OK`, or fewer events taken) ends the
scenario as `fail`, and the implementation says why on standard error.

## The scenario environment

What the world does around a scenario. It is what core's conformance
runner does today (`tests/conformance/run_c.c`), and nothing more:

- **The host:** config `{"systemID": "conformance", "system":
  {"ticksPerSecond": 1000}}` (`run_c.c`, `host_config`), one datamodel, no
  I/O processors, no peers. The scenarios send only to actors of the same
  host (`#_scxml_<session>`, `#_parent`, `#_<invokeid>`).
- **Time:** the monotonic clock starts at 0 and moves only with `advance`
  steps; `wallNs` is 0 (`run_c.c`, `run_scenario`).
- **After every step** the loop runs until the host is idle at the current
  time: `ta_host_run`, the effects handled as below and acknowledged, again
  until `TA_RUN_IDLE` with nothing left to answer, at most 1,000 times
  (`run_c.c`, `settle`).
- **`WAKE`:** the actor's blob from the store, `ta_host_wake`
  (`run_c.c`, `settle`; `tinyactors-core/docs/bindings.md` section 4).
- **`REQUEST`** (a `<invoke srcexpr>` the image did not resolve): its
  `auxiliary` with a leading `file://`, `file:` or `./` removed names a
  chart of the effect's segment's image; `ta_host_resume` with that chart's
  index + 1, or with `TA_ERROR_INVALID` when there is none, which the
  invoking chart sees as `error.execution` (`run_c.c`, `answer_request`).
- **`JOB_DONE`** ends a `migrate` step.
- **`LOG`** with the label `Outcome` decides the outcome: the first one,
  `pass` or anything else (`fail`), from any session, and the scenario
  stops there, as for a W3C test (`tests/w3c/run_w3c.c`, `run_one`).
  Other logs, `DONE` and `ERROR` are ignored.
- **The outcome** is `compile` when a document does not compile, and
  `timeout` when the steps are done, the host is idle and no `Outcome` was
  logged.

## The W3C environment

What `tinyactors-core/tests/w3c/run_w3c.c` does around a test:

- **The host:** `ticksPerSecond` 1000, `maxEvents` 1024, `maxActors` 64,
  one I/O processor: BasicHTTP (`basic_http.h`, `TA_BASIC_HTTP_TYPE`) at
  `http://localhost:8080/scxml`, synchronous (`TA_IO_PROCESSOR_SYNC`).
  The image holds the start document and its dependencies, and the start
  chart is booted.
- **A `SEND` to that processor** is written as an HTTP POST
  (`ta_http_encode_post`) and, after the acknowledgement, read back
  (`ta_http_parse_request`) and delivered to the sender: the POST's event
  name, `origin` the location, `origintype` the BasicHTTP type, `raw` the
  message, and `data` an object of its form fields, as text. A send that
  cannot be written is answered with `TA_EVENT_IN_ERROR_COMMUNICATION`
  and its sendid.
- **Time:** when the host is idle, the clock moves to its next deadline;
  with none, the test is over.
- **Bounds:** at most 200 runs of 20,000 fuel; a run that ends
  `TA_RUN_EXHAUSTED` or `TA_RUN_FAULT` ends the test.
- **The outcome** as for a scenario: the first `Outcome` log; `timeout`
  without one; `compile` when the documents do not compile.

## Adding a method

A method is added when a suite needs a core call no method reaches. It
names the calls it makes, takes their struct fields in camelCase, and
answers `status` and the calls' outputs. It goes in the table above, in
`ta-conform` and in each binding's adapter, in the same change.

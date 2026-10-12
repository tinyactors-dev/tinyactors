# The mapping table: the JavaScript API and core

Each item of `@tinyactors/node`'s API next to what core has behind it
(tinyactors-core/docs/bindings.md, section 2: "An item with no core
function behind it does not belong"). Written before the item, in the
same commit, and kept in step with `index.d.ts`.

**The rule:** every row's Core cell names, in backticks, at least one of

- a function of core's API descriptions
  (`tinyactors-core/include/*.api.json`): `ta_host_send`;
- a constant of them: an effect (`TA_EFFECT_LOG`, `TA_HOST_EFFECT_PEER`),
  a run status (`TA_RUN_IDLE`), a flag;
- a request method of `tinyactors-core/docs/requests.md`: `actor.context`;

and nothing else in backticks. `mise run lint` checks it
(`mise/lib/api_map.rb` at the repository root): a name core does not have
fails. Say in Notes how the item uses them: the turn, a request, the
handling of an effect.

A core function no item uses is listed below the table: used internally
(say where) or not exposed (say why).

A row marked *planned* is an item the plan (ROADMAP.md) calls for,
written ahead so its core is checked and the API review sees it; the
commit that adds the item removes the mark.

| API item | Core | Notes |
|---|---|---|
| `encode(value): Uint8Array` | `ta_cbor_encode` | The value table (`docs/values.md`) as tokens built by `src/values.c`, written in one call; the same conversion as requests and event data. The adapter writes the driver's frames with it. |
| `decode(bytes): unknown` | `ta_cbor_decode` | One call into tokens, then JavaScript values by the same table; what it cannot read is refused, never made into a value. The adapter reads the driver's frames with it. |
| `TinyactorsError` (`code`, `reason`, `path`, `status`) | `ta_host_request`, `ta_host_create`, `TA_ERROR_INVALID` | What core refused: a request's or a config's error reply, unchanged (`code`, `reason`, `path`, the message), or a call's status (`status`; the message names the call and the status, as the Python binding's). `docs/decisions.md`, "Errors". |
| `OK`, `ERROR_EXECUTION` … `ERROR_ABORTED` | `TA_OK`, `TA_ERROR_EXECUTION`, `TA_ERROR_COMMUNICATION`, `TA_ERROR_INVALID`, `TA_ERROR_NO_MEMORY`, `TA_ERROR_STALE`, `TA_ERROR_STATE`, `TA_ERROR_UNSUPPORTED`, `TA_ERROR_FATAL`, `TA_ERROR_ABORTED` | Core's statuses by name, to match a `TinyactorsError`'s `status` on; the Python binding's module constants. |
| `compile(charts, {resolve}): Uint8Array` | `ta_compile_charts`, `ta_compile_image`, `ta_compile_diagnostics`, `ta_compile_free` | The charts (`{name, text}`) into one image's bytes; `resolve({base, source})` answers a static `src` (`ta_compile_options.resolve`). Refused: `TinyactorsError` with core's `diagnostics`. |
| `new Host({config, datamodel, quickjs, clock, on})` | `ta_host_create`, `ta_datamodel_ecmascript_new`, `ta_datamodel_ecmascript_binding`, `ta_datamodel_environment_in`, `ta_datamodel_environment_charge` | The config as CBOR by the value table; a refusal throws `environment->error`'s reply. One datamodel per host: `'native'` (the default: `src/datamodel/`, core's vtable over the application's engine) or `'quickjs'` (configured by `quickjs`: its realm). `docs/decisions.md`, "The Host's API". |
| `host.addImage(image): bigint` | `ta_image_open`, `ta_host_add_image`, `TA_HOST_EFFECT_SEGMENT_FREED` | A copy of the bytes, kept until the segment is freed or the host closes; the segment. |
| `host.boot(segment)` | `ta_host_boot` | The actors a system image declares; then the turn. |
| `host.spawn(chart, {name, params, traceparent}): bigint` | `ta_host_chart`, `ta_host_spawn`, `ta_host_advance_time` | The current version of the chart named; `params` made a payload of the host's datamodel; the clocks advanced first (bindings.md, section 7); the session; then the turn. |
| `host.send(session, event, data)` | `ta_host_send` | `data` made a payload of the host's datamodel; then the turn. |
| `host.sendMany([{session, event, data}]): number` | `ta_host_send_many` | One batch; the length of the accepted prefix (the rest are released). |
| `host.terminate(session)` | `ta_host_terminate` | Then the turn. |
| `host.request(method, params): unknown` | `ta_host_request` | `{"method", "params", "id"}` by the value table; the `result`, or the `error` thrown as `TinyactorsError`; a reply that does not fit is asked again in the length it needs. |
| `host.addPeer({id, trusted}): bigint` | `ta_host_peer_add`, `TA_PEER_TRUSTED` | The peer. |
| `host.deliver(peer, envelope): number` | `ta_host_envelope_read`, `ta_host_deliver`, `TA_ROUTE_DELIVERED`, `TA_ROUTE_REFUSED` | An envelope's bytes from another process; delivered also when its data is not one item (core refuses it and answers the sender); the `TA_ROUTE_*` outcome. |
| `host.close()` | `ta_host_free`, `ta_datamodel_ecmascript_free` | Frees the host, its datamodel and its images, and drops every reference it holds; from a handler, after the loop returns. |
| `on.log({session, label, text})` | `TA_EFFECT_LOG`, `ta_host_effect_text` | The `<log>`'s label (auxiliary) and text (event). |
| `on.done({session, name, data, final})` | `TA_EFFECT_DONE`, `ta_host_effect_number` | The actor's name (target), its donedata (the payload through its datamodel's `to_wire`), the `<final>` that ended it (auxiliary). |
| `on.error({session, code})` | `TA_EFFECT_ERROR` | The fault code (flags); the actor was reclaimed. |
| `on.peer({peer, envelope})`, `on.peerError({peer, envelope})` | `TA_HOST_EFFECT_PEER`, `TA_HOST_EFFECT_PEER_ERROR`, `ta_host_envelope` | The envelope or error envelope as bytes, for the transport to the peer's process. |
| the turn | `ta_host_advance_time`, `ta_host_run`, `ta_host_effects`, `ta_host_effects_ack`, `TA_RUN_IDLE`, `TA_RUN_YIELDED`, `TA_RUN_EFFECTS_FULL`, `TA_RUN_EFFECTS`, `TA_RUN_EXHAUSTED`, `TA_RUN_FAULT` | Not an item: what an input, the timer and `increment` run (`src/loop.c`): one `setTimeout` at `next_deadline`, `setImmediate` after a yielded run or an input. |
| `SimulatedClock`, `increment(ms)`, `now()` | `ta_host_advance_time`, `ta_host_run`, `TA_RUN_IDLE` | A host created with it arms no timer. `increment(ms)` advances the clock by `ms` (the wall clock is the simulated time) and runs the loop until `TA_RUN_IDLE`; inputs run the turn at the current time. The adapter's `advance` steps use it. |

## Core functions no item uses

Written out at M1's end (ROADMAP.md, M1, step 8). So far:

- `ta_host_effects`, `ta_host_effects_ack`, `ta_host_effect_number`,
  `ta_host_effect_text`: used by the loop and the effect records
  (`src/loop.c`, `src/effects.c`), not items of their own.
- `ta_compile_system`, and `ta_compile_options`'s `TA_COMPILE_WERROR` and
  `TA_COMPILE_STRIP_DEBUG`: not exposed in M1 (`docs/decisions.md`, "The
  Host's API"); a system manifest's `compile` is a later milestone's.

# The mapping table: the Python API and core

Each item of the `tinyactors` module's API next to what core has behind
it (tinyactors-core/docs/bindings.md, section 2: "An item with no core
function behind it does not belong"). Written before the item, in the
same commit, and kept in step with the module.

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
| `tinyactors.encode(value) -> bytes` | `ta_cbor_encode` | The value table (`docs/values.md`) as tokens built by `src/values.c`, written in one call; the same conversion as requests and event data. The adapter writes the driver's frames with it. |
| `tinyactors.decode(data) -> object` | `ta_cbor_decode` | One call into tokens, then Python objects by the same table; what it cannot read is refused, never made into a value. The adapter reads the driver's frames with it. |
| `tinyactors.Address(text)` | `ta_cbor_encode`, `ta_cbor_decode` | An actor reference in a value: tag 32 on its text, which the CBOR functions write and read (`docs/values.md`). |
| `tinyactors.UNDEFINED` | `ta_cbor_encode`, `ta_cbor_decode` | CBOR's `undefined` in a value, distinct from `None` (`docs/values.md`). |
| `tinyactors.TinyactorsError` (`status`, `code`, `reason`, `path`) | `ta_host_request`, `ta_host_create`, `ta_cbor_decode` | What core refused: a request's or config's error reply unchanged, or the status of a call that failed (`docs/errors.md`). |
| `tinyactors.OK`, `ERROR_EXECUTION` … `ERROR_ABORTED` | `TA_OK`, `TA_ERROR_EXECUTION`, `TA_ERROR_COMMUNICATION`, `TA_ERROR_INVALID`, `TA_ERROR_NO_MEMORY`, `TA_ERROR_STALE`, `TA_ERROR_STATE`, `TA_ERROR_UNSUPPORTED`, `TA_ERROR_FATAL`, `TA_ERROR_ABORTED` | Core's statuses (`types.h`), which a `TinyactorsError`'s `status` holds. |
| `SimulatedClock`, `increment(ms)` (planned) | `ta_host_advance_time`, `ta_host_run`, `TA_RUN_IDLE` | A host created with it arms no timer. `increment(ms)` advances the clock by `ms` (the wall clock is the simulated time) and runs the loop until `TA_RUN_IDLE`; inputs run the turn at the current time. The adapter's `advance` steps use it. |

## Core functions no item uses

TODO.

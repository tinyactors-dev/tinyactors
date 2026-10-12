# The API's shape: decisions

What the guide and the headers leave open about the shape of the Python
API, decided for M1 and checked with the reasoned-analysis skill (a fresh
agent's refutation, 2026-10-12: items A1 to A5, each confirmed or found
plausible; what it found is below). Each item's core is in
`api-map.md`; the API review after M1 may change any of them.

| Decision | Source, and what the check found |
|---|---|
| **One handler per effect kind, keyword arguments of `Host`**: `on_log(session, label, text)`, `on_done(session, data)`, `on_error(session, code)`, `on_peer(peer, envelope)`; one call per effect, with its fields read by the accessors. Effects of later milestones are not handed on yet. | bindings.md, section 4 (the effect table) and section 6 ("hands the language one record per effect"). `PEER` and `PEER_ERROR` both go to `on_peer`: both are bytes for the same peer, and carrying them is the application's (bindings.md, section 9); a receiver tells them apart when it reads them. |
| **Inputs run the turn.** Each input gives core the call, then runs the host until idle at the current time; an input from a handler only makes the turn run again. | bindings.md, section 3 ("inputs run the turn at the current time"; "A caller's own clock"), section 5 ("Between runs, any call is fine, including from an effect handler"). |
| **`Host.run()` is the synchronous driver**: turns on the real clock, sleeping until the next deadline, until there is none. | bindings.md, section 3 ("a synchronous one (run until idle, sleeping to the next deadline ...)"). With telemetry (a later milestone) the encoder's flush is always a deadline, so `run()` then serves until it is interrupted (unverified until telemetry is in). |
| **Run statuses**: `YIELDED` (after a check for signals), `EFFECTS_FULL` and `EFFECTS` run again at once; `EXHAUSTED` raises `MemoryError("ta_host_run: TA_RUN_EXHAUSTED", pool)` with core's `TA_POOL_*`; `FAULT` raises `TinyactorsError` with the run's status. The effects are acknowledged after every run, and the first handler's exception is raised after the turn. | bindings.md, section 3 (the loop and "whatever a handler raised: ack anyway, finish the turn, then raise the first error"). The check asked that `EXHAUSTED` carry the pool core names (`run.detail`); it does. |
| **Charts are text, compiled into an image by `add_image(charts, files=None)`**: a dict of name to SCXML text, the first the default chart, and `files` what a `src` names, looked up as written. Sessions and peers are `int`s, as core's ids. | `compile.h` (`ta_compile_charts`, the resolver), `host.h` (session ids, peer handles). |
| **Data for a chart is made for the chart's datamodel**: a Python chart's by the value table straight into a payload, an ECMAScript chart's through CBOR and its `from_wire`. | `host.h`: a payload belongs to one datamodel (`ta_envelope.datamodel`, `ta_effect.datamodel`); bindings.md, section 2 (no serialization on the hot path). |
| **`SimulatedClock`** (xstate's name, our meaning): `increment(ms)`, the clock's hosts' monotonic time in their own ticks (`host.config`), the wall clock the simulated time. | The root ROADMAP.md, step 8, M1; bindings.md, section 3. |

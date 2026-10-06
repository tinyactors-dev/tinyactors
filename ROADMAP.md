# Roadmap

Work across packages, in the order it is meant to happen. Ideas that are
not decided yet are in [`docs/proposals/`](docs/proposals/README.md). How
the layers divide the work is in the [README](README.md#layers-and-responsibilities).

The roadmap of the earlier core and the ring protocol is kept at the tag
`ring-proto-final`; [ADR 0004](docs/adr/0004-replace-the-core-with-the-standalone-runtime.md)
says why it ended.

## Next

1. **Telemetry.** Core has none. The design follows its rule that effects
   are pulled, never pushed through a callback:
   - core writes trace records (macrostep begin and end, microsteps,
     conditions, assignments, sends, invocations) into a fixed, lossy
     buffer in the system's block, beside the effects outbox; a full buffer
     drops the record and counts it, it never stops the run;
   - the host drains it with a call pair like `ta_system_effects` and
     `ta_system_effects_ack`;
   - `ta_event_in` gains an optional `traceparent`; a traced event carries
     its context in the string slab, so the 40-byte event record does not
     grow; delayed sends and hibernated or migrated actors keep it
     (`docs/durability.md`);
   - `ta_effect` carries the step's trace and span IDs for `SEND`, `INVOKE`
     and `LOG`, so an I/O processor writes `traceparent` without calling
     into core; span IDs are computed from the session and step counters;
   - metrics come from `ta_system_stats_get` as they are.
2. **Port `tinyactors-otlp`** to read that buffer. Its protobuf encoding
   stays; its input side (`ta_system_tel_peek`, `ta_trace_record_text`)
   changes.
3. **Rebuild release packaging** on [ADR 0001](docs/adr/0001-packaging-and-releases.md)'s
   machinery. Done: `@tinyactors/node` (decision 18) and the C libraries,
   libtinyactors and libtinyactors_runtime (decision 19). Left: the web
   package and the PostgreSQL source archive
   (`tinyactors-core/docs/distribution.md` is the specification).
4. **Port the binding surface check** (`tinyactors-core/tools/check_surface.py`)
   onto the pinned toolchain (`Toolchain::Binary` instead of `cc` and `nm`
   from PATH) and run it in `mise run test`.
5. **Changelog components** (`mise/lib/changelog/components.rb`). Done:
   `napi` is the Node.js binding (`tinyactors-core/bindings/node`, ADR 0001
   decision 18), and `core` reads the descriptions of both libraries,
   `include/tinyactors.api.json` and `include/tinyactors_runtime.api.json`
   (`mise run api`).
6. **`tinyactors-scxml`:** decide whether the C SCXML writer is still
   wanted (the JavaScript binding writes SCXML itself); if so, port it to
   the compiler's model, and bring its decoder's edge cases into the
   compiler's tests either way. Consider tinyxml for the compiler's XML
   reader (`tinyactors-core/src/compile/xml.c`).

## Open questions

- **Refusing a send to the sending chart.** A full pool stops the run
  until the host grows the block; the chart never hears about it. An
  `error.execution` to the sender when a pool is full would let charts
  handle overload themselves.

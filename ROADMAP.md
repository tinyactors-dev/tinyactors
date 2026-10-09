# Roadmap

Work across packages, in the order it is meant to happen. Ideas that are
not decided yet are in [`docs/proposals/`](docs/proposals/README.md). How
the layers divide the work is in the [README](README.md#layers-and-responsibilities).

The roadmap of the earlier core and the ring protocol is kept at commit
`943b1f31^` (the parent of the commit that replaced that core; the ADRs
call it the tag `ring-proto-final`, which was never pushed);
[ADR 0004](docs/adr/0004-replace-the-core-with-the-standalone-runtime.md)
says why it ended.

## Next

1. **Rebuild release packaging** on [ADR 0001](docs/adr/0001-packaging-and-releases.md)'s
   machinery. Done: `@tinyactors/node` (decision 18) and the C libraries,
   libtinyactors and libtinyactors_runtime (decision 19). Left: the web
   package and the PostgreSQL source archive
   (`tinyactors-core/docs/distribution.md` is the specification).
2. **Port the binding surface check** (`tinyactors-core/tools/check_surface.py`)
   onto the pinned toolchain (`Toolchain::Binary` instead of `cc` and `nm`
   from PATH) and run it in `mise run test`. Its link recipes no longer
   build: the node recipe compiles `allocator_wasm.c` for the host, the web
   recipe misses `ta_runtime_malloc`; `ta_wasm_alloc`, `ta_wasm_free` and
   `ta_datamodel_ecmascript_wasm_set_time` are not classified.
3. **Changelog components** (`mise/lib/changelog/components.rb`). Done:
   `napi` is the Node.js binding (`tinyactors-core/bindings/node`, ADR 0001
   decision 18), and `core` reads the descriptions of both libraries,
   `include/tinyactors.api.json` and `include/tinyactors_runtime.api.json`
   (`mise run api`).
4. **Tracing, what is left** after
   [ADR 0006](docs/adr/0006-tracing-is-a-core-feature.md):
   - **metrics:** `metricsToOTLP` (0.1's metrics as OTLP), histograms of
     macrostep durations with **exemplars** pointing at the sampled step
     that produced them;
   - **watching:** 0.1's `watchTraces` (full detail for chosen sessions,
     addresses or charts, whatever the sampling);
   - **the PostgreSQL extension** traces nothing (`TA_TRACING_DISABLED`):
     drain the journal at the end of a statement, or keep records in a
     table;
   - **definition logs** (0.1's `tinyactors.definition.*`): a log record
     when a version is linked, retired and freed.
5. **`tinyactors-scxml`:** decide whether the C SCXML writer is still
   wanted (the JavaScript binding writes SCXML itself); if so, port it to
   the compiler's model, and bring its decoder's edge cases into the
   compiler's tests either way. Consider tinyxml for the compiler's XML
   reader (`tinyactors-core/src/compile/xml.c`).

## Open questions

- **Refusing a send to the sending chart.** A full pool stops the run
  until the host grows the block; the chart never hears about it. An
  `error.execution` to the sender when a pool is full would let charts
  handle overload themselves.

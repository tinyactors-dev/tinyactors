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

1. **Release packaging** on [ADR 0001](docs/adr/0001-packaging-and-releases.md)'s
   machinery. Done, and released as 0.2.0 on 2026-10-06: `@tinyactors/node`
   (decision 18, tag `tinyactors-napi/v0.2.0`), and the C libraries
   libtinyactors, libtinyactors_runtime and libtinyactors_datamodel_ecmascript
   (decisions 19 and 20, tag `tinyactors-core/v0.2.0`). Left: the web npm
   package and the PostgreSQL source archive (PGXN/PGXS); no task builds
   either yet. `tinyactors-core/docs/distribution.md` describes what is
   built and the plan for the two that are not.
2. **Tracing, what is left** after
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
3. **`tinyactors-scxml`.** Done (2026-10-09,
   [proposal](docs/proposals/scxml-in-core.md)): the parked codec is
   deleted, its error cases with positions test the compiler, and
   `ta_xml_writer` (`compile.h`) writes the SCXML of charts a binding
   builds; the JavaScript bindings use it. The compiler keeps its own XML
   reader (`src/compile/xml.c`); core does not use tinyxml.

## Open questions

- **Refusing a send to the sending chart.** A full pool stops the run
  until the host grows the block; the chart never hears about it. An
  `error.execution` to the sender when a pool is full would let charts
  handle overload themselves.

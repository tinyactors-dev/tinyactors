# Research

Background research that informed designs in this repository, kept so it
does not need to be repeated. **Research notes are not decisions and not
instructions.** They record what other projects did and what their sources
say, with citations, as of the date on each note. Decisions live in ADRs,
plans in roadmaps, and possible future work in `docs/proposals/`.

| Note | Date | Used by |
|---|---|---|
| [Distribution in actor runtimes, in hindsight: Orleans, Akka, Erlang/Elixir](actor-runtime-distribution-hindsight.md) | 2026-09-24 | [distributed systems proposal](../proposals/distributed-systems.md) |
| [Handing trace records to a host: OTLP protobuf, OTLP JSON, OTel Arrow, a custom columnar layout](trace-handoff-formats.md) | 2026-09-24 | [napi ADR 0016](../../tinyactors-napi/docs/adr/0016-traces-leave-the-addon-as-otlp-protobuf.md) (Proposed) |
| [OpenTelemetry metrics and logs for an actor runtime](otel-metrics-and-logs-for-actors.md) | 2026-09-24 | [core ADR 0072](../../tinyactors-core/docs/adr/0072-metrics-are-counters-core-keeps-logs-are-records-it-already-writes.md) (Proposed) |
| [Observability: build or reuse? Where tracing's time goes, and what the OpenTelemetry SDK would cost instead](observability-build-vs-reuse.md) | 2026-09-24 | [napi ADR 0017](../../tinyactors-napi/docs/adr/0017-observability-owns-its-encoder-and-makes-off-and-sampled-out-free.md) (Proposed) |
| [Pluggable data models: how SCXML implementations let users bring one, and what it costs in napi](pluggable-datamodels.md) | 2026-09-25 | [napi ADR 0019](../../tinyactors-napi/docs/adr/0019-a-data-model-in-javascript-compiles-source-for-the-built-in-one.md) (Proposed) |
| [Loading actors on demand: how virtual-actor runtimes hold mail while an actor starts](loading-actors-on-demand.md) | 2026-09-25 | [napi ADR 0019](../../tinyactors-napi/docs/adr/0019-mail-for-an-unloaded-actor-waits-while-a-loader-brings-it-back.md) (Proposed) |

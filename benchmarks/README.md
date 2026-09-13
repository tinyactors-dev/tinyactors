# Tinyactors benchmarks

The benchmark suite measures the cost of the tinyactors core under hosted
conditions. It covers actor memory overhead, message throughput, routing, and
long-running mixed workloads. The suite also acts as a stress test: every run
checks actor state, message counts, arena accounting, and clean shutdown.

These benchmarks are diagnostic measurements, not performance tests with
portable pass/fail thresholds. CPU frequency, compiler, allocator settings,
sanitizers, and operating-system scheduling all affect the results.

## Quick start

Build the suite from the repository root:

```sh
make benchmark
```

Run an individual workload:

```sh
build/tinyactors-bench 64 1000000
```

Run the complete built-in benchmark set:

```sh
make benchmark
```

The existing `bench-entities` target remains a focused 200,000-entity
scale probe:

```sh
make benchmark-entities
```

## Workloads

### `actors`

Measures the memory added by live actors at increasing population sizes. The
suite separates fixed system and definition memory from the per-actor slope,
and reports allocator capacity separately from live bytes. It exercises zero,
small, and larger byte-model states as well as native value handles where the
host configuration supports them.

The important result is the steady-state slope across several population
sizes, not the one-actor result. Arena chunking and index growth can make a
single measurement misleading.

### `messages`

Measures message admission, queueing, dispatch, and actor-to-actor routing.
Payload sizes include empty messages and byte payloads from small control
messages through larger copied buffers. The workload reports messages per
second, nanoseconds per message, peak queued memory, and queue high-water
marks.

The suite includes single-actor, pre-queued, and ring-routing cases. A ring
of actors exposes routing and internal-send costs that are invisible in a
single-actor dispatch loop.

### `stress`

Runs a deterministic mixed workload containing many actors, variable payloads,
external and internal events, actor creation and release, delayed sends, timer
activity, inspection, and optional tracing. The random seed is printed with
each result so a failure can be reproduced.

Use a fixed operation count in CI and a duration for local soak testing:

```sh
build/tinyactors-bench stress --operations 1000000 --seed 7
build/tinyactors-bench stress --seconds 300 --seed 7
```

## Common options

```text
--actors N          number of actors in the workload
--messages N        number of messages to admit or consume
--payload-bytes N   byte payload size
--rounds N          measured repetitions
--operations N      fixed stress-workload size
--seconds N         stress duration
--seed N            deterministic workload seed
--warmup N          unmeasured warmup repetitions
--json              emit one machine-readable result object per case
```

Run `build/tinyactors-bench --help` for the defaults and options supported by
the installed build.

## Reading the results

Human-readable output includes the case name, workload size, elapsed time,
throughput, peak arena bytes, allocation counts, and validation status. JSON
output is intended for retaining results in CI or comparing compiler and
platform changes.

Memory results distinguish:

* fixed system and definition bytes;
* actor control and data bytes;
* index, registry, and queue bytes;
* allocator slack and peak capacity.

Throughput results distinguish admission from processing where the workload
can measure both phases. Do not compare a run with tracing, sanitizers, or
debug allocators to an optimized release run.

## Correctness and failure handling

Each case validates that expected messages are consumed, callbacks observe the
expected state, and the system reaches stability at run boundaries. Teardown
stops and releases actors, destroys the system, closes the arena, and verifies
that the benchmark allocator has returned to its baseline.

A nonzero exit status means the workload failed its invariant checks or the
runtime returned an unexpected status. A performance regression is reported
as data; it does not by itself make the command fail.

## Reproducible runs

For useful comparisons, keep the following constant:

* compiler and optimization flags;
* CPU governor and machine load;
* arena chunk and limit settings;
* actor, message, and payload counts;
* warmup and repetition counts;
* stress seed.

Record the commit, compiler version, platform, and command line with saved
JSON output. Run several repetitions and compare medians rather than a single
wall-clock sample.

## Sanitizers and profiling

The suite is hosted code and may be run with the repository's sanitizer
targets or with an external profiler. Sanitizers are useful for stress and
ownership validation but substantially change timings. For CPU profiling,
prefer a long fixed-operation run with warmup disabled only when measuring
startup behavior explicitly.

The benchmark allocator reports core arena accounting; it does not attempt to
include the host allocator's own metadata or the memory used by the benchmark
process outside the supplied arenas.

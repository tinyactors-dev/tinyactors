# Benchmark results, keyed by machine

What `mise run bench` and `mise run bench-compare --store` recorded. The
same build can measure tens of percent apart on two days on one machine
(tinyactors-napi measured one 30% apart), so figures are only comparable
under one **machine signature**, and best side by side: `mise run
bench-compare <commit>` builds the other commit and runs both alternately.
Never compare with numbers from another day or another machine.

## Layout

- `<signature>/`: one folder per machine signature, the first 12 hex
  digits of a SHA-256 over the machine's fields (`mise/bench.rb`): what
  `Toolchain::Host` reports for a benchmark (CPU model, core counts, RAM,
  OS and version, architecture, and on macOS the power source and
  low-power mode), plus the compiler (the pinned clang's version line) and
  the optimization flags. A new clang or new flags is a new signature.
- `<signature>/machine.json`: those fields, readable.
- `<signature>/<commit>.jsonl`: one JSON record per line, appended by each
  run at that commit.

## A record

| Field | Meaning |
|---|---|
| `machine`, `commit`, `dirty` | the signature, the build (short hash of `HEAD`), and whether `staticjs/src`, `include`, `bench/bench.c` or `bench/modules` had changes. A `dirty` record measured `commit` plus uncommitted changes, which nobody can rebuild: its `commit` is the parent of what it measured |
| `time`, `compiler`, `flags`, `version` | when (UTC), the compiler and flags, the library's `sjs_version()` |
| `conditionsBefore`, `conditionsAfter` | `loadAverage` (1, 5, 15 min) and macOS `cpuSpeedLimit` around the run; not part of the signature |
| `bench`, `n`, `unit`, `unitsPerOp` | the case (`bench/bench.c` lists them), its size, and its unit: `byte`, `eval`, `element` or `op` |
| `samples`, `itersPerSample` | timed samples, and ops in each |
| `nsPerOp` | `median` (the headline), `best`, `p25`, `p75` of the samples' ns per op |
| `spreadPercent` | (p75 - p25) / median, in percent: past 5% is too noisy to compare |
| `opsPerSecond`, `unitsPerSecond` | from the median |
| `fuelPerOp`, `slicesPerOp` | evaluations: fuel used, and `sjs_vm_run` calls per op |
| `eventsPerOp` | parse and compile: the parser's events |
| `memory.setupBytes` | bytes live after setup: the program and its inputs |
| `memory.peakBytes` | the op's transient peak above what was live when it began |
| `memory.heldBytes` | bytes live while the op holds its result: a program's size, a value's |
| `memory.allocsPerOp`, `freesPerOp`, `allocatedBytesPerOp` | what one op asked of the allocator |
| `memory.retainedBytes` | bytes one op left behind: always 0, or the case fails |
| `error` | instead of the above, when the case could not run on that build |

Memory is counted by the bench's allocator, under both the context and the
parser, in one untimed op; allocation is deterministic, so it has no
spread.

Read a folder with `jq`, leaving out dirty records, for example the median
ns per element of every `eval.array` case of each commit:

    jq -c 'select((.dirty | not) and (.bench | startswith("eval.array"))) | [.commit, .bench, .n, .nsPerOp.median / .unitsPerOp, .memory.peakBytes]' 92727593a59b/*.jsonl

# Benchmarks

For contributors: how to measure staticjs and compare a change with the
build before it. What the figures mean for a user is in
[../guide/performance.md](../guide/performance.md).

`bench/bench.c` measures each stage on the inputs a statechart gives it:

- **compile**: a chart's modules (`bench/modules/power.js` and
  `campaign.js`, from the spec), a module of 32 KiB, and guards, pushed
  whole, 64 bytes or one byte at a time, and the parser alone; in MB/s.
- **eval**: guards, an assign, a send's target, loading a module and
  calling a function from it, in evaluations per second; and the built-ins
  over arrays, objects and sets of growing size, in elements per second,
  so a row shows whether the time per element is flat or grows with n.
- **write**: rows, numbers and strings as text, with a large buffer and a
  64-byte one, in MB/s.
- **vm** and **values**: a fresh evaluator, a kept one, abandoning one,
  running on little fuel at a time, and building values through the API.

Each case gives its throughput (median of 15 samples of at least 8 ops,
with the spread between the quartiles) and its memory: the bytes its
inputs take, the op's peak, what its result holds, allocations per op, and
bytes left behind, which must be none. `mise run bench` builds with the
pinned clang at `-O2`, prints a table, and stores the records under
`bench/results/<machine signature>/` (see its README). `mise run
bench-compare <commit>` builds that commit's library beside this tree's,
with the same bench program (or, when today's does not compile against
the older `staticjs.h`, that commit's own, with a warning: those rows then
compare two programs as well), and runs both alternately, 4 processes a
side in ABBA order. It flags a throughput more than 5% slower in every run
and any memory increase; a case the older build cannot run is reported as
such.

**Noise.** Identical code does not measure the same twice: on machine
`92727593a59b` (a 4-core cloud VM), `mise run bench-compare HEAD` with
nothing changed moved half the rows by more than 1%, one in ten by more
than 3.5%, and three of 78 by more than 5% (one by 31%), while each side's
four runs spanned 4.5% for the median row. So a row gives each side's
range over its runs (`runs o/n`), a change past 5% inside that range is
reported as within the noise, not as slower, and `?` marks a row whose
runs or samples vary past 5%, which cannot resolve a 5% change; rerun
those cases with `--runs=8` before reading anything into them.

**Other engines.** `mise run bench-engines` runs the cases listed in
`bench/engines/cases.mjs` (bench.c's own, each at one size) in staticjs,
in Node with its JIT, in Node `--jitless`, and in quickjs-wasi (QuickJS-NG
compiled to WebAssembly, run by Node), and writes a report: time per op
in each, the ratios to staticjs and their geometric means, the cost of
starting an evaluation, and memory, with the method and its caveats.

- **What it needs:** Node (pinned in the root `mise.toml`), and npm the
  first time, to install the quickjs-wasi pinned in
  `bench/engines/package-lock.json` into `bench/engines/node_modules/`
  (ignored by git): the first run needs the network, later ones do not.
- **How long:** a full run takes 15 to 20 minutes on a 4-core machine,
  with nothing else running (`--quick` with a filter, under a minute).
- **Verified first.** The JavaScript engines build bench.c's fixtures with
  `bench/engines/guest.js`, a hand-written copy of its generators. Before
  timing anything, the task checks, at each case's smoke size and its
  measured size, that every fixture and every engine's result equals
  staticjs's (length and hash, through `bench/engines/dump.c`, which
  includes bench.c), and that each engine gives bench.c's expected value
  at the smoke size. A difference stops the run, naming the case: after a
  change to a fixture in bench.c, change guest.js to match.
- **Interleaved.** Each case runs 5 rounds (`--rounds <n>`); a round runs
  the four engines one after another, each in a process of its own, never
  two at once, and the next round in the reverse order. A cell is the
  median of the rounds.
- **Where the report goes.** A full run appends its records to
  `bench/results/<signature>/engines-<commit>.jsonl` and writes its report
  to `engines-<commit>.md` beside them and to `bench/engines/REPORT.md`,
  which always holds the latest; commit them. A run with a filter
  (`mise run bench-engines eval.array`) or `--quick` is a look, not a
  record: it writes `build/engines/report.md` only, as `--no-store` does.
- **Reading it.** A ratio is the other engine's time over staticjs's, so
  "3× faster" means staticjs took a third of the time. `±x%` follows a
  figure whose middle rounds spread past 10%. `≈` marks a ratio whose two
  ranges overlap: it is not resolved, the engines may be equal. The
  compile rows favour Node, which compiles inner functions lazily.

**Reporting.** Give throughput and memory together, name the machine
signature, and compare builds side by side with `bench-compare`, never
against numbers from another day or another machine.

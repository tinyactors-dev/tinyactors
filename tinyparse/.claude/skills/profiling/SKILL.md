---
name: profiling
description: Reach for a profiler, sanitizer, or other measurement tool FIRST when a tinyparse question is about speed, memory, a hang, or a crash — instead of guessing, eyeballing, or trial-and-error (e.g. hunting for a crashing input depth by hand). Use when a parse is slow or appears to hang, when memory balloons, when something segfaults or might overflow the stack, when "is this faster?" needs an answer, or when you catch yourself sweeping a parameter to find a threshold. Covers which tool answers which question (callgrind/gprof/perf for where time goes, AddressSanitizer/UBSan for crashes and overflow, `ulimit -s` for stack bounds, getrusage/`/usr/bin/time` for peak memory) and the existing tinyparse profiling scaffold.
---

# Profile first; do not guess

The rule: when the question is *empirical* — why is this slow, where does the
memory go, does this crash, is this faster — answer it with a **tool that
measures**, not by reading code, picking numbers, or sweeping a parameter until
something happens. A profiler or sanitizer gives the answer in one run; guessing
burns an afternoon and often measures the wrong thing. If you find yourself
trying value after value to provoke a behaviour (a depth that crashes, a size
that OOMs), **stop** — that is the signal to reach for the tool that shows the
behaviour directly.

## Which tool answers which question

- **"Where does the time go?"** — `callgrind` (deterministic instruction counts,
  great for a small corpus) or `gprof`/`perf` (sampled wall time). The tinyparse
  profiling scaffold is already wired: see the *Appendix: reproduce* in
  `docs/performance.md` (callgrind_annotate, gprof). The old `bench/prof/`
  driver is gone; `bench/bench_tinyparse`, which `mise run bench:javascript`
  builds, carries function symbols, so run it straight under callgrind (or
  rebuild it with `-O2 -g -fno-omit-frame-pointer`) over a few corpus passes and
  read the top of `callgrind_annotate`. The hot function is named, not guessed.
- **"Is this faster?"** — the benchmark harnesses, not intuition:
  `mise run bench:perf-gate` (the JS-corpus ratchet),
  `mise run bench:react`, `bench:parsers` and `bench:tsc` (the real-workload,
  multi-parser comparison; `mise run bench:fetch` first, for the references).
  Report the median, name the hardware.
- **"Does it crash / can it overflow?"** — a sanitizer or a bound, never a manual
  depth search:
  - **AddressSanitizer** (`-fsanitize=address`) reports a stack-overflow or a
    bad access with the exact frame, immediately, on the first offending run.
  - **`ulimit -s`** shrinks the C stack so a recursion bug surfaces at a small,
    fast depth instead of needing millions of frames — and, conversely, proves an
    *iterative* parser survives a deep input where a recursive one would not:
    `( ulimit -s 256 && ./parser deep-input )` should exit cleanly, not by signal.
    `mise run gate:vm-nocrash` and `gate:parser-nocrash` are this check, standing:
    one 200,000-deep input under a 512 KiB stack.
  - **UBSan** (`-fsanitize=undefined`) catches the overflow/UB a crash hides.
  The right assertion for a no-crash guarantee is "exits without a crash signal on
  pathological input," verified once with a tool — not "depth N works" for a magic N.
- **"How much memory?"** — `getrusage` peak RSS (the bench harnesses and
  `bench/reclaim_test.c` print `peak_rss_kb`; `mise run gate:reclaim` holds it)
  or `/usr/bin/time -v`, not a mental model of allocations.

## The discipline

1. Form the question as something measurable.
2. Pick the tool above; run it once.
3. Read what it says; act on the named cause.
4. Re-run the tool to confirm the fix moved the number.

A claim about speed, memory, or a crash that is not backed by a tool's output is
a guess. The profiling scaffold and the gates above exist so the answer is one
command away — use them before writing speculative code.

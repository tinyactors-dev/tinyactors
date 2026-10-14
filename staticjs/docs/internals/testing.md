# Testing: how the rule is checked

For contributors. The rule is that a program staticjs accepts is valid
JavaScript, and evaluates to the value JavaScript gives, or to a fault,
never to a different value. This page says how the test suite holds staticjs
to it, and which task runs what. `../../AGENTS.md` has the rules for
changing staticjs; [extending.md](extending.md) gives the steps for each
kind of change.

## Against Node: `mise run diff-node`

`mise run diff-node` checks the rule: every case in `tests/spec/` that expects a
value is evaluated in Node and must give the same value (a case with
`@seed` in a `node --random-seed` of its own, so `Math.random` draws the
same values); every case staticjs
refuses as a syntax error must be one JavaScript refuses too; every case it
refuses as unsupported must be valid JavaScript; every case it refuses for a
name (bound nowhere, not exported, no such module) must throw in JavaScript
too; and 100,000 of each of these must give Node's bits: doubles
formatted as `String(x)`; decimal strings (random digits, long ones, and
the exact midpoints between two doubles) read by `Number(s)`; strings with
white space, signs, prefixes and junk read by `Number(s)` and
`parseFloat(s)`, and by `parseInt(s, radix)` in every radix; doubles
through `Math.sqrt` and `Math.fround`; argument lists through `Math.hypot`;
and pairs through `Math.clz32` and `Math.imul`; and BigInts of 1 to 20
limbs through `+ - * / % **` and comparison, compared with doubles, and
written as `String(x)` and `toString(radix)`, read back, rounded as
`Number(x)` and wrapped by `BigInt.asIntN` and `asUintN`, and a fixed
list of 100 doubles (ties, values just under and over a half, subnormals,
1e21 and the largest double) through `toFixed`, `toExponential` and
`toPrecision` with 24 counts of digits each, 0 to 100, and 123 doubles
(integers to the largest double, fractions to the smallest subnormal,
negatives) with each radix's powers and their neighbours through
`toString(radix)` in every radix 2 to 36, and a fixed list of 7,909 pairs
(specials, subnormals, integers odd and even, halves, negative bases,
bases next to 1 to huge powers, results next to overflow and underflow and
in the subnormals) through `Math.pow(x, y)` and `x ** y`, and a fixed
list of 8,391 doubles (specials, subnormals, the thresholds of fdlibm's
ranges and their neighbours, integers, halves, powers of two and ten,
values next to 1 and -1, multiples of ln 2) through each of `Math.exp`,
`expm1`, `log`, `log1p`, `log2` and `log10`, a fixed list of 8,693
doubles (specials, subnormals, the thresholds of the argument reduction
and of each kernel, every power of two, integers, the doubles next to
multiples of pi/4 and pi/2, the doubles closest to a multiple of pi/2 of
many binary exponents, values in [-1, 1]) through each of `Math.sin`,
`cos`, `tan`, `asin`, `acos` and `atan`, and a fixed list of 2,179 pairs
(every special case, quotients next to 2^60 and 2^-60, x next to 1, small
integers) through `Math.atan2(y, x)`, and a fixed list of 6,965 doubles
(specials, subnormals, the thresholds of each function's ranges, among
them the overflow threshold of `sinh` and `cosh`, every power of two,
integers, halves, values next to 1 and -1, cubes) through each of
`Math.sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh` and `cbrt`
(`number_test --print`; these Math lists are compared on x86_64 only, as
Node on arm64 computes some last bits with fused multiply-adds); and random JSON documents read by `JSON.parse` and written back
by `JSON.stringify`, the same documents with a byte or two changed (a
SyntaxError where JavaScript throws one), and `JSON.stringify` calls with
replacers and spaces (`json_test --print`).


## The test programs

The tests are `tests/spec/*.spec` (read by `tests/spec_test.c`, which runs
each case with the source pushed whole, a byte at a time and in chunks, on
different amounts of fuel, and once more with shapes off, which must give
the same program but for its tags, the same outcome and no less fuel),
`tests/api_test.c`, `tests/matcher_test.c` (the shape matcher on every
spec source, the deepest and widest programs, unreadable streams and
running out of memory), `tests/number_test.c`, `tests/json_test.c`
(random JSON read and written back, and mutated JSON read or refused),
`tests/temporal_test.c` (Temporal on fixed lists of strings, dates, bags
and options, which diff-node compares with the vendored polyfill, and
Date's strings, `Date.UTC` and getters, which it compares with Node's),
`tests/regex_test.c` (2,000 random regular expression literals, each
classified alike whether pushed whole or a byte at a time; `--print` gives
`diff-node` random literals and matches to compare with Node) and
`tests/names_test.c` (the table of prototype names is sorted, each name
once, so its binary search finds every entry).
`mise run test` also runs `bench --smoke` ([benchmarks.md](benchmarks.md)), which times nothing.

The examples (`examples/`, `mise run examples`) are built from the
amalgamation and the public header alone, under AddressSanitizer and
UBSan, and each one's output must equal `examples/expected/<name>.txt`.
`mise run wasm-test` runs the browser example's Node script against the
module it builds, and checks that its default limits are the header's.
When you change what an example prints, change its expected file in the
same commit. When you change `examples/hello.c`, change its copy in
`docs/guide/getting-started.md` too: `mise run examples` fails while the
two differ.

## Tasks

```sh
mise run build              # the library (freestanding) and the test programs
mise run test               # everything below, the way `mise run test` at the root runs it
mise run test --full        # also fail every allocation of every spec case
mise run shapes [--check]   # write src/shapes.h and src/shapes_table.h from src/shapes.rules, or check them
mise run shapes-test        # the shapes generator's own tests
mise run check-parameters   # at most three parameters per function
mise run check-recursion    # no cycle in the call graph
mise run check-freestanding # builds for every release target, wasm32 included, with no C library
mise run asan               # the tests under AddressSanitizer and UBSan
mise run diff-node          # the spec against JavaScript
mise run amalgamate [--check] # include/staticjs.c and staticjs_wasm.c, the single-file library, or check them
mise run api [--check]      # include/staticjs.api.json, the API description, or check it
mise run wasm-test          # staticjs.wasm, built as the release builds it, run in Node, with the browser example
mise run examples           # examples/, built from the amalgamation under the sanitizers, their output checked
mise run dist [debug]       # dist/: the header, amalgamation, API description and libstaticjs.a for this machine
mise run release -- <version> --build-only  # build, verify and pack every target: a look at a release
mise run release -- <version> [--skip-containers] [--skip-tests] [--publish]  # a dry run, or with --publish the release
mise run bench [filter] [--quick]                  # the benchmarks, optimized: throughput and memory
mise run bench-compare <commit> [--runs=3] [--store] # this tree against another commit, side by side
mise run bench-engines [filter] [--quick]          # staticjs against Node and quickjs-wasi (benchmarks.md)
```

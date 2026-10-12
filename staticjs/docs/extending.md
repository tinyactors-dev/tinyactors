# Extending staticjs

How to add to staticjs and keep it correct, fast and reviewable. Each section
is a goal and the steps that reach it. They come from building the language,
its built-ins, shapes, owned mutation, BigInt, `Map` and JSON. Where a step
exists because something went wrong, it says what happened.

The detailed references are:

- `../AGENTS.md`: the rule, the C rules, tests and benchmarks.
- `../README.md`: the language, and where it faults.
- `builtins.md`: adding a built-in. Section 10 lists the mistakes already
  made.
- `matcher.md`: the event stream, shapes and rewrites.
- `mutation.md`: owned mutation and how to roll it back.
- `../src/README.md`: what each source file holds.

This guide links them into procedures. It does not repeat them.

---

## To decide whether a feature belongs in staticjs

1. **State what JavaScript does,** in Node 22, including every case where it
   coerces, reaches a prototype, compares identities, or changes a value
   another name can see.
2. **Apply the rule.** A program staticjs accepts gives JavaScript's value,
   or a fault, never a different value. For each case, choose:
   - **JavaScript's value**, implemented exactly;
   - **a fault** when it runs: where JavaScript coerces an object, depends
     on identity, or gives a value staticjs cannot represent (a lone
     surrogate, an array hole);
   - **a refusal** when compiled: `unsupported` for JavaScript that staticjs
     leaves out, and `syntax` only where JavaScript also refuses.
     `diff-node` checks both, and it caught a wrong `syntax` refusal
     (`[0 || 1, 0 ?? 2]`, `38adf25`).
3. **Leave out what can't be done exactly and cheaply,** with a fault. For
   example, the reviver of `JSON.parse` faults.
4. **Prefer faulting over a near-match.** A needless fault (refusing what
   JavaScript converts without coercing an object) is a defect too, but a
   lesser one. Check with Node before refusing.

## To write a spec case

1. **Pick the file in `tests/spec/` for the topic.** The format is at the top
   of `tests/spec_test.c`:
   - `--- value` for a value;
   - `--- fault words` for a fault when it runs;
   - `--- error unsupported words` or `--- error syntax words` for a refusal
     when compiled;
   - `@shapes` to name each function's shape;
   - `@clock <epoch nanoseconds>` to set the host clock that `Temporal.Now`
     reads (`docs/temporal.md`, "Host clock");
   - `@seed <n>` to set the seed `Math.random` draws from, a nonzero
     int32 (`docs/temporal.md`, "Host seed"): diff-node runs the case in
     a `node --random-seed=<n>` of its own.
2. **Never type an expected value by hand.** Put the expression in a small
   Node script, `eval` it, and paste what it prints. The cases in
   `group_by.spec`, `array_methods.spec`, `numbers.spec`, `bigint.spec`,
   `map.spec` and `json.spec` were made this way. Hand-written values were
   wrong more often than the code.
3. **Know where `JSON.stringify` and the staticjs writer differ:**

   | Value | `JSON.stringify` | Do this in the case |
   |---|---|---|
   | `undefined` in an array | `null` | test `x === undefined` |
   | `NaN`, `Infinity` | `null` | wrap in `String(...)` |
   | `-0` | `0` | `1 / x === -Infinity` |
   | `Map`, `Set` | `{}` | spread into an array |
   | BigInt | throws | `String(...)` |
4. **Don't write a case that staticjs refuses for some other reason.** Each
   of these has wasted a test run:
   - `===` on two objects;
   - `+` between a string and a number;
   - `Object.is`;
   - an array, a value read through it, and a mutation of it in one
     expression (an alias);
   - a callback that mutates a captured variable.
5. **Write the deliberate faults as fault cases,** where Node gives a value
   and staticjs refuses. The choice then stays visible, and stays a fault.
6. **Check with `build/spec_test file.spec`, then `mise run diff-node`.** The
   second runs every case in Node too, and compares.

## To add a built-in function or method

1. **Follow `builtins.md` from top to bottom,** with its checklist. In short:
   - **Leaf or loop:** a leaf unless JavaScript calls back; a loop calls back
     once per element.
   - **Register it:** an ID in its receiver's group, and move the group's
     bound if it is the last; a `NAMES` column; `sjs_builtin_loops` if it
     loops.
   - **References:** arguments are borrowed, and `call->result` is owned and
     kept current.
   - **Lookups** go through the hash index.
   - **Fuel** is charged for the work done.
2. **Write the spec cases first** (above).
3. **Put new code at the end of `builtins.c` (or its own file),** behind a
   `noinline` function if it would grow a hot function. Placement moves
   unchanged loops by up to 20% (see "To tell a real slowdown from code
   placement").
4. **Don't name a built-in in a fault message in shared code** (`call()`,
   `sjs_loop_start`, `sjs_loop_step`, helpers), not even with a ternary on
   the ID. Check with `git diff -U0 | grep sjs_vm_fault`.
5. **Update every list that names similar built-ins:** `README.md`,
   `builtins.md`, `matcher.md` and comments. Use
   `git grep '<similar built-in>'` to find them.
6. **Add a bench case if it walks data** ("To add a benchmark case").
7. **Run the checks** ("To verify a change") and commit ("To commit").

## To make a callback run in C (a shape fast path)

1. **Find or write the shape.** `src/shapes.rules` holds the rules
   (`matcher.md` sections 3 and 11). Reuse an existing shape when the
   callback is the same: `Object.groupBy`, `Array.from` and `Map.groupBy`
   all reuse PLUCK, and `findLastIndex` reuses the predicate shapes.
2. **If you change the rules:** run `mise run shapes`, commit the generated
   `src/shapes.h` and `src/shapes_table.h`, and run `mise run shapes-test`.
3. **Pair the shape with the built-in** with a row in `shape_runners` in
   `builtins.c`.
4. **In the step:**
   - check every precondition on every element;
   - for any value the bytecode would fault on, call back
     (`call_element`), so the fault keeps its message and position;
   - keep stack room equal to what the bytecode would push;
   - charge one unit per lookup;
   - do one unit of work per step, so evaluation still pauses inside the
     loop.

   `Object.groupBy` first grouped by any plucked value and faulted at the
   loop's position. That was the one bug the reviewers of the stdlib
   workflow caught.
5. **Give every fast-path case an `@shapes` line,** or nothing shows the C
   path ran. The spec suite also runs every case with shapes off and
   compares outcome and fuel.
6. **Commit the plain built-in first, then the fast path,** and quote
   `bench-compare HEAD <case> --quick` with the fast path uncommitted
   against the plain one (old → new, and fuel). A number for the new build
   alone measures nothing.

## To desugar syntax with a rewrite rule

Use this when a form should mean something the compiler can't compile
directly (`o.items.push(x)`, `s += x`). The examples are `MUTATE_PATH`,
`APPEND` and `APPEND_SUM`.

1. **Read `matcher.md` section 12.** A rewrite changes the code, so it runs
   with shapes on and off: `no_shapes` turns off only tags and their fast
   paths.
2. **Declare it** in `src/shapes.rules` with `rewrite NAME;` and anchored
   rules. These are left-spine forms whose nodes all start where the root
   name starts: `_` for any child, `...` for any rest.
3. **Handle the report in the compiler.** Add a case to `on_rewrite`, then
   rewrite the instructions already emitted at the node's end, as
   `path_mutation_end` does. Ownership and scope checks stay in the
   compiler; the matcher only matches structure.
4. **If the form isn't a left spine** (a template, a bracket that takes
   nothing before it), the lane can't anchor it. Recognize it in the
   compiler and document why, as `matcher.md` 12.7–12.8 does for the
   template append.
5. **Test it:** spec cases, `matcher_test` reports, and an
   `api_test` case that compiles with `no_shapes`.

## To add syntax

1. **Parser:** emit events in evaluation order, each with its `node` and
   `start` (`matcher.md` section 1). Never pull input or recurse: every
   stage is a state machine fed by its caller.
2. **Public header:** add new event kinds to `include/staticjs.h`.
   Declare each in `src/shapes.rules`'s event language, or the generator
   fails, and regenerate.
3. **Compiler:** handle the events as they arrive. If a decision needs
   later events, buffer only what it needs and rewrite after the fact, as
   assignment paths do.
4. **Classify each refusal:** `syntax` only where JavaScript's parser
   refuses too (reassigning a `const` is `unsupported`, because JavaScript
   only throws when the line runs). Check each one with `diff-node`.
5. **Keep the last-read moves sound:** any write to a slot after a read
   must cancel the move of earlier reads (`let`, `75af512`).
6. **Put mixing checks and similar per-expression state per expression:**
   reset them where one expression ends inside a bracket (the `??` bug).

## To add a value kind

The examples are BigInt, `Map` and iterators.

1. **Add the kind and heap type in `src/internal.h`.** A new kind
   renumbers the ones after it, so bump `SJS_VERSION_MINOR`.
2. **Find every switch on kind and handle the new one:** the writer,
   `typeof`, `sjs_is_primitive`, release, `sjs_same_value_zero`,
   `hash_value`, property reads, `JSON.stringify`, and the shape fast paths,
   which must fall back for it. Use `git grep 'case SJS_SET'` to find them.
3. **Allocate only through the context,** and release without recursion.
4. **Bound its size with a limit in `sjs_limits`,** and charge fuel in
   proportion to its work. That makes `2n ** 100000000n` a limit fault
   instead of running out of memory.
5. **Add public API in `include/staticjs.h`,** at most three parameters per
   function (use a struct: `sjs_map_entry`).
6. **Reuse the existing containers.** A map is a set's index with a value
   beside each key.
7. **Write a randomized comparison with Node** if the kind computes
   (BigInt: 500,000 checks; "To make results bit-identical").

## To add an operation that mutates

1. **Read `mutation.md`.** A value may change only while one holder has it:
   - **When compiled,** the variable must be the function's own, and never
     copied, captured or passed.
   - **When run,** the value must have one holder (`sjs_value_unique`).
     This catches what the compiler cannot see.
2. **Extend the compile-time ownership checks** (`binding.use`,
   `note_use`), not only the run-time one.
3. **Removing from a container** compacts it and keeps the hash index exact
   (`mutation.md`, "Removal"). It also shrinks the container once it is
   under a quarter full.
4. **Tag every hunk with a comment containing `MUTATION`.** Put the tests
   in `tests/spec/mutation.spec` and `test_mutation_*`, update
   `mutation.md`'s tables, and end the commit body with `Mutation: yes`.
5. **Keep optimizations that don't need mutation separate:** the in-place
   string append is tagged `STRING_APPEND` and survives a rollback.

## To roll mutation back

1. Run `git log --grep '^Mutation: yes'`, and revert the commits newest
   first.
2. Or remove what `git grep -n MUTATION -- staticjs` marks, and restore what
   the comments say.
3. Restore the spec messages that `mutation.md`, "Rolling it back", lists.
   Keep anything tagged `STRING_APPEND`.

## To make number results bit-identical to V8

1. **Use no C library.** Use builtins that compile to an instruction on
   every target (`__builtin_elementwise_sqrt`). `__builtin_sqrt` calls
   libm. `mise run check-freestanding` builds every release target and fails
   on any libm call or 128-bit division.
2. **Keep `#pragma STDC FP_CONTRACT OFF`** in files with float arithmetic,
   so no target fuses a multiply and an add.
   - **The reference for Math is V8 without contraction: Node on x86_64.**
     V8's `ieee754.cc` is plain C++, and what the C compiler fuses decides
     its last bit. Node's build (`common.gypi`, `toolchain.gypi`) turns
     contraction off only for s390x and ppc64; on x86_64 the baseline
     instruction set has no fused multiply-add, so nothing is fused, but on
     arm64 the compiler fuses where it chooses, and those choices (GCC's,
     for Node's release builds) cannot be reproduced. So staticjs computes
     what V8 computes without contraction, the same bits on every target
     (its own arm64 build gives the x86_64 build's bits), and those are
     Node's on x86_64. Node 22 on arm64 differs in the last bit for some
     inputs: in a million inputs per function, about 6,500 for `sin`,
     5,000 for `tan`, 3,400 for `asin`, 2,700 for `cos`, 1,900 for `log2`,
     1,100 each for `expm1` and `acos`, 100 to 500 for `atan`, `log1p`,
     `pow`, `exp` and `atan2`, 30 to 130 for the hyperbolic functions, 2
     for `log` and none for `log10` and `cbrt`. diff-node therefore compares the fixed
     Math lists of `number_test --print` (tags U, V and W) only where
     `process.arch` is x64, and says on any other host that it skipped
     them; every other check, and every Math case in `tests/spec`, runs
     there too: a spec case uses inputs whose value is the same in Node on
     x86_64 and on arm64.
   - **parseInt is the exception, by the maintainer's decision.**
     `parseInt` in a radix that is not a power of two (`radix_accumulate`
     in `src/number.c`, `RADIX_FUSED`) rounds as Node does on the platform
     staticjs runs on: fused on aarch64, as Node's arm64 build is. So
     staticjs's arm64 build gives other `parseInt` bits than its x86_64
     build, each equal to its own platform's Node, while Math gives V8's
     bits without contraction on every target.
3. **Where JavaScript's specification isn't one IEEE operation,** copy V8's
   algorithm (`Math.hypot`, `parseInt` in other radixes). musl's libm (MIT)
   can be cloned from `git.musl-libc.org`; keep its notice.
   - **What V8 runs for Math.** Node 22 is V8 12.4.254.21. Its Math functions
     and `**` run `v8::base::ieee754` (`src/base/ieee754.cc`), V8's own
     port of fdlibm, never the C library: fetch it from
     `raw.githubusercontent.com/v8/v8/12.4.254.21/src/base/ieee754.cc`.
     `src/math.c` ports it line for line, with fdlibm's and V8's notices.
   - **pow.** `Math.pow` (`src/builtins/math.tq`, `MathPowImpl`) and `**`
     (`Exponentiate` in `src/builtins/number.tq`, the interpreter's and
     baseline's `Generate_ExponentiateWithFeedback` in
     `src/ic/binary-op-assembler.cc`) both end in `Float64Pow`, which every
     tier lowers to a call of `base::ieee754::pow` (`ieee754_pow_function`
     in `src/codegen/external-reference.cc`); the parser's constant folding
     (`src/parsing/parser.cc`) and TurboFan's and Turboshaft's
     (`machine-operator-reducer.cc`, `machine-optimization-reducer.h`) call
     the same function. 12.4 has a single `pow`: no `legacy::pow`, no
     `src/numbers/math.cc`, no wrapper. It is fdlibm's `__ieee754_pow` with
     JavaScript's rules first: `x ** ±0` is 1 even for NaN, NaN in gives
     NaN, and `±1 ** ±Infinity` is NaN (C's pow gives 1); `y = ±1` gives
     `x` or `1 / x`, `y = 2` gives `x * x`, and `y = 0.5` with `x >= +0`
     gives `sqrt(x)` (the correctly rounded instruction); a negative base
     needs an integer exponent (`yisint`), else NaN; then the log2/exp2
     algorithm, with `scalbn` (exact: one rounding) for a subnormal result.
     Optimized code rewrites `x ** 2` to `x * x`, `x ** 0.5` to a square
     root and `x ** 0` to 1, which give the same values, except that
     Turboshaft's `x ** 0.5` takes `sqrt(-0)`, -0, where `pow` gives +0;
     Node's cold code, which diff-node runs, gives +0, as staticjs does.
     V8 returns a signalling NaN where `src/math.c` returns a quiet one,
     which nothing in staticjs can tell apart.
   - **exp, expm1 and the logarithms.** `Math.exp`, `expm1`, `log`,
     `log1p`, `log2` and `log10` (`src/builtins/math.tq`) convert their
     argument and call `Float64Exp` and the others, which every tier lowers
     to a call of `base::ieee754::exp` and so on; there is no JavaScript
     special case before them. In fdlibm's code, V8's `exp` returns `E` for
     `exp(1)` (the algorithm alone gets its last bit wrong), and `log10`
     calls V8's own `log`. They differ from glibc's in the last bit for
     many inputs (`Math.exp(125.10776284998587)`, `Math.log(850)`), so a C
     library's would not do even where one exists.
   - **No glibc.** V8 can take `sin` and `cos` from glibc
     (`third_party/glibc`, `V8_USE_LIBM_TRIG_FUNCTIONS`, flag
     `--use-libm-trig-functions`), but Node 22's build does not: `node
     --v8-options` has no such flag, so `sin` and `cos` are fdlibm's too
     (the same file's `fdlibm_sin` and `fdlibm_cos`, which without that
     macro are named `sin` and `cos`, and which `ieee754_sin_function`
     points to). They differ from glibc's in the last bit for many inputs
     (`Math.sin(18)`, `Math.cos(0.1)`, `Math.tan(4)`), as `asin`, `acos`,
     `atan` and `atan2` do. `Math.atan2(y, x)` converts y first.
     `__kernel_rem_pio2` calls the C library's `scalbn` and `floor`, both
     exact, so `src/math.c` uses fdlibm's own `scalbn` and number.c's
     floor; V8's `volatile` constants (atan2's `tiny`, atan's `atanlo[3]`)
     only keep additions that round to the same bits.
   - **Hyperbolic functions and cbrt.** `Math.sinh`, `cosh`, `tanh`,
     `asinh`, `acosh`, `atanh` and `cbrt` convert their argument and call
     `base::ieee754::sinh` and the others, with no JavaScript special case.
     The hyperbolic ones call V8's own `exp`, `expm1`, `log`, `log1p` and
     the square root instruction, so `src/math.c` calls its ports of those;
     V8's `sinh` and `cosh` are its own rewrite of fdlibm's (an overflow
     threshold of 710.4758600739439, a `cosh(x) = 1` cut at 2^-55), and its
     `cbrt` is FreeBSD's `s_cbrt.c` (a rounding of t to 23 bits through its
     bits), not fdlibm's. glibc's last bit differs for many inputs
     (`Math.cbrt(0.1)`, `Math.cosh(0.42)`, `Math.asinh(2.02)`). `acosh` of
     x < 1 and `atanh` of |x| > 1 give a signalling NaN in V8, a quiet one
     here; `atanh(±1)` is ±Infinity.
4. **Add a sweep:** `number_test --print N` writes lines that
   `diff-node` compares with Node on every test run. Use 100,000 cases per
   function, or for a Math function a fixed list of at most 10,000 inputs
   (tag W for `pow`: specials, the thresholds of fdlibm's ranges, integers,
   halves; tag U for a one-argument function, `U <name> <x> <f(x)>`, which
   diff-node checks as `Math[name](x)`, so a new one needs only its name in
   `UNARY_NAMES`), and run a million once before committing. `pow`'s million
   pairs (seeded: random bits, moderate magnitudes, integers, results next
   to overflow and underflow, bases next to 1 to huge powers, subnormal
   bases, special exponents, negative bases) agreed bit for bit, on
   x86_64 and on wasm32 (the same source built with `zig cc -target
   wasm32-freestanding` and run in Node), and so did a million inputs to
   each of `exp`, `expm1`, `log`, `log1p`, `log2` and `log10` (random bits,
   the main range, values next to 1 or 0, the thresholds of each range,
   integers, subnormals, powers of two and ten, multiples of ln 2), and to
   each of `sin`, `cos`, `tan`, `asin`, `acos` and `atan` and pairs to
   `atan2` (also the doubles next to multiples of pi/2 and pi/4, huge
   arguments, the doubles closest to multiples of pi/2, values in [-1, 1]
   and next to +-1 and 0.5; for `atan2`, every special case and quotients
   next to 2^60), and to each of `sinh`, `cosh`, `tanh`, `asinh`, `acosh`,
   `atanh` and `cbrt` (also the overflow threshold of `sinh` and `cosh`,
   values near 22, 2^28 and 1, and for `cbrt` subnormals, every binary
   exponent and cubes of integers). A function of its own list, such as the
   trigonometric ones, is printed with `print_trig_input` or
   `print_hyperbolic_input` beside `print_unary_input`; a two-argument one
   has its own tag (V for `atan2`).

## To verify a change

Run these in `staticjs/`, with `LANG=C.UTF-8 LC_ALL=C.UTF-8` set for Ruby:

1. While iterating, run `build/spec_test some.spec` for one file.
2. Before committing, run:
   - `mise run test`: the suite, checks, `diff-node` and bench smoke;
   - `mise run asan`;
   - `mise run test --full`: every allocation of every case fails in turn;
   - `mise run --force test --full` if mise says the sources are up to
     date.
3. After changing the build, a target or numeric code, also run
   `mise run check-freestanding`.
4. The repository root's `mise run test` also tests other projects.

## To add a benchmark case

1. In `bench/bench.c`:
   - name it `group.subject[.variant]`;
   - give it a unit;
   - give it sizes: L for linear work, Q for work quadratic today;
   - give it the value at its smoke size, generated in Node at every size
     it runs.
2. **Make sure the source does the work the name says.** `Array.from(rows)`
   shares the array, so it measures nothing.

## To measure a change

1. While iterating, run `mise run bench-compare <base> <filter> --quick`.
   Before committing, run it without `--quick`, and rerun noisy rows
   (`?`) with `--runs=8`.
2. **Quote only what it flags,** with the machine hash, in the commit body.
   A change marked `?` is not evidence either way.
3. **Check the rows you didn't mean to touch,** at least `eval.guard` and
   the loops near your code.
4. **To find where time goes,** run the bench under callgrind or cachegrind
   on one case, and count instructions per element.

## To tell a real slowdown from code placement

A row that slows on a change it doesn't run is usually placement.
`eval.array.lastIndexOf` has been flagged on four unrelated changes.

1. Compile `src/` at both commits with the bench's flags, into a
   scratch directory: `git archive <commit> staticjs/src staticjs/include`,
   then the pinned clang with `-O2 -fno-stack-protector -fvisibility=hidden
   -ffreestanding`.
2. Disassemble the case's per-element functions with the pinned
   `llvm-objdump` (`mise which llvm-objdump`), and diff them.
3. **Identical instructions mean placement.** Say so in the commit, with the
   evidence.
4. **If they differ,** it is real: fix it. A helper pushed out of line by a
   new test at the top of `sjs_loop_step` cost every C-path loop 10–14%,
   and `always_inline` and `noinline` fixed it. Keep new code at the ends of
   files and out of `execute()`'s cases.

## To make something faster

1. **Measure first:** bench, callgrind and the comparison with Node and
   QuickJS. A tighter dispatch loop looked promising and made nothing
   faster.
2. **What paid off, in order:**
   - inline push, pop and reference counting;
   - hash indexes, so lookups don't scan;
   - checking for recursion once per loop, not per call;
   - keeping one callback frame per loop;
   - shapes in C;
   - updating a value nothing else holds in place: objects, arrays and
     strings; also BigInt operands and iterators.
3. **What did not pay:** fusing `filter` into `map` while still calling each
   callback, which mispredicted more branches than it saved.

## To delegate work to agents

1. **Run agents one after another in one working tree,** never two editing
   at once. Use no worktrees; commit and push on the feature branch.
2. **Give each agent a brief with:**
   - the base commit;
   - which documents to read;
   - what the previous agent handed over;
   - the semantics to match;
   - the quality bar: Node-generated cases, `test --full`, `bench-compare`
     with placement proven;
   - the tags;
   - the commit format;
   - what to return, including notes for the next agent.
3. **When it finishes:**
   - check that the tree is clean and the branch is pushed;
   - run `test --full`;
   - write your own edge cases with values from Node;
   - commit them as a `test(staticjs)` commit.

   Spot checks of this kind found a wrong value and a leak (`o.sort(f)` on
   an object with its own `sort`), and a wrong syntax refusal.
4. **A reviewer agent is worth its cost when it is adversarial:** it
   generates edge cases from Node, runs them with shapes on and off, greps
   fault messages and measures fast paths. A reviewer that only reads a
   checklist missed what the checklist named.
5. **Don't commit an agent's work in progress yourself,** even when a hook
   asks: it puts unreviewed code on the branch.

## To commit

1. **Scope the subject with the component** (`feat(staticjs)`,
   `fix(staticjs)`, `perf(staticjs)`, `test(staticjs)`, `docs(staticjs)`),
   and say what a reader gains.
2. **In the body, give:** the semantics and what faults; which checks
   passed (name `test --full`); `bench-compare` rows with the machine hash;
   memory and fuel changes; what was left out. End with `Mutation: yes` for
   mutation work.
3. **Make small commits:** the plain feature, then its fast path, then docs
   if separate.
4. **Never rewrite pushed history** to fix a message. Say it in the next
   commit if it matters.

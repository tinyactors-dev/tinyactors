# Running other people's expressions: QuickJS, sandboxed JavaScript and rule languages, read for staticjs

**Date:** 2026-10-14
**Used by:** the front page of staticjs's site (`staticjs/site/index.md`).
This is market research for positioning, not a decision.

## Question

An application developer must run an expression, a rule or a small
program that someone else wrote: a guard in a statechart, a filter a user
typed, a pricing formula, a computed field, a rule in a workflow tool.
What do they reach for today, what does each option promise, and what goes
wrong in practice? This note records what the main options say about
themselves and what their users and security advisories report, so that
staticjs can be described against them honestly.

It does not repeat [tinyjs](tinyjs.md)'s prior-art table (Duktape, XS and
xsnap, QuickJS in wasmtime, QuickJS with Asyncify), which covers resumable
execution and snapshots rather than this use.

## Caveats

- What a project says about itself is quoted as its claim, not checked.
- CVEs are cited from the CVE records (cve.org) and show that a class of
  bug occurs; they are not a count or a ranking of how safe a project is.
- Pain points come from documentation, advisories and engineering posts.
  Searches for first-hand Hacker News threads on QuickJS sandboxing and on
  JSON Logic's readability found none; where a point rests on a single
  secondary source, it says so.
- Figures for staticjs come from this repository
  (`staticjs/bench/engines/REPORT.md`): one run on one 4-core x86_64
  machine, October 2026. They show proportions, not a promise.

## Summary

| Option | What it is | Stops a runaway program | Bounds memory | Same result on every machine | Author learns |
|---|---|---|---|---|---|
| QuickJS, QuickJS-NG (C) | all of modern JavaScript, small embeddable engine [1][2] | interrupt handler the host writes [3] | `setMemoryLimit` [3] | host's job (see "Determinism") | nothing new |
| quickjs-emscripten, quickjs-wasi (wasm) | QuickJS(-NG) in WebAssembly for JavaScript hosts [3][4] | interrupt handler, deadline [3] | memory limit [3] | host's job | nothing new |
| Javy (Shopify Functions) | QuickJS in wasm, run under an instruction budget [5][6] | the wasm runtime's instruction count | wasm memory | instruction count, not time [5][6] | nothing new |
| Node `vm` | a second V8 context | `timeout`, which promises escape [7] | no | no | nothing new |
| vm2 | a sandbox over `vm` | inherits `vm` | no | no | nothing new |
| isolated-vm | V8 isolates from Node | timeouts (unverified here) | memory limit per isolate (unverified here) | no | nothing new |
| SES / Hardened JS | object-capability confinement in one realm [10] | no: "indefinite amount of time" [10] | no [10] | no | nothing new (strict mode) |
| workerd (Cloudflare Workers) | V8 isolates as a multi-tenant runtime [11] | CPU limits of the platform (unverified here) | memory limits of the platform (unverified here) | no | nothing new |
| CEL | non-Turing-complete expression language [12][13] | a cost budget, and a static cost estimate [13] | via cost | yes, by design | CEL |
| JSON Logic | rules as JSON, no loops or setters [14] | no loops | not stated | "deterministic computation time" [14] | JSON Logic's operators |
| Expr (Go) | Go-centric expression language [15] | "designed to prevent infinite loops" [15] | not stated | not stated | Expr |
| JSONata | Turing-complete query and transform language [16] | `timeout`, `stack` options [16] | via `stack` | not stated | JSONata |
| Jexl (npm) | JavaScript-like expression language [17] | not stated | not stated | not stated | Jexl's grammar |
| filtrex | spreadsheet-like expressions compiled to JS functions [18] | no loops | not stated | not stated | filtrex's grammar |

Five findings matter most for staticjs:

1. **Sandboxes over a general engine keep failing in the same place: the
   boundary between guest and host.** vm2 was discontinued in 2023 after a
   sandbox escape its maintainer said could not be properly fixed, and a
   new escape through `Promise.prototype.then` followed in January 2026
   [8]. Node says `vm` "is not a security mechanism" [7]. isolated-vm's
   README calls running untrusted code "an extraordinarily difficult
   problem" [9]. n8n's expression evaluator allowed remote code execution
   by authenticated users (CVE-2025-68613, critical) [19], and the npm
   evaluator expr-eval allowed code execution through prototype pollution
   (CVE-2025-13204) [20]. Figma left the Realms shim for QuickJS in wasm
   after escapes were found in 2019 [21].
2. **A timeout is not the same as termination.** Node's `vm` `timeout` is
   escaped by a promise callback, which runs after the timed call returns
   [7]. JSONata is Turing-complete and asks hosts that run user
   expressions to set `timeout` and `stack` [16]; a regular expression in
   its `$toMillis` could still backtrack superlinearly (CVE-2026-52746)
   [22]. QuickJS stops runaway code only through an interrupt handler the
   host writes [3]. Even CEL, which cannot loop, ships a runtime cost
   budget and a static estimate because an expression over a large list
   can still be expensive [13].
3. **Time limits differ between machines; teams want a count instead.**
   Shopify ran functions under a 5 ms limit, noted it depends on the
   machine and its load, and said it was exploring "a gas-like approach
   as a machine- and situation-independent measurement" [5]; its docs now
   limit a function to 11 million WebAssembly instructions for carts up
   to 200 lines, and say JavaScript, being interpreted, costs "many more"
   instructions per line than Rust [6].
4. **JavaScript engines do not agree on the last bits of `Math`.**
   ECMAScript leaves `sin`, `cos` and `pow` approximate. WebKit's tracker
   has a report that JIT-compiled code gave a different `pow` from the
   interpreter, from a developer who avoided the built-in functions
   "because we need deterministic results" [23]; Firefox's has 32-bit and
   64-bit builds disagreeing on `Math.sin(Math.PI)` [24]. staticjs's own
   comparison with Node compares `Math` on x86_64 only, because Node on
   arm64 computes some last bits differently (fused multiply-adds)
   (`staticjs/docs/internals/testing.md`).
5. **Rule languages are pure and end, but every one is a new language.**
   CEL [12], JSON Logic [14], Expr [15], JSONata [16], Jexl [17] and
   filtrex [18] each ask the author to learn its syntax and semantics, and
   ports of one language disagree: a Python JSON Logic port says other
   Python ports "don't emulate all the JavaScript operator behaviors quite
   right" [25]. CEL's cost model also shows that "cannot loop" alone does
   not bound cost [13].

## QuickJS and its WebAssembly builds

**What they say.** Bellard's QuickJS: "367 KiB of x86 code for a simple
hello world program", "very low startup time" (a runtime's life cycle in
"less than 300 microseconds"), and "nearly 100%" of test262 for ES2025
[1]. QuickJS-NG is a fork "with the intent of reigniting its
development", aiming at "the latest ECMAScript specification" [2].
quickjs-emscripten: "Safely execute untrusted Javascript in your
Javascript" [3]. quickjs-wasi runs QuickJS-NG compiled to wasm, with
snapshot and restore of the whole VM (unverified: from package listings,
not its README) [4].

**Who uses it for untrusted code.** Figma's plugins (QuickJS cross-compiled
to wasm, since 2019) [21]; Shopify Functions through Javy, which embeds
QuickJS [5]; tools and agents that sandbox generated code (the search
found several; none checked here).

**What users must handle.**

- *Termination is the host's code.* quickjs-emscripten's runtime has
  `setInterruptHandler`, `setMemoryLimit` and `setMaxStackSize`; its
  example interrupts "after 1024 calls to the interrupt handler" [3]. What
  counts as too long, and whether that is the same on every machine, is
  the host's decision.
- *Lifetimes.* Handles into the wasm heap are not garbage-collected; each
  must be `dispose()`d, and disposing a context with a handle still alive
  throws [3].
- *Size and async.* The default quickjs-emscripten build is about 500K;
  the asyncified build is 1M and "bigger and runs slower" [3]. A user
  report says a Javy update roughly doubled a Shopify Function's wasm and
  pushed it past a 256 KB cap (unverified: one forum post) [26].
- *Memory safety of the engine.* QuickJS is C with a large surface: CVEs
  include use-after-free in async functions with closures
  (CVE-2023-48184), in the standard library's promise rejection check
  (CVE-2025-62491) and, in QuickJS-NG up to 0.11.0, in Atomics
  (CVE-2026-1144) [27]. In wasm, such a bug corrupts the guest's linear
  memory rather than the host's.
- *Start-up.* In staticjs's comparison, a fresh `QuickJS.create` in
  quickjs-wasi (a new wasm instance from a precompiled module) took about
  1.4 ms, and `evalCode` of a one-line guard about 8.9 µs
  (`staticjs/bench/engines/REPORT.md`).

**MicroQuickJS**, Bellard's 2025 engine for embedded systems, runs a
subset of JavaScript in as little as 10 kB of RAM; Simon Willison found it
"very well suited to the sandbox problem", with time and memory limits
built in and a regex engine that calls the interrupt handler while
backtracking [28]. It was not measured here.

## Sandboxed V8 and Hardened JavaScript

- **Node `vm`.** "The `node:vm` module is not a security mechanism. Do not
  use it to run untrusted code." Its `timeout` is escaped by a promise:
  the docs' own example schedules an endless loop with
  `Promise.resolve().then(...)` under a 5 ms timeout, and "the scheduled
  loop is never interrupted" [7]. In staticjs's comparison, running a
  compiled guard with `vm.Script.runInContext` took 1.4 µs and a fresh
  context about 330 µs (`REPORT.md`).
- **vm2.** CVE-2023-37466: "The library contains critical security issues
  and should not be used for production. The maintenance of the project
  has been discontinued." Development resumed; CVE-2026-22709 (January
  2026) is another escape through promise callbacks, fixed in 3.10.2 [8].
- **isolated-vm.** In "maintenance mode"; "Running untrusted code is an
  extraordinarily difficult problem which must be approached with great
  care" [9]. A secondary summary says its README warns that V8 cannot
  recover from running out of memory, so hostile code can crash the
  process; the README fetched for this note did not show that sentence
  (unverified) [9].
- **SES / Hardened JavaScript.** Confines strict-mode programs with no
  ambient authority, but says guests may "execute for an indefinite amount
  of time" and "allocate arbitrary amounts of memory"; `Math.random()` is
  shared state, a covert channel [10].
- **workerd.** Check Point Research (August 2026) calls it "an in-process
  sandbox that relies entirely on V8 to isolate untrusted code" and
  reports five vulnerabilities in its native code, two rated critical,
  fixed in v1.20260619.1 [11]. Deno's isolates were not researched for
  this note.

## Duktape and Boa

Duktape is in [tinyjs](tinyjs.md#prior-art-and-the-case-against-building):
ES5.1, last release 2022, and its timeout hook "is not a reliable
mechanism against deliberately malicious code". Boa describes itself as
"an experimental JavaScript lexer, parser and interpreter written in
Rust" supporting "more than 90%" of the latest specification [29]; its
README says nothing about limits or sandboxing.

## Rule and expression languages

- **CEL.** "CEL programs cannot loop forever" and "a CEL program only
  computes an output from its inputs" [12]. Kubernetes: "CEL is non-Turing
  complete and offers a variety of production safety controls to limit
  execution time"; every expression runs under a runtime cost budget, and
  the API server may reject an expression whose worst-case estimated cost
  is too high [13].
- **JSON Logic.** "a small, safe way to delegate one decision": "no
  setters, no loops, no functions or gotos", "deterministic computation
  time", "We never `eval()`" [14]. Rules are nested JSON operators. Ports
  differ: the Ruby port implements a narrower feature set, and a Python
  port exists because others did not match the JavaScript one [25].
- **Expr.** "a Go-centric expression language designed to deliver dynamic
  configurations": memory-safe, side-effect-free, "designed to prevent
  infinite loops"; the README names users including Google, Uber,
  ByteDance, Argo and OpenTelemetry [15].
- **JSONata.** "a Turing-complete expression language, and as such, it is
  possible to write unbounded, or infinite loops"; hosts of user
  expressions should set `stack` and `timeout` [16]. CVE-2026-52746:
  superlinear regex backtracking in `$toMillis` [22].
- **Jexl.** "Powerful context-based expression parser and evaluator"; no
  safety claims in its README [17]. Its last npm release, 2.3.0, is from
  September 2020 (npm registry).
- **filtrex.** For "allowing end-users to enter arbitrary expressions
  without p0wning you"; "Expressions cannot escape the sandbox";
  compiled to JavaScript functions [18].
- **expr-eval** (npm). CVE-2025-13204: prototype pollution leading to
  "arbitrary code execution" by anyone with access to the evaluation
  interface; fixed in a fork [20].

A recurring note in the JavaScript evaluators the search found
(`jse-eval`, `expression-eval`, `simple-eval`): each warns that parsing a
restricted grammar is not a sandbox (unverified beyond the search
results' quotations) [30].

## How staticjs compares, on this repository's evidence

These are the claims the site may make, with where each is checked:

- **Values.** A program staticjs accepts is valid JavaScript and gives
  Node's value or a fault (`mise run diff-node`, every spec case and
  100,000-input sweeps of numbers, JSON and regular expressions).
- **Termination without a timer.** No loops but the built-ins over
  collections, no recursion; evaluation runs on fuel the host pushes, and
  the fuel of a program on given inputs is the same on every machine
  (`staticjs/site/guide/performance.md`). This is the "gas-like,
  machine-independent measurement" Shopify was looking for [5], at the
  level of the language rather than of wasm instructions.
- **No ambient authority to remove.** A program reaches only what the host
  passes in; the clock and `Math.random`'s seed are the host's. There is
  no sandbox boundary to escape because there is no host object inside.
- **Determinism.** `Math` gives V8's bits, the same on every machine
  (`guide/reference.md`; compared with Node on x86_64).
- **Memory.** Only through the host's allocator, with limits on every
  construct; an evaluator with `max_stack = 256` is about 41 KiB.
- **Speed** (`REPORT.md`, one machine): over 33 evaluation cases,
  QuickJS-NG in wasm took 4.81 times staticjs's time (geometric mean),
  Node `--jitless` 1.10, Node with its JIT 0.43. A compiled guard
  evaluates in 0.14 µs from C.
- **What it gives up.** No `for`/`while`, recursion, classes, `async`,
  I/O or npm modules. QuickJS runs all of JavaScript; staticjs does not.
- **Size.** `staticjs.wasm` is about 950 KiB (`build/wasm`, October 2026),
  larger than quickjs-emscripten's default 500K build [3]. Size is not
  an advantage.

## Sources

1. QuickJS (Fabrice Bellard), home page: https://bellard.org/quickjs/
2. QuickJS-NG, documentation home: https://quickjs-ng.github.io/quickjs/
3. quickjs-emscripten, README: https://github.com/justjake/quickjs-emscripten
4. quickjs-wasi, package listing: https://socket.dev/npm/package/quickjs-wasi/overview/3.3.0
5. Shopify Engineering, "JavaScript in WebAssembly for Shopify Functions": https://shopify.engineering/javascript-in-webassembly-for-shopify-functions
6. Shopify, "Optimize instruction counts": https://shopify.dev/docs/apps/build/functions/optimize-instruction-counts
7. Node.js, `vm` documentation (warning; "Timeout interactions with asynchronous tasks and Promises"): https://nodejs.org/api/vm.html
8. vm2: CVE-2023-37466, https://www.cve.org/CVERecord?id=CVE-2023-37466 ; CVE-2026-22709, https://www.cve.org/CVERecord?id=CVE-2026-22709 ; heise, https://heise.de/-11158187
9. isolated-vm, README: https://github.com/laverdet/isolated-vm ; secondary summary: https://socket.dev/npm/package/isolated-vm/overview/6.1.2
10. SES (Endo), README: https://github.com/endojs/endo/tree/master/packages/ses
11. Check Point Research, "When agentic glue melts" (August 2026): https://research.checkpoint.com/2026/when-agentic-glue-melts/
12. CEL language definition: https://github.com/google/cel-spec/blob/master/doc/langdef.md
13. Kubernetes, "Common Expression Language in Kubernetes", resource constraints: https://kubernetes.io/docs/reference/using-api/cel/
14. JsonLogic: https://jsonlogic.com/
15. Expr, README: https://github.com/expr-lang/expr
16. JSONata, "Configuring Guardrails": https://docs.jsonata.org/guardrails
17. Jexl, README: https://github.com/TomFrost/Jexl ; npm registry: https://registry.npmjs.org/jexl
18. filtrex, README: https://github.com/joewalnes/filtrex
19. n8n, CVE-2025-68613: https://www.cve.org/CVERecord?id=CVE-2025-68613
20. expr-eval, CVE-2025-13204: https://www.cve.org/CVERecord?id=CVE-2025-13204
21. Figma, "An update on plugin security" (2019-10-02): https://www.figma.com/blog/an-update-on-plugin-security/
22. JSONata, CVE-2026-52746: https://www.cve.org/CVERecord?id=CVE-2026-52746
23. WebKit bug 200412, "Jitted code gives different floating point result": https://bugs.webkit.org/show_bug.cgi?id=200412 ; see also https://bugs.webkit.org/show_bug.cgi?id=114852
24. Mozilla bug 531915: https://bugzilla.mozilla.org/show_bug.cgi?id=531915
25. panzi-json-logic (PyPI): https://pypi.org/project/panzi-json-logic ; json-logic-ruby: https://github.com/bhgames/json-logic-ruby
26. Shopify community forum, Javy plugin and wasm size: https://community.shopify.dev/t/javascript-function-wasm-size-doubles-on-shopify-cli-4-4-0-exceeds-256kb-deploy-fails-with-version-couldnt-be-created/36487
27. QuickJS CVEs: https://www.cve.org/CVERecord?id=CVE-2023-48184 , https://www.cve.org/CVERecord?id=CVE-2025-62491 , https://www.cve.org/CVERecord?id=CVE-2026-1144
28. Simon Willison on MicroQuickJS (2025-12-23): https://simonwillison.net/2025/Dec/23/microquickjs/
29. Boa, README: https://github.com/boa-dev/boa
30. jse-eval: https://www.npmjs.com/package/jse-eval ; simple-eval: https://www.npmjs.com/package/simple-eval

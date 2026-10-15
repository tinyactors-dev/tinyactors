# staticjs: facts (the only claims allowed)

The fact sheet a writer or a critic works from, and nothing else. Check a
fact against the code before you hand this out (`include/staticjs.h`,
`docs/guide/`, `bench/engines/REPORT.md`), and change it here when it
changes. Keep the uses in their own section: a writer who sees a use in
every fact writes about the use, not the engine.

## What it is
- A JavaScript engine for a pure, total subset of JavaScript. Freestanding C11 library and a WebAssembly module. The maintainer's summary: "basically CEL with JavaScript syntax and user functions". The page headline is fixed: "A small, embeddable and fast JavaScript engine".
- Status: version 0.9.0, part of the tinyactors repository.

## Uses (name them at most once, after the description)
- Plugins: a function a user writes to transform their own records. The host compiles it once, binds `record` and helpers as globals, and evaluates it per record; native functions reach the host's tables. `examples/host.c`: `({ id: record.id, eur: cents(record.amount * rate(record.currency)) })` with a native `rate()` and a module `money`.
- Code mode: a program an agent writes to call the host's tools (native functions passed as globals) instead of one tool call at a time. A native must not change what a program can see, so tools read, and the program returns what should happen for the host to carry out: `orders("open").filter((order) => order.total > 500).map((order) => ({ flag: order.id, notify: customer(order.customer).manager }))`.
- Not named on the site: statecharts, guards, SCXML.

## The language
- A program is one expression (read as JavaScript reads `( source )`), or a module: imports, const declarations, exports. A module's value is the object of its exports.
- Has: numbers, BigInt, strings, template literals, booleans, null, undefined, arrays, objects (spread, computed and shorthand keys), Set, Map, Temporal (PlainDate, PlainTime, PlainDateTime, Duration, Instant; ISO calendar, UTC and fixed offsets), Date (UTC); operators + - * / % **, comparisons, === !==, && || ??, ?:, typeof, in, optional chaining ?.; arrow functions with an expression body or a block body of const, if/else and return; destructuring with defaults and rest; spread arguments.
- Built-ins: array methods (map, filter, reduce, find, some, every, flatMap, toSorted, toReversed, with, at, includes, join, slice, ...), string methods, regular expressions (linear time, no backtracking), Set algebra (union, intersection, difference, ...), Map, Object.keys/values/entries/fromEntries/groupBy/assign/freeze, Math (the same bits as V8 on x86_64), JSON.parse/stringify, encodeURI..., parseInt/parseFloat, Number methods, BigInt.asIntN/asUintN.
- Leaves out: loops (for, while, do), recursion, classes, async/await and promises, generators, try/catch/throw, var, labels, switch, eval, this, prototypes, getters/setters, I/O. A function may change only a value it owns (push onto an array it made, for example); shared values never change.
- Exact: a program staticjs accepts is valid JavaScript and evaluates to the value JavaScript (Node) gives, or to a fault. Never a different value. Where JavaScript would coerce, compare objects by identity or look on a prototype, staticjs faults. Examples (engine's own messages):
  - `[1, 2] === [1, 2]` → JavaScript: false; staticjs: fault: === on two objects compares identity, which staticjs does not have
  - `"5" * 2` → JavaScript: 10; staticjs: fault: arithmetic in staticjs is on numbers
  - `({}).a.b` → JavaScript: TypeError; staticjs: fault: cannot read a property of undefined
  - `() => { for (;;) {} }` → staticjs refuses it when compiled: staticjs has no loops
  A fault has a position (line, column) and a fixed English message. Refusals at compile time are syntax (JavaScript refuses it too), unsupported, name or limit errors.
- Tested against Node: every case of the spec suite (about 4,200) also runs in Node and must agree; plus hundreds of thousands of generated inputs for number formatting, JSON, regular expressions, Temporal.

## Properties
- Total: every evaluation ends. The evaluator runs on fuel the host pushes: one unit per instruction and one per element a built-in walks. When the fuel runs out it pauses; the host pushes more and it resumes from where it stopped, inside a callback included.
- Hermetic: a program reads only the values the host passes in (globals by name) and imports only modules the host provides. No file system, network, clock or randomness of its own; the host sets the clock (Temporal.Now, Date.now) and Math.random's seed.
- Deterministic: the same program with the same inputs gives the same value and uses the same fuel on every machine.
- Bounded: every byte comes from the allocator the host gives; every construct has a limit (stack, frames, string size, nesting, ...) set by the host. An evaluator with max_stack = 256 is about 41 KiB. No recursion anywhere in the C code.
- Host functions: the host adds native functions (C callbacks) as globals.

## The library
- Freestanding C11: no C library (only stdint.h, stddef.h, stdbool.h). One public header, staticjs.h, prefix sjs_.
- Three ways to get it:
  1. Prebuilt static and shared libraries for Linux and macOS, x86_64 and arm64: `cc -std=c11 -Iinclude app.c lib/libstaticjs.a`
  2. The amalgamation: staticjs.c and staticjs.h, built with clang: `clang -O2 -I. app.c staticjs.c`
  3. The npm package @tinyactors/staticjs: staticjs as WebAssembly for Node and browsers, values in and out as JavaScript values: `npm install @tinyactors/staticjs`. Under it, staticjs.wasm, a module that imports nothing and brings its own allocator.
- Pipeline, each stage pushed by its caller, none blocks or calls back for input: bytes into the parser (it sends events to a sink, operands before their operator); the compiler is that sink and makes bytecode as events arrive, and tags functions whose shape a built-in can run in C (x => x.key, (a, b) => a.k - b.k), which saves fuel and never changes a value; the evaluator runs on pushed fuel; the writer writes a value as JavaScript source into whatever buffer space the host gives it. A host can stream source in chunks.
- Memory, as WebAssembly: everything staticjs has is in the module's one linear memory, which JavaScript sees as one ArrayBuffer (`memory.buffer`), in 64 KiB pages. In address order: the C stack (the first 1 MiB, growing down), the module's data (about 60 KB: the built-ins' tables and constants), then the heap. The module's allocator hands out blocks from the heap in power-of-two sizes, reuses freed blocks first, and grows memory a page at a time; WebAssembly memory never shrinks. The site's module starts at 17 pages (1,114,112 bytes).
- Pointers are numbers: on WebAssembly a pointer is a byte offset into that ArrayBuffer. A host in JavaScript reads a value where it lies: `new Uint8Array(memory.buffer, address, 16)` is a value's 16 bytes. A number is in the value itself (its 8 bytes are the double); a string, array, object, Map or Set is a pointer to a heap object. Byte order is little-endian.
- The front page reads its own module's memory live: it runs a program, then draws memory.buffer to scale, the heap with where the context, evaluator, program, result and its object are, and the result's 16 bytes.
- Inspection: a paused evaluation can be read (its frames: the program, map at element 2 of 4, the callback, with positions); a compiled program lists its functions and their shapes.
- The site has a pipeline explorer: type a program or pick one; see parser events (pushed 1, 4 or 16 bytes at a time, or whole), the compiler's functions and shapes and fuel with and without shapes, the evaluator one unit of fuel at a time with its frames, and the writer's output in 16-byte pieces. It runs on staticjs.wasm in the browser.
- The front page has a live, editable code sample (filter and sort a list of orders, with Temporal dates), evaluated in the page; it shows the result and the fuel used.

## Performance (one machine, a 4-core x86_64; proportions, measured)
- From C: evaluating a compiled filter expression (`event.f > 25`) takes about 0.14 µs; compiling it 6.4 µs. Node's vm.Script.runInContext of the same expression: 1.4 µs; creating a fresh V8 context: 330 µs.
- Over 33 evaluation cases, staticjs native: Node with its JIT takes 0.43 of staticjs's time (Node's JIT is faster); Node --jitless about the same as staticjs; QuickJS-NG compiled to WebAssembly takes 4.8 times staticjs's time.
- Live on the front page: staticjs and QuickJS-NG (npm package quickjs-wasi), both compiled to WebAssembly, run eight programs in the reader's browser in a web worker: a small expression, filter and map over 10,000 orders, sort 5,000 rows by a key, group and count 10,000 numbers, sum 100,000 numbers with reduce, work on 2,000 strings, a regular expression over 2,000 addresses, a JSON round trip of 1,000 records. A row is timed only when both engines return the same result. Median of five samples, each repeating the program at least 40 ms. staticjs is called from JavaScript once per run, as a host calls its C API; QuickJS-NG loops inside its engine and is called once per sample. It also shows how long each engine takes to start from its compiled module (about 0.1–0.4 ms for staticjs, 0.8–3 ms for QuickJS-NG in Chromium). In Chromium on the maintainer's machine staticjs was faster on 7 of 8 programs (1.1x to 2.2x) and slower on one (1.1–1.2x).
- Never claim staticjs is faster than Node's JIT. Never claim staticjs is smaller than QuickJS in bytes (its wasm is ~950 KiB, QuickJS's ~620 KiB).

## The alternatives (from their own pages)
- QuickJS (Bellard; QuickJS-NG fork): "a small and embeddable Javascript engine"; nearly 100% of the ECMAScript test suite (ES2025); a few C files, no external dependency; reference-counting GC; runs all of JavaScript, so a program that does not end is stopped by an interrupt handler the host writes; a memory limit on the runtime.
- CEL: "a non-Turing complete language designed for simplicity, speed, safety, and portability"; C-like syntax of its own; a program is a single expression; macros (all, exists, exists_one, filter, map); hosts provide functions and variables; no function definitions in the language; implementations in Go, Java, C++; type checking; formal verification.
- Starlark: "a dialect of Python intended for use as a configuration language"; deterministic, hermetic, parallel (shared data frozen); def functions, no recursion; no unbounded loops (for over finite sequences); implementations in Go, Java, Rust.

## Limitations to state honestly
- Not all of JavaScript: no loops, classes, exceptions, async, generators; programs written for Node usually need rewriting with map/filter/reduce.
- Faults where JavaScript would coerce or compare identity, so some valid JavaScript faults.
- Temporal: ISO calendar only, UTC and fixed offsets (no IANA time zones). Date: UTC only.
- Regular expressions: no lookbehind/lookahead, backreferences, \p{...} or the v flag.
- Version 0.9: the API may change.
- Docs: Guide (getting started, the language, faults and refusals, embedding, performance, browser and Node, quick reference), the C header staticjs.h, the pipeline explorer.

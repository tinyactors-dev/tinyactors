# Writing for the staticjs site

How the site's pages are written, taken from how three neighbors write
about themselves: the [quickjs-wasi README](https://github.com/vercel-labs/quickjs-wasi),
Starlark (its [README](https://github.com/bazelbuild/starlark),
[spec](https://github.com/bazelbuild/starlark/blob/master/spec.md) and
[starlark-go README](https://github.com/google/starlark-go)) and CEL (the
[cel-spec README](https://github.com/google/cel-spec),
[introduction](https://github.com/google/cel-spec/blob/master/doc/intro.md) and
[cel-go README](https://github.com/google/cel-go)). Each rule quotes them.

## 1. Describe the engine, not the reader's job

None of the three describes what the reader is going through. They
describe the language and the library, and they let the reader decide
what it is for.

> Starlark is an untyped dynamic language with high-level data types,
> first-class functions with lexical scope, and automatic memory management.

> A snapshotable JavaScript runtime via WebAssembly.

So the subject of a sentence is staticjs, the language, a program, the
evaluator or a function of the API. It is not "your users", "someone
else", "code you didn't write" or "your build".

- Don't: "A pure subset of JavaScript, for running code your users write."
- Do: "staticjs is a JavaScript engine for a pure subset of the language."

## 2. Use cases come late, once, as examples

Starlark names its use case after it has described the language, in one
paragraph, with a concrete example:

> For example, Starlark was originally developed for the Bazel build tool.
> Bazel uses Starlark as the notation both for its BUILD files ... and for
> its macro language.

CEL lists two "Key Applications" in one line each, below the definition.
quickjs-wasi names none.

For staticjs:
- Say at most once on the front page what it is used for, after the
  description.
- The uses the documentation names are two, both with a concrete program:
  - **Plugins:** a function a user writes to transform their own records,
    compiled once by the host and evaluated per record, with the host's
    tables reached through native functions (`examples/host.c`, `rate()`).
  - **Code mode:** a program an agent writes to call the host's tools,
    which are native functions, instead of one tool call at a time. Tools
    read; the program returns what should happen, and the host does it.
- No statecharts, guards, SCXML or `In()` in user-facing pages or
  examples. The internals keep their statechart workloads, because the
  benchmarks measure them; the guide calls the same expression a filter.
- Never put a use case in a heading or a lede. "Rules", "guards",
  "filters", "policies" and "formulas" are not the product.

## 3. Open with "X is a Y", then its one difference

> Starlark in Go is an interpreter for Starlark, implemented in Go.

> The Common Expression Language (CEL) is a non-Turing complete language
> designed for simplicity, speed, safety, and portability.

> QuickJS is a small and embeddable Javascript engine.

The first sentence puts the thing in a category. The second says what
sets it apart, in the category's own terms.

## 4. Compare to a language the reader knows: "Like X ... Unlike X ..."

> Like Python, it is an untyped dynamic language ... Unlike CPython,
> independent Starlark threads execute in parallel.

> CEL's C-like syntax looks nearly identical to equivalent expressions in
> C++, Go, Java, and TypeScript.

For staticjs the reference point is JavaScript and Node: "Like
JavaScript, it has arrow functions, destructuring, Map, Set, BigInt and
Temporal. Unlike JavaScript, it has no loops, classes or exceptions, and
every evaluation ends."

## 5. State what the language leaves out as a plain list

> There are no user-defined types, no inheritance, no reflection, no
> exceptions, no explicit memory management. Execution is finite. The
> language does not allow recursion or unbounded loops.

No apology and no "so that you can". If the reason matters, give it in
one sentence, as a design fact:

> CEL evaluates in linear time, is mutation free, and not Turing-complete.
> This limitation is a feature of the language design.

## 6. A property word gets one defining sentence

> **Deterministic evaluation**. Executing the same code twice will give
> the same results.
> **Hermetic execution**. Execution cannot access the file system,
> network, system clock.

The label is the property's own name ("deterministic", "hermetic",
"total"). The sentence after it defines it, and goes no further: no
second sentence that sells it.

## 7. Show code early, with its result in a comment

> ```
> has(account.user_id) || has(account.gaia_id)    // true if either one is set
> size(account.emails) > 0                        // true if emails is non-empty
> ```

> ```
> names = ", ".join(people.keys())  # Alice, Bob, Charlie, Dave
> ```

- The examples work on ordinary data (accounts, coins, people) and show
  the language, not a scenario.
- Code comes before prose about the code.
- Results appear as comments or as REPL output.

## 8. Headings are nouns that name a thing

quickjs-wasi's headings: Install, Usage, Basic evaluation, Working with
values, Host functions, Memory limits, Interrupt handler, How it works,
Limitations and future work. Starlark's are Overview, Design Principles,
Tour and Evolution.

- No sentences ("Watch a program go through the pipeline").
- No claims ("Small, embeddable, fast, and it always ends").
- No addressing the reader ("What you get").
- No contrasts ("Where staticjs stops instead of guessing").

## 9. A section starts with what to do, or what it is

> Determine the variables and functions you want to provide to CEL. Parse
> and check an expression to make sure it's valid. Then evaluate the
> output AST against some input.

> Restrict how much memory the QuickJS runtime can allocate. When
> exceeded, allocations fail and surface as JS exceptions.

No lede restates what the section contains. Steps are imperative
("Build the code:", "Run the interpreter:"). A section ends when its
content ends, without a summary line.

## 10. Numbers go in passing, where they belong

> instantiation from a compiled module skips recompiling the ~620 KB
> binary on every `QuickJS.create()` call.

> The handler is called approximately once per JS bytecode instruction,
> so it should be fast.

A number is part of a technical sentence, with its unit and its context.
It is not a headline and not a marketing row. Benchmarks belong to a
section about performance, with how they were measured.

## 11. Explain the mechanism

quickjs-wasi has "How it works", which opens with "The core insight" and
then the facts:

> WebAssembly linear memory is a flat byte array. Everything QuickJS
> allocates ... lives in this linear memory.

A short explanation of how staticjs works (push-based stages, fuel, no
recursion, values without pointers) persuades more than any adjective.

## 12. Limitations are a section of their own

quickjs-wasi ends with "Limitations and future work", one bullet per
limit, each with what happens:

> deep recursion causes a WASM trap (not a catchable exception).

Say what staticjs does not do (the whole language, exceptions,
asynchronous code), and point to the place that lists it.

## 13. Positioning

- **The category:** a JavaScript engine. The headline is fixed: "A small,
  embeddable and fast JavaScript engine".
- **The one-line comparison:** staticjs is basically CEL with JavaScript
  syntax and user functions. It may appear once, in the comparison.
- **The competitor:** QuickJS, the small embeddable engine that runs all
  of JavaScript. The front page races the two, both as WebAssembly, live.
- **The neighbors:** CEL and Starlark, compared as facts in a table, in
  their own words.
- **Claims never made:** that staticjs is faster than Node's JIT (Node
  takes 0.43 of its time), or smaller than QuickJS in bytes (its wasm is
  about 950 KiB, QuickJS's about 620 KiB).

## 14. Diagrams come from the design system

The site draws with components ported from the `tinyactors-design` skill
(at the repository's root) into `.vitepress/theme/components/ds/`. Use one
before drawing anything new, and port another from the skill the same way
when none fits:

| Component | Draws | Used for |
|---|---|---|
| `ArchDiagram` on a `Sheet` | blocks, zones and wires whose kind says what a hop is (stream, call, shared memory) | the pipeline: host, parser, compiler, program, evaluator, writer |
| `SpanTree` | a waterfall of nested spans on an axis | the frames of a real evaluation, on an axis of fuel |
| `MemoryLayout` | a struct's bytes, field by field | `sjs_value`'s 16 bytes |
| `MemoryMap` (the site's own, from the memory sketches' strip and hex map) | the module's `memory.buffer` to scale, the heap enlarged with pins, a value's bytes in hex tinted by field, read live | the Memory part of How it works |

- A diagram shows a fact a reader could check in the code, and runs live
  where it can (`EvaluationFrames` runs its program in the page).
- Labels are in sentence case. The site leaves out the design system's
  uppercase labels and eyebrows, and keeps labels few: a component says
  what it is by what it draws.
- A diagram gets a short caption that says how to read it, and a
  paragraph or a table in the markdown version (`index.md`) that says the
  same.

## 15. Sentences

- **Shape:** subject, verb, object. Vary the length; the neighbors mix
  four-word sentences ("Execution is finite.") with long ones.
- **Adjectives:** categorical (small, simple, deterministic, hermetic),
  never evaluative (powerful, seamless, robust, blazing).
- **Person:**
  - "We" only for the project's own choices ("We try to limit the
    number of concepts").
  - "You" only in instructions ("You can use it as an expressive
    notation for structured data").
- **Quoting the project:** a plain fact may be quoted as it stands:
  "Starlark is indeed a very small language."

## 16. What we kept getting wrong

These patterns made earlier drafts read as generated. Check every page
for them.

| Pattern | Example from an earlier draft |
|---|---|
| The reader's job as the subject | "code someone else writes", "for running code your users write" |
| A use case in a heading or lede | "When a rule is wrong", "Let others write the rules" |
| Lists of three as structure | "Small, embeddable, fast, and it always ends" |
| "X, not Y" contrasts | "a fault, not 10", "proportions, not promises" |
| Mirrored sentences | "It is QuickJS's embeddability in a language that cannot run away, and Starlark's determinism for JavaScript." |
| The page describing itself | "These numbers were measured in this browser just now." |
| A lede that restates the section | "Use prebuilt libraries, one C file, or WebAssembly. Pick the one that fits your build." |
| Wordplay | "Every one of them *ends*." |
| Every block in the same rhythm | label, fact, fact, number, six times |
| A use the documentation no longer names | "a guard in a statechart", SCXML's `In()` as the native example |
| The same fact in three sections | the four stages in the overview, the install section and How it works |
| Eyebrows and labels on every block | a small capital label over each heading and card |

## 17. The front page

The order is fixed by the maintainer:

1. **Hero:** the headline and one sentence, what it is, beside the live,
   editable sample.
2. **Overview:**
   - what the language has, compared with JavaScript (rule 4);
   - what it leaves out (rule 5);
   - its properties, each defined in one sentence (rule 6);
   - the four programs where it stops, with results (rule 7).
3. **Install:** the three ways in, as commands.
4. **Performance:** the measurements with their method, and the live race
   against QuickJS.
5. **Limitations:** one bullet per limit, with what happens.
6. **Comparison:** QuickJS, CEL and Starlark, as facts in a table.
7. **How it works:**
   - the pipeline diagram;
   - the frames of an evaluation;
   - memory: the module's one linear memory and a value's bytes, live (`MemoryMap`), then `sjs_value`'s layout;
   - the link to the pipeline explorer.
8. **Documentation:** links to the guide, the language and the C header.

Each fact appears in one section only. `HomePage.vue` renders the page.
`index.md` mirrors it as markdown, in the same order, for the `.md` routes
and `llms.txt`. Change both together.

How the copy is made and reviewed (fact sheet, roast, one writer per
section, critique, editing pass) is the `site-copy` skill in
`staticjs/.agents/skills/site-copy`.

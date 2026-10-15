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
- Name a real use, and only a true one: staticjs is being built as the
  data model of tinyactors' statecharts, and says so only in those terms.
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

## 13. Sentences

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

## 14. What we kept getting wrong

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

## 15. A front page in this style

The order the neighbors follow:

1. The name and one sentence: what it is.
2. Two or three short paragraphs:
   - what the language has, compared with JavaScript (rule 4);
   - what it leaves out (rule 5);
   - its properties, each defined in one sentence (rule 6).
3. A code example with its result (rule 7). On this site it is live.
4. Install or get it: the three ways in, as commands.
5. How it works, briefly (rule 11), with a link to the pipeline explorer.
6. Performance: the measurements and the live comparison, with the method.
7. Comparison with QuickJS, CEL and Starlark, as facts in a table.
8. Limitations, and links: guide, language reference, C header.

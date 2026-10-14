# staticjs internals

For contributors: how staticjs is built, tested and measured, and why its
parts are designed as they are. To use staticjs, read
the [guide](../README.md) instead. Start with [AGENTS.md](../../AGENTS.md): the rule,
the C rules, and what each kind of change must come with.

## Working on staticjs

| Page | Read it to |
|---|---|
| [extending.md](extending.md) | add to staticjs and keep it correct, fast and reviewable: a step list for each goal (syntax, a built-in, a value kind, a shape, mutation, numeric code, measuring, committing) |
| [builtins.md](builtins.md) | add a built-in function or method without slowing evaluation, and the mistakes already made |
| [builtins-status.md](builtins-status.md) | see every JavaScript built-in in scope, whether it is built, and why |
| [testing.md](testing.md) | see how the rule is checked against Node, what each test program covers, and every task |
| [benchmarks.md](benchmarks.md) | measure a change, compare it with the build before it, and compare staticjs with other engines |
| [`../../src/README.md`](../../src/README.md) | find which source file holds what |

## Design notes

Each note records how one part works and why. Where it says what was
planned, it also says what was built and where the build departed from the
plan.

| Note | Covers |
|---|---|
| [matcher.md](matcher.md) | the event stream's node identity, the shape rules (`src/shapes.rules`) and their generator, the matcher, and the built-ins that run a tagged callback in C; rewrites |
| [mutation.md](mutation.md) | owned mutation: the rule at compile time, the check at run time, each mutating form, and how to roll it back |
| [regex.md](regex.md) | regular expressions as a linear-time automaton run as a loop frame: the instruction set, what is accepted or refused, fuel and limits |
| [temporal.md](temporal.md) | Temporal values: representation, wide integers, durations, rounding, the host clock and the host seed |
| [date.md](date.md) | Date as a Temporal-kind value in UTC: representation, parsing, setters |

What a user can rely on is in the guide (`../guide/`). The notes explain
how it is built and may go deeper than any contract. When a note and the
guide disagree on behaviour, the code and its tests decide; fix whichever
is wrong.

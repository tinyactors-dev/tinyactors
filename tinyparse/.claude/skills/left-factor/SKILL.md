---
name: left-factor
description: Left-factor a tinyparse `.scm` grammar so its ordered choices stop re-parsing a shared prefix — the transform that turns a PEG's quadratic/exponential backtracking into linear descent. Use when a grammar parses pathologically slowly or uses huge memory on deeply nested input (expression ladders are the classic culprit), when porting a tree-sitter grammar (whose unordered `choice` the LR table left-factors for free, but ordered PEG choice does not), or when asked to make a tinyparse grammar faster at the source rather than leaning on the rule memo. Covers how to spot a re-parsing choice, the transform, the verification bar (conformance + the engine differential + a memo-OFF before/after), and the traps.
---

# Left-factoring a tinyparse grammar

A PEG's `(choice a b)` is **ordered and backtracking**: it tries `a` fully, and if
`a` fails it throws that entire parse away and tries `b` from the same offset. When
`a` and `b` begin with the same sub-parse, that shared prefix is parsed twice — and
if the pattern nests (an expression grammar, say), the doubling compounds toward
exponential. tree-sitter grammars do not show this because their `choice` is
*unordered* and the generated LR table merges the shared prefix into one state; when
you port such a grammar to ordered PEG choice, the merge is gone and the re-parse
appears. Left-factoring puts the merge back, by hand, in the grammar.

This is the *source-level* cure. tinyparse also has a runtime rule memo (a
packrat safety net), but factoring is better where it applies: it removes the work
instead of caching it, costs no memory, and helps every engine. Use both — factor
the hot rules, let the memo catch the rest.

## Spotting a re-parsing choice

Look for an ordered choice whose alternatives share a leading sub-parse. The
giveaway shapes:

- **A precedence ladder** where a high-precedence form and a fall-through both
  descend into the same operand:
  ```scheme
  (rule assignment
    (choice
      (seq (field left: lhs) "=" (field right: assignment))   ; parses lhs, needs "="
      ternary))                                               ; re-parses the operand
  ```
  On a non-assignment, the first alternative parses `lhs` (often the *whole*
  operand), fails for lack of `=`, and the second re-parses it as a `ternary`.
- **`A x | A y`** — two alternatives with a literal/rule common head `A`.
- **`A | A b`** — an optional tail; the bare `A` and the `A b` form re-parse `A`.

If the alternatives start with *different* first tokens, first-set prediction
already skips the wrong ones — those are fine, leave them.

## The transform

Parse the shared prefix once, then branch on what follows.

1. **Optional tail.** `(choice (seq P Q) P)` → `(seq P (optional Q))`. The classic
   ladder fix: parse the operand once as the most general form, then an optional
   continuation decides the production.
   ```scheme
   (rule assignment
     (choice
       arrow_function          ; distinctive prefix, fails fast — keep ahead
       yield_expression
       (seq (field left: ternary)
            (optional (seq (field operator: (choice "=" "+=" …))
                           (field right: assignment))))))
   ```
2. **Factor a common head.** `(choice (seq A x) (seq A y))` → `(seq A (choice x y))`,
   when `A` carries no field/alias that must differ between the branches.
3. **Keep distinctive prefixes first.** Alternatives that fail fast on a wrong lead
   byte (a keyword form, a `(`-led form) can stay ahead of the factored tail; they
   never descend the shared operand, so they cost nothing.

What you are removing is a *successful* prefix parse that gets discarded. A choice
whose earlier alternative fails *immediately* is not the problem.

## Verifying (the bar — do not skip)

Tree shapes may shift (e.g. an assignment's LHS becomes the ternary-derived node
rather than a dedicated `lhs` node). That is acceptable: tinyparse conformance is
**accept-level**, not tree-equality (`docs/decisions.md`). The bar is:

1. **Conformance unchanged:** `mise run test` stays 296/296 (or
   whatever the current count is). The corpus must still accept what it accepted and
   reject what it rejected.
2. **Engines agree:** `mise run gate vm-diff` green — the change is in the
   grammar, so every engine sees it; they must still produce identical trees.
3. **It actually helped:** measure with the memo OFF so you see the *grammar*
   effect, not the memo's. Pack and time a pathological input both ways:
   ```sh
   TINYPARSE_RULEMEMO=off <harness> <file>   # before vs after
   ```
   A good factoring drops a deeply nested file from seconds to milliseconds and its
   peak memory from hundreds of MB to tens. Record the before/after.

## Traps

- **Do not change what the corpus accepts or rejects.** If a factoring makes a
  previously-rejected input parse (or vice versa), it is wrong — the metric catches
  it. Left-factoring is meant to be behaviour-preserving at the accept level.
- **Field/alias on the shared prefix.** If the two branches need the prefix bound to
  *different* fields/aliases, you cannot factor it into one parse without losing
  that distinction — leave it, or push the field onto the branches.
- **Edit the grammar, not the generated C.** These are `.scm` data; the engines read
  them directly. After editing, **re-pack** any cached `.tpg` the benchmarks load
  (`tinyparse pack …`), or they will run the old grammar.
- **Right-associativity via recursion stays.** `(seq left op (… assignment))` with
  the rule recursing on its right is how right-assoc is expressed; the optional-tail
  factoring preserves it (`(optional (seq op right:assignment))`).

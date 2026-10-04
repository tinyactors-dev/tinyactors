# examples/heredoc — deferred-slot and parametrized lexical goal

This example is the primary iteration vehicle for the **deferred-slot /
parametrized-goal** feature (see `docs/lex-goal-implementation.md`). It
implements a minimal shell grammar that parses POSIX-style heredocs
correctly, including nested heredocs inside command substitutions.

## What the grammar covers

```
cat <<EOF
echo Hello
echo $(
  cat <<NESTED
world
NESTED
)
EOF
```

Heredocs are context-sensitive in two ways that make them hard to parse
with context-free rules:

1. **The closing delimiter is determined at match time**, not at compile
   time. After `<<EOF` the grammar must remember "EOF" and match a line
   that begins with exactly that text.

2. **Heredoc bodies appear after the command line**, not inline. The `<<`
   operator appears on the command line; the body is the text that follows
   the terminating newline of that line.

Both problems are solved without special-casing in the engine, using three
grammar primitives and one parametrized lexical goal.

## The three deferred-slot primitives

### `(deferred-slot NAME)` — per-invocation FIFO slot

Declares a named slot scoped to the current rule invocation. Every time a
rule containing `(deferred-slot NAME)` is entered, a fresh slot instance
is created. Nested invocations of the same rule each get their own
instance, so inner and outer heredoc queues never share state.

```scheme
(rule CompleteCommand
  (seq
    (deferred-slot heredoc-bodies)   ; fresh slot for this invocation
    ...))
```

### `(defer SLOT PATTERN)` — match and enqueue

Matches PATTERN, captures the matched byte span in the current
invocation's SLOT, and returns the CST node produced by the match. It is
composable with `(field ...)`:

```scheme
(rule Redirect
  (seq HeredocOp
       (field word: (defer heredoc-bodies Word))))
```

This produces the same CST as `(field word: Word)` and additionally
enqueues the delimiter text ("EOF", "NESTED", …) onto `heredoc-bodies`.

### `(parse-deferred SLOT RULE)` — drain and parse

Drains the slot FIFO in order, invoking RULE once per item. Before each
invocation it installs the captured text as the active parametrized
pattern; after the invocation it uninstalls it. An empty slot is a no-op
(commands with no redirects drain nothing).

```scheme
(rule CompleteCommand
  (seq
    (deferred-slot heredoc-bodies)
    (field command: Pipeline)
    Newline
    (parse-deferred heredoc-bodies HeredocBody)))
```

POSIX ordering falls out naturally: multiple heredocs on one command line
(`cat <<A <<B`) enqueue `[A, B]` left-to-right; the drain processes them
in the same order, matching the bodies `A-body … A … B-body … B` as POSIX
requires.

## The parametrized lexical goal

```scheme
(token HeredocDelimLine)

(lex-goal heredoc-body-mode
  HeredocDelimLine: (param delimiter (seq delimiter "\n")))
```

`HeredocDelimLine` is a **parametrized token**: it has no static pattern.
The `(param delimiter ...)` form declares `delimiter` as the name for the
captured bytes inside the pattern body. The body `(seq delimiter "\n")` is
written in the grammar's own pattern language — the same forms available
everywhere else in the grammar.

When `parse-deferred` processes an item with captured text `T`, it
instantiates the body by substituting `delimiter` with a
`PP_OP_LITERAL(T, len(T))` node, producing a concrete pattern, and pushes
it onto `heredoc-body-mode`'s `HeredocDelimLine` stack. The lexer, while
`heredoc-body-mode` is active, reads the top of that stack to resolve
`HeredocDelimLine`. When `HeredocBody` returns, `parse-deferred` pops the
pattern, restoring whatever was beneath (the enclosing delimiter, or nothing
if this is the outermost heredoc).

A grammar that needed to match the delimiter surrounded by optional
whitespace, or case-insensitively, would write a different body — no engine
change required.

### Why a stack, not a single slot

Consider the nested case:

```
cat <<EOF          ← outer, delimiter EOF
echo $(
  cat <<NESTED     ← inner, delimiter NESTED
world
NESTED             ← inner delimiter line; outer EOF pattern must survive this
)
EOF                ← outer delimiter line
```

When parsing the inner `HeredocBody` (delimiter `NESTED`), the outer
`HeredocBody` (delimiter `EOF`) is still active on the call stack. If the
goal-table entry were a single slot, installing `NESTED` would overwrite
`EOF`. The stack ensures:

- outer `parse-deferred` pushes `EOF` pattern
- inner `parse-deferred` pushes `NESTED` pattern on top
- inner `HeredocBody` sees `NESTED\n` and stops correctly
- inner `parse-deferred` pops `NESTED`; outer `EOF` is restored
- outer `HeredocBody` sees `EOF\n` and stops correctly

### Why separate deferred-slot instances handle nesting

The `BodyCommand` rule (used inside both `HeredocBody` and
`CommandSubstitution`) declares its own `(deferred-slot heredoc-bodies)`.
When `cat <<NESTED` is parsed inside a `CommandSubstitution` inside the
outer `HeredocBody`, the `BodyCommand` invocation gets a fresh
`heredoc-bodies` slot. The `(defer heredoc-bodies Word)` in its `Redirect`
writes `NESTED` into that fresh slot, not the outer `CompleteCommand`'s
slot (which already contains `EOF` and has been drained). The two slot
instances are completely independent.

## Parse trace

Input:
```
cat <<EOF
echo Hello
echo $(
  cat <<NESTED
world
NESTED
)
EOF
```

The trace shows rule activations, slot state, and goal-stack transitions.
`slot[X]` is the current content of the named slot; `goal-stack` is the
pattern stack for `HeredocDelimLine` in `heredoc-body-mode`.

```
Program @0
  CompleteCommand @0
    slot heredoc-bodies = []
    Pipeline @0
      SimpleCommand @0
        Word("cat") @0..3
        Redirect @4
          HeredocOp("<<") @4..5
          defer(heredoc-bodies) Word("EOF") @6..8
            slot heredoc-bodies = [{delim:"EOF"}]
    Newline("\n") @9
    parse-deferred(heredoc-bodies):
      pop {delim:"EOF"}
      goal-stack[HeredocDelimLine]: push pattern(EOF\n)
      → HeredocBody @10

      HeredocBody @10        [goal:heredoc-body-mode, top=EOF\n]
        repeat (not HeredocDelimLine BodyCommand):

          iter 1 @10:
            try HeredocDelimLine: "echo Hello\n" ≠ "EOF\n" → no match → proceed
            BodyCommand @10
              slot heredoc-bodies = []
              Pipeline @10
                SimpleCommand @10
                  Word("echo") @10..13
                  Argument: Word("Hello") @15..19
              Newline("\n") @20
              parse-deferred: slot empty → no-op

          iter 2 @21:
            try HeredocDelimLine: "echo $(\n" ≠ "EOF\n" → no match → proceed
            BodyCommand @21
              slot heredoc-bodies = []
              Pipeline @21
                SimpleCommand @21
                  Word("echo") @21..24
                  Argument: CommandSubstitution @26

                  CommandSubstitution @26   [extras:none]
                    DollarParen("$(") @26..27
                    repeat Newline: Newline("\n") @28
                    repeat BodyCommand:

                      iter 1 @29:
                        BodyCommand @29
                          slot heredoc-bodies = []
                          Pipeline @29         [extras:all within BodyCommand]
                            SimpleCommand @29
                              Word("cat") @31..33   [Space @29..30 as trivia]
                              Redirect @34
                                HeredocOp("<<") @34..35
                                defer(heredoc-bodies) Word("NESTED") @36..41
                                  slot heredoc-bodies = [{delim:"NESTED"}]
                          Newline("\n") @42
                          parse-deferred(heredoc-bodies):
                            pop {delim:"NESTED"}
                            goal-stack[HeredocDelimLine]: push pattern(NESTED\n)
                                                          [EOF\n saved beneath]
                            → HeredocBody @43

                            HeredocBody @43  [goal:heredoc-body-mode, top=NESTED\n]
                              repeat (not HeredocDelimLine BodyCommand):

                                iter 1 @43:
                                  try HeredocDelimLine: "world\n" ≠ "NESTED\n" → proceed
                                  BodyCommand @43
                                    slot heredoc-bodies = []
                                    Pipeline @43
                                      SimpleCommand @43
                                        Word("world") @43..47
                                    Newline("\n") @48
                                    parse-deferred: slot empty → no-op

                                iter 2 @49:
                                  try HeredocDelimLine: "NESTED\n" = "NESTED\n" → MATCH
                                  → stop repeat

                              field end: HeredocDelimLine("NESTED\n") @49..55

                            goal-stack[HeredocDelimLine]: pop NESTED\n → restore EOF\n

                      iter 2 @56:
                        try BodyCommand: Pipeline → Word → ")" not Word → fail
                        → stop repeat

                    CloseParen(")") @56

              Newline("\n") @57
              parse-deferred: slot empty → no-op

          iter 3 @58:
            try HeredocDelimLine: "EOF\n" = "EOF\n" → MATCH → stop repeat

        field end: HeredocDelimLine("EOF\n") @58..61

      goal-stack[HeredocDelimLine]: pop EOF\n → stack empty

  EOF @62
```

## Why inner delimiter = outer delimiter still works

If the input were:

```
cat <<EOF
cat <<EOF
inner
EOF
outer
EOF
```

The outer `CompleteCommand` defers `EOF` into its slot. After the outer
newline, the outer drain pushes `EOF\n` onto the goal-stack and enters the
outer `HeredocBody`. Inside, `BodyCommand` parses `cat <<EOF\n`: the inner
`BodyCommand`'s slot gets `{delim:"EOF"}`, its drain pushes another
`EOF\n` on top of the goal-stack, and the inner `HeredocBody` starts.
`"inner\n"` does not match `EOF\n`, so it is parsed as a BodyCommand.
`"EOF\n"` matches: inner HeredocBody stops, inner drain pops the stack.
Back in the outer HeredocBody, `"outer\n"` does not match `EOF\n` (now
restored as the top), so it is parsed as another BodyCommand. `"EOF\n"`
matches: outer HeredocBody stops.

The stacked delimiter correctly scopes each body to its own matching line.

# Deferred-slot and parametrized lexical goal — implementation specification

This document specifies the engine changes needed to make the
`(deferred-slot …)` / `(defer …)` / `(parse-deferred …)` grammar forms and
the `(param …)` lex-goal entry work. Together they implement context-dependent
body parsing — heredocs are the motivating case, but the primitives are
general. `examples/heredoc/heredoc.scm` is the iteration vehicle;
`grammars/std/posixsh/` is the end goal.

## The problem in one paragraph

A heredoc delimiter (`<<EOF`) appears on the command line. Its body — the
text that follows the terminating newline of that line — must be matched
against the exact literal `EOF`, a string known only at parse time. The body
also contains structured shell code (`$(…)` substitutions, nested heredocs),
not raw bytes. Context-free rule structure cannot express either property.
The design document sketched `(queue …)` / `(enqueue …)` / `(drain …)` as
the primitives; this document specifies their grammar-author-facing names,
the IR they compile to, and the engine changes that make them run.

## Grammar forms

### `(deferred-slot NAME)`

Declares a named FIFO slot scoped to the current rule invocation. Each time
the enclosing rule is entered, the engine creates a fresh, empty slot. Nested
invocations of rules that share the same slot name each get independent
instances — the inner `BodyCommand`'s `heredoc-bodies` and the outer
`CompleteCommand`'s `heredoc-bodies` never share state. The declaration is
zero-width: it matches no input and emits no CST node.

### `(defer SLOT PATTERN)`

Matches PATTERN (any grammar expression: a token, rule reference, or inline
composite), copies the matched byte span into the arena, and enqueues
`(text, text_len, node_index)` onto the named slot of the innermost enclosing
rule invocation that declared it. Returns the CST node produced by the match.

The byte copy is made at enqueue time — immediately after the sub-pattern match
succeeds, inside the execution of `TP_OP_DEFER` — into the **parse arena** (not
the grammar arena; the grammar arena is immutable after compile). The
`tp_deferred_item.text` pointer therefore points into the parse arena and remains
valid for the entire parse. Lazy copying (at drain time) is explicitly ruled out:
in streaming mode the input buffer may be a sliding window replaced before
`parse-deferred` runs, so the bytes must be captured before the buffer can move.

Composable with `(field …)`:

```scheme
(field word: (defer heredoc-bodies Word))
```

This builds the same CST as `(field word: Word)` and additionally enqueues
the delimiter text.

If the match fails, nothing is enqueued. If the rule backtracks past a
successful `(defer …)`, the enqueued item MUST be rolled back (see [Slot
rollback on backtrack](#slot-rollback-on-backtrack)).

### `(parse-deferred SLOT RULE)`

Drains the slot FIFO in order, invoking RULE once per item. For each item,
before invoking RULE:

1. Construct a runtime pattern from the captured text (see [Parametrized
   patterns](#parametrized-patterns)).
2. Push the pattern onto the parametrized goal-table entry's stack.

After RULE returns, pop the pattern. An empty slot is a no-op: a
`SimpleCommand` with no `<<` redirects drains nothing and RULE is never called.

If RULE fails on a given item — for example, because a heredoc has no closing
delimiter and the mandatory `(field end: HeredocDelimLine)` cannot match —
tinyparse's normal parse-error recovery applies inside RULE: it synthesizes
MISSING nodes and advances the cursor as usual. `parse-deferred` treats each
RULE invocation as non-failing from its own perspective: it always drains all
slot items regardless of whether individual RULE invocations encountered errors.
Error nodes produced by recovery are contained within each RULE's CST subtree
and do not propagate upward to the host parse. The input cursor advances to
wherever RULE left it (including past any recovery). Remaining slot items are
still drained in order after a failed invocation. This matches the isolation
guarantee of injection (`inject.c`): a failed embedded parse produces an
error-containing subtree, not a host-parse failure.

The compiler resolves which goal-table entry to push onto at compile time.
When processing `(parse-deferred SLOT RULE)`, the compiler performs a **full
recursive subtree scan** of RULE's compiled IR body — not just the outermost
node — and finds every `TP_OP_GOAL` node anywhere in the subtree, including
inside `(seq …)`, `(choice …)`, or other composite forms.

For each `TP_OP_GOAL` node found, the compiler looks up the matching
`tp_lex_goal` entry where `goal_symbol == intern(GOAL-NAME)` and
`is_parametrized == true`. Multiple `TP_OP_GOAL` nodes that resolve to the
**same** `goal_entry_index` are fine — all `(goal heredoc-body-mode …)`
references in a rule body refer to the same entry and do not conflict.

If `TP_OP_GOAL` nodes in different branches resolve to **different**
parametrized `goal_entry_index` values, a compile error is raised: the
`parse-deferred` opcode stores a single `goal_entry_index` and cannot know
which goal to push onto at the call site when more than one parametrized entry
is reachable.

If no `TP_OP_GOAL` node is found anywhere in the subtree, the compiler raises
a warning: the drain will never push onto any parametrized entry.

The resolved `goal_entry_index` (the index into `grammar->lex_goals[]` of the
parametrized goal-table entry) is stored as the `max` field of
`TP_OP_PARSE_DEFERRED` (see the IR field assignment table above).

### `(param NAME BODY)` in `(lex-goal …)`

Marks a goal-table entry as parametrized. `NAME` is a symbol that stands for
the captured literal inside `BODY`; `BODY` is any pattern expression in the
grammar's own language. At drain time the engine instantiates `BODY` by
substituting every occurrence of `NAME` with a `TP_OP_LITERAL` of the
captured text. For the heredoc delimiter:

```scheme
(lex-goal heredoc-body-mode
  HeredocDelimLine: (param delimiter (seq delimiter "\n")))
```

`delimiter` is bound to the captured bytes; `(seq delimiter "\n")` is the
pattern the lexer tries. A grammar that needed to match the delimiter
case-insensitively, or surrounded by optional whitespace, would write a
different `BODY` — no engine change required. A static redirect target in
the same entry is an error.

At compile time, `BODY` is compiled as a new internal rule and appended to the
grammar's rule table. The goal-table entry stores the internal rule's index
(a `uint32_t`) rather than a pointer to the IR. This keeps `tp_lex_goal`
entirely integer-valued, so it remains serializable by `pack.c` without any
new serialization path. `tp_param_instantiate` looks up the rule by its index
in `grammar->rules` at instantiation time.

BODY is restricted to token-level combinators: `TP_OP_LITERAL`, `TP_OP_CLASS`,
`TP_OP_ANY`, `TP_OP_SEQ`, `TP_OP_CHOICE`, `TP_OP_REPEAT`, `TP_OP_OPTIONAL`,
`TP_OP_NOT` (with token-level children only), and `TP_OP_PARAM_REF`. The
instantiated BODY is executed by the token matcher (`tp_vm_goal_redirect`),
which handles only token-level opcodes. A rule reference (`TP_OP_REF`), goal
form (`TP_OP_GOAL`), field form (`TP_OP_FIELD`), or any of the new
deferred-slot opcodes (`TP_OP_DEFERRED_SLOT`, `TP_OP_DEFER`,
`TP_OP_PARSE_DEFERRED`) inside BODY would either silently produce a wrong
result or crash. The compiler MUST detect and reject these at grammar-compile
time — this is a static restriction, not a runtime check.

## IR nodes

Four new `tp_ir` node types (add to the `TP_OP_*` enum in `src/internal.h`):

| Opcode | Operands | Meaning |
|---|---|---|
| `TP_OP_DEFERRED_SLOT` | `slot_name` (interned symbol) | Declare a slot; zero-width |
| `TP_OP_DEFER` | `slot_name`; child[0]: sub-pattern | Match child[0], enqueue result |
| `TP_OP_PARSE_DEFERRED` | `slot_name`, `rule_index`, `goal_entry_index` | Drain slot, invoke rule per item |
| `TP_OP_PARAM_REF` | `param_index` (u8, always 0 for now) | Leaf; replaced by `TP_OP_LITERAL` at instantiation |

`TP_OP_DEFER` wraps its sub-pattern as child[0] so the pattern compiles and
links through the normal IR machinery without a new allocation path.
`TP_OP_PARSE_DEFERRED`'s `rule_index` is the resolved rule index in the
grammar's rule table, established at grammar-compile time. `goal_entry_index`
is the index into `grammar->lex_goals[]` of the parametrized goal-table entry
that receives the pushed pattern; it is also resolved statically at
grammar-compile time (see [Grammar forms: parse-deferred](#parse-deferred-slot-rule)).

`TP_OP_PARAM_REF` is a leaf in the compiled `BODY` of a `(param NAME BODY)`
entry. The grammar compiler emits it wherever `NAME` appears in `BODY`.
It carries no children and no static bytes; `tp_param_instantiate` replaces
it with a `TP_OP_LITERAL` during drain.

### IR field assignments

Each `tp_ir` node has the fields `op`, `symbol` (one `uint32_t`), `min` and
`max` (two more `uint32_t`s, normally used only by `TP_OP_REPEAT`), and a
`children` array counted by `child_count`. For the new opcodes, `min` and `max`
are repurposed as general-purpose integer operand slots — this is safe because
none of the new opcodes are `TP_OP_REPEAT` nodes and none will be processed by
REPEAT-specific paths.

| Opcode | `symbol` | `min` | `max` | `children` |
|---|---|---|---|---|
| `TP_OP_DEFERRED_SLOT` | `slot_name` (interned symbol) | — | — | none |
| `TP_OP_DEFER` | `slot_name` (interned symbol) | — | — | `children[0]` = sub-pattern |
| `TP_OP_PARSE_DEFERRED` | `slot_name` (interned symbol) | `rule_index` | `goal_entry_index` | none |
| `TP_OP_PARAM_REF` | — (zero) | `param_index` (always 0) | — (zero) | none |

For `TP_OP_DEFER`, no integer operand other than `slot_name` is required: the
sub-pattern in `children[0]` carries all structural information about what to
match and enqueue.

For `TP_OP_PARSE_DEFERRED`, `min` holds the target rule's index in
`grammar->rules[]` (`rule_index`, resolved at grammar-compile time) and `max`
holds the index into `grammar->lex_goals[]` for the parametrized goal-table
entry that receives the pushed pattern (`goal_entry_index`, also resolved at
grammar-compile time by the pass described in [Grammar forms:
parse-deferred](#parse-deferred-slot-rule)).

For `TP_OP_PARAM_REF`, `symbol` is unused and left zero. `min` holds
`param_index`, which is always 0 in the current design (one parameter per
`(param …)` entry). `max` is unused and left zero.

## VM state

### Captured item

```c
/* ** One item captured by (defer …) and held until (parse-deferred …) drains it.
** text points into the parse arena; it is not NUL-terminated.
** node_index is the CST node index of the node produced by the (defer …) match —
** for example, the Redirect node's word: child that holds the delimiter text.
** It is stored for post-parse use: tooling and tree-walking code can use it to
** cross-link the heredoc body (built during drain) back to the specific redirect
** node in the CST that opened it, without re-scanning the source.
** node_index is NOT read during drain or pattern instantiation; it is purely
** post-parse metadata. */
typedef struct tp_deferred_item {
    uint8_t  *text;
    uint32_t  text_len;
    uint32_t  node_index; /* post-parse cross-link: CST node produced by (defer …) */
    uint32_t  _pad;
} tp_deferred_item;
```

### Slot frame

```c
/* ** One named slot, scoped to a single rule invocation.
** items is arena-allocated and grows by doubling. */
typedef struct tp_slot_frame {
    uint32_t          slot_name;  /* interned symbol */
    tp_deferred_item *items;
    uint32_t          count;
    uint32_t          capacity;
} tp_slot_frame;
```

Because tinyparse arenas are append-only (no `realloc`), "grows by doubling" means:
allocate a new block of doubled capacity from the arena, copy the existing
items into it, update `items` to point at the new block, update `capacity`.
The old block is abandoned in the arena (it cannot be freed). On first push
(`items == NULL`, `capacity == 0`), `tp_slot_push` allocates an initial block
(e.g., 4 items) from the arena and sets `items` and `capacity`. This
first-push NULL case is part of `tp_slot_push`'s contract.

### Slot stack on the VM call frame

The VM already maintains a call frame stack for rule recursion. Each frame
gains a `tp_slot_frame` array sized at compile time to the maximum number of
distinct `(deferred-slot …)` declarations in the rule:

```c
/* ** Per-invocation slot storage, embedded in the VM call frame.
** slot_count is set at grammar-compile time; slots is an inline array. */
uint32_t       slot_count;
tp_slot_frame *slots;        /* points into the frame's own arena allocation */
```

`TP_OP_DEFERRED_SLOT` initialises the matching entry in the current frame's
`slots` array to `{slot_name, NULL, 0, 0}`. No heap allocation until the first
`TP_OP_DEFER` enqueues an item.

### Per-rule slot count

The slot count for each frame must be communicated from the grammar compiler to
the code paths that initialize new call frames. The mechanism differs between
the VM interpreter and the generated C emitter.

**`tp_rule_def` gains a `slot_count` field.** The grammar compiler, after
compiling a rule's body, counts the distinct `(deferred-slot …)` declarations
in that rule's own body (not counting calls to other rules — only declarations
lexically present in this rule's body). It stores that count as `slot_count` in
the rule's `tp_rule_def` entry in `grammar->rules[]`. Rules with no
`(deferred-slot …)` declarations have `slot_count == 0`. `tp_rule_def` is
serialized by `pack.c`'s rule-table loop (currently writes `symbol + flags +
IR(body)` per rule); adding `slot_count` requires a fourth word per rule entry
in the image and a corresponding read in `tp_load_image` — this is a `pack.c`
change and is covered by the `TP_PACK_ABI` bump. By contrast, the `slot_count`
field added to `tp_instruction` is a runtime-only struct (bytecode is rebuilt
from the grammar at parse time, never serialized) and requires no `pack.c`
change.

**`vmcompile.c` annotates each `TP_VM_CALL` instruction.** When lowering a
call to a rule, `vmcompile.c` reads `target_rule->slot_count` and records it as
a new `slot_count` field on the emitted `tp_instruction`. A new field on
`tp_instruction` is cleaner than encoding the count in the high bits of `target`
and avoids any masking logic at dispatch time.

**`tp_vm_call_enter` uses the annotated count.** When `tp_vm_call_enter`
processes a `TP_VM_CALL` instruction, it reads `instruction->slot_count` and
allocates that many zeroed `tp_slot_frame` entries from the parse arena into the
new frame's `slots` array. The frame's `slot_count` field is set to the same
value. Frames for rules with `slot_count == 0` receive a NULL `slots` pointer
and skip the arena allocation entirely.

**Generated C emitter.** When the emitter generates the C function for a rule,
it counts the `TP_OP_DEFERRED_SLOT` nodes in the rule's IR and allocates that
many `tp_slot_frame` entries from the parse arena at rule entry (matching the
VM interpreter path via `tp_vm_call_enter`). No `slot_count` annotation on the
call instruction is needed in generated code — the count is baked into the
generated C source as a fixed number of arena-allocation statements.

### Slot name resolution

`TP_OP_DEFER` and `TP_OP_PARSE_DEFERRED` resolve `slot_name` by walking the
frame stack from innermost to outermost, stopping at the first frame whose
`slots` array contains an entry with a matching `slot_name`. This is lexical
scoping: a `(defer heredoc-bodies …)` inside `BodyCommand` writes into the
innermost frame that declared `heredoc-bodies` — which is `BodyCommand`'s own
frame, not the outer `CompleteCommand`'s frame, even though both use the name.

## Parametrized patterns

### Pattern instantiation

`tp_param_instantiate(arena, grammar, rule_index, text, text_len)` is the sole
runtime function that produces a parametrized pattern. It looks up
`grammar->rules[rule_index]` to obtain the template IR, walks that IR tree,
copies each node into the arena, and replaces every `TP_OP_PARAM_REF` leaf
with a freshly allocated `TP_OP_LITERAL(text, text_len)`. The result is a
fully concrete IR subtree that the lexer runs without knowing how it was
constructed.

The walk is a simple recursive copy: for non-leaf nodes, copy the node and
recurse into children; for `TP_OP_PARAM_REF`, emit `TP_OP_LITERAL` instead.
The arena passed to `tp_param_instantiate` is `state->parser->arena` (the parse
arena); its lifetime spans the whole parse, so instantiated patterns remain
valid until the parse completes. The lexer's lazy token-program lowering
(`ir->token_program`) writes back into the instantiated IR node the first time
a given pattern is matched. Instantiated nodes are therefore mutable (they are
arena-local to the parse and not shared across parsers or parse runs), and the
lazy write-back is safe.

`tp_param_stack` (the per-parse runtime stack of instantiated patterns for a
goal-table entry) lives in `tinyparse_parser`, not in `tinyparse_grammar`.
The grammar is loaded once and shared across concurrent parses; each parser
allocates its own `param_stacks` array (one `tp_param_stack` per parametrized
goal-table entry) in the parse arena when the parser is initialized. The
`tp_lex_goal` struct carries only the compile-time `template_rule_index` and
the `is_parametrized` flag; no runtime state lives in the grammar.

`param_stacks` is indexed by a **compacted index** over only the parametrized
entries, because parametrized entries are rare and a full-width array indexed by
the raw `lex_goals[]` position would waste space. At grammar-compile time, the
compiler assigns a compact `param_index` (0, 1, 2, …) to each parametrized
`tp_lex_goal` entry in the order they appear in `grammar->lex_goals[]`.
Non-parametrized entries receive no index. The `goal_entry_index` stored in
`TP_OP_PARSE_DEFERRED` (and in the corresponding IR operand’s `max` field) is
this compact index, not the raw `lex_goals[]` position. The grammar stores the
total count of parametrized entries as `grammar->param_goal_count`, a new
`uint32_t` field on `tinyparse_grammar`. At parser-init time,
`tinyparse_parser_new` allocates `param_goal_count` zeroed `tp_param_stack`
entries from the parse arena; no scan of `lex_goals[]` is needed at init time
because the count is precomputed.

### Goal-table entry layout

`tp_lex_goal` is updated to distinguish static redirect entries from
parametrized entries. The third field changes meaning; a flag records which
kind each entry is:

```c
/* ** One entry in the grammar's lexical goal table.
** For static entries, target_symbol is the interned goal to redirect to.
** For parametrized entries, template_rule_index is the internal rule whose
** body is instantiated at drain time; target_symbol is unused (zero).
** All fields are uint32_t so the struct is trivially serializable. */
typedef struct tp_lex_goal {
    uint32_t goal_symbol;           /* interned symbol of the owning goal */
    uint32_t trigger_symbol;        /* interned symbol of the trigger token */
    union {
        uint32_t target_symbol;         /* static: redirect target goal */
        uint32_t template_rule_index;   /* parametrized: internal rule index */
    };
    uint32_t is_parametrized;       /* 0 = static redirect, 1 = parametrized */
} tp_lex_goal;
```

The on-disk layout for each `tp_lex_goal` entry is four `uint32_t` words in
this order: `goal_symbol`, `trigger_symbol`, union-word (`target_symbol` or
`template_rule_index` — the same bytes regardless of which union member is
active), `is_parametrized`. `pack.c`'s write loop must be extended from three
words to four; the read loop must likewise read four words and reconstruct the
union and flag. `TP_PACK_ABI` must be bumped because the meaning of the third
word has changed and a fourth word is now present. The four new `TP_OP_*`
values are appended after the existing `TP_OP_EOF` entry (not inserted before
it), so `TP_OP_EOF`'s numeric value is unchanged and the ABI tag changes only
from `sizeof(tp_lex_goal)` growing from 12 to 16 bytes plus the new enum
values at the tail.

The template rule appended by the `(param …)` compiler is an ordinary rule in
`grammar->rules` and is therefore already serialized by `pack.c` along with
every other rule. No new serialization path is required.

### Goal-table stack

A parametrized goal-table entry's runtime stack lives in `tinyparse_parser`
(see [Pattern instantiation](#pattern-instantiation)). The stack type:

```c
typedef struct tp_param_stack {
    const tp_ir **patterns;  /* arena-allocated, grows by doubling */
    uint32_t      depth;
    uint32_t      capacity;
} tp_param_stack;
```

`parse-deferred` prologue: call `tp_param_instantiate`, push the result.
`parse-deferred` epilogue: pop (decrement `depth`; the arena allocation stays).

The lexer, when resolving a goal-table redirect for a parametrized entry,
reads `stack->patterns[stack->depth - 1]`. Depth zero means the entry is
active but unparametrised — the lexer treats it as no-match, which causes an
ERROR node in the body rule. A grammar-compile-time warning SHOULD flag any
code path that can reach a `(param …)` entry outside a `(parse-deferred …)`.

When a `tinyparse_parser` is re-initialized with a different grammar (the
existing API permits this), `param_stacks` must be re-derived.
`tinyparse_parser_reset(parser, new_grammar)` (or the equivalent re-init
path) must free the old `param_stacks` block if the parse arena is being reset,
then allocate `new_grammar->param_goal_count` zeroed `tp_param_stack` entries
from the (reset) parse arena. If the arena is not being reset (incremental
reuse), `param_stacks` must be reallocated from the arena afresh — the old
entries are abandoned. The simplest contract: `tinyparse_parser` must always
be fully re-initialized (arena included) when changing grammars; reuse across
grammar changes with a live arena is unsupported.

### Why a stack, not a single value

The outer `HeredocBody` (delimiter `EOF`) is on the call stack when the inner
`HeredocBody` (delimiter `NESTED`) is entered. A single slot would require
save/restore at every `HeredocBody` entry. The stack makes the push/pop in
`parse-deferred` the only bookkeeping site: inner pushes on top, inner pops,
outer pattern is restored without touching `HeredocBody` itself.

## Rule-level NOT

`TP_OP_NOT` already stores two children: child[0] is the negative lookahead
and child[1] is the "match if not" body. The VM saves the input position,
tries child[0], restores the position regardless of outcome, then fails if
child[0] matched and executes child[1] if it did not.

child[0] is always a token-level pattern — it is a lookahead into raw bytes
and must be expressible as a literal, class, `any`, or a composite of those
token-level combinators. child[1], however, may be either token-level or a
rule reference. The lowering path in `vmcompile.c` therefore depends on what
child[1] contains:

- **Token-level child[1]** (a `TP_OP_LITERAL`, `TP_OP_CLASS`, `TP_OP_ANY`,
  or a composite of those with no `TP_OP_REF` anywhere in the subtree): lower
  `TP_OP_NOT` to `TP_VM_BYTE` as today. The existing
  `tp_vm_do_byte_pattern` / `tp_token_match_memo` path handles the whole
  expression.

- **Rule-level child[1]** (a `TP_OP_REF` node, or any composite subtree
  containing a `TP_OP_REF`): lower `TP_OP_NOT` to a new `TP_VM_NOT_RULE`
  instruction. `TP_VM_NOT_RULE` saves the input position, executes the
  token-level lookahead child[0] via the token matcher, restores the
  position, fails if child[0] matched, and dispatches child[1] via
  `TP_VM_CALL` if child[0] did not match. The token matcher is never handed
  a `TP_OP_REF` node; the rule call is handled by the rule-call machinery as
  normal. Adding `TP_VM_NOT_RULE` to `tp_vm_op` is part of the `TP_PACK_ABI`
  bump — the new enum value is appended after `TP_OP_EOF`, so the existing
  `TP_OP_EOF` ordinal is unchanged.

The grammar compiler inspects child[1] at compile time to select the path. The
VM gains the `TP_VM_NOT_RULE` opcode for the rule-level case. The existing
`TP_VM_BYTE` opcode is unchanged.

The grammar reader currently restricts child[1] to the keyword `any`. The
change: when the reader encounters `(not EXPR0 EXPR1)` and EXPR1 is not `any`,
compile EXPR1 normally and use the result as child[1]. `(not HeredocDelimLine
BodyCommand)` — the content pattern in `HeredocBody` — requires this extension.

## Grammar reader and compiler changes

### New clauses

The reader's s-expression dispatch MUST handle:

- `(deferred-slot NAME)` → emit `TP_OP_DEFERRED_SLOT(intern(NAME))`
- `(defer SLOT EXPR)` → emit `TP_OP_DEFER(intern(SLOT), compile(EXPR))`
- `(parse-deferred SLOT RULE-NAME)` → emit `TP_OP_PARSE_DEFERRED(intern(SLOT), resolve_rule(RULE-NAME))`
- `(lex-goal NAME key: (param PARAM-NAME BODY))` → compile `BODY` with `PARAM-NAME` references emitted as `TP_OP_PARAM_REF`; append the compiled IR as a new internal rule in the grammar's rule table; store that rule's index in the goal-table entry as `template_rule_index` and set `is_parametrized = 1`; reject a static target in the same entry

### Compile-time scope validation

At grammar-compile time:

- Every `(defer SLOT …)` MUST reference a SLOT declared by `(deferred-slot SLOT)` in a lexically enclosing rule. A dangling `defer` is a compile error.
- Every `(parse-deferred SLOT RULE)` MUST reference a SLOT declared in the same rule's body or an enclosing rule's body. A declared-but-never-drained slot SHOULD produce a warning (it is legal but almost certainly a mistake).
- A `(param …)` goal-table entry reachable from outside any `(parse-deferred …)` SHOULD produce a warning.
- BODY inside `(param NAME BODY)` MUST contain only token-level combinators
  (`TP_OP_LITERAL`, `TP_OP_CLASS`, `TP_OP_ANY`, `TP_OP_SEQ`, `TP_OP_CHOICE`,
  `TP_OP_REPEAT`, `TP_OP_OPTIONAL`, `TP_OP_NOT`, `TP_OP_PARAM_REF`). A rule
  reference (`TP_OP_REF`), `(goal …)` form (`TP_OP_GOAL`), `(field …)` form
  (`TP_OP_FIELD`), `(deferred-slot …)`, `(defer …)`, or `(parse-deferred …)`
  anywhere in BODY is a compile error. The restriction check is a **recursive
  scan** of the BODY subtree: it descends into all children of every node,
  including children of `TP_OP_NOT`. A `TP_OP_NOT` node is allowed in BODY,
  but a `TP_OP_REF` anywhere in its subtree — including child[1] — is still
  caught by the recursive scan and is a compile error. In other words, the
  allowed/forbidden list applies at every node in the BODY subtree, not just
  at the top level.

### Rule-level NOT reader change

In the clause handling `(not EXPR0 EXPR1)`: compile EXPR0 normally. If EXPR1
is the atom `any`, emit `TP_OP_ANY` for child[1] as before. Otherwise compile
EXPR1 normally and use the result. The two-child structure of `TP_OP_NOT` is
unchanged.

## Generated C emitter changes

The emitter produces generated code for each new opcode. Generated parsers
and the VM MUST produce byte-identical trees (the `cc-diff` gate).

| Opcode | Generated code shape |
|---|---|
| `TP_OP_DEFERRED_SLOT` | Allocate the `tp_slot_frame` from the parse arena at rule entry: `tp_slot_frame *slot_N = tp_arena_alloc(arena, sizeof(tp_slot_frame)); *slot_N = (tp_slot_frame){name, NULL, 0, 0};`. The pointer is a local variable but the storage is arena-backed, matching the VM interpreter path via `tp_vm_call_enter`. A C-stack local is not used here because its lifetime would end when the function returns, making it unsafe to access via pointers held in the slot-save stack or during drain. |
| `TP_OP_DEFER` | Match the child pattern, capture `(input + match_start, match_len, node)`, call `tp_slot_push(slot_N, item, arena)` |
| `TP_OP_PARSE_DEFERRED` | Emit a loop over `slot_N->items[i]`: call `tp_param_instantiate(arena, grammar, op.rule_index, item.text, item.text_len)`, then `tp_param_stack_push(&parser->param_stacks[op.goal_entry_index], pattern, arena)`, then the target rule function, then `tp_param_stack_pop(&parser->param_stacks[op.goal_entry_index])` |

`tp_param_instantiate`, `tp_slot_push`, `tp_param_stack_push`, and
`tp_param_stack_pop` live in the tinyparse runtime library — not in the
generated code — so the generated parser stays lean and the parametrized logic
has exactly one implementation.

## Slot rollback on backtrack

`TP_OP_DEFER` must be backtrack-safe. When a `(choice …)` alternative or an
`(optional …)` body fails, any items enqueued during that branch must be
discarded so the slot appears as it did before the branch was entered.

### Parallel slot-save stack

The rollback mechanism uses a **parallel slot-save stack** maintained on
`tinyparse_parser` alongside the existing choice-save stack. Three new
fields are added to the parser struct (not to `tp_vm_frame`):

```c
uint32_t *slot_save_stack;   /* arena-allocated, grows by doubling */
uint32_t  slot_save_count;
uint32_t  slot_save_capacity;
```

The `tp_vm_frame` struct is unchanged.

### Saving slot state on choice entry

When `TP_VM_CHOICE` or `TP_VM_OPTIONAL` saves state (before entering a
speculative branch), it additionally pushes the `count` of every active slot
onto `slot_save_stack` in a single batch. "Active slots" means all
`tp_slot_frame` entries found across all VM call frames currently on the
stack, visited from innermost to outermost (`tp_vm_stack.frames[0..count)` in
reverse, innermost last by convention). Frames where `slots == NULL` (rules
with `slot_count == 0`) are skipped — dereferencing `frame->slots` without
checking for NULL would crash. The batch is preceded by a sentinel value
(`TP_NO_INDEX`) so the restore step knows how many values to pop:

```
slot_save_stack: [..., TP_NO_INDEX, count_A, count_B, count_C]
                              ^--- batch start marker
```

Each slot's count is pushed in the same traversal order used for name
resolution (innermost frame first). The push happens before
`TP_VM_CHOICE`/`TP_VM_OPTIONAL` saves its own state fields (`builder->count`,
`position`, `sink->count`).

If the arena allocation for the batch fails mid-write — that is, the sentinel
has been pushed but not all slot counts have been written yet — the save path
MUST roll back `slot_save_count` to its value before the push began before
returning `TP_VM_OOM`. The partial batch (sentinel plus any partial counts) is
abandoned in the arena (append-only; cannot be freed), but `slot_save_count`
is reset so subsequent operations do not see a malformed partial batch.

### Restoring slot state on backtrack

On the backtrack path, `TP_VM_CHOICE`/`TP_VM_OPTIONAL` first restores its
own saved state fields, then pops exactly one sentinel-delimited batch from
`slot_save_stack`: pop values and restore each active slot's `count` field in
reverse order until a `TP_NO_INDEX` sentinel is found, then pop the sentinel
itself. With nested choices there are multiple `TP_NO_INDEX` sentinels on the
flat stack — one per nested choice entry. Popping only until (and including)
the first sentinel encountered ensures exactly one push/pop cycle per
choice entry/backtrack pair, regardless of nesting depth. The outer choice's
sentinel remains beneath on the stack and is consumed only when the outer
choice itself backtracks. Truncating `count` makes any items appended during
the failed branch invisible to subsequent operations; the arena memory for
those items is not freed (the arena is append-only), but it is unreachable.

### Placement constraint on `(parse-deferred …)`

`(parse-deferred SLOT RULE)` is a **committed operation** and MUST NOT appear
inside a speculative context — a `(choice …)` alternative or an `(optional …)`
body that may fail.

The reason: `parse-deferred` drains the slot FIFO and, for each item, invokes
RULE, which advances the input cursor. Once an item is drained and RULE has
consumed input, there is no safe way to un-drain the slot or restore the
input position to where it was before that invocation. The goal-table stack
push is similarly non-atomic: it happens item by item, so a failure mid-drain
leaves the stack partially pushed. Neither the drained items nor the partial
goal-stack pushes are covered by the slot-save mechanism above.

The grammar compiler SHOULD detect and warn when `(parse-deferred …)` appears
statically inside a `(choice …)` alternative or an `(optional …)` body. When
the compiler cannot determine the context statically, the parser's behavior
under backtracking past a `parse-deferred` is undefined.

`parse-deferred` MUST NOT appear in a position within a `(repeat …)` body
where the repeat body can fail after `parse-deferred` has run. Concretely: if
`parse-deferred` is not the last element of the `(seq …)` that forms the
repeat body, and something after it can fail, the repeat's backtrack mechanism
cannot undo the drain. The compiler SHOULD warn when `parse-deferred` appears
in a `(repeat …)` body in a non-final position.

The safe pattern in all three contexts (`(choice …)`, `(optional …)`,
`(repeat …)`) is the same: `parse-deferred` as the **last element of a
committed `(seq …)`**, where nothing after it can fail on the current
iteration. For example, `BodyCommand`'s structure (Pipeline → Newline →
`parse-deferred`) is safe because `parse-deferred` is the last element:
`Newline` must match before `parse-deferred` runs, and nothing follows it in
the body.

For `(seq …)` outside any of these speculative contexts: a `(seq …)` that has
already matched earlier mandatory elements is committed to its current
alternative; later elements are not on a backtracking path. For example,
`CompleteCommand`'s `(seq …)` matches its mandatory command elements first,
then ends with `(parse-deferred heredoc-bodies HeredocBody)`. Backtracking
past the seq's start is still possible before any mandatory element is
consumed, but `parse-deferred` is never reached on that backtracking path.

## Implementation order

The ten milestones below build on each other. Each one has a concrete
verification step; do not proceed to the next until the step is green.

1. **Reader: parse new forms, emit placeholder IR.**
   `tinyparse check examples/heredoc/heredoc.scm` reaches "grammar
   compiled" without error. The VM will not execute the new opcodes correctly
   yet — that is expected.

2. **VM: `TP_OP_DEFERRED_SLOT` and `TP_OP_DEFER` (without drain).**
   The slot is allocated and populated; `parse-deferred` is a no-op.
   Verify that `(defer …)` matches correctly: the produced CST node is
   identical to the unwrapped match (compare with `(field word: Word)` vs
   `(field word: (defer slot Word))`).

3. **VM: `TP_OP_PARSE_DEFERRED` (non-parametrised).**
   Drain the slot, invoke the target rule per item. Use a test grammar
   where the body rule takes no parametrised input — confirm the FIFO
   ordering with two items.

4. **Rule-level NOT reader extension.**
   `(not Word SimpleCommand)` compiles without error. Verify by hand that a
   rule using it stops the repeat at the correct input position.

5. **Slot rollback on backtrack.**
   Write a grammar with a `(choice (seq (defer …) fail) (seq …))` and confirm
   that after the first branch fails, the slot is empty, not populated.

6. **Parametrised goal: stack infrastructure.**
   Add `tp_param_stack` to parametrised goal-table entries. Confirm that
   `tp_param_stack_push` and `tp_param_stack_pop` are symmetric (a push
   followed by a pop leaves the stack at its prior state).

7. **Pattern instantiation.**
   Implement `tp_param_instantiate`. Wire it into the `parse-deferred` prologue.
   Run:
   ```sh
   printf 'cat <<EOF\nhello\nEOF\n' \
     | tinyparse run examples/heredoc/heredoc.scm --dump-tree
   ```
   Expect: no ERROR or MISSING nodes.

8. **Nested heredoc.**
   Run the full nested example from `examples/heredoc/README.md`. Expect: the
   inner `HeredocBody` (delimiter `NESTED`) and the outer (delimiter `EOF`)
   both terminate at the correct lines, no ERROR nodes.

9. **Emitter: generated C support for new opcodes.**
   Add the generated-code shapes from the table above. Run:
   ```sh
   mise run gate:vm-diff --full
   mise run gate:cc-diff --full
   ```
   Both MUST be green for any grammar that uses the new forms.

10. **`grammars/std/posixsh/` grammar.**
    Begin the POSIX sh port. The infrastructure is ready; the grammar work
    follows the `add-std-grammar` skill.

## Failure cases

**Empty slot at drain.** A `SimpleCommand` with no `<<` redirects leaves
the slot empty. `parse-deferred` iterates zero times and is a no-op. Correct.

**Multiple heredocs on one line.** `cat <<A <<B` enqueues `[{A}, {B}]`.
The drain processes them left to right, matching body A then body B. POSIX
requires this order; the FIFO gives it.

**Unclosed heredoc.** `(repeat (not HeredocDelimLine BodyCommand))` exhausts
input without matching `HeredocDelimLine`. The mandatory `(field end:
HeredocDelimLine)` fails; normal parse-error recovery emits a MISSING node.
The error is contained inside `HeredocBody`; the host parse continues.

**RULE failure mid-drain.** If RULE fails on one of the slot items — the unclosed
heredoc case above is a concrete example — `parse-deferred` does not abort the
drain. Normal parse-error recovery runs inside RULE, synthesizing MISSING nodes
and advancing the cursor. `parse-deferred` pops the parametrized pattern as usual
and continues to the next slot item. Any remaining items are drained in order. The
input cursor is left wherever RULE's recovery stopped; subsequent slot items
receive that as their starting position. Error nodes are fully contained within
each RULE invocation's CST subtree and are invisible to the host parse.

**Delimiter mid-line.** `some text EOF` at a content position: the
instantiated pattern is `(seq delimiter "\n")` evaluated at the current scan
position. That position is always at line-start (each `BodyCommand` ends with
an explicit `Newline`), so the pattern implicitly requires line-start without
an anchor opcode.

Note: the line-start guarantee is a **grammar author obligation**, not a
property the engine enforces. The engine does not check that parsing resumes
at a line boundary before trying `HeredocDelimLine`. If a grammar using
`parse-deferred` constructs `BodyCommand` rules that do not end with
`Newline`, or embeds `parse-deferred` in a context where the cursor is not at
a line start, the instantiated `(seq delimiter "\n")` pattern would be tried
at the wrong position and silently fail to match — producing a MISSING or
ERROR node rather than a diagnostic about the invariant violation. The grammar
author must maintain the invariant that the cursor is at a line start whenever
the next `HeredocDelimLine` attempt occurs.

**Parametrised stack at depth zero.** If `HeredocDelimLine` is evaluated
while the stack is empty — which should not happen if the grammar is correct,
since `HeredocBody` is only reachable through `parse-deferred` — the lexer
returns no match, `HeredocBody` produces an ERROR node, and the host parse
continues. The grammar-compile-time warning (milestone 1) catches the
structural cause before it reaches runtime.

**Deeply nested heredocs.** The stack is arena-allocated; the depth is
bounded by the heredoc nesting in the input. Shell code rarely nests heredocs
more than two or three levels deep. No explicit depth limit is needed.

**Delimiter equals outer delimiter.** `cat <<EOF / cat <<EOF / inner / EOF /
outer / EOF` — the inner drain pushes the `EOF\n` pattern on top of the outer
`EOF\n` pattern. The inner `HeredocBody` consumes the first `EOF\n` and stops.
The inner drain pops; the outer `HeredocBody` sees the second `EOF\n` and
stops. Correct because the stack, not a single slot, holds the active pattern.

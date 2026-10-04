# The codegen transformation: the IR grammar → the parser a human would write

This is the blueprint for the Phase 17 emitter. The generator walks the **IR** — the
compiled grammar as *named* rules and tokens — and emits the recursive-descent parser
a person would write by hand from that grammar. It does not touch the VM lowering
(`pp_program`/`pp_instruction`); the VM (`pp_vm_run`) is only the differential oracle
that `cc-diff` checks the output against, byte for byte.

## The source of truth is the IR, by name

A compiled grammar is, literally:

```c
struct pocketparse_grammar {
    pp_interner   interner;     /* symbol id  <->  name  (pp_intern_name) */
    pp_token_def *tokens;  uint32_t token_count;   /* { symbol; pp_ir *pattern; } */
    pp_rule_def  *rules;   uint32_t rule_count;    /* { symbol; pp_ir *body;    } */
    pp_lex_goal  *lex_goals; pp_extra *extras; uint32_t start_symbol; …
};
```

Each `pp_rule_def.body` and `pp_token_def.pattern` is a `pp_ir` tree of `PP_OP_*`
nodes — the grammar, not a lowering. Every `symbol` resolves to its source name
through the interner. So the emitter's job is a tree-walk of named definitions, and
the output reads like the grammar:

- one C function **per named rule** — `<prefix>_parse_<rule_name>` (`parse_expression`,
  `parse_arrow_function`), NOT `rule_0`/`rule_1`;
- one C function **per named token** — `<prefix>_scan_<token_name>` (`scan_identifier`,
  `scan_string`);
- `PP_OP_REF` → a direct call to `parse_<name>` (rule) or `scan_<name>` (token), the
  name taken from the referenced symbol;
- node kinds, fields, aliases, and goals named from their symbols
  (`PP_KIND_arrow_function`, `PP_FIELD_body`, `PP_GOAL_expr`).

`<prefix>` defaults from the grammar name + language and is overridable, so
`javascript` and `json` parsers coexist in one program (W17-3).

## A worked example — read it and it is obviously hand-written

Grammar:

```scheme
(rule List (seq (field open: "(") (repeat Form) (field close: ")")))
(rule Form (choice Atom List))
```

Emitted (sketch; `node`/`sink`/`extras` are the shared CST runtime, below):

```c
static int parse_Form(parser *p, sink *out);

static int parse_List(parser *p, sink *out) {
    if (++p->depth > p->depth_limit) { p->depth--; return FAIL; }      /* crash guard */
    consume_extras(p, out);
    uint32_t start = p->pos;
    uint32_t node  = node_open(p, PP_KIND_List, start);
    sink kids = SINK_INLINE;
    if (scan_lparen(p, PP_FIELD_open, &kids) != MATCH) goto fail;       /* (field open: "(") */
    for (;;) {                                                          /* (repeat Form) */
        uint32_t m = p->pos, k = kids.count;
        if (parse_Form(p, &kids) != MATCH) { p->pos = m; kids.count = k; break; }
        if (p->pos == m) break;                                         /* zero-width guard */
    }
    if (scan_rparen(p, PP_FIELD_close, &kids) != MATCH) goto fail;      /* (field close: ")") */
    node_close(p, node, &kids); sink_push(out, node);
    p->depth--; return MATCH;
fail:
    p->builder.count = node; p->pos = start; p->depth--; return FAIL;
}

static int parse_Form(parser *p, sink *out) {                           /* (choice Atom List) */
    uint8_t c = peek(p);                                                /* FIRST sets baked */
    if (FIRST_Atom_has(c) && parse_Atom(p, out) == MATCH) return MATCH;
    if (c == '(' /* FIRST(List) */ && parse_List(p, out) == MATCH) return MATCH;
    return FAIL;
}
```

There is no program array, no opcode, no frame loop — the rule names, the field names,
the `(` literal, and the FIRST-set branches are all in the source, because they were
all in the IR.

## What is baked vs. what stays a runtime primitive

The grammar is baked away; the primitives that *build the packed CST and implement the
special features* stay shared helpers — exactly as a hand-written parser still calls
its own `make_node`/`intern`. This is what "ax orchestrates the state machines" means.

| baked from the IR into the generated C | stays a shared runtime primitive |
|---|---|
| a named function per rule/token; `REF` → a named call | the arena CST builder (`node_open`/`node_close`, packed nodes) |
| `SEQ`/`CHOICE`/`REPEAT`/`OPTIONAL`/`NOT` as control flow | the child-index **sink** (frame-inline) |
| `choice_first_set` (already on each IR node) → byte tests | the extras policy engine (`consume_extras`) |
| per-token lexers from the token pattern IR | the **inject** runtime (`pp_inject_run`) for nesting |
| `recover_*` constants; kinds/fields/aliases/goals by name | the diagnostics buffer; the arena; the actor host |

The generated parser carries **no packrat memo** — it is the code a human would
type, and a hand-written recursive-descent parser does not memoize. The rule-memo
backstop lives **only on the VM** (`pp_vm_run`), which must bound backtracking for
arbitrary runtime-supplied grammars. The std grammars the generated parser ships are
left-factored to linear, so the generated path needs no backstop; dropping it is
byte-identical (a memo is a pure cache) and is what makes the generated parser read
*and run* like hand-written descent. See "Memo: VM-only" below.

## Each IR op → C

Translating a `pp_ir` node, given the active `(field, alias)` decoration and the
caller's `sink`:

- **`PP_OP_REF`** → `parse_<name>` if the symbol is a rule, `scan_<name>` if a token.
  A rule call opens a node of that rule's kind; a token call matches a leaf. Leading
  extras are consumed before the call (matching the grammar's trivia policy).
- **`PP_OP_SEQ`** → its children in order; a child's failure jumps to the rule's
  `fail`. The grammar's end-of-input recovery (synthesize a missing required element
  when recovery is armed) is emitted inline.
- **`PP_OP_CHOICE`** → save `(pos, builder.count, sink.count)`; consume shared leading
  extras once; for each child, a **baked FIRST-set test** (`choice_first_set`, already
  computed on the IR by following references through the grammar) guards the attempt;
  restore the marks between attempts; first match wins. When `choice_first_open` is
  set (nullable / lead byte not pinned down), the guard is omitted and the alternative
  is always tried — soundness preserved.
- **`PP_OP_REPEAT(min,max)`** → a `for`/`while` with the bound checks and the
  zero-width guard; recovery disarmed inside; per-iteration mark save/restore.
- **`PP_OP_OPTIONAL`** → save marks, try the child, restore on failure, always succeed.
- **`PP_OP_NOT`** (negative lookahead) → save `(pos,count)`, run `children[0]`; if it
  matched, restore and fail; else restore and match `children[1]` if present.
- **`PP_OP_FIELD` / `PP_OP_ALIAS`** → translate `children[0]` with the field, or the
  alias kind, threaded into the node it opens.
- **`PP_OP_EOF`** → consume extras; succeed iff `pos == length`.
- **Token leaf ops** (`PP_OP_LITERAL`/`CLASS`/`CLASS_NAME`/`ANY`, and the
  `NOT_KEYWORD` form) appear inside token patterns and compile to the per-token lexer.

## The lexer: per-token baked C (the big throughput win)

Each `pp_token_def.pattern` compiles to `scan_<name>`: a literal keyword → byte
compares (`p[0]=='i' && p[1]=='f'`); a class → a loop over a `static const uint8_t
class[256]`; a repeat of a class → a tight `while (class[*q]) q++`; the `not`-guarded
scans (comment/string/regex) → bounded `while (q<end && !delim) q++`. A token call
consumes leading extras (unless `no_extras`), matches at `pos` **consulting
`active_goal`** for the redirect, pushes a leaf node `(token-kind, start, end)`, and
advances. This replaces the generic token VM (`pp_token_run`, the ~25% slice) with the
straight-line code a hand lexer is — the main reason the result should reach toward
tsc/tsgo. Tables are `static const` (the `no-statics` gate forbids only *mutable*
statics).

## The context primitives (heredocs) — preserved by name

- **`PP_OP_GOAL`** → `uint32_t saved = p->active_goal; p->active_goal = PP_GOAL_<name>;
  …child…; p->active_goal = saved;`. The baked token matchers consult `active_goal`,
  and the grammar's `lex_goals` table (a compile-time fact) tells the emitter which
  terminals are redirect triggers, so it emits the redirected scan directly. This is
  the regex-vs-divide / heredoc-body mechanism, intact.
- **`PP_OP_NO_EXTRAS`** → save/set/restore `p->no_extras` around the child.
- **`PP_OP_CLASS_NAME`** → the baked class matcher consults the host class binding.

## Error reporting — baked, first-class

**`PP_OP_RECOVER`** carries `recover_message`/`recover_help`/`recover_label`/
`recover_insert` on the IR node. The emitter bakes them: try the body; on failure,
restore, arm recovery with those literal strings and the insert token, re-run; the
shared `recover_insert` primitive synthesizes the zero-width *missing* node and the
`pp_diag_recover(message, help, label)` diagnostic. Phase 17 adds a
**diagnostics-parity gate** so the generated parser's diagnostics match the VM's on a
broken-input corpus — error reporting may be extended, never regressed. Because the
messages are baked from the IR, the generated parser can also report richer,
location-precise diagnostics a human would write.

## Language embedding / nesting — preserved, made native, coordinated by ax

**`PP_OP_INJECT`** carries `children[0]` (the region pattern) and `language_symbol`.
The emitter bakes the host side as: match the region to get `[span_start, span_end)`,
drop the raw region nodes, then hand the span to the runtime inject hook:

```c
/* PP_OP_INJECT, embedded language = language_symbol */
uint32_t span0 = p->pos;
if (region(p, &kids) != MATCH) goto fail;
uint32_t span1 = p->pos;
p->builder.count = region_node;                              /* drop the raw region nodes */
pp_inject(p->session, PP_LANG_<name>, span0, span1, out);    /* ax resolves + runs + splices */
```

The host parser names the embedded language (a compile-time constant from
`language_symbol`) but is **not linked against it**. The resolution, execution, and
isolation are ax's job. This is what "ax orchestrates the state machines" means in
practice, and it is the design's answer for nesting.

### How ax is used for the inject hook

1. **The parse is an ax actor — the *session*.** This already exists
   (`pp_execute_vm_actor`): a parser actor is spawned, `pocketparse_push` routes input
   as `input.chunk` events, the parse runs in `input.end`. The session actor owns the
   arena, the tree builder, the diagnostics, and the input buffer. The generated native
   parser runs *inside* the session, against that state.

2. **A language registry, held by the engine/system, resolves `language_symbol` → a
   parser.** Separately-compiled grammars (each with its own symbol prefix) register
   their entry point by language name —
   `pocketparse_register_language(engine, "javascript", <javascript parse entry>)` —
   and `pp_inject` looks the embedded language up there. A registry entry may be a
   *generated* parser (native nesting) or a VM grammar (the dynamic case); the host
   does not care which. This is what lets multiple languages coexist and nest in one
   project without the host binary linking every embeddable grammar, and it
   generalizes today's compile-time `embedded_grammar` (which stays a valid entry).

3. **The embedded parse is a supervised child of the session.** ax runs language L's
   parser over `[span0, span1)` **sharing the session's arena and builder**, so the
   embedded subtree is constructed in place and spliced into `out`, every node stamped
   with the language tag (the same `language_tag` `--dump-tree` already surfaces).
   Supervision is the reliability guarantee: a malformed or failing embedded parse is
   isolated — it yields a language-tagged error/opaque node plus a diagnostic, and the
   host tree and process are untouched. Injection depth stays bounded (by nesting of
   injections, not input depth), as it is today.

4. **ax picks the execution mode:**
   - **Synchronous** for the fast, pre-compiled case: the span is fully known, so the
     embedded generated parser runs to completion over it and splices — no suspension,
     full native speed. This is the common path.
   - **Message-coordinated** for the streaming case: when the embedded span arrives
     incrementally, the session drives language L's *actor* with the span as messages
     and splices the reply when complete. The generated native host parser only needs
     the synchronous mode; the async/streaming embedding is the VM/actor engine's job —
     the same split as the buffer-friendly `NEED_MORE_INPUT` boundary.

Why ax rather than a bare function-pointer table: the table would cover only the
static, in-binary case. ax additionally gives **registration/resolution by name**
(dynamic, separately-compiled composition), **supervision/isolation** of embedded
parses (the reliability that makes complicated nesting safe), and **message
coordination** for streamed embedding — all on the *same session abstraction that
already owns the arena and streaming*. The per-grammar symbol prefix keeps two
generated parsers collision-free in one binary; ax keeps them composable at runtime.
This is W17-3.

## The CST — built exactly as today

`node_open`/`node_close` are the inlined node lifecycle (open a node of the rule's
kind at the cursor; gather child indices in the sink; on success link children, extend
the span, push the node; on failure rewind `builder.count`, `pos`, `sink.count`).
`node_open` is `pp_tree_builder_push` and `node_close` is `pp_vm_sink_close` — the same
calls the VM makes, so the tree is the same tightly-packed, arena-allocated,
cursor-navigable CST. Non-negotiable and unchanged.

**W17-CST — the build path is incremental, not a second pass.** The child links are
wired *as each child is emitted*: `pp_vm_sink_push` sets the child's `parent` and the
previous sibling's `next_sibling` while the just-written node is still hot in cache, so
the close (`pp_vm_sink_close`) is O(1) — stamp `first_child`/`child_count`, terminate the
final sibling, extend the span — rather than the cold second pass over the child array the
earlier `pp_vm_link_children` performed (that function survives only for the root /
buffer-friendly sinks, once per parse). The previous sibling is read from the sink's
`items[count-1]`, the same ledger a failed ordered-choice attempt truncates, so the
incremental link is correct after rollback with no extra bookkeeping. The node array is
pre-sized from the input length so its doubling-copy schedule no longer dominates
`pp_tree_builder_push`. Both are pure performance restructurings of the *internal* build
path — the tree the cursor navigates is byte-identical (cc-diff 271/271).

**W17-FLAT — hidden rules collapse in the shared close.** A rule marked
`(hidden …)` (PP_RULE_HIDDEN) is omitted from the tree when it was a pure
pass-through: the body contributed exactly one child and the call carried no
alias. The decision lives in the shared node-close epilogue — `pp_vm_exit` (the
generated parser's inline CALL epilogue) and `pp_vm_call_exit` (the VM's), the
same two functions both engines already share for link/close/push. When a hidden
rule turns out to be a pass-through, neither pushes the wrapper node; both call
`pp_vm_collapse_hidden`, which re-stamps the single child with the call's field
and pushes *it* in the wrapper's place, leaving the wrapper node orphaned (nothing
links to it, so the cursor never reaches it). Because the mechanism is in the
shared close, the VM and the generated parser flatten identically by construction
— `cc-diff` stays 271/271 with no new code on the generated side beyond the marker
flag riding the rule. The hidden-by-symbol lookup is an O(1) byte array built once
per grammar (like the token index), consulted at CALL entry to set a frame flag.
A collapsed success is never memoized (the memo would snapshot the orphaned
un-collapsed subtree); the memo is a pure cache, so skipping the entry is sound.
The flattening removed ~61% of the React corpus's CST nodes (the single-child
expression-ladder and dispatch wrappers), matching tree-sitter's tree shape.

## Memo: VM-only — the generated parser has none, crash-safety, buffer-friendliness, prefix

- **Memo is VM-only.** The generated parser carries **no** rule memo: no probe, no
  store, no snapshot, no replay, and no `memo_active` / `call_attempts` /
  `memo_threshold` bookkeeping. A hand-written RD parser does not memoize, and that
  is what the generator types. The generated driver pins the memo OFF
  (`memo_threshold = 0xFFFFFFFF`, `memo_active = 0`) so even a CALL lifecycle that
  delegates to the shared prologue (recovery armed, a `no_extras` scope) cannot wake
  it. The VM (`pp_vm_run`) keeps the memo because it must bound super-linear
  backtracking for arbitrary runtime-supplied grammars; the std grammars the
  generated parser ships are left-factored to linear, so it needs no backstop.
  Removing the memo is byte-identical (a memo is a pure cache — `cc-diff` stays
  271/271) and removes per-rule bookkeeping a hand parser would never write. ax's
  role here is to manage the logical processes (the parse session, nested/embedded
  parsers, streaming), not a per-rule parse machinery.
- **Depth guard** — `++p->depth > p->depth_limit` returns FAIL; the limit is measured
  (above the deepest real input, below the `ulimit -s 512` overflow point).
- **NEED_MORE_INPUT** — the top-level item loop returns it when the buffer ends before
  an item completes; ax refills the growable arena buffer and resumes at the next item
  (native recursion can't suspend mid-stack, and ax has no fiber). The explicit-stack
  VM/actor stays the engine for arbitrary-granularity async push.
- **Symbol prefix** — every emitted symbol carries `<prefix>` from grammar name +
  language, overridable.

## Why this is both hand-written and correct

It is hand-written because it is generated from the IR by name: named functions, named
calls, literal tokens, FIRST-set branches — the grammar transcribed. It is correct
because the CST construction and every special feature are the *same runtime calls the
VM makes*, so `cc-diff` stays byte-identical and nothing is dropped. The speed is the
generic-engine-vs-hand-written gap closed: per-token lexers instead of a token VM,
compile-time FIRST tests instead of runtime prediction, a native call graph with
locals instead of an operand-reading loop.

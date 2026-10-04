/*
** internal.h -- shared internal contract for the freestanding pocketparse core.
**
** Everything declared here is private to the library; none of it appears in the
** public header. The modules (reader, grammar, token, parse, tree, diag) share
** these structs and helpers. Like the rest of src/ (except cli.c) this file is
** freestanding: it depends only on pocketparse.h, never on libc. All memory
** comes from arenas carved from the host grant (arena.c).
*/
#ifndef POCKETPARSE_INTERNAL_H
#define POCKETPARSE_INTERNAL_H

#include "pocketparse.h"

/* ** The region allocator (arena.c). The host's grant is owned by one pool; an
** arena bump-allocates over a chain of chunks drawn from that pool and is reset
** as a whole, never freed object by object. Every hand-out is 16-byte aligned,
** pp_alloc zeroes it and pp_alloc_raw does not, and NULL is the only
** out-of-memory signal. An arena never reallocates in place, so a growing array
** allocates a fresh, larger block and copies. A child arena shares its parent's
** pool: resetting it returns its chunks to the pool's free list for reuse, which
** is how one engine runs many parses without accreting their trees. */
typedef struct pp_pool  pp_pool;
typedef struct pp_chunk pp_chunk;
typedef struct pp_arena {
    pp_pool  *pool;
    pp_chunk *head;     /* the chunk being filled; older ones chained behind */
} pp_arena;

/* ** The root arena over a host grant, carved from the front of the grant;
** NULL when the grant cannot hold the bookkeeping plus one chunk. */
pp_arena *pp_arena_open(pocketparse_memory memory);

/* ** A child arena on the parent's pool (its bookkeeping comes from the parent,
** which must outlive it); NULL on a spent grant. */
pp_arena *pp_arena_child(pp_arena *parent);

/* ** Return every chunk of the arena to its pool's free list; the arena stays
** usable and everything allocated from it becomes invalid. */
void pp_arena_reset(pp_arena *arena);

/* ** n zeroed, aligned bytes from the arena, or NULL. */
void *pp_alloc(pp_arena *arena, size_t n);

/* ** n aligned bytes from the arena, NOT zeroed, or NULL. */
void *pp_alloc_raw(pp_arena *arena, size_t n);

/* ** One installed host binding: the public binding record stored verbatim. The
** name bytes are copied into the engine arena so the binding outlives the host's
** pocketparse_binding. For a CLASS binding, function is a pocketparse_class_fn the
** matcher calls per byte; user rides back to it unchanged. */
typedef struct pp_binding {
    pocketparse_string       name;
    pocketparse_binding_kind kind;
    void                    *function;
    void                    *user;
} pp_binding;

/* ** One compile-time grammar parameter: an interned-free name slice copied into
** the engine arena, plus a truthy flag. Parameters feed the (when PARAM: X)
** combinator at compile time -- they live on the engine, not the grammar, since
** the same source compiles to different grammars under different parameters. */
typedef struct pp_param {
    pocketparse_string name;
    int                value;
} pp_param;

/* ** One installed host plug: the public plug record stored verbatim. The name
** bytes are copied into the engine arena so the plug outlives the host's
** pocketparse_plug. emit is the message sink the engine calls on a boundary
** crossing (trace, diagnostic, module); user rides back to it unchanged. Plugs
** are cold-path host boundaries, never the hot scanner/parser loop. */
typedef struct pp_plug {
    pocketparse_string name;
    pocketparse_result (*emit)(void *user, const pocketparse_message *message);
    void              *user;
} pp_plug;

/* ** One registered embedded language (W17-3). `name' is the interned language
** name an (inject grammar: NAME ...) form resolves; `entry' is the host's
** registration verbatim -- the embedded grammar plus an optional generated-parser
** run function (NULL means run the bytecode VM over the grammar). The name bytes
** are copied into the engine arena; the entry's grammar/parse pointers are host-
** owned and must outlive the engine's parses. */
typedef struct pp_language_slot {
    pocketparse_string         name;
    pocketparse_language_entry entry;
} pp_language_slot;

/* ** The engine state. It lives inside the arena it owns. fault is the last
** error message surfaced by pocketparse_fault. The intern table maps grammar
** and tree names to dense symbol ids so node kinds and fields are compared by
** integer, never by string, on the hot path. module_resolve / module_user hold
** the host's module seam (see pocketparse_set_module_resolver): the freestanding
** core calls module_resolve to obtain an (import NAME)'d module's bytes, since it
** has no filesystem of its own. bindings/binding_count hold the host bindings
** registered by pocketparse_bind; a (class NAME) token primitive resolves NAME to
** a POCKETPARSE_BIND_CLASS binding here and calls it for each candidate byte.
** plugs/plug_count hold the host plugs installed by pocketparse_plug_install; an
** engine trace point (pp_trace) looks up the plug named "trace" here and, only if
** one is installed, builds a message and calls its emit -- otherwise it is a cheap
** no-op so the hot path pays nothing for tracing nobody asked for. */
struct pocketparse_engine {
    pp_arena                   *arena;
    pocketparse_string          fault;
    uint32_t                    flags;
    pocketparse_module_resolver module_resolve;
    void                       *module_user;
    pp_binding                 *bindings;
    uint32_t                    binding_count;
    uint32_t                    binding_capacity;
    pp_plug                    *plugs;
    uint32_t                    plug_count;
    uint32_t                    plug_capacity;
    pp_param                   *params;
    uint32_t                    param_count;
    uint32_t                    param_capacity;
    /* W67 rule-memo force override (host-set, since the freestanding core cannot
    ** read the environment). 0 = adaptive (the default: the memo wakes only when a
    ** parse goes super-linear); 1 = always on from the first call; 2 = off. The CLI
    ** maps POCKETPARSE_RULEMEMO=always/off to this so the differential can exercise
    ** the replay path; ordinary consumers leave it adaptive. */
    int                         vm_rule_memo_force;
    /* W17-3 language registry for native nesting. An (inject grammar: NAME ...)
    ** form resolves NAME here at parse time to the parser that re-parses the
    ** embedded region. Each slot maps an interned name id to a language entry whose
    ** parse function is NULL (run the bytecode VM over the entry's grammar) or set
    ** (a separately-compiled generated parser's run entry). Registering NAME wires
    ** a host parser to an embedded parser at runtime, so the host binary names the
    ** language but never links it directly. NULL/empty until the host registers a
    ** language; the compile-time embedded_grammar stays the fallback, so in-grammar
    ** injection without registration is unchanged. */
    pp_language_slot           *languages;
    uint32_t                    language_count;
    uint32_t                    language_capacity;
    /* W16-3 compiled-parser symbol prefix (host-set). When NULL the emitted
    ** parser keeps its historical file-scope symbol names (pp_parser_p<N>,
    ** pp_compiled_parser_load/_run, every emitted static) so existing callers and
    ** ci/pocketparse-cc-diff.sh are byte-for-byte unchanged. When set to a prefix
    ** P, EVERY emitted file-scope symbol is renamed to P_..., so two parsers
    ** compiled with distinct prefixes link into one program with no collision. The
    ** CLI sets it from `c ... --parser[-lib] --name P'. It points at host-owned
    ** storage (argv) that outlives the synchronous emit, so the library copies
    ** nothing. */
    const char                 *emit_parser_prefix;
};

/* ** Set the compiled-parser symbol prefix for the next pocketparse_emit_c
** (see emit_parser_prefix). Private, host-set; NULL restores the default names. */
void pp_engine_set_parser_prefix(pocketparse_engine *engine, const char *prefix);

/* ** Set the rule-memo force override (see vm_rule_memo_force). Private, host-set. */
void pp_engine_force_rule_memo(pocketparse_engine *engine, int mode);

/* ** Test whether a grammar parameter is set (truthy) on the engine. Returns 1
** when a parameter of that name is set, 0 otherwise (unknown or cleared). The
** (when PARAM: X) compiler calls this to decide whether X contributes. */
int pp_engine_param_set(const pocketparse_engine *engine,
                        pocketparse_string name);

/* ** Resolve a class name to its installed CLASS binding, or NULL if no such
** binding exists. Lives in engine.c; the matcher calls it for (class NAME). */
const pp_binding *pp_engine_find_class(const pocketparse_engine *engine,
                                       pocketparse_string name);

/* ** Resolve a registered embedded language by name, or NULL if none is registered
** (see the W17-3 language registry). Lives in engine.c; pp_inject_run calls it to
** decide whether a registered (possibly generated) parser handles the region. */
const pocketparse_language_entry *pp_engine_find_language(
    const pocketparse_engine *engine, pocketparse_string name);

/* ** Look up an installed plug by name, or NULL if none is installed under that
** name. Lives in engine.c; pp_trace uses it to find the "trace" sink. */
const pp_plug *pp_engine_find_plug(const pocketparse_engine *engine,
                                   pocketparse_string name);

/* ** Emit one engine trace event through the installed "trace" plug, if any. The
** event name and the optional data slice become a pocketparse_message {name=event,
** data=data}; the plug's emit is then called. With no trace plug installed this is
** a cheap no-op: it formats and allocates nothing. Freestanding -- it only calls
** the host's emit callback. Trace points live on cold/structural transitions
** (token consumed, rule enter/exit), never the inner scanner loop. */
void pp_trace(pocketparse_engine *engine, const char *event,
              pocketparse_string data);

/* ** Reader form tags. The .scm reader produces a tree of these forms; the
** grammar compiler walks them. Atoms carry a byte slice into the grammar
** source; lists carry a child vector. */
typedef enum pp_form_kind {
    PP_FORM_LIST = 0,
    PP_FORM_SYMBOL,
    PP_FORM_KEYWORD,   /* trailing-colon symbol, colon stripped from text */
    PP_FORM_STRING,    /* decoded string bytes (escapes processed)        */
    PP_FORM_INTEGER,
    PP_FORM_BYTES      /* byte/hex literal, decoded into bytes            */
} pp_form_kind;

/* ** One reader form. For lists, items/count name the children. For leaves,
** text is the decoded payload (symbol text, string bytes, ...). integer holds
** the decoded value for PP_FORM_INTEGER. start_byte is the source offset, kept
** for diagnostics. */
typedef struct pp_form {
    pp_form_kind     kind;
    pocketparse_string text;
    int64_t          integer;
    struct pp_form **items;
    uint32_t         count;
    uint32_t         start_byte;
} pp_form;

/* ** The result of reading a whole .scm source: the top-level forms, plus a
** success flag and an error message + offset when the bytes are malformed
** (unterminated list/string, stray delimiter). */
typedef struct pp_read_result {
    pp_form  **forms;
    uint32_t   count;
    int        ok;
    const char *error;
    uint32_t   error_byte;
} pp_read_result;

/* ** Read .scm source bytes into a form tree, allocating from the arena. */
pp_read_result pp_read(pp_arena *arena, pocketparse_string source);

/* ** Token/rule IR opcodes. A single node type expresses both token patterns
** (literals, classes, repeats) and rule bodies (seq/choice/repeat/field), so
** the executor and the lexer share one tree shape. */
typedef enum pp_op {
    PP_OP_LITERAL = 0, /* match exact bytes (text)                         */
    PP_OP_CLASS,       /* match one byte in/out of a class set            */
    PP_OP_CLASS_NAME,  /* match one byte a host CLASS binding (text) accepts*/
    PP_OP_ANY,         /* match any one byte                               */
    PP_OP_SEQ,         /* all children in order                           */
    PP_OP_CHOICE,      /* first matching child                            */
    PP_OP_REPEAT,      /* child repeated min..max (max==0 means unbounded) */
    PP_OP_OPTIONAL,    /* child zero or one time                          */
    PP_OP_NOT,         /* negative lookahead child[0], then match child[1] */
    PP_OP_FIELD,       /* child[0] tagged with a field name (symbol)      */
    PP_OP_ALIAS,       /* child[0] reported under an alias kind           */
    PP_OP_REF,         /* reference to a named rule or token (symbol)      */
    PP_OP_EOF,         /* end of input                                    */
    PP_OP_RECOVER,     /* child[0] is the body; recovery fires on EOF      */
    PP_OP_GOAL,        /* match child[0] with the active lexical goal=symbol*/
    PP_OP_NOT_KEYWORD, /* match child[0], reject if the slice is a reserved */
                       /* word -- child[1..] are the reserved-word literals */
    PP_OP_NO_EXTRAS,   /* match child[0] with extras consumption disabled   */
    PP_OP_INJECT,      /* match child[0] (the region), then re-parse the     */
                       /* matched span with embedded_grammar and splice the  */
                       /* result tagged with the language symbol (W62)       */
    PP_OP_DEFERRED_SLOT, /* declare a slot; zero-width. operand: symbol =     */
                       /* slot_name (lex-goal-implementation)                */
    PP_OP_DEFER,       /* match child[0], enqueue. operand: symbol =          */
                       /* slot_name; children[0] = sub-pattern               */
    PP_OP_PARSE_DEFERRED, /* drain slot. operands: symbol = slot_name; min =  */
                       /* rule_index; max = goal_entry_index (compact param  */
                       /* index). no children                                */
    PP_OP_PARAM_REF    /* leaf, replaced by PP_OP_LITERAL at instantiation.   */
                       /* operand: min = param_index (always 0);             */
                       /* symbol/max unused/zero                             */
} pp_op;

/* ** A 256-bit byte class with a negation flag, for regex character classes
** like [a-z] or [^()]. */
typedef struct pp_class {
    uint8_t  bits[32];
    int      negated;
} pp_class;

/* ** One IR node. text holds literal bytes; cls a byte class; symbol a rule/
** token/field/alias id; min/max bound a repeat. children name the subnodes.
** The recover_* fields carry the (recover BODY (on eof ...)) DSL payload and are
** meaningful only for PP_OP_RECOVER: recover_message is the diagnostic message,
** recover_help its help text, recover_label its label message, and recover_insert
** the literal bytes synthesized as a zero-width missing node when the body hits
** end-of-input where a terminal was expected. They round-trip through pack.c. */
typedef struct pp_ir {
    pp_op            op;
    pocketparse_string text;
    pp_class         cls;
    uint32_t         symbol;
    uint32_t         min;
    uint32_t         max;
    struct pp_ir   **children;
    uint32_t         child_count;
    pocketparse_string recover_message;
    pocketparse_string recover_help;
    pocketparse_string recover_label;
    pocketparse_string recover_insert;
    /* First-byte dispatch table (Phase 10 token-match speedup). Computed lazily
    ** on the first match attempt and cached here -- the node is arena-owned and
    ** mutable. first_byte_state is 0 until computed, then 1; first_byte_any is 1
    ** when the pattern is nullable or its lead byte cannot be pinned down (a host
    ** (class NAME), `any`, or a `not` body), in which case dispatch must not
    ** prune; otherwise first_byte_set holds the 256-bit set of bytes the pattern
    ** can begin with. Zeroed by pp_alloc on both the compile and .ppg-load paths,
    ** so the lazy guard is sound without explicit initialisation. */
    uint8_t          first_byte_state;
    uint8_t          first_byte_any;
    uint8_t          first_byte_set[32];
    /* Resolving FIRST set for ordered-choice prediction (Phase 10 W46). Unlike
    ** first_byte_* above -- which is a local, token-pattern lead-byte set that
    ** treats every rule/token reference as "open" -- this set is computed by
    ** following references through the grammar, so a choice alternative that
    ** begins with (ref if_statement) carries `i' (from the `if' keyword) here. It
    ** is computed lazily per node and consulted only to skip a choice alternative
    ** whose lead byte cannot match the lookahead. SOUNDNESS: choice_first_open is
    ** 1 (pruning disabled for this node) whenever the alternative is nullable, can
    ** consume extras before its first significant byte, or reaches anything whose
    ** lead set cannot be pinned down (host class, any, not, an unresolved or
    ** recursive reference). choice_first_state is 0 until computed, then 1. */
    uint8_t          choice_first_state;
    uint8_t          choice_first_open;
    uint8_t          choice_first_set[32];
    /* Language injection (W62), meaningful only for PP_OP_INJECT. child[0] is the
    ** REGION sub-pattern that delimits the embedded span; embedded_grammar is the
    ** grammar named by `grammar:`, resolved and compiled once at compile time
    ** through the engine's module resolver, so the parse path never reads the
    ** filesystem; language_symbol is that grammar name interned into the host
    ** grammar's interner, used as the per-node language tag the splice stamps and
    ** --dump-tree surfaces as a `[lang]` annotation. Both are zero/NULL for every
    ** other op. */
    pocketparse_grammar *embedded_grammar;
    uint32_t             language_symbol;
    /* W15-2 compiled token program. A token pattern is matched on the hot path by
    ** an ITERATIVE bytecode loop (pp_token_run), not the recursive tree-walk
    ** pp_match_ir. The flat program this IR lowers to is built lazily on the first
    ** match attempt and cached here -- the node is arena-owned and mutable, like the
    ** first-byte cache above. token_program is NULL until lowered; token_program_tried
    ** records that lowering was attempted (so a pattern that cannot lower -- a depth
    ** the lowerer rejects, or an allocation failure -- is not retried every match and
    ** simply falls back to pp_match_ir). See pp_token_program in token.c. */
    struct pp_token_program *token_program;
    uint8_t                  token_program_tried;
} pp_ir;

/* ** W15-2 token bytecode opcode -- a JUMP-THREADED register ISA, the form a
** hand-written lexer takes. A token pattern's restricted IR (literal, class, any,
** named class, seq, choice, repeat, optional, not, not-keyword) lowers to a flat,
** LINEAR instruction stream with explicit jumps; pp_token_run walks it with one
** program counter `pc' and one cursor `pos', plus a tiny fixed save-stack only for
** the backtrack points an ordered choice / optional / repeat-exit needs. There is
** no C recursion (a deep pattern overflows nothing) and -- crucially -- no per-node
** frame state machine: the linear spine (the common keyword / identifier / number)
** runs as straight-line code, byte-for-byte the moves pp_match_ir makes, but
** without the recursive call and resume overhead that made a frame VM slower.
**
** The matcher is PURE (input -> end offset; no tree build, extras or goal), and
** token matching is POSSESSIVE PEG (greedy, no cross-element backtracking: a
** committed repeat/choice is never undone), so the save-stack only ever holds the
** "where to resume if THIS alternative fails" points, never a full continuation.
** SUCCESS removes the alternative's save point (T_COMMIT); a leaf FAILURE pops the
** nearest save point, restores its cursor and jumps to its handler (T_FAIL implied
** by every match op). The lowering mirrors pp_match_ir's control flow exactly, so
** the end offset is byte-identical -- the differential harness verifies it. */
typedef enum pp_token_op {
    PP_TOK_LITERAL = 0, /* match exact bytes (source->text); fail -> backtrack  */
    PP_TOK_ANY,         /* match any one byte; fail -> backtrack                */
    PP_TOK_CLASS,       /* match one byte in source->cls; fail -> backtrack     */
    PP_TOK_CLASS_NAME,  /* match one byte a host CLASS binding accepts; fail bt */
    PP_TOK_STAR,        /* greedy possessive repeat of a single LEAF (the fused */
                        /* identifier/number/whitespace loop): consume the leaf */
                        /* (op2/source) while it matches, up to max; fail only  */
                        /* if fewer than min matched. The single hottest loop.  */
    PP_TOK_SAVE,        /* push (target, pos): a backtrack point for a choice    */
                        /* alternative / optional body that may fail            */
    PP_TOK_COMMIT,      /* pop the top save point (the alternative matched),     */
                        /* then jump to target                                  */
    PP_TOK_JMP,         /* pc = target                                          */
    PP_TOK_MARK,        /* record the cursor into the mark register (a not-      */
                        /* keyword span start); never a backtrack point         */
    PP_TOK_NOT_KEYWORD, /* the matched span [mark,pos) must not equal a reserved */
                        /* word (the literals ride on source->children[1..])    */
    PP_TOK_NOT_BEGIN,   /* open a negative-lookahead guard: push a negative save  */
                        /* whose target is the continuation reached when the      */
                        /* guard FAILS (so the (not GUARD ...) succeeds). The      */
                        /* guard sub-program follows; a leaf failure inside it     */
                        /* pops this save, restores the cursor, and resumes at the */
                        /* target -- the guard did not match here.                 */
    PP_TOK_NOT_END,     /* close a negative-lookahead guard: reached only when the */
                        /* guard MATCHED, so the (not GUARD ...) must FAIL. Drop    */
                        /* the negative save, restore the cursor to the lookahead   */
                        /* start, and backtrack to the enclosing save (or fail).    */
    PP_TOK_MATCH        /* accept: the whole pattern matched, end = pos          */
} pp_token_op;

/* ** One token-bytecode instruction in the threaded stream. `source' points back at
** the IR node a leaf lowered from (its literal bytes / class / named class / the
** not-keyword reserved-word list), so the operands are read in place, never copied.
** `target' is a jump/branch destination (a pc); `op2' names the leaf op a
** PP_TOK_STAR repeats and `source' carries its bytes/class; min/max bound the star. */
typedef struct pp_token_instruction {
    pp_token_op  op;
    const pp_ir *source;   /* leaf operands: literal text, class, named class   */
    uint32_t     target;   /* SAVE/COMMIT/JMP: a destination pc                 */
    pp_token_op  op2;      /* PP_TOK_STAR: the leaf op being repeated           */
    uint32_t     min;      /* PP_TOK_STAR lower bound                           */
    uint32_t     max;      /* PP_TOK_STAR upper bound (0 = unbounded)           */
} pp_token_instruction;

/* ** A lowered token pattern: the flat, jump-threaded instruction array, terminated
** by a PP_TOK_MATCH. Built once per token pattern (pp_token_program_get) and
** read-only during matching. start_program is always 0 (the stream is linear). */
typedef struct pp_token_program {
    pp_token_instruction *instructions;
    uint32_t              instruction_count;
    uint32_t              instruction_capacity;
    uint32_t              start_program;
    int                   ok;          /* 0 if lowering failed (use pp_match_ir) */
} pp_token_program;

/* ** W59 bytecode VM instruction set.
**
** The grammar's rule/token IR is lowered to a flat instruction array that a
** single dispatch loop (pp_execute_vm in vm.c) walks with explicit arena-grown
** backtrack and return stacks instead of the C call stack. The ISA is structural:
** each IR combinator lowers to a short, fixed instruction sequence with a
** well-defined save/restore order, so the concrete syntax tree is byte-identical
** between the VM and the C-compiled
** parser (cc-diff), all anchored by the conformance suite. Terminals call the
** SAME token matcher (pp_token_match_memo) and the SAME tree builder
** (pp_tree_builder_*); only dispatch differs between the lowered forms.
**
** Operand conventions per opcode are documented at each lowering site in
** vmcompile.c. `target' is a program index (resolved at lower time); `symbol' is
** an interned id (kind/field/alias/goal); `pattern' points at the IR node a
** terminal matches (so pp_token_match_memo and first-byte dispatch run verbatim);
** `aux' carries a small immediate (repeat bounds, no-extras mode). */
typedef enum pp_vm_op {
    PP_VM_MATCH = 0,   /* match pattern (token/literal/byte op) at cursor, emit leaf */
    PP_VM_EOF,         /* consume extras, then succeed iff cursor is at end of input */
    PP_VM_CALL,        /* enter rule at target: open a node, parse body, link, close  */
    PP_VM_RETURN,      /* close the current rule node and resume the caller           */
    PP_VM_CHOICE,      /* begin an ordered choice over a contiguous alternative list  */
    PP_VM_REPEAT,      /* run a repeat (min/max in aux/symbol) over a sub-program     */
    PP_VM_OPTIONAL,    /* run child sub-program zero or one time, always succeed      */
    PP_VM_SEQ,         /* run a contiguous list of child sub-programs in order        */
    PP_VM_FIELD,       /* decorate the child sub-program's node with a field symbol   */
    PP_VM_ALIAS,       /* report the child sub-program's node under an alias kind     */
    PP_VM_GOAL,        /* set the active lexical goal around the child sub-program    */
    PP_VM_NO_EXTRAS,   /* toggle extras consumption around the child sub-program      */
    PP_VM_RECOVER,     /* run the child sub-program with end-of-input recovery armed  */
    PP_VM_BYTE,        /* anonymous one-byte op (any/class/class-name/not) in a rule  */
    PP_VM_INJECT,      /* match the region sub-program, then splice an embedded parse */
    PP_VM_HALT,        /* end of the start program: stop the dispatch loop            */
    PP_VM_NOT_RULE,    /* rule-level negative-lookahead: child[0] is a token          */
                       /* lookahead, child[1] a rule call. Lowered/dispatched in a    */
                       /* later phase (lex-goal-implementation)                       */
    PP_VM_DEFERRED_SLOT, /* declare a per-invocation slot in the enclosing rule frame */
                       /* (symbol = slot_name); zero-width success, emits no node     */
                       /* (lex-goal-implementation)                                   */
    PP_VM_DEFER,       /* match the child sub-program (target), copy its matched byte  */
                       /* span into the parse arena and enqueue it onto the named slot */
                       /* (symbol = slot_name); the CST is identical to the unwrapped  */
                       /* match (lex-goal-implementation)                             */
    PP_VM_PARSE_DEFERRED /* drain a slot, invoking a rule per item. A NO-OP placeholder */
                       /* this phase; Phase 4 implements the drain. Carries           */
                       /* symbol = slot_name, min = rule_index, max = goal_entry_index */
                       /* (lex-goal-implementation)                                   */
} pp_vm_op;

/* ** One VM instruction. A combinator's children are not inlined; instead the
** instruction names a sub-program by index range (start/count) or a single child
** program (target), which the dispatch loop runs through the return stack. This
** keeps each instruction fixed-width and the lowering a faithful, auditable image
** of the IR tree -- the byte-identical contract is verified by the differential
** harness, not assumed. */
typedef struct pp_instruction {
    pp_vm_op  op;
    pp_ir    *pattern;   /* PP_VM_MATCH / PP_VM_BYTE: the IR node to match        */
    pp_ir    *source;    /* the IR node this instruction lowered from (for W46    */
                         /* choice prediction, which resolves through the node)   */
    uint32_t  symbol;    /* interned kind/field/alias/goal id, or PP_NO_SYMBOL    */
    uint32_t  target;    /* PP_VM_CALL/CHOICE/REPEAT/OPTIONAL/...: a child program*/
    uint32_t  start;     /* PP_VM_SEQ/CHOICE: first child program in the table    */
    uint32_t  count;     /* PP_VM_SEQ/CHOICE: number of child programs            */
    uint32_t  min;       /* PP_VM_REPEAT lower bound                              */
    uint32_t  max;       /* PP_VM_REPEAT upper bound (0 = unbounded)              */
    uint32_t  aux;       /* small immediate (PP_VM_NO_EXTRAS mode, etc.)          */
    uint8_t   tail;      /* PP_VM_CALL in tail position (W14-6): the iterative    */
                         /* driver reuses the current frame instead of pushing   */
    uint32_t  slot_count;/* PP_VM_CALL: target rule's deferred-slot count         */
                         /* (runtime only, not serialized)                       */
} pp_instruction;

/* ** A lowered grammar: the flat instruction array plus a side table of
** sub-program entry points the combinators reference, all arena-allocated. A
** "program" is an index into instructions where execution of one IR node begins;
** combinators reference their children by program index (children[]). The whole
** structure is built once per parse by pp_compile_program and read-only during
** dispatch. */
typedef struct pp_program {
    pp_arena       *arena;
    pp_instruction *instructions;
    uint32_t        instruction_count;
    uint32_t        instruction_capacity;
    uint32_t       *children;          /* flat child-program index table        */
    uint32_t        child_count;
    uint32_t        child_capacity;
    uint32_t        start_program;     /* entry point for the grammar start rule */
    const pocketparse_grammar *grammar;
} pp_program;

/* ** Lower a compiled grammar's start rule (and everything it reaches) to a flat
** bytecode program in `arena', ready for pp_execute_vm. Returns NULL on
** allocation failure. The lowering reuses the W44 compile-time symbol ids cached
** in the IR and the W46 first-sets (consulted from the IR nodes at dispatch
** time), so prediction and the no-intern guarantee carry over unchanged. */
pp_program *pp_compile_program(pp_arena *arena,
                               const pocketparse_grammar *grammar,
                               uint32_t start_symbol);

/* ** Count a lowered program's rule CALLs and how many sit in TAIL position (the
** W14-6 tail-call analysis): a CALL whose signal becomes the enclosing rule's
** signal with no epilogue, set during lowering by pp_mark_tail_calls. Writes the
** totals through the out-params (either may be NULL). Reproducible from the CLI
** (`analyze-tailcalls`) so the design evidence can be regenerated. Lives in
** vmcompile.c. */
void pp_count_tail_calls(const pp_program *program,
                         uint32_t *out_total, uint32_t *out_tail);

/* ** Run the compiled grammar over a parser's buffered input via the in-process
** bytecode VM, building the concrete syntax tree. This is the engine (W14-5
** retired the recursive IR interpreter); the tree is byte-identical between
** the VM and the C-compiled parser (ci/pocketparse-cc-diff.sh), with the conformance suite as
** the standing correctness anchor. Returns POCKETPARSE_OK on a full parse or
** POCKETPARSE_SYNTAX_ERROR (with a diagnostic) on failure. */
pocketparse_result pp_execute_vm(pocketparse_parser *parser);

/* ** One slot of the per-position token-match memo (Phase 10). Whether the slot
** is occupied is held OUTSIDE the slot, in the memo's parallel `live' array
** (see pp_match_memo) -- the slot's own fields are never read until `live' says
** it is occupied, so the slot data needs no clearing. The key is the triple
** (pattern, offset, goal): the IR pattern pointer being matched, the input
** offset, and the active lexical goal at that offset -- the goal MUST be part of
** the key because the W27 lex-goal feature can redirect a trigger to a different
** token at the same offset, so a result cached under one goal is not valid under
** another. The value is the match outcome: `matched' is 1 on success with `end'
** the end offset, 0 on a recorded failure. */
typedef struct pp_match_slot {
    const pp_ir *pattern;
    uint32_t     offset;
    uint32_t     goal;
    uint32_t     end;
    uint8_t      matched;
} pp_match_slot;

/* ** The per-parse token-match memo: an open-addressing hash table over
** (pattern, offset, goal) -> outcome, so a token interpreted (or refuted) at an
** offset is not re-run when an ordered choice retries the same offset. It is
** scoped to a single parse and sized to the input length, keeping memory
** O(input).
**
** ZEROING (W15-1). The slot table dominated the parse: pp_alloc clears every
** block, and the slots array is ~24 bytes per slot sized to input*4, so a 6 KB
** parse memset ~768 KB it then overwrites -- the single largest cost in the
** profile. The fix splits occupancy from data. `slots' is allocated RAW (its
** bytes are read only after the table reports the slot occupied), and a parallel
** one-byte-per-slot `live' array -- a 24th of the slot bytes -- is the only thing
** zeroed: live[i] == 0 means slot i is empty, != 0 means occupied. So the probe
** consults `live', never the uninitialised slot, and the per-parse zeroing drops
** from the whole table to the occupancy column. slots/live/mask name the table
** (capacity is a power of two); arena is where it grows. A NULL table
** (capacity 0) disables memoisation -- the matcher then behaves exactly as
** before. */
typedef struct pp_match_memo {
    pp_arena      *arena;
    pp_match_slot *slots;     /* raw (not zeroed): read only when live[i] != 0 */
    uint8_t       *live;      /* zeroed occupancy column, one byte per slot     */
    uint32_t       mask;      /* capacity - 1 (capacity is a power of two) */
    uint32_t       count;
    uint32_t       capacity;
} pp_match_memo;

/* ** Initialise an empty memo bound to `arena' and sized for an input of
** `input_length' bytes. Allocates the slot table; on allocation failure the memo
** is left with capacity 0, which simply disables memoisation. */
void pp_match_memo_init(pp_match_memo *memo, pp_arena *arena,
                        uint32_t input_length);

/* ** Per-position "no extras here" memo (Phase 10). Backtracking revisits the
** same input offset many times, and at each visit pp_consume_extras re-scans the
** trivia tokens (whitespace, comments) only to find -- at most offsets -- that
** none begin here, leaving the position untouched and emitting nothing. That
** "nothing to consume" verdict depends only on (offset, active goal, no_extras
** scope) and is recomputed every time. This memo caches it: one byte per input
** offset records whether the offset has already been proven extras-free under a
** given goal, so a revisit answers O(1) instead of rescanning.
**
** SOUNDNESS. The memo records ONLY the empty verdict (no extras consumed, no
** node emitted). When extras ARE present pp_consume_extras must still run, since
** it appends trivia/error leaf nodes into the sink and the memo cannot replay
** that emission; those offsets are simply left uncached. A cache hit therefore
** means "skip a scan that would have done nothing", which is behaviour-identical
** to running it. A cache miss recomputes, so correctness never depends on the
** cache. The goal is part of the key because a lex-goal can change which tokens
** count as extras at the same offset.
**
** marks[offset] is 0 for "unknown" or 1 for "proven empty". The memo is consulted
** ONLY when the active goal is PP_NO_SYMBOL -- the common no-lex-goal case, which
** is where the backtracking re-scans concentrate. Under any other goal
** pp_consume_extras bypasses the memo and recomputes, so a goal-dependent verdict
** is never cached under the wrong key. The no_extras scope is handled earlier
** still: pp_consume_extras returns before it consults the memo at all. marks/length
** name the array; a NULL array disables the memo. */
typedef struct pp_extras_memo {
    uint8_t  *marks;          /* one byte per input offset (length+1 entries) */
    uint32_t  length;         /* number of entries (input_length + 1)         */
    pp_arena *arena;          /* arena to allocate marks from (lazily)        */
    uint32_t  pending_length; /* input length the marks size to when ensured  */
} pp_extras_memo;

/* ** Prepare an extras memo for `input_length' bytes WITHOUT allocating the marks:
** the marks array (input_length+1 bytes, zeroed) only helps once a parse backtracks,
** which a clean predictive parse never does, so allocating and zeroing it up front
** was wasted work on the common path (W17-GOAL). pp_extras_memo_ensure allocates it
** lazily when the parse goes super-linear (the same wake as the rule memo); until
** then marks is NULL, which disables the memo and pp_consume_extras behaves exactly
** as before -- the memo is a pure cache, so the verdict is byte-identical. */
void pp_extras_memo_init(pp_extras_memo *memo, pp_arena *arena,
                         uint32_t input_length);

/* ** Allocate the extras memo's zeroed marks array (lazy; see pp_extras_memo_init).
** Idempotent: a no-op once marks is allocated. On allocation failure marks stays
** NULL, which simply leaves the memo disabled. */
void pp_extras_memo_ensure(pp_extras_memo *memo);

/* ** A compiled token: its kind symbol and the IR pattern that recognises it. */
typedef struct pp_token_def {
    uint32_t symbol;
    pp_ir   *pattern;
} pp_token_def;

/* ** Per-rule flags carried on pp_rule_def. */
enum {
    /* A HIDDEN rule (the .scm leading-underscore marker, tree-sitter's
    ** convention) is omitted from the CST when it is a pure pass-through -- it
    ** contributed exactly one child, no operator/extra structure, and the call
    ** carried no alias. In that case its single child is promoted to the parent
    ** (re-stamped with the call's field), so the precedence-ladder and dispatch
    ** wrappers (`expression`, `additive_expression`, ...) collapse out, matching
    ** tree-sitter's tree. A hidden rule that DID add structure (an operator
    ** matched, two-plus children) still emits its node. See pp_vm_exit /
    ** pp_vm_call_exit, where the collapse lives in the shared close path so the
    ** VM and the generated parser flatten identically. */
    PP_RULE_HIDDEN = 1u << 0,
    /* A TRANSPARENT_ALWAYS rule (W17-RECLAIM) is a HIDDEN rule the grammar
    ** compiler proved can ONLY ever contribute exactly one child and never a
    ** field/operator/second child of its own -- a bare dispatch wrapper, a
    ** `(choice ref ref ...)' of plain references, a single `(field f: ref)' or a
    ** lone `(ref ...)'. Such a rule ALWAYS collapses, so the W17-FLAT exit-time
    ** collapse (which still allocated and wrote the wrapper node at entry, only to
    ** orphan it) is pure waste for it. The entry path skips the node entirely:
    ** it runs the body straight into the CALLER's child sink with the call's
    ** field/alias forwarded, so the single child lands in the wrapper's place with
    ** no node ever created -- byte-identical to the collapse, with the allocation
    ** reclaimed. Computed once in pocketparse_compile_scheme after refs resolve
    ** (so it can follow a `(choice ref)' through the rule table), it implies
    ** PP_RULE_HIDDEN and rides the same packed flags field. The operator-ladder
    ** rules (additive_expression: they earn a node when an operator matches) are
    ** NOT transparent-always and keep the exit-time collapse. */
    PP_RULE_TRANSPARENT_ALWAYS = 1u << 1
};

/* ** A compiled rule: its kind symbol, per-rule flags, and the IR body. */
typedef struct pp_rule_def {
    uint32_t symbol;
    uint32_t flags;
    uint32_t slot_count; /* distinct (deferred-slot) declarations in this rule's */
                         /* own body (lex-goal-implementation)                   */
    pp_ir   *body;
} pp_rule_def;

/* ** Extras policy kinds, mirroring the design's trivia/skip/error. */
typedef enum pp_extra_policy {
    PP_EXTRA_TRIVIA = 0,
    PP_EXTRA_SKIP,
    PP_EXTRA_ERROR
} pp_extra_policy;

/* ** How a trivia kind attaches to the tree (the (extras ...) `attach:' modifier).
** TRAILING is the historical behaviour: the trivia leaf links into the sink being
** filled the instant it is scanned, so it lands inside whatever node is open. LEADING
** holds the trivia and re-emits it at the next sibling boundary, ahead of the next
** real token rather than inside a node that is closing -- the placement a JSDoc or
** statement-boundary comment wants. */
typedef enum pp_extra_attach {
    PP_EXTRA_ATTACH_TRAILING = 0,
    PP_EXTRA_ATTACH_LEADING
} pp_extra_attach;

/* ** One extras entry: a token symbol, how to treat it between elements (policy),
** and which direction it attaches (attach). `attach' is consulted only for a
** PP_EXTRA_TRIVIA entry; skip/error keep the historical attach-on-sight path. */
typedef struct pp_extra {
    uint32_t        token_symbol;
    pp_extra_policy policy;
    pp_extra_attach attach;
} pp_extra;

/* ** One lexical-goal redirect: while the named goal is the parser's active
** lexical goal, a terminal reference to trigger_symbol is scanned not as itself
** but as target_symbol's token pattern, and the leaf is emitted under
** target_symbol's kind. This is parser-directed lexing -- the same source byte
** tokenizes differently depending on which goal a rule selected with (goal NAME
** X). A grammar that names no (lex-goal ...) form has an empty table and behaves
** exactly as before. The (goal ...) combinator selects goal_symbol as active;
** the scanner consults this table to resolve the ambiguous lexeme. */
typedef struct pp_lex_goal {
    uint32_t goal_symbol;
    uint32_t trigger_symbol;
    union {
        uint32_t target_symbol;       /* static: redirect target goal */
        uint32_t template_rule_index; /* parametrized: internal rule index */
    };
    uint32_t is_parametrized;         /* 0 = static redirect, 1 = parametrized */
} pp_lex_goal;

/* ** One deferred (slot) item: a captured source slice plus the placeholder node
** it was enqueued under, drained later by PP_OP_PARSE_DEFERRED. `text`/`text_len`
** name the matched bytes; `node_index` the CST node to fill in once re-parsed.
** Part of the deferred-slot runtime state (lex-goal-implementation, "VM state"). */
typedef struct pp_deferred_item {
    uint8_t  *text;
    uint32_t  text_len;
    uint32_t  node_index;
    uint32_t  _pad;
} pp_deferred_item;

/* ** One slot frame: the per-invocation queue a (deferred-slot NAME) declaration
** opens. `slot_name` is the interned slot id; `items` is the grown-by-doubling
** queue PP_OP_DEFER appends to and PP_OP_PARSE_DEFERRED drains. Stored on the
** owning pp_vm_frame, NULL when the rule declares no slots
** (lex-goal-implementation, "VM state"). */
typedef struct pp_slot_frame {
    uint32_t          slot_name;
    pp_deferred_item *items;
    uint32_t          count;
    uint32_t          capacity;
} pp_slot_frame;

/* ** One active slot scope: the slots a single rule invocation declared, pushed on
** rule entry and popped on rule exit. This is the UNIFIED slot-resolution stack
** (lex-goal-implementation, Option A): both the bytecode VM and the generated C
** parser scan parser->slot_scopes to resolve (defer …)/(parse-deferred …) names
** and to snapshot slot counts for backtrack rollback. The VM cannot share its
** explicit pp_vm_stack with the generated parser (whose rule invocations are TRUE
** C recursion -- each rule's pp_vm_frame is a C-stack local, not in a scannable
** array), so the slot scope lives on the parser where both executors reach it. */
typedef struct pp_slot_scope {
    pp_slot_frame *slots;       /* this invocation's slot array (arena-backed) */
    uint32_t       slot_count;  /* number of slots in the array                */
} pp_slot_scope;

/* ** One parametrized-goal stack: the pattern arguments a parametrized lex-goal
** entry is instantiated with, one stack per parametrized goal entry. PP_OP_GOAL
** pushes the active pattern; PP_OP_PARAM_REF reads the top; the entry's compact
** index (goal_entry_index) selects which stack. Grown by doubling
** (lex-goal-implementation, "Goal-table stack"). */
typedef struct pp_param_stack {
    const pp_ir **patterns;
    uint32_t      depth;
    uint32_t      capacity;
} pp_param_stack;

/* ** Append one captured item onto a slot's FIFO, growing the arena-backed item
** array by doubling (arenas never realloc, so growth allocates a fresh, larger
** block and copies; the old block is abandoned). The first push allocates an
** initial block of 4 items. Returns 0 only on allocation failure
** (lex-goal-implementation, "Slot frame"). */
int pp_slot_push(pp_arena *arena, pp_slot_frame *slot, pp_deferred_item item);

/* ** Push an instantiated pattern onto a parametrized goal entry's stack. The
** patterns array is arena-allocated and grows by doubling (arenas never
** realloc: a new block is allocated and the existing pointers copied). Returns 0
** only on allocation failure (lex-goal-implementation, "Goal-table stack"). */
int pp_param_stack_push(pp_arena *arena, pp_param_stack *stack, const pp_ir *pattern);

/* ** Pop the top pattern (decrement depth; the arena block stays). A pop on an
** empty stack is a no-op (defensive). */
void pp_param_stack_pop(pp_param_stack *stack);

/* ** Allocate a parser's per-parametrized-goal stack array from `arena', one zeroed
** pp_param_stack per parametrized goal entry (grammar->param_goal_count). Writes
** the array to *out (0 when the grammar has no parametrized goals). Returns 0 only
** on allocation failure (lex-goal-implementation, "Pattern instantiation"). */
int pp_param_stacks_alloc(pp_arena *arena, const pocketparse_grammar *grammar,
                          pp_param_stack **out);

/* ** Allocate `slot_count' zeroed (slot_name == PP_NO_SYMBOL) pp_slot_frame entries
** from `arena' and push them as a new active slot scope on the parser. Called on
** rule entry by BOTH the VM dispatch loop and the generated rule function when the
** rule declares deferred slots. Returns the allocated slot array (NULL only on OOM,
** which the caller treats as PP_VM_OOM). slot_count must be > 0. The unified scope
** stack is what (defer …)/(parse-deferred …) and the slot-save rollback scan
** (lex-goal-implementation, Option A). */
pp_slot_frame *pp_slot_scope_push(pocketparse_parser *parser, pp_arena *arena,
                                  uint32_t slot_count);

/* ** Pop the innermost active slot scope (the matching pp_slot_scope_push). A pop on
** an empty stack is a defensive no-op. The arena memory stays (append-only). */
void pp_slot_scope_pop(pocketparse_parser *parser);
/* The pp_vm_state-taking slot helpers (pp_slot_claim, pp_slot_resolve, and the
** slot-save trio) are declared after the pp_vm_state typedef below. */

/* ** Build a concrete pattern from a (param NAME BODY) template by copying the
** template rule's IR (grammar->rules[rule_index].body) and replacing every
** PP_OP_PARAM_REF leaf with a PP_OP_LITERAL of the captured bytes. `arena' is the
** parse arena; the result lives for the whole parse and is mutable (token_program
** lowers into it lazily). Returns 0 on a bad index or any allocation failure --
** callers degrade to no-pattern -> no-match (lex-goal-implementation,
** "Pattern instantiation"). Defined in token.c next to pp_compile_pattern. */
const pp_ir *pp_param_instantiate(pp_arena *arena, const pocketparse_grammar *grammar,
                                  uint32_t rule_index, const uint8_t *text,
                                  uint32_t text_len);

/* ** A dense, arena-backed string-interning table. Names map to small ids; the
** name bytes are copied into the arena so they outlive the grammar source. */
typedef struct pp_interner {
    pp_arena          *arena;
    pocketparse_string *names;
    uint32_t           count;
    uint32_t           capacity;
} pp_interner;

/* ** A compiled grammar: the interner (shared with the tree for symbol names),
** the token and rule tables, the extras policy, and the start rule symbol. */
struct pocketparse_grammar {
    pp_arena      *arena;
    pp_interner    interner;
    /* W17-3 grammar name from the source `(grammar NAME ...)' form (e.g. "json",
    ** "javascript"). The emitter derives the DEFAULT symbol prefix from it, so two
    ** parsers compiled from differently named grammars carry disjoint symbols with
    ** no explicit --name. Interned into this grammar's own interner at compile time;
    ** PP_NO_SYMBOL when the grammar was built without a name (e.g. loaded from a
    ** binary .ppg that predates name capture), in which case the emitter falls back
    ** to the historical default symbol names. */
    uint32_t       name_symbol;
    pp_token_def  *tokens;
    uint32_t       token_count;
    pp_rule_def   *rules;
    uint32_t       rule_count;
    pp_extra      *extras;
    uint32_t       extra_count;
    pp_lex_goal   *lex_goals;
    uint32_t       lex_goal_count;
    /* Count of parametrized lex_goal entries (is_parametrized != 0); the compact
    ** param-stack dimension. Recomputed on load (lex-goal-implementation). */
    uint32_t       param_goal_count;
    uint32_t       start_symbol;
    /* W59 bytecode-VM program cache. The IR->bytecode lowering is a pure function
    ** of (grammar, start symbol), so it is done once and memoised here, then reused
    ** across every parse that runs the VM -- the lowering cost is paid at most once
    ** per start symbol, not per parse. NULL until the VM is first run for a given
    ** start symbol; vm_program_start records which start symbol it was lowered for.
    ** The recursive-descent default never touches these fields. */
    pp_program    *vm_program;
    uint32_t       vm_program_start;
    /* W66 token-by-symbol index. pp_find_token was a linear scan over `tokens'
    ** run on every terminal match (and inside the extras loop) to resolve a
    ** symbol to its token def -- O(token_count) per byte. This lazily-built array,
    ** indexed by symbol id, makes that lookup O(1). NULL until first built; like
    ** vm_program it is a runtime cache (never serialized) and a pure function of
    ** the token table, so it is built once and reused across every parse. A symbol
    ** outside [0, token_index_size) is not a token kind, so the lookup returns
    ** NULL just as the scan would. */
    pp_token_def **token_index;
    uint32_t       token_index_size;
    /* W17-FLAT hidden-rule-by-symbol index. A hidden rule (PP_RULE_HIDDEN) is
    ** collapsed in the shared node-close when it is a pure pass-through; the
    ** close path needs an O(1) symbol->hidden lookup on the hot path. This byte
    ** array, indexed by symbol id, holds 1 for a hidden rule's symbol and 0
    ** otherwise. Like token_index it is a pure function of the rule table, built
    ** once (lazily) and cached, never serialized; NULL until first built, when
    ** the close path falls back to treating every rule as visible (so an
    ** allocation failure is sound, merely un-collapsed). A symbol outside
    ** [0, hidden_index_size) is not a hidden rule. */
    uint8_t       *hidden_index;
    uint32_t       hidden_index_size;
    uint8_t        hidden_index_built; /* 0 until pp_vm_build_hidden_index ran */
    /* W16-1 extras lead-byte fast path. pp_vm_consume_extras runs before every
    ** terminal, and at the overwhelming majority of positions the cursor byte
    ** begins no extra at all (it is an identifier, operator or string byte, not
    ** whitespace or a comment opener), yet the scanner still looped over every
    ** extras entry and ran pp_token_match_memo for each. This 32-byte set is the
    ** UNION of each extra token's first-byte set: a cursor byte outside it cannot
    ** begin ANY extra, so the whole loop is skipped with one bitset probe. It is
    ** a pure function of the extras table + token patterns, so it is computed once
    ** (lazily, on first consume) and reused across every parse, like vm_program
    ** and token_index. extras_lead_state is 0 until computed, then 1;
    ** extras_lead_open is 1 when some extra's lead byte cannot be pinned down (a
    ** nullable or lead-unknown pattern), in which case the fast path is disabled
    ** and the scanner falls back to the full loop -- always sound. */
    uint8_t        extras_lead_state;
    uint8_t        extras_lead_open;
    uint8_t        extras_lead[32];
};

#ifdef PP_DEBUG_INTERN_GUARD
/* ** Debug tripwire counting pp_intern calls (see util.c). pp_execute_vm snapshots
** it to assert no interning happens between push and finish. */
extern unsigned long pp_intern_call_count;
#endif

/* ** Intern a name, returning its id (creating it if new). */
uint32_t pp_intern(pp_interner *interner, pocketparse_string name);

/* ** Intern a C literal name. */
uint32_t pp_intern_c(pp_interner *interner, const char *literal);

/* ** Look up a name without creating it; returns 1 and writes *out_id on hit. */
int pp_intern_find(const pp_interner *interner, pocketparse_string name,
                   uint32_t *out_id);

/* ** The printable name for an interned id. */
pocketparse_string pp_intern_name(const pp_interner *interner, uint32_t id);

/* ** The language tag (W62 injection) of the node under a cursor: the interned id
** of the grammar/language that node was parsed by, or PP_NO_SYMBOL for a node from
** the host grammar. INTERNAL ONLY -- it is not in the frozen public header; the
** CLI's --dump-tree calls it to print the `[lang]' annotation. Lives in tree.c. */
uint32_t pp_tree_cursor_language_tag(const pocketparse_tree_cursor *cursor);

/* ** Compare two byte slices for equality. */
int pp_string_equal(pocketparse_string a, pocketparse_string b);

/* ** Compare a slice against a C literal. */
int pp_string_equal_c(pocketparse_string a, const char *literal);

/* ** Build a pocketparse_string from a C literal (length computed by hand). */
pocketparse_string pp_text(const char *literal);

/* ** A node record in the contiguous CST. parent/first_child/child indices and
** the kind/field symbols make traversal and the dump cheap. The records are
** appended in pre-order, so a parent always precedes its children. */
typedef struct pp_node {
    uint32_t kind;        /* interned kind name (rule/token/alias)          */
    uint32_t field;       /* interned field name, or PP_NO_SYMBOL           */
    uint32_t start_byte;
    uint32_t end_byte;
    uint32_t parent;      /* index of parent, or PP_NO_INDEX for the root   */
    uint32_t first_child; /* index of first child, or PP_NO_INDEX           */
    uint32_t child_count;
    uint32_t next_sibling;/* index of next sibling, or PP_NO_INDEX          */
    uint32_t flags;
    /* Language tag (W62 injection): the interned name of the grammar/language a
    ** node was parsed by, or PP_NO_SYMBOL for a node that belongs to the host
    ** grammar (the common case). Set on every node spliced in from an embedded
    ** parse, and on the injection node itself, so a composite multi-language tree
    ** records which language each subtree came from. INTERNAL only -- it never
    ** appears in the frozen public header; it is surfaced solely through
    ** --dump-tree's `[lang]` annotation and the CST metadata. */
    uint32_t language_tag;
} pp_node;

#define PP_NO_INDEX  0xFFFFFFFFu
#define PP_NO_SYMBOL 0xFFFFFFFFu

/* ** Internal node flag, mirrored by the public POCKETPARSE_NODE_MISSING: a
** zero-width node synthesized by recovery for a terminal the input omitted. */
#define PP_NODE_MISSING 0x1u

/* ** The concrete syntax tree: a contiguous node array plus the interner that
** names kinds and fields. root is the index of the Source node. */
struct pocketparse_tree {
    pp_arena         *arena;
    const pp_interner *interner;
    pp_node          *nodes;
    uint32_t          node_count;
    uint32_t          node_capacity;
    uint32_t          root;
};

/* ** A growable node builder used while parsing, then frozen into a tree. */
typedef struct pp_tree_builder {
    pp_arena *arena;
    pp_node  *nodes;
    uint32_t  count;
    uint32_t  capacity;
} pp_tree_builder;

/* ** Append a node record to the builder, returning its index (or PP_NO_INDEX
** on allocation failure). The node starts childless and unlinked. */
uint32_t pp_tree_builder_push(pp_tree_builder *builder, uint32_t kind,
                              uint32_t field, uint32_t start_byte,
                              uint32_t end_byte, uint32_t parent);

/* ** A diagnostic collector. Diagnostics and their labels live in the arena. */
typedef struct pp_diag_list {
    pp_arena               *arena;
    pocketparse_diagnostic *items;
    uint32_t                count;
    uint32_t                capacity;
} pp_diag_list;

/* ** Append an error diagnostic with a single label over [start,end). */
int pp_diag_error(pp_diag_list *list, const char *code, const char *message,
                  uint32_t start_byte, uint32_t end_byte,
                  const char *label_message);

/* ** Append an error-severity recovery diagnostic. Unlike pp_diag_error the
** message/help/label are author-supplied byte slices (from the grammar's recover
** DSL) rather than C literals, and the help text rides along; the single label
** spans [start,end). Returns 1 on success, 0 on allocation failure. Lives in
** diag.c. An empty help slice means "no help line". */
int pp_diag_recover(pp_diag_list *list, pocketparse_string message,
                    pocketparse_string help, pocketparse_string label_message,
                    uint32_t start_byte, uint32_t end_byte);

/* ** A growable list of child node indices, the rule frame's "sink": each
** combinator pushes the nodes it produces here and the enclosing rule links
** them once its body matches. Same growth policy as parse.c's pp_sink. (Defined
** here, ahead of pocketparse_parser, because the buffer-friendly resumption state
** embeds one -- see input_complete / bf_children below.) */
typedef struct pp_vm_sink {
    uint32_t *items;
    uint32_t  count;
    uint32_t  capacity;
    /* W17-CST incremental sibling linking. `parent' is the node index every node
    ** pushed into this sink is a DIRECT child of (the rule frame's node, or the
    ** root). pp_vm_sink_push wires each child's parent/next_sibling AS it is
    ** appended -- while the just-emitted node is still hot in cache -- instead of a
    ** separate pp_vm_link_children second pass that re-walked the child array cold
    ** at rule close. The links derive purely from `items' (the rollback ledger), so
    ** a failed-attempt rollback (items count truncated) re-derives the correct last
    ** sibling on the next push; pp_vm_sink_close then terminates the final sibling
    ** and stamps first_child/child_count/span on the parent in O(1). PP_NO_INDEX
    ** means "parent not yet known" -- a push then only records the index (the legacy
    ** behaviour), and the linking is done by an explicit pp_vm_link_children pass. */
    uint32_t  parent;
} pp_vm_sink;

/* ** The parser lifecycle: a four-state machine that orchestrates one parse
** around the executor (the bytecode VM, or a generated parser's run function).
**
**                  push ok                     push fails
**   (parser_new) ACCEPTING --------> ACCEPTING ----------> DROPPED
**                     |                                      |
**                     | finish / run                         | finish / run
**                     v                                      v
**                 FINISHED                                FAILED
**     (the executor ran over the buffer;       (the executor never ran; finish
**      the tree and diagnostics are built)      returns the push's failure)
**
** ACCEPTING buffers each pushed chunk into the parser's arena. A push that cannot
** buffer its bytes moves the parser to DROPPED and records the result in
** push_failed; later pushes still buffer, but the input is known incomplete, so
** finishing never runs the executor over it. FINISHED and FAILED are final: a
** push there is INVALID, and a second finish reports whether a tree was built.
** The machine is plain C over the parser struct -- the transitions live in
** parse.c (pocketparse_push, pocketparse_finish, pp_parser_run). */
typedef enum pp_parser_state {
    PP_PARSER_ACCEPTING = 0,
    PP_PARSER_DROPPED,
    PP_PARSER_FINISHED,
    PP_PARSER_FAILED
} pp_parser_state;

/* ** An executor: runs a grammar over the parser's buffered input, building the
** tree into the parser. pp_execute_vm is one; a generated parser's execute
** function is another. */
typedef pocketparse_result (*pp_parser_executor)(pocketparse_parser *parser);

/* ** The finishing transition, shared by pocketparse_finish, a generated parser's
** run entry, and an injected sub-parse. From DROPPED or FAILED it moves to (or
** stays in) FAILED and returns push_failed without running anything; otherwise
** it runs `execute' and moves to FINISHED, returning the executor's result. A
** generated run entry called again on a FINISHED parser runs again, as it did
** before the lifecycle existed. Lives in parse.c. */
pocketparse_result pp_parser_run(pocketparse_parser *parser,
                                 pp_parser_executor execute);

/* ** The lowercase name of the parser's lifecycle state ("accepting",
** "finished", ...), for the CLI's POCKETPARSE_TRACE_LIFECYCLE report. Lives in
** parse.c. */
const char *pp_parser_state_name(const pocketparse_parser *parser);

/* ** The streaming parser. Input chunks accumulate into an arena-backed byte
** buffer; the grammar is executed over the buffer at finish time. The CST and
** diagnostics are built into the parser's own arena. */
struct pocketparse_parser {
    pocketparse_engine        *engine;
    pocketparse_grammar       *grammar;
    uint32_t                   start_symbol;
    pp_arena                  *arena;

    uint8_t                   *input;
    uint32_t                   input_length;
    uint32_t                   input_capacity;

    /* W17-4 buffer-friendly streaming of the GENERATED native parser. The
    ** generated parser is true C recursion, so it cannot suspend mid-stack; it can
    ** only suspend at the SHALLOW top-level-item boundary (the start rule's repeat
    ** of top-level items), where the C stack is empty. A host that drives the
    ** generated parser over a GROWING buffer sets input_complete = 0 while more
    ** bytes may still arrive and calls the generated <prefix>_run_chunk: the parser
    ** parses every top-level item that completes strictly within the buffer, then,
    ** if the cursor reaches the buffer end with input still incomplete, rolls back
    ** to the start of the unfinished item and returns POCKETPARSE_MORE. The host
    ** appends more bytes, sets input_complete = 1 on the final chunk, and calls
    ** again; the parse RESUMES at the saved boundary (bf_position) into the SAME
    ** root and child sink, so the finished tree is byte-identical to one whole
    ** parse. input_complete defaults to 1 (whole input known up front -- the
    ** historical pocketparse_finish / <prefix>_run path, unchanged and
    ** byte-identical). The bf_* fields carry the resumption state across calls and
    ** are touched only on the run_chunk path. */
    int                        input_complete;
    int                        bf_active;
    uint32_t                   bf_root;
    uint32_t                   bf_position;
    pp_vm_sink                 bf_children;

    pp_tree_builder            builder;
    pp_diag_list               diagnostics;
    pocketparse_tree          *tree;
    pp_interner                tree_interner; /* copy of grammar names      */

    /* Where this parser is in its lifecycle (see pp_parser_state). */
    pp_parser_state            state;

    /* The FIRST failed push's result. A push that failed must never let finish
    ** report success with a degenerate tree over a truncated buffer: finish (and a
    ** generated parser's run) returns this code instead of parsing. */
    pocketparse_result         push_failed;

    /* Deferred-slot / parametrized-goal runtime state (lex-goal-implementation). */
    struct pp_param_stack *param_stacks;   /* one per parametrized goal entry; arena */
    uint32_t  *slot_save_stack;            /* arena, grows by doubling */
    uint32_t   slot_save_count;
    uint32_t   slot_save_capacity;
    /* The UNIFIED active-slot-scope stack (Option A): one entry per rule invocation
    ** that declared deferred slots, innermost on top. Both the VM and the generated
    ** parser push on rule entry / pop on exit and resolve slot names by scanning it
    ** (lex-goal-implementation, "Slot name resolution"). */
    struct pp_slot_scope *slot_scopes;     /* arena, grows by doubling */
    uint32_t   slot_scope_count;
    uint32_t   slot_scope_capacity;
};

/* ** Compile a pattern form (a literal, a (regex ...), or a combinator like
** seq/choice/repeat/optional/field/alias/not/any) into IR, resolving named
** references through the interner. Shared by token bodies and rule bodies. On
** failure returns NULL and writes a message + offset. */
pp_ir *pp_compile_pattern(const pocketparse_engine *engine, pp_arena *arena,
                          pp_interner *interner, const pp_form *form,
                          const char **error, uint32_t *error_byte);

/* ** Resolve and compile the grammar named by an (inject grammar: NAME ...) form
** into its own grammar handle, through the engine's module resolver (W62). Lives
** in grammar.c; called by the inject combinator in token.c at compile time.
** Returns 1 and writes *out_grammar on success, 0 with *error set on failure. */
int pp_resolve_embedded_grammar(pocketparse_engine *engine,
                                pocketparse_string name,
                                pocketparse_grammar **out_grammar,
                                const char **error);

/* ** Parse a regex-subset pattern string into IR (character classes with ranges
** and negation, `+`/`*` postfix, literal runs, `.` as any). */
pp_ir *pp_compile_regex(pp_arena *arena, pocketparse_string pattern,
                        const char **error);

/* ** Try to match a token's IR pattern against the input starting at `start`.
** Returns 1 and writes the end offset on a match, 0 otherwise. Used by the
** scanner to recognise the next token. The engine carries the host CLASS
** bindings a (class NAME) atom in the pattern resolves against; pass NULL only
** when the pattern is known to contain no named classes. */
int pp_token_match(const pocketparse_engine *engine, const pp_ir *pattern,
                   const uint8_t *input, uint32_t length, uint32_t start,
                   uint32_t *out_end);

/* ** Match a token pattern with the per-position memo and first-byte dispatch
** applied (Phase 10). Identical in result to pp_token_match -- it only does less
** redundant work -- but it consults/updates `memo' keyed by (pattern, start,
** goal) so a retry at the same offset under the same goal is answered from the
** cache, and it skips the interpreter when the lead byte cannot begin a match.
** `memo' may be NULL (no caching) and `goal' is the active lexical goal that
** disambiguates the key. The parse executor calls this; query.c, which has no
** parse position to retry, keeps using pp_token_match. */
int pp_token_match_memo(const pocketparse_engine *engine, pp_ir *pattern,
                        const uint8_t *input, uint32_t length, uint32_t start,
                        uint32_t goal, pp_match_memo *memo, uint32_t *out_end);

/* ** Build the grammar's extras lead-byte fast-path set (W16-1) -- the union of
** every extra token's first-byte lead set -- caching it on the grammar so the
** per-terminal extras scan can reject a "no extra here" cursor byte with one
** bitset probe instead of looping over the extras table. Computed once; sound
** because an extra with an unpinnable lead byte sets extras_lead_open, which
** disables the fast path (the full loop still runs). */
void pp_extras_lead_build(pocketparse_grammar *grammar);

/* ** Decide whether the lookahead byte at `start' (a position past consumed
** extras) could begin a match of ordered-choice alternative `pattern', using a
** resolving FIRST set computed by following references through `grammar' (W46).
** Returns 1 when the alternative might match -- always, when its FIRST set is
** open (nullable or lead-unknown) -- and 0 only when its lead byte provably
** excludes the lookahead. The result is cached on the node, so the grammar walk
** runs at most once per distinct alternative. Used by the choice executor to skip
** alternatives that cannot match; correctness is byte-for-byte identical because
** an open set never prunes. */
int pp_choice_first_admits(const pocketparse_grammar *grammar, pp_ir *pattern,
                           const uint8_t *input, uint32_t length,
                           uint32_t start);

/* ** Hot-path wrapper for pp_choice_first_admits (W16-1). The choice executor calls
** the predictor once per alternative per visit, so on a hot grammar this fires
** millions of times -- yet after the FIRST visit the verdict is a cached bitset
** test, dwarfed by the cross-translation-unit call's own prologue/epilogue (the
** profile put that call overhead above the test it guards). This inline does the
** cached case in place -- the same `open => admit', `past end => reject', else a
** lead-set bit test pp_choice_first_admits performs -- and falls back to the full
** (mutating) function only on the cold first visit, when the lazy FIRST-set walk
** must run. Byte-for-byte identical: same set, same verdict, only the call is
** elided. Inline in the header so BOTH the VM (vm.c) and the generated parser see
** it. */
static inline int pp_choice_first_admits_fast(
    const pocketparse_grammar *grammar, pp_ir *pattern,
    const uint8_t *input, uint32_t length, uint32_t start)
{
    if (pattern->choice_first_state == 0)
        return pp_choice_first_admits(grammar, pattern, input, length, start);
    if (pattern->choice_first_open)
        return 1;
    if (start >= length)
        return 0;
    {
        uint8_t b = input[start];
        return (pattern->choice_first_set[b >> 3] >> (b & 7)) & 1;
    }
}

/* ** Language injection (W62): parse the host-input span [span_start, span_end)
** with the embedded grammar named in `inject' and splice the embedded tree under
** an injection node, offset-mapped to absolute positions, re-interned into the
** host grammar's interner, and language-tagged. The injection node is pushed to
** the caller's child sink through `append', supplied by the caller over its own
** sink type (so inject.c needs no knowledge of the VM's sink). Returns 1 on
** success (injection node emitted), 0 on allocation failure. Lives in inject.c;
** called from the PP_VM_INJECT case of the bytecode VM. A malformed embedded
** region is contained under the injection node (its own ERROR subtree), never a
** host-parse failure. */
int pp_inject_run(pocketparse_parser *host, const pp_ir *inject,
                  uint32_t span_start, uint32_t span_end,
                  int (*append)(void *sink_user, uint32_t node_index),
                  void *sink_user);

/* ** Append input bytes to a parser's buffer (used by pocketparse_push). */
pocketparse_result pp_parser_buffer(pocketparse_parser *parser,
                                    pocketparse_string bytes);

/* ** The three linearized-stream token kinds. A tree is flattened to this
** ENTER/LEAF/EXIT stream and a query is matched by parsing it (see query.c and
** the design's "Queries as parsers over linearized trees"). An internal node
** (one with children) opens with ENTER and closes with EXIT; a leaf node (a
** token, no children) is a single LEAF. The stream is ENTER/EXIT balanced, which
** is what the descendant "skip one balanced subtree" rule rides on. */
typedef enum pp_lin_kind {
    PP_LIN_ENTER = 0,
    PP_LIN_LEAF,
    PP_LIN_EXIT
} pp_lin_kind;

/* ** One token of the linearized stream. kind tags ENTER/LEAF/EXIT; node_kind is
** the interned kind name; field is the interned field name on an ENTER/LEAF, or
** PP_NO_SYMBOL; node_index is the source CST node (for span/text); start_byte and
** end_byte are that node's byte span, carried so a capture binds without a second
** tree walk. */
typedef struct pp_lin_token {
    pp_lin_kind kind;
    uint32_t    node_kind;
    uint32_t    field;
    uint32_t    node_index;
    uint32_t    start_byte;
    uint32_t    end_byte;
} pp_lin_token;

/* ** A linearized tree: the contiguous ENTER/LEAF/EXIT token array a query
** parses, plus the interner that names the kinds and fields (borrowed from the
** tree). The tokens live in the arena passed to pp_linearize. */
typedef struct pp_linearization {
    pp_lin_token      *tokens;
    uint32_t           count;
    const pp_interner *interner;
} pp_linearization;

/* ** Flatten a tree to its ENTER/LEAF/EXIT stream by a pre-order walk, allocating
** the token array from the engine's arena. Returns 1 on success, 0 on allocation
** failure (the output is left empty). The walk visits every node, in source
** order. The engine is the source of arena memory; it is not otherwise read. */
int pp_linearize(pocketparse_engine *engine, const pocketparse_tree *tree,
                 pp_linearization *out);

/* ** A query-capture report: a binding produced by a successful match. name is
** the @capture name (without the @), span is the matched node's byte range, and
** text is those bytes (sliced from the input the host supplies). The CLI prints
** one of these per line; a host could collect them instead. */
typedef struct pp_query_capture {
    pocketparse_string name;
    uint32_t           start_byte;
    uint32_t           end_byte;
    pocketparse_string text;
} pp_query_capture;

/* ** The sink a query match reports captures through. emit is called once per
** capture of every accepted match, in match order; user rides back unchanged.
** Reporting through a callback keeps query.c freestanding -- it never does IO of
** its own. */
typedef struct pp_query_sink {
    void (*emit)(void *user, const pp_query_capture *capture);
    void  *user;
} pp_query_sink;

/* ** Read query .scm source, compile each pattern to a matcher over the
** linearized stream, run it against the tree, and report captures of every
** accepted match through `sink`. A pattern matches at any depth (descendant
** matching skips one balanced subtree); a `.` anchor disables the skip between
** adjacent children; `#eq?`/`#match?` predicates filter a whole match. `input`
** is the parsed bytes, used to slice capture text. The engine carries the regex
** machinery a `#match?` predicate uses. Returns POCKETPARSE_OK on a clean run
** (including zero matches) or POCKETPARSE_INVALID on a malformed query, writing
** a message to *error. Allocates from the engine's arena. */
pocketparse_result pp_query_run(pocketparse_engine *engine,
                                const pocketparse_tree *tree,
                                pocketparse_string query_source,
                                pocketparse_string input,
                                const pp_query_sink *sink,
                                const char **error);

/* ======================================================================== */
/* The shared VM/RD runtime (W14-3).                                         */
/*                                                                           */
/* The bytecode VM (vm.c) and the compiled parser the emitter generates       */
/* (emitparser.c -> a host-compiled file) run over the SAME state and          */
/* the SAME helpers, so a generated parser builds a byte-identical tree. The   */
/* VM interprets the lowered program in pp_vm_run; the generated parser bakes  */
/* the program structure into an explicit, arena-backed frame stack driven by  */
/* one dispatch loop that calls these helpers directly. Everything below is    */
/* private to the library; the                                                 */
/* generated file pulls it in through internal.h. The token matcher, tree      */
/* builder, extras, cursor and arena are reused verbatim -- only the rule-     */
/* level dispatch is compiled away. These declarations come last because the   */
/* state struct embeds the memo and builder types defined above.              */
/* ======================================================================== */


/* ** Rule-memo slot verdict codes (the `matched' field) and the snapshot
** threshold, shared by vm.c and the generated compiled parser (emitparser.c),
** which inlines the CALL memo logic into its explicit-stack frame machine. */
#define PP_RULE_FAIL    0u   /* the body fails at this key                     */
#define PP_RULE_SUCCESS 1u   /* the body succeeds; snapshot/end are valid      */
#define PP_RULE_SEEN    2u   /* the body succeeded once; not yet snapshotted   */

/* ** A success is snapshotted only when its subtree is at least this many nodes:
** tiny subtrees are cheaper to re-parse than to snapshot and copy. */
#define PP_RULE_MEMO_MIN_SUBTREE 32u

/* ** One entry in the rule memo (W67): a cached verdict for a rule body run at a
** key (program, offset, goal, field, alias). See vm.c for the full rationale.
** Occupancy lives OUTSIDE the slot, in the memo's `live' column (W15-1) -- the
** slot's own fields are read only after `live' says it is occupied, so the slot
** data needs no clearing. */
typedef struct pp_rule_slot {
    uint32_t program;
    uint32_t offset;
    uint32_t goal;
    uint32_t field;
    uint32_t alias;
    uint8_t  matched;
    uint32_t end;
    pp_node *snapshot;
    uint32_t snap_count;
} pp_rule_slot;

/* ** The rule memo: an open-addressing hash scoped to one parse and sized to the
** input. A NULL table (capacity 0) disables it.
**
** ZEROING (W15-1). Like the token-match memo, the rule-slot table is sized to
** input and ~48 bytes per slot, so pp_alloc would memset ~768 KB every parse --
** and the rule memo is DORMANT on a clean parse (it wakes only when backtracking
** goes super-linear), so that zeroing was pure waste on the common path. The fix
** is the same split: `slots' is raw (read only when occupied), and a parallel
** one-byte-per-slot `live' column -- the only thing zeroed -- carries occupancy:
** live[i] == 0 means slot i is empty. */
typedef struct pp_rule_memo {
    pp_arena     *arena;
    pp_rule_slot *slots;     /* raw (not zeroed): read only when live[i] != 0 */
    uint8_t      *live;      /* zeroed occupancy column, one byte per slot     */
    uint32_t      mask;
    uint32_t      count;
    uint32_t      capacity;     /* 0 until the tables are allocated (lazily)      */
    uint32_t      pending_length;/* input length the tables size to when ensured  */
} pp_rule_memo;

/* ** The whole mutable parse state both executors thread through the helpers:
** the cursor, the active lexical goal, recovery, the extras/token/rule memos,
** and the adaptive rule-memo activation counters. */
/* ** One held `attach: leading' trivia, kept as a plain span (kind symbol + byte
** range), NOT a tree node. Holding spans rather than pre-allocated nodes keeps the
** held buffer out of the builder.count rollback triad: a held record costs nothing
** in the tree until it is flushed, and rolling it back is truncating an array, not
** unwinding nodes. See pp_vm_state.held. */
typedef struct pp_held_extra {
    uint32_t symbol;
    uint32_t start;
    uint32_t end;
} pp_held_extra;

typedef struct pp_vm_state {
    pocketparse_parser  *parser;
    const pocketparse_grammar *grammar;
    const pp_program    *program;
    const uint8_t       *input;
    uint32_t             length;
    uint32_t             position;
    /* The held `attach: leading' trivia buffer. When a
    ** trivia kind marked LEADING is scanned, its span is appended here instead of being
    ** linked into the current sink; the next pp_vm_consume_extras flushes the buffer
    ** into ITS sink (as preceding siblings) before scanning, so a comment a closing
    ** node tried to claim re-emerges ahead of the next sibling in the parent.
    **
    ** SOUNDNESS. The buffer is mutable state that must survive PEG backtracking and the
    ** rule memo. Two facts make the discipline simple and provably correct:
    **   1. Records are plain spans, never destroyed -- only `held_count' moves. So a
    **      combinator that saves `held_count' before a speculative child and restores
    **      it on the child's failure brings the held spans BACK, because the span data
    **      past the restored count still sits untouched in `held'. A flush that
    **      drained the buffer into a sink which then rolls back is undone by the SAME
    **      restore: builder.count/sink->count erase the materialised nodes, and the
    **      restored `held_count' re-exposes the spans for the retry to re-flush. This
    **      is option (c) of the design note -- snapshot the count, which (because spans
    **      are immutable until overwritten by a genuinely new forward scan) restores
    **      the contents. No re-derivation and no deferred materialisation needed.
    **   2. The cursor rewinds with the sink, so a span that was rolled back is re-held
    **      from the same bytes if the retry walks them again; it is never both held and
    **      re-scanned (held wins, the retry flushes it). Hence no duplication.
    ** Every speculative combinator (CHOICE/OPTIONAL/REPEAT), the CALL frame, and the
    ** inline enter/exit save `held_count' alongside saved_sink and restore it on
    ** failure. The rule memo is disabled while the buffer is non-empty across a call
    ** boundary (see pp_vm_call_enter), so a memoed replay can never duplicate a flushed
    ** comment. Allocated from parser->arena; reset by pp_vm_state_init. */
    pp_held_extra       *held;
    uint32_t             held_count;
    uint32_t             held_capacity;
    /* The byte offset just past the last MATERIALIZED content -- a real token leaf, a
    ** flushed `leading' trivia node, or an attached `trailing' trivia node. A HELD
    ** `attach: leading' comment does NOT advance it: its bytes belong to the next
    ** sibling, not to whatever node is closing. A rule node closes its extent at this
    ** mark rather than at state->position, so a comment captured during the node's (or a
    ** descendant's) trailing-element consume -- which advanced the cursor past it before
    ** it floated out as a sibling -- is excluded from the node's span. Without this the
    ** closing node's end_byte ran across the floated comment, overlapping it as a
    ** sibling; the principle is that a declaration's extent shrinks back to the bytes it
    ** actually covers. It tracks state->position exactly whenever no
    ** comment is held, so the `trailing'-only default is byte-identical. Like the held
    ** buffer it is speculative state: saved alongside saved_pos/saved_held at every
    ** combinator and the CALL frame, restored on failure. Reset by pp_vm_state_init. */
    uint32_t             content_end;
    const pp_ir         *recover;
    int                  recover_fired;
    uint32_t             recover_start;
    uint32_t             active_goal;
    int                  no_extras;
    pp_match_memo        memo;
    pp_extras_memo       extras_memo;
    pp_rule_memo         rule_memo;
    uint32_t             call_attempts;
    uint32_t             memo_threshold;
    int                  memo_active;
    /* The native recursive-descent depth guard (W16-2). The generated parser
    ** (emitparser.c) is true C recursion -- one C call per rule body -- so a
    ** deeply nested / adversarial input could overflow the host C stack. `depth'
    ** counts rule-body entries; when it would exceed `depth_limit' the generated
    ** rule returns PP_VM_FAIL instead of recursing, so a pathological input fails
    ** GRACEFULLY rather than crashing. The VM (pp_vm_run) iterates on an arena
    ** stack and never recurses, so it leaves depth_limit at PP_NO_INDEX (no guard);
    ** pp_vm_state_init sets that default and the generated driver lowers it to the
    ** measured limit. */
    uint32_t             depth;
    uint32_t             depth_limit;
    /* W16-GOAL: when the caller's pp_vm_frame lives on a STABLE address for the
    ** rule body's whole lifetime -- which holds for the generated native recursive-
    ** descent parser, where each frame is a true C local -- pp_vm_call_enter uses the
    ** frame's inline `children_storage' instead of allocating a fresh sink struct
    ** per rule, removing one hot per-rule allocation. The VM (pp_vm_run) keeps its
    ** frames on an arena stack that RELOCATES when it grows, so a pointer into a
    ** frame would dangle; it leaves this 0 and call_enter heap-allocates as before.
    ** pp_vm_state_init sets the safe default (0); the generated driver sets it to 1. */
    int                  children_inline;
    /* W17-2 baked extras scanner. pp_vm_consume_extras matches the trivia tokens
    ** (whitespace, comments) by looping pp_token_match_memo over the extras table --
    ** the generic token VM, the last large token-VM slice once the per-token leaf
    ** scanners are baked. The generated parser sets this to a baked scanner: for the
    ** cursor it returns the FIRST extras-table entry (in order) whose baked matcher
    ** matches with a non-empty span, plus its end, so consume_extras runs the same
    ** policy / node / diagnostic logic but skips the token VM. PURE over the input
    ** span; on a hit writes *out_extra (index into grammar->extras) and *out_end and
    ** returns 1, else 0. The VM leaves it NULL (its extras stay on the generic loop);
    ** byte-identity holds because the baked matcher returns the identical end. */
    int (*extras_match)(const pocketparse_engine *engine, const uint8_t *input,
                        uint32_t length, uint32_t start,
                        uint32_t *out_extra, uint32_t *out_end);
} pp_vm_state;

/* ** Claim a deferred-slot declaration (PP_OP_DEFERRED_SLOT) in the innermost active
** scope (this rule invocation's own). Idempotent for a name already claimed; 0 only
** when the scope has no free slot (a slot_count under-count -- an invariant break).
** Shared by the VM and the generated parser. */
int pp_slot_claim(pp_vm_state *state, uint32_t slot_name);

/* ** Resolve a deferred slot by name (for PP_OP_DEFER / PP_OP_PARSE_DEFERRED),
** scanning active scopes innermost-to-outermost -- lexical scoping. NULL when no
** enclosing scope declared the name. Shared by the VM and the generated parser. */
pp_slot_frame *pp_slot_resolve(pp_vm_state *state, uint32_t slot_name);

/* ** Slot-save rollback for speculative combinators (choice/optional/repeat). On
** branch entry pp_slot_save_push snapshots every active slot's count behind a
** sentinel; on branch failure pp_slot_save_restore_pop truncates each slot back to
** its snapshot (discarding items a failed branch enqueued); on branch success
** pp_slot_save_commit_pop drops the snapshot, keeping the items. Both the VM and
** the generated parser call these at the SAME points so backtrack rollback is
** byte-identical (lex-goal-implementation, "Slot rollback on backtrack"). Returns 0
** only on allocation failure. */
int  pp_slot_save_push(pp_vm_state *state);
void pp_slot_save_restore_pop(pp_vm_state *state);
void pp_slot_save_commit_pop(pp_vm_state *state);

/* ** The signal a (sub-)parse returns: matched, failed, or out of memory. */
typedef enum pp_vm_signal {
    PP_VM_FAIL = 0,
    PP_VM_MATCH_OK = 1,
    PP_VM_OOM = -1
} pp_vm_signal;

/* ** One frame of the explicit work/continuation stack the iterative driver
** (pp_vm_run, W14-6) pumps. Each frame is one in-progress program node carrying
** the marks the old C recursion held in locals -- the saved builder count, input
** position and sink count, the child sink, the repeat counter, the saved
** recover/goal/no_extras, and the rule node / memo key for a CALL -- so resuming a
** frame after its child frame resolves picks up exactly where the C return would
** have. `step' is the phase within the op (which child / which attempt is in
** flight). The same struct is shared by vm.c's driver and the generated compiled
** parser (emitparser.c references it), so there is one frame layout. */
typedef struct pp_vm_frame {
    uint32_t      program;        /* program index being executed        */
    uint32_t      step;           /* phase within this op                 */
    pp_vm_sink   *sink;           /* sink this program writes into        */
    uint32_t      field;
    uint32_t      alias;
    uint32_t      saved_count;    /* builder/pos/sink marks for restore   */
    uint32_t      saved_pos;
    uint32_t      saved_sink;
    uint32_t      saved_held;     /* held-trivia high-water mark for restore */
    uint32_t      saved_content;  /* content_end mark for restore         */
    uint32_t      i;              /* SEQ/CHOICE child index               */
    uint32_t      reps;           /* REPEAT iteration count               */
    uint32_t      look;           /* CHOICE lookahead position            */
    const pp_ir  *outer_recover;  /* saved recovery scope                 */
    int           outer_fired;
    uint32_t      outer_start;
    uint32_t      saved_goal;     /* saved lexical goal (GOAL)            */
    int           saved_no_extras;/* saved no_extras (NO_EXTRAS)          */
    pp_vm_sink   *children;       /* CALL: the rule body's child sink     */
    pp_vm_sink    children_storage;/* CALL: inline sink storage, used when   */
                                  /* state->children_inline (stable C frame) */
    uint32_t      node;           /* CALL: the rule node index            */
    uint32_t      start;          /* CALL: the rule node start byte       */
    uint32_t      kind;           /* CALL: the rule node kind             */
    uint32_t      target;         /* CALL: body program / memo key        */
    uint32_t      memo_pos;       /* CALL: memo key offset                */
    uint32_t      diag_before;    /* CALL: diagnostics count on entry     */
    int           memoable;       /* CALL: memo eligible at this site     */
    int           seen_success;   /* CALL: a marked success is being retried */
    int           hidden;         /* CALL: rule is PP_RULE_HIDDEN, no alias (W17-FLAT) */
    int           transparent;    /* CALL: rule is TRANSPARENT_ALWAYS, no alias,    */
                                  /* no node pushed; body ran into the caller's sink */
                                  /* (W17-RECLAIM)                                   */
    int           seen_transparent_mark; /* CALL (VM only): the TRANSPARENT_ALWAYS  */
                                  /* mark, read once at prologue entry and consulted */
                                  /* after the memo probe to arm `transparent'       */
    uint32_t       slot_count;    /* CALL: number of deferred-slot frames for this   */
                                  /* invocation (lex-goal-implementation)            */
    pp_slot_frame *slots;         /* CALL: per-invocation slot storage, NULL when    */
                                  /* slot_count == 0 (lex-goal-implementation)       */
} pp_vm_frame;

/* ** The explicit work/continuation stack, grown geometrically from the parse
** arena (arenas never realloc in place, so growth copies; the lifetime is the
** parse). Shared by vm.c and the generated parser. */
typedef struct pp_vm_stack {
    pp_vm_frame *frames;
    uint32_t     count;
    uint32_t     capacity;
} pp_vm_stack;

/* ** Push a fresh frame for `program' onto the arena-backed stack. The new frame's
** step starts at 0; its scratch marks are zeroed lazily by the op that uses them.
** Returns 0 only on allocation failure. */
int pp_vm_stack_push(pp_arena *arena, pp_vm_stack *stack, uint32_t program,
                     pp_vm_sink *sink, uint32_t field, uint32_t alias);

/* ** The CALL prologue/epilogue, factored out of the old recursive
** pp_vm_run_call_body so the iterative driver runs the rule body on the SAME
** explicit stack (no C recursion). `enter' does the memo probe, the leading-extras
** consume and the node open; it returns 1 to descend into the body or 0 to resolve
** immediately with *resolved holding the signal. `exit' does the link / memo store
** after the body returns `sig' and returns the CALL frame's signal. Both reproduce
** pp_vm_run_call_body's prologue/epilogue byte-for-byte; the generated compiled
** parser calls the same two functions. */
int pp_vm_call_enter(pp_vm_state *state, pp_vm_frame *frame,
                     uint32_t target, uint32_t symbol, pp_vm_sink *sink,
                     uint32_t field, uint32_t alias, int *resolved);
int pp_vm_call_exit(pp_vm_state *state, pp_vm_frame *frame, pp_vm_sink *sink,
                    uint32_t field, uint32_t alias, int sig);

/* ** The raw hidden-by-symbol index byte for `symbol' (W17-RECLAIM): bit 0
** (PP_RULE_HIDDEN) marks an exit-time collapse candidate, bit 1
** (PP_RULE_TRANSPARENT_ALWAYS) a rule whose wrapper node the entry path skips.
** Inlined so the generated-parser prologue reads both marks with ONE array probe
** instead of two cross-TU calls (pp_grammar_rule_hidden + ..._transparent); the
** index lives directly on the grammar. Returns 0 when the index was not built or
** the symbol is out of range -- a rule simply stays visible (sound). */
static inline uint32_t pp_grammar_rule_marks(const pocketparse_grammar *grammar,
                                             uint32_t symbol)
{
    if (grammar->hidden_index != 0 && symbol < grammar->hidden_index_size)
        return grammar->hidden_index[symbol];
    return 0;
}

/* The clean-path inline lifecycle below references these runtime helpers (also
** declared in full further down); declare them here so the inline bodies compile. */
int pp_vm_consume_extras(pp_vm_state *state, pp_vm_sink *sink);
int pp_vm_sink_grow(pp_arena *arena, pp_vm_sink *sink);
void pp_vm_link_children(pp_tree_builder *builder, uint32_t parent,
                         const pp_vm_sink *children);

/* ** True when `symbol' names a HIDDEN rule (PP_RULE_HIDDEN). A pure read of the
** hidden-by-symbol index (W17-FLAT); the index is built once, eagerly, by
** pp_grammar_build_hidden_index before the parse loop runs (mirroring token_index
** / extras_lead), so the hot path is one bounds check + one array read on a const
** grammar. Returns 0 when no rule is hidden or the index was not built. */
int pp_grammar_rule_hidden(const pocketparse_grammar *grammar, uint32_t symbol);

/* ** Build the grammar's hidden-rule-by-symbol index if not already built
** (W17-FLAT). Called once before the parse loop, like the token index. */
void pp_grammar_build_hidden_index(pocketparse_grammar *grammar);

/* ** The shared hidden-rule collapse, run from the node-close epilogue when a
** rule marked PP_RULE_HIDDEN turned out to be a pure pass-through (W17-FLAT):
** the body contributed exactly one child and the call carried no alias. Instead
** of emitting the wrapper node `node', promote its single child into the caller's
** sink, re-stamped with the call's field, so the wrapper is omitted from the tree
** and its child takes its place. The wrapper node slot is left orphaned (nothing
** links to it; the cursor never reaches it). Returns 1 on success, 0 on OOM (the
** push grow failed). It is the single mechanism the VM and the generated parser
** both call, so the two engines flatten identically. */
static inline int pp_vm_collapse_hidden(pp_vm_state *state, pp_vm_frame *fr,
                                        pp_vm_sink *sink, uint32_t field);

/* ** Append a node index to a sink, and (when the sink names a parent) wire the
** child's parent/next_sibling inline -- the W17-CST incremental sibling linking.
** It is inline at every emit site because it is the single hottest CST call (one
** per node), and the parent-known branch the compiler folds away per call site.
** The grow is the only out-of-line slow path (pp_vm_sink_grow). The previous
** sibling is read from items[count-1] (the rollback ledger), not a cached pointer,
** so a failed-attempt rollback that truncated the count re-derives the correct
** last sibling on the next push; pp_vm_sink_close terminates the final one. */
static inline int pp_vm_sink_push(pp_tree_builder *builder, pp_arena *arena,
                                  pp_vm_sink *sink, uint32_t index)
{
    if (sink->count == sink->capacity && !pp_vm_sink_grow(arena, sink))
        return 0;
    if (sink->parent != PP_NO_INDEX) {
        builder->nodes[index].parent = sink->parent;
        if (sink->count > 0)
            builder->nodes[sink->items[sink->count - 1]].next_sibling = index;
    }
    sink->items[sink->count++] = index;
    return 1;
}

/* ** Finalise a parent's children after its body matched (W17-CST): the O(1)
** replacement for the pp_vm_link_children second pass when the sink linked its
** siblings inline. pp_vm_sink_push already wired every child's parent and forward
** next_sibling, so only three things remain: stamp first_child + child_count on the
** parent, terminate the final sibling (its next_sibling was left dangling), and
** extend the parent span to cover the children. Reads only items[0], items[count-1]
** and count -- constant time, where pp_vm_link_children walked all children. */
static inline void pp_vm_sink_close(pp_tree_builder *builder, uint32_t parent,
                                    const pp_vm_sink *children)
{
    pp_node *p = &builder->nodes[parent];
    p->child_count = children->count;
    if (children->count > 0) {
        uint32_t first = children->items[0];
        uint32_t last  = children->items[children->count - 1];
        uint32_t s  = builder->nodes[first].start_byte;
        uint32_t en = builder->nodes[last].end_byte;
        p->first_child = first;
        builder->nodes[last].next_sibling = PP_NO_INDEX;
        if (s < p->start_byte)
            p->start_byte = s;
        if (en > p->end_byte)
            p->end_byte = en;
    } else {
        p->first_child = PP_NO_INDEX;
    }
}

/* ** The hidden-rule collapse (declared above). The caller has already proved
** the rule is hidden, the call carried no alias, and the body contributed exactly
** one child (fr->children->count == 1). Promote that child: re-stamp it with the
** call's field (so a ladder wrapper's inner `left:'/`condition:' field is replaced
** by whatever the outer call wanted, exactly as tree-sitter attaches a field only
** to the surviving node), reset its parent/sibling links so pp_vm_sink_push wires
** them afresh against the real parent, and push it into the caller's sink in the
** wrapper's place. The wrapper node fr->node is never linked anywhere, so it is
** invisible to the cursor. */
static inline int pp_vm_collapse_hidden(pp_vm_state *state, pp_vm_frame *fr,
                                        pp_vm_sink *sink, uint32_t field)
{
    pp_tree_builder *builder = &state->parser->builder;
    uint32_t child = fr->children->items[0];
    builder->nodes[child].field        = field;
    builder->nodes[child].parent       = PP_NO_INDEX;
    builder->nodes[child].next_sibling = PP_NO_INDEX;
    if (sink && !pp_vm_sink_push(builder, state->parser->arena, sink, child))
        return 0;
    return 1;
}

/* ** Inline fast-reject for the extras (whitespace/comment) scan (W17-GOAL).
** pp_vm_consume_extras runs before every terminal and on every rule entry, so it is
** the single hottest cross-TU call once the lifecycle is inlined. At the dominant
** position the cursor byte begins NO extra (an identifier/operator/string byte), and
** the function already proves that with one lead-byte bitset probe and returns -- but
** as a real call. This wrapper does that O(1) reject INLINE at every call site, so the
** common "nothing to consume" case never makes the call; only a byte that could begin
** an extra (or a no_extras scope, or an unpinnable lead set, or a lexical goal that
** could redirect a lexeme) falls through to the full pp_vm_consume_extras. The probe
** is byte-for-byte the function's own fast path, ordered identically (no_extras first,
** then the lead-byte set), so the result is identical. The lead set is built lazily by
** pp_vm_state_init before any consume runs, so extras_lead_state is set here. */
static inline int pp_vm_extras(pp_vm_state *state, pp_vm_sink *sink)
{
    const pocketparse_grammar *grammar = state->grammar;
    /* A held `attach: leading' comment must drain at THIS sibling boundary, so the
    ** lead-byte reject below cannot short-circuit when the buffer is non-empty: fall
    ** through to pp_vm_consume_extras, whose first act is to flush it. With an empty
    ** buffer (the dominant case) this is the historical fast path, byte-for-byte. */
    if (state->held_count == 0) {
        if (state->no_extras)
            return 1;
        if (!grammar->extras_lead_open && grammar->extras_lead_state) {
            uint32_t start = state->position;
            if (start >= state->length)
                return 1;                      /* end of input: no extra can match */
            {
                uint8_t b = state->input[start];
                if (((grammar->extras_lead[b >> 3] >> (b & 7)) & 1) == 0)
                    return 1;                  /* this byte begins no extra */
            }
        }
    }
    return pp_vm_consume_extras(state, sink);
}

/* ** The per-rule CALL lifecycle for the GENERATED native parser (Phase 17
** de-packrat). The generated recursive-descent parser is the code a human would
** write by hand, and a hand-written RD parser carries NO packrat memo -- so these
** two `static inline' wrappers carry none either: no probe, no store, no snapshot,
** no replay, and no call_attempts / memo_threshold / memo_active bookkeeping. They
** do ONLY the work a hand parser does: save the marks, consume leading extras, open
** the node, and (on exit) link the children, close the span, push the node, inlined
** into the rule function with the call's field/alias in registers.
**
** They DELEGATE to the shared pp_vm_call_enter/exit only when the lifecycle is
** genuinely non-trivial -- recovery armed, a no_extras scope in force, the VM's
** relocatable (non-inline) sink, or a trace plug installed. Those delegated calls
** carry the shared memo code, but it is INERT on the generated path: the generated
** driver never wakes the memo (memo_active stays 0 -- the generated parser does not
** set the threshold/counter at all), so pp_vm_call_enter's `fr->memoable' is always
** 0 and its probe/store never run. The memo lives ONLY on the VM (pp_vm_run), which
** keeps it because it must bound backtracking for arbitrary runtime-supplied
** grammars; the std grammars the generated parser ships are left-factored to linear,
** so it needs no backstop. Stripping the memo here is byte-identical (a memo is a
** pure cache) and is what makes the generated parser read and run like hand-written
** recursive descent. */
static inline int pp_vm_enter(pp_vm_state *state, pp_vm_frame *fr,
                              uint32_t symbol, pp_vm_sink *sink,
                              uint32_t field, uint32_t alias, int *resolved)
{
    /* Delegate only for the genuinely non-trivial lifecycle (recovery armed, a
    ** no_extras scope, a relocatable sink, a trace plug); no memo condition appears
    ** because the generated parser carries no memo. */
    if (state->recover != 0 || state->no_extras != 0 ||
        !state->children_inline || state->parser->engine->plug_count != 0)
        return pp_vm_call_enter(state, fr, symbol, symbol, sink,
                                field, alias, resolved);
    /* The hand-written steady state, inlined: marks, extras, node open. */
    fr->kind        = (alias != PP_NO_SYMBOL) ? alias : symbol;
    fr->saved_count = state->parser->builder.count;
    fr->saved_pos   = state->position;
    fr->saved_sink  = sink ? sink->count : 0;
    /* Held-trivia and content-end marks, captured BEFORE the leading-extras consume
    ** below (which may flush the buffer / advance content_end). On a failed body the
    ** exit restores both, re-exposing any spans a flush drained so the retry re-flushes
    ** them and recomputing the extent -- see pp_vm_state.held / content_end. */
    fr->saved_held    = state->held_count;
    fr->saved_content = state->content_end;
    fr->memoable    = 0;
    /* W17-FLAT / W17-RECLAIM: read both rule marks with ONE inline index probe.
    ** Bit 0 (PP_RULE_HIDDEN) makes the rule an exit-time collapse candidate; bit 1
    ** (PP_RULE_TRANSPARENT_ALWAYS) additionally lets the entry path skip the wrapper
    ** node outright -- consume leading extras into the caller's sink (as the node
    ** open would have), then run the body straight into THAT sink so its single
    ** child lands in the wrapper's place with no node ever allocated, the call's
    ** field re-stamped onto it at exit. Both gated on no alias (an aliased call must
    ** keep the renamed node). One probe, not two -- the two-call form measured
    ** slower than the node it saved on the expression-heavy corpus (W17-RECLAIM). */
    {
        uint32_t marks = (alias == PP_NO_SYMBOL)
                         ? pp_grammar_rule_marks(state->grammar, symbol) : 0;
        fr->hidden      = (marks & PP_RULE_HIDDEN) != 0;
        fr->transparent = (marks & PP_RULE_TRANSPARENT_ALWAYS) != 0;
    }
    if (!pp_vm_extras(state, sink)) { *resolved = PP_VM_OOM; return 0; }
    if (fr->transparent) {
        fr->children = sink;          /* body emits into the caller's sink */
        fr->start    = state->position;
        /* fr->node carries no wrapper node here; repurpose it to hold the sink
        ** index of the FIRST node the body will contribute -- captured AFTER the
        ** leading extras were consumed into the caller's sink, so it names the
        ** body's significant child (the one the collapse re-stamps), not a leading
        ** trivia node. fr->saved_sink stays pre-extras for the failure rollback. */
        fr->node     = sink ? sink->count : 0;
        return 1;
    }
    fr->children = &fr->children_storage;
    fr->children->items = 0; fr->children->count = 0; fr->children->capacity = 0;
    /* Open at content_end (the last materialized byte), not the raw cursor, so a node
    ** whose enter consume held a comment does not open past it. content_end ==
    ** state->position whenever nothing is held. See pp_vm_call_enter / content_end. */
    fr->start = state->content_end;
    fr->node  = pp_tree_builder_push(&state->parser->builder, fr->kind, field,
                                     fr->start, fr->start, PP_NO_INDEX);
    if (fr->node == PP_NO_INDEX) { *resolved = PP_VM_OOM; return 0; }
    /* W17-CST: name the parent so the body's children link inline as they emit. */
    fr->children->parent = fr->node;
    return 1;
}

/* ** The hand-written CALL epilogue (see pp_vm_enter). `fr->memoable' is 0 exactly
** when pp_vm_enter took its inline path, so when it delegated (the genuinely
** non-trivial cases) we route to pp_vm_call_exit for the right restore/link; on the
** inline path we do the bare link/close/push a hand parser does, with no memo
** store. Byte-identical either way. */
static inline int pp_vm_exit(pp_vm_state *state, pp_vm_frame *fr, pp_vm_sink *sink,
                             uint32_t field, uint32_t alias, int sig)
{
    if (fr->memoable || state->parser->engine->plug_count != 0)
        return pp_vm_call_exit(state, fr, sink, field, alias, sig);
    if (sig == PP_VM_OOM)
        return PP_VM_OOM;
    if (sig != PP_VM_MATCH_OK) {
        state->parser->builder.count = fr->saved_count;
        state->position = fr->saved_pos;
        if (sink)
            sink->count = fr->saved_sink;
        state->held_count = fr->saved_held;
        state->content_end = fr->saved_content;
        return PP_VM_FAIL;
    }
    /* W17-RECLAIM: a transparent rule pushed no node and ran its body straight into
    ** the caller's sink, so its single child already sits at sink[saved_sink] with
    ** parent/sibling wired to the real parent. Only re-stamp the call's field onto
    ** it -- exactly what the collapse did, with the wrapper allocation reclaimed. */
    if (fr->transparent) {
        if (sink && sink->count > fr->node)
            state->parser->builder.nodes[sink->items[fr->node]].field = field;
        return PP_VM_MATCH_OK;
    }
    /* W17-FLAT: a hidden rule that turned out to be a pure pass-through (exactly
    ** one child, no operator/extra structure) is omitted -- promote its single
    ** child in the wrapper's place. Otherwise it emits its node normally. */
    if (fr->hidden && fr->children->count == 1) {
        if (!pp_vm_collapse_hidden(state, fr, sink, field))
            return PP_VM_OOM;
        return PP_VM_MATCH_OK;
    }
    pp_vm_sink_close(&state->parser->builder, fr->node, fr->children);
    /* Close the extent at the last materialized byte (content_end), not the raw cursor,
    ** so a held/floated `attach: leading' comment is excluded from this node's span;
    ** content_end == state->position whenever nothing is held, so the default is
    ** byte-identical. See pp_vm_call_exit. */
    if (state->content_end > state->parser->builder.nodes[fr->node].end_byte)
        state->parser->builder.nodes[fr->node].end_byte = state->content_end;
    if (sink && !pp_vm_sink_push(&state->parser->builder, state->parser->arena,
                                 sink, fr->node))
        return PP_VM_OOM;
    return PP_VM_MATCH_OK;
}

/* ** Run one lowered program to completion on an explicit, arena-backed frame
** stack, building into `sink' with the given field/alias decoration (W14-6). This
** is the iterative driver -- there is NO C recursion across rule calls, so a
** deeply nested input grows the arena stack instead of overflowing the C stack.
** It is the single source of truth: the bytecode VM (pp_execute_vm) and the
** generated compiled parser both drive parsing through it. Returns a pp_vm_signal.
** The grammar it walks is state->program (set by pp_vm_state_init). */
int pp_vm_run(pp_vm_state *state, uint32_t program, pp_vm_sink *sink,
              uint32_t field, uint32_t alias);

/* ** Consume extras at the cursor into `sink' (trivia/error become leaf nodes,
** skip is dropped). Returns 0 only on allocation failure. */
int pp_vm_consume_extras(pp_vm_state *state, pp_vm_sink *sink);

/* ** Flush every held `attach: leading' trivia span into `sink' as leaf nodes (in
** the order held, as preceding siblings), then clear the buffer. A no-op when the
** buffer is empty. Returns 0 only on allocation failure. Called at the start of
** every consume and by the end-of-input finalizers so a held doc comment lands ahead
** of the next sibling rather than being stranded. */
int pp_vm_flush_held(pp_vm_state *state, pp_vm_sink *sink);

/* ** Link a vector of child indices into a parent node and extend its span. */
void pp_vm_link_children(pp_tree_builder *builder, uint32_t parent,
                         const pp_vm_sink *children);

/* ** Synthesize the active recover node's zero-width MISSING insert. Returns 0
** on allocation failure. */
int pp_vm_recover_insert(pp_vm_state *state, pp_vm_sink *sink);

/* ** Emit a rule/token trace event by symbol name (no-op without a trace plug). */
void pp_vm_trace_symbol(pp_vm_state *state, const char *event, uint32_t symbol);

/* ** Match a MATCH instruction's pattern (token/literal) at the cursor, applying
** the lexical-goal redirect, and emit a leaf. The pattern/kind are the IR node
** the terminal matches and its kind symbol. Returns 1 match, 0 no-match, -1 OOM. */
int pp_vm_do_match_pattern(pp_vm_state *state, pp_ir *pattern, uint32_t kind,
                           pp_vm_sink *sink, uint32_t field, uint32_t alias);

/* ** Run a byte-level op (any/class/class-name/not) at the cursor: consume extras,
** match, advance WITHOUT emitting a node. Returns 1 match, 0 no-match, -1 OOM. */
int pp_vm_do_byte_pattern(pp_vm_state *state, pp_ir *pattern, pp_vm_sink *sink);

/* ** Evaluate a rule-level NOT's token-level guard purely (no node, no cursor
** advance): 1 if the guard matches (the NOT must FAIL), 0 otherwise (run the body).
** `guard' is child[0]'s base token pattern; `trigger' the guard's token symbol
** (PP_NO_SYMBOL for a literal/class guard), through which a live parametrized goal
** redirects. Shared by the VM's PP_VM_NOT_RULE and the generated rule-level NOT so
** the guard is byte-identical (lex-goal-implementation, "Rule-level NOT"). */
int pp_vm_not_rule_guard(pp_vm_state *state, const pp_ir *guard, uint32_t trigger);

/* ** Match a terminal with a BAKED per-token matcher (W17-2): consume extras, apply
** the lexical-goal redirect (generic fall-back), match the token via `matchfn' (a
** pure per-token end-finder the generated parser bakes from the token IR, replacing
** the generic token VM pp_token_run), emit the leaf and advance. Byte-identical to
** pp_vm_terminal. `kind' is the token kind symbol; `pattern' its IR. Returns 1/0/-1. */
int pp_vm_terminal_baked(pp_vm_state *state, pp_ir *pattern, uint32_t kind,
                         pp_vm_sink *sink, uint32_t field, uint32_t alias,
                         int (*matchfn)(const pocketparse_engine *,
                                        const uint8_t *, uint32_t, uint32_t,
                                        uint32_t *));

/* ** Run the EOF op: consume extras, then succeed iff the cursor is at end of
** input. Returns PP_VM_MATCH_OK / PP_VM_FAIL / PP_VM_OOM. */
int pp_vm_do_eof(pp_vm_state *state, pp_vm_sink *sink);

/* ** Run the INJECT op: match the region body (via `body'), then splice an
** embedded parse of that span under an injection node. `inject' is the inject IR
** node. Returns PP_VM_MATCH_OK / PP_VM_FAIL / PP_VM_OOM. */
int pp_vm_do_inject(pp_vm_state *state, pp_vm_sink *sink, pp_ir *inject,
                    int (*body)(pp_vm_state *, pp_vm_sink *,
                                uint32_t, uint32_t, void *),
                    void *body_user);

/* ** The rule-memo probe/store and the success snapshot/replay (W67), used by the
** shared CALL prologue/epilogue (pp_vm_call_enter/exit) the iterative driver and
** the generated compiled parser both run. Probe returns the matching used slot, the empty
** slot to insert at, or NULL when full; store records a verdict (and, for a
** success, the snapshot subtree); snapshot_subtree copies [root,end_count) into a
** fresh base-0 array; replay_success copies a cached snapshot into the builder as
** fresh nodes and advances the cursor. See vm.c for the full rationale. */
pp_rule_slot *pp_rule_memo_probe(pp_rule_memo *memo, uint32_t program,
                                 uint32_t offset, uint32_t goal,
                                 uint32_t field, uint32_t alias);
/* ** Whether a probed rule slot is occupied -- the memo's `live' column, not a
** field in the (raw) slot, so an empty slot's fields are never read (W15-1). A
** NULL slot (full table) is not live. */
int pp_rule_slot_live(const pp_rule_memo *memo, const pp_rule_slot *slot);
void pp_rule_memo_store(pp_rule_memo *memo, pp_rule_slot *slot,
                        uint32_t program, uint32_t offset, uint32_t goal,
                        uint32_t field, uint32_t alias, uint8_t matched,
                        uint32_t end, pp_node *snapshot, uint32_t snap_count);
pp_node *pp_vm_snapshot_subtree(pp_tree_builder *builder, uint32_t root,
                                uint32_t end_count, uint32_t *out_count);
int pp_vm_replay_success(pp_vm_state *state, pp_vm_sink *sink,
                         const pp_rule_slot *slot);

/* ** Initialise a pp_vm_state for a fresh parse of `parser' over `program',
** building the token index and the (adaptive) rule memo. Mirrors the setup at
** the top of pp_execute_vm so a generated parser starts identically. */
void pp_vm_state_init(pp_vm_state *state, pocketparse_parser *parser,
                      const pp_program *program);

/* ** Freeze the parser's node builder into an immutable tree rooted at `root'. */
pocketparse_result pp_vm_freeze_tree(pocketparse_parser *parser, uint32_t root);

/* ** The buffer-friendly top-level item loop shared by every generated parser
** (W17-4). `state' has already been initialised by the generated <prefix>_run_chunk
** (it wires the baked extras scanner and the measured depth limit, exactly as the
** whole-input driver does) and points at the parser whose bf_* fields carry the
** resumption state. `item_fn' is the generated function that parses ONE top-level
** item (the start rule's repeat child) into the given sink; `has_eof' is non-zero
** when the start body ends in EOF (so trailing input is an error only once the
** input is complete); `min' is the repeat's lower bound.
**
** Each call parses every top-level item that completes STRICTLY within the buffer
** (the cursor ending before parser->input_length proves a following byte delimits
** the item). When the cursor reaches the buffer end with parser->input_complete
** still 0, the loop rolls back to the start of the unfinished item, saves the
** boundary in parser->bf_position, and returns POCKETPARSE_MORE without freezing.
** When input_complete is set, the loop finalises exactly like the whole-input
** driver -- trailing extras, link, EOF/trailing check, freeze -- so the resulting
** tree is byte-identical to a single whole parse. The root and the child sink
** persist across calls in the parser, so items committed on earlier calls are kept
** and the parse resumes at bf_position rather than re-running from byte 0. */
pocketparse_result pp_parser_chunk_loop(
    pp_vm_state *state,
    int (*item_fn)(pp_vm_state *, pp_vm_sink *, uint32_t, uint32_t),
    int has_eof, uint32_t min);

/* ** Emit a compiled-parser C source for a grammar (W14-3): the grammar baked
** into an explicit, arena-backed frame stack driven by one dispatch loop,
** byte-identical to the VM. The bytes are carved from the engine arena and
** returned through out_source; the host does the file IO. Lives in
** emitparser.c; pocketparse_emit_c calls it behind POCKETPARSE_EMIT_PARSER.
** When `lib' is non-zero the standalone main()/dump helpers are omitted and the
** callable entries pp_compiled_parser_load/_run are emitted instead, so a host
** can drive the compiled parser in-process (POCKETPARSE_EMIT_PARSER_LIB). */
pocketparse_result pp_emit_parser(pocketparse_engine *engine,
                                  const pocketparse_grammar *grammar,
                                  int lib,
                                  pocketparse_string *out_source);

#endif /* POCKETPARSE_INTERNAL_H */

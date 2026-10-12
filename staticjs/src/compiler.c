/*
 * The compiler: a sink of syntax events that emits bytecode as each event
 * arrives. Events come in evaluation order, so most become one instruction;
 * the branching constructs keep their jumps on a stack and patch them when
 * their end arrives. Every jump goes forward: there is no loop.
 *
 * Statements: an if is a JUMP_FALSY over its consequent, and a JUMP over the
 * alternative after a consequent that does not end in a return. An else-if
 * chain is one construct, whose jumps to its end wait in c->jumps until its
 * IF_END, so a chain of any length nests once. Whether code falls through is
 * a peephole (ends_in_return): the last instruction is a RETURN and no jump
 * lands after it. Falling off the end of a body returns undefined.
 *
 * Scopes: one stack holds the scopes of functions and of block statements.
 * A block's names are its own, in slots of its function after those in use
 * when it begins; when it ends, the slots are free for what follows, and a
 * function's locals are the most it uses at once. Reusing a slot is safe: a
 * closure copies what it captures when it is made, and a slot is set before
 * it is read, as a name cannot be read before its declaration.
 *
 * Names are resolved when they are read. A function captures the values of
 * the names it uses from enclosing functions when it is created: every
 * binding it can capture is const (MUTATION: a let, which statements assign
 * to, is never captured), so a copy is the same as a reference. A name may not be
 * used before its const declaration or import, which also makes a function
 * unable to refer to itself: a name read is noted in every scope it is
 * looked for in, blocks included, and a later declaration there is refused. A name bound nowhere is reported at the end, so
 * that a syntax error after it comes first, as in JavaScript.
 *
 * A module's value is its namespace: an object of every export, with keys in
 * the order JavaScript sorts a namespace's (by UTF-16 code unit). Exports are
 * collected as they arrive and resolved, sorted and merged with the modules
 * of `export *` at the end, since `export { a }` may come before `const a`.
 *
 * Every lookup by name goes through a hash index, so that compiling takes
 * time in proportion to the source, and to each module it imports from once.
 *
 * Parameters compile in order, as they arrive: ARROW_BEGIN says how many
 * there are, so they take the first slots, and a pattern's names the ones
 * after. A default value is code at the start of the body that runs when
 * its parameter or element is undefined; the parser sends its events before
 * the target's, so nothing jumps backwards. A default may read the
 * parameters before it, which are bound by then; reading a later one, its
 * own, or a name the body declares is refused as a read before the
 * declaration (JavaScript throws, or, through the separate scope it gives
 * the parameters, finds another binding). With that, the separate scope
 * needs no work of its own: a body's const cannot share a parameter's name
 * (declared twice), and a closure in a default captures parameters by
 * value, which every binding being const makes the same as a reference (a
 * parameter is never assigned to).
 *
 * The last read of a slot moves its value instead of copying it: a LOCAL
 * becomes a TAKE_LOCAL, which leaves the slot undefined, so that a value the
 * slot alone held reaches what reads it with one reference and can be
 * updated in place (sjs_value_unique). Every jump goes forward and a
 * function's body runs at most once per frame, so on any path through a
 * frame instructions run in increasing pc order: a read with no access to
 * its slot at a later pc is the last on every path. The compiler keeps, per
 * slot of each open function, the pc of its latest LOCAL (c->last_reads),
 * and turns it into a TAKE_LOCAL when nothing can follow it: when the block
 * whose slot it is ends (the slot is written before it is read again) and
 * when the function ends. A read that does not copy the value out cancels
 * the move: a closure capturing the slot (its CLOSURE runs after every LOCAL
 * so far), a parameter's DEFAULT_LOCAL, and a write. A module's exports are
 * reads at its end, so a const it exports is never moved before then.
 *
 * MUTATION: two more places end a read's path (docs/mutation.md, "let" and
 * "Last reads in branches"). The store of a let's new value follows its
 * value on every path, so the value's last read of the let moves
 * (store_slot). A branch of an if or a conditional does not run after
 * another: at a branch's end, the last read of each slot in it is parked
 * (c->parked_reads), fenced from the reads of the branches that follow,
 * until the construct ends; then a later read cancels it as any other, or
 * the end of the slot's block or function moves it. A branch that ends in
 * a return moves its last reads there (end_branch).
 *
 * Shapes: unless the config says no_shapes, every event goes through the
 * matcher (matcher.c) before the compiler handles it. The matcher reports a
 * function whose body is a known shape during the ARROW_END that closes it,
 * while the function is still the innermost scope here, and the compiler
 * records the shape on its sjs_function_info. The code is the same either
 * way (docs/matcher.md).
 *
 * MUTATION: rewrites. The matcher also reports, with no_shapes too, a node
 * a rewrite's rules match, during the event that ends it: a mutating method
 * called on a path (MUTATE_PATH), which the compiler then compiles as a path
 * to the container it changes (docs/matcher.md, section 12). Restore:
 * matching only unless no_shapes, as `matching` said. MUTATION: and the
 * appends to a let, `s += e` (APPEND) and `s = s + e1 + e2` (APPEND_SUM),
 * whose read of the let moves its value (docs/mutation.md, "Appending to a
 * let").
 */
#include "matcher.h"
#include "regex.h"

/* Kinds of binding. */
enum { B_LOCAL = 0, B_CONSTANT };

/* MUTATION: what the function has done with a binding's value so far, as
 * owned mutation needs to know (docs/mutation.md, "The rule"). */
enum { USE_MUTATED = 1, USE_CAPTURED = 2, USE_COPIED = 4, USE_PASSED = 8, USE_STORED = 16 };

typedef struct {
    uint32_t name_offset;
    uint32_t name_length;
    uint32_t index;        /* local slot, or constant */
    uint8_t kind;
    uint8_t use;           /* MUTATION: USE_* */
    uint32_t changed_at;   /* MUTATION: its last mutation's receiver's pc, plus one; 0 for none */
    uint8_t declared_let;  /* MUTATION: by let: statements assign to it */
    uint32_t reads;        /* MUTATION: how many times a name read it: an append's move needs one */
} binding;

/* An index from names to numbers: open addressing with linear probing. The
 * names are held in a buffer, by offset, as a buffer moves when it grows. */
typedef struct {
    uint32_t hash;
    uint32_t offset;
    uint32_t length;
    uint32_t value;        /* the number plus one; 0 marks an empty slot */
} index_slot;

typedef struct {
    sjs_buf slots;         /* index_slot, a power of two of them */
    uint32_t count;
    const sjs_buf *text;   /* the buffer the names are in */
} name_index;

/* Kinds of scope: a function's (the program's too), or a block statement's. */
enum { SC_FUNCTION = 0, SC_BLOCK };

typedef struct {
    uint8_t kind;          /* SC_* */
    uint32_t function_scope; /* the function scope this one is in: itself for a function */
    uint32_t function;     /* SC_FUNCTION: its sjs_function_info */
    uint32_t bindings_base;
    uint16_t params;       /* as ARROW_BEGIN said, a rest parameter included */
    uint16_t params_seen;
    uint16_t slots;        /* SC_FUNCTION: the next free slot */
    uint16_t slot_high;    /* SC_FUNCTION: the most slots in use at once, its locals */
    uint16_t slot_base;    /* SC_BLOCK: the function's next free slot when it began */
    uint16_t capture_count;
    uint8_t params_open;   /* between ARROW_BEGIN and PARAMS_END */
    uint8_t rest;          /* the last parameter is a rest parameter */
    /* The parameters' first read of a name before its declaration: refused
     * at PARAMS_END, so that a parameter given twice, a syntax error, comes
     * first. The name is in c->names. */
    uint8_t early_read;
    uint32_t early_read_offset;
    uint32_t early_read_length;
    sjs_position early_read_at;
    uint8_t block;         /* SC_FUNCTION: its body is a block */
    uint32_t node;         /* SC_FUNCTION: its ARROW_BEGIN's node, which a shape's report names */
    uint32_t reads_base;   /* SC_FUNCTION: its slots' entries in c->last_reads */
    uint32_t parked_base;  /* MUTATION: SC_FUNCTION: its reads in c->parked_reads */
    sjs_position at;
    name_index names;      /* its bindings, by name */
    name_index read;       /* names read in it, or in a function it holds, that it did not bind then */
} scope;

/* An entry of c->last_reads: the pc of the LOCAL that is the slot's last
 * read so far, plus one, 0 for none; MUTATION: and how many of its reads in
 * branches that have ended wait in c->parked_reads (restore: the pc alone,
 * a uint32_t). */
typedef struct {
    uint32_t pc;
    uint32_t parked;
} slot_reads;

/* MUTATION: a slot's last read in a branch that has ended (end_branch):
 * its entry in c->last_reads, the LOCAL's pc plus one (0 once it has
 * ended), and whether it is fenced from the reads of the branch that
 * follows. */
typedef struct {
    uint32_t entry;
    uint32_t pc;
    uint8_t fenced;
} parked_read;

/* A capture a function being compiled takes from the one that creates it. */
typedef struct {
    uint32_t scope;
    uint32_t binding;      /* the binding captured, where it is declared */
    uint16_t from_capture; /* the source is the creator's capture, not its local */
    uint16_t source;       /* the creator's slot or capture */
    uint16_t own;          /* the capture's index in its function */
} capture;

/* Open constructs. */
enum { K_LOGICAL = 1, K_CONDITIONAL, K_CHAIN, K_CALL, K_NEW, K_FUNCTION, K_CONST, K_TEMPLATE, K_DEFAULT, K_IF, K_BLOCK,
       K_ASSIGN /* MUTATION */ };

/* construct.branch of a K_IF: in which part of the if, or of its else-if
 * chain, the events are. KI_ELSE_IF waits for the next test's IF. */
enum { KI_CONSEQUENT = 0, KI_ELSE_IF, KI_ALTERNATIVE };

typedef struct {
    uint8_t kind;
    uint8_t method;
    uint8_t spread;
    uint8_t exported;      /* K_CONST: export what it binds */
    uint8_t branch;        /* K_IF: KI_* */
    uint32_t jump;         /* an operand to patch; K_IF: the pending JUMP_FALSY's, 0 for none */
    uint32_t jump_else;
    uint32_t depth;        /* K_CHAIN: the chain's depth */
    uint32_t jumps_base;   /* K_CHAIN, K_IF: its jumps, in c->jumps */
    uint32_t param;        /* K_DEFAULT: the parameter's slot + 1, or 0 for an element's */
    uint8_t map;           /* K_NEW: new Map, not new Set */
    /* MUTATION: K_CALL: a method that changes the variable `receiver` (a
     * binding), whether it returns its receiver, and whether it sorts. */
    uint8_t mutate;
    uint8_t returns;
    uint8_t sorts;
    uint32_t receiver;
    /* MUTATION: K_ASSIGN: its operator (0 for `=`), the binding at its
     * target's root (`receiver`), its keys, the read of its root's pc, and
     * for && || ?? the jump past the store (`jump`). */
    uint8_t assign;
    uint32_t keys;
    uint32_t root_pc;
    uint8_t declares_let;  /* MUTATION: K_CONST: a let */
    /* MUTATION: K_CALL: its node, which a rewrite's report names; the pc of
     * its callee's GET_MEMBER (0 for none); its callee is a path the call
     * holds until it ends (path.held), and what was done with its root's
     * value before the call (binding.use); and the matcher reported
     * MUTATE_PATH. */
    uint32_t node;
    uint32_t member;
    uint8_t held;
    uint8_t root_use;
    uint8_t mutate_path;
    uint8_t append;        /* MUTATION: K_ASSIGN: the matcher reported APPEND */
    uint32_t reads;        /* MUTATION: K_ASSIGN: its root's binding.reads at ASSIGN */
    /* MUTATION: K_CALL of Object.assign: AT_* (assign_argument), and where
     * its first argument's code starts. */
    uint8_t assigns;
    /* MUTATION: K_CALL of a name a method of another kind shares
     * (sjs_mutation_shared): why the call is an ordinary one where another
     * name's would be refused (FB_*, FB_NONE if it is not), for an
     * expression statement to refuse it with; `receiver` holds the
     * binding, or the global, it names. */
    uint8_t fallback;
    uint32_t target_pc;
    /* MUTATION: K_IF, K_CONDITIONAL: where the code of the branch being
     * compiled starts, and the counts of c->parked_reads when the construct
     * began and when the branch began (end_branch). */
    uint32_t branch_pc;
    uint32_t parked_base;
    uint32_t parked_mark;
} construct;

/* MUTATION: construct.assigns: a call of Object.assign before its first
 * argument ends, and one whose first argument is a variable it changes;
 * AT_FREEZING, a call of Object.freeze or Object.seal before it ends. */
enum { AT_NONE = 0, AT_PENDING, AT_VARIABLE, AT_FREEZING };

/* An array or object pattern being compiled. An object pattern keeps its
 * keys, for a rest element to leave out: the constants of the static ones,
 * and the slots that hold the computed ones. */
enum { PK_ARRAY = 0, PK_OBJECT };

typedef struct {
    uint8_t kind;
    uint32_t keys_base;    /* in pattern_keys */
    uint32_t slots_base;   /* in pattern_slots */
} pattern_frame;

/* An object literal being compiled. While `gathering`, its properties so
 * far have static keys (their constants in literal_keys, from keys_base)
 * and only their values are on the stack: OBJECT_LITERAL makes the object
 * of them all at once, at the end or before the first property that needs
 * the object to exist (a spread, a computed key, a key repeated or an array
 * index, whose order a put settles). After that, each property is put. */
#define LITERAL_GATHERED_MAX 32u   /* values held on the stack at most */

typedef struct {
    uint32_t keys_base;    /* in literal_keys */
    bool gathering;
} literal_frame;

enum {
    NS_NONE = 0, NS_MATH, NS_OBJECT, NS_NUMBER, NS_ARRAY, NS_STRING, NS_BIGINT, NS_MAP, NS_JSON,
    /* Temporal, and its types and Now, each a namespace of its own:
     * Temporal.PlainDate.from. */
    NS_TEMPORAL, NS_TEMPORAL_PLAIN_DATE, NS_TEMPORAL_PLAIN_TIME, NS_TEMPORAL_PLAIN_DATE_TIME, NS_TEMPORAL_DURATION,
    NS_TEMPORAL_INSTANT, NS_TEMPORAL_NOW,
    /* Date: Date.now, Date.parse, Date.UTC; alone, Date called (docs/date.md). */
    NS_DATE
};

/* An explicit export: resolved when it arrives, or at the end of the module. */
enum { XE_RESOLVED = 0, XE_LOCAL };

typedef struct {
    uint32_t name_offset;  /* the exported name, in names */
    uint32_t name_length;
    uint32_t local_offset; /* XE_LOCAL: the name to resolve at the end */
    uint32_t local_length;
    uint8_t state;         /* XE_* */
    uint8_t kind;          /* B_LOCAL or B_CONSTANT, once resolved */
    uint8_t string_name;   /* the exported name was a string literal */
    uint32_t index;        /* slot or constant */
    sjs_position at;
} export_entry;

/* The module of an `export *`, once per module: borrowed, as find() lends it
 * while the compiler runs. */
typedef struct {
    sjs_value exports;
    sjs_position at;
} star_source;

/* MUTATION: the instruction read_name compiled last, from `pc` to `end`,
 * and what it read: a slot of this function, a capture, a global or an
 * import (NR_*); `which` is its binding, or the global. */
enum { NR_NONE = 0, NR_OWN, NR_CAPTURE, NR_GLOBAL, NR_IMPORT };

typedef struct {
    uint32_t pc;
    uint32_t end;
    uint32_t which;
    uint8_t kind;
    sjs_position at;
} name_read;

/* MUTATION: the last mutation's last instruction (SJS_OP_MUTATE, or the
 * SJS_OP_SORTED after a sort), from `pc` to `end`, of binding `receiver`;
 * `returns`: its method returns the receiver. MUTATION: through a path
 * (PATH_CALL, or PATH_SORTED after a sort), its keys and the pc of its
 * flags. */
typedef struct {
    uint32_t pc;
    uint32_t end;
    uint32_t receiver;
    uint8_t returns;
    uint32_t keys;         /* MUTATION: through a path */
    uint32_t flags_at;
} mutation_site;

/* MUTATION: the last ordinary call of a shared name whose receiver a
 * mutation would have refused (construct.fallback), from its CALL at `pc`
 * to `end`: as a statement it is refused with that reason, FB_*, of the
 * binding or the global `which`. */
typedef struct {
    uint32_t pc;
    uint32_t end;
    uint32_t which;
    uint8_t why;
} fallback_site;

/* MUTATION: a path, which an assignment's target is: a name read, and the
 * members and indices read of it since (`a.b[i].c`), each a step: the pc
 * of its GET_MEMBER or GET_INDEX, in c->path_steps from steps_base on.
 * `end` is here() after its last instruction. */
typedef struct {
    name_read root;
    uint32_t end;
    uint32_t steps_base;
    uint8_t held;          /* MUTATION: a call's callee, kept until the call ends */
} path;

/* Names to sort as a namespace sorts its keys; rank and index break ties.
 * Rank 0: exports[index]. Rank 1: key `entry` of stars[index]'s module. */
typedef struct {
    sjs_str name;
    uint32_t rank;
    uint32_t index;
    uint32_t entry;
} sort_item;

struct sjs_compiler {
    sjs_context *context;
    sjs_goal goal;
    /* The tee before the compiler's own handling of each event: always, for
     * the rewrites (MUTATION); with no_shapes, for them alone. */
    sjs_matcher matcher;
    const sjs_str *globals;
    uint32_t global_count;
    name_index global_index;   /* the first global of each name */
    sjs_buf global_text;
    sjs_modules modules;
    sjs_error_slot error;
    bool failed;
    bool ended;
    bool finished;
    /* The program. */
    sjs_buf code;
    sjs_buf numbers;
    sjs_buf constants;
    sjs_buf functions;
    sjs_buf captures;
    sjs_buf lines;
    /* Scopes and names. */
    sjs_buf scopes;
    sjs_buf bindings;
    sjs_buf names;
    sjs_buf pending_captures;
    /* For each slot of each open function, from its reads_base: the pc of
     * the LOCAL that is its last read so far, plus one; 0 for none
     * (slot_reads). MUTATION: and the reads parked at the end of a branch
     * (parked_read), from each open function's parked_base. */
    sjs_buf last_reads;
    sjs_buf parked_reads;
    /* Constants already made: numbers by their bits, strings by their text. */
    name_index number_index;
    name_index string_index;
    sjs_buf string_text;
    /* Constructs. */
    sjs_buf constructs;
    sjs_buf jumps;
    /* A PARAM whose pattern has not started. */
    bool param_pending;
    uint32_t param_index;
    /* Patterns open: pattern_frame, with their keys (uint32_t constants) and
     * the slots of their computed keys (uint32_t). */
    sjs_buf patterns;
    sjs_buf pattern_keys;
    sjs_buf pattern_slots;
    /* Object literals open, and the constants of their gathered keys. */
    sjs_buf literals;
    sjs_buf literal_keys;
    /* Method calls: the last instruction, and the last jump target. */
    uint32_t last_op;
    bool last_op_valid;
    uint32_t last_label;
    /* `Math` and the like, waiting for their `.name`. */
    uint8_t space;
    sjs_position space_at;
    /* Export the names the next const binds. */
    bool exporting;
    /* A module's exports by name, its export * modules by name, the
     * namespaces it imported by module, and room to sort names. */
    sjs_buf exports;
    name_index export_names;
    sjs_buf stars;
    name_index star_modules;
    name_index namespaces;
    sjs_buf sorting;
    /* The first name read that is bound nowhere, reported at the end. */
    bool unbound;
    uint32_t unbound_offset;
    uint32_t unbound_length;
    sjs_position unbound_at;
    /* MUTATION: the last name read and the last mutation, for a call to
     * find its receiver; a read of a binding (plus one) whose use the next
     * event tells; and a refusal the compiler reads on after, as the parser
     * does, reported at the end unless a syntax error comes first. */
    name_read last_name;
    mutation_site last_mutation;
    fallback_site last_fallback; /* MUTATION: shared names */
    uint32_t pending_read;
    bool refusing;
    sjs_error_slot refusal;
    /* MUTATION: the paths an assignment could be to, innermost last; and a
     * path the next CALL_BEGIN's call holds. */
    sjs_buf paths;
    sjs_buf path_steps;
    bool call_holds_path;  /* MUTATION */
    /* MUTATION: the spine of + the matcher last reported as APPEND_SUM: its
     * start, the read of its root (a slot of this function: its pc and
     * binding), and the pc of its last BINARY; start UINT32_MAX for none. */
    struct {
        uint32_t start;
        uint32_t root_pc;
        uint32_t which;
        uint32_t binary_pc;
    } sum;
};

static const sjs_allocator *alloc(sjs_compiler *c) {
    return &c->context->counting;
}

static sjs_result compile_fail(sjs_compiler *c, sjs_position at, sjs_problem problem) {
    if (!c->failed) {
        sjs_error_set(&c->error, at, problem);
        c->failed = true;
    }
    return SJS_ERR_PROGRAM;
}

static sjs_result fail_name(sjs_compiler *c, const sjs_event *e, sjs_problem problem) {
    compile_fail(c, e->at, problem);
    sjs_error_subject(&c->error, e->text);
    return SJS_ERR_PROGRAM;
}

static sjs_result fail_result(sjs_compiler *c, sjs_result r, sjs_position at) {
    if (r == SJS_ERR_NOMEM) return compile_fail(c, at, (sjs_problem){ SJS_ERROR_MEMORY, "out of memory" });
    if (r == SJS_ERR_LIMIT) return compile_fail(c, at, (sjs_problem){ SJS_ERROR_LIMIT, "the program is larger than the limits allow" });
    return compile_fail(c, at, (sjs_problem){ SJS_ERROR_SYNTAX, "the program could not be compiled" });
}

static sjs_bytes bytes_of(const void *ptr, uint32_t length) {
    return (sjs_bytes){ ptr, length };
}

static sjs_result buf_add(sjs_compiler *c, sjs_buf *buf, sjs_bytes bytes) {
    return sjs_buf_append(alloc(c), buf, bytes);
}

/* ------------------------------------------------------------------------
 * Indices by name
 */

static uint32_t name_hash(sjs_str name) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < name.length; i++) {
        h ^= (uint8_t)name.ptr[i];
        h *= 16777619u;
    }
    return h;
}

static name_index index_over(const sjs_buf *text) {
    name_index x = { { 0 }, 0, text };
    return x;
}

/* The number `name` maps to, if the index holds it. */
static bool index_find(const name_index *x, sjs_str name, uint32_t *value) {
    uint32_t capacity = SJS_BUF_COUNT(x->slots, index_slot);
    if (capacity == 0) return false;
    uint32_t hash = name_hash(name);
    for (uint32_t i = hash & (capacity - 1u);; i = (i + 1u) & (capacity - 1u)) {
        const index_slot *slot = &SJS_BUF_AT(x->slots, index_slot, i);
        if (slot->value == 0) return false;
        if (slot->hash == hash && sjs_str_equal((sjs_str){ (const char *)x->text->ptr + slot->offset, slot->length }, name)) {
            *value = slot->value - 1u;
            return true;
        }
    }
}

static void index_place(sjs_buf *slots, index_slot slot) {
    uint32_t capacity = SJS_BUF_COUNT(*slots, index_slot);
    uint32_t i = slot.hash & (capacity - 1u);
    while (SJS_BUF_AT(*slots, index_slot, i).value != 0) i = (i + 1u) & (capacity - 1u);
    SJS_BUF_AT(*slots, index_slot, i) = slot;
}

/* Map the name at `slot.offset` of the index's text to `slot.value`. The
 * caller knows the index does not hold the name yet. */
static sjs_result index_put(sjs_compiler *c, name_index *x, index_slot slot) {
    uint32_t capacity = SJS_BUF_COUNT(x->slots, index_slot);
    if ((x->count + 1u) * 2u > capacity) {
        if (capacity >= (1u << 26)) return SJS_ERR_LIMIT;
        uint32_t grown = capacity == 0 ? 8u : capacity * 2u;
        sjs_buf slots = { 0 };
        sjs_result r = sjs_buf_reserve(alloc(c), &slots, grown * (uint32_t)sizeof(index_slot));
        if (r != SJS_OK) return r;
        slots.length = grown * (uint32_t)sizeof(index_slot);
        sjs_zero(slots.ptr, slots.length);
        for (uint32_t i = 0; i < capacity; i++) {
            index_slot old = SJS_BUF_AT(x->slots, index_slot, i);
            if (old.value != 0) index_place(&slots, old);
        }
        sjs_buf_free(alloc(c), &x->slots);
        x->slots = slots;
    }
    slot.hash = name_hash((sjs_str){ (const char *)x->text->ptr + slot.offset, slot.length });
    slot.value++;
    index_place(&x->slots, slot);
    x->count++;
    return SJS_OK;
}

static index_slot at_offset(uint32_t offset, uint32_t length, uint32_t value) {
    index_slot slot = { 0, offset, length, value };
    return slot;
}

static void index_free(sjs_compiler *c, name_index *x) {
    sjs_buf_free(alloc(c), &x->slots);
    x->count = 0;
}

/* Keep a copy of `name` in c->names; answers its offset. */
static sjs_result keep_name(sjs_compiler *c, sjs_str name, uint32_t *offset) {
    *offset = c->names.length;
    return buf_add(c, &c->names, bytes_of(name.ptr, name.length));
}

/* ------------------------------------------------------------------------
 * Emitting code
 */

static uint32_t here(const sjs_compiler *c) {
    return c->code.length;
}

static sjs_result mark(sjs_compiler *c, sjs_position at) {
    uint32_t n = SJS_BUF_COUNT(c->lines, sjs_line);
    if (n > 0 && SJS_BUF_AT(c->lines, sjs_line, n - 1).at.offset == at.offset) return SJS_OK;
    if (n > 0 && SJS_BUF_AT(c->lines, sjs_line, n - 1).pc == here(c)) {
        SJS_BUF_AT(c->lines, sjs_line, n - 1).at = at;
        return SJS_OK;
    }
    sjs_line line = { here(c), at };
    return buf_add(c, &c->lines, bytes_of(&line, sizeof line));
}

static sjs_result op(sjs_compiler *c, uint8_t opcode, sjs_position at) {
    sjs_result r = mark(c, at);
    if (r != SJS_OK) return fail_result(c, r, at);
    c->last_op = here(c);
    c->last_op_valid = true;
    r = buf_add(c, &c->code, bytes_of(&opcode, 1));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, at);
}

static sjs_result u8(sjs_compiler *c, uint8_t v) {
    sjs_result r = buf_add(c, &c->code, bytes_of(&v, 1));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, c->error.error.at);
}

static sjs_result u16(sjs_compiler *c, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    sjs_result r = buf_add(c, &c->code, bytes_of(b, 2));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, c->error.error.at);
}

static sjs_result u32(sjs_compiler *c, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    sjs_result r = buf_add(c, &c->code, bytes_of(b, 4));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, c->error.error.at);
}

/* An instruction with one operand, and where in the source it came from. */
typedef struct {
    uint8_t opcode;
    uint32_t operand;
    sjs_position at;
} instruction;

static instruction ins(uint8_t opcode, uint32_t operand, sjs_position at) {
    instruction i = { opcode, operand, at };
    return i;
}

static sjs_result op16(sjs_compiler *c, instruction i) {
    if (i.operand > UINT16_MAX) return compile_fail(c, i.at, (sjs_problem){ SJS_ERROR_LIMIT, "a function has more than 65535 names" });
    if (op(c, i.opcode, i.at) != SJS_OK) return SJS_ERR_PROGRAM;
    return u16(c, (uint16_t)i.operand);
}

static sjs_result op32(sjs_compiler *c, instruction i) {
    if (op(c, i.opcode, i.at) != SJS_OK) return SJS_ERR_PROGRAM;
    return u32(c, i.operand);
}

/* A jump whose target comes later; answers where its operand is. */
static sjs_result jump(sjs_compiler *c, instruction i, uint32_t *operand) {
    if (op(c, i.opcode, i.at) != SJS_OK) return SJS_ERR_PROGRAM;
    *operand = here(c);
    return u32(c, 0);
}

/* Point a jump here. */
static void land(sjs_compiler *c, uint32_t operand) {
    uint32_t target = here(c);
    uint8_t *p = c->code.ptr + operand;
    p[0] = (uint8_t)target;
    p[1] = (uint8_t)(target >> 8);
    p[2] = (uint8_t)(target >> 16);
    p[3] = (uint8_t)(target >> 24);
    c->last_label = target;
}

/* ------------------------------------------------------------------------
 * Constants
 */

/* Numbers are found by their bits, which tell 0 from -0. */
static sjs_result number_index(sjs_compiler *c, double value, uint32_t *index) {
    if (index_find(&c->number_index, (sjs_str){ (const char *)&value, sizeof value }, index)) return SJS_OK;
    *index = SJS_BUF_COUNT(c->numbers, double);
    sjs_result r = buf_add(c, &c->numbers, bytes_of(&value, sizeof value));
    if (r == SJS_OK) r = index_put(c, &c->number_index, at_offset(*index * (uint32_t)sizeof value, sizeof value, *index));
    return r;
}

static sjs_result number_constant(sjs_compiler *c, double value, sjs_position at) {
    uint32_t index;
    sjs_result r = number_index(c, value, &index);
    if (r != SJS_OK) return fail_result(c, r, at);
    return op32(c, ins(SJS_OP_NUMBER_, index, at));
}

/* The constant holding `value`; takes over the caller's reference. */
static sjs_result value_constant(sjs_compiler *c, sjs_value value, uint32_t *index) {
    *index = SJS_BUF_COUNT(c->constants, sjs_value);
    sjs_result r = buf_add(c, &c->constants, bytes_of(&value, sizeof value));
    if (r != SJS_OK) sjs_release(c->context, value);
    return r;
}

/* A BigInt literal, made once, as a constant of the program. */
static sjs_result bigint_constant(sjs_compiler *c, const sjs_event *e) {
    sjs_value value;
    sjs_result r = sjs_bigint_from_digits(c->context, (sjs_bigint_digits){ e->text, e->count, false }, &value);
    if (r == SJS_ERR_LIMIT) return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_LIMIT, "a BigInt has more bits than max_bigint_bits" });
    uint32_t index;
    if (r == SJS_OK) r = value_constant(c, value, &index);
    if (r != SJS_OK) return fail_result(c, r, e->at);
    return op32(c, ins(SJS_OP_CONSTANT, index, e->at));
}

/* A regular expression literal, compiled once, as a constant of the
 * program (docs/regex.md, 7.1): no program can tell one value from a new
 * object per evaluation, as === on objects faults and lastIndex never
 * changes. */
static sjs_result regex_constant(sjs_compiler *c, const sjs_event *e) {
    sjs_value value;
    sjs_result r = sjs_regex_compile(c->context, (sjs_regex_literal){ e->text, e->count }, &value);
    if (r == SJS_ERR_LIMIT) {
        return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_LIMIT, "a regular expression needs more states than max_regex_states" });
    }
    if (r == SJS_ERR_PROGRAM) return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_SYNTAX, "the regular expression is not one staticjs compiles" });
    uint32_t index;
    if (r == SJS_OK) r = value_constant(c, value, &index);
    if (r != SJS_OK) return fail_result(c, r, e->at);
    return op32(c, ins(SJS_OP_CONSTANT, index, e->at));
}

static sjs_result string_constant(sjs_compiler *c, const sjs_event *e, uint32_t *index) {
    sjs_str text = e->text;
    sjs_position at = e->at;
    if (index_find(&c->string_index, text, index)) return SJS_OK;
    sjs_string *s;
    sjs_result r = sjs_string_intern(c->context, text, &s);
    if (r == SJS_OK) {
        /* A key constant knows its prototype names and methods from now. */
        s->h.flags |= SJS_STRING_NAMED;
        s->h.name_id = sjs_name_id(text);
        r = value_constant(c, sjs_heap_value(SJS_STRING, &s->h), index);
    }
    uint32_t offset = c->string_text.length;
    if (r == SJS_OK) r = buf_add(c, &c->string_text, bytes_of(text.ptr, text.length));
    if (r == SJS_OK) r = index_put(c, &c->string_index, at_offset(offset, text.length, *index));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, at);
}

/* ------------------------------------------------------------------------
 * Scopes
 */

static scope *current(sjs_compiler *c) {
    return &SJS_BUF_AT(c->scopes, scope, SJS_BUF_COUNT(c->scopes, scope) - 1);
}

static uint32_t scope_count(const sjs_compiler *c) {
    return SJS_BUF_COUNT(c->scopes, scope);
}

/* The function scope the current scope is in. */
static scope *function_scope(sjs_compiler *c) {
    return &SJS_BUF_AT(c->scopes, scope, current(c)->function_scope);
}

static sjs_result open_function_scope(sjs_compiler *c, sjs_position at) {
    sjs_function_info info = { 0 };
    scope s = { 0 };
    s.kind = SC_FUNCTION;
    s.function_scope = scope_count(c);
    s.function = SJS_BUF_COUNT(c->functions, sjs_function_info);
    s.bindings_base = SJS_BUF_COUNT(c->bindings, binding);
    s.reads_base = SJS_BUF_COUNT(c->last_reads, slot_reads); /* MUTATION: restore uint32_t */
    s.parked_base = SJS_BUF_COUNT(c->parked_reads, parked_read); /* MUTATION */
    s.at = at;
    s.names = index_over(&c->names);
    s.read = index_over(&c->names);
    info.code = here(c);
    sjs_result r = buf_add(c, &c->functions, bytes_of(&info, sizeof info));
    if (r == SJS_OK) r = buf_add(c, &c->scopes, bytes_of(&s, sizeof s));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, at);
}

/* A block statement's scope: its names of its own, in the slots after the
 * ones its function uses when it begins. */
static sjs_result open_block_scope(sjs_compiler *c, sjs_position at) {
    scope s = { 0 };
    s.kind = SC_BLOCK;
    s.function_scope = current(c)->function_scope;
    s.bindings_base = SJS_BUF_COUNT(c->bindings, binding);
    s.slot_base = function_scope(c)->slots;
    s.at = at;
    s.names = index_over(&c->names);
    s.read = index_over(&c->names);
    sjs_result r = buf_add(c, &c->scopes, bytes_of(&s, sizeof s));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, at);
}

/* ------------------------------------------------------------------------
 * Last reads: the last LOCAL of a slot becomes a TAKE_LOCAL, which moves
 * the value out instead of copying it (see the header).
 */

/* Room in c->last_reads for the current function's slots below `slots`. */
static sjs_result reserve_reads(sjs_compiler *c, uint32_t slots, sjs_position at) {
    uint32_t want = function_scope(c)->reads_base + slots;
    uint32_t have = SJS_BUF_COUNT(c->last_reads, slot_reads); /* MUTATION: restore uint32_t */
    if (want <= have) return SJS_OK;
    uint32_t extra = (want - have) * (uint32_t)sizeof(slot_reads); /* MUTATION: restore uint32_t */
    sjs_result r = sjs_buf_reserve(alloc(c), &c->last_reads, extra);
    if (r != SJS_OK) return fail_result(c, r, at);
    sjs_zero(c->last_reads.ptr + c->last_reads.length, extra);
    c->last_reads.length += extra;
    return SJS_OK;
}

/* MUTATION: a slot's entry; restore last_read returning its uint32_t. */
static slot_reads *slot_entry(sjs_compiler *c, uint32_t function_scope_index, uint32_t slot) {
    return &SJS_BUF_AT(c->last_reads, slot_reads, SJS_BUF_AT(c->scopes, scope, function_scope_index).reads_base + slot);
}

static uint32_t *last_read(sjs_compiler *c, uint32_t function_scope_index, uint32_t slot) {
    return &slot_entry(c, function_scope_index, slot)->pc; /* MUTATION: restore the entry itself */
}

/* The LOCAL at `entry` - 1 moves its value. */
static void take_read(sjs_compiler *c, uint32_t entry) {
    if (entry != 0 && c->code.ptr[entry - 1u] == SJS_OP_LOCAL) c->code.ptr[entry - 1u] = SJS_OP_TAKE_LOCAL;
}

/* MUTATION: reads in a branch that has ended (docs/mutation.md, "Last
 * reads in branches"): what ends a slot's parked reads. A read, a capture
 * or a write cancels every one that is not fenced (PARKED_CANCEL); the end
 * of the slot's block or function moves every one (PARKED_SETTLE); a store
 * of a let's value moves the ones in its value, from `from`, and cancels
 * the others not fenced (PARKED_STORE); the end of a branch in a return
 * moves the ones in it, from `from`, and keeps the others (PARKED_RETURN). */
enum { PARKED_CANCEL = 0, PARKED_SETTLE, PARKED_STORE, PARKED_RETURN };

typedef struct {
    uint32_t function_scope;
    uint32_t slot;
    uint8_t how;           /* PARKED_* */
    uint32_t from;
} parked_end;

static void end_parked(sjs_compiler *c, parked_end e) {
    slot_reads *entry = slot_entry(c, e.function_scope, e.slot);
    const scope *f = &SJS_BUF_AT(c->scopes, scope, e.function_scope);
    uint32_t key = f->reads_base + e.slot;
    for (uint32_t i = SJS_BUF_COUNT(c->parked_reads, parked_read); entry->parked != 0 && i-- > f->parked_base;) {
        parked_read *p = &SJS_BUF_AT(c->parked_reads, parked_read, i);
        if (p->pc == 0 || p->entry != key || (p->fenced && e.how != PARKED_SETTLE)) continue;
        bool in = e.how == PARKED_SETTLE || ((e.how == PARKED_STORE || e.how == PARKED_RETURN) && p->pc > e.from);
        if (!in && e.how == PARKED_RETURN) continue;
        if (in) take_read(c, p->pc);
        p->pc = 0;
        entry->parked--;
    }
}

/* MUTATION: the parked reads from `from` on are fenced from the reads that
 * follow (`fenced`), or no longer (the construct ends). */
static void fence_parked(sjs_compiler *c, uint32_t from, uint8_t fenced) {
    uint32_t n = SJS_BUF_COUNT(c->parked_reads, parked_read);
    for (uint32_t i = from; i < n; i++) SJS_BUF_AT(c->parked_reads, parked_read, i).fenced = fenced;
}

/* Read a slot of the current function: a LOCAL, the slot's last read so far. */
static sjs_result read_slot(sjs_compiler *c, uint32_t slot, sjs_position at) {
    if (op16(c, ins(SJS_OP_LOCAL, slot, at)) != SJS_OK) return SJS_ERR_PROGRAM;
    end_parked(c, (parked_end){ current(c)->function_scope, slot, PARKED_CANCEL, 0 }); /* MUTATION */
    *last_read(c, current(c)->function_scope, slot) = c->last_op + 1u;
    return SJS_OK;
}

/* Something else reads the slot after its last LOCAL so far (a closure
 * capturing it, a DEFAULT_LOCAL), or writes it: that LOCAL stays a copy. */
static void forget_read(sjs_compiler *c, uint32_t function_scope_index, uint32_t slot) {
    end_parked(c, (parked_end){ function_scope_index, slot, PARKED_CANCEL, 0 }); /* MUTATION */
    *last_read(c, function_scope_index, slot) = 0;
}

/* Slots of one function, from `from` up to `to`. */
typedef struct {
    uint32_t function_scope;
    uint32_t from;
    uint32_t to;
} slot_range;

/* The slots are not read again: each one's last LOCAL moves its value. */
static void settle_reads(sjs_compiler *c, slot_range range) {
    for (uint32_t slot = range.from; slot < range.to; slot++) {
        uint32_t *entry = last_read(c, range.function_scope, slot);
        take_read(c, *entry);
        *entry = 0;
        end_parked(c, (parked_end){ range.function_scope, slot, PARKED_SETTLE, 0 }); /* MUTATION */
    }
}

/* SET_LOCAL or PATTERN_COMPUTED: a write of a slot of the current function. */
static sjs_result write_slot(sjs_compiler *c, instruction i) {
    forget_read(c, current(c)->function_scope, i.operand);
    return op16(c, i);
}

/* MUTATION: a let's new value is stored (variable_store), and its code
 * starts at `from`: every last read of the slot in it is the last of the
 * old value on every path, as the store follows it with nothing between
 * that reads the slot, so it moves; an earlier read stays a copy, as a
 * path that skips the store may read the slot again (docs/mutation.md,
 * "let"). */
static sjs_result store_slot(sjs_compiler *c, instruction i, uint32_t from) {
    uint32_t *entry = last_read(c, current(c)->function_scope, i.operand);
    if (*entry > from) take_read(c, *entry);
    *entry = 0;
    end_parked(c, (parked_end){ current(c)->function_scope, i.operand, PARKED_STORE, from });
    return op16(c, i);
}

/* MUTATION: a branch of an if or a conditional ends here; its code starts
 * at `from`. When it ends in a return, every last read in it is the last
 * on every path through it, and moves. When another branch follows, which
 * does not run after this one, its last reads wait, parked, until what
 * follows the whole construct; they and the reads parked by the
 * constructs in it, from `fence` on, are fenced from the next branch's. */
typedef struct {
    uint32_t from;
    uint32_t fence;
    bool returns;
} branch_end;

static sjs_result end_branch(sjs_compiler *c, branch_end b, sjs_position at) {
    uint32_t from = b.from;
    bool returns = b.returns;
    uint32_t function_scope_index = current(c)->function_scope;
    uint32_t base = function_scope(c)->reads_base;
    uint32_t slots = SJS_BUF_COUNT(c->last_reads, slot_reads) - base;
    for (uint32_t slot = 0; slot < slots; slot++) {
        slot_reads *entry = slot_entry(c, function_scope_index, slot);
        if (returns) end_parked(c, (parked_end){ function_scope_index, slot, PARKED_RETURN, from });
        if (entry->pc <= from) continue;
        if (returns) {
            take_read(c, entry->pc);
        } else {
            parked_read p = { base + slot, entry->pc, 0 };
            sjs_result r = buf_add(c, &c->parked_reads, bytes_of(&p, sizeof p));
            if (r != SJS_OK) return fail_result(c, r, at);
            entry = slot_entry(c, function_scope_index, slot);
            entry->parked++;
        }
        entry->pc = 0;
    }
    if (!returns) fence_parked(c, b.fence, 1);
    return SJS_OK;
}

/* The block ends: its names go, and its slots are free for what follows.
 * The names read in it stay noted in the scopes around it, as a function's
 * do. No pending capture refers to its bindings: a capture is taken by a
 * function inside the block, which has ended before it. */
static void close_block_scope(sjs_compiler *c) {
    scope s = *current(c);
    /* Its slots are written before they are read again. */
    settle_reads(c, (slot_range){ s.function_scope, s.slot_base, function_scope(c)->slots });
    c->bindings.length = s.bindings_base * (uint32_t)sizeof(binding);
    index_free(c, &current(c)->names);
    index_free(c, &current(c)->read);
    c->scopes.length -= (uint32_t)sizeof(scope);
    SJS_BUF_AT(c->scopes, scope, s.function_scope).slots = s.slot_base;
}

static bool reserved_binding(sjs_str name) {
    return sjs_str_equal(name, SJS_STR("undefined")) || sjs_str_equal(name, SJS_STR("NaN")) ||
           sjs_str_equal(name, SJS_STR("Infinity"));
}

/* What a name is bound to: a local slot or a constant. */
typedef struct {
    uint8_t kind;
    uint32_t index;
} target;

/* An explicit export. Two of one name are a syntax error, found here, as
 * export * never gives a name an explicit export gives. */
static sjs_result add_export(sjs_compiler *c, const export_entry *entry, sjs_str name) {
    uint32_t seen;
    if (index_find(&c->export_names, name, &seen)) {
        compile_fail(c, entry->at, (sjs_problem){ SJS_ERROR_SYNTAX, "this name is exported twice" });
        sjs_error_subject(&c->error, name);
        return SJS_ERR_PROGRAM;
    }
    export_entry x = *entry;
    x.name_length = name.length;
    uint32_t index = SJS_BUF_COUNT(c->exports, export_entry);
    sjs_result r = keep_name(c, name, &x.name_offset);
    if (r == SJS_OK) r = buf_add(c, &c->exports, bytes_of(&x, sizeof x));
    if (r == SJS_OK) r = index_put(c, &c->export_names, at_offset(x.name_offset, x.name_length, index));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, entry->at);
}

static sjs_result fail_early_read(sjs_compiler *c, sjs_position at, sjs_str name) {
    compile_fail(c, at, (sjs_problem){ SJS_ERROR_UNSUPPORTED,
                                       "this name is used before its declaration or import; staticjs functions cannot call themselves" });
    sjs_error_subject(&c->error, name);
    return SJS_ERR_PROGRAM;
}

/* Bind `e->text` in the current scope. */
static sjs_result declare(sjs_compiler *c, const sjs_event *e, target to) {
    uint8_t kind = to.kind;
    uint32_t index = to.index;
    uint32_t scope_index = scope_count(c) - 1;
    if (reserved_binding(e->text)) return fail_name(c, e, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs does not rebind this name" });
    uint32_t seen;
    if (index_find(&current(c)->names, e->text, &seen)) return fail_name(c, e, (sjs_problem){ SJS_ERROR_SYNTAX, "this name is declared twice" });
    /* A name read in this scope before now, here or in a function it holds,
     * meant something else then: JavaScript would find this binding. */
    if (index_find(&current(c)->read, e->text, &seen)) {
        if (!current(c)->params_open) return fail_early_read(c, e->at, e->text);
        if (!current(c)->early_read) {
            uint32_t offset;
            sjs_result r = keep_name(c, e->text, &offset);
            if (r != SJS_OK) return fail_result(c, r, e->at);
            scope *s = current(c);
            s->early_read = 1;
            s->early_read_offset = offset;
            s->early_read_length = e->text.length;
            s->early_read_at = e->at;
        }
    }
    binding b = { 0 };
    b.name_length = e->text.length;
    b.index = index;
    b.kind = kind;
    uint32_t which = SJS_BUF_COUNT(c->bindings, binding);
    /* MUTATION: a name a let binds. */
    uint32_t constructs = SJS_BUF_COUNT(c->constructs, construct);
    const construct *declaring = constructs == 0 ? NULL : &SJS_BUF_AT(c->constructs, construct, constructs - 1);
    b.declared_let = declaring != NULL && declaring->kind == K_CONST ? declaring->declares_let : 0u;
    sjs_result r = keep_name(c, e->text, &b.name_offset);
    if (r == SJS_OK) r = buf_add(c, &c->bindings, bytes_of(&b, sizeof b));
    if (r == SJS_OK) r = index_put(c, &current(c)->names, at_offset(b.name_offset, b.name_length, which));
    if (r != SJS_OK) return fail_result(c, r, e->at);
    uint32_t open = SJS_BUF_COUNT(c->constructs, construct);
    const construct *k = open == 0 ? NULL : &SJS_BUF_AT(c->constructs, construct, open - 1);
    if (k != NULL && k->kind == K_CONST && k->exported && scope_index == 0) {
        export_entry x = { 0 };
        x.state = XE_RESOLVED;
        x.kind = kind;
        x.index = index;
        x.at = e->at;
        return add_export(c, &x, e->text);
    }
    return SJS_OK;
}

/* A slot of the current function, the first free one: a block's slots are
 * free again after it. */
static sjs_result new_slot(sjs_compiler *c, sjs_position at, uint32_t *slot) {
    scope *s = function_scope(c);
    if (s->slots == UINT16_MAX) return compile_fail(c, at, (sjs_problem){ SJS_ERROR_LIMIT, "a function has more than 65535 names" });
    *slot = s->slots++;
    if (s->slots > s->slot_high) s->slot_high = s->slots;
    return reserve_reads(c, s->slots, at);
}

/* `e->text` was read in scope `scope_index`, or a function it holds, and
 * the scope does not bind it (yet). */
static sjs_result note_outer_ref(sjs_compiler *c, uint32_t scope_index, const sjs_event *e) {
    uint32_t seen;
    if (index_find(&SJS_BUF_AT(c->scopes, scope, scope_index).read, e->text, &seen)) return SJS_OK;
    uint32_t offset;
    sjs_result r = keep_name(c, e->text, &offset);
    if (r == SJS_OK) r = index_put(c, &SJS_BUF_AT(c->scopes, scope, scope_index).read, at_offset(offset, e->text.length, 0));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, e->at);
}

/* The capture of made->binding in scope made->scope, made if it is new. */
static sjs_result find_capture(sjs_compiler *c, capture *made) {
    uint32_t s = made->scope;
    uint32_t which = made->binding;
    uint32_t n = SJS_BUF_COUNT(c->pending_captures, capture);
    for (uint32_t i = 0; i < n; i++) {
        capture *k = &SJS_BUF_AT(c->pending_captures, capture, i);
        if (k->scope == s && k->binding == which) {
            *made = *k;
            return SJS_OK;
        }
    }
    scope *sc = &SJS_BUF_AT(c->scopes, scope, s);
    if (sc->capture_count == UINT16_MAX) return SJS_ERR_LIMIT;
    made->scope = s;
    made->binding = which;
    made->own = sc->capture_count++;
    return buf_add(c, &c->pending_captures, bytes_of(made, sizeof *made));
}

/* MUTATION: owned mutation (docs/mutation.md). Everything from here to
 * "MUTATION ends" exists only for it. */

/* Refuse the program, as unsupported, and read on: the compiler takes no
 * more events but the END, where it reports this (on_event), so that a
 * syntax error the parser finds later is still the program's error. */
typedef struct {
    const char *message;
    sjs_str subject;
} refusal;

static sjs_result refuse_later(sjs_compiler *c, sjs_position at, refusal why) {
    if (c->refusing) return SJS_OK;
    sjs_error_set(&c->refusal, at, (sjs_problem){ SJS_ERROR_UNSUPPORTED, why.message });
    sjs_error_subject(&c->refusal, why.subject);
    c->refusing = true;
    return SJS_OK;
}

static sjs_str binding_name(const sjs_compiler *c, uint32_t which) {
    const binding *b = &SJS_BUF_AT(c->bindings, binding, which);
    return (sjs_str){ (const char *)c->names.ptr + b->name_offset, b->name_length };
}

static const char *const refused_captured =
    "staticjs changes a variable's value only while the variable holds it alone, and a function captured this one: build the value with reduce instead";

static const char *const refused_copied =
    "staticjs changes a variable's value only while the variable holds it alone, and this one was copied before";
static const char *const refused_passed =
    "staticjs changes a variable's value only while the variable holds it alone, and this one was passed to a function before";
static const char *const refused_stored =
    "staticjs changes a variable's value only while the variable holds it alone, and this one was stored in an array or an object before";

/* Why a mutation of the binding is refused, from what was done with it
 * before; NULL when nothing was. */
static const char *refused_use(uint8_t use) {
    if ((use & USE_CAPTURED) != 0) return refused_captured;
    if ((use & USE_COPIED) != 0) return refused_copied;
    if ((use & USE_PASSED) != 0) return refused_passed;
    if ((use & USE_STORED) != 0) return refused_stored;
    return NULL;
}

/* A binding of an enclosing function is captured: refused if it was
 * changed, and noted, so that a later change is refused. */
static const char *const refused_let_capture =
    "a function captures this let variable, and a function keeps a copy of what it captures: declare it with const, or build the value with reduce";

static sjs_result note_capture(sjs_compiler *c, uint32_t which, sjs_position at) {
    binding *b = &SJS_BUF_AT(c->bindings, binding, which);
    /* A capture copies the value, and JavaScript's closure would see what
     * is assigned to the variable later. */
    if (b->declared_let) return refuse_later(c, at, (refusal){ refused_let_capture, binding_name(c, which) });
    b->use |= USE_CAPTURED;
    if ((b->use & USE_MUTATED) != 0) return refuse_later(c, at, (refusal){ refused_captured, binding_name(c, which) });
    return SJS_OK;
}

/* The event after a read of a binding of this function says what the read
 * does with its value: one it may keep is noted. Every other event keeps
 * nothing: a member, a spread, a pattern, an operator, a test, a return,
 * or an operand that an index or an operator consumes with the value. */
static void note_use(sjs_compiler *c, uint32_t kind) {
    binding *b = &SJS_BUF_AT(c->bindings, binding, c->pending_read - 1u);
    c->pending_read = 0;
    switch (kind) {
    case SJS_EV_ARGUMENT:
        b->use |= USE_PASSED;
        break;
    case SJS_EV_ELEMENT:
    case SJS_EV_PROPERTY:
        b->use |= USE_STORED;
        break;
    case SJS_EV_BIND:
    case SJS_EV_DEFAULT_END:
    case SJS_EV_EXPORT_DEFAULT:
    case SJS_EV_LOGICAL:
    case SJS_EV_LOGICAL_END:
    case SJS_EV_CONDITIONAL_ELSE:
    case SJS_EV_CONDITIONAL_END:
    case SJS_EV_ASSIGN_END:
        b->use |= USE_COPIED;
        break;
    default:
        break;
    }
}

static void note_read(sjs_compiler *c, name_read read) {
    read.end = here(c);
    c->last_name = read;
}

/* Paths (the type's comment). The parser sends an assignment's target as
 * it sends any read, and ASSIGN after it, so the compiler keeps the paths
 * that could be one: a name read begins a path, a MEMBER or an INDEX of it
 * goes on with it, and any other event whose node starts where the path
 * does ends it, as that node holds the path (sjs_event.start: an
 * operator's, a member's, an index's or a call's node starts with its first
 * operand). The paths read in an index's key are inside it, and end with
 * it. */
static path *top_path(sjs_compiler *c) {
    uint32_t n = SJS_BUF_COUNT(c->paths, path);
    return n == 0 ? NULL : &SJS_BUF_AT(c->paths, path, n - 1u);
}

static void drop_paths(sjs_compiler *c, uint32_t from) {
    if (from >= SJS_BUF_COUNT(c->paths, path)) return;
    c->path_steps.length = SJS_BUF_AT(c->paths, path, from).steps_base * (uint32_t)sizeof(uint32_t);
    c->paths.length = from * (uint32_t)sizeof(path);
}

/* The name read_name has just read begins a path. */
static sjs_result path_begin(sjs_compiler *c) {
    path p = { c->last_name, here(c), SJS_BUF_COUNT(c->path_steps, uint32_t), 0 }; /* MUTATION: held */
    sjs_result r = buf_add(c, &c->paths, bytes_of(&p, sizeof p));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, p.root.at);
}

/* MUTATION: a path whose last step is the GET_MEMBER just compiled, after
 * one step or more: a call of it is a method called on a path. */
static bool path_callee(const sjs_compiler *c, const path *p) {
    uint32_t steps = SJS_BUF_COUNT(c->path_steps, uint32_t) - p->steps_base;
    return steps >= 2u && p->end == here(c) && c->last_op_valid &&
           SJS_BUF_AT(c->path_steps, uint32_t, p->steps_base + steps - 1u) == c->last_op &&
           c->code.ptr[c->last_op] == SJS_OP_GET_MEMBER;
}

/* Before an event: the paths whose root is at or after where its node
 * starts end, but one that a MEMBER or an INDEX goes on with. ASSIGN and
 * UPDATE take theirs from the top. A CALL_BEGIN of a method on the path on
 * top holds it until its CALL_END, so that a rewrite reported then finds
 * its steps (MUTATION, "Methods through a path"). */
static void paths_before(sjs_compiler *c, const sjs_event *e) {
    if (e->kind == SJS_EV_ASSIGN || e->kind == SJS_EV_UPDATE || e->kind == SJS_EV_DELETE) return; /* MUTATION: DELETE, step 3 */
    bool goes_on = e->kind == SJS_EV_MEMBER || e->kind == SJS_EV_INDEX;
    uint32_t count = SJS_BUF_COUNT(c->paths, path); /* MUTATION: the top, for a call */
    uint32_t n = count;
    while (n > 0) {
        path *p = &SJS_BUF_AT(c->paths, path, n - 1u);
        uint32_t root = p->root.at.offset;
        if (root < e->start.offset || (goes_on && root == e->start.offset) || p->held) break;
        /* MUTATION: a method called on a path. */
        if (e->kind == SJS_EV_CALL_BEGIN && n == count && root == e->start.offset && path_callee(c, p)) {
            p->held = 1;
            c->call_holds_path = true;
            break;
        }
        n--;
    }
    drop_paths(c, n);
}

/* The GET_MEMBER or GET_INDEX at `pc`, just compiled for `e`, is a step of
 * the path on top if its node starts at the path's root; for a member, if
 * nothing came between them either. Otherwise that path ends. */
static sjs_result path_step(sjs_compiler *c, const sjs_event *e, uint32_t pc) {
    path *p = top_path(c);
    if (p == NULL || p->root.at.offset != e->start.offset) return SJS_OK;
    if (e->kind == SJS_EV_MEMBER && p->end != pc) {
        drop_paths(c, SJS_BUF_COUNT(c->paths, path) - 1u);
        return SJS_OK;
    }
    p->end = here(c);
    sjs_result r = buf_add(c, &c->path_steps, bytes_of(&pc, sizeof pc));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, e->at);
}

/* MUTATION ends. */

/* Read a name: a local, a capture, a constant, a global or a built-in. The
 * name is noted as read in every scope it is looked for in, blocks
 * included, so that a later declaration there is refused. */
static sjs_result read_name(sjs_compiler *c, const sjs_event *e) {
    uint32_t count = scope_count(c);
    uint32_t inner = count - 1;
    /* MUTATION: what is read, for a call to find its receiver. */
    name_read read = { here(c), 0, 0, NR_NONE, e->at };
    for (uint32_t s = count; s-- > 0;) {
        uint32_t i;
        if (index_find(&SJS_BUF_AT(c->scopes, scope, s).names, e->text, &i)) {
            binding *b = &SJS_BUF_AT(c->bindings, binding, i);
            read.which = i;
            if (b->kind == B_CONSTANT) {
                if (op32(c, ins(SJS_OP_CONSTANT, b->index, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
                read.kind = NR_IMPORT; /* MUTATION */
                note_read(c, read);
                return path_begin(c);
            }
            uint32_t owner = SJS_BUF_AT(c->scopes, scope, s).function_scope;
            b->reads++; /* MUTATION: appending to a let */
            if (owner == current(c)->function_scope) {
                if (read_slot(c, b->index, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
                /* MUTATION: the next event tells what the read does. */
                read.kind = NR_OWN;
                note_read(c, read);
                c->pending_read = i + 1u;
                return path_begin(c);
            }
            /* MUTATION */
            if (note_capture(c, i, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
            read.kind = NR_CAPTURE;
            /* Captured: through every function between. The CLOSURE that
             * makes the outermost of them reads the slot, after any LOCAL of
             * it emitted so far, as its function emits nothing until then. */
            forget_read(c, owner, b->index);
            uint16_t from_capture = 0;
            uint32_t index = b->index;
            for (uint32_t t = s + 1; t <= inner; t++) {
                if (SJS_BUF_AT(c->scopes, scope, t).kind != SC_FUNCTION) continue;
                capture made = { 0 };
                made.scope = t;
                made.binding = i;
                made.from_capture = from_capture;
                made.source = (uint16_t)index;
                sjs_result r = find_capture(c, &made);
                if (r != SJS_OK) return fail_result(c, r, e->at);
                from_capture = 1;
                index = made.own;
            }
            if (op16(c, ins(SJS_OP_CAPTURE, index, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
            note_read(c, read); /* MUTATION */
            return path_begin(c);
        }
        if (note_outer_ref(c, s, e) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    uint32_t global;
    if (index_find(&c->global_index, e->text, &global)) {
        if (op16(c, ins(SJS_OP_GLOBAL, global, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
        /* MUTATION */
        read.kind = NR_GLOBAL;
        read.which = global;
        note_read(c, read);
        return path_begin(c);
    }
    if (sjs_str_equal(e->text, SJS_STR("undefined"))) return op(c, SJS_OP_UNDEFINED_, e->at);
    if (sjs_str_equal(e->text, SJS_STR("NaN"))) return number_constant(c, 0.0 / 0.0, e->at);
    if (sjs_str_equal(e->text, SJS_STR("Infinity"))) return number_constant(c, 1.0 / 0.0, e->at);
    static const struct { const char *name; uint8_t space; } spaces[] = {
        { "Math", NS_MATH }, { "Object", NS_OBJECT }, { "Number", NS_NUMBER }, { "Array", NS_ARRAY },
        { "String", NS_STRING }, { "BigInt", NS_BIGINT }, { "Map", NS_MAP }, { "JSON", NS_JSON },
        { "Temporal", NS_TEMPORAL }, { "Date", NS_DATE }
    };
    for (uint32_t i = 0; i < sizeof spaces / sizeof spaces[0]; i++) {
        if (sjs_str_equal(e->text, sjs_cstr(spaces[i].name))) {
            c->space = spaces[i].space;
            c->space_at = e->at;
            return SJS_OK;
        }
    }
    sjs_builtin_id id = sjs_builtin_lookup(SJS_STR(""), e->text);
    if (id != SJS_BUILTIN_NONE) {
        uint32_t index;
        if (value_constant(c, sjs_builtin_value(id), &index) != SJS_OK) return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_MEMORY, "out of memory" });
        return op32(c, ins(SJS_OP_CONSTANT, index, e->at));
    }
    /* RegExp is JavaScript's, bound in every program: patterns made when the
     * program runs are left out (docs/regex.md, 12.4). */
    if (sjs_str_equal(e->text, SJS_STR("RegExp"))) {
        return fail_name(c, e, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no RegExp: write a regular expression literal" });
    }
    /* Bound nowhere: reported at the end, after any syntax error. */
    if (!c->unbound) {
        c->unbound = true;
        c->unbound_offset = c->names.length;
        c->unbound_length = e->text.length;
        c->unbound_at = e->at;
        sjs_result r = buf_add(c, &c->names, bytes_of(e->text.ptr, e->text.length));
        if (r != SJS_OK) return fail_result(c, r, e->at);
    }
    return op(c, SJS_OP_UNDEFINED_, e->at);
}

static sjs_result fail_unbound(sjs_compiler *c) {
    compile_fail(c, c->unbound_at, (sjs_problem){ SJS_ERROR_NAME, "this name is not bound (a const is visible after its declaration)" });
    sjs_error_subject(&c->error, (sjs_str){ (const char *)c->names.ptr + c->unbound_offset, c->unbound_length });
    return SJS_ERR_PROGRAM;
}

/* `Math.PI`, `Number.MAX_SAFE_INTEGER`, `Math.max`. */
static sjs_result read_space_member(sjs_compiler *c, const sjs_event *e) {
    static const char *const space_names[] = {
        "", "Math", "Object", "Number", "Array", "String", "BigInt", "Map", "JSON",
        "Temporal", "Temporal.PlainDate", "Temporal.PlainTime", "Temporal.PlainDateTime", "Temporal.Duration",
        "Temporal.Instant", "Temporal.Now", "Date"
    };
    static const struct { uint8_t space; const char *name; double value; } numbers[] = {
        { NS_MATH, "PI", 3.141592653589793 }, { NS_MATH, "E", 2.718281828459045 },
        { NS_MATH, "LN2", 0.6931471805599453 }, { NS_MATH, "LN10", 2.302585092994046 },
        { NS_MATH, "LOG2E", 1.4426950408889634 }, { NS_MATH, "LOG10E", 0.4342944819032518 },
        { NS_MATH, "SQRT2", 1.4142135623730951 }, { NS_MATH, "SQRT1_2", 0.7071067811865476 },
        { NS_NUMBER, "MAX_SAFE_INTEGER", 9007199254740991.0 }, { NS_NUMBER, "MIN_SAFE_INTEGER", -9007199254740991.0 },
        { NS_NUMBER, "EPSILON", 2.220446049250313e-16 }, { NS_NUMBER, "MAX_VALUE", 1.7976931348623157e308 },
        { NS_NUMBER, "MIN_VALUE", 5e-324 }
    };
    uint8_t space = c->space;
    c->space = NS_NONE;
    if (space == NS_TEMPORAL) {
        /* Temporal.PlainDate, ...: a namespace whose members follow. */
        for (uint8_t inner = NS_TEMPORAL_PLAIN_DATE; inner <= NS_TEMPORAL_NOW; inner++) {
            if (sjs_str_equal(e->text, sjs_cstr(space_names[inner] + 9))) {
                c->space = inner;
                return SJS_OK;
            }
        }
    }
    for (uint32_t i = 0; i < sizeof numbers / sizeof numbers[0]; i++) {
        if (numbers[i].space == space && sjs_str_equal(e->text, sjs_cstr(numbers[i].name))) {
            return number_constant(c, numbers[i].value, e->at);
        }
    }
    if (space == NS_NUMBER) {
        if (sjs_str_equal(e->text, SJS_STR("POSITIVE_INFINITY"))) return number_constant(c, 1.0 / 0.0, e->at);
        if (sjs_str_equal(e->text, SJS_STR("NEGATIVE_INFINITY"))) return number_constant(c, -1.0 / 0.0, e->at);
        if (sjs_str_equal(e->text, SJS_STR("NaN"))) return number_constant(c, 0.0 / 0.0, e->at);
    }
    sjs_builtin_id id = sjs_builtin_lookup(sjs_cstr(space_names[space]), e->text);
    if (id == SJS_BUILTIN_NONE) return fail_name(c, e, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs does not have this built-in" });
    uint32_t index;
    if (value_constant(c, sjs_builtin_value(id), &index) != SJS_OK) return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_MEMORY, "out of memory" });
    return op32(c, ins(SJS_OP_CONSTANT, index, e->at));
}

/* ------------------------------------------------------------------------
 * Constructs
 */

static sjs_result push_construct(sjs_compiler *c, construct k, sjs_position at) {
    if (SJS_BUF_COUNT(c->constructs, construct) >= c->context->limits.max_nesting) {
        return compile_fail(c, at, (sjs_problem){ SJS_ERROR_LIMIT, "the program nests deeper than max_nesting" });
    }
    sjs_result r = buf_add(c, &c->constructs, bytes_of(&k, sizeof k));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, at);
}

static construct *top_construct(sjs_compiler *c) {
    uint32_t n = SJS_BUF_COUNT(c->constructs, construct);
    return n == 0 ? NULL : &SJS_BUF_AT(c->constructs, construct, n - 1);
}

static construct pop_construct(sjs_compiler *c) {
    construct k = *top_construct(c);
    c->constructs.length -= (uint32_t)sizeof(construct);
    return k;
}

static sjs_result expect_construct(sjs_compiler *c, uint8_t kind, sjs_position at) {
    construct *k = top_construct(c);
    if (k == NULL || k->kind != kind) return compile_fail(c, at, (sjs_problem){ SJS_ERROR_SYNTAX, "the events are out of order" });
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * Functions
 */

/* ARROW_BEGIN: its parameters take the first slots. */
static sjs_result begin_function(sjs_compiler *c, const sjs_event *e) {
    sjs_position at = e->at;
    if (e->count > UINT16_MAX) return compile_fail(c, at, (sjs_problem){ SJS_ERROR_LIMIT, "a function has too many parameters" });
    construct k = { 0 };
    k.kind = K_FUNCTION;
    if (jump(c, ins(SJS_OP_JUMP, 0, at), &k.jump) != SJS_OK) return SJS_ERR_PROGRAM;
    if (push_construct(c, k, at) != SJS_OK) return SJS_ERR_PROGRAM;
    if (open_function_scope(c, at) != SJS_OK) return SJS_ERR_PROGRAM;
    scope *s = current(c);
    s->node = e->node;
    s->params = (uint16_t)e->count;
    s->slots = (uint16_t)e->count;
    s->slot_high = (uint16_t)e->count;
    s->params_open = 1;
    return reserve_reads(c, e->count, at);
}

/* Close the innermost function: its captures go to the program's table. */
static sjs_result close_function_scope(sjs_compiler *c, sjs_position at) {
    scope s = *current(c);
    uint32_t scope_index = scope_count(c) - 1;
    /* The function's code is complete: each slot's last LOCAL moves. */
    settle_reads(c, (slot_range){ scope_index, 0, s.slot_high });
    c->last_reads.length = s.reads_base * (uint32_t)sizeof(slot_reads); /* MUTATION: restore uint32_t */
    c->parked_reads.length = s.parked_base * (uint32_t)sizeof(parked_read); /* MUTATION */
    sjs_function_info *info = &SJS_BUF_AT(c->functions, sjs_function_info, s.function);
    info->params = s.params;
    info->locals = s.slot_high;
    info->flags = s.rest ? SJS_FUNCTION_REST : 0u;
    info->capture_count = s.capture_count;
    info->captures = SJS_BUF_COUNT(c->captures, sjs_capture_info);
    uint32_t n = SJS_BUF_COUNT(c->pending_captures, capture);
    sjs_capture_info none = { 0, 0 };
    for (uint16_t i = 0; i < s.capture_count; i++) {
        sjs_result r = buf_add(c, &c->captures, bytes_of(&none, sizeof none));
        if (r != SJS_OK) return fail_result(c, r, at);
    }
    for (uint32_t i = 0; i < n; i++) {
        capture *k = &SJS_BUF_AT(c->pending_captures, capture, i);
        if (k->scope != scope_index) continue;
        sjs_capture_info *ci = &SJS_BUF_AT(c->captures, sjs_capture_info, info->captures + k->own);
        ci->from_capture = k->from_capture;
        ci->index = k->source;
    }
    /* Drop this scope's captures, keeping the others in order. */
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        capture k = SJS_BUF_AT(c->pending_captures, capture, i);
        if (k.scope == scope_index) continue;
        SJS_BUF_AT(c->pending_captures, capture, kept++) = k;
    }
    c->pending_captures.length = kept * (uint32_t)sizeof(capture);
    c->bindings.length = s.bindings_base * (uint32_t)sizeof(binding);
    /* The names this function read stay noted in the scopes around it: a
     * later declaration there would change what it read. */
    index_free(c, &current(c)->names);
    index_free(c, &current(c)->read);
    c->scopes.length -= (uint32_t)sizeof(scope);
    return SJS_OK;
}

static sjs_result end_function(sjs_compiler *c, sjs_position at) {
    scope *s = current(c);
    if (s->kind != SC_FUNCTION || s->params_open || scope_count(c) < 2) {
        return compile_fail(c, at, (sjs_problem){ SJS_ERROR_SYNTAX, "the events are out of order" });
    }
    if (expect_construct(c, K_FUNCTION, at) != SJS_OK) return SJS_ERR_PROGRAM;
    if (!s->block) {
        if (op(c, SJS_OP_RETURN, at) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    uint32_t function = current(c)->function;
    if (close_function_scope(c, at) != SJS_OK) return SJS_ERR_PROGRAM;
    construct k = pop_construct(c);
    land(c, k.jump);
    return op32(c, ins(SJS_OP_CLOSURE, function, at));
}

/* ------------------------------------------------------------------------
 * Patterns: each takes the value on top of the stack, or a parameter
 */

static sjs_result out_of_order(sjs_compiler *c, sjs_position at) {
    return compile_fail(c, at, (sjs_problem){ SJS_ERROR_SYNTAX, "the events are out of order" });
}

/* PARAM and REST_PARAM: the next parameter, whose pattern follows. */
static sjs_result param_event(sjs_compiler *c, const sjs_event *e) {
    scope *s = current(c);
    bool rest = e->kind == SJS_EV_REST_PARAM;
    /* A rest parameter is the last: the parser finds one that is not before
     * PARAMS_END, so only a parameter after it is out of order here. */
    if (!s->params_open || c->param_pending || s->rest || e->count != s->params_seen || e->count >= s->params) {
        return out_of_order(c, e->at);
    }
    s->params_seen++;
    if (rest) s->rest = 1;
    c->param_pending = true;
    c->param_index = e->count;
    return SJS_OK;
}

static sjs_result params_end(sjs_compiler *c, const sjs_event *e) {
    scope *s = current(c);
    if (!s->params_open || c->param_pending || s->params_seen != s->params) return out_of_order(c, e->at);
    if (expect_construct(c, K_FUNCTION, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    s->params_open = 0;
    if (s->early_read) {
        return fail_early_read(c, s->early_read_at, (sjs_str){ (const char *)c->names.ptr + s->early_read_offset, s->early_read_length });
    }
    return SJS_OK;
}

/* DEFAULT: a parameter's default runs when its slot is undefined; an
 * element's or a property's, when the value on the stack is, which it
 * replaces. The parameter state is kept across the default, whose arrow
 * functions have parameters of their own. */
static sjs_result default_begin(sjs_compiler *c, const sjs_event *e) {
    construct k = { 0 };
    k.kind = K_DEFAULT;
    if (c->param_pending) {
        c->param_pending = false;
        k.param = c->param_index + 1u;
        /* A guard, unreachable today: nothing reads a parameter before its
         * default, as a read there of the parameter itself, or of a later
         * one, is refused (patterns.spec, "before its declaration"). It
         * keeps a LOCAL of the slot from moving should that change. */
        forget_read(c, current(c)->function_scope, c->param_index);
        if (op16(c, ins(SJS_OP_DEFAULT_LOCAL, c->param_index, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
        k.jump = here(c);
        if (u32(c, 0) != SJS_OK) return SJS_ERR_PROGRAM;
    } else if (jump(c, ins(SJS_OP_DEFAULT, 0, e->at), &k.jump) != SJS_OK) {
        return SJS_ERR_PROGRAM;
    }
    return push_construct(c, k, e->at);
}

static sjs_result default_end(sjs_compiler *c, const sjs_event *e) {
    if (expect_construct(c, K_DEFAULT, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    construct k = pop_construct(c);
    if (k.param != 0 && write_slot(c, ins(SJS_OP_SET_LOCAL, k.param - 1u, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
    land(c, k.jump);
    if (k.param != 0) {
        /* The parameter's pattern follows. */
        c->param_pending = true;
        c->param_index = k.param - 1u;
    }
    return SJS_OK;
}

/* A pattern begins on a parameter: put it on the stack. */
static sjs_result take_param(sjs_compiler *c, sjs_position at) {
    if (!c->param_pending) return SJS_OK;
    c->param_pending = false;
    return read_slot(c, c->param_index, at);
}

static pattern_frame *top_pattern(sjs_compiler *c) {
    uint32_t n = SJS_BUF_COUNT(c->patterns, pattern_frame);
    return n == 0 ? NULL : &SJS_BUF_AT(c->patterns, pattern_frame, n - 1);
}

static sjs_result open_pattern(sjs_compiler *c, const sjs_event *e, uint8_t kind) {
    if (take_param(c, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    pattern_frame f = { kind, SJS_BUF_COUNT(c->pattern_keys, uint32_t), SJS_BUF_COUNT(c->pattern_slots, uint32_t) };
    sjs_result r = buf_add(c, &c->patterns, bytes_of(&f, sizeof f));
    if (r != SJS_OK) return fail_result(c, r, e->at);
    return op(c, kind == PK_ARRAY ? SJS_OP_PATTERN_ARRAY : SJS_OP_PATTERN_OBJECT, e->at);
}

static sjs_result close_pattern_frame(sjs_compiler *c, const sjs_event *e, uint8_t kind) {
    pattern_frame *f = top_pattern(c);
    if (f == NULL || f->kind != kind) return out_of_order(c, e->at);
    c->pattern_keys.length = f->keys_base * (uint32_t)sizeof(uint32_t);
    c->pattern_slots.length = f->slots_base * (uint32_t)sizeof(uint32_t);
    c->patterns.length -= (uint32_t)sizeof(pattern_frame);
    /* The source. */
    return op(c, SJS_OP_POP, e->at);
}

/* Is the innermost pattern of this kind? */
static bool in_pattern(sjs_compiler *c, uint8_t kind) {
    pattern_frame *f = top_pattern(c);
    return f != NULL && f->kind == kind;
}

static sjs_result pattern_key(sjs_compiler *c, const sjs_event *e) {
    if (!in_pattern(c, PK_OBJECT)) return out_of_order(c, e->at);
    uint32_t index;
    if (string_constant(c, e, &index) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_result r = buf_add(c, &c->pattern_keys, bytes_of(&index, sizeof index));
    if (r != SJS_OK) return fail_result(c, r, e->at);
    return op32(c, ins(SJS_OP_PATTERN_KEY, index, e->at));
}

/* `[key]: target`: the key, made a string, waits in a slot of its own for
 * a rest element to leave it out. */
static sjs_result pattern_computed_key(sjs_compiler *c, const sjs_event *e) {
    if (!in_pattern(c, PK_OBJECT)) return out_of_order(c, e->at);
    uint32_t slot;
    if (new_slot(c, e->at, &slot) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_result r = buf_add(c, &c->pattern_slots, bytes_of(&slot, sizeof slot));
    if (r != SJS_OK) return fail_result(c, r, e->at);
    return write_slot(c, ins(SJS_OP_PATTERN_COMPUTED, slot, e->at));
}

/* Constants `keys[base..]`, keys, as one array constant; UINT32_MAX for none. */
typedef struct {
    const sjs_buf *keys;
    uint32_t base;
} key_list;

static sjs_result keys_constant(sjs_compiler *c, key_list list, uint32_t *index) {
    uint32_t n = SJS_BUF_COUNT(*list.keys, uint32_t) - list.base;
    *index = UINT32_MAX;
    if (n == 0) return SJS_OK;
    sjs_array *keys;
    sjs_result r = sjs_array_make(c->context, n, &keys);
    if (r != SJS_OK) return r;
    for (uint32_t i = 0; i < n; i++) {
        sjs_value key = SJS_BUF_AT(c->constants, sjs_value, SJS_BUF_AT(*list.keys, uint32_t, list.base + i));
        sjs_retain(key);
        keys->items[keys->length++] = key;
    }
    return value_constant(c, sjs_heap_value(SJS_ARRAY, &keys->h), index);
}

/* `...rest`: in an array, the elements from its index on; in an object, the
 * properties its keys do not name. */
static sjs_result pattern_rest(sjs_compiler *c, const sjs_event *e) {
    pattern_frame *f = top_pattern(c);
    if (f == NULL) return out_of_order(c, e->at);
    if (f->kind == PK_ARRAY) return op32(c, ins(SJS_OP_PATTERN_REST, e->count, e->at));
    pattern_frame frame = *f;
    uint32_t computed = SJS_BUF_COUNT(c->pattern_slots, uint32_t) - frame.slots_base;
    if (computed > UINT16_MAX) return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_LIMIT, "a pattern has too many computed keys" });
    for (uint32_t i = 0; i < computed; i++) {
        if (read_slot(c, SJS_BUF_AT(c->pattern_slots, uint32_t, frame.slots_base + i), e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    uint32_t keys;
    sjs_result r = keys_constant(c, (key_list){ &c->pattern_keys, frame.keys_base }, &keys);
    if (r != SJS_OK) return fail_result(c, r, e->at);
    if (op32(c, ins(SJS_OP_PATTERN_REST_OBJECT, keys, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
    return u16(c, (uint16_t)computed);
}

static sjs_result pattern_event(sjs_compiler *c, const sjs_event *e) {
    switch (e->kind) {
    case SJS_EV_PARAM:
    case SJS_EV_REST_PARAM:
        return param_event(c, e);
    case SJS_EV_BIND: {
        if (c->param_pending) {
            /* A plain parameter is its own slot. */
            c->param_pending = false;
            return declare(c, e, (target){ B_LOCAL, c->param_index });
        }
        uint32_t slot;
        if (new_slot(c, e->at, &slot) != SJS_OK) return SJS_ERR_PROGRAM;
        if (write_slot(c, ins(SJS_OP_SET_LOCAL, slot, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
        return declare(c, e, (target){ B_LOCAL, slot });
    }
    case SJS_EV_ARRAY_PATTERN_BEGIN:
        return open_pattern(c, e, PK_ARRAY);
    case SJS_EV_OBJECT_PATTERN_BEGIN:
        return open_pattern(c, e, PK_OBJECT);
    case SJS_EV_PATTERN_INDEX:
        if (!in_pattern(c, PK_ARRAY)) return out_of_order(c, e->at);
        return op32(c, ins(SJS_OP_PATTERN_INDEX, e->count, e->at));
    case SJS_EV_PATTERN_KEY:
        return pattern_key(c, e);
    case SJS_EV_PATTERN_COMPUTED_KEY:
        return pattern_computed_key(c, e);
    case SJS_EV_PATTERN_REST:
        return pattern_rest(c, e);
    case SJS_EV_ARRAY_PATTERN_END:
        return close_pattern_frame(c, e, PK_ARRAY);
    default: /* SJS_EV_OBJECT_PATTERN_END */
        return close_pattern_frame(c, e, PK_OBJECT);
    }
}

static bool is_pattern_event(uint32_t kind) {
    return kind == SJS_EV_PARAM || kind == SJS_EV_REST_PARAM || kind == SJS_EV_BIND || kind == SJS_EV_ARRAY_PATTERN_BEGIN ||
           kind == SJS_EV_ARRAY_PATTERN_END || kind == SJS_EV_OBJECT_PATTERN_BEGIN || kind == SJS_EV_OBJECT_PATTERN_END ||
           kind == SJS_EV_PATTERN_INDEX || kind == SJS_EV_PATTERN_KEY || kind == SJS_EV_PATTERN_COMPUTED_KEY ||
           kind == SJS_EV_PATTERN_REST;
}

/* ------------------------------------------------------------------------
 * Modules
 */

static bool at_module_level(sjs_compiler *c, const sjs_event *e) {
    if (c->goal == SJS_GOAL_MODULE && scope_count(c) == 1 && SJS_BUF_COUNT(c->constructs, construct) == 0) return true;
    compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_SYNTAX, "the events are out of order" });
    return false;
}

/* The exports of the module `e->text`. */
static sjs_result module_exports(sjs_compiler *c, const sjs_event *e, sjs_value *exports) {
    if (c->modules.v == NULL || c->modules.v->find == NULL) {
        return fail_name(c, e, (sjs_problem){ SJS_ERROR_NAME, "there are no modules to import from" });
    }
    if (c->modules.v->find(c->modules.self, e->text, exports) != SJS_OK || exports->kind != SJS_OBJECT || exports->internal != 0) {
        return fail_name(c, e, (sjs_problem){ SJS_ERROR_NAME, "there is no module of this name" });
    }
    return SJS_OK;
}

static sjs_str entry_key(const sjs_object *o, uint32_t i) {
    return sjs_string_view(o->entries[i].key);
}

/* A namespace orders its keys as strings, which an object cannot do for
 * array indices; and an object cannot hold __proto__. */
static sjs_result namespace_keys(sjs_compiler *c, const sjs_event *e, sjs_value exports) {
    const sjs_object *o = SJS_AS_OBJECT(exports);
    for (uint32_t i = 0; i < o->count; i++) {
        if ((o->entries[i].key->h.flags & SJS_STRING_INDEX) != 0) {
            return fail_name(c, e, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "a namespace cannot hold this module's keys in order: one is an array index" });
        }
        if (sjs_str_equal(entry_key(o, i), SJS_STR("__proto__"))) {
            return fail_name(c, e, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "a namespace cannot hold this module's key __proto__: staticjs objects have no prototype" });
        }
    }
    return SJS_OK;
}

/* Does `a` come before `b`? By UTF-16 code units, then rank, then index. */
static bool sorts_before(const sort_item *a, const sort_item *b) {
    int32_t order = sjs_utf16_compare(a->name, b->name);
    if (order != 0) return order < 0;
    if (a->rank != b->rank) return a->rank < b->rank;
    return a->index < b->index;
}

static void sift_down(sort_item *items, uint32_t root, uint32_t count) {
    for (;;) {
        uint32_t child = 2u * root + 1u;
        if (child >= count) return;
        if (child + 1u < count && sorts_before(&items[child], &items[child + 1u])) child++;
        if (!sorts_before(&items[root], &items[child])) return;
        sort_item swap = items[root];
        items[root] = items[child];
        items[child] = swap;
        root = child;
    }
}

/* Heap sort: in place, without recursion, and total, so deterministic. */
static void sort_names(sort_item *items, uint32_t count) {
    for (uint32_t i = count / 2u; i-- > 0;) sift_down(items, i, count);
    for (uint32_t end = count; end-- > 1u;) {
        sort_item swap = items[0];
        items[0] = items[end];
        items[end] = swap;
        sift_down(items, 0, end);
    }
}

static sjs_result add_sort_item(sjs_compiler *c, sort_item item) {
    return buf_add(c, &c->sorting, bytes_of(&item, sizeof item));
}

/* A copy of `o` with its keys sorted. Its keys are known to be distinct and
 * none an array index, so the entries go in as they are. */
static sjs_result sorted_copy(sjs_compiler *c, const sjs_object *o, sjs_value *out) {
    c->sorting.length = 0;
    sjs_result r = SJS_OK;
    for (uint32_t i = 0; i < o->count && r == SJS_OK; i++) r = add_sort_item(c, (sort_item){ entry_key(o, i), 0, i, 0 });
    if (r != SJS_OK) return r;
    const sort_item *items = (const sort_item *)(const void *)c->sorting.ptr;
    sort_names((sort_item *)(void *)c->sorting.ptr, o->count);
    sjs_object *copy;
    r = sjs_object_make(c->context, o->count, &copy);
    if (r != SJS_OK) return r;
    for (uint32_t i = 0; i < o->count; i++) {
        const sjs_entry *entry = &o->entries[items[i].index];
        copy->entries[i] = *entry;
        sjs_retain(sjs_heap_value(SJS_STRING, &entry->key->h));
        sjs_retain(entry->value);
    }
    copy->count = o->count;
    *out = sjs_heap_value(SJS_OBJECT, &copy->h);
    return SJS_OK;
}

/* The module's exports as a namespace, once per module: the same object when
 * its keys are in order already, as every staticjs module's are; a sorted
 * copy if not. */
static sjs_result namespace_constant(sjs_compiler *c, const sjs_event *e, uint32_t *index) {
    if (index_find(&c->namespaces, e->text, index)) return SJS_OK;
    sjs_value exports;
    if (module_exports(c, e, &exports) != SJS_OK) return SJS_ERR_PROGRAM;
    if (namespace_keys(c, e, exports) != SJS_OK) return SJS_ERR_PROGRAM;
    const sjs_object *o = SJS_AS_OBJECT(exports);
    bool sorted = true;
    for (uint32_t i = 1; i < o->count && sorted; i++) sorted = sjs_utf16_compare(entry_key(o, i - 1), entry_key(o, i)) < 0;
    sjs_value value = exports;
    sjs_result r = SJS_OK;
    if (sorted) sjs_retain(exports);
    else r = sorted_copy(c, o, &value);
    if (r == SJS_OK) r = value_constant(c, value, index);
    uint32_t offset;
    if (r == SJS_OK) r = keep_name(c, e->text, &offset);
    if (r == SJS_OK) r = index_put(c, &c->namespaces, at_offset(offset, e->text.length, *index));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, e->at);
}

/* The value of the export `e->alias` of the module `e->text`, as a constant. */
static sjs_result export_constant(sjs_compiler *c, const sjs_event *e, uint32_t *index) {
    sjs_value exports;
    if (module_exports(c, e, &exports) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_value value;
    if (sjs_object_get(exports, e->alias, &value) != SJS_OK) {
        sjs_event named = *e;
        named.text = e->alias;
        return fail_name(c, &named, (sjs_problem){ SJS_ERROR_NAME, "the module does not export this name" });
    }
    sjs_retain(value);
    if (value_constant(c, value, index) != SJS_OK) return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_MEMORY, "out of memory" });
    return SJS_OK;
}

static sjs_result import(sjs_compiler *c, const sjs_event *e) {
    if (!at_module_level(c, e)) return SJS_ERR_PROGRAM;
    sjs_event local = *e;
    local.text = e->name;
    uint32_t index;
    switch (e->count) {
    case SJS_IMPORT_NAME:
        if (export_constant(c, e, &index) != SJS_OK) return SJS_ERR_PROGRAM;
        return declare(c, &local, (target){ B_CONSTANT, index });
    case SJS_IMPORT_NAMESPACE:
        if (namespace_constant(c, e, &index) != SJS_OK) return SJS_ERR_PROGRAM;
        return declare(c, &local, (target){ B_CONSTANT, index });
    case SJS_IMPORT_MODULE: {
        sjs_value exports;
        return module_exports(c, e, &exports);
    }
    default:
        return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_SYNTAX, "the events are out of order" });
    }
}

/* `export * from`: every export but default, unless an explicit export of
 * the same name hides it. The module's keys are read at the end, once
 * however often it is named. */
static sjs_result export_star(sjs_compiler *c, const sjs_event *e) {
    uint32_t seen;
    if (index_find(&c->star_modules, e->text, &seen)) return SJS_OK;
    sjs_value exports;
    if (module_exports(c, e, &exports) != SJS_OK) return SJS_ERR_PROGRAM;
    if (namespace_keys(c, e, exports) != SJS_OK) return SJS_ERR_PROGRAM;
    star_source source = { exports, e->at };
    uint32_t index = SJS_BUF_COUNT(c->stars, star_source);
    uint32_t offset;
    sjs_result r = keep_name(c, e->text, &offset);
    if (r == SJS_OK) r = buf_add(c, &c->stars, bytes_of(&source, sizeof source));
    if (r == SJS_OK) r = index_put(c, &c->star_modules, at_offset(offset, e->text.length, index));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, e->at);
}

static sjs_result export_from(sjs_compiler *c, const sjs_event *e) {
    if (!at_module_level(c, e)) return SJS_ERR_PROGRAM;
    export_entry x = { 0 };
    x.state = XE_RESOLVED;
    x.kind = B_CONSTANT;
    x.at = e->at;
    switch (e->count) {
    case SJS_IMPORT_NAME:
        if (export_constant(c, e, &x.index) != SJS_OK) return SJS_ERR_PROGRAM;
        return add_export(c, &x, e->name);
    case SJS_IMPORT_NAMESPACE:
        if (namespace_constant(c, e, &x.index) != SJS_OK) return SJS_ERR_PROGRAM;
        return add_export(c, &x, e->name);
    case SJS_IMPORT_STAR:
        return export_star(c, e);
    case SJS_IMPORT_MODULE: {
        sjs_value exports;
        return module_exports(c, e, &exports);
    }
    default:
        return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_SYNTAX, "the events are out of order" });
    }
}

/* `export { alias as name }`: resolved at the end, as `alias` may be declared later. */
static sjs_result export_local(sjs_compiler *c, const sjs_event *e) {
    if (!at_module_level(c, e)) return SJS_ERR_PROGRAM;
    export_entry x = { 0 };
    x.state = XE_LOCAL;
    x.string_name = e->count != 0 ? 1u : 0u;
    x.local_length = e->alias.length;
    x.at = e->at;
    sjs_result r = keep_name(c, e->alias, &x.local_offset);
    if (r != SJS_OK) return fail_result(c, r, e->at);
    return add_export(c, &x, e->name);
}

/* `export default value`: the value goes in a slot of its own. */
static sjs_result export_default(sjs_compiler *c, const sjs_event *e) {
    if (!at_module_level(c, e)) return SJS_ERR_PROGRAM;
    export_entry x = { 0 };
    x.state = XE_RESOLVED;
    x.kind = B_LOCAL;
    x.at = e->at;
    if (new_slot(c, e->at, &x.index) != SJS_OK) return SJS_ERR_PROGRAM;
    if (write_slot(c, ins(SJS_OP_SET_LOCAL, x.index, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
    return add_export(c, &x, SJS_STR("default"));
}

static sjs_str names_at(const sjs_compiler *c, uint32_t offset, uint32_t length) {
    return (sjs_str){ (const char *)c->names.ptr + offset, length };
}

static export_entry *export_at(sjs_compiler *c, uint32_t i) {
    return &SJS_BUF_AT(c->exports, export_entry, i);
}

/* Resolve `export { a }` against the module's own declarations and imports;
 * never a global or a built-in. */
static sjs_result resolve_locals(sjs_compiler *c) {
    uint32_t n = SJS_BUF_COUNT(c->exports, export_entry);
    const name_index *module_names = &SJS_BUF_AT(c->scopes, scope, 0).names;
    for (uint32_t i = 0; i < n; i++) {
        export_entry *x = export_at(c, i);
        if (x->state != XE_LOCAL) continue;
        sjs_str local = names_at(c, x->local_offset, x->local_length);
        uint32_t found;
        if (!index_find(module_names, local, &found)) {
            compile_fail(c, x->at, (sjs_problem){ SJS_ERROR_SYNTAX, "this name is exported but the module does not declare it" });
            sjs_error_subject(&c->error, local);
            return SJS_ERR_PROGRAM;
        }
        const binding *b = &SJS_BUF_AT(c->bindings, binding, found);
        x->state = XE_RESOLVED;
        x->kind = b->kind;
        x->index = b->index;
    }
    return SJS_OK;
}

/* A refusal decided before the module's value is built. */
typedef struct {
    bool found;
    sjs_position at;
    const char *message;
    sjs_str subject;
} deferred;

static void defer(deferred *d, sjs_position at, const char *message) {
    if (d->found) return;
    d->found = true;
    d->at = at;
    d->message = message;
}

/* Sort the explicit exports, and every key of each export * module but
 * default and the explicit ones, by name; keep one per name. A name only
 * export * gives, from two modules, is ambiguous: refused, as staticjs cannot
 * tell whether the two are one binding. Leaves the kept ones in `sorting`,
 * and answers how many. */
static sjs_result settle_exports(sjs_compiler *c, deferred *ambiguous, uint32_t *kept) {
    uint32_t n = SJS_BUF_COUNT(c->exports, export_entry);
    c->sorting.length = 0;
    sjs_result r = SJS_OK;
    for (uint32_t i = 0; i < n && r == SJS_OK; i++) {
        const export_entry *x = export_at(c, i);
        r = add_sort_item(c, (sort_item){ names_at(c, x->name_offset, x->name_length), 0, i, 0 });
    }
    uint32_t stars = SJS_BUF_COUNT(c->stars, star_source);
    for (uint32_t s = 0; s < stars && r == SJS_OK; s++) {
        const sjs_object *o = SJS_AS_OBJECT(SJS_BUF_AT(c->stars, star_source, s).exports);
        for (uint32_t k = 0; k < o->count && r == SJS_OK; k++) {
            sjs_str key = entry_key(o, k);
            uint32_t explicit;
            if (sjs_str_equal(key, SJS_STR("default")) || index_find(&c->export_names, key, &explicit)) continue;
            r = add_sort_item(c, (sort_item){ key, 1, s, k });
        }
    }
    if (r != SJS_OK) return fail_result(c, r, c->error.error.at);
    sort_item *items = (sort_item *)(void *)c->sorting.ptr;
    uint32_t count = SJS_BUF_COUNT(c->sorting, sort_item);
    sort_names(items, count);
    *kept = 0;
    for (uint32_t start = 0, end = 0; start < count; start = end) {
        for (end = start + 1; end < count && sjs_str_equal(items[end].name, items[start].name); end++) {}
        /* An explicit export is alone in its group, and stars of one module
         * are one star_source. */
        if (end - start > 1 && !ambiguous->found) {
            ambiguous->subject = items[start].name;
            defer(ambiguous, SJS_BUF_AT(c->stars, star_source, items[start + 1].index).at,
                  "two export * give this name; staticjs cannot tell whether they are one binding");
        }
        items[(*kept)++] = items[start];
    }
    return SJS_OK;
}

static sjs_result fail_deferred(sjs_compiler *c, const deferred *d) {
    compile_fail(c, d->at, (sjs_problem){ SJS_ERROR_UNSUPPORTED, d->message });
    sjs_error_subject(&c->error, d->subject);
    return SJS_ERR_PROGRAM;
}

/* Push the value of a kept export. */
static sjs_result export_value(sjs_compiler *c, const sort_item *item, sjs_position at) {
    if (item->rank == 1) {
        const sjs_object *o = SJS_AS_OBJECT(SJS_BUF_AT(c->stars, star_source, item->index).exports);
        sjs_value value = o->entries[item->entry].value;
        sjs_retain(value);
        uint32_t index;
        if (value_constant(c, value, &index) != SJS_OK) return compile_fail(c, at, (sjs_problem){ SJS_ERROR_MEMORY, "out of memory" });
        return op32(c, ins(SJS_OP_CONSTANT, index, at));
    }
    const export_entry *x = export_at(c, item->index);
    if (x->kind == B_CONSTANT) return op32(c, ins(SJS_OP_CONSTANT, x->index, at));
    return read_slot(c, x->index, at);
}

/* A module's value: its namespace. Syntax errors come first, as JavaScript
 * finds them when it parses; then a name bound nowhere; then what staticjs
 * leaves out. This orders only what is found here, at the end: an error
 * found earlier has stopped the compiler already. */
static sjs_result module_end(sjs_compiler *c, sjs_position at) {
    if (resolve_locals(c) != SJS_OK) return SJS_ERR_PROGRAM;
    if (c->unbound) return fail_unbound(c);
    deferred refused = { 0 };
    uint32_t kept;
    if (settle_exports(c, &refused, &kept) != SJS_OK) return SJS_ERR_PROGRAM;
    const sort_item *items = (const sort_item *)(const void *)c->sorting.ptr;
    deferred names = { 0 };
    for (uint32_t i = 0; i < kept && !names.found; i++) {
        if (items[i].rank != 0) continue;
        const export_entry *x = export_at(c, items[i].index);
        names.subject = items[i].name;
        if (x->string_name) defer(&names, x->at, "staticjs names exports with names, not strings");
        else if (sjs_str_equal(items[i].name, SJS_STR("__proto__"))) defer(&names, x->at, "staticjs objects have no prototype, so __proto__ cannot be exported");
    }
    if (names.found) return fail_deferred(c, &names);
    if (refused.found) return fail_deferred(c, &refused);
    if (op(c, SJS_OP_OBJECT_NEW, at) != SJS_OK) return SJS_ERR_PROGRAM;
    for (uint32_t i = 0; i < kept; i++) {
        /* Neither `sorting` nor `names`, which the names point into, grows below. */
        sjs_event name = { 0 };
        name.text = items[i].name;
        name.at = at;
        uint32_t index;
        if (string_constant(c, &name, &index) != SJS_OK) return SJS_ERR_PROGRAM;
        if (op32(c, ins(SJS_OP_CONSTANT, index, at)) != SJS_OK) return SJS_ERR_PROGRAM;
        if (export_value(c, &items[i], at) != SJS_OK) return SJS_ERR_PROGRAM;
        if (op(c, SJS_OP_OBJECT_PUT, at) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    return op(c, SJS_OP_RETURN, at);
}

/* ------------------------------------------------------------------------
 * Object literals
 */

static literal_frame *top_literal(sjs_compiler *c) {
    uint32_t n = SJS_BUF_COUNT(c->literals, literal_frame);
    return n == 0 ? NULL : &SJS_BUF_AT(c->literals, literal_frame, n - 1);
}

static sjs_result object_begin(sjs_compiler *c, const sjs_event *e) {
    literal_frame f = { SJS_BUF_COUNT(c->literal_keys, uint32_t), true };
    sjs_result r = buf_add(c, &c->literals, bytes_of(&f, sizeof f));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, e->at);
}

/* Make the object of the keys gathered so far (SJS_LITERAL_* flags), and
 * put the literal's later properties. */
static sjs_result object_make(sjs_compiler *c, sjs_position at, uint8_t flags) {
    literal_frame *f = top_literal(c);
    if (f == NULL) return out_of_order(c, at);
    if (!f->gathering) return SJS_OK;
    uint32_t keys;
    sjs_result r = keys_constant(c, (key_list){ &c->literal_keys, f->keys_base }, &keys);
    if (r != SJS_OK) return fail_result(c, r, at);
    f->gathering = false;
    c->literal_keys.length = f->keys_base * (uint32_t)sizeof(uint32_t);
    if (op32(c, ins(SJS_OP_OBJECT_LITERAL, keys, at)) != SJS_OK) return SJS_ERR_PROGRAM;
    return u8(c, flags);
}

/* Can the key, a constant, join the keys gathered? */
static bool gathers(sjs_compiler *c, const literal_frame *f, uint32_t index) {
    uint32_t n = SJS_BUF_COUNT(c->literal_keys, uint32_t);
    if (n - f->keys_base >= LITERAL_GATHERED_MAX) return false;
    const sjs_string *key = SJS_AS_STRING(SJS_BUF_AT(c->constants, sjs_value, index));
    /* An array index goes before the other keys, in order: a put places it. */
    if ((key->h.flags & SJS_STRING_INDEX) != 0) return false;
    /* The parser refuses this key; a put would fault on it. */
    if (sjs_str_equal(sjs_string_view(key), SJS_STR("__proto__"))) return false;
    /* Constants are one per text, so a repeated key has the same one. */
    for (uint32_t i = f->keys_base; i < n; i++) {
        if (SJS_BUF_AT(c->literal_keys, uint32_t, i) == index) return false;
    }
    return true;
}

static sjs_result object_key(sjs_compiler *c, const sjs_event *e) {
    literal_frame *f = top_literal(c);
    if (f == NULL) return out_of_order(c, e->at);
    uint32_t index;
    if (string_constant(c, e, &index) != SJS_OK) return SJS_ERR_PROGRAM;
    if (f->gathering && gathers(c, f, index)) {
        sjs_result r = buf_add(c, &c->literal_keys, bytes_of(&index, sizeof index));
        return r == SJS_OK ? SJS_OK : fail_result(c, r, e->at);
    }
    if (object_make(c, e->at, SJS_LITERAL_OPEN) != SJS_OK) return SJS_ERR_PROGRAM;
    return op32(c, ins(SJS_OP_CONSTANT, index, e->at));
}

static sjs_result object_property(sjs_compiler *c, const sjs_event *e) {
    literal_frame *f = top_literal(c);
    if (f == NULL) return out_of_order(c, e->at);
    /* A gathered value stays on the stack until the object is made. */
    if (f->gathering) return SJS_OK;
    return op(c, SJS_OP_OBJECT_PUT, e->at);
}

static sjs_result object_end(sjs_compiler *c, const sjs_event *e) {
    if (object_make(c, e->at, 0) != SJS_OK) return SJS_ERR_PROGRAM;
    c->literals.length -= (uint32_t)sizeof(literal_frame);
    c->last_op_valid = false;
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * Events
 */

/* MUTATION: refusals, named once, so that a shared name's fallback can
 * keep one as a byte (fallback_of). */
static const char *const refused_global = "staticjs changes only a value a variable of the function holds, and this is a global";
static const char *const refused_import = "staticjs changes only a value a variable of the function holds, and this is an import";
static const char *const refused_path_changing =
    "staticjs does not change a variable with one of its methods while it calls a method on a path from it";

/* MUTATION: shared names: why a call fell back to an ordinary one
 * (construct.fallback), kept as a byte. */
enum { FB_NONE = 0, FB_GLOBAL, FB_IMPORT, FB_CAPTURED, FB_COPIED, FB_PASSED, FB_STORED, FB_PATH_CHANGING, FB_COUNT };

static const char *fallback_reason(uint8_t why) {
    const char *const reasons[FB_COUNT] = { NULL, refused_global, refused_import, refused_captured,
                                            refused_copied, refused_passed, refused_stored, refused_path_changing };
    return why < FB_COUNT ? reasons[why] : NULL;
}

static uint8_t fallback_of(const char *why) {
    for (uint8_t i = 1; i < FB_COUNT; i++) {
        if (fallback_reason(i) == why) return i;
    }
    return FB_NONE;
}

/* MUTATION: the variable `read` read just before, the receiver of a
 * mutation, if the function owns it: its read becomes a RECEIVER. Refused
 * when the variable is a global, an import or a capture, or was copied,
 * passed or stored before (false).
 *
 * MUTATION: a shared name (sjs_mutation_shared), `fallback` not NULL: the
 * run decides, as the evaluator checks what the compiler would refuse
 * (docs/mutation.md, "Shared names"). A variable of the function is
 * claimed whatever was done with it, and not noted as changed, so that a
 * later capture is not refused; anything else is an ordinary call (false),
 * with the reason in `fallback` (FB_*) for a statement to give. */
static bool claim_receiver(sjs_compiler *c, const name_read *read, uint8_t *fallback) {
    const char *why = NULL;
    sjs_str name = read->kind == NR_GLOBAL ? c->globals[read->which] : binding_name(c, read->which);
    binding *b = read->kind == NR_GLOBAL ? NULL : &SJS_BUF_AT(c->bindings, binding, read->which);
    if (read->kind == NR_GLOBAL) {
        why = refused_global;
    } else if (read->kind == NR_IMPORT) {
        why = refused_import;
    } else if (read->kind == NR_CAPTURE) {
        why = refused_captured;
    } else if (fallback == NULL) {
        why = refused_use(b->use);
    }
    if (why != NULL) {
        if (fallback != NULL) {
            *fallback = fallback_of(why);
        } else {
            refuse_later(c, read->at, (refusal){ why, name });
        }
        return false;
    }
    if (fallback == NULL) b->use |= USE_MUTATED;
    b->changed_at = read->pc + 1u;
    c->code.ptr[read->pc] = SJS_OP_RECEIVER;
    return true;
}

/* MUTATION: `x.m(` where m names a method that changes its receiver
 * (sjs_mutation_of), the GET_MEMBER of m at c->last_op. A mutation when x
 * is a variable this function owns, read just before, or the value a
 * mutation of one returned its receiver as: then the read becomes a
 * RECEIVER, and m's GET_MEMBER a METHOD_MUTATE. Refused when x is a name
 * the function does not own; any other receiver is an ordinary method call
 * (false), which faults when it runs on an array, a set or a map. */
static bool mutation_begin(sjs_compiler *c, construct *k) {
    uint32_t member = c->last_op;
    const sjs_string *key = SJS_AS_STRING(SJS_BUF_AT(c->constants, sjs_value, sjs_read32(c->code.ptr + member + 1u)));
    const sjs_name *name = sjs_name_of(key);
    sjs_builtin_id id = sjs_mutation_named(name);
    if (id == SJS_BUILTIN_NONE) return false;
    const name_read *read = &c->last_name;
    const mutation_site *chain = &c->last_mutation;
    if (read->end == member && c->last_label <= read->pc && read->kind != NR_NONE) {
        /* MUTATION: a shared name falls back to an ordinary call. */
        uint8_t fallback = FB_NONE;
        if (!claim_receiver(c, read, sjs_mutation_shared(name) ? &fallback : NULL)) {
            k->fallback = fallback;
            k->receiver = read->which;
            return false;
        }
        k->receiver = read->which;
    } else if (chain->end == member && c->last_label <= chain->pc && chain->returns && chain->keys == 0) { /* MUTATION: keys, a path's chain is its own */
        /* The receiver goes on as the reference it was. */
        c->code.ptr[chain->pc + 3u] |= SJS_CALL_CHAIN;
        k->receiver = chain->receiver;
    } else {
        return false;
    }
    c->code.ptr[member] = SJS_OP_METHOD_MUTATE;
    k->mutate = 1;
    k->returns = sjs_mutation_returns_receiver(id) ? 1u : 0u;
    k->sorts = id == SJS_METHOD_ARRAY_SORT ? 1u : 0u;
    return true;
}

/* MUTATION: the call of a mutation: MUTATE, after its arguments, and for
 * a sort SORTED, which puts the sorted array back. The mutation reads the
 * slot after them, so no read of it among them moves its value
 * (forget_read). A method that returns its receiver gives a copy of the
 * variable's value, a read whose use the next event tells. The last
 * instruction's flags take CHAIN (mutation_begin). */
static sjs_result mutation_end(sjs_compiler *c, const construct *k, const sjs_event *e) {
    if (op(c, SJS_OP_MUTATE, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    uint32_t pc = c->last_op;
    if (u16(c, (uint16_t)e->count) != SJS_OK) return SJS_ERR_PROGRAM;
    if (u8(c, k->spread ? SJS_CALL_SPREAD : 0u) != SJS_OK) return SJS_ERR_PROGRAM;
    if (k->sorts) {
        if (op16(c, ins(SJS_OP_SORTED, SJS_BUF_AT(c->bindings, binding, k->receiver).index, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
        pc = c->last_op;
        if (u8(c, 0) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    c->last_op_valid = false;
    c->last_mutation = (mutation_site){ pc, here(c), k->receiver, k->returns, 0, 0 }; /* MUTATION: no keys */
    forget_read(c, current(c)->function_scope, SJS_BUF_AT(c->bindings, binding, k->receiver).index);
    if (k->returns) c->pending_read = k->receiver + 1u;
    return SJS_OK;
}

/* MUTATION: Object.assign(x, ...sources) where x is a variable the
 * function owns, read as the whole first argument (docs/mutation.md,
 * "Object.assign"): the read becomes a RECEIVER, by the rules of a
 * mutation, and the call an OBJECT_ASSIGN after the sources, which changes
 * the variable's object in place. Any other first argument is an ordinary
 * call's, which changes nothing a program can see (assign_call). CALL_BEGIN
 * notes a callee that is Object.assign's constant; its first ARGUMENT (or
 * SPREAD_ARGUMENT) decides, before note_use would count the read as passed.
 * Object.freeze and Object.seal of such a variable compile the same way,
 * OBJECT_ASSIGN telling them by the callee when it runs; as their call
 * changes nothing when the value is already frozen or sealed, a variable
 * the function owns is their receiver whatever was done with it before,
 * and the run decides, as for a shared name (docs/mutation.md, "Frozen and
 * sealed values"). AT_PENDING, AT_FREEZING, or AT_NONE for another callee. */
static uint8_t assign_callee(const sjs_compiler *c) {
    if (!c->last_op_valid || c->last_label > c->last_op || c->code.ptr[c->last_op] != SJS_OP_CONSTANT) return AT_NONE;
    sjs_value v = SJS_BUF_AT(c->constants, sjs_value, sjs_read32(c->code.ptr + c->last_op + 1u));
    if (v.kind != SJS_FUNCTION) return AT_NONE;
    if (v.as.heap == sjs_builtin_value(SJS_BUILTIN_OBJECT_ASSIGN).as.heap) return AT_PENDING;
    if (v.as.heap == sjs_builtin_value(SJS_BUILTIN_OBJECT_FREEZE).as.heap) return AT_FREEZING;
    return v.as.heap == sjs_builtin_value(SJS_BUILTIN_OBJECT_SEAL).as.heap ? AT_FREEZING : AT_NONE;
}

static void assign_argument(sjs_compiler *c, const sjs_event *e) {
    construct *k = top_construct(c);
    if (k == NULL || k->kind != K_CALL || (k->assigns != AT_PENDING && k->assigns != AT_FREEZING)) return;
    bool freezing = k->assigns == AT_FREEZING;
    k->assigns = AT_NONE;
    const name_read *read = &c->last_name;
    if (e->kind != SJS_EV_ARGUMENT || read->pc != k->target_pc || read->end != here(c) || c->last_label > read->pc ||
        read->kind == NR_NONE) {
        return;
    }
    uint8_t fallback = FB_NONE;
    if (!claim_receiver(c, read, freezing ? &fallback : NULL)) {
        /* MUTATION: Object.freeze or Object.seal of a capture, a global or
         * an import: an ordinary call, which a statement refuses with the
         * reason. */
        k->fallback = fallback;
        k->receiver = read->which;
        return;
    }
    k->assigns = AT_VARIABLE;
    k->receiver = read->which;
    c->pending_read = 0;
}

/* OBJECT_ASSIGN, after the sources. As mutation_end: no read among them
 * moves the variable's value, and the copy of it the call gives is a read
 * whose use the next event tells. */
static sjs_result object_assign_end(sjs_compiler *c, const construct *k, const sjs_event *e) {
    if (op(c, SJS_OP_OBJECT_ASSIGN, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    uint32_t pc = c->last_op;
    if (u16(c, (uint16_t)(e->count - 1u)) != SJS_OK) return SJS_ERR_PROGRAM;
    if (u8(c, k->spread ? SJS_CALL_SPREAD : 0u) != SJS_OK) return SJS_ERR_PROGRAM;
    c->last_op_valid = false;
    c->last_mutation = (mutation_site){ pc, here(c), k->receiver, 0, 0, 0 };
    forget_read(c, current(c)->function_scope, SJS_BUF_AT(c->bindings, binding, k->receiver).index);
    c->pending_read = k->receiver + 1u;
    return SJS_OK;
}

/* MUTATION: a method that changes its receiver, called on a path from a
 * variable the function owns (docs/mutation.md, "Methods through a path"):
 * `o.items.push(x)`, `s.byID[k].tags.add(t)`, and a chain of them. The
 * matcher reported MUTATE_PATH for the call (on_rewrite), whose rules decide
 * the form: a mutating method's name, called on a name and one step or
 * more. Its code is already there, as an ordinary method call on a path,
 * which the call held (paths_before): as an assignment's target, the read
 * of the root becomes a RECEIVER and each step pushes its key; the method's
 * METHOD_MEMBER becomes a PATH_METHOD, which pushes its name; and PATH_CALL,
 * after the arguments, finds the container when it runs and changes it in
 * place. A chain goes on from the last such call, whose CHAIN flag leaves
 * the reference and the keys for the next. What needs scopes is decided
 * here: the root is a variable the function owns, by the rules of a
 * mutation, and no method changed it in the keys or the arguments. */

/* The path the call held is its callee: the root's read becomes a
 * RECEIVER and each step but the method a key. k->member is the method's
 * METHOD_MEMBER, the path's last step. Answers the keys, or 0 when the
 * call stays an ordinary one (refused, or no path). */
static uint32_t path_mutation_target(sjs_compiler *c, construct *k, const sjs_event *e) {
    const path *p = top_path(c);
    if (p == NULL || !p->held) return 0;
    uint32_t steps = SJS_BUF_COUNT(c->path_steps, uint32_t) - p->steps_base;
    name_read root = p->root;
    const char *why = NULL;
    sjs_str name = root.kind == NR_GLOBAL ? c->globals[root.which] : binding_name(c, root.which);
    if (root.kind == NR_GLOBAL) {
        why = refused_global;
    } else if (root.kind == NR_IMPORT) {
        why = refused_import;
    } else {
        /* What the arguments do with the root's value is the evaluator's to
         * check, as for a mutation of a variable: it holds the value too
         * when the call runs (`o.items.push(o)`, a cycle). A capture is
         * refused wherever it is. */
        binding *b = &SJS_BUF_AT(c->bindings, binding, root.which);
        why = root.kind == NR_CAPTURE ? refused_captured : refused_use((uint8_t)(k->root_use | (b->use & USE_CAPTURED)));
        if (why == NULL && b->changed_at > root.pc + 1u) why = refused_path_changing;
    }
    if (why != NULL || steps < 2u || SJS_BUF_AT(c->path_steps, uint32_t, p->steps_base + steps - 1u) != k->member) {
        /* MUTATION: a shared name falls back to an ordinary call, which
         * the evaluator checks as any is. */
        const sjs_string *key = SJS_AS_STRING(SJS_BUF_AT(c->constants, sjs_value, sjs_read32(c->code.ptr + k->member + 1u)));
        if (why != NULL && sjs_mutation_shared(sjs_name_of(key))) {
            k->fallback = fallback_of(why);
            k->receiver = root.which;
        } else if (why != NULL) {
            refuse_later(c, root.at, (refusal){ why, name });
        }
        drop_paths(c, SJS_BUF_COUNT(c->paths, path) - 1u);
        return 0;
    }
    if (steps - 1u > UINT16_MAX) {
        compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_LIMIT, "a method's path has too many keys" });
        return 0;
    }
    binding *b = &SJS_BUF_AT(c->bindings, binding, root.which);
    b->use |= USE_MUTATED;
    b->changed_at = root.pc + 1u;
    c->code.ptr[root.pc] = SJS_OP_RECEIVER;
    for (uint32_t i = 0; i + 1u < steps; i++) {
        uint8_t *step = c->code.ptr + SJS_BUF_AT(c->path_steps, uint32_t, p->steps_base + i);
        *step = *step == SJS_OP_GET_MEMBER ? SJS_OP_CONSTANT : SJS_OP_PATH_INDEX;
    }
    k->receiver = root.which;
    drop_paths(c, SJS_BUF_COUNT(c->paths, path) - 1u);
    return steps - 1u;
}

/* CALL_END of a call the matcher reported as MUTATE_PATH, or that held a
 * path: PATH_CALL, and PATH_SORTED after a sort. A call that is not a
 * mutation through a path clears k->mutate_path and is compiled as any
 * call is. */
static sjs_result path_mutation_end(sjs_compiler *c, construct *k, const sjs_event *e) {
    uint32_t keys = 0;
    sjs_builtin_id id = SJS_BUILTIN_NONE;
    if (k->member != 0 && c->code.ptr[k->member] == SJS_OP_METHOD_MEMBER) {
        const sjs_string *key = SJS_AS_STRING(SJS_BUF_AT(c->constants, sjs_value, sjs_read32(c->code.ptr + k->member + 1u)));
        id = sjs_mutation_named(sjs_name_of(key));
    }
    const mutation_site *chain = &c->last_mutation;
    if (!k->mutate_path || id == SJS_BUILTIN_NONE) {
        if (k->held) drop_paths(c, SJS_BUF_COUNT(c->paths, path) - 1u);
        k->mutate_path = 0;
        return SJS_OK;
    }
    if (k->held) {
        keys = path_mutation_target(c, k, e);
        if (c->failed) return SJS_ERR_PROGRAM;
    } else if (chain->keys != 0 && chain->end == k->member && c->last_label <= chain->pc && chain->returns) {
        /* A chain: the last mutation left the reference and its keys. */
        c->code.ptr[chain->flags_at] |= SJS_CALL_CHAIN;
        keys = chain->keys;
        k->receiver = chain->receiver;
    }
    if (keys == 0) {
        k->mutate_path = 0;
        return SJS_OK;
    }
    c->code.ptr[k->member] = SJS_OP_PATH_METHOD;
    if (op(c, SJS_OP_PATH_CALL, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    uint32_t pc = c->last_op;
    if (u16(c, (uint16_t)keys) != SJS_OK || u16(c, (uint16_t)e->count) != SJS_OK) return SJS_ERR_PROGRAM;
    uint32_t flags_at = here(c);
    if (u8(c, k->spread ? SJS_CALL_SPREAD : 0u) != SJS_OK) return SJS_ERR_PROGRAM;
    if (id == SJS_METHOD_ARRAY_SORT) {
        if (op(c, SJS_OP_PATH_SORTED, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
        pc = c->last_op;
        if (u16(c, (uint16_t)keys) != SJS_OK) return SJS_ERR_PROGRAM;
        flags_at = here(c);
        if (u8(c, 0) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    c->last_op_valid = false;
    bool returns = sjs_mutation_returns_receiver(id);
    c->last_mutation = (mutation_site){ pc, here(c), k->receiver, returns ? 1u : 0u, keys, flags_at };
    /* The call reads the root's slot after the arguments: none of their
     * reads moves its value. */
    forget_read(c, current(c)->function_scope, SJS_BUF_AT(c->bindings, binding, k->receiver).index);
    return SJS_OK;
}

static sjs_result call_begin(sjs_compiler *c, const sjs_event *e) {
    construct k = { 0 };
    k.kind = K_CALL;
    /* MUTATION: what a rewrite of the call needs (path_mutation_end). */
    k.node = e->node;
    k.held = c->call_holds_path ? 1u : 0u;
    c->call_holds_path = false;
    if (k.held && top_path(c)->root.kind == NR_OWN) k.root_use = SJS_BUF_AT(c->bindings, binding, top_path(c)->root.which).use;
    /* MUTATION: Object.assign, Object.freeze and Object.seal, whose first
     * argument may be a variable they change. */
    k.assigns = assign_callee(c);
    if (k.assigns != AT_NONE) k.target_pc = here(c);
    /* `a.b(...)` and `a[b](...)` call with a receiver. */
    if (c->last_op_valid && c->last_label <= c->last_op) {
        uint8_t *last = c->code.ptr + c->last_op;
        if (*last == SJS_OP_GET_MEMBER) k.member = c->last_op; /* MUTATION */
        if (*last == SJS_OP_GET_MEMBER && mutation_begin(c, &k)) {
            /* MUTATION: a method that changes the variable it is called on. */
        } else if (*last == SJS_OP_GET_MEMBER) {
            *last = SJS_OP_METHOD_MEMBER;
            k.method = 1;
        } else if (*last == SJS_OP_GET_INDEX) {
            *last = SJS_OP_METHOD_INDEX;
            k.method = 1;
        }
    }
    return push_construct(c, k, e->at);
}

static sjs_result call_end(sjs_compiler *c, const sjs_event *e, uint8_t opcode) {
    uint8_t kind = opcode == SJS_OP_CALL ? K_CALL : K_NEW;
    if (expect_construct(c, kind, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    construct k = pop_construct(c);
    if (e->count > UINT16_MAX) return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_LIMIT, "a call has too many arguments" });
    if (k.mutate) return mutation_end(c, &k, e); /* MUTATION */
    if (k.assigns == AT_VARIABLE) return object_assign_end(c, &k, e); /* MUTATION: Object.assign */
    if (k.held || k.mutate_path) { /* MUTATION: a method called on a path */
        sjs_result r = path_mutation_end(c, &k, e);
        if (r != SJS_OK || k.mutate_path) return r;
    }
    if (op(c, opcode, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    uint32_t pc = c->last_op; /* MUTATION: shared names */
    if (u16(c, (uint16_t)e->count) != SJS_OK) return SJS_ERR_PROGRAM;
    uint8_t flags = (uint8_t)((k.method ? SJS_CALL_METHOD : 0u) | (k.spread ? SJS_CALL_SPREAD : 0u));
    sjs_result r = u8(c, flags);
    c->last_op_valid = false;
    /* MUTATION: a shared name's ordinary call, which a statement refuses. */
    if (k.fallback != FB_NONE) c->last_fallback = (fallback_site){ pc, here(c), k.receiver, k.fallback };
    return r;
}

static sjs_result spread_argument(sjs_compiler *c, const sjs_event *e) {
    construct *k = top_construct(c);
    if (k == NULL || (k->kind != K_CALL && k->kind != K_NEW)) return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_SYNTAX, "the events are out of order" });
    k->spread = 1;
    return op(c, SJS_OP_SPREAD, e->at);
}

static sjs_result compile_logical(sjs_compiler *c, const sjs_event *e) {
    construct k = { 0 };
    k.kind = K_LOGICAL;
    uint8_t opcode = e->count == SJS_OP_AND ? SJS_OP_JUMP_FALSY_KEEP
                   : (e->count == SJS_OP_OR ? SJS_OP_JUMP_TRUTHY_KEEP : SJS_OP_JUMP_DEFINED_KEEP);
    if (jump(c, ins(opcode, 0, e->at), &k.jump) != SJS_OK) return SJS_ERR_PROGRAM;
    return push_construct(c, k, e->at);
}

static sjs_result compile_optional(sjs_compiler *c, const sjs_event *e) {
    construct *k = top_construct(c);
    if (k == NULL || k->kind != K_CHAIN || k->depth != e->count) {
        construct chain = { 0 };
        chain.kind = K_CHAIN;
        chain.depth = e->count;
        chain.jumps_base = SJS_BUF_COUNT(c->jumps, uint32_t);
        if (push_construct(c, chain, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    uint32_t operand;
    if (jump(c, ins(SJS_OP_JUMP_NULLISH, 0, e->at), &operand) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_result r = buf_add(c, &c->jumps, bytes_of(&operand, sizeof operand));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, e->at);
}

static sjs_result chain_end(sjs_compiler *c, const sjs_event *e) {
    construct *k = top_construct(c);
    if (k == NULL || k->kind != K_CHAIN || k->depth != e->count) return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_SYNTAX, "the events are out of order" });
    construct chain = pop_construct(c);
    uint32_t n = SJS_BUF_COUNT(c->jumps, uint32_t);
    for (uint32_t i = chain.jumps_base; i < n; i++) land(c, SJS_BUF_AT(c->jumps, uint32_t, i));
    c->jumps.length = chain.jumps_base * (uint32_t)sizeof(uint32_t);
    return SJS_OK;
}

static sjs_result const_end(sjs_compiler *c, const sjs_event *e) {
    if (expect_construct(c, K_CONST, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    pop_construct(c);
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * Statements
 */

/* Does the code end in a RETURN that nothing jumps past? Then what follows
 * is reached only by a jump that lands later, and falling off the end needs
 * no code. A peephole: no jump lands after the last instruction, which is a
 * RETURN. */
static bool ends_in_return(const sjs_compiler *c) {
    return c->last_op_valid && c->code.ptr[c->last_op] == SJS_OP_RETURN && c->last_op + 1u == here(c) &&
           c->last_label <= c->last_op;
}

/* Can a statement begin here? In a function's block body, at its top or in
 * a block statement or a branch of an if. */
static bool statement_level(sjs_compiler *c) {
    if (!function_scope(c)->block) return false;
    const construct *k = top_construct(c);
    if (k == NULL) return false;
    if (k->kind == K_FUNCTION) return current(c)->kind == SC_FUNCTION && scope_count(c) > 1;
    if (k->kind == K_BLOCK) return true;
    return k->kind == K_IF && (k->branch == KI_CONSEQUENT || k->branch == KI_ALTERNATIVE);
}

/* IF: after the test. A new if, or the next test of an else-if chain. */
static sjs_result compile_if(sjs_compiler *c, const sjs_event *e) {
    construct *chain = top_construct(c);
    if (chain != NULL && chain->kind == K_IF && chain->branch == KI_ELSE_IF) {
        uint32_t operand;
        if (jump(c, ins(SJS_OP_JUMP_FALSY, 0, e->at), &operand) != SJS_OK) return SJS_ERR_PROGRAM;
        chain = top_construct(c);
        chain->jump = operand;
        chain->branch = KI_CONSEQUENT;
        chain->branch_pc = here(c); /* MUTATION: end_branch */
        chain->parked_mark = SJS_BUF_COUNT(c->parked_reads, parked_read); /* MUTATION */
        return SJS_OK;
    }
    if (!statement_level(c)) return out_of_order(c, e->at);
    construct k = { 0 };
    k.kind = K_IF;
    k.branch = KI_CONSEQUENT;
    k.jumps_base = SJS_BUF_COUNT(c->jumps, uint32_t);
    if (jump(c, ins(SJS_OP_JUMP_FALSY, 0, e->at), &k.jump) != SJS_OK) return SJS_ERR_PROGRAM;
    k.branch_pc = here(c); /* MUTATION: end_branch */
    k.parked_base = SJS_BUF_COUNT(c->parked_reads, parked_read); /* MUTATION */
    k.parked_mark = k.parked_base; /* MUTATION */
    return push_construct(c, k, e->at);
}

/* ELSE: the consequent ends. Unless it ends in a return, it jumps to the
 * end of the chain; the test's jump lands on what follows. */
static sjs_result compile_else(sjs_compiler *c, const sjs_event *e) {
    construct *k = top_construct(c);
    if (k == NULL || k->kind != K_IF || k->branch != KI_CONSEQUENT) return out_of_order(c, e->at);
    if (end_branch(c, (branch_end){ k->branch_pc, k->parked_mark, ends_in_return(c) }, e->at) != SJS_OK) return SJS_ERR_PROGRAM; /* MUTATION */
    k = top_construct(c);
    if (!ends_in_return(c)) {
        uint32_t operand;
        if (jump(c, ins(SJS_OP_JUMP, 0, e->at), &operand) != SJS_OK) return SJS_ERR_PROGRAM;
        sjs_result r = buf_add(c, &c->jumps, bytes_of(&operand, sizeof operand));
        if (r != SJS_OK) return fail_result(c, r, e->at);
        k = top_construct(c);
    }
    land(c, k->jump);
    k->jump = 0;
    k->branch = e->count != 0 ? KI_ELSE_IF : KI_ALTERNATIVE;
    k->branch_pc = here(c); /* MUTATION: end_branch */
    k->parked_mark = SJS_BUF_COUNT(c->parked_reads, parked_read); /* MUTATION */
    return SJS_OK;
}

/* IF_END: every jump of the chain that is still open lands here. */
static sjs_result compile_if_end(sjs_compiler *c, const sjs_event *e) {
    construct *top = top_construct(c);
    if (top == NULL || top->kind != K_IF || top->branch == KI_ELSE_IF) return out_of_order(c, e->at);
    /* MUTATION: the last branch's reads are fenced from no other. */
    if (ends_in_return(c) && end_branch(c, (branch_end){ top->branch_pc, top->parked_mark, true }, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    construct k = pop_construct(c);
    fence_parked(c, k.parked_base, 0); /* MUTATION */
    if (k.jump != 0) land(c, k.jump);
    uint32_t n = SJS_BUF_COUNT(c->jumps, uint32_t);
    for (uint32_t i = k.jumps_base; i < n; i++) land(c, SJS_BUF_AT(c->jumps, uint32_t, i));
    c->jumps.length = k.jumps_base * (uint32_t)sizeof(uint32_t);
    return SJS_OK;
}

/* BLOCK_STATEMENT_BEGIN: a scope of its own, and no code. */
static sjs_result compile_block_begin(sjs_compiler *c, const sjs_event *e) {
    if (!statement_level(c)) return out_of_order(c, e->at);
    construct k = { 0 };
    k.kind = K_BLOCK;
    if (push_construct(c, k, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    return open_block_scope(c, e->at);
}

static sjs_result compile_block_end(sjs_compiler *c, const sjs_event *e) {
    if (expect_construct(c, K_BLOCK, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    if (current(c)->kind != SC_BLOCK) return out_of_order(c, e->at);
    pop_construct(c);
    close_block_scope(c);
    return SJS_OK;
}

static sjs_result compile_return(sjs_compiler *c, const sjs_event *e) {
    if (!statement_level(c)) return out_of_order(c, e->at);
    if (e->count == 0 && op(c, SJS_OP_UNDEFINED_, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    return op(c, SJS_OP_RETURN, e->at);
}

/* MUTATION: EXPRESSION_STATEMENT, after a call: the statement is a
 * mutation, or a chain of them, whose value goes; any other is refused. */
static sjs_result compile_expression_statement(sjs_compiler *c, const sjs_event *e) {
    if (!statement_level(c)) return out_of_order(c, e->at);
    if (c->last_mutation.end != here(c) || c->last_label > c->last_mutation.pc) {
        /* MUTATION: a shared name's call that a mutation would refuse. */
        const fallback_site *f = &c->last_fallback;
        if (f->why != FB_NONE && f->end == here(c) && c->last_label <= f->pc) {
            sjs_str name = f->why == FB_GLOBAL ? c->globals[f->which] : binding_name(c, f->which);
            return refuse_later(c, e->start, (refusal){ fallback_reason(f->why), name });
        }
        return refuse_later(c, e->start, (refusal){ "an expression statement has no effect in staticjs, unless it changes a variable with one of its methods", SJS_STR("") });
    }
    return op(c, SJS_OP_POP, e->at);
}

/* MUTATION: assignment (docs/mutation.md, "Assignment"): a statement that
 * stores into a property or an element of a variable the function owns.
 * Its target's code is already there, as a read: the read of the root
 * becomes a RECEIVER, a reference to its slot that holds nothing, and each
 * step pushes its key, a GET_MEMBER's constant or a GET_INDEX's key made a
 * string, or kept a number for an array (PATH_INDEX). PATH then finds the
 * place they name when it runs,
 * through containers the place before holds alone. */
enum { TARGET_TAKEN = 0, TARGET_REFUSED, TARGET_FAILED };

static const char *const refused_target = "staticjs assigns only to a property or an element of a variable the function owns";
static const char *const refused_changing = "staticjs does not change a variable with one of its methods while it assigns to it";

static bool assign_operator(uint32_t op) {
    return op == 0 || (op >= SJS_OP_ADD && op <= SJS_OP_REMAINDER) || op == SJS_OP_AND || op == SJS_OP_OR || op == SJS_OP_COALESCE ||
           op == SJS_OP_EXPONENT;
}

static bool logical_operator(uint32_t op) {
    return op == SJS_OP_AND || op == SJS_OP_OR || op == SJS_OP_COALESCE;
}

/* The target is a variable, the path on top with no step: a let of this
 * function. A plain assignment does not read it, so its read goes. */
static uint8_t variable_target(sjs_compiler *c, construct *k) {
    name_read root = top_path(c)->root;
    const char *why = NULL;
    sjs_str name = root.kind == NR_GLOBAL ? c->globals[root.which] : binding_name(c, root.which);
    if (root.kind == NR_GLOBAL) {
        why = "staticjs assigns only to a variable declared with let, and this is a global";
    } else if (root.kind == NR_IMPORT) {
        why = "staticjs assigns only to a variable declared with let, and this is an import";
    } else if (root.kind == NR_CAPTURE) {
        why = refused_let_capture;
    } else if (!SJS_BUF_AT(c->bindings, binding, root.which).declared_let) {
        why = "staticjs assigns only to a variable declared with let, and this one is a const or a parameter";
    }
    if (why != NULL) {
        refuse_later(c, root.at, (refusal){ why, name });
        return TARGET_REFUSED;
    }
    drop_paths(c, SJS_BUF_COUNT(c->paths, path) - 1u);
    forget_read(c, current(c)->function_scope, SJS_BUF_AT(c->bindings, binding, root.which).index);
    if (k->assign == 0) {
        c->code.length = root.pc;
        c->last_op_valid = false;
        c->last_name.kind = NR_NONE;
    }
    k->receiver = root.which;
    k->keys = 0;
    k->root_pc = root.pc;
    return TARGET_TAKEN;
}

/* The target of an assignment or an update: the path on top, which ends
 * here. Its root must be a variable the function owns, under the rules of a
 * mutation; k takes the root's binding, its keys and the read's pc. */
static uint8_t assign_target(sjs_compiler *c, const sjs_event *e, construct *k) {
    path *p = top_path(c);
    if (p == NULL || p->end != here(c)) {
        refuse_later(c, e->start, (refusal){ refused_target, SJS_STR("") });
        return TARGET_REFUSED;
    }
    name_read root = p->root;
    uint32_t keys = SJS_BUF_COUNT(c->path_steps, uint32_t) - p->steps_base;
    if (keys == 0) return variable_target(c, k);
    if (root.kind == NR_GLOBAL) {
        refuse_later(c, root.at, (refusal){ "staticjs changes only a value a variable of the function holds, and this is a global", c->globals[root.which] });
        return TARGET_REFUSED;
    }
    sjs_str name = binding_name(c, root.which);
    if (root.kind == NR_IMPORT) {
        refuse_later(c, root.at, (refusal){ "staticjs changes only a value a variable of the function holds, and this is an import", name });
        return TARGET_REFUSED;
    }
    binding *b = &SJS_BUF_AT(c->bindings, binding, root.which);
    const char *why = root.kind == NR_CAPTURE ? refused_captured : refused_use(b->use);
    if (why == NULL && b->changed_at > root.pc + 1u) why = refused_changing;
    if (why != NULL) {
        refuse_later(c, root.at, (refusal){ why, name });
        return TARGET_REFUSED;
    }
    if (keys > UINT16_MAX) {
        compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_LIMIT, "an assignment's target has too many keys" });
        return TARGET_FAILED;
    }
    b->use |= USE_MUTATED;
    c->code.ptr[root.pc] = SJS_OP_RECEIVER;
    for (uint32_t i = 0; i < keys; i++) {
        uint8_t *step = c->code.ptr + SJS_BUF_AT(c->path_steps, uint32_t, p->steps_base + i);
        *step = *step == SJS_OP_GET_MEMBER ? SJS_OP_CONSTANT : SJS_OP_PATH_INDEX;
    }
    forget_read(c, current(c)->function_scope, b->index);
    drop_paths(c, SJS_BUF_COUNT(c->paths, path) - 1u);
    c->last_op_valid = false;
    k->receiver = root.which;
    k->keys = keys;
    k->root_pc = root.pc;
    return TARGET_TAKEN;
}

/* PATH, over the target's keys; the instruction's operand is its mode. */
static sjs_result emit_path(sjs_compiler *c, const construct *k, instruction mode) {
    if (op(c, SJS_OP_PATH, mode.at) != SJS_OK) return SJS_ERR_PROGRAM;
    if (u16(c, (uint16_t)k->keys) != SJS_OK) return SJS_ERR_PROGRAM;
    return u8(c, (uint8_t)mode.operand);
}

/* The value stored has been read, and nothing read the root's slot since
 * that it moves (forget_read); a method may not have changed the variable
 * meanwhile, as JavaScript holds the containers of the path from before. */
static sjs_result assign_store(sjs_compiler *c, const construct *k, const sjs_event *e) {
    binding *b = &SJS_BUF_AT(c->bindings, binding, k->receiver);
    if (b->changed_at > k->root_pc + 1u) return refuse_later(c, e->start, (refusal){ refused_changing, binding_name(c, k->receiver) });
    forget_read(c, current(c)->function_scope, b->index);
    return emit_path(c, k, ins(SJS_OP_PATH, SJS_PATH_STORE, e->at));
}

/* A variable's new value: as far as the compiler knows, nothing else holds
 * it yet. */
static sjs_result variable_store(sjs_compiler *c, const construct *k, const sjs_event *e) {
    binding *b = &SJS_BUF_AT(c->bindings, binding, k->receiver);
    b->use &= (uint8_t)~(USE_COPIED | USE_PASSED | USE_STORED);
    /* MUTATION: the value's code starts at the target's read (a compound
     * assignment's, or where a plain one's was), or after the jump of
     * && || ??, which skips it and the store. Restore write_slot. */
    uint32_t from = logical_operator(k->assign) ? k->jump + 1u : k->root_pc;
    return store_slot(c, ins(SJS_OP_SET_LOCAL, b->index, e->at), from);
}

static sjs_result target_store(sjs_compiler *c, const construct *k, const sjs_event *e) {
    return k->keys == 0 ? variable_store(c, k, e) : assign_store(c, k, e);
}

/* MUTATION: appending to a let (docs/mutation.md, "Appending to a let").
 * `s += e`, `s = s + e1 + e2` and `s = `${s}...``, of a let the function
 * owns: its read becomes a TAKE_LOCAL, which moves the value out of the
 * slot, so that + (or the template's join) finds a string nothing else
 * holds and extends it in place (STRING_APPEND, vm.c); the store puts the
 * result back. Only when the value reads the let no other time
 * (append_move): such a read runs between the move and the store, and
 * would find the slot empty; so would a method of the let's own, which
 * reads it by name first. The store comes after the whole value on every
 * path, and a capture of a let is refused, so the move is never seen. */
static void append_take(sjs_compiler *c, uint32_t pc, const binding *b) {
    if (c->code.ptr[pc] != SJS_OP_LOCAL || sjs_read16(c->code.ptr + pc + 1u) != b->index) return;
    c->code.ptr[pc] = SJS_OP_TAKE_LOCAL;
}

/* MUTATION: the matcher reported APPEND_SUM for a + whose spine starts at
 * `start`: the innermost finds its root's read in the paths, which still
 * hold it, as the + has not been handled yet; an outer one goes on with the
 * same root. */
static void append_sum(sjs_compiler *c, uint32_t start) {
    if (c->sum.start != start) {
        c->sum.start = UINT32_MAX;
        for (uint32_t n = SJS_BUF_COUNT(c->paths, path); n-- > 0;) {
            const path *p = &SJS_BUF_AT(c->paths, path, n);
            if (p->root.at.offset != start) continue;
            if (p->root.kind != NR_OWN) return;
            c->sum.start = start;
            c->sum.root_pc = p->root.pc;
            c->sum.which = p->root.which;
            break;
        }
        if (c->sum.start == UINT32_MAX) return;
    }
    c->sum.binary_pc = here(c);
}

/* MUTATION: the value of `s = ...` is a template whose head is empty and
 * whose first substitution starts with a read of s: CONSTANT "", LOCAL s,
 * ..., CONCAT. A template is a bracket over no node before it, which the
 * matcher's anchored lane does not match (docs/matcher.md, 12.7), so the
 * compiler finds the form in its own code. */
static bool append_template(sjs_compiler *c, const construct *k) {
    uint32_t at = k->root_pc;
    if (here(c) < at + 11u || c->code.ptr[at] != SJS_OP_CONSTANT) return false;
    if (c->last_op != here(c) - 3u || c->code.ptr[c->last_op] != SJS_OP_CONCAT) return false;
    sjs_value head = SJS_BUF_AT(c->constants, sjs_value, sjs_read32(c->code.ptr + at + 1u));
    return head.kind == SJS_STRING && SJS_AS_STRING(head)->length == 0;
}

/* MUTATION: at ASSIGN_END of a let's assignment, before its operator and
 * its store: move the read of the let that an append starts with, when the
 * value read the let no other time (binding.reads): `s += e` reads it as
 * its target, before ASSIGN; `s = s + ...` once in the value, and its
 * target's read is gone (variable_target). */
static void append_move(sjs_compiler *c, const construct *k) {
    const binding *b = &SJS_BUF_AT(c->bindings, binding, k->receiver);
    if (b->reads != k->reads + (k->assign == 0 ? 1u : 0u)) return;
    if (k->assign == SJS_OP_ADD && k->append) {
        append_take(c, k->root_pc, b);
    } else if (k->assign == 0 && c->sum.start != UINT32_MAX && c->sum.which == k->receiver &&
               c->sum.root_pc == k->root_pc && c->last_op == c->sum.binary_pc && here(c) == c->sum.binary_pc + 2u) {
        append_take(c, k->root_pc, b);
    } else if (k->assign == 0 && append_template(c, k)) {
        append_take(c, k->root_pc + 5u, b);
    }
}

/* ASSIGN: the target is compiled; the value follows. A compound
 * assignment reads the target's value first; && || ?? skip the value and
 * the store by its truth. */
static sjs_result assign_begin(sjs_compiler *c, const sjs_event *e) {
    if (!statement_level(c) || !assign_operator(e->count)) return out_of_order(c, e->at);
    construct k = { 0 };
    k.kind = K_ASSIGN;
    k.assign = (uint8_t)e->count;
    k.node = e->node; /* MUTATION: an append's report names it */
    uint8_t taken = assign_target(c, e, &k);
    if (taken != TARGET_TAKEN) return taken == TARGET_REFUSED ? SJS_OK : SJS_ERR_PROGRAM;
    k.reads = SJS_BUF_AT(c->bindings, binding, k.receiver).reads; /* MUTATION: appending to a let */
    if (k.assign != 0 && k.keys != 0 && emit_path(c, &k, ins(SJS_OP_PATH, SJS_PATH_LOAD, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
    if (logical_operator(k.assign)) {
        uint8_t opcode = k.assign == SJS_OP_AND ? SJS_OP_JUMP_FALSY_KEEP
                       : (k.assign == SJS_OP_OR ? SJS_OP_JUMP_TRUTHY_KEEP : SJS_OP_JUMP_DEFINED_KEEP);
        if (jump(c, ins(opcode, 0, e->at), &k.jump) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    return push_construct(c, k, e->at);
}

/* ASSIGN_END: the value is on the stack. */
static sjs_result assign_end(sjs_compiler *c, const sjs_event *e) {
    if (expect_construct(c, K_ASSIGN, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    construct k = pop_construct(c);
    if (e->count != k.assign) return out_of_order(c, e->at);
    if (k.keys == 0) append_move(c, &k); /* MUTATION: appending to a let */
    if (k.assign != 0 && !logical_operator(k.assign)) {
        if (op(c, SJS_OP_BINARY_, e->at) != SJS_OK || u8(c, k.assign) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    if (target_store(c, &k, e) != SJS_OK) return SJS_ERR_PROGRAM;
    if (!logical_operator(k.assign)) return SJS_OK;
    /* The target's value decided: it goes, with the path. */
    uint32_t after;
    if (jump(c, ins(SJS_OP_JUMP, 0, e->at), &after) != SJS_OK) return SJS_ERR_PROGRAM;
    land(c, k.jump);
    if (k.keys == 0 ? op(c, SJS_OP_POP, e->at) != SJS_OK : emit_path(c, &k, ins(SJS_OP_PATH, SJS_PATH_DROP, e->at)) != SJS_OK) {
        return SJS_ERR_PROGRAM;
    }
    land(c, after);
    return SJS_OK;
}

/* UPDATE: `t++` or `t--`, a statement. ++ subtracts -1, as JavaScript adds
 * to a number only; staticjs's + would join a string. */
static sjs_result compile_update(sjs_compiler *c, const sjs_event *e) {
    if (!statement_level(c) || (e->count != SJS_OP_ADD && e->count != SJS_OP_SUBTRACT)) return out_of_order(c, e->at);
    construct k = { 0 };
    k.assign = SJS_OP_SUBTRACT; /* it reads its target */
    uint8_t taken = assign_target(c, e, &k);
    if (taken != TARGET_TAKEN) return taken == TARGET_REFUSED ? SJS_OK : SJS_ERR_PROGRAM;
    if (k.keys != 0 && emit_path(c, &k, ins(SJS_OP_PATH, SJS_PATH_LOAD, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
    if (number_constant(c, e->count == SJS_OP_ADD ? -1.0 : 1.0, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    if (op(c, SJS_OP_BINARY_, e->at) != SJS_OK || u8(c, SJS_OP_SUBTRACT) != SJS_OK) return SJS_ERR_PROGRAM;
    return target_store(c, &k, e);
}

/* MUTATION: DELETE, `delete t;` (docs/mutation.md, "delete"): the target
 * is a path with one key or more, under the rules of an assignment's, and
 * PATH removes the property its keys name. The parser refuses a name. */
static sjs_result compile_delete(sjs_compiler *c, const sjs_event *e) {
    if (!statement_level(c)) return out_of_order(c, e->at);
    const path *p = top_path(c);
    if (p == NULL || p->end != here(c)) {
        return refuse_later(c, e->start, (refusal){ "staticjs deletes only a property of an object, on a path from a variable the function owns", SJS_STR("") });
    }
    if (SJS_BUF_COUNT(c->path_steps, uint32_t) == p->steps_base) return out_of_order(c, e->at);
    construct k = { 0 };
    uint8_t taken = assign_target(c, e, &k);
    if (taken != TARGET_TAKEN) return taken == TARGET_REFUSED ? SJS_OK : SJS_ERR_PROGRAM;
    return emit_path(c, &k, ins(SJS_OP_PATH, SJS_PATH_DELETE, e->at));
}

/* BLOCK_END: falling off the end of a body returns undefined. */
static sjs_result compile_body_end(sjs_compiler *c, const sjs_event *e) {
    if (expect_construct(c, K_FUNCTION, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    if (!statement_level(c)) return out_of_order(c, e->at);
    if (ends_in_return(c)) return SJS_OK;
    if (op(c, SJS_OP_UNDEFINED_, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    return op(c, SJS_OP_RETURN, e->at);
}

static sjs_result compile_body_begin(sjs_compiler *c, const sjs_event *e) {
    if (expect_construct(c, K_FUNCTION, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    scope *s = current(c);
    if (s->kind != SC_FUNCTION || s->params_open || s->block || scope_count(c) < 2) return out_of_order(c, e->at);
    s->block = 1;
    return SJS_OK;
}

static sjs_result compile_const_begin(sjs_compiler *c, const sjs_event *e) {
    /* In a function, a const is a statement of its block body or of a block
     * in it; never the single statement of an if or an else, which
     * JavaScript refuses, and whose binding would outlive its branch. */
    if (scope_count(c) > 1 && !statement_level(c)) return out_of_order(c, e->at);
    const construct *top = top_construct(c);
    if (top != NULL && top->kind == K_IF) return out_of_order(c, e->at);
    construct k = { 0 };
    k.kind = K_CONST;
    k.exported = c->exporting ? 1u : 0u;
    c->exporting = false;
    /* MUTATION: let (count 1), and let with no value (2), whose value is
     * undefined; in a function only. Restore pushing the construct alone. */
    if (e->count > 2u || (e->count != 0 && scope_count(c) == 1)) return out_of_order(c, e->at);
    k.declares_let = e->count != 0 ? 1u : 0u;
    if (e->count == 2u && op(c, SJS_OP_UNDEFINED_, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    return push_construct(c, k, e->at);
}

static sjs_result end(sjs_compiler *c, const sjs_event *e) {
    if (SJS_BUF_COUNT(c->constructs, construct) != 0 || scope_count(c) != 1) {
        return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_SYNTAX, "the events are out of order" });
    }
    sjs_result r;
    if (c->goal == SJS_GOAL_MODULE) r = module_end(c, e->at);
    else if (c->unbound) r = fail_unbound(c);
    else r = op(c, SJS_OP_RETURN, e->at);
    if (r != SJS_OK) return SJS_ERR_PROGRAM;
    if (close_function_scope(c, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    c->ended = true;
    return SJS_OK;
}

/* `new Temporal.PlainDate(...)` and `new Date(...)`: a call of the type's
 * constructor, a function the function table holds under the space "new",
 * which no name reaches. The callee goes first, as a call's does; NEW_END
 * makes the call. Where the program binds Temporal (or Date) itself, `new`
 * would construct from its value, which staticjs does not. */
static sjs_result new_builtin(sjs_compiler *c, const sjs_event *e, sjs_str name) {
    uint32_t found;
    bool bound = index_find(&c->global_index, name, &found);
    for (uint32_t s = scope_count(c); !bound && s-- > 0;) bound = index_find(&SJS_BUF_AT(c->scopes, scope, s).names, name, &found);
    sjs_builtin_id id = sjs_builtin_lookup(SJS_STR("new"), e->text);
    if (bound || id == SJS_BUILTIN_NONE) {
        return fail_name(c, e, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs constructs only Set and Map, Date, and the Temporal types it has" });
    }
    uint32_t index;
    if (value_constant(c, sjs_builtin_value(id), &index) != SJS_OK) return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_MEMORY, "out of memory" });
    if (op32(c, ins(SJS_OP_CONSTANT, index, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
    construct k = { 0 };
    k.kind = K_CALL;
    k.node = e->node;
    c->last_op_valid = false;
    return push_construct(c, k, e->at);
}

static sjs_result event(sjs_compiler *c, const sjs_event *e) {
    /* `String`, `Number`, `BigInt` and `Date` are functions as well as
     * namespaces: unless a member follows, each is the function, read where
     * the name was (Date's faults when called: docs/date.md). */
    if ((c->space == NS_STRING || c->space == NS_NUMBER || c->space == NS_BIGINT || c->space == NS_DATE) && e->kind != SJS_EV_MEMBER) {
        sjs_builtin_id id = c->space == NS_STRING ? SJS_BUILTIN_STRING : c->space == NS_NUMBER ? SJS_BUILTIN_NUMBER
                          : c->space == NS_BIGINT ? SJS_BUILTIN_BIGINT : SJS_BUILTIN_DATE_CALL;
        c->space = NS_NONE;
        uint32_t index;
        if (value_constant(c, sjs_builtin_value(id), &index) != SJS_OK) return compile_fail(c, c->space_at, (sjs_problem){ SJS_ERROR_MEMORY, "out of memory" });
        if (op32(c, ins(SJS_OP_CONSTANT, index, c->space_at)) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    if (e->kind == SJS_EV_ARGUMENT || e->kind == SJS_EV_SPREAD_ARGUMENT) assign_argument(c, e); /* MUTATION: Object.assign */
    if (c->pending_read != 0) note_use(c, e->kind); /* MUTATION */
    if (c->paths.length != 0) paths_before(c, e); /* MUTATION */
    if (c->space != NS_NONE && e->kind != SJS_EV_MEMBER) {
        return compile_fail(c, c->space_at, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "Math, Object and Array are used only as Math.name" });
    }
    if (is_pattern_event(e->kind)) return pattern_event(c, e);
    switch (e->kind) {
    case SJS_EV_NUMBER:
        return number_constant(c, e->number, e->at);
    case SJS_EV_BIGINT:
        return bigint_constant(c, e);
    case SJS_EV_REGEXP:
        return regex_constant(c, e);
    case SJS_EV_STRING: {
        uint32_t index;
        if (string_constant(c, e, &index) != SJS_OK) return SJS_ERR_PROGRAM;
        return op32(c, ins(SJS_OP_CONSTANT, index, e->at));
    }
    case SJS_EV_TRUE:
        return op(c, SJS_OP_TRUE_, e->at);
    case SJS_EV_FALSE:
        return op(c, SJS_OP_FALSE_, e->at);
    case SJS_EV_NULL:
        return op(c, SJS_OP_NULL_, e->at);
    case SJS_EV_NAME:
        return read_name(c, e);
    case SJS_EV_TEMPLATE_BEGIN:
        return SJS_OK;
    case SJS_EV_TEMPLATE_END:
        if (e->count > UINT16_MAX) return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_LIMIT, "a template has too many parts" });
        return op16(c, ins(SJS_OP_CONCAT, e->count, e->at));
    case SJS_EV_ARRAY_BEGIN:
        return op(c, SJS_OP_ARRAY_NEW, e->at);
    case SJS_EV_ELEMENT:
        return op(c, SJS_OP_ARRAY_PUSH, e->at);
    case SJS_EV_SPREAD_ELEMENT:
        return op(c, SJS_OP_ARRAY_SPREAD, e->at);
    case SJS_EV_ARRAY_END:
        c->last_op_valid = false;
        return SJS_OK;
    case SJS_EV_OBJECT_END:
        return object_end(c, e);
    case SJS_EV_OBJECT_BEGIN:
        return object_begin(c, e);
    case SJS_EV_KEY:
        return object_key(c, e);
    case SJS_EV_COMPUTED_KEY:
        /* The key is on the stack, above the values gathered. */
        if (object_make(c, e->at, SJS_LITERAL_ABOVE | SJS_LITERAL_OPEN) != SJS_OK) return SJS_ERR_PROGRAM;
        return op(c, SJS_OP_TO_KEY, e->at);
    case SJS_EV_PROPERTY:
        return object_property(c, e);
    case SJS_EV_SPREAD_PROPERTY:
        if (object_make(c, e->at, SJS_LITERAL_ABOVE | SJS_LITERAL_OPEN) != SJS_OK) return SJS_ERR_PROGRAM;
        return op(c, SJS_OP_OBJECT_SPREAD, e->at);
    case SJS_EV_MEMBER: {
        if (c->space != NS_NONE) return read_space_member(c, e);
        uint32_t index;
        if (string_constant(c, e, &index) != SJS_OK) return SJS_ERR_PROGRAM;
        if (op32(c, ins(SJS_OP_GET_MEMBER, index, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
        return path_step(c, e, c->last_op); /* MUTATION: restore returning the op32 */
    }
    case SJS_EV_INDEX:
        if (op(c, SJS_OP_GET_INDEX, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
        return path_step(c, e, c->last_op); /* MUTATION: restore returning the op */
    case SJS_EV_OPTIONAL:
        return compile_optional(c, e);
    case SJS_EV_CHAIN_END:
        return chain_end(c, e);
    case SJS_EV_CALL_BEGIN:
        return call_begin(c, e);
    case SJS_EV_ARGUMENT:
        return SJS_OK;
    case SJS_EV_SPREAD_ARGUMENT:
        return spread_argument(c, e);
    case SJS_EV_CALL_END:
        return call_end(c, e, SJS_OP_CALL);
    case SJS_EV_NEW_BEGIN: {
        if (e->text.length > 9 && sjs_str_equal((sjs_str){ e->text.ptr, 9 }, SJS_STR("Temporal."))) return new_builtin(c, e, SJS_STR("Temporal"));
        if (sjs_str_equal(e->text, SJS_STR("Date"))) return new_builtin(c, e, SJS_STR("Date"));
        construct k = { 0 };
        k.kind = K_NEW;
        k.map = sjs_str_equal(e->text, SJS_STR("Map")) ? 1u : 0u;
        return push_construct(c, k, e->at);
    }
    case SJS_EV_NEW_END: {
        const construct *k = top_construct(c);
        if (k != NULL && k->kind == K_CALL) return call_end(c, e, SJS_OP_CALL); /* new Temporal.X */
        return call_end(c, e, k != NULL && k->kind == K_NEW && k->map ? SJS_OP_NEW_MAP : SJS_OP_NEW_SET);
    }
    case SJS_EV_UNARY:
        if (op(c, SJS_OP_UNARY_, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
        return u8(c, (uint8_t)e->count);
    case SJS_EV_BINARY:
        if (op(c, SJS_OP_BINARY_, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
        return u8(c, (uint8_t)e->count);
    case SJS_EV_LOGICAL:
        return compile_logical(c, e);
    case SJS_EV_LOGICAL_END:
        if (expect_construct(c, K_LOGICAL, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
        land(c, pop_construct(c).jump);
        return SJS_OK;
    case SJS_EV_CONDITIONAL: {
        construct k = { 0 };
        k.kind = K_CONDITIONAL;
        if (jump(c, ins(SJS_OP_JUMP_FALSY, 0, e->at), &k.jump) != SJS_OK) return SJS_ERR_PROGRAM;
        k.branch_pc = here(c); /* MUTATION: end_branch */
        k.parked_base = SJS_BUF_COUNT(c->parked_reads, parked_read); /* MUTATION */
        return push_construct(c, k, e->at);
    }
    case SJS_EV_CONDITIONAL_ELSE: {
        if (expect_construct(c, K_CONDITIONAL, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
        construct *k = top_construct(c);
        if (end_branch(c, (branch_end){ k->branch_pc, k->parked_base, false }, e->at) != SJS_OK) return SJS_ERR_PROGRAM; /* MUTATION */
        uint32_t after;
        if (jump(c, ins(SJS_OP_JUMP, 0, e->at), &after) != SJS_OK) return SJS_ERR_PROGRAM;
        k = top_construct(c);
        k->jump_else = after;
        land(c, k->jump);
        return SJS_OK;
    }
    case SJS_EV_CONDITIONAL_END: {
        if (expect_construct(c, K_CONDITIONAL, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
        construct k = pop_construct(c);
        fence_parked(c, k.parked_base, 0); /* MUTATION: end_branch */
        land(c, k.jump_else);
        return SJS_OK;
    }
    case SJS_EV_ARROW_BEGIN:
        return begin_function(c, e);
    case SJS_EV_PARAMS_END:
        return params_end(c, e);
    case SJS_EV_DEFAULT:
        return default_begin(c, e);
    case SJS_EV_DEFAULT_END:
        return default_end(c, e);
    case SJS_EV_ARROW_END:
        return end_function(c, e->at);
    case SJS_EV_BLOCK_BEGIN:
        return compile_body_begin(c, e);
    case SJS_EV_RETURN:
        return compile_return(c, e);
    case SJS_EV_BLOCK_END:
        return compile_body_end(c, e);
    case SJS_EV_BLOCK_STATEMENT_BEGIN:
        return compile_block_begin(c, e);
    case SJS_EV_BLOCK_STATEMENT_END:
        return compile_block_end(c, e);
    case SJS_EV_IF:
        return compile_if(c, e);
    case SJS_EV_ELSE:
        return compile_else(c, e);
    case SJS_EV_IF_END:
        return compile_if_end(c, e);
    case SJS_EV_EXPRESSION_STATEMENT: /* MUTATION */
        return compile_expression_statement(c, e);
    case SJS_EV_ASSIGN: /* MUTATION */
        return assign_begin(c, e);
    case SJS_EV_ASSIGN_END: /* MUTATION */
        return assign_end(c, e);
    case SJS_EV_UPDATE: /* MUTATION */
        return compile_update(c, e);
    case SJS_EV_DELETE: /* MUTATION */
        return compile_delete(c, e);
    case SJS_EV_CONST_BEGIN:
        return compile_const_begin(c, e);
    case SJS_EV_CONST_END:
        return const_end(c, e);
    case SJS_EV_EXPORT:
        c->exporting = true;
        return SJS_OK;
    case SJS_EV_IMPORT:
        return import(c, e);
    case SJS_EV_EXPORT_DEFAULT:
        return export_default(c, e);
    case SJS_EV_EXPORT_NAME:
        return export_local(c, e);
    case SJS_EV_EXPORT_FROM:
        return export_from(c, e);
    case SJS_EV_END:
        return end(c, e);
    default:
        return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_SYNTAX, "unknown event" });
    }
}

static sjs_result on_event(void *self, const sjs_event *e) {
    sjs_compiler *c = self;
    if (c->failed) return SJS_ERR_PROGRAM;
    if (c->ended) return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_SYNTAX, "an event after the end" });
    if (c->refusing) {
        /* MUTATION: refuse_later: read on, and report it at the end. */
        if (e->kind != SJS_EV_END) return SJS_OK;
        c->error = c->refusal;
        c->error.error.subject.ptr = c->error.subject;
        c->failed = true;
        return SJS_ERR_PROGRAM;
    }
    { /* MUTATION: always, for the rewrites. Restore `if (c->matching)`. */
        /* The matcher sees the event first: a report about the function an
         * ARROW_END closes comes while it is still open here; one about a
         * rewrite, while the node's construct is (MUTATION). */
        sjs_result r = sjs_matcher_event(&c->matcher, e);
        if (r != SJS_OK) return c->failed ? SJS_ERR_PROGRAM : fail_result(c, r, e->at);
    }
    return event(c, e);
}

static const sjs_sink_vtable sink_vtable = { on_event };

/* A text a shape records, as an event string_constant finds its constant
 * by: the one the body's GET_MEMBER or CONSTANT already uses. */
static sjs_event shape_text(const sjs_shape_match *match, sjs_str text) {
    sjs_event e = { 0 };
    e.kind = SJS_EV_STRING;
    e.text = text;
    e.at = match->start;
    return e;
}

/* The matcher found that the innermost function computes a shape: record
 * it, with its arguments, on the function's info. A report for any other
 * node is ignored; the parser's ids make that never happen. */
static sjs_result on_shape(void *self, const sjs_shape_match *match) {
    sjs_compiler *c = self;
    if (c->failed) return SJS_ERR_PROGRAM;
    if (scope_count(c) < 2 || function_scope(c)->node != match->node) return SJS_OK;
    uint32_t function = function_scope(c)->function;
    uint32_t args[SJS_SHAPE_ARGS] = { 0 };
    for (uint32_t i = 0; i < SJS_SHAPE_ARGS; i++) {
        const sjs_shape_value *arg = &match->args[i];
        sjs_event text = shape_text(match, arg->text);
        switch (sjs_shape_arg_kind(match->shape, i)) {
        case SJS_SHAPE_ARG_TEXT:
            /* The key's string constant: the one GET_MEMBER reads it with. */
            if (string_constant(c, &text, &args[i]) != SJS_OK) return SJS_ERR_PROGRAM;
            break;
        case SJS_SHAPE_ARG_NUMBER: {
            sjs_result r = number_index(c, arg->number, &args[i]);
            if (r != SJS_OK) return fail_result(c, r, match->start);
            break;
        }
        case SJS_SHAPE_ARG_OPERAND: {
            /* The constant the literal's own NUMBER_ or CONSTANT pushes: the
             * body is compiled, so it is found, not made. */
            uint32_t index = 0;
            if (arg->operand == SJS_OPERAND_NUMBER) {
                sjs_result r = number_index(c, arg->number, &index);
                if (r != SJS_OK) return fail_result(c, r, match->start);
            } else if (arg->operand == SJS_OPERAND_TEXT && string_constant(c, &text, &index) != SJS_OK) {
                return SJS_ERR_PROGRAM;
            }
            /* Too many constants to say which: the function stays untagged. */
            if (index >= SJS_SHAPE_OPERAND_INDICES) return SJS_OK;
            args[i] = sjs_shape_operand(arg->operand, index);
            break;
        }
        default:
            args[i] = arg->count;
            break;
        }
    }
    sjs_function_info *info = &SJS_BUF_AT(c->functions, sjs_function_info, function);
    info->shape = (uint8_t)match->shape;
    for (uint32_t i = 0; i < SJS_SHAPE_ARGS; i++) info->shape_args[i] = args[i];
    return SJS_OK;
}

/* MUTATION: the matcher found that a rewrite's rules match the node the
 * event it is about to hand over ends (docs/matcher.md, section 12). The
 * construct of that node takes it, and its end compiles it. A report for
 * any other node is ignored; the parser's ids make that never happen. */
static sjs_result on_rewrite(void *self, const sjs_rewrite_match *match) {
    sjs_compiler *c = self;
    if (c->failed) return SJS_ERR_PROGRAM;
    construct *k = top_construct(c);
    if (match->rewrite == SJS_REWRITE_MUTATE_PATH && k != NULL && k->kind == K_CALL && k->node == match->node) {
        k->mutate_path = 1;
    }
    /* MUTATION: appending to a let. */
    if (match->rewrite == SJS_REWRITE_APPEND && k != NULL && k->kind == K_ASSIGN && k->node == match->node) k->append = 1;
    if (match->rewrite == SJS_REWRITE_APPEND_SUM) append_sum(c, match->start.offset);
    return SJS_OK;
}

static const sjs_shape_reporter_vtable shape_reporter_vtable = { on_shape, on_rewrite /* MUTATION */ };

/* ------------------------------------------------------------------------
 * The public API
 */

sjs_result sjs_compiler_init(sjs_compiler **out, const sjs_compiler_config *config) {
    if (out == NULL || config == NULL || config->context == NULL || (config->globals == NULL && config->global_count != 0)) {
        return SJS_ERR_INVALID;
    }
    sjs_compiler *c = sjs_mem_alloc(&config->context->counting, sizeof *c);
    if (c == NULL) return SJS_ERR_NOMEM;
    sjs_zero(c, sizeof *c);
    c->context = config->context;
    c->goal = config->goal;
    c->globals = config->globals;
    c->global_count = config->global_count;
    c->modules = config->modules;
    c->number_index = index_over(&c->numbers);
    c->string_index = index_over(&c->string_text);
    c->global_index = index_over(&c->global_text);
    c->sum.start = UINT32_MAX; /* MUTATION: no APPEND_SUM yet */
    c->export_names = index_over(&c->names);
    c->star_modules = index_over(&c->names);
    c->namespaces = index_over(&c->names);
    /* MUTATION: a matcher with no_shapes too, for the rewrites alone.
     * Restore: none with no_shapes. */
    sjs_matcher_config matcher = { alloc(c), c->context->limits.max_nesting, { c, &shape_reporter_vtable }, config->no_shapes };
    sjs_matcher_init(&c->matcher, &matcher);
    sjs_result r = SJS_OK;
    for (uint32_t i = 0; i < c->global_count && r == SJS_OK; i++) {
        /* A name given twice is the first. */
        uint32_t seen;
        if (index_find(&c->global_index, c->globals[i], &seen)) continue;
        uint32_t offset = c->global_text.length;
        r = buf_add(c, &c->global_text, bytes_of(c->globals[i].ptr, c->globals[i].length));
        if (r == SJS_OK) r = index_put(c, &c->global_index, at_offset(offset, c->globals[i].length, i));
    }
    sjs_position start = { 0, 1, 1 };
    if (r == SJS_OK && open_function_scope(c, start) != SJS_OK) r = SJS_ERR_NOMEM;
    if (r != SJS_OK) {
        sjs_compiler_destroy(c);
        return r;
    }
    *out = c;
    return SJS_OK;
}

sjs_sink sjs_compiler_sink(sjs_compiler *compiler) {
    return (sjs_sink){ compiler, &sink_vtable };
}

/* One allocation holds the program and every table it reads. */
static size_t align8(size_t n) {
    return (n + 7u) & ~(size_t)7u;
}

sjs_result sjs_compiler_finish(sjs_compiler *compiler, sjs_program **out) {
    if (compiler == NULL || out == NULL) return SJS_ERR_INVALID;
    if (compiler->failed) return SJS_ERR_PROGRAM;
    if (!compiler->ended || compiler->finished) return SJS_ERR_INVALID;
    sjs_compiler *c = compiler;
    size_t size = align8(sizeof(sjs_program));
    size_t at_numbers = size;
    size += align8(c->numbers.length);
    size_t at_constants = size;
    size += align8(c->constants.length);
    size_t at_functions = size;
    size += align8(c->functions.length);
    size_t at_captures = size;
    size += align8(c->captures.length);
    size_t at_lines = size;
    size += align8(c->lines.length);
    size_t at_code = size;
    size += align8(c->code.length);
    uint8_t *block = sjs_mem_alloc(alloc(c), size);
    if (block == NULL) return SJS_ERR_NOMEM;
    sjs_zero(block, size);
    sjs_program *p = (sjs_program *)(void *)block;
    p->refs = 1;
    p->goal = c->goal;
    p->context = c->context;
    p->size = size;
    sjs_copy(block + at_numbers, c->numbers.ptr, c->numbers.length);
    sjs_copy(block + at_constants, c->constants.ptr, c->constants.length);
    sjs_copy(block + at_functions, c->functions.ptr, c->functions.length);
    sjs_copy(block + at_captures, c->captures.ptr, c->captures.length);
    sjs_copy(block + at_lines, c->lines.ptr, c->lines.length);
    sjs_copy(block + at_code, c->code.ptr, c->code.length);
    p->numbers = (const double *)(void *)(block + at_numbers);
    p->number_count = SJS_BUF_COUNT(c->numbers, double);
    p->constants = (sjs_value *)(void *)(block + at_constants);
    p->constant_count = SJS_BUF_COUNT(c->constants, sjs_value);
    p->functions = (const sjs_function_info *)(void *)(block + at_functions);
    p->function_count = SJS_BUF_COUNT(c->functions, sjs_function_info);
    p->captures = (const sjs_capture_info *)(void *)(block + at_captures);
    p->capture_count = SJS_BUF_COUNT(c->captures, sjs_capture_info);
    p->lines = (const sjs_line *)(void *)(block + at_lines);
    p->line_count = SJS_BUF_COUNT(c->lines, sjs_line);
    p->code = block + at_code;
    p->code_length = c->code.length;
    p->global_count = c->global_count;
    /* The program owns the constants now. */
    c->constants.length = 0;
    c->finished = true;
    *out = p;
    return SJS_OK;
}

sjs_result sjs_compiler_error(const sjs_compiler *compiler, sjs_error *out) {
    if (compiler == NULL || out == NULL) return SJS_ERR_INVALID;
    if (!compiler->failed) return SJS_ERR_NOT_FOUND;
    *out = compiler->error.error;
    return SJS_OK;
}

void sjs_compiler_destroy(sjs_compiler *compiler) {
    if (compiler == NULL) return;
    sjs_compiler *c = compiler;
    const sjs_allocator *a = alloc(c);
    uint32_t n = SJS_BUF_COUNT(c->constants, sjs_value);
    for (uint32_t i = 0; i < n; i++) sjs_release(c->context, SJS_BUF_AT(c->constants, sjs_value, i));
    uint32_t scopes = SJS_BUF_COUNT(c->scopes, scope);
    for (uint32_t i = 0; i < scopes; i++) {
        index_free(c, &SJS_BUF_AT(c->scopes, scope, i).names);
        index_free(c, &SJS_BUF_AT(c->scopes, scope, i).read);
    }
    name_index *indices[] = { &c->number_index, &c->string_index, &c->global_index, &c->export_names, &c->star_modules, &c->namespaces };
    for (uint32_t i = 0; i < sizeof indices / sizeof indices[0]; i++) index_free(c, indices[i]);
    sjs_buf *bufs[] = { &c->code, &c->numbers, &c->constants, &c->functions, &c->captures, &c->lines,
                        &c->scopes, &c->bindings, &c->names, &c->pending_captures, &c->last_reads, &c->string_text, &c->global_text,
                        &c->constructs, &c->jumps, &c->patterns, &c->pattern_keys, &c->pattern_slots, &c->literals, &c->literal_keys, &c->exports,
                        &c->stars, &c->sorting, &c->paths, &c->path_steps, &c->parked_reads /* MUTATION */ };
    for (uint32_t i = 0; i < sizeof bufs / sizeof bufs[0]; i++) sjs_buf_free(a, bufs[i]);
    sjs_matcher_free(&c->matcher); /* MUTATION: always made */
    sjs_mem_free(a, c, sizeof *c);
}

sjs_position sjs_program_position(const sjs_program *program, uint32_t pc) {
    sjs_position at = { 0, 1, 1 };
    for (uint32_t i = 0; i < program->line_count; i++) {
        if (program->lines[i].pc > pc) break;
        at = program->lines[i].at;
    }
    return at;
}

/* ------------------------------------------------------------------------
 * Parse and compile in one call
 */

sjs_result sjs_compile(const sjs_compile_params *params, sjs_program **out) {
    if (params == NULL || out == NULL || params->compiler.context == NULL) return SJS_ERR_INVALID;
    sjs_compiler *compiler;
    sjs_result r = sjs_compiler_init(&compiler, &params->compiler);
    if (r != SJS_OK) return r;
    sjs_parser_config config = { params->compiler.context->counting, params->compiler.context->limits,
                                 params->compiler.goal, sjs_compiler_sink(compiler) };
    sjs_parser *parser;
    r = sjs_parser_init(&parser, &config);
    if (r != SJS_OK) {
        sjs_compiler_destroy(compiler);
        return r;
    }
    r = sjs_parser_push(parser, (sjs_bytes){ (const uint8_t *)params->source.ptr, params->source.length });
    if (r == SJS_OK) r = sjs_parser_finish(parser);
    if (r == SJS_OK) r = sjs_compiler_finish(compiler, out);
    if (r == SJS_ERR_PROGRAM && params->error != NULL) {
        if (sjs_parser_error(parser, params->error) != SJS_OK) sjs_compiler_error(compiler, params->error);
    }
    sjs_parser_destroy(parser);
    sjs_compiler_destroy(compiler);
    return r;
}

/*
 * The compiler: a sink of syntax events that emits bytecode as each event
 * arrives. Events come in evaluation order, so most become one instruction;
 * the branching constructs keep their jumps on a stack and patch them when
 * their end arrives.
 *
 * Names are resolved when they are read. A function captures the values of
 * the names it uses from enclosing functions when it is created: every
 * binding is const, so a copy is the same as a reference. A name may not be
 * used before its const declaration or import, which also makes a function
 * unable to refer to itself. A name bound nowhere is reported at the end, so
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
 * value, which every binding being const makes the same as a reference.
 */
#include "internal.h"

/* Kinds of binding. */
enum { B_LOCAL = 0, B_CONSTANT };

typedef struct {
    uint32_t name_offset;
    uint32_t name_length;
    uint32_t index;        /* local slot, or constant */
    uint8_t kind;
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

typedef struct {
    uint32_t function;
    uint32_t bindings_base;
    uint32_t jump_over;    /* operand of the jump around the body */
    uint16_t params;       /* as ARROW_BEGIN said, a rest parameter included */
    uint16_t params_seen;
    uint16_t slots;
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
    uint8_t block;
    uint8_t returned;
    sjs_position at;
    name_index names;      /* its bindings, by name */
    name_index read;       /* names read in it, or in a function it holds, that it did not bind then */
} scope;

/* A capture a function being compiled takes from the one that creates it. */
typedef struct {
    uint32_t scope;
    uint32_t binding;      /* the binding captured, where it is declared */
    uint16_t from_capture; /* the source is the creator's capture, not its local */
    uint16_t source;       /* the creator's slot or capture */
    uint16_t own;          /* the capture's index in its function */
} capture;

/* Open constructs. */
enum { K_LOGICAL = 1, K_CONDITIONAL, K_CHAIN, K_CALL, K_NEW, K_FUNCTION, K_CONST, K_TEMPLATE, K_DEFAULT };

typedef struct {
    uint8_t kind;
    uint8_t method;
    uint8_t spread;
    uint8_t exported;      /* K_CONST: export what it binds */
    uint32_t jump;         /* an operand to patch */
    uint32_t jump_else;
    uint32_t depth;        /* K_CHAIN: the chain's depth */
    uint32_t jumps_base;   /* K_CHAIN: its jumps */
    uint32_t param;        /* K_DEFAULT: the parameter's slot + 1, or 0 for an element's */
} construct;

/* An array or object pattern being compiled. An object pattern keeps its
 * keys, for a rest element to leave out: the constants of the static ones,
 * and the slots that hold the computed ones. */
enum { PK_ARRAY = 0, PK_OBJECT };

typedef struct {
    uint8_t kind;
    uint32_t keys_base;    /* in pattern_keys */
    uint32_t slots_base;   /* in pattern_slots */
} pattern_frame;

enum { NS_NONE = 0, NS_MATH, NS_OBJECT, NS_NUMBER, NS_ARRAY };

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
static sjs_result number_constant(sjs_compiler *c, double value, sjs_position at) {
    uint32_t index;
    if (!index_find(&c->number_index, (sjs_str){ (const char *)&value, sizeof value }, &index)) {
        index = SJS_BUF_COUNT(c->numbers, double);
        sjs_result r = buf_add(c, &c->numbers, bytes_of(&value, sizeof value));
        if (r == SJS_OK) r = index_put(c, &c->number_index, at_offset(index * (uint32_t)sizeof value, sizeof value, index));
        if (r != SJS_OK) return fail_result(c, r, at);
    }
    return op32(c, ins(SJS_OP_NUMBER_, index, at));
}

/* The constant holding `value`; takes over the caller's reference. */
static sjs_result value_constant(sjs_compiler *c, sjs_value value, uint32_t *index) {
    *index = SJS_BUF_COUNT(c->constants, sjs_value);
    sjs_result r = buf_add(c, &c->constants, bytes_of(&value, sizeof value));
    if (r != SJS_OK) sjs_release(c->context, value);
    return r;
}

static sjs_result string_constant(sjs_compiler *c, const sjs_event *e, uint32_t *index) {
    sjs_str text = e->text;
    sjs_position at = e->at;
    if (index_find(&c->string_index, text, index)) return SJS_OK;
    sjs_string *s;
    sjs_result r = sjs_string_make(c->context, text, &s);
    if (r == SJS_OK) r = value_constant(c, sjs_heap_value(SJS_STRING, &s->h), index);
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

static sjs_result open_scope(sjs_compiler *c, sjs_position at) {
    sjs_function_info info = { 0 };
    scope s = { 0 };
    s.function = SJS_BUF_COUNT(c->functions, sjs_function_info);
    s.bindings_base = SJS_BUF_COUNT(c->bindings, binding);
    s.at = at;
    s.names = index_over(&c->names);
    s.read = index_over(&c->names);
    info.code = here(c);
    sjs_result r = buf_add(c, &c->functions, bytes_of(&info, sizeof info));
    if (r == SJS_OK) r = buf_add(c, &c->scopes, bytes_of(&s, sizeof s));
    return r == SJS_OK ? SJS_OK : fail_result(c, r, at);
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

static sjs_result new_slot(sjs_compiler *c, sjs_position at, uint32_t *slot) {
    scope *s = current(c);
    if (s->slots == UINT16_MAX) return compile_fail(c, at, (sjs_problem){ SJS_ERROR_LIMIT, "a function has more than 65535 names" });
    *slot = s->slots++;
    return SJS_OK;
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

/* Read a name: a local, a capture, a constant, a global or a built-in. */
static sjs_result read_name(sjs_compiler *c, const sjs_event *e) {
    uint32_t count = scope_count(c);
    uint32_t inner = count - 1;
    for (uint32_t s = count; s-- > 0;) {
        uint32_t i;
        if (index_find(&SJS_BUF_AT(c->scopes, scope, s).names, e->text, &i)) {
            binding *b = &SJS_BUF_AT(c->bindings, binding, i);
            if (b->kind == B_CONSTANT) return op32(c, ins(SJS_OP_CONSTANT, b->index, e->at));
            if (s == inner) return op16(c, ins(SJS_OP_LOCAL, b->index, e->at));
            /* Captured: through every function between. */
            uint16_t from_capture = 0;
            uint32_t index = b->index;
            for (uint32_t t = s + 1; t <= inner; t++) {
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
            return op16(c, ins(SJS_OP_CAPTURE, index, e->at));
        }
        if (note_outer_ref(c, s, e) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    uint32_t global;
    if (index_find(&c->global_index, e->text, &global)) return op16(c, ins(SJS_OP_GLOBAL, global, e->at));
    if (sjs_str_equal(e->text, SJS_STR("undefined"))) return op(c, SJS_OP_UNDEFINED_, e->at);
    if (sjs_str_equal(e->text, SJS_STR("NaN"))) return number_constant(c, 0.0 / 0.0, e->at);
    if (sjs_str_equal(e->text, SJS_STR("Infinity"))) return number_constant(c, 1.0 / 0.0, e->at);
    static const struct { const char *name; uint8_t space; } spaces[] = {
        { "Math", NS_MATH }, { "Object", NS_OBJECT }, { "Number", NS_NUMBER }, { "Array", NS_ARRAY }
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
    static const char *const space_names[] = { "", "Math", "Object", "Number", "Array" };
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
    if (open_scope(c, at) != SJS_OK) return SJS_ERR_PROGRAM;
    scope *s = current(c);
    s->params = (uint16_t)e->count;
    s->slots = (uint16_t)e->count;
    s->params_open = 1;
    return SJS_OK;
}

/* Close the innermost function: its captures go to the program's table. */
static sjs_result close_scope(sjs_compiler *c, sjs_position at) {
    scope s = *current(c);
    uint32_t scope_index = scope_count(c) - 1;
    sjs_function_info *info = &SJS_BUF_AT(c->functions, sjs_function_info, s.function);
    info->params = s.params;
    info->locals = s.slots;
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
    if (!s->block) {
        if (op(c, SJS_OP_RETURN, at) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    uint32_t function = s->function;
    if (close_scope(c, at) != SJS_OK) return SJS_ERR_PROGRAM;
    if (expect_construct(c, K_FUNCTION, at) != SJS_OK) return SJS_ERR_PROGRAM;
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
    if (k.param != 0 && op16(c, ins(SJS_OP_SET_LOCAL, k.param - 1u, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
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
    return op16(c, ins(SJS_OP_LOCAL, c->param_index, at));
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
    return op16(c, ins(SJS_OP_PATTERN_COMPUTED, slot, e->at));
}

/* The static keys of the innermost object pattern, as one array constant. */
static sjs_result static_keys(sjs_compiler *c, const pattern_frame *f, uint32_t *index) {
    uint32_t n = SJS_BUF_COUNT(c->pattern_keys, uint32_t) - f->keys_base;
    *index = UINT32_MAX;
    if (n == 0) return SJS_OK;
    sjs_array *keys;
    sjs_result r = sjs_array_make(c->context, n, &keys);
    if (r != SJS_OK) return r;
    for (uint32_t i = 0; i < n; i++) {
        sjs_value key = SJS_BUF_AT(c->constants, sjs_value, SJS_BUF_AT(c->pattern_keys, uint32_t, f->keys_base + i));
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
        if (op16(c, ins(SJS_OP_LOCAL, SJS_BUF_AT(c->pattern_slots, uint32_t, frame.slots_base + i), e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    uint32_t keys;
    sjs_result r = static_keys(c, &frame, &keys);
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
        if (op16(c, ins(SJS_OP_SET_LOCAL, slot, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
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
    if (op16(c, ins(SJS_OP_SET_LOCAL, x.index, e->at)) != SJS_OK) return SJS_ERR_PROGRAM;
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
    return op16(c, ins(SJS_OP_LOCAL, x->index, at));
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
 * Events
 */

static sjs_result call_begin(sjs_compiler *c, const sjs_event *e) {
    construct k = { 0 };
    k.kind = K_CALL;
    /* `a.b(...)` and `a[b](...)` call with a receiver. */
    if (c->last_op_valid && c->last_label <= c->last_op) {
        uint8_t *last = c->code.ptr + c->last_op;
        if (*last == SJS_OP_GET_MEMBER) {
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
    if (op(c, opcode, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    if (u16(c, (uint16_t)e->count) != SJS_OK) return SJS_ERR_PROGRAM;
    uint8_t flags = (uint8_t)((k.method ? SJS_CALL_METHOD : 0u) | (k.spread ? SJS_CALL_SPREAD : 0u));
    sjs_result r = u8(c, flags);
    c->last_op_valid = false;
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

static sjs_result end(sjs_compiler *c, const sjs_event *e) {
    if (SJS_BUF_COUNT(c->constructs, construct) != 0 || scope_count(c) != 1) {
        return compile_fail(c, e->at, (sjs_problem){ SJS_ERROR_SYNTAX, "the events are out of order" });
    }
    sjs_result r;
    if (c->goal == SJS_GOAL_MODULE) r = module_end(c, e->at);
    else if (c->unbound) r = fail_unbound(c);
    else r = op(c, SJS_OP_RETURN, e->at);
    if (r != SJS_OK) return SJS_ERR_PROGRAM;
    if (close_scope(c, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
    c->ended = true;
    return SJS_OK;
}

static sjs_result event(sjs_compiler *c, const sjs_event *e) {
    if (c->space != NS_NONE && e->kind != SJS_EV_MEMBER) {
        return compile_fail(c, c->space_at, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "Math, Object, Number and Array are used only as Math.name" });
    }
    if (is_pattern_event(e->kind)) return pattern_event(c, e);
    switch (e->kind) {
    case SJS_EV_NUMBER:
        return number_constant(c, e->number, e->at);
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
    case SJS_EV_OBJECT_END:
        c->last_op_valid = false;
        return SJS_OK;
    case SJS_EV_OBJECT_BEGIN:
        return op(c, SJS_OP_OBJECT_NEW, e->at);
    case SJS_EV_KEY: {
        uint32_t index;
        if (string_constant(c, e, &index) != SJS_OK) return SJS_ERR_PROGRAM;
        return op32(c, ins(SJS_OP_CONSTANT, index, e->at));
    }
    case SJS_EV_COMPUTED_KEY:
        return op(c, SJS_OP_TO_KEY, e->at);
    case SJS_EV_PROPERTY:
        return op(c, SJS_OP_OBJECT_PUT, e->at);
    case SJS_EV_SPREAD_PROPERTY:
        return op(c, SJS_OP_OBJECT_SPREAD, e->at);
    case SJS_EV_MEMBER: {
        if (c->space != NS_NONE) return read_space_member(c, e);
        uint32_t index;
        if (string_constant(c, e, &index) != SJS_OK) return SJS_ERR_PROGRAM;
        return op32(c, ins(SJS_OP_GET_MEMBER, index, e->at));
    }
    case SJS_EV_INDEX:
        return op(c, SJS_OP_GET_INDEX, e->at);
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
        construct k = { 0 };
        k.kind = K_NEW;
        return push_construct(c, k, e->at);
    }
    case SJS_EV_NEW_END:
        return call_end(c, e, SJS_OP_NEW_SET);
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
        return push_construct(c, k, e->at);
    }
    case SJS_EV_CONDITIONAL_ELSE: {
        if (expect_construct(c, K_CONDITIONAL, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
        construct *k = top_construct(c);
        uint32_t after;
        if (jump(c, ins(SJS_OP_JUMP, 0, e->at), &after) != SJS_OK) return SJS_ERR_PROGRAM;
        k = top_construct(c);
        k->jump_else = after;
        land(c, k->jump);
        return SJS_OK;
    }
    case SJS_EV_CONDITIONAL_END:
        if (expect_construct(c, K_CONDITIONAL, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
        land(c, pop_construct(c).jump_else);
        return SJS_OK;
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
        current(c)->block = 1;
        return SJS_OK;
    case SJS_EV_RETURN:
        if (e->count == 0 && op(c, SJS_OP_UNDEFINED_, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
        current(c)->returned = 1;
        return op(c, SJS_OP_RETURN, e->at);
    case SJS_EV_BLOCK_END:
        if (current(c)->returned) return SJS_OK;
        if (op(c, SJS_OP_UNDEFINED_, e->at) != SJS_OK) return SJS_ERR_PROGRAM;
        return op(c, SJS_OP_RETURN, e->at);
    case SJS_EV_CONST_BEGIN: {
        construct k = { 0 };
        k.kind = K_CONST;
        k.exported = c->exporting ? 1u : 0u;
        c->exporting = false;
        return push_construct(c, k, e->at);
    }
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
    return event(c, e);
}

static const sjs_sink_vtable sink_vtable = { on_event };

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
    c->export_names = index_over(&c->names);
    c->star_modules = index_over(&c->names);
    c->namespaces = index_over(&c->names);
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
    if (r == SJS_OK && open_scope(c, start) != SJS_OK) r = SJS_ERR_NOMEM;
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
                        &c->scopes, &c->bindings, &c->names, &c->pending_captures, &c->string_text, &c->global_text,
                        &c->constructs, &c->jumps, &c->patterns, &c->pattern_keys, &c->pattern_slots, &c->exports,
                        &c->stars, &c->sorting };
    for (uint32_t i = 0; i < sizeof bufs / sizeof bufs[0]; i++) sjs_buf_free(a, bufs[i]);
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

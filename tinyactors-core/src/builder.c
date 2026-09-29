#include "builder.h"
#include "system.h"
#include "interpreter.h"

/* A state by its interned name, for the name index built after pass one. */
typedef struct {
    ta_atom name;
    ta_state_id state;
} ta_state_key;

/*
 * A known span the spec gives an element (ADR 0085), recorded during the
 * walk and laid out once the counts are final. An initial transition's ID
 * is its index among the initial transitions until then.
 */
typedef struct {
    ta_element_kind kind;
    bool initial;
    uint32_t id;
    /* The spec's span, from 1. */
    uint32_t span;
} ta_span_record;

/*
 * The compiler's explicit stacks (ADR 0060: no recursion in core).
 *
 * A level of pass one: one list of sibling states, the parent they belong
 * to, and the next one to collect. `current` is the state whose children
 * the level above is collecting.
 */
typedef struct {
    const ta_scxml_state_spec *specs;
    ta_state_id parent;
    ta_state_id current;
    uint32_t count;
    uint32_t base;
    uint32_t index;
} ta_collect_frame;

/*
 * A block of executable content being compiled. `action` is the action at
 * `index` while its nested blocks are compiled; `stage` says which one:
 * an <if>'s then block, then its else block, or a <foreach>'s body.
 */
typedef enum {
    TA_BLOCK_NEXT = 0,
    TA_BLOCK_THEN,
    TA_BLOCK_ELSE,
    TA_BLOCK_BODY
} ta_block_stage;

typedef struct {
    const ta_scxml_action_spec *specs;
    ta_spec_member member;
    ta_block_stage stage;
    uint32_t count;
    uint32_t base;
    uint32_t index;
    ta_action_def action;
} ta_block_frame;

/*
 * At or below this many states the name index is scanned; above it, it is
 * binary-searched. The index is sorted either way, so both give the same
 * answer.
 */
#define TA_COMPILER_LINEAR_NAMES 16u

/*
 * Compiles a ta_scxml_spec in two passes. Pass one assigns state IDs in
 * document (pre-)order and records child ranges; pass two resolves targets and
 * compiles transitions, initial transitions, executable content, and data.
 * Every table is built in a growable vector and finally copied into the
 * definition's arena, which owns all compiled storage.
 *
 * Errors (ADR 0058). The compiler keeps the path to the element it is working
 * on: the state it is in (`path_state`, whose own path from the root follows
 * from the state table) and a stack of steps below it. Descending pushes a
 * step and returning pops it; nothing else happens on success. The first
 * fault stops the compile and is recorded once, with the path copied into the
 * caller's error arena.
 */
typedef struct {
    ta_system *system;
    ta_allocator allocator;
    const ta_scxml_spec *spec;
    ta_definition *definition;
    ta_vec states;       /* ta_state_def */
    ta_vec state_specs;  /* const ta_scxml_state_spec * */
    ta_vec children;     /* ta_state_id */
    ta_vec transitions;  /* ta_transition_def */
    ta_vec initials;     /* ta_initial_transition_def */
    ta_vec targets;      /* ta_state_id */
    ta_vec actions;      /* ta_action_def */
    ta_vec handlers;     /* ta_span32: one action block per handler */
    ta_vec params;       /* ta_param_def */
    ta_vec data_decls;   /* ta_data_decl */
    ta_vec initializers; /* ta_initializer_def */
    ta_vec contents;     /* ta_content_def */
    ta_vec patterns;     /* ta_event_pattern */
    ta_vec segments;     /* ta_event_pattern_segment */
    ta_vec expressions;  /* ta_expression */
    ta_vec roles;        /* ta_expr_role, by expression */
    ta_vec history;      /* ta_history_slot */
    ta_vec done_data;    /* ta_done_data_def */
    ta_vec invokes;      /* ta_invoke_decl */
    ta_vec texts;        /* ta_str: authored text kept for export */
    ta_vec path;         /* ta_spec_step: the steps below path_state */
    ta_vec names;        /* ta_state_key: every state, sorted by (name, state) after pass one */
    ta_vec levels;       /* ta_collect_frame: pass one's stack */
    ta_vec blocks;       /* ta_block_frame: the stack of nested blocks being compiled */
    ta_vec spans;        /* ta_span_record: the known spans, in walk order */
    uint32_t action_depth; /* the deepest stack of blocks so far */
    ta_state_id path_state;
    ta_compile_error *error;
    ta_arena *error_arena;
    /* Replay (ADR 0058 section 3): stop at this expression ID. */
    uint32_t locate;
    bool failed;
    uint64_t hash;
} ta_compiler;

typedef struct {
    ta_vec *vec;
    void **out;
    uint32_t *count;
} ta_compiler_seal_params;

#define TA_PHRASE(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

/* ------------------------------------------------------------------------
 * Faults and the path
 */

typedef struct {
    ta_compile_error_code code;
    ta_str subject;
    uint32_t subject_offset;
    uint32_t subject_length;
    ta_str related;
    ta_str message;
    /* The result to return; zero picks the code's usual one. */
    ta_result result;
} ta_compiler_fault;

static ta_state_def *ta_compiler_state(ta_compiler *compiler, ta_state_id state) {
    return ta_vec_at(&compiler->states, state);
}

static const ta_scxml_state_spec *ta_compiler_state_spec(ta_compiler *compiler, ta_state_id state) {
    return *(const ta_scxml_state_spec **)ta_vec_at(&compiler->state_specs, state);
}

/* The state's index in its parent's `states` list: specs are arrays. */
static uint32_t ta_compiler_state_index(ta_compiler *compiler, ta_state_id state) {
    ta_state_id parent = ta_compiler_state(compiler, state)->parent;
    const ta_scxml_state_spec *list =
        parent == TA_STATE_NONE ? compiler->spec->states : ta_compiler_state_spec(compiler, parent)->states;
    return (uint32_t)(ta_compiler_state_spec(compiler, state) - list);
}

static void ta_compiler_copy_path(ta_compiler *compiler) {
    uint32_t depth = 0;
    for (ta_state_id at = compiler->path_state; at != TA_STATE_NONE; at = ta_compiler_state(compiler, at)->parent) {
        depth += 1u;
    }
    uint32_t length = depth + compiler->path.count;
    if (length == 0) {
        return;
    }
    ta_spec_step *steps = ta_arena_alloc(compiler->error_arena, (size_t)length * sizeof(ta_spec_step),
                                         _Alignof(ta_spec_step));
    if (steps == NULL) {
        return; /* the error keeps its code and subject without a path */
    }
    uint32_t index = depth;
    for (ta_state_id at = compiler->path_state; at != TA_STATE_NONE; at = ta_compiler_state(compiler, at)->parent) {
        index -= 1u;
        steps[index] = (ta_spec_step){ .member = TA_SPEC_MEMBER_STATES,
                                       .index = ta_compiler_state_index(compiler, at) };
    }
    for (uint32_t step = 0; step < compiler->path.count; step += 1u) {
        steps[depth + step] = *(const ta_spec_step *)ta_vec_at(&compiler->path, step);
    }
    compiler->error->path = steps;
    compiler->error->path_length = length;
}

static ta_result ta_compiler_record(ta_compiler *compiler, const ta_compiler_fault *fault) {
    ta_result result = fault->result;
    if (result == TA_OK) {
        switch (fault->code) {
            case TA_COMPILE_ERROR_DATAMODEL_MISMATCH:
            case TA_COMPILE_ERROR_UNSUPPORTED_CONSTRUCT:
            case TA_COMPILE_ERROR_LIMIT:
                result = TA_ERR_UNSUPPORTED;
                break;
            default:
                result = TA_ERR_INVALID;
                break;
        }
    }
    if (compiler->failed) {
        return result;
    }
    compiler->failed = true;
    if (compiler->error == NULL) {
        return result;
    }
    *compiler->error = (ta_compile_error){
        .code = fault->code,
        .subject = fault->subject,
        .subject_offset = fault->subject_offset,
        .subject_length = fault->subject_length,
        .related = fault->related,
        .message = fault->message,
    };
    if (compiler->error_arena != NULL) {
        ta_compiler_copy_path(compiler);
    }
    return result;
}

static ta_result ta_compiler_fail(ta_compiler *compiler, ta_compile_error_code code, ta_str subject,
                                  ta_str message) {
    return ta_compiler_record(compiler, &(ta_compiler_fault){ .code = code, .subject = subject, .message = message });
}

static ta_result ta_compiler_enter(ta_compiler *compiler, ta_spec_member member, uint32_t index) {
    ta_spec_step step = { .member = member, .index = index };
    return ta_vec_push(&compiler->path, &step);
}

/* Steps are popped only on success: a fault ends the compile. */
static void ta_compiler_leave(ta_compiler *compiler) {
    compiler->path.count -= 1u;
}

/* Starts the path over at `state` (TA_STATE_NONE: the root). */
static void ta_compiler_at(ta_compiler *compiler, ta_state_id state) {
    compiler->path_state = state;
    compiler->path.count = 0;
}

static ta_result ta_compiler_fail_at(ta_compiler *compiler, ta_spec_member member, uint32_t index,
                                     const ta_compiler_fault *fault) {
    ta_result entered = ta_compiler_enter(compiler, member, index);
    return entered != TA_OK ? entered : ta_compiler_record(compiler, fault);
}

/*
 * Checks the span an element names and records it when it is known
 * (ADR 0085). Nothing about a span is hashed: where an element was written
 * is not what it means, so moving it keeps the revision.
 */
static ta_result ta_compiler_span(ta_compiler *compiler, const ta_span_record *record) {
    if (record->span == 0) {
        return TA_OK;
    }
    const ta_scxml_spec *spec = compiler->spec;
    if (record->span > spec->span_count || spec->spans == NULL) {
        return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_SPAN, TA_SPEC_INDEX_NONE,
                                   &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MALFORMED_SPEC,
                                                         .message = TA_PHRASE("the span is not in the spec's spans") });
    }
    const ta_source_span *span = &spec->spans[record->span - 1u];
    if (span->line == 0) {
        return TA_OK;
    }
    if (span->source >= (spec->source_count == 0 ? 1u : spec->source_count)) {
        return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_SPAN, TA_SPEC_INDEX_NONE,
                                   &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MALFORMED_SPEC,
                                                         .message = TA_PHRASE("the span names no source") });
    }
    return ta_vec_push(&compiler->spans, record);
}

/*
 * Optimal string alignment distance, a transposition counting one edit, so
 * "idel" is one edit from "idle". Error path only. UINT32_MAX when the rows
 * cannot be allocated.
 */
static uint32_t ta_compiler_distance(ta_allocator allocator, ta_str left, ta_str right) {
    uint32_t width = right.len + 1u;
    size_t size = (size_t)width * 3u * sizeof(uint32_t);
    uint32_t *rows = ta_mem_alloc(allocator, size, _Alignof(uint32_t));
    if (rows == NULL) {
        return UINT32_MAX;
    }
    uint32_t *before = rows;
    uint32_t *previous = rows + width;
    uint32_t *current = rows + 2u * width;
    for (uint32_t column = 0; column < width; column += 1u) {
        previous[column] = column;
    }
    for (uint32_t row = 1; row <= left.len; row += 1u) {
        current[0] = row;
        for (uint32_t column = 1; column < width; column += 1u) {
            uint32_t cost = left.ptr[row - 1u] == right.ptr[column - 1u] ? 0u : 1u;
            uint32_t best = previous[column] + 1u;
            if (current[column - 1u] + 1u < best) {
                best = current[column - 1u] + 1u;
            }
            if (previous[column - 1u] + cost < best) {
                best = previous[column - 1u] + cost;
            }
            if (row > 1u && column > 1u && left.ptr[row - 1u] == right.ptr[column - 2u] &&
                left.ptr[row - 2u] == right.ptr[column - 1u] && before[column - 2u] + 1u < best) {
                best = before[column - 2u] + 1u;
            }
            current[column] = best;
        }
        uint32_t *spare = before;
        before = previous;
        previous = current;
        current = spare;
    }
    uint32_t distance = previous[right.len];
    ta_mem_free(allocator, rows, &(ta_allocator_free_params){ .size = size, .alignment = _Alignof(uint32_t) });
    return distance;
}

/* The state ID nearest to `id`, within a third of its length; the first
 * such in document order on a tie. Empty when none is that near. */
static ta_str ta_compiler_nearest(ta_compiler *compiler, ta_str id) {
    ta_str nearest = {0};
    uint32_t best = UINT32_MAX;
    if (id.len > compiler->system->params[TA_PARAM_COMPILE_NEAREST_BYTES]) {
        return nearest; /* nobody misspells a name this long by a few letters */
    }
    for (ta_state_id state = 0; state < compiler->states.count; state += 1u) {
        ta_str candidate = ta_compiler_state_spec(compiler, state)->id;
        uint32_t gap = candidate.len > id.len ? candidate.len - id.len : id.len - candidate.len;
        if (3u * (uint64_t)gap > id.len) {
            continue; /* the distance is at least the gap in length */
        }
        uint32_t distance = ta_compiler_distance(compiler->allocator, id, candidate);
        if (distance != 0 && distance < best && 3u * (uint64_t)distance <= id.len) {
            best = distance;
            nearest = candidate;
        }
    }
    return nearest;
}

/* ------------------------------------------------------------------------
 * Leaves
 */

static void ta_compiler_hash(ta_compiler *compiler, const void *bytes, size_t size) {
    const unsigned char *in = bytes;
    for (size_t index = 0; index < size; index += 1) {
        compiler->hash ^= in[index];
        compiler->hash *= 1099511628211ull;
    }
}

static void ta_compiler_hash_str(ta_compiler *compiler, ta_str text) {
    ta_compiler_hash(compiler, &text.len, sizeof(text.len));
    if (text.ptr != NULL) {
        ta_compiler_hash(compiler, text.ptr, text.len);
    }
}

static void ta_compiler_hash_u32(ta_compiler *compiler, uint32_t value) {
    ta_compiler_hash(compiler, &value, sizeof(value));
}

static ta_atom_table *ta_compiler_atoms(ta_compiler *compiler) {
    return &compiler->system->atoms;
}

static ta_result ta_compiler_atom(ta_compiler *compiler, ta_str text, ta_atom *out) {
    if (text.ptr == NULL || text.len == 0) {
        return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MISSING_VALUE, text, TA_PHRASE("required text is empty"));
    }
    ta_compiler_hash_str(compiler, text);
    return ta_atom_intern(ta_compiler_atoms(compiler), text, out);
}

static ta_result ta_compiler_atom_at(ta_compiler *compiler, ta_spec_member member, ta_str text, ta_atom *out) {
    ta_result result = ta_compiler_enter(compiler, member, TA_SPEC_INDEX_NONE);
    if (result == TA_OK) {
        result = ta_compiler_atom(compiler, text, out);
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    return result;
}

ta_result ta_duration_parse(ta_str text, ta_time *out) {
    if (text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    uint32_t index = 0;
    uint64_t whole = 0;
    uint64_t fraction = 0;
    uint64_t scale = 1;
    bool digits = false;
    while (index < text.len && text.ptr[index] >= '0' && text.ptr[index] <= '9') {
        if (whole > (UINT64_MAX - 9u) / 10u) {
            return TA_ERR_INVALID;
        }
        whole = whole * 10u + (uint64_t)(text.ptr[index] - '0');
        digits = true;
        index += 1;
    }
    if (index < text.len && text.ptr[index] == '.') {
        index += 1;
        bool fraction_digits = false;
        while (index < text.len && text.ptr[index] >= '0' && text.ptr[index] <= '9') {
            if (scale < 1000000000ull) {
                fraction = fraction * 10u + (uint64_t)(text.ptr[index] - '0');
                scale *= 10u;
            }
            fraction_digits = true;
            index += 1;
        }
        if (!fraction_digits) {
            return TA_ERR_INVALID;
        }
        digits = true; /* ".5s": SCXML durations are CSS2 times, whose whole part is optional */
    }
    if (!digits) {
        return TA_ERR_INVALID;
    }
    ta_str unit = { .ptr = text.ptr + index, .len = text.len - index };
    uint64_t multiplier = 0;
    if (ta_str_equal(unit, (ta_str){ .ptr = "ms", .len = 2 })) {
        multiplier = 1;
    } else if (ta_str_equal(unit, (ta_str){ .ptr = "s", .len = 1 })) {
        multiplier = 1000;
    } else {
        return TA_ERR_INVALID;
    }
    if (whole > UINT64_MAX / multiplier) {
        return TA_ERR_INVALID;
    }
    /* Sub-millisecond remainders are truncated. */
    *out = whole * multiplier + (fraction * multiplier) / scale;
    return TA_OK;
}

typedef struct {
    const ta_expression *expression;
    ta_expr_role role;
} ta_compiler_expression_params;

static ta_result ta_compiler_expression(ta_compiler *compiler, const ta_compiler_expression_params *params,
                                        ta_expr_id *out) {
    const ta_expression *expression = params->expression;
    if (compiler->locate == compiler->expressions.count &&
        (expression->kind == TA_EXPRESSION_SOURCE || expression->kind == TA_EXPRESSION_HOST)) {
        /* Replay: this is the expression the data model refused. */
        return ta_compiler_fail(compiler, TA_COMPILE_ERROR_EXPRESSION_REJECTED,
                                expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source : (ta_str){0},
                                TA_PHRASE("the data model refused this expression"));
    }
    ta_result result = ta_compiler_span(compiler, &(ta_span_record){ .kind = TA_ELEMENT_EXPRESSION,
                                                                     .id = compiler->expressions.count,
                                                                     .span = expression->span });
    if (result != TA_OK) {
        return result;
    }
    ta_expression copy = { .kind = expression->kind };
    switch (expression->kind) {
        case TA_EXPRESSION_SOURCE:
            ta_compiler_hash_str(compiler, expression->as.source);
            if (expression->as.source.ptr == NULL) {
                return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MISSING_VALUE, (ta_str){0},
                                        TA_PHRASE("an expression has no source text"));
            }
            result = ta_arena_copy_str(&compiler->definition->arena, expression->as.source,
                                       &copy.as.source);
            break;
        case TA_EXPRESSION_HOST:
            /*
             * The fingerprint cannot see into a host expression, only where
             * one is (ADR 0087): a length no source text can have marks it,
             * and registration compares the handles themselves.
             */
            ta_compiler_hash_u32(compiler, UINT32_MAX);
            compiler->definition->host_expressions = true;
            if (expression->as.host.v == NULL || expression->as.host.v->retain == NULL) {
                return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                        TA_PHRASE("a host expression has no retain operation"));
            }
            copy.as.host = expression->as.host;
            result = expression->as.host.v->retain(expression->as.host.self, expression->as.host.value,
                                                   compiler->allocator, &copy.as.host.value);
            if (result != TA_OK && result != TA_ERR_NOMEM) {
                return ta_compiler_record(compiler, &(ta_compiler_fault){
                                                        .code = TA_COMPILE_ERROR_HOST_EXPRESSION,
                                                        .message = TA_PHRASE("a host expression could not be retained"),
                                                        .result = result,
                                                    });
            }
            break;
        case TA_EXPRESSION_NONE:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MISSING_VALUE, (ta_str){0},
                                    TA_PHRASE("an expression is required"));
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown expression kind"));
    }
    if (result != TA_OK) {
        return result;
    }
    *out = compiler->expressions.count;
    /* The role first: a failed push leaves both vectors as they were. */
    result = ta_vec_push(&compiler->roles, &params->role);
    if (result == TA_OK) {
        result = ta_vec_push(&compiler->expressions, &copy);
        if (result != TA_OK) {
            compiler->roles.count -= 1u;
        }
    }
    if (result != TA_OK && copy.kind == TA_EXPRESSION_HOST && copy.as.host.v->release != NULL) {
        copy.as.host.v->release(copy.as.host.self, copy.as.host.value, compiler->allocator);
    }
    return result;
}

typedef struct {
    ta_spec_member member;
    const ta_expression *expression;
    ta_expr_role role;
} ta_compiler_expression_at_params;

static ta_result ta_compiler_expression_at(ta_compiler *compiler, const ta_compiler_expression_at_params *params,
                                           ta_expr_id *out) {
    ta_result result = ta_compiler_enter(compiler, params->member, TA_SPEC_INDEX_NONE);
    if (result == TA_OK) {
        result = ta_compiler_expression(compiler,
                                        &(ta_compiler_expression_params){ params->expression, params->role }, out);
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    return result;
}

/* A dotted event name, as <raise event> and <send event> write one. */
static ta_result ta_compiler_event_name(ta_compiler *compiler, ta_str text, ta_event_name_id *out) {
    ta_result result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_EVENT, TA_SPEC_INDEX_NONE);
    if (result != TA_OK) {
        return result;
    }
    ta_compiler_hash_str(compiler, text);
    result = ta_event_name_intern_dotted(&compiler->system->event_names, text, out);
    if (result == TA_ERR_INVALID) {
        return text.len == 0
                   ? ta_compiler_fail(compiler, TA_COMPILE_ERROR_MISSING_VALUE, text, TA_PHRASE("an event name is required"))
                   : ta_compiler_fail(compiler, TA_COMPILE_ERROR_BAD_EVENT_DESCRIPTOR, text,
                                      TA_PHRASE("an event name has an empty segment"));
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    return result;
}

/*
 * Keeps authored text that the compiler normalizes, so ta_definition_export()
 * can rebuild the exact input (ADR 0042). The text is copied into the
 * definition's arena.
 */
static ta_result ta_compiler_keep_text(ta_compiler *compiler, ta_str text, ta_authored_text_id *out) {
    ta_str copy = {0};
    ta_result result = ta_arena_copy_str(&compiler->definition->arena, text, &copy);
    if (result == TA_OK) {
        *out = compiler->texts.count;
        result = ta_vec_push(&compiler->texts, &copy);
    }
    return result;
}

/* `media_type` is empty for literal text (a send target, an invoke src). */
static ta_result ta_compiler_content(ta_compiler *compiler, ta_str media_type, ta_bytes bytes,
                                     ta_content_id *out) {
    ta_content_def content = {0};
    if (media_type.len != 0) {
        ta_result copied = ta_arena_copy_str(&compiler->definition->arena, media_type, &content.media_type);
        if (copied != TA_OK) {
            return copied;
        }
    }
    /* A media type is part of what the chart says, so it is part of its
     * revision (ADR 0053): two charts differing only in one must not share a
     * fingerprint. */
    ta_compiler_hash_str(compiler, media_type);
    if (bytes.len != 0) {
        uint8_t *copy = ta_arena_alloc(&compiler->definition->arena, bytes.len, 1);
        if (copy == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_copy(copy, bytes.ptr, bytes.len);
        content.bytes = (ta_bytes){ .ptr = copy, .len = bytes.len };
    }
    ta_compiler_hash(compiler, bytes.ptr == NULL ? "" : (const void *)bytes.ptr, bytes.len);
    *out = compiler->contents.count;
    return ta_vec_push(&compiler->contents, &content);
}

static ta_result ta_compiler_value(ta_compiler *compiler, const ta_scxml_value_spec *spec,
                                   ta_value_source *out) {
    ta_compiler_hash_u32(compiler, spec->kind);
    switch (spec->kind) {
        case TA_SCXML_VALUE_NONE:
            *out = (ta_value_source){ .kind = TA_VALUE_SOURCE_NONE };
            return TA_OK;
        case TA_SCXML_VALUE_EXPR:
            out->kind = TA_VALUE_SOURCE_EXPR;
            return ta_compiler_expression(compiler, &(ta_compiler_expression_params){ &spec->as.expr, TA_EXPR_ROLE_VALUE },
                                          &out->as.expr);
        case TA_SCXML_VALUE_LOCATION:
            out->kind = TA_VALUE_SOURCE_LOCATION;
            return ta_compiler_expression(compiler,
                                          &(ta_compiler_expression_params){ &spec->as.location, TA_EXPR_ROLE_LOCATION },
                                          &out->as.location);
        case TA_SCXML_VALUE_CONTENT:
            out->kind = TA_VALUE_SOURCE_CONTENT;
            return ta_compiler_content(compiler, spec->as.content.media_type, spec->as.content.bytes,
                                       &out->as.content);
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown value kind"));
    }
}

static ta_result ta_compiler_value_at(ta_compiler *compiler, ta_spec_member member,
                                      const ta_scxml_value_spec *spec, ta_value_source *out) {
    ta_result result = ta_compiler_enter(compiler, member, TA_SPEC_INDEX_NONE);
    if (result == TA_OK) {
        result = ta_compiler_value(compiler, spec, out);
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    return result;
}

static ta_result ta_compiler_params(ta_compiler *compiler, const ta_scxml_param_spec *specs,
                                    ta_span32 *out) {
    uint32_t count = 0;
    while (specs != NULL && specs[count].name.ptr != NULL) {
        count += 1;
    }
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->params, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        ta_param_def param = {0};
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_PARAMS, index);
        if (result == TA_OK) {
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_NAME, specs[index].name, &param.name);
        }
        if (result == TA_OK) {
            result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &specs[index].value, &param.value);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            *(ta_param_def *)ta_vec_at(&compiler->params, base + index) = param;
        }
    }
    *out = (ta_span32){ .offset = base, .length = count };
    return result;
}

static ta_result ta_compiler_block(ta_compiler *compiler, ta_spec_member member,
                                   const ta_scxml_action_spec *specs, ta_span32 *out);

static ta_result ta_compiler_malformed(ta_compiler *compiler, ta_spec_member member) {
    return ta_compiler_fail_at(compiler, member, TA_SPEC_INDEX_NONE,
                               &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MALFORMED_SPEC,
                                                     .message = TA_PHRASE("unknown kind") });
}

static ta_result ta_compiler_send(ta_compiler *compiler, const ta_scxml_send_spec *spec,
                                  ta_send_action *out) {
    ta_result result = TA_OK;
    switch (spec->id.kind) {
        case TA_SCXML_ID_NONE:
            out->id.kind = TA_ID_SOURCE_GENERATED;
            break;
        case TA_SCXML_ID_LITERAL:
            out->id.kind = TA_ID_SOURCE_LITERAL;
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_ID, spec->id.as.literal, &out->id.as.literal);
            break;
        case TA_SCXML_ID_LOCATION:
            out->id.kind = TA_ID_SOURCE_LOCATION;
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_ID, &spec->id.as.location, TA_EXPR_ROLE_LOCATION },
                                               &out->id.as.location);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_ID);
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->type.kind) {
        case TA_SCXML_STRING_NONE:
            out->type.kind = TA_TEXT_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->type.kind = TA_TEXT_SOURCE_ATOM;
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_TYPE, spec->type.as.literal, &out->type.as.atom);
            break;
        case TA_SCXML_STRING_EXPR:
            out->type.kind = TA_TEXT_SOURCE_EXPR;
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_TYPE, &spec->type.as.expr, TA_EXPR_ROLE_VALUE },
                                               &out->type.as.expr);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_TYPE);
    }
    if (result != TA_OK) {
        return result;
    }
    out->delay.text = TA_AUTHORED_TEXT_NONE;
    ta_compiler_hash_u32(compiler, spec->event.kind);
    if (spec->event.kind == TA_SCXML_EVENT_EXPR) {
        out->event.kind = TA_EVENT_SOURCE_EXPR;
        result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_EVENT, &spec->event.as.expr, TA_EXPR_ROLE_VALUE },
                                           &out->event.as.expr);
    } else if (spec->event.kind == TA_SCXML_EVENT_NAME) {
        out->event.kind = TA_EVENT_SOURCE_NAME;
        result = ta_compiler_event_name(compiler, spec->event.as.name, &out->event.as.name);
    } else if (spec->event.kind == TA_SCXML_EVENT_NONE) {
        return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_EVENT, TA_SPEC_INDEX_NONE,
                                   &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MISSING_VALUE,
                                                         .message = TA_PHRASE("a send needs an event") });
    } else {
        return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_EVENT);
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->target.kind) {
        case TA_SCXML_STRING_NONE:
            out->target.kind = TA_VALUE_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->target.kind = TA_VALUE_SOURCE_CONTENT;
            result = ta_compiler_content(compiler, (ta_str){0},
                                         (ta_bytes){ .ptr = (const uint8_t *)spec->target.as.literal.ptr,
                                                     .len = spec->target.as.literal.len },
                                         &out->target.as.content);
            break;
        case TA_SCXML_STRING_EXPR:
            out->target.kind = TA_VALUE_SOURCE_EXPR;
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_TARGET, &spec->target.as.expr, TA_EXPR_ROLE_VALUE },
                                               &out->target.as.expr);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_TARGET);
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->delay.kind) {
        case TA_SCXML_STRING_NONE:
            out->delay.kind = TA_DELAY_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->delay.kind = TA_DELAY_SOURCE_LITERAL;
            ta_compiler_hash_str(compiler, spec->delay.as.literal);
            result = ta_duration_parse(spec->delay.as.literal, &out->delay.as.literal);
            if (result == TA_ERR_INVALID) {
                return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_DELAY, TA_SPEC_INDEX_NONE,
                                           &(ta_compiler_fault){
                                               .code = TA_COMPILE_ERROR_BAD_DURATION,
                                               .subject = spec->delay.as.literal,
                                               .message = TA_PHRASE("not an SCXML duration such as \"500ms\" or \"2.5s\""),
                                           });
            }
            if (result == TA_OK) {
                result = ta_compiler_keep_text(compiler, spec->delay.as.literal, &out->delay.text);
            }
            break;
        case TA_SCXML_STRING_EXPR:
            out->delay.kind = TA_DELAY_SOURCE_EXPR;
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_DELAY, &spec->delay.as.expr, TA_EXPR_ROLE_VALUE },
                                               &out->delay.as.expr);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_DELAY);
    }
    if (result == TA_OK) {
        result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->data, &out->data);
    }
    if (result == TA_OK) {
        result = ta_compiler_params(compiler, spec->params, &out->params);
    }
    if (result == TA_OK && out->params.length != 0 && out->data.kind != TA_VALUE_SOURCE_NONE) {
        result = ta_compiler_fail(compiler, TA_COMPILE_ERROR_EXCLUSIVE_MEMBERS, (ta_str){0},
                                  TA_PHRASE("a send has both params and content"));
    }
    return result;
}

/*
 * <foreach>: the array is an expression; item and index are variable names
 * whose legality the data model judges when it binds them (SCXML 4.6). The
 * body is compiled by ta_compiler_block after this.
 */
static ta_result ta_compiler_foreach(ta_compiler *compiler, const ta_scxml_action_spec *spec,
                                     ta_foreach_action *out) {
    ta_result result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_EXPR, &spec->as.foreach.array, TA_EXPR_ROLE_VALUE }, &out->array);
    if (result == TA_OK) {
        result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_ITEM, spec->as.foreach.item, &out->item);
    }
    if (result == TA_OK && spec->as.foreach.index.ptr != NULL) {
        out->has_index = true;
        result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_INDEX, spec->as.foreach.index, &out->index);
    }
    return result;
}

/*
 * One action, up to its nested blocks: everything an <if> or <foreach> has
 * before them. ta_compiler_block compiles the nested blocks next, in the
 * order the recursive compiler did.
 */
static ta_result ta_compiler_action(ta_compiler *compiler, const ta_scxml_action_spec *spec,
                                    ta_action_def *out) {
    ta_compiler_hash_u32(compiler, spec->kind);
    ta_result result = TA_OK;
    switch (spec->kind) {
        case TA_SCXML_ACTION_RAISE:
            out->kind = TA_ACTION_RAISE;
            result = ta_compiler_event_name(compiler, spec->as.raise.event, &out->as.raise.event);
            if (result == TA_OK) {
                result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->as.raise.data,
                                              &out->as.raise.data);
            }
            return result;
        case TA_SCXML_ACTION_SEND:
            out->kind = TA_ACTION_SEND;
            return ta_compiler_send(compiler, &spec->as.send, &out->as.send);
        case TA_SCXML_ACTION_CANCEL:
            out->kind = TA_ACTION_CANCEL;
            if (spec->as.cancel.sendid.kind == TA_SCXML_STRING_EXPR) {
                out->as.cancel.sendid.kind = TA_TEXT_SOURCE_EXPR;
                return ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_SENDID, &spec->as.cancel.sendid.as.expr, TA_EXPR_ROLE_VALUE },
                                                 &out->as.cancel.sendid.as.expr);
            }
            if (spec->as.cancel.sendid.kind == TA_SCXML_STRING_NONE) {
                return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_SENDID, TA_SPEC_INDEX_NONE,
                                           &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MISSING_VALUE,
                                                                 .message = TA_PHRASE("a cancel needs a sendid") });
            }
            if (spec->as.cancel.sendid.kind != TA_SCXML_STRING_LITERAL) {
                return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_SENDID);
            }
            out->as.cancel.sendid.kind = TA_TEXT_SOURCE_ATOM;
            return ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_SENDID, spec->as.cancel.sendid.as.literal,
                                       &out->as.cancel.sendid.as.atom);
        case TA_SCXML_ACTION_ASSIGN:
            out->kind = TA_ACTION_ASSIGN;
            if (spec->as.assign.value.kind == TA_SCXML_VALUE_NONE) {
                return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, TA_SPEC_INDEX_NONE,
                                           &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MISSING_VALUE,
                                                                 .message = TA_PHRASE("an assign needs a value") });
            }
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_LOCATION, &spec->as.assign.location, TA_EXPR_ROLE_LOCATION },
                                               &out->as.assign.location);
            if (result == TA_OK) {
                result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->as.assign.value,
                                              &out->as.assign.value);
            }
            return result;
        case TA_SCXML_ACTION_SCRIPT:
            out->kind = TA_ACTION_SCRIPT;
            return ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_EXPR, &spec->as.script.script, TA_EXPR_ROLE_SCRIPT }, &out->as.script);
        case TA_SCXML_ACTION_LOG:
            out->kind = TA_ACTION_LOG;
            if (spec->as.log.label.ptr != NULL) {
                out->as.log.has_label = true;
                result = ta_atom_intern(ta_compiler_atoms(compiler), spec->as.log.label, &out->as.log.label);
            }
            if (result == TA_OK && spec->as.log.expr.kind != TA_EXPRESSION_NONE) {
                out->as.log.has_expr = true;
                result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_EXPR, &spec->as.log.expr, TA_EXPR_ROLE_VALUE },
                                                   &out->as.log.expr);
            }
            return result;
        case TA_SCXML_ACTION_IF:
            out->kind = TA_ACTION_IF;
            return ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_CONDITION, &spec->as.conditional.condition, TA_EXPR_ROLE_CONDITION },
                                             &out->as.conditional.condition);
        case TA_SCXML_ACTION_FOREACH:
            out->kind = TA_ACTION_FOREACH;
            return ta_compiler_foreach(compiler, spec, &out->as.foreach);
        case TA_SCXML_ACTION_INVOKE:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_UNSUPPORTED_CONSTRUCT, (ta_str){0},
                                    TA_PHRASE("<invoke> is not executable content"));
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown action kind"));
    }
}

/* Starts a block: reserves its actions, so nested blocks append behind it. */
static ta_result ta_compiler_block_open(ta_compiler *compiler, ta_spec_member member,
                                        const ta_scxml_action_spec *specs) {
    ta_block_frame frame = { .specs = specs, .member = member };
    while (specs != NULL && specs[frame.count].kind != TA_SCXML_ACTION_NONE) {
        frame.count += 1;
    }
    ta_result result = ta_vec_extend(&compiler->actions, frame.count, &frame.base);
    if (result == TA_OK) {
        result = ta_vec_push(&compiler->blocks, &frame);
    }
    if (result == TA_OK && compiler->blocks.count > compiler->action_depth) {
        compiler->action_depth = compiler->blocks.count;
    }
    return result;
}

static ta_block_frame *ta_compiler_block_top(ta_compiler *compiler) {
    return ta_vec_at(&compiler->blocks, compiler->blocks.count - 1u);
}

/* Stores the finished action of the top block and moves to the next one. */
static void ta_compiler_block_store(ta_compiler *compiler, ta_block_frame *frame) {
    ta_compiler_leave(compiler);
    *(ta_action_def *)ta_vec_at(&compiler->actions, frame->base + frame->index) = frame->action;
    frame->index += 1u;
    frame->stage = TA_BLOCK_NEXT;
}

/*
 * Gives a finished nested block to the action that owns it, in the top
 * frame, and opens the owner's next nested block if it has one.
 */
static ta_result ta_compiler_block_deliver(ta_compiler *compiler, ta_span32 span) {
    ta_block_frame *frame = ta_compiler_block_top(compiler);
    switch (frame->stage) {
        case TA_BLOCK_THEN:
            frame->action.as.conditional.then_actions = span;
            frame->stage = TA_BLOCK_ELSE;
            return ta_compiler_block_open(compiler, TA_SPEC_MEMBER_ELSE,
                                          frame->specs[frame->index].as.conditional.else_actions);
        case TA_BLOCK_ELSE:
            frame->action.as.conditional.else_actions = span;
            ta_compiler_block_store(compiler, frame);
            return TA_OK;
        case TA_BLOCK_BODY:
            frame->action.as.foreach.body_actions = span;
            ta_compiler_block_store(compiler, frame);
            return TA_OK;
        default:
            return TA_ERR_INVALID; /* unreachable: only an owner waits for a block */
    }
}

/*
 * Compiles a block of executable content and every block nested in it,
 * without recursion (ADR 0060). Each open block is a frame; an <if> or
 * <foreach> in a block compiles up to its nested blocks, opens the first,
 * and resumes when it is delivered. Actions, expressions, nested block
 * reservations and path steps come in exactly the order the recursive
 * compiler produced them, so revisions and numbering do not change.
 */
static ta_result ta_compiler_block(ta_compiler *compiler, ta_spec_member member,
                                   const ta_scxml_action_spec *specs, ta_span32 *out) {
    uint32_t floor = compiler->blocks.count;
    ta_result result = ta_compiler_block_open(compiler, member, specs);
    while (result == TA_OK) {
        ta_block_frame *frame = ta_compiler_block_top(compiler);
        if (frame->index == frame->count) {
            ta_span32 span = { .offset = frame->base, .length = frame->count };
            compiler->blocks.count -= 1u;
            if (compiler->blocks.count == floor) {
                *out = span;
                return TA_OK;
            }
            result = ta_compiler_block_deliver(compiler, span);
            continue;
        }
        const ta_scxml_action_spec *spec = &frame->specs[frame->index];
        ta_action_def action = {0};
        result = ta_compiler_enter(compiler, frame->member, frame->index);
        if (result == TA_OK) {
            result = ta_compiler_span(compiler, &(ta_span_record){ .kind = TA_ELEMENT_ACTION,
                                                                   .id = frame->base + frame->index,
                                                                   .span = spec->span });
        }
        if (result == TA_OK) {
            result = ta_compiler_action(compiler, spec, &action);
        }
        if (result != TA_OK) {
            break;
        }
        frame = ta_compiler_block_top(compiler);
        frame->action = action;
        if (action.kind == TA_ACTION_IF) {
            frame->stage = TA_BLOCK_THEN;
            result = ta_compiler_block_open(compiler, TA_SPEC_MEMBER_THEN, spec->as.conditional.then_actions);
        } else if (action.kind == TA_ACTION_FOREACH) {
            frame->stage = TA_BLOCK_BODY;
            result = ta_compiler_block_open(compiler, TA_SPEC_MEMBER_BODY, spec->as.foreach.body);
        } else {
            ta_compiler_block_store(compiler, frame);
        }
    }
    compiler->blocks.count = floor;
    return result;
}

/*
 * Compiles a list of <onentry> or <onexit> handlers. Each handler keeps its
 * own action block, so an error in one handler does not skip the next.
 */
static ta_result ta_compiler_handlers(ta_compiler *compiler, ta_spec_member member,
                                      const ta_scxml_handler_spec *specs, ta_span32 *out) {
    uint32_t count = 0;
    while (specs != NULL && specs[count].actions != NULL) {
        count += 1;
    }
    ta_compiler_hash_u32(compiler, count);
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->handlers, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        ta_span32 block = {0};
        result = ta_compiler_enter(compiler, member, index);
        if (result == TA_OK) {
            result = ta_compiler_block(compiler, TA_SPEC_MEMBER_ACTIONS, specs[index].actions, &block);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            ta_compiler_hash_u32(compiler, block.length);
            *(ta_span32 *)ta_vec_at(&compiler->handlers, base + index) = block;
        }
    }
    *out = (ta_span32){ .offset = base, .length = count };
    return result;
}

static ta_result ta_compiler_kind(ta_compiler *compiler, ta_scxml_state_kind kind, ta_state_kind *out) {
    switch (kind) {
        case TA_SCXML_STATE_ATOMIC: *out = TA_STATE_ATOMIC; return TA_OK;
        case TA_SCXML_STATE_COMPOUND: *out = TA_STATE_COMPOUND; return TA_OK;
        case TA_SCXML_STATE_PARALLEL: *out = TA_STATE_PARALLEL; return TA_OK;
        case TA_SCXML_STATE_FINAL: *out = TA_STATE_FINAL; return TA_OK;
        case TA_SCXML_STATE_HISTORY_SHALLOW: *out = TA_STATE_HISTORY_SHALLOW; return TA_OK;
        case TA_SCXML_STATE_HISTORY_DEEP: *out = TA_STATE_HISTORY_DEEP; return TA_OK;
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown state kind"));
    }
}

static bool ta_state_kind_is_history(ta_state_kind kind) {
    return kind == TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_HISTORY_DEEP;
}

static bool ta_state_key_before(ta_state_key left, ta_state_key right) {
    return left.name < right.name || (left.name == right.name && left.state < right.state);
}

static void ta_state_keys_sift(ta_state_key *keys, uint32_t root, uint32_t count) {
    for (;;) {
        uint32_t largest = root;
        uint32_t left = 2u * root + 1u;
        uint32_t right = left + 1u;
        if (left < count && ta_state_key_before(keys[largest], keys[left])) {
            largest = left;
        }
        if (right < count && ta_state_key_before(keys[largest], keys[right])) {
            largest = right;
        }
        if (largest == root) {
            return;
        }
        ta_state_key swap = keys[root];
        keys[root] = keys[largest];
        keys[largest] = swap;
        root = largest;
    }
}

/*
 * Sorts by (name, state): insertion sort for a few keys, heapsort otherwise,
 * so the sort is O(n log n), in place, and allocates nothing. The order is
 * total, so stability does not matter.
 */
static void ta_state_keys_sort(ta_state_key *keys, uint32_t count) {
    if (count <= TA_COMPILER_LINEAR_NAMES) {
        for (uint32_t index = 1; index < count; index += 1u) {
            ta_state_key key = keys[index];
            uint32_t at = index;
            while (at > 0 && ta_state_key_before(key, keys[at - 1u])) {
                keys[at] = keys[at - 1u];
                at -= 1u;
            }
            keys[at] = key;
        }
        return;
    }
    for (uint32_t root = count / 2u; root > 0; root -= 1u) {
        ta_state_keys_sift(keys, root - 1u, count);
    }
    for (uint32_t end = count - 1u; end > 0; end -= 1u) {
        ta_state_key swap = keys[0];
        keys[0] = keys[end];
        keys[end] = swap;
        ta_state_keys_sift(keys, 0, end);
    }
}

/* The state named `name`. Pass two only: the index is complete and has no
 * duplicates. */
static bool ta_compiler_find(ta_compiler *compiler, ta_atom name, ta_state_id *out) {
    const ta_state_key *keys = compiler->names.items;
    uint32_t count = compiler->names.count;
    if (count <= TA_COMPILER_LINEAR_NAMES) {
        for (uint32_t index = 0; index < count; index += 1u) {
            if (keys[index].name == name) {
                *out = keys[index].state;
                return true;
            }
        }
        return false;
    }
    uint32_t low = 0;
    uint32_t high = count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (keys[middle].name < name) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    if (low < count && keys[low].name == name) {
        *out = keys[low].state;
        return true;
    }
    return false;
}

/*
 * Builds the name index over the states pass one collected, and reports a
 * duplicate ID. Sorted by (name, state), each run of equal names lists its
 * states in document order. So the second key of a run is the duplicate the
 * walk meets first, and the first key is the definition it repeats. Across
 * runs the smallest such second state wins: that is the duplicate a
 * per-state check during pass one would report.
 *
 * `collected` is the result of pass one. A pass-one fault stops collection
 * at a state after every collected one, so a duplicate among the collected
 * states comes earlier in document order and replaces that fault. That keeps
 * the first error in compile order (ADR 0058 section 2).
 */
static ta_result ta_compiler_index_names(ta_compiler *compiler, ta_result collected) {
    uint32_t count = compiler->states.count;
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->names, count, &base);
    if (result != TA_OK) {
        return collected != TA_OK ? collected : result;
    }
    ta_state_key *keys = compiler->names.items;
    for (ta_state_id state = 0; state < count; state += 1u) {
        keys[state] = (ta_state_key){ .name = ta_compiler_state(compiler, state)->name, .state = state };
    }
    ta_state_keys_sort(keys, count);
    ta_state_id duplicate = TA_STATE_NONE;
    ta_state_id first = TA_STATE_NONE;
    for (uint32_t index = 1, run = 0; index < count; index += 1u) {
        if (keys[index].name != keys[index - 1u].name) {
            run = index;
        } else if (index == run + 1u && (duplicate == TA_STATE_NONE || keys[index].state < duplicate)) {
            duplicate = keys[index].state;
            first = keys[run].state;
        }
    }
    if (duplicate == TA_STATE_NONE) {
        return collected;
    }
    if (collected != TA_OK && !compiler->failed) {
        return collected; /* an allocation failure, not a fault in the chart */
    }
    compiler->failed = false;
    ta_compiler_at(compiler, ta_compiler_state(compiler, duplicate)->parent);
    result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_STATES, ta_compiler_state_index(compiler, duplicate));
    if (result != TA_OK) {
        return result;
    }
    return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_ID, TA_SPEC_INDEX_NONE,
                               &(ta_compiler_fault){
                                   .code = TA_COMPILE_ERROR_DUPLICATE_STATE,
                                   .subject = ta_compiler_state_spec(compiler, duplicate)->id,
                                   .related = ta_compiler_state_spec(compiler, first)->id,
                                   .message = TA_PHRASE("duplicate state ID"),
                               });
}

/* Opens a level of pass one: reserves its slots in `children`. */
static ta_result ta_compiler_level_open(ta_compiler *compiler, const ta_scxml_state_spec *specs, ta_state_id parent) {
    ta_collect_frame frame = { .specs = specs, .parent = parent, .current = TA_STATE_NONE };
    while (specs != NULL && specs[frame.count].kind != TA_SCXML_STATE_NONE) {
        frame.count += 1;
    }
    ta_result result = ta_vec_extend(&compiler->children, frame.count, &frame.base);
    return result == TA_OK ? ta_vec_push(&compiler->levels, &frame) : result;
}

/*
 * Pass one: assigns IDs in document pre-order and records child ranges,
 * without recursion (ADR 0060). A state's children are collected right after
 * it, before its next sibling, as the recursive compiler did, so IDs, the
 * layout of `children` and the hash do not change.
 */
static ta_result ta_compiler_collect(ta_compiler *compiler, const ta_scxml_state_spec *specs,
                                     ta_state_id parent, ta_span32 *out) {
    ta_result result = ta_compiler_level_open(compiler, specs, parent);
    while (result == TA_OK) {
        ta_collect_frame *frame = ta_vec_at(&compiler->levels, compiler->levels.count - 1u);
        if (frame->index == frame->count) {
            ta_span32 span = { .offset = frame->base, .length = frame->count };
            compiler->levels.count -= 1u;
            if (compiler->levels.count == 0) {
                *out = span;
                return TA_OK;
            }
            frame = ta_vec_at(&compiler->levels, compiler->levels.count - 1u);
            ta_compiler_state(compiler, frame->current)->children = span;
            frame->index += 1u;
            continue;
        }
        const ta_scxml_state_spec *spec = &frame->specs[frame->index];
        ta_state_def state = {
            .parent = frame->parent,
            .initial = TA_INITIAL_TRANSITION_NONE,
            .done_data = TA_DONE_DATA_NONE,
            .history_slot = UINT32_MAX,
        };
        ta_compiler_at(compiler, frame->parent);
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_STATES, frame->index);
        if (result == TA_OK) {
            ta_compiler_hash_u32(compiler, spec->kind);
            result = ta_compiler_kind(compiler, spec->kind, &state.kind);
        }
        if (result == TA_OK) {
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_ID, spec->id, &state.name);
        }
        if (result == TA_OK) {
            result = ta_compiler_span(compiler, &(ta_span_record){ .kind = TA_ELEMENT_STATE,
                                                                   .id = compiler->states.count,
                                                                   .span = spec->span });
        }
        if (result != TA_OK) {
            break;
        }
        ta_state_id id = compiler->states.count;
        *(ta_state_id *)ta_vec_at(&compiler->children, frame->base + frame->index) = id;
        frame->current = id;
        result = ta_vec_push(&compiler->states, &state);
        if (result == TA_OK) {
            result = ta_vec_push(&compiler->state_specs, &spec);
        }
        if (result == TA_OK) {
            result = ta_compiler_level_open(compiler, spec->states, id);
        }
    }
    compiler->levels.count = 0;
    return result;
}

static ta_result ta_compiler_resolve(ta_compiler *compiler, ta_str id, ta_state_id *out) {
    ta_atom atom = 0;
    ta_result result = ta_compiler_atom(compiler, id, &atom);
    if (result != TA_OK) {
        return result;
    }
    if (ta_compiler_find(compiler, atom, out)) {
        return TA_OK;
    }
    return ta_compiler_record(compiler, &(ta_compiler_fault){
                                            .code = TA_COMPILE_ERROR_UNKNOWN_STATE,
                                            .subject = id,
                                            .related = ta_compiler_nearest(compiler, id),
                                            .message = TA_PHRASE("unknown state"),
                                        });
}

static bool ta_compiler_descends(ta_compiler *compiler, ta_state_id state, ta_state_id ancestor) {
    if (ancestor == TA_STATE_NONE) {
        return true;
    }
    for (ta_state_id at = ta_compiler_state(compiler, state)->parent; at != TA_STATE_NONE;
         at = ta_compiler_state(compiler, at)->parent) {
        if (at == ancestor) {
            return true;
        }
    }
    return false;
}

typedef struct {
    const ta_str *ids;
    /* Targets must be proper descendants of this state (TA_STATE_NONE: any). */
    ta_state_id within;
    ta_span32 *out;
} ta_compiler_targets_params;

static ta_result ta_compiler_targets(ta_compiler *compiler, const ta_compiler_targets_params *params) {
    uint32_t count = 0;
    while (params->ids != NULL && params->ids[count].ptr != NULL) {
        count += 1;
    }
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->targets, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        ta_state_id target = TA_STATE_NONE;
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_TARGETS, index);
        if (result == TA_OK) {
            result = ta_compiler_resolve(compiler, params->ids[index], &target);
        }
        if (result == TA_OK && !ta_compiler_descends(compiler, target, params->within)) {
            result = ta_compiler_record(compiler, &(ta_compiler_fault){
                                                      .code = TA_COMPILE_ERROR_TARGET_OUTSIDE,
                                                      .subject = params->ids[index],
                                                      .related = ta_compiler_state_spec(compiler, params->within)->id,
                                                      .message = TA_PHRASE("target is not inside its state"),
                                                  });
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            *(ta_state_id *)ta_vec_at(&compiler->targets, base + index) = target;
        }
    }
    *params->out = (ta_span32){ .offset = base, .length = count };
    return result;
}

/* Parses space-separated descriptors; a `*` segment is a wildcard. */
static ta_result ta_compiler_descriptors(ta_compiler *compiler, ta_str text, ta_span32 *out) {
    ta_compiler_hash_str(compiler, text);
    uint32_t first = compiler->patterns.count;
    uint32_t index = 0;
    while (text.ptr != NULL && index < text.len) {
        while (index < text.len && (text.ptr[index] == ' ' || text.ptr[index] == '\t' ||
                                    text.ptr[index] == '\n' || text.ptr[index] == '\r')) {
            index += 1;
        }
        uint32_t start = index;
        while (index < text.len && text.ptr[index] != ' ' && text.ptr[index] != '\t' &&
               text.ptr[index] != '\n' && text.ptr[index] != '\r') {
            index += 1;
        }
        if (index == start) {
            break;
        }
        ta_event_pattern pattern = { .segments = { .offset = compiler->segments.count } };
        uint32_t segment_start = start;
        if (index - start == 2 && text.ptr[start] == '.' && text.ptr[start + 1u] == '*') {
            /* ".*" is the empty prefix with the optional ".*" suffix: every event. */
            segment_start = start + 1u;
        }
        for (uint32_t at = segment_start; at <= index; at += 1) {
            if (at < index && text.ptr[at] != '.') {
                continue;
            }
            ta_str segment = { .ptr = text.ptr + segment_start, .len = at - segment_start };
            if (segment.len == 0) {
                return ta_compiler_record(compiler, &(ta_compiler_fault){
                                                        .code = TA_COMPILE_ERROR_BAD_EVENT_DESCRIPTOR,
                                                        .subject = text,
                                                        .subject_offset = start,
                                                        .subject_length = index - start,
                                                        .message = TA_PHRASE("an event descriptor has an empty segment"),
                                                    });
            }
            ta_event_pattern_segment compiled = {0};
            ta_result result = TA_OK;
            if (segment.len == 1 && segment.ptr[0] == '*') {
                compiled.flags = TA_EVENT_PATTERN_SEGMENT_WILDCARD;
                compiled.atom = TA_ATOM_NONE;
            } else {
                result = ta_atom_intern(ta_compiler_atoms(compiler), segment, &compiled.atom);
            }
            /* A trailing `.*` adds nothing to SCXML prefix matching. */
            bool trailing_wildcard = at == index && pattern.segments.length > 0 &&
                                     compiled.flags == TA_EVENT_PATTERN_SEGMENT_WILDCARD;
            if (result == TA_OK && !trailing_wildcard) {
                result = ta_vec_push(&compiler->segments, &compiled);
                pattern.segments.length += 1;
            }
            if (result != TA_OK) {
                return result;
            }
            segment_start = at + 1u;
        }
        ta_result result = ta_vec_push(&compiler->patterns, &pattern);
        if (result != TA_OK) {
            return result;
        }
    }
    *out = (ta_span32){ .offset = first, .length = compiler->patterns.count - first };
    return TA_OK;
}

static ta_result ta_compiler_transition(ta_compiler *compiler, ta_state_id source,
                                        const ta_scxml_transition_spec *spec) {
    ta_transition_def transition = {
        .source = source,
        .condition = UINT32_MAX,
        .document_order = compiler->transitions.count,
        .event_text = TA_AUTHORED_TEXT_NONE,
    };
    ta_compiler_hash_u32(compiler, spec->kind);
    if (spec->kind == TA_SCXML_TRANSITION_EXTERNAL) {
        transition.type = TA_TRANSITION_EXTERNAL;
    } else if (spec->kind == TA_SCXML_TRANSITION_INTERNAL) {
        transition.type = TA_TRANSITION_INTERNAL;
    } else {
        return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                TA_PHRASE("unknown transition kind"));
    }
    /* Transitions are numbered in the order they are stored, after their content. */
    ta_result result = ta_compiler_span(compiler, &(ta_span_record){ .kind = TA_ELEMENT_TRANSITION,
                                                                     .id = compiler->transitions.count,
                                                                     .span = spec->span });
    if (result == TA_OK) {
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_EVENT, TA_SPEC_INDEX_NONE);
    }
    if (result == TA_OK) {
        result = ta_compiler_descriptors(compiler, spec->event, &transition.event_patterns);
    }
    if (result == TA_OK && spec->event.ptr != NULL) {
        result = ta_compiler_keep_text(compiler, spec->event, &transition.event_text);
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    if (result == TA_OK && spec->condition.kind != TA_EXPRESSION_NONE) {
        result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_CONDITION, &spec->condition, TA_EXPR_ROLE_CONDITION },
                                           &transition.condition);
    }
    if (result == TA_OK) {
        result = ta_compiler_targets(compiler, &(ta_compiler_targets_params){
                                                   .ids = spec->targets,
                                                   .within = TA_STATE_NONE,
                                                   .out = &transition.targets,
                                               });
    }
    if (result == TA_OK) {
        result = ta_compiler_block(compiler, TA_SPEC_MEMBER_ACTIONS, spec->actions, &transition.actions);
    }
    if (result == TA_OK) {
        result = ta_vec_push(&compiler->transitions, &transition);
    }
    return result;
}

typedef struct {
    ta_state_id owner;
    const ta_scxml_initial_spec *spec;
    ta_span32 children;
    ta_initial_transition_id *out;
} ta_compiler_initial_params;

static ta_result ta_compiler_initial(ta_compiler *compiler, const ta_compiler_initial_params *params) {
    ta_initial_transition_def initial = { .owner = params->owner };
    ta_result result = TA_OK;
    if (params->spec != NULL) {
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_INITIAL, TA_SPEC_INDEX_NONE);
        if (result == TA_OK) {
            result = ta_compiler_span(compiler, &(ta_span_record){ .kind = TA_ELEMENT_TRANSITION,
                                                                   .initial = true,
                                                                   .id = compiler->initials.count,
                                                                   .span = params->spec->span });
        }
        if (result != TA_OK) {
            return result;
        }
    }
    if (params->spec != NULL && params->spec->targets != NULL && params->spec->targets[0].ptr != NULL) {
        initial.authored_targets = true;
        result = ta_compiler_targets(compiler, &(ta_compiler_targets_params){
                                                   .ids = params->spec->targets,
                                                   .within = params->owner,
                                                   .out = &initial.targets,
                                               });
    } else {
        /* SCXML default: the first child state in document order. */
        ta_state_id first = TA_STATE_NONE;
        for (uint32_t index = 0; index < params->children.length && first == TA_STATE_NONE; index += 1) {
            ta_state_id child = *(ta_state_id *)ta_vec_at(&compiler->children, params->children.offset + index);
            if (!ta_state_kind_is_history(ta_compiler_state(compiler, child)->kind)) {
                first = child;
            }
        }
        if (first == TA_STATE_NONE) {
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_BAD_STRUCTURE, (ta_str){0},
                                    TA_PHRASE("no child state to enter"));
        }
        initial.targets.offset = compiler->targets.count;
        initial.targets.length = 1;
        result = ta_vec_push(&compiler->targets, &first);
    }
    if (result == TA_OK && params->spec != NULL) {
        result = ta_compiler_block(compiler, TA_SPEC_MEMBER_ACTIONS, params->spec->actions, &initial.actions);
    }
    if (result == TA_OK) {
        if (params->spec != NULL) {
            ta_compiler_leave(compiler);
        }
        *params->out = compiler->initials.count;
        result = ta_vec_push(&compiler->initials, &initial);
    }
    return result;
}

static ta_result ta_compiler_data(ta_compiler *compiler, const ta_scxml_data_spec *specs, ta_state_id owner) {
    for (uint32_t index = 0; specs != NULL && specs[index].name.ptr != NULL; index += 1) {
        ta_data_decl decl = { .owner_state = owner, .initializer = TA_INITIALIZER_NONE };
        ta_result result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_DATA, index);
        if (result == TA_OK) {
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_NAME, specs[index].name, &decl.name);
        }
        if (result == TA_OK && specs[index].initializer.kind != TA_SCXML_VALUE_NONE) {
            ta_initializer_def initializer = {0};
            result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &specs[index].initializer,
                                          &initializer.value);
            if (result == TA_OK) {
                decl.initializer = compiler->initializers.count;
                result = ta_vec_push(&compiler->initializers, &initializer);
            }
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            result = ta_vec_push(&compiler->data_decls, &decl);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

/* <donedata>: exactly one of content or params (SCXML 5.5). */
static ta_result ta_compiler_done_data(ta_compiler *compiler, const ta_scxml_done_data_spec *spec,
                                       ta_done_data_id *out) {
    ta_done_data_def done = {0};
    ta_result result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_DONE_DATA, TA_SPEC_INDEX_NONE);
    if (result == TA_OK) {
        result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->content, &done.content);
    }
    if (result == TA_OK) {
        result = ta_compiler_params(compiler, spec->params, &done.params);
    }
    if (result == TA_OK && done.params.length != 0 && done.content.kind != TA_VALUE_SOURCE_NONE) {
        result = ta_compiler_fail(compiler, TA_COMPILE_ERROR_EXCLUSIVE_MEMBERS, (ta_str){0},
                                  TA_PHRASE("a donedata has both params and content"));
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
        *out = compiler->done_data.count;
        result = ta_vec_push(&compiler->done_data, &done);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Invoke
 */

static ta_result ta_compiler_invoke_text(ta_compiler *compiler, const ta_scxml_string_spec *spec,
                                         ta_value_source *out) {
    ta_compiler_hash_u32(compiler, spec->kind);
    switch (spec->kind) {
        case TA_SCXML_STRING_NONE:
            *out = (ta_value_source){ .kind = TA_VALUE_SOURCE_NONE };
            return TA_OK;
        case TA_SCXML_STRING_LITERAL:
            out->kind = TA_VALUE_SOURCE_CONTENT;
            return ta_compiler_content(compiler, (ta_str){0},
                                       (ta_bytes){ .ptr = (const uint8_t *)spec->as.literal.ptr,
                                                   .len = spec->as.literal.len },
                                       &out->as.content);
        case TA_SCXML_STRING_EXPR:
            out->kind = TA_VALUE_SOURCE_EXPR;
            return ta_compiler_expression(compiler, &(ta_compiler_expression_params){ &spec->as.expr, TA_EXPR_ROLE_VALUE },
                                          &out->as.expr);
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown kind"));
    }
}

static ta_result ta_compiler_invoke(ta_compiler *compiler, const ta_scxml_invoke_spec *spec,
                                    ta_invoke_decl *out) {
    ta_result result = TA_OK;
    switch (spec->id.kind) {
        case TA_SCXML_ID_NONE:
            out->id.kind = TA_ID_SOURCE_GENERATED;
            break;
        case TA_SCXML_ID_LITERAL:
            out->id.kind = TA_ID_SOURCE_LITERAL;
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_ID, spec->id.as.literal, &out->id.as.literal);
            break;
        case TA_SCXML_ID_LOCATION:
            out->id.kind = TA_ID_SOURCE_LOCATION;
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_ID, &spec->id.as.location, TA_EXPR_ROLE_LOCATION },
                                               &out->id.as.location);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_ID);
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->type.kind) {
        case TA_SCXML_STRING_NONE:
            out->type.kind = TA_TEXT_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->type.kind = TA_TEXT_SOURCE_ATOM;
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_TYPE, spec->type.as.literal, &out->type.as.atom);
            break;
        case TA_SCXML_STRING_EXPR:
            out->type.kind = TA_TEXT_SOURCE_EXPR;
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_TYPE, &spec->type.as.expr, TA_EXPR_ROLE_VALUE },
                                               &out->type.as.expr);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_TYPE);
    }
    if (result == TA_OK) {
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_SRC, TA_SPEC_INDEX_NONE);
        if (result == TA_OK) {
            result = ta_compiler_invoke_text(compiler, &spec->src, &out->src);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
        }
    }
    if (result == TA_OK) {
        result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->data, &out->data);
    }
    if (result == TA_OK) {
        result = ta_compiler_params(compiler, spec->params, &out->params);
    }
    if (result == TA_OK) {
        result = ta_compiler_block(compiler, TA_SPEC_MEMBER_FINALIZE, spec->finalize, &out->finalize_actions);
    }
    out->autoforward = spec->autoforward;
    ta_compiler_hash_u32(compiler, spec->autoforward ? 1u : 0u);
    /* SCXML: src and inline content are exclusive. */
    if (result == TA_OK && out->src.kind != TA_VALUE_SOURCE_NONE && out->data.kind != TA_VALUE_SOURCE_NONE) {
        result = ta_compiler_fail(compiler, TA_COMPILE_ERROR_EXCLUSIVE_MEMBERS, (ta_str){0},
                                  TA_PHRASE("an invoke has both src and content"));
    }
    return result;
}

static ta_result ta_compiler_invokes(ta_compiler *compiler, const ta_scxml_invoke_spec *specs, ta_state_id owner) {
    for (uint32_t index = 0; specs[index].kind != TA_SCXML_INVOKE_NONE; index += 1) {
        ta_invoke_decl decl = { .owner = owner };
        ta_result result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_INVOKES, index);
        if (result == TA_OK) {
            result = ta_compiler_invoke(compiler, &specs[index], &decl);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            result = ta_vec_push(&compiler->invokes, &decl);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

static ta_result ta_compiler_structure(ta_compiler *compiler, ta_spec_member member, uint32_t index,
                                       ta_str message) {
    ta_compiler_fault fault = { .code = TA_COMPILE_ERROR_BAD_STRUCTURE, .message = message };
    return member == TA_SPEC_MEMBER_NONE ? ta_compiler_record(compiler, &fault)
                                         : ta_compiler_fail_at(compiler, member, index, &fault);
}

/* Pass two for one state. */
static ta_result ta_compiler_compile_state(ta_compiler *compiler, ta_state_id id) {
    const ta_scxml_state_spec *spec = ta_compiler_state_spec(compiler, id);
    ta_state_def state = *ta_compiler_state(compiler, id);
    ta_state_def parent = {0};
    if (state.parent != TA_STATE_NONE) {
        parent = *ta_compiler_state(compiler, state.parent);
    }
    ta_compiler_at(compiler, id);
    uint32_t real_children = 0;
    for (uint32_t index = 0; index < state.children.length; index += 1) {
        ta_state_id child = *(ta_state_id *)ta_vec_at(&compiler->children, state.children.offset + index);
        real_children += ta_state_kind_is_history(ta_compiler_state(compiler, child)->kind) ? 0u : 1u;
    }
    bool history = ta_state_kind_is_history(state.kind);
    if (spec->done_data != NULL && state.kind != TA_STATE_FINAL) {
        return ta_compiler_structure(compiler, TA_SPEC_MEMBER_DONE_DATA, TA_SPEC_INDEX_NONE,
                                     TA_PHRASE("only a final state has <donedata>"));
    }
    switch (state.kind) {
        case TA_STATE_ATOMIC:
        case TA_STATE_FINAL:
            if (state.children.length != 0) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_STATES, 0,
                                             TA_PHRASE("an atomic or final state has no child states"));
            }
            if (spec->initial != NULL) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_INITIAL, TA_SPEC_INDEX_NONE,
                                             TA_PHRASE("an atomic or final state has no <initial>"));
            }
            break;
        case TA_STATE_COMPOUND:
        case TA_STATE_PARALLEL:
            if (real_children == 0) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_NONE, 0,
                                             TA_PHRASE("a compound or parallel state needs a child state"));
            }
            if (state.kind == TA_STATE_PARALLEL && spec->initial != NULL) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_INITIAL, TA_SPEC_INDEX_NONE,
                                             TA_PHRASE("a parallel state has no <initial>"));
            }
            break;
        default:
            if (state.children.length != 0) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_STATES, 0,
                                             TA_PHRASE("a history state has no child states"));
            }
            if (state.parent == TA_STATE_NONE ||
                (parent.kind != TA_STATE_COMPOUND && parent.kind != TA_STATE_PARALLEL)) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_NONE, 0,
                                             TA_PHRASE("a history state must be inside a compound or parallel state"));
            }
            break;
    }

    state.transitions.offset = compiler->transitions.count;
    ta_result result = TA_OK;
    for (uint32_t index = 0; result == TA_OK && spec->transitions != NULL &&
                             spec->transitions[index].kind != TA_SCXML_TRANSITION_NONE;
         index += 1) {
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_TRANSITIONS, index);
        if (result == TA_OK) {
            result = ta_compiler_transition(compiler, id, &spec->transitions[index]);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
        }
    }
    state.transitions.length = compiler->transitions.count - state.transitions.offset;
    if (result != TA_OK) {
        return result;
    }
    if (history) {
        /* A history state has exactly one targetless-event default transition into its parent. */
        ta_compiler_fault fault = { .code = TA_COMPILE_ERROR_BAD_HISTORY_DEFAULT,
                                    .message = TA_PHRASE("a history state needs exactly one default transition") };
        if (state.transitions.length != 1) {
            return state.transitions.length == 0
                       ? ta_compiler_record(compiler, &fault)
                       : ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_TRANSITIONS, 1, &fault);
        }
        const ta_transition_def *fallback = ta_vec_at(&compiler->transitions, state.transitions.offset);
        if (fallback->event_patterns.length != 0 || fallback->condition != UINT32_MAX ||
            fallback->targets.length == 0) {
            fault.message = TA_PHRASE("a history default transition has targets and no event or condition");
            return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_TRANSITIONS, 0, &fault);
        }
        for (uint32_t index = 0; index < fallback->targets.length; index += 1) {
            ta_state_id target = *(ta_state_id *)ta_vec_at(&compiler->targets, fallback->targets.offset + index);
            if (!ta_compiler_descends(compiler, target, state.parent)) {
                result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_TRANSITIONS, 0);
                if (result == TA_OK) {
                    result = ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_TARGETS, index,
                                                 &(ta_compiler_fault){
                                                     .code = TA_COMPILE_ERROR_TARGET_OUTSIDE,
                                                     .subject = spec->transitions[0].targets[index],
                                                     .related = ta_compiler_state_spec(compiler, state.parent)->id,
                                                     .message = TA_PHRASE("target is not inside its state"),
                                                 });
                }
                return result;
            }
        }
        state.history_slot = compiler->history.count;
        ta_history_slot slot = {0};
        result = ta_vec_push(&compiler->history, &slot);
    }
    if (result == TA_OK && state.kind == TA_STATE_COMPOUND) {
        result = ta_compiler_initial(compiler, &(ta_compiler_initial_params){
                                                   .owner = id,
                                                   .spec = spec->initial,
                                                   .children = state.children,
                                                   .out = &state.initial,
                                               });
    }
    if (result == TA_OK) {
        result = ta_compiler_handlers(compiler, TA_SPEC_MEMBER_ONENTRY, spec->onentry, &state.onentry_handlers);
    }
    if (result == TA_OK) {
        result = ta_compiler_handlers(compiler, TA_SPEC_MEMBER_ONEXIT, spec->onexit, &state.onexit_handlers);
    }
    if (result == TA_OK) {
        result = ta_compiler_data(compiler, spec->data, id);
    }
    if (result == TA_OK && spec->done_data != NULL) {
        result = ta_compiler_done_data(compiler, spec->done_data, &state.done_data);
    }
    /* Invoke: compile this state's <invoke> children. */
    if (result == TA_OK && spec->invokes != NULL && spec->invokes[0].kind != TA_SCXML_INVOKE_NONE) {
        result = history ? ta_compiler_structure(compiler, TA_SPEC_MEMBER_INVOKES, 0,
                                                 TA_PHRASE("a history state has no <invoke>"))
                         : ta_compiler_invokes(compiler, spec->invokes, id);
    }
    if (result == TA_OK) {
        ta_atom segment = state.name;
        result = ta_event_name_child(&compiler->system->event_names, TA_EVENT_NAME_DONE_STATE, segment,
                                     &state.done_event);
    }
    if (result == TA_OK) {
        *ta_compiler_state(compiler, id) = state;
    }
    return result;
}

static ta_result ta_compiler_seal(ta_compiler *compiler, const ta_compiler_seal_params *params) {
    *params->out = NULL;
    if (params->count != NULL) {
        *params->count = params->vec->count;
    }
    if (params->vec->count == 0) {
        return TA_OK;
    }
    size_t size = params->vec->count * params->vec->element_size;
    void *copy = ta_arena_alloc(&compiler->definition->arena, size, params->vec->element_alignment);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, params->vec->items, size);
    *params->out = copy;
    return TA_OK;
}

static void ta_compiler_destroy(ta_compiler *compiler) {
    ta_vec *vecs[] = {
        &compiler->states, &compiler->state_specs, &compiler->children, &compiler->transitions,
        &compiler->initials, &compiler->targets, &compiler->actions, &compiler->handlers, &compiler->params,
        &compiler->data_decls, &compiler->initializers, &compiler->contents, &compiler->patterns,
        &compiler->segments, &compiler->expressions, &compiler->roles, &compiler->history, &compiler->done_data,
        &compiler->invokes, &compiler->texts, &compiler->path, &compiler->names, &compiler->levels,
        &compiler->blocks, &compiler->spans,
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(vecs); index += 1) {
        ta_vec_destroy(vecs[index]);
    }
}

/*
 * The side table of spans (ADR 0085), once every count is final: one span
 * per element, by kind and ID, and the names of the sources. Nothing is
 * kept when the spec knew no span, so a definition compiled without them
 * costs what it did before.
 */
static ta_result ta_compiler_lay_out_spans(ta_compiler *compiler) {
    ta_definition *definition = compiler->definition;
    if (compiler->spans.count == 0) {
        return TA_OK;
    }
    const ta_scxml_spec *spec = compiler->spec;
    uint32_t count = ta_definition_span_base(definition, TA_ELEMENT_EXPRESSION) +
                     ta_definition_element_count(definition, TA_ELEMENT_EXPRESSION);
    /* One block: the spans, the sources, then the sources' names. */
    size_t sources_at = (size_t)count * sizeof(ta_source_span);
    size_t names_at = sources_at + (size_t)spec->source_count * sizeof(ta_scxml_source);
    size_t bytes = names_at;
    for (uint32_t index = 0; index < spec->source_count; index += 1u) {
        bytes += spec->sources[index].name.len;
    }
    uint8_t *block = ta_mem_alloc(definition->allocator, bytes, _Alignof(ta_source_span));
    if (block == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_source_span *spans = (ta_source_span *)(void *)block;
    ta_mem_zero(spans, sources_at);
    for (uint32_t index = 0; index < compiler->spans.count; index += 1u) {
        const ta_span_record *record = ta_vec_at(&compiler->spans, index);
        uint32_t id = record->initial ? definition->transition_count + record->id : record->id;
        spans[ta_definition_span_base(definition, record->kind) + id] = spec->spans[record->span - 1u];
    }
    ta_scxml_source *sources = spec->source_count != 0 ? (ta_scxml_source *)(void *)(block + sources_at) : NULL;
    char *names = (char *)block + names_at;
    for (uint32_t index = 0; index < spec->source_count; index += 1u) {
        ta_str name = spec->sources[index].name;
        if (name.len != 0) {
            ta_mem_copy(names, name.ptr, name.len);
        }
        sources[index].name = (ta_str){ .ptr = names, .len = name.len };
        names += name.len;
    }
    definition->spans = spans;
    definition->span_count = count;
    definition->sources = sources;
    definition->source_count = spec->source_count;
    definition->span_bytes = bytes;
    definition->span_allocator = definition->allocator;
    return TA_OK;
}

static ta_result ta_compiler_identity(ta_compiler *compiler) {
    ta_definition *definition = compiler->definition;
    const ta_scxml_spec *spec = compiler->spec;
    ta_str family = spec->name;
    ta_bytes revision = {0};
    static const char hex[] = "0123456789abcdef";
    char fingerprint[16];
    /* An identity with an empty revision names only the family: the
     * revision is still the fingerprint (ADR 0077). */
    bool explicit_revision = spec->identity != NULL && spec->identity->revision.len != 0;
    definition->fingerprinted = !explicit_revision;
    /* Kept whatever the revision, so an explicit revision reused for other
     * content is caught at registration (ADR 0077). */
    definition->content_hash = compiler->hash;
    /* A family-only identity is the default for a chart with no name. */
    if (explicit_revision || (spec->identity != NULL && (family.ptr == NULL || family.len == 0))) {
        family = spec->identity->family.name;
    }
    if (explicit_revision) {
        revision = spec->identity->revision;
    } else {
        for (uint32_t index = 0; index < 16; index += 1) {
            fingerprint[index] = hex[(compiler->hash >> (60u - index * 4u)) & 0xfu];
        }
        revision = (ta_bytes){ .ptr = (const uint8_t *)fingerprint, .len = 16 };
    }
    if (family.ptr == NULL || family.len == 0) {
        return ta_compiler_fail_at(compiler, spec->identity != NULL ? TA_SPEC_MEMBER_IDENTITY : TA_SPEC_MEMBER_NAME,
                                   TA_SPEC_INDEX_NONE,
                                   &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_NO_FAMILY,
                                                         .message = TA_PHRASE("the chart has no name and no identity family") });
    }
    ta_result result = ta_arena_copy_str(&definition->arena, family, &definition->identity.family.name);
    ta_str revision_copy = {0};
    if (result == TA_OK) {
        result = ta_arena_copy_str(&definition->arena,
                                   (ta_str){ .ptr = (const char *)revision.ptr, .len = revision.len },
                                   &revision_copy);
    }
    definition->identity.revision = (ta_bytes){ .ptr = (const uint8_t *)revision_copy.ptr,
                                                .len = revision_copy.len };
    if (result == TA_OK) {
        result = ta_atom_intern(ta_compiler_atoms(compiler), family, &definition->family);
    }
    definition->document_name = TA_ATOM_NONE;
    if (result == TA_OK && spec->name.ptr != NULL && spec->name.len != 0) {
        result = ta_atom_intern(ta_compiler_atoms(compiler), spec->name, &definition->document_name);
    }
    definition->authored_binding = spec->binding;
    if (result == TA_OK && spec->datamodel.ptr != NULL && spec->datamodel.len != 0) {
        result = ta_arena_copy_str(&definition->arena, spec->datamodel, &definition->authored_datamodel);
    }
    return result;
}

static ta_result ta_compiler_finish(ta_compiler *compiler) {
    ta_definition *definition = compiler->definition;
    const ta_compiler_seal_params seals[] = {
        { &compiler->states, (void **)&definition->states, &definition->state_count },
        { &compiler->children, (void **)&definition->children, &definition->child_count },
        { &compiler->transitions, (void **)&definition->transitions, &definition->transition_count },
        { &compiler->initials, (void **)&definition->initial_transitions, &definition->initial_transition_count },
        { &compiler->targets, (void **)&definition->targets, &definition->target_count },
        { &compiler->actions, (void **)&definition->action_defs, &definition->action_count },
        { &compiler->handlers, (void **)&definition->handlers, &definition->handler_count },
        { &compiler->params, (void **)&definition->params, &definition->param_count },
        { &compiler->data_decls, (void **)&definition->data_decls, &definition->data_decl_count },
        { &compiler->initializers, (void **)&definition->initializers, &definition->initializer_count },
        { &compiler->contents, (void **)&definition->contents, &definition->content_count },
        { &compiler->patterns, (void **)&definition->event_patterns, &definition->event_pattern_count },
        { &compiler->segments, (void **)&definition->event_pattern_segments, &definition->event_pattern_segment_count },
        { &compiler->history, (void **)&definition->history.slots, &definition->history.slot_count },
        { &compiler->done_data, (void **)&definition->done_data, &definition->done_data_count },
        { &compiler->invokes, (void **)&definition->invokes, &definition->invoke_decl_count },
        { &compiler->texts, (void **)&definition->authored_texts, &definition->authored_text_count },
    };
    ta_result result = TA_OK;
    for (uint32_t index = 0; result == TA_OK && index < TA_ARRAY_COUNT(seals); index += 1) {
        result = ta_compiler_seal(compiler, &seals[index]);
    }
    if (result != TA_OK) {
        return result;
    }
    result = ta_definition_place_actions(definition);
    if (result != TA_OK) {
        return result;
    }
    definition->max_action_depth = compiler->action_depth;
    if (definition->global_script_actions.length != 0 && definition->max_action_depth == 0) {
        definition->max_action_depth = 1;
    }
    /* Depths in one pass: every parent has a lower ID than its children. */
    if (definition->state_count != 0) {
        size_t size = (size_t)definition->state_count * sizeof(uint32_t);
        uint32_t *depths = ta_mem_alloc(compiler->allocator, size, _Alignof(uint32_t));
        if (depths == NULL) {
            return TA_ERR_NOMEM;
        }
        for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
            ta_state_id parent = definition->states[state].parent;
            depths[state] = parent == TA_STATE_NONE ? 1u : depths[parent] + 1u;
            if (depths[state] > definition->max_state_depth) {
                definition->max_state_depth = depths[state];
            }
        }
        ta_mem_free(compiler->allocator, depths,
                    &(ta_allocator_free_params){ .size = size, .alignment = _Alignof(uint32_t) });
    }
    uint32_t words = ta_bits_words(definition->state_count);
    if (words * 8u > UINT16_MAX) {
        return ta_compiler_fail(compiler, TA_COMPILE_ERROR_LIMIT, (ta_str){0},
                                TA_PHRASE("the chart has more states than a configuration can hold"));
    }
    definition->configuration = (ta_configuration_layout){
        .kind = TA_CONFIG_WORDS,
        .bytes = (uint16_t)(words * 8u),
        .alignment = (uint16_t)_Alignof(uint64_t),
        .atomic_state_count = definition->state_count,
    };
    for (uint32_t slot = 0; slot < definition->history.slot_count; slot += 1) {
        definition->history.slots[slot] = (ta_history_slot){ .offset = slot * words * 8u,
                                                             .bytes = (uint16_t)(words * 8u) };
    }
    definition->history.storage_bytes = definition->history.slot_count * words * 8u;
    return ta_compiler_identity(compiler);
}

/*
 * The compile order, which decides which fault a chart with several reports
 * (ADR 0058 section 2): the data-model requirement; pass one over every state
 * in document order (kind, ID, duplicates); the root's <data>; pass two for
 * each state in document order; the root <initial>; the root <script>; the
 * configuration limit and the family; then the data model's preparation.
 */
/* One history state on the walk below, and the next default target to follow. */
typedef struct {
    ta_state_id state;
    uint32_t next;
} ta_history_walk;

/* The default targets of a compiled history state. */
static ta_span32 ta_compiler_history_targets(ta_compiler *compiler, ta_state_id history) {
    const ta_state_def *state = ta_compiler_state(compiler, history);
    const ta_transition_def *fallback = ta_vec_at(&compiler->transitions, state->transitions.offset);
    return fallback->targets;
}

/*
 * A history state's default may target another history state, which is
 * then entered through its own default. That chain must end in real
 * states: SCXML 3.11 requires a target to become a legal configuration
 * once ancestors and defaults are added, and a loop never reaches an
 * atomic state. So a loop, `h` targeting `h` or `h1` and `h2` targeting
 * each other, is BAD_HISTORY_DEFAULT.
 *
 * A depth-first walk over the edges "this history's default targets that
 * history state", with an explicit stack (ADR 0060). Walks start from the
 * history states in document order and each state is finished once, so
 * the loop reported is the same every time. The error is at the target
 * that closes the loop, with the history it returns to as `related`.
 */
static ta_result ta_compiler_history_loops(ta_compiler *compiler) {
    uint32_t count = compiler->states.count;
    uint32_t histories = 0;
    for (ta_state_id state = 0; state < count; state += 1u) {
        histories += ta_state_kind_is_history(ta_compiler_state(compiler, state)->kind) ? 1u : 0u;
    }
    if (histories < 1u) {
        return TA_OK;
    }
    enum { UNSEEN = 0, ON_WALK = 1, FINISHED = 2 };
    ta_vec marks;
    ta_vec walk;
    ta_vec_init(&marks, compiler->allocator, TA_VEC_LAYOUT(uint8_t));
    ta_vec_init(&walk, compiler->allocator, TA_VEC_LAYOUT(ta_history_walk));
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&marks, count, &base);
    for (ta_state_id start = 0; result == TA_OK && start < count; start += 1u) {
        uint8_t *mark = marks.items;
        if (!ta_state_kind_is_history(ta_compiler_state(compiler, start)->kind) || mark[start] != UNSEEN) {
            continue;
        }
        mark[start] = ON_WALK;
        result = ta_vec_push(&walk, &(ta_history_walk){ .state = start });
        while (result == TA_OK && walk.count != 0) {
            ta_history_walk *top = ta_vec_at(&walk, walk.count - 1u);
            ta_span32 targets = ta_compiler_history_targets(compiler, top->state);
            if (top->next == targets.length) {
                ((uint8_t *)marks.items)[top->state] = FINISHED;
                walk.count -= 1u;
                continue;
            }
            uint32_t index = top->next;
            top->next += 1u;
            ta_state_id history = top->state;
            ta_state_id target = *(ta_state_id *)ta_vec_at(&compiler->targets, targets.offset + index);
            if (!ta_state_kind_is_history(ta_compiler_state(compiler, target)->kind)) {
                continue;
            }
            uint8_t seen = ((uint8_t *)marks.items)[target];
            if (seen == ON_WALK) {
                ta_compiler_at(compiler, history);
                result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_TRANSITIONS, 0);
                if (result == TA_OK) {
                    result = ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_TARGETS, index,
                                                 &(ta_compiler_fault){
                                                     .code = TA_COMPILE_ERROR_BAD_HISTORY_DEFAULT,
                                                     .subject = ta_compiler_state_spec(compiler, history)
                                                                    ->transitions[0].targets[index],
                                                     .related = ta_compiler_state_spec(compiler, target)->id,
                                                     .message = TA_PHRASE("history defaults loop and never reach a state"),
                                                 });
                }
            } else if (seen == UNSEEN) {
                ((uint8_t *)marks.items)[target] = ON_WALK;
                result = ta_vec_push(&walk, &(ta_history_walk){ .state = target });
            }
        }
    }
    ta_vec_destroy(&walk);
    ta_vec_destroy(&marks);
    return result;
}

static ta_result ta_compiler_run(ta_compiler *compiler, ta_datamodel datamodel) {
    const ta_scxml_spec *spec = compiler->spec;
    ta_definition *definition = compiler->definition;
    if (spec->datamodel.ptr != NULL && spec->datamodel.len != 0 && !ta_str_equal(spec->datamodel, datamodel.v->name)) {
        return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_DATAMODEL, TA_SPEC_INDEX_NONE,
                                   &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_DATAMODEL_MISMATCH,
                                                         .subject = spec->datamodel,
                                                         .message = TA_PHRASE("the chart requires another data model") });
    }
    ta_compiler_hash_str(compiler, spec->name);
    ta_compiler_hash_u32(compiler, spec->binding);
    ta_span32 roots = {0};
    ta_result result = ta_compiler_collect(compiler, spec->states, TA_STATE_NONE, &roots);
    result = ta_compiler_index_names(compiler, result);
    ta_compiler_at(compiler, TA_STATE_NONE);
    if (result == TA_OK && compiler->states.count == 0) {
        result = ta_compiler_fail(compiler, TA_COMPILE_ERROR_NO_STATES, (ta_str){0},
                                  TA_PHRASE("the chart has no states"));
    }
    /* Data declarations are listed root first, then by state in document order. */
    if (result == TA_OK) {
        result = ta_compiler_data(compiler, spec->data, TA_STATE_NONE);
    }
    for (ta_state_id state = 0; result == TA_OK && state < compiler->states.count; state += 1) {
        result = ta_compiler_compile_state(compiler, state);
    }
    if (result == TA_OK) {
        result = ta_compiler_history_loops(compiler);
    }
    if (result == TA_OK) {
        ta_compiler_at(compiler, TA_STATE_NONE);
        result = ta_compiler_initial(compiler, &(ta_compiler_initial_params){
                                                   .owner = TA_STATE_NONE,
                                                   .spec = spec->initial,
                                                   .children = roots,
                                                   .out = &definition->root_initial,
                                               });
    }
    if (result == TA_OK && spec->script.kind != TA_EXPRESSION_NONE) {
        uint32_t base = 0;
        ta_action_def script = { .kind = TA_ACTION_SCRIPT };
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_SCRIPT, TA_SPEC_INDEX_NONE);
        if (result == TA_OK) {
            result = ta_compiler_span(compiler, &(ta_span_record){ .kind = TA_ELEMENT_ACTION,
                                                                   .id = compiler->actions.count,
                                                                   .span = spec->script_span });
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_SCRIPT, &spec->script, TA_EXPR_ROLE_SCRIPT }, &script.as.script);
        }
        if (result == TA_OK) {
            result = ta_vec_extend(&compiler->actions, 1, &base);
        }
        if (result == TA_OK) {
            *(ta_action_def *)ta_vec_at(&compiler->actions, base) = script;
            definition->global_script_actions = (ta_span32){ .offset = base, .length = 1 };
        }
    }
    if (result == TA_OK) {
        ta_compiler_at(compiler, TA_STATE_NONE);
        result = ta_compiler_finish(compiler);
    }
    return result;
}

static bool ta_datamodel_complete(const ta_datamodel *datamodel) {
    const ta_datamodel_vtable *v = datamodel->v;
    return v != NULL && v->prepare_definition != NULL && v->destroy_definition != NULL &&
           v->instance_size != NULL && v->instance_alignment != NULL && v->init_instance != NULL &&
           v->destroy_instance != NULL;
}

typedef struct {
    ta_compile_error *error;
    ta_arena *error_arena;
    /* UINT32_MAX for a compile; otherwise a replay that stops at this
     * expression ID and never prepares or keeps a definition. */
    uint32_t locate;
    ta_expression_rejection *rejected;
    ta_definition **out;
} ta_compile_run_params;

static ta_result ta_compile(ta_system *system, const ta_definition_compile_params *params,
                            const ta_compile_run_params *run) {
    const ta_scxml_spec *spec = params->spec;
    ta_definition *definition = ta_mem_alloc(params->allocator, sizeof(ta_definition),
                                             _Alignof(ta_definition));
    if (definition == NULL) {
        return TA_ERR_NOMEM;
    }
    *definition = (ta_definition){
        .allocator = params->allocator,
        .system = system,
        .root_initial = TA_INITIAL_TRANSITION_NONE,
        .binding_mode = spec->binding == TA_SCXML_BINDING_LATE ? TA_BINDING_LATE : TA_BINDING_EARLY,
        .datamodel = params->datamodel,
    };
    ta_result result = ta_arena_init(&definition->arena, params->allocator, 4096);
    if (result != TA_OK) {
        ta_mem_free(params->allocator, definition,
                    &(ta_allocator_free_params){ .size = sizeof(ta_definition),
                                                 .alignment = _Alignof(ta_definition) });
        return result;
    }

    ta_compiler compiler = {
        .system = system,
        .allocator = params->allocator,
        .spec = spec,
        .definition = definition,
        .path_state = TA_STATE_NONE,
        .error = run->error,
        .error_arena = run->error_arena,
        .locate = run->locate,
        .hash = 14695981039346656037ull,
    };
    const struct { ta_vec *vec; const ta_tree_list_layout *layout; } vecs[] = {
        { &compiler.states, TA_VEC_LAYOUT(ta_state_def) },
        { &compiler.state_specs, TA_VEC_LAYOUT(const ta_scxml_state_spec *) },
        { &compiler.children, TA_VEC_LAYOUT(ta_state_id) },
        { &compiler.transitions, TA_VEC_LAYOUT(ta_transition_def) },
        { &compiler.initials, TA_VEC_LAYOUT(ta_initial_transition_def) },
        { &compiler.targets, TA_VEC_LAYOUT(ta_state_id) },
        { &compiler.actions, TA_VEC_LAYOUT(ta_action_def) },
        { &compiler.handlers, TA_VEC_LAYOUT(ta_span32) },
        { &compiler.params, TA_VEC_LAYOUT(ta_param_def) },
        { &compiler.data_decls, TA_VEC_LAYOUT(ta_data_decl) },
        { &compiler.initializers, TA_VEC_LAYOUT(ta_initializer_def) },
        { &compiler.contents, TA_VEC_LAYOUT(ta_content_def) },
        { &compiler.patterns, TA_VEC_LAYOUT(ta_event_pattern) },
        { &compiler.segments, TA_VEC_LAYOUT(ta_event_pattern_segment) },
        { &compiler.expressions, TA_VEC_LAYOUT(ta_expression) },
        { &compiler.roles, TA_VEC_LAYOUT(ta_expr_role) },
        { &compiler.history, TA_VEC_LAYOUT(ta_history_slot) },
        { &compiler.done_data, TA_VEC_LAYOUT(ta_done_data_def) },
        { &compiler.invokes, TA_VEC_LAYOUT(ta_invoke_decl) },
        { &compiler.texts, TA_VEC_LAYOUT(ta_str) },
        { &compiler.path, TA_VEC_LAYOUT(ta_spec_step) },
        { &compiler.names, TA_VEC_LAYOUT(ta_state_key) },
        { &compiler.levels, TA_VEC_LAYOUT(ta_collect_frame) },
        { &compiler.blocks, TA_VEC_LAYOUT(ta_block_frame) },
        { &compiler.spans, TA_VEC_LAYOUT(ta_span_record) },
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(vecs); index += 1) {
        ta_vec_init(vecs[index].vec, params->allocator, vecs[index].layout);
    }

    result = ta_compiler_run(&compiler, params->datamodel);
    if ((result == TA_ERR_INVALID || result == TA_ERR_UNSUPPORTED) && !compiler.failed) {
        /* A refusal from below the compiler's own checks. */
        ta_compiler_at(&compiler, TA_STATE_NONE);
        (void)ta_compiler_fail(&compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                               TA_PHRASE("the spec was refused"));
    }
    /* The expression table moves to the definition even on failure, so it is released once. */
    ta_result sealed = ta_compiler_seal(&compiler, &(ta_compiler_seal_params){
                                                       .vec = &compiler.expressions,
                                                       .out = (void **)&definition->expressions,
                                                       .count = &definition->expression_count,
                                                   });
    if (sealed == TA_OK) {
        sealed = ta_compiler_seal(&compiler, &(ta_compiler_seal_params){
                                                 .vec = &compiler.roles,
                                                 .out = (void **)&definition->expression_roles,
                                             });
    }
    if (sealed != TA_OK) {
        /* Could not move host expressions; release them from the vector. */
        for (uint32_t index = 0; index < compiler.expressions.count; index += 1) {
            ta_expression *expression = ta_vec_at(&compiler.expressions, index);
            if (expression->kind == TA_EXPRESSION_HOST && expression->as.host.v->release != NULL) {
                expression->as.host.v->release(expression->as.host.self, expression->as.host.value,
                                               params->allocator);
            }
        }
        definition->expression_count = 0;
        result = result == TA_OK ? sealed : result;
    }
    if (result == TA_OK && run->locate == UINT32_MAX) {
        result = ta_compiler_lay_out_spans(&compiler);
    }
    ta_compiler_destroy(&compiler);

    if (run->locate != UINT32_MAX) {
        ta_definition_destroy(definition);
        return result;
    }
    if (result == TA_OK) {
        result = params->datamodel.v->prepare_definition(
            params->datamodel.self,
            &(ta_datamodel_prepare_definition_params){
                .expressions = definition->expressions,
                .roles = definition->expression_roles,
                .expression_count = definition->expression_count,
                .allocator = params->allocator,
                .shared = &definition->datamodel_shared,
                .rejected = run->rejected,
            });
        definition->datamodel_prepared = result == TA_OK;
    }
    if (result == TA_OK) {
        definition->datamodel_instance_size =
            params->datamodel.v->instance_size(params->datamodel.self, definition->datamodel_shared);
        definition->datamodel_instance_alignment =
            params->datamodel.v->instance_alignment(params->datamodel.self, definition->datamodel_shared);
        if (definition->datamodel_instance_alignment == 0) {
            definition->datamodel_instance_alignment = 1;
        }
        ta_actor_layout(definition);
    }
    if (result != TA_OK) {
        ta_definition_destroy(definition);
        return result;
    }
    definition->unregistered_next = system->unregistered_definitions;
    if (definition->unregistered_next != NULL) {
        definition->unregistered_next->unregistered_previous = definition;
    }
    system->unregistered_definitions = definition;
    *run->out = definition;
    return TA_OK;
}

/*
 * The data model refused the prepared expression table. Name the expression
 * it pointed at by replaying the walk to it: expression IDs are assigned in
 * walk order, so nothing is stored per expression on the success path.
 */
/* The longest prefix of `text` of at most `limit` bytes that does not end
 * inside a UTF-8 sequence. */
static uint32_t ta_utf8_prefix(ta_str text, uint32_t limit) {
    if (text.len <= limit) {
        return text.len;
    }
    uint32_t end = limit;
    while (end > 0 && ((unsigned char)text.ptr[end] & 0xC0u) == 0x80u) {
        end -= 1u;
    }
    return end;
}

static void ta_compile_rejected(ta_system *system, const ta_definition_compile_params *params,
                                const ta_expression_rejection *rejection) {
    ta_compile_error *error = params->error;
    /* Copied first: the message is the adapter's until core calls it again. */
    ta_str message = TA_PHRASE("the data model refused this expression");
    if (params->error_arena != NULL && rejection->message.ptr != NULL && rejection->message.len != 0) {
        ta_str copy = {0};
        ta_str kept = { .ptr = rejection->message.ptr,
                        .len = ta_utf8_prefix(rejection->message, system->params[TA_PARAM_COMPILE_MESSAGE_BYTES]) };
        if (ta_arena_copy_str(params->error_arena, kept, &copy) == TA_OK) {
            message = copy;
        }
    }
    if (rejection->expression != TA_EXPR_ID_NONE) {
        ta_result replayed = ta_compile(system, params, &(ta_compile_run_params){
                                                            .error = error,
                                                            .error_arena = params->error_arena,
                                                            .locate = rejection->expression,
                                                        });
        if (replayed == TA_OK) {
            *error = (ta_compile_error){0}; /* no such expression: about none */
        }
    }
    error->code = TA_COMPILE_ERROR_EXPRESSION_REJECTED;
    if (rejection->expression != TA_EXPR_ID_NONE && rejection->offset <= error->subject.len &&
        rejection->length <= error->subject.len - rejection->offset) {
        error->subject_offset = rejection->offset;
        error->subject_length = rejection->length;
    }
    error->message = message;
}

/*
 * A compiled definition whose interpreter needs more scratch than the
 * system's interpreterBytes (ADR 0092 section 3) is refused here, before it
 * can be registered, with both figures in the error.
 */
static ta_result ta_compile_check_scratch(ta_system *system, const ta_definition_compile_params *params) {
    ta_definition *definition = *params->out;
    size_t need = ta_interpreter_scratch_need(definition);
    uint32_t have = system->params[TA_PARAM_INTERPRETER_BYTES];
    if (need <= have) {
        return TA_OK;
    }
    ta_definition_destroy(definition);
    *params->out = NULL;
    if (params->error == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_str message = TA_PHRASE("the chart needs more interpreter scratch than this system has (interpreterBytes)");
    char text[128];
    ta_text_writer writer = { .buffer = text, .capacity = sizeof text };
    ta_text_put(&writer, TA_PHRASE("the chart needs "));
    ta_text_put_u64(&writer, (uint64_t)need);
    ta_text_put(&writer, TA_PHRASE(" bytes of interpreter scratch; this system has "));
    ta_text_put_u64(&writer, have);
    ta_text_put(&writer, TA_PHRASE(" (interpreterBytes)"));
    uint32_t needed = 0;
    ta_str copy = { 0 };
    if (params->error_arena != NULL && ta_text_finish(&writer, &needed) == TA_OK &&
        ta_arena_copy_str(params->error_arena, (ta_str){ .ptr = text, .len = writer.length }, &copy) == TA_OK) {
        message = copy;
    }
    *params->error = (ta_compile_error){ .code = TA_COMPILE_ERROR_LIMIT, .message = message };
    return TA_ERR_UNSUPPORTED;
}

ta_result ta_definition_compile_spec(ta_system *system, const ta_definition_compile_params *params) {
    if (params != NULL && params->error != NULL) {
        *params->error = (ta_compile_error){0};
    }
    if (system == NULL || params == NULL || params->spec == NULL || params->out == NULL ||
        params->allocator.v == NULL || !ta_datamodel_complete(&params->datamodel)) {
        return TA_ERR_INVALID;
    }
    ta_expression_rejection rejection = { .expression = TA_EXPR_ID_NONE };
    ta_compile_error error = {0};
    ta_result result = ta_compile(system, params, &(ta_compile_run_params){
                                                      .error = params->error != NULL ? &error : NULL,
                                                      .error_arena = params->error_arena,
                                                      .locate = UINT32_MAX,
                                                      .rejected = &rejection,
                                                      .out = params->out,
                                                  });
    if (result == TA_OK) {
        return ta_compile_check_scratch(system, params);
    }
    if (params->error == NULL || (result != TA_ERR_INVALID && result != TA_ERR_UNSUPPORTED)) {
        return result;
    }
    if (error.code == TA_COMPILE_ERROR_NONE) {
        /* The compiler passed the spec and the data model refused it. */
        *params->error = (ta_compile_error){0};
        ta_compile_rejected(system, params, &rejection);
        return result;
    }
    *params->error = error;
    return result;
}

/* ------------------------------------------------------------------------
 * Formatting
 */

/*
 * The formatter's output: one buffer of compileTextBytes, allocated once.
 * Appending past the end is cut off, which the field limits below keep from
 * happening; it is the backstop, not the rule.
 */
typedef struct {
    char *bytes;
    uint32_t length;
    uint32_t capacity;
} ta_text_builder;

static void ta_text_append(ta_text_builder *text, ta_str piece) {
    uint32_t room = text->capacity - text->length;
    uint32_t count = ta_utf8_prefix(piece, room);
    if (count != 0) {
        ta_mem_copy(text->bytes + text->length, piece.ptr, count);
        text->length += count;
    }
}

static void ta_text_append_u32(ta_text_builder *text, uint32_t value) {
    char digits[10];
    uint32_t count = 0;
    do {
        digits[sizeof(digits) - 1u - count] = (char)('0' + value % 10u);
        value /= 10u;
        count += 1u;
    } while (value != 0);
    ta_text_append(text, (ta_str){ .ptr = digits + sizeof(digits) - count, .len = count });
}

/*
 * Output limits for one field, and how much of a deep path is shown, from
 * the line's size (compileTextBytes, ADR 0092): IDs, the subject and related
 * text get a sixteenth, the message three thirty-seconds, and the path what
 * is left once those and its elision marker have their worst case, a
 * quarter of its steps at the head. At 1024 bytes that is 64, 96, and 2 and
 * 6 steps, the limits this formatter always had.
 */
typedef struct {
    uint32_t id;
    uint32_t text;
    uint32_t head;
    uint32_t tail;
} ta_format_limits;

static ta_format_limits ta_format_limits_for(uint32_t size) {
    ta_format_limits limits = { .id = size / 16u, .text = size * 3u / 32u };
    /* ": " message ' "' subject '"' "; it must be inside \"" related '"' */
    uint32_t fixed = 2u + limits.text + 2u + limits.id + 1u + 22u + limits.id + 1u;
    uint32_t elision = 19u; /* ".[4294967295 steps]" */
    /* "." member "[" index "]" "(" id ")": the longest member name is 11 bytes */
    uint32_t step = 1u + 11u + 12u + 2u + limits.id;
    uint32_t steps = size > fixed + elision ? (size - fixed - elision) / step : 0u;
    if (steps < 2u) {
        steps = 2u;
    }
    limits.head = steps / 4u != 0 ? steps / 4u : 1u;
    limits.tail = steps - limits.head;
    return limits;
}

/*
 * The length in bytes of the unit of `text` starting at `at`: a well-formed
 * UTF-8 sequence, or one byte that is not part of one.
 */
static uint32_t ta_utf8_unit(ta_str text, uint32_t at) {
    unsigned char lead = (unsigned char)text.ptr[at];
    uint32_t length = lead < 0xC0u ? 1u : lead < 0xE0u ? 2u : lead < 0xF0u ? 3u : lead < 0xF8u ? 4u : 1u;
    if (length > text.len - at) {
        return 1u;
    }
    for (uint32_t index = 1; index < length; index += 1u) {
        if (((unsigned char)text.ptr[at + index] & 0xC0u) != 0x80u) {
            return 1u;
        }
    }
    return length;
}

/* How a unit is written: escaped when it would break the line, or, in a
 * quoted field, the quotes. */
static uint32_t ta_text_escape(ta_str text, uint32_t at, uint32_t unit, bool quoted, char out[4]) {
    unsigned char byte = (unsigned char)text.ptr[at];
    static const char hex[] = "0123456789abcdef";
    if (unit == 1u && (byte < 0x20u || byte == 0x7Fu || byte >= 0x80u)) {
        if (byte == '\n' || byte == '\t' || byte == '\r') {
            out[0] = '\\';
            out[1] = byte == '\n' ? 'n' : byte == '\t' ? 't' : 'r';
            return 2u;
        }
        out[0] = '\\';
        out[1] = 'x';
        out[2] = hex[byte >> 4];
        out[3] = hex[byte & 0xFu];
        return 4u;
    }
    if (quoted && unit == 1u && (byte == '"' || byte == '\\')) {
        out[0] = '\\';
        out[1] = (char)byte;
        return 2u;
    }
    ta_mem_copy(out, text.ptr + at, unit);
    return unit;
}

/*
 * Appends `value` escaped, in at most `limit` bytes of output. When it does
 * not fit, it is cut at a unit boundary and ends in "...". A `quoted` field
 * also escapes quotes and backslashes.
 */
static void ta_text_append_field(ta_text_builder *text, ta_str value, uint32_t limit, bool quoted) {
    uint32_t total = 0;
    char unit_text[4];
    for (uint32_t at = 0; at < value.len && total <= limit;) {
        uint32_t unit = ta_utf8_unit(value, at);
        total += ta_text_escape(value, at, unit, quoted, unit_text);
        at += unit;
    }
    uint32_t budget = total <= limit ? limit : limit - 3u;
    uint32_t written = 0;
    for (uint32_t at = 0; at < value.len;) {
        uint32_t unit = ta_utf8_unit(value, at);
        uint32_t size = ta_text_escape(value, at, unit, quoted, unit_text);
        if (written + size > budget) {
            break;
        }
        ta_text_append(text, (ta_str){ .ptr = unit_text, .len = size });
        written += size;
        at += unit;
    }
    if (total > limit) {
        ta_text_append(text, TA_PHRASE("..."));
    }
}

static ta_str ta_spec_member_name(ta_spec_member member) {
    switch (member) {
        case TA_SPEC_MEMBER_STATES: return TA_PHRASE("states");
        case TA_SPEC_MEMBER_TRANSITIONS: return TA_PHRASE("transitions");
        case TA_SPEC_MEMBER_TARGETS: return TA_PHRASE("targets");
        case TA_SPEC_MEMBER_ONENTRY: return TA_PHRASE("onentry");
        case TA_SPEC_MEMBER_ONEXIT: return TA_PHRASE("onexit");
        case TA_SPEC_MEMBER_ACTIONS: return TA_PHRASE("actions");
        case TA_SPEC_MEMBER_THEN: return TA_PHRASE("then");
        case TA_SPEC_MEMBER_ELSE: return TA_PHRASE("else");
        case TA_SPEC_MEMBER_BODY: return TA_PHRASE("body");
        case TA_SPEC_MEMBER_FINALIZE: return TA_PHRASE("finalize");
        case TA_SPEC_MEMBER_INVOKES: return TA_PHRASE("invokes");
        case TA_SPEC_MEMBER_DATA: return TA_PHRASE("data");
        case TA_SPEC_MEMBER_PARAMS: return TA_PHRASE("params");
        case TA_SPEC_MEMBER_INITIAL: return TA_PHRASE("initial");
        case TA_SPEC_MEMBER_DONE_DATA: return TA_PHRASE("done_data");
        case TA_SPEC_MEMBER_SCRIPT: return TA_PHRASE("script");
        case TA_SPEC_MEMBER_ID: return TA_PHRASE("id");
        case TA_SPEC_MEMBER_NAME: return TA_PHRASE("name");
        case TA_SPEC_MEMBER_EVENT: return TA_PHRASE("event");
        case TA_SPEC_MEMBER_CONDITION: return TA_PHRASE("condition");
        case TA_SPEC_MEMBER_TYPE: return TA_PHRASE("type");
        case TA_SPEC_MEMBER_SRC: return TA_PHRASE("src");
        case TA_SPEC_MEMBER_TARGET: return TA_PHRASE("target");
        case TA_SPEC_MEMBER_DELAY: return TA_PHRASE("delay");
        case TA_SPEC_MEMBER_DATA_VALUE: return TA_PHRASE("value");
        case TA_SPEC_MEMBER_LOCATION: return TA_PHRASE("location");
        case TA_SPEC_MEMBER_EXPR: return TA_PHRASE("expr");
        case TA_SPEC_MEMBER_ITEM: return TA_PHRASE("item");
        case TA_SPEC_MEMBER_INDEX: return TA_PHRASE("index");
        case TA_SPEC_MEMBER_SENDID: return TA_PHRASE("sendid");
        case TA_SPEC_MEMBER_DATAMODEL: return TA_PHRASE("datamodel");
        case TA_SPEC_MEMBER_IDENTITY: return TA_PHRASE("identity");
        case TA_SPEC_MEMBER_SPAN: return TA_PHRASE("span");
        default: return TA_PHRASE("?");
    }
}

ta_result ta_compile_error_format(const ta_compile_error_format_params *params) {
    if (params == NULL || params->error == NULL || params->arena == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_compile_error *error = params->error;
    uint32_t size = params->system != NULL ? params->system->params[TA_PARAM_COMPILE_TEXT_BYTES]
                                           : (uint32_t)TA_PARAM_DEFAULT_COMPILE_TEXT_BYTES;
    ta_format_limits limits = ta_format_limits_for(size);
    ta_text_builder text = { .bytes = ta_arena_alloc(params->arena, size, 1), .capacity = size };
    if (text.bytes == NULL) {
        return TA_ERR_NOMEM;
    }
    /*
     * A deep path shows its first and last steps. States are resolved while
     * the path goes through states only, so the walk follows every step,
     * shown or not.
     */
    const ta_scxml_state_spec *list = params->spec != NULL ? params->spec->states : NULL;
    uint32_t length = error->path_length;
    bool elide = length > limits.head + limits.tail;
    for (uint32_t index = 0; index < length; index += 1u) {
        ta_spec_step step = error->path[index];
        const ta_scxml_state_spec *state = NULL;
        if (step.member == TA_SPEC_MEMBER_STATES && list != NULL) {
            state = &list[step.index];
            list = state->states;
        } else {
            list = NULL;
        }
        bool shown = !elide || index < limits.head || index >= length - limits.tail;
        if (elide && index == limits.head) {
            ta_text_append(&text, TA_PHRASE(".["));
            ta_text_append_u32(&text, length - limits.head - limits.tail);
            ta_text_append(&text, TA_PHRASE(" steps]"));
        }
        if (!shown) {
            continue;
        }
        if (index != 0) {
            ta_text_append(&text, TA_PHRASE("."));
        }
        ta_text_append(&text, ta_spec_member_name(step.member));
        if (step.index != TA_SPEC_INDEX_NONE) {
            ta_text_append(&text, TA_PHRASE("["));
            ta_text_append_u32(&text, step.index);
            ta_text_append(&text, TA_PHRASE("]"));
        }
        if (state != NULL) {
            ta_text_append(&text, TA_PHRASE("("));
            ta_text_append_field(&text, state->id, limits.id, false);
            ta_text_append(&text, TA_PHRASE(")"));
        }
    }
    if (length != 0) {
        ta_text_append(&text, TA_PHRASE(": "));
    }
    ta_text_append_field(&text, error->message.len != 0 ? error->message : TA_PHRASE("the spec was refused"),
                         limits.text, false);
    if (error->subject.len != 0) {
        ta_text_append(&text, TA_PHRASE(" \""));
        ta_text_append_field(&text, error->subject, limits.id, true);
        ta_text_append(&text, TA_PHRASE("\""));
    }
    if (error->related.len != 0) {
        switch (error->code) {
            case TA_COMPILE_ERROR_UNKNOWN_STATE:
                ta_text_append(&text, TA_PHRASE("; nearest is \""));
                break;
            case TA_COMPILE_ERROR_TARGET_OUTSIDE:
                ta_text_append(&text, TA_PHRASE("; it must be inside \""));
                break;
            default:
                ta_text_append(&text, TA_PHRASE("; see also \""));
                break;
        }
        ta_text_append_field(&text, error->related, limits.id, true);
        ta_text_append(&text, TA_PHRASE("\""));
    }
    *params->out = (ta_str){ .ptr = text.bytes, .len = text.length };
    return TA_OK;
}

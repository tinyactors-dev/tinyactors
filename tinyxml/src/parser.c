/*
 * The parser's core: construction, feeding, the step loop, and the helpers
 * every state uses. See parser.h for the shape of the whole, and
 * docs/internal/parser.md.
 */
#include "parser.h"

#define STAGING_DEFAULT ((uint32_t)65536u)
#define STAGING_MINIMUM ((uint32_t)64u)
#define STAGING_PADDING ((uint32_t)64u)

/* ------------------------------------------------------------------------
 * Byte classes.
 */

bool txp_is_space(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

bool txp_name_start_at(const uint8_t *bytes, uint32_t *width) {
    return txml_text_is_name_start(txml_text_decode(bytes, width));
}

bool txp_name_char_at(const uint8_t *bytes, uint32_t *width) {
    return txml_text_is_name_char(txml_text_decode(bytes, width));
}

scan txp_fail_name_char(txml_parser *p, uint32_t index) {
    return txp_fail(p, PROBLEM(TXML_DOC_NAME, "not a legal name character"), txp_position_at(p, index));
}

uint32_t txp_skip_space(const uint8_t *bytes, uint32_t from, uint32_t limit) {
    while (from < limit && txp_is_space(bytes[from])) {
        from += 1u;
    }
    return from;
}

uint32_t txp_scan_name(const uint8_t *bytes, uint32_t from, uint32_t limit) {
    uint32_t width = 0u;
    if (from >= limit || !txp_name_start_at(bytes + from, &width)) {
        return from;
    }
    uint32_t i = from + width;
    while (i < limit && txp_name_char_at(bytes + i, &width)) {
        i += width;
    }
    return i;
}

scan txp_eof_in_markup(txml_parser *p) {
    return txp_fail(p, PROBLEM(TXML_DOC_UNEXPECTED_EOF, "input ended inside markup"), txp_position_now(p));
}

scan txp_intern(txml_parser *p, const intern_request *request, uint32_t *id) {
    const txml_result result = txml_doc_intern_name(p->doc, &request->params, id);
    if (result == TXML_ERR_LIMIT && p->doc->names.count >= p->limits.max_names) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_NAMES, "too many distinct names"), request->at);
    }
    return result == TXML_OK ? SCAN_OK : txp_fail_result(p, result, request->at);
}

uint32_t txp_literal_length(const char *literal) {
    uint32_t length = 0u;
    while (literal[length] != '\0') {
        length += 1u;
    }
    return length;
}

bool txp_bytes_are(const uint8_t *bytes, uint32_t length, const char *literal) {
    if (length != txp_literal_length(literal)) {
        return false;
    }
    for (uint32_t i = 0u; i < length; i++) {
        if (bytes[i] != (uint8_t)literal[i]) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Positions.
 */

uint32_t txp_limit(const txml_parser *p) {
    return p->bad != IDX_NONE ? p->bad : p->valid;
}

txml_position txp_position_now(const txml_parser *p) {
    return (txml_position){ p->consumed, p->line, p->column };
}

txml_position txp_position_at(const txml_parser *p, uint32_t index) {
    txml_position position = txp_position_now(p);
    for (uint32_t i = p->start; i < index && i < p->end; i++) {
        position.offset += 1u;
        if (p->window[i] == '\n') {
            position.line += 1u;
            position.column = 1u;
        } else {
            position.column += 1u;
        }
    }
    return position;
}

txml_position txp_position_after(const txml_parser *p, uint32_t index) {
    txml_position position = txp_position_at(p, index);
    position.offset += 1u;
    position.column += 1u;
    return position;
}

txml_position txp_cursor_position(const txml_parser *p, position_cursor *cursor, uint32_t index) {
    if (!cursor->valid || index < cursor->index) {
        cursor->at = txp_position_now(p);
        cursor->index = p->start;
        cursor->valid = true;
    }
    for (; cursor->index < index && cursor->index < p->end; cursor->index++) {
        cursor->at.offset += 1u;
        if (p->window[cursor->index] == '\n') {
            cursor->at.line += 1u;
            cursor->at.column = 1u;
        } else {
            cursor->at.column += 1u;
        }
    }
    return cursor->at;
}

void txp_commit(txml_parser *p, uint32_t index) {
    for (uint32_t i = p->start; i < index; i++) {
        if (p->window[i] == '\n') {
            p->line += 1u;
            p->column = 1u;
        } else {
            p->column += 1u;
        }
    }
    p->consumed += index - p->start;
    p->start = index;
}

/* ------------------------------------------------------------------------
 * Errors, the budget, appending.
 */

scan txp_fail(txml_parser *p, const problem *what, txml_position at) {
    p->failed = true;
    p->error.code = what->code;
    p->error.limit = what->limit;
    p->error.at = at;
    p->error.message = (txml_str){ what->message, txp_literal_length(what->message) };
    return SCAN_FAIL;
}

scan txp_fail_result(txml_parser *p, txml_result result, txml_position at) {
    if (result == TXML_ERR_NOMEM) {
        return SCAN_NOMEM;
    }
    if (result == TXML_ERR_LIMIT) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_DOCUMENT_BYTES, "the document exceeds a storage limit"), at);
    }
    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "internal inconsistency"), at);
}

scan txp_check_budget(txml_parser *p, txml_position at) {
    const txml_doc *doc = p->doc;
    if ((uint64_t)doc->values.used + doc->tree.used > p->limits.max_document_bytes) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_DOCUMENT_BYTES, "the document exceeds max_document_bytes"), at);
    }
    return SCAN_OK;
}

bool txp_budget_spent(const txml_parser *p) {
    return p->step_budget != 0u && p->appended >= p->step_budget;
}

uint32_t txp_parent_node(const txml_parser *p) {
    return p->depth == 0u ? 0u : p->stack[p->depth - 1u].node;
}

scan txp_append_leaf(txml_parser *p, const leaf *what) {
    uint32_t index = 0u;

    txml_doc_run_finish(p->doc, &p->run);
    const uint32_t next = p->doc->nodes.count;
    const txml_node_append append = {
        .node = {
            .kind = (uint8_t)what->kind, .flags = (uint8_t)what->flags, .depth = (uint16_t)(p->depth + 1u),
            .parent = txp_parent_node(p), .end = next + 1u, .name = what->name,
            .as.text = { p->run.offset, p->run.length },
        },
        .position = what->position,
        .end = what->end,
    };
    const txml_result result = txml_doc_append_node(p->doc, &append, &index);
    if (result != TXML_OK) {
        return txp_fail_result(p, result, what->position);
    }
    p->appended += 1u;
    return txp_check_budget(p, what->position);
}

snapshot txp_snapshot(txml_parser *p) {
    snapshot s = { p->doc->nodes.count, p->doc->attributes.count, p->doc->node_prefixes.count,
                   p->doc->attribute_prefixes.count, p->bindings, txml_arena_position(&p->stack_arena) };
    return s;
}

void txp_rollback_nodes(txml_parser *p, const snapshot *s) {
    p->doc->nodes.count = s->nodes;
    p->doc->positions.count = s->nodes;
    p->doc->attributes.count = s->attributes;
    p->doc->attribute_sources.count = s->attributes;
    p->doc->node_prefixes.count = s->node_prefixes;
    p->doc->attribute_prefixes.count = s->attribute_prefixes;
}

/* ------------------------------------------------------------------------
 * Construction.
 */

txml_result txml_parser_init(txml_parser **out, const txml_parser_config *config) {
    if (out == NULL || config == NULL || config->allocator.v == NULL || config->allocator.v->alloc == NULL ||
        config->allocator.v->free == NULL) {
        return TXML_ERR_INVALID;
    }
    const uint32_t staging = config->staging_length == 0u ? STAGING_DEFAULT : config->staging_length;
    if (staging < STAGING_MINIMUM || config->limits.max_attributes == 0u || config->limits.max_depth == 0u ||
        config->limits.max_name_length == 0u) {
        return TXML_ERR_INVALID;
    }

    txml_parser *p = (txml_parser *)config->allocator.v->alloc(config->allocator.self, sizeof(txml_parser),
                                                               _Alignof(txml_parser));
    if (p == NULL) {
        return TXML_ERR_NOMEM;
    }
    txml_text_zero(p, sizeof(*p));
    p->allocator = config->allocator;
    p->limits = config->limits;
    p->step_budget = config->step_budget;
    p->staging_length = staging;
    p->bad = IDX_NONE;
    p->line = p->column = 1u;
    p->state = &txp_state_start;
    p->stage = STAGE_PROLOG;
    p->error = (txml_document_error){ TXML_DOC_OK, TXML_LIMIT_NONE, { 0u, 1u, 1u }, { "", 0u } };
    p->reference_capacity = config->limits.max_name_length + 2u;

    txml_result result = txml_arena_init(&p->parser_arena, config->allocator, config->chunk_size);
    if (result == TXML_OK) {
        result = txml_arena_init(&p->stack_arena, config->allocator, config->chunk_size);
    }
    if (result == TXML_OK) {
        result = txml_arena_init(&p->tag_arena, config->allocator, config->chunk_size);
    }
    if (result == TXML_OK) {
        p->window = (uint8_t *)txml_arena_alloc(&p->parser_arena, (size_t)staging + STAGING_PADDING, 16u);
        p->name = (uint8_t *)txml_arena_alloc(&p->parser_arena, config->limits.max_name_length, 1u);
        p->reference = (uint8_t *)txml_arena_alloc(&p->parser_arena, p->reference_capacity, 1u);
        p->records = (attribute_record *)txml_arena_alloc(
            &p->parser_arena, (size_t)config->limits.max_attributes * sizeof(attribute_record), _Alignof(attribute_record));
        p->stack = (open_element *)txml_arena_alloc(&p->parser_arena, (size_t)config->limits.max_depth * sizeof(open_element),
                                                    _Alignof(open_element));
        p->frames = (frame *)txml_arena_alloc(&p->parser_arena, ((size_t)config->limits.max_depth + 1u) * sizeof(frame),
                                              _Alignof(frame));
        if (p->window == NULL || p->name == NULL || p->reference == NULL || p->records == NULL || p->stack == NULL ||
            p->frames == NULL) {
            result = TXML_ERR_NOMEM;
        }
    }
    if (result == TXML_OK) {
        /* A mark taken on an empty arena would free the whole first chunk on
         * release, once per tag; one byte keeps that chunk alive. */
        if (txml_arena_alloc(&p->stack_arena, 1u, 1u) == NULL || txml_arena_alloc(&p->tag_arena, 1u, 1u) == NULL) {
            result = TXML_ERR_NOMEM;
        }
    }
    if (result == TXML_OK) {
        for (uint32_t i = 0u; i < STAGING_PADDING; i++) {
            p->window[staging + i] = 0u;
        }
        result = txml_doc_create(&p->doc, config);
    }
    if (result != TXML_OK) {
        txml_parser_destroy(p);
        return result;
    }
    *out = p;
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * Feeding. Bytes are validated as they arrive: a whole valid character moves
 * `valid` past it, a bad one sets `bad`, and a character cut off by the end of
 * the chunk stays unreadable until its remaining bytes come.
 */

uint32_t txml_parse_feed(txml_parser *p, txml_bytes chunk, bool final) {
    if (p == NULL || p->done || p->failed) {
        return 0u;
    }
    if (p->final_seen) {
        p->fed_after_final = chunk.length != 0u;
        return 0u;
    }
    if (p->start != 0u) {
        for (uint32_t i = p->start; i < p->end; i++) {
            p->window[i - p->start] = p->window[i];
        }
        p->end -= p->start;
        p->valid -= p->start;
        if (p->bad != IDX_NONE) {
            p->bad -= p->start;
        }
        p->start = 0u;
    }
    const uint32_t room = p->staging_length - p->end;
    const uint32_t take = chunk.length < room ? chunk.length : room;
    for (uint32_t i = 0u; i < take; i++) {
        p->window[p->end + i] = chunk.ptr[i];
    }
    p->end += take;
    p->fed += take;

    if (p->bad == IDX_NONE) {
        const txml_text_scan checked = txml_text_scan_utf8(p->window + p->valid, p->end - p->valid);
        if (checked.invalid != UINT32_MAX) {
            p->bad = p->valid + checked.invalid;
        }
        p->valid += checked.complete;
    }
    if (final && take == chunk.length) {
        p->final_seen = true;
        if (p->bad == IDX_NONE && p->valid < p->end) {
            p->bad = p->valid; /* input ended inside a character */
        }
    }
    return take;
}

/* ------------------------------------------------------------------------
 * The step loop.
 */

static txml_step step_result(scan outcome) {
    switch (outcome) {
    case SCAN_YIELD:
        return TXML_STEP_PROGRESS;
    case SCAN_NOMEM:
        return TXML_STEP_NEED_MEMORY;
    case SCAN_FAIL:
        return TXML_STEP_FAILED;
    default:
        return TXML_STEP_DONE;
    }
}

txml_step txml_parse_step(txml_parser *p) {
    if (p->failed) {
        return TXML_STEP_FAILED;
    }
    if (p->done) {
        return TXML_STEP_DONE;
    }
    if (p->fed_after_final) {
        txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "input fed after the final chunk"), txp_position_now(p));
        return TXML_STEP_FAILED;
    }

    p->appended = 0u;
    for (;;) {
        scan outcome = SCAN_OK;

        /* An entity expansion that ran out of memory finishes first. */
        if (p->dec.expanding) {
            outcome = txp_dec_resume(p);
            if (outcome != SCAN_OK) {
                return step_result(outcome);
            }
        }

        if (p->start == txp_limit(p)) {
            if (p->bad != IDX_NONE) {
                txp_fail(p, PROBLEM(TXML_DOC_ENCODING, "not well-formed UTF-8 or a forbidden character"),
                         txp_position_at(p, p->bad));
                return TXML_STEP_FAILED;
            }
            if (!p->final_seen) {
                return TXML_STEP_NEED_INPUT;
            }
            outcome = p->state->at_eof(p);
        } else {
            outcome = p->state->run(p);
        }

        if (outcome == SCAN_DONE) {
            p->done = true;
        }
        if (outcome != SCAN_OK) {
            return step_result(outcome);
        }
    }
}

const txml_doc *txml_parser_doc(const txml_parser *p) {
    return p == NULL ? NULL : p->doc;
}

txml_result txml_parser_error(const txml_parser *p, txml_document_error *out) {
    if (p == NULL || !p->failed) {
        return TXML_ERR_NOT_FOUND;
    }
    *out = p->error;
    return TXML_OK;
}

uint64_t txml_parser_offset(const txml_parser *p) {
    return p == NULL ? 0u : p->fed;
}

void txml_parser_destroy(txml_parser *p) {
    if (p == NULL) {
        return;
    }
    const txml_allocator allocator = p->allocator;
    if (!p->detached && p->doc != NULL) {
        txml_doc_destroy(p->doc);
    }
    txml_arena_destroy(&p->tag_arena);
    txml_arena_destroy(&p->stack_arena);
    txml_arena_destroy(&p->parser_arena);
    txml_allocator_free_params params = { .ptr = p, .size = sizeof(txml_parser), .alignment = _Alignof(txml_parser) };
    allocator.v->free(allocator.self, &params);
}

txml_result txml_parser_finish(txml_parser *p, txml_doc **out) {
    if (p == NULL || out == NULL || !p->done) {
        return TXML_ERR_INVALID;
    }
    *out = p->doc;
    p->detached = true;
    txml_parser_destroy(p);
    return TXML_OK;
}

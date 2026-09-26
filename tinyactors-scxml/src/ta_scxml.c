/*
 * The SCXML 1.0 XML codec (core ADR 0053) over txml.
 *
 * Decode parses the input with txml into the operation's arena and walks the
 * concrete syntax tree into a ta_scxml_spec, with the span of every element
 * it read (core ADR 0086). Encode writes a spec with txml's event writer, one
 * element per line, and reports where it wrote each element. Nothing here
 * uses the C library or performs I/O; the arena is the only memory.
 *
 * Text in the spec points into the input wherever the decoded text is
 * byte-for-byte what the input holds. Where XML changed it (a character or
 * entity reference, attribute-value normalization of a tab or line end, a
 * CRLF, text split across CDATA sections), it points into the parsed
 * document's value pool, which txml filled and which also lives in the arena.
 * The codec itself copies only what it assembles: a joined script body, and
 * the re-serialization of a <content> that holds markup (SCXML G.6).
 *
 * Recursion follows the document's nesting, which max_depth bounds. Every
 * other bound is a field of ta_scxml_codec_params; reaching one is
 * TA_CHART_ERROR_LIMIT naming the field.
 */
#include "ta_scxml.h"
#include "txml.h"

#define SCXML_NS TA_SCXML_NAMESPACE

/* ------------------------------------------------------------------------
 * Bytes and text, without a C library.
 */

static uint32_t cstr_length(const char *text) {
    uint32_t length = 0;
    while (text[length] != '\0') {
        length += 1u;
    }
    return length;
}

static bool same_bytes(const void *left, const void *right, uint32_t length) {
    const uint8_t *a = left;
    const uint8_t *b = right;
    for (uint32_t index = 0; index < length; index += 1u) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

static void copy_bytes(void *to, const void *from, uint32_t length) {
    uint8_t *out = to;
    const uint8_t *in = from;
    for (uint32_t index = 0; index < length; index += 1u) {
        out[index] = in[index];
    }
}

static ta_str cstr(const char *text) {
    return (ta_str){ .ptr = text, .len = cstr_length(text) };
}

static bool str_is(ta_str text, const char *literal) {
    uint32_t length = cstr_length(literal);
    return text.len == length && (length == 0 || same_bytes(text.ptr, literal, length));
}

static bool str_equal(ta_str left, ta_str right) {
    return left.len == right.len && (left.len == 0 || same_bytes(left.ptr, right.ptr, left.len));
}

static bool txml_is(txml_str text, const char *literal) {
    return str_is((ta_str){ .ptr = text.ptr, .len = text.length }, literal);
}

/* A present value is never a null pointer, so it differs from an absent one. */
static ta_str from_txml(txml_str text) {
    return (ta_str){ .ptr = text.ptr != NULL ? text.ptr : "", .len = text.length };
}

static txml_str to_txml(ta_str text) {
    return (txml_str){ .ptr = text.ptr != NULL ? text.ptr : "", .length = text.len };
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool blank(txml_str text) {
    for (uint32_t index = 0; index < text.length; index += 1u) {
        if (!is_space(text.ptr[index])) {
            return false;
        }
    }
    return true;
}

static ta_chart_position position_from(txml_position at) {
    return (ta_chart_position){ .offset = at.offset, .line = at.line, .column = at.column };
}

/* ------------------------------------------------------------------------
 * txml over the operation's arena: allocations live until the arena is
 * reset, so nothing is ever freed here and no txml object is destroyed.
 */

static void *arena_txml_alloc(void *self, size_t size, size_t alignment) {
    return ta_arena_alloc(self, size == 0 ? 1 : size, alignment);
}

static void arena_txml_free(void *self, const txml_allocator_free_params *params) {
    (void)self;
    (void)params;
}

static const txml_allocator_vtable ARENA_TXML = { .alloc = arena_txml_alloc, .free = arena_txml_free };

static txml_allocator txml_over(ta_arena *arena) {
    return (txml_allocator){ .self = arena, .v = &ARENA_TXML };
}

#define CHUNK_SIZE 16384u

/* The txml limits a codec's bounds pass through to. */
static txml_limits txml_limits_of(const ta_scxml_codec_params *limits) {
    return (txml_limits){
        .max_depth = limits->max_depth,
        .max_attributes = limits->max_attributes,
        .max_name_length = limits->max_name_length,
        .max_names = limits->max_names,
        .max_document_bytes = limits->max_parse_bytes,
        .entity_expansion_budget = limits->max_entity_expansion,
    };
}

/* Parses the concatenation of `parts`. TXML_STEP_DONE, TXML_STEP_FAILED
 * (read txml_parser_error), or TXML_STEP_NEED_MEMORY. */
static txml_step parse(ta_arena *arena, const ta_scxml_codec_params *limits, const ta_bytes *parts, uint32_t count,
                       txml_parser **out) {
    txml_parser_config config = {
        .allocator = txml_over(arena),
        .limits = txml_limits_of(limits),
        .staging_length = CHUNK_SIZE,
        .chunk_size = CHUNK_SIZE,
    };
    if (txml_parser_init(out, &config) != TXML_OK) {
        return TXML_STEP_NEED_MEMORY;
    }
    uint32_t part = 0;
    uint32_t offset = 0;
    for (;;) {
        txml_step step = txml_parse_step(*out);
        if (step == TXML_STEP_PROGRESS) {
            continue;
        }
        if (step != TXML_STEP_NEED_INPUT) {
            return step;
        }
        if (part == count) {
            return TXML_STEP_FAILED; /* the final chunk was fed and more is wanted */
        }
        const ta_bytes *chunk = &parts[part];
        uint32_t rest = chunk->len - offset;
        bool last = part + 1u == count;
        uint32_t taken = txml_parse_feed(*out, (txml_bytes){ .ptr = rest == 0 ? (const uint8_t *)"" : chunk->ptr + offset,
                                                             .length = rest },
                                         last);
        offset += taken;
        if (taken == rest) {
            part += 1u;
            offset = 0;
        }
    }
}

/* ------------------------------------------------------------------------
 * Output: txml's writer fills a small buffer; its drains collect here, and
 * the result is one contiguous arena block.
 */

typedef struct sink_chunk {
    struct sink_chunk *next;
    uint32_t length;
    const uint8_t *bytes;
} sink_chunk;

typedef struct {
    ta_arena *arena;
    txml_writer *writer;
    sink_chunk *head;
    sink_chunk *tail;
    uint64_t total;
} output;

static bool output_take(output *out) {
    txml_bytes bytes = txml_writer_output(out->writer);
    if (bytes.length == 0) {
        return true;
    }
    sink_chunk *chunk = ta_arena_alloc(out->arena, sizeof(sink_chunk), _Alignof(sink_chunk));
    uint8_t *copy = ta_arena_alloc(out->arena, bytes.length, 1);
    if (chunk == NULL || copy == NULL || out->total + bytes.length > UINT32_MAX) {
        return false;
    }
    copy_bytes(copy, bytes.ptr, bytes.length);
    *chunk = (sink_chunk){ .next = NULL, .length = bytes.length, .bytes = copy };
    if (out->tail == NULL) {
        out->head = chunk;
    } else {
        out->tail->next = chunk;
    }
    out->tail = chunk;
    out->total += bytes.length;
    return true;
}

/* Drives one writer call to completion. */
static txml_result output_pump(output *out, txml_result result) {
    while (result == TXML_NEED_SPACE) {
        if (!output_take(out)) {
            return TXML_ERR_NOMEM;
        }
        result = txml_writer_drain(out->writer);
    }
    return result;
}

static txml_result output_init(output *out, ta_arena *arena, const ta_scxml_codec_params *limits, uint32_t flags) {
    *out = (output){ .arena = arena };
    uint8_t *buffer = ta_arena_alloc(arena, 4096, 1);
    if (buffer == NULL) {
        return TXML_ERR_NOMEM;
    }
    txml_writer_config config = {
        .allocator = txml_over(arena),
        .output = { .ptr = buffer, .capacity = 4096 },
        .limits = txml_limits_of(limits),
        .flags = flags,
    };
    return txml_writer_init(&out->writer, &config);
}

static txml_result output_finish(output *out, ta_bytes *bytes) {
    txml_result result = output_pump(out, txml_writer_finish(out->writer));
    if (result != TXML_OK) {
        return result;
    }
    if (!output_take(out)) {
        return TXML_ERR_NOMEM;
    }
    (void)txml_writer_drain(out->writer);
    uint8_t *joined = ta_arena_alloc(out->arena, out->total == 0 ? 1 : (size_t)out->total, 1);
    if (joined == NULL) {
        return TXML_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (sink_chunk *chunk = out->head; chunk != NULL; chunk = chunk->next) {
        copy_bytes(joined + at, chunk->bytes, chunk->length);
        at += chunk->length;
    }
    *bytes = (ta_bytes){ .ptr = joined, .len = at };
    return TXML_OK;
}

/* Every child node of `parent`, each with its subtree, as a content fragment
 * (txml ADR 0024): the namespace declarations a node needs from outside it
 * are written on it, which is what SCXML G.6 requires of <content>. */
static txml_result serialize_children(ta_arena *arena, const ta_scxml_codec_params *limits, txml_cursor parent,
                                      ta_bytes *bytes) {
    output out;
    txml_result result = output_init(&out, arena, limits, TXML_WRITE_FRAGMENT);
    txml_cursor child = parent;
    for (txml_move move = txml_first_child(&child); result == TXML_OK && move == TXML_MOVED;
         move = txml_next_sibling(&child)) {
        result = output_pump(&out, txml_write_subtree(out.writer, child));
    }
    return result == TXML_OK ? output_finish(&out, bytes) : result;
}

/* ========================================================================
 * Decode
 */

typedef struct {
    const ta_chart_decode_params *params;
    const ta_scxml_codec_params *limits;
    ta_arena *arena;
    const txml_doc *doc;
    ta_bytes input;
    ta_chart_error error;
    uint32_t anonymous;
    /* Where each element was read (ADR 0086): the spec's spans table as it
     * grows, and its sources, the input first and then every file the
     * resolver fetched. Growing either can run out of memory anywhere in the
     * walk, so that is remembered and reported once, at the end. */
    ta_source_span *spans;
    uint32_t span_count;
    uint32_t span_capacity;
    ta_scxml_source *sources;
    uint32_t source_count;
    uint32_t source_capacity;
    bool out_of_memory;
} decoder;

/* An attribute the element may carry, and what the document gave it. */
typedef struct {
    const char *name;
    bool present;
    ta_str value;
    ta_chart_position at;
    /* Where it was written, when it has source text. */
    bool spanned;
    txml_attribute_span where;
} attr;

#define ATTR(name_) { .name = (name_) }

static ta_chart_position node_position(txml_cursor node) {
    txml_position at = { 0 };
    (void)txml_node_position(node, &at);
    return position_from(at);
}

static ta_str local_name(txml_cursor node) {
    txml_qname name;
    if (txml_doc_name_at(node.doc, txml_node_name_id(node), &name) != TXML_OK) {
        return cstr("");
    }
    return from_txml(name.local);
}

static bool in_scxml_namespace(txml_cursor node) {
    txml_qname name;
    return txml_kind(node) == TXML_NODE_ELEMENT && txml_doc_name_at(node.doc, txml_node_name_id(node), &name) == TXML_OK &&
           txml_is(name.ns, SCXML_NS);
}

static bool is_element(txml_cursor node, const char *local) {
    return in_scxml_namespace(node) && str_is(local_name(node), local);
}

static ta_result fail_at(decoder *d, ta_chart_error_code code, ta_chart_position at, ta_str construct, ta_str attribute,
                         const char *message) {
    d->error = (ta_chart_error){ .code = code, .at = at, .construct = construct, .attribute = attribute,
                                 .message = cstr(message) };
    return TA_ERR_INVALID;
}

static ta_result fail(decoder *d, ta_chart_error_code code, txml_cursor node, const char *message) {
    return fail_at(d, code, node_position(node), local_name(node), (ta_str){ 0 }, message);
}

static ta_result fail_attr(decoder *d, ta_chart_error_code code, txml_cursor node, const attr *attribute,
                           const char *message) {
    return fail_at(d, code, attribute->present ? attribute->at : node_position(node), local_name(node),
                   cstr(attribute->name), message);
}

/* One text value the codec produces is at most max_text_bytes. */
static ta_result bound_text(decoder *d, uint64_t length, ta_chart_position at, ta_str construct) {
    if (length > d->limits->max_text_bytes) {
        return fail_at(d, TA_CHART_ERROR_LIMIT, at, cstr("max_text_bytes"), construct,
                       "a text value is longer than max_text_bytes");
    }
    return TA_OK;
}

/* The input bytes a value came from, when they are exactly the value. */
static ta_str slice_or(decoder *d, uint64_t start, uint64_t end, txml_str value) {
    if (end >= start && end - start == value.length && end <= d->input.len &&
        (value.length == 0 || same_bytes(d->input.ptr + start, value.ptr, value.length))) {
        return (ta_str){ .ptr = (const char *)d->input.ptr + start, .len = value.length };
    }
    return from_txml(value);
}

/*
 * Reads an element's attributes into `attrs`. A namespace declaration is not
 * an attribute, and an attribute in another namespace belongs to another
 * vocabulary, so both are skipped. Any other attribute the table does not
 * name is TA_CHART_ERROR_UNKNOWN_ATTRIBUTE.
 */
static ta_result read_attrs(decoder *d, txml_cursor node, attr *attrs, uint32_t count) {
    txml_attr_cursor cursor;
    for (txml_move move = txml_first_attribute(node, &cursor); move == TXML_MOVED; move = txml_next_attribute(&cursor)) {
        txml_attribute_view view;
        txml_qname name;
        if (txml_attribute_read(cursor, &view) != TXML_OK || txml_doc_name_at(d->doc, view.name, &name) != TXML_OK) {
            return TA_ERR_INVALID;
        }
        if ((view.flags & TXML_ATTR_NS_DECL) != 0u || name.ns.length != 0) {
            continue;
        }
        txml_attribute_span span;
        bool spanned = txml_attribute_span_read(cursor, &span) == TXML_OK;
        ta_chart_position at = spanned ? position_from(span.start) : node_position(node);
        attr *found = NULL;
        for (uint32_t index = 0; index < count && found == NULL; index += 1u) {
            if (txml_is(name.local, attrs[index].name)) {
                found = &attrs[index];
            }
        }
        if (found == NULL) {
            return fail_at(d, TA_CHART_ERROR_UNKNOWN_ATTRIBUTE, at, local_name(node), from_txml(name.local),
                           "the element has no such attribute");
        }
        ta_result bounded = bound_text(d, view.value.length, at, from_txml(name.local));
        if (bounded != TA_OK) {
            return bounded;
        }
        found->present = true;
        found->at = at;
        found->spanned = spanned;
        found->where = spanned ? span : (txml_attribute_span){ 0 };
        found->value = spanned ? slice_or(d, span.value_start.offset, span.value_end.offset, view.value)
                               : from_txml(view.value);
    }
    return TA_OK;
}

/* `a` and `b` are an SCXML pair such as `event` / `eventexpr`. */
static ta_result exclusive(decoder *d, txml_cursor node, const attr *a, const attr *b) {
    if (a->present && b->present) {
        return fail_attr(d, TA_CHART_ERROR_CONFLICTING_ATTRIBUTE, node, b, "an attribute and its expression form");
    }
    return TA_OK;
}

static ta_expression source(ta_str text) {
    return (ta_expression){ .kind = TA_EXPRESSION_SOURCE, .as.source = text };
}

/* ------------------------------------------------------------------------
 * Spans (ADR 0086): where each element and expression was read.
 */

/* Doubles an arena array of `size`-byte items; the old one stays in the
 * arena, which the operation resets anyway. */
static void *grow(decoder *d, void *items, uint32_t *capacity, size_t size) {
    uint32_t wanted = *capacity == 0 ? 16u : *capacity * 2u;
    uint8_t *grown = ta_arena_alloc(d->arena, (size_t)wanted * size, 8);
    if (grown == NULL || wanted < *capacity) {
        d->out_of_memory = true;
        return NULL;
    }
    if (items != NULL) {
        copy_bytes(grown, items, (uint32_t)(*capacity * size));
    }
    *capacity = wanted;
    return grown;
}

/* Adds `span` to the table; its index from 1, or 0 when out of memory. */
static uint32_t add_span(decoder *d, ta_source_span span) {
    if (d->span_count == d->span_capacity) {
        ta_source_span *grown = grow(d, d->spans, &d->span_capacity, sizeof(ta_source_span));
        if (grown == NULL) {
            return 0;
        }
        d->spans = grown;
    }
    d->spans[d->span_count++] = span;
    return d->span_count;
}

/* Adds a source; its index, or 0 when out of memory. */
static uint32_t add_source(decoder *d, ta_str name) {
    if (d->source_count == d->source_capacity) {
        ta_scxml_source *grown = grow(d, d->sources, &d->source_capacity, sizeof(ta_scxml_source));
        if (grown == NULL) {
            return 0;
        }
        d->sources = grown;
    }
    d->sources[d->source_count] = (ta_scxml_source){ .name = name.ptr != NULL ? name : cstr("") };
    return d->source_count++;
}

static ta_source_span span_of(txml_position start, txml_position end) {
    return (ta_source_span){ .offset = start.offset, .length = (uint32_t)(end.offset - start.offset),
                             .line = start.line, .column = start.column,
                             .end_line = end.line, .end_column = end.column };
}

/* An element, from its start tag through its end tag. */
static uint32_t element_span(decoder *d, txml_cursor node) {
    txml_span span;
    return txml_node_span(node, &span) == TXML_OK ? add_span(d, span_of(span.start, span.end)) : 0u;
}

/* A whole attribute, name and value: the `initial` attribute. */
static uint32_t attribute_span(decoder *d, const attr *attribute) {
    return attribute->spanned ? add_span(d, span_of(attribute->where.start, attribute->where.end)) : 0u;
}

/* An expression attribute: its value inside the quotes, as written. */
static ta_expression expression(decoder *d, const attr *attribute) {
    ta_expression out = source(attribute->value);
    if (attribute->spanned) {
        out.span = add_span(d, span_of(attribute->where.value_start, attribute->where.value_end));
    }
    return out;
}

/* The character data of `parent` as written: from its first text node to
 * the end of its last, inside the CDATA markers when there are some. */
static uint32_t text_span(decoder *d, txml_cursor parent) {
    txml_span first = { 0 };
    txml_span last = { 0 };
    bool found = false;
    bool first_cdata = false;
    bool last_cdata = false;
    txml_cursor child = parent;
    for (txml_move move = txml_first_child(&child); move == TXML_MOVED; move = txml_next_sibling(&child)) {
        txml_span span;
        if (txml_kind(child) != TXML_NODE_TEXT || txml_node_span(child, &span) != TXML_OK) {
            continue;
        }
        bool cdata = (txml_flags(child) & TXML_NODE_CDATA) != 0u;
        if (!found) {
            first = span;
            first_cdata = cdata;
            found = true;
        }
        last = span;
        last_cdata = cdata;
    }
    if (!found) {
        return 0;
    }
    if (first_cdata) {
        first.start.offset += 9u; /* <![CDATA[ */
        first.start.column += 9u;
    }
    if (last_cdata) {
        last.end.offset -= 3u; /* ]]> */
        last.end.column -= 3u;
    }
    return add_span(d, span_of(first.start, last.end));
}

/* All of a fetched file, which is a source of its own. */
static uint32_t file_span(decoder *d, ta_str src, ta_bytes text) {
    uint32_t index = add_source(d, src);
    if (index == 0) {
        return 0;
    }
    ta_source_span span = { .source = index, .length = text.len, .line = 1, .column = 1, .end_line = 1, .end_column = 1 };
    for (uint32_t at = 0; at < text.len; at += 1u) {
        if (text.ptr[at] == '\n') {
            span.end_line += 1u;
            span.end_column = 1;
        } else {
            span.end_column += 1u;
        }
    }
    return add_span(d, span);
}

static ta_scxml_string_spec string_pair(decoder *d, const attr *literal, const attr *expression_attr) {
    if (literal->present) {
        return (ta_scxml_string_spec){ .kind = TA_SCXML_STRING_LITERAL, .as.literal = literal->value };
    }
    if (expression_attr->present) {
        return (ta_scxml_string_spec){ .kind = TA_SCXML_STRING_EXPR, .as.expr = expression(d, expression_attr) };
    }
    return (ta_scxml_string_spec){ 0 };
}

static ta_scxml_id_spec id_pair(decoder *d, const attr *literal, const attr *location) {
    if (literal->present) {
        return (ta_scxml_id_spec){ .kind = TA_SCXML_ID_LITERAL, .as.literal = literal->value };
    }
    if (location->present) {
        return (ta_scxml_id_spec){ .kind = TA_SCXML_ID_LOCATION, .as.location = expression(d, location) };
    }
    return (ta_scxml_id_spec){ 0 };
}

static void *alloc_array(decoder *d, uint32_t count, size_t size, size_t alignment) {
    uint8_t *block = ta_arena_alloc(d->arena, (size_t)(count + 1u) * size, alignment);
    if (block != NULL) {
        for (size_t index = 0; index < (size_t)(count + 1u) * size; index += 1u) {
            block[index] = 0;
        }
    }
    return block;
}

#define ALLOC_LIST(d, count, type) ((type *)alloc_array((d), (count), sizeof(type), _Alignof(type)))

/*
 * An XML ID list (`target`, `initial`, `namelist`): the whitespace-separated
 * tokens of `value`, as slices of it, terminated by an empty ta_str. Null for
 * a list with no tokens.
 */
static ta_result token_list(decoder *d, ta_str value, const ta_str **out) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < value.len;) {
        while (index < value.len && is_space(value.ptr[index])) {
            index += 1u;
        }
        if (index < value.len) {
            count += 1u;
        }
        while (index < value.len && !is_space(value.ptr[index])) {
            index += 1u;
        }
    }
    *out = NULL;
    if (count == 0) {
        return TA_OK;
    }
    ta_str *tokens = ALLOC_LIST(d, count, ta_str);
    if (tokens == NULL) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (uint32_t index = 0; index < value.len;) {
        while (index < value.len && is_space(value.ptr[index])) {
            index += 1u;
        }
        uint32_t start = index;
        while (index < value.len && !is_space(value.ptr[index])) {
            index += 1u;
        }
        if (index > start) {
            tokens[at++] = (ta_str){ .ptr = value.ptr + start, .len = index - start };
        }
    }
    *out = tokens;
    return TA_OK;
}

/* The first element child of `parent`. */
static bool first_element(txml_cursor parent, txml_cursor *out) {
    txml_cursor child = parent;
    if (txml_first_child(&child) != TXML_MOVED) {
        return false;
    }
    if (txml_kind(child) != TXML_NODE_ELEMENT && txml_next_element(&child) != TXML_MOVED) {
        return false;
    }
    *out = child;
    return true;
}

/* Elements hold only elements here: text that is not whitespace is misplaced. */
static ta_result only_elements(decoder *d, txml_cursor parent) {
    txml_cursor child = parent;
    for (txml_move move = txml_first_child(&child); move == TXML_MOVED; move = txml_next_sibling(&child)) {
        if (txml_kind(child) == TXML_NODE_TEXT && ((txml_flags(child) & TXML_NODE_CDATA) != 0u || !blank(txml_node_text(child)))) {
            return fail_at(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, node_position(child), local_name(parent),
                           (ta_str){ 0 }, "text where only elements belong");
        }
    }
    return TA_OK;
}

/* Whether `parent` has element children, and whether it has text that is not
 * whitespace. */
static void content_shape(txml_cursor parent, bool *elements, bool *text) {
    *elements = false;
    *text = false;
    txml_cursor child = parent;
    for (txml_move move = txml_first_child(&child); move == TXML_MOVED; move = txml_next_sibling(&child)) {
        if (txml_kind(child) == TXML_NODE_ELEMENT) {
            *elements = true;
        } else if (txml_kind(child) == TXML_NODE_TEXT &&
                   ((txml_flags(child) & TXML_NODE_CDATA) != 0u || !blank(txml_node_text(child)))) {
            *text = true;
        }
    }
}

/*
 * The character data of an element without element children: its text and
 * CDATA nodes joined, comments and processing instructions dropped. A single
 * node whose text is what the input holds is a slice of the input.
 */
static ta_result character_data(decoder *d, txml_cursor parent, ta_str *out) {
    uint32_t nodes = 0;
    uint64_t total = 0;
    txml_cursor only = parent;
    txml_cursor child = parent;
    for (txml_move move = txml_first_child(&child); move == TXML_MOVED; move = txml_next_sibling(&child)) {
        if (txml_kind(child) == TXML_NODE_TEXT) {
            nodes += 1u;
            total += txml_node_text(child).length;
            only = child;
        }
    }
    if (nodes == 0) {
        *out = cstr("");
        return TA_OK;
    }
    ta_result bounded = bound_text(d, total, node_position(parent), local_name(parent));
    if (bounded != TA_OK) {
        return bounded;
    }
    if (nodes == 1) {
        txml_str text = txml_node_text(only);
        txml_span span;
        if (txml_node_span(only, &span) == TXML_OK) {
            bool cdata = (txml_flags(only) & TXML_NODE_CDATA) != 0u;
            uint64_t start = span.start.offset + (cdata ? 9u : 0u); /* <![CDATA[ */
            uint64_t end = span.end.offset - (cdata ? 3u : 0u);    /* ]]> */
            *out = slice_or(d, start, end, text);
        } else {
            *out = from_txml(text);
        }
        return TA_OK;
    }
    char *joined = ta_arena_alloc(d->arena, total == 0 ? 1 : (size_t)total, 1);
    if (joined == NULL) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    child = parent;
    for (txml_move move = txml_first_child(&child); move == TXML_MOVED; move = txml_next_sibling(&child)) {
        if (txml_kind(child) == TXML_NODE_TEXT) {
            txml_str text = txml_node_text(child);
            copy_bytes(joined + at, text.ptr, text.length);
            at += text.length;
        }
    }
    *out = (ta_str){ .ptr = joined, .len = at };
    return TA_OK;
}

/*
 * A literal content: the character data of an element with no element
 * children, otherwise every child re-serialized as a fragment with the
 * namespace declarations it needs (SCXML G.6). Its media type is the codec's.
 */
static ta_result literal_content(decoder *d, txml_cursor parent, ta_scxml_value_spec *out) {
    bool elements = false;
    bool text = false;
    content_shape(parent, &elements, &text);
    ta_bytes bytes = { 0 };
    if (elements) {
        txml_result written = serialize_children(d->arena, d->limits, parent, &bytes);
        if (written == TXML_ERR_NOMEM) {
            return TA_ERR_NOMEM;
        }
        if (written != TXML_OK) {
            return fail(d, TA_CHART_ERROR_LIMIT, parent, "the content cannot be serialized");
        }
        ta_result bounded = bound_text(d, bytes.len, node_position(parent), local_name(parent));
        if (bounded != TA_OK) {
            return bounded;
        }
    } else {
        ta_str data;
        ta_result result = character_data(d, parent, &data);
        if (result != TA_OK) {
            return result;
        }
        bytes = (ta_bytes){ .ptr = (const uint8_t *)data.ptr, .len = data.len };
    }
    *out = (ta_scxml_value_spec){ .kind = TA_SCXML_VALUE_CONTENT,
                                  .as.content = { .media_type = cstr(TA_SCXML_MEDIA_TYPE), .bytes = bytes } };
    return TA_OK;
}

/* A `src` the embedder's resolver fetches: <data src>, <script src>. */
static ta_result fetch(decoder *d, txml_cursor node, const attr *src, ta_bytes *out) {
    const ta_chart_src_resolver *resolver = &d->params->resolver;
    if (resolver->v == NULL || resolver->v->fetch == NULL) {
        return fail_attr(d, TA_CHART_ERROR_SRC_UNRESOLVED, node, src, "no resolver for src");
    }
    ta_result result = resolver->v->fetch(resolver->self, src->value, d->arena, out);
    if (result == TA_ERR_NOT_FOUND || result == TA_ERR_INVALID) {
        return fail_attr(d, TA_CHART_ERROR_SRC_UNRESOLVED, node, src, "the resolver does not know this src");
    }
    return result == TA_OK ? bound_text(d, out->len, src->at, cstr("src")) : result;
}

static ta_result decode_actions(decoder *d, txml_cursor first, bool has_first, bool branches,
                                const ta_scxml_action_spec **out, txml_cursor *marker, bool *has_marker);

/* <content>: `expr`, or a literal body. */
static ta_result decode_content(decoder *d, txml_cursor node, ta_scxml_value_spec *out) {
    attr attrs[] = { ATTR("expr") };
    ta_result result = read_attrs(d, node, attrs, 1);
    if (result != TA_OK) {
        return result;
    }
    if (attrs[0].present) {
        bool elements = false;
        bool text = false;
        content_shape(node, &elements, &text);
        if (elements || text) {
            return fail_attr(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, node, &attrs[0], "content has both expr and a body");
        }
        *out = (ta_scxml_value_spec){ .kind = TA_SCXML_VALUE_EXPR, .as.expr = expression(d, &attrs[0]) };
        return TA_OK;
    }
    return literal_content(d, node, out);
}

/*
 * The payload of <send>, <invoke> or <donedata>: `namelist` and <param>s as
 * params, and at most one <content>. <finalize> is left to <invoke>.
 */
typedef struct {
    const attr *namelist; /* null where the element has none */
    bool finalize_allowed;
    bool params_with_content;
    const ta_scxml_param_spec *params;
    ta_scxml_value_spec content;
} payload;

static ta_result decode_payload(decoder *d, txml_cursor node, payload *out) {
    const ta_str *names = NULL;
    uint32_t name_count = 0;
    if (out->namelist != NULL && out->namelist->present) {
        ta_result result = token_list(d, out->namelist->value, &names);
        if (result != TA_OK) {
            return result;
        }
        while (names != NULL && names[name_count].ptr != NULL) {
            name_count += 1u;
        }
    }
    uint32_t params = 0;
    uint32_t contents = 0;
    txml_cursor child;
    for (bool more = first_element(node, &child); more; more = txml_next_element(&child) == TXML_MOVED) {
        if (is_element(child, "param")) {
            params += 1u;
        } else if (is_element(child, "content")) {
            contents += 1u;
            if (contents > 1u) {
                return fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, child, "a second <content>");
            }
        } else if (!(out->finalize_allowed && is_element(child, "finalize"))) {
            return fail(d, in_scxml_namespace(child) ? TA_CHART_ERROR_MISPLACED_CONSTRUCT : TA_CHART_ERROR_UNKNOWN_CONSTRUCT,
                        child, "not allowed here");
        }
    }
    if (contents != 0 && params + name_count != 0 && !out->params_with_content) {
        return fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, node, "<param> or namelist with <content>");
    }
    out->params = NULL;
    if (params + name_count != 0) {
        ta_scxml_param_spec *list = ALLOC_LIST(d, params + name_count, ta_scxml_param_spec);
        if (list == NULL) {
            return TA_ERR_NOMEM;
        }
        /* SCXML: namelist entries are params whose value is the location of
         * the same name. */
        for (uint32_t index = 0; index < name_count; index += 1u) {
            list[index] = (ta_scxml_param_spec){
                .name = names[index],
                .value = { .kind = TA_SCXML_VALUE_LOCATION, .as.location = source(names[index]) },
            };
        }
        uint32_t at = name_count;
        for (bool more = first_element(node, &child); more; more = txml_next_element(&child) == TXML_MOVED) {
            if (!is_element(child, "param")) {
                continue;
            }
            attr attrs[] = { ATTR("name"), ATTR("expr"), ATTR("location") };
            ta_result result = read_attrs(d, child, attrs, 3);
            if (result == TA_OK) {
                result = only_elements(d, child);
            }
            if (result == TA_OK && first_element(child, &(txml_cursor){ 0 })) {
                result = fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, child, "<param> has no children");
            }
            if (result == TA_OK) {
                result = exclusive(d, child, &attrs[1], &attrs[2]);
            }
            if (result != TA_OK) {
                return result;
            }
            if (!attrs[0].present) {
                return fail_attr(d, TA_CHART_ERROR_MISSING_ATTRIBUTE, child, &attrs[0], "<param> needs a name");
            }
            if (!attrs[1].present && !attrs[2].present) {
                return fail_attr(d, TA_CHART_ERROR_MISSING_ATTRIBUTE, child, &attrs[1], "<param> needs expr or location");
            }
            list[at++] = (ta_scxml_param_spec){
                .name = attrs[0].value,
                .value = attrs[1].present
                             ? (ta_scxml_value_spec){ .kind = TA_SCXML_VALUE_EXPR, .as.expr = expression(d, &attrs[1]) }
                             : (ta_scxml_value_spec){ .kind = TA_SCXML_VALUE_LOCATION,
                                                      .as.location = expression(d, &attrs[2]) },
            };
        }
        out->params = list;
    }
    out->content = (ta_scxml_value_spec){ 0 };
    for (bool more = first_element(node, &child); more; more = txml_next_element(&child) == TXML_MOVED) {
        if (is_element(child, "content")) {
            return decode_content(d, child, &out->content);
        }
    }
    return TA_OK;
}

/* Platform-specific executable content (SCXML 4.10), lowered to a <send> by
 * the embedder (ADR 0046). */
static ta_result decode_foreign(decoder *d, txml_cursor node, ta_scxml_action_spec *out) {
    const ta_chart_lowering *lowering = &d->params->lowering;
    if (lowering->v == NULL || lowering->v->lower == NULL) {
        return fail(d, TA_CHART_ERROR_FOREIGN_CONTENT, node, "platform-specific content and no lowering");
    }
    output element;
    ta_bytes bytes = { 0 };
    txml_result written = output_init(&element, d->arena, d->limits, TXML_WRITE_FRAGMENT);
    if (written == TXML_OK) {
        written = output_pump(&element, txml_write_subtree(element.writer, node));
    }
    if (written == TXML_OK) {
        written = output_finish(&element, &bytes);
    }
    if (written != TXML_OK) {
        return written == TXML_ERR_NOMEM ? TA_ERR_NOMEM : fail(d, TA_CHART_ERROR_LIMIT, node, "cannot serialize");
    }
    ta_chart_position at = node_position(node);
    ta_scxml_send_spec send = { 0 };
    ta_result result = lowering->v->lower(lowering->self, &at, bytes, d->arena, &send);
    if (result == TA_ERR_NOT_FOUND || result == TA_ERR_INVALID) {
        return fail(d, TA_CHART_ERROR_FOREIGN_CONTENT, node, "the lowering refuses this element");
    }
    if (result != TA_OK) {
        return result;
    }
    *out = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_SEND, .as.send = send };
    return TA_OK;
}

static ta_result decode_script_text(decoder *d, txml_cursor node, ta_expression *out) {
    attr attrs[] = { ATTR("src") };
    ta_result result = read_attrs(d, node, attrs, 1);
    if (result != TA_OK) {
        return result;
    }
    bool elements = false;
    bool text = false;
    content_shape(node, &elements, &text);
    if (elements) {
        return fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, node, "<script> holds text, not elements");
    }
    if (attrs[0].present) {
        if (text) {
            return fail_attr(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, node, &attrs[0], "<script> has both src and a body");
        }
        ta_bytes fetched = { 0 };
        result = fetch(d, node, &attrs[0], &fetched);
        if (result == TA_OK) {
            *out = source((ta_str){ .ptr = fetched.ptr != NULL ? (const char *)fetched.ptr : "", .len = fetched.len });
            out->span = file_span(d, attrs[0].value, fetched);
        }
        return result;
    }
    ta_str body;
    result = character_data(d, node, &body);
    if (result == TA_OK) {
        *out = source(body);
        out->span = text_span(d, node);
    }
    return result;
}

static ta_result decode_send(decoder *d, txml_cursor node, ta_scxml_send_spec *out) {
    enum { EVENT, EVENTEXPR, TARGET, TARGETEXPR, TYPE, TYPEEXPR, ID, IDLOCATION, DELAY, DELAYEXPR, NAMELIST, COUNT };
    attr attrs[COUNT] = { ATTR("event"), ATTR("eventexpr"), ATTR("target"), ATTR("targetexpr"), ATTR("type"),
                          ATTR("typeexpr"), ATTR("id"), ATTR("idlocation"), ATTR("delay"), ATTR("delayexpr"),
                          ATTR("namelist") };
    ta_result result = read_attrs(d, node, attrs, COUNT);
    for (uint32_t pair = EVENT; result == TA_OK && pair < NAMELIST; pair += 2u) {
        result = exclusive(d, node, &attrs[pair], &attrs[pair + 1u]);
    }
    if (result == TA_OK) {
        result = only_elements(d, node);
    }
    if (result != TA_OK) {
        return result;
    }
    if (!attrs[EVENT].present && !attrs[EVENTEXPR].present) {
        return fail_attr(d, TA_CHART_ERROR_MISSING_ATTRIBUTE, node, &attrs[EVENT], "<send> needs event or eventexpr");
    }
    *out = (ta_scxml_send_spec){
        .id = id_pair(d, &attrs[ID], &attrs[IDLOCATION]),
        .type = string_pair(d, &attrs[TYPE], &attrs[TYPEEXPR]),
        .event = attrs[EVENT].present
                     ? (ta_scxml_event_spec){ .kind = TA_SCXML_EVENT_NAME, .as.name = attrs[EVENT].value }
                     : (ta_scxml_event_spec){ .kind = TA_SCXML_EVENT_EXPR, .as.expr = expression(d, &attrs[EVENTEXPR]) },
        .target = string_pair(d, &attrs[TARGET], &attrs[TARGETEXPR]),
        .delay = string_pair(d, &attrs[DELAY], &attrs[DELAYEXPR]),
    };
    payload body = { .namelist = &attrs[NAMELIST] };
    result = decode_payload(d, node, &body);
    if (result == TA_OK) {
        out->params = body.params;
        out->data = body.content;
    }
    return result;
}

static ta_result decode_if(decoder *d, txml_cursor node, ta_scxml_action_spec *out);

static ta_result decode_action(decoder *d, txml_cursor node, ta_scxml_action_spec *out) {
    if (!in_scxml_namespace(node)) {
        return decode_foreign(d, node, out);
    }
    ta_str name = local_name(node);
    ta_result result = TA_OK;
    if (str_is(name, "raise")) {
        attr attrs[] = { ATTR("event") };
        result = read_attrs(d, node, attrs, 1);
        if (result == TA_OK && first_element(node, &(txml_cursor){ 0 })) {
            result = fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, node, "<raise> has no children");
        }
        if (result == TA_OK && !attrs[0].present) {
            result = fail_attr(d, TA_CHART_ERROR_MISSING_ATTRIBUTE, node, &attrs[0], "<raise> needs an event");
        }
        if (result == TA_OK) {
            *out = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = attrs[0].value } };
        }
        return result;
    }
    if (str_is(name, "send")) {
        *out = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_SEND };
        return decode_send(d, node, &out->as.send);
    }
    if (str_is(name, "cancel")) {
        attr attrs[] = { ATTR("sendid"), ATTR("sendidexpr") };
        result = read_attrs(d, node, attrs, 2);
        if (result == TA_OK) {
            result = exclusive(d, node, &attrs[0], &attrs[1]);
        }
        if (result == TA_OK && !attrs[0].present && !attrs[1].present) {
            result = fail_attr(d, TA_CHART_ERROR_MISSING_ATTRIBUTE, node, &attrs[0], "<cancel> needs sendid or sendidexpr");
        }
        if (result == TA_OK) {
            *out = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_CANCEL,
                                           .as.cancel = { .sendid = string_pair(d, &attrs[0], &attrs[1]) } };
        }
        return result;
    }
    if (str_is(name, "assign")) {
        attr attrs[] = { ATTR("location"), ATTR("expr") };
        result = read_attrs(d, node, attrs, 2);
        if (result != TA_OK) {
            return result;
        }
        if (!attrs[0].present) {
            return fail_attr(d, TA_CHART_ERROR_MISSING_ATTRIBUTE, node, &attrs[0], "<assign> needs a location");
        }
        bool elements = false;
        bool text = false;
        content_shape(node, &elements, &text);
        *out = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_ASSIGN,
                                       .as.assign = { .location = expression(d, &attrs[0]) } };
        if (attrs[1].present) {
            if (elements || text) {
                return fail_attr(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, node, &attrs[1], "<assign> has both expr and a body");
            }
            out->as.assign.value = (ta_scxml_value_spec){ .kind = TA_SCXML_VALUE_EXPR, .as.expr = expression(d, &attrs[1]) };
            return TA_OK;
        }
        if (!elements && !text) {
            return fail_attr(d, TA_CHART_ERROR_MISSING_ATTRIBUTE, node, &attrs[1], "<assign> needs expr or a body");
        }
        return literal_content(d, node, &out->as.assign.value);
    }
    if (str_is(name, "script")) {
        *out = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_SCRIPT };
        return decode_script_text(d, node, &out->as.script.script);
    }
    if (str_is(name, "log")) {
        attr attrs[] = { ATTR("label"), ATTR("expr") };
        result = read_attrs(d, node, attrs, 2);
        if (result == TA_OK && first_element(node, &(txml_cursor){ 0 })) {
            result = fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, node, "<log> has no children");
        }
        if (result == TA_OK) {
            *out = (ta_scxml_action_spec){
                .kind = TA_SCXML_ACTION_LOG,
                .as.log = { .label = attrs[0].present ? attrs[0].value : (ta_str){ 0 },
                            .expr = attrs[1].present ? expression(d, &attrs[1]) : (ta_expression){ 0 } },
            };
        }
        return result;
    }
    if (str_is(name, "if")) {
        return decode_if(d, node, out);
    }
    if (str_is(name, "foreach")) {
        attr attrs[] = { ATTR("array"), ATTR("item"), ATTR("index") };
        result = read_attrs(d, node, attrs, 3);
        if (result == TA_OK) {
            result = only_elements(d, node);
        }
        if (result != TA_OK) {
            return result;
        }
        if (!attrs[0].present || !attrs[1].present) {
            return fail_attr(d, TA_CHART_ERROR_MISSING_ATTRIBUTE, node, attrs[0].present ? &attrs[1] : &attrs[0],
                             "<foreach> needs array and item");
        }
        *out = (ta_scxml_action_spec){
            .kind = TA_SCXML_ACTION_FOREACH,
            .as.foreach = { .array = expression(d, &attrs[0]), .item = attrs[1].value,
                            .index = attrs[2].present ? attrs[2].value : (ta_str){ 0 } },
        };
        txml_cursor first;
        bool has_first = first_element(node, &first);
        return decode_actions(d, first, has_first, false, &out->as.foreach.body, NULL, NULL);
    }
    if (str_is(name, "elseif") || str_is(name, "else")) {
        return fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, node, "outside an <if>");
    }
    return fail(d, TA_CHART_ERROR_UNKNOWN_CONSTRUCT, node, "not executable content");
}

/*
 * The executable content from `first` on. With `branches`, it stops at an
 * <elseif> or <else>, which is returned in `marker`. An empty block is null.
 */
static ta_result decode_actions(decoder *d, txml_cursor first, bool has_first, bool branches,
                                const ta_scxml_action_spec **out, txml_cursor *marker, bool *has_marker) {
    uint32_t count = 0;
    txml_cursor node = first;
    bool more = has_first;
    while (more && !(branches && (is_element(node, "elseif") || is_element(node, "else")))) {
        count += 1u;
        more = txml_next_element(&node) == TXML_MOVED;
    }
    if (has_marker != NULL) {
        *has_marker = more;
        *marker = node;
    }
    *out = NULL;
    if (count == 0) {
        return TA_OK;
    }
    ta_scxml_action_spec *list = ALLOC_LIST(d, count, ta_scxml_action_spec);
    if (list == NULL) {
        return TA_ERR_NOMEM;
    }
    node = first;
    for (uint32_t index = 0; index < count; index += 1u) {
        ta_result result = decode_action(d, node, &list[index]);
        if (result != TA_OK) {
            return result;
        }
        list[index].span = element_span(d, node);
        (void)txml_next_element(&node);
    }
    *out = list;
    return TA_OK;
}

/* What follows an <elseif> or <else> marker, as the else branch. */
static ta_result decode_else(decoder *d, txml_cursor marker, bool has_marker, const ta_scxml_action_spec **out) {
    *out = NULL;
    if (!has_marker) {
        return TA_OK;
    }
    txml_cursor next = marker;
    bool has_next = txml_next_element(&next) == TXML_MOVED;
    txml_cursor after;
    bool has_after = false;
    if (is_element(marker, "else")) {
        ta_result result = read_attrs(d, marker, NULL, 0);
        if (result == TA_OK && first_element(marker, &(txml_cursor){ 0 })) {
            result = fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, marker, "<else> is empty");
        }
        if (result == TA_OK) {
            result = decode_actions(d, next, has_next, true, out, &after, &has_after);
        }
        if (result == TA_OK && has_after) {
            result = fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, after, "after <else>");
        }
        return result;
    }
    /* <elseif cond>: an <if> in the else branch. */
    attr attrs[] = { ATTR("cond") };
    ta_result result = read_attrs(d, marker, attrs, 1);
    if (result == TA_OK && first_element(marker, &(txml_cursor){ 0 })) {
        result = fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, marker, "<elseif> is empty");
    }
    if (result == TA_OK && !attrs[0].present) {
        result = fail_attr(d, TA_CHART_ERROR_MISSING_ATTRIBUTE, marker, &attrs[0], "<elseif> needs a cond");
    }
    if (result != TA_OK) {
        return result;
    }
    ta_scxml_action_spec *branch = ALLOC_LIST(d, 1, ta_scxml_action_spec);
    if (branch == NULL) {
        return TA_ERR_NOMEM;
    }
    /* An <elseif> is an <if> in the else branch, written where the <elseif> is. */
    branch[0] = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_IF, .span = element_span(d, marker),
                                        .as.conditional = { .condition = expression(d, &attrs[0]) } };
    result = decode_actions(d, next, has_next, true, &branch[0].as.conditional.then_actions, &after, &has_after);
    if (result == TA_OK) {
        result = decode_else(d, after, has_after, &branch[0].as.conditional.else_actions);
    }
    *out = branch;
    return result;
}

static ta_result decode_if(decoder *d, txml_cursor node, ta_scxml_action_spec *out) {
    attr attrs[] = { ATTR("cond") };
    ta_result result = read_attrs(d, node, attrs, 1);
    if (result == TA_OK) {
        result = only_elements(d, node);
    }
    if (result == TA_OK && !attrs[0].present) {
        result = fail_attr(d, TA_CHART_ERROR_MISSING_ATTRIBUTE, node, &attrs[0], "<if> needs a cond");
    }
    if (result != TA_OK) {
        return result;
    }
    *out = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_IF, .as.conditional = { .condition = expression(d, &attrs[0]) } };
    txml_cursor first;
    bool has_first = first_element(node, &first);
    txml_cursor marker;
    bool has_marker = false;
    result = decode_actions(d, first, has_first, true, &out->as.conditional.then_actions, &marker, &has_marker);
    if (result == TA_OK) {
        result = decode_else(d, marker, has_marker, &out->as.conditional.else_actions);
    }
    return result;
}

/* A container of executable content: <onentry>, <onexit>, <finalize>, and a
 * <transition>'s body. */
static ta_result decode_block(decoder *d, txml_cursor node, const ta_scxml_action_spec **out) {
    ta_result result = only_elements(d, node);
    if (result != TA_OK) {
        return result;
    }
    txml_cursor first;
    bool has_first = first_element(node, &first);
    return decode_actions(d, first, has_first, false, out, NULL, NULL);
}

static ta_result decode_transition(decoder *d, txml_cursor node, ta_scxml_transition_spec *out) {
    attr attrs[] = { ATTR("event"), ATTR("cond"), ATTR("target"), ATTR("type") };
    ta_result result = read_attrs(d, node, attrs, 4);
    if (result != TA_OK) {
        return result;
    }
    *out = (ta_scxml_transition_spec){ .kind = TA_SCXML_TRANSITION_EXTERNAL, .span = element_span(d, node) };
    if (attrs[3].present) {
        if (str_is(attrs[3].value, "internal")) {
            out->kind = TA_SCXML_TRANSITION_INTERNAL;
        } else if (!str_is(attrs[3].value, "external")) {
            return fail_attr(d, TA_CHART_ERROR_BAD_VALUE, node, &attrs[3], "type is internal or external");
        }
    }
    if (attrs[0].present) {
        out->event = attrs[0].value;
    }
    if (attrs[1].present) {
        out->condition = expression(d, &attrs[1]);
    }
    if (attrs[2].present) {
        result = token_list(d, attrs[2].value, &out->targets);
    }
    if (result == TA_OK) {
        result = decode_block(d, node, &out->actions);
    }
    return result;
}

/* A <datamodel>'s <data> children, appended to `list` from `at`. */
static ta_result decode_data(decoder *d, txml_cursor node, ta_scxml_data_spec *out) {
    attr attrs[] = { ATTR("id"), ATTR("src"), ATTR("expr") };
    ta_result result = read_attrs(d, node, attrs, 3);
    if (result != TA_OK) {
        return result;
    }
    if (!attrs[0].present) {
        return fail_attr(d, TA_CHART_ERROR_MISSING_ATTRIBUTE, node, &attrs[0], "<data> needs an id");
    }
    bool elements = false;
    bool text = false;
    content_shape(node, &elements, &text);
    uint32_t initializers = (attrs[1].present ? 1u : 0u) + (attrs[2].present ? 1u : 0u) + (elements || text ? 1u : 0u);
    if (initializers > 1u) {
        return fail_attr(d, TA_CHART_ERROR_CONFLICTING_ATTRIBUTE, node, attrs[1].present ? &attrs[1] : &attrs[2],
                         "<data> takes one of src, expr and a body");
    }
    *out = (ta_scxml_data_spec){ .name = attrs[0].value };
    if (attrs[2].present) {
        out->initializer = (ta_scxml_value_spec){ .kind = TA_SCXML_VALUE_EXPR, .as.expr = expression(d, &attrs[2]) };
    } else if (attrs[1].present) {
        ta_bytes fetched = { 0 };
        result = fetch(d, node, &attrs[1], &fetched);
        if (result == TA_OK) {
            /* A fetched resource is literal content like an inline body, and
             * labelled like one: the codec knows no more about it, and encode
             * writes it back inline. */
            out->initializer = (ta_scxml_value_spec){
                .kind = TA_SCXML_VALUE_CONTENT,
                .as.content = { .media_type = cstr(TA_SCXML_MEDIA_TYPE),
                                .bytes = fetched.ptr != NULL ? fetched : (ta_bytes){ .ptr = (const uint8_t *)"", .len = 0 } },
            };
        }
    } else if (elements || text) {
        result = literal_content(d, node, &out->initializer);
    }
    return result;
}

static ta_result decode_datamodel(decoder *d, txml_cursor parent, const ta_scxml_data_spec **out) {
    uint32_t count = 0;
    txml_cursor child;
    for (bool more = first_element(parent, &child); more; more = txml_next_element(&child) == TXML_MOVED) {
        if (!is_element(child, "datamodel")) {
            continue;
        }
        ta_result result = read_attrs(d, child, NULL, 0);
        if (result == TA_OK) {
            result = only_elements(d, child);
        }
        if (result != TA_OK) {
            return result;
        }
        txml_cursor data;
        for (bool each = first_element(child, &data); each; each = txml_next_element(&data) == TXML_MOVED) {
            if (!is_element(data, "data")) {
                return fail(d, in_scxml_namespace(data) ? TA_CHART_ERROR_MISPLACED_CONSTRUCT : TA_CHART_ERROR_UNKNOWN_CONSTRUCT,
                            data, "only <data> belongs in <datamodel>");
            }
            count += 1u;
        }
    }
    *out = NULL;
    if (count == 0) {
        return TA_OK;
    }
    ta_scxml_data_spec *list = ALLOC_LIST(d, count, ta_scxml_data_spec);
    if (list == NULL) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (bool more = first_element(parent, &child); more; more = txml_next_element(&child) == TXML_MOVED) {
        if (!is_element(child, "datamodel")) {
            continue;
        }
        txml_cursor data;
        for (bool each = first_element(child, &data); each; each = txml_next_element(&data) == TXML_MOVED) {
            ta_result result = decode_data(d, data, &list[at++]);
            if (result != TA_OK) {
                return result;
            }
        }
    }
    *out = list;
    return TA_OK;
}

static bool is_state_element(txml_cursor node) {
    return is_element(node, "state") || is_element(node, "parallel") || is_element(node, "final") ||
           is_element(node, "history");
}

/*
 * A state without an id gets one (SCXML 3.14). It must not be any state's
 * authored id anywhere in the document, inline machines included.
 */
static ta_result anonymous_id(decoder *d, ta_str *out) {
    static const char prefix[] = "_ta_anonymous_";
    for (;;) {
        d->anonymous += 1u;
        char digits[10];
        uint32_t digit_count = 0;
        for (uint32_t value = d->anonymous; value != 0; value /= 10u) {
            digits[digit_count++] = (char)('0' + value % 10u);
        }
        uint32_t length = (uint32_t)(sizeof(prefix) - 1u) + digit_count;
        char *text = ta_arena_alloc(d->arena, length, 1);
        if (text == NULL) {
            return TA_ERR_NOMEM;
        }
        copy_bytes(text, prefix, (uint32_t)(sizeof(prefix) - 1u));
        for (uint32_t index = 0; index < digit_count; index += 1u) {
            text[sizeof(prefix) - 1u + index] = digits[digit_count - 1u - index];
        }
        ta_str candidate = { .ptr = text, .len = length };
        bool taken = false;
        uint32_t nodes = txml_doc_node_count(d->doc);
        for (uint32_t index = 0; index < nodes && !taken; index += 1u) {
            txml_cursor node = txml_cursor_at(d->doc, index);
            if (!is_state_element(node)) {
                continue;
            }
            txml_attr_cursor cursor;
            for (txml_move move = txml_first_attribute(node, &cursor); move == TXML_MOVED && !taken;
                 move = txml_next_attribute(&cursor)) {
                txml_attribute_view view;
                txml_qname name;
                if (txml_attribute_read(cursor, &view) == TXML_OK && txml_doc_name_at(d->doc, view.name, &name) == TXML_OK &&
                    name.ns.length == 0 && txml_is(name.local, "id") && str_equal(from_txml(view.value), candidate)) {
                    taken = true;
                }
            }
        }
        if (!taken) {
            *out = candidate;
            return TA_OK;
        }
    }
}

static ta_result decode_states(decoder *d, txml_cursor parent, const ta_scxml_state_spec **out);

/* <initial><transition target>actions</transition></initial>. */
static ta_result decode_initial_element(decoder *d, txml_cursor node, ta_scxml_initial_spec *out) {
    ta_result result = read_attrs(d, node, NULL, 0);
    if (result == TA_OK) {
        result = only_elements(d, node);
    }
    if (result != TA_OK) {
        return result;
    }
    txml_cursor transition;
    bool has = first_element(node, &transition);
    txml_cursor extra = transition;
    if (!has || !is_element(transition, "transition") || txml_next_element(&extra) == TXML_MOVED) {
        return fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, node, "<initial> holds exactly one <transition>");
    }
    attr attrs[] = { ATTR("target") };
    result = read_attrs(d, transition, attrs, 1);
    if (result == TA_OK && !attrs[0].present) {
        result = fail_attr(d, TA_CHART_ERROR_MISSING_ATTRIBUTE, transition, &attrs[0], "the initial transition needs a target");
    }
    if (result == TA_OK) {
        result = token_list(d, attrs[0].value, &out->targets);
    }
    if (result == TA_OK) {
        result = decode_block(d, transition, &out->actions);
    }
    out->span = element_span(d, node);
    return result;
}

static ta_result decode_invoke(decoder *d, txml_cursor node, ta_scxml_invoke_spec *out) {
    enum { TYPE, TYPEEXPR, SRC, SRCEXPR, ID, IDLOCATION, NAMELIST, AUTOFORWARD, COUNT };
    attr attrs[COUNT] = { ATTR("type"), ATTR("typeexpr"), ATTR("src"), ATTR("srcexpr"),
                          ATTR("id"), ATTR("idlocation"), ATTR("namelist"), ATTR("autoforward") };
    ta_result result = read_attrs(d, node, attrs, COUNT);
    for (uint32_t pair = TYPE; result == TA_OK && pair < NAMELIST; pair += 2u) {
        result = exclusive(d, node, &attrs[pair], &attrs[pair + 1u]);
    }
    if (result == TA_OK) {
        result = only_elements(d, node);
    }
    if (result != TA_OK) {
        return result;
    }
    bool autoforward = false;
    if (attrs[AUTOFORWARD].present) {
        if (str_is(attrs[AUTOFORWARD].value, "true")) {
            autoforward = true;
        } else if (!str_is(attrs[AUTOFORWARD].value, "false")) {
            return fail_attr(d, TA_CHART_ERROR_BAD_VALUE, node, &attrs[AUTOFORWARD], "autoforward is true or false");
        }
    }
    *out = (ta_scxml_invoke_spec){
        .kind = TA_SCXML_INVOKE,
        .id = id_pair(d, &attrs[ID], &attrs[IDLOCATION]),
        .type = string_pair(d, &attrs[TYPE], &attrs[TYPEEXPR]),
        .src = string_pair(d, &attrs[SRC], &attrs[SRCEXPR]),
        .autoforward = autoforward,
    };
    payload body = { .namelist = &attrs[NAMELIST], .finalize_allowed = true, .params_with_content = true };
    result = decode_payload(d, node, &body);
    if (result != TA_OK) {
        return result;
    }
    out->params = body.params;
    out->data = body.content;
    uint32_t finalizers = 0;
    txml_cursor child;
    for (bool more = first_element(node, &child); more; more = txml_next_element(&child) == TXML_MOVED) {
        if (!is_element(child, "finalize")) {
            continue;
        }
        if (++finalizers > 1u) {
            return fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, child, "a second <finalize>");
        }
        result = read_attrs(d, child, NULL, 0);
        if (result == TA_OK) {
            result = decode_block(d, child, &out->finalize);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

static ta_result decode_state(decoder *d, txml_cursor node, ta_scxml_state_spec *out) {
    ta_str element = local_name(node);
    bool is_state = str_is(element, "state");
    bool is_final = str_is(element, "final");
    bool is_history = str_is(element, "history");
    attr attrs[] = { ATTR("id"), ATTR("initial"), ATTR("type") };
    /* <state> may name its initial children; <history> its depth. */
    attr *allowed_extra = is_state ? &attrs[1] : is_history ? &attrs[2] : NULL;
    attr table[2] = { attrs[0], allowed_extra != NULL ? *allowed_extra : attrs[0] };
    ta_result result = read_attrs(d, node, table, allowed_extra != NULL ? 2u : 1u);
    if (result == TA_OK) {
        result = only_elements(d, node);
    }
    if (result != TA_OK) {
        return result;
    }
    attr id = table[0];
    attr extra = allowed_extra != NULL ? table[1] : (attr){ 0 };

    *out = (ta_scxml_state_spec){ .span = element_span(d, node) };
    if (id.present) {
        out->id = id.value;
    } else {
        result = anonymous_id(d, &out->id);
        if (result != TA_OK) {
            return result;
        }
    }

    if (is_history) {
        out->kind = TA_SCXML_STATE_HISTORY_SHALLOW;
        if (extra.present) {
            if (str_is(extra.value, "deep")) {
                out->kind = TA_SCXML_STATE_HISTORY_DEEP;
            } else if (!str_is(extra.value, "shallow")) {
                return fail_attr(d, TA_CHART_ERROR_BAD_VALUE, node, &extra, "type is shallow or deep");
            }
        }
        txml_cursor transition;
        bool has = first_element(node, &transition);
        txml_cursor after = transition;
        if (!has || !is_element(transition, "transition") || txml_next_element(&after) == TXML_MOVED) {
            return fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, node, "<history> holds exactly one <transition>");
        }
        ta_scxml_transition_spec *transitions = ALLOC_LIST(d, 1, ta_scxml_transition_spec);
        if (transitions == NULL) {
            return TA_ERR_NOMEM;
        }
        result = decode_transition(d, transition, &transitions[0]);
        out->transitions = transitions;
        return result;
    }

    /* Count each kind of child; the lists keep document order. */
    uint32_t transitions = 0, onentry = 0, onexit = 0, states = 0, invokes = 0, initials = 0, donedata = 0;
    txml_cursor child;
    txml_cursor initial_element = node;
    txml_cursor donedata_element = node;
    for (bool more = first_element(node, &child); more; more = txml_next_element(&child) == TXML_MOVED) {
        if (is_element(child, "transition") && !is_final) {
            transitions += 1u;
        } else if (is_element(child, "onentry")) {
            onentry += 1u;
        } else if (is_element(child, "onexit")) {
            onexit += 1u;
        } else if (is_state_element(child) && !is_final) {
            states += 1u;
        } else if (is_element(child, "invoke") && !is_final) {
            invokes += 1u;
        } else if (is_element(child, "initial") && is_state) {
            initials += 1u;
            initial_element = child;
        } else if (is_element(child, "donedata") && is_final) {
            donedata += 1u;
            donedata_element = child;
        } else if (!is_element(child, "datamodel") || is_final) {
            return fail(d, in_scxml_namespace(child) ? TA_CHART_ERROR_MISPLACED_CONSTRUCT : TA_CHART_ERROR_UNKNOWN_CONSTRUCT,
                        child, "not allowed in this state");
        }
    }
    if (initials > 1u || donedata > 1u) {
        return fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, initials > 1u ? initial_element : donedata_element,
                    "at most one of these");
    }
    if (initials != 0 && extra.present) {
        return fail_attr(d, TA_CHART_ERROR_CONFLICTING_ATTRIBUTE, node, &extra, "both an initial attribute and <initial>");
    }
    out->kind = is_final ? TA_SCXML_STATE_FINAL
                : str_is(element, "parallel") ? TA_SCXML_STATE_PARALLEL
                : states != 0 ? TA_SCXML_STATE_COMPOUND
                              : TA_SCXML_STATE_ATOMIC;

    if (extra.present || initials != 0) {
        ta_scxml_initial_spec *initial = ALLOC_LIST(d, 0, ta_scxml_initial_spec);
        if (initial == NULL) {
            return TA_ERR_NOMEM;
        }
        result = extra.present ? token_list(d, extra.value, &initial->targets)
                               : decode_initial_element(d, initial_element, initial);
        if (result != TA_OK) {
            return result;
        }
        if (extra.present) {
            initial->span = attribute_span(d, &extra);
        }
        out->initial = initial;
    }

    ta_scxml_transition_spec *transition_list = transitions != 0 ? ALLOC_LIST(d, transitions, ta_scxml_transition_spec) : NULL;
    ta_scxml_handler_spec *entry_list = onentry != 0 ? ALLOC_LIST(d, onentry, ta_scxml_handler_spec) : NULL;
    ta_scxml_handler_spec *exit_list = onexit != 0 ? ALLOC_LIST(d, onexit, ta_scxml_handler_spec) : NULL;
    ta_scxml_invoke_spec *invoke_list = invokes != 0 ? ALLOC_LIST(d, invokes, ta_scxml_invoke_spec) : NULL;
    if ((transitions != 0 && transition_list == NULL) || (onentry != 0 && entry_list == NULL) ||
        (onexit != 0 && exit_list == NULL) || (invokes != 0 && invoke_list == NULL)) {
        return TA_ERR_NOMEM;
    }
    /* An empty handler (<onentry/>) still needs a non-null action list. */
    static const ta_scxml_action_spec no_actions[1] = { { 0 } };
    uint32_t at_transition = 0, at_entry = 0, at_exit = 0, at_invoke = 0;
    for (bool more = first_element(node, &child); more && result == TA_OK; more = txml_next_element(&child) == TXML_MOVED) {
        if (is_element(child, "transition")) {
            result = decode_transition(d, child, &transition_list[at_transition++]);
        } else if (is_element(child, "onentry") || is_element(child, "onexit")) {
            ta_scxml_handler_spec *handler = is_element(child, "onentry") ? &entry_list[at_entry++] : &exit_list[at_exit++];
            result = read_attrs(d, child, NULL, 0);
            if (result == TA_OK) {
                result = decode_block(d, child, &handler->actions);
            }
            if (result == TA_OK && handler->actions == NULL) {
                handler->actions = no_actions;
            }
        } else if (is_element(child, "invoke")) {
            result = decode_invoke(d, child, &invoke_list[at_invoke++]);
        } else if (is_element(child, "donedata")) {
            ta_scxml_done_data_spec *done = ALLOC_LIST(d, 0, ta_scxml_done_data_spec);
            if (done == NULL) {
                return TA_ERR_NOMEM;
            }
            result = read_attrs(d, child, NULL, 0);
            if (result == TA_OK) {
                result = only_elements(d, child);
            }
            payload body = { 0 };
            if (result == TA_OK) {
                result = decode_payload(d, child, &body);
            }
            done->params = body.params;
            done->content = body.content;
            out->done_data = done;
        }
    }
    if (result != TA_OK) {
        return result;
    }
    out->transitions = transition_list;
    out->onentry = entry_list;
    out->onexit = exit_list;
    out->invokes = invoke_list;
    result = decode_datamodel(d, node, &out->data);
    if (result == TA_OK && states != 0) {
        result = decode_states(d, node, &out->states);
    }
    return result;
}

static ta_result decode_states(decoder *d, txml_cursor parent, const ta_scxml_state_spec **out) {
    uint32_t count = 0;
    txml_cursor child;
    for (bool more = first_element(parent, &child); more; more = txml_next_element(&child) == TXML_MOVED) {
        count += is_state_element(child) ? 1u : 0u;
    }
    *out = NULL;
    if (count == 0) {
        return TA_OK;
    }
    ta_scxml_state_spec *list = ALLOC_LIST(d, count, ta_scxml_state_spec);
    if (list == NULL) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (bool more = first_element(parent, &child); more; more = txml_next_element(&child) == TXML_MOVED) {
        if (is_state_element(child)) {
            ta_result result = decode_state(d, child, &list[at++]);
            if (result != TA_OK) {
                return result;
            }
        }
    }
    *out = list;
    return TA_OK;
}

static ta_result decode_root(decoder *d, txml_cursor root, ta_scxml_spec *spec) {
    if (!is_element(root, "scxml")) {
        return fail(d, TA_CHART_ERROR_NOT_CHART, root, "the document element is not <scxml> in the SCXML namespace");
    }
    enum { INITIAL, NAME, DATAMODEL, BINDING, VERSION, COUNT };
    attr attrs[COUNT] = { ATTR("initial"), ATTR("name"), ATTR("datamodel"), ATTR("binding"), ATTR("version") };
    ta_result result = read_attrs(d, root, attrs, COUNT);
    if (result == TA_OK) {
        result = only_elements(d, root);
    }
    if (result != TA_OK) {
        return result;
    }
    *spec = (ta_scxml_spec){ .identity = d->params->identity };
    if (attrs[NAME].present) {
        spec->name = attrs[NAME].value;
    }
    if (attrs[DATAMODEL].present) {
        spec->datamodel = attrs[DATAMODEL].value;
    }
    if (attrs[BINDING].present) {
        if (str_is(attrs[BINDING].value, "early")) {
            spec->binding = TA_SCXML_BINDING_EARLY;
        } else if (str_is(attrs[BINDING].value, "late")) {
            spec->binding = TA_SCXML_BINDING_LATE;
        } else {
            return fail_attr(d, TA_CHART_ERROR_BAD_VALUE, root, &attrs[BINDING], "binding is early or late");
        }
    }
    if (attrs[VERSION].present && !str_is(attrs[VERSION].value, "1.0")) {
        return fail_attr(d, TA_CHART_ERROR_BAD_VALUE, root, &attrs[VERSION], "version is 1.0");
    }
    if (attrs[INITIAL].present) {
        ta_scxml_initial_spec *initial = ALLOC_LIST(d, 0, ta_scxml_initial_spec);
        if (initial == NULL) {
            return TA_ERR_NOMEM;
        }
        result = token_list(d, attrs[INITIAL].value, &initial->targets);
        if (result != TA_OK) {
            return result;
        }
        initial->span = attribute_span(d, &attrs[INITIAL]);
        spec->initial = initial;
    }
    uint32_t scripts = 0;
    txml_cursor child;
    for (bool more = first_element(root, &child); more; more = txml_next_element(&child) == TXML_MOVED) {
        if (is_element(child, "script")) {
            if (++scripts > 1u) {
                return fail(d, TA_CHART_ERROR_MISPLACED_CONSTRUCT, child, "a second <script> in <scxml>");
            }
            result = decode_script_text(d, child, &spec->script);
            if (result != TA_OK) {
                return result;
            }
            spec->script_span = element_span(d, child);
        } else if (is_element(child, "history") ||
                   !(is_state_element(child) || is_element(child, "datamodel"))) {
            return fail(d, in_scxml_namespace(child) ? TA_CHART_ERROR_MISPLACED_CONSTRUCT : TA_CHART_ERROR_UNKNOWN_CONSTRUCT,
                        child, "not allowed in <scxml>");
        }
    }
    result = decode_datamodel(d, root, &spec->data);
    if (result == TA_OK) {
        result = decode_states(d, root, &spec->states);
    }
    if (result == TA_OK && d->out_of_memory) {
        result = TA_ERR_NOMEM;
    }
    spec->spans = d->spans;
    spec->span_count = d->span_count;
    spec->sources = d->sources;
    spec->source_count = d->source_count;
    return result;
}

/* The field of ta_scxml_codec_params behind a txml limit. */
static const char *limit_name(txml_limit_kind kind) {
    switch (kind) {
        case TXML_LIMIT_DEPTH: return "max_depth";
        case TXML_LIMIT_ATTRIBUTES: return "max_attributes";
        case TXML_LIMIT_NAME_LENGTH: return "max_name_length";
        case TXML_LIMIT_NAMES: return "max_names";
        case TXML_LIMIT_DOCUMENT_BYTES: return "max_parse_bytes";
        case TXML_LIMIT_ENTITY_EXPANSION: return "max_entity_expansion";
        default: return "limit";
    }
}

/* Where byte `offset` of the input is, counting lines as txml does. */
static ta_chart_position input_position(ta_bytes input, uint64_t offset) {
    ta_chart_position at = { .offset = offset, .line = 1, .column = 1 };
    for (uint64_t index = 0; index < offset && index < input.len; index += 1u) {
        if (input.ptr[index] == '\n') {
            at.line += 1u;
            at.column = 1;
        } else {
            at.column += 1u;
        }
    }
    return at;
}

static ta_result scxml_decode(void *self, const ta_chart_decode_params *params) {
    const ta_scxml_codec *codec = self;
    if (codec == NULL || params == NULL || params->arena == NULL || params->out == NULL ||
        (params->input.ptr == NULL && params->input.len != 0)) {
        return TA_ERR_INVALID;
    }
    decoder d = { .params = params, .limits = &codec->limits, .arena = params->arena, .input = params->input };
    (void)add_source(&d, params->name);
    if (params->input.len > codec->limits.max_document_bytes) {
        ta_result result = fail_at(&d, TA_CHART_ERROR_LIMIT, input_position(params->input, codec->limits.max_document_bytes),
                                   cstr("max_document_bytes"), (ta_str){ 0 }, "the input is longer than max_document_bytes");
        if (params->error != NULL) {
            *params->error = d.error;
        }
        return result;
    }
    txml_parser *parser = NULL;
    txml_step step = parse(params->arena, d.limits, &params->input, 1, &parser);
    ta_result result = TA_OK;
    if (step == TXML_STEP_NEED_MEMORY) {
        result = TA_ERR_NOMEM;
    } else if (step != TXML_STEP_DONE) {
        txml_document_error error = { 0 };
        (void)txml_parser_error(parser, &error);
        bool limit = error.code == TXML_DOC_LIMIT;
        result = fail_at(&d, limit ? TA_CHART_ERROR_LIMIT : TA_CHART_ERROR_SYNTAX, position_from(error.at),
                         limit ? cstr(limit_name(error.limit)) : (ta_str){ 0 }, (ta_str){ 0 }, "not well-formed XML");
        d.error.message = from_txml(error.message);
    } else {
        d.doc = txml_parser_doc(parser);
        txml_cursor root;
        ta_scxml_spec *spec = ta_arena_alloc(params->arena, sizeof(ta_scxml_spec), _Alignof(ta_scxml_spec));
        if (spec == NULL) {
            result = TA_ERR_NOMEM;
        } else if (!first_element(txml_doc_root(d.doc), &root)) {
            result = fail_at(&d, TA_CHART_ERROR_NOT_CHART, (ta_chart_position){ 0 }, (ta_str){ 0 }, (ta_str){ 0 },
                             "no document element");
        } else {
            result = decode_root(&d, root, spec);
            if (result == TA_OK) {
                *params->out = spec;
            }
        }
    }
    if (result == TA_ERR_INVALID && params->error != NULL) {
        *params->error = d.error;
    }
    return result;
}

/* ========================================================================
 * Encode
 */

/* Where one element or expression landed in the output (ADR 0086). */
typedef struct {
    uint32_t span;
    uint64_t start;
    uint64_t end;
} landed;

typedef struct {
    const ta_chart_encode_params *params;
    const ta_scxml_codec_params *limits;
    ta_arena *arena;
    output out;
    ta_chart_error error;
    /* Elements open around the one being written, and whether each has an
     * element child yet: a parent with one ends on a line of its own. */
    uint32_t depth;
    bool *nested;
    /* Where each element with a span landed, reported once the output is
     * whole and its lines are known. */
    landed *landings;
    uint32_t landing_count;
    uint32_t landing_capacity;
} encoder;

static ta_result refuse(encoder *e, const char *construct, const char *message) {
    e->error = (ta_chart_error){ .code = TA_CHART_ERROR_UNREPRESENTABLE, .construct = cstr(construct),
                                 .message = cstr(message) };
    return TA_ERR_INVALID;
}

static ta_result written(encoder *e, txml_result result, const char *construct) {
    result = output_pump(&e->out, result);
    if (result == TXML_OK) {
        return TA_OK;
    }
    if (result == TXML_ERR_NOMEM) {
        return TA_ERR_NOMEM;
    }
    if (result == TXML_ERR_LIMIT) {
        e->error = (ta_chart_error){ .code = TA_CHART_ERROR_LIMIT, .construct = cstr("max_depth"),
                                     .attribute = cstr(construct),
                                     .message = cstr("the spec nests deeper than max_depth, or a name is too long") };
        return TA_ERR_INVALID;
    }
    return refuse(e, construct, "text that XML cannot carry");
}

/* Bytes written so far: drained, and still in the writer's buffer. */
static uint64_t output_offset(const output *out) {
    return out->total + txml_writer_output(out->writer).length;
}

/*
 * A line break and two spaces per open element, as text. The codec indents
 * by itself rather than with TXML_WRITE_INDENT, which would also indent the
 * markup of a <content> and so change its bytes: only SCXML elements that
 * hold elements get whitespace, and decode ignores whitespace there.
 */
static ta_result indent(encoder *e, uint32_t depth) {
    static const char spaces[] = "\n                                ";
    ta_result result = written(e, txml_write_text(e->out.writer, (txml_str){ spaces, 1 }), "indentation");
    for (uint32_t left = 2u * depth; result == TA_OK && left != 0;) {
        uint32_t chunk = left < sizeof(spaces) - 2u ? left : (uint32_t)(sizeof(spaces) - 2u);
        result = written(e, txml_write_text(e->out.writer, (txml_str){ spaces + 1, chunk }), "indentation");
        left -= chunk;
    }
    return result;
}

/* Starts an element on a line of its own; `at` is where its `<` is. */
static ta_result start_at(encoder *e, const char *local, uint64_t *at) {
    ta_result result = TA_OK;
    if (e->depth != 0) {
        e->nested[e->depth - 1u] = true;
        result = indent(e, e->depth);
    }
    *at = output_offset(&e->out);
    txml_write_name name = { .ns = TXML_STR(SCXML_NS), .local = { local, cstr_length(local) }, .prefix = { "", 0 } };
    if (result == TA_OK) {
        result = written(e, txml_write_start(e->out.writer, &name), local);
    }
    /* The writer refuses to nest deeper than max_depth, which sizes `nested`. */
    if (result == TA_OK) {
        e->nested[e->depth++] = false;
    }
    return result;
}

static ta_result start(encoder *e, const char *local) {
    uint64_t at = 0;
    return start_at(e, local, &at);
}

static ta_result end(encoder *e) {
    e->depth -= 1u;
    ta_result result = e->nested[e->depth] ? indent(e, e->depth) : TA_OK;
    return result == TA_OK ? written(e, txml_write_end(e->out.writer), "end tag") : result;
}

/* Records that the element or expression with `span` occupies `start` up to
 * where the output is now. */
static ta_result landed_at(encoder *e, uint32_t span, uint64_t start) {
    if (span == 0 || e->params->spans.v == NULL) {
        return TA_OK;
    }
    if (e->landing_count == e->landing_capacity) {
        uint32_t capacity = e->landing_capacity == 0 ? 64u : e->landing_capacity * 2u;
        landed *grown = ta_arena_alloc(e->arena, (size_t)capacity * sizeof(landed), _Alignof(landed));
        if (grown == NULL) {
            return TA_ERR_NOMEM;
        }
        if (e->landings != NULL) {
            copy_bytes(grown, e->landings, (uint32_t)(e->landing_count * sizeof(landed)));
        }
        e->landings = grown;
        e->landing_capacity = capacity;
    }
    e->landings[e->landing_count++] = (landed){ .span = span, .start = start, .end = output_offset(&e->out) };
    return TA_OK;
}

/* Ends an element started at `at` and records where it landed. */
static ta_result end_landed(encoder *e, uint32_t span, uint64_t at) {
    ta_result result = end(e);
    return result == TA_OK ? landed_at(e, span, at) : result;
}

static ta_result attribute(encoder *e, const char *local, ta_str value) {
    txml_write_attr attr = { .name = { .ns = { "", 0 }, .local = { local, cstr_length(local) }, .prefix = { "", 0 } },
                             .value = to_txml(value) };
    return written(e, txml_write_attribute(e->out.writer, &attr), local);
}

/* An expression's source text, from the host's printer for a host handle. */
static ta_result expression_text(encoder *e, const ta_expression *expression, const char *construct, ta_str *out) {
    if (expression->kind == TA_EXPRESSION_SOURCE) {
        *out = expression->as.source;
        return TA_OK;
    }
    if (expression->kind == TA_EXPRESSION_HOST) {
        const ta_chart_expression_printer *printer = &e->params->expressions;
        if (printer->v == NULL || printer->v->print == NULL) {
            return refuse(e, construct, "a host expression and no printer");
        }
        ta_result result = printer->v->print(printer->self, &expression->as.host, e->arena, out);
        return result == TA_ERR_INVALID || result == TA_ERR_NOT_FOUND ? refuse(e, construct, "the printer refuses it")
                                                                      : result;
    }
    return refuse(e, construct, "an absent expression");
}

/* An expression as an attribute; its span is the value inside the quotes. */
static ta_result expression_attribute(encoder *e, const char *local, const ta_expression *expression) {
    if (expression->kind == TA_EXPRESSION_NONE) {
        return TA_OK;
    }
    ta_str text;
    ta_result result = expression_text(e, expression, local, &text);
    uint64_t before = output_offset(&e->out);
    if (result == TA_OK) {
        result = attribute(e, local, text);
    }
    if (result == TA_OK && expression->span != 0 && e->params->spans.v != NULL) {
        /* The attribute is written as ` local="value"`. */
        uint64_t value = before + cstr_length(local) + 3u;
        result = landed_at(e, expression->span, value);
        if (result == TA_OK) {
            e->landings[e->landing_count - 1u].end -= 1u;
        }
    }
    return result;
}

/* An expression as an element's character data: a <script>. */
static ta_result expression_body(encoder *e, const ta_expression *expression, const char *construct) {
    ta_str text;
    ta_result result = expression_text(e, expression, construct, &text);
    /* The first text closes the start tag with a `>`. */
    uint64_t body = output_offset(&e->out) + 1u;
    if (result == TA_OK) {
        result = written(e, txml_write_text(e->out.writer, to_txml(text)), construct);
    }
    if (result == TA_OK && text.len == 0) {
        body = output_offset(&e->out);
    }
    return result == TA_OK ? landed_at(e, expression->span, body) : result;
}

static ta_result string_attribute(encoder *e, const char *literal, const char *expression, const ta_scxml_string_spec *spec) {
    if (spec->kind == TA_SCXML_STRING_LITERAL) {
        return attribute(e, literal, spec->as.literal);
    }
    if (spec->kind == TA_SCXML_STRING_EXPR) {
        return expression_attribute(e, expression, &spec->as.expr);
    }
    return TA_OK;
}

static ta_result id_attribute(encoder *e, const ta_scxml_id_spec *spec) {
    if (spec->kind == TA_SCXML_ID_LITERAL) {
        return attribute(e, "id", spec->as.literal);
    }
    if (spec->kind == TA_SCXML_ID_LOCATION) {
        return expression_attribute(e, "idlocation", &spec->as.location);
    }
    return TA_OK;
}

/* A target or initial list, joined with single spaces. */
static ta_result list_attribute(encoder *e, const char *local, const ta_str *list) {
    if (list == NULL || list[0].ptr == NULL) {
        return TA_OK;
    }
    uint64_t length = 0;
    uint32_t count = 0;
    for (; list[count].ptr != NULL; count += 1u) {
        length += list[count].len + 1u;
    }
    char *joined = ta_arena_alloc(e->arena, (size_t)length, 1);
    if (joined == NULL || length > UINT32_MAX) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (uint32_t index = 0; index < count; index += 1u) {
        if (index != 0) {
            joined[at++] = ' ';
        }
        copy_bytes(joined + at, list[index].ptr, list[index].len);
        at += list[index].len;
    }
    return attribute(e, local, (ta_str){ .ptr = joined, .len = at });
}

/*
 * A literal content, in this codec's media type, back as the body it was
 * decoded from. Decode made markup into a re-serialized fragment and text
 * into its character data; which one these bytes are is recovered by parsing
 * them where decode found them (in the SCXML default namespace) and checking
 * that serializing them again gives the same bytes. Only then are they
 * written as markup, so decoding the output gives these bytes back.
 */
static ta_result content_body(encoder *e, const ta_scxml_value_spec *value, const char *construct) {
    if (!str_is(value->as.content.media_type, TA_SCXML_MEDIA_TYPE)) {
        return refuse(e, construct, "literal content of another media type");
    }
    ta_bytes bytes = value->as.content.bytes;
    static const char open[] = "<w xmlns=\"" SCXML_NS "\">";
    static const char close[] = "</w>";
    const ta_bytes parts[3] = {
        { .ptr = (const uint8_t *)open, .len = (uint32_t)(sizeof(open) - 1u) },
        { .ptr = bytes.ptr != NULL ? bytes.ptr : (const uint8_t *)"", .len = bytes.len },
        { .ptr = (const uint8_t *)close, .len = (uint32_t)(sizeof(close) - 1u) },
    };
    txml_parser *parser = NULL;
    txml_step step = parse(e->arena, e->limits, parts, 3, &parser);
    if (step == TXML_STEP_NEED_MEMORY) {
        return TA_ERR_NOMEM;
    }
    if (step == TXML_STEP_DONE) {
        txml_cursor wrapper;
        txml_cursor element;
        if (first_element(txml_doc_root(txml_parser_doc(parser)), &wrapper) && first_element(wrapper, &element)) {
            ta_bytes again = { 0 };
            txml_result result = serialize_children(e->arena, e->limits, wrapper, &again);
            if (result == TXML_ERR_NOMEM) {
                return TA_ERR_NOMEM;
            }
            if (result == TXML_OK && again.len == bytes.len && (bytes.len == 0 || same_bytes(again.ptr, bytes.ptr, bytes.len))) {
                txml_cursor child = wrapper;
                ta_result written_result = TA_OK;
                for (txml_move move = txml_first_child(&child); written_result == TA_OK && move == TXML_MOVED;
                     move = txml_next_sibling(&child)) {
                    written_result = written(e, txml_write_subtree(e->out.writer, child), construct);
                }
                return written_result;
            }
        }
    }
    return written(e, txml_write_text(e->out.writer, (txml_str){ .ptr = (const char *)parts[1].ptr, .length = bytes.len }),
                   construct);
}

/* <content expr> or a literal <content>; `data` of <send>, <invoke>, <donedata>. */
static ta_result content_element(encoder *e, const ta_scxml_value_spec *value, const char *construct) {
    if (value->kind == TA_SCXML_VALUE_NONE) {
        return TA_OK;
    }
    if (value->kind == TA_SCXML_VALUE_LOCATION) {
        return refuse(e, construct, "a location as content");
    }
    ta_result result = start(e, "content");
    if (result == TA_OK) {
        result = value->kind == TA_SCXML_VALUE_EXPR ? expression_attribute(e, "expr", &value->as.expr)
                                                    : content_body(e, value, construct);
    }
    return result == TA_OK ? end(e) : result;
}

static ta_result params_elements(encoder *e, const ta_scxml_param_spec *params) {
    for (uint32_t index = 0; params != NULL && params[index].name.ptr != NULL; index += 1u) {
        const ta_scxml_param_spec *param = &params[index];
        ta_result result = start(e, "param");
        if (result == TA_OK) {
            result = attribute(e, "name", param->name);
        }
        if (result == TA_OK) {
            if (param->value.kind == TA_SCXML_VALUE_EXPR) {
                result = expression_attribute(e, "expr", &param->value.as.expr);
            } else if (param->value.kind == TA_SCXML_VALUE_LOCATION) {
                result = expression_attribute(e, "location", &param->value.as.location);
            } else {
                result = refuse(e, "param", "a param needs expr or location");
            }
        }
        if (result == TA_OK) {
            result = end(e);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

static ta_result actions(encoder *e, const ta_scxml_action_spec *list);

/* The rest of an <if> after its then-branch: an else branch that is exactly
 * one <if> is written back as <elseif>. */
static ta_result else_branch(encoder *e, const ta_scxml_action_spec *list) {
    if (list == NULL || list[0].kind == TA_SCXML_ACTION_NONE) {
        return TA_OK;
    }
    if (list[0].kind == TA_SCXML_ACTION_IF && list[1].kind == TA_SCXML_ACTION_NONE) {
        uint64_t at = 0;
        ta_result result = start_at(e, "elseif", &at);
        if (result == TA_OK) {
            result = expression_attribute(e, "cond", &list[0].as.conditional.condition);
        }
        if (result == TA_OK) {
            result = end_landed(e, list[0].span, at);
        }
        if (result == TA_OK) {
            result = actions(e, list[0].as.conditional.then_actions);
        }
        return result == TA_OK ? else_branch(e, list[0].as.conditional.else_actions) : result;
    }
    ta_result result = start(e, "else");
    if (result == TA_OK) {
        result = end(e);
    }
    return result == TA_OK ? actions(e, list) : result;
}

static ta_result action(encoder *e, const ta_scxml_action_spec *spec) {
    ta_result result = TA_OK;
    uint64_t at = output_offset(&e->out);
    switch (spec->kind) {
        case TA_SCXML_ACTION_RAISE:
            if (spec->as.raise.data.kind != TA_SCXML_VALUE_NONE) {
                return refuse(e, "raise", "<raise> carries no data");
            }
            result = start_at(e, "raise", &at);
            if (result == TA_OK) {
                result = attribute(e, "event", spec->as.raise.event);
            }
            break;
        case TA_SCXML_ACTION_SEND: {
            const ta_scxml_send_spec *send = &spec->as.send;
            result = start_at(e, "send", &at);
            if (result == TA_OK) {
                if (send->event.kind == TA_SCXML_EVENT_NAME) {
                    result = attribute(e, "event", send->event.as.name);
                } else if (send->event.kind == TA_SCXML_EVENT_EXPR) {
                    result = expression_attribute(e, "eventexpr", &send->event.as.expr);
                }
            }
            if (result == TA_OK) {
                result = string_attribute(e, "target", "targetexpr", &send->target);
            }
            if (result == TA_OK) {
                result = string_attribute(e, "type", "typeexpr", &send->type);
            }
            if (result == TA_OK) {
                result = id_attribute(e, &send->id);
            }
            if (result == TA_OK) {
                result = string_attribute(e, "delay", "delayexpr", &send->delay);
            }
            if (result == TA_OK) {
                result = params_elements(e, send->params);
            }
            if (result == TA_OK) {
                result = content_element(e, &send->data, "send");
            }
            break;
        }
        case TA_SCXML_ACTION_CANCEL:
            result = start_at(e, "cancel", &at);
            if (result == TA_OK) {
                result = string_attribute(e, "sendid", "sendidexpr", &spec->as.cancel.sendid);
            }
            break;
        case TA_SCXML_ACTION_ASSIGN:
            result = start_at(e, "assign", &at);
            if (result == TA_OK) {
                result = expression_attribute(e, "location", &spec->as.assign.location);
            }
            if (result == TA_OK) {
                const ta_scxml_value_spec *value = &spec->as.assign.value;
                if (value->kind == TA_SCXML_VALUE_EXPR) {
                    result = expression_attribute(e, "expr", &value->as.expr);
                } else if (value->kind == TA_SCXML_VALUE_CONTENT) {
                    result = content_body(e, value, "assign");
                } else {
                    result = refuse(e, "assign", "<assign> takes expr or a body");
                }
            }
            break;
        case TA_SCXML_ACTION_SCRIPT:
            result = start_at(e, "script", &at);
            if (result == TA_OK) {
                result = expression_body(e, &spec->as.script.script, "script");
            }
            break;
        case TA_SCXML_ACTION_LOG:
            result = start_at(e, "log", &at);
            if (result == TA_OK && spec->as.log.label.ptr != NULL) {
                result = attribute(e, "label", spec->as.log.label);
            }
            if (result == TA_OK) {
                result = expression_attribute(e, "expr", &spec->as.log.expr);
            }
            break;
        case TA_SCXML_ACTION_IF:
            result = start_at(e, "if", &at);
            if (result == TA_OK) {
                result = expression_attribute(e, "cond", &spec->as.conditional.condition);
            }
            if (result == TA_OK) {
                result = actions(e, spec->as.conditional.then_actions);
            }
            if (result == TA_OK) {
                result = else_branch(e, spec->as.conditional.else_actions);
            }
            break;
        case TA_SCXML_ACTION_FOREACH:
            result = start_at(e, "foreach", &at);
            if (result == TA_OK) {
                result = expression_attribute(e, "array", &spec->as.foreach.array);
            }
            if (result == TA_OK) {
                result = attribute(e, "item", spec->as.foreach.item);
            }
            if (result == TA_OK && spec->as.foreach.index.ptr != NULL) {
                result = attribute(e, "index", spec->as.foreach.index);
            }
            if (result == TA_OK) {
                result = actions(e, spec->as.foreach.body);
            }
            break;
        default:
            return refuse(e, "action", "no SCXML form for this action");
    }
    return result == TA_OK ? end_landed(e, spec->span, at) : result;
}

static ta_result actions(encoder *e, const ta_scxml_action_spec *list) {
    for (uint32_t index = 0; list != NULL && list[index].kind != TA_SCXML_ACTION_NONE; index += 1u) {
        ta_result result = action(e, &list[index]);
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

static ta_result datamodel(encoder *e, const ta_scxml_data_spec *data) {
    if (data == NULL || data[0].name.ptr == NULL) {
        return TA_OK;
    }
    ta_result result = start(e, "datamodel");
    for (uint32_t index = 0; result == TA_OK && data[index].name.ptr != NULL; index += 1u) {
        const ta_scxml_value_spec *value = &data[index].initializer;
        result = start(e, "data");
        if (result == TA_OK) {
            result = attribute(e, "id", data[index].name);
        }
        if (result == TA_OK) {
            if (value->kind == TA_SCXML_VALUE_EXPR) {
                result = expression_attribute(e, "expr", &value->as.expr);
            } else if (value->kind == TA_SCXML_VALUE_CONTENT) {
                result = content_body(e, value, "data");
            } else if (value->kind == TA_SCXML_VALUE_LOCATION) {
                result = refuse(e, "data", "a location as a data initializer");
            }
        }
        if (result == TA_OK) {
            result = end(e);
        }
    }
    return result == TA_OK ? end(e) : result;
}

static ta_result transition(encoder *e, const ta_scxml_transition_spec *spec) {
    uint64_t at = 0;
    ta_result result = start_at(e, "transition", &at);
    if (result == TA_OK && spec->event.ptr != NULL) {
        result = attribute(e, "event", spec->event);
    }
    if (result == TA_OK) {
        result = expression_attribute(e, "cond", &spec->condition);
    }
    if (result == TA_OK) {
        result = list_attribute(e, "target", spec->targets);
    }
    if (result == TA_OK && spec->kind == TA_SCXML_TRANSITION_INTERNAL) {
        result = attribute(e, "type", cstr("internal"));
    }
    if (result == TA_OK) {
        result = actions(e, spec->actions);
    }
    return result == TA_OK ? end_landed(e, spec->span, at) : result;
}

static ta_result handlers(encoder *e, const char *local, const ta_scxml_handler_spec *list) {
    for (uint32_t index = 0; list != NULL && list[index].actions != NULL; index += 1u) {
        ta_result result = start(e, local);
        if (result == TA_OK) {
            result = actions(e, list[index].actions);
        }
        if (result == TA_OK) {
            result = end(e);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

static ta_result invoke(encoder *e, const ta_scxml_invoke_spec *spec) {
    ta_result result = start(e, "invoke");
    if (result == TA_OK) {
        result = string_attribute(e, "type", "typeexpr", &spec->type);
    }
    if (result == TA_OK) {
        result = string_attribute(e, "src", "srcexpr", &spec->src);
    }
    if (result == TA_OK) {
        result = id_attribute(e, &spec->id);
    }
    if (result == TA_OK && spec->autoforward) {
        result = attribute(e, "autoforward", cstr("true"));
    }
    if (result == TA_OK) {
        result = params_elements(e, spec->params);
    }
    if (result == TA_OK) {
        result = content_element(e, &spec->data, "invoke");
    }
    if (result == TA_OK && spec->finalize != NULL) {
        result = start(e, "finalize");
        if (result == TA_OK) {
            result = actions(e, spec->finalize);
        }
        if (result == TA_OK) {
            result = end(e);
        }
    }
    return result == TA_OK ? end(e) : result;
}

static ta_result states(encoder *e, const ta_scxml_state_spec *list);

static ta_result state(encoder *e, const ta_scxml_state_spec *spec) {
    const char *local = "state";
    switch (spec->kind) {
        case TA_SCXML_STATE_ATOMIC:
        case TA_SCXML_STATE_COMPOUND:
            break;
        case TA_SCXML_STATE_PARALLEL:
            local = "parallel";
            break;
        case TA_SCXML_STATE_FINAL:
            local = "final";
            break;
        case TA_SCXML_STATE_HISTORY_SHALLOW:
        case TA_SCXML_STATE_HISTORY_DEEP:
            local = "history";
            break;
        default:
            return refuse(e, "state", "an unknown kind of state");
    }
    uint64_t at = 0;
    ta_result result = start_at(e, local, &at);
    if (result == TA_OK) {
        result = attribute(e, "id", spec->id);
    }
    if (result == TA_OK && spec->kind == TA_SCXML_STATE_HISTORY_DEEP) {
        result = attribute(e, "type", cstr("deep"));
    }
    const ta_scxml_initial_spec *initial = spec->initial;
    bool initial_element = initial != NULL && initial->actions != NULL && initial->actions[0].kind != TA_SCXML_ACTION_NONE;
    if (result == TA_OK && initial != NULL && !initial_element) {
        uint64_t before = output_offset(&e->out);
        result = list_attribute(e, "initial", initial->targets);
        /* The attribute's leading space is not part of it. */
        if (result == TA_OK) {
            result = landed_at(e, initial->span, before + 1u);
        }
    }
    uint64_t initial_at = 0;
    if (result == TA_OK && initial_element) {
        result = start_at(e, "initial", &initial_at);
        if (result == TA_OK) {
            result = start(e, "transition");
        }
        if (result == TA_OK) {
            result = list_attribute(e, "target", initial->targets);
        }
        if (result == TA_OK) {
            result = actions(e, initial->actions);
        }
        if (result == TA_OK) {
            result = end(e);
        }
        if (result == TA_OK) {
            result = end_landed(e, initial->span, initial_at);
        }
    }
    if (result == TA_OK) {
        result = datamodel(e, spec->data);
    }
    if (result == TA_OK) {
        result = handlers(e, "onentry", spec->onentry);
    }
    if (result == TA_OK) {
        result = handlers(e, "onexit", spec->onexit);
    }
    for (uint32_t index = 0; result == TA_OK && spec->transitions != NULL &&
                             spec->transitions[index].kind != TA_SCXML_TRANSITION_NONE;
         index += 1u) {
        result = transition(e, &spec->transitions[index]);
    }
    for (uint32_t index = 0; result == TA_OK && spec->invokes != NULL && spec->invokes[index].kind != TA_SCXML_INVOKE_NONE;
         index += 1u) {
        result = invoke(e, &spec->invokes[index]);
    }
    if (result == TA_OK && spec->done_data != NULL) {
        result = start(e, "donedata");
        if (result == TA_OK) {
            result = params_elements(e, spec->done_data->params);
        }
        if (result == TA_OK) {
            result = content_element(e, &spec->done_data->content, "donedata");
        }
        if (result == TA_OK) {
            result = end(e);
        }
    }
    if (result == TA_OK) {
        result = states(e, spec->states);
    }
    return result == TA_OK ? end_landed(e, spec->span, at) : result;
}

static ta_result states(encoder *e, const ta_scxml_state_spec *list) {
    for (uint32_t index = 0; list != NULL && list[index].kind != TA_SCXML_STATE_NONE; index += 1u) {
        ta_result result = state(e, &list[index]);
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

static ta_result encode_spec(encoder *e, const ta_scxml_spec *spec) {
    if (spec->initial != NULL && spec->initial->actions != NULL && spec->initial->actions[0].kind != TA_SCXML_ACTION_NONE) {
        return refuse(e, "scxml", "<scxml> has no <initial> element for initial actions");
    }
    ta_result result = start(e, "scxml");
    if (result == TA_OK) {
        result = attribute(e, "version", cstr("1.0"));
    }
    if (result == TA_OK && spec->name.len != 0) {
        result = attribute(e, "name", spec->name);
    }
    if (result == TA_OK && spec->datamodel.len != 0) {
        result = attribute(e, "datamodel", spec->datamodel);
    }
    if (result == TA_OK && spec->binding != TA_SCXML_BINDING_DEFAULT) {
        result = attribute(e, "binding", cstr(spec->binding == TA_SCXML_BINDING_LATE ? "late" : "early"));
    }
    if (result == TA_OK && spec->initial != NULL) {
        uint64_t before = output_offset(&e->out);
        result = list_attribute(e, "initial", spec->initial->targets);
        if (result == TA_OK) {
            result = landed_at(e, spec->initial->span, before + 1u);
        }
    }
    if (result == TA_OK) {
        result = datamodel(e, spec->data);
    }
    if (result == TA_OK && spec->script.kind != TA_EXPRESSION_NONE) {
        uint64_t at = 0;
        result = start_at(e, "script", &at);
        if (result == TA_OK) {
            result = expression_body(e, &spec->script, "script");
        }
        if (result == TA_OK) {
            result = end_landed(e, spec->script_span, at);
        }
    }
    if (result == TA_OK) {
        result = states(e, spec->states);
    }
    return result == TA_OK ? end(e) : result;
}

/* Where byte `offset` is: the line whose start is the last at or before it. */
static void place(const uint64_t *starts, uint32_t lines, uint64_t offset, uint32_t *line, uint32_t *column) {
    uint32_t low = 0;
    uint32_t high = lines;
    while (high - low > 1u) {
        uint32_t middle = low + (high - low) / 2u;
        if (starts[middle] <= offset) {
            low = middle;
        } else {
            high = middle;
        }
    }
    *line = low + 1u;
    *column = (uint32_t)(offset - starts[low]) + 1u;
}

/* Tells the caller's sink where each element landed, with lines and columns
 * counted as txml counts them in a decode. */
static ta_result report_landings(encoder *e, ta_bytes output) {
    const ta_chart_span_sink *sink = &e->params->spans;
    if (sink->v == NULL || e->landing_count == 0) {
        return TA_OK;
    }
    uint32_t lines = 1;
    for (uint32_t at = 0; at < output.len; at += 1u) {
        lines += output.ptr[at] == '\n' ? 1u : 0u;
    }
    uint64_t *starts = ta_arena_alloc(e->arena, (size_t)lines * sizeof(uint64_t), _Alignof(uint64_t));
    if (starts == NULL) {
        return TA_ERR_NOMEM;
    }
    starts[0] = 0;
    for (uint32_t at = 0, line = 1; at < output.len; at += 1u) {
        if (output.ptr[at] == '\n') {
            starts[line++] = at + 1u;
        }
    }
    for (uint32_t index = 0; index < e->landing_count; index += 1u) {
        const landed *where = &e->landings[index];
        ta_source_span span = { .offset = where->start, .length = (uint32_t)(where->end - where->start) };
        place(starts, lines, where->start, &span.line, &span.column);
        place(starts, lines, where->end, &span.end_line, &span.end_column);
        sink->v->written(sink->self, where->span, &span);
    }
    return TA_OK;
}

static ta_result scxml_encode(void *self, const ta_chart_encode_params *params) {
    const ta_scxml_codec *codec = self;
    if (codec == NULL || params == NULL || params->spec == NULL || params->arena == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    encoder e = { .params = params, .limits = &codec->limits, .arena = params->arena };
    e.nested = ta_arena_alloc(params->arena, (size_t)codec->limits.max_depth + 1u, 1);
    txml_result initialized = output_init(&e.out, params->arena, e.limits, TXML_WRITE_DECLARATION | TXML_WRITE_EMPTY_TAGS);
    if (initialized != TXML_OK || e.nested == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_result result = encode_spec(&e, params->spec);
    if (result == TA_OK) {
        txml_result finished = output_finish(&e.out, params->out);
        result = finished == TXML_OK ? TA_OK : finished == TXML_ERR_NOMEM ? TA_ERR_NOMEM : refuse(&e, "scxml", "unfinished");
    }
    if (result == TA_OK) {
        result = report_landings(&e, *params->out);
    }
    if (result == TA_ERR_INVALID && params->error != NULL) {
        *params->error = e.error;
    }
    return result;
}

/* ========================================================================
 * The codec
 */

static const ta_str EXTENSIONS[] = { { .ptr = "scxml", .len = 5 }, { 0 } };

static const ta_chart_codec_vtable SCXML_CODEC = {
    .info = {
        .name = { .ptr = TA_SCXML_MEDIA_TYPE, .len = (uint32_t)(sizeof(TA_SCXML_MEDIA_TYPE) - 1u) },
        .extensions = EXTENSIONS,
        .description = { .ptr = "SCXML 1.0 documents (XML, over txml)", .len = 36 },
        .flags = TA_CHART_CODEC_DECODES | TA_CHART_CODEC_ENCODES,
    },
    .decode = scxml_decode,
    .encode = scxml_encode,
};

static uint64_t or_default64(uint64_t value, uint64_t fallback) {
    return value != 0 ? value : fallback;
}

static uint32_t or_default32(uint32_t value, uint32_t fallback) {
    return value != 0 ? value : fallback;
}

ta_result ta_scxml_codec_init(ta_scxml_codec *codec, const ta_scxml_codec_params *params) {
    if (codec == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_scxml_codec_params given = params != NULL ? *params : (ta_scxml_codec_params){ 0 };
    ta_scxml_codec_params limits = {
        .max_document_bytes = or_default64(given.max_document_bytes, TA_SCXML_DEFAULT_MAX_DOCUMENT_BYTES),
        .max_parse_bytes = or_default64(given.max_parse_bytes, TA_SCXML_DEFAULT_MAX_PARSE_BYTES),
        .max_depth = or_default32(given.max_depth, TA_SCXML_DEFAULT_MAX_DEPTH),
        .max_attributes = or_default32(given.max_attributes, TA_SCXML_DEFAULT_MAX_ATTRIBUTES),
        .max_text_bytes = or_default32(given.max_text_bytes, TA_SCXML_DEFAULT_MAX_TEXT_BYTES),
        .max_entity_expansion = or_default64(given.max_entity_expansion, TA_SCXML_DEFAULT_MAX_ENTITY_EXPANSION),
        .max_name_length = or_default32(given.max_name_length, TA_SCXML_DEFAULT_MAX_NAME_LENGTH),
        .max_names = or_default32(given.max_names, TA_SCXML_DEFAULT_MAX_NAMES),
    };
    if (limits.max_depth > 65535u || limits.max_parse_bytes > UINT32_MAX) {
        return TA_ERR_INVALID;
    }
    codec->limits = limits;
    return TA_OK;
}

ta_chart_codec ta_scxml_codec_adapter(ta_scxml_codec *codec) {
    return (ta_chart_codec){ .self = codec, .v = &SCXML_CODEC };
}

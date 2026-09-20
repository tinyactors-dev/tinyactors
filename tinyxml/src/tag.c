/*
 * Start tags and end tags.
 *
 * A start tag is a state machine over its own grammar: name, attribute name,
 * '=', quoted value, and back. Names are bounded, so they are copied out of
 * the window as they are read; values stream into pool runs. Nothing about the
 * tag is resolved until '>' because a declaration may follow the attribute
 * that uses it: at that point the namespaces are resolved, ATTLIST defaults are
 * added, names are interned in the order the tests pin, and the element and its
 * attributes are appended together.
 */
#include "parser.h"

static const char XMLNS_URI[] = "http://www.w3.org/2000/xmlns/";
static const char XML_URI[] = "http://www.w3.org/XML/1998/namespace";

/* ------------------------------------------------------------------------
 * Names and bindings.
 */

static bool split_qname(const uint8_t *s, uint32_t length, txml_str *prefix, txml_str *local) {
    uint32_t colon = length;
    for (uint32_t i = 0u; i < length; i++) {
        if (s[i] == ':') {
            if (colon != length) {
                return false;
            }
            colon = i;
        }
    }
    if (colon == length) {
        *prefix = (txml_str){ "", 0u };
        *local = (txml_str){ (const char *)s, length };
        return true;
    }
    if (colon == 0u || colon + 1u == length) {
        return false;
    }
    *prefix = (txml_str){ (const char *)s, colon };
    *local = (txml_str){ (const char *)s + colon + 1u, length - colon - 1u };
    return true;
}

static const binding *lookup(const txml_parser *p, txml_str prefix) {
    for (const binding *b = p->bindings; b != NULL; b = b->prev) {
        if (txml_str_equal(b->prefix, prefix)) {
            return b;
        }
    }
    return NULL;
}

static scan push_binding(txml_parser *p, txml_str prefix, uint32_t namespace_id) {
    binding *b = (binding *)txml_arena_alloc(&p->stack_arena, sizeof(binding), _Alignof(binding));
    char *copy = prefix.length == 0u ? NULL : (char *)txml_arena_alloc(&p->stack_arena, prefix.length, 1u);
    if (b == NULL || (prefix.length != 0u && copy == NULL)) {
        return SCAN_NOMEM;
    }
    for (uint32_t i = 0u; i < prefix.length; i++) {
        copy[i] = prefix.ptr[i];
    }
    b->prev = p->bindings;
    b->prefix = (txml_str){ copy == NULL ? "" : copy, prefix.length };
    b->namespace_id = namespace_id;
    p->bindings = b;
    return SCAN_OK;
}

typedef struct {
    txml_str prefix;
    bool attribute;
    txml_position at;
} name_ref;

/* Resolve a written name to a namespace ID. */
static scan resolve(txml_parser *p, const name_ref *name, uint32_t *namespace_id) {
    if (name->prefix.length == 0u) {
        const binding *b = name->attribute ? NULL : lookup(p, name->prefix);
        *namespace_id = b == NULL ? 0u : b->namespace_id;
        return SCAN_OK;
    }
    if (txml_str_equal(name->prefix, TXML_STR("xmlns"))) {
        return txp_fail(p, PROBLEM(TXML_DOC_UNDECLARED_PREFIX, "the xmlns prefix is reserved"), name->at);
    }
    const binding *b = lookup(p, name->prefix);
    if (b != NULL) {
        *namespace_id = b->namespace_id;
        return SCAN_OK;
    }
    if (txml_str_equal(name->prefix, TXML_STR("xml"))) {
        const txml_result result = txml_doc_intern_namespace(p->doc, TXML_STR(XML_URI), namespace_id);
        return result == TXML_OK ? SCAN_OK : txp_fail_result(p, result, name->at);
    }
    return txp_fail(p, PROBLEM(TXML_DOC_UNDECLARED_PREFIX, "undeclared namespace prefix"), name->at);
}

/* ------------------------------------------------------------------------
 * Attribute values.
 */

/* Decide where a value's decoded bytes go. A namespace declaration is
 * assembled whole so it can be interned; anything else streams into the pool. */
static void begin_value(txml_parser *p, attribute_record *record) {
    const bool bare = record->prefix.length == 0u && txml_str_equal(record->local, TXML_STR("xmlns"));
    const bool prefixed = txml_str_equal(record->prefix, TXML_STR("xmlns"));

    record->declaration = bare || prefixed;
    txp_dec_reset(p, DECODE_ATTRIBUTE);
    if (record->declaration) {
        p->small.length = 0u;
        p->sink = &txp_sink_buffer;
    } else {
        txml_doc_run_begin(p->doc, &p->run);
        p->sink = &txp_sink_run;
    }
}

/* The value is complete. A declaration is stored once, as the interned
 * namespace URI, and binds its prefix; anything else is committed to the
 * pool. Safe to retry after SCAN_NOMEM. */
static scan finish_value(txml_parser *p, attribute_record *record) {
    if (!record->declaration) {
        txml_doc_run_finish(p->doc, &p->run);
        record->value_offset = p->run.offset;
        record->value_length = p->run.length;
        return SCAN_OK;
    }

    const bool bare = record->prefix.length == 0u;
    const txml_str declared = bare ? (txml_str){ "", 0u } : record->local;
    const txml_str value = { (const char *)p->small.data, p->small.length };

    /* Namespaces in XML 1.0 §3: the xmlns prefix is never declared; the xml
     * prefix is declared only as its own namespace; and neither reserved
     * namespace is bound to any other prefix or to the default. */
    const bool is_xml_uri = txml_str_equal(value, TXML_STR(XML_URI));
    const bool is_xmlns_uri = txml_str_equal(value, TXML_STR(XMLNS_URI));

    if (!bare && txml_str_equal(declared, TXML_STR("xmlns"))) {
        return txp_fail(p, PROBLEM(TXML_DOC_UNDECLARED_PREFIX, "the xmlns prefix cannot be declared"), record->position);
    }
    if (!bare && txml_str_equal(declared, TXML_STR("xml"))) {
        if (!is_xml_uri) {
            return txp_fail(p, PROBLEM(TXML_DOC_UNDECLARED_PREFIX, "the xml prefix can only be bound to the XML namespace"),
                            record->position);
        }
    } else if (is_xml_uri) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "the XML namespace can only be bound to the xml prefix"), record->position);
    }
    if (is_xmlns_uri) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "the xmlns namespace cannot be bound"), record->position);
    }
    if (!bare && value.length == 0u) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "a prefix cannot be bound to the empty namespace"), record->position);
    }
    uint32_t namespace_id = 0u;
    const txml_result result = txml_doc_intern_namespace(p->doc, value, &namespace_id);
    if (result != TXML_OK) {
        return txp_fail_result(p, result, record->position);
    }
    const txml_namespace_entry *entry = txml_doc_namespace(p->doc, namespace_id);
    record->value_offset = entry->uri_offset;
    record->value_length = entry->uri_length;
    return push_binding(p, declared, namespace_id);
}

/* Add the ATTLIST defaults of the element that the tag did not write. */
static scan add_defaults(txml_parser *p) {
    tag_work *t = &p->tag;
    const element_defaults *list = txp_find_element_defaults(p, t->raw_name);

    if (list == NULL) {
        return SCAN_OK;
    }
    for (const attlist_default *d = list->first; d != NULL; d = d->next) {
        txml_str prefix, local;
        if (!split_qname((const uint8_t *)d->name.ptr, d->name.length, &prefix, &local)) {
            return txp_fail(p, PROBLEM(TXML_DOC_NAME, "not a legal qualified name"), p->construct);
        }
        bool written = false;
        for (uint32_t k = 0u; k < t->count; k++) {
            if (txml_str_equal(prefix, p->records[k].prefix) && txml_str_equal(local, p->records[k].local)) {
                written = true;
                break;
            }
        }
        if (written) {
            continue;
        }
        if (t->count >= p->limits.max_attributes) {
            return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_ATTRIBUTES, "too many attributes"), p->construct);
        }

        attribute_record *record = &p->records[t->count];
        record->prefix = prefix;
        record->local = local;
        record->position = p->construct;
        record->defaulted = true;
        begin_value(p, record);

        uint32_t consumed = 0u;
        scan outcome = txp_dec_feed(p, &(dec_input){ (const uint8_t *)d->value.ptr, d->value.length }, &consumed);
        if (outcome == SCAN_OK) {
            outcome = txp_dec_end(p);
        }
        if (outcome == SCAN_OK) {
            outcome = finish_value(p, record);
        }
        if (outcome != SCAN_OK) {
            return outcome;
        }
        t->count += 1u;
    }
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * The end of a start tag.
 */

static scan finish_tag(txml_parser *p) {
    tag_work *t = &p->tag;
    scan outcome;

    /* A retry after running out of memory starts from the written attributes. */
    if (!t->finishing) {
        t->finishing = true;
        t->written = t->count;
        t->written_bindings = p->bindings;
        t->written_mark = txml_arena_position(&p->stack_arena);
    } else {
        t->count = t->written;
        p->bindings = t->written_bindings;
        txml_arena_release(&p->stack_arena, t->written_mark);
        txp_rollback_nodes(p, &t->before);
    }

    outcome = add_defaults(p);
    if (outcome != SCAN_OK) {
        return outcome;
    }

    /* Resolve the element, then each attribute in order: written first, then
     * defaults. Interning order is part of the observable name table. */
    uint32_t element_namespace = 0u;
    uint32_t element_name = 0u;
    outcome = resolve(p, &(name_ref){ t->element_prefix, false, t->name_position }, &element_namespace);
    if (outcome == SCAN_OK) {
        const intern_request request = { { element_namespace, t->element_local, t->element_prefix }, t->name_position };
        outcome = txp_intern(p, &request, &element_name);
    }
    if (outcome != SCAN_OK) {
        return outcome;
    }
    for (uint32_t k = 0u; k < t->count; k++) {
        attribute_record *record = &p->records[k];
        uint32_t namespace_id = 0u;

        if (record->declaration) {
            const txml_result result = txml_doc_intern_namespace(p->doc, TXML_STR(XMLNS_URI), &namespace_id);
            if (result != TXML_OK) {
                return txp_fail_result(p, result, record->position);
            }
        } else {
            outcome = resolve(p, &(name_ref){ record->prefix, true, record->position }, &namespace_id);
            if (outcome != SCAN_OK) {
                return outcome;
            }
        }
        const intern_request request = { { namespace_id, record->local, record->prefix }, record->position };
        outcome = txp_intern(p, &request, &record->name_id);
        if (outcome != SCAN_OK) {
            return outcome;
        }
        for (uint32_t earlier = 0u; earlier < k; earlier++) {
            if (p->records[earlier].name_id == record->name_id) {
                return txp_fail(p, PROBLEM(TXML_DOC_DUPLICATE_ATTRIBUTE, "duplicate attribute"), record->position);
            }
        }
    }

    const uint32_t index = p->doc->nodes.count;
    const txml_node_append append = {
        .node = {
            .kind = TXML_NODE_ELEMENT, .flags = t->empty ? TXML_NODE_EMPTY : TXML_NODE_OPEN,
            .depth = (uint16_t)(p->depth + 1u), .parent = txp_parent_node(p), .end = t->empty ? index + 1u : TXML_INDEX_NONE,
            .name = element_name, .as.element = { p->doc->attributes.count, t->count },
        },
        .position = p->construct,
    };
    uint32_t appended_index = 0u;
    txml_result result = txml_doc_append_node(p->doc, &append, &appended_index);
    if (result == TXML_OK) {
        result = txml_doc_record_prefix(p->doc, &(txml_prefix_request){ false, appended_index, t->element_prefix });
    }
    for (uint32_t k = 0u; result == TXML_OK && k < t->count; k++) {
        uint32_t name = p->records[k].name_id;
        name |= k + 1u == t->count ? TXML_ATTRIBUTE_LAST : 0u;
        name |= p->records[k].defaulted ? TXML_ATTRIBUTE_DEFAULTED : 0u;
        const txml_attribute_entry entry = { name, p->records[k].value_offset, p->records[k].value_length };
        uint32_t attribute_index = 0u;
        result = txml_doc_append_attribute(p->doc, &entry, &attribute_index);
        if (result == TXML_OK) {
            result = txml_doc_record_prefix(p->doc, &(txml_prefix_request){ true, attribute_index, p->records[k].prefix });
        }
    }
    if (result != TXML_OK) {
        txp_rollback_nodes(p, &t->before);
        return txp_fail_result(p, result, p->construct);
    }

    if (t->empty) {
        p->bindings = t->before.bindings;
        txml_arena_release(&p->stack_arena, t->before.mark);
        if (p->depth == 0u) {
            p->seen_root = true;
            p->stage = STAGE_EPILOG;
        }
    } else {
        p->stack[p->depth] = (open_element){ index, t->before.bindings, t->before.mark, t->raw_name };
        p->depth += 1u;
        p->stage = STAGE_CONTENT;
    }
    txml_arena_release(&p->tag_arena, t->tag_mark);
    p->appended += 1u;
    return txp_check_budget(p, p->construct);
}

/* ------------------------------------------------------------------------
 * Names within a tag.
 */

static scan finish_element_name(txml_parser *p) {
    tag_work *t = &p->tag;
    char *copy = (char *)txml_arena_alloc(&p->stack_arena, p->name_length, 1u);

    if (copy == NULL) {
        return SCAN_NOMEM;
    }
    for (uint32_t i = 0u; i < p->name_length; i++) {
        copy[i] = (char)p->name[i];
    }
    t->raw_name = (txml_str){ copy, p->name_length };
    if (!split_qname((const uint8_t *)copy, p->name_length, &t->element_prefix, &t->element_local)) {
        return txp_fail(p, PROBLEM(TXML_DOC_NAME, "not a legal qualified name"), p->name_position);
    }
    t->name_position = p->name_position;
    t->phase = TP_SPACE;
    t->seen_space = false;
    return SCAN_OK;
}

static scan finish_attribute_name(txml_parser *p) {
    tag_work *t = &p->tag;
    char *copy = (char *)txml_arena_alloc(&p->tag_arena, p->name_length, 1u);

    if (copy == NULL) {
        return SCAN_NOMEM;
    }
    for (uint32_t i = 0u; i < p->name_length; i++) {
        copy[i] = (char)p->name[i];
    }
    attribute_record *record = &p->records[t->count];
    record->defaulted = false;
    record->declaration = false;
    record->position = p->name_position;
    if (!split_qname((const uint8_t *)copy, p->name_length, &record->prefix, &record->local)) {
        return txp_fail(p, PROBLEM(TXML_DOC_NAME, "not a legal qualified name"), p->name_position);
    }
    t->phase = TP_AFTER_NAME;
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * The start-tag state.
 */

scan txp_begin_start_tag(txml_parser *p) {
    const uint8_t b = p->window[p->start];
    tag_work *t = &p->tag;

    if (p->stage == STAGE_EPILOG) {
        return txp_fail(p, PROBLEM(TXML_DOC_TRAILING_CONTENT, "a second document element"), p->construct);
    }
    if (p->depth >= p->limits.max_depth) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_DEPTH, "elements nested too deeply"), p->construct);
    }
    uint32_t width = 0u;
    if (!txp_name_start_at(p->window + p->start, &width)) {
        if (b >= 0x80u || txp_name_char_at(p->window + p->start, &width)) {
            return txp_fail(p, PROBLEM(TXML_DOC_NAME, "a name cannot start with this character"), txp_position_now(p));
        }
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "'<' does not start markup here"), p->construct);
    }

    t->phase = TP_ELEMENT_NAME;
    t->seen_space = false;
    t->empty = false;
    t->count = 0u;
    t->finishing = false;
    t->before = txp_snapshot(p);
    t->tag_mark = txml_arena_position(&p->tag_arena);
    p->name_length = 0u;
    p->name_position = txp_position_now(p);
    p->state = &txp_state_start_tag;
    return SCAN_OK;
}

static scan start_tag_run(txml_parser *p) {
    tag_work *t = &p->tag;
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;
    scan outcome;

    while (i < limit) {
        const uint8_t b = w[i];
        uint32_t width = 0u;
        bool finish = false;

        switch (t->phase) {
        case TP_ELEMENT_NAME:
        case TP_ATTRIBUTE_NAME:
            if (txp_name_char_at(w + i, &width)) {
                if (p->name_length + width > p->limits.max_name_length) {
                    return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_NAME_LENGTH, "name too long"), p->name_position);
                }
                for (uint32_t k = 0u; k < width; k++) {
                    p->name[p->name_length++] = w[i + k];
                }
                i += width;
                break;
            }
            if (b >= 0x80u) {
                txp_commit(p, i);
                return txp_fail_name_char(p, i);
            }
            /* The name is complete; the delimiter is read again in the next phase. */
            outcome = t->phase == TP_ELEMENT_NAME ? finish_element_name(p) : finish_attribute_name(p);
            if (outcome != SCAN_OK) {
                txp_commit(p, i);
                return outcome;
            }
            break;

        case TP_SPACE:
            if (txp_is_space(b)) {
                t->seen_space = true;
                i += 1u;
            } else if (b == '>') {
                finish = true;
            } else if (b == '/') {
                txp_commit(p, i);
                t->slash_position = txp_position_now(p);
                t->phase = TP_SLASH;
                i += 1u;
            } else if (!t->seen_space) {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "attributes must be separated by whitespace"), txp_position_at(p, i));
            } else if (!txp_name_start_at(w + i, &width)) {
                if (b >= 0x80u) {
                    return txp_fail_name_char(p, i);
                }
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected an attribute name"), txp_position_at(p, i));
            } else if (t->count >= p->limits.max_attributes) {
                return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_ATTRIBUTES, "too many attributes"), txp_position_at(p, i));
            } else {
                txp_commit(p, i);
                p->name_length = 0u;
                p->name_position = txp_position_now(p);
                t->phase = TP_ATTRIBUTE_NAME;
            }
            break;

        case TP_SLASH:
            if (b != '>') {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected an attribute name"), t->slash_position);
            }
            t->empty = true;
            finish = true;
            break;

        case TP_AFTER_NAME:
            if (txp_is_space(b)) {
                i += 1u;
            } else if (b == '=') {
                t->phase = TP_BEFORE_VALUE;
                i += 1u;
            } else {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected '=' after an attribute name"), txp_position_at(p, i));
            }
            break;

        case TP_BEFORE_VALUE:
            if (txp_is_space(b)) {
                i += 1u;
            } else if (b == '"' || b == '\'') {
                begin_value(p, &p->records[t->count]);
                t->quote = b;
                t->phase = TP_VALUE;
                i += 1u;
            } else {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "attribute values must be quoted"), txp_position_at(p, i));
            }
            break;

        default: { /* TP_VALUE */
            uint32_t j = i;
            while (j < limit && w[j] != t->quote) {
                j += 1u;
            }
            if (j > i) {
                uint32_t consumed = 0u;
                outcome = txp_dec_feed(p, &(dec_input){ w + i, j - i }, &consumed);
                txp_commit(p, i + consumed);
                if (outcome != SCAN_OK || consumed < j - i) {
                    return outcome;
                }
            }
            i = j;
            if (i < limit) { /* the closing quote */
                outcome = txp_dec_end(p);
                if (outcome == SCAN_OK) {
                    outcome = finish_value(p, &p->records[t->count]);
                }
                if (outcome != SCAN_OK) {
                    txp_commit(p, i);
                    return outcome;
                }
                t->count += 1u;
                t->phase = TP_SPACE;
                t->seen_space = false;
                i += 1u;
            }
            break;
        }
        }

        if (finish) {
            outcome = finish_tag(p);
            if (outcome != SCAN_OK) {
                txp_commit(p, i);
                return outcome;
            }
            txp_commit(p, i + 1u);
            p->state = &txp_state_content;
            return SCAN_OK;
        }
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_start_tag = { "start-tag", start_tag_run, txp_eof_in_markup };

/* ------------------------------------------------------------------------
 * End tags. The name is compared with the open element's as it arrives, so
 * nothing is buffered.
 */

void txp_begin_end_tag(txml_parser *p) {
    p->end_tag.phase = 0u;
    p->end_tag.matched = 0u;
    p->state = &txp_state_end_tag;
}

static scan end_tag_run(txml_parser *p) {
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;

    while (i < limit) {
        const uint8_t b = w[i];

        uint32_t width = 0u;
        if (p->end_tag.phase == 0u) {
            if (!txp_name_start_at(w + i, &width)) {
                if (b >= 0x80u) {
                    return txp_fail_name_char(p, i);
                }
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected an element name"), txp_position_at(p, i));
            }
            if (p->depth == 0u) {
                return txp_fail(p, PROBLEM(TXML_DOC_TAG_MISMATCH, "end tag with no open element"), p->construct);
            }
            p->end_tag.phase = 1u;
        } else if (p->end_tag.phase == 1u) {
            const txml_str raw = p->stack[p->depth - 1u].raw_name;
            if (txp_name_char_at(w + i, &width)) {
                for (uint32_t k = 0u; k < width; k++) {
                    if (p->end_tag.matched >= raw.length || (uint8_t)raw.ptr[p->end_tag.matched] != w[i + k]) {
                        return txp_fail(p, PROBLEM(TXML_DOC_TAG_MISMATCH, "end tag does not match the open element"),
                                        p->construct);
                    }
                    p->end_tag.matched += 1u;
                }
                i += width;
            } else if (b >= 0x80u) {
                return txp_fail_name_char(p, i);
            } else {
                if (p->end_tag.matched != raw.length) {
                    return txp_fail(p, PROBLEM(TXML_DOC_TAG_MISMATCH, "end tag does not match the open element"), p->construct);
                }
                p->end_tag.junk_position = txp_position_at(p, i);
                p->end_tag.phase = 2u;
            }
        } else if (txp_is_space(b)) {
            i += 1u;
        } else if (b == '>') {
            open_element *top = &p->stack[p->depth - 1u];
            txml_doc_close_node(p->doc, top->node, p->doc->nodes.count);
            p->bindings = top->bindings;
            txml_arena_release(&p->stack_arena, top->mark);
            p->depth -= 1u;
            if (p->depth == 0u) {
                p->seen_root = true;
                p->stage = STAGE_EPILOG;
            }
            txp_commit(p, i + 1u);
            p->state = &txp_state_content;
            return SCAN_OK;
        } else {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "junk in an end tag"), p->end_tag.junk_position);
        }
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_end_tag = { "end-tag", end_tag_run, txp_eof_in_markup };

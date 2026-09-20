/*
 * The DOCTYPE and its internal subset.
 *
 * The declaration's text streams into a pool run exactly as written, while a
 * second view of the same bytes, a small state machine, finds what matters in
 * them: the internal subset, and within it the ENTITY and ATTLIST declarations,
 * which are assembled whole in the small buffer and read from there because
 * they are kept anyway. Comments, processing instructions and the other
 * declarations stream past without being held.
 *
 * Every byte is interpreted before any of it is emitted, and the run is
 * reserved first, so an allocation that fails can only fail before a byte is
 * consumed.
 */
#include "parser.h"

/* ------------------------------------------------------------------------
 * Reading a collected declaration. The buffer holds the bytes after the
 * keyword, through the closing '>'.
 */

/* Advance past a declaration's closing '>' outside quotes. */
static bool skip_declaration(const uint8_t *w, uint32_t *i, uint32_t end) {
    uint8_t quote = 0u;
    for (; *i < end; *i += 1u) {
        const uint8_t b = w[*i];
        if (quote != 0u) {
            if (b == quote) {
                quote = 0u;
            }
        } else if (b == '"' || b == '\'') {
            quote = b;
        } else if (b == '>') {
            *i += 1u;
            return true;
        }
    }
    return false;
}

/* A quoted literal at *i; on success `literal` is its contents. */
typedef struct {
    const uint8_t *bytes;
    uint32_t end;
} source_range;

static bool read_literal(const source_range *range, uint32_t *i, txml_str *literal) {
    const uint8_t *w = range->bytes;
    const uint32_t end = range->end;
    if (*i >= end || (w[*i] != '"' && w[*i] != '\'')) {
        return false;
    }
    const uint8_t quote = w[*i];
    uint32_t stop = *i + 1u;
    while (stop < end && w[stop] != quote) {
        stop += 1u;
    }
    if (stop >= end) {
        return false;
    }
    *literal = (txml_str){ (const char *)w + *i + 1u, stop - *i - 1u };
    *i = stop + 1u;
    return true;
}

/* A name in a collected declaration stopped at `at`: a non-ASCII character
 * there is an illegal name character, since only ASCII delimits markup. */
static scan bad_name_char(txml_parser *p, const uint8_t *w, uint32_t at) {
    if (at < p->small.length && w[at] >= 0x80u) {
        return txp_fail(p, PROBLEM(TXML_DOC_NAME, "not a legal name character"), txp_small_position_at(p, at));
    }
    return SCAN_OK;
}

/* <!ENTITY name "value"> */
static scan read_entity(txml_parser *p) {
    const uint8_t *w = p->small.data;
    const uint32_t end = p->small.length;
    uint32_t j = txp_skip_space(w, 0u, end);

    if (j == 0u) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected whitespace after ENTITY"), txp_small_position_at(p, 0u));
    }
    if (j < end && w[j] == '%') { /* parameter entities are read over, never expanded */
        uint32_t i = 0u;
        return skip_declaration(w, &i, end) ? SCAN_OK
                                            : txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "unterminated declaration"), txp_small_position_at(p, j));
    }
    const uint32_t name_end = txp_scan_name(w, j, end);
    scan named = bad_name_char(p, w, name_end);
    if (named != SCAN_OK) {
        return named;
    }
    if (name_end == j) {
        return txp_fail(p, PROBLEM(TXML_DOC_NAME, "expected an entity name"), txp_small_position_at(p, j));
    }
    const txml_str name = { (const char *)w + j, name_end - j };
    j = txp_skip_space(w, name_end, end);
    txml_str value;
    if (j < end && read_literal(&(source_range){ w, end }, &j, &value)) {
        const scan outcome = txp_add_entity(p, name, value);
        if (outcome != SCAN_OK) {
            return outcome;
        }
    }
    /* External entities are recorded nowhere and never fetched. */
    uint32_t i = j;
    return skip_declaration(w, &i, end) ? SCAN_OK
                                        : txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "unterminated declaration"), txp_small_position_at(p, j));
}

/* <!ATTLIST element (name type default)* > */
static scan read_attlist(txml_parser *p) {
    const uint8_t *w = p->small.data;
    const uint32_t end = p->small.length;
    uint32_t j = txp_skip_space(w, 0u, end);
    const uint32_t element_end = txp_scan_name(w, j, end);
    scan named = bad_name_char(p, w, element_end);

    if (named != SCAN_OK) {
        return named;
    }
    if (j == 0u || element_end == j) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected an element name in ATTLIST"), txp_small_position_at(p, j));
    }
    const txml_str element = { (const char *)w + j, element_end - j };
    j = element_end;

    for (;;) {
        j = txp_skip_space(w, j, end);
        if (j >= end) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "unterminated ATTLIST"), txp_small_position_at(p, j));
        }
        if (w[j] == '>') {
            return SCAN_OK;
        }
        const uint32_t name_end = txp_scan_name(w, j, end);
        named = bad_name_char(p, w, name_end);
        if (named != SCAN_OK) {
            return named;
        }
        if (name_end == j) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected an attribute name in ATTLIST"), txp_small_position_at(p, j));
        }
        const txml_str name = { (const char *)w + j, name_end - j };
        j = txp_skip_space(w, name_end, end);

        /* The type: a keyword, or an enumeration in parentheses. */
        if (j < end && w[j] == '(') {
            while (j < end && w[j] != ')') {
                j += 1u;
            }
            j += 1u;
        } else {
            const uint32_t type_end = txp_scan_name(w, j, end);
            named = bad_name_char(p, w, type_end);
            if (named != SCAN_OK) {
                return named;
            }
            if (type_end == j) {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected an attribute type"), txp_small_position_at(p, j));
            }
            const bool notation = txp_bytes_are(w + j, type_end - j, "NOTATION");
            j = type_end;
            if (notation) {
                j = txp_skip_space(w, j, end);
                while (j < end && w[j] != ')') {
                    j += 1u;
                }
                j += 1u;
            }
        }
        j = txp_skip_space(w, j, end);

        txml_str value;
        if (j < end && w[j] == '#') {
            const uint32_t keyword_end = txp_scan_name(w, j + 1u, end);
            named = bad_name_char(p, w, keyword_end);
            if (named != SCAN_OK) {
                return named;
            }
            const bool fixed = txp_bytes_are(w + j + 1u, keyword_end - j - 1u, "FIXED");
            j = keyword_end;
            if (!fixed) {
                continue; /* #REQUIRED and #IMPLIED supply no value */
            }
            j = txp_skip_space(w, j, end);
        }
        if (j >= end || !read_literal(&(source_range){ w, end }, &j, &value)) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected a default value"), txp_small_position_at(p, j));
        }
        const scan outcome = txp_add_default(p, &(default_request){ element, name, value });
        if (outcome != SCAN_OK) {
            return outcome;
        }
    }
}

/* ------------------------------------------------------------------------
 * The DOCTYPE state.
 */

scan txp_begin_doctype(txml_parser *p) {
    if (p->stage != STAGE_PROLOG || p->seen_doctype) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "a DOCTYPE must come once, before the document element"), p->construct);
    }
    txml_doc_run_begin(p->doc, &p->run);
    txp_dec_reset(p, DECODE_VERBATIM);
    p->sink = &txp_sink_run;
    p->doctype = (doctype_work){ .phase = DP_SPACE };
    p->state = &txp_state_doctype;
    return SCAN_OK;
}

static bool is_upper(uint8_t b) {
    return b >= 'A' && b <= 'Z';
}

static bool keyword_is(const doctype_work *d, const char *literal) {
    return txp_bytes_are((const uint8_t *)d->keyword, d->keyword_length, literal);
}

static scan unrecognized(txml_parser *p, txml_position at) {
    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "unrecognized declaration in the internal subset"), at);
}

static scan doctype_run(txml_parser *p) {
    doctype_work *d = &p->doctype;
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;
    position_cursor cursor = { 0u, { 0u, 0u, 0u }, false };
    scan outcome = SCAN_OK;
    bool ended = false;

    const txml_result reserved = txml_doc_run_reserve(p->doc, &p->run, limit - i);
    if (reserved != TXML_OK) {
        return txp_fail_result(p, reserved, p->construct);
    }

    while (i < limit) {
        const uint8_t b = w[i];
        uint32_t width = 0u;
        switch (d->phase) {
        case DP_SPACE:
            if (!txp_is_space(b)) {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected a name after DOCTYPE"), txp_position_at(p, i));
            }
            d->phase = DP_SPACE_SKIP;
            i += 1u;
            break;
        case DP_SPACE_SKIP:
            if (txp_is_space(b)) {
                i += 1u;
            } else if (!txp_name_start_at(w + i, &width)) {
                if (b >= 0x80u) {
                    return txp_fail_name_char(p, i);
                }
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected a name after DOCTYPE"), txp_position_at(p, i));
            } else {
                d->phase = DP_NAME;
            }
            break;
        case DP_NAME:
            if (txp_name_char_at(w + i, &width)) {
                i += width;
            } else if (b >= 0x80u) {
                return txp_fail_name_char(p, i);
            } else {
                d->phase = DP_REST;
            }
            break;
        case DP_REST:
        case DP_AFTER:
            if (d->quote != 0u) {
                if (b == d->quote) {
                    d->quote = 0u;
                }
            } else if (b == '"' || b == '\'') {
                d->quote = b;
            } else if (b == '[' && d->phase == DP_REST) {
                d->phase = DP_SUBSET;
            } else if (b == '>') {
                ended = true;
                goto stop;
            }
            i += 1u;
            break;
        case DP_SUBSET:
            if (txp_is_space(b)) {
                i += 1u;
            } else if (b == '%') {
                d->phase = DP_PARAMETER;
                i += 1u;
            } else if (b == ']') {
                d->phase = DP_AFTER;
                i += 1u;
            } else if (b == '<') {
                d->lt_position = txp_cursor_position(p, &cursor, i);
                d->phase = DP_LT;
                i += 1u;
            } else {
                return unrecognized(p, txp_position_at(p, i));
            }
            break;
        case DP_PARAMETER:
            if (b == ';') {
                d->phase = DP_SUBSET;
            }
            i += 1u;
            break;
        case DP_LT:
            if (b == '?') {
                d->phase = DP_PI;
                d->pending = 0u;
            } else if (b == '!') {
                d->phase = DP_BANG;
            } else {
                return unrecognized(p, d->lt_position);
            }
            i += 1u;
            break;
        case DP_BANG:
            if (b == '-') {
                d->phase = DP_DASH;
                i += 1u;
            } else if (is_upper(b)) {
                d->keyword_length = 0u;
                d->phase = DP_KEYWORD;
            } else {
                return unrecognized(p, d->lt_position);
            }
            break;
        case DP_DASH:
            if (b != '-') {
                return unrecognized(p, d->lt_position);
            }
            d->phase = DP_COMMENT;
            d->pending = 0u;
            i += 1u;
            break;
        case DP_KEYWORD:
            if (is_upper(b)) {
                if (d->keyword_length >= sizeof(d->keyword)) {
                    return unrecognized(p, d->lt_position);
                }
                d->keyword[d->keyword_length++] = (char)b;
                i += 1u;
            } else if (keyword_is(d, "ENTITY") || keyword_is(d, "ATTLIST")) {
                d->entity_declaration = keyword_is(d, "ENTITY");
                p->small.length = 0u;
                p->small_position = txp_cursor_position(p, &cursor, i);
                d->quote = 0u;
                d->phase = DP_COLLECT;
            } else if (keyword_is(d, "ELEMENT") || keyword_is(d, "NOTATION")) {
                d->quote = 0u;
                d->phase = DP_SKIP;
            } else {
                return unrecognized(p, d->lt_position);
            }
            break;
        case DP_COMMENT: /* skip to "-->" */
            if (b == '-') {
                d->pending = d->pending < 2u ? d->pending + 1u : 2u;
            } else if (b == '>' && d->pending == 2u) {
                d->phase = DP_SUBSET;
                d->pending = 0u;
            } else {
                d->pending = 0u;
            }
            i += 1u;
            break;
        case DP_PI: /* skip to "?>" */
            if (b == '?') {
                d->pending = 1u;
            } else if (b == '>' && d->pending == 1u) {
                d->phase = DP_SUBSET;
                d->pending = 0u;
            } else {
                d->pending = 0u;
            }
            i += 1u;
            break;
        case DP_SKIP:
            if (d->quote != 0u) {
                if (b == d->quote) {
                    d->quote = 0u;
                }
            } else if (b == '"' || b == '\'') {
                d->quote = b;
            } else if (b == '>') {
                d->phase = DP_SUBSET;
            }
            i += 1u;
            break;
        default: /* DP_COLLECT */
            outcome = txp_buffer_append(p, &b, 1u);
            if (outcome != SCAN_OK) {
                goto stop;
            }
            if (d->quote != 0u) {
                if (b == d->quote) {
                    d->quote = 0u;
                }
            } else if (b == '"' || b == '\'') {
                d->quote = b;
            } else if (b == '>') {
                outcome = d->entity_declaration ? read_entity(p) : read_attlist(p);
                if (outcome != SCAN_OK) {
                    p->small.length -= 1u; /* this byte is read again on retry */
                    goto stop;
                }
                d->phase = DP_SUBSET;
            }
            i += 1u;
            break;
        }
    }

stop:
    if (outcome == SCAN_FAIL) {
        return outcome;
    }
    if (i > p->start) {
        uint32_t consumed = 0u;
        txp_dec_feed(p, &(dec_input){ w + p->start, i - p->start }, &consumed);
    }
    txp_commit(p, i);
    if (!ended) {
        return outcome;
    }

    outcome = txp_append_leaf(p, &(leaf){ TXML_NODE_DOCTYPE, 0u, TXML_NAME_NONE, p->construct });
    if (outcome != SCAN_OK) {
        return outcome;
    }
    txp_commit(p, i + 1u);
    p->seen_doctype = true;
    p->state = &txp_state_content;
    return SCAN_OK;
}

const state txp_state_doctype = { "doctype", doctype_run, txp_eof_in_markup };

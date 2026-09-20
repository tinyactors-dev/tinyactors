/*
 * The DOCTYPE and its internal subset.
 *
 * The declaration's text streams into a pool run exactly as written, while a
 * second view of the same bytes, a state machine, walks its structure: the name,
 * the external identifier, the internal subset, and within it the declarations.
 * Comments, processing instructions and parameter-entity references are
 * checked and streamed past. The external identifier and each ENTITY, ATTLIST,
 * ELEMENT and NOTATION declaration are assembled whole in the small buffer, the
 * same buffer and bound the XML declaration uses, and read there against the
 * grammar by subset.c; only ENTITY and ATTLIST declarations are kept.
 *
 * Every byte is interpreted before any of it is emitted, and the run is
 * reserved first, so an allocation that fails can only fail before a byte is
 * consumed.
 */
#include "parser.h"

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

/* A name was needed at window[index]: an illegal name character, or plain
 * syntax. */
static scan expected_name(txml_parser *p, uint32_t index, const char *message) {
    uint32_t width = 0u;
    const uint8_t b = p->window[index];
    if (b >= 0x80u) {
        return txp_fail_name_char(p, index);
    }
    if (txp_name_char_at(p->window + index, &width)) {
        return txp_fail(p, PROBLEM(TXML_DOC_NAME, "a name cannot start with this character"), txp_position_at(p, index));
    }
    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, message), txp_position_at(p, index));
}

/* Track the quotes of a collected declaration. */
static void follow_quote(doctype_work *d, uint8_t b) {
    if (d->quote != 0u) {
        if (b == d->quote) {
            d->quote = 0u;
        }
    } else if (b == '"' || b == '\'') {
        d->quote = b;
    }
}

static bool is_xml_pi_target(const doctype_work *d) {
    return d->keyword_length == 3u && (((uint8_t)d->keyword[0] | 0x20u) == 'x') &&
           (((uint8_t)d->keyword[1] | 0x20u) == 'm') && (((uint8_t)d->keyword[2] | 0x20u) == 'l');
}

/* The byte ending a collected declaration has been appended: read it. */
static scan finish_collected(txml_parser *p, declaration_kind kind) {
    const scan outcome = txp_read_declaration(p, kind);
    if (outcome != SCAN_OK) {
        p->small.length -= 1u; /* this byte is read again on retry */
    }
    return outcome;
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
                return expected_name(p, i, "expected a name after DOCTYPE");
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
                /* Everything up to '[' or '>' is the external identifier. */
                p->small.length = 0u;
                p->small_position = txp_cursor_position(p, &cursor, i);
                d->quote = 0u;
                d->phase = DP_HEAD;
            }
            break;
        case DP_HEAD:
            outcome = txp_buffer_append(p, &b, 1u);
            if (outcome != SCAN_OK) {
                goto stop;
            }
            follow_quote(d, b);
            if (d->quote == 0u && (b == '[' || b == '>')) {
                outcome = finish_collected(p, DECL_DOCTYPE_HEAD);
                if (outcome != SCAN_OK) {
                    goto stop;
                }
                if (b == '>') {
                    ended = true;
                    goto stop;
                }
                d->phase = DP_SUBSET;
            }
            i += 1u;
            break;
        case DP_AFTER:
            if (txp_is_space(b)) {
                i += 1u;
            } else if (b == '>') {
                ended = true;
                goto stop;
            } else {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected '>' after the internal subset"), txp_position_at(p, i));
            }
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
            if (!txp_name_start_at(w + i, &width)) {
                return expected_name(p, i, "expected a name in a parameter-entity reference");
            }
            d->phase = DP_PARAMETER_NAME;
            i += width;
            break;
        case DP_PARAMETER_NAME:
            if (txp_name_char_at(w + i, &width)) {
                i += width;
            } else if (b == ';') {
                d->phase = DP_SUBSET;
                i += 1u;
            } else if (b >= 0x80u) {
                return txp_fail_name_char(p, i);
            } else {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "a parameter-entity reference must end in ';'"),
                                txp_position_at(p, i));
            }
            break;
        case DP_LT:
            if (b == '?') {
                d->phase = DP_PI_START;
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
            } else if (b == '[') {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "conditional sections are not allowed in an internal subset"),
                                d->lt_position);
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
            } else if (keyword_is(d, "ENTITY") || keyword_is(d, "ATTLIST") || keyword_is(d, "ELEMENT") ||
                       keyword_is(d, "NOTATION")) {
                d->kind = keyword_is(d, "ENTITY")    ? DECL_ENTITY
                          : keyword_is(d, "ATTLIST") ? DECL_ATTLIST
                          : keyword_is(d, "ELEMENT") ? DECL_ELEMENT
                                                     : DECL_NOTATION;
                p->small.length = 0u;
                p->small_position = txp_cursor_position(p, &cursor, i);
                d->quote = 0u;
                d->phase = DP_COLLECT;
            } else {
                return unrecognized(p, d->lt_position);
            }
            break;
        case DP_COMMENT: /* skip to "-->"; a "--" that is not its start is an error */
            if (b == '-') {
                if (d->pending == 2u) {
                    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "'--' inside a comment"), txp_position_at(p, i));
                }
                d->pending += 1u;
            } else if (d->pending == 2u) {
                if (b != '>') {
                    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "'--' inside a comment"), txp_position_at(p, i));
                }
                d->phase = DP_SUBSET;
                d->pending = 0u;
            } else {
                d->pending = 0u;
            }
            i += 1u;
            break;
        case DP_PI_START:
            if (!txp_name_start_at(w + i, &width)) {
                if (b >= 0x80u) {
                    return txp_fail_name_char(p, i);
                }
                return txp_fail(p, PROBLEM(TXML_DOC_NAME, "expected a processing instruction target"), txp_position_at(p, i));
            }
            d->keyword_length = 0u;
            d->phase = DP_PI_TARGET;
            break;
        case DP_PI_TARGET:
            if (b == ':') {
                return txp_fail(p, PROBLEM(TXML_DOC_NAME, "a target cannot contain a colon"), txp_position_at(p, i));
            }
            if (txp_name_char_at(w + i, &width)) {
                if (d->keyword_length < 4u) {
                    d->keyword[d->keyword_length] = (char)b;
                }
                d->keyword_length += 1u;
                i += width;
            } else if (b >= 0x80u) {
                return txp_fail_name_char(p, i);
            } else if (is_xml_pi_target(d)) {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "the XML declaration is only allowed at the start of the document"),
                                d->lt_position);
            } else if (b == '?' || txp_is_space(b)) {
                d->phase = DP_PI;
                d->pending = 0u;
            } else {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected whitespace after the target"), txp_position_at(p, i));
            }
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
        default: /* DP_COLLECT */
            outcome = txp_buffer_append(p, &b, 1u);
            if (outcome != SCAN_OK) {
                goto stop;
            }
            follow_quote(d, b);
            if (d->quote == 0u && b == '>') {
                outcome = finish_collected(p, d->kind);
                if (outcome != SCAN_OK) {
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

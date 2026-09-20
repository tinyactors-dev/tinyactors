/*
 * Content and the constructs that are not tags: text, comments, CDATA
 * sections, processing instructions, and the XML declaration. Each is a state
 * that reads what is available, keeps whatever a chunk boundary could split
 * (a held-back '-', ']' or '?', a partial literal) in the parser, and streams
 * its bytes into a pool run.
 */
#include "parser.h"

/* ------------------------------------------------------------------------
 * Helpers.
 */

static txml_position position_of(txml_parser *p, uint32_t index) {
    txp_commit(p, index);
    return txp_position_now(p);
}

/* Start streaming a run of decoded bytes. */
static void begin_run(txml_parser *p, decode_mode mode) {
    txml_doc_run_begin(p->doc, &p->run);
    txp_dec_reset(p, mode);
    p->sink = &txp_sink_run;
}

/* Feed `bytes` through the decoder. Returns the outcome of the feed. */
static scan feed(txml_parser *p, const uint8_t *bytes, uint32_t length) {
    uint32_t consumed = 0u;
    return txp_dec_feed(p, &(dec_input){ bytes, length }, &consumed);
}

/* Feed the window bytes [from, to) and commit what was taken. */
static scan feed_window(txml_parser *p, uint32_t from, uint32_t to) {
    uint32_t consumed = 0u;
    const scan outcome = txp_dec_feed(p, &(dec_input){ p->window + from, to - from }, &consumed);
    txp_commit(p, from + consumed);
    return outcome;
}

/* ------------------------------------------------------------------------
 * Content: text, and the '<' that ends it.
 */

static scan flush_text(txml_parser *p) {
    scan outcome = txp_dec_end(p);
    if (outcome != SCAN_OK) {
        return outcome;
    }
    outcome = txp_append_leaf(p, &(leaf){ TXML_NODE_TEXT, 0u, TXML_NAME_NONE, p->construct });
    if (outcome == SCAN_OK) {
        p->text_active = false;
    }
    return outcome;
}

static scan content_run(txml_parser *p) {
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    const uint32_t i = p->start;

    if (w[i] == '<') {
        if (p->text_active) {
            const scan outcome = flush_text(p);
            if (outcome != SCAN_OK) {
                return outcome;
            }
        }
        p->construct = txp_position_now(p);
        txp_commit(p, i + 1u);
        p->state = &txp_state_lt;
        return SCAN_OK;
    }

    if (!p->text_active) {
        if (txp_budget_spent(p)) {
            return SCAN_YIELD;
        }
        p->text_active = true;
        p->construct = txp_position_now(p);
        begin_run(p, DECODE_TEXT);
    }
    uint32_t j = i;
    while (j < limit && w[j] != '<') {
        j += 1u;
    }
    if (p->stage != STAGE_CONTENT) {
        /* Outside the document element only whitespace is character data. */
        for (uint32_t k = i; k < j; k++) {
            if (!txp_is_space(w[k])) {
                return txp_fail(p, p->seen_root ? PROBLEM(TXML_DOC_TRAILING_CONTENT, "content after the document element")
                                                : PROBLEM(TXML_DOC_SYNTAX, "text before the document element"),
                                txp_position_at(p, k));
            }
        }
    }
    return feed_window(p, i, j);
}

static scan content_eof(txml_parser *p) {
    if (p->text_active) {
        const scan outcome = flush_text(p);
        if (outcome != SCAN_OK) {
            return outcome;
        }
    }
    if (p->depth != 0u) {
        return txp_fail(p, PROBLEM(TXML_DOC_UNEXPECTED_EOF, "input ended with elements still open"), txp_position_now(p));
    }
    if (!p->seen_root) {
        return txp_fail(p, PROBLEM(TXML_DOC_NO_ROOT, "no document element"), txp_position_now(p));
    }
    txml_doc_complete(p->doc);
    return SCAN_DONE;
}

const state txp_state_content = { "content", content_run, content_eof };

/*
 * The start of the input. A UTF-8 byte order mark is an encoding signature,
 * not content: it is consumed without a node, counted in the offset, and does
 * not move the column. U+FEFF anywhere else is character data. The first
 * character below `valid` is whole, so all three bytes are here. UTF-16 marks
 * are not valid UTF-8 and fail as encoding errors before this state runs.
 */
static scan start_run(txml_parser *p) {
    const uint8_t *w = p->window + p->start;

    if (w[0] == 0xEFu && w[1] == 0xBBu && w[2] == 0xBFu) {
        txp_commit(p, p->start + 3u);
        p->column = 1u;
        p->bom_length = 3u;
    }
    p->state = &txp_state_content;
    return SCAN_OK;
}

const state txp_state_start = { "start", start_run, content_eof };

/* ------------------------------------------------------------------------
 * After '<': which construct is this?
 */

static scan lt_run(txml_parser *p) {
    const uint8_t b = p->window[p->start];

    if (b == '/') {
        txp_commit(p, p->start + 1u);
        txp_begin_end_tag(p);
        return SCAN_OK;
    }
    /* Everything else appends a node, so the budget is checked here; an end
     * tag only patches one and never stalls a step. */
    if (txp_budget_spent(p)) {
        return SCAN_YIELD;
    }
    if (b == '!') {
        txp_commit(p, p->start + 1u);
        p->state = &txp_state_bang;
        return SCAN_OK;
    }
    if (b == '?') {
        txp_commit(p, p->start + 1u);
        p->name_length = 0u;
        p->pi.has_colon = false;
        p->state = &txp_state_pi_target;
        return SCAN_OK;
    }
    return txp_begin_start_tag(p);
}

const state txp_state_lt = { "lt", lt_run, txp_eof_in_markup };

static void set_literal(txml_parser *p, literal_kind kind, const char *expect) {
    p->literal.kind = kind;
    p->literal.expect = expect;
    p->literal.index = 0u;
}

/* After "<!": a comment, a CDATA section, or a DOCTYPE. */
static scan bang_run(txml_parser *p) {
    const uint8_t b = p->window[p->start];

    switch (b) {
    case '-':
        set_literal(p, LIT_COMMENT, "-");
        break;
    case '[':
        set_literal(p, LIT_CDATA, "CDATA[");
        break;
    case 'D':
        set_literal(p, LIT_DOCTYPE, "OCTYPE");
        break;
    default:
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "unrecognized markup declaration"), p->construct);
    }
    txp_commit(p, p->start + 1u);
    p->state = &txp_state_literal;
    return SCAN_OK;
}

const state txp_state_bang = { "bang", bang_run, txp_eof_in_markup };

static scan begin_comment(txml_parser *p) {
    begin_run(p, DECODE_VERBATIM);
    p->comment.dashes = 0u;
    p->state = &txp_state_comment;
    return SCAN_OK;
}

static scan begin_cdata(txml_parser *p) {
    if (p->stage != STAGE_CONTENT) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "CDATA outside the document element"), p->construct);
    }
    begin_run(p, DECODE_VERBATIM);
    p->cdata.brackets = 0u;
    p->state = &txp_state_cdata;
    return SCAN_OK;
}

/* The rest of a fixed opener: the second '-', "CDATA[", "OCTYPE". */
static scan literal_run(txml_parser *p) {
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    const uint32_t length = txp_literal_length(p->literal.expect);
    uint32_t i = p->start;

    while (i < limit && p->literal.index < length) {
        if (w[i] != (uint8_t)p->literal.expect[p->literal.index]) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "unrecognized markup declaration"), p->construct);
        }
        p->literal.index += 1u;
        i += 1u;
    }
    txp_commit(p, i);
    if (p->literal.index < length) {
        return SCAN_OK;
    }
    switch (p->literal.kind) {
    case LIT_COMMENT:
        return begin_comment(p);
    case LIT_CDATA:
        return begin_cdata(p);
    default:
        return txp_begin_doctype(p);
    }
}

const state txp_state_literal = { "literal", literal_run, txp_eof_in_markup };

/* ------------------------------------------------------------------------
 * Comments. A '-' is held back until the next byte says whether it belongs to
 * the body or to the "-->" that ends it.
 */

static scan comment_run(txml_parser *p) {
    static const uint8_t dash = '-';
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;
    scan outcome;

    while (i < limit) {
        const uint8_t b = w[i];

        if (p->comment.dashes != 0u) {
            if (b == '-') {
                if (p->comment.dashes == 2u) {
                    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "'--' inside a comment"), p->comment.first_dash);
                }
                p->comment.dashes = 2u;
                i += 1u;
                continue;
            }
            if (p->comment.dashes == 2u) {
                if (b != '>') {
                    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "'--' inside a comment"), p->comment.first_dash);
                }
                outcome = txp_append_leaf(p, &(leaf){ TXML_NODE_COMMENT, 0u, TXML_NAME_NONE, p->construct });
                txp_commit(p, outcome == SCAN_OK ? i + 1u : i);
                if (outcome == SCAN_OK) {
                    p->state = &txp_state_content;
                }
                return outcome;
            }
            outcome = feed(p, &dash, 1u); /* one '-' was just body text */
            if (outcome != SCAN_OK) {
                txp_commit(p, i);
                return outcome;
            }
            p->comment.dashes = 0u;
        }
        if (b == '-') {
            p->comment.first_dash = position_of(p, i);
            p->comment.dashes = 1u;
            i += 1u;
            continue;
        }
        uint32_t j = i;
        while (j < limit && w[j] != '-') {
            j += 1u;
        }
        outcome = feed_window(p, i, j);
        if (outcome != SCAN_OK || p->start != j) {
            return outcome;
        }
        i = j;
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_comment = { "comment", comment_run, txp_eof_in_markup };

/* ------------------------------------------------------------------------
 * CDATA sections: up to two ']' are held back the same way.
 */

static scan cdata_run(txml_parser *p) {
    static const uint8_t brackets[2] = { ']', ']' };
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;
    scan outcome;

    while (i < limit) {
        const uint8_t b = w[i];

        if (b == ']') {
            if (p->cdata.brackets == 2u) {
                outcome = feed(p, brackets, 1u); /* the oldest ']' is body text */
                if (outcome != SCAN_OK) {
                    txp_commit(p, i);
                    return outcome;
                }
            } else {
                p->cdata.brackets += 1u;
            }
            i += 1u;
            continue;
        }
        if (p->cdata.brackets == 2u && b == '>') {
            outcome = txp_append_leaf(p, &(leaf){ TXML_NODE_TEXT, TXML_NODE_CDATA, TXML_NAME_NONE, p->construct });
            txp_commit(p, outcome == SCAN_OK ? i + 1u : i);
            if (outcome == SCAN_OK) {
                p->state = &txp_state_content;
            }
            return outcome;
        }
        if (p->cdata.brackets != 0u) {
            outcome = feed(p, brackets, p->cdata.brackets);
            if (outcome != SCAN_OK) {
                txp_commit(p, i);
                return outcome;
            }
            p->cdata.brackets = 0u;
        }
        uint32_t j = i;
        while (j < limit && w[j] != ']') {
            j += 1u;
        }
        outcome = feed_window(p, i, j);
        if (outcome != SCAN_OK || p->start != j) {
            return outcome;
        }
        i = j;
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_cdata = { "cdata", cdata_run, txp_eof_in_markup };

/* ------------------------------------------------------------------------
 * Processing instructions.
 */

static scan finish_pi(txml_parser *p) {
    uint32_t name_id = 0u;

    /* The target is interned after the data run is finished, because the pool
     * has one writer at a time. */
    txml_doc_run_finish(p->doc, &p->run);
    const intern_request request = { { 0u, { (const char *)p->name, p->name_length }, { "", 0u } }, p->construct };
    const scan outcome = txp_intern(p, &request, &name_id);
    if (outcome != SCAN_OK) {
        return outcome;
    }
    return txp_append_leaf(p, &(leaf){ TXML_NODE_PI, 0u, name_id, p->construct });
}

static scan begin_declaration(txml_parser *p) {
    p->small.length = 0u;
    p->small_position = p->construct;
    p->small_position.offset += 5u; /* after "<?xml" */
    p->small_position.column += 5u;
    p->declaration.question = 0u;
    p->state = &txp_state_declaration;
    return SCAN_OK;
}

/* The delimiter that ends a PI target has arrived at window[start]. */
static scan target_done(txml_parser *p, uint8_t delimiter) {
    const uint8_t *name = p->name;
    const bool any_xml = p->name_length == 3u && (name[0] | 0x20u) == 'x' && (name[1] | 0x20u) == 'm' && (name[2] | 0x20u) == 'l';

    if (p->pi.has_colon) {
        return txp_fail(p, PROBLEM(TXML_DOC_NAME, "a target cannot contain a colon"), p->pi.colon_position);
    }
    if (any_xml) {
        if (txp_bytes_are(name, 3u, "xml") && p->construct.offset == p->bom_length && txp_is_space(delimiter)) {
            return begin_declaration(p);
        }
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "the XML declaration is only allowed at the start of the document"),
                        p->construct);
    }
    if (delimiter == '?') {
        p->pi.question_position = txp_position_now(p);
        txp_commit(p, p->start + 1u);
        p->state = &txp_state_pi_question;
        return SCAN_OK;
    }
    if (txp_is_space(delimiter)) {
        p->state = &txp_state_pi_space;
        return SCAN_OK;
    }
    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected whitespace after the target"), txp_position_now(p));
}

static scan pi_target_run(txml_parser *p) {
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;

    while (i < limit) {
        const uint8_t b = w[i];

        uint32_t width = 0u;
        if (p->name_length == 0u) {
            if (!txp_name_start_at(w + i, &width)) {
                return txp_fail(p, PROBLEM(TXML_DOC_NAME, "expected a processing instruction target"), txp_position_at(p, i));
            }
            p->name_position = position_of(p, i);
        }
        if (!txp_name_char_at(w + i, &width)) {
            if (b >= 0x80u) {
                return txp_fail_name_char(p, i);
            }
            txp_commit(p, i);
            return target_done(p, b);
        }
        if (p->name_length + width > p->limits.max_name_length) {
            return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_NAME_LENGTH, "name too long"), p->name_position);
        }
        if (b == ':' && !p->pi.has_colon) {
            p->pi.has_colon = true;
            p->pi.colon_position = position_of(p, i);
        }
        for (uint32_t k = 0u; k < width; k++) {
            p->name[p->name_length++] = w[i + k];
        }
        i += width;
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_pi_target = { "pi-target", pi_target_run, txp_eof_in_markup };

/* "<?target?>": nothing may follow the '?' but '>'. */
static scan pi_question_run(txml_parser *p) {
    if (p->window[p->start] != '>') {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected whitespace after the target"), p->pi.question_position);
    }
    txml_doc_run_begin(p->doc, &p->run);
    const scan outcome = finish_pi(p);
    if (outcome != SCAN_OK) {
        return outcome;
    }
    txp_commit(p, p->start + 1u);
    p->state = &txp_state_content;
    return SCAN_OK;
}

const state txp_state_pi_question = { "pi-question", pi_question_run, txp_eof_in_markup };

static scan pi_space_run(txml_parser *p) {
    const uint32_t limit = txp_limit(p);
    const uint32_t i = txp_skip_space(p->window, p->start, limit);

    txp_commit(p, i);
    if (i < limit) {
        begin_run(p, DECODE_VERBATIM);
        p->pi.question = 0u;
        p->state = &txp_state_pi_data;
    }
    return SCAN_OK;
}

const state txp_state_pi_space = { "pi-space", pi_space_run, txp_eof_in_markup };

static scan pi_data_run(txml_parser *p) {
    static const uint8_t question = '?';
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;
    scan outcome;

    while (i < limit) {
        const uint8_t b = w[i];

        if (b == '?') {
            if (p->pi.question == 1u) {
                outcome = feed(p, &question, 1u);
                if (outcome != SCAN_OK) {
                    txp_commit(p, i);
                    return outcome;
                }
            }
            p->pi.question = 1u;
            i += 1u;
            continue;
        }
        if (p->pi.question == 1u && b == '>') {
            outcome = finish_pi(p);
            txp_commit(p, outcome == SCAN_OK ? i + 1u : i);
            if (outcome == SCAN_OK) {
                p->state = &txp_state_content;
            }
            return outcome;
        }
        if (p->pi.question == 1u) {
            outcome = feed(p, &question, 1u);
            if (outcome != SCAN_OK) {
                txp_commit(p, i);
                return outcome;
            }
            p->pi.question = 0u;
        }
        uint32_t j = i;
        while (j < limit && w[j] != '?') {
            j += 1u;
        }
        outcome = feed_window(p, i, j);
        if (outcome != SCAN_OK || p->start != j) {
            return outcome;
        }
        i = j;
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_pi_data = { "pi-data", pi_data_run, txp_eof_in_markup };

/* ------------------------------------------------------------------------
 * The XML declaration. It is small and has a fixed shape, so it is gathered
 * whole in the small buffer and read from there.
 */

/* The pseudo-attribute values of an XML declaration have fixed shapes:
 * version "1.N", an encoding name, standalone "yes" or "no". Checking them
 * keeps a value from carrying markup that the writer would have to escape. */
static bool declaration_value_valid(uint32_t which, const uint8_t *value, uint32_t length) {
    if (which == 0u) {
        if (length < 3u || value[0] != '1' || value[1] != '.') {
            return false;
        }
        for (uint32_t i = 2u; i < length; i++) {
            if (value[i] < '0' || value[i] > '9') {
                return false;
            }
        }
        return true;
    }
    if (which == 2u) {
        return txp_bytes_are(value, length, "yes") || txp_bytes_are(value, length, "no");
    }
    if (length == 0u || !((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z'))) {
        return false;
    }
    for (uint32_t i = 1u; i < length; i++) {
        const uint8_t b = value[i];
        if (!((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '.' || b == '_' || b == '-')) {
            return false;
        }
    }
    return true;
}

static scan finish_declaration(txml_parser *p) {
    static const char *const names[3] = { "version", "encoding", "standalone" };
    const uint8_t *w = p->small.data;
    const uint32_t close = p->small.length;
    uint32_t i = txp_skip_space(w, 0u, close);
    uint32_t count = 0u;
    uint32_t next_allowed = 0u;
    struct { uint32_t name, offset, length; } parsed[3];

    while (i < close) {
        const uint32_t name_end = txp_scan_name(w, i, close);
        uint32_t which = 3u;
        for (uint32_t k = next_allowed; k < 3u; k++) {
            if (txp_bytes_are(w + i, name_end - i, names[k])) {
                which = k;
                break;
            }
        }
        if (which == 3u || (count == 0u && which != 0u)) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "malformed XML declaration"), txp_small_position_at(p, i));
        }
        next_allowed = which + 1u;
        i = txp_skip_space(w, name_end, close);
        if (i >= close || w[i] != '=') {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected '=' in the XML declaration"), txp_small_position_at(p, i));
        }
        i = txp_skip_space(w, i + 1u, close);
        if (i >= close || (w[i] != '"' && w[i] != '\'')) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "declaration values must be quoted"), txp_small_position_at(p, i));
        }
        uint32_t value_end = i + 1u;
        while (value_end < close && w[value_end] != w[i]) {
            value_end += 1u;
        }
        if (value_end >= close) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "unterminated declaration value"), txp_small_position_at(p, i));
        }
        if (!declaration_value_valid(which, w + i + 1u, value_end - i - 1u)) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "malformed XML declaration value"), txp_small_position_at(p, i));
        }
        if (which == 1u) {
            const uint8_t *e = w + i + 1u;
            const uint32_t n = value_end - i - 1u;
            const bool utf8 = n == 5u && (e[0] | 0x20u) == 'u' && (e[1] | 0x20u) == 't' && (e[2] | 0x20u) == 'f' &&
                              e[3] == '-' && e[4] == '8';
            if (!utf8) {
                return txp_fail(p, PROBLEM(TXML_DOC_ENCODING, "only UTF-8 is supported"), txp_small_position_at(p, i));
            }
        }
        parsed[count].name = which;
        parsed[count].offset = i + 1u;
        parsed[count].length = value_end - i - 1u;
        count += 1u;
        i = value_end + 1u;
        const uint32_t after = txp_skip_space(w, i, close);
        if (after == i && after < close) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "declaration values must be separated by whitespace"),
                            txp_small_position_at(p, i));
        }
        i = after;
    }
    if (count == 0u) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "the XML declaration needs a version"), p->construct);
    }

    const snapshot before = txp_snapshot(p);
    const uint32_t index = p->doc->nodes.count;
    const txml_node_append append = {
        .node = {
            .kind = TXML_NODE_DECLARATION, .depth = 1u, .parent = 0u, .end = index + 1u, .name = TXML_NAME_NONE,
            .as.element = { p->doc->attributes.count, count },
        },
        .position = p->construct,
    };
    uint32_t appended_index = 0u;
    txml_result result = txml_doc_append_node(p->doc, &append, &appended_index);
    for (uint32_t k = 0u; result == TXML_OK && k < count; k++) {
        uint32_t name_id = 0u;
        uint32_t offset = 0u;
        uint32_t attribute_index = 0u;
        const char *name = names[parsed[k].name];
        const txml_intern_params params = { 0u, { name, txp_literal_length(name) }, { "", 0u } };
        result = txml_doc_intern_name(p->doc, &params, &name_id);
        if (result == TXML_OK) {
            result = txml_doc_pool_append(p->doc, (txml_str){ (const char *)w + parsed[k].offset, parsed[k].length }, &offset);
        }
        if (result == TXML_OK) {
            const txml_attribute_entry entry = { name_id | (k + 1u == count ? TXML_ATTRIBUTE_LAST : 0u), offset, parsed[k].length };
            result = txml_doc_append_attribute(p->doc, &entry, &attribute_index);
        }
    }
    if (result != TXML_OK) {
        txp_rollback_nodes(p, &before);
        return txp_fail_result(p, result, p->construct);
    }
    p->appended += 1u;
    return txp_check_budget(p, p->construct);
}

static scan declaration_run(txml_parser *p) {
    static const uint8_t question = '?';
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;
    scan outcome;

    while (i < limit) {
        const uint8_t b = w[i];

        if (b == '?') {
            if (p->declaration.question == 1u) {
                outcome = txp_buffer_append(p, &question, 1u);
                if (outcome != SCAN_OK) {
                    txp_commit(p, i);
                    return outcome;
                }
            }
            p->declaration.question = 1u;
            i += 1u;
            continue;
        }
        if (p->declaration.question == 1u && b == '>') {
            outcome = finish_declaration(p);
            txp_commit(p, outcome == SCAN_OK ? i + 1u : i);
            if (outcome == SCAN_OK) {
                p->state = &txp_state_content;
            }
            return outcome;
        }
        if (p->declaration.question == 1u) {
            outcome = txp_buffer_append(p, &question, 1u);
            if (outcome != SCAN_OK) {
                txp_commit(p, i);
                return outcome;
            }
            p->declaration.question = 0u;
        }
        uint32_t j = i;
        while (j < limit && w[j] != '?') {
            j += 1u;
        }
        outcome = txp_buffer_append(p, w + i, j - i);
        if (outcome != SCAN_OK) {
            txp_commit(p, i);
            return outcome;
        }
        i = j;
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_declaration = { "declaration", declaration_run, txp_eof_in_markup };

/*
 * The declarations of a DOCTYPE, read whole.
 *
 * doctype.c streams the DOCTYPE's bytes through a state machine and, for each
 * declaration that has structure worth checking, assembles the bytes after its
 * keyword in the small buffer. This module reads that buffer against the XML 1.0
 * grammar. ENTITY and ATTLIST declarations are recorded (they change what a
 * document means); ELEMENT and NOTATION declarations, and the external
 * identifier of the DOCTYPE itself, are only checked.
 *
 * A non-validating processor still rejects a malformed declaration: it is a
 * well-formedness error. What is not checked is what needs the whole DTD to
 * decide, and the content model of an ELEMENT declaration beyond its shape:
 * parentheses balance, items and separators alternate, quantifiers follow an
 * item, and a mixed model is `(#PCDATA)` or `(#PCDATA|a|b)*`. A group may mix
 * `,` and `|`, which the grammar forbids, because telling them apart needs a
 * stack as deep as the model.
 *
 * Every function here is iterative and reads only [0, limit), where `limit`
 * is the index of the byte that ended the declaration.
 */
#include "parser.h"

typedef struct {
    txml_parser *p;
    const uint8_t *w;
    uint32_t limit; /* the terminating '>' or '[' */
    uint32_t i;
} reader;

typedef struct {
    uint32_t start;
    uint32_t end;
} span;

typedef enum {
    LITERAL_ENTITY_VALUE,
    LITERAL_ATTRIBUTE_VALUE,
    LITERAL_SYSTEM,
    LITERAL_PUBID
} literal_type;

#define SYNTAX(message) PROBLEM(TXML_DOC_SYNTAX, (message))
#define BAD_NAME(message) PROBLEM(TXML_DOC_NAME, (message))

static scan raise(const reader *r, const problem *what, uint32_t at) {
    return txp_fail(r->p, what, txp_small_position_at(r->p, at));
}

static txml_str text_of(const reader *r, span range) {
    return (txml_str){ (const char *)r->w + range.start, range.end - range.start };
}

/* Skip whitespace; the result is how much there was. */
static uint32_t skip_space(reader *r) {
    const uint32_t from = r->i;
    r->i = txp_skip_space(r->w, r->i, r->limit);
    return r->i - from;
}

static scan need_space(reader *r) {
    if (skip_space(r) == 0u) {
        return raise(r, SYNTAX("expected whitespace"), r->i);
    }
    return SCAN_OK;
}

/* Nothing may follow but whitespace. */
static scan finish(reader *r) {
    skip_space(r);
    if (r->i != r->limit) {
        return raise(r, SYNTAX("unexpected character in the declaration"), r->i);
    }
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * Names and keywords.
 */

/* A name where one must be. A non-ASCII character that cannot be part of a
 * name is illegal, not merely unexpected, since no such character delimits
 * markup; so is an ASCII name character that cannot start one. */
static scan bad_name_start(const reader *r, uint32_t at) {
    uint32_t width = 0u;
    if (at < r->limit && (r->w[at] >= 0x80u || txp_name_char_at(r->w + at, &width))) {
        return raise(r, BAD_NAME("not a legal name character here"), at);
    }
    return raise(r, SYNTAX("expected a name"), at);
}

static scan read_name(reader *r, span *out, bool ncname) {
    const uint32_t at = r->i;
    uint32_t width = 0u;

    if (at >= r->limit || !txp_name_start_at(r->w + at, &width)) {
        return bad_name_start(r, at);
    }
    const uint32_t stop = txp_scan_name(r->w, at, r->limit);
    if (stop < r->limit && r->w[stop] >= 0x80u) {
        return raise(r, BAD_NAME("not a legal name character"), stop);
    }
    if (ncname) {
        for (uint32_t k = at; k < stop; k++) {
            if (r->w[k] == ':') {
                return raise(r, BAD_NAME("a colon is not allowed in this name"), k);
            }
        }
    }
    *out = (span){ at, stop };
    r->i = stop;
    return SCAN_OK;
}

static scan read_nmtoken(reader *r) {
    const uint32_t at = r->i;
    uint32_t width = 0u;
    uint32_t k = at;

    while (k < r->limit && txp_name_char_at(r->w + k, &width)) {
        k += width;
    }
    if (k == at) {
        return bad_name_start(r, at);
    }
    if (k < r->limit && r->w[k] >= 0x80u) {
        return raise(r, BAD_NAME("not a legal name character"), k);
    }
    r->i = k;
    return SCAN_OK;
}

/* The run of name characters at r->i, without consuming it. */
static span peek_word(const reader *r) {
    return (span){ r->i, txp_scan_name(r->w, r->i, r->limit) };
}

static bool word_is(const reader *r, span word, const char *literal) {
    return txp_bytes_are(r->w + word.start, word.end - word.start, literal);
}

static scan expect_keyword(reader *r, const char *keyword) {
    const span word = peek_word(r);
    if (!word_is(r, word, keyword)) {
        return raise(r, SYNTAX("expected a keyword here"), r->i);
    }
    r->i = word.end;
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * Literals and references.
 */

static bool pubid_char(uint8_t b) {
    if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9')) {
        return true;
    }
    switch (b) {
    case ' ': case '\r': case '\n': case '-': case '\'': case '(': case ')': case '+': case ',': case '.':
    case '/': case ':': case '=': case '?': case ';': case '!': case '*': case '#': case '@': case '$':
    case '_': case '%':
        return true;
    default:
        return false;
    }
}

/* The reference starting at `at` ('&'): well-formed, ending in ';'. `next` is
 * the index after it. Whether a named entity exists is settled where it is
 * used, not here. */
static scan check_reference(const reader *r, uint32_t at, uint32_t *next) {
    uint32_t stop = at + 1u;

    if (stop < r->limit && r->w[stop] == '#') {
        while (stop < r->limit && r->w[stop] != ';' && r->w[stop] != '"' && r->w[stop] != '\'') {
            stop += 1u;
        }
        if (stop >= r->limit || r->w[stop] != ';') {
            return raise(r, SYNTAX("malformed character reference"), at);
        }
        const charref_check checked = txp_check_character_reference(r->w + at + 1u, stop - at - 1u);
        if (checked == CHARREF_ENCODING) {
            return raise(r, PROBLEM(TXML_DOC_ENCODING, "character reference outside the Char production"), at);
        }
        if (checked != CHARREF_OK) {
            return raise(r, SYNTAX("malformed character reference"), at);
        }
    } else {
        stop = txp_scan_name(r->w, at + 1u, r->limit);
        if (stop == at + 1u) {
            return raise(r, SYNTAX("malformed reference"), at);
        }
        if (stop < r->limit && r->w[stop] >= 0x80u) {
            return raise(r, BAD_NAME("not a legal name character"), stop);
        }
        if (stop >= r->limit || r->w[stop] != ';') {
            return raise(r, SYNTAX("a reference must end in ';'"), at);
        }
        for (uint32_t k = at + 1u; k < stop; k++) {
            if (r->w[k] == ':') {
                return raise(r, BAD_NAME("a colon is not allowed in an entity name"), k);
            }
        }
    }
    *next = stop + 1u;
    return SCAN_OK;
}

/* A quoted literal at r->i; `out` is its contents, checked for the kind. */
static scan read_literal(reader *r, literal_type type, txml_str *out) {
    const uint32_t at = r->i;

    if (at >= r->limit || (r->w[at] != '"' && r->w[at] != '\'')) {
        return raise(r, SYNTAX("expected a quoted literal"), at);
    }
    const uint8_t quote = r->w[at];
    uint32_t k = at + 1u;

    while (k < r->limit && r->w[k] != quote) {
        const uint8_t b = r->w[k];
        if (type == LITERAL_ENTITY_VALUE && b == '%') {
            return raise(r, SYNTAX("a parameter-entity reference is not allowed in an internal-subset entity value"), k);
        }
        if (type == LITERAL_ATTRIBUTE_VALUE && b == '<') {
            return raise(r, SYNTAX("'<' in an attribute default"), k);
        }
        if (type == LITERAL_PUBID && !pubid_char(b)) {
            return raise(r, SYNTAX("not a legal public identifier character"), k);
        }
        if ((type == LITERAL_ENTITY_VALUE || type == LITERAL_ATTRIBUTE_VALUE) && b == '&') {
            const scan checked = check_reference(r, k, &k);
            if (checked != SCAN_OK) {
                return checked;
            }
            continue;
        }
        k += 1u;
    }
    if (k >= r->limit) {
        return raise(r, SYNTAX("unterminated literal"), at);
    }
    *out = (txml_str){ (const char *)r->w + at + 1u, k - at - 1u };
    r->i = k + 1u;
    return SCAN_OK;
}

/* SYSTEM "lit" | PUBLIC "pubid" "lit". A notation may leave the system literal
 * out (`public_alone`). */
static scan read_external_id(reader *r, bool public_alone) {
    const uint32_t at = r->i;
    const span word = peek_word(r);
    txml_str ignored;
    scan outcome;

    if (word_is(r, word, "SYSTEM")) {
        r->i = word.end;
        outcome = need_space(r);
        return outcome != SCAN_OK ? outcome : read_literal(r, LITERAL_SYSTEM, &ignored);
    }
    if (!word_is(r, word, "PUBLIC")) {
        return raise(r, SYNTAX("expected SYSTEM or PUBLIC"), at);
    }
    r->i = word.end;
    outcome = need_space(r);
    if (outcome == SCAN_OK) {
        outcome = read_literal(r, LITERAL_PUBID, &ignored);
    }
    if (outcome != SCAN_OK) {
        return outcome;
    }
    const uint32_t after_pubid = r->i;
    const uint32_t spaces = skip_space(r);
    if (r->i < r->limit && (r->w[r->i] == '"' || r->w[r->i] == '\'')) {
        if (spaces == 0u) {
            return raise(r, SYNTAX("expected whitespace"), r->i);
        }
        return read_literal(r, LITERAL_SYSTEM, &ignored);
    }
    if (!public_alone) {
        return raise(r, SYNTAX("a PUBLIC identifier needs a system literal"), r->i);
    }
    r->i = after_pubid;
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * <!ENTITY ...>
 */

static scan read_entity(reader *r) {
    scan outcome = need_space(r);
    span name;
    txml_str value;

    if (outcome != SCAN_OK) {
        return outcome;
    }
    const bool parameter = r->i < r->limit && r->w[r->i] == '%';
    if (parameter) {
        r->i += 1u;
        outcome = need_space(r);
        if (outcome != SCAN_OK) {
            return outcome;
        }
    }
    outcome = read_name(r, &name, true);
    if (outcome == SCAN_OK) {
        outcome = need_space(r);
    }
    if (outcome != SCAN_OK) {
        return outcome;
    }

    if (r->i < r->limit && (r->w[r->i] == '"' || r->w[r->i] == '\'')) {
        outcome = read_literal(r, LITERAL_ENTITY_VALUE, &value);
        if (outcome == SCAN_OK && !parameter) {
            outcome = txp_add_entity(r->p, text_of(r, name), value);
        }
        return outcome != SCAN_OK ? outcome : finish(r);
    }

    /* External entities are checked and never fetched, so a reference to one
     * stays undeclared. Only a general entity may be unparsed. */
    outcome = read_external_id(r, false);
    if (outcome != SCAN_OK) {
        return outcome;
    }
    const uint32_t spaces = skip_space(r);
    if (!parameter && r->i < r->limit) {
        if (spaces == 0u) {
            return raise(r, SYNTAX("expected whitespace"), r->i);
        }
        outcome = expect_keyword(r, "NDATA");
        if (outcome == SCAN_OK) {
            outcome = need_space(r);
        }
        if (outcome == SCAN_OK) {
            outcome = read_name(r, &name, true);
        }
        if (outcome != SCAN_OK) {
            return outcome;
        }
    }
    return finish(r);
}

/* ------------------------------------------------------------------------
 * <!ATTLIST ...>
 */

/* '(' Nmtoken ('|' Nmtoken)* ')', or with Names for a notation type. */
static scan read_enumeration(reader *r, bool notation) {
    span name;
    scan outcome;

    if (r->i >= r->limit || r->w[r->i] != '(') {
        return raise(r, SYNTAX("expected '('"), r->i);
    }
    r->i += 1u;
    for (;;) {
        skip_space(r);
        outcome = notation ? read_name(r, &name, true) : read_nmtoken(r);
        if (outcome != SCAN_OK) {
            return outcome;
        }
        skip_space(r);
        if (r->i < r->limit && r->w[r->i] == '|') {
            r->i += 1u;
            continue;
        }
        if (r->i < r->limit && r->w[r->i] == ')') {
            r->i += 1u;
            return SCAN_OK;
        }
        return raise(r, SYNTAX("expected '|' or ')'"), r->i);
    }
}

static scan read_attribute_type(reader *r) {
    static const char *const keywords[] = { "CDATA", "ID", "IDREF", "IDREFS", "ENTITY", "ENTITIES", "NMTOKEN", "NMTOKENS" };

    if (r->i < r->limit && r->w[r->i] == '(') {
        return read_enumeration(r, false);
    }
    const span word = peek_word(r);
    if (word_is(r, word, "NOTATION")) {
        r->i = word.end;
        const scan outcome = need_space(r);
        return outcome != SCAN_OK ? outcome : read_enumeration(r, true);
    }
    for (uint32_t k = 0u; k < sizeof(keywords) / sizeof(keywords[0]); k++) {
        if (word_is(r, word, keywords[k])) {
            r->i = word.end;
            return SCAN_OK;
        }
    }
    return raise(r, SYNTAX("unknown attribute type"), r->i);
}

static scan read_attlist(reader *r) {
    scan outcome = need_space(r);
    span element;
    span name;
    txml_str value;

    if (outcome == SCAN_OK) {
        outcome = read_name(r, &element, false);
    }
    if (outcome != SCAN_OK) {
        return outcome;
    }

    for (;;) {
        const uint32_t spaces = skip_space(r);
        if (r->i == r->limit) {
            return SCAN_OK;
        }
        if (spaces == 0u) {
            return raise(r, SYNTAX("expected whitespace"), r->i);
        }
        outcome = read_name(r, &name, false);
        if (outcome == SCAN_OK) {
            outcome = need_space(r);
        }
        if (outcome == SCAN_OK) {
            outcome = read_attribute_type(r);
        }
        if (outcome == SCAN_OK) {
            outcome = need_space(r);
        }
        if (outcome != SCAN_OK) {
            return outcome;
        }

        bool has_value = true;
        if (r->i < r->limit && r->w[r->i] == '#') {
            const uint32_t hash = r->i;
            r->i += 1u;
            const span word = peek_word(r);
            if (word_is(r, word, "REQUIRED") || word_is(r, word, "IMPLIED")) {
                r->i = word.end;
                has_value = false;
            } else if (word_is(r, word, "FIXED")) {
                r->i = word.end;
                outcome = need_space(r);
                if (outcome != SCAN_OK) {
                    return outcome;
                }
            } else {
                return raise(r, SYNTAX("unknown default declaration"), hash);
            }
        }
        if (has_value) {
            outcome = read_literal(r, LITERAL_ATTRIBUTE_VALUE, &value);
            if (outcome == SCAN_OK) {
                outcome = txp_add_default(r->p, &(default_request){ text_of(r, element), text_of(r, name), value });
            }
            if (outcome != SCAN_OK) {
                return outcome;
            }
        }
    }
}

/* ------------------------------------------------------------------------
 * <!ELEMENT ...> and <!NOTATION ...>
 */

static bool is_quantifier(uint8_t b) {
    return b == '?' || b == '*' || b == '+';
}

/* A parenthesised content model, read without recursion: only the depth is
 * kept. */
static scan read_content_model(reader *r) {
    uint32_t depth = 0u;
    bool expect_item = true; /* an item must come next */
    bool mixed = false;
    uint32_t mixed_names = 0u;
    span name;

    for (;;) {
        skip_space(r);
        if (r->i >= r->limit) {
            return raise(r, SYNTAX("unbalanced parentheses in a content model"), r->i);
        }
        const uint8_t b = r->w[r->i];
        const uint32_t at = r->i;

        if (b == '(') {
            if (!expect_item || mixed) {
                return raise(r, SYNTAX("unexpected '(' in a content model"), at);
            }
            depth += 1u;
            r->i += 1u;
            skip_space(r);
            if (depth == 1u && r->i < r->limit && r->w[r->i] == '#') {
                const span word = (span){ r->i, txp_scan_name(r->w, r->i + 1u, r->limit) };
                if (!word_is(r, word, "#PCDATA")) {
                    return raise(r, SYNTAX("expected #PCDATA"), r->i);
                }
                mixed = true;
                expect_item = false;
                r->i = word.end;
            }
        } else if (b == '|' || b == ',') {
            if (expect_item || (mixed && b == ',')) {
                return raise(r, SYNTAX("unexpected separator in a content model"), at);
            }
            expect_item = true;
            r->i += 1u;
        } else if (b == ')') {
            if (expect_item || depth == 0u) {
                return raise(r, SYNTAX("unexpected ')' in a content model"), at);
            }
            depth -= 1u;
            r->i += 1u;
            if (mixed && depth == 0u) {
                if (r->i < r->limit && r->w[r->i] == '*') {
                    r->i += 1u;
                } else if (mixed_names > 0u) {
                    return raise(r, SYNTAX("a mixed content model with names must end in ')*'"), r->i);
                }
                return SCAN_OK;
            }
            if (r->i < r->limit && is_quantifier(r->w[r->i])) {
                r->i += 1u;
            }
            expect_item = false;
            if (depth == 0u) {
                return SCAN_OK;
            }
        } else if (b == '#') {
            return raise(r, SYNTAX("#PCDATA must come first in a mixed content model"), at);
        } else {
            if (!expect_item) {
                return raise(r, SYNTAX("expected a separator or ')' in a content model"), at);
            }
            const scan outcome = read_name(r, &name, false);
            if (outcome != SCAN_OK) {
                return outcome;
            }
            expect_item = false;
            if (mixed) {
                mixed_names += 1u;
            } else if (r->i < r->limit && is_quantifier(r->w[r->i])) {
                r->i += 1u;
            }
        }
    }
}

static scan read_element(reader *r) {
    scan outcome = need_space(r);
    span name;

    if (outcome == SCAN_OK) {
        outcome = read_name(r, &name, false);
    }
    if (outcome == SCAN_OK) {
        outcome = need_space(r);
    }
    if (outcome != SCAN_OK) {
        return outcome;
    }
    const span word = peek_word(r);
    if (word_is(r, word, "EMPTY") || word_is(r, word, "ANY")) {
        r->i = word.end;
    } else if (r->i < r->limit && r->w[r->i] == '(') {
        outcome = read_content_model(r);
        if (outcome != SCAN_OK) {
            return outcome;
        }
    } else {
        return raise(r, SYNTAX("expected EMPTY, ANY or a content model"), r->i);
    }
    return finish(r);
}

static scan read_notation(reader *r) {
    scan outcome = need_space(r);
    span name;

    if (outcome == SCAN_OK) {
        outcome = read_name(r, &name, true);
    }
    if (outcome == SCAN_OK) {
        outcome = need_space(r);
    }
    if (outcome == SCAN_OK) {
        outcome = read_external_id(r, true);
    }
    return outcome != SCAN_OK ? outcome : finish(r);
}

/* What follows the DOCTYPE's name: an optional external identifier. */
static scan read_doctype_head(reader *r) {
    const uint32_t spaces = skip_space(r);

    if (r->i == r->limit) {
        return SCAN_OK;
    }
    if (spaces == 0u) {
        return raise(r, SYNTAX("expected whitespace"), r->i);
    }
    const scan outcome = read_external_id(r, false);
    return outcome != SCAN_OK ? outcome : finish(r);
}

scan txp_read_declaration(txml_parser *p, declaration_kind kind) {
    reader r = { p, p->small.data, p->small.length - 1u, 0u };

    switch (kind) {
    case DECL_ENTITY:
        return read_entity(&r);
    case DECL_ATTLIST:
        return read_attlist(&r);
    case DECL_ELEMENT:
        return read_element(&r);
    case DECL_NOTATION:
        return read_notation(&r);
    default:
        return read_doctype_head(&r);
    }
}

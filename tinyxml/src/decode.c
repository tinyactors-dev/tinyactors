/*
 * Decoding: character and entity references, line ends, attribute whitespace.
 *
 * Bytes arrive in pieces, so everything that can be split by a chunk boundary
 * is carried in `parser->dec`: a '\r' waiting to swallow a following '\n', the
 * run of ']' that may become "]]>", and a reference that has seen '&' but not
 * yet ';'. The output goes to a sink, which is either the pending pool run or
 * the small buffer. Entity expansion runs on an explicit frame stack.
 */
#include "parser.h"

/* ------------------------------------------------------------------------
 * Sinks.
 */

static scan run_put(txml_parser *p, const uint8_t *bytes, uint32_t length) {
    const txml_result result = txml_doc_run_append(p->doc, &p->run, (txml_str){ (const char *)bytes, length });
    return result == TXML_OK ? SCAN_OK : txp_fail_result(p, result, p->construct);
}

static scan buffer_put(txml_parser *p, const uint8_t *bytes, uint32_t length) {
    return txp_buffer_append(p, bytes, length);
}

const sink txp_sink_run = { run_put };
const sink txp_sink_buffer = { buffer_put };

scan txp_buffer_append(txml_parser *p, const uint8_t *bytes, uint32_t length) {
    buffer *b = &p->small;

    if (length > p->limits.max_document_bytes || b->length > p->limits.max_document_bytes - length) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_DOCUMENT_BYTES, "a declaration exceeds max_document_bytes"), p->construct);
    }
    if (b->length + length > b->capacity) {
        uint64_t capacity = (uint64_t)b->capacity * 2u;
        if (capacity < 256u) {
            capacity = 256u;
        }
        if (capacity < (uint64_t)b->length + length) {
            capacity = (uint64_t)b->length + length;
        }
        if (capacity > UINT32_MAX) {
            capacity = UINT32_MAX;
        }
        uint8_t *bigger = (uint8_t *)txml_arena_alloc(&p->parser_arena, (size_t)capacity, 8u);
        if (bigger == NULL) {
            return SCAN_NOMEM;
        }
        for (uint32_t i = 0u; i < b->length; i++) {
            bigger[i] = b->data[i];
        }
        b->data = bigger;
        b->capacity = (uint32_t)capacity;
    }
    for (uint32_t i = 0u; i < length; i++) {
        b->data[b->length + i] = bytes[i];
    }
    b->length += length;
    return SCAN_OK;
}

/* The position of small.data[index], counting from where the buffer began. */
txml_position txp_small_position_at(const txml_parser *p, uint32_t index) {
    txml_position position = p->small_position;
    for (uint32_t i = 0u; i < index && i < p->small.length; i++) {
        position.offset += 1u;
        if (p->small.data[i] == '\n') {
            position.line += 1u;
            position.column = 1u;
        } else {
            position.column += 1u;
        }
    }
    return position;
}

/* ------------------------------------------------------------------------
 * References.
 */

static uint32_t encode_utf8(uint32_t code, uint8_t *out) {
    if (code < 0x80u) {
        out[0] = (uint8_t)code;
        return 1u;
    }
    if (code < 0x800u) {
        out[0] = (uint8_t)(0xC0u | (code >> 6));
        out[1] = (uint8_t)(0x80u | (code & 0x3Fu));
        return 2u;
    }
    if (code < 0x10000u) {
        out[0] = (uint8_t)(0xE0u | (code >> 12));
        out[1] = (uint8_t)(0x80u | ((code >> 6) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | (code & 0x3Fu));
        return 3u;
    }
    out[0] = (uint8_t)(0xF0u | (code >> 18));
    out[1] = (uint8_t)(0x80u | ((code >> 12) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | ((code >> 6) & 0x3Fu));
    out[3] = (uint8_t)(0x80u | (code & 0x3Fu));
    return 4u;
}

static bool is_xml_char(uint32_t code) {
    return code == 0x9u || code == 0xAu || code == 0xDu || (code >= 0x20u && code <= 0xD7FFu) ||
           (code >= 0xE000u && code <= 0xFFFDu) || (code >= 0x10000u && code <= 0x10FFFFu);
}

typedef enum { REF_BYTES, REF_ENTITY, REF_MALFORMED, REF_ENCODING, REF_UNDECLARED, REF_NAME } ref_kind;

typedef struct {
    ref_kind kind;
    uint8_t bytes[4];
    uint32_t count;
    const entity *entity;
} ref_result;

/* Parse a numeric character reference body: "#123" or "#x7B". */
static ref_kind parse_character_reference(const uint8_t *name, uint32_t length, uint32_t *code_out) {
    uint32_t code = 0u;
    uint32_t k = 1u;
    uint32_t radix = 10u;

    if (length > 1u && name[1] == 'x') {
        radix = 16u;
        k = 2u;
    }
    if (k == length) {
        return REF_MALFORMED;
    }
    for (; k < length; k++) {
        uint32_t digit;
        if (name[k] >= '0' && name[k] <= '9') {
            digit = (uint32_t)(name[k] - '0');
        } else if (radix == 16u && name[k] >= 'a' && name[k] <= 'f') {
            digit = (uint32_t)(name[k] - 'a') + 10u;
        } else if (radix == 16u && name[k] >= 'A' && name[k] <= 'F') {
            digit = (uint32_t)(name[k] - 'A') + 10u;
        } else {
            return REF_MALFORMED;
        }
        code = code * radix + digit;
        if (code > 0x10FFFFu) {
            return REF_ENCODING;
        }
    }
    if (!is_xml_char(code)) {
        return REF_ENCODING;
    }
    *code_out = code;
    return REF_BYTES;
}

charref_check txp_check_character_reference(const uint8_t *body, uint32_t length) {
    uint32_t code = 0u;
    const ref_kind kind = parse_character_reference(body, length, &code);
    if (kind == REF_BYTES) {
        return CHARREF_OK;
    }
    return kind == REF_ENCODING ? CHARREF_ENCODING : CHARREF_MALFORMED;
}

static bool is_ref_byte(uint8_t b) {
    return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '_' || b == ':' || b == '-' ||
           b == '.' || b == '#';
}

/* Classify the text between '&' and ';'. */
static void classify_reference(const txml_parser *p, txml_str text, ref_result *out) {
    const uint8_t *name = (const uint8_t *)text.ptr;
    const uint32_t length = text.length;

    out->count = 0u;
    out->entity = NULL;
    if (length == 0u) {
        out->kind = REF_MALFORMED;
    } else if (name[0] == '#') {
        uint32_t code = 0u;
        out->kind = parse_character_reference(name, length, &code);
        if (out->kind == REF_BYTES) {
            out->count = encode_utf8(code, out->bytes);
        }
    } else if (txp_bytes_are(name, length, "amp")) {
        out->kind = REF_BYTES;
        out->bytes[0] = '&';
        out->count = 1u;
    } else if (txp_bytes_are(name, length, "lt")) {
        out->kind = REF_BYTES;
        out->bytes[0] = '<';
        out->count = 1u;
    } else if (txp_bytes_are(name, length, "gt")) {
        out->kind = REF_BYTES;
        out->bytes[0] = '>';
        out->count = 1u;
    } else if (txp_bytes_are(name, length, "apos")) {
        out->kind = REF_BYTES;
        out->bytes[0] = '\'';
        out->count = 1u;
    } else if (txp_bytes_are(name, length, "quot")) {
        out->kind = REF_BYTES;
        out->bytes[0] = '"';
        out->count = 1u;
    } else if (txp_scan_name(name, 0u, length) != length) {
        out->kind = REF_NAME;
    } else {
        out->entity = txp_find_entity(p, (txml_str){ (const char *)name, length });
        out->kind = out->entity == NULL ? REF_UNDECLARED : REF_ENTITY;
    }
}

static scan fail_reference(txml_parser *p, ref_kind kind) {
    const txml_position at = p->dec.ref_position;
    switch (kind) {
    case REF_ENCODING:
        return txp_fail(p, PROBLEM(TXML_DOC_ENCODING, "character reference outside the Char production"), at);
    case REF_UNDECLARED:
        return txp_fail(p, PROBLEM(TXML_DOC_UNDECLARED_ENTITY, "undeclared entity"), at);
    case REF_NAME:
        return txp_fail(p, PROBLEM(TXML_DOC_NAME, "not a legal entity name"), at);
    default:
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "malformed reference"), at);
    }
}

/* ------------------------------------------------------------------------
 * Entity expansion.
 */

static scan push_frame(txml_parser *p, const entity *e) {
    decoder *d = &p->dec;

    if (d->level >= p->limits.max_depth) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_DEPTH, "entities nested too deeply"), d->ref_position);
    }
    for (uint32_t k = 1u; k <= d->level; k++) {
        if (p->frames[k].entity == e) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "recursive entity reference"), d->ref_position);
        }
    }
    d->level += 1u;
    p->frames[d->level] = (frame){ (const uint8_t *)e->value.ptr, e->value.length, 0u, e };
    d->expanding = true;
    return SCAN_OK;
}

/* Everything produced under an entity, nested or not, is charged. */
static scan emit_expansion(txml_parser *p, const uint8_t *bytes, uint32_t length) {
    const scan outcome = p->sink->put(p, bytes, length);
    if (outcome != SCAN_OK) {
        return outcome;
    }
    p->expansion_used += length;
    if (p->expansion_used > p->limits.entity_expansion_budget) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_ENTITY_EXPANSION, "entity expansion exceeds its budget"),
                        p->dec.ref_position);
    }
    return SCAN_OK;
}

static bool frame_special(decode_mode mode, uint8_t b) {
    switch (b) {
    case '\r':
    case '&':
    case '<':
        return true;
    case '\n':
    case '\t':
        return mode == DECODE_ATTRIBUTE;
    case ']':
        return mode == DECODE_TEXT;
    default:
        return false;
    }
}

scan txp_dec_resume(txml_parser *p) {
    decoder *d = &p->dec;

    while (d->level != 0u) {
        frame *f = &p->frames[d->level];
        if (f->position == f->length) {
            d->level -= 1u;
            continue;
        }

        /* A span of ordinary bytes goes out in one piece. */
        uint32_t stop = f->position;
        while (stop < f->length && !frame_special(d->mode, f->source[stop])) {
            stop += 1u;
        }
        if (stop > f->position) {
            const scan outcome = emit_expansion(p, f->source + f->position, stop - f->position);
            if (outcome != SCAN_OK) {
                return outcome;
            }
            f->position = stop;
            continue;
        }

        const uint8_t b = f->source[f->position];
        if (b == '\r') {
            const uint8_t out = d->mode == DECODE_ATTRIBUTE ? (uint8_t)' ' : (uint8_t)'\n';
            const scan outcome = emit_expansion(p, &out, 1u);
            if (outcome != SCAN_OK) {
                return outcome;
            }
            const bool pair = f->position + 1u < f->length && f->source[f->position + 1u] == '\n';
            f->position += pair ? 2u : 1u;
        } else if (b == '\n' || b == '\t') {
            const uint8_t out = ' ';
            const scan outcome = emit_expansion(p, &out, 1u);
            if (outcome != SCAN_OK) {
                return outcome;
            }
            f->position += 1u;
        } else if (b == '<') {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, d->mode == DECODE_ATTRIBUTE ? "'<' in an attribute value"
                                                                                    : "markup in entity replacement text is not supported"),
                            d->ref_position);
        } else if (b == ']') {
            if (f->position + 2u < f->length && f->source[f->position + 1u] == ']' && f->source[f->position + 2u] == '>') {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "']]>' in character data"), d->ref_position);
            }
            const uint8_t out = ']';
            const scan outcome = emit_expansion(p, &out, 1u);
            if (outcome != SCAN_OK) {
                return outcome;
            }
            f->position += 1u;
        } else { /* '&' */
            uint32_t semicolon = f->position + 1u;
            while (semicolon < f->length && f->source[semicolon] != ';') {
                semicolon += 1u;
            }
            if (semicolon == f->length) {
                return fail_reference(p, REF_MALFORMED);
            }
            ref_result ref;
            classify_reference(p, (txml_str){ (const char *)f->source + f->position + 1u, semicolon - f->position - 1u }, &ref);
            if (ref.kind == REF_BYTES) {
                const scan outcome = emit_expansion(p, ref.bytes, ref.count);
                if (outcome != SCAN_OK) {
                    return outcome;
                }
                f->position = semicolon + 1u;
            } else if (ref.kind == REF_ENTITY) {
                f->position = semicolon + 1u;
                const scan outcome = push_frame(p, ref.entity);
                if (outcome != SCAN_OK) {
                    return outcome;
                }
            } else {
                return fail_reference(p, ref.kind);
            }
        }
    }
    d->expanding = false;
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * The decoder proper.
 */

void txp_dec_reset(txml_parser *p, decode_mode mode) {
    p->dec = (decoder){ .mode = mode };
}

scan txp_dec_end(txml_parser *p) {
    if (p->dec.in_ref) {
        return fail_reference(p, REF_MALFORMED);
    }
    return SCAN_OK;
}

static bool special(decode_mode mode, uint8_t b) {
    switch (mode) {
    case DECODE_VERBATIM:
        return b == '\r';
    case DECODE_ATTRIBUTE:
        return b == '\r' || b == '\n' || b == '\t' || b == '&' || b == '<';
    default:
        return b == '\r' || b == '&' || b == ']' || b == '>';
    }
}

/* The position of a byte the decoder is looking at: in the window when it
 * came from there, else wherever the construct began. The cursor keeps a run
 * of references in one span from rescanning the span for each. */
static txml_position source_position(const txml_parser *p, position_cursor *cursor, const uint8_t *at) {
    const uintptr_t address = (uintptr_t)at;
    const uintptr_t low = (uintptr_t)p->window;
    if (address >= low && address < low + p->end) {
        return txp_cursor_position(p, cursor, (uint32_t)(address - low));
    }
    return p->construct;
}

/* The ';' of a reference arrived. Returns SCAN_OK when the reference is
 * fully handled. On SCAN_NOMEM, `in_ref` still set means the ';' was not
 * consumed; cleared means an expansion is pending and it was. */
static scan finish_reference(txml_parser *p) {
    decoder *d = &p->dec;
    ref_result ref;

    classify_reference(p, (txml_str){ (const char *)p->reference, d->ref_length }, &ref);
    if (ref.kind == REF_BYTES) {
        const scan outcome = p->sink->put(p, ref.bytes, ref.count);
        if (outcome == SCAN_OK) {
            d->in_ref = false;
        }
        return outcome;
    }
    if (ref.kind == REF_ENTITY) {
        const scan outcome = push_frame(p, ref.entity);
        if (outcome != SCAN_OK) {
            return outcome;
        }
        d->in_ref = false;
        return txp_dec_resume(p);
    }
    return fail_reference(p, ref.kind);
}

scan txp_dec_feed(txml_parser *p, const dec_input *input, uint32_t *consumed) {
    decoder *d = &p->dec;
    const uint8_t *s = input->bytes;
    const uint32_t n = input->length;
    uint32_t i = 0u;
    uint32_t from = 0u; /* the start of the ordinary bytes not yet emitted */
    position_cursor cursor = { 0u, { 0u, 0u, 0u }, false };
    scan outcome;

    *consumed = 0u;
    if (d->expanding) {
        outcome = txp_dec_resume(p);
        if (outcome != SCAN_OK) {
            return outcome;
        }
    }
    if (d->brackets != 0u && n != 0u && s[0] != ']' && s[0] != '>') {
        d->brackets = 0u;
    }

    while (i < n) {
        const uint8_t b = s[i];

        if (d->in_ref) {
            if (b == ';') {
                outcome = finish_reference(p);
                if (outcome == SCAN_OK) {
                    i += 1u;
                    from = i;
                    continue;
                }
                *consumed = (outcome == SCAN_NOMEM && !d->in_ref) ? i + 1u : i;
                return outcome;
            }
            /* The characters are checked as a whole name when the ';' arrives;
             * here only what could never be part of one is refused. */
            if (d->ref_length >= p->reference_capacity || !(b >= 0x80u || is_ref_byte(b))) {
                return fail_reference(p, REF_MALFORMED);
            }
            p->reference[d->ref_length++] = b;
            i += 1u;
            from = i;
            continue;
        }
        if (d->after_cr) {
            d->after_cr = false;
            if (b == '\n') {
                i += 1u;
                from = i;
                continue;
            }
        }
        if (!special(d->mode, b)) {
            i += 1u;
            continue;
        }

        if (b == ']') {
            /* Take the whole run of ']' with the ordinary bytes before it. */
            uint32_t j = i;
            while (j < n && s[j] == ']') {
                j += 1u;
            }
            outcome = p->sink->put(p, s + from, j - from);
            if (outcome != SCAN_OK) {
                *consumed = from;
                return outcome;
            }
            d->brackets += j - i;
            if (d->brackets > 2u) {
                d->brackets = 2u;
            }
            i = j;
            from = i;
            if (i < n && s[i] != '>') {
                d->brackets = 0u;
            }
            continue;
        }

        if (i > from) {
            outcome = p->sink->put(p, s + from, i - from);
            if (outcome != SCAN_OK) {
                *consumed = from;
                return outcome;
            }
            from = i;
        }

        if (b == '>') {
            if (d->brackets >= 2u) {
                txml_position at = source_position(p, &cursor, s + i);
                at.offset -= 2u;
                at.column -= 2u;
                *consumed = i;
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "']]>' in character data"), at);
            }
            d->brackets = 0u;
            i += 1u; /* an ordinary byte; it goes out with the next span */
        } else if (b == '&') {
            d->in_ref = true;
            d->ref_length = 0u;
            d->ref_position = source_position(p, &cursor, s + i);
            d->brackets = 0u;
            i += 1u;
            from = i;
        } else if (b == '<') {
            *consumed = i;
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "'<' in an attribute value"), source_position(p, &cursor, s + i));
        } else { /* '\r', or attribute whitespace */
            const uint8_t out = (d->mode == DECODE_ATTRIBUTE || b == '\t') ? (uint8_t)' ' : (uint8_t)'\n';
            outcome = p->sink->put(p, &out, 1u);
            if (outcome != SCAN_OK) {
                *consumed = i;
                return outcome;
            }
            d->after_cr = b == '\r';
            d->brackets = 0u;
            i += 1u;
            from = i;
        }
    }

    if (i > from) {
        outcome = p->sink->put(p, s + from, i - from);
        if (outcome != SCAN_OK) {
            *consumed = from;
            return outcome;
        }
    }
    *consumed = n;
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * Entity literals: character references are read when the entity is declared.
 */

/* Where in the small buffer `at` points, for an error position. */
static txml_position literal_position(const txml_parser *p, const uint8_t *at) {
    if (p->small.data != NULL && at >= p->small.data && at < p->small.data + p->small.length) {
        return txp_small_position_at(p, (uint32_t)(at - p->small.data));
    }
    return p->construct;
}

scan txp_expand_character_references(txml_parser *p, txml_str literal, txml_str *expanded) {
    const uint8_t *s = (const uint8_t *)literal.ptr;
    uint8_t *out = literal.length == 0u ? NULL : (uint8_t *)txml_arena_alloc(&p->parser_arena, literal.length, 1u);
    uint32_t n = 0u;
    uint32_t i = 0u;

    if (literal.length != 0u && out == NULL) {
        return SCAN_NOMEM;
    }
    while (i < literal.length) {
        if (s[i] != '&' || i + 1u >= literal.length || s[i + 1u] != '#') {
            out[n++] = s[i++];
            continue;
        }
        uint32_t semicolon = i + 2u;
        while (semicolon < literal.length && s[semicolon] != ';') {
            semicolon += 1u;
        }
        uint32_t code = 0u;
        if (semicolon == literal.length) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "malformed character reference"), literal_position(p, s + i));
        }
        const ref_kind kind = parse_character_reference(s + i + 1u, semicolon - i - 1u, &code);
        if (kind == REF_ENCODING) {
            return txp_fail(p, PROBLEM(TXML_DOC_ENCODING, "character reference outside the Char production"),
                            literal_position(p, s + i));
        }
        if (kind != REF_BYTES) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "malformed character reference"), literal_position(p, s + i));
        }
        /* A reference is never longer than the bytes it stands for. */
        n += encode_utf8(code, out + n);
        i = semicolon + 1u;
    }
    *expanded = (txml_str){ out == NULL ? "" : (const char *)out, n };
    return SCAN_OK;
}

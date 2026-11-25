#include "cli/report.h"

#include "base/format.h"
#include "base/memory.h"
#include <tinyactors/cbor.h>

/* What a node is. */
typedef enum report_kind {
    REPORT_OBJECT = 0,
    REPORT_LIST = 1,
    REPORT_TEXT = 2,
    REPORT_NUMBER = 3,
    REPORT_BOOLEAN = 4,
    REPORT_BLOCK = 5,
    REPORT_VALUE = 6 /* one CBOR item; `text` its diagnostic notation */
} report_kind;

typedef enum report_constants {
    REPORT_NODE_ALIGN = 8,
    REPORT_INDENT = 2,      /* spaces per level, JSON and XML */
    REPORT_HEADING_MAX = 6, /* Markdown has six levels of heading */
    REPORT_FENCE_MIN = 3    /* the shortest code fence */
} report_constants;

struct ta_report_node {
    uint32_t kind;
    ta_string_view name;     /* a member's name; an item's is its list's item name */
    ta_string_view item;     /* a list's: the name of its items */
    ta_string_view text;     /* a text's value, a block's text */
    ta_string_view language; /* a block's */
    uint64_t number;         /* a number's value; a boolean's, 0 or 1 */
    ta_bytes value;          /* a value's CBOR */
    ta_report_node *first, *last, *next;
};

static const char report_hex_digits[] = "0123456789ABCDEF";

/* ---- building ---------------------------------------------------------- */

static ta_report_node *report_node_new(ta_report *report, uint32_t kind, ta_string_view name) {
    ta_report_node *node =
        (ta_report_node *)ta_growable_arena_alloc(report->arena, sizeof *node, REPORT_NODE_ALIGN);
    if (!node) {
        report->status = TA_ERROR_NO_MEMORY;
        return NULL;
    }
    ta_memset(node, 0, sizeof *node);
    node->kind = kind;
    node->name = name;
    return node;
}

/* A member of the innermost open object, or an item of the open list. */
static ta_report_node *report_add(ta_report *report, uint32_t kind, ta_string_view name) {
    if (report->status != TA_OK) return NULL;
    ta_report_node *parent = report->open[report->depth - 1];
    ta_report_node *node =
        report_node_new(report, kind, parent->kind == REPORT_LIST ? parent->item : name);
    if (!node) return NULL;
    if (parent->last)
        parent->last->next = node;
    else
        parent->first = node;
    parent->last = node;
    return node;
}

/* Open a container: TA_ERROR_STATE past TA_REPORT_MAX_DEPTH. */
static ta_report_node *report_open(ta_report *report, uint32_t kind, ta_string_view name) {
    if (report->status != TA_OK) return NULL;
    if (report->depth >= TA_REPORT_MAX_DEPTH) {
        report->status = TA_ERROR_STATE;
        return NULL;
    }
    ta_report_node *node = report_add(report, kind, name);
    if (node) report->open[report->depth++] = node;
    return node;
}

void ta_report_init(ta_report *report, ta_growable_arena *arena, ta_string_view title) {
    report->arena = arena;
    report->root = NULL;
    report->depth = 0;
    report->status = TA_OK;
    report->root = report_node_new(report, REPORT_OBJECT, title);
    if (report->root) {
        report->open[0] = report->root;
        report->depth = 1;
    }
}

void ta_report_object(ta_report *report, ta_string_view name) {
    report_open(report, REPORT_OBJECT, name);
}

void ta_report_list(ta_report *report, ta_report_list_names names) {
    ta_report_node *node = report_open(report, REPORT_LIST, names.name);
    if (node) node->item = names.item;
}

void ta_report_end(ta_report *report) {
    if (report->status != TA_OK) return;
    if (report->depth <= 1) {
        report->status = TA_ERROR_STATE;
        return;
    }
    report->depth--;
}

void ta_report_text(ta_report *report, ta_string_view name, ta_string_view value) {
    ta_report_node *node = report_add(report, REPORT_TEXT, name);
    if (node) node->text = value;
}

void ta_report_number(ta_report *report, ta_string_view name, uint64_t value) {
    ta_report_node *node = report_add(report, REPORT_NUMBER, name);
    if (node) node->number = value;
}

void ta_report_boolean(ta_report *report, ta_string_view name, int value) {
    ta_report_node *node = report_add(report, REPORT_BOOLEAN, name);
    if (node) node->number = value ? 1 : 0;
}

void ta_report_block(ta_report *report, ta_string_view name, ta_report_code code) {
    ta_report_node *node = report_add(report, REPORT_BLOCK, name);
    if (!node) return;
    node->language = code.language;
    node->text = code.text;
}

void ta_report_value(ta_report *report, ta_string_view name, ta_bytes cbor) {
    if (report->status != TA_OK) return;
    ta_buffer probe = {(void *)0, 0, 0};
    ta_status status = ta_cbor_diagnostic(cbor, &probe);
    if (status != TA_ERROR_NO_MEMORY) { /* an empty text is no item either */
        report->status = TA_ERROR_INVALID;
        return;
    }
    char *text = (char *)ta_growable_arena_alloc(report->arena, probe.length, 1);
    if (!text) {
        report->status = TA_ERROR_NO_MEMORY;
        return;
    }
    ta_buffer room = {text, probe.length, 0};
    if (ta_cbor_diagnostic(cbor, &room) != TA_OK) {
        report->status = TA_ERROR_INVALID;
        return;
    }
    ta_report_node *node = report_add(report, REPORT_VALUE, name);
    if (!node) return;
    node->text = (ta_string_view){text, (uint32_t)room.length};
    node->value = cbor;
}

ta_string_view ta_report_copy(ta_report *report, ta_string_view text) {
    ta_string_view copy = {"", 0};
    if (report->status != TA_OK || text.length == 0) return copy;
    char *bytes = (char *)ta_growable_arena_alloc(report->arena, text.length, 1);
    if (!bytes) {
        report->status = TA_ERROR_NO_MEMORY;
        return copy;
    }
    ta_memcpy(bytes, text.pointer, text.length);
    copy.pointer = bytes;
    copy.length = text.length;
    return copy;
}

/* ---- writing ----------------------------------------------------------- */

static void put(ta_output *out, ta_string_view text) {
    if (text.length) ta_output_write(out, text.pointer, text.length);
}

static void put_literal(ta_output *out, const char *text) {
    put(out, ta_string(text));
}

static void put_byte(ta_output *out, char byte) {
    ta_output_write(out, &byte, 1);
}

static void put_repeat(ta_output *out, char byte, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        put_byte(out, byte);
}

static void put_number(ta_output *out, uint64_t value) {
    char digits[TA_FORMAT_U64_MAX];
    uint32_t length = ta_format_u64(digits, value);
    ta_output_write(out, digits, length);
}

/* The bytes of `text` from `start` up to `end`. */
typedef struct report_span {
    ta_string_view text;
    uint32_t start, end;
} report_span;

static void put_span(ta_output *out, report_span span) {
    if (span.end > span.start)
        ta_output_write(out, span.text.pointer + span.start, span.end - span.start);
}

/* A scalar: one line of Markdown, one JSON value without members of the
 * report's own. A value is one, but not an XML attribute (xml_open). */
static int report_is_scalar(const ta_report_node *node) {
    return node->kind == REPORT_TEXT || node->kind == REPORT_NUMBER ||
           node->kind == REPORT_BOOLEAN || node->kind == REPORT_VALUE;
}

static int report_same_name(ta_string_view a, ta_string_view b) {
    return a.length == b.length && ta_memcmp(a.pointer, b.pointer, a.length) == 0;
}

/* ---- values: a CBOR item walked without recursion ------------------------ */

/* An open container of the item: what it still holds, and the place of its
 * next item (in a map, keys at even places, values at odd ones). */
typedef struct value_frame {
    uint64_t remaining;
    uint32_t kind; /* TA_CBOR_ARRAY, TA_CBOR_MAP or TA_CBOR_TAG */
    uint32_t position;
} value_frame;

typedef struct value_walk {
    ta_cbor_reader reader;
    value_frame stack[TA_CBOR_DEPTH_MAX + 1];
    uint32_t depth;
} value_walk;

typedef enum value_step_kind {
    VALUE_END = 0,  /* the item is read */
    VALUE_ITEM = 1, /* an item (a container opens with it) */
    VALUE_CLOSE = 2 /* the innermost container ends */
} value_step_kind;

typedef struct value_step {
    uint32_t kind;      /* value_step_kind */
    uint32_t parent;    /* the container it is in (TA_CBOR_*), 0 at the top */
    uint32_t position;  /* its place there */
    uint32_t depth;     /* containers around it; a closing one's own depth */
    uint32_t container; /* VALUE_CLOSE: the kind that ends */
    ta_cbor_item item;
} value_step;

static void value_walk_init(value_walk *walk, ta_bytes cbor) {
    ta_cbor_reader_init(&walk->reader, cbor);
    walk->depth = 0;
}

static int value_is_key(const value_step *step) {
    return step->parent == TA_CBOR_MAP && step->position % 2 == 0;
}

static void value_next(value_walk *walk, value_step *step) {
    ta_memset(step, 0, sizeof *step);
    if (walk->depth && walk->stack[walk->depth - 1].remaining == 0) {
        walk->depth--;
        step->kind = VALUE_CLOSE;
        step->container = walk->stack[walk->depth].kind;
        step->depth = walk->depth;
        if (walk->depth) step->parent = walk->stack[walk->depth - 1].kind;
        return;
    }
    if (ta_cbor_read(&walk->reader, &step->item) != 1) return; /* VALUE_END */
    step->kind = VALUE_ITEM;
    step->depth = walk->depth;
    if (walk->depth) {
        value_frame *parent = &walk->stack[walk->depth - 1];
        step->parent = parent->kind;
        step->position = parent->position++;
        parent->remaining--;
    }
    uint32_t kind = step->item.kind;
    if ((kind != TA_CBOR_ARRAY && kind != TA_CBOR_MAP && kind != TA_CBOR_TAG) ||
        walk->depth > TA_CBOR_DEPTH_MAX)
        return;
    value_frame *frame = &walk->stack[walk->depth++];
    frame->kind = kind;
    frame->position = 0;
    frame->remaining = kind == TA_CBOR_TAG   ? 1u
                       : kind == TA_CBOR_MAP ? (uint64_t)step->item.count * 2u
                                             : step->item.count;
}

/* 1 when every key of every map is text or an integer: JSON and XML write
 * the item as values; otherwise as its diagnostic notation. */
static int value_keys_plain(ta_bytes cbor) {
    value_walk walk;
    value_step step;
    value_walk_init(&walk, cbor);
    for (value_next(&walk, &step); step.kind != VALUE_END; value_next(&walk, &step)) {
        uint32_t kind = step.item.kind;
        if (step.kind == VALUE_ITEM && value_is_key(&step) && kind != TA_CBOR_TEXT &&
            kind != TA_CBOR_UINT && kind != TA_CBOR_NEGINT)
            return 0;
    }
    return 1;
}

/* An integer in decimal: -1 - n for a negative one. */
static void put_integer(ta_output *out, const ta_cbor_item *item) {
    if (item->kind == TA_CBOR_UINT) {
        char digits[TA_FORMAT_U64_MAX];
        ta_output_write(out, digits, ta_format_u64(digits, item->value));
        return;
    }
    ta_output_write(out, "-", 1);
    if (item->value == UINT64_MAX) { /* -2^64, past 64 bits */
        ta_output_write(out, "18446744073709551616", 20);
        return;
    }
    char digits[TA_FORMAT_U64_MAX];
    ta_output_write(out, digits, ta_format_u64(digits, item->value + 1u));
}

/* A float as diagnostic notation writes it, the shortest decimal that
 * reads back as it: the double encoded alone and written so. */
typedef struct value_float_text {
    char text[40];
    uint32_t length;
} value_float_text;

static void value_float(uint64_t bits, value_float_text *out) {
    unsigned char encoded[9];
    encoded[0] = 0xFB; /* major 7, a double */
    for (uint32_t i = 0; i < 8; i++)
        encoded[1 + i] = (unsigned char)(bits >> (56 - 8 * i));
    ta_buffer room = {out->text, sizeof out->text, 0};
    out->length = ta_cbor_diagnostic((ta_bytes){encoded, sizeof encoded}, &room) == TA_OK
                      ? (uint32_t)room.length
                      : 0;
}

/* NaN, Infinity and -Infinity, which JSON has no number for. */
static int value_float_is_number(const value_float_text *number) {
    char first = number->length ? number->text[0] : 'N';
    return first != 'N' && first != 'I' && !(first == '-' && number->length > 1 && number->text[1] == 'I');
}

static void put_hexadecimal(ta_output *out, ta_bytes bytes) {
    static const char lower[] = "0123456789abcdef";
    const unsigned char *pointer = (const unsigned char *)bytes.pointer;
    for (size_t i = 0; i < bytes.length; i++) {
        char pair[2] = {lower[pointer[i] >> 4], lower[pointer[i] & 15u]};
        ta_output_write(out, pair, 2);
    }
}

/* ---- JSON -------------------------------------------------------------- */

static void json_string(ta_output *out, ta_string_view text) {
    report_span span = {text, 0, 0};
    put_byte(out, '"');
    for (uint32_t i = 0; i < text.length; i++) {
        unsigned char byte = (unsigned char)text.pointer[i];
        if (byte != '"' && byte != '\\' && byte >= 0x20) continue;
        span.end = i;
        put_span(out, span);
        span.start = i + 1;
        if (byte == '"')
            put_literal(out, "\\\"");
        else if (byte == '\\')
            put_literal(out, "\\\\");
        else if (byte == '\n')
            put_literal(out, "\\n");
        else if (byte == '\t')
            put_literal(out, "\\t");
        else if (byte == '\r')
            put_literal(out, "\\r");
        else {
            put_literal(out, "\\u00");
            put_byte(out, report_hex_digits[byte >> 4]);
            put_byte(out, report_hex_digits[byte & 0xF]);
        }
    }
    span.end = text.length;
    put_span(out, span);
    put_byte(out, '"');
}

typedef struct report_frame {
    const ta_report_node *node, *cursor;
    uint32_t level, index, scalar_run;
} report_frame;

static void report_frame_set(report_frame *frame, const ta_report_node *node, uint32_t level) {
    frame->node = node;
    frame->cursor = node->first;
    frame->level = level;
    frame->index = 0;
    frame->scalar_run = 0;
}

/* A scalar item of a value as JSON (report.h, ta_report_value). */
static void json_scalar(ta_output *out, const ta_cbor_item *item) {
    switch (item->kind) {
    case TA_CBOR_UINT:
    case TA_CBOR_NEGINT:
        put_integer(out, item);
        break;
    case TA_CBOR_TEXT:
        json_string(out, (ta_string_view){(const char *)item->bytes.pointer, (uint32_t)item->bytes.length});
        break;
    case TA_CBOR_BYTES:
        put_literal(out, "{\"bytes\": \"");
        put_hexadecimal(out, item->bytes);
        put_literal(out, "\"}");
        break;
    case TA_CBOR_FLOAT: {
        value_float_text number;
        value_float(item->value, &number);
        int plain = value_float_is_number(&number);
        if (!plain) put_literal(out, "{\"float\": \"");
        ta_output_write(out, number.text, number.length);
        if (!plain) put_literal(out, "\"}");
        break;
    }
    default: /* a simple value */
        if (item->value == TA_CBOR_FALSE) put_literal(out, "false");
        else if (item->value == TA_CBOR_TRUE) put_literal(out, "true");
        else if (item->value == TA_CBOR_NULL) put_literal(out, "null");
        else if (item->value == TA_CBOR_UNDEFINED) put_literal(out, "{\"undefined\": true}");
        else {
            put_literal(out, "{\"simple\": ");
            put_number(out, item->value);
            put_byte(out, '}');
        }
        break;
    }
}

static void json_cbor(ta_output *out, const ta_report_node *node) {
    if (!value_keys_plain(node->value)) {
        put_literal(out, "{\"diagnostic\": ");
        json_string(out, node->text);
        put_byte(out, '}');
        return;
    }
    value_walk walk;
    value_step step;
    value_walk_init(&walk, node->value);
    for (value_next(&walk, &step); step.kind != VALUE_END; value_next(&walk, &step)) {
        if (step.kind == VALUE_CLOSE) {
            put_byte(out, step.container == TA_CBOR_ARRAY ? ']' : '}');
            continue;
        }
        if ((step.parent == TA_CBOR_ARRAY || step.parent == TA_CBOR_MAP) && step.position)
            put_literal(out, step.parent == TA_CBOR_MAP && step.position % 2 ? ": " : ", ");
        if (value_is_key(&step)) { /* text, or an integer written as text */
            if (step.item.kind == TA_CBOR_TEXT) {
                json_scalar(out, &step.item);
            } else {
                put_byte(out, '"');
                put_integer(out, &step.item);
                put_byte(out, '"');
            }
            continue;
        }
        if (step.item.kind == TA_CBOR_ARRAY) put_byte(out, '[');
        else if (step.item.kind == TA_CBOR_MAP) put_byte(out, '{');
        else if (step.item.kind == TA_CBOR_TAG) {
            put_literal(out, "{\"tag\": ");
            put_number(out, step.item.value);
            put_literal(out, ", \"value\": ");
        } else json_scalar(out, &step.item);
    }
}

static void json_value(ta_output *out, const ta_report_node *node) {
    switch (node->kind) {
    case REPORT_VALUE:
        json_cbor(out, node);
        break;
    case REPORT_NUMBER:
        put_number(out, node->number);
        break;
    case REPORT_BOOLEAN:
        put_literal(out, node->number ? "true" : "false");
        break;
    case REPORT_OBJECT:
        put_literal(out, node->first ? "{" : "{}");
        break;
    case REPORT_LIST:
        put_literal(out, node->first ? "[" : "[]");
        break;
    default:
        json_string(out, node->text);
        break; /* a text, a block */
    }
}

static void json_render(const ta_report *report, ta_output *out) {
    report_frame stack[TA_REPORT_MAX_DEPTH];
    uint32_t count = 0;
    json_value(out, report->root);
    if (report->root->first) report_frame_set(&stack[count++], report->root, 0);
    while (count > 0) {
        report_frame *frame = &stack[count - 1];
        const ta_report_node *child = frame->cursor;
        if (!child) {
            put_byte(out, '\n');
            put_repeat(out, ' ', (count - 1) * REPORT_INDENT);
            put_byte(out, frame->node->kind == REPORT_LIST ? ']' : '}');
            count--;
            continue;
        }
        frame->cursor = child->next;
        if (child != frame->node->first) put_byte(out, ',');
        put_byte(out, '\n');
        put_repeat(out, ' ', count * REPORT_INDENT);
        if (frame->node->kind == REPORT_OBJECT) {
            json_string(out, child->name);
            put_literal(out, ": ");
        }
        json_value(out, child);
        if ((child->kind == REPORT_OBJECT || child->kind == REPORT_LIST) && child->first &&
            count < TA_REPORT_MAX_DEPTH)
            report_frame_set(&stack[count++], child, 0);
    }
    put_byte(out, '\n');
}

/* ---- XML --------------------------------------------------------------- */

/* Text or an attribute's value. NUL cannot appear in XML: dropped. In an
 * attribute, tab, newline and carriage return are references too, which
 * attribute normalization would otherwise turn into spaces. */
static void xml_escape(ta_output *out, ta_string_view text, int attribute) {
    report_span span = {text, 0, 0};
    for (uint32_t i = 0; i < text.length; i++) {
        unsigned char byte = (unsigned char)text.pointer[i];
        int plain_space = !attribute && (byte == '\t' || byte == '\n' || byte == '\r');
        if (byte != '&' && byte != '<' && byte != '>' && byte != '"' &&
            (byte >= 0x20 || plain_space))
            continue;
        span.end = i;
        put_span(out, span);
        span.start = i + 1;
        if (byte == '&')
            put_literal(out, "&amp;");
        else if (byte == '<')
            put_literal(out, "&lt;");
        else if (byte == '>')
            put_literal(out, "&gt;");
        else if (byte == '"')
            put_literal(out, "&quot;");
        else if (byte != 0) {
            put_literal(out, "&#x");
            if (byte >= 0x10) put_byte(out, report_hex_digits[byte >> 4]);
            put_byte(out, report_hex_digits[byte & 0xF]);
            put_byte(out, ';');
        }
    }
    span.end = text.length;
    put_span(out, span);
}

static void xml_value(ta_output *out, const ta_report_node *node, int attribute) {
    if (node->kind == REPORT_NUMBER)
        put_number(out, node->number);
    else if (node->kind == REPORT_BOOLEAN)
        put_literal(out, node->number ? "true" : "false");
    else
        xml_escape(out, node->text, attribute);
}

/* An object's scalar but a value: one of its attributes. */
static int xml_is_attribute(const ta_report_node *node) {
    return report_is_scalar(node) && node->kind != REPORT_VALUE;
}

/* A scalar item of a value as its typed element (report.h, ta_report_value). */
static void xml_scalar(ta_output *out, const ta_cbor_item *item) {
    switch (item->kind) {
    case TA_CBOR_UINT:
    case TA_CBOR_NEGINT:
        put_literal(out, "<integer>");
        put_integer(out, item);
        put_literal(out, "</integer>");
        break;
    case TA_CBOR_TEXT:
        put_literal(out, "<text>");
        xml_escape(out, (ta_string_view){(const char *)item->bytes.pointer, (uint32_t)item->bytes.length}, 0);
        put_literal(out, "</text>");
        break;
    case TA_CBOR_BYTES:
        put_literal(out, "<bytes>");
        put_hexadecimal(out, item->bytes);
        put_literal(out, "</bytes>");
        break;
    case TA_CBOR_FLOAT: {
        value_float_text number;
        value_float(item->value, &number);
        put_literal(out, "<float>");
        ta_output_write(out, number.text, number.length);
        put_literal(out, "</float>");
        break;
    }
    default:
        if (item->value == TA_CBOR_FALSE) put_literal(out, "<false/>");
        else if (item->value == TA_CBOR_TRUE) put_literal(out, "<true/>");
        else if (item->value == TA_CBOR_NULL) put_literal(out, "<null/>");
        else if (item->value == TA_CBOR_UNDEFINED) put_literal(out, "<undefined/>");
        else {
            put_literal(out, "<simple>");
            put_number(out, item->value);
            put_literal(out, "</simple>");
        }
        break;
    }
}

/* A value's elements, one a line, from `indent`: a map's entries hold their
 * values one level in. */
static void xml_cbor(ta_output *out, const ta_report_node *node, uint32_t indent) {
    if (!value_keys_plain(node->value)) {
        put_repeat(out, ' ', indent);
        put_literal(out, "<diagnostic>");
        xml_escape(out, node->text, 0);
        put_literal(out, "</diagnostic>\n");
        return;
    }
    value_walk walk;
    value_step step;
    uint32_t entries = 0; /* map entries open: each indents its value */
    value_walk_init(&walk, node->value);
    for (value_next(&walk, &step); step.kind != VALUE_END; value_next(&walk, &step)) {
        uint32_t at = indent + (step.depth + entries) * REPORT_INDENT;
        if (step.kind == VALUE_CLOSE) {
            put_repeat(out, ' ', at);
            put_literal(out, step.container == TA_CBOR_ARRAY ? "</array>\n"
                             : step.container == TA_CBOR_MAP ? "</map>\n"
                                                             : "</tag>\n");
        } else if (value_is_key(&step)) {
            put_repeat(out, ' ', at);
            put_literal(out, "<entry key=\"");
            if (step.item.kind == TA_CBOR_TEXT)
                xml_escape(out, (ta_string_view){(const char *)step.item.bytes.pointer, (uint32_t)step.item.bytes.length}, 1);
            else
                put_integer(out, &step.item);
            put_literal(out, "\">\n");
            entries++;
            continue;
        } else {
            put_repeat(out, ' ', at);
            if (step.item.kind == TA_CBOR_ARRAY) put_literal(out, "<array>\n");
            else if (step.item.kind == TA_CBOR_MAP) put_literal(out, "<map>\n");
            else if (step.item.kind == TA_CBOR_TAG) {
                put_literal(out, "<tag number=\"");
                put_number(out, step.item.value);
                put_literal(out, "\">\n");
            } else {
                xml_scalar(out, &step.item);
                put_byte(out, '\n');
            }
            uint32_t kind = step.item.kind;
            if (kind == TA_CBOR_ARRAY || kind == TA_CBOR_MAP || kind == TA_CBOR_TAG) continue;
        }
        /* an item ended: a map's value ends its entry */
        if (step.parent == TA_CBOR_MAP && entries) {
            entries--;
            put_repeat(out, ' ', indent + (step.depth + entries) * REPORT_INDENT);
            put_literal(out, "</entry>\n");
        }
    }
}

/* A scalar item or a block: <name>text</name>; a value: its elements
 * inside <name>. */
static void xml_leaf(ta_output *out, const ta_report_node *node, uint32_t indent) {
    if (node->kind == REPORT_VALUE) {
        put_repeat(out, ' ', indent);
        put_byte(out, '<');
        put(out, node->name);
        put_literal(out, ">\n");
        xml_cbor(out, node, indent + REPORT_INDENT);
        put_repeat(out, ' ', indent);
        put_literal(out, "</");
        put(out, node->name);
        put_literal(out, ">\n");
        return;
    }
    put_repeat(out, ' ', indent);
    put_byte(out, '<');
    put(out, node->name);
    put_byte(out, '>');
    xml_value(out, node, 0);
    put_literal(out, "</");
    put(out, node->name);
    put_literal(out, ">\n");
}

/* An element's children: an object's are its members but its scalars (its
 * attributes); a list's are all its items. */
static const ta_report_node *xml_next_child(const ta_report_node *parent,
                                            const ta_report_node *child) {
    if (parent->kind == REPORT_OBJECT)
        while (child && xml_is_attribute(child))
            child = child->next;
    return child;
}

/* The start tag of an object or a list. 1: it has children, and is left open. */
static int xml_open(ta_output *out, const ta_report_node *node, uint32_t indent) {
    put_repeat(out, ' ', indent);
    put_byte(out, '<');
    put(out, node->name);
    if (node->kind == REPORT_OBJECT)
        for (const ta_report_node *member = node->first; member; member = member->next) {
            if (!xml_is_attribute(member)) continue;
            put_byte(out, ' ');
            put(out, member->name);
            put_literal(out, "=\"");
            xml_value(out, member, 1);
            put_byte(out, '"');
        }
    if (!xml_next_child(node, node->first)) {
        put_literal(out, "/>\n");
        return 0;
    }
    put_literal(out, ">\n");
    return 1;
}

static void xml_render(const ta_report *report, ta_output *out) {
    report_frame stack[TA_REPORT_MAX_DEPTH];
    uint32_t count = 0;
    put_literal(out, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    if (xml_open(out, report->root, 0)) report_frame_set(&stack[count++], report->root, 0);
    while (count > 0) {
        report_frame *frame = &stack[count - 1];
        const ta_report_node *child = xml_next_child(frame->node, frame->cursor);
        if (!child) {
            put_repeat(out, ' ', (count - 1) * REPORT_INDENT);
            put_literal(out, "</");
            put(out, frame->node->name);
            put_literal(out, ">\n");
            count--;
            continue;
        }
        frame->cursor = child->next;
        if (child->kind != REPORT_OBJECT && child->kind != REPORT_LIST)
            xml_leaf(out, child, count * REPORT_INDENT);
        else if (xml_open(out, child, count * REPORT_INDENT) && count < TA_REPORT_MAX_DEPTH)
            report_frame_set(&stack[count++], child, 0);
    }
}

/* ---- Markdown ---------------------------------------------------------- */

typedef struct markdown {
    ta_output *out;
    uint32_t started; /* 1 once a block was written: the next is set apart by a blank line */
} markdown;

static void markdown_block(markdown *document) {
    if (document->started) put_byte(document->out, '\n');
    document->started = 1;
}

/* "bootActors" as "Boot actors"; an acronym (a run of capitals, as the
 * repository spells them: "sessionID", "otlpExporter") stays in capitals:
 * "Session ID". */
static int is_upper(char byte) {
    return byte >= 'A' && byte <= 'Z';
}

/* camelCase writes an acronym that starts a name in lowercase ("abi",
 * "idPage"), where its letters cannot tell it from a word: these are known
 * (the repository's acronyms.txt, those a report names). */
static const char *const report_acronyms[] = {"abi", "api", "cbor", "http", "id", "io", "json",
                                              "otlp", "scxml", "sdk", "uri", "url", "xml"};

/* The length of the first word of `name` when it is a known acronym. */
static uint32_t leading_acronym(ta_string_view name) {
    uint32_t length = 0;
    while (length < name.length && !is_upper(name.pointer[length]))
        length++;
    for (uint32_t i = 0; i < sizeof report_acronyms / sizeof report_acronyms[0]; i++) {
        const char *acronym = report_acronyms[i];
        uint32_t j = 0;
        while (j < length && acronym[j] == name.pointer[j])
            j++;
        if (j == length && !acronym[j]) return length;
    }
    return 0;
}

static void markdown_words(ta_output *out, ta_string_view name) {
    uint32_t leading = leading_acronym(name);
    for (uint32_t i = 0; i < leading; i++)
        put_byte(out, (char)(name.pointer[i] - 'a' + 'A'));
    for (uint32_t i = leading; i < name.length; i++) {
        char byte = name.pointer[i];
        int upper = is_upper(byte), previous_upper = i && is_upper(name.pointer[i - 1]);
        int next_upper = i + 1 < name.length && is_upper(name.pointer[i + 1]);
        int next_lower = i + 1 < name.length && name.pointer[i + 1] >= 'a' && name.pointer[i + 1] <= 'z';
        if (i == 0) { /* the first word starts with a capital */
            put_byte(out, byte >= 'a' && byte <= 'z' ? (char)(byte - 'a' + 'A') : byte);
            continue;
        }
        if (upper) {
            /* a word starts here: after a lowercase letter, or where an
             * acronym ends and a word follows ("OTLPExporter") */
            if (!previous_upper || next_lower) put_byte(out, ' ');
            /* a word of its own is lowercase; an acronym's letters stay */
            int acronym = next_upper || (!next_lower) || previous_upper;
            if (!acronym || (previous_upper && next_lower)) byte = (char)(byte - 'A' + 'a');
        }
        put_byte(out, byte);
    }
}

static void markdown_heading(markdown *document, uint32_t level, ta_string_view name) {
    markdown_block(document);
    put_repeat(document->out, '#', level < REPORT_HEADING_MAX ? level : REPORT_HEADING_MAX);
    put_byte(document->out, ' ');
    markdown_words(document->out, name);
}

/* Where a '<' outside a code span leaves a value: a renderer would take
 * "<state>" for an HTML tag, and an entity ("&lt;") reads badly in a
 * terminal. So such a value is written as code, as written: whole, when it
 * has no backtick ("`attempts < 3`"), else each word holding the '<'. */
static int markdown_bare_angle(ta_string_view text, int *backticks) {
    int code = 0, angle = 0;
    *backticks = 0;
    for (uint32_t i = 0; i < text.length; i++) {
        if (text.pointer[i] == '`') code = !code, *backticks = 1;
        if (text.pointer[i] == '<' && !code) angle = 1;
    }
    return angle;
}

/* The bytes of a value, in a table's cell with a pipe escaped (GitHub
 * reads "\|" so in a code span too) and a line break a space. */
static void markdown_bytes(ta_output *out, report_span span, int cell) {
    uint32_t start = span.start;
    for (uint32_t i = span.start; i < span.end; i++) {
        char byte = span.text.pointer[i];
        if (!cell || (byte != '|' && byte != '\n' && byte != '\r')) continue;
        put_span(out, (report_span){span.text, start, i});
        put_literal(out, byte == '|' ? "\\|" : " ");
        start = i + 1;
    }
    put_span(out, (report_span){span.text, start, span.end});
}

static int is_space(char byte) {
    return byte == ' ' || byte == '\n' || byte == '\r' || byte == '\t';
}

static void markdown_text(ta_output *out, ta_string_view text, int cell) {
    int backticks;
    if (!markdown_bare_angle(text, &backticks)) {
        markdown_bytes(out, (report_span){text, 0, text.length}, cell);
        return;
    }
    if (!backticks) {
        put_byte(out, '`');
        markdown_bytes(out, (report_span){text, 0, text.length}, cell);
        put_byte(out, '`');
        return;
    }
    /* Words: a word with a '<' outside a code span, and no backtick of its
     * own, is written as code. */
    int code = 0;
    for (uint32_t i = 0; i < text.length;) {
        uint32_t end = i;
        int angle = 0, tick = 0;
        while (end < text.length && !is_space(text.pointer[end])) {
            if (text.pointer[end] == '`') tick = 1;
            if (text.pointer[end] == '<' && !code && !tick) angle = 1;
            end++;
        }
        for (uint32_t j = i; j < end; j++)
            if (text.pointer[j] == '`') code = !code;
        int quoted = angle && !tick;
        if (quoted) put_byte(out, '`');
        markdown_bytes(out, (report_span){text, i, end}, cell);
        if (quoted) put_byte(out, '`');
        uint32_t next = end;
        while (next < text.length && is_space(text.pointer[next]))
            next++;
        markdown_bytes(out, (report_span){text, end, next}, cell);
        i = next;
    }
}

static int markdown_has_backtick(ta_string_view text) {
    for (uint32_t i = 0; i < text.length; i++)
        if (text.pointer[i] == '`') return 1;
    return 0;
}

static void markdown_value(ta_output *out, const ta_report_node *node, int cell) {
    if (node->kind == REPORT_NUMBER)
        put_number(out, node->number);
    else if (node->kind == REPORT_BOOLEAN)
        put_literal(out, node->number ? "yes" : "no");
    else if (node->kind == REPORT_VALUE && !markdown_has_backtick(node->text)) {
        put_byte(out, '`'); /* as code: its quotes and stars are no emphasis */
        markdown_bytes(out, (report_span){node->text, 0, node->text.length}, cell);
        put_byte(out, '`');
    } else
        markdown_text(out, node->text, cell);
}

/* A fence longer than any run of backticks in the text. */
static void markdown_fence(markdown *document, const ta_report_node *node) {
    uint32_t run = 0, longest = 0;
    for (uint32_t i = 0; i < node->text.length; i++) {
        run = node->text.pointer[i] == '`' ? run + 1 : 0;
        if (run > longest) longest = run;
    }
    uint32_t fence = longest + 1 > REPORT_FENCE_MIN ? longest + 1 : REPORT_FENCE_MIN;
    markdown_block(document);
    put_repeat(document->out, '`', fence);
    put(document->out, node->language);
    put_byte(document->out, '\n');
    put(document->out, node->text);
    if (node->text.length && node->text.pointer[node->text.length - 1] != '\n')
        put_byte(document->out, '\n');
    put_repeat(document->out, '`', fence);
    put_byte(document->out, '\n');
}

/* 1 when `member` of `item` names a column an earlier member (of this item
 * or an earlier one) named already. */
static int markdown_column_seen(const ta_report_node *list, const ta_report_node *item,
                                const ta_report_node *member) {
    for (const ta_report_node *other = list->first; other; other = other->next)
        for (const ta_report_node *earlier = other->first; earlier; earlier = earlier->next) {
            if (other == item && earlier == member) return 0;
            if (report_same_name(earlier->name, member->name)) return 1;
        }
    return 0;
}

/* A list whose items are all objects of scalars, with a column at least. */
static int markdown_is_table(const ta_report_node *list) {
    int columns = 0;
    for (const ta_report_node *item = list->first; item; item = item->next) {
        if (item->kind != REPORT_OBJECT) return 0;
        for (const ta_report_node *member = item->first; member; member = member->next) {
            if (!report_is_scalar(member)) return 0;
            columns = 1;
        }
    }
    return columns;
}

static void markdown_row(ta_output *out, const ta_report_node *list, const ta_report_node *row) {
    put_byte(out, '|');
    for (const ta_report_node *item = list->first; item; item = item->next)
        for (const ta_report_node *column = item->first; column; column = column->next) {
            if (markdown_column_seen(list, item, column)) continue;
            put_byte(out, ' ');
            if (!row)
                markdown_words(out, column->name);
            else
                for (const ta_report_node *cell = row->first; cell; cell = cell->next)
                    if (report_same_name(cell->name, column->name)) {
                        markdown_value(out, cell, 1);
                        break;
                    }
            put_literal(out, " |");
        }
    put_byte(out, '\n');
}

static void markdown_table(markdown *document, const ta_report_node *list) {
    ta_output *out = document->out;
    markdown_block(document);
    markdown_row(out, list, NULL);
    put_byte(out, '|');
    for (const ta_report_node *item = list->first; item; item = item->next)
        for (const ta_report_node *column = item->first; column; column = column->next)
            if (!markdown_column_seen(list, item, column)) put_literal(out, " --- |");
    put_byte(out, '\n');
    for (const ta_report_node *row = list->first; row; row = row->next)
        markdown_row(out, list, row);
}

/* What follows a node's heading. 1: an object,
 * or a list whose items take headings of their own: the caller pushes it. */
static int markdown_body(markdown *document, const ta_report_node *node) {
    if (node->kind == REPORT_OBJECT) return 1;
    if (node->kind == REPORT_BLOCK) {
        markdown_fence(document, node);
        return 0;
    }
    if (node->kind != REPORT_LIST) {
        markdown_block(document);
        markdown_value(document->out, node, 0);
        put_byte(document->out, '\n');
        return 0;
    }
    if (!node->first) {
        markdown_block(document);
        put_literal(document->out, "_none_\n");
        return 0;
    }
    int scalars = 1;
    for (const ta_report_node *item = node->first; item; item = item->next)
        if (!report_is_scalar(item)) scalars = 0;
    if (scalars) {
        markdown_block(document);
        for (const ta_report_node *item = node->first; item; item = item->next) {
            put_literal(document->out, "- ");
            markdown_value(document->out, item, 0);
            put_byte(document->out, '\n');
        }
        return 0;
    }
    if (markdown_is_table(node)) {
        markdown_table(document, node);
        return 0;
    }
    return 1;
}

/* An item's heading: "State 2: idle", the item's first text when it is an
 * object with one. */
static void markdown_item_heading(markdown *document, const report_frame *frame,
                                  const ta_report_node *item) {
    markdown_heading(document, frame->level + 1, item->name);
    put_byte(document->out, ' ');
    put_number(document->out, frame->index);
    if (item->kind == REPORT_OBJECT)
        for (const ta_report_node *member = item->first; member; member = member->next)
            if (member->kind == REPORT_TEXT) {
                put_literal(document->out, ": ");
                put(document->out, member->text);
                break;
            }
    put_byte(document->out, '\n');
}

static void markdown_render(const ta_report *report, ta_output *out) {
    report_frame stack[TA_REPORT_MAX_DEPTH];
    uint32_t count = 0;
    markdown document = {out, 0};
    markdown_heading(&document, 1, report->root->name);
    put_byte(out, '\n');
    report_frame_set(&stack[count++], report->root, 1);
    while (count > 0) {
        report_frame *frame = &stack[count - 1];
        const ta_report_node *child = frame->cursor;
        if (!child) {
            count--;
            continue;
        }
        frame->cursor = child->next;
        if (frame->node->kind == REPORT_OBJECT && report_is_scalar(child)) {
            if (!frame->scalar_run) markdown_block(&document);
            frame->scalar_run = 1;
            put_literal(out, "- **");
            markdown_words(out, child->name);
            put_literal(out, "**: ");
            markdown_value(out, child, 0);
            put_byte(out, '\n');
            continue;
        }
        frame->scalar_run = 0;
        if (frame->node->kind == REPORT_LIST) {
            frame->index++;
            markdown_item_heading(&document, frame, child);
        } else {
            markdown_heading(&document, frame->level + 1, child->name);
            put_byte(out, '\n');
        }
        uint32_t level = frame->level + 1;
        if (markdown_body(&document, child) && count < TA_REPORT_MAX_DEPTH)
            report_frame_set(&stack[count++], child, level);
    }
}

/* ---- rendering --------------------------------------------------------- */

ta_status ta_report_render(ta_report *report, uint32_t format, ta_output *out) {
    if (report->status != TA_OK) return report->status;
    if (format > TA_REPORT_XML) return TA_ERROR_INVALID;
    report->depth = 1; /* whatever is still open is closed */
    if (format == TA_REPORT_MARKDOWN)
        markdown_render(report, out);
    else if (format == TA_REPORT_JSON)
        json_render(report, out);
    else
        xml_render(report, out);
    return TA_OK;
}

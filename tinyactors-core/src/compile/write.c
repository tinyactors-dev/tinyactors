/* write.c — ta_xml_writer: XML from calls, for charts and manifests a
 * binding builds. It escapes, indents and checks nesting; it knows no SCXML
 * (the compiler validates the text). Every byte it writes is one that
 * compile/xml.c reads back as written. */
#include "compile/compile_support.h"

#include <string.h>

typedef struct open_element {
    uint32_t name, name_length; /* the name, in the output */
    uint8_t started;            /* '>' is written: the element has content */
    uint8_t last_was_element;
} open_element;

struct ta_xml_writer {
    ta_allocator allocator;
    ta_vector output; /* bytes */
    ta_vector stack;  /* open_element */
    ta_status status; /* sticky: the first error */
    int root_done;
};

static ta_status fail(ta_xml_writer *writer, ta_status status) {
    if (writer->status == TA_OK) writer->status = status;
    return writer->status;
}

static void put(ta_xml_writer *writer, const char *bytes, uint32_t length) {
    if (!length) return;
    unsigned char *p = (unsigned char *)ta_vector_push_many(&writer->output, length);
    if (p)
        memcpy(p, bytes, length);
    else
        fail(writer, TA_ERROR_NO_MEMORY);
}

static void put_c_string(ta_xml_writer *writer, const char *text) {
    put(writer, text, (uint32_t)strlen(text));
}

static void line(ta_xml_writer *writer, uint32_t depth) {
    put(writer, "\n", 1);
    for (uint32_t i = 0; i < depth; i++)
        put(writer, "  ", 2);
}

static open_element *top(ta_xml_writer *writer) {
    return writer->stack.length
               ? &TA_VECTOR_AT(&writer->stack, open_element, writer->stack.length - 1)
               : NULL;
}

/* An XML name: a letter, '_', ':' or a non-ASCII byte, then also digits,
 * '-' and '.'. */
static int is_name(ta_string_view name) {
    if (!name.length || !name.pointer) return 0;
    for (uint32_t i = 0; i < name.length; i++) {
        unsigned char c = (unsigned char)name.pointer[i];
        int start = (c | 32) - 'a' < 26u || c == '_' || c == ':' || c >= 0x80;
        if (!start && (i == 0 || !((c - '0') < 10u || c == '-' || c == '.'))) return 0;
    }
    return 1;
}

/* XML 1.0 allows no control characters but tab, line feed and carriage return. */
static int is_character_data(ta_string_view text) {
    for (uint32_t i = 0; i < text.length; i++) {
        unsigned char c = (unsigned char)text.pointer[i];
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return 0;
    }
    return 1;
}

/* `quote` is 0 for text, else the attribute's quote character. */
static void escaped(ta_xml_writer *writer, ta_string_view text, char quote) {
    for (uint32_t i = 0; i < text.length; i++) {
        char c = text.pointer[i];
        const unsigned char *end = writer->output.data + writer->output.length;
        if (c == '&')
            put_c_string(writer, "&amp;");
        else if (c == '<')
            put_c_string(writer, "&lt;");
        else if (c == '>' && !quote && writer->output.length >= 2 && end[-1] == ']' &&
                 end[-2] == ']')
            put_c_string(writer, "&gt;"); /* "]]>" may not appear in text */
        else if (c == '"' && quote == '"')
            put_c_string(writer, "&quot;");
        else if (c == '\r')
            put_c_string(writer, "&#13;"); /* a parser reads a raw one as a line end */
        else if ((c == '\n' || c == '\t') && quote)
            put_c_string(writer, c == '\n' ? "&#10;" : "&#9;"); /* else read as a space */
        else
            put(writer, &c, 1);
    }
}

/* The open element gets content: close its start tag. */
static void start_content(ta_xml_writer *writer, open_element *element) {
    if (element->started) return;
    put(writer, ">", 1);
    element->started = 1;
}

ta_status ta_xml_writer_new(const ta_allocator *allocator, ta_xml_writer **out) {
    ta_allocator chosen = allocator && allocator->alloc && allocator->free
                              ? *allocator
                              : ta_compile_default_allocator();
    *out = NULL;
    ta_xml_writer *writer = (ta_xml_writer *)chosen.alloc(chosen.context, sizeof(ta_xml_writer), 8);
    if (!writer) return TA_ERROR_NO_MEMORY;
    memset(writer, 0, sizeof *writer);
    writer->allocator = chosen;
    ta_vector_init(&writer->output, &writer->allocator, 1);
    ta_vector_init(&writer->stack, &writer->allocator, sizeof(open_element));
    *out = writer;
    return TA_OK;
}

ta_status ta_xml_writer_open(ta_xml_writer *writer, ta_string_view name) {
    if (writer->status) return writer->status;
    if (!is_name(name)) return fail(writer, TA_ERROR_INVALID);
    open_element *parent = top(writer);
    if (!parent && writer->root_done) return fail(writer, TA_ERROR_STATE); /* one root */
    if (parent) {
        start_content(writer, parent);
        line(writer, writer->stack.length);
        parent->last_was_element = 1;
    }
    put(writer, "<", 1);
    uint32_t at = writer->output.length;
    put(writer, name.pointer, name.length);
    open_element *element = (open_element *)ta_vector_push(&writer->stack);
    if (!element) return fail(writer, TA_ERROR_NO_MEMORY);
    element->name = at;
    element->name_length = name.length;
    return writer->status;
}

ta_status ta_xml_writer_attribute(ta_xml_writer *writer, ta_string_view name,
                                  ta_string_view value) {
    if (writer->status) return writer->status;
    open_element *element = top(writer);
    if (!element || element->started) return fail(writer, TA_ERROR_STATE);
    if (!is_name(name) || !is_character_data(value)) return fail(writer, TA_ERROR_INVALID);
    /* '"', unless the value has '"' and no '\'': then it reads unescaped */
    char quote = '"';
    if (value.length && memchr(value.pointer, '"', value.length) &&
        !memchr(value.pointer, '\'', value.length))
        quote = '\'';
    put(writer, " ", 1);
    put(writer, name.pointer, name.length);
    put(writer, "=", 1);
    put(writer, &quote, 1);
    escaped(writer, value, quote);
    put(writer, &quote, 1);
    return writer->status;
}

ta_status ta_xml_writer_text(ta_xml_writer *writer, ta_string_view text) {
    if (writer->status) return writer->status;
    open_element *element = top(writer);
    if (!element) return fail(writer, TA_ERROR_STATE);
    if (!is_character_data(text)) return fail(writer, TA_ERROR_INVALID);
    if (!text.length) return TA_OK;
    start_content(writer, element);
    escaped(writer, text, 0);
    element->last_was_element = 0;
    return writer->status;
}

ta_status ta_xml_writer_close(ta_xml_writer *writer) {
    if (writer->status) return writer->status;
    open_element *element = top(writer);
    if (!element) return fail(writer, TA_ERROR_STATE);
    open_element closing = *element;
    writer->stack.length--;
    if (!closing.started) {
        put(writer, "/>", 2);
    } else {
        if (closing.last_was_element) line(writer, writer->stack.length);
        put(writer, "</", 2);
        /* the name is earlier in the output, which may move as it grows */
        unsigned char *p =
            (unsigned char *)ta_vector_push_many(&writer->output, closing.name_length + 1);
        if (!p) return fail(writer, TA_ERROR_NO_MEMORY);
        memcpy(p, writer->output.data + closing.name, closing.name_length);
        p[closing.name_length] = '>';
    }
    if (!writer->stack.length) writer->root_done = 1;
    return writer->status;
}

ta_status ta_xml_writer_finish(ta_xml_writer *writer, ta_string_view *out) {
    out->pointer = NULL;
    out->length = 0;
    if (writer->status) return writer->status;
    if (writer->stack.length || !writer->root_done) return fail(writer, TA_ERROR_STATE);
    out->pointer = (const char *)writer->output.data;
    out->length = writer->output.length;
    return TA_OK;
}

void ta_xml_writer_free(ta_xml_writer *writer) {
    if (!writer) return;
    ta_allocator allocator = writer->allocator;
    ta_vector_free(&writer->output);
    ta_vector_free(&writer->stack);
    allocator.free(allocator.context, writer, sizeof *writer);
}

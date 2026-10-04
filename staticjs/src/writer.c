/*
 * The writer: a value to JavaScript source, into a caller's buffer, resuming
 * where it stopped when the buffer fills. Nested values are walked with an
 * explicit stack. The text is JSON where JSON can say it, and is always a
 * JavaScript expression that evaluates to an equal value.
 */
#include "internal.h"

/* Where a container is: its opening is written, and then each item. */
enum { W_START = 0, W_ITEM, W_ITEM_VALUE, W_KEY_COLON, W_VALUE };

typedef struct {
    sjs_value value;
    uint32_t index;
    uint8_t phase;
} wframe;

struct sjs_writer {
    sjs_context *context;
    sjs_str function_text;
    sjs_buf stack;
    const uint8_t *pending;
    uint32_t pending_length;
    uint8_t scratch[32];
    bool in_string;
    sjs_str string;
    uint32_t string_at;
    bool failed;
};

static sjs_result push(sjs_writer *w, sjs_value value) {
    wframe f = { value, 0, W_START };
    return sjs_buf_append(&w->context->counting, &w->stack, (sjs_bytes){ (const uint8_t *)&f, sizeof f });
}

sjs_result sjs_writer_init(sjs_writer **out, const sjs_writer_config *config) {
    if (out == NULL || config == NULL || config->context == NULL || config->value.internal != 0) return SJS_ERR_INVALID;
    sjs_writer *w = sjs_mem_alloc(&config->context->counting, sizeof *w);
    if (w == NULL) return SJS_ERR_NOMEM;
    sjs_zero(w, sizeof *w);
    w->context = config->context;
    w->function_text = config->function_text;
    if (push(w, config->value) != SJS_OK) {
        sjs_writer_destroy(w);
        return SJS_ERR_NOMEM;
    }
    *out = w;
    return SJS_OK;
}

void sjs_writer_destroy(sjs_writer *writer) {
    if (writer == NULL) return;
    const sjs_allocator *a = &writer->context->counting;
    sjs_buf_free(a, &writer->stack);
    sjs_mem_free(a, writer, sizeof *writer);
}

static void say(sjs_writer *w, const char *text) {
    w->pending = (const uint8_t *)text;
    w->pending_length = sjs_cstr(text).length;
}

static void say_scratch(sjs_writer *w, uint32_t length) {
    w->pending = w->scratch;
    w->pending_length = length;
}

static void begin_string(sjs_writer *w, sjs_str s) {
    w->in_string = true;
    w->string = s;
    w->string_at = 0;
    say(w, "\"");
}

/* The next piece of the string being written. */
static void string_piece(sjs_writer *w) {
    sjs_str s = w->string;
    uint32_t start = w->string_at;
    uint32_t i = start;
    while (i < s.length) {
        uint8_t c = (uint8_t)s.ptr[i];
        if (c < 0x20u || c == '"' || c == '\\') break;
        i++;
    }
    if (i > start) {
        w->pending = (const uint8_t *)s.ptr + start;
        w->pending_length = i - start;
        w->string_at = i;
        return;
    }
    if (i >= s.length) {
        w->in_string = false;
        say(w, "\"");
        return;
    }
    uint8_t c = (uint8_t)s.ptr[i];
    w->string_at = i + 1;
    const char *escape = NULL;
    switch (c) {
    case '"': escape = "\\\""; break;
    case '\\': escape = "\\\\"; break;
    case '\n': escape = "\\n"; break;
    case '\r': escape = "\\r"; break;
    case '\t': escape = "\\t"; break;
    case '\b': escape = "\\b"; break;
    case '\f': escape = "\\f"; break;
    default: break;
    }
    if (escape != NULL) {
        say(w, escape);
        return;
    }
    static const char hex[] = "0123456789abcdef";
    sjs_copy(w->scratch, "\\u00", 4);
    w->scratch[4] = (uint8_t)hex[c >> 4];
    w->scratch[5] = (uint8_t)hex[c & 15u];
    say_scratch(w, 6);
}

static void say_number(sjs_writer *w, double d) {
    if (sjs_number_is_negative_zero(d)) {
        say(w, "-0");
        return;
    }
    say_scratch(w, sjs_number_format(d, (char *)w->scratch));
}

/* Advance by one piece of output; false when everything is written. */
static sjs_result next(sjs_writer *w, bool *more) {
    uint32_t n = SJS_BUF_COUNT(w->stack, wframe);
    if (n == 0) {
        *more = false;
        return SJS_OK;
    }
    *more = true;
    wframe *f = &SJS_BUF_AT(w->stack, wframe, n - 1);
    sjs_value v = f->value;
    if (f->phase == W_START) {
        switch (v.kind) {
        case SJS_UNDEFINED:
            say(w, "undefined");
            break;
        case SJS_NULL:
            say(w, "null");
            break;
        case SJS_BOOLEAN:
            say(w, v.boolean ? "true" : "false");
            break;
        case SJS_NUMBER:
            say_number(w, v.as.number);
            break;
        case SJS_STRING:
            begin_string(w, sjs_string_view(SJS_AS_STRING(v)));
            break;
        case SJS_ARRAY:
            say(w, "[");
            f->phase = W_ITEM;
            return SJS_OK;
        case SJS_SET:
            say(w, "new Set([");
            f->phase = W_ITEM;
            return SJS_OK;
        case SJS_OBJECT:
            say(w, "{");
            f->phase = W_ITEM;
            return SJS_OK;
        default:
            if (w->function_text.length == 0) return SJS_ERR_INVALID;
            w->pending = (const uint8_t *)w->function_text.ptr;
            w->pending_length = w->function_text.length;
            break;
        }
        w->stack.length -= (uint32_t)sizeof(wframe);
        return SJS_OK;
    }
    uint32_t count = sjs_length(v);
    switch (f->phase) {
    case W_ITEM:
        if (f->index == count) {
            say(w, v.kind == SJS_ARRAY ? "]" : (v.kind == SJS_SET ? "])" : "}"));
            w->stack.length -= (uint32_t)sizeof(wframe);
            return SJS_OK;
        }
        f->phase = W_ITEM_VALUE;
        if (f->index > 0) say(w, ",");
        return SJS_OK;
    case W_ITEM_VALUE:
        if (v.kind == SJS_OBJECT) {
            begin_string(w, sjs_string_view(SJS_AS_OBJECT(v)->entries[f->index].key));
            f->phase = W_KEY_COLON;
            return SJS_OK;
        }
        f->phase = W_ITEM;
        return push(w, SJS_AS_ARRAY(v)->items[f->index++]);
    case W_KEY_COLON:
        say(w, ":");
        f->phase = W_VALUE;
        return SJS_OK;
    default: { /* W_VALUE */
        f->phase = W_ITEM;
        sjs_value item = SJS_AS_OBJECT(v)->entries[f->index++].value;
        return push(w, item);
    }
    }
}

sjs_result sjs_writer_write(sjs_writer *writer, sjs_buffer *buffer) {
    if (writer == NULL || buffer == NULL || (buffer->ptr == NULL && buffer->capacity != 0) || buffer->length > buffer->capacity) {
        return SJS_ERR_INVALID;
    }
    sjs_writer *w = writer;
    if (w->failed) return SJS_ERR_INVALID;
    for (;;) {
        while (w->pending_length > 0) {
            uint32_t room = buffer->capacity - buffer->length;
            if (room == 0) return SJS_NEED_SPACE;
            uint32_t take = room < w->pending_length ? room : w->pending_length;
            sjs_copy(buffer->ptr + buffer->length, w->pending, take);
            buffer->length += take;
            w->pending += take;
            w->pending_length -= take;
        }
        if (w->in_string) {
            string_piece(w);
            continue;
        }
        bool more;
        sjs_result r = next(w, &more);
        if (r != SJS_OK) {
            w->failed = true;
            return r == SJS_ERR_INVALID ? SJS_ERR_INVALID : SJS_ERR_NOMEM;
        }
        if (!more) return SJS_OK;
    }
}

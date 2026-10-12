/*
 * The writer: a value to JavaScript source, into a caller's buffer, resuming
 * where it stopped when the buffer fills. Nested values are walked with an
 * explicit stack. The text is JSON where JSON can say it, and is always a
 * JavaScript expression that evaluates to an equal value.
 */
#include "regex.h"

/* Where a container is: its opening is written, and then each item. A
 * map's item is an entry, `[key,value]`: its key (W_ITEM_VALUE), then the
 * comma and its value (W_ENTRY_VALUE), then its `]` (W_ENTRY_END). */
enum { W_START = 0, W_ITEM, W_ITEM_VALUE, W_KEY_COLON, W_VALUE, W_ENTRY_VALUE, W_ENTRY_END, W_EXTRA, W_EXTRA_END };

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
    sjs_buf digits;        /* a BigInt's, while they are written; its `n` follows */
    sjs_buf literal;       /* a regular expression's or a Temporal value's, while it is written */
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
    sjs_buf_free(a, &writer->digits);
    sjs_buf_free(a, &writer->literal);
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
    w->string_at = i + 1;
    say_scratch(w, sjs_json_escape((uint8_t)s.ptr[i], (char *)w->scratch));
}

/* The map, the set or the array whose entries a map or an iterator of its
 * entries writes, or NULL for any other value. */
static const sjs_array *entries_of(sjs_value v) {
    if (v.kind == SJS_MAP) return SJS_AS_ARRAY(v);
    if (v.kind != SJS_ITERATOR) return NULL;
    const sjs_iterator *it = SJS_AS_ITERATOR(v);
    return it->items.kind == SJS_UNDEFINED && it->h.flags == SJS_ITERATE_ENTRIES ? SJS_AS_ARRAY(it->source) : NULL;
}

/* MUTATION: where what an iterator yields starts, past what next() took;
 * 0 for any other value. Restore: without it, and its uses. */
static uint32_t written_from(sjs_value v) {
    return v.kind == SJS_ITERATOR ? SJS_AS_ITERATOR(v)->position : 0u;
}

/* The key (`part` 0) or the value (1) of the entry at `index` of what
 * entries_of gives: a map's, a set's member, which is both, or an array's
 * element, whose key is its index. */
static sjs_value entry_part(const sjs_array *entries, uint32_t index, uint32_t part) {
    if (entries->h.type == SJS_HEAP_MAP) return entries->items[2u * index + part];
    if (entries->h.type == SJS_HEAP_ARRAY && part == 0) return sjs_num((double)index);
    return entries->items[index];
}

/* The value an iterator yields at `index`: from its items, a set's member,
 * an array's element or index, or its map's key or value (an entry's is
 * written by entries_of's phases). */
static sjs_value iterated(sjs_value v, uint32_t index) {
    const sjs_iterator *it = SJS_AS_ITERATOR(v);
    index += it->position; /* MUTATION */
    if (it->items.kind == SJS_ARRAY) return SJS_AS_ARRAY(it->items)->items[index];
    return entry_part(SJS_AS_ARRAY(it->source), index, it->h.flags == SJS_ITERATE_VALUES ? 1u : 0u);
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
        case SJS_BIGINT: {
            sjs_result r = sjs_bigint_format(&w->context->counting, (sjs_bigint_radix){ SJS_AS_BIGINT(v), 10u }, &w->digits);
            if (r != SJS_OK) return r;
            w->pending = w->digits.ptr;
            w->pending_length = w->digits.length;
            break;
        }
        case SJS_ARRAY:
            /* A match result: its elements, then its other properties. */
            say(w, sjs_array_extra(v) ? "Object.assign([" : "[");
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
        case SJS_MAP:
            say(w, "new Map([");
            f->phase = W_ITEM;
            return SJS_OK;
        case SJS_ITERATOR:
            say(w, "[");
            f->phase = W_ITEM;
            return SJS_OK;
        case SJS_REGEXP: {
            /* As its literal: /source/flags, the flags in their order. */
            const sjs_allocator *a = &w->context->counting;
            char flags[8];
            sjs_str source = sjs_regex_source(v.as.heap);
            uint32_t n = sjs_regex_flag_text(sjs_regex_flag_set(v.as.heap), flags);
            sjs_result r = sjs_buf_append(a, &w->literal, (sjs_bytes){ (const uint8_t *)"/", 1 });
            if (r == SJS_OK) r = sjs_buf_append(a, &w->literal, (sjs_bytes){ (const uint8_t *)source.ptr, source.length });
            if (r == SJS_OK) r = sjs_buf_append(a, &w->literal, (sjs_bytes){ (const uint8_t *)"/", 1 });
            if (r == SJS_OK) r = sjs_buf_append(a, &w->literal, (sjs_bytes){ (const uint8_t *)flags, n });
            if (r != SJS_OK) return r;
            w->pending = w->literal.ptr;
            w->pending_length = w->literal.length;
            break;
        }
        case SJS_TEMPORAL: {
            /* Its type's from() of its ISO string (docs/temporal.md), which
             * needs no escape: Temporal.PlainDate.from("2024-01-31"). */
            const sjs_allocator *a = &w->context->counting;
            char text[SJS_TEMPORAL_TEXT_MAX];
            uint32_t n = sjs_temporal_text(SJS_AS_TEMPORAL(v), text);
            sjs_str type = sjs_temporal_type_name(sjs_temporal_type_of(v));
            /* A Date (docs/date.md): new Date("2024-01-31T00:00:00.000Z"), and
             * new Date(NaN) when it is invalid. */
            bool date = sjs_temporal_type_of(v) == SJS_TEMPORAL_DATE;
            sjs_str open = date ? (n == 0 ? SJS_STR("new Date(NaN") : SJS_STR("new Date(\"")) : SJS_STR("Temporal.");
            sjs_result r = sjs_buf_append(a, &w->literal, (sjs_bytes){ (const uint8_t *)open.ptr, open.length });
            if (r == SJS_OK && !date) r = sjs_buf_append(a, &w->literal, (sjs_bytes){ (const uint8_t *)type.ptr, type.length });
            if (r == SJS_OK && !date) r = sjs_buf_append(a, &w->literal, (sjs_bytes){ (const uint8_t *)".from(\"", 7u });
            if (r == SJS_OK) r = sjs_buf_append(a, &w->literal, (sjs_bytes){ (const uint8_t *)text, n });
            sjs_str close = date && n == 0 ? SJS_STR(")") : SJS_STR("\")");
            if (r == SJS_OK) r = sjs_buf_append(a, &w->literal, (sjs_bytes){ (const uint8_t *)close.ptr, close.length });
            if (r != SJS_OK) return r;
            w->pending = w->literal.ptr;
            w->pending_length = w->literal.length;
            break;
        }
        default:
            if (w->function_text.length == 0) return SJS_ERR_INVALID;
            w->pending = (const uint8_t *)w->function_text.ptr;
            w->pending_length = w->function_text.length;
            break;
        }
        w->stack.length -= (uint32_t)sizeof(wframe);
        return SJS_OK;
    }
    uint32_t count = v.kind == SJS_ITERATOR ? sjs_iterator_count(SJS_AS_ITERATOR(v)) : sjs_length(v);
    switch (f->phase) {
    case W_EXTRA:
        say(w, "],");
        f->phase = W_EXTRA_END;
        return push(w, sjs_heap_value(SJS_OBJECT, SJS_AS_ARRAY(v)->extra));
    case W_EXTRA_END:
        say(w, ")");
        w->stack.length -= (uint32_t)sizeof(wframe);
        return SJS_OK;
    case W_ITEM:
        if (f->index == count) {
            if (sjs_array_extra(v)) {
                f->phase = W_EXTRA;
                return SJS_OK;
            }
            if (v.kind == SJS_ITERATOR) say(w, "].values()");
            else say(w, v.kind == SJS_ARRAY ? "]" : (v.kind == SJS_SET || v.kind == SJS_MAP ? "])" : "}"));
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
        if (entries_of(v) != NULL) {
            say(w, "[");
            f->phase = W_ENTRY_VALUE;
            return push(w, entry_part(entries_of(v), f->index + written_from(v), 0)); /* MUTATION: written_from */
        }
        f->phase = W_ITEM;
        if (v.kind == SJS_ITERATOR) return push(w, iterated(v, f->index++));
        return push(w, SJS_AS_ARRAY(v)->items[f->index++]);
    case W_KEY_COLON:
        say(w, ":");
        f->phase = W_VALUE;
        return SJS_OK;
    case W_ENTRY_VALUE:
        say(w, ",");
        f->phase = W_ENTRY_END;
        return push(w, entry_part(entries_of(v), f->index + written_from(v), 1)); /* MUTATION: written_from */
    case W_ENTRY_END:
        say(w, "]");
        f->index++;
        f->phase = W_ITEM;
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
        if (w->digits.ptr != NULL) {
            sjs_buf_free(&w->context->counting, &w->digits);
            say(w, "n");
            continue;
        }
        if (w->literal.ptr != NULL) sjs_buf_free(&w->context->counting, &w->literal);
        bool more;
        sjs_result r = next(w, &more);
        if (r != SJS_OK) {
            w->failed = true;
            return r == SJS_ERR_INVALID ? SJS_ERR_INVALID : SJS_ERR_NOMEM;
        }
        if (!more) return SJS_OK;
    }
}

/* JSON's escape (QuoteJSONString's) of `"`, `\\` or a control character
 * below U+0020, written to `out` (room for 6): its length. The writer's
 * strings and JSON.stringify's are escaped alike. */
uint32_t sjs_json_escape(uint8_t c, char *out) {
    char short_form = c == '"' ? '"' : c == '\\' ? '\\' : c == '\n' ? 'n' : c == '\r' ? 'r' : c == '\t' ? 't'
                    : c == '\b' ? 'b' : c == '\f' ? 'f' : 0;
    out[0] = '\\';
    if (short_form != 0) {
        out[1] = short_form;
        return 2;
    }
    static const char hex[] = "0123456789abcdef";
    out[1] = 'u';
    out[2] = '0';
    out[3] = '0';
    out[4] = hex[c >> 4];
    out[5] = hex[c & 15u];
    return 6;
}

/*
 * The writer: a value in a format, into a caller's buffer, resuming where it
 * stopped when the buffer fills. Nested values are walked with an explicit
 * stack. As source, the text is JSON where JSON can say it, and is always a
 * JavaScript expression that evaluates to an equal value. As JSON or CBOR
 * (next_data), it is what JSON.stringify keeps of the value.
 */
#include "regex.h"

/* Where a container is: its opening is written, and then each item. A
 * map's item is an entry, `[key,value]`: its key (W_ITEM_VALUE), then the
 * comma and its value (W_ENTRY_VALUE), then its `]` (W_ENTRY_END). */
enum { W_START = 0, W_ITEM, W_ITEM_VALUE, W_KEY_COLON, W_VALUE, W_ENTRY_VALUE, W_ENTRY_END, W_EXTRA, W_EXTRA_END };

typedef struct {
    sjs_value value;
    uint32_t index;
    uint32_t written;     /* JSON and CBOR: the items written, which members left out do not count */
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
    /* A second piece, written after `pending`: CBOR's text after its head. */
    const uint8_t *then;
    uint32_t then_length;
    sjs_format format;
    sjs_error *error;
    bool failed;
};

static sjs_result push(sjs_writer *w, sjs_value value) {
    wframe f = { value, 0, 0, W_START };
    return sjs_buf_append(&w->context->counting, &w->stack, (sjs_bytes){ (const uint8_t *)&f, sizeof f });
}

sjs_result sjs_writer_init(sjs_writer **out, const sjs_writer_config *config) {
    if (out == NULL || config == NULL || config->context == NULL || config->value.internal != 0 ||
        config->format > SJS_FORMAT_CBOR) {
        return SJS_ERR_INVALID;
    }
    sjs_writer *w = sjs_mem_alloc(&config->context->counting, sizeof *w);
    if (w == NULL) return SJS_ERR_NOMEM;
    sjs_zero(w, sizeof *w);
    w->context = config->context;
    w->function_text = config->function_text;
    w->format = config->format;
    w->error = config->error;
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

/* The value an iterator yields at `index` (an entry's is written by
 * entries_of's phases). */
static sjs_value iterated(sjs_value v, uint32_t index) {
    sjs_iterated item;
    sjs_iterator_at(v, index, &item);
    return item.value;
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
            sjs_regexp_text re;
            sjs_regexp_read(v, &re);
            sjs_result r = sjs_buf_append(a, &w->literal, (sjs_bytes){ (const uint8_t *)"/", 1 });
            if (r == SJS_OK) r = sjs_buf_append(a, &w->literal, (sjs_bytes){ (const uint8_t *)re.source.ptr, re.source.length });
            if (r == SJS_OK) r = sjs_buf_append(a, &w->literal, (sjs_bytes){ (const uint8_t *)"/", 1 });
            if (r == SJS_OK) r = sjs_buf_append(a, &w->literal, (sjs_bytes){ (const uint8_t *)re.flags, re.flag_count });
            if (r != SJS_OK) return r;
            w->pending = w->literal.ptr;
            w->pending_length = w->literal.length;
            break;
        }
        case SJS_TEMPORAL: {
            /* Its type's from() of its ISO string (docs/internals/temporal.md), which
             * needs no escape: Temporal.PlainDate.from("2024-01-31"). */
            const sjs_allocator *a = &w->context->counting;
            char text[SJS_TEMPORAL_TEXT_MAX];
            uint32_t n = sjs_temporal_text(SJS_AS_TEMPORAL(v), text);
            sjs_str type = sjs_temporal_type_name(sjs_temporal_type_of(v));
            /* A Date (docs/internals/date.md): new Date("2024-01-31T00:00:00.000Z"), and
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
    uint32_t count = v.kind == SJS_ITERATOR ? sjs_iterator_left(SJS_AS_ITERATOR(v)) : sjs_length(v);
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

/* ------------------------------------------------------------------------
 * JSON and CBOR: what JSON.stringify keeps (docs/guide/embedding.md, "Writing
 * values"), in JSON's text or as a CBOR neutral value
 */

/* How JSON.stringify sees a value: left out (undefined, a function), {} (a
 * map, a set, an iterator, a regular expression: no own enumerable
 * property), an array, an object, or one item. */
enum { D_LEFT_OUT, D_EMPTY, D_ARRAY, D_OBJECT, D_ITEM };

static int writer_data_kind(sjs_value v) {
    switch (v.kind) {
    case SJS_UNDEFINED:
    case SJS_FUNCTION:
        return D_LEFT_OUT;
    case SJS_SET:
    case SJS_MAP:
    case SJS_ITERATOR:
    case SJS_REGEXP:
        return D_EMPTY;
    case SJS_ARRAY:
        return D_ARRAY;
    case SJS_OBJECT:
        return D_OBJECT;
    default:
        return D_ITEM;
    }
}

/* The deepest a CBOR neutral value nests, as tinyactors-core's codec reads it. */
#define WRITER_CBOR_DEPTH 64u

static sjs_result writer_refuse(sjs_writer *w, const char *message) {
    if (w->error != NULL) {
        *w->error = (sjs_error){ SJS_ERROR_UNSUPPORTED, { 0, 0, 0 }, sjs_cstr(message), { "", 0 } };
    }
    return SJS_ERR_PROGRAM;
}

/* A CBOR head: the major type and its argument, shortest, into `out` (room
 * for 9): its length. */
static uint32_t write_cbor_head(uint8_t major, uint64_t n, uint8_t *out) {
    uint8_t m = (uint8_t)(major << 5);
    if (n < 24u) {
        out[0] = (uint8_t)(m | n);
        return 1;
    }
    uint32_t bytes = n <= 0xFFu ? 1u : n <= 0xFFFFu ? 2u : n <= 0xFFFFFFFFu ? 4u : 8u;
    out[0] = (uint8_t)(m | (bytes == 1u ? 24u : bytes == 2u ? 25u : bytes == 4u ? 26u : 27u));
    for (uint32_t i = 0; i < bytes; i++) out[1u + i] = (uint8_t)(n >> (8u * (bytes - 1u - i)));
    return 1u + bytes;
}

static void say_cbor_head(sjs_writer *w, uint8_t major, uint64_t n) {
    say_scratch(w, write_cbor_head(major, n, w->scratch));
}

/* A head, then `bytes`: CBOR's text. */
static void say_cbor_text(sjs_writer *w, sjs_str text) {
    say_cbor_head(w, 3, text.length);
    w->then = (const uint8_t *)text.ptr;
    w->then_length = text.length;
}

typedef union {
    double d;
    uint64_t u;
} writer_double_bits;

typedef union {
    float f;
    uint32_t u;
} writer_float_bits;

/* `d` as a half-precision float, when one holds it exactly. */
static bool writer_half_of(double d, uint16_t *out) {
    writer_float_bits b;
    b.f = (float)d;
    if ((double)b.f != d) return false;
    uint32_t sign = (b.u >> 16) & 0x8000u;
    int32_t exponent = (int32_t)((b.u >> 23) & 0xFFu) - 127;
    uint32_t mantissa = b.u & 0x7FFFFFu;
    if (((b.u >> 23) & 0xFFu) == 0xFFu) {           /* an infinity (NaN is written on its own) */
        *out = (uint16_t)(sign | 0x7C00u);
        return mantissa == 0;
    }
    if ((b.u & 0x7FFFFFFFu) == 0) {                  /* zero, either sign */
        *out = (uint16_t)sign;
        return true;
    }
    if (exponent >= -14 && exponent <= 15) {        /* a normal half: 10 bits of mantissa */
        if ((mantissa & 0x1FFFu) != 0) return false;
        *out = (uint16_t)(sign | (uint32_t)(exponent + 15) << 10 | mantissa >> 13);
        return true;
    }
    if (exponent < -24 || exponent >= -14) return false;
    /* A subnormal half: the value is m * 2^-24 with m below 1024. */
    uint32_t full = mantissa | 0x800000u;           /* 24 bits, the point after the first */
    uint32_t shift = (uint32_t)(-14 - exponent) + 13u;
    if ((full & ((1u << shift) - 1u)) != 0) return false;
    *out = (uint16_t)(sign | full >> shift);
    return true;
}

/* A number as the ECMAScript data model writes it: an integer within
 * +-(2^53 - 1) as one, NaN as f9 7e00, any other number as the shortest
 * float that holds it. */
static void say_cbor_number(sjs_writer *w, double d) {
    uint8_t *o = w->scratch;
    if (d == sjs_number_trunc(d) && d <= 9007199254740991.0 && d >= -9007199254740991.0 &&
        !sjs_number_is_negative_zero(d)) {
        if (d >= 0) say_cbor_head(w, 0, (uint64_t)d);
        else say_cbor_head(w, 1, (uint64_t)(-d) - 1u);
        return;
    }
    uint16_t half;
    if (d != d) {
        o[0] = 0xF9;
        o[1] = 0x7E;
        o[2] = 0x00;
        say_scratch(w, 3);
        return;
    }
    if (writer_half_of(d, &half)) {
        o[0] = 0xF9;
        o[1] = (uint8_t)(half >> 8);
        o[2] = (uint8_t)half;
        say_scratch(w, 3);
        return;
    }
    writer_float_bits f;
    f.f = (float)d;
    if ((double)f.f == d) {
        o[0] = 0xFA;
        for (uint32_t i = 0; i < 4u; i++) o[1u + i] = (uint8_t)(f.u >> (8u * (3u - i)));
        say_scratch(w, 5);
        return;
    }
    writer_double_bits b;
    b.d = d;
    o[0] = 0xFB;
    for (uint32_t i = 0; i < 8u; i++) o[1u + i] = (uint8_t)(b.u >> (8u * (7u - i)));
    say_scratch(w, 9);
}

/* A BigInt as an integer: within -2^64 .. 2^64 - 1, or refused. */
static sjs_result say_cbor_bigint(sjs_writer *w, const sjs_bigint *b) {
    bool negative = sjs_bigint_negative(b);
    uint64_t magnitude = 0;
    if (b->length == 3u && negative && b->limbs[0] == 0 && b->limbs[1] == 0 && b->limbs[2] == 1u) {
        say_cbor_head(w, 1, UINT64_MAX);                 /* -2^64 */
        return SJS_OK;
    }
    if (b->length > 2u) return writer_refuse(w, "a BigInt outside -2^64 to 2^64 - 1 is not a CBOR neutral value");
    for (uint32_t i = b->length; i-- > 0;) magnitude = magnitude << 32 | b->limbs[i];
    if (negative) say_cbor_head(w, 1, magnitude - 1u);
    else say_cbor_head(w, 0, magnitude);
    return SJS_OK;
}

/* One item: null, a boolean, a number, a string, a BigInt, a Date or a
 * Temporal value (as its toJSON gives it). */
static sjs_result say_item(sjs_writer *w, sjs_value v) {
    bool cbor = w->format == SJS_FORMAT_CBOR;
    switch (v.kind) {
    case SJS_NULL:
        say(w, cbor ? "\xF6" : "null");
        return SJS_OK;
    case SJS_BOOLEAN:
        say(w, cbor ? (v.boolean ? "\xF5" : "\xF4") : (v.boolean ? "true" : "false"));
        return SJS_OK;
    case SJS_NUMBER: {
        double d = v.as.number;
        if (cbor) say_cbor_number(w, d);
        else if (!sjs_number_is_finite(d)) say(w, "null");
        else say_scratch(w, sjs_number_format(d + 0.0, (char *)w->scratch));   /* -0 is "0" */
        return SJS_OK;
    }
    case SJS_STRING: {
        sjs_str text = sjs_string_view(SJS_AS_STRING(v));
        if (cbor) say_cbor_text(w, text);
        else begin_string(w, text);
        return SJS_OK;
    }
    case SJS_BIGINT:
        if (!cbor) return writer_refuse(w, "JSON has no BigInt: JSON.stringify throws a TypeError for one");
        return say_cbor_bigint(w, SJS_AS_BIGINT(v));
    default: { /* SJS_TEMPORAL: its toJSON, a string; an invalid Date's is null */
        char text[SJS_TEMPORAL_TEXT_MAX];
        uint32_t n = sjs_temporal_text(SJS_AS_TEMPORAL(v), text);
        if (n == 0) {
            say(w, cbor ? "\xF6" : "null");
            return SJS_OK;
        }
        sjs_result r = sjs_buf_append(&w->context->counting, &w->literal, (sjs_bytes){ (const uint8_t *)text, n });
        if (r != SJS_OK) return r;
        sjs_str kept = { (const char *)w->literal.ptr, w->literal.length };
        if (cbor) say_cbor_text(w, kept);
        else begin_string(w, kept);
        return SJS_OK;
    }
    }
}

/* The members of an object JSON.stringify keeps: those whose value it does
 * not leave out. */
static uint32_t writer_kept_members(const sjs_object *o) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < o->count; i++) n += writer_data_kind(o->entries[i].value) != D_LEFT_OUT;
    return n;
}

static sjs_result next_data(sjs_writer *w, bool *more) {
    uint32_t n = SJS_BUF_COUNT(w->stack, wframe);
    if (n == 0) {
        *more = false;
        return SJS_OK;
    }
    *more = true;
    bool cbor = w->format == SJS_FORMAT_CBOR;
    wframe *f = &SJS_BUF_AT(w->stack, wframe, n - 1);
    sjs_value v = f->value;
    int kind = writer_data_kind(v);
    if (f->phase == W_START) {
        /* Each frame on the stack is a container around this one. */
        if (cbor && n > WRITER_CBOR_DEPTH && (kind != D_ITEM || v.kind == SJS_TEMPORAL)) {
            return writer_refuse(w, "a value nested deeper than 64 is not a CBOR neutral value");
        }
        switch (kind) {
        case D_LEFT_OUT:
            /* Only the whole value: an array writes null for one, and an
             * object leaves its member out. */
            if (!cbor) return writer_refuse(w, "JSON.stringify gives undefined for this value, which is no JSON text");
            say(w, "\xF6");
            break;
        case D_EMPTY:
            say(w, cbor ? "\xA0" : "{}");
            break;
        case D_ARRAY:
            if (cbor) say_cbor_head(w, 4, sjs_length(v));
            else say(w, "[");
            f->phase = W_ITEM;
            return SJS_OK;
        case D_OBJECT:
            if (cbor) say_cbor_head(w, 5, writer_kept_members(SJS_AS_OBJECT(v)));
            else say(w, "{");
            f->phase = W_ITEM;
            return SJS_OK;
        default: {
            sjs_result r = say_item(w, v);
            if (r != SJS_OK) return r;
            break;
        }
        }
        w->stack.length -= (uint32_t)sizeof(wframe);
        return SJS_OK;
    }
    switch (f->phase) {
    case W_ITEM:
        if (kind == D_OBJECT) {
            const sjs_object *o = SJS_AS_OBJECT(v);
            while (f->index < o->count && writer_data_kind(o->entries[f->index].value) == D_LEFT_OUT) f->index++;
        }
        if (f->index == sjs_length(v)) {
            if (!cbor) say(w, kind == D_ARRAY ? "]" : "}");
            w->stack.length -= (uint32_t)sizeof(wframe);
            return SJS_OK;
        }
        f->phase = W_ITEM_VALUE;
        if (!cbor && f->written > 0) say(w, ",");
        return SJS_OK;
    case W_ITEM_VALUE:
        f->written++;
        if (kind == D_ARRAY) {
            sjs_value item = SJS_AS_ARRAY(v)->items[f->index++];
            f->phase = W_ITEM;
            return push(w, writer_data_kind(item) == D_LEFT_OUT ? sjs_null() : item);
        }
        f->phase = W_KEY_COLON;
        if (cbor) say_cbor_text(w, sjs_string_view(SJS_AS_OBJECT(v)->entries[f->index].key));
        else begin_string(w, sjs_string_view(SJS_AS_OBJECT(v)->entries[f->index].key));
        return SJS_OK;
    case W_KEY_COLON:
        f->phase = W_VALUE;
        if (!cbor) say(w, ":");
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
        if (w->then_length > 0) {
            w->pending = w->then;
            w->pending_length = w->then_length;
            w->then_length = 0;
            continue;
        }
        if (w->digits.ptr != NULL) {
            sjs_buf_free(&w->context->counting, &w->digits);
            say(w, "n");
            continue;
        }
        if (w->literal.ptr != NULL) sjs_buf_free(&w->context->counting, &w->literal);
        bool more;
        sjs_result r = w->format == SJS_FORMAT_SOURCE ? next(w, &more) : next_data(w, &more);
        if (r != SJS_OK) {
            w->failed = true;
            return r == SJS_ERR_INVALID || r == SJS_ERR_PROGRAM ? r : SJS_ERR_NOMEM;
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

/*
 * JSON.parse and JSON.stringify. Both are leaves (docs/builtins.md): their
 * input is one value, and each finishes in one C call. Neither recurses: the
 * containers open while a text is read, or while a value is written, are an
 * explicit stack, as everything in staticjs keeps one.
 *
 * Where JavaScript calls a function, staticjs faults: a reviver, a replacer
 * function, a toJSON method. Where JavaScript would make a string staticjs
 * cannot hold (a lone surrogate), or an object it cannot have (one with the
 * key __proto__), it faults too.
 */
#include "vm.h"

static sjs_value json_arg(const sjs_leaf_call *call, uint32_t i) {
    return i < call->count ? call->args[i] : sjs_undefined();
}

static bool json_callable(sjs_value v) {
    return v.kind == SJS_FUNCTION && v.internal == SJS_INTERNAL_NONE;
}

/* ------------------------------------------------------------------------
 * JSON.parse: ECMA-404's grammar, as ECMAScript's JSON.parse reads it
 */

/* What the reader expects next. */
enum { JSON_VALUE = 0, JSON_FIRST_ITEM, JSON_FIRST_KEY, JSON_KEY, JSON_AFTER, JSON_END };

/* A container being filled: owned until it is closed and put into the one
 * around it, or becomes the result. */
typedef struct {
    sjs_value container;   /* an array or an object */
    sjs_string *key;       /* an object's key, read, whose value comes next; or NULL */
} json_level;

typedef struct {
    sjs_vm *vm;
    sjs_leaf_call *call;   /* the value read whole goes to call->result */
    sjs_str text;
    uint32_t at;
    uint8_t state;
    sjs_buf levels;        /* json_level */
    sjs_buf scratch;       /* the text of a string with escapes */
    uint64_t cost;         /* a unit per value made, and per entry an index key moves */
} json_reader;

/* A SyntaxError in JavaScript: a fault naming where, in UTF-16 code units,
 * as JavaScript counts a position. */
static sjs_result json_syntax(json_reader *r, const char *message) {
    char subject[48];
    uint32_t length = sjs_cstr("at position ").length;
    sjs_copy(subject, "at position ", length);
    length += sjs_number_format((double)sjs_utf16_length((sjs_str){ r->text.ptr, r->at }), subject + length);
    sjs_vm_fault(r->vm, message);
    sjs_error_subject(&r->vm->error, (sjs_str){ subject, length });
    return SJS_ERR_PROGRAM;
}

static sjs_result json_syntax_end(json_reader *r) {
    return json_syntax(r, "JSON.parse: the text ends too early (a SyntaxError in JavaScript)");
}

static sjs_result json_unexpected(json_reader *r) {
    return json_syntax(r, "JSON.parse: unexpected character (a SyntaxError in JavaScript)");
}

/* JSON's white space: space, tab, LF and CR, no more. */
static void json_skip_space(json_reader *r) {
    while (r->at < r->text.length) {
        char c = r->text.ptr[r->at];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return;
        r->at++;
    }
}

static bool json_digit(char c) {
    return c >= '0' && c <= '9';
}

/* The four hex digits at `at`, or -1. */
static int32_t json_hex4(sjs_str t, uint32_t at) {
    if (t.length - at < 4u) return -1;
    int32_t v = 0;
    for (uint32_t k = 0; k < 4u; k++) {
        char c = t.ptr[at + k];
        int32_t d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}

static sjs_result json_scratch(json_reader *r, sjs_str bytes) {
    sjs_result e = sjs_buf_append(&r->vm->context->counting, &r->scratch, (sjs_bytes){ (const uint8_t *)bytes.ptr, bytes.length });
    return e == SJS_OK ? SJS_OK : sjs_vm_fail(r->vm, e);
}

/* `\uXXXX` at r->at, and a second one when the first is a high surrogate:
 * a pair is one code point. A surrogate without its other half is a lone
 * one, which JavaScript keeps in the string and staticjs cannot. */
static sjs_result json_unicode(json_reader *r) {
    sjs_str t = r->text;
    int32_t unit = json_hex4(t, r->at + 2u);
    if (unit < 0) {
        r->at += 2u;
        return json_syntax(r, "JSON.parse: a bad Unicode escape (a SyntaxError in JavaScript)");
    }
    r->at += 6u;
    uint32_t point = (uint32_t)unit;
    if (point >= 0xD800u && point <= 0xDFFFu) {
        bool paired = false;
        if (point <= 0xDBFFu && t.length - r->at >= 2u && t.ptr[r->at] == '\\' && t.ptr[r->at + 1u] == 'u') {
            int32_t low = json_hex4(t, r->at + 2u);
            if (low < 0) {
                r->at += 2u;
                return json_syntax(r, "JSON.parse: a bad Unicode escape (a SyntaxError in JavaScript)");
            }
            if (low >= 0xDC00 && low <= 0xDFFF) {
                point = 0x10000u + ((point - 0xD800u) << 10) + ((uint32_t)low - 0xDC00u);
                r->at += 6u;
                paired = true;
            }
        }
        if (!paired) return sjs_vm_fault(r->vm, "JSON.parse would make a string with a lone surrogate, which a staticjs string cannot hold");
    }
    uint8_t bytes[4];
    uint32_t n = sjs_utf8_encode(point, bytes);
    return json_scratch(r, (sjs_str){ (const char *)bytes, n });
}

/* The text of the string whose first byte is at r->at, after its quote:
 * borrowed from the text when it has no escape, else in r->scratch. */
static sjs_result json_string_text(json_reader *r, sjs_str *out) {
    sjs_str t = r->text;
    uint32_t i = r->at;
    while (i < t.length) {
        uint8_t c = (uint8_t)t.ptr[i];
        if (c == '"' || c == '\\' || c < 0x20u) break;
        i++;
    }
    if (i < t.length && t.ptr[i] == '"') {
        *out = (sjs_str){ t.ptr + r->at, i - r->at };
        r->at = i + 1u;
        return SJS_OK;
    }
    r->scratch.length = 0;
    for (;;) {
        if (json_scratch(r, (sjs_str){ t.ptr + r->at, i - r->at }) != SJS_OK) return SJS_ERR_PROGRAM;
        r->at = i;
        if (i == t.length) return json_syntax(r, "JSON.parse: a string is not closed (a SyntaxError in JavaScript)");
        uint8_t c = (uint8_t)t.ptr[i];
        if (c == '"') break;
        if (c < 0x20u) return json_syntax(r, "JSON.parse: a control character in a string (a SyntaxError in JavaScript)");
        /* A backslash. */
        if (i + 1u == t.length) {
            r->at = i + 1u;
            return json_syntax(r, "JSON.parse: a string is not closed (a SyntaxError in JavaScript)");
        }
        char e = t.ptr[i + 1u];
        const char *plain = e == '"' ? "\"" : e == '\\' ? "\\" : e == '/' ? "/" : e == 'b' ? "\b" : e == 'f' ? "\f"
                          : e == 'n' ? "\n" : e == 'r' ? "\r" : e == 't' ? "\t" : NULL;
        if (plain != NULL) {
            if (json_scratch(r, (sjs_str){ plain, 1 }) != SJS_OK) return SJS_ERR_PROGRAM;
            r->at = i + 2u;
        } else if (e == 'u') {
            if (json_unicode(r) != SJS_OK) return SJS_ERR_PROGRAM;
        } else {
            r->at = i + 1u;
            return json_syntax(r, "JSON.parse: a bad escape in a string (a SyntaxError in JavaScript)");
        }
        i = r->at;
        while (i < t.length) {
            uint8_t b = (uint8_t)t.ptr[i];
            if (b == '"' || b == '\\' || b < 0x20u) break;
            i++;
        }
    }
    r->at = i + 1u;
    *out = (sjs_str){ (const char *)r->scratch.ptr, r->scratch.length };
    return SJS_OK;
}

/* A number at r->at: `-`? int frac? exp?, its decimal read exactly. */
static sjs_result json_number(json_reader *r, double *out) {
    sjs_str t = r->text;
    uint32_t i = r->at;
    bool negative = t.ptr[i] == '-';
    if (negative) i++;
    uint32_t first = i;
    if (i < t.length && t.ptr[i] == '0') {
        i++;
    } else if (i < t.length && json_digit(t.ptr[i])) {
        while (i < t.length && json_digit(t.ptr[i])) i++;
    } else {
        r->at = i;
        return i == t.length ? json_syntax_end(r) : json_syntax(r, "JSON.parse: a number needs a digit here (a SyntaxError in JavaScript)");
    }
    int64_t exponent = 0;
    if (i < t.length && t.ptr[i] == '.') {
        uint32_t fraction = ++i;
        while (i < t.length && json_digit(t.ptr[i])) i++;
        if (i == fraction) {
            r->at = i;
            return i == t.length ? json_syntax_end(r) : json_syntax(r, "JSON.parse: a number needs a digit here (a SyntaxError in JavaScript)");
        }
        exponent = -(int64_t)(i - fraction);
    }
    uint32_t end = i;
    if (i < t.length && (t.ptr[i] == 'e' || t.ptr[i] == 'E')) {
        i++;
        int64_t sign = 1;
        if (i < t.length && (t.ptr[i] == '+' || t.ptr[i] == '-')) {
            if (t.ptr[i] == '-') sign = -1;
            i++;
        }
        uint32_t digits = i;
        int64_t e = 0;
        for (; i < t.length && json_digit(t.ptr[i]); i++) {
            /* Saturated: 10^(10^11) is Infinity, whatever the digits. */
            if (e < 100000000000) e = e * 10 + (t.ptr[i] - '0');
        }
        if (i == digits) {
            r->at = i;
            return i == t.length ? json_syntax_end(r) : json_syntax(r, "JSON.parse: a number needs a digit here (a SyntaxError in JavaScript)");
        }
        exponent += sign * e;
    }
    double v = sjs_number_from_decimal((sjs_str){ t.ptr + first, end - first }, exponent);
    *out = negative ? -v : v;
    r->at = i;
    return SJS_OK;
}

/* `true`, `false` or `null` at r->at. */
static sjs_result json_word(json_reader *r, sjs_str word) {
    for (uint32_t k = 0; k < word.length; k++) {
        if (r->at == r->text.length) return json_syntax_end(r);
        if (r->text.ptr[r->at] != word.ptr[k]) return json_unexpected(r);
        r->at++;
    }
    return SJS_OK;
}

static json_level *json_top(json_reader *r) {
    uint32_t n = SJS_BUF_COUNT(r->levels, json_level);
    return n == 0 ? NULL : &SJS_BUF_AT(r->levels, json_level, n - 1u);
}

/* A value read: the result, or an item of the container open. Takes over
 * `v`. */
static sjs_result json_deliver(json_reader *r, sjs_value v) {
    sjs_context *context = r->vm->context;
    json_level *l = json_top(r);
    r->cost++;
    if (l == NULL) {
        r->call->result = v;
        r->state = JSON_END;
        return SJS_OK;
    }
    r->state = JSON_AFTER;
    if (l->container.kind == SJS_ARRAY) {
        sjs_array *a = SJS_AS_ARRAY(l->container);
        sjs_result e = sjs_array_grow(context, &a, 1);
        l->container.as.heap = &a->h;
        if (e != SJS_OK) {
            sjs_unref(context, v);
            return sjs_vm_fail(r->vm, e);
        }
        a->items[a->length++] = v;
        return SJS_OK;
    }
    /* A key the object has keeps its place and takes the value, as
     * CreateDataProperty does; an index key goes among the index keys, in
     * order, which moves the entries after it. */
    sjs_object *o = SJS_AS_OBJECT(l->container);
    sjs_string *key = l->key;
    l->key = NULL;
    uint32_t before = o->count;
    sjs_result e = sjs_object_put_key(context, &o, (sjs_put){ key, v });
    l->container.as.heap = &o->h;
    if (e != SJS_OK) {
        sjs_unref(context, sjs_heap_value(SJS_STRING, &key->h));
        sjs_unref(context, v);
        return sjs_vm_fail(r->vm, e);
    }
    if (o->count != before && (key->h.flags & SJS_STRING_INDEX) != 0) {
        const sjs_entry *at = sjs_object_find(o, sjs_string_view(key));
        uint32_t moved = o->count - 1u - (uint32_t)(at - o->entries);
        if (moved != 0) r->cost += moved + (o->index != NULL ? o->count : 0u);
    }
    return SJS_OK;
}

static sjs_result json_open(json_reader *r, uint8_t kind) {
    sjs_vm *vm = r->vm;
    if (SJS_BUF_COUNT(r->levels, json_level) >= vm->context->limits.max_nesting) {
        sjs_vm_fail(vm, SJS_ERR_LIMIT);
        sjs_error_subject(&vm->error, SJS_STR("max_nesting"));
        return SJS_ERR_PROGRAM;
    }
    json_level l = { sjs_undefined(), NULL };
    sjs_result e;
    if (kind == SJS_ARRAY) {
        sjs_array *a;
        e = sjs_array_make(vm->context, 4, &a);
        if (e == SJS_OK) l.container = sjs_heap_value(SJS_ARRAY, &a->h);
    } else {
        sjs_object *o;
        e = sjs_object_make(vm->context, 4, &o);
        if (e == SJS_OK) l.container = sjs_heap_value(SJS_OBJECT, &o->h);
    }
    if (e == SJS_OK) e = sjs_buf_append(&vm->context->counting, &r->levels, (sjs_bytes){ (const uint8_t *)&l, sizeof l });
    if (e != SJS_OK) {
        sjs_unref(vm->context, l.container);
        return sjs_vm_fail(vm, e);
    }
    r->state = kind == SJS_ARRAY ? JSON_FIRST_ITEM : JSON_FIRST_KEY;
    return SJS_OK;
}

static sjs_result json_close(json_reader *r) {
    json_level l = *json_top(r);
    r->levels.length -= (uint32_t)sizeof l;
    r->at++;
    return json_deliver(r, l.container);
}

/* A key, its colon after it. */
static sjs_result json_key(json_reader *r) {
    sjs_vm *vm = r->vm;
    if (r->text.ptr[r->at] != '"') return json_syntax(r, "JSON.parse: a key must be a string (a SyntaxError in JavaScript)");
    r->at++;
    sjs_str text;
    if (json_string_text(r, &text) != SJS_OK) return SJS_ERR_PROGRAM;
    if (sjs_str_equal(text, SJS_STR("__proto__"))) {
        return sjs_vm_fault(vm, "JSON.parse would make an object with the key __proto__: staticjs objects have no prototype");
    }
    sjs_string *key;
    sjs_result e = sjs_string_intern(vm->context, text, &key);
    if (e != SJS_OK) return sjs_vm_fail(vm, e);
    json_top(r)->key = key;
    json_skip_space(r);
    if (r->at == r->text.length) return json_syntax_end(r);
    if (r->text.ptr[r->at] != ':') return json_syntax(r, "JSON.parse: a colon must follow a key (a SyntaxError in JavaScript)");
    r->at++;
    r->state = JSON_VALUE;
    return SJS_OK;
}

static sjs_result json_value(json_reader *r) {
    sjs_vm *vm = r->vm;
    char c = r->text.ptr[r->at];
    switch (c) {
    case '{':
        r->at++;
        return json_open(r, SJS_OBJECT);
    case '[':
        r->at++;
        return json_open(r, SJS_ARRAY);
    case '"': {
        r->at++;
        sjs_str text;
        if (json_string_text(r, &text) != SJS_OK) return SJS_ERR_PROGRAM;
        sjs_string *s;
        sjs_result e = sjs_string_make(vm->context, text, &s);
        if (e != SJS_OK) return sjs_vm_fail(vm, e);
        return json_deliver(r, sjs_heap_value(SJS_STRING, &s->h));
    }
    case 't':
        if (json_word(r, SJS_STR("true")) != SJS_OK) return SJS_ERR_PROGRAM;
        return json_deliver(r, sjs_boolean(true));
    case 'f':
        if (json_word(r, SJS_STR("false")) != SJS_OK) return SJS_ERR_PROGRAM;
        return json_deliver(r, sjs_boolean(false));
    case 'n':
        if (json_word(r, SJS_STR("null")) != SJS_OK) return SJS_ERR_PROGRAM;
        return json_deliver(r, sjs_null());
    default:
        if (c == '-' || json_digit(c)) {
            double d;
            if (json_number(r, &d) != SJS_OK) return SJS_ERR_PROGRAM;
            return json_deliver(r, sjs_num(d));
        }
        return json_unexpected(r);
    }
}

/* What follows an item: a comma, or its container's end. */
static sjs_result json_after(json_reader *r) {
    char c = r->text.ptr[r->at];
    bool array = json_top(r)->container.kind == SJS_ARRAY;
    if (c == ',') {
        r->at++;
        r->state = array ? JSON_VALUE : JSON_KEY;
        return SJS_OK;
    }
    if (c == (array ? ']' : '}')) return json_close(r);
    return json_syntax(r, array ? "JSON.parse: a comma or ] must follow an item (a SyntaxError in JavaScript)"
                                : "JSON.parse: a comma or } must follow a property (a SyntaxError in JavaScript)");
}

static sjs_result json_read(json_reader *r) {
    for (;;) {
        json_skip_space(r);
        if (r->state == JSON_END) {
            if (r->at == r->text.length) return SJS_OK;
            return json_syntax(r, "JSON.parse: text after the value (a SyntaxError in JavaScript)");
        }
        if (r->at == r->text.length) return json_syntax_end(r);
        sjs_result e;
        switch (r->state) {
        case JSON_FIRST_ITEM:
            if (r->text.ptr[r->at] == ']') e = json_close(r);
            else e = json_value(r);
            break;
        case JSON_FIRST_KEY:
            if (r->text.ptr[r->at] == '}') e = json_close(r);
            else e = json_key(r);
            break;
        case JSON_KEY:
            e = json_key(r);
            break;
        case JSON_AFTER:
            e = json_after(r);
            break;
        default: /* JSON_VALUE */
            e = json_value(r);
            break;
        }
        if (e != SJS_OK) return e;
    }
}

/* JSON.parse(text): ToString of a primitive, as JavaScript reads one. A
 * fuel unit per 16 bytes of text and per value made, and per entry moved
 * when an index key comes before index keys already in its object. */
sjs_result sjs_json_parse(sjs_vm *vm, sjs_leaf_call *call) {
    sjs_value text = json_arg(call, 0);
    if (json_callable(json_arg(call, 1))) {
        return sjs_vm_fault(vm, "JSON.parse with a reviver calls it back for every value, which staticjs does not");
    }
    if (!sjs_is_primitive(text)) return sjs_vm_fault(vm, "JSON.parse reads a string: staticjs turns only primitives into strings");
    if (text.kind == SJS_BIGINT) {
        return sjs_vm_fault(vm, "staticjs turns a BigInt into a string only with String, toString, + or a template");
    }
    char room[32];
    json_reader r = { 0 };
    r.vm = vm;
    r.call = call;
    r.text = sjs_primitive_text(text, room);
    r.state = JSON_VALUE;
    sjs_result result = json_read(&r);
    uint32_t n = SJS_BUF_COUNT(r.levels, json_level);
    for (uint32_t i = 0; i < n; i++) {
        json_level *l = &SJS_BUF_AT(r.levels, json_level, i);
        sjs_unref(vm->context, l->container);
        if (l->key != NULL) sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &l->key->h));
    }
    sjs_buf_free(&vm->context->counting, &r.levels);
    sjs_buf_free(&vm->context->counting, &r.scratch);
    call->cost = r.cost + r.text.length / 16u;
    return result;
}

/* ------------------------------------------------------------------------
 * JSON.stringify
 */

/* A container being written: borrowed from the value written. */
typedef struct {
    sjs_value value;
    uint32_t index;     /* the next item, or the next key of the list */
    uint32_t written;   /* items written */
} json_frame;

typedef struct {
    sjs_vm *vm;
    sjs_buf out;
    sjs_buf frames;     /* json_frame */
    sjs_array *keys;    /* an array replacer's keys, a set of strings, owned; or NULL */
    uint64_t visited;   /* fuel: a unit per value and per key looked up */
    uint64_t omitted;   /* members left out, which count toward the output's limit */
    uint32_t gap_length;
    char gap[32];       /* at most 10 UTF-16 code units: 30 bytes */
} json_writer;

/* The output, the members left out included, is at most max_string_bytes,
 * so that the work of one call is bounded by its limits. */
static sjs_result json_put(json_writer *w, sjs_str s) {
    sjs_context *context = w->vm->context;
    if ((uint64_t)w->out.length + s.length + w->omitted > context->limits.max_string_bytes) return sjs_vm_fail(w->vm, SJS_ERR_LIMIT);
    sjs_result e = sjs_buf_append(&context->counting, &w->out, (sjs_bytes){ (const uint8_t *)s.ptr, s.length });
    return e == SJS_OK ? SJS_OK : sjs_vm_fail(w->vm, e);
}

static sjs_result json_omit(json_writer *w) {
    w->visited++;
    w->omitted++;
    return json_put(w, SJS_STR(""));
}

/* QuoteJSONString, with the writer's escapes. */
static sjs_result json_quote(json_writer *w, sjs_str s) {
    if (json_put(w, SJS_STR("\"")) != SJS_OK) return SJS_ERR_PROGRAM;
    uint32_t start = 0;
    for (uint32_t i = 0; i < s.length; i++) {
        uint8_t c = (uint8_t)s.ptr[i];
        if (c >= 0x20u && c != '"' && c != '\\') continue;
        char escape[8];
        uint32_t n = sjs_json_escape(c, escape);
        if (json_put(w, (sjs_str){ s.ptr + start, i - start }) != SJS_OK) return SJS_ERR_PROGRAM;
        if (json_put(w, (sjs_str){ escape, n }) != SJS_OK) return SJS_ERR_PROGRAM;
        start = i + 1u;
    }
    if (json_put(w, (sjs_str){ s.ptr + start, s.length - start }) != SJS_OK) return SJS_ERR_PROGRAM;
    return json_put(w, SJS_STR("\""));
}

/* A line break and `depth` gaps, when there is a gap. */
static sjs_result json_indent(json_writer *w, uint32_t depth) {
    if (w->gap_length == 0) return SJS_OK;
    if (json_put(w, SJS_STR("\n")) != SJS_OK) return SJS_ERR_PROGRAM;
    for (uint32_t i = 0; i < depth; i++) {
        if (json_put(w, (sjs_str){ w->gap, w->gap_length }) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    return SJS_OK;
}

/* Left out of an object, null in an array, undefined at the top. */
static bool json_omitted(sjs_value v) {
    return v.kind == SJS_UNDEFINED || v.kind == SJS_FUNCTION;
}

/* A map, a set, an iterator or a regular expression: an object with no own
 * enumerable property, `{}`. A key of an array replacer that JavaScript
 * would find on its prototype (a method, size, a flag's getter) or as an
 * own property that is not enumerable (lastIndex) faults. */
static sjs_result json_empty(json_writer *w, sjs_value v) {
    if (w->keys != NULL) {
        uint8_t flag = v.kind == SJS_MAP ? SJS_NAME_ON_MAP : v.kind == SJS_SET ? SJS_NAME_ON_SET
                     : v.kind == SJS_REGEXP ? SJS_NAME_ON_REGEXP : SJS_NAME_ON_ITERATOR;
        sjs_str own = v.kind == SJS_REGEXP ? SJS_STR("lastIndex") : v.kind != SJS_ITERATOR ? SJS_STR("size") : SJS_STR("");
        for (uint32_t i = 0; i < w->keys->length; i++) {
            const sjs_string *key = SJS_AS_STRING(w->keys->items[i]);
            const sjs_name *name = sjs_name_of(key);
            if ((name != NULL && (name->flags & flag) != 0) || (own.length > 0 && sjs_str_equal(sjs_string_view(key), own))) {
                sjs_vm_fault(w->vm, "JSON.stringify would read this key of its list from a prototype");
                sjs_error_subject(&w->vm->error, sjs_string_view(key));
                return SJS_ERR_PROGRAM;
            }
        }
        w->visited += w->keys->length;
    }
    return json_put(w, SJS_STR("{}"));
}

/* A value: written whole, or its container opened. */
static sjs_result json_value_out(json_writer *w, sjs_value v) {
    w->visited++;
    switch (v.kind) {
    case SJS_NULL:
        return json_put(w, SJS_STR("null"));
    case SJS_BOOLEAN:
        return json_put(w, v.boolean ? SJS_STR("true") : SJS_STR("false"));
    case SJS_NUMBER: {
        if (!sjs_number_is_finite(v.as.number)) return json_put(w, SJS_STR("null"));
        char digits[32];
        return json_put(w, (sjs_str){ digits, sjs_number_format(v.as.number, digits) });
    }
    case SJS_STRING:
        return json_quote(w, sjs_string_view(SJS_AS_STRING(v)));
    case SJS_BIGINT:
        return sjs_vm_fault(w->vm, "JSON.stringify of a BigInt throws a TypeError in JavaScript");
    case SJS_OBJECT: {
        const sjs_entry *e = sjs_object_find(SJS_AS_OBJECT(v), SJS_STR("toJSON"));
        if (e != NULL && json_callable(e->value)) {
            return sjs_vm_fault(w->vm, "JSON.stringify calls an object's toJSON method, which staticjs does not");
        }
        break;
    }
    case SJS_ARRAY:
        break;
    case SJS_TEMPORAL: {
        /* Its toJSON gives its ISO string, which is written; a Date's gives
         * null when it is invalid (docs/date.md). */
        char text[SJS_TEMPORAL_TEXT_MAX];
        uint32_t n = sjs_temporal_text(SJS_AS_TEMPORAL(v), text);
        if (n == 0) return json_put(w, SJS_STR("null"));
        return json_quote(w, (sjs_str){ text, n });
    }
    default:
        return json_empty(w, v);
    }
    if (json_put(w, v.kind == SJS_ARRAY ? SJS_STR("[") : SJS_STR("{")) != SJS_OK) return SJS_ERR_PROGRAM;
    json_frame f = { v, 0, 0 };
    sjs_result e = sjs_buf_append(&w->vm->context->counting, &w->frames, (sjs_bytes){ (const uint8_t *)&f, sizeof f });
    return e == SJS_OK ? SJS_OK : sjs_vm_fail(w->vm, e);
}

/* The container on top is done: its end, on a line of its own when it has
 * items and there is a gap. */
static sjs_result json_close_out(json_writer *w, json_frame *f) {
    uint32_t depth = SJS_BUF_COUNT(w->frames, json_frame);
    sjs_str end = f->value.kind == SJS_ARRAY ? SJS_STR("]") : SJS_STR("}");
    if (f->written != 0 && json_indent(w, depth - 1u) != SJS_OK) return SJS_ERR_PROGRAM;
    w->frames.length -= (uint32_t)sizeof(json_frame);
    return json_put(w, end);
}

/* The next item of the container on top. */
static sjs_result json_step(json_writer *w) {
    uint32_t depth = SJS_BUF_COUNT(w->frames, json_frame);
    json_frame *f = &SJS_BUF_AT(w->frames, json_frame, depth - 1u);
    if (f->value.kind == SJS_ARRAY) {
        const sjs_array *a = SJS_AS_ARRAY(f->value);
        if (f->index == a->length) return json_close_out(w, f);
        sjs_value item = a->items[f->index++];
        if (f->written++ != 0 && json_put(w, SJS_STR(",")) != SJS_OK) return SJS_ERR_PROGRAM;
        if (json_indent(w, depth) != SJS_OK) return SJS_ERR_PROGRAM;
        if (json_omitted(item)) {
            w->visited++;
            return json_put(w, SJS_STR("null"));
        }
        return json_value_out(w, item);
    }
    const sjs_object *o = SJS_AS_OBJECT(f->value);
    const sjs_entry *e;
    if (w->keys != NULL) {
        if (f->index == w->keys->length) return json_close_out(w, f);
        const sjs_string *key = SJS_AS_STRING(w->keys->items[f->index++]);
        e = sjs_object_find(o, sjs_string_view(key));
        if (e == NULL) {
            const sjs_name *name = sjs_name_of(key);
            if (name != NULL && (name->flags & SJS_NAME_OBJECT) != 0) {
                sjs_vm_fault(w->vm, "JSON.stringify would read this key of its list from a prototype");
                sjs_error_subject(&w->vm->error, sjs_string_view(key));
                return SJS_ERR_PROGRAM;
            }
            return json_omit(w);
        }
    } else {
        if (f->index == o->count) return json_close_out(w, f);
        e = &o->entries[f->index++];
    }
    if (json_omitted(e->value)) return json_omit(w);
    if (f->written++ != 0 && json_put(w, SJS_STR(",")) != SJS_OK) return SJS_ERR_PROGRAM;
    if (json_indent(w, depth) != SJS_OK || json_quote(w, sjs_string_view(e->key)) != SJS_OK) return SJS_ERR_PROGRAM;
    if (json_put(w, w->gap_length != 0 ? SJS_STR(": ") : SJS_STR(":")) != SJS_OK) return SJS_ERR_PROGRAM;
    return json_value_out(w, e->value);
}

/* The gap from `space`: a number of spaces, clamped to 10, or a string's
 * first 10 UTF-16 code units, which must not end inside a pair. Anything
 * else gives none, as in JavaScript. A number between 0 and 1 faults: the
 * specification gives no gap, and V8 (Node) breaks lines all the same. */
static sjs_result json_gap(json_writer *w, sjs_value space) {
    if (space.kind == SJS_NUMBER) {
        double d = space.as.number;
        if (d > 0 && d < 1) {
            /* The specification gives no gap; V8 breaks lines with an empty one. */
            return sjs_vm_fault(w->vm, "JSON.stringify with a space between 0 and 1, which engines write differently");
        }
        uint32_t n = d >= 10.0 ? 10u : d >= 1.0 ? (uint32_t)d : 0u;
        for (uint32_t i = 0; i < n; i++) w->gap[i] = ' ';
        w->gap_length = n;
        return SJS_OK;
    }
    if (space.kind != SJS_STRING) return SJS_OK;
    sjs_str s = sjs_string_view(SJS_AS_STRING(space));
    uint32_t units = 0;
    uint32_t i = 0;
    while (i < s.length && units < 10u) {
        uint32_t next = i;
        int32_t point = sjs_utf8_next(s, &next);
        units += point >= 0x10000 ? 2u : 1u;
        if (units > 10u) {
            return sjs_vm_fault(w->vm, "JSON.stringify's space would end inside a surrogate pair, which a staticjs string cannot hold");
        }
        i = next;
    }
    sjs_copy(w->gap, s.ptr, i);
    w->gap_length = i;
    return SJS_OK;
}

/* An array replacer's keys: its strings and numbers' strings, each once, in
 * order; anything else in it is skipped, as in JavaScript. */
static sjs_result json_keys(json_writer *w, sjs_value replacer) {
    sjs_context *context = w->vm->context;
    const sjs_array *a = SJS_AS_ARRAY(replacer);
    sjs_value set;
    sjs_result e = sjs_set_new(context, 0, &set);
    if (e != SJS_OK) return sjs_vm_fail(w->vm, e);
    w->keys = SJS_AS_ARRAY(set);
    w->visited += a->length;
    for (uint32_t i = 0; i < a->length; i++) {
        sjs_value item = a->items[i];
        if (item.kind != SJS_STRING && item.kind != SJS_NUMBER) continue;
        sjs_string *key;
        e = sjs_to_key(context, item, &key);
        if (e == SJS_OK) {
            e = sjs_set_insert(context, &w->keys, sjs_heap_value(SJS_STRING, &key->h));
            if (e != SJS_OK) sjs_unref(context, sjs_heap_value(SJS_STRING, &key->h));
        }
        if (e != SJS_OK) return sjs_vm_fail(w->vm, e);
    }
    return SJS_OK;
}

static sjs_result json_write(json_writer *w, sjs_leaf_call *call) {
    sjs_value value = json_arg(call, 0);
    sjs_value replacer = json_arg(call, 1);
    if (json_callable(replacer)) {
        return sjs_vm_fault(w->vm, "JSON.stringify with a replacer function calls it back for every value, which staticjs does not");
    }
    if (replacer.kind == SJS_ARRAY && json_keys(w, replacer) != SJS_OK) return SJS_ERR_PROGRAM;
    if (json_gap(w, json_arg(call, 2)) != SJS_OK) return SJS_ERR_PROGRAM;
    if (json_omitted(value)) return SJS_OK;
    if (json_value_out(w, value) != SJS_OK) return SJS_ERR_PROGRAM;
    while (w->frames.length != 0) {
        if (json_step(w) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    sjs_string *s;
    sjs_result e = sjs_string_make(w->vm->context, (sjs_str){ (const char *)w->out.ptr, w->out.length }, &s);
    if (e != SJS_OK) return sjs_vm_fail(w->vm, e);
    call->result = sjs_heap_value(SJS_STRING, &s->h);
    return SJS_OK;
}

/* JSON.stringify(value, replacer, space): a fuel unit per 16 bytes written,
 * per value visited and per key of an array replacer looked up. */
sjs_result sjs_json_stringify(sjs_vm *vm, sjs_leaf_call *call) {
    json_writer w = { 0 };
    w.vm = vm;
    sjs_result result = json_write(&w, call);
    call->cost = w.visited + w.out.length / 16u;
    sjs_buf_free(&vm->context->counting, &w.out);
    sjs_buf_free(&vm->context->counting, &w.frames);
    if (w.keys != NULL) sjs_unref(vm->context, sjs_heap_value(SJS_SET, &w.keys->h));
    return result;
}

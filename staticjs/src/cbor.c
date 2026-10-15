/*
 * A value read from CBOR: one neutral value (tinyactors-core/docs/protocol.md,
 * "Values"), as the ECMAScript data model reads one. An integer within
 * +-(2^53 - 1) is a number and any other a BigInt; a float is a number; a
 * map with text keys is an object; a reference (tag 32 on text) is its
 * address. Nothing else is a neutral value: another tag, another simple
 * value, an indefinite length, a key that is not text, a value nested
 * deeper than 64, or bytes after the item. staticjs has no bytes, so a
 * byte string is a neutral value it does not have. The containers open are
 * an explicit stack, as everything in staticjs keeps one.
 */
#include "internal.h"

/* The deepest a neutral value nests (tinyactors-core's codec). */
#define CBOR_READ_DEPTH 64u

/* A container being filled: owned until it is full and put into the one
 * around it, or becomes the result. */
typedef struct {
    sjs_value container;   /* an array or an object */
    uint32_t left;         /* items still to come; an object's entries */
    sjs_string *key;       /* an object's key, read, whose value comes next; or NULL */
} cbor_level;

typedef struct {
    sjs_context *context;
    sjs_bytes bytes;
    uint32_t at;
    sjs_buf levels;        /* cbor_level */
    sjs_value result;
    sjs_read_problem *problem;
} cbor_reader;

static sjs_result cbor_problem(cbor_reader *r, sjs_error_code code, const char *message) {
    *r->problem = (sjs_read_problem){ code, message, { "", 0 }, r->at };
    return SJS_ERR_PROGRAM;
}

static sjs_result cbor_malformed(cbor_reader *r, const char *message) {
    return cbor_problem(r, SJS_ERROR_SYNTAX, message);
}

static sjs_result cbor_fail(cbor_reader *r, sjs_result e) {
    return e == SJS_ERR_LIMIT ? cbor_problem(r, SJS_ERROR_LIMIT, "a value is larger than the limits allow")
                              : cbor_problem(r, SJS_ERROR_MEMORY, "out of memory");
}

static cbor_level *cbor_top(cbor_reader *r) {
    uint32_t n = SJS_BUF_COUNT(r->levels, cbor_level);
    return n == 0 ? NULL : &SJS_BUF_AT(r->levels, cbor_level, n - 1u);
}

static uint32_t cbor_left(const cbor_reader *r) {
    return r->bytes.length - r->at;
}

/* An item's head: its major type and its argument, at r->at. */
typedef struct {
    uint8_t major;
    uint8_t info;          /* the low five bits */
    uint64_t argument;
} cbor_head;

static sjs_result cbor_read_head(cbor_reader *r, cbor_head *out) {
    if (cbor_left(r) == 0) return cbor_malformed(r, "the CBOR ends inside an item");
    uint8_t first = r->bytes.ptr[r->at];
    out->major = (uint8_t)(first >> 5);
    out->info = (uint8_t)(first & 31u);
    if (out->info >= 28u) {
        return cbor_malformed(r, out->info == 31u ? "an indefinite length is not a neutral value" : "a CBOR head that is not defined");
    }
    uint32_t size = out->info < 24u ? 0u : 1u << (out->info - 24u);
    if (cbor_left(r) - 1u < size) return cbor_malformed(r, "the CBOR ends inside an item");
    r->at++;
    out->argument = out->info < 24u ? out->info : 0u;
    for (uint32_t i = 0; i < size; i++) out->argument = out->argument << 8 | r->bytes.ptr[r->at++];
    return SJS_OK;
}

/* 2^n for n within a double's exponents, made exactly. */
static double cbor_power_of_two(int32_t n) {
    double v = 1.0;
    double step = n < 0 ? 0.5 : 2.0;
    for (int32_t i = n < 0 ? -n : n; i > 0; i--) v *= step;
    return v;
}

static double cbor_half(uint16_t h) {
    int32_t exponent = (h >> 10) & 31;
    double mantissa = (double)(h & 0x3FFu);
    double v = exponent == 0 ? mantissa * cbor_power_of_two(-24)
             : exponent == 31 ? (mantissa == 0 ? 1.0 / 0.0 : 0.0 / 0.0)
                              : (1024.0 + mantissa) * cbor_power_of_two(exponent - 25);
    return (h & 0x8000u) != 0 ? -v : v;
}

typedef union {
    uint32_t u;
    float f;
} cbor_float_bits;

typedef union {
    uint64_t u;
    double d;
} cbor_double_bits;

/* An integer as the ECMAScript data model reads one: a number within
 * +-(2^53 - 1), else a BigInt. `negative` reads -1 - n. */
static sjs_result cbor_integer(cbor_reader *r, const cbor_head *head, sjs_value *out) {
    uint64_t n = head->argument;
    bool negative = head->major == 1u;
    const uint64_t safe = UINT64_C(9007199254740991);
    if (!negative && n <= safe) {
        *out = sjs_num((double)n);
        return SJS_OK;
    }
    if (negative && n < safe) {
        *out = sjs_num(-(double)n - 1.0);
        return SJS_OK;
    }
    /* The magnitude in hexadecimal: n, or n + 1 for a negative one, which
     * reaches 2^64. */
    char hex[17];
    uint32_t length = 0;
    bool carry = negative && n == UINT64_MAX;
    uint64_t magnitude = negative ? (carry ? 0u : n + 1u) : n;
    char digits[16];
    uint32_t count = 0;
    do {
        digits[count++] = "0123456789abcdef"[magnitude & 15u];
        magnitude >>= 4;
    } while (magnitude != 0);
    if (carry) {
        hex[length++] = '1';
        for (uint32_t i = 0; i < 16u; i++) hex[length++] = '0';
    } else {
        while (count > 0) hex[length++] = digits[--count];
    }
    sjs_result e = sjs_bigint_from_digits(r->context, (sjs_bigint_digits){ { hex, length }, 16u, negative }, out);
    return e == SJS_OK ? SJS_OK : cbor_fail(r, e);
}

/* Text of `length` bytes at r->at: well-formed UTF-8, which leaves out an
 * encoded surrogate (CESU-8). */
static sjs_result cbor_text(cbor_reader *r, uint64_t length, sjs_str *out) {
    if (length > cbor_left(r)) return cbor_malformed(r, "the CBOR ends inside an item");
    sjs_str text = { (const char *)r->bytes.ptr + r->at, (uint32_t)length };
    uint32_t valid = sjs_utf8_valid_length(text);
    if (valid != text.length) {
        r->at += valid;
        return cbor_malformed(r, "text that is not UTF-8");
    }
    r->at += (uint32_t)length;
    *out = text;
    return SJS_OK;
}

/* A value read: the result, or an item of the container open, which is put
 * into the one around it once it is full. Takes over `v`. */
static sjs_result cbor_deliver(cbor_reader *r, sjs_value v) {
    for (;;) {
        cbor_level *l = cbor_top(r);
        if (l == NULL) {
            r->result = v;
            return SJS_OK;
        }
        if (l->container.kind == SJS_ARRAY) {
            sjs_array *a = SJS_AS_ARRAY(l->container);
            a->items[a->length++] = v;   /* made with room for every item */
        } else {
            sjs_object *o = SJS_AS_OBJECT(l->container);
            sjs_string *key = l->key;
            l->key = NULL;
            sjs_result e = sjs_object_put_key(r->context, &o, (sjs_put){ key, v });
            l->container.as.heap = &o->h;
            if (e != SJS_OK) {
                sjs_unref(r->context, sjs_heap_value(SJS_STRING, &key->h));
                sjs_unref(r->context, v);
                return cbor_fail(r, e);
            }
        }
        if (--l->left != 0) return SJS_OK;
        v = l->container;
        r->levels.length -= (uint32_t)sizeof(cbor_level);
    }
}

/* An array or a map of `count` items. */
static sjs_result cbor_open(cbor_reader *r, uint8_t major, uint64_t count) {
    /* Every item takes a byte at least, and a map's entry two. */
    if (count > cbor_left(r) / (major == 5u ? 2u : 1u)) return cbor_malformed(r, "the CBOR ends inside an item");
    uint32_t depth = SJS_BUF_COUNT(r->levels, cbor_level) + 1u;
    if (depth > CBOR_READ_DEPTH) return cbor_malformed(r, "a value nested deeper than 64 is not a neutral value");
    if (depth > r->context->limits.max_nesting) {
        cbor_fail(r, SJS_ERR_LIMIT);
        r->problem->subject = SJS_STR("max_nesting");
        return SJS_ERR_PROGRAM;
    }
    cbor_level l = { sjs_undefined(), (uint32_t)count, NULL };
    sjs_result e;
    if (major == 4u) {
        sjs_array *a;
        e = sjs_array_make(r->context, (uint32_t)count, &a);
        if (e == SJS_OK) l.container = sjs_heap_value(SJS_ARRAY, &a->h);
    } else {
        sjs_object *o;
        e = sjs_object_make(r->context, (uint32_t)count, &o);
        if (e == SJS_OK) l.container = sjs_heap_value(SJS_OBJECT, &o->h);
    }
    if (e != SJS_OK) return cbor_fail(r, e);
    if (count == 0) return cbor_deliver(r, l.container);
    e = sjs_buf_append(&r->context->counting, &r->levels, (sjs_bytes){ (const uint8_t *)&l, sizeof l });
    if (e != SJS_OK) {
        sjs_unref(r->context, l.container);
        return cbor_fail(r, e);
    }
    return SJS_OK;
}

/* A map's key: text, of which "__proto__" is one staticjs objects do not
 * have. */
static sjs_result cbor_key(cbor_reader *r, cbor_level *l) {
    uint32_t at = r->at;
    cbor_head head;
    if (cbor_read_head(r, &head) != SJS_OK) return SJS_ERR_PROGRAM;
    if (head.major != 3u) {
        r->at = at;
        return cbor_malformed(r, "a map's key that is not text is not a neutral value");
    }
    sjs_str text;
    if (cbor_text(r, head.argument, &text) != SJS_OK) return SJS_ERR_PROGRAM;
    if (sjs_str_equal(text, SJS_STR("__proto__"))) {
        r->at = at;
        return cbor_problem(r, SJS_ERROR_UNSUPPORTED, "an object with the key __proto__: staticjs objects have no prototype");
    }
    sjs_result e = sjs_string_intern(r->context, text, &l->key);
    return e == SJS_OK ? SJS_OK : cbor_fail(r, e);
}

/* The item at r->at, delivered. */
static sjs_result cbor_item(cbor_reader *r) {
    uint32_t at = r->at;
    cbor_head head;
    if (cbor_read_head(r, &head) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_value v;
    switch (head.major) {
    case 0:
    case 1:
        if (cbor_integer(r, &head, &v) != SJS_OK) return SJS_ERR_PROGRAM;
        return cbor_deliver(r, v);
    case 2:
        r->at = at;
        return cbor_problem(r, SJS_ERROR_UNSUPPORTED, "bytes: staticjs has no byte string");
    case 3:
    text: {
        sjs_str text;
        if (cbor_text(r, head.argument, &text) != SJS_OK) return SJS_ERR_PROGRAM;
        sjs_string *s;
        sjs_result e = sjs_string_make(r->context, text, &s);
        if (e != SJS_OK) return cbor_fail(r, e);
        return cbor_deliver(r, sjs_heap_value(SJS_STRING, &s->h));
    }
    case 4:
    case 5:
        return cbor_open(r, head.major, head.argument);
    case 6:
        /* A reference: tag 32 on its address, which is text. */
        if (head.argument != 32u) {
            r->at = at;
            return cbor_malformed(r, "a tag other than 32 is not a neutral value");
        }
        at = r->at;
        if (cbor_read_head(r, &head) != SJS_OK) return SJS_ERR_PROGRAM;
        if (head.major != 3u) {
            r->at = at;
            return cbor_malformed(r, "tag 32 is on text, an address");
        }
        goto text;
    default:
        break;
    }
    /* Major type 7: false, true, null, undefined and floats. */
    switch (head.info) {
    case 20:
        v = sjs_boolean(false);
        break;
    case 21:
        v = sjs_boolean(true);
        break;
    case 22:
        v = sjs_null();
        break;
    case 23:
        v = sjs_undefined();
        break;
    case 25:
        v = sjs_num(cbor_half((uint16_t)head.argument));
        break;
    case 26: {
        cbor_float_bits b;
        b.u = (uint32_t)head.argument;
        v = sjs_num((double)b.f);
        break;
    }
    case 27: {
        cbor_double_bits b;
        b.u = head.argument;
        v = sjs_num(b.d);
        break;
    }
    default:
        r->at = at;
        return cbor_malformed(r, "a simple value other than false, true, null and undefined is not a neutral value");
    }
    return cbor_deliver(r, v);
}

sjs_result sjs_cbor_read(const sjs_read_input *in, sjs_value *out) {
    sjs_context *context = in->context;
    sjs_bytes bytes = in->bytes;
    cbor_reader r = { 0 };
    r.context = context;
    r.bytes = bytes;
    r.result = sjs_undefined();
    r.problem = in->problem;
    sjs_result result = SJS_OK;
    bool done = false;
    while (result == SJS_OK && !done) {
        cbor_level *l = cbor_top(&r);
        if (l != NULL && l->container.kind == SJS_OBJECT && l->key == NULL) {
            result = cbor_key(&r, l);
            continue;
        }
        result = cbor_item(&r);
        done = cbor_top(&r) == NULL;
    }
    if (result == SJS_OK && r.at != bytes.length) result = cbor_malformed(&r, "bytes after the item");
    uint32_t n = SJS_BUF_COUNT(r.levels, cbor_level);
    for (uint32_t i = 0; i < n; i++) {
        cbor_level *l = &SJS_BUF_AT(r.levels, cbor_level, i);
        sjs_unref(context, l->container);
        if (l->key != NULL) sjs_unref(context, sjs_heap_value(SJS_STRING, &l->key->h));
    }
    sjs_buf_free(&context->counting, &r.levels);
    if (result != SJS_OK) {
        sjs_unref(context, r.result);
        return result;
    }
    *out = r.result;
    return SJS_OK;
}

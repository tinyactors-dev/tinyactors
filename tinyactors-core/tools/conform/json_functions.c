/* The json datamodel's function library for the conformance suite
 * (tinyactors-test/suites/w3c/confJSON.xsl): what the conf: notation's
 * comparisons and arithmetic become, as functions, since the datamodel has
 * no operators. A spike, on a branch.
 *
 * eq ne lt le gt ge and or not add concat isDefined has length append text
 * startsWith contains
 */
#include "conform.h"

#include <math.h>
#include <stdio.h>
#include <string.h>


/* The functions below write their result with a ta_cbor_writer; the
 * adapter runs one over the room the datamodel lends (call->result). */
typedef struct writer_call {
    ta_bytes arguments;
    ta_cbor_writer *result;
    ta_string_view message;
} writer_call;

typedef ta_status (*writer_function)(writer_call *call);

typedef struct writer_entry {
    const char *name;
    writer_function function;
} writer_entry;

static ta_status writer_adapter(ta_json_call *call) {
    const writer_entry *entry = (const writer_entry *)call->context;
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){call->result->pointer, call->result->capacity, 0}, NULL);
    writer_call inner = {call->arguments, &writer, {NULL, 0}};
    ta_status status = entry->function(&inner);
    call->message = inner.message;
    if (status != TA_OK) return status;
    ta_bytes out;
    status = ta_cbor_writer_bytes(&writer, &out);
    if (status != TA_OK) return status;
    call->result->length = out.length;
    return TA_OK;
}

typedef struct fv {
    const uint8_t *p;
    uint32_t n;
} fv;

static int head(fv v, unsigned *major, uint64_t *value, uint32_t *length) {
    if (!v.n) return 0;
    *major = v.p[0] >> 5;
    unsigned info = v.p[0] & 31;
    uint32_t extra = info < 24 ? 0 : info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : 99;
    if (extra == 99 || 1 + extra > v.n) return 0;
    *value = info < 24 ? info : 0;
    for (uint32_t i = 0; i < extra; i++) *value = (*value << 8) | v.p[1 + i];
    *length = 1 + extra;
    return 1;
}

static uint32_t size_of(fv v) {
    uint64_t pending = 1;
    uint32_t at = 0;
    while (pending) {
        unsigned major;
        uint64_t value;
        uint32_t length;
        if (!head((fv){v.p + at, v.n - at}, &major, &value, &length)) return 0;
        pending--;
        at += length;
        if (major == 2 || major == 3) at += (uint32_t)value;
        else if (major == 4) pending += value;
        else if (major == 5) pending += 2 * value;
        else if (major == 6) pending += 1;
    }
    return at;
}

/* The call's arguments, split. */
static int arguments(writer_call *call, fv *out, uint32_t want) {
    fv all = {call->arguments.pointer, (uint32_t)call->arguments.length};
    unsigned major;
    uint64_t count;
    uint32_t length;
    if (!head(all, &major, &count, &length) || major != 4 || count != want) {
        call->message = (ta_string_view){"wrong number of arguments", 25};
        return 0;
    }
    uint32_t at = length;
    for (uint32_t i = 0; i < want; i++) {
        out[i].p = all.p + at;
        out[i].n = size_of((fv){all.p + at, all.n - at});
        at += out[i].n;
    }
    return 1;
}

/* A number: 1 and *d (and *exact for integers), or 0. */
static int number(fv v, double *d) {
    unsigned major;
    uint64_t value;
    uint32_t length;
    if (!head(v, &major, &value, &length)) return 0;
    if (major == 0) return *d = (double)value, 1;
    if (major == 1) return *d = -1.0 - (double)value, 1;
    if (major == 7 && length == 9) {
        memcpy(d, &value, 8);
        return 1;
    }
    if (major == 7 && length == 5) {
        uint32_t bits = (uint32_t)value;
        float f;
        memcpy(&f, &bits, 4);
        *d = f;
        return 1;
    }
    if (major == 7 && length == 3) { /* half */
        unsigned h = (unsigned)value, e = (h >> 10) & 31, m = h & 1023;
        double r = e == 0 ? ldexp(m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexp(m + 1024, (int)e - 25);
        *d = (h & 0x8000) ? -r : r;
        return 1;
    }
    return 0;
}

static int text(fv v, ta_string_view *out) {
    unsigned major;
    uint64_t value;
    uint32_t length;
    if (!head(v, &major, &value, &length)) return 0;
    if (major == 6 && value == 32) return text((fv){v.p + length, v.n - length}, out);
    if (major != 3) return 0;
    *out = (ta_string_view){(const char *)v.p + length, (uint32_t)value};
    return 1;
}

static int boolean(fv v, int *out) {
    if (v.n == 1 && (v.p[0] == 0xF5 || v.p[0] == 0xF4)) return *out = v.p[0] == 0xF5, 1;
    return 0;
}

static int map_get(fv map, ta_string_view key, fv *out) {
    unsigned major;
    uint64_t count;
    uint32_t at;
    if (!head(map, &major, &count, &at) || major != 5) return 0;
    for (uint64_t i = 0; i < count; i++) {
        fv k = {map.p + at, size_of((fv){map.p + at, map.n - at})};
        at += k.n;
        fv v = {map.p + at, size_of((fv){map.p + at, map.n - at})};
        at += v.n;
        ta_string_view kt;
        if (text(k, &kt) && kt.length == key.length && !memcmp(kt.pointer, key.pointer, key.length)) {
            *out = v;
            return 1;
        }
    }
    return 0;
}

static int equal(fv a, fv b) {
    double x, y;
    if (number(a, &x) && number(b, &y)) return x == y;
    if (a.n == b.n && !memcmp(a.p, b.p, a.n)) return 1;
    unsigned ma, mb;
    uint64_t ca, cb;
    uint32_t la, lb;
    if (!head(a, &ma, &ca, &la) || !head(b, &mb, &cb, &lb) || ma != mb || ca != cb) return 0;
    if (ma == 4) {
        uint32_t ia = la, ib = lb;
        for (uint64_t i = 0; i < ca; i++) {
            fv ea = {a.p + ia, size_of((fv){a.p + ia, a.n - ia})}, eb = {b.p + ib, size_of((fv){b.p + ib, b.n - ib})};
            if (!equal(ea, eb)) return 0;
            ia += ea.n;
            ib += eb.n;
        }
        return 1;
    }
    if (ma == 5) {
        uint32_t at = la;
        for (uint64_t i = 0; i < ca; i++) {
            fv k = {a.p + at, size_of((fv){a.p + at, a.n - at})};
            at += k.n;
            fv v = {a.p + at, size_of((fv){a.p + at, a.n - at})};
            at += v.n;
            ta_string_view kt;
            fv other;
            if (!text(k, &kt) || !map_get(b, kt, &other) || !equal(v, other)) return 0;
        }
        return 1;
    }
    return 0;
}

static ta_status give_bool(writer_call *call, int b) {
    ta_cbor_write_simple(call->result, b ? TA_CBOR_TRUE : TA_CBOR_FALSE);
    return TA_OK;
}

static ta_status refuse(writer_call *call, const char *why) {
    call->message = (ta_string_view){why, (uint32_t)strlen(why)};
    return TA_ERROR_INVALID;
}

static ta_status f_equal(writer_call *c) {
    fv a[2];
    return arguments(c, a, 2) ? give_bool(c, equal(a[0], a[1])) : TA_ERROR_INVALID;
}
static ta_status f_ne(writer_call *c) {
    fv a[2];
    return arguments(c, a, 2) ? give_bool(c, !equal(a[0], a[1])) : TA_ERROR_INVALID;
}

static int compare(writer_call *c, int *out) {
    fv a[2];
    if (!arguments(c, a, 2)) return 0;
    double x, y;
    ta_string_view s, t;
    if (number(a[0], &x) && number(a[1], &y)) return *out = x < y ? -1 : x > y ? 1 : 0, 1;
    if (text(a[0], &s) && text(a[1], &t)) {
        int r = memcmp(s.pointer, t.pointer, s.length < t.length ? s.length : t.length);
        return *out = r ? r : (int)s.length - (int)t.length, 1;
    }
    refuse(c, "compare: two numbers or two strings");
    return 0;
}
static ta_status f_lt(writer_call *c) { int r; return compare(c, &r) ? give_bool(c, r < 0) : TA_ERROR_INVALID; }
static ta_status f_le(writer_call *c) { int r; return compare(c, &r) ? give_bool(c, r <= 0) : TA_ERROR_INVALID; }
static ta_status f_gt(writer_call *c) { int r; return compare(c, &r) ? give_bool(c, r > 0) : TA_ERROR_INVALID; }
static ta_status f_ge(writer_call *c) { int r; return compare(c, &r) ? give_bool(c, r >= 0) : TA_ERROR_INVALID; }

static ta_status f_and(writer_call *c) {
    fv a[2];
    int x, y;
    if (!arguments(c, a, 2) || !boolean(a[0], &x) || !boolean(a[1], &y)) return refuse(c, "and: two booleans");
    return give_bool(c, x && y);
}
static ta_status f_or(writer_call *c) {
    fv a[2];
    int x, y;
    if (!arguments(c, a, 2) || !boolean(a[0], &x) || !boolean(a[1], &y)) return refuse(c, "or: two booleans");
    return give_bool(c, x || y);
}
static ta_status f_not(writer_call *c) {
    fv a[1];
    int x;
    if (!arguments(c, a, 1) || !boolean(a[0], &x)) return refuse(c, "not: a boolean");
    return give_bool(c, !x);
}

static ta_status f_add(writer_call *c) {
    fv a[2];
    double x, y;
    if (!arguments(c, a, 2) || !number(a[0], &x) || !number(a[1], &y)) return refuse(c, "add: two numbers");
    double r = x + y;
    if (r == floor(r) && fabs(r) < 9.0e15) {
        if (r < 0) ta_cbor_write_negative(c->result, (uint64_t)(-1 - (int64_t)r));
        else ta_cbor_write_uint(c->result, (uint64_t)r);
    } else
        ta_cbor_write_float(c->result, r);
    return TA_OK;
}

static ta_status f_concat(writer_call *c) {
    fv a[2];
    ta_string_view s, t;
    if (!arguments(c, a, 2) || !text(a[0], &s) || !text(a[1], &t)) return refuse(c, "concat: two strings");
    char buffer[4096];
    if (s.length + t.length > sizeof buffer) return refuse(c, "concat: too long");
    memcpy(buffer, s.pointer, s.length);
    memcpy(buffer + s.length, t.pointer, t.length);
    ta_cbor_write_text(c->result, (ta_string_view){buffer, s.length + t.length});
    return TA_OK;
}

static ta_status f_is_defined(writer_call *c) {
    fv a[1];
    if (!arguments(c, a, 1)) return TA_ERROR_INVALID;
    return give_bool(c, !(a[0].n == 1 && a[0].p[0] == 0xF7));
}

static ta_status f_has(writer_call *c) {
    fv a[2], v;
    ta_string_view key;
    if (!arguments(c, a, 2) || !text(a[1], &key)) return refuse(c, "has: a map and a key");
    return give_bool(c, map_get(a[0], key, &v));
}

static ta_status f_length(writer_call *c) {
    fv a[1];
    unsigned major;
    uint64_t count;
    uint32_t length;
    if (!arguments(c, a, 1) || !head(a[0], &major, &count, &length) || (major != 4 && major != 3))
        return refuse(c, "length: an array or a string");
    ta_cbor_write_uint(c->result, count);
    return TA_OK;
}

/* The writer has no raw item call: an item's tokens are written again. */
static void write_item(ta_cbor_writer *w, fv v) {
    ta_cbor_reader r;
    ta_cbor_item it;
    ta_cbor_reader_init(&r, (ta_bytes){v.p, v.n});
    while (ta_cbor_read(&r, &it) == 1) {
        switch (it.kind) {
        case TA_CBOR_UINT: ta_cbor_write_uint(w, it.value); break;
        case TA_CBOR_NEGINT: ta_cbor_write_negative(w, it.value); break;
        case TA_CBOR_TEXT: ta_cbor_write_text(w, (ta_string_view){it.bytes.pointer, (uint32_t)it.bytes.length}); break;
        case TA_CBOR_BYTES: ta_cbor_write_bytes(w, it.bytes); break;
        case TA_CBOR_ARRAY: ta_cbor_write_array(w, it.count); break;
        case TA_CBOR_MAP: ta_cbor_write_map(w, it.count); break;
        case TA_CBOR_TAG: ta_cbor_write_tag(w, it.value); break;
        case TA_CBOR_SIMPLE: ta_cbor_write_simple(w, (uint32_t)it.value); break;
        case TA_CBOR_FLOAT: {
            double d;
            memcpy(&d, &it.value, 8);
            ta_cbor_write_float(w, d);
            break;
        }
        }
    }
}

static ta_status f_append(writer_call *c) {
    fv a[2];
    unsigned major;
    uint64_t count;
    uint32_t length;
    if (!arguments(c, a, 2) || !head(a[0], &major, &count, &length) || major != 4) return refuse(c, "append: an array");
    ta_cbor_write_array(c->result, (uint32_t)count + 1);
    uint32_t at = length;
    for (uint64_t i = 0; i < count; i++) {
        fv e = {a[0].p + at, size_of((fv){a[0].p + at, a[0].n - at})};
        write_item(c->result, e);
        at += e.n;
    }
    write_item(c->result, a[1]);
    return TA_OK;
}

static ta_status f_text(writer_call *c) {
    fv a[1];
    ta_string_view s;
    if (!arguments(c, a, 1)) return TA_ERROR_INVALID;
    if (text(a[0], &s)) {
        ta_cbor_write_text(c->result, s);
        return TA_OK;
    }
    char buffer[1024];
    ta_buffer out = {buffer, sizeof buffer, 0};
    if (ta_cbor_diagnostic((ta_bytes){a[0].p, a[0].n}, &out) != TA_OK) return refuse(c, "text: too long");
    ta_cbor_write_text(c->result, (ta_string_view){buffer, (uint32_t)out.length});
    return TA_OK;
}

static ta_status f_starts_with(writer_call *c) {
    fv a[2];
    ta_string_view s, t;
    if (!arguments(c, a, 2) || !text(a[0], &s) || !text(a[1], &t)) return refuse(c, "startsWith: two strings");
    return give_bool(c, t.length <= s.length && !memcmp(s.pointer, t.pointer, t.length));
}

static ta_status f_contains(writer_call *c) {
    fv a[2];
    ta_string_view s, t;
    if (!arguments(c, a, 2) || !text(a[0], &s) || !text(a[1], &t)) return refuse(c, "contains: two strings");
    for (uint32_t i = 0; i + t.length <= s.length; i++)
        if (!memcmp(s.pointer + i, t.pointer, t.length)) return give_bool(c, 1);
    return give_bool(c, 0);
}

ta_status conform_json_functions(ta_datamodel_json *json) {
    static const writer_entry table[] = {{"eq", f_equal},         {"ne", f_ne},         {"lt", f_lt},         {"le", f_le},
                 {"gt", f_gt},         {"ge", f_ge},         {"and", f_and},       {"or", f_or},
                 {"not", f_not},       {"add", f_add},       {"concat", f_concat}, {"isDefined", f_is_defined},
                 {"has", f_has},       {"length", f_length}, {"append", f_append}, {"text", f_text},
                 {"startsWith", f_starts_with}, {"contains", f_contains}};
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        ta_json_function_entry entry;
        memset(&entry, 0, sizeof entry);
        entry.size = sizeof entry;
        entry.name = (ta_string_view){table[i].name, (uint32_t)strlen(table[i].name)};
        entry.function = writer_adapter;
        entry.context = (void *)&table[i];
        ta_status status = ta_datamodel_json_register(json, &entry);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

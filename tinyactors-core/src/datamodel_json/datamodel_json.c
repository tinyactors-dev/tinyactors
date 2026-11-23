/* datamodel_json.c — the "json" datamodel, a spike of
 * docs/proposals/json-datamodel.md (branch work, not a decided layer).
 *
 * A session's variables are one canonical CBOR map; expressions are paths,
 * JSON literals, calls of registered functions and In(). Freestanding: it
 * allocates only through the allocator in its config.
 *
 * Spike simplifications, each noted where it is: _event is built as one CBOR
 * map at bind_event (its data copied, not referenced); a <foreach> copies its
 * array; _ioprocessors is per session; the parser recurses (bounded at 16);
 * duplicate keys in input are not refused; depth past 63 is not checked on
 * writes (core's reader still refuses past 64 where it reads).
 */
#include <tinyactors/datamodel_json.h>
#include <tinyactors/image.h>
#include <tinyactors/table.h>

#include "base/memory.h"

#define NIL 0xFFFFFFFFu
#define MAX_NEST 16

/* ---- memory ------------------------------------------------------------------------------ */

struct ta_datamodel_json;
typedef struct jmem {
    ta_allocator allocator;
    size_t bytes; /* held now, for measurement */
} jmem;

static void *jalloc(jmem *m, size_t size) {
    if (!size) size = 1;
    size_t *p = (size_t *)m->allocator.alloc(m->allocator.context, size + 8, 8);
    if (!p) return 0;
    *p = size;
    m->bytes += size + 8;
    return p + 1;
}

static void jfree(jmem *m, void *q) {
    if (!q) return;
    size_t *p = (size_t *)q - 1;
    m->bytes -= *p + 8;
    m->allocator.free(m->allocator.context, p, *p + 8);
}

/* A growable byte buffer. */
typedef struct jbuf {
    jmem *m;
    uint8_t *p;
    size_t n, cap;
    int failed;
} jbuf;

static void jb_init(jbuf *b, jmem *m) {
    b->m = m;
    b->p = 0;
    b->n = b->cap = 0;
    b->failed = 0;
}

static int jb_reserve(jbuf *b, size_t more) {
    if (b->failed) return 0;
    if (b->n + more <= b->cap) return 1;
    size_t cap = b->cap ? b->cap * 2 : 64;
    while (cap < b->n + more) cap *= 2;
    uint8_t *q = (uint8_t *)jalloc(b->m, cap);
    if (!q) {
        b->failed = 1;
        return 0;
    }
    if (b->n) ta_memcpy(q, b->p, b->n);
    jfree(b->m, b->p);
    b->p = q;
    b->cap = cap;
    return 1;
}

static void jb_put(jbuf *b, const void *p, size_t n) {
    if (!n || !jb_reserve(b, n)) return;
    ta_memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void jb_byte(jbuf *b, uint8_t c) { jb_put(b, &c, 1); }

static void jb_free(jbuf *b) {
    jfree(b->m, b->p);
    b->p = 0;
    b->n = b->cap = 0;
}

/* ---- CBOR heads, over canonical bytes ----------------------------------------------------- */

typedef struct jv {
    const uint8_t *p;
    uint32_t n;
} jv;

static void put_head(jbuf *b, unsigned major, uint64_t v) {
    uint8_t h[9];
    size_t l;
    h[0] = (uint8_t)(major << 5);
    if (v < 24) {
        h[0] |= (uint8_t)v;
        l = 1;
    } else if (v <= 0xFF) {
        h[0] |= 24;
        h[1] = (uint8_t)v;
        l = 2;
    } else if (v <= 0xFFFF) {
        h[0] |= 25;
        h[1] = (uint8_t)(v >> 8);
        h[2] = (uint8_t)v;
        l = 3;
    } else if (v <= 0xFFFFFFFFu) {
        h[0] |= 26;
        for (int i = 0; i < 4; i++) h[1 + i] = (uint8_t)(v >> (24 - 8 * i));
        l = 5;
    } else {
        h[0] |= 27;
        for (int i = 0; i < 8; i++) h[1 + i] = (uint8_t)(v >> (56 - 8 * i));
        l = 9;
    }
    jb_put(b, h, l);
}

typedef struct jhead {
    unsigned major, info;
    uint64_t value;
    uint32_t length; /* the head's bytes */
} jhead;

static int read_head(jv v, jhead *h) {
    if (!v.n) return 0;
    h->major = v.p[0] >> 5;
    h->info = v.p[0] & 31;
    uint32_t extra = h->info < 24 ? 0 : h->info == 24 ? 1 : h->info == 25 ? 2 : h->info == 26 ? 4 : h->info == 27 ? 8 : 99;
    if (extra == 99 || 1 + extra > v.n) return 0;
    h->value = h->info < 24 ? h->info : 0;
    for (uint32_t i = 0; i < extra; i++) h->value = (h->value << 8) | v.p[1 + i];
    h->length = 1 + extra;
    return 1;
}

/* The bytes of the item at the start of v (0 on malformed input). */
static uint32_t item_size(jv v) {
    uint64_t pending = 1;
    uint32_t at = 0;
    while (pending) {
        jhead h;
        if (!read_head((jv){v.p + at, v.n - at}, &h)) return 0;
        pending--;
        at += h.length;
        if (h.major == 2 || h.major == 3) {
            if (h.value > v.n - at) return 0;
            at += (uint32_t)h.value;
        } else if (h.major == 4)
            pending += h.value;
        else if (h.major == 5)
            pending += 2 * h.value;
        else if (h.major == 6)
            pending += 1;
    }
    return at;
}

static jv item(jv v) { return (jv){v.p, item_size(v)}; }

static int is_text(jv v, ta_string_view *out) {
    jhead h;
    if (!read_head(v, &h)) return 0;
    if (h.major == 6 && h.value == TA_CBOR_TAG_URI) { /* an address: its text */
        jv inner = {v.p + h.length, v.n - h.length};
        return read_head(inner, &h) && h.major == 3 && (out->pointer = (const char *)inner.p + h.length,
                                                        out->length = (uint32_t)h.value, 1);
    }
    if (h.major != 3) return 0;
    out->pointer = (const char *)v.p + h.length;
    out->length = (uint32_t)h.value;
    return 1;
}

static int simple_is(jv v, unsigned s) { return v.n == 1 && v.p[0] == (0xE0 | s); }

static int view_eq(ta_string_view a, const char *p, uint32_t n) {
    return a.length == n && ta_memcmp(a.pointer, p, n) == 0;
}

/* In the map v, the value under key: 1 and *out, or 0. */
static int map_find(jv v, ta_string_view key, jv *out) {
    jhead h;
    if (!read_head(v, &h) || h.major != 5) return 0;
    uint32_t at = h.length;
    for (uint64_t i = 0; i < h.value; i++) {
        jv k = item((jv){v.p + at, v.n - at});
        if (!k.n) return 0;
        at += k.n;
        jv val = item((jv){v.p + at, v.n - at});
        if (!val.n) return 0;
        ta_string_view kt;
        if (is_text(k, &kt) && view_eq(key, kt.pointer, kt.length)) {
            *out = val;
            return 1;
        }
        at += val.n;
    }
    return 0;
}

static int array_at(jv v, uint64_t index, jv *out) {
    jhead h;
    if (!read_head(v, &h) || h.major != 4 || index >= h.value) return 0;
    uint32_t at = h.length;
    for (uint64_t i = 0;; i++) {
        jv x = item((jv){v.p + at, v.n - at});
        if (!x.n) return 0;
        if (i == index) {
            *out = x;
            return 1;
        }
        at += x.n;
    }
}

/* ---- the datamodel's tables ----------------------------------------------------------- */

typedef struct jfunction {
    ta_string_view name, chart; /* copied */
    ta_json_function function;
    void *context;
} jfunction;

enum { N_ERROR = 1, N_LITERAL, N_PATH, N_CALL, N_IN };
enum { R_VARIABLE = 1, R_EVENT, R_SESSIONID, R_NAME, R_IOPROCESSORS };
enum { S_KEY = 1, S_POSITION, S_PATH };

typedef struct jnode {
    uint8_t kind, root;
    uint16_t count; /* segments, arguments */
    uint32_t a, b, c;
} jnode;

typedef struct jsegment {
    uint32_t kind, a, b;
} jsegment;

typedef struct jchart {
    const ta_image *image;
    uint32_t chart, segment;
    struct jchart *next; /* every attached chart, for a restore */
    jbuf nodes, segments, arguments, strings, literals; /* vectors */
    uint32_t *expression_node;                          /* per expression id */
    uint32_t expression_count;
    uint32_t *data_node; /* per <data>: the path of its id */
    uint32_t data_count;
} jchart;

typedef struct jsession {
    jchart *chart;
    uint8_t *document; /* one CBOR map */
    uint32_t document_length;
    uint8_t *event; /* _event as one CBOR map, or none */
    uint32_t event_length;
    uint8_t *system; /* [sessionid, name, ioprocessors] */
    uint32_t system_length;
    uint8_t *init; /* the params, until destroy */
    uint32_t init_length;
    uint8_t *kept; /* the blob's document, for migrate */
    uint32_t kept_length;
    uint32_t pending_fuel;
} jsession;

typedef struct jpayload {
    uint32_t references, length;
    uint8_t *bytes;
} jpayload;

typedef struct jloop {
    uint8_t *bytes;
    uint32_t length;
} jloop;

struct ta_datamodel_json {
    jmem memory;
    uint32_t max_document_bytes;
    jfunction *functions;
    uint32_t function_count, function_capacity;
    ta_table sessions, payloads, loops;
    jbuf out;      /* the last output: valid until the next call */
    jchart *charts;
    uint8_t *room; /* the result room */
    char message[256];
    uint32_t message_length, reason;
};

static void set_message(ta_datamodel_json *dm, const char *text, ta_string_view detail) {
    uint32_t n = 0;
    while (text[n] && n < 200) {
        dm->message[n] = text[n];
        n++;
    }
    for (uint32_t i = 0; i < detail.length && n < sizeof dm->message; i++) dm->message[n++] = detail.pointer[i];
    dm->message_length = n;
    dm->reason = TA_DATAMODEL_FAILURE_EXCEPTION;
}

static ta_string_view none_view(void) { return (ta_string_view){"", 0}; }

/* ---- parsing ----------------------------------------------------------------------------- */

typedef struct jparser {
    ta_datamodel_json *dm;
    jchart *chart;
    const char *s;
    uint32_t i, n, depth;
    const char *error;
} jparser;

static jnode *node_at(jchart *c, uint32_t i) { return (jnode *)(void *)c->nodes.p + i; }
static jsegment *segment_at(jchart *c, uint32_t i) { return (jsegment *)(void *)c->segments.p + i; }
static uint32_t argument_at(jchart *c, uint32_t i) { return ((uint32_t *)(void *)c->arguments.p)[i]; }

static uint32_t add_node(jchart *c, jnode node) {
    uint32_t i = (uint32_t)(c->nodes.n / sizeof node);
    jb_put(&c->nodes, &node, sizeof node);
    return i;
}

static void ws(jparser *p) {
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r'))
        p->i++;
}

static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }

static ta_string_view name(jparser *p) {
    ta_string_view v = {p->s + p->i, 0};
    if (p->i >= p->n || !is_alpha(p->s[p->i])) return v;
    while (p->i < p->n && (is_alpha(p->s[p->i]) || is_digit(p->s[p->i]) || p->s[p->i] == '-')) p->i++;
    v.length = (uint32_t)(p->s + p->i - v.pointer);
    return v;
}

static int fail(jparser *p, const char *why) {
    if (!p->error) p->error = why;
    return 0;
}

static void put_utf8(jbuf *b, uint32_t c) {
    if (c < 0x80)
        jb_byte(b, (uint8_t)c);
    else if (c < 0x800) {
        jb_byte(b, (uint8_t)(0xC0 | (c >> 6)));
        jb_byte(b, (uint8_t)(0x80 | (c & 63)));
    } else if (c < 0x10000) {
        jb_byte(b, (uint8_t)(0xE0 | (c >> 12)));
        jb_byte(b, (uint8_t)(0x80 | ((c >> 6) & 63)));
        jb_byte(b, (uint8_t)(0x80 | (c & 63)));
    } else {
        jb_byte(b, (uint8_t)(0xF0 | (c >> 18)));
        jb_byte(b, (uint8_t)(0x80 | ((c >> 12) & 63)));
        jb_byte(b, (uint8_t)(0x80 | ((c >> 6) & 63)));
        jb_byte(b, (uint8_t)(0x80 | (c & 63)));
    }
}

static int hex4(jparser *p, uint32_t *out) {
    if (p->i + 4 > p->n) return 0;
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        char c = p->s[p->i++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* A quoted string's UTF-8 into text. */
static int string_text(jparser *p, jbuf *text) {
    char quote = p->s[p->i++];
    while (p->i < p->n && p->s[p->i] != quote) {
        char c = p->s[p->i++];
        if ((unsigned char)c < 0x20) return fail(p, "a control character in a string");
        if (c != '\\') {
            jb_byte(text, (uint8_t)c);
            continue;
        }
        if (p->i >= p->n) return fail(p, "an unfinished escape");
        char e = p->s[p->i++];
        switch (e) {
        case '"': case '\'': case '\\': case '/': jb_byte(text, (uint8_t)e); break;
        case 'b': jb_byte(text, 8); break;
        case 'f': jb_byte(text, 12); break;
        case 'n': jb_byte(text, 10); break;
        case 'r': jb_byte(text, 13); break;
        case 't': jb_byte(text, 9); break;
        case 'u': {
            uint32_t c1, c2;
            if (!hex4(p, &c1)) return fail(p, "a bad \\u escape");
            if (c1 >= 0xD800 && c1 < 0xDC00) {
                if (p->i + 6 > p->n || p->s[p->i] != '\\' || p->s[p->i + 1] != 'u') return fail(p, "a lone surrogate");
                p->i += 2;
                if (!hex4(p, &c2) || c2 < 0xDC00 || c2 >= 0xE000) return fail(p, "a lone surrogate");
                c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
            } else if (c1 >= 0xDC00 && c1 < 0xE000)
                return fail(p, "a lone surrogate");
            put_utf8(text, c1);
            break;
        }
        default: return fail(p, "an unknown escape");
        }
    }
    if (p->i >= p->n) return fail(p, "an unterminated string");
    p->i++;
    return 1;
}

static void put_text(jbuf *b, const void *s, size_t n) {
    put_head(b, 3, n);
    jb_put(b, s, n);
}

static double pow10_exact(int e) {
    double r = 1;
    while (e-- > 0) r *= 10;
    return r;
}

/* A number into out as CBOR: integers exact; decimals on Clinger's fast path. */
static int number(jparser *p, jbuf *out) {
    int negative = 0;
    if (p->s[p->i] == '-') {
        negative = 1;
        p->i++;
    }
    if (p->i >= p->n || !is_digit(p->s[p->i])) return fail(p, "a malformed number");
    if (p->s[p->i] == '0' && p->i + 1 < p->n && is_digit(p->s[p->i + 1])) return fail(p, "a leading zero");
    uint64_t mantissa = 0;
    int digits = 0, overflow = 0, fraction = 0, exponent = 0, decimal = 0;
    while (p->i < p->n && is_digit(p->s[p->i])) {
        unsigned d = (unsigned)(p->s[p->i++] - '0');
        if (mantissa > (UINT64_MAX - d) / 10) overflow = 1;
        else mantissa = mantissa * 10 + d;
        digits++;
    }
    if (p->i < p->n && p->s[p->i] == '.') {
        decimal = 1;
        p->i++;
        if (p->i >= p->n || !is_digit(p->s[p->i])) return fail(p, "a malformed fraction");
        while (p->i < p->n && is_digit(p->s[p->i])) {
            unsigned d = (unsigned)(p->s[p->i++] - '0');
            if (mantissa > (UINT64_MAX - d) / 10) overflow = 1;
            else mantissa = mantissa * 10 + d;
            fraction++;
        }
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        decimal = 1;
        p->i++;
        int sign = 1;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) sign = p->s[p->i++] == '-' ? -1 : 1;
        if (p->i >= p->n || !is_digit(p->s[p->i])) return fail(p, "a malformed exponent");
        while (p->i < p->n && is_digit(p->s[p->i])) {
            if (exponent < 10000) exponent = exponent * 10 + (p->s[p->i] - '0');
            p->i++;
        }
        exponent *= sign;
    }
    (void)digits;
    if (!decimal) {
        if (overflow) return fail(p, "an integer past 64 bits");
        if (negative && mantissa == 0) {
            ta_cbor_writer w;
            uint8_t room[16];
            ta_cbor_writer_init(&w, (ta_buffer){room, sizeof room, 0}, 0);
            ta_cbor_write_float(&w, -0.0);
            ta_bytes b;
            ta_cbor_writer_bytes(&w, &b);
            jb_put(out, b.pointer, b.length);
            return 1;
        }
        put_head(out, negative ? 1 : 0, negative ? mantissa - 1 : mantissa);
        return 1;
    }
    if (overflow) return fail(p, "a decimal that cannot be read exactly");
    int e10 = exponent - fraction;
    double value;
    if (mantissa == 0)
        value = 0;
    else {
        if (mantissa > (1ull << 53)) return fail(p, "a decimal that cannot be read exactly");
        while (e10 > 22 && mantissa * 10 <= (1ull << 53)) {
            mantissa *= 10;
            e10--;
        }
        if (e10 > 22 || e10 < -22) return fail(p, "a decimal that cannot be read exactly");
        value = e10 >= 0 ? (double)mantissa * pow10_exact(e10) : (double)mantissa / pow10_exact(-e10);
    }
    if (negative) value = -value;
    if (value == 0 && negative) value = -0.0;
    /* an integral value in range is an integer */
    if (value == (double)(int64_t)value && !(value == 0 && negative) && value > -9.2e18 && value < 9.2e18) {
        int64_t i = (int64_t)value;
        put_head(out, i < 0 ? 1 : 0, i < 0 ? (uint64_t)(-1 - i) : (uint64_t)i);
        return 1;
    }
    ta_cbor_writer w;
    uint8_t room[16];
    ta_cbor_writer_init(&w, (ta_buffer){room, sizeof room, 0}, 0);
    ta_cbor_write_float(&w, value);
    ta_bytes b;
    ta_cbor_writer_bytes(&w, &b);
    jb_put(out, b.pointer, b.length);
    return 1;
}

/* A literal into out as CBOR. */
static int literal(jparser *p, jbuf *out) {
    if (++p->depth > 63) return fail(p, "a literal nested too deep");
    ws(p);
    if (p->i >= p->n) return fail(p, "a value expected");
    char c = p->s[p->i];
    int ok = 1;
    if (c == '"' || c == '\'') {
        jbuf text;
        jb_init(&text, &p->dm->memory);
        ok = string_text(p, &text);
        if (ok) put_text(out, text.p, text.n);
        jb_free(&text);
    } else if (c == '-' || is_digit(c))
        ok = number(p, out);
    else if (c == '[' || c == '{') {
        p->i++;
        jbuf items;
        jb_init(&items, &p->dm->memory);
        uint64_t count = 0;
        ws(p);
        char close = c == '[' ? ']' : '}';
        if (p->i < p->n && p->s[p->i] == close)
            p->i++;
        else
            for (;;) {
                if (c == '{') {
                    ws(p);
                    if (p->i >= p->n || (p->s[p->i] != '"' && p->s[p->i] != '\'')) {
                        ok = fail(p, "a map key must be a string");
                        break;
                    }
                    jbuf key;
                    jb_init(&key, &p->dm->memory);
                    ok = string_text(p, &key);
                    if (ok) put_text(&items, key.p, key.n);
                    jb_free(&key);
                    ws(p);
                    if (!ok || p->i >= p->n || p->s[p->i++] != ':') {
                        ok = fail(p, "':' expected");
                        break;
                    }
                }
                if (!(ok = literal(p, &items))) break;
                count++;
                ws(p);
                if (p->i < p->n && p->s[p->i] == ',') {
                    p->i++;
                    continue;
                }
                if (p->i < p->n && p->s[p->i] == close) {
                    p->i++;
                    break;
                }
                ok = fail(p, "',' or a closing bracket expected");
                break;
            }
        if (ok) {
            put_head(out, c == '[' ? 4 : 5, count);
            jb_put(out, items.p, items.n);
        }
        jb_free(&items);
    } else {
        ta_string_view w = name(p);
        if (view_eq(w, "true", 4)) jb_byte(out, 0xF5);
        else if (view_eq(w, "false", 5)) jb_byte(out, 0xF4);
        else if (view_eq(w, "null", 4)) jb_byte(out, 0xF6);
        else if (view_eq(w, "undefined", 9)) jb_byte(out, 0xF7);
        else ok = fail(p, "a literal expected");
    }
    p->depth--;
    return ok;
}

static uint32_t copy_string(jchart *c, ta_string_view v) {
    uint32_t at = (uint32_t)c->strings.n;
    jb_put(&c->strings, v.pointer, v.length);
    return at;
}

static uint32_t expression(jparser *p);

static const ta_string_view reserved[] = {{"true", 4}, {"false", 5}, {"null", 4}, {"undefined", 9}, {"In", 2}};

static int is_reserved(ta_string_view v) {
    for (unsigned i = 0; i < sizeof reserved / sizeof reserved[0]; i++)
        if (view_eq(v, reserved[i].pointer, reserved[i].length)) return 1;
    return v.length && v.pointer[0] == '_';
}

/* The function registered for the chart under name, else for every chart. */
static uint32_t find_function(ta_datamodel_json *dm, ta_string_view chart, ta_string_view fname) {
    uint32_t general = NIL;
    for (uint32_t i = 0; i < dm->function_count; i++) {
        jfunction *f = &dm->functions[i];
        if (!view_eq(fname, f->name.pointer, f->name.length)) continue;
        if (f->chart.length && view_eq(chart, f->chart.pointer, f->chart.length)) return i;
        if (!f->chart.length) general = i;
    }
    return general;
}

static uint32_t path_rest(jparser *p, jnode node) {
    jchart *c = p->chart;
    jsegment segs[64];
    uint32_t count = 0;
    for (;;) {
        ws(p);
        if (p->i >= p->n) break;
        char ch = p->s[p->i];
        if (ch == '.') {
            p->i++;
            ws(p);
            ta_string_view k = name(p);
            if (!k.length) return fail(p, "a name after '.'"), NIL;
            if (count == 64) return fail(p, "a path too long"), NIL;
            segs[count++] = (jsegment){S_KEY, copy_string(c, k), k.length};
        } else if (ch == '[') {
            p->i++;
            ws(p);
            if (count == 64) return fail(p, "a path too long"), NIL;
            if (p->i < p->n && (p->s[p->i] == '"' || p->s[p->i] == '\'')) {
                jbuf key;
                jb_init(&key, &p->dm->memory);
                int ok = string_text(p, &key);
                uint32_t at = (uint32_t)c->strings.n;
                if (ok) jb_put(&c->strings, key.p, key.n);
                segs[count++] = (jsegment){S_KEY, at, (uint32_t)key.n};
                jb_free(&key);
                if (!ok) return NIL;
            } else if (p->i < p->n && is_digit(p->s[p->i])) {
                if (p->s[p->i] == '0' && p->i + 1 < p->n && is_digit(p->s[p->i + 1])) return fail(p, "a leading zero"), NIL;
                uint64_t v = 0;
                while (p->i < p->n && is_digit(p->s[p->i])) {
                    v = v * 10 + (uint64_t)(p->s[p->i++] - '0');
                    if (v > 0xFFFFFFFFu) return fail(p, "a position past 2^32 - 1"), NIL;
                }
                segs[count++] = (jsegment){S_POSITION, (uint32_t)v, 0};
            } else {
                if (++p->depth > MAX_NEST) return fail(p, "a path nested too deep"), NIL;
                uint32_t inner = expression(p);
                p->depth--;
                if (inner == NIL) return NIL;
                if (node_at(c, inner)->kind != N_PATH) return fail(p, "an index is a position, a string or a path"), NIL;
                segs[count++] = (jsegment){S_PATH, inner, 0};
            }
            ws(p);
            if (p->i >= p->n || p->s[p->i++] != ']') return fail(p, "']' expected"), NIL;
        } else
            break;
    }
    node.count = (uint16_t)count;
    node.c = (uint32_t)(c->segments.n / sizeof(jsegment));
    jb_put(&c->segments, segs, count * sizeof(jsegment));
    return add_node(c, node);
}

static uint32_t expression(jparser *p) {
    jchart *c = p->chart;
    ws(p);
    if (p->i >= p->n) return fail(p, "an expression expected"), NIL;
    char ch = p->s[p->i];
    if (ch == '"' || ch == '\'' || ch == '-' || is_digit(ch) || ch == '[' || ch == '{') {
        uint32_t at = (uint32_t)c->literals.n;
        if (!literal(p, &c->literals)) return NIL;
        return add_node(c, (jnode){N_LITERAL, 0, 0, at, (uint32_t)c->literals.n - at, 0});
    }
    uint32_t save = p->i;
    ta_string_view w = name(p);
    if (!w.length) return fail(p, "an expression expected"), NIL;
    if (view_eq(w, "true", 4) || view_eq(w, "false", 5) || view_eq(w, "null", 4) || view_eq(w, "undefined", 9)) {
        p->i = save;
        uint32_t at = (uint32_t)c->literals.n;
        if (!literal(p, &c->literals)) return NIL;
        return add_node(c, (jnode){N_LITERAL, 0, 0, at, (uint32_t)c->literals.n - at, 0});
    }
    ws(p);
    int call = p->i < p->n && p->s[p->i] == '(';
    if (view_eq(w, "In", 2)) {
        if (!call) return fail(p, "In is reserved"), NIL;
        p->i++;
        ws(p);
        if (p->i >= p->n || (p->s[p->i] != '"' && p->s[p->i] != '\'')) return fail(p, "In takes a state's id"), NIL;
        jbuf id;
        jb_init(&id, &p->dm->memory);
        int ok = string_text(p, &id);
        uint32_t state = ok ? ta_image_state_by_name(c->image, c->chart, (ta_string_view){(const char *)id.p, (uint32_t)id.n}) : NIL;
        jb_free(&id);
        if (!ok) return NIL;
        ws(p);
        if (p->i >= p->n || p->s[p->i++] != ')') return fail(p, "')' expected"), NIL;
        if (state == NIL) return fail(p, "In() of a state the chart lacks"), NIL;
        return add_node(c, (jnode){N_IN, 0, 0, state, 0, 0});
    }
    if (call) {
        if (is_reserved(w)) return fail(p, "a reserved word called"), NIL;
        p->i++;
        if (++p->depth > MAX_NEST) return fail(p, "calls nested too deep"), NIL;
        uint32_t args[32], count = 0;
        ws(p);
        if (p->i < p->n && p->s[p->i] == ')')
            p->i++;
        else
            for (;;) {
                uint32_t a = expression(p);
                if (a == NIL) return NIL;
                if (count == 32) return fail(p, "too many arguments"), NIL;
                args[count++] = a;
                ws(p);
                if (p->i < p->n && p->s[p->i] == ',') {
                    p->i++;
                    continue;
                }
                if (p->i < p->n && p->s[p->i] == ')') {
                    p->i++;
                    break;
                }
                return fail(p, "',' or ')' expected"), NIL;
            }
        p->depth--;
        ta_string_view chart_name = ta_image_chart_name(c->image, c->chart);
        uint32_t f = find_function(p->dm, chart_name, w);
        if (f == NIL) return fail(p, "no function registered under that name"), NIL;
        uint32_t first = (uint32_t)(c->arguments.n / sizeof(uint32_t));
        jb_put(&c->arguments, args, count * sizeof(uint32_t));
        return add_node(c, (jnode){N_CALL, 0, (uint16_t)count, f, 0, first});
    }
    uint8_t root = R_VARIABLE;
    if (view_eq(w, "_event", 6)) root = R_EVENT;
    else if (view_eq(w, "_sessionid", 10)) root = R_SESSIONID;
    else if (view_eq(w, "_name", 5)) root = R_NAME;
    else if (view_eq(w, "_ioprocessors", 13)) root = R_IOPROCESSORS;
    else if (is_reserved(w)) return fail(p, "a reserved name"), NIL;
    return path_rest(p, (jnode){N_PATH, root, 0, copy_string(c, w), w.length, 0});
}

/* An expression's node, or an N_ERROR node with the parse error. */
static uint32_t parse_expression(ta_datamodel_json *dm, jchart *c, ta_expression_info *info) {
    jparser p = {dm, c, info->source.pointer, 0, info->source.length, 0, 0};
    uint32_t node;
    if (info->kind == TA_EXPRESSION_CONTENT) { /* content is a literal, or else text */
        uint32_t at = (uint32_t)c->literals.n;
        node = literal(&p, &c->literals) ? add_node(c, (jnode){N_LITERAL, 0, 0, at, (uint32_t)c->literals.n - at, 0}) : NIL;
        p.error = 0;
    } else
        node = expression(&p);
    if (node != NIL) {
        ws(&p);
        if (p.i != p.n) node = NIL, fail(&p, "text after the expression");
    }
    if (node != NIL && info->kind == TA_EXPRESSION_LOCATION && node_at(c, node)->kind != N_PATH)
        node = NIL, fail(&p, "a location must be a path");
    if (node == NIL && info->kind == TA_EXPRESSION_CONTENT) {
        /* content that is not a literal is its text, whitespace normalized */
        uint32_t at = (uint32_t)c->literals.n;
        jbuf text;
        jb_init(&text, &dm->memory);
        int space = 0;
        for (uint32_t i = 0; i < info->source.length; i++) {
            char ch = info->source.pointer[i];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                space = text.n > 0;
                continue;
            }
            if (space) jb_byte(&text, ' ');
            space = 0;
            jb_byte(&text, (uint8_t)ch);
        }
        if (text.n && text.p[0] == '<') {
            jb_free(&text);
            fail(&p, "XML content");
        } else {
            put_text(&c->literals, text.p, text.n);
            jb_free(&text);
            return add_node(c, (jnode){N_LITERAL, 0, 0, at, (uint32_t)c->literals.n - at, 0});
        }
    }
    if (node == NIL) {
        ta_string_view why = {p.error ? p.error : "a malformed expression", 0};
        while (why.pointer[why.length]) why.length++;
        return add_node(c, (jnode){N_ERROR, 0, 0, copy_string(c, why), why.length, 0});
    }
    return node;
}

/* ---- evaluation ------------------------------------------------------------------------ */

typedef struct jeval {
    ta_datamodel_json *dm;
    jsession *s;
    ta_datamodel_environment *environment;
    void *temporaries[MAX_NEST * 2 + 4];
    uint32_t temporary_count;
    uint64_t fuel;
} jeval;

static void eval_end(jeval *e) {
    for (uint32_t i = 0; i < e->temporary_count; i++) jfree(&e->dm->memory, e->temporaries[i]);
    e->temporary_count = 0;
    uint64_t fuel = e->fuel + e->s->pending_fuel;
    e->fuel = 0;
    if (e->environment) {
        e->s->pending_fuel = 0;
        if (fuel) ta_datamodel_environment_charge(e->environment, fuel > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)fuel);
    } else
        e->s->pending_fuel = fuel > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)fuel;
}

static int eval_error(jeval *e, const char *why, ta_string_view detail) {
    set_message(e->dm, why, detail);
    return 0;
}

static int eval_node(jeval *e, uint32_t index, jv *out);

static int step(jeval *e, const jsegment *seg, jv *value) {
    jchart *c = e->s->chart;
    if (seg->kind == S_KEY) {
        ta_string_view key = {(const char *)c->strings.p + seg->a, seg->b};
        if (!map_find(*value, key, value)) return eval_error(e, "no key ", key);
        return 1;
    }
    uint64_t position = seg->a;
    if (seg->kind == S_PATH) {
        jv index;
        if (!eval_node(e, seg->a, &index)) return 0;
        ta_string_view key;
        jhead h;
        if (is_text(index, &key)) {
            if (!map_find(*value, key, value)) return eval_error(e, "no key ", key);
            return 1;
        }
        if (!read_head(index, &h) || h.major != 0 || h.value > 0xFFFFFFFFu)
            return eval_error(e, "an index is a key or a position", none_view());
        position = h.value;
    }
    if (!array_at(*value, position, value)) return eval_error(e, "no item at that position", none_view());
    return 1;
}

static int path_root(jeval *e, const jnode *node, jv *out) {
    jsession *s = e->s;
    jchart *c = s->chart;
    jv system = {s->system, s->system_length};
    switch (node->root) {
    case R_VARIABLE: {
        ta_string_view key = {(const char *)c->strings.p + node->a, node->b};
        if (!map_find((jv){s->document, s->document_length}, key, out)) return eval_error(e, "no variable ", key);
        return 1;
    }
    case R_EVENT:
        if (!s->event) return eval_error(e, "_event is unbound", none_view());
        *out = (jv){s->event, s->event_length};
        return 1;
    case R_SESSIONID: return array_at(system, 0, out);
    case R_NAME: return array_at(system, 1, out);
    case R_IOPROCESSORS: return array_at(system, 2, out);
    }
    return 0;
}

static int call_function(jeval *e, const jnode *node, jv *out) {
    ta_datamodel_json *dm = e->dm;
    jchart *c = e->s->chart;
    jbuf args;
    jb_init(&args, &dm->memory);
    put_head(&args, 4, node->count);
    for (uint32_t i = 0; i < node->count; i++) {
        jv a;
        if (!eval_node(e, argument_at(c, node->c + i), &a)) {
            jb_free(&args);
            return 0;
        }
        jb_put(&args, a.p, a.n);
    }
    if (args.failed) return jb_free(&args), eval_error(e, "out of memory", none_view());
    jfunction *f = &dm->functions[node->a];
    ta_cbor_writer w;
    ta_cbor_writer_init(&w, (ta_buffer){dm->room, dm->max_document_bytes, 0}, 0);
    ta_json_call call = {sizeof call, node->count, f->context, {args.p, args.n}, &w, {0, 0}};
    ta_status status = f->function(&call);
    e->fuel += 1 + args.n / 64;
    jb_free(&args);
    ta_bytes result;
    if (status != TA_OK) {
        if (call.message.length) return eval_error(e, "", call.message);
        return eval_error(e, "the function failed: ", f->name);
    }
    if (ta_cbor_writer_bytes(&w, &result) != TA_OK || !result.length)
        return eval_error(e, "the function wrote no single value: ", f->name);
    uint8_t *copy = (uint8_t *)jalloc(&dm->memory, result.length);
    if (!copy || e->temporary_count == sizeof e->temporaries / sizeof e->temporaries[0])
        return jfree(&dm->memory, copy), eval_error(e, "out of memory", none_view());
    ta_memcpy(copy, result.pointer, result.length);
    e->temporaries[e->temporary_count++] = copy;
    *out = (jv){copy, (uint32_t)result.length};
    return 1;
}

static int eval_node(jeval *e, uint32_t index, jv *out) {
    jchart *c = e->s->chart;
    const jnode *node = node_at(c, index);
    switch (node->kind) {
    case N_ERROR: return eval_error(e, "", (ta_string_view){(const char *)c->strings.p + node->a, node->b});
    case N_LITERAL: *out = (jv){c->literals.p + node->a, node->b}; return 1;
    case N_IN: {
        if (!e->environment) return eval_error(e, "In() outside a step", none_view());
        static const uint8_t t = 0xF5, f = 0xF4;
        *out = (jv){ta_datamodel_environment_in(e->environment, node->a) ? &t : &f, 1};
        return 1;
    }
    case N_CALL: return call_function(e, node, out);
    case N_PATH: {
        jv v;
        if (!path_root(e, node, &v)) return 0;
        for (uint32_t i = 0; i < node->count; i++)
            if (!step(e, segment_at(c, node->c + i), &v)) return 0;
        e->fuel += v.n / 64;
        *out = v;
        return 1;
    }
    }
    return eval_error(e, "no such expression", none_view());
}

static int eval_expression(jeval *e, ta_expression x, jv *out) {
    jchart *c = e->s->chart;
    if (x >= c->expression_count) return eval_error(e, "no such expression", none_view());
    return eval_node(e, c->expression_node[x], out);
}

/* ---- writing a path ---------------------------------------------------------------------- */

/* The document with the value at path replaced, or added (add_root: a root
 * the document lacks is added, as a <foreach> declares its item). */
static int write_path(jeval *e, uint32_t path_node, jv value, int add_root) {
    jsession *s = e->s;
    jchart *c = s->chart;
    const jnode *node = node_at(c, path_node);
    if (node->kind == N_ERROR) return eval_node(e, path_node, &value);
    if (node->kind != N_PATH) return eval_error(e, "a location must be a path", none_view());
    if (node->root != R_VARIABLE) return eval_error(e, "only a variable can be written", none_view());
    jv doc = {s->document, s->document_length};
    /* the container whose child changes, and the child's range */
    jv container = doc;
    uint32_t start = 0, end = 0, found;
    ta_string_view key = {(const char *)c->strings.p + node->a, node->b};
    jsegment root_segment = {S_KEY, node->a, node->b};
    const jsegment *last = &root_segment;
    for (uint32_t i = 0; i <= node->count; i++) {
        const jsegment *seg = i == 0 ? &root_segment : segment_at(c, node->c + i - 1);
        if (i == node->count) {
            last = seg;
            break;
        }
        jv next = container;
        if (!step(e, seg, &next)) return 0;
        container = next;
    }
    /* find the last step's child in container */
    jhead h;
    if (!read_head(container, &h)) return eval_error(e, "a malformed document", none_view());
    uint64_t position = last->a;
    jsegment resolved;
    if (last->kind == S_PATH) {
        jv index;
        if (!eval_node(e, last->a, &index)) return 0;
        jhead ih;
        if (is_text(index, &key)) resolved = (jsegment){S_KEY, 0, 0};
        else if (read_head(index, &ih) && ih.major == 0) position = ih.value, resolved = (jsegment){S_POSITION, 0, 0};
        else return eval_error(e, "an index is a key or a position", none_view());
        last = &resolved;
    } else if (last->kind == S_KEY && last != &root_segment)
        key = (ta_string_view){(const char *)c->strings.p + last->a, last->b};
    uint32_t base = (uint32_t)(container.p - doc.p);
    uint32_t container_size = item_size(container);
    found = 0;
    if (last->kind == S_KEY) {
        if (h.major != 5) return eval_error(e, "a key of something not a map", none_view());
        uint32_t at = h.length;
        for (uint64_t i = 0; i < h.value; i++) {
            jv k = item((jv){container.p + at, container.n - at});
            jv v = item((jv){container.p + at + k.n, container.n - at - k.n});
            ta_string_view kt;
            if (is_text(k, &kt) && view_eq(key, kt.pointer, kt.length)) {
                start = base + at + k.n;
                end = start + v.n;
                found = 1;
                break;
            }
            at += k.n + v.n;
        }
        if (!found && last == &root_segment && !add_root) return eval_error(e, "no variable ", key);
    } else {
        if (h.major != 4) return eval_error(e, "a position in something not an array", none_view());
        if (position < h.value) {
            jv v;
            array_at(container, position, &v);
            start = (uint32_t)(v.p - doc.p);
            end = start + v.n;
            found = 1;
        } else if (position != h.value)
            return eval_error(e, "a position past the array's end", none_view());
    }
    jbuf out;
    jb_init(&out, &e->dm->memory);
    if (found) {
        jb_put(&out, doc.p, start);
        jb_put(&out, value.p, value.n);
        jb_put(&out, doc.p + end, doc.n - end);
    } else {
        uint32_t tail = base + container_size;
        jb_put(&out, doc.p, base);
        put_head(&out, h.major, h.value + 1);
        jb_put(&out, doc.p + base + h.length, tail - base - h.length);
        if (last->kind == S_KEY) put_text(&out, key.pointer, key.length);
        jb_put(&out, value.p, value.n);
        jb_put(&out, doc.p + tail, doc.n - tail);
    }
    if (out.failed || out.n > e->dm->max_document_bytes) {
        jb_free(&out);
        set_message(e->dm, "the document would pass its limit", none_view());
        e->dm->reason = TA_DATAMODEL_FAILURE_MEMORY;
        return 0;
    }
    e->fuel += out.n / 64;
    uint8_t *bytes = (uint8_t *)jalloc(&e->dm->memory, out.n);
    if (!bytes) return jb_free(&out), eval_error(e, "out of memory", none_view());
    ta_memcpy(bytes, out.p, out.n);
    jfree(&e->dm->memory, s->document);
    s->document = bytes;
    s->document_length = (uint32_t)out.n;
    jb_free(&out);
    return 1;
}

/* ---- canonical input ------------------------------------------------------------------- */

/* bytes, checked against the value table, written canonically into out. */
static ta_status canonical(ta_datamodel_json *dm, ta_bytes bytes, jbuf *out) {
    ta_cbor_reader r;
    ta_cbor_item it;
    ta_cbor_reader_init(&r, bytes);
    ta_cbor_writer w;
    ta_cbor_writer_init(&w, (ta_buffer){0, 0, 0}, &dm->memory.allocator);
    int32_t got;
    while ((got = ta_cbor_read(&r, &it)) == 1) {
        switch (it.kind) {
        case TA_CBOR_UINT: ta_cbor_write_uint(&w, it.value); break;
        case TA_CBOR_NEGINT: ta_cbor_write_negative(&w, it.value); break;
        case TA_CBOR_TEXT: ta_cbor_write_text(&w, (ta_string_view){(const char *)it.bytes.pointer, (uint32_t)it.bytes.length}); break;
        case TA_CBOR_BYTES: ta_cbor_write_bytes(&w, it.bytes); break;
        case TA_CBOR_ARRAY: ta_cbor_write_array(&w, it.count); break;
        case TA_CBOR_MAP: ta_cbor_write_map(&w, it.count); break;
        case TA_CBOR_TAG:
            if (it.value != TA_CBOR_TAG_URI) return ta_cbor_writer_free(&w), TA_ERROR_INVALID;
            ta_cbor_write_tag(&w, it.value);
            break;
        case TA_CBOR_SIMPLE:
            if (it.value < TA_CBOR_FALSE || it.value > TA_CBOR_UNDEFINED) return ta_cbor_writer_free(&w), TA_ERROR_INVALID;
            ta_cbor_write_simple(&w, (uint32_t)it.value);
            break;
        case TA_CBOR_FLOAT: {
            double d;
            ta_memcpy(&d, &it.value, sizeof d);
            uint64_t bits = it.value;
            int negative_zero = d == 0 && (bits >> 63);
            if (d == d && !negative_zero && d >= -9.2e18 && d <= 1.8e19 && d == (double)(int64_t)d && d < 9.2e18) {
                int64_t i = (int64_t)d;
                if (i < 0) ta_cbor_write_negative(&w, (uint64_t)(-1 - i));
                else ta_cbor_write_uint(&w, (uint64_t)i);
            } else
                ta_cbor_write_float(&w, d);
            break;
        }
        }
    }
    ta_bytes written;
    ta_status status = got < 0 ? (ta_status)got : ta_cbor_writer_bytes(&w, &written);
    if (status == TA_OK) jb_put(out, written.pointer, written.length);
    ta_cbor_writer_free(&w);
    if (status == TA_OK && out->failed) status = TA_ERROR_NO_MEMORY;
    return status;
}

/* ---- payloads -------------------------------------------------------------------------- */

static ta_payload payload_new(ta_datamodel_json *dm, const void *bytes, size_t length) {
    uint64_t handle;
    uint8_t *copy = (uint8_t *)jalloc(&dm->memory, length);
    if (!copy) return 0;
    jpayload *row = (jpayload *)ta_table_add(&dm->payloads, &handle);
    if (!row) return jfree(&dm->memory, copy), 0;
    ta_memcpy(copy, bytes, length);
    row->references = 1;
    row->length = (uint32_t)length;
    row->bytes = copy;
    return handle;
}

static void j_retain(void *d, ta_payload p) {
    jpayload *row = (jpayload *)ta_table_get(&((ta_datamodel_json *)d)->payloads, p);
    if (row) row->references++;
}

static void j_release(void *d, ta_payload p) {
    ta_datamodel_json *dm = (ta_datamodel_json *)d;
    jpayload *row = (jpayload *)ta_table_get(&dm->payloads, p);
    if (!row || --row->references) return;
    jfree(&dm->memory, row->bytes);
    ta_table_remove(&dm->payloads, p);
}

/* ---- the vtable ------------------------------------------------------------------------ */

static jsession *session_of(const ta_datamodel_call *call) {
    return (jsession *)ta_table_get(&((ta_datamodel_json *)call->datamodel)->sessions, call->instance);
}

static ta_status j_attach(void *d, const ta_datamodel_chart *chart, void **context) {
    ta_datamodel_json *dm = (ta_datamodel_json *)d;
    jchart *c = (jchart *)jalloc(&dm->memory, sizeof *c);
    if (!c) return TA_ERROR_NO_MEMORY;
    ta_memset(c, 0, sizeof *c);
    c->image = chart->image;
    c->chart = chart->chart;
    c->segment = chart->segment;
    c->next = dm->charts;
    dm->charts = c;
    jb_init(&c->nodes, &dm->memory);
    jb_init(&c->segments, &dm->memory);
    jb_init(&c->arguments, &dm->memory);
    jb_init(&c->strings, &dm->memory);
    jb_init(&c->literals, &dm->memory);
    c->expression_count = ta_image_expression_count(chart->image, chart->chart);
    c->expression_node = (uint32_t *)jalloc(&dm->memory, sizeof(uint32_t) * (c->expression_count + 1));
    if (!c->expression_node) return TA_ERROR_NO_MEMORY;
    for (uint32_t x = 0; x < c->expression_count; x++) {
        ta_expression_info info;
        if (ta_image_expression(chart->image, (ta_chart_item){chart->chart, x}, &info) != TA_OK) {
            c->expression_node[x] = NIL;
            continue;
        }
        if (info.kind == TA_EXPRESSION_SCRIPT || info.kind == TA_EXPRESSION_GLOBAL_SCRIPT) {
            ta_string_view why = {"the json datamodel has no <script>", 34};
            c->expression_node[x] = add_node(c, (jnode){N_ERROR, 0, 0, copy_string(c, why), why.length, 0});
            continue;
        }
        c->expression_node[x] = parse_expression(dm, c, &info);
    }
    c->data_count = ta_image_data_count(chart->image, chart->chart);
    c->data_node = (uint32_t *)jalloc(&dm->memory, sizeof(uint32_t) * (c->data_count + 1));
    if (!c->data_node) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < c->data_count; i++) {
        ta_data_info info;
        ta_image_data(chart->image, (ta_chart_item){chart->chart, i}, &info);
        c->data_node[i] = add_node(c, (jnode){N_PATH, R_VARIABLE, 0, copy_string(c, info.id), info.id.length, 0});
    }
    if (c->nodes.failed || c->segments.failed || c->arguments.failed || c->strings.failed || c->literals.failed)
        return TA_ERROR_NO_MEMORY;
    *context = c;
    return TA_OK;
}

static void j_detach(void *d, void *context) {
    ta_datamodel_json *dm = (ta_datamodel_json *)d;
    jchart *c = (jchart *)context;
    if (!c) return;
    for (jchart **at = &dm->charts; *at; at = &(*at)->next)
        if (*at == c) {
            *at = c->next;
            break;
        }
    jb_free(&c->nodes);
    jb_free(&c->segments);
    jb_free(&c->arguments);
    jb_free(&c->strings);
    jb_free(&c->literals);
    jfree(&dm->memory, c->expression_node);
    jfree(&dm->memory, c->data_node);
    jfree(&dm->memory, c);
}

static uint8_t *bytes_of(ta_datamodel_json *dm, jbuf *b, uint32_t *length) {
    uint8_t *copy = b->failed ? 0 : (uint8_t *)jalloc(&dm->memory, b->n);
    if (copy) ta_memcpy(copy, b->p, b->n);
    *length = (uint32_t)b->n;
    jb_free(b);
    return copy;
}

static int system_of(ta_datamodel_json *dm, jsession *s, const ta_datamodel_system_variables *v) {
    jbuf b;
    jb_init(&b, &dm->memory);
    put_head(&b, 4, 3);
    put_text(&b, v->sessionid.pointer, v->sessionid.length);
    put_text(&b, v->name.pointer, v->name.length);
    /* each processor under its type, and SCXML's two under their short names too */
    uint32_t entries = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass) put_head(&b, 5, entries);
        for (uint32_t i = 0; i < v->io_processor_count; i++) {
            ta_string_view type = v->io_processors[i].type, alias = {0, 0};
            if (type.length > 20 && !ta_memcmp(type.pointer + type.length - 20, "#SCXMLEventProcessor", 20)) alias = (ta_string_view){"scxml", 5};
            if (type.length > 24 && !ta_memcmp(type.pointer + type.length - 24, "#BasicHTTPEventProcessor", 24)) alias = (ta_string_view){"basichttp", 9};
            for (int k = 0; k < (alias.length ? 2 : 1); k++) {
                if (!pass) {
                    entries++;
                    continue;
                }
                ta_string_view key = k ? alias : type;
                put_text(&b, key.pointer, key.length);
                put_head(&b, 5, 1);
                put_text(&b, "location", 8);
                put_text(&b, v->io_processors[i].location.pointer, v->io_processors[i].location.length);
            }
        }
    }
    s->system = bytes_of(dm, &b, &s->system_length);
    return s->system != 0;
}

/* A document of every declared variable, each from `from` when it has it, else undefined. */
static int document_of(ta_datamodel_json *dm, jsession *s, jv from) {
    jchart *c = s->chart;
    jbuf b;
    jb_init(&b, &dm->memory);
    uint32_t n = ta_image_data_count(c->image, c->chart), count = 0;
    jbuf items;
    jb_init(&items, &dm->memory);
    for (uint32_t i = 0; i < n; i++) {
        ta_data_info info;
        ta_image_data(c->image, (ta_chart_item){c->chart, i}, &info);
        jv v;
        put_text(&items, info.id.pointer, info.id.length);
        if (from.n && map_find(from, info.id, &v)) jb_put(&items, v.p, v.n);
        else jb_byte(&items, 0xF7);
        count++;
    }
    /* a <foreach> item or index the chart declares that way, kept */
    for (uint32_t x = 0; x < c->expression_count && from.n; x++) {
        ta_expression_info info;
        if (ta_image_expression(c->image, (ta_chart_item){c->chart, x}, &info) != TA_OK || info.kind != TA_EXPRESSION_ITEM) continue;
        jv v, already;
        if (map_find((jv){items.p, (uint32_t)items.n}, info.source, &already)) continue;
        if (!map_find(from, info.source, &v)) continue;
        put_text(&items, info.source.pointer, info.source.length);
        jb_put(&items, v.p, v.n);
        count++;
    }
    put_head(&b, 5, count);
    jb_put(&b, items.p, items.n);
    jb_free(&items);
    jfree(&dm->memory, s->document);
    s->document = bytes_of(dm, &b, &s->document_length);
    return s->document != 0;
}

static ta_status session_new(ta_datamodel_json *dm, const ta_datamodel_start *start, uint64_t *out, jsession **made) {
    jsession *s = (jsession *)ta_table_add(&dm->sessions, out);
    if (!s) return TA_ERROR_NO_MEMORY;
    s->chart = (jchart *)start->chart_context;
    if (!system_of(dm, s, start->system_variables)) return TA_ERROR_NO_MEMORY;
    *made = s;
    return TA_OK;
}

static ta_status j_create(void *d, const ta_datamodel_start *start, uint64_t *out) {
    ta_datamodel_json *dm = (ta_datamodel_json *)d;
    jsession *s;
    ta_status status = session_new(dm, start, out, &s);
    if (status != TA_OK) return status;
    if (!document_of(dm, s, (jv){0, 0})) return TA_ERROR_NO_MEMORY;
    if (start->init) {
        jpayload *row = (jpayload *)ta_table_get(&dm->payloads, start->init);
        jhead h;
        if (row && read_head((jv){row->bytes, row->length}, &h) && h.major == 5) {
            s->init = (uint8_t *)jalloc(&dm->memory, row->length);
            if (s->init) ta_memcpy(s->init, row->bytes, row->length), s->init_length = row->length;
        }
        j_release(d, start->init);
    }
    return TA_OK;
}

static void j_destroy(const ta_datamodel_call *call) {
    ta_datamodel_json *dm = (ta_datamodel_json *)call->datamodel;
    jsession *s = session_of(call);
    if (!s) return;
    jfree(&dm->memory, s->document);
    jfree(&dm->memory, s->event);
    jfree(&dm->memory, s->system);
    jfree(&dm->memory, s->init);
    jfree(&dm->memory, s->kept);
    ta_table_remove(&dm->sessions, call->instance);
}

#define EVAL_BEGIN(call)                                                                                     \
    jsession *s = session_of(call);                                                                          \
    if (!s) return TA_ERROR_INVALID;                                                                          \
    jeval e = {(ta_datamodel_json *)(call)->datamodel, s, (call)->environment, {0}, 0, 0};                   \
    ((ta_datamodel_json *)(call)->datamodel)->reason = TA_DATAMODEL_FAILURE_EXCEPTION

static ta_status done(jeval *e, int ok) {
    eval_end(e);
    return ok ? TA_OK : TA_ERROR_EXECUTION;
}

static ta_status j_init_data(const ta_datamodel_call *call, uint32_t declaration) {
    EVAL_BEGIN(call);
    ta_data_info info;
    if (ta_image_data(s->chart->image, (ta_chart_item){s->chart->chart, declaration}, &info) != TA_OK) return done(&e, 0);
    jv value;
    if (declaration >= s->chart->data_count) return done(&e, 0);
    uint32_t root = s->chart->data_node[declaration];
    if (s->init && map_find((jv){s->init, s->init_length}, info.id, &value)) return done(&e, write_path(&e, root, value, 0));
    if (info.expression == TA_NIL) return done(&e, 1);
    if (!eval_expression(&e, info.expression, &value)) return done(&e, 0);
    return done(&e, write_path(&e, root, value, 0));
}

static ta_status j_bind_event(const ta_datamodel_call *call, const ta_event_view *view) {
    ta_datamodel_json *dm = (ta_datamodel_json *)call->datamodel;
    jsession *s = session_of(call);
    if (!s) return TA_ERROR_INVALID;
    static const char *const types[] = {"platform", "internal", "external"};
    jbuf b;
    jb_init(&b, &dm->memory);
    put_head(&b, 5, 8);
#define TEXT_FIELD(key, view_field)                                                                   \
    put_text(&b, key, sizeof key - 1);                                                                \
    put_text(&b, (view_field).pointer, (view_field).length)
    TEXT_FIELD("name", view->name);
    put_text(&b, "type", 4);
    put_text(&b, types[view->type % 3], view->type % 3 == 1 ? 8 : 8);
    TEXT_FIELD("sendid", view->sendid);
    TEXT_FIELD("origin", view->origin);
    TEXT_FIELD("origintype", view->origintype);
    TEXT_FIELD("invokeid", view->invokeid);
    TEXT_FIELD("raw", view->raw);
#undef TEXT_FIELD
    put_text(&b, "data", 4);
    jpayload *row = view->data ? (jpayload *)ta_table_get(&dm->payloads, view->data) : 0;
    if (row) jb_put(&b, row->bytes, row->length); /* spike: copied, not referenced */
    else jb_byte(&b, 0xF7);
    jfree(&dm->memory, s->event);
    s->event = bytes_of(dm, &b, &s->event_length); /* NULL on failure: unbound */
    jfree(&dm->memory, s->kept);                    /* the first event: no migrate asked */
    s->kept = 0;
    return s->event ? TA_OK : TA_ERROR_NO_MEMORY;
}

static ta_status j_condition(const ta_datamodel_call *call, ta_expression x, int *out) {
    EVAL_BEGIN(call);
    jv v;
    if (!eval_expression(&e, x, &v)) return done(&e, 0);
    if (simple_is(v, TA_CBOR_TRUE)) *out = 1;
    else if (simple_is(v, TA_CBOR_FALSE)) *out = 0;
    else return eval_error(&e, "a condition must be true or false", none_view()), done(&e, 0);
    return done(&e, 1);
}

static ta_status out_text(ta_datamodel_json *dm, ta_string_view text, ta_string_view *out) {
    dm->out.n = 0;
    jb_put(&dm->out, text.pointer, text.length);
    if (dm->out.failed) return TA_ERROR_NO_MEMORY;
    *out = (ta_string_view){(const char *)dm->out.p, text.length};
    return TA_OK;
}

static ta_status j_string(const ta_datamodel_call *call, ta_expression x, ta_string_view *out) {
    EVAL_BEGIN(call);
    jv v;
    ta_string_view text;
    if (!eval_expression(&e, x, &v)) return done(&e, 0);
    if (!is_text(v, &text)) return eval_error(&e, "text expected", none_view()), done(&e, 0);
    ta_status status = out_text(e.dm, text, out);
    eval_end(&e);
    return status;
}

static ta_status j_assign(const ta_datamodel_call *call, ta_expression location, ta_expression x) {
    EVAL_BEGIN(call);
    jv v;
    if (!eval_expression(&e, x, &v)) return done(&e, 0);
    if (location >= s->chart->expression_count) return done(&e, 0);
    return done(&e, write_path(&e, s->chart->expression_node[location], v, 0));
}

static ta_status j_assign_string(const ta_datamodel_call *call, ta_expression location, ta_string_view value) {
    EVAL_BEGIN(call);
    jbuf b;
    jb_init(&b, &e.dm->memory);
    put_text(&b, value.pointer, value.length);
    int ok = !b.failed && location < s->chart->expression_count &&
             write_path(&e, s->chart->expression_node[location], (jv){b.p, (uint32_t)b.n}, 0);
    jb_free(&b);
    return done(&e, ok);
}

static ta_status j_script(const ta_datamodel_call *call, ta_expression x) {
    EVAL_BEGIN(call);
    jv v;
    (void)eval_expression(&e, x, &v); /* an N_ERROR node: no scripts */
    return done(&e, 0);
}

static ta_status j_log(const ta_datamodel_call *call, ta_expression x, ta_string_view *out) {
    EVAL_BEGIN(call);
    jv v;
    ta_string_view text;
    if (!eval_expression(&e, x, &v)) return done(&e, 0);
    ta_status status;
    if (is_text(v, &text))
        status = out_text(e.dm, text, out);
    else {
        ta_datamodel_json *dm = e.dm;
        for (;;) {
            ta_buffer b = {dm->out.p, dm->out.cap, 0};
            status = ta_cbor_diagnostic((ta_bytes){v.p, v.n}, &b);
            if (status == TA_ERROR_NO_MEMORY && jb_reserve(&dm->out, b.length)) continue;
            if (status == TA_OK) *out = (ta_string_view){(const char *)dm->out.p, (uint32_t)b.length};
            break;
        }
    }
    eval_end(&e);
    return status;
}

static ta_status j_foreach_begin(const ta_datamodel_call *call, ta_expression array, ta_datamodel_loop *loop) {
    EVAL_BEGIN(call);
    jv v;
    jhead h;
    if (!eval_expression(&e, array, &v)) return done(&e, 0);
    if (!read_head(v, &h) || h.major != 4) return eval_error(&e, "<foreach> needs an array", none_view()), done(&e, 0);
    jloop *row = (jloop *)ta_table_add(&e.dm->loops, &loop->iter);
    uint8_t *copy = (uint8_t *)jalloc(&e.dm->memory, v.n); /* spike: copied */
    if (!row || !copy) return done(&e, 0);
    ta_memcpy(copy, v.p, v.n);
    row->bytes = copy;
    row->length = v.n;
    loop->n = (uint32_t)h.value;
    e.fuel += v.n / 64;
    return done(&e, 1);
}

static ta_status j_foreach_item(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    EVAL_BEGIN(call);
    jloop *row = (jloop *)ta_table_get(&e.dm->loops, loop->iter);
    jv v;
    if (!row || !array_at((jv){row->bytes, row->length}, loop->i, &v)) return done(&e, 0);
    if (loop->item >= s->chart->expression_count || !write_path(&e, s->chart->expression_node[loop->item], v, 1))
        return done(&e, 0);
    if (loop->index != TA_NIL) {
        uint8_t number[9];
        jbuf b;
        jb_init(&b, &e.dm->memory);
        put_head(&b, 0, loop->i);
        uint32_t n = (uint32_t)b.n;
        ta_memcpy(number, b.p, n);
        jb_free(&b);
        if (!write_path(&e, s->chart->expression_node[loop->index], (jv){number, n}, 1)) return done(&e, 0);
    }
    return done(&e, 1);
}

static void j_foreach_end(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_datamodel_json *dm = (ta_datamodel_json *)call->datamodel;
    jloop *row = (jloop *)ta_table_get(&dm->loops, loop->iter);
    if (!row) return;
    jfree(&dm->memory, row->bytes);
    ta_table_remove(&dm->loops, loop->iter);
}

static ta_string_view path_name(jchart *c, uint32_t node_index) {
    const jnode *node = node_at(c, node_index);
    if (node->kind != N_PATH) return none_view();
    if (!node->count) return (ta_string_view){(const char *)c->strings.p + node->a, node->b};
    const jsegment *last = segment_at(c, node->c + node->count - 1);
    if (last->kind != S_KEY) return none_view();
    return (ta_string_view){(const char *)c->strings.p + last->a, last->b};
}

static ta_status j_capture(const ta_datamodel_call *call, const ta_datamodel_capture *spec, ta_payload *out) {
    EVAL_BEGIN(call);
    jchart *c = s->chart;
    jv v;
    if (spec->content != TA_NIL) {
        if (!eval_expression(&e, spec->content, &v)) return done(&e, 0);
        *out = payload_new(e.dm, v.p, v.n);
        return done(&e, *out != 0);
    }
    /* names and values, a later name replacing an earlier one */
    uint32_t total = spec->namelist_count + spec->param_count;
    ta_string_view names[64];
    jv values[64];
    uint32_t count = 0;
    if (total > 64) return eval_error(&e, "too many names", none_view()), done(&e, 0);
    for (uint32_t i = 0; i < total; i++) {
        ta_string_view key;
        if (i < spec->namelist_count) {
            ta_expression x = spec->namelist[i];
            if (x >= c->expression_count || !eval_expression(&e, x, &v)) return done(&e, 0);
            key = path_name(c, c->expression_node[x]);
        } else {
            uint32_t k = i - spec->namelist_count;
            key = ta_image_string(c->image, spec->params[2 * k]);
            if (!eval_expression(&e, spec->params[2 * k + 1], &v)) return done(&e, 0);
        }
        uint32_t j = 0;
        while (j < count && !view_eq(names[j], key.pointer, key.length)) j++;
        names[j] = key;
        values[j] = v;
        if (j == count) count++;
    }
    jbuf b;
    jb_init(&b, &e.dm->memory);
    put_head(&b, 5, count);
    for (uint32_t i = 0; i < count; i++) {
        put_text(&b, names[i].pointer, names[i].length);
        jb_put(&b, values[i].p, values[i].n);
    }
    *out = b.failed ? 0 : payload_new(e.dm, b.p, b.n);
    jb_free(&b);
    return done(&e, *out != 0);
}

static ta_status j_custom(const ta_datamodel_call *call, uint32_t definition) {
    (void)call;
    (void)definition;
    return TA_ERROR_UNSUPPORTED;
}

static ta_status j_serialize(const ta_datamodel_call *call, ta_bytes *out) {
    ta_datamodel_json *dm = (ta_datamodel_json *)call->datamodel;
    jsession *s = session_of(call);
    if (!s) return TA_ERROR_INVALID;
    dm->out.n = 0;
    put_head(&dm->out, 5, 2);
    put_text(&dm->out, "vars", 4);
    put_head(&dm->out, 2, s->document_length);
    jb_put(&dm->out, s->document, s->document_length);
    put_text(&dm->out, "event", 5);
    if (s->event) {
        put_head(&dm->out, 2, s->event_length);
        jb_put(&dm->out, s->event, s->event_length);
    } else
        jb_byte(&dm->out, 0xF6);
    if (dm->out.failed) return TA_ERROR_NO_MEMORY;
    *out = (ta_bytes){dm->out.p, dm->out.n};
    return TA_OK;
}

static int blob_field(jv blob, const char *key, jv *out) {
    jv v;
    jhead h;
    if (!map_find(blob, (ta_string_view){key, (uint32_t)(key[0] == 'v' ? 4 : 5)}, &v) || !read_head(v, &h)) return 0;
    if (h.major != 2) return 0;
    *out = (jv){v.p + h.length, (uint32_t)h.value};
    return 1;
}

static ta_status j_deserialize(void *d, const ta_datamodel_start *start, uint64_t *out) {
    ta_datamodel_json *dm = (ta_datamodel_json *)d;
    jv blob = {(const uint8_t *)start->state.pointer, (uint32_t)start->state.length}, vars, event;
    if (!blob_field(blob, "vars", &vars)) return TA_ERROR_INVALID;
    jsession *s;
    ta_status status = session_new(dm, start, out, &s);
    if (status != TA_OK) return status;
    if (!document_of(dm, s, vars)) return TA_ERROR_NO_MEMORY;
    s->kept = (uint8_t *)jalloc(&dm->memory, vars.n);
    if (!s->kept) return TA_ERROR_NO_MEMORY;
    ta_memcpy(s->kept, vars.p, vars.n);
    s->kept_length = vars.n;
    if (blob_field(blob, "event", &event)) {
        s->event = (uint8_t *)jalloc(&dm->memory, event.n);
        if (!s->event) return TA_ERROR_NO_MEMORY;
        ta_memcpy(s->event, event.p, event.n);
        s->event_length = event.n;
    }
    return TA_OK;
}

static ta_status j_migrate(const ta_datamodel_call *call, ta_payload *bindings) {
    ta_datamodel_json *dm = (ta_datamodel_json *)call->datamodel;
    jsession *s = session_of(call);
    *bindings = 0;
    if (!s || !s->kept) return TA_OK;
    *bindings = payload_new(dm, s->kept, s->kept_length);
    jfree(&dm->memory, s->kept);
    s->kept = 0;
    return *bindings ? TA_OK : TA_ERROR_NO_MEMORY;
}

static ta_status j_to_wire(void *d, ta_payload p, ta_bytes *out) {
    jpayload *row = (jpayload *)ta_table_get(&((ta_datamodel_json *)d)->payloads, p);
    if (!row) return TA_ERROR_INVALID;
    *out = (ta_bytes){row->bytes, row->length};
    return TA_OK;
}

static ta_status j_from_wire(void *d, ta_bytes cbor, ta_payload *out) {
    ta_datamodel_json *dm = (ta_datamodel_json *)d;
    jbuf b;
    jb_init(&b, &dm->memory);
    ta_status status = canonical(dm, cbor, &b);
    if (status == TA_OK) {
        *out = payload_new(dm, b.p, b.n);
        if (!*out) status = TA_ERROR_NO_MEMORY;
    }
    jb_free(&b);
    return status;
}

static ta_status j_variable(const ta_datamodel_call *call, ta_string_view name_, ta_payload *out) {
    ta_datamodel_json *dm = (ta_datamodel_json *)call->datamodel;
    jsession *s = session_of(call);
    jv v;
    *out = 0;
    if (!s || !map_find((jv){s->document, s->document_length}, name_, &v)) return TA_ERROR_INVALID;
    if (simple_is(v, TA_CBOR_UNDEFINED)) return TA_OK;
    *out = payload_new(dm, v.p, v.n);
    return *out ? TA_OK : TA_ERROR_NO_MEMORY;
}

static ta_status j_failure(const ta_datamodel_call *call, ta_datamodel_failure *out) {
    ta_datamodel_json *dm = (ta_datamodel_json *)call->datamodel;
    out->reason = dm->reason;
    out->message = (ta_string_view){dm->message, dm->message_length};
    return TA_OK;
}

static const ta_datamodel_vtable json_vtable = {
    sizeof(ta_datamodel_vtable),
    TA_DATAMODEL_EVENT_DETACHABLE,
    "json",
    j_attach,
    j_detach,
    j_create,
    j_destroy,
    j_init_data,
    j_bind_event,
    0, /* detach_event: _event is the datamodel's already */
    j_condition,
    j_string,
    j_assign,
    j_assign_string,
    j_script,
    j_log,
    j_foreach_begin,
    j_foreach_item,
    j_foreach_end,
    j_capture,
    j_retain,
    j_release,
    j_custom,
    j_serialize,
    j_deserialize,
    j_migrate,
    j_to_wire,
    j_from_wire,
    j_to_wire,   /* payload_serialize */
    j_from_wire, /* payload_deserialize */
    j_variable,
    j_failure,
};

/* ---- snapshots ------------------------------------------------------------------------- */

static void put_bytes_or_null(jbuf *b, const uint8_t *p, uint32_t n) {
    if (!p) {
        jb_byte(b, 0xF6);
        return;
    }
    put_head(b, 2, n);
    jb_put(b, p, n);
}

static int put_namespace(jbuf *b, const ta_table *table) {
    ta_buffer probe = {0, 0, 0};
    ta_status status = ta_table_snapshot(table, &probe);
    if (status != TA_ERROR_NO_MEMORY && status != TA_OK) return 0;
    if (!jb_reserve(b, probe.length + 9)) return 0;
    put_head(b, 2, probe.length);
    if (!jb_reserve(b, probe.length)) return 0;
    ta_buffer room = {b->p + b->n, probe.length, 0};
    if (ta_table_snapshot(table, &room) != TA_OK) return 0;
    b->n += room.length;
    return 1;
}

ta_status ta_datamodel_json_snapshot(ta_datamodel_json *dm, ta_buffer *out) {
    jbuf b;
    jb_init(&b, &dm->memory);
    put_head(&b, 4, 6);
    if (!put_namespace(&b, &dm->sessions) || !put_namespace(&b, &dm->payloads) || !put_namespace(&b, &dm->loops))
        return jb_free(&b), TA_ERROR_NO_MEMORY;
    uint32_t count = 0;
    for (uint32_t slot = 0; slot < dm->sessions.capacity; slot++) count += ta_table_at(&dm->sessions, slot) != 0;
    put_head(&b, 4, count);
    for (uint32_t slot = 0; slot < dm->sessions.capacity; slot++) {
        jsession *x = (jsession *)ta_table_at(&dm->sessions, slot);
        if (!x) continue;
        put_head(&b, 4, 9);
        put_head(&b, 0, ta_table_handle(&dm->sessions, slot));
        put_head(&b, 0, x->chart->segment);
        put_head(&b, 0, x->chart->chart);
        put_bytes_or_null(&b, x->document, x->document_length);
        put_bytes_or_null(&b, x->event, x->event_length);
        put_bytes_or_null(&b, x->system, x->system_length);
        put_bytes_or_null(&b, x->init, x->init_length);
        put_bytes_or_null(&b, x->kept, x->kept_length);
        put_head(&b, 0, x->pending_fuel);
    }
    count = 0;
    for (uint32_t slot = 0; slot < dm->payloads.capacity; slot++) count += ta_table_at(&dm->payloads, slot) != 0;
    put_head(&b, 4, count);
    for (uint32_t slot = 0; slot < dm->payloads.capacity; slot++) {
        jpayload *x = (jpayload *)ta_table_at(&dm->payloads, slot);
        if (!x) continue;
        put_head(&b, 4, 3);
        put_head(&b, 0, ta_table_handle(&dm->payloads, slot));
        put_head(&b, 0, x->references);
        put_bytes_or_null(&b, x->bytes, x->length);
    }
    count = 0;
    for (uint32_t slot = 0; slot < dm->loops.capacity; slot++) count += ta_table_at(&dm->loops, slot) != 0;
    put_head(&b, 4, count);
    for (uint32_t slot = 0; slot < dm->loops.capacity; slot++) {
        jloop *x = (jloop *)ta_table_at(&dm->loops, slot);
        if (!x) continue;
        put_head(&b, 4, 2);
        put_head(&b, 0, ta_table_handle(&dm->loops, slot));
        put_bytes_or_null(&b, x->bytes, x->length);
    }
    if (b.failed) return jb_free(&b), TA_ERROR_NO_MEMORY;
    ta_status status = TA_OK;
    if (b.n > out->capacity) status = TA_ERROR_NO_MEMORY;
    else ta_memcpy(out->pointer, b.p, b.n);
    out->length = b.n;
    jb_free(&b);
    return status;
}

/* A reader over the snapshot's items. */
typedef struct jread {
    jv rest;
    int bad;
} jread;

static uint64_t read_number(jread *r) {
    jhead h;
    if (r->bad || !read_head(r->rest, &h) || h.major != 0) return r->bad = 1, 0;
    r->rest.p += h.length, r->rest.n -= h.length;
    return h.value;
}

static uint64_t read_open(jread *r, unsigned major) {
    jhead h;
    if (r->bad || !read_head(r->rest, &h) || h.major != major) return r->bad = 1, 0;
    r->rest.p += h.length, r->rest.n -= h.length;
    return h.value;
}

/* A byte string's copy, or NULL for null. */
static uint8_t *read_copy(ta_datamodel_json *dm, jread *r, uint32_t *length) {
    jhead h;
    *length = 0;
    if (r->bad || !read_head(r->rest, &h)) return r->bad = 1, (uint8_t *)0;
    if (h.major == 7) {
        r->rest.p += 1, r->rest.n -= 1;
        return 0;
    }
    if (h.major != 2 || h.length + h.value > r->rest.n) return r->bad = 1, (uint8_t *)0;
    uint8_t *copy = (uint8_t *)jalloc(&dm->memory, h.value);
    if (!copy) return r->bad = 1, (uint8_t *)0;
    ta_memcpy(copy, r->rest.p + h.length, h.value);
    *length = (uint32_t)h.value;
    r->rest.p += h.length + h.value, r->rest.n -= h.length + (uint32_t)h.value;
    return copy;
}

static ta_status read_namespace(ta_datamodel_json *dm, jread *r, ta_table *table) {
    uint32_t n;
    uint8_t *bytes = read_copy(dm, r, &n);
    if (!bytes) return TA_ERROR_INVALID;
    ta_status status = ta_table_restore(table, (ta_bytes){bytes, n});
    jfree(&dm->memory, bytes);
    return status;
}

ta_status ta_datamodel_json_restore(ta_datamodel_json *dm, ta_bytes state) {
    jread r = {{(const uint8_t *)state.pointer, (uint32_t)state.length}, 0};
    if (read_open(&r, 4) != 6) return TA_ERROR_INVALID;
    ta_status status;
    if ((status = read_namespace(dm, &r, &dm->sessions)) != TA_OK) return status;
    if ((status = read_namespace(dm, &r, &dm->payloads)) != TA_OK) return status;
    if ((status = read_namespace(dm, &r, &dm->loops)) != TA_OK) return status;
    uint64_t count = read_open(&r, 4);
    for (uint64_t i = 0; i < count && !r.bad; i++) {
        read_open(&r, 4);
        uint64_t handle = read_number(&r);
        uint32_t segment = (uint32_t)read_number(&r), chart = (uint32_t)read_number(&r);
        jsession *x = (jsession *)ta_table_get(&dm->sessions, handle);
        jchart *c = dm->charts;
        while (c && (c->segment != segment || c->chart != chart)) c = c->next;
        if (!x || !c) return TA_ERROR_INVALID;
        x->chart = c;
        x->document = read_copy(dm, &r, &x->document_length);
        x->event = read_copy(dm, &r, &x->event_length);
        x->system = read_copy(dm, &r, &x->system_length);
        x->init = read_copy(dm, &r, &x->init_length);
        x->kept = read_copy(dm, &r, &x->kept_length);
        x->pending_fuel = (uint32_t)read_number(&r);
    }
    count = read_open(&r, 4);
    for (uint64_t i = 0; i < count && !r.bad; i++) {
        read_open(&r, 4);
        jpayload *x = (jpayload *)ta_table_get(&dm->payloads, read_number(&r));
        if (!x) return TA_ERROR_INVALID;
        x->references = (uint32_t)read_number(&r);
        x->bytes = read_copy(dm, &r, &x->length);
    }
    count = read_open(&r, 4);
    for (uint64_t i = 0; i < count && !r.bad; i++) {
        read_open(&r, 4);
        jloop *x = (jloop *)ta_table_get(&dm->loops, read_number(&r));
        if (!x) return TA_ERROR_INVALID;
        x->bytes = read_copy(dm, &r, &x->length);
    }
    return r.bad ? TA_ERROR_INVALID : TA_OK;
}

/* ---- the public calls ------------------------------------------------------------------ */

void ta_datamodel_json_config_normalize(ta_datamodel_json_config *config) {
    if (!config->max_document_bytes) config->max_document_bytes = TA_DATAMODEL_JSON_MAX_DOCUMENT_BYTES_DEFAULT;
}

ta_status ta_datamodel_json_new(const ta_datamodel_json_config *config_, ta_datamodel_json **out) {
    ta_datamodel_json_config config = *config_;
    ta_datamodel_json_config_normalize(&config);
    jmem m = {config.allocator, 0};
    ta_datamodel_json *dm = (ta_datamodel_json *)jalloc(&m, sizeof *dm);
    if (!dm) return TA_ERROR_NO_MEMORY;
    ta_memset(dm, 0, sizeof *dm);
    dm->memory = m;
    dm->max_document_bytes = config.max_document_bytes;
    dm->room = (uint8_t *)jalloc(&dm->memory, config.max_document_bytes);
    if (!dm->room) return jfree(&dm->memory, dm), TA_ERROR_NO_MEMORY;
    ta_table_config sessions = {sizeof sessions, sizeof(jsession), &dm->memory.allocator};
    ta_table_config payloads = {sizeof payloads, sizeof(jpayload), &dm->memory.allocator};
    ta_table_config loops = {sizeof loops, sizeof(jloop), &dm->memory.allocator};
    ta_table_init(&dm->sessions, &sessions);
    ta_table_init(&dm->payloads, &payloads);
    ta_table_init(&dm->loops, &loops);
    jb_init(&dm->out, &dm->memory);
    *out = dm;
    return TA_OK;
}

static ta_string_view copy_view(jmem *m, ta_string_view v) {
    char *p = (char *)jalloc(m, v.length);
    if (p && v.length) ta_memcpy(p, v.pointer, v.length);
    return (ta_string_view){p, p ? v.length : 0};
}

ta_status ta_datamodel_json_register(ta_datamodel_json *dm, const ta_json_function_entry *entry) {
    if (!entry->name.length || is_reserved(entry->name) || !entry->function) return TA_ERROR_INVALID;
    for (uint32_t i = 0; i < dm->function_count; i++)
        if (view_eq(entry->name, dm->functions[i].name.pointer, dm->functions[i].name.length) &&
            view_eq(entry->chart, dm->functions[i].chart.pointer, dm->functions[i].chart.length))
            return TA_ERROR_INVALID;
    if (dm->function_count == dm->function_capacity) {
        uint32_t cap = dm->function_capacity ? dm->function_capacity * 2 : 16;
        jfunction *f = (jfunction *)jalloc(&dm->memory, sizeof(jfunction) * cap);
        if (!f) return TA_ERROR_NO_MEMORY;
        if (dm->function_count) ta_memcpy(f, dm->functions, sizeof(jfunction) * dm->function_count);
        jfree(&dm->memory, dm->functions);
        dm->functions = f;
        dm->function_capacity = cap;
    }
    jfunction *f = &dm->functions[dm->function_count++];
    f->name = copy_view(&dm->memory, entry->name);
    f->chart = entry->chart.length ? copy_view(&dm->memory, entry->chart) : none_view();
    f->function = entry->function;
    f->context = entry->context;
    return TA_OK;
}

ta_datamodel_binding ta_datamodel_json_binding(ta_datamodel_json *dm) {
    ta_datamodel_binding binding = {&json_vtable, dm};
    return binding;
}

size_t ta_datamodel_json_bytes(const ta_datamodel_json *dm) {
    /* the tables allocate through the raw allocator: rows and a slot's metadata */
    return dm->memory.bytes + (size_t)dm->sessions.capacity * (dm->sessions.row_size + 8) +
           (size_t)dm->payloads.capacity * (dm->payloads.row_size + 8) +
           (size_t)dm->loops.capacity * (dm->loops.row_size + 8);
}

void ta_datamodel_json_free(ta_datamodel_json *dm) {
    if (!dm) return;
    for (uint32_t slot = 0; slot < dm->sessions.capacity; slot++) {
        jsession *s = (jsession *)ta_table_at(&dm->sessions, slot);
        if (!s) continue;
        jfree(&dm->memory, s->document);
        jfree(&dm->memory, s->event);
        jfree(&dm->memory, s->system);
        jfree(&dm->memory, s->init);
        jfree(&dm->memory, s->kept);
    }
    for (uint32_t slot = 0; slot < dm->payloads.capacity; slot++) {
        jpayload *p = (jpayload *)ta_table_at(&dm->payloads, slot);
        if (p) jfree(&dm->memory, p->bytes);
    }
    for (uint32_t slot = 0; slot < dm->loops.capacity; slot++) {
        jloop *l = (jloop *)ta_table_at(&dm->loops, slot);
        if (l) jfree(&dm->memory, l->bytes);
    }
    ta_table_free(&dm->sessions);
    ta_table_free(&dm->payloads);
    ta_table_free(&dm->loops);
    for (uint32_t i = 0; i < dm->function_count; i++) {
        jfree(&dm->memory, (void *)dm->functions[i].name.pointer);
        if (dm->functions[i].chart.length) jfree(&dm->memory, (void *)dm->functions[i].chart.pointer);
    }
    jfree(&dm->memory, dm->functions);
    jb_free(&dm->out);
    jfree(&dm->memory, dm->room);
    jmem m = dm->memory;
    jfree(&m, dm);
}

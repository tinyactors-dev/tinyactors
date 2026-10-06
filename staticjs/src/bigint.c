/*
 * BigInts: integers of any size up to the context's max_bigint_bits, heap
 * values like strings, immutable once shared and reference counted. A sign
 * and a magnitude in 32-bit limbs, least significant first. Without a C
 * library and without 128-bit arithmetic (check-freestanding): a product is
 * 32 x 32 -> 64 bits, and a division divides 64 bits by 32.
 *
 * Everything a program can reach is allocated through the context, and every
 * operation charges fuel for its limb work, so the size limit bounds memory
 * and fuel bounds time.
 */
#include "internal.h"

/* ------------------------------------------------------------------------
 * Blocks
 */

/* Limbs of the largest BigInt the limit allows, rounded up. */
static uint32_t bigint_limit_limbs(const sjs_context *context) {
    return context->limits.max_bigint_bits / 32u + 1u;
}

/* A BigInt with room for `capacity` limbs, 0n until they are written; at
 * most two limbs past the limit, for a result whose length is known only
 * when it is computed. */
static sjs_result bigint_alloc(sjs_context *context, uint32_t capacity, sjs_bigint **out) {
    if (capacity > bigint_limit_limbs(context) + 2u) return SJS_ERR_LIMIT;
    size_t size = sizeof(sjs_bigint) + (size_t)capacity * sizeof(uint32_t);
    sjs_bigint *b = sjs_mem_alloc(&context->counting, size);
    if (b == NULL) return SJS_ERR_NOMEM;
    sjs_zero(b, sizeof *b);
    b->h.refs = 1;
    b->h.type = SJS_HEAP_BIGINT;
    b->h.size = (uint32_t)size;
    b->capacity = capacity;
    *out = b;
    return SJS_OK;
}

static void bigint_free(sjs_context *context, sjs_bigint *b) {
    sjs_mem_free(&context->counting, b, b->h.size);
}

static uint32_t limb_bits(uint32_t v) {
    return v == 0 ? 0 : 32u - (uint32_t)__builtin_clz(v);
}

static uint64_t bigint_bits(const sjs_bigint *b) {
    return b->length == 0 ? 0 : (uint64_t)(b->length - 1u) * 32u + limb_bits(b->limbs[b->length - 1u]);
}

/* Drop the zero limbs on top, give the value its sign (none for 0n), and
 * check the limit. On SJS_ERR_LIMIT the block is the caller's to free. */
static sjs_result bigint_finish(sjs_context *context, sjs_bigint *b, bool negative) {
    while (b->length > 0 && b->limbs[b->length - 1u] == 0) b->length--;
    b->h.flags = negative && b->length != 0 ? SJS_BIGINT_NEGATIVE : 0u;
    if (bigint_bits(b) > context->limits.max_bigint_bits) return SJS_ERR_LIMIT;
    return SJS_OK;
}

/* limbs = limbs * m + add, in place; the room for one more limb is the
 * caller's to have made. */
static void bigint_scale_add(sjs_bigint *b, uint32_t m, uint32_t add) {
    uint64_t carry = add;
    for (uint32_t i = 0; i < b->length; i++) {
        uint64_t t = (uint64_t)b->limbs[i] * m + carry;
        b->limbs[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry != 0) b->limbs[b->length++] = (uint32_t)carry;
}

/* limbs = limbs / divisor, in place; the remainder. */
static uint32_t bigint_divide_small(uint32_t *limbs, uint32_t length, uint32_t divisor) {
    uint64_t rest = 0;
    for (uint32_t i = length; i-- > 0;) {
        uint64_t current = rest << 32 | limbs[i];
        limbs[i] = (uint32_t)(current / divisor);
        rest = current % divisor;
    }
    return (uint32_t)rest;
}

/* ------------------------------------------------------------------------
 * Text
 */

static const char BIGINT_DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";

/* A digit's value in any radix up to 36; 36 for anything else. */
static uint32_t bigint_digit(char c) {
    if (c >= '0' && c <= '9') return (uint32_t)(c - '0');
    if (c >= 'a' && c <= 'z') return (uint32_t)(c - 'a') + 10u;
    if (c >= 'A' && c <= 'Z') return (uint32_t)(c - 'A') + 10u;
    return 36u;
}

/* log2 of a radix that is a power of two, or 0. */
static uint32_t radix_log2(uint32_t radix) {
    return (radix & (radix - 1u)) == 0 ? limb_bits(radix) - 1u : 0u;
}

/* Where the white space (and line terminators) from `at` end. */
static uint32_t bigint_skip_space(sjs_str text, uint32_t at) {
    while (at < text.length) {
        uint32_t next = at;
        if (!sjs_is_white_space((uint32_t)sjs_utf8_next(text, &next))) break;
        at = next;
    }
    return at;
}

bool sjs_bigint_parse(sjs_str text, sjs_bigint_digits *out) {
    uint32_t at = bigint_skip_space(text, 0);
    *out = (sjs_bigint_digits){ { text.ptr + at, 0 }, 10u, false };
    if (at == text.length) return true;
    if (text.ptr[at] == '+' || text.ptr[at] == '-') {
        out->negative = text.ptr[at] == '-';
        at++;
    } else if (text.ptr[at] == '0' && at + 1u < text.length) {
        char p = text.ptr[at + 1u];
        out->radix = p == 'x' || p == 'X' ? 16u : p == 'o' || p == 'O' ? 8u : p == 'b' || p == 'B' ? 2u : 10u;
        if (out->radix != 10u) at += 2u;
    }
    uint32_t start = at;
    while (at < text.length && bigint_digit(text.ptr[at]) < out->radix) at++;
    if (at == start) return false;
    out->digits = (sjs_str){ text.ptr + start, at - start };
    return bigint_skip_space(text, at) == text.length;
}

sjs_result sjs_bigint_from_digits(sjs_context *context, sjs_bigint_digits digits, sjs_value *out) {
    sjs_str s = digits.digits;
    uint32_t at = 0;
    while (at < s.length && s.ptr[at] == '0') at++;
    uint64_t count = s.length - at;
    uint32_t log2 = radix_log2(digits.radix);
    /* The value's bits lie between these bounds (3321/1000 < log2(10) <
     * 1701/512), which are at most 4 bits apart: past the limit by the
     * lower one is too large; the upper one sizes the block, at most a
     * limb past the limit. */
    uint64_t least = count == 0 ? 0 : log2 != 0 ? (count - 1u) * log2 + 1u : (count - 1u) * 3321u / 1000u + 1u;
    uint64_t most = log2 != 0 ? count * log2 : count * 1701u / 512u + 1u;
    if (least > context->limits.max_bigint_bits) return SJS_ERR_LIMIT;
    sjs_bigint *b;
    sjs_result r = bigint_alloc(context, (uint32_t)((most + 31u) / 32u), &b);
    if (r != SJS_OK) return r;
    sjs_zero(b->limbs, (size_t)b->capacity * sizeof(uint32_t));
    if (log2 != 0) {
        uint64_t bit = 0;
        for (uint32_t i = s.length; i > at; i--, bit += log2) {
            uint32_t v = bigint_digit(s.ptr[i - 1u]);
            uint32_t limb = (uint32_t)(bit / 32u);
            uint32_t shift = (uint32_t)(bit % 32u);
            b->limbs[limb] |= v << shift;
            if (shift + log2 > 32u && limb + 1u < b->capacity) b->limbs[limb + 1u] |= v >> (32u - shift);
        }
        b->length = b->capacity;
    } else {
        /* Nine digits at a time: quadratic, as V8's is for a long string. */
        uint32_t first = (uint32_t)(count % 9u == 0 ? 9u : count % 9u);
        while (at < s.length) {
            uint32_t chunk = 0;
            uint32_t scale = 1;
            for (uint32_t k = 0; k < first; k++) {
                chunk = chunk * 10u + (uint32_t)(s.ptr[at++] - '0');
                scale *= 10u;
            }
            bigint_scale_add(b, scale, chunk);
            first = 9u;
        }
    }
    r = bigint_finish(context, b, digits.negative);
    if (r != SJS_OK) {
        bigint_free(context, b);
        return r;
    }
    *out = sjs_heap_value(SJS_BIGINT, &b->h);
    return SJS_OK;
}

uint64_t sjs_bigint_text_cost(uint64_t limbs, uint32_t radix) {
    if (radix_log2(radix) != 0) return limbs / 16u;
    return limbs * limbs / 32u;
}

/* The digits of a power-of-two radix, from the top: linear. */
static void bigint_format_bits(const sjs_bigint *b, uint32_t log2, sjs_buf *out) {
    uint64_t bits = bigint_bits(b);
    uint64_t count = (bits + log2 - 1u) / log2;
    uint32_t mask = (1u << log2) - 1u;
    for (uint64_t d = count; d-- > 0;) {
        uint64_t bit = d * log2;
        uint32_t limb = (uint32_t)(bit / 32u);
        uint32_t shift = (uint32_t)(bit % 32u);
        uint32_t v = b->limbs[limb] >> shift;
        if (shift + log2 > 32u && limb + 1u < b->length) v |= b->limbs[limb + 1u] << (32u - shift);
        out->ptr[out->length++] = (uint8_t)BIGINT_DIGITS[v & mask];
    }
}

/* The digits of any other radix: repeated division of a copy by the
 * largest power of the radix a limb holds, a chunk of digits at a time from
 * the bottom; quadratic, which the limit bounds. */
typedef struct {
    uint32_t power;   /* radix^digits */
    uint32_t digits;
} radix_chunk;

static radix_chunk chunk_of(uint32_t radix) {
    radix_chunk c = { radix, 1u };
    while ((uint64_t)c.power * radix <= UINT32_MAX) {
        c.power *= radix;
        c.digits++;
    }
    return c;
}

static sjs_result bigint_format_chunks(const sjs_allocator *allocator, sjs_bigint_radix r, sjs_buf *out) {
    const sjs_bigint *b = r.value;
    radix_chunk c = chunk_of(r.radix);
    uint64_t chunks = bigint_bits(b) / (limb_bits(c.power) - 1u) + 1u;
    uint64_t size = ((uint64_t)b->length + chunks) * sizeof(uint32_t);
    if (size > UINT32_MAX) return SJS_ERR_NOMEM;
    uint32_t *scratch = sjs_mem_alloc(allocator, (size_t)size);
    if (scratch == NULL) return SJS_ERR_NOMEM;
    uint32_t *rest = scratch;
    uint32_t *found = scratch + b->length;
    sjs_copy(rest, b->limbs, (size_t)b->length * sizeof(uint32_t));
    uint32_t length = b->length;
    uint32_t n = 0;
    while (length > 0) {
        found[n++] = bigint_divide_small(rest, length, c.power);
        while (length > 0 && rest[length - 1u] == 0) length--;
    }
    /* The top chunk without its leading zeros, every other one whole. */
    uint32_t v = found[n - 1u];
    char top[32];
    uint32_t t = 0;
    while (v != 0) {
        top[t++] = BIGINT_DIGITS[v % r.radix];
        v /= r.radix;
    }
    while (t > 0) out->ptr[out->length++] = (uint8_t)top[--t];
    for (uint32_t i = n - 1u; i-- > 0;) {
        v = found[i];
        for (uint32_t k = c.digits; k-- > 0;) {
            out->ptr[out->length + k] = (uint8_t)BIGINT_DIGITS[v % r.radix];
            v /= r.radix;
        }
        out->length += c.digits;
    }
    sjs_mem_free(allocator, scratch, (size_t)size);
    return SJS_OK;
}

sjs_result sjs_bigint_format(const sjs_allocator *allocator, sjs_bigint_radix r, sjs_buf *out) {
    const sjs_bigint *b = r.value;
    if (b->length == 0) return sjs_buf_append(allocator, out, (sjs_bytes){ (const uint8_t *)"0", 1 });
    uint32_t log2 = radix_log2(r.radix);
    uint64_t bits = bigint_bits(b);
    radix_chunk c = chunk_of(r.radix);
    uint64_t room = 1u + (log2 != 0 ? bits / log2 + 1u : (bits / (limb_bits(c.power) - 1u) + 1u) * c.digits);
    if (room > UINT32_MAX) return SJS_ERR_NOMEM;
    sjs_result result = sjs_buf_reserve(allocator, out, (uint32_t)room);
    if (result != SJS_OK) return result;
    if (sjs_bigint_negative(b)) out->ptr[out->length++] = '-';
    if (log2 != 0) {
        bigint_format_bits(b, log2, out);
        return SJS_OK;
    }
    uint32_t mark = out->length;
    result = bigint_format_chunks(allocator, r, out);
    if (result != SJS_OK) out->length = mark - (sjs_bigint_negative(b) ? 1u : 0u);
    return result;
}

sjs_result sjs_bigint_to_string(sjs_context *context, sjs_bigint_radix r, sjs_string **out) {
    sjs_buf text = { 0 };
    sjs_result result = sjs_bigint_format(&context->counting, r, &text);
    if (result == SJS_OK) result = sjs_string_make(context, (sjs_str){ (const char *)text.ptr, text.length }, out);
    sjs_buf_free(&context->counting, &text);
    return result;
}

/* ------------------------------------------------------------------------
 * Equality
 */

bool sjs_bigint_equal(const sjs_bigint *a, const sjs_bigint *b) {
    if (a->length != b->length || a->h.flags != b->h.flags) return false;
    for (uint32_t i = 0; i < a->length; i++) {
        if (a->limbs[i] != b->limbs[i]) return false;
    }
    return true;
}

uint32_t sjs_bigint_hash(const sjs_bigint *b) {
    uint32_t h = sjs_bigint_negative(b) ? 0x9e3779b9u : 2166136261u;
    for (uint32_t i = 0; i < b->length; i++) h = (h ^ b->limbs[i]) * 16777619u;
    return h;
}

/* ------------------------------------------------------------------------
 * Magnitudes: limbs, least significant first, the top one not 0
 */

typedef struct {
    const uint32_t *limbs;
    uint32_t length;
} magnitude;

static magnitude magnitude_of(const sjs_bigint *b) {
    return (magnitude){ b->limbs, b->length };
}

static int magnitude_compare(magnitude a, magnitude b) {
    if (a.length != b.length) return a.length < b.length ? -1 : 1;
    for (uint32_t i = a.length; i-- > 0;) {
        if (a.limbs[i] != b.limbs[i]) return a.limbs[i] < b.limbs[i] ? -1 : 1;
    }
    return 0;
}

/* out = a + b; room for the longer's length and one more. out may be
 * either operand's limbs: each limb is read before it is written. */
static uint32_t magnitude_add(uint32_t *out, magnitude a, magnitude b) {
    if (a.length < b.length) {
        magnitude t = a;
        a = b;
        b = t;
    }
    uint64_t carry = 0;
    for (uint32_t i = 0; i < a.length; i++) {
        carry += (uint64_t)a.limbs[i] + (i < b.length ? b.limbs[i] : 0u);
        out[i] = (uint32_t)carry;
        carry >>= 32;
    }
    out[a.length] = (uint32_t)carry;
    return a.length + 1u;
}

/* out = a - b, a at least b; room for a's length. out may be either
 * operand's limbs. */
static uint32_t magnitude_subtract(uint32_t *out, magnitude a, magnitude b) {
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < a.length; i++) {
        uint64_t t = (uint64_t)a.limbs[i] - (i < b.length ? b.limbs[i] : 0u) - borrow;
        out[i] = (uint32_t)t;
        borrow = t >> 63;
    }
    return a.length;
}

/* out = a * b, schoolbook; room for both lengths, and none of either
 * operand's limbs. The products it makes. */
static uint64_t magnitude_multiply(uint32_t *out, magnitude a, magnitude b) {
    sjs_zero(out, ((size_t)a.length + b.length) * sizeof(uint32_t));
    for (uint32_t i = 0; i < a.length; i++) {
        uint64_t carry = 0;
        uint64_t x = a.limbs[i];
        if (x == 0) continue;
        for (uint32_t j = 0; j < b.length; j++) {
            uint64_t t = x * b.limbs[j] + out[i + j] + carry;
            out[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        out[i + b.length] = (uint32_t)carry;
    }
    return (uint64_t)a.length * b.length;
}

/* Knuth's algorithm D (TAOCP 4.3.1), as in Hacker's Delight's divmnu:
 * u / v with v of two limbs or more and u at least as long; q gets
 * u.length - v.length + 1 limbs, r (unless NULL) v.length; the scratch
 * holds u.length + 1 + v.length limbs. */
typedef struct {
    magnitude u;
    magnitude v;
    uint32_t *q;
    uint32_t *r;
    uint32_t *scratch;
} division;

static void magnitude_divide(const division *d) {
    uint32_t n = d->v.length;
    uint32_t m = d->u.length - n;
    const uint32_t *u = d->u.limbs;
    const uint32_t *v = d->v.limbs;
    uint32_t *un = d->scratch;
    uint32_t *vn = d->scratch + d->u.length + 1u;
    /* Normalize: shift both so that v's top bit is set. */
    uint32_t s = (uint32_t)__builtin_clz(v[n - 1u]);
    for (uint32_t i = n - 1u; i > 0; i--) vn[i] = v[i] << s | (s != 0 ? v[i - 1u] >> (32u - s) : 0u);
    vn[0] = v[0] << s;
    un[m + n] = s != 0 ? u[m + n - 1u] >> (32u - s) : 0u;
    for (uint32_t i = m + n - 1u; i > 0; i--) un[i] = u[i] << s | (s != 0 ? u[i - 1u] >> (32u - s) : 0u);
    un[0] = u[0] << s;
    for (uint32_t j = m + 1u; j-- > 0;) {
        /* Estimate the quotient limb from the top two limbs, and correct
         * it with the third: then it is at most one too large. */
        uint64_t top = (uint64_t)un[j + n] << 32 | un[j + n - 1u];
        uint64_t qhat = top / vn[n - 1u];
        uint64_t rhat = top % vn[n - 1u];
        while (qhat > UINT32_MAX || qhat * vn[n - 2u] > (rhat << 32 | un[j + n - 2u])) {
            qhat--;
            rhat += vn[n - 1u];
            if (rhat > UINT32_MAX) break;
        }
        /* Multiply and subtract. */
        uint64_t borrow = 0;
        uint64_t carry = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t p = qhat * vn[i] + carry;
            carry = p >> 32;
            uint64_t t = (uint64_t)un[i + j] - (uint32_t)p - borrow;
            un[i + j] = (uint32_t)t;
            borrow = t >> 63;
        }
        uint64_t t = (uint64_t)un[j + n] - carry - borrow;
        un[j + n] = (uint32_t)t;
        if ((t >> 63) != 0) {
            /* One too large: add v back. */
            qhat--;
            uint64_t c = 0;
            for (uint32_t i = 0; i < n; i++) {
                c += (uint64_t)un[i + j] + vn[i];
                un[i + j] = (uint32_t)c;
                c >>= 32;
            }
            un[j + n] += (uint32_t)c;
        }
        d->q[j] = (uint32_t)qhat;
    }
    if (d->r == NULL) return;
    for (uint32_t i = 0; i < n; i++) d->r[i] = un[i] >> s | (s != 0 ? un[i + 1u] << (32u - s) : 0u);
}

/* ------------------------------------------------------------------------
 * Arithmetic
 */

/* The result is an operand, shared. */
static sjs_result bigint_share(sjs_bigint_op *op, sjs_value v) {
    sjs_ref(v);
    op->result = v;
    return SJS_OK;
}

/* A block of `capacity` limbs for a result. */
typedef struct {
    sjs_bigint *block;
    bool negative;
} bigint_made;

static sjs_result bigint_give(sjs_context *context, sjs_bigint_op *op, bigint_made made) {
    sjs_result r = bigint_finish(context, made.block, made.negative);
    if (r != SJS_OK) {
        /* An operand taken over stays its caller's to release. */
        if (op->took == 0) bigint_free(context, made.block);
        return r;
    }
    op->result = sjs_heap_value(SJS_BIGINT, &made.block->h);
    return SJS_OK;
}

/* A small result: a magnitude of up to 64 bits. */
static sjs_result bigint_small(sjs_context *context, sjs_bigint_op *op, uint64_t value) {
    bool negative = op->negative;
    sjs_bigint *b;
    sjs_result r = bigint_alloc(context, 2u, &b);
    if (r != SJS_OK) return r;
    b->limbs[0] = (uint32_t)value;
    b->limbs[1] = (uint32_t)(value >> 32);
    b->length = 2u;
    return bigint_give(context, op, (bigint_made){ b, negative });
}

/* BIGINT_REUSE: an operand the caller holds alone (op->reuse) with room
 * for `need` limbs becomes the result, written in place, as a string
 * nothing else holds is extended (README.md, "A value nothing else holds is
 * updated in place"): no program can tell, as nothing else sees it. Else a
 * new block of `need` limbs and `slack` more, for a result that will grow. */
typedef struct {
    uint32_t need;
    uint32_t slack;
    sjs_bigint **out;
} bigint_room;

static sjs_result bigint_target(sjs_context *context, sjs_bigint_op *op, bigint_room room) {
    sjs_bigint **out = room.out;
    if ((op->reuse & SJS_BIGINT_REUSE_A) != 0 && SJS_AS_BIGINT(op->a)->capacity >= room.need) {
        op->took = SJS_BIGINT_REUSE_A;
        *out = SJS_AS_BIGINT(op->a);
        return SJS_OK;
    }
    if ((op->reuse & SJS_BIGINT_REUSE_B) != 0 && SJS_AS_BIGINT(op->b)->capacity >= room.need) {
        op->took = SJS_BIGINT_REUSE_B;
        *out = SJS_AS_BIGINT(op->b);
        return SJS_OK;
    }
    uint32_t limit = bigint_limit_limbs(context) + 2u;
    uint32_t capacity = room.need;
    if (room.slack <= limit && capacity <= limit - room.slack) capacity += room.slack;
    return bigint_alloc(context, capacity, out);
}

/* a + b, or a - b: signs decide whether magnitudes add or subtract. Each
 * limb is read before it is written, so the result may be either operand. */
static sjs_result bigint_add(sjs_context *context, sjs_bigint_op *op) {
    const sjs_bigint *a = SJS_AS_BIGINT(op->a);
    const sjs_bigint *b = SJS_AS_BIGINT(op->b);
    bool subtract = op->op == SJS_OP_SUBTRACT;
    if (b->length == 0) return bigint_share(op, op->a);
    if (a->length == 0 && !subtract) return bigint_share(op, op->b);
    bool an = sjs_bigint_negative(a);
    bool bn = sjs_bigint_negative(b) != subtract;
    magnitude x = magnitude_of(a);
    magnitude y = magnitude_of(b);
    uint32_t longer = x.length > y.length ? x.length : y.length;
    sjs_bigint *out;
    sjs_result r = bigint_target(context, op, (bigint_room){ longer + 1u, 0, &out });
    if (r != SJS_OK) return r;
    bool negative = an;
    if (an == bn) {
        out->length = magnitude_add(out->limbs, x, y);
    } else if (magnitude_compare(x, y) >= 0) {
        out->length = magnitude_subtract(out->limbs, x, y);
    } else {
        out->length = magnitude_subtract(out->limbs, y, x);
        negative = bn;
    }
    op->cost = longer / 16u;
    return bigint_give(context, op, (bigint_made){ out, negative });
}

static sjs_result bigint_copy(sjs_context *context, sjs_bigint_op *op, const sjs_bigint *from) {
    sjs_bigint *out;
    sjs_result r = bigint_alloc(context, from->length, &out);
    if (r != SJS_OK) return r;
    sjs_copy(out->limbs, from->limbs, (size_t)from->length * sizeof(uint32_t));
    out->length = from->length;
    op->cost = from->length / 16u;
    return bigint_give(context, op, (bigint_made){ out, op->negative });
}

static bool is_one(const sjs_bigint *b) {
    return b->length == 1u && b->limbs[0] == 1u;
}

static sjs_result bigint_multiply(sjs_context *context, sjs_bigint_op *op) {
    const sjs_bigint *a = SJS_AS_BIGINT(op->a);
    const sjs_bigint *b = SJS_AS_BIGINT(op->b);
    op->negative = sjs_bigint_negative(a) != sjs_bigint_negative(b);
    if (a->length == 0) return bigint_share(op, op->a);
    if (b->length == 0) return bigint_share(op, op->b);
    if (is_one(b)) return sjs_bigint_negative(b) ? bigint_copy(context, op, a) : bigint_share(op, op->a);
    if (is_one(a)) return sjs_bigint_negative(a) ? bigint_copy(context, op, b) : bigint_share(op, op->b);
    if (b->length == 1u && (op->reuse & SJS_BIGINT_REUSE_A) != 0 && a->capacity > a->length) {
        /* BIGINT_REUSE: times one limb, in place. */
        op->took = SJS_BIGINT_REUSE_A;
        sjs_bigint *out = SJS_AS_BIGINT(op->a);
        op->cost = out->length / 16u;
        bigint_scale_add(out, b->limbs[0], 0);
        return bigint_give(context, op, (bigint_made){ out, op->negative });
    }
    if (a->length == 1u && b->length == 1u) return bigint_small(context, op, (uint64_t)a->limbs[0] * b->limbs[0]);
    /* The product has the operands' bits together, or one fewer. */
    if (bigint_bits(a) + bigint_bits(b) - 1u > context->limits.max_bigint_bits) return SJS_ERR_LIMIT;
    /* Schoolbook cannot write over an operand. A product of an operand
     * nothing else holds, a reduce's accumulator, often grows on: room for
     * a quarter more, which a multiplication by one limb fills in place. */
    sjs_bigint *out;
    uint32_t need = a->length + b->length;
    sjs_bigint_op fresh = { 0 };
    sjs_result r = bigint_target(context, &fresh, (bigint_room){ need, op->reuse != 0 ? need / 8u + 1u : 0u, &out });
    if (r != SJS_OK) return r;
    op->cost = magnitude_multiply(out->limbs, magnitude_of(a), magnitude_of(b)) / 16u;
    out->length = a->length + b->length;
    return bigint_give(context, op, (bigint_made){ out, op->negative });
}

/* a / b, truncated toward zero, or a % b, with a's sign. */
static sjs_result bigint_divide(sjs_context *context, sjs_bigint_op *op) {
    const sjs_bigint *a = SJS_AS_BIGINT(op->a);
    const sjs_bigint *b = SJS_AS_BIGINT(op->b);
    bool quotient = op->op == SJS_OP_DIVIDE;
    if (b->length == 0) {
        op->fault = "JavaScript throws a RangeError when a BigInt is divided by 0n";
        return SJS_ERR_PROGRAM;
    }
    op->negative = quotient ? sjs_bigint_negative(a) != sjs_bigint_negative(b) : sjs_bigint_negative(a);
    if (magnitude_compare(magnitude_of(a), magnitude_of(b)) < 0) {
        if (!quotient) return bigint_share(op, op->a);
        return bigint_small(context, op, 0);
    }
    if (quotient && is_one(b)) return sjs_bigint_negative(b) ? bigint_copy(context, op, a) : bigint_share(op, op->a);
    if (a->length <= 2u) {
        uint64_t x = a->limbs[0] | (a->length > 1u ? (uint64_t)a->limbs[1] << 32 : 0u);
        uint64_t y = b->limbs[0] | (b->length > 1u ? (uint64_t)b->limbs[1] << 32 : 0u);
        return bigint_small(context, op, quotient ? x / y : x % y);
    }
    uint32_t n = b->length;
    uint32_t length = n == 1u ? a->length : quotient ? a->length - n + 1u : n;
    sjs_bigint *out;
    sjs_result r = bigint_alloc(context, length, &out);
    if (r != SJS_OK) return r;
    out->length = length;
    if (n == 1u) {
        sjs_copy(out->limbs, a->limbs, (size_t)a->length * sizeof(uint32_t));
        out->length = a->length;
        uint32_t rest = bigint_divide_small(out->limbs, a->length, b->limbs[0]);
        if (!quotient) {
            out->limbs[0] = rest;
            out->length = 1u;
        }
        op->cost = a->length / 16u;
        return bigint_give(context, op, (bigint_made){ out, op->negative });
    }
    /* Scratch for the normalized operands, and for the quotient when the
     * result is the remainder. */
    size_t limbs = (size_t)a->length + 1u + n + (quotient ? 0u : a->length - n + 1u);
    uint32_t *scratch = sjs_mem_alloc(&context->counting, limbs * sizeof(uint32_t));
    if (scratch == NULL) {
        bigint_free(context, out);
        return SJS_ERR_NOMEM;
    }
    division d = { magnitude_of(a), magnitude_of(b), quotient ? out->limbs : scratch + a->length + 1u + n,
                   quotient ? NULL : out->limbs, scratch };
    magnitude_divide(&d);
    sjs_mem_free(&context->counting, scratch, limbs * sizeof(uint32_t));
    op->cost = (uint64_t)(a->length - n + 1u) * n / 16u;
    return bigint_give(context, op, (bigint_made){ out, op->negative });
}

/* a ** b, by squaring and multiplying, from the exponent's top bit; a
 * power of two by a shift. */
static sjs_result bigint_power(sjs_context *context, sjs_bigint_op *op) {
    const sjs_bigint *a = SJS_AS_BIGINT(op->a);
    const sjs_bigint *b = SJS_AS_BIGINT(op->b);
    if (sjs_bigint_negative(b)) {
        op->fault = "JavaScript throws a RangeError for a BigInt to a negative power";
        return SJS_ERR_PROGRAM;
    }
    op->negative = false;
    if (b->length == 0) return bigint_small(context, op, 1u);
    bool odd = (b->limbs[0] & 1u) != 0;
    op->negative = sjs_bigint_negative(a) && odd;
    if (a->length == 0) return bigint_share(op, op->a);
    if (is_one(a)) return sjs_bigint_negative(a) && !odd ? bigint_small(context, op, 1u) : bigint_share(op, op->a);
    if (b->length > 1u) return SJS_ERR_LIMIT; /* 2^(2^32) has more bits than any limit */
    uint32_t e = b->limbs[0];
    uint64_t bits = bigint_bits(a);
    if ((bits - 1u) * e + 1u > context->limits.max_bigint_bits) return SJS_ERR_LIMIT;
    uint64_t most = bits * e / 32u + 2u;
    uint32_t capacity = most > bigint_limit_limbs(context) + 2u ? bigint_limit_limbs(context) + 2u : (uint32_t)most;
    sjs_bigint *out;
    sjs_result r = bigint_alloc(context, capacity, &out);
    if (r != SJS_OK) return r;
    sjs_zero(out->limbs, (size_t)capacity * sizeof(uint32_t));
    bool single = a->limbs[a->length - 1u] == (1u << (limb_bits(a->limbs[a->length - 1u]) - 1u));
    for (uint32_t i = 0; single && i + 1u < a->length; i++) single = a->limbs[i] == 0;
    if (single) {
        uint64_t at = (bits - 1u) * e;
        out->limbs[at / 32u] = 1u << (at % 32u);
        out->length = (uint32_t)(at / 32u) + 1u;
        op->cost = out->length / 16u;
        return bigint_give(context, op, (bigint_made){ out, op->negative });
    }
    uint32_t *scratch = sjs_mem_alloc(&context->counting, (size_t)capacity * sizeof(uint32_t));
    if (scratch == NULL) {
        bigint_free(context, out);
        return SJS_ERR_NOMEM;
    }
    /* `x` holds the power so far, in one of the two blocks; each product
     * goes into the other. */
    uint32_t *x = out->limbs;
    uint32_t *other = scratch;
    sjs_copy(x, a->limbs, (size_t)a->length * sizeof(uint32_t));
    uint32_t length = a->length;
    uint64_t work = 0;
    r = SJS_OK;
    for (uint32_t bit = limb_bits(e) - 1u; bit-- > 0 && r == SJS_OK;) {
        for (uint32_t pass = 0; pass < 2u && r == SJS_OK; pass++) {
            magnitude by = pass == 0 ? (magnitude){ x, length } : magnitude_of(a);
            if (pass == 1u && ((e >> bit) & 1u) == 0) break;
            if ((uint64_t)length + by.length > capacity) {
                r = SJS_ERR_LIMIT;
                break;
            }
            work += magnitude_multiply(other, (magnitude){ x, length }, by);
            length += by.length;
            while (length > 0 && other[length - 1u] == 0) length--;
            uint32_t *t = x;
            x = other;
            other = t;
            if ((uint64_t)(length - 1u) * 32u + limb_bits(x[length - 1u]) > context->limits.max_bigint_bits) r = SJS_ERR_LIMIT;
        }
    }
    if (r == SJS_OK && x != out->limbs) sjs_copy(out->limbs, x, (size_t)length * sizeof(uint32_t));
    sjs_mem_free(&context->counting, scratch, (size_t)capacity * sizeof(uint32_t));
    if (r != SJS_OK) {
        bigint_free(context, out);
        return r;
    }
    out->length = length;
    op->cost = work / 16u;
    return bigint_give(context, op, (bigint_made){ out, op->negative });
}

sjs_result sjs_bigint_binary(sjs_context *context, sjs_bigint_op *op) {
    op->result = sjs_undefined();
    op->cost = 0;
    op->fault = NULL;
    op->negative = false;
    op->took = 0;
    switch (op->op) {
    case SJS_OP_ADD:
    case SJS_OP_SUBTRACT:
        return bigint_add(context, op);
    case SJS_OP_MULTIPLY:
        return bigint_multiply(context, op);
    case SJS_OP_DIVIDE:
    case SJS_OP_REMAINDER:
        return bigint_divide(context, op);
    case SJS_OP_EXPONENT:
        return bigint_power(context, op);
    default: { /* SJS_OP_NEGATE */
        const sjs_bigint *a = SJS_AS_BIGINT(op->a);
        if (a->length == 0) return bigint_share(op, op->a);
        op->negative = !sjs_bigint_negative(a);
        if ((op->reuse & SJS_BIGINT_REUSE_A) != 0) {
            /* BIGINT_REUSE: the sign alone changes. */
            op->took = SJS_BIGINT_REUSE_A;
            return bigint_give(context, op, (bigint_made){ SJS_AS_BIGINT(op->a), op->negative });
        }
        return bigint_copy(context, op, a);
    }
    }
}

/* ------------------------------------------------------------------------
 * Comparison, with a BigInt or a number
 */

int sjs_bigint_compare(const sjs_bigint *a, const sjs_bigint *b) {
    bool an = sjs_bigint_negative(a);
    if (an != sjs_bigint_negative(b)) return an ? -1 : 1;
    int c = magnitude_compare(magnitude_of(a), magnitude_of(b));
    return an ? -c : c;
}

/* A finite double's integer part as a magnitude (at most 1024 bits), and
 * whether it has a fraction. */
typedef struct {
    uint32_t limbs[33];
    uint32_t length;
    bool fraction;
} double_parts;

static void double_split(double d, double_parts *out) {
    union {
        double d;
        uint64_t u;
    } bits;
    bits.d = d;
    uint64_t u = bits.u & ~(1ull << 63);
    int32_t exponent = (int32_t)(u >> 52);
    uint64_t mantissa = u & ((1ull << 52) - 1u);
    sjs_zero(out, sizeof *out);
    if (exponent == 0) {
        out->fraction = mantissa != 0; /* a subnormal: under 1 */
        return;
    }
    mantissa |= 1ull << 52;
    int32_t shift = exponent - 1075; /* value = mantissa * 2^shift */
    if (shift <= -53) {
        out->fraction = true;
        return;
    }
    if (shift < 0) {
        out->fraction = (mantissa & ((1ull << -shift) - 1u)) != 0;
        mantissa >>= -shift;
        shift = 0;
    }
    uint32_t at = (uint32_t)shift / 32u;
    uint32_t rest = (uint32_t)shift % 32u;
    uint64_t low = mantissa << rest; /* the bits past 64 follow */
    out->limbs[at] = (uint32_t)low;
    out->limbs[at + 1u] = (uint32_t)(low >> 32);
    out->limbs[at + 2u] = rest == 0 ? 0u : (uint32_t)(mantissa >> (64u - rest));
    out->length = at + 3u;
    while (out->length > 0 && out->limbs[out->length - 1u] == 0) out->length--;
}

int sjs_bigint_compare_number(const sjs_bigint *a, double d) {
    if (d != d) return SJS_BIGINT_UNORDERED;
    if (d == 1.0 / 0.0) return -1;
    if (d == -1.0 / 0.0) return 1;
    int as = a->length == 0 ? 0 : sjs_bigint_negative(a) ? -1 : 1;
    int ds = d > 0 ? 1 : d < 0 ? -1 : 0;
    if (as != ds) return as < ds ? -1 : 1;
    if (as == 0) return 0;
    double_parts parts;
    double_split(d, &parts);
    int c = magnitude_compare(magnitude_of(a), (magnitude){ parts.limbs, parts.length });
    if (c == 0 && parts.fraction) c = -1;
    return as < 0 ? -c : c;
}

/* ------------------------------------------------------------------------
 * Conversions: numbers, and BigInt.asIntN and asUintN
 */

sjs_result sjs_bigint_from_number(sjs_context *context, double d, sjs_value *out) {
    double_parts parts;
    double_split(d, &parts);
    sjs_bigint *b;
    sjs_result r = bigint_alloc(context, parts.length, &b);
    if (r != SJS_OK) return r;
    sjs_copy(b->limbs, parts.limbs, (size_t)parts.length * sizeof(uint32_t));
    b->length = parts.length;
    r = bigint_finish(context, b, d < 0);
    if (r != SJS_OK) {
        bigint_free(context, b);
        return r;
    }
    *out = sjs_heap_value(SJS_BIGINT, &b->h);
    return SJS_OK;
}

double sjs_bigint_to_number(const sjs_bigint *b) {
    if (b->length == 0) return 0;
    uint64_t bits = bigint_bits(b);
    sjs_number_top top = { 0, 0, false };
    if (bits > 2048u) {
        top.q = 1ull << 63;
        top.e2 = 2048; /* past the largest double: Infinity */
    } else if (bits <= 64u) {
        uint64_t v = b->limbs[0] | (b->length > 1u ? (uint64_t)b->limbs[1] << 32 : 0u);
        top.q = v << (64u - bits);
        top.e2 = (int32_t)bits - 64;
    } else {
        /* The top 64 bits, and whether any below them is set. */
        uint64_t below = bits - 64u;
        uint32_t at = (uint32_t)(below / 32u);
        uint32_t shift = (uint32_t)(below % 32u);
        uint64_t low = b->limbs[at] | (uint64_t)b->limbs[at + 1u] << 32;
        uint64_t high = at + 2u < b->length ? b->limbs[at + 2u] : 0u;
        top.q = low >> shift | (shift != 0 ? high << (64u - shift) : 0u);
        top.e2 = (int32_t)below;
        top.sticky = shift != 0 && (b->limbs[at] & ((1u << shift) - 1u)) != 0;
        for (uint32_t i = 0; i < at && !top.sticky; i++) top.sticky = b->limbs[i] != 0;
    }
    double d = sjs_number_from_top(top);
    return sjs_bigint_negative(b) ? -d : d;
}

/* The `bits` low bits of a magnitude, in all of out's limbs. */
static void low_bits(sjs_bigint *out, const sjs_bigint *x, uint64_t bits) {
    for (uint32_t i = 0; i < out->length; i++) out->limbs[i] = i < x->length ? x->limbs[i] : 0u;
    if (bits % 32u != 0) out->limbs[out->length - 1u] &= (1u << (bits % 32u)) - 1u;
}

/* 2^bits - v, for v from 1 to 2^bits - 1, in place. */
static void complement(uint32_t *v, uint32_t width, uint64_t bits) {
    uint64_t carry = 1;
    for (uint32_t i = 0; i < width; i++) {
        carry += (uint32_t)~v[i];
        v[i] = (uint32_t)carry;
        carry >>= 32;
    }
    if (bits % 32u != 0) v[width - 1u] &= (1u << (bits % 32u)) - 1u;
}

static bool limbs_zero(const uint32_t *v, uint32_t width) {
    for (uint32_t i = 0; i < width; i++) {
        if (v[i] != 0) return false;
    }
    return true;
}

sjs_result sjs_bigint_wrap(sjs_context *context, sjs_bigint_wrap_params w, sjs_value *out) {
    const sjs_bigint *x = SJS_AS_BIGINT(w.value);
    bool negative = sjs_bigint_negative(x);
    uint64_t bits = bigint_bits(x);
    /* Where the value is its own result. */
    bool power_of_two = x->length != 0 && (x->limbs[x->length - 1u] & (x->limbs[x->length - 1u] - 1u)) == 0;
    for (uint32_t i = 0; power_of_two && i + 1u < x->length; i++) power_of_two = x->limbs[i] == 0;
    bool same = x->length == 0 ? true
              : !negative ? (w.is_signed ? bits < w.bits : bits <= w.bits)
              : w.is_signed && (bits < w.bits || (bits == w.bits && power_of_two));
    if (same) {
        sjs_ref(w.value);
        *out = w.value;
        return SJS_OK;
    }
    if (negative && !w.is_signed && w.bits > context->limits.max_bigint_bits) return SJS_ERR_LIMIT;
    uint32_t width = (uint32_t)((w.bits + 31u) / 32u);
    sjs_bigint *b;
    sjs_result r = bigint_alloc(context, width, &b);
    if (r != SJS_OK) return r;
    b->length = width;
    low_bits(b, x, w.bits);
    uint64_t sign_bit = w.bits - 1u;
    bool result_negative = false;
    if (!limbs_zero(b->limbs, width)) {
        if (negative) complement(b->limbs, width, w.bits); /* x mod 2^bits */
        if (w.is_signed && ((b->limbs[sign_bit / 32u] >> (sign_bit % 32u)) & 1u) != 0) {
            complement(b->limbs, width, w.bits); /* minus 2^bits */
            result_negative = true;
        }
    }
    r = bigint_finish(context, b, result_negative);
    if (r != SJS_OK) {
        bigint_free(context, b);
        return r;
    }
    *out = sjs_heap_value(SJS_BIGINT, &b->h);
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * The public API
 */

sjs_result sjs_bigint_new(sjs_context *context, sjs_str text, sjs_value *out) {
    if (context == NULL || out == NULL || (text.ptr == NULL && text.length != 0)) return SJS_ERR_INVALID;
    sjs_bigint_digits digits;
    if (!sjs_utf8_valid(text) || !sjs_bigint_parse(text, &digits)) return SJS_ERR_INVALID;
    return sjs_bigint_from_digits(context, digits, out);
}

uint32_t sjs_bigint_text_bound(sjs_value bigint) {
    if (bigint.kind != SJS_BIGINT || bigint.internal != 0) return 0;
    /* A value under 2^bits has at most bits * log10(2) + 1 digits, and
     * 1234/4096 > log10(2); one more for the sign. */
    uint64_t bound = bigint_bits(SJS_AS_BIGINT(bigint)) * 1234u / 4096u + 2u;
    return bound > UINT32_MAX ? UINT32_MAX : (uint32_t)bound;
}

sjs_result sjs_bigint_text(sjs_context *context, sjs_value bigint, sjs_buffer *buffer) {
    if (context == NULL || buffer == NULL || bigint.kind != SJS_BIGINT || bigint.internal != 0 ||
        (buffer->ptr == NULL && buffer->capacity != 0) || buffer->length > buffer->capacity) {
        return SJS_ERR_INVALID;
    }
    sjs_buf text = { 0 };
    sjs_result r = sjs_bigint_format(&context->counting, (sjs_bigint_radix){ SJS_AS_BIGINT(bigint), 10u }, &text);
    if (r == SJS_OK && text.length > buffer->capacity - buffer->length) r = SJS_NEED_SPACE;
    if (r == SJS_OK) {
        sjs_copy(buffer->ptr + buffer->length, text.ptr, text.length);
        buffer->length += text.length;
    }
    sjs_buf_free(&context->counting, &text);
    return r;
}

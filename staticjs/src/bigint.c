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

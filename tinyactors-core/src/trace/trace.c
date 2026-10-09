/* trace.c — trace values: span IDs from step keys, traceparent text, the
 * seeded generator and ratio sampling. Ported bit for bit from the earlier
 * core (943b1f31^:tinyactors-core/src/trace.c), so span IDs and generated
 * trace IDs are the ones 0.1 produced. */
#include <tinyactors/trace.h>

#include "base/config.h"
#include "trace/span.h"

ta_span_id ta_step_key_span_id(ta_step_key key) {
    if (key.session == 0) return key.macrostep;
    return ta_step_span_finish(ta_step_span_prefix(key.session, key.macrostep), key.microstep);
}

ta_trace_context ta_trace_context_child(const ta_trace_context *context, ta_step_key key) {
    ta_trace_context child = *context;
    child.parent = ta_step_key_span_id(key);
    child.pad = 0;
    return child;
}

/* ---- traceparent text --------------------------------------------------------------- */

/* A lowercase hex digit's value plus one, 0 for anything else. */
static const uint8_t trace_hex_value[256] = {
    ['0'] = 1, ['1'] = 2,  ['2'] = 3,  ['3'] = 4,  ['4'] = 5,  ['5'] = 6,  ['6'] = 7,  ['7'] = 8,
    ['8'] = 9, ['9'] = 10, ['a'] = 11, ['b'] = 12, ['c'] = 13, ['d'] = 14, ['e'] = 15, ['f'] = 16,
};

/* `count` lowercase hex digits (at most 16), a digit at a time. */
static int trace_hex_read(const char *text, uint32_t count, uint64_t *out) {
    uint64_t value = 0;
    uint32_t valid = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t digit = trace_hex_value[(uint8_t)text[i]];
        valid &= digit != 0;
        value = (value << 4) | (uint64_t)((digit - 1u) & 0xfu);
    }
    *out = value;
    return valid != 0;
}

/* Eight lowercase hex digits at once (SWAR), independent of byte order. */
static int trace_hex_read8(const char *text, uint32_t *out) {
    const unsigned char *bytes = (const unsigned char *)text;
    uint64_t x = 0;
    for (uint32_t i = 0; i < 8u; i++)
        x |= (uint64_t)bytes[i] << (8u * i);
    const uint64_t ones = 0x0101010101010101u, highs = 0x8080808080808080u;
    if (x & highs) return 0; /* not ASCII; keeps the additions below inside each byte */
    /* bit 7 of each byte: b >= lo is (b + 0x80 - lo), b > hi is (b + 0x7f - hi) */
    uint64_t digit = (x + ones * 0x50u) & ~(x + ones * 0x46u); /* '0'..'9' */
    uint64_t alpha = (x + ones * 0x1fu) & ~(x + ones * 0x19u); /* 'a'..'f' */
    if (((digit | alpha) & highs) != highs) return 0;
    /* a digit's value is its low nibble, plus 9 for a letter (bit 6 set) */
    uint64_t v = (x & (ones * 0x0fu)) + ((x >> 6) & ones) * 9u;
    /* the first character is in the lowest byte and is the most significant nibble */
    v = ((v << 4) | (v >> 8)) & 0x00ff00ff00ff00ffu;
    v = ((v << 8) | (v >> 16)) & 0x0000ffff0000ffffu;
    v = ((v << 16) | (v >> 32)) & 0x00000000ffffffffu;
    *out = (uint32_t)v;
    return 1;
}

static int trace_hex_read16(const char *text, uint64_t *out) {
    uint32_t high = 0, low = 0;
    int high_valid = trace_hex_read8(text, &high);
    int low_valid = trace_hex_read8(text + 8, &low);
    *out = ((uint64_t)high << 32) | low;
    return high_valid && low_valid;
}

static int trace_space(char c) {
    return c == ' ' || c == '\t';
}

ta_status ta_traceparent_parse(ta_string_view text, ta_trace_context *out) {
    if (!out || (text.length && !text.pointer)) return TA_ERROR_INVALID;
    const char *p = text.pointer;
    uint32_t length = text.length;
    while (length && trace_space(p[0])) {
        p++;
        length--;
    }
    while (length && trace_space(p[length - 1u]))
        length--;
    uint64_t version = 0, high = 0, low = 0, span = 0, flags = 0;
    if (length < TA_TRACEPARENT_LENGTH || !trace_hex_read(p, 2u, &version) || version == 0xffu ||
        p[2] != '-' || p[35] != '-' || p[52] != '-' || !trace_hex_read16(p + 3, &high) ||
        !trace_hex_read16(p + 19, &low) || !trace_hex_read16(p + 36, &span) ||
        !trace_hex_read(p + 53, 2u, &flags))
        return TA_ERROR_INVALID;
    /* version 00 is exactly 55 characters; a later one may carry more after a dash */
    if (length != TA_TRACEPARENT_LENGTH && (version == 0 || p[TA_TRACEPARENT_LENGTH] != '-'))
        return TA_ERROR_INVALID;
    if ((high == 0 && low == 0) || span == 0) return TA_ERROR_INVALID;
    out->trace.high = high;
    out->trace.low = low;
    out->parent = span;
    out->flags = (uint32_t)flags;
    out->pad = 0;
    return TA_OK;
}

static void trace_hex_write(char *out, uint64_t value, uint32_t digits) {
    static const char digit[] = "0123456789abcdef";
    for (uint32_t i = 0; i < digits; i++)
        out[i] = digit[(value >> (4u * (digits - 1u - i))) & 0xfu];
}

uint32_t ta_traceparent_format(const ta_trace_context *context, char *buffer, uint32_t capacity) {
    if (!context || (context->trace.high == 0 && context->trace.low == 0) || !context->parent ||
        !buffer || capacity < TA_TRACEPARENT_LENGTH)
        return 0;
    buffer[0] = '0';
    buffer[1] = '0';
    buffer[2] = '-';
    trace_hex_write(buffer + 3, context->trace.high, 16u);
    trace_hex_write(buffer + 19, context->trace.low, 16u);
    buffer[35] = '-';
    trace_hex_write(buffer + 36, context->parent, 16u);
    buffer[52] = '-';
    trace_hex_write(buffer + 53, context->flags & 0xffu, 2u);
    return TA_TRACEPARENT_LENGTH;
}

uint32_t ta_trace_id_format(ta_trace_id trace, char *buffer, uint32_t capacity) {
    if (!buffer || capacity < TA_TRACE_ID_TEXT_LENGTH) return 0;
    trace_hex_write(buffer, trace.high, 16u);
    trace_hex_write(buffer + 16, trace.low, 16u);
    return TA_TRACE_ID_TEXT_LENGTH;
}

uint32_t ta_span_id_format(ta_span_id span, char *buffer, uint32_t capacity) {
    if (!buffer || capacity < TA_SPAN_ID_TEXT_LENGTH) return 0;
    trace_hex_write(buffer, span, 16u);
    return TA_SPAN_ID_TEXT_LENGTH;
}

/* ---- the generator: xoshiro256**, seeded through trace_splitmix64 -------------------------- */

static uint64_t trace_splitmix64(uint64_t *state) {
    *state += 0x9e3779b97f4a7c15ull;
    uint64_t z = *state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint64_t trace_rotate_left(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static uint64_t trace_generator_next(ta_trace_generator *generator) {
    uint64_t *s = generator->state;
    uint64_t result = trace_rotate_left(s[1] * 5u, 7) * 9u;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = trace_rotate_left(s[3], 45);
    return result;
}

void ta_trace_generator_seed(ta_trace_generator *generator, uint64_t seed) {
    uint64_t state = seed;
    for (uint32_t i = 0; i < 4u; i++)
        generator->state[i] = trace_splitmix64(&state);
}

ta_trace_id ta_trace_generator_next(ta_trace_generator *generator) {
    ta_trace_id trace = {0, 0};
    while (trace.high == 0 && trace.low == 0) {
        trace.high = trace_generator_next(generator);
        trace.low = trace_generator_next(generator);
    }
    return trace;
}

/* ---- sampling ------------------------------------------------------------------------- */

#define TRACE_RANDOM_BITS ((uint64_t)1u << 56)

uint64_t ta_trace_ratio_threshold(uint32_t parts, uint32_t whole) {
    if (whole == 0 || parts == 0) return TRACE_RANDOM_BITS; /* nothing is sampled */
    if (parts >= whole) return 0;
    /* round(rejected * 2^56 / whole), in 64 bits: 2^56 = q * whole + r */
    uint64_t rejected = (uint64_t)(whole - parts);
    uint64_t q = TRACE_RANDOM_BITS / whole, r = TRACE_RANDOM_BITS % whole;
    return rejected * q + (rejected * r + whole / 2u) / whole;
}

int ta_trace_sampled(ta_trace_id trace, uint64_t threshold) {
    return (trace.low & (TRACE_RANDOM_BITS - 1u)) >= threshold;
}

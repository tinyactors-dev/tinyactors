#include "trace.h"
#include "system.h"
#include "interpreter.h"

/* ------------------------------------------------------------------------
 * The null sink (ADR 0067 section 2)
 */

static void ta_null_trace_write(void *self, const ta_trace_record *record) {
    (void)self;
    (void)record;
}

static const ta_trace_sink_vtable ta_null_trace_vtable = { .write = ta_null_trace_write };

ta_trace_sink ta_null_trace_sink(void) {
    return (ta_trace_sink){ .self = NULL, .v = &ta_null_trace_vtable };
}

ta_result ta_system_set_trace_sink(ta_system *system, ta_trace_sink sink) {
    if (system == NULL || sink.v == NULL || sink.v->write == NULL) {
        return TA_ERR_INVALID;
    }
    system->trace = sink;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Step keys as span IDs (section 6)
 */

/* The finalizer of MurmurHash3's 64-bit variant: every input bit reaches
 * every output bit. Fixed, so every host derives the same span IDs. */
static uint64_t ta_trace_mix(uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

uint64_t ta_step_key_span_id(ta_step_key key) {
    if (key.session == 0) {
        return key.macrostep;
    }
    uint64_t span = ta_trace_mix(key.session);
    span = ta_trace_mix(span ^ key.macrostep);
    span = ta_trace_mix(span ^ ((uint64_t)key.microstep | ((uint64_t)1u << 32)));
    return span == 0 ? 1u : span;
}

/* ------------------------------------------------------------------------
 * What is built
 */

uint32_t ta_trace_macrostep_kinds(const ta_trace_sink *sink, const ta_trace_context *context) {
    if (!ta_trace_context_empty(context) && (context->flags & TA_TRACE_FLAG_SAMPLED) == 0) {
        return sink->unsampled_kinds;
    }
    return (context->detail & TA_TRACE_DETAIL_WATCHED) != 0 ? sink->watched_kinds : sink->kinds;
}

ta_step_key ta_trace_lifecycle_key(ta_actor *actor) {
    actor->trace_lifecycle += 1u;
    return (ta_step_key){ .session = actor->session_id, .microstep = actor->trace_lifecycle };
}

void ta_trace_lifecycle(ta_system *system, ta_actor *actor, ta_trace_kind kind,
                        const ta_trace_parent *parent) {
    ta_step_key key = ta_trace_lifecycle_key(actor);
    /* Counters and occupancy (ADR 0072): every residency change passes here. */
    switch (kind) {
    case TA_TRACE_SPAWNED: TA_COUNT(system, actor->definition, spawned); break;
    case TA_TRACE_LOADED:
        TA_COUNT(system, actor->definition, loaded);
        ta_interpreter_occupancy(actor, true);
        break;
    case TA_TRACE_UNLOADED:
        TA_COUNT(system, actor->definition, unloaded);
        ta_interpreter_occupancy(actor, false);
        break;
    case TA_TRACE_DESTROYED:
        TA_COUNT(system, actor->definition, destroyed);
        ta_interpreter_occupancy(actor, false);
        break;
    default: break;
    }
    ta_trace_context none = { 0 };
    if (!ta_trace_lifecycle_wanted(&system->trace, kind, parent != NULL ? &parent->context : &none)) {
        return;
    }
    ta_trace_record record = ta_trace_record_blank(kind);
    record.key = key;
    record.actor = actor->record.id;
    record.definition = actor->definition->id;
    record.now = ta_actor_now(system, actor);
    if (parent != NULL) {
        record.context = parent->context;
        record.cause = parent->cause;
    }
    ta_trace_write(&system->trace, &record);
}

ta_trace_record ta_trace_record_blank(ta_trace_kind kind) {
    return (ta_trace_record){
        .kind = kind,
        .position = { .at = TA_TRACE_AT_NONE, .state = TA_STATE_NONE, .transition = TA_TRANSITION_NONE,
                      .action = TA_ACTION_NONE },
        .state = TA_STATE_NONE,
        .expr = TA_EXPR_ID_NONE,
        .action = TA_ACTION_NONE,
        .invoke = TA_INVOKE_ID_NONE,
    };
}

/* ------------------------------------------------------------------------
 * traceparent text (ADR 0073 section 1)
 */

#define TA_TRACEPARENT_LEN 55u

/*
 * A lowercase hex digit's value plus one, and 0 for anything else, so the
 * table's default (zero) is "not a digit". A traceparent has 52 digits,
 * read on every post that carries one: a lookup and an AND per digit, and
 * one test per field, keep that cheap (ADR 0073, napi ADR 0017 section 2).
 */
static const uint8_t ta_hex_value[256] = {
    ['0'] = 1, ['1'] = 2, ['2'] = 3, ['3'] = 4, ['4'] = 5, ['5'] = 6, ['6'] = 7, ['7'] = 8, ['8'] = 9, ['9'] = 10,
    ['a'] = 11, ['b'] = 12, ['c'] = 13, ['d'] = 14, ['e'] = 15, ['f'] = 16,
};

/* Reads `count` lowercase hex digits (at most 16) into `*out`, one at a time: the 2-digit fields. */
static bool ta_hex_read(const char *text, uint32_t count, uint64_t *out) {
    uint64_t value = 0;
    uint32_t valid = 1;
    for (uint32_t index = 0; index < count; index += 1) {
        uint32_t digit = ta_hex_value[(uint8_t)text[index]];
        valid &= digit != 0;
        value = (value << 4) | (uint64_t)((digit - 1u) & 0xfu);
    }
    *out = value;
    return valid != 0;
}

/*
 * Eight lowercase hex digits at once (SWAR): loaded with shifts, so the
 * result does not depend on byte order, validated with byte-wise range
 * tests, and packed into 32 bits. On 16-digit fields this is several times
 * faster than a digit at a time.
 */
static bool ta_hex_read8(const char *text, uint32_t *out) {
    const unsigned char *bytes = (const unsigned char *)text;
    uint64_t x = 0;
    for (uint32_t index = 0; index < 8u; index += 1u) {
        x |= (uint64_t)bytes[index] << (8u * index);
    }
    const uint64_t ones = 0x0101010101010101u;
    const uint64_t highs = 0x8080808080808080u;
    if ((x & highs) != 0) {
        return false; /* not ASCII; also keeps the additions below inside each byte */
    }
    /* Bit 7 of each byte: b >= lo is (b + 0x80 - lo), b > hi is (b + 0x7f - hi). */
    uint64_t digit = (x + ones * 0x50u) & ~(x + ones * 0x46u);  /* '0'..'9' */
    uint64_t alpha = (x + ones * 0x1fu) & ~(x + ones * 0x19u);  /* 'a'..'f' */
    if (((digit | alpha) & highs) != highs) {
        return false;
    }
    /* A digit's value is its low nibble, plus 9 for a letter (bit 6 set). */
    uint64_t v = (x & (ones * 0x0fu)) + ((x >> 6) & ones) * 9u;
    /* The first character is in the lowest byte and is the most significant nibble. */
    v = ((v << 4) | (v >> 8)) & 0x00ff00ff00ff00ffu;
    v = ((v << 8) | (v >> 16)) & 0x0000ffff0000ffffu;
    v = ((v << 16) | (v >> 32)) & 0x00000000ffffffffu;
    *out = (uint32_t)v;
    return true;
}

/* Reads 16 lowercase hex digits into `*out`. */
static bool ta_hex_read16(const char *text, uint64_t *out) {
    uint32_t high = 0;
    uint32_t low = 0;
    bool high_valid = ta_hex_read8(text, &high);
    bool low_valid = ta_hex_read8(text + 8, &low);
    *out = ((uint64_t)high << 32) | low;
    return high_valid && low_valid;
}

static const char ta_hex_digits[] = "0123456789abcdef";

/* Writes `count` hex digits of `value`, most significant first. */
static void ta_hex_write(char *out, uint64_t value, uint32_t count) {
    for (uint32_t index = count; index > 0; index -= 1) {
        out[index - 1u] = ta_hex_digits[value & 0xfu];
        value >>= 4;
    }
}

static bool ta_trace_space(char c) {
    return c == ' ' || c == '\t';
}

ta_result ta_traceparent_parse(const ta_traceparent_parse_params *params) {
    if (params == NULL || params->out == NULL || (params->text.len != 0 && params->text.ptr == NULL)) {
        return TA_ERR_INVALID;
    }
    const char *text = params->text.ptr;
    uint32_t len = params->text.len;
    while (len != 0 && ta_trace_space(text[0])) {
        text += 1;
        len -= 1;
    }
    while (len != 0 && ta_trace_space(text[len - 1u])) {
        len -= 1;
    }
    uint64_t version = 0;
    uint64_t high = 0;
    uint64_t low = 0;
    uint64_t span = 0;
    uint64_t flags = 0;
    if (len < TA_TRACEPARENT_LEN || !ta_hex_read(text, 2u, &version) || version == 0xffu || text[2] != '-' ||
        text[35] != '-' || text[52] != '-' || !ta_hex_read16(text + 3, &high) ||
        !ta_hex_read16(text + 19, &low) || !ta_hex_read16(text + 36, &span) ||
        !ta_hex_read(text + 53, 2u, &flags)) {
        return TA_ERR_INVALID;
    }
    /* Version 00 is exactly 55 characters; a later one may carry more after a dash. */
    if (len != TA_TRACEPARENT_LEN && (version == 0 || text[TA_TRACEPARENT_LEN] != '-')) {
        return TA_ERR_INVALID;
    }
    if ((high == 0 && low == 0) || span == 0) {
        return TA_ERR_INVALID;
    }
    *params->out = (ta_trace_parent){
        .context = { .trace_id_high = high, .trace_id_low = low, .flags = (uint8_t)flags },
        .cause = { .session = 0, .macrostep = span, .microstep = 0 },
    };
    return TA_OK;
}

/* Writes `len` characters and a NUL through the buffer convention. */
static ta_result ta_trace_text_begin(char *buffer, uint32_t capacity, uint32_t *needed, uint32_t len) {
    if (needed == NULL) {
        return TA_ERR_INVALID;
    }
    *needed = len + 1u;
    if (buffer == NULL || capacity < len + 1u) {
        return TA_ERR_NOMEM;
    }
    buffer[len] = '\0';
    return TA_OK;
}

ta_result ta_traceparent_format(const ta_traceparent_format_params *params) {
    if (params == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t span = ta_step_key_span_id(params->parent.cause);
    if (ta_trace_context_empty(&params->parent.context) || span == 0) {
        if (params->needed != NULL) {
            *params->needed = 0;
        }
        return TA_ERR_INVALID;
    }
    ta_result result = ta_trace_text_begin(params->buffer, params->capacity, params->needed, TA_TRACEPARENT_LEN);
    if (result != TA_OK) {
        return result;
    }
    char *out = params->buffer;
    out[0] = '0';
    out[1] = '0';
    out[2] = '-';
    ta_hex_write(out + 3, params->parent.context.trace_id_high, 16u);
    ta_hex_write(out + 19, params->parent.context.trace_id_low, 16u);
    out[35] = '-';
    ta_hex_write(out + 36, span, 16u);
    out[52] = '-';
    ta_hex_write(out + 53, params->parent.context.flags, 2u);
    return TA_OK;
}

ta_result ta_trace_id_format(const ta_trace_id_format_params *params) {
    if (params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_trace_text_begin(params->buffer, params->capacity, params->needed, 32u);
    if (result == TA_OK) {
        ta_hex_write(params->buffer, params->context.trace_id_high, 16u);
        ta_hex_write(params->buffer + 16, params->context.trace_id_low, 16u);
    }
    return result;
}

ta_result ta_span_id_format(const ta_span_id_format_params *params) {
    if (params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_trace_text_begin(params->buffer, params->capacity, params->needed, 16u);
    if (result == TA_OK) {
        ta_hex_write(params->buffer, params->span_id, 16u);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Sampling (ADR 0073 section 3)
 */

#define TA_TRACE_RANDOM_BITS ((uint64_t)1u << 56)

uint64_t ta_trace_ratio_threshold(uint32_t parts, uint32_t whole) {
    if (whole == 0 || parts == 0) {
        return TA_TRACE_RANDOM_BITS; /* nothing is sampled */
    }
    if (parts >= whole) {
        return 0;
    }
    /* round(rejected * 2^56 / whole), in 64 bits: 2^56 = q * whole + r. */
    uint64_t rejected = (uint64_t)(whole - parts);
    uint64_t q = TA_TRACE_RANDOM_BITS / whole;
    uint64_t r = TA_TRACE_RANDOM_BITS % whole;
    return rejected * q + (rejected * r + whole / 2u) / whole;
}

bool ta_trace_sampled_at(const ta_trace_context *context, uint64_t threshold) {
    return context != NULL && (context->trace_id_low & (TA_TRACE_RANDOM_BITS - 1u)) >= threshold;
}

void ta_system_set_trace_ratio(ta_system *system, uint32_t parts, uint32_t whole) {
    if (system != NULL) {
        system->trace_threshold = ta_trace_ratio_threshold(parts, whole);
    }
}

/* ------------------------------------------------------------------------
 * The generator (ADR 0073 section 2): xoshiro256**, seeded through SplitMix64
 */

static uint64_t ta_splitmix64(uint64_t *state) {
    *state += 0x9e3779b97f4a7c15ull;
    uint64_t z = *state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint64_t ta_rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static uint64_t ta_trace_next(ta_system *system) {
    uint64_t *s = system->trace_rng;
    uint64_t result = ta_rotl(s[1] * 5u, 7) * 9u;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = ta_rotl(s[3], 45);
    return result;
}

void ta_system_seed_trace_ids(ta_system *system, uint64_t seed) {
    if (system == NULL) {
        return;
    }
    uint64_t state = seed;
    for (uint32_t index = 0; index < 4u; index += 1) {
        system->trace_rng[index] = ta_splitmix64(&state);
    }
}

uint64_t ta_system_new_span_id(ta_system *system) {
    if (system == NULL) {
        return 0;
    }
    uint64_t span = 0;
    while (span == 0) {
        span = ta_trace_next(system);
    }
    return span;
}

ta_trace_context ta_system_new_trace(ta_system *system) {
    if (system == NULL) {
        return (ta_trace_context){0};
    }
    ta_trace_context context = { .flags = TA_TRACE_FLAG_RANDOM };
    while (context.trace_id_high == 0 && context.trace_id_low == 0) {
        context.trace_id_high = ta_trace_next(system);
        context.trace_id_low = ta_trace_next(system);
    }
    if (ta_trace_sampled_at(&context, system->trace_threshold)) {
        context.flags |= TA_TRACE_FLAG_SAMPLED;
    }
    return context;
}

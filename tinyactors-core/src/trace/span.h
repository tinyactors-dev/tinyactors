/* span.h — the mix behind ta_step_key_span_id, in two halves: what depends
 * on the session and the macrostep, and the microstep. The kernel keeps the
 * first half for the macrostep it writes records about, so a record costs
 * one round of the mix instead of six (docs/adr/0006 in the repository
 * root, performance). trace.c computes span IDs with the same functions. */
#ifndef TINYACTORS_TRACE_SPAN_H
#define TINYACTORS_TRACE_SPAN_H

#include <tinyactors/trace.h>

#include "base/config.h"

/* MurmurHash3's 64-bit finalizer: a bijection in which every input bit
 * reaches every output bit; 0 maps to 0. */
TA_INLINE uint64_t ta_trace_mix64(uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

/* The span IDs of a macrostep's steps share this (session not 0). */
TA_INLINE uint64_t ta_step_span_prefix(uint64_t session, uint64_t macrostep) {
    return ta_trace_mix64(ta_trace_mix64(session) ^ macrostep);
}

/* The span ID of microstep `microstep` (0: the macrostep) under `prefix`. */
TA_INLINE ta_span_id ta_step_span_finish(uint64_t prefix, uint32_t microstep) {
    uint64_t span = ta_trace_mix64(prefix ^ ((uint64_t)microstep | ((uint64_t)1u << 32)));
    return span == 0 ? 1u : span;
}

#endif /* TINYACTORS_TRACE_SPAN_H */

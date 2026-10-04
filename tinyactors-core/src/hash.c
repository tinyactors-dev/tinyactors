#include "hash.h"

uint32_t ta_fnv1a32(uint32_t state, const void *bytes, size_t size) {
    const unsigned char *in = bytes;
    for (size_t index = 0; index < size; index += 1) {
        state ^= in[index];
        state *= 16777619u;
    }
    return state;
}

uint64_t ta_fnv1a64(uint64_t state, const void *bytes, size_t size) {
    const unsigned char *in = bytes;
    for (size_t index = 0; index < size; index += 1) {
        state ^= in[index];
        state *= 1099511628211ull;
    }
    return state;
}

uint64_t ta_mix64(uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

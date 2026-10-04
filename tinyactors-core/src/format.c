/*
 * Implementation of block D6 (format). The contract, lifetime and usage are
 * in format.h; this comment records what the code relies on.
 *
 * Lifetime and allocation. Call lifetime only. Nothing here allocates,
 * frees or keeps a pointer. The writers fill a buffer the caller owns and
 * sized beforehand; they never look past the count they were given or
 * return, and write no terminator. The caller sizes the buffer with
 * ta_dec_len (decimal) or from the digit count it passes (hex), so no size
 * arithmetic happens here and none can overflow.
 *
 * Ordering. Both writers fill from the last digit backwards, so the most
 * significant digit lands first and no scratch buffer or reversal is needed.
 * Decimal peels one digit per division by 10; hex peels one nibble per shift
 * by 4. ta_hex_write keeps only the low "count" digits of the value and
 * pads on the left with zeros, which is what fixed-width fingerprints and
 * IDs need.
 *
 * Case. ta_hex_write always writes lower case, which is the form persisted
 * in IDs and fingerprints. ta_hex_digit takes the case as an argument for
 * the callers that print upper case. ta_hex_digit_value accepts both and
 * returns -1 for anything else, so parsing is case-insensitive while output
 * is canonical.
 *
 * Complexity. Each function is O(digits); ta_hex_digit and
 * ta_hex_digit_value are O(1).
 */

#include "format.h"

uint32_t ta_dec_len(uint64_t value) {
    uint32_t count = 1;
    while (value >= 10u) {
        value /= 10u;
        count += 1;
    }
    return count;
}

uint32_t ta_dec_write(char *out, uint64_t value) {
    uint32_t count = ta_dec_len(value);
    for (uint32_t index = count; index > 0; index -= 1) {
        out[index - 1u] = (char)('0' + value % 10u);
        value /= 10u;
    }
    return count;
}

char ta_hex_digit(uint32_t nibble, bool upper) {
    static const char lower[] = "0123456789abcdef";
    static const char capital[] = "0123456789ABCDEF";
    return (upper ? capital : lower)[nibble & 0xfu];
}

void ta_hex_write(char *out, uint64_t value, uint32_t count) {
    for (uint32_t index = count; index > 0; index -= 1) {
        out[index - 1u] = ta_hex_digit((uint32_t)(value & 0xfu), false);
        value >>= 4;
    }
}

int ta_hex_digit_value(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

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

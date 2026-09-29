#include "memory.h"

const void *tah_mem_find(const void *from, unsigned char byte, size_t size) {
    const unsigned char *in = from;
    for (size_t index = 0; index < size; index += 1u) {
        if (in[index] == byte) {
            return in + index;
        }
    }
    return NULL;
}

size_t tah_str_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length += 1u;
    }
    return length;
}

uint32_t tah_format_u64(char *out, uint64_t value) {
    char reversed[20];
    uint32_t count = 0;
    do {
        reversed[count] = (char)('0' + value % 10u);
        count += 1u;
        value /= 10u;
    } while (value != 0u);
    for (uint32_t index = 0; index < count; index += 1u) {
        out[index] = reversed[count - 1u - index];
    }
    return count;
}

#include "memory.h"

void tah_mem_copy(void *to, const void *from, size_t size) {
    unsigned char *out = to;
    const unsigned char *in = from;
    for (size_t index = 0; index < size; index += 1u) {
        out[index] = in[index];
    }
}

void tah_mem_set(void *to, unsigned char byte, size_t size) {
    unsigned char *out = to;
    for (size_t index = 0; index < size; index += 1u) {
        out[index] = byte;
    }
}

bool tah_mem_equal(const void *left, const void *right, size_t size) {
    const unsigned char *a = left;
    const unsigned char *b = right;
    for (size_t index = 0; index < size; index += 1u) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

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

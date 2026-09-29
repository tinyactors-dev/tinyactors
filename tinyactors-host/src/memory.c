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

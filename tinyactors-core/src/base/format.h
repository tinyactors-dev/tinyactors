/* format.h — decimal numbers and bounded text, without the C library.
 *
 * What snprintf did for the layers that may not call it: ta_format_u64
 * writes a number, and ta_text appends views and numbers to a buffer of
 * fixed capacity. An append that does not fit writes nothing and sets
 * `overflow`; the caller checks it and reports an error. A text is never
 * cut short silently. */
#ifndef TINYACTORS_BASE_FORMAT_H
#define TINYACTORS_BASE_FORMAT_H

#include "base/config.h"

/* The longest decimal uint64_t (18446744073709551615). */
#define TA_FORMAT_U64_MAX 20u

/* Write `value` in decimal into buffer (room for TA_FORMAT_U64_MAX bytes;
 * no terminator). Returns the length. */
uint32_t ta_format_u64(char *buffer, uint64_t value);

typedef struct ta_text {
    char *pointer;
    uint32_t length, capacity;
    uint32_t overflow; /* 1 once an append did not fit */
} ta_text;

TA_INLINE ta_text ta_text_init(char *buffer, uint32_t capacity) {
    ta_text text = {buffer, 0, capacity, 0};
    return text;
}

void ta_text_append(ta_text *text, ta_string_view view);
void ta_text_append_u64(ta_text *text, uint64_t value);

#endif /* TINYACTORS_BASE_FORMAT_H */

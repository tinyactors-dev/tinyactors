/* format.h — decimal numbers and bounded text, without the C library.
 *
 * What snprintf did for the layers that may not call it: ta_format_u64
 * writes a number, and ta_text appends views and numbers to a buffer of
 * fixed capacity. An append that does not fit writes nothing and sets
 * `overflow`; the caller checks it and reports an error. A text is never
 * cut short silently.
 *
 * ta_format_double and ta_format_quoted write a double and a string as JSON
 * writes them (and CBOR's diagnostic notation, ta_cbor_diagnostic) into an
 * output (base/output.h): it grows through its allocator when it has one;
 * when it cannot, what fits is written and `length` goes on counting, so
 * ta_output_fits tells the caller. */
#ifndef TINYACTORS_BASE_FORMAT_H
#define TINYACTORS_BASE_FORMAT_H

#include "base/config.h"
#include "base/output.h"

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

/* The shortest decimal that reads back as `value` (Steele and White's
 * Dragon4), with a point or an exponent: 1.0, 0.1, -0.0, 1.0e+300,
 * 1.0e-7; NaN, Infinity, -Infinity. */
void ta_format_double(ta_output *output, double value);
/* `text` quoted and escaped as JSON escapes it: `"` and `\` with a
 * backslash, \b \f \n \r \t, the other control characters below 0x20 as
 * \u00xx (lowercase hexadecimal); every other byte as it is. */
void ta_format_quoted(ta_output *output, ta_string_view text);

#endif /* TINYACTORS_BASE_FORMAT_H */

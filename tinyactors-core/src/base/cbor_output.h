/* base/cbor_output.h — CBOR written straight into a caller's bytes, item by
 * item, for core's own replies (src/host/request.c). Shortest heads, as
 * ta_cbor_encode writes them. `length` goes on counting past `capacity`, so
 * one pass tells a caller what a reply needs (TA_ERROR_NO_MEMORY). */
#ifndef TINYACTORS_BASE_CBOR_OUTPUT_H
#define TINYACTORS_BASE_CBOR_OUTPUT_H
#include <tinyactors/types.h>

typedef struct cbor_output {
    unsigned char *bytes;
    size_t capacity, length;
} cbor_output;

enum {
    CBOR_MAJOR_UINT = 0,
    CBOR_MAJOR_NEGINT = 1,
    CBOR_MAJOR_BYTES = 2,
    CBOR_MAJOR_TEXT = 3,
    CBOR_MAJOR_ARRAY = 4,
    CBOR_MAJOR_MAP = 5,
    CBOR_MAJOR_TAG = 6,
    CBOR_MAJOR_SIMPLE = 7
};

void cbor_output_bytes(cbor_output *output, const void *bytes, size_t length);
void cbor_output_head(cbor_output *output, uint32_t major, uint64_t argument);
void cbor_output_float(cbor_output *output, uint64_t bits); /* a double's bits */

static inline void cbor_output_text(cbor_output *output, ta_string_view text) {
    cbor_output_head(output, CBOR_MAJOR_TEXT, text.length);
    cbor_output_bytes(output, text.pointer, text.length);
}
static inline void cbor_output_uint(cbor_output *output, uint64_t value) {
    cbor_output_head(output, CBOR_MAJOR_UINT, value);
}
static inline void cbor_output_simple(cbor_output *output, uint32_t simple) {
    cbor_output_head(output, CBOR_MAJOR_SIMPLE, simple);
}
#endif /* TINYACTORS_BASE_CBOR_OUTPUT_H */

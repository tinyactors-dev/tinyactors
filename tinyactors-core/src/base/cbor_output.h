/* base/cbor_output.h — CBOR written straight into an output (base/output.h),
 * item by item, for core's own replies (src/host/request.c,
 * the host). Shortest heads, as the CBOR writer writes them. */
#ifndef TINYACTORS_BASE_CBOR_OUTPUT_H
#define TINYACTORS_BASE_CBOR_OUTPUT_H
#include <tinyactors/types.h>

#include "base/output.h"

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

void cbor_output_head(ta_output *output, uint32_t major, uint64_t argument);
void cbor_output_float(ta_output *output, uint64_t bits); /* a double's bits */

static inline void cbor_output_text(ta_output *output, ta_string_view text) {
    cbor_output_head(output, CBOR_MAJOR_TEXT, text.length);
    ta_output_write(output, text.pointer, text.length);
}
static inline void cbor_output_uint(ta_output *output, uint64_t value) {
    cbor_output_head(output, CBOR_MAJOR_UINT, value);
}
static inline void cbor_output_simple(ta_output *output, uint32_t simple) {
    cbor_output_head(output, CBOR_MAJOR_SIMPLE, simple);
}
#endif /* TINYACTORS_BASE_CBOR_OUTPUT_H */

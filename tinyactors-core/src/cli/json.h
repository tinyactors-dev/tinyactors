/* cli/json.h — JSON text (RFC 8259) to one CBOR item and back, at the
 * command's JSON-RPC wire: RFC 8949, section 6.2 reading (integers to 64
 * bits, integral numbers as integers: docs/protocol.md has a float be a
 * number that is not an integer) and 6.1 writing. The command's own: in no
 * released library. Freestanding: no C library, no recursion (explicit
 * stacks of TA_JSON_MAX_DEPTH), no allocation but the writer's allocator,
 * the output's and the arena given for keys. */
#ifndef TINYACTORS_CLI_JSON_H
#define TINYACTORS_CLI_JSON_H

#include <tinyactors/arena.h>
#include <tinyactors/cbor.h>

#include "base/output.h"

typedef enum ta_json_limits {
    TA_JSON_MAX_DEPTH = 64 /* containers open at once: TA_CBOR_DEPTH_MAX */
} ta_json_limits;

/* Where a text was refused, and why (a static text). */
typedef struct ta_json_failure {
    size_t offset;
    ta_string_view reason;
} ta_json_failure;

/* Reading, as RFC 8949, section 6.2 has it:
 *   - a number whose value is an integer from -2^64 to 2^64-1, written with
 *     or without a fraction or an exponent (7, 7.0, 7e0, 70e-1), is that
 *     integer; any other number is the nearest double, ties to even
 *     (cli/decimal.h), and a negative zero (-0, -0.0) is the float -0.0;
 *     a number whose magnitude rounds past the largest double is refused;
 *   - a string is text: UTF-8 checked, \u escapes joined in pairs, a lone
 *     surrogate refused, a control character refused unescaped;
 *   - arrays and objects keep their order; a key repeated in an object is
 *     refused;
 *   - true, false and null are themselves.
 * Whitespace may surround the one value; nothing else may. */
typedef struct ta_json_reading {
    ta_string_view text;      /* one JSON text, UTF-8 */
    ta_cbor_writer *writer;   /* receives one item; containers TA_CBOR_COUNT_LATER */
    ta_growable_arena *arena; /* the keys of the objects open, to refuse a repeated one, and
                               * strings with escapes as they read: kept until the caller
                               * releases them (the writer may grow through the same arena) */
    ta_json_failure failure;  /* set on TA_ERROR_INVALID and TA_ERROR_LIMIT */
} ta_json_reading;

/* TA_OK; TA_ERROR_INVALID: not JSON, not UTF-8, a lone surrogate, a key
 * repeated in an object, a number out of range; TA_ERROR_LIMIT: nested
 * deeper than TA_JSON_MAX_DEPTH; TA_ERROR_NO_MEMORY: the writer or the
 * arena refused. */
ta_status ta_json_read(ta_json_reading *reading);

/* What writing changed (6.1's substitutions). */
typedef enum ta_json_changes {
    TA_JSON_CHANGED_BYTES = 1u << 0,     /* bytes as base64url text */
    TA_JSON_CHANGED_UNDEFINED = 1u << 1, /* undefined, another simple value: null */
    TA_JSON_CHANGED_FLOAT = 1u << 2,     /* NaN, an infinity: null */
    TA_JSON_CHANGED_TAG = 1u << 3        /* a tag dropped: an address as its text */
} ta_json_changes;

/* Writing, as RFC 8949, section 6.1 has it, compact (no whitespace):
 * integers in decimal, exact over -2^64 … 2^64-1; a float as the shortest
 * decimal that reads back as it (ta_format_double: 1.0, -0.0, 1.0e+300);
 * text quoted (ta_format_quoted); bytes as base64url text without padding;
 * a tag's content without the tag; undefined and the other simple values
 * as null. A map whose key is not text is TA_ERROR_UNSUPPORTED: JSON's keys
 * are strings, and the value table's too. */
typedef struct ta_json_writing {
    ta_bytes cbor;    /* one item, read with ta_cbor_reader */
    ta_output *out;   /* compact JSON appended */
    uint32_t changes; /* ta_json_changes, set by the call */
} ta_json_writing;

/* TA_OK; TA_ERROR_INVALID and TA_ERROR_UNSUPPORTED as ta_cbor_read, and
 * TA_ERROR_UNSUPPORTED for a key that is not text; TA_ERROR_NO_MEMORY: the
 * output could not grow (its length is what the text needs). */
ta_status ta_json_write(ta_json_writing *writing);

/* Bytes written as 6.1 writes them, base64url text without padding (RFC
 * 4648, section 5), read back into out->pointer (out->length set): what a
 * value's CBOR twin, an image or a blob arrives as. At most
 * text.length / 4 * 3 + 2 bytes. TA_OK; TA_ERROR_INVALID: a character
 * outside the alphabet, padding, or a length no such text has (one
 * character past a multiple of four); TA_ERROR_NO_MEMORY: out->capacity is
 * too small (out->length the bytes it needs). */
ta_status ta_json_base64url_read(ta_string_view text, ta_buffer *out);

#endif /* TINYACTORS_CLI_JSON_H */

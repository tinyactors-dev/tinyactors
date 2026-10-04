#pragma once

#include "types.h"

/*
 * Package format is block D6 of the ring protocol's catalog: the small
 * formatters core uses to write numbers as text, in one place.
 *
 * What it is for. Core writes decimal numbers (send IDs, counters in error
 * messages), lowercase hex (trace and span IDs, revision fingerprints, \xHH
 * escapes), uppercase hex (percent-encoding in URIs), and reads a hex digit
 * back (percent-decoding). Each used to be written out locally, three times
 * for decimal and four for hex. This block is the one copy. It also owns
 * reading a hex digit, ta_hex_digit_value, so writer and reader live side
 * by side.
 *
 * Contract. Every function works on a buffer the caller supplies and sizes.
 * Nothing here writes a terminating NUL, formats a sign, or pads, except
 * where a function says so. The functions measure and write separately, so a
 * caller can measure, reserve exactly that many bytes, then fill them:
 *
 *     uint32_t digits = ta_dec_len(serial);
 *     char *text = ta_arena_alloc(&arena, prefix + digits, 1);
 *     if (text == NULL) { ... }             // the caller's failure path
 *     ta_dec_write(text + prefix, serial);
 *
 * Lifetime. Call. A function writes into the caller's buffer and keeps no
 * pointer after it returns. The destination decides how long the text
 * lives: a stack buffer (the call), the render arena (until the macrostep's
 * next render reset) or a definition arena (the definition's lifetime).
 *
 * Allocation. Never. There is no allocator parameter and no failure result;
 * the only allocation near these functions is the caller's, which keeps its
 * own safe point and its own failure path. Safe at any point, including
 * inside a macrostep.
 *
 * Invariants. A buffer shorter than the documented count is a caller bug,
 * not a runtime error; the documented upper bound for decimal is
 * TA_DEC_U64_DIGITS, and a caller that cannot know the value in advance
 * sizes a stack buffer by it. Output is deterministic and independent of the
 * locale. Hex output is lowercase unless the caller asks for uppercase.
 *
 * Complexity. O(digits) for decimal (a division per digit), O(count) for
 * hex, O(1) for the digit helpers.
 *
 * Not here. The traceparent readers in trace.c (ta_hex_read and friends) are
 * tuned word-at-a-time parsers for one fixed format (ADR 0073) and stay with
 * their user. The decimal parsers stay with the lexers that own their error
 * handling. ta_str_from_cstr, which turns a NUL-terminated literal into a
 * ta_str, is the block's third helper and lives in types.h beside the type
 * it builds.
 *
 * Example:
 *
 *     char digits[TA_DEC_U64_DIGITS];
 *     uint32_t count = ta_dec_write(digits, value);
 *     ta_text_put(writer, (ta_str){ .ptr = digits, .len = count });
 *
 *     char span[16];
 *     ta_hex_write(span, span_id, 16u);        // "00f067aa0ba902b7"
 */

/* TA_DEC_U64_DIGITS is the number of decimal digits of UINT64_MAX: the size of a buffer that holds any value ta_dec_write can write. */
#define TA_DEC_U64_DIGITS 20u

/*
 * ta_dec_len returns the number of decimal digits of value, 1 for 0. It lets
 * a caller measure, reserve exactly that many bytes, then fill with
 * ta_dec_write. Never allocates.
 */
uint32_t ta_dec_len(uint64_t value);

/*
 * ta_dec_write writes value in decimal at out and returns the number of
 * bytes written, which is exactly ta_dec_len(value) and at most
 * TA_DEC_U64_DIGITS. It writes no NUL and touches no byte past that count.
 * out must have room for it; out is only written during the call.
 */
uint32_t ta_dec_write(char *out, uint64_t value);

/*
 * ta_hex_digit returns the ASCII digit for a nibble, lowercase or, when
 * upper is true, uppercase. Only the low four bits of nibble are used, so a
 * caller may pass an unmasked byte shifted down.
 */
char ta_hex_digit(uint32_t nibble, bool upper);

/*
 * ta_hex_write writes the low count hex digits of value at out, most
 * significant first, lowercase, keeping leading zeros: exactly count bytes,
 * no NUL. count must be 1 to 16; out must have room for it.
 */
void ta_hex_write(char *out, uint64_t value, uint32_t count);

/*
 * ta_hex_digit_value returns the value 0 to 15 of a hex digit in either
 * case, or -1 for any other character. It is the inverse of ta_hex_digit
 * for the characters that function writes.
 */
int ta_hex_digit_value(char digit);

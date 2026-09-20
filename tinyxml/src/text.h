/* Private: byte-level text checks shared by the parser and the writer. */
#pragma once

#include "../include/txml.h"

/* Index of the first byte that is not well-formed UTF-8 or not an XML Char, or
 * `length` when there is none. A sequence cut off by the end counts as
 * invalid. */
uint32_t txml_text_first_invalid(const uint8_t *bytes, uint32_t length);

/*
 * The same check for input that arrives in pieces. `invalid` is the index of
 * the lead byte of the first bad character, or UINT32_MAX; `complete` is the
 * index just past the last whole character, which is less than `length` only
 * when the bytes end inside a sequence that could still turn out valid.
 */
typedef struct {
    uint32_t invalid;
    uint32_t complete;
} txml_text_scan;

txml_text_scan txml_text_scan_utf8(const uint8_t *bytes, uint32_t length);

/*
 * One character of well-formed UTF-8. `bytes` must start at a character
 * boundary with the whole character present, which the parser guarantees for
 * everything below `valid` and the writer by checking its input first. Returns
 * the code point and sets `width` to its length in bytes.
 */
uint32_t txml_text_decode(const uint8_t *bytes, uint32_t *width);

/* XML 1.0 (fifth edition) NameStartChar and NameChar, which include ':'. */
bool txml_text_is_name_start(uint32_t code);
bool txml_text_is_name_char(uint32_t code);

/*
 * Set `length` bytes to zero. The compiler calls memset behind the code's back
 * for a large struct initialiser, and can turn a plain byte loop into the same
 * call; either would make the library depend on the C library. This is the
 * loop, with the recognition that would undo it switched off where the compiler
 * offers a way to say so.
 */
void txml_text_zero(void *destination, size_t length);

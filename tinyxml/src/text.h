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

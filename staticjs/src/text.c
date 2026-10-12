/* UTF-8, and JavaScript's UTF-16 view of it. */
#include "internal.h"

int32_t sjs_utf8_next(sjs_str text, uint32_t *index) {
    uint32_t i = *index;
    const uint8_t *s = (const uint8_t *)text.ptr;
    uint8_t b = s[i];
    uint32_t point;
    uint32_t extra;
    uint32_t minimum;
    if (b < 0x80u) {
        *index = i + 1;
        return b;
    } else if ((b & 0xE0u) == 0xC0u) {
        point = b & 0x1Fu;
        extra = 1;
        minimum = 0x80u;
    } else if ((b & 0xF0u) == 0xE0u) {
        point = b & 0x0Fu;
        extra = 2;
        minimum = 0x800u;
    } else if ((b & 0xF8u) == 0xF0u) {
        point = b & 0x07u;
        extra = 3;
        minimum = 0x10000u;
    } else {
        return -1;
    }
    if (text.length - i <= extra) return -1;
    for (uint32_t k = 1; k <= extra; k++) {
        uint8_t c = s[i + k];
        if ((c & 0xC0u) != 0x80u) return -1;
        point = (point << 6) | (c & 0x3Fu);
    }
    if (point < minimum || point > 0x10FFFFu || (point >= 0xD800u && point <= 0xDFFFu)) return -1;
    *index = i + 1 + extra;
    return (int32_t)point;
}

bool sjs_utf8_valid(sjs_str text) {
    uint32_t i = 0;
    while (i < text.length) {
        if (sjs_utf8_next(text, &i) < 0) return false;
    }
    return true;
}

uint32_t sjs_utf8_encode(uint32_t point, uint8_t *out) {
    if (point < 0x80u) {
        out[0] = (uint8_t)point;
        return 1;
    }
    if (point < 0x800u) {
        out[0] = (uint8_t)(0xC0u | (point >> 6));
        out[1] = (uint8_t)(0x80u | (point & 0x3Fu));
        return 2;
    }
    if (point < 0x10000u) {
        out[0] = (uint8_t)(0xE0u | (point >> 12));
        out[1] = (uint8_t)(0x80u | ((point >> 6) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | (point & 0x3Fu));
        return 3;
    }
    out[0] = (uint8_t)(0xF0u | (point >> 18));
    out[1] = (uint8_t)(0x80u | ((point >> 12) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | ((point >> 6) & 0x3Fu));
    out[3] = (uint8_t)(0x80u | (point & 0x3Fu));
    return 4;
}

uint32_t sjs_utf16_length(sjs_str text) {
    uint32_t units = 0;
    for (uint32_t i = 0; i < text.length; i++) {
        uint8_t b = (uint8_t)text.ptr[i];
        if ((b & 0xC0u) != 0x80u) units += (b >= 0xF0u) ? 2u : 1u;
    }
    return units;
}

/* The UTF-16 code unit a code point starts with: comparing these, and the
 * code points when they are equal, orders strings as UTF-16 does. */
static uint32_t first_unit(uint32_t point) {
    return point < 0x10000u ? point : 0xD800u + ((point - 0x10000u) >> 10);
}

int32_t sjs_utf16_compare(sjs_str a, sjs_str b) {
    /* Equal bytes are equal code points: skip the bytes the two share, back
     * to the start of the code point where they part (valid UTF-8 parts
     * inside one in both or in neither), and compare that one. ASCII, or
     * any text, costs a byte comparison per shared byte. */
    uint32_t n = a.length < b.length ? a.length : b.length;
    uint32_t p = 0;
    while (p < n && a.ptr[p] == b.ptr[p]) p++;
    if (p == n) return a.length == b.length ? 0 : a.length < b.length ? -1 : 1;
    uint8_t x = (uint8_t)a.ptr[p];
    uint8_t y = (uint8_t)b.ptr[p];
    if (x < 0x80u && y < 0x80u) return x < y ? -1 : 1;
    while (p > 0 && ((uint8_t)a.ptr[p] & 0xC0u) == 0x80u) p--;
    uint32_t i = p;
    uint32_t j = p;
    uint32_t pa = (uint32_t)sjs_utf8_next(a, &i);
    uint32_t pb = (uint32_t)sjs_utf8_next(b, &j);
    uint32_t ua = first_unit(pa);
    uint32_t ub = first_unit(pb);
    if (ua != ub) return ua < ub ? -1 : 1;
    /* Two supplementary code points with the same high surrogate. */
    return pa < pb ? -1 : 1;
}

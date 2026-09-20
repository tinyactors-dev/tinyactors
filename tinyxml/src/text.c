/* The byte comparisons txml needs without a C library. */
#include "text.h"

static bool bytes_equal(const char *left, const char *right, uint32_t length) {
    for (uint32_t i = 0u; i < length; i++) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

bool txml_str_equal(txml_str left, txml_str right) {
    return left.length == right.length && bytes_equal(left.ptr, right.ptr, left.length);
}

bool txml_qname_equal(const txml_qname *left, const txml_qname *right) {
    return txml_str_equal(left->ns, right->ns) && txml_str_equal(left->local, right->local);
}

bool txml_str_starts_with(txml_str text, txml_str prefix) {
    return prefix.length <= text.length && bytes_equal(text.ptr, prefix.ptr, prefix.length);
}

txml_text_scan txml_text_scan_utf8(const uint8_t *s, uint32_t length) {
    txml_text_scan result = { UINT32_MAX, length };
    uint32_t i = 0u;

    while (i < length) {
        const uint8_t b = s[i];
        uint32_t need = 0u;
        uint8_t low = 0x80u, high = 0xBFu;

        if (b < 0x80u) {
            if (b < 0x20u && b != '\t' && b != '\n' && b != '\r') {
                result.invalid = i;
                result.complete = i;
                return result;
            }
            i += 1u;
            continue;
        }
        if (b >= 0xC2u && b <= 0xDFu) {
            need = 1u;
        } else if (b >= 0xE0u && b <= 0xEFu) {
            need = 2u;
            if (b == 0xE0u) {
                low = 0xA0u;
            } else if (b == 0xEDu) {
                high = 0x9Fu;
            }
        } else if (b >= 0xF0u && b <= 0xF4u) {
            need = 3u;
            if (b == 0xF0u) {
                low = 0x90u;
            } else if (b == 0xF4u) {
                high = 0x8Fu;
            }
        } else {
            result.invalid = i;
            result.complete = i;
            return result;
        }

        /* Check the continuation bytes that are here; a short tail is
         * incomplete rather than invalid. */
        const uint32_t have = length - i - 1u < need ? length - i - 1u : need;
        for (uint32_t k = 1u; k <= have; k++) {
            const uint8_t c = s[i + k];
            const bool ok = k == 1u ? (c >= low && c <= high) : (c & 0xC0u) == 0x80u;
            if (!ok) {
                result.invalid = i;
                result.complete = i;
                return result;
            }
        }
        if (have < need) {
            result.complete = i;
            return result;
        }
        if (b == 0xEFu && s[i + 1u] == 0xBFu && (s[i + 2u] == 0xBEu || s[i + 2u] == 0xBFu)) {
            result.invalid = i; /* U+FFFE and U+FFFF */
            result.complete = i;
            return result;
        }
        i += need + 1u;
    }
    return result;
}

uint32_t txml_text_first_invalid(const uint8_t *s, uint32_t length) {
    const txml_text_scan scan = txml_text_scan_utf8(s, length);
    return scan.invalid != UINT32_MAX ? scan.invalid : scan.complete;
}

uint32_t txml_text_decode(const uint8_t *s, uint32_t *width) {
    const uint8_t b = s[0];

    if (b < 0x80u) {
        *width = 1u;
        return b;
    }
    if (b < 0xE0u) {
        *width = 2u;
        return ((uint32_t)(b & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
    }
    if (b < 0xF0u) {
        *width = 3u;
        return ((uint32_t)(b & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) | (uint32_t)(s[2] & 0x3Fu);
    }
    *width = 4u;
    return ((uint32_t)(b & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) | ((uint32_t)(s[2] & 0x3Fu) << 6) |
           (uint32_t)(s[3] & 0x3Fu);
}

static bool in(uint32_t code, uint32_t low, uint32_t high) {
    return code >= low && code <= high;
}

bool txml_text_is_name_start(uint32_t c) {
    if (c < 0x80u) {
        return in(c, 'a', 'z') || in(c, 'A', 'Z') || c == '_' || c == ':';
    }
    return in(c, 0xC0u, 0xD6u) || in(c, 0xD8u, 0xF6u) || in(c, 0xF8u, 0x2FFu) || in(c, 0x370u, 0x37Du) ||
           in(c, 0x37Fu, 0x1FFFu) || in(c, 0x200Cu, 0x200Du) || in(c, 0x2070u, 0x218Fu) || in(c, 0x2C00u, 0x2FEFu) ||
           in(c, 0x3001u, 0xD7FFu) || in(c, 0xF900u, 0xFDCFu) || in(c, 0xFDF0u, 0xFFFDu) || in(c, 0x10000u, 0xEFFFFu);
}

bool txml_text_is_name_char(uint32_t c) {
    if (c < 0x80u) {
        return txml_text_is_name_start(c) || in(c, '0', '9') || c == '-' || c == '.';
    }
    return txml_text_is_name_start(c) || c == 0xB7u || in(c, 0x300u, 0x36Fu) || in(c, 0x203Fu, 0x2040u);
}

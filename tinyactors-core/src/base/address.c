/* address.c — tinyactors addresses (see include/tinyactors/address.h). Freestanding. */
#include <tinyactors/address.h>

#include "base/format.h"

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}
static int is_lower_alnum(char c) {
    return (c >= 'a' && c <= 'z') || is_digit(c);
}
static int is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int hex_value(char c) {
    return is_digit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
}
static int is_unreserved(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '-' ||
           c == '.' || c == '_' || c == '~';
}

int ta_address_system_valid(ta_string_view s) {
    if (!s.pointer || s.length == 0 || s.length > TA_ADDRESS_SYSTEM_MAX) return 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= s.length; i++) {
        if (i < s.length && s.pointer[i] != '.') {
            char c = s.pointer[i];
            if (!is_lower_alnum(c) && c != '-') return 0;
            continue;
        }
        uint32_t n = i - start; /* one label */
        if (n == 0 || n > 63 || s.pointer[start] == '-' || s.pointer[i - 1] == '-') return 0;
        start = i + 1;
    }
    return 1;
}

static int prefix(ta_string_view s, const char *p, uint32_t n) {
    if (s.length < n) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (s.pointer[i] != p[i]) return 0;
    return 1;
}

ta_status ta_address_parse_local(ta_string_view l, ta_address *out) {
    out->kind = 0;
    out->pad = 0;
    out->system = (ta_string_view){(const char *)0, 0};
    out->local = l;
    out->name = (ta_string_view){(const char *)0, 0};
    out->session = 0;
    if (!l.pointer) return TA_ERROR_INVALID;
    if (prefix(l, "#_scxml_", 8)) {
        uint32_t n = l.length - 8;
        const char *d = l.pointer + 8;
        if (n == 0 || n > 20 || (d[0] == '0')) return TA_ERROR_INVALID;
        uint64_t v = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!is_digit(d[i])) return TA_ERROR_INVALID;
            uint64_t dv = (uint64_t)(d[i] - '0');
            if (v > (UINT64_MAX - dv) / 10) return TA_ERROR_INVALID; /* above 2^64-1 */
            v = v * 10 + dv;
        }
        out->kind = TA_ADDRESS_SESSION;
        out->session = v;
        return TA_OK;
    }
    if (l.length >= 2 && l.pointer[0] == '@') {
        for (uint32_t i = 1; i < l.length; i++) {
            char c = l.pointer[i];
            if (c == '%') {
                if (l.length - i < 3) return TA_ERROR_INVALID; /* "%" and two hex digits */
                if (!is_hex(l.pointer[i + 1]) || !is_hex(l.pointer[i + 2])) return TA_ERROR_INVALID;
                i += 2;
            } else if (!is_unreserved(c)) {
                return TA_ERROR_INVALID;
            }
        }
        out->kind = TA_ADDRESS_NAME;
        out->name = (ta_string_view){l.pointer + 1, l.length - 1};
        return TA_OK;
    }
    return TA_ERROR_INVALID;
}

ta_status ta_address_parse(ta_string_view t, ta_address *out) {
    if (!t.pointer || !prefix(t, TA_ADDRESS_SCHEME, 13)) {
        ta_address_parse_local((ta_string_view){(const char *)0, 0}, out);
        return TA_ERROR_INVALID;
    }
    uint32_t i = 13;
    while (i < t.length && t.pointer[i] != '/')
        i++;
    ta_string_view system = {t.pointer + 13, i - 13};
    if (i == t.length || !ta_address_system_valid(system)) {
        ta_address_parse_local((ta_string_view){(const char *)0, 0}, out);
        return TA_ERROR_INVALID;
    }
    ta_status status =
        ta_address_parse_local((ta_string_view){t.pointer + i + 1, t.length - i - 1}, out);
    out->system = system;
    return status;
}

static uint32_t put(char *buffer, uint32_t capacity, uint32_t at, const char *s, uint32_t n) {
    if (at > capacity || n > capacity - at) return UINT32_MAX;
    for (uint32_t i = 0; i < n; i++)
        buffer[at + i] = s[i];
    return at + n;
}

uint32_t ta_address_format(ta_string_view system, uint64_t session, char *buffer,
                           uint32_t capacity) {
    if (!session || !ta_address_system_valid(system)) return 0;
    char digits[TA_FORMAT_U64_MAX];
    uint32_t nd = ta_format_u64(digits, session);
    uint32_t at = put(buffer, capacity, 0, TA_ADDRESS_SCHEME, 13);
    if (at != UINT32_MAX) at = put(buffer, capacity, at, system.pointer, system.length);
    if (at != UINT32_MAX) at = put(buffer, capacity, at, "/#_scxml_", 9);
    if (at != UINT32_MAX) at = put(buffer, capacity, at, digits, nd);
    return at == UINT32_MAX ? 0 : at;
}

uint32_t ta_address_format_name(ta_string_view system, ta_string_view name, char *buffer,
                                uint32_t capacity) {
    if (!ta_address_system_valid(system) || !name.pointer || !name.length) return 0;
    static const char hexd[] = "0123456789ABCDEF";
    uint32_t at = put(buffer, capacity, 0, TA_ADDRESS_SCHEME, 13);
    if (at != UINT32_MAX) at = put(buffer, capacity, at, system.pointer, system.length);
    if (at != UINT32_MAX) at = put(buffer, capacity, at, "/@", 2);
    for (uint32_t i = 0; i < name.length && at != UINT32_MAX; i++) {
        char c = name.pointer[i];
        if (is_unreserved(c)) {
            at = put(buffer, capacity, at, &c, 1);
        } else {
            unsigned char u = (unsigned char)c;
            char e[3] = {'%', hexd[u >> 4], hexd[u & 15]};
            at = put(buffer, capacity, at, e, 3);
        }
    }
    return at == UINT32_MAX ? 0 : at;
}

uint32_t ta_address_name_decode(ta_string_view name, char *buffer, uint32_t capacity) {
    uint32_t at = 0;
    for (uint32_t i = 0; i < name.length; i++) {
        char c = name.pointer[i];
        if (c == '%' && name.length - i >= 3 && is_hex(name.pointer[i + 1]) &&
            is_hex(name.pointer[i + 2])) {
            c = (char)(hex_value(name.pointer[i + 1]) * 16 + hex_value(name.pointer[i + 2]));
            i += 2;
        }
        if (at >= capacity) return 0;
        buffer[at++] = c;
    }
    return at;
}

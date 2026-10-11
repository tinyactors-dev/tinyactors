/* json.c — reading and writing JSON text in place (base/json.h). Freestanding. */
#include "base/json.h"

#include "base/memory.h"

static int json_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

char ta_json_peek(ta_string_view text, uint32_t *at) {
    while (*at < text.length && json_space(text.pointer[*at]))
        (*at)++;
    if (*at >= text.length) return 0;
    char c = text.pointer[*at];
    return c == '-' || (c >= '0' && c <= '9') ? '0' : c;
}

/* past the string whose quote is at *at */
static int json_skip_string(ta_string_view text, uint32_t *at) {
    uint32_t i = *at + 1;
    while (i < text.length && text.pointer[i] != '"') {
        if ((unsigned char)text.pointer[i] < 32) return 0;
        i += text.pointer[i] == '\\' ? 2 : 1;
    }
    if (i >= text.length) return 0;
    *at = i + 1;
    return 1;
}

static int json_skip_word(ta_string_view text, uint32_t *at, const char *word, uint32_t n) {
    if (text.length - *at < n || ta_memcmp(text.pointer + *at, word, n) != 0) return 0;
    *at += n;
    return 1;
}

static int json_skip_number(ta_string_view text, uint32_t *at) {
    uint32_t i = *at, digits = 0;
    if (i < text.length && text.pointer[i] == '-') i++;
    for (; i < text.length; i++) {
        char c = text.pointer[i];
        if (c >= '0' && c <= '9')
            digits++;
        else if (!(c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-'))
            break;
    }
    if (!digits) return 0;
    *at = i;
    return 1;
}

int ta_json_skip(ta_string_view text, uint32_t *at, ta_string_view *value) {
    char c = ta_json_peek(text, at);
    uint32_t start = *at;
    int ok;
    if (c == '"')
        ok = json_skip_string(text, at);
    else if (c == 't')
        ok = json_skip_word(text, at, "true", 4);
    else if (c == 'f')
        ok = json_skip_word(text, at, "false", 5);
    else if (c == 'n')
        ok = json_skip_word(text, at, "null", 4);
    else if (c == '0')
        ok = json_skip_number(text, at);
    else if (c == '{' || c == '[') { /* brackets counted, strings skipped: no recursion */
        uint32_t depth = 0, i = *at;
        ok = 0;
        while (i < text.length) {
            char d = text.pointer[i];
            if (d == '"') {
                if (!json_skip_string(text, &i)) break;
                continue;
            }
            if (d == '{' || d == '[')
                depth++;
            else if (d == '}' || d == ']') {
                if (depth == 0) break;
                if (--depth == 0) {
                    ok = 1;
                    i++;
                    break;
                }
            }
            i++;
        }
        if (ok) *at = i;
    } else {
        ok = 0;
    }
    if (ok && value) *value = (ta_string_view){text.pointer + start, *at - start};
    return ok;
}

/* the next item of a container (`close` is its bracket): 1 with *at at it, 0 at the end */
static int json_next_item(ta_string_view text, uint32_t *at, int first, char open, char close) {
    char c = ta_json_peek(text, at);
    if (first) {
        if (c != open) return -1;
        (*at)++;
        c = ta_json_peek(text, at);
        if (c == close) {
            (*at)++;
            return 0;
        }
        return 1;
    }
    if (c == close) {
        (*at)++;
        return 0;
    }
    if (c != ',') return -1;
    (*at)++;
    return 1;
}

int ta_json_member(ta_string_view text, uint32_t *at, int first, ta_string_view *key,
                   ta_string_view *value) {
    int more = json_next_item(text, at, first, '{', '}');
    if (more <= 0) return more;
    ta_string_view quoted;
    if (ta_json_peek(text, at) != '"' || !ta_json_skip(text, at, &quoted)) return -1;
    *key = (ta_string_view){quoted.pointer + 1, quoted.length - 2};
    if (ta_json_peek(text, at) != ':') return -1;
    (*at)++;
    return ta_json_skip(text, at, value) ? 1 : -1;
}

int ta_json_element(ta_string_view text, uint32_t *at, int first, ta_string_view *value) {
    int more = json_next_item(text, at, first, '[', ']');
    if (more <= 0) return more;
    return ta_json_skip(text, at, value) ? 1 : -1;
}

static int json_hex4(const char *p, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        uint32_t d = c >= '0' && c <= '9'   ? (uint32_t)(c - '0')
                     : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10)
                     : c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10)
                                            : 16u;
        if (d == 16) return 0;
        v = v << 4 | d;
    }
    *out = v;
    return 1;
}

int32_t ta_json_string(ta_string_view value, char *out, uint32_t capacity) {
    if (value.length < 2 || value.pointer[0] != '"' || value.pointer[value.length - 1] != '"')
        return -1;
    uint32_t n = 0, end = value.length - 1;
    for (uint32_t i = 1; i < end;) {
        char c = value.pointer[i++];
        uint32_t code;
        if (c != '\\') {
            if (n >= capacity) return -1;
            out[n++] = c;
            continue;
        }
        if (i >= end) return -1;
        c = value.pointer[i++];
        switch (c) {
        case 'b':
            code = 8;
            break;
        case 'f':
            code = 12;
            break;
        case 'n':
            code = 10;
            break;
        case 'r':
            code = 13;
            break;
        case 't':
            code = 9;
            break;
        case 'u':
            if (end - i < 4 || !json_hex4(value.pointer + i, &code)) return -1;
            i += 4;
            if (code >= 0xD800 && code < 0xDC00 && end - i >= 6 && value.pointer[i] == '\\' &&
                value.pointer[i + 1] == 'u') { /* a surrogate pair */
                uint32_t low;
                if (json_hex4(value.pointer + i + 2, &low) && low >= 0xDC00 && low < 0xE000) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    i += 6;
                }
            }
            break;
        default:
            code = (unsigned char)c;
            break; /* " \ / */
        }
        uint32_t bytes = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
        if (capacity - n < bytes) return -1;
        if (bytes == 1)
            out[n++] = (char)code;
        else if (bytes == 2) {
            out[n++] = (char)(0xC0 | code >> 6);
            out[n++] = (char)(0x80 | (code & 63));
        } else if (bytes == 3) {
            out[n++] = (char)(0xE0 | code >> 12);
            out[n++] = (char)(0x80 | (code >> 6 & 63));
            out[n++] = (char)(0x80 | (code & 63));
        } else {
            out[n++] = (char)(0xF0 | code >> 18);
            out[n++] = (char)(0x80 | (code >> 12 & 63));
            out[n++] = (char)(0x80 | (code >> 6 & 63));
            out[n++] = (char)(0x80 | (code & 63));
        }
    }
    return (int32_t)n;
}

int ta_json_integer(ta_string_view value, uint64_t *out) {
    uint64_t v = 0;
    uint32_t i = 0;
    while (i < value.length && value.pointer[i] >= '0' && value.pointer[i] <= '9')
        v = v * 10 + (uint64_t)(value.pointer[i++] - '0');
    if (!i) return 0;
    *out = v;
    return 1;
}

size_t ta_json_write_string(char *out, ta_string_view s) {
    size_t n = 0;
    if (out) out[n] = '"';
    n++;
    const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < s.length; i++) {
        unsigned char c = (unsigned char)s.pointer[i];
        if (c < 32) {
            if (out) {
                ta_memcpy(out + n, "\\u00", 4);
                out[n + 4] = hex[c >> 4];
                out[n + 5] = hex[c & 15];
            }
            n += 6;
        } else {
            if (c == '"' || c == '\\') {
                if (out) out[n] = '\\';
                n++;
            }
            if (out) out[n] = (char)c;
            n++;
        }
    }
    if (out) out[n] = '"';
    return n + 1;
}

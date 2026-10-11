/* json.c — JSON strings for the state value (base/json.h). Freestanding. */
#include "base/json.h"

#include "base/memory.h"

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

/* format.c — decimal numbers and bounded text (base/format.h). */
#include "base/format.h"

#include "base/memory.h"

uint32_t ta_format_u64(char *buffer, uint64_t value) {
    char reversed[TA_FORMAT_U64_MAX];
    uint32_t n = 0;
    do {
        reversed[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value);
    for (uint32_t i = 0; i < n; i++)
        buffer[i] = reversed[n - 1 - i];
    return n;
}

void ta_text_append(ta_text *text, ta_string_view view) {
    if (text->overflow || view.length > text->capacity - text->length) {
        text->overflow = 1;
        return;
    }
    if (view.length) ta_memcpy(text->pointer + text->length, view.pointer, view.length);
    text->length += view.length;
}

void ta_text_append_u64(ta_text *text, uint64_t value) {
    char digits[TA_FORMAT_U64_MAX];
    uint32_t n = ta_format_u64(digits, value);
    ta_text_append(text, (ta_string_view){digits, n});
}

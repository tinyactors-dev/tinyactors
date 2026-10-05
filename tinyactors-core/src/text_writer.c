#include "text_writer.h"
#include "memory.h"
#include "../include/tinyactors.h"

void ta_text_put(ta_text_writer *writer, ta_str text) {
    /* The text's bytes [length, length + len) that fall in the window
     * [skip, skip + capacity) are stored, in one copy. */
    uint64_t first = writer->length;
    uint64_t end = first + text.len;
    uint64_t window = writer->skip;
    uint64_t window_end = window + writer->capacity;
    uint64_t from = first > window ? first : window;
    uint64_t to = end < window_end ? end : window_end;
    if (writer->buffer != NULL && from < to) {
        char *out = writer->buffer + (from - window);
        const char *in = text.ptr + (from - first);
        uint32_t count = (uint32_t)(to - from);
        for (uint32_t index = 0; index < count; index += 1u) {
            out[index] = in[index];
        }
    }
    writer->length += text.len;
}

uint32_t ta_text_format_u64(char out[TA_TEXT_U64_DIGITS], uint64_t value) {
    char reversed[TA_TEXT_U64_DIGITS];
    uint32_t count = 0;
    do {
        reversed[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    for (uint32_t index = 0; index < count; index += 1) {
        out[index] = reversed[count - 1u - index];
    }
    return count;
}

void ta_text_format_hex(char *out, uint64_t value, uint32_t count) {
    static const char digits[] = "0123456789abcdef";
    for (uint32_t index = count; index > 0; index -= 1) {
        out[index - 1u] = digits[value & 0xfu];
        value >>= 4;
    }
}

void ta_text_put_u64(ta_text_writer *writer, uint64_t value) {
    char digits[TA_TEXT_U64_DIGITS];
    uint32_t count = ta_text_format_u64(digits, value);
    ta_text_put(writer, (ta_str){ .ptr = digits, .len = count });
}

ta_result ta_text_finish(ta_text_writer *writer, uint32_t *needed) {
    *needed = writer->length + 1u;
    if (writer->buffer == NULL || writer->capacity < writer->length + 1u) {
        return TA_ERR_NOMEM;
    }
    writer->buffer[writer->length] = '\0';
    return TA_OK;
}

ta_result ta_text_render(ta_allocator allocator, const ta_text_render_params *params) {
    ta_text_writer measure = {0};
    if (!params->write(params->context, &measure)) {
        return TA_ERR_NOT_FOUND;
    }
    char *buffer = ta_mem_alloc(allocator, (size_t)measure.length + 1u, 1);
    if (buffer == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_text_writer writer = { .buffer = buffer, .capacity = measure.length + 1u };
    (void)params->write(params->context, &writer);
    buffer[measure.length] = '\0';
    *params->out = (ta_str){ .ptr = buffer, .len = measure.length };
    return TA_OK;
}

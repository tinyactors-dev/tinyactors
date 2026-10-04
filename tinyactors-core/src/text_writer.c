#include "text_writer.h"
#include "memory.h"
#include "../include/tinyactors.h"

void ta_text_put(ta_text_writer *writer, ta_str text) {
    for (uint32_t index = 0; index < text.len; index += 1) {
        if (writer->buffer != NULL && writer->length >= writer->skip &&
            writer->length - writer->skip < writer->capacity) {
            writer->buffer[writer->length - writer->skip] = text.ptr[index];
        }
        writer->length += 1;
    }
}

void ta_text_put_u64(ta_text_writer *writer, uint64_t value) {
    char digits[20];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    char ordered[20];
    for (uint32_t index = 0; index < count; index += 1) {
        ordered[index] = digits[count - 1u - index];
    }
    ta_text_put(writer, (ta_str){ .ptr = ordered, .len = count });
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

#include "types.h"
#include "format.h"

uint32_t ta_version(void) {
    return TA_VERSION;
}

void *ta_mem_alloc(ta_allocator allocator, size_t size, size_t alignment) {
    if (allocator.v == NULL || allocator.v->alloc == NULL || size == 0) {
        return NULL;
    }
    return allocator.v->alloc(allocator.self, size, alignment);
}

void ta_mem_free(ta_allocator allocator, void *ptr, const ta_allocator_free_params *layout) {
    if (ptr == NULL || allocator.v == NULL || allocator.v->free == NULL) {
        return;
    }
    ta_allocator_free_params params = *layout;
    params.ptr = ptr;
    allocator.v->free(allocator.self, &params);
}

void ta_mem_copy(void *dst, const void *src, size_t size) {
    unsigned char *out = dst;
    const unsigned char *in = src;
    for (size_t index = 0; index < size; index += 1) {
        out[index] = in[index];
    }
}

void ta_mem_zero(void *dst, size_t size) {
    unsigned char *out = dst;
    for (size_t index = 0; index < size; index += 1) {
        out[index] = 0;
    }
}

bool ta_str_equal(ta_str left, ta_str right) {
    if (left.len != right.len) {
        return false;
    }
    for (uint32_t index = 0; index < left.len; index += 1) {
        if (left.ptr[index] != right.ptr[index]) {
            return false;
        }
    }
    return true;
}

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
    char digits[TA_DEC_U64_DIGITS];
    uint32_t count = ta_dec_write(digits, value);
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

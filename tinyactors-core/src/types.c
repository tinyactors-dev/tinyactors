#include "types.h"

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

void ta_vec_init(ta_vec *vec, ta_allocator allocator, const ta_tree_list_layout *layout) {
    *vec = (ta_vec){
        .allocator = allocator,
        .element_size = layout->element_size,
        .element_alignment = layout->element_alignment,
    };
}

void ta_vec_destroy(ta_vec *vec) {
    if (vec->items != NULL) {
        ta_mem_free(vec->allocator, vec->items,
                    &(ta_allocator_free_params){
                        .size = vec->capacity * vec->element_size,
                        .alignment = vec->element_alignment,
                    });
    }
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

ta_result ta_vec_reserve(ta_vec *vec, uint32_t count) {
    if (count <= vec->capacity) {
        return TA_OK;
    }
    uint32_t capacity = vec->capacity == 0 ? 8u : vec->capacity;
    while (capacity < count) {
        if (capacity > UINT32_MAX / 2u) {
            return TA_ERR_NOMEM;
        }
        capacity *= 2u;
    }
    void *items = ta_mem_alloc(vec->allocator, capacity * vec->element_size, vec->element_alignment);
    if (items == NULL) {
        return TA_ERR_NOMEM;
    }
    if (vec->items != NULL) {
        ta_mem_copy(items, vec->items, vec->count * vec->element_size);
        ta_mem_free(vec->allocator, vec->items,
                    &(ta_allocator_free_params){
                        .size = vec->capacity * vec->element_size,
                        .alignment = vec->element_alignment,
                    });
    }
    vec->items = items;
    vec->capacity = capacity;
    return TA_OK;
}

ta_result ta_vec_extend(ta_vec *vec, uint32_t count, uint32_t *out) {
    if (count > UINT32_MAX - vec->count) {
        return TA_ERR_NOMEM;
    }
    ta_result result = ta_vec_reserve(vec, vec->count + count);
    if (result != TA_OK) {
        return result;
    }
    if (out != NULL) {
        *out = vec->count;
    }
    if (count != 0) {
        ta_mem_zero((unsigned char *)vec->items + vec->count * vec->element_size,
                    count * vec->element_size);
    }
    vec->count += count;
    return TA_OK;
}

ta_result ta_vec_push(ta_vec *vec, const void *element) {
    uint32_t index = 0;
    ta_result result = ta_vec_extend(vec, 1, &index);
    if (result == TA_OK) {
        ta_mem_copy(ta_vec_at(vec, index), element, vec->element_size);
    }
    return result;
}

void *ta_vec_at(const ta_vec *vec, uint32_t index) {
    return (unsigned char *)vec->items + (size_t)index * vec->element_size;
}

void ta_text_put(ta_text_writer *writer, ta_str text) {
    for (uint32_t index = 0; index < text.len; index += 1) {
        if (writer->buffer != NULL && writer->length < writer->capacity) {
            writer->buffer[writer->length] = text.ptr[index];
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

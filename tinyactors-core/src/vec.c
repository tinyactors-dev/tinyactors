#include "vec.h"

/* The package comment is in vec.h. */

size_t ta_vec_bytes(const ta_vec *vec) {
    /* ta_vec_reserve checked this product before it allocated the block. */
    return (size_t)vec->capacity * vec->element_size;
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
                        .size = ta_vec_bytes(vec),
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
    size_t bytes = 0;
    if (!ta_checked_mul(capacity, vec->element_size, &bytes)) {
        return TA_ERR_NOMEM;
    }
    void *items = ta_mem_alloc(vec->allocator, bytes, vec->element_alignment);
    if (items == NULL) {
        return TA_ERR_NOMEM;
    }
    if (vec->items != NULL) {
        /* count <= old capacity, whose product was checked when it was allocated. */
        ta_mem_copy(items, vec->items, (size_t)vec->count * vec->element_size);
        ta_mem_free(vec->allocator, vec->items,
                    &(ta_allocator_free_params){
                        .size = ta_vec_bytes(vec),
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
        ta_mem_zero((unsigned char *)vec->items + (size_t)vec->count * vec->element_size,
                    (size_t)count * vec->element_size);
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

/*
 * Where column `column` goes in a block of `capacity` items, and how big and
 * how aligned the whole block is. The rest of the table is placed too,
 * because the total needs every column. It never allocates, and answers
 * TA_ERR_NOMEM, writing nothing, when a size does not fit.
 */
typedef struct {
    uint32_t capacity;
    uint32_t column;
} ta_columns_query;

typedef struct {
    size_t offset;
    size_t bytes;
    size_t alignment;
} ta_columns_placement;

static ta_result ta_columns_place(const ta_columns_layout *layout, ta_columns_query query,
                                  ta_columns_placement *out) {
    ta_layout block;
    ta_layout_init(&block, 0);
    size_t offset = 0;
    for (uint32_t index = 0; index < layout->column_count; index += 1u) {
        const ta_column *column = &layout->columns[index];
        size_t at = ta_layout_add(&block, &(ta_layout_entry){ .size = column->size,
                                                             .alignment = column->alignment,
                                                             .count = query.capacity });
        offset = index == query.column ? at : offset;
    }
    size_t bytes = 0;
    size_t alignment = 0;
    ta_result result = ta_layout_finish(&block, &bytes, &alignment);
    if (result == TA_OK) {
        *out = (ta_columns_placement){ .offset = offset, .bytes = bytes, .alignment = alignment };
    }
    return result;
}

size_t ta_columns_bytes(const ta_columns_layout *layout, const ta_columns *table) {
    ta_columns_placement placement = { 0 };
    if (table->block == NULL ||
        ta_columns_place(layout, (ta_columns_query){ .capacity = table->capacity }, &placement) != TA_OK) {
        return 0;
    }
    return placement.bytes;
}

void *ta_columns_at(const ta_columns_layout *layout, const ta_columns *table, uint32_t index) {
    ta_columns_placement placement = { 0 };
    if (table->block == NULL || index >= layout->column_count ||
        ta_columns_place(layout, (ta_columns_query){ .capacity = table->capacity, .column = index },
                         &placement) != TA_OK) {
        return NULL;
    }
    return (unsigned char *)table->block + placement.offset;
}

void ta_columns_release(ta_allocator allocator, const ta_columns_layout *layout, ta_columns *table) {
    ta_columns_placement placement = { 0 };
    /* A block exists only because this same placement fit when it was allocated. */
    if (table->block != NULL &&
        ta_columns_place(layout, (ta_columns_query){ .capacity = table->capacity }, &placement) == TA_OK) {
        ta_mem_free(allocator, table->block,
                    &(ta_allocator_free_params){ .size = placement.bytes, .alignment = placement.alignment });
    }
    *table = (ta_columns){ 0 };
}

ta_result ta_columns_grow(ta_allocator allocator, const ta_columns_layout *layout, ta_columns *table) {
    if (table->count < table->capacity) {
        return TA_OK;
    }
    uint32_t capacity = layout->first_capacity;
    if (table->capacity != 0) {
        if (table->capacity > UINT32_MAX / 2u) {
            return TA_ERR_NOMEM;
        }
        capacity = table->capacity * 2u;
    }
    ta_columns_placement next = { 0 };
    if (ta_columns_place(layout, (ta_columns_query){ .capacity = capacity }, &next) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    unsigned char *block = ta_mem_alloc(allocator, next.bytes, next.alignment);
    if (block == NULL) {
        return TA_ERR_NOMEM;
    }
    /* From here nothing fails: the old block's placement fit when it was made. */
    for (uint32_t index = 0; table->block != NULL && index < layout->column_count; index += 1u) {
        ta_columns_placement from = { 0 };
        ta_columns_placement to = { 0 };
        (void)ta_columns_place(layout, (ta_columns_query){ .capacity = table->capacity, .column = index }, &from);
        (void)ta_columns_place(layout, (ta_columns_query){ .capacity = capacity, .column = index }, &to);
        ta_mem_copy(block + to.offset, (unsigned char *)table->block + from.offset,
                    (size_t)table->count * layout->columns[index].size);
    }
    uint32_t count = table->count;
    ta_columns_release(allocator, layout, table);
    *table = (ta_columns){ .block = block, .count = count, .capacity = capacity };
    return TA_OK;
}

#include "layout.h"
#include "../include/tinyactors.h"

void ta_layout_init(ta_layout *layout) {
    *layout = (ta_layout){ .bytes = 0, .alignment = 1 };
}

ta_result ta_layout_add(ta_layout *layout, const ta_layout_entry *entry, size_t *offset) {
    if (entry->alignment == 0) {
        return TA_ERR_INVALID;
    }
    if (entry->size != 0 && entry->count > SIZE_MAX / entry->size) {
        return TA_ERR_NOMEM;
    }
    size_t span = entry->size * entry->count;
    size_t remainder = layout->bytes % entry->alignment;
    size_t padding = remainder == 0 ? 0 : entry->alignment - remainder;
    if (padding > SIZE_MAX - layout->bytes) {
        return TA_ERR_NOMEM;
    }
    size_t start = layout->bytes + padding;
    if (span > SIZE_MAX - start) {
        return TA_ERR_NOMEM;
    }
    layout->bytes = start + span;
    if (entry->alignment > layout->alignment) {
        layout->alignment = entry->alignment;
    }
    if (offset != NULL) {
        *offset = start;
    }
    return TA_OK;
}

#include "layout.h"

/*
 * The implementation of package layout (see layout.h for the contract).
 * Everything here is arithmetic on size_t values passed in; nothing is
 * allocated, read from memory or kept.
 */

bool ta_checked_mul(size_t a, size_t b, size_t *out) {
    /* Dividing, not multiplying and comparing: the division cannot wrap. */
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    *out = a * b;
    return true;
}

bool ta_checked_add(size_t a, size_t b, size_t *out) {
    if (b > SIZE_MAX - a) {
        return false;
    }
    *out = a + b;
    return true;
}

void ta_layout_init(ta_layout *layout, size_t start) {
    *layout = (ta_layout){ .internal_end = start, .internal_alignment = 1u, .internal_overflowed = false };
}

size_t ta_layout_add(ta_layout *layout, const ta_layout_entry *entry) {
    size_t offset = 0;
    size_t bytes = 0;
    size_t end = 0;
    if (layout->internal_overflowed) {
        return 0;
    }
    if (entry->alignment == 0) {
        layout->internal_overflowed = true;
        return 0;
    }
    /* Round the end up by the gap to the next multiple, adding the gap
     * rather than the whole alignment so that an end already aligned (and
     * one close to SIZE_MAX) is not reported as an overflow. */
    size_t remainder = layout->internal_end % entry->alignment;
    size_t gap = remainder == 0 ? 0 : entry->alignment - remainder;
    if (!ta_checked_add(layout->internal_end, gap, &offset) || !ta_checked_mul(entry->size, entry->count, &bytes) ||
        !ta_checked_add(offset, bytes, &end)) {
        layout->internal_overflowed = true;
        return 0;
    }
    layout->internal_end = end;
    if (entry->alignment > layout->internal_alignment) {
        layout->internal_alignment = entry->alignment;
    }
    return offset;
}

ta_result ta_layout_finish(const ta_layout *layout, size_t *bytes, size_t *alignment) {
    if (layout->internal_overflowed) {
        return TA_ERR_NOMEM;
    }
    if (bytes != NULL) {
        *bytes = layout->internal_end;
    }
    if (alignment != NULL) {
        *alignment = layout->internal_alignment;
    }
    return TA_OK;
}

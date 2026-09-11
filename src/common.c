#include "internal.h"

void tai_copy(void *destination, const void *source, size_t size)
{
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    size_t i;
    for (i = 0; i < size; ++i) out[i] = in[i];
}

void tai_zero(void *destination, size_t size)
{
    unsigned char *out = (unsigned char *)destination;
    size_t i;
    for (i = 0; i < size; ++i) out[i] = 0;
}

ta_status_t tai_alloc(ta_arena_t *arena, size_t size, void **out)
{
    ta_layout_t layout = { size, TAI_ALIGNMENT };
    return ta_arena_alloc(arena, &layout, out);
}

ta_status_t tai_array(ta_arena_t *arena, const tai_array_layout_t *layout, void **out)
{
    ta_layout_t request;
    if (layout == NULL || out == NULL) return TA_INVALID;
    *out = NULL;
    if (layout->count == 0) return TA_OK;
    if (layout->stride == 0 || layout->count > (size_t)-1 / layout->stride) return TA_LIMIT;
    request.size = layout->count * layout->stride;
    request.alignment = layout->alignment;
    return ta_arena_alloc(arena, &request, out);
}

ta_status_t tai_text_copy(ta_arena_t *arena, ta_text_t text, ta_text_t *out)
{
    ta_status_t status;
    void *copy;
    ta_layout_t layout = { text.size == 0 ? 1 : text.size, 1 };
    if (out == NULL || (text.data == NULL && text.size != 0)) return TA_INVALID;
    *out = (ta_text_t){NULL, 0};
    if (text.data == NULL) return TA_OK;
    status = ta_arena_alloc(arena, &layout, &copy);
    if (status != TA_OK) return status;
    tai_copy(copy, text.data, text.size);
    if (text.size == 0) ((char *)copy)[0] = 0;
    *out = (ta_text_t){ (const char *)copy, text.size };
    return TA_OK;
}

ta_status_t tai_cstr_copy(ta_arena_t *arena, const char *text, const char **out)
{
    ta_text_t slice;
    ta_text_t copy;
    ta_status_t status;
    if (out == NULL) return TA_INVALID;
    *out = NULL;
    if (text == NULL) return TA_OK;
    slice = ta_text_from_cstr(text);
    if (slice.size == (size_t)-1) return TA_LIMIT;
    ++slice.size;
    status = tai_text_copy(arena, slice, &copy);
    if (status == TA_OK) *out = copy.data;
    return status;
}

int tai_text_compare(ta_text_t left, ta_text_t right)
{
    size_t i, length = left.size < right.size ? left.size : right.size;
    for (i = 0; i < length; ++i) {
        unsigned char l = (unsigned char)left.data[i], r = (unsigned char)right.data[i];
        if (l != r) return l < r ? -1 : 1;
    }
    return left.size == right.size ? 0 : (left.size < right.size ? -1 : 1);
}

uint8_t tai_text_valid(ta_text_t text, uint8_t allow_empty)
{
    size_t i;
    if (text.data == NULL) return (uint8_t)(allow_empty && text.size == 0);
    if (!allow_empty && text.size == 0) return 0;
    for (i = 0; i < text.size; ++i) if (text.data[i] == '\0') return 0;
    return 1;
}

uint8_t tai_event_matches(ta_text_t descriptor, ta_text_t event)
{
    size_t i, count = descriptor.size;
    if (count == 1 && descriptor.data[0] == '*') return event.size != 0;
    if (count >= 2 && descriptor.data[count - 1] == '*' && descriptor.data[count - 2] == '.')
        count -= 2;
    else if (count != 0 && descriptor.data[count - 1] == '.') --count;
    if (count == 0 || event.size < count) return 0;
    for (i = 0; i < count; ++i) if (descriptor.data[i] != event.data[i]) return 0;
    return (uint8_t)(event.size == count || event.data[count] == '.');
}

uint64_t tai_hash(ta_text_t text)
{
    uint64_t hash = 14695981039346656037ull;
    size_t i;
    for (i = 0; i < text.size; ++i) {
        hash ^= (unsigned char)text.data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

void tai_mutated(ta_system_t *system)
{
    if (system->mutation == UINT64_MAX) system->mutation_exhausted = 1;
    else ++system->mutation;
}

void tai_saturate(uint64_t *counter)
{
    if (*counter != UINT64_MAX) ++*counter;
}

ta_status_t tai_scope(ta_system_t *system, ta_lifetime_t lifetime, ta_arena_t **out)
{
    if (system == NULL || out == NULL || lifetime >= TA_LIFETIME_COUNT) return TA_INVALID;
    return ta_arena_open(system->arena, &system->config.arenas[lifetime], out);
}

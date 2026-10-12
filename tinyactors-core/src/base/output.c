/* output.c — bytes into a room grown through an optional allocator
 * (base/output.h). */
#include "base/output.h"

#include "base/memory.h"

ta_output ta_output_over(ta_buffer room, const ta_allocator *allocator) {
    ta_output output;
    output.bytes = (unsigned char *)room.pointer;
    output.capacity = room.pointer ? room.capacity : 0;
    output.length = 0;
    output.allocator = allocator;
    output.room = output.bytes;
    return output;
}

int ta_output_reserve(ta_output *output, size_t more) {
    if (output->length > output->capacity) return 0; /* it already did not fit */
    if (more <= output->capacity - output->length) return 1;
    if (!output->allocator || more > (size_t)-1 / 2 - output->length) return 0;
    size_t need = output->length + more;
    size_t grown = output->capacity > 64 ? output->capacity : 64;
    while (grown < need)
        grown *= 2;
    unsigned char *block =
        (unsigned char *)output->allocator->alloc(output->allocator->context, grown, 8);
    if (!block) return 0;
    if (output->length) ta_memcpy(block, output->bytes, output->length);
    if (output->bytes && output->bytes != output->room)
        output->allocator->free(output->allocator->context, output->bytes, output->capacity);
    output->bytes = block;
    output->capacity = grown;
    return 1;
}

void ta_output_write(ta_output *output, const void *bytes, size_t length) {
    if (length && ta_output_reserve(output, length))
        ta_memcpy(output->bytes + output->length, bytes, length);
    output->length += length;
}

int ta_output_fits(const ta_output *output) {
    return output->length <= output->capacity;
}

void ta_output_patch(ta_output *output, size_t offset, ta_bytes bytes) {
    if (ta_output_fits(output) && offset <= output->length &&
        bytes.length <= output->length - offset && bytes.length)
        ta_memcpy(output->bytes + offset, bytes.pointer, bytes.length);
}

void ta_output_free(ta_output *output) {
    if (output->allocator && output->bytes && output->bytes != output->room)
        output->allocator->free(output->allocator->context, output->bytes, output->capacity);
    output->bytes = output->room;
    output->capacity = 0;
    output->length = 0;
}

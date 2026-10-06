/* frame.c — link frames on byte streams (see include/tinyactors/frame.h). Freestanding. */
#include <tinyactors/frame.h>
#include "base/memory.h"

void ta_frame_header(uint32_t length, uint8_t out[TA_FRAME_HEADER]) {
    out[0] = (uint8_t)(length >> 24);
    out[1] = (uint8_t)(length >> 16);
    out[2] = (uint8_t)(length >> 8);
    out[3] = (uint8_t)length;
}

void ta_frame_reader_init(ta_frame_reader *reader, uint32_t max_frame) {
    ta_memset(reader, 0, sizeof *reader);
    reader->size = sizeof *reader;
    reader->max_frame = max_frame;
}

uint32_t ta_frame_read(ta_frame_reader *reader, uint8_t *buffer, const uint8_t *in, uint32_t n,
                       uint32_t *event) {
    uint32_t i = 0;
    *event = TA_FRAME_MORE;
    while (i < n) {
        if (reader->head < TA_FRAME_HEADER) { /* the prefix, possibly split */
            if (reader->head == 0) reader->length = 0;
            reader->length = (reader->length << 8) | in[i++];
            if (++reader->head < TA_FRAME_HEADER) continue;
            reader->have = 0;
            reader->skip = reader->length > reader->max_frame;
            if (reader->skip) {
                *event = TA_FRAME_REFUSED;
                return i;
            }
            if (reader->length == 0) {
                reader->head = 0;
                *event = TA_FRAME_READY;
                return i;
            }
            continue;
        }
        uint32_t k = reader->length - reader->have;
        if (k > n - i) k = n - i;
        if (!reader->skip) ta_memcpy(buffer + reader->have, in + i, k);
        reader->have += k;
        i += k;
        if (reader->have < reader->length) continue;
        reader->head = 0; /* the frame is complete: the next prefix follows */
        if (reader->skip) {
            reader->skip = 0;
            continue;
        }
        *event = TA_FRAME_READY;
        return i;
    }
    return i;
}

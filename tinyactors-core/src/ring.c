#include "ring.h"

static void ta_ring_free_bytes(ta_ring *ring) {
    if (ring->bytes == NULL) {
        return;
    }
    ta_mem_free(ring->allocator, ring->bytes,
                &(ta_allocator_free_params){ .size = ring->capacity, .alignment = TA_RING_ALIGNMENT });
}

/* The offset in the buffer of a counter value. */
static uint32_t ta_ring_offset(const ta_ring *ring, uint64_t counter) {
    return (uint32_t)(counter % ring->capacity);
}

/* Writes a prefix (kind, flags 0, size) at `at`. */
static void ta_ring_write_prefix(unsigned char *at, uint16_t kind, uint32_t size) {
    uint16_t flags = 0;
    ta_mem_copy(at, &kind, sizeof(kind));
    ta_mem_copy(at + 2, &flags, sizeof(flags));
    ta_mem_copy(at + 4, &size, sizeof(size));
}

ta_result ta_ring_init(ta_ring *ring, const ta_ring_params *params) {
    uint32_t max_record = params->max_record_bytes;
    if ((params->mode != TA_RING_LOSSLESS && params->mode != TA_RING_LOSSY) ||
        max_record % TA_RING_ALIGNMENT != 0 || max_record < TA_RING_RECORD_MIN_BYTES ||
        params->capacity % TA_RING_ALIGNMENT != 0 || params->capacity < max_record) {
        return TA_ERR_INVALID;
    }
    unsigned char *bytes = ta_mem_alloc(params->allocator, params->capacity, TA_RING_ALIGNMENT);
    if (bytes == NULL) {
        return TA_ERR_NOMEM;
    }
    *ring = (ta_ring){
        .allocator = params->allocator,
        .bytes = bytes,
        .capacity = params->capacity,
        .max_record_bytes = max_record,
        .mode = params->mode,
        .next_seq = 1u,
    };
    return TA_OK;
}

void ta_ring_destroy(ta_ring *ring) {
    ta_ring_free_bytes(ring);
    ring->bytes = NULL;
    ring->capacity = 0;
}

ta_result ta_ring_reserve(ta_ring *ring, uint32_t size, ta_ring_record *out) {
    if (size < TA_RING_RECORD_MIN_BYTES || size > ring->max_record_bytes) {
        return TA_ERR_INVALID;
    }
    /* max_record_bytes is a multiple of 8, so rounding up stays within it. */
    uint32_t aligned = TA_ALIGN_UP(size, TA_RING_ALIGNMENT);
    uint32_t available = ta_ring_available(ring);
    uint32_t at = ta_ring_offset(ring, ring->write);
    uint32_t to_end = ring->capacity - at;
    uint32_t pad = aligned <= to_end ? 0u : to_end;
    /* pad + aligned <= 2 * capacity, which a uint64_t holds. */
    uint64_t need = (uint64_t)pad + aligned;
    if (need > available) {
        if (pad != 0 && need > ring->capacity && ring->write == ring->head && pad <= available) {
            /* No release can ever make room for both: commit the pad
             * alone, so the next reservation starts at offset 0. */
            ta_ring_write_prefix(ring->bytes + at, 0u, pad);
            ring->write += pad;
            ring->head = ring->write;
        }
        if (ring->mode == TA_RING_LOSSY) {
            if (ring->unreported.count == 0) {
                ring->unreported.first_seq = ring->next_seq;
            }
            ring->unreported.count += 1u;
            ring->dropped += 1u;
            ring->next_seq += 1u;
        }
        return TA_ERR_FULL;
    }
    if (pad != 0) {
        ta_ring_write_prefix(ring->bytes + at, 0u, pad);
        ring->write += pad;
        at = 0;
    }
    unsigned char *record = ring->bytes + at;
    uint64_t seq = ring->next_seq;
    ta_ring_write_prefix(record, 0u, aligned);
    ta_mem_copy(record + TA_RING_PREFIX_BYTES, &seq, sizeof(seq));
    ring->write += aligned;
    ring->next_seq += 1u;
    *out = (ta_ring_record){ .bytes = record, .size = aligned, .kind = 0u, .seq = seq };
    return TA_OK;
}

ta_result ta_ring_peek(ta_ring *ring, ta_ring_record *out) {
    for (;;) {
        if (ring->tail == ring->head) {
            return TA_ERR_NOT_FOUND;
        }
        uint32_t at = ta_ring_offset(ring, ring->tail);
        uint32_t to_end = ring->capacity - at;
        uint64_t committed = ring->head - ring->tail;
        if (at % TA_RING_ALIGNMENT != 0 || to_end < TA_RING_PREFIX_BYTES || committed < TA_RING_PREFIX_BYTES) {
            return TA_ERR_CORRUPT;
        }
        unsigned char *record = ring->bytes + at;
        uint16_t kind;
        uint32_t size;
        ta_mem_copy(&kind, record, sizeof(kind));
        ta_mem_copy(&size, record + 4, sizeof(size));
        if (size < TA_RING_PREFIX_BYTES || size % TA_RING_ALIGNMENT != 0 || size > to_end || size > committed) {
            return TA_ERR_CORRUPT;
        }
        if (kind == 0) {
            /* A pad fills the buffer to its end; it holds nothing, so
             * release it and read on. */
            if (size != to_end) {
                return TA_ERR_CORRUPT;
            }
            ring->tail += size;
            continue;
        }
        if (size < TA_RING_RECORD_MIN_BYTES || size > ring->max_record_bytes) {
            return TA_ERR_CORRUPT;
        }
        uint64_t seq;
        ta_mem_copy(&seq, record + TA_RING_PREFIX_BYTES, sizeof(seq));
        bool in_order = ring->mode == TA_RING_LOSSLESS ? seq == ring->read_seq + 1u : seq > ring->read_seq;
        if (!in_order) {
            return TA_ERR_CORRUPT;
        }
        *out = (ta_ring_record){ .bytes = record, .size = size, .kind = kind, .seq = seq };
        return TA_OK;
    }
}

ta_result ta_ring_release(ta_ring *ring) {
    ta_ring_record record;
    ta_result result = ta_ring_peek(ring, &record);
    if (result != TA_OK) {
        return result;
    }
    ring->tail += record.size;
    ring->read_seq = record.seq;
    return TA_OK;
}

ta_result ta_ring_take_drops(ta_ring *ring, ta_ring_drops *out) {
    if (ring->unreported.count == 0) {
        return TA_ERR_NOT_FOUND;
    }
    *out = ring->unreported;
    ring->unreported = (ta_ring_drops){ 0 };
    return TA_OK;
}

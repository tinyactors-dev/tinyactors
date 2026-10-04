#include "internal.h"

/* A spec is valid when progress is possible: the largest record fits an empty ring, and every size is a multiple of 8. */
static bool spec_valid(const ta_ring_spec *spec) {
    return spec->capacity % TA_RING_ALIGN == 0 && spec->max_record_bytes % TA_RING_ALIGN == 0 &&
           spec->max_record_bytes >= TA_RING_MIN_RECORD && spec->max_record_bytes <= spec->capacity &&
           (spec->mode == TA_RING_LOSSLESS || spec->mode == TA_RING_LOSSY);
}

/* ta_rings_layout: see ring.h. The control block is entry 0; the data areas follow, each 8-aligned. */
ta_result ta_rings_layout(const ta_rings_specs *specs, ta_rings_plan *plan) {
    for (uint32_t i = 0; i < TA_RING_COUNT; i += 1) {
        if (!spec_valid(&specs->ring[i])) {
            return TA_ERR_INVALID;
        }
    }
    ta_layout layout;
    ta_layout_init(&layout, 0);
    size_t control = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(ta_ring_control), _Alignof(ta_ring_control), 1 });
    size_t h2c = ta_layout_add(&layout, &(ta_layout_entry){ 1, TA_RING_ALIGN, specs->ring[TA_RING_H2C].capacity });
    size_t c2h = ta_layout_add(&layout, &(ta_layout_entry){ 1, TA_RING_ALIGN, specs->ring[TA_RING_C2H].capacity });
    size_t tel = ta_layout_add(&layout, &(ta_layout_entry){ 1, TA_RING_ALIGN, specs->ring[TA_RING_TEL].capacity });
    size_t bytes, alignment;
    ta_result result = ta_layout_finish(&layout, &bytes, &alignment);
    if (result != TA_OK) {
        return result;
    }
    (void)control; /* the control block is at the origin */
    *plan = (ta_rings_plan){ .bytes = bytes, .alignment = alignment, .data_offset = { h2c, c2h, tel } };
    return TA_OK;
}

/* ta_rings_init: see ring.h. Nothing is written until every check has passed. */
ta_result ta_rings_init(ta_rings *rings, const ta_rings_init_params *params) {
    ta_mem_zero(rings, sizeof *rings);
    ta_rings_plan plan;
    ta_result result = ta_rings_layout(&params->specs, &plan);
    if (result != TA_OK) {
        return result;
    }
    if (params->memory == NULL || params->memory_bytes < plan.bytes ||
        (uintptr_t)params->memory % plan.alignment != 0) {
        return TA_ERR_INVALID;
    }
    unsigned char *base = params->memory;
    ta_ring_control *control = (ta_ring_control *)(void *)base;
    ta_mem_zero(control, sizeof *control);
    control->magic = TA_RING_MAGIC;
    control->version = TA_RING_VERSION;
    control->ring_count = TA_RING_COUNT;
    rings->control = control;
    for (uint32_t i = 0; i < TA_RING_COUNT; i += 1) {
        const ta_ring_spec *spec = &params->specs.ring[i];
        ta_ring_shared *shared = &control->ring[i];
        shared->capacity = spec->capacity;
        shared->offset = plan.data_offset[i];
        shared->next_seq = 1;
        shared->max_record_bytes = spec->max_record_bytes;
        shared->mode = (uint32_t)spec->mode;
        rings->ring[i] = (ta_ring){ .shared = shared,
                                    .data = base + plan.data_offset[i],
                                    .capacity = spec->capacity,
                                    .next_seq = 1,
                                    .expected_seq = 1,
                                    .max_record_bytes = spec->max_record_bytes,
                                    .mode = spec->mode };
    }
    return TA_OK;
}

/* Native-order stores of the header cells the ring writes. Records are 8-aligned, but a copy is the portable way to touch a field of bytes. */
static void put_u32(unsigned char *at, uint32_t value) { ta_mem_copy(at, &value, sizeof value); }
static void put_u64(unsigned char *at, uint64_t value) { ta_mem_copy(at, &value, sizeof value); }

/*
 * pad_write writes the 8-byte pad prefix (kind 0, flags 0, size = bytes to
 * the end of the buffer) at buffer offset `at`. A pad is the only thing the
 * ring writes besides size and seq, and it is written at commit, when the
 * bytes are certainly free (reserve counted them in its need).
 */
static void pad_write(ta_ring *ring, uint64_t at, uint32_t bytes) {
    ta_mem_zero(ring->data + at, 4);
    put_u32(ring->data + at + 4, bytes);
}

/*
 * refresh_peer_tail reads the consumer's tail from the control block. The
 * host writes it on c2h and tel, so it is untrusted: it must not go back
 * (counters only grow) and must not pass what has been committed. Only
 * differences are compared (I1), so a counter that wrapped u64 in theory
 * would still be judged by distance.
 */
static bool refresh_peer_tail(ta_ring *ring) {
    uint64_t tail = ring->shared->tail;
    if (tail - ring->peer_tail > ring->head - ring->peer_tail) {
        return false; /* either behind what was seen, or ahead of head: unsigned distances catch both */
    }
    ring->peer_tail = tail;
    return true;
}

/*
 * refuse answers a reservation that does not fit. In a lossless ring it
 * changes nothing; the caller retries after a release.
 */
static ta_ring_status refuse(ta_ring *ring, uint32_t size, ta_ring_reservation *out) {
    *out = (ta_ring_reservation){ .bytes = NULL, .size = size, .seq = ring->next_seq };
    return TA_RING_FULL;
}

/* ta_ring_reserve: see ring.h. */
ta_ring_status ta_ring_reserve(ta_ring *ring, uint32_t size, ta_ring_reservation *out) {
    if (ring->pending_size != 0) {
        return TA_RING_BUSY;
    }
    /* Round up to 8 without wrapping u32: a size within 7 of the top is too big in any ring. */
    if (size > UINT32_MAX - (TA_RING_ALIGN - 1)) {
        return TA_RING_TOO_BIG;
    }
    uint32_t rounded = (size + (TA_RING_ALIGN - 1)) & ~(TA_RING_ALIGN - 1);
    if (rounded < TA_RING_MIN_RECORD || rounded > ring->max_record_bytes) {
        return TA_RING_TOO_BIG;
    }
    if (!refresh_peer_tail(ring)) {
        return TA_RING_CORRUPT;
    }
    uint64_t position = ring->head % ring->capacity;
    uint64_t to_end = ring->capacity - position; /* at least 8: head is a multiple of 8 */
    uint64_t pad = rounded > to_end ? to_end : 0;
    uint64_t free_bytes = ring->capacity - (ring->head - ring->peer_tail);
    if (pad + rounded > free_bytes) {
        /*
         * Does not fit. If nothing is unreleased and the record cannot fit
         * even from offset 0 after the pad, the pad alone makes progress:
         * commit it (it holds no seq) and let the consumer's next peek
         * release it, after which the record starts at offset 0.
         */
        if (ring->head == ring->peer_tail && pad != 0) {
            pad_write(ring, position, (uint32_t)pad);
            ring->head += pad;
            ring->shared->head = ring->head;
        }
        return refuse(ring, rounded, out);
    }
    ring->pending_size = rounded;
    ring->pending_pad = (uint32_t)pad;
    ring->pending_seq = ring->next_seq;
    *out = (ta_ring_reservation){ .bytes = ring->data + (pad != 0 ? 0 : position), .size = rounded, .seq = ring->next_seq };
    return TA_RING_OK;
}

/* ta_ring_commit: see ring.h. Bytes first, counters last, so a reader never sees a half-written record. */
void ta_ring_commit(ta_ring *ring) {
    if (ring->pending_size == 0) {
        return;
    }
    uint64_t position = ring->head % ring->capacity;
    unsigned char *record = ring->data + (ring->pending_pad != 0 ? 0 : position);
    if (ring->pending_pad != 0) {
        pad_write(ring, position, ring->pending_pad);
    }
    put_u32(record + 4, ring->pending_size);
    put_u64(record + 8, ring->pending_seq);
    ring->head += (uint64_t)ring->pending_pad + ring->pending_size;
    ring->next_seq = ring->pending_seq + 1;
    ring->shared->head = ring->head;
    ring->shared->next_seq = ring->next_seq;
    ring->pending_size = 0;
    ring->pending_pad = 0;
}

/* ta_ring_abandon: see ring.h. The reservation never wrote a byte or a counter, so forgetting it is all. */
void ta_ring_abandon(ta_ring *ring) {
    ring->pending_size = 0;
    ring->pending_pad = 0;
}

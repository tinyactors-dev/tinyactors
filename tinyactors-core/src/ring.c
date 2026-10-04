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

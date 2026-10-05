#include "tinyactors_host/adapter_trace.h"

#include "tinyactors_host/system.h"
#include "system_private.h"

/* tah_trace_set_masks: see adapter_trace.h. */
ta_result tah_trace_set_masks(tah_system *system, const tah_trace_masks *masks) {
    tah_trace_drain(system); /* under the masks the records were written with */
    return ta_system_set_trace_masks(system->core, &(ta_trace_masks){ .kinds = masks->kinds,
                                                                      .watched_kinds = masks->watched_kinds,
                                                                      .unsampled_kinds = masks->unsampled_kinds,
                                                                      .values = masks->values });
}

/* tah_trace_drain: see adapter_trace.h. */
uint32_t tah_trace_drain(tah_system *system) {
    if (system->core == NULL) {
        return 0;
    }
    uint32_t read = 0;
    ta_ring_record raw;
    while (ta_ring_peek(system->core, TA_RING_TEL, &raw) == TA_OK) {
        ta_tel_entry entry;
        if (system->tracing && ta_tel_decode(&raw, &entry) == TA_OK) {
            uint32_t before = system->otlp.span_count;
            if (entry.type == TA_TEL_TRACE) {
                tah_otlp_write(&system->otlp, &entry.trace);
            } else {
                tah_otlp_dropped(&system->otlp, &entry.dropped);
            }
            if (system->otlp.span_count != before) {
                tah_system_want_turn(system);
            }
        }
        ta_ring_release(system->core, TA_RING_TEL, raw.size);
        read += 1u;
    }
    return read;
}

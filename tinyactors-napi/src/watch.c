#include "watch.h"

#include <string.h>

#include "allocator.h"

void tna_trace_watch_init(tna_trace_watch *watch, ta_allocator allocator) {
    *watch = (tna_trace_watch){ .allocator = allocator };
}

static void tna_trace_watch_free(tna_trace_watch *watch) {
    uint32_t count = watch->session_count + watch->definition_count;
    if (watch->ids != NULL) {
        tna_free(watch->allocator, &(ta_allocator_free_params){ .ptr = watch->ids, .size = count * sizeof(uint64_t),
                                                                .alignment = _Alignof(uint64_t) });
    }
    watch->ids = NULL;
    watch->session_count = 0;
    watch->definition_count = 0;
}

void tna_trace_watch_destroy(tna_trace_watch *watch) {
    tna_trace_watch_free(watch);
}

ta_result tna_trace_watch_set(tna_trace_watch *watch, const tna_trace_watch_set_params *params) {
    uint32_t count = params->session_count + params->definition_count;
    uint64_t *ids = NULL;
    if (count != 0) {
        ids = tna_alloc(watch->allocator, count * sizeof(uint64_t), _Alignof(uint64_t));
        if (ids == NULL) {
            return TA_ERR_NOMEM;
        }
        if (params->session_count != 0) {
            memcpy(ids, params->sessions, params->session_count * sizeof(uint64_t));
        }
        for (uint32_t at = 0; at < params->definition_count; at += 1u) {
            ids[params->session_count + at] = params->definitions[at];
        }
    }
    tna_trace_watch_free(watch);
    watch->ids = ids;
    watch->session_count = params->session_count;
    watch->definition_count = params->definition_count;
    return TA_OK;
}

static bool tna_trace_watch_has(const uint64_t *ids, uint32_t count, uint64_t id) {
    for (uint32_t at = 0; at < count; at += 1u) {
        if (ids[at] == id) {
            return true;
        }
    }
    return false;
}

void tna_trace_watch_mark(const tna_trace_watch *watch, ta_system *core, ta_actor_id actor, ta_trace_parent *trace) {
    if (tna_trace_watch_empty(watch)) {
        return;
    }
    ta_actor_inspection view;
    if (ta_actor_inspect(core, actor, &view) != TA_OK ||
        (!tna_trace_watch_has(watch->ids, watch->session_count, view.session_id) &&
         !tna_trace_watch_has(watch->ids + watch->session_count, watch->definition_count, view.definition_id))) {
        return;
    }
    if (trace->context.trace_id_high == 0 && trace->context.trace_id_low == 0) {
        trace->context = ta_system_new_trace(core);
    }
    trace->context.flags |= TA_TRACE_FLAG_SAMPLED;
    trace->context.detail |= TA_TRACE_DETAIL_WATCHED;
}

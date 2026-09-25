#include "watch.h"

#include "allocator.h"

#define TNA_ID_EMPTY UINT64_MAX

void tna_trace_watch_init(tna_trace_watch *watch, ta_allocator allocator) {
    *watch = (tna_trace_watch){ .allocator = allocator };
}

static void tna_id_set_free(ta_allocator allocator, tna_id_set *set) {
    if (set->slots != NULL) {
        tna_free(allocator, &(ta_allocator_free_params){ .ptr = set->slots, .size = set->capacity * sizeof(uint64_t),
                                                         .alignment = _Alignof(uint64_t) });
    }
    *set = (tna_id_set){0};
}

void tna_trace_watch_destroy(tna_trace_watch *watch) {
    tna_id_set_free(watch->allocator, &watch->sessions);
    tna_id_set_free(watch->allocator, &watch->definitions);
}

/* SplitMix64's finalizer: session and definition IDs are sequential. */
static uint64_t tna_id_hash(uint64_t id) {
    id ^= id >> 30;
    id *= 0xbf58476d1ce4e5b9u;
    id ^= id >> 27;
    id *= 0x94d049bb133111ebu;
    return id ^ (id >> 31);
}

bool tna_id_set_has(const tna_id_set *set, uint64_t id) {
    if (id == TNA_ID_EMPTY) {
        return set->has_max;
    }
    if (set->capacity == 0) {
        return false;
    }
    uint32_t mask = set->capacity - 1u;
    for (uint32_t at = (uint32_t)tna_id_hash(id) & mask;; at = (at + 1u) & mask) {
        uint64_t slot = set->slots[at];
        if (slot == id) {
            return true;
        }
        if (slot == TNA_ID_EMPTY) {
            return false;
        }
    }
}

static void tna_id_set_put(tna_id_set *set, uint64_t id) {
    if (id == TNA_ID_EMPTY) {
        set->count += set->has_max ? 0u : 1u;
        set->has_max = true;
        return;
    }
    uint32_t mask = set->capacity - 1u;
    for (uint32_t at = (uint32_t)tna_id_hash(id) & mask;; at = (at + 1u) & mask) {
        if (set->slots[at] == id) {
            return;
        }
        if (set->slots[at] == TNA_ID_EMPTY) {
            set->slots[at] = id;
            set->count += 1u;
            return;
        }
    }
}

/* Builds a set of `count` IDs. TA_ERR_NOMEM leaves `out` empty. */
static ta_result tna_id_set_build(ta_allocator allocator, const uint64_t *ids, uint32_t count, tna_id_set *out) {
    *out = (tna_id_set){0};
    if (count == 0) {
        return TA_OK;
    }
    if (count > UINT32_MAX / 4u) {
        return TA_ERR_NOMEM;
    }
    uint32_t capacity = 8u;
    while (capacity < count * 2u) {
        capacity *= 2u;
    }
    uint64_t *slots = tna_alloc(allocator, capacity * sizeof(uint64_t), _Alignof(uint64_t));
    if (slots == NULL) {
        return TA_ERR_NOMEM;
    }
    for (uint32_t at = 0; at < capacity; at += 1u) {
        slots[at] = TNA_ID_EMPTY;
    }
    *out = (tna_id_set){ .slots = slots, .capacity = capacity };
    for (uint32_t at = 0; at < count; at += 1u) {
        tna_id_set_put(out, ids[at]);
    }
    return TA_OK;
}

ta_result tna_trace_watch_set(tna_trace_watch *watch, const tna_trace_watch_set_params *params) {
    tna_id_set sessions;
    tna_id_set definitions;
    if (tna_id_set_build(watch->allocator, params->sessions, params->session_count, &sessions) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    if (tna_id_set_build(watch->allocator, params->definitions, params->definition_count, &definitions) != TA_OK) {
        tna_id_set_free(watch->allocator, &sessions);
        return TA_ERR_NOMEM;
    }
    tna_trace_watch_destroy(watch);
    watch->sessions = sessions;
    watch->definitions = definitions;
    return TA_OK;
}

void tna_trace_watch_mark(const tna_trace_watch *watch, ta_system *core, ta_actor_id actor, ta_trace_parent *trace) {
    if (tna_trace_watch_empty(watch)) {
        return;
    }
    ta_actor_inspection view;
    if (ta_actor_inspect(core, actor, &view) != TA_OK ||
        (!tna_id_set_has(&watch->sessions, view.session_id) &&
         !tna_id_set_has(&watch->definitions, view.definition_id))) {
        return;
    }
    if (trace->context.trace_id_high == 0 && trace->context.trace_id_low == 0) {
        trace->context = ta_system_new_trace(core);
    }
    trace->context.flags |= TA_TRACE_FLAG_SAMPLED;
    trace->context.detail |= TA_TRACE_DETAIL_WATCHED;
}

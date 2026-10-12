/* handles.c — the native datamodel's two tables of handles, for payloads
 * (with reference counts) and for sessions: a handle is its slot and a
 * generation, never a pointer, so every JavaScript value the binding holds
 * is in a slot (one napi_ref each), and a stale handle is refused
 * (bindings.md, section 8). A payload's count lives here, not in its
 * napi_ref: a finalizer may call only basic-env functions, and
 * napi_reference_unref is not one, napi_delete_reference is (Node.js 22's
 * include/node/js_native_api.h:339, 355). */
#include "datamodel.h"

#include <stdlib.h>

/* handle = generation << 32 | (slot + 1) */
static uint64_t ta_napi_handle(uint32_t slot, uint32_t generation) {
    return (uint64_t)generation << 32 | (uint64_t)(slot + 1);
}

/* A slot's value is held in a one-item array, since Node-API version 8
 * makes references to objects, functions and symbols only, and a payload
 * may be a number, a string or undefined. */
uint64_t ta_napi_slots_add(ta_napi_datamodel *datamodel, ta_napi_slots *slots, const ta_napi_slot_init *init) {
    napi_env env = datamodel->env;
    napi_ref reference = NULL;
    napi_value holder;
    if (init->value && (napi_create_array_with_length(env, 1, &holder) != napi_ok ||
                        napi_set_element(env, holder, 0, init->value) != napi_ok ||
                        napi_create_reference(env, holder, 1, &reference) != napi_ok))
        return 0;
    uint32_t slot;
    if (slots->count && slots->free != TA_NIL) {
        slot = slots->free;
        slots->free = slots->items[slot].next_free;
    } else {
        if (slots->count == slots->capacity) {
            uint32_t capacity = slots->capacity ? slots->capacity * 2 : 64;
            ta_napi_slot *items = (ta_napi_slot *)realloc(slots->items, capacity * sizeof *items);
            if (!items) {
                if (reference) napi_delete_reference(datamodel->env, reference);
                return 0;
            }
            slots->items = items;
            slots->capacity = capacity;
        }
        if (!slots->count) slots->free = TA_NIL;
        slot = slots->count++;
        slots->items[slot].generation = 1;
    }
    ta_napi_slot *item = &slots->items[slot];
    item->value = reference;
    item->data = init->data;
    item->references = 1;
    item->next_free = TA_NIL;
    return ta_napi_handle(slot, item->generation);
}

ta_napi_slot *ta_napi_slots_get(ta_napi_slots *slots, uint64_t handle) {
    uint32_t slot = (uint32_t)(handle & 0xFFFFFFFFu);
    uint32_t generation = (uint32_t)(handle >> 32);
    if (!slot || slot > slots->count) return NULL;
    ta_napi_slot *item = &slots->items[slot - 1];
    return item->references && item->generation == generation ? item : NULL;
}

void ta_napi_slots_remove(ta_napi_datamodel *datamodel, ta_napi_slots *slots, uint64_t handle) {
    ta_napi_slot *item = ta_napi_slots_get(slots, handle);
    if (!item) return;
    if (item->value) ta_napi_defer_drop(datamodel, item->value);
    item->value = NULL;
    item->data = NULL;
    item->references = 0;
    item->generation++;
    item->next_free = slots->free;
    slots->free = (uint32_t)((handle & 0xFFFFFFFFu) - 1);
}

void ta_napi_defer_drop(ta_napi_datamodel *datamodel, napi_ref reference) {
    if (datamodel->drop_count == datamodel->drop_capacity) {
        uint32_t capacity = datamodel->drop_capacity ? datamodel->drop_capacity * 2 : 64;
        napi_ref *drops = (napi_ref *)realloc(datamodel->drops, capacity * sizeof *drops);
        if (!drops) { /* no room to defer it: drop it now */
            napi_delete_reference(datamodel->env, reference);
            return;
        }
        datamodel->drops = drops;
        datamodel->drop_capacity = capacity;
    }
    datamodel->drops[datamodel->drop_count++] = reference;
}

uint32_t ta_napi_datamodel_drain(ta_napi_datamodel *datamodel) {
    uint32_t dropped = 0;
    if (!datamodel) return 0;
    while (datamodel->drop_count) {
        napi_ref reference = datamodel->drops[--datamodel->drop_count];
        napi_delete_reference(datamodel->env, reference);
        dropped++;
    }
    return dropped;
}

napi_status ta_napi_slot_value(ta_napi_datamodel *datamodel, const ta_napi_slot *slot, napi_value *out) {
    napi_value holder;
    if (!slot->value) return napi_get_undefined(datamodel->env, out);
    napi_status status = napi_get_reference_value(datamodel->env, slot->value, &holder);
    return status == napi_ok ? napi_get_element(datamodel->env, holder, 0, out) : status;
}

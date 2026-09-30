#include "tinyactors_host/offloads.h"

#include "memory.h"
#include "offloads_private.h"


void tah_offloads_init(tah_offloads *offloads, ta_allocator allocator) {
    *offloads = (tah_offloads){ .allocator = allocator };
    tah_map_init(&offloads->entries, allocator);
}

static void tah_offload_free(tah_offloads *offloads, tah_offload *entry) {
    for (uint32_t index = 0; index < entry->handle_count; index += 1) {
        ta_value_release(&entry->handles[index]);
    }
    if (entry->handles != NULL) {
        offloads->allocator.v->free(offloads->allocator.self,
                                    &(ta_allocator_free_params){ .ptr = entry->handles,
                                                                 .size = (size_t)entry->handle_count * sizeof(ta_value),
                                                                 .alignment = _Alignof(ta_value) });
    }
    offloads->bytes_in_use -= entry->length;
    offloads->allocator.v->free(offloads->allocator.self,
                                &(ta_allocator_free_params){ .ptr = entry,
                                                             .size = sizeof(tah_offload) + entry->length,
                                                             .alignment = _Alignof(tah_offload) });
}

ta_result tah_offloads_put(tah_offloads *offloads, const tah_offload_put_params *params) {
    size_t size = sizeof(tah_offload) + params->bytes.len;
    tah_offload *entry = offloads->allocator.v->alloc(offloads->allocator.self, size, _Alignof(tah_offload));
    if (entry == NULL) {
        return TA_ERR_NOMEM;
    }
    uint64_t key = (uint64_t)(uintptr_t)params->address;
    uint64_t previous = 0;
    bool replaced = tah_map_get(&offloads->entries, key, &previous);
    if (tah_map_put(&offloads->entries, key, (uint64_t)(uintptr_t)entry) != TA_OK) {
        offloads->allocator.v->free(offloads->allocator.self,
                                    &(ta_allocator_free_params){ .ptr = entry, .size = size,
                                                                 .alignment = _Alignof(tah_offload) });
        return TA_ERR_NOMEM;
    }
    *entry = (tah_offload){
        .bytes = (uint8_t *)(entry + 1),
        .length = params->bytes.len,
        .handles = params->handles,
        .handle_count = params->handle_count,
    };
    if (params->bytes.len != 0) {
        tah_mem_copy(entry->bytes, params->bytes.ptr, params->bytes.len);
    }
    offloads->bytes_in_use += entry->length;
    if (replaced) {
        tah_offload_free(offloads, (tah_offload *)(uintptr_t)previous);
    }
    return TA_OK;
}

const tah_offload *tah_offloads_find(const tah_offloads *offloads, const ta_address *address) {
    uint64_t value = 0;
    return tah_map_get(&offloads->entries, (uint64_t)(uintptr_t)address, &value) ? (const tah_offload *)(uintptr_t)value
                                                                                  : NULL;
}

bool tah_offloads_bytes(const tah_offloads *offloads, const ta_address *address, ta_bytes *out) {
    const tah_offload *entry = tah_offloads_find(offloads, address);
    if (entry == NULL) {
        return false;
    }
    *out = (ta_bytes){ .ptr = entry->bytes, .len = (uint32_t)entry->length };
    return true;
}

bool tah_offloads_drop(tah_offloads *offloads, const ta_address *address) {
    uint64_t key = (uint64_t)(uintptr_t)address;
    uint64_t value = 0;
    if (!tah_map_get(&offloads->entries, key, &value)) {
        return false;
    }
    (void)tah_map_remove(&offloads->entries, key);
    tah_offload_free(offloads, (tah_offload *)(uintptr_t)value);
    return true;
}

uint32_t tah_offloads_count(const tah_offloads *offloads) {
    return tah_map_count(&offloads->entries);
}

void tah_offloads_destroy(tah_offloads *offloads) {
    uint32_t cursor = 0;
    uint64_t key = 0;
    uint64_t value = 0;
    while (tah_map_next(&offloads->entries, &cursor, &key, &value)) {
        tah_offload_free(offloads, (tah_offload *)(uintptr_t)value);
    }
    tah_map_destroy(&offloads->entries);
    tah_map_init(&offloads->entries, offloads->allocator);
}

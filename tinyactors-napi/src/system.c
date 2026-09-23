#include "system.h"

#include <stdlib.h>

ta_result tna_system_create(tna_system **out) {
    if (out == NULL) {
        return TA_ERR_INVALID;
    }
    *out = NULL;
    tna_system *system = malloc(sizeof *system);
    if (system == NULL) {
        return TA_ERR_NOMEM;
    }
    *system = (tna_system){0};
    tna_allocator_init(&system->allocator);
    ta_result result = tna_scratch_init(&system->scratch, &system->allocator);
    if (result != TA_OK) {
        free(system);
        return result;
    }
    ta_system_config config = {
        .allocator = tna_allocator_for(&system->allocator, TNA_MEMORY_CORE),
        .initial_time = 0,
    };
    result = ta_system_create(&config, &system->core);
    if (result != TA_OK) {
        tna_scratch_destroy(&system->scratch);
        free(system);
        return result;
    }
    *out = system;
    return TA_OK;
}

void tna_system_close(tna_system *system) {
    if (system == NULL || system->closed) {
        return;
    }
    system->closed = true;
    ta_system_destroy(system->core);
    system->core = NULL;
    tna_scratch_destroy(&system->scratch);
    system->leaked_bytes = tna_allocator_total(&system->allocator).bytes_in_use;
}

bool tna_system_closed(const tna_system *system) {
    return system == NULL || system->closed;
}

size_t tna_system_leaked_bytes(const tna_system *system) {
    return system == NULL ? 0 : system->leaked_bytes;
}

void tna_system_free(tna_system *system) {
    if (system == NULL) {
        return;
    }
    tna_system_close(system);
    free(system);
}

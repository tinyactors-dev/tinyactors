#include "configuration.h"
#include "memory.h"
#include "bitset.h"
#include "../include/tinyactors.h"

bool ta_configuration_contains_atomic(const ta_configuration *config,
                                      ta_state_id atomic_state) {
    if (atomic_state >= config->layout->atomic_state_count) {
        return false;
    }
    return ta_bits_get(config->storage, atomic_state);
}

void ta_configuration_set_atomic(ta_configuration *config,
                                 ta_state_id atomic_state,
                                 bool active) {
    if (atomic_state < config->layout->atomic_state_count) {
        ta_bits_put(config->storage, atomic_state, active);
    }
}

void ta_configuration_clear(ta_configuration *config) {
    ta_mem_zero(config->storage, config->layout->bytes);
}

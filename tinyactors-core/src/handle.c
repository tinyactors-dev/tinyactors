#include "handle.h"

/*
 * Implementation of block E1 (handle). The contract, lifetime and usage are
 * in handle.h. Nothing here allocates, can fail or keeps state; the layout
 * is fixed by ADR 0059 and persisted, so the shifts and masks below are the
 * only place it is written down.
 */

ta_handle ta_handle_make(uint32_t slot, uint32_t generation) {
    return ((ta_handle)generation << 32) | slot;
}

uint32_t ta_handle_slot(ta_handle handle) {
    return (uint32_t)(handle & 0xffffffffu);
}

uint32_t ta_handle_generation(ta_handle handle) {
    return (uint32_t)(handle >> 32);
}

bool ta_handle_valid(ta_handle handle) {
    return ta_handle_generation(handle) != 0u;
}

bool ta_handle_matches(ta_handle handle, uint32_t current_generation) {
    return ta_handle_valid(handle) && ta_handle_generation(handle) == current_generation;
}

uint32_t ta_handle_next_generation(uint32_t current_generation) {
    return current_generation == UINT32_MAX || current_generation == 0u ? 1u : current_generation + 1u;
}

#pragma once

#include "configuration.h"

typedef struct {
    uint32_t offset;
    uint16_t bytes;
    uint16_t flags;
} ta_history_slot;

typedef struct {
    ta_history_slot *slots;
    uint32_t slot_count;
    uint32_t storage_bytes;
} ta_history_layout;

typedef struct {
    void *storage;
    const ta_history_layout *layout;
} ta_history_state;

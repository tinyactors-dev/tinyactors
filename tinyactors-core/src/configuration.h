#pragma once

#include "types.h"

typedef enum {
    TA_CONFIG_SINGLE_LEAF,
    TA_CONFIG_BITMAP32,
    TA_CONFIG_BITMAP64,
    TA_CONFIG_WORDS
} ta_configuration_kind;

typedef struct {
    ta_configuration_kind kind;
    uint16_t bytes;
    uint16_t alignment;
    uint32_t atomic_state_count;
} ta_configuration_layout;

typedef struct ta_configuration ta_configuration;
struct ta_configuration {
    void *storage;
    const ta_configuration_layout *layout;
};

bool ta_configuration_contains_atomic(const ta_configuration *config,
                                      ta_state_id atomic_state);
void ta_configuration_set_atomic(ta_configuration *config,
                                 ta_state_id atomic_state,
                                 bool active);
void ta_configuration_clear(ta_configuration *config);

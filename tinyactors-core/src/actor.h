#pragma once

#include "continuation.h"
#include "definition.h"
#include "invoke.h"

typedef enum {
    TA_ACTOR_UNLOADED,
    TA_ACTOR_IDLE,
    TA_ACTOR_RUNNING,
    TA_ACTOR_DONE
} ta_actor_residency;

typedef struct {
    ta_actor_id id;
    ta_definition_id definition;
    ta_atom name;
    ta_actor_residency residency;
    uint32_t generation;
} ta_actor_record;

typedef struct {
    ta_actor_record record;
    const ta_definition *definition;
    ta_configuration configuration;
    ta_history_state history;
    ta_continuation continuation;
    void *datamodel_instance;
    ta_queued_event *internal_events;
    uint32_t internal_count;
    uint32_t internal_capacity;
    ta_active_invoke *invokes;
    uint32_t invoke_count;
    uint32_t invoke_capacity;
} ta_actor;

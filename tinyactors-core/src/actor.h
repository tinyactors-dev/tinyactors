#pragma once

#include "continuation.h"
#include "definition.h"
#include "invoke.h"

typedef struct {
    ta_actor_id id;
    ta_definition_id definition;
    ta_actor_ref self;
    ta_actor_residency residency;
} ta_actor_record;

typedef struct ta_activation_effects ta_activation_effects;

/* Allocated only for actors that use one of these uncommon facilities. */
typedef struct {
    ta_queued_event *internal_events;
    uint32_t internal_count;
    uint32_t internal_capacity;
    ta_active_invoke *invokes;
    uint32_t invoke_count;
    uint32_t invoke_capacity;
    ta_activation_effects *activation_effects;
    ta_actor_parent parent;
    ta_actor_completion completion;
    bool has_parent;
    bool completion_pending;
} ta_actor_sparse_state;

typedef struct {
    ta_actor_record record;
    const ta_definition *definition;
    /* One definition-sized allocation backs configuration and history. */
    void *session_storage;
    ta_configuration configuration;
    ta_history_state history;
    /* Null while stable; allocated only for an in-progress macrostep. */
    ta_continuation *continuation;
    void *datamodel_instance;
    ta_actor_sparse_state *sparse;
    ta_actor_id replacement_of;
    uint64_t session_epoch;
    bool published;
} ta_actor;


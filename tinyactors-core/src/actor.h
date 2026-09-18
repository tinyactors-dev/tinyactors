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
    ta_vec internal_events; /* ta_queued_event, FIFO */
    ta_active_invoke *invokes;
    uint32_t invoke_count;
    uint32_t invoke_capacity;
    /* Parallel to `invokes`; see invoke.h. */
    ta_invoke_meta *invoke_meta;
    /* States entered in this macrostep whose invokes start when it ends. */
    ta_vec pending_invokes;
    ta_activation_effects *activation_effects;
    ta_actor_parent parent;
    ta_actor_completion completion;
    bool has_parent;
    bool completion_pending;
} ta_actor_sparse_state;

struct ta_actor {
    ta_actor_record record;
    const ta_definition *definition;
    /* One definition-sized allocation backs configuration, history, and late-binding bits. */
    void *session_storage;
    size_t session_bytes;
    ta_configuration configuration;
    ta_history_state history;
    /* Late binding: one bit per state whose data has been initialized. */
    uint64_t *late_initialized;
    void *datamodel_instance;
    bool datamodel_initialized;
    ta_actor_sparse_state *sparse;
    ta_actor_id replacement_of;
    uint64_t session_epoch;
    bool published;
    ta_dm_host_impl host_impl;
    ta_dm_host host;

    /* Execution state between microsteps; a macrostep resumes from here. */
    bool bootstrapped;
    bool macrostep;
    bool finished;
    bool has_external_event;
    ta_event external_event;
    bool has_current_event;
    ta_event current_event;
    uint64_t next_send_id;
    /*
     * While the interpreter runs, the full active set it is updating (ancestors
     * included), so In() sees states as SCXML defines them mid-microstep.
     */
    const uint64_t *live_active;
};

typedef struct {
    ta_actor *actor;
    uint32_t generation;
} ta_actor_slot;

ta_actor *ta_actor_lookup(const ta_system *system, ta_actor_id id);
bool ta_actor_state_active(const ta_actor *actor, ta_state_id state);

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
    *system = (tna_system){
        .budgets = {
            .resumes = TNA_DEFAULT_RESUMES_PER_TURN,
            .messages = TNA_DEFAULT_MESSAGE_BUDGET,
            .microsteps = TNA_DEFAULT_RESUME_BUDGET,
            .actions = TNA_DEFAULT_ACTION_BUDGET,
        },
    };
    tna_allocator_init(&system->allocator);
    tna_sched_init(&system->sched, tna_allocator_for(&system->allocator, TNA_MEMORY_SCHEDULING),
                   tna_allocator_for(&system->allocator, TNA_MEMORY_ENVELOPES));
    tna_directory_init(&system->directory, tna_allocator_for(&system->allocator, TNA_MEMORY_DIRECTORY));
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
    (void)ta_system_set_actor_directory(system->core, tna_directory_adapter(&system->directory));
    *out = system;
    return TA_OK;
}

void tna_system_close(tna_system *system) {
    if (system == NULL || system->closed) {
        return;
    }
    system->closed = true;
    system->turn_pending = false;
    /*
     * Nothing may reach the plumbing while core tears down, and every
     * reference the addon holds to a core object (a named origin in an
     * envelope, an address in the directory) goes back first (ADR 0012).
     */
    (void)ta_system_set_actor_directory(system->core, (ta_actor_directory){0});
    tna_sched_discard_mail(&system->sched);
    tna_directory_clear(&system->directory);
    ta_system_destroy(system->core);
    system->core = NULL;
    tna_sched_destroy(&system->sched);
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

bool tna_system_quiet(const tna_system *system) {
    return tna_sched_quiet(&system->sched);
}

void tna_system_want_turn(tna_system *system) {
    if (system == NULL || system->closed || system->turn_pending || tna_system_quiet(system)) {
        return;
    }
    system->turn_pending = true;
    if (system->wake != NULL) {
        system->wake(system, system->wake_context);
    }
}

void tna_system_turn(tna_system *system) {
    system->turn_pending = false;
    if (system->closed) {
        return;
    }
    tna_sched_turn(&system->sched, &(tna_turn_params){
                                       .core = system->core,
                                       .arena = &system->scratch.turn,
                                       .inside_core = &system->inside_core,
                                       .budgets = system->budgets,
                                   });
}

void tna_system_end_turn(tna_system *system) {
    if (system->closed) {
        return;
    }
    system->sched.records = NULL;
    system->sched.last_record = NULL;
    system->sched.record_count = 0;
    tna_scratch_reset_turn(&system->scratch);
    tna_system_want_turn(system);
}

ta_result tna_system_post(tna_system *system, ta_actor_id target, const ta_event *event) {
    if (system == NULL || system->closed) {
        return TA_ERR_INVALID;
    }
    ta_result result = tna_sched_post(&system->sched, target, event);
    if (result == TA_OK) {
        tna_system_want_turn(system);
    }
    return result;
}

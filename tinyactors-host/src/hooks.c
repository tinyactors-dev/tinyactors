#include "tinyactors_host/hooks.h"

ta_result tah_hook_actor_created(tah_system *system, ta_actor_id actor) {
    ta_result result = tah_sched_add(&system->sched, actor);
    if (result != TA_OK) {
        system->inside_core = true;
        (void)ta_actor_destroy(system->core, actor);
        system->inside_core = false;
        return result;
    }
    tah_system_want_turn(system);
    return TA_OK;
}

ta_result tah_hook_actor_spawned(tah_system *system, ta_actor_id actor) {
    /* SPAWNED first, while the actor is as its binding made it (kernel ADR 0002). */
    tah_input_mark mark = { 0 };
    ta_result result = tah_inputs_spawned(system, actor, &mark);
    if (result == TA_OK) {
        result = tah_hook_actor_created(system, actor);
        if (result != TA_OK) {
            tah_inputs_undo(system, &mark);
        }
        return result;
    }
    system->inside_core = true;
    (void)ta_actor_destroy(system->core, actor);
    system->inside_core = false;
    tah_inputs_removed(system, actor);
    return result;
}

ta_result tah_hook_actor_removed(tah_system *system, ta_actor_id actor) {
    tah_inputs_removed(system, actor);
    tah_sched_remove(&system->sched, actor, TAH_DEAD_DESTROYED);
    tah_system_want_turn(system); /* its mail is reported as dead letters */
    return TA_OK;
}

ta_result tah_hook_actor_unloaded(tah_system *system, ta_actor_id actor) {
    tah_sched_remove(&system->sched, actor, TAH_DEAD_UNLOADED);
    tah_system_want_turn(system);
    return TA_OK;
}

ta_result tah_hook_actor_published(tah_system *system, const ta_actor_publish_params *params) {
    ta_result result = TA_OK;
    if (params->incumbent != 0) {
        result = tah_sched_transfer(&system->sched, &(tah_sched_transfer_params){
                                                        .from = params->incumbent,
                                                        .to = params->actor,
                                                    });
    }
    /* A replay's invocations were retired as unavailable (core ADR 0089),
     * which leaves the actor a macrostep to run. */
    ta_actor_inspection view;
    if (result == TA_OK && ta_actor_inspect(system->core, params->actor, &view) == TA_OK &&
        view.macrostep_in_progress) {
        result = tah_sched_wake(&system->sched, params->actor);
    }
    if (result == TA_OK) {
        tah_system_want_turn(system);
    }
    return result;
}

ta_result tah_hook_actor_abandoned(tah_system *system, ta_actor_id actor) {
    /* Core abandoned the macrostep whatever the table knows; an actor the
     * table does not know has nothing to settle. */
    (void)tah_sched_abandoned(&system->sched, actor);
    tah_system_want_turn(system); /* the settlement, and mail it may take now */
    return TA_OK;
}

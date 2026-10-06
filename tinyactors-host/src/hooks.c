#include "tinyactors_host/hooks.h"
#include "tinyactors_host/recorder.h"
#include "system_private.h"

/*
 * The hooks ask for a turn and little else: the inputs of an actor are core's
 * (a new actor's bootstrap waits in its started queue, a destroyed or unloaded
 * actor's inputs are refused when ta_run reaches them, a published actor takes
 * over its incumbent's, an abandoned macrostep is reported by the next
 * ta_run), so the kernel keeps no entry to fill or clear.
 */

ta_result tah_hook_actor_created(tah_system *system, ta_actor_id actor) {
    (void)actor;
    tah_system_want_turn(system);
    return TA_OK;
}

ta_result tah_hook_actor_removed(tah_system *system, ta_actor_id actor) {
    tah_recorder_removed(system, actor);
    tah_system_want_turn(system); /* its inputs are refused, and reported as dead letters */
    return TA_OK;
}

ta_result tah_hook_actor_unloaded(tah_system *system, ta_actor_id actor) {
    (void)actor;
    tah_system_want_turn(system);
    return TA_OK;
}

ta_result tah_hook_actor_published(tah_system *system, const ta_actor_publish_params *params) {
    (void)params;
    /* A replay's invocations were retired as unavailable (core ADR 0089), which leaves the actor a macrostep to run. */
    tah_system_want_turn(system);
    return TA_OK;
}

ta_result tah_hook_actor_abandoned(tah_system *system, ta_actor_id actor) {
    (void)actor;
    tah_system_want_turn(system); /* the settlement, and the inputs that were behind it */
    return TA_OK;
}

#include "hooks.h"

/* Step 5: put the actor on the runnable queue for bootstrap (ADR 0005). */
void tna_hook_actor_created(tna_system *system, ta_actor_id actor) {
    (void)system;
    (void)actor;
}

/* Step 5: drop the actor's mailbox and clear its scheduling entry. */
void tna_hook_actor_removed(tna_system *system, ta_actor_id actor) {
    (void)system;
    (void)actor;
}

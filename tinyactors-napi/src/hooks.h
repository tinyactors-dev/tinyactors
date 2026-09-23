#pragma once
/*
 * Post-call hooks (ADR 0013): bindings.rb names the core functions whose
 * generated binding calls one of these after a successful call, so the
 * plumbing sees the change.
 *
 * - tna_hook_actor_created: after ta_actor_spawn, ta_actor_load,
 *   ta_actor_reconstruct and ta_actor_prepare_replacement. Step 5 puts the
 *   new actor on the runnable queue for bootstrap (ADR 0005).
 * - tna_hook_actor_removed: after ta_actor_destroy and ta_actor_unload.
 *   Step 5 drops the actor's mailbox and clears its scheduling entry.
 *
 * Until step 5 they do nothing: an actor spawned through the generated layer
 * is created but never bootstrapped, because nothing schedules it. That is
 * deliberate; scheduling is not half-implemented here.
 *
 * Hooks take the native system, not a call, so they stay free of Node-API
 * and native tests can drive them.
 */
#include "system.h"
#include "tinyactors.h"

void tna_hook_actor_created(tna_system *system, ta_actor_id actor);
void tna_hook_actor_removed(tna_system *system, ta_actor_id actor);

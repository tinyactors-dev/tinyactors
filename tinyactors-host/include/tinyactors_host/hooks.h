#pragma once
/*
 * Post-call hooks (ADR 0013): bindings.rb names the core functions whose
 * generated binding calls one of these after a successful call, so the
 * plumbing sees the change. A hook that fails makes the call throw with its
 * result.
 *
 * - tah_hook_actor_created: after ta_actor_spawn, ta_actor_load,
 *   ta_actor_reconstruct and ta_actor_prepare. It asks for a turn: the new
 *   actor's bootstrap waits in core's started queue (ADR 0005, core ADR
 *   0099), and the kernel keeps no entry for the actor.
 * - tah_hook_actor_removed: after ta_actor_destroy. It tells the recorder and
 *   asks for a turn: the inputs core still holds for the actor are refused
 *   when ta_run reaches them, and the turn reports them as dead letters.
 * - tah_hook_actor_unloaded: after ta_actor_unload. The same, for an actor
 *   that is out: its inputs are refused with reason `destroyed`. The
 *   JavaScript API refuses to unload an actor that has inputs waiting, so this
 *   only happens through the raw generated layer (ROADMAP step 14).
 * - tah_hook_actor_published: after ta_actor_publish. Core moved the
 *   incumbent's waiting inputs to the published actor (ta_mail_retarget), so
 *   nothing accepted for the address is lost when the incumbent is destroyed
 *   (ROADMAP step 14). It asks for a turn: publishing a replay can leave the
 *   actor a macrostep to run (core ADR 0089).
 * - tah_hook_actor_abandoned: after ta_actor_abandon. The send() that
 *   followed the abandoned macrostep rejects with `abandoned` when the next
 *   ta_run reports the end (ADR 0026, core ADR 0082).
 *
 * Hooks take the native system, not a call, so they stay free of Node-API
 * and native tests can drive them.
 */
#include "system.h"
#include "tinyactors.h"

ta_result tah_hook_actor_created(tah_system *system, ta_actor_id actor);
ta_result tah_hook_actor_removed(tah_system *system, ta_actor_id actor);
ta_result tah_hook_actor_unloaded(tah_system *system, ta_actor_id actor);
ta_result tah_hook_actor_published(tah_system *system, const ta_actor_publish_params *params);
ta_result tah_hook_actor_abandoned(tah_system *system, ta_actor_id actor);

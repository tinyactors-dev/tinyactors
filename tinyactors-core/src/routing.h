#pragma once

#include "address.h"
#include "event.h"
#include "fault.h"
#include "../include/tinyactors.h"

/* External delivery and the router boundary are fully public. */

/*
 * A named ta_actor_ref is a reference to an interned address (ADR 0065).
 * An owner retains it and releases it; both ignore a runtime ref.
 */
void ta_actor_ref_retain(const ta_actor_ref *ref);
void ta_actor_ref_release(ta_actor_ref *ref);

/*
 * Makes a named ref one of this system's handles: a detached address (from a
 * session image) is interned and the ref then holds a new reference. A ref
 * that is already this system's, or a runtime ref, is left alone.
 */
ta_result ta_actor_ref_localize(ta_system *system, ta_actor_ref *ref);
/* Detaches a named ref into `arena`, for an image; a runtime ref is left alone. */
ta_result ta_actor_ref_detach(ta_arena *arena, ta_actor_ref *ref);
/* The origin of an event, when it is an actor. */
ta_actor_ref *ta_event_origin_ref(ta_event *event);

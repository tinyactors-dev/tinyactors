/**
 * @file routing.h
 *
 * # Actor references and external deliveries
 *
 * The private helpers for a `ta_actor_ref`, the public reference to a
 * session: a runtime one (an actor ID) or a named one (an interned
 * address, ADR 0065). The unit also defines the public
 * `ta_external_delivery_retain` and `ta_external_delivery_release`, which
 * a router uses to keep a delivery past its callback.
 *
 * ## Contract and memory
 *
 * - A named ref holds one reference to its address. Retaining and
 *   releasing it count that reference, allocate nothing and cannot fail;
 *   both leave a runtime ref alone.
 * - Localizing turns a detached address (from a session image) into this
 *   system's interned one, which may allocate in the address table and
 *   answers `TA_ERR_NOMEM` when it cannot; detaching copies the address
 *   into a caller's arena for an image.
 * - Retaining a delivery retains its destination and its event's data;
 *   releasing gives both back.
 *
 * ## Users
 *
 * The actor, session images, the interpreter's sends and the host's
 * router. The router contract is described in `docs/internal/routing.md`.
 */
#pragma once

#include "address.h"
#include "event.h"
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

/**
 * @file datamodel.h
 *
 * # The data model's view of its actor
 *
 * A `ta_dm_host` is what core hands a data model so it can read the
 * statechart it serves: `In(state)`, the bound event (`_event`), the
 * session's identity and time, the event's fields and the session's
 * `_ioprocessors` entries as text, and state names. The public header
 * declares the data-model interface and the `ta_dm_host_*` functions; this
 * header defines the view's private layout, `ta_dm_host_impl`, which
 * resolves every question through the calling actor, so a data model never
 * holds a pointer to interpreter internals.
 *
 * ## Contract and memory
 *
 * - Each resident actor owns one `ta_dm_host_impl` and one `ta_dm_host`
 *   pointing at it, set when the actor is made. Both borrow the system and
 *   the actor and live as long as the actor.
 * - A view with no system or actor answers as for none (false, no event,
 *   runtime ID 0, time 0), and its text and state-name calls answer
 *   `TA_ERR_INVALID`; text or a state that is not there answers
 *   `TA_ERR_NOT_FOUND`.
 * - Text the view renders is written into the render area of the running
 *   interpreter call and lives until that call ends; the view itself never
 *   calls the allocator.
 *
 * ## Users
 *
 * The resident actor, which makes the view, and every data model through
 * the public `ta_dm_host_*` functions. The data model's contract is in
 * `docs/internal/datamodel.md` and the `datamodel-*` documents beside it.
 */
#pragma once

#include "../include/tinyactors.h"

typedef struct ta_actor ta_actor;

/* The data-model host view resolves everything through the calling actor. */
typedef struct {
    ta_system *system;
    ta_actor *actor;
} ta_dm_host_impl;

struct ta_dm_host {
    const ta_dm_host_impl *impl;
};

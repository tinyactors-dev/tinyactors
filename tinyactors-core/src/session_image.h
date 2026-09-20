#pragma once

#include "actor.h"

/* Session images are opaque; struct ta_session_image is private to session_image.c. */

/*
 * Restoring an image into a fresh session. This is not public: an image is
 * restored with ta_actor_load(), which is a residency change of a named
 * actor, and a session has to have a name to be restored under (ADR 0051).
 * A reconstructed session gets a new runtime ID, so anything still holding
 * the old one — a delivery the host kept, a session URI a chart saved — is
 * addressing a session that no longer exists. A name is what survives that;
 * an anonymous session has nothing that does.
 */
typedef struct {
    const ta_session_image *image;
    const ta_value *datamodel_binding;
    ta_actor_id *out;
} ta_actor_reconstruct_params;

ta_result ta_actor_reconstruct(ta_system *system, const ta_actor_reconstruct_params *params);

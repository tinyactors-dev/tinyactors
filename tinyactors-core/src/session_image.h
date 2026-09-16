#pragma once

#include "actor.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A session image is an owned in-memory representation of Tinyactors-managed
 * interpreter state.  It is not a persistence wire format and does not contain
 * the host-owned data-model object graph.  Host values referenced by
 * interpreter-owned events, timers, or continuation state are retained as host
 * references according to ta_value lifetime rules.
 */
typedef struct {
    ta_actor_id actor;
    ta_session_image **out;
} ta_actor_capture_params;

typedef struct {
    const ta_session_image *image;
    const ta_value *datamodel_binding;
    ta_actor_id *out;
} ta_actor_reconstruct_params;

ta_result ta_actor_capture(ta_system *system,
                           const ta_actor_capture_params *params);
ta_result ta_actor_reconstruct(ta_system *system,
                               const ta_actor_reconstruct_params *params);
void ta_session_image_destroy(ta_session_image *image);

#ifdef __cplusplus
}
#endif

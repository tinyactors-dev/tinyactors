#pragma once

#include "actor.h"


#ifdef __cplusplus
extern "C" {
#endif

ta_result ta_snapshot_batch_begin(ta_system *system, ta_snapshot_batch **out);
ta_result ta_snapshot_batch_unload(ta_snapshot_batch *batch, ta_actor_id actor);
ta_result ta_snapshot_batch_commit(ta_snapshot_batch *batch);
void ta_snapshot_batch_abort(ta_snapshot_batch *batch);

#ifdef __cplusplus
}
#endif

#include "actor.h"
#include "timer.h"

typedef struct {
    uint32_t version;
    ta_definition_id definition;
    ta_actor_id actor;
    ta_atom name;
    ta_time captured_at;
} ta_snapshot_header;

/*
 * A session snapshot is the complete durable state of one actor.  It includes
 * its actor-owned delayed sends; timers are not a separately persisted unit.
 */
ta_result ta_snapshot_session(ta_system *system,
                              const ta_actor *actor,
                              ta_snapshot_writer *writer);
ta_result ta_restore_session(ta_system *system,
                             ta_actor *actor,
                             ta_snapshot_reader *reader);

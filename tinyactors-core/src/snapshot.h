#pragma once

#include "actor.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "timer.h"

/*
 * A decoded logical checkpoint header. This is not the checkpoint wire layout:
 * writers encode the contained strings and bytes explicitly, never this C
 * struct or its runtime handles. `self_address` is absent for an anonymous
 * runtime actor; the host supplies its address when restoring one.
 */
typedef struct {
    uint32_t format_version;
    ta_definition_identity definition;
    bool has_self_address;
    ta_actor_address self_address;
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

#ifdef __cplusplus
}
#endif

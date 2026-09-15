#pragma once

#include "actor.h"
#include "timer.h"

struct ta_snapshot_writer {
    void *self;
    ta_result (*write)(void *self, const void *data, size_t size);
};

struct ta_snapshot_reader {
    void *self;
    ta_result (*read)(void *self, void *data, size_t size);
};

typedef struct {
    uint32_t version;
    ta_definition_id definition;
    ta_actor_id actor;
    ta_atom name;
    ta_time captured_at;
} ta_snapshot_header;

ta_result ta_snapshot_actor(ta_system *system,
                            const ta_actor *actor,
                            ta_snapshot_writer *writer);
ta_result ta_restore_actor(ta_system *system,
                           ta_actor *actor,
                           ta_snapshot_reader *reader);

ta_result ta_snapshot_actor_timers(const ta_system *system,
                                   ta_actor_id actor,
                                   ta_snapshot_writer *writer);
ta_result ta_restore_actor_timers(ta_system *system,
                                  ta_actor_id actor,
                                  ta_snapshot_reader *reader);

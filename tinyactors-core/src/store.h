#pragma once

#include "snapshot.h"

typedef struct ta_store_vtable {
    ta_result (*save)(void *self,
                      ta_actor_id actor,
                      ta_snapshot_writer *writer);
    ta_result (*load)(void *self,
                      ta_actor_id actor,
                      ta_snapshot_reader *reader);
    ta_result (*remove)(void *self, ta_actor_id actor);
} ta_store_vtable;

typedef struct {
    void *self;
    const ta_store_vtable *v;
} ta_store;

#pragma once

#include "types.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef struct { void *self; ta_result (*write)(void *self, const void *data, size_t size); } ta_snapshot_writer;
typedef struct { void *self; ta_result (*read)(void *self, void *data, size_t size); } ta_snapshot_reader;
typedef struct { void *self; ta_snapshot_writer writer; } ta_store_save;
typedef struct { void *self; ta_snapshot_reader reader; } ta_store_load;
typedef struct { void *self; } ta_store_batch;
typedef struct { ta_store_batch *batch; ta_actor_id actor; ta_store_save *out; } ta_store_begin_save_params;
typedef struct { ta_store_batch *batch; ta_actor_id actor; } ta_store_remove_params;
typedef struct ta_store_vtable {
    ta_result (*begin_batch)(void *self, ta_store_batch *out);
    ta_result (*begin_save)(void *self, const ta_store_begin_save_params *params);
    ta_result (*stage_remove)(void *self, const ta_store_remove_params *params);
    ta_result (*commit_batch)(void *self, ta_store_batch *batch);
    void (*abort_batch)(void *self, ta_store_batch *batch);
    ta_result (*open_load)(void *self, ta_actor_id actor, ta_store_load *out);
    void (*close_load)(void *self, ta_store_load *load);
} ta_store_vtable;
typedef struct { void *self; const ta_store_vtable *v; } ta_store;

#ifdef __cplusplus
}
#endif

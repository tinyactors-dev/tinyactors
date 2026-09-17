#pragma once

#include "value.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef enum { TA_EVENT_EXTERNAL = 0, TA_EVENT_INTERNAL = 1, TA_EVENT_PLATFORM = 2 } ta_event_type;
typedef enum { TA_ORIGIN_NONE = 0, TA_ORIGIN_ACTOR = 1, TA_ORIGIN_IO = 2, TA_ORIGIN_INVOKE = 3 } ta_origin_kind;
typedef struct { ta_origin_kind kind; union { ta_actor_ref actor; ta_invoke_id invoke; uint64_t opaque; } as; } ta_origin;
typedef struct { ta_event_name_id name; ta_event_type type; ta_send_id send_id; ta_origin origin; ta_invoke_id invoke_id; ta_value data; } ta_event;
/* invoke_id is TA_INVOKE_ID_NONE unless an invoked child generated the event. */
typedef struct { ta_event_name_id detail; int32_t code; ta_str message; ta_value data; } ta_fault;
typedef enum { TA_OP_OK = 0, TA_OP_ERROR = 1 } ta_op_status;
typedef enum { TA_RUN_STABLE = 0, TA_RUN_PAUSED = 1, TA_RUN_DONE = 2, TA_RUN_ERROR = 3 } ta_run_status;

#ifdef __cplusplus
}
#endif

#include "event_name.h"
#include "value.h"

typedef struct {
    ta_event event;
    uint32_t flags;
} ta_queued_event;

enum {
    TA_EVENT_FLAG_OWNS_DATA = 1u << 0,
    TA_EVENT_FLAG_GENERATED = 1u << 1
};

ta_result ta_event_clone(const ta_event *event, ta_event *out);
void ta_event_release(ta_event *event);

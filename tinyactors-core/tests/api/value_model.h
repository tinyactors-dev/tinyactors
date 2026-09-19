// expect error: ta_deliver: takes ta_value inputs; add TA_VALUE_MODEL(kind, path) or TA_ANY_VALUE_MODEL
// expect error: ta_send: value model path actor is not a ta_actor_id
#include <stdint.h>
typedef uint32_t ta_other_id;
typedef struct ta_value ta_value TA_HOST_VALUE;
struct ta_value { void *self; void *v; };
typedef struct { ta_value data; } ta_event;
TA_API void ta_deliver(TA_IN const ta_event *event);
TA_API void ta_send(ta_other_id actor, TA_IN const ta_event *event) TA_VALUE_MODEL(actor, actor);

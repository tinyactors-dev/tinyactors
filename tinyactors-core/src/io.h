#pragma once

#include "event.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef struct { ta_actor_id source; ta_send_id send_id; ta_event_name_id event; ta_atom io_type; ta_value target; ta_value data; } ta_send;
typedef struct ta_io_vtable { ta_op_status (*send)(void *self, const ta_send *send, ta_fault *fault); } ta_io_vtable;
typedef struct { void *self; const ta_io_vtable *v; } ta_io;

#ifdef __cplusplus
}
#endif

#include "fault.h"

typedef struct {
    ta_atom type;
    ta_io io;
} ta_io_registration;

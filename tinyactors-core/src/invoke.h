#pragma once

#include "event.h"
#include "fault.h"

typedef struct {
    ta_atom type;
    ta_invoker invoker;
} ta_invoker_registration;

/*
 * Private bookkeeping for one active invocation, kept parallel to the public
 * ta_active_invoke array of the actor's sparse state. `child` is set when the
 * service spawned an SCXML session with this invocation as its parent.
 */
typedef struct {
    ta_invoke_decl_id decl;
    ta_state_id state;
    ta_invoker invoker;
    ta_actor_id child;
    bool has_child;
} ta_invoke_meta;

/* The send ID of a child's queued done.invoke completion record. */
#define TA_SEND_ID_COMPLETION (UINT64_MAX - 1u)

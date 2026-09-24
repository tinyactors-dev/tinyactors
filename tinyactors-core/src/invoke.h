#pragma once

#include "datamodel.h"
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

/*
 * Retires an invocation whose service did not survive a session image, and
 * queues its error.communication.invoke.<id>.unavailable event.
 */
ta_result ta_invoke_report_lost(ta_system *system, ta_actor *actor, ta_invoke_id invoke);

/* The origin of an event from `invoke`: its ID and its invoker's registry index (ADR 0068). */
ta_origin ta_invoke_origin_of(const ta_system *system, const ta_active_invoke *invoke);
/* The origin of an event from the actor's active invocation `invoke`; false when there is none. */
bool ta_invoke_origin_for(const ta_system *system, const ta_actor *actor, ta_invoke_id invoke, ta_origin *out);

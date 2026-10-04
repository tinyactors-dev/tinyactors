#pragma once

#include "datamodel.h"
#include "event.h"

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
    /* Started while its actor replayed (ADR 0089): no invoker was called, so
     * none is cancelled or forwarded to, and nothing runs behind it. */
    bool replayed;
} ta_invoke_meta;

/* The send ID of a child's queued done.invoke completion record. */
#define TA_SEND_ID_COMPLETION (UINT64_MAX - 1u)

/*
 * Retires an invocation whose service did not survive a session image, and
 * queues its error.communication.invoke.<id>.unavailable event.
 */
ta_result ta_invoke_report_lost(ta_system *system, ta_actor *actor, ta_invoke_id invoke);
/*
 * Publishing a replaying actor retires its active invocations (ADR 0089
 * section 6). The first makes room for their unavailable events, and
 * changes nothing visible when it fails; the second then queues them and
 * forgets the invocations, and cannot fail. It answers whether it queued one.
 */
/*
 * A replaying actor restored from a checkpoint (ADR 0091) takes back its
 * active invocations as replayed ones: active in core, with no invoker
 * called, so the services' later events still match them.
 */
typedef struct {
    ta_invoke_id id;
    ta_atom type;
    ta_invoke_decl_id decl;
} ta_invoke_restore_params;
ta_result ta_invoke_restore(ta_system *system, ta_actor *actor, const ta_invoke_restore_params *params);
/* Whether `invoke` is one of the actor's active invocations. */
bool ta_invoke_is_active(const ta_actor *actor, ta_invoke_id invoke);
ta_result ta_invoke_prepare_retire(ta_system *system, ta_actor *actor);
bool ta_invoke_retire_all(ta_system *system, ta_actor *actor);

/* The origin of an event from `invoke`: its ID and its invoker's registry index (ADR 0068). */
ta_origin ta_invoke_origin_of(const ta_system *system, const ta_active_invoke *invoke);
/* The origin of an event from the actor's active invocation `invoke`; false when there is none. */
bool ta_invoke_origin_for(const ta_system *system, const ta_actor *actor, ta_invoke_id invoke, ta_origin *out);

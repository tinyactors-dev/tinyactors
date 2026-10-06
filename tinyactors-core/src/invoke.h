#pragma once

/*
 * invoke: the active invocations of a session (SCXML <invoke>), and the
 * reports that services make about them.
 *
 * Purpose. A state that is entered with an <invoke> starts a service through a
 * registered invoker at the end of the macrostep; the service lives while the
 * state is active, and ends when the state exits (cancel), when it finishes
 * (done.invoke, <finalize>) or when it reports an error. This unit owns the
 * bookkeeping that ties those together: the invoke table, ID interning,
 * autoforwarding and the application of error reports (ADR 0081, ADR 0089).
 *
 * Lifetime. Everything here belongs to a session. An invocation's record
 * (ta_invoke_meta, kept parallel to the public ta_active_invoke array of the
 * actor's sparse state) is allocated when the session starts the invocation
 * (or restores it, ta_invoke_restore) and freed when the invocation is
 * retired or the session is destroyed (ta_invoke_release). Nothing may hold a
 * pointer into the table past the macrostep that found it: the table moves
 * when it grows. An error report is an envelope (lifetime: envelope) from its
 * admission until ta_run takes it; ta_invoke_report_take consumes what it
 * needs and the envelope keeps its own event references.
 *
 * Allocation. Starting and restoring an invocation, and interning an ID,
 * allocate through the system's allocator, at the safe points the catalog
 * names (the end of a macrostep, ta_run's take, restore). The query functions
 * (ta_invoke_table_bytes, ta_invoke_table_decl, ta_invoke_is_active,
 * ta_invoke_origin_of, ta_invoke_origin_for) never allocate. Sizes are
 * computed with checked arithmetic, and a failed allocation leaves the
 * session as it was. One known exception: admitting an error report interns
 * the event name error.communication.invoke.<id>.<reason> (see
 * ta_invoke_report_error in invoke.c), which can allocate inside the parent's
 * macrostep when the invoker's start fails; it answers a clean TA_ERR_NOMEM
 * with nothing queued, and it goes with the name area in unit 4.2b.
 *
 * Invariants. An invocation is active from its start until it is retired
 * exactly once; its ID is the interned text's atom + 1, so the text is always
 * recoverable; a replayed invocation (ADR 0089) has no invoker called and
 * nothing running behind it. A report for an invocation that is no longer
 * active is dropped (TA_ERR_NOT_FOUND), never applied to a successor.
 *
 * Complexity. Lookup of an invocation by ID is linear in the session's active
 * invocations, which is small by construction (bounded by the definition's
 * <invoke> elements in the active configuration).
 *
 * Usage. The interpreter calls ta_invoke_state_entered and
 * ta_invoke_state_exited as states change and ta_invoke_start_pending at the
 * end of the macrostep. A service reports through the public
 * ta_invoke_report API, which admits an envelope; ta_run then calls
 * ta_invoke_report_take, which queues the report's event and starts the
 * macrostep that handles it.
 */

#include "datamodel.h"
#include "event.h"
#include "envelope.h"

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

/*
 * ta_invoke_table_bytes is the size of the block behind an actor's invoke
 * table (0 when it has none): the figure ta_actor_inspect adds to the
 * actor's allocated bytes. It never allocates.
 */
size_t ta_invoke_table_bytes(const ta_columns *table);

/*
 * ta_invoke_table_decl is the declaration (index into the definition's
 * invokes) of the invoke at `index`, which must be below the table's count.
 * It never allocates.
 */
ta_invoke_decl_id ta_invoke_table_decl(const ta_columns *table, uint32_t index);

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

/*
 * ta_invoke_report_take applies the invocation error report in `envelope`
 * (TA_ENVELOPE_REPORT) to `actor` when ta_run takes it: it queues the
 * report's event on the session's internal queue (the envelope keeps its own
 * references), counts and records it as raised outside a macrostep, retires
 * the invocation if the report ends it, moves a replaying session's clock to
 * the report's time (ADR 0094) and starts a macrostep under the report's
 * trace. TA_ERR_NOT_FOUND: the invocation is not active any more; nothing
 * changed. TA_ERR_NOMEM: the cell for the started macrostep, or the queue,
 * could not be had; nothing changed (the report can be taken again). Allocates
 * only for those, at ta_run's take, which is a safe point.
 */
ta_result ta_invoke_report_take(ta_system *system, ta_actor *actor, ta_envelope *envelope);

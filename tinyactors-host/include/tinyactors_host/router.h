#pragma once
/*
 * Where mail goes that core cannot deliver itself (ADR 0006, core ADR 0099).
 * Core keeps mail between sessions: to a resident one in its pool, and to
 * one that is not resident waiting for the host's answer to a load request.
 * This module answers those requests and posts the mail JavaScript sends by
 * name. It has no Node-API in it, so native tests drive it.
 *
 * tah_router_answer() decides, for one request core owes (ta_load_request):
 * - a runtime ID of a session that is gone or finished, whose family has a
 *   loader: a load keyed by the session (loads.h, napi ADR 0032 section 2);
 * - a named address with a load in flight, or whose family has a loader: a
 *   load for the address (napi ADR 0020);
 * - a named address resident again since core asked: the mail goes to it;
 * - anything else has nowhere to go (napi ADR 0022): each message becomes
 *   error.communication for its sender, with the reason (`not-found`,
 *   `unloaded`, `done`) and the target as its data.
 */
#include "scheduler.h"
#include "tinyactors.h"

typedef struct tah_system tah_system;

/* Answers one load request core owes; see the file comment. */
void tah_router_answer(tah_system *system, const ta_load_request *request);

/* Why a load request is answered with a failure, and for which target. */
typedef struct {
    tah_dead_reason reason;
    const ta_actor_ref *destination;
} tah_router_failure;

/*
 * Answers load request `request` with a failure: each message waiting for
 * it becomes error.communication for its sender, carrying `{ reason,
 * target }` from the system's unreachable_data callback.
 */
void tah_router_fail(tah_system *system, uint64_t request, const tah_router_failure *failure);

/*
 * Accepts a message JavaScript posts for a named destination: the waiting
 * mailbox of a load in flight for it, or of a new load when its family has
 * a loader and it has no resident actor (napi ADR 0020); otherwise the
 * resident actor's mailbox, or a dead letter when the name has none. `message->target`
 * is ignored. On TA_OK the system owns the event's references and the waiter;
 * `address` stays the caller's. TA_ERR_FULL: core's pool is at its
 * admission watermark and the message may not be held
 * (tah_message.may_overflow); TA_ERR_NOMEM: nothing accepted.
 */
ta_result tah_router_post_named(tah_system *system, ta_address *address, const tah_message *message);

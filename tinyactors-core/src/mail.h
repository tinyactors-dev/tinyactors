#pragma once

#include "types.h"
#include "event.h"
#include "routing.h"
#include "trace.h"

/*
 * Package mail turns a <send> to a local session into an envelope, and
 * decides what happens to that envelope until its receiver takes it (ADR 0099
 * sections 4 and 6). It is policy over package envelope (the pool, the run
 * queue, the quotas) and package run (the slot that holds the running input);
 * it adds no storage of its own.
 *
 * What it is for. Mail between sessions used to leave core through the host's
 * router, which could only accept, ask to be offered the event again, or fail
 * after the fact (ADR 0040). A send could not be refused in the macrostep
 * that made it, and a full mailbox overflowed without bound. Here the
 * sender's own input pays for the mail it sends, from a claim made before the
 * input started (the macrostep reserve), so a send inside a macrostep is
 * either an envelope or a clean refusal in that macrostep.
 *
 * What it does, in the order a mail lives.
 *
 *   1. ta_mail_send, in the sender's macrostep: find the receiver without
 *      allocating, take one envelope from the running input's claim, fill it
 *      (the event is retained, not copied: ADR 0050), and append it to the
 *      input's outgoing list in send order. Nothing else sees it yet.
 *   2. ta_mail_macrostep_ended, when the input completes: the whole outgoing
 *      list is spliced onto the end of the run queue in one step, so a
 *      receiver never takes mail from a macrostep that has not ended, and one
 *      macrostep's mail keeps its order.
 *      ta_mail_macrostep_discarded instead, when the input is abandoned or
 *      reaches the microstep limit, or its session goes: the list is freed
 *      unseen (ADR 0099 section 4; ADR 0082).
 *   3. ta_run takes the envelope like any input. A receiver that is gone or
 *      has finished does not get it: ta_mail_recycle turns the envelope into
 *      the data-free error.communication its sender takes (section 6).
 *
 * Lifetimes. Each thing here has exactly one.
 *
 *   - call: a ta_mail_params and the answer. The destination and the event
 *     are borrowed for the call; on every answer the caller still owns what it
 *     passed, because the envelope retains its own references.
 *   - input: the outgoing list and the claim it pays from, both in the run
 *     slot (run.h), from the input's take until its COMPLETED notice.
 *   - envelope: a mail envelope, from ta_mail_send until the receiver's input
 *     completes (ta_envelope_release), or until it is recycled; a recycled
 *     envelope is the same envelope, now an error for the sender, and lives
 *     until the sender's input completes. Anything holding its index (the
 *     outgoing list, then the run queue, then the slot) drops it first.
 *   - system: nothing. This package keeps no state between calls.
 *
 * When it allocates. Never. Every function here is allowed inside a
 * macrostep: ta_mail_send takes an envelope that the pool already holds, under
 * the claim, and answers TA_MAIL_FULL when there is none (the claim is spent,
 * or the pool is at its bound). The pool grows only at the safe points the
 * envelope package names (admission, before an input is taken, at a due
 * dispatch, at a publish), so a fanout fails at the same send whether the
 * system is cold or warm. Event names are interned trie IDs in this step, so
 * a recycled error needs no name storage; the per-envelope reservation of
 * ADR 0099 section 6 arrives with the name area (ADR 0100).
 *
 * Invariants.
 *   M1: an envelope on the outgoing list is RUNNABLE, made from the claim,
 *       and on no other list; its receiver's `queued` counts it.
 *   M2: the quota stays balanced (envelope.h I1): every outgoing envelope is
 *       "used" from the moment it is made; the claim holds only the rest.
 *   M3: after an input completes, its outgoing list is empty and its claim is
 *       zero, whichever way it completed.
 *   M4: the order of mail from one sender to one receiver is the order of the
 *       sends: lists are appended in send order and spliced in one piece.
 *
 * Complexity. ta_mail_send is O(1) (a table lookup for a named destination).
 * ta_mail_macrostep_ended is O(1). ta_mail_macrostep_discarded is
 * O(outgoing envelopes), the cost of freeing them. ta_mail_recycle is O(1).
 *
 * Failure. ta_mail_send changes nothing unless it answers TA_MAIL_QUEUED.
 * No function here fails after it has begun to change something.
 *
 * Transitional. Until the router is deleted (the rest of unit 4.2c), mail
 * whose receiver this package cannot place yet, a session that is not
 * resident, goes to the external event router as before: ta_mail_send answers
 * TA_MAIL_ROUTER and has changed nothing.
 *
 * Example (inside a macrostep):
 *
 *	ta_mail_params mail = { .sender = actor->record.id, .destination = destination,
 *	                        .event = event, .trace = trace };
 *	switch (ta_mail_send(system, &mail)) {
 *	case TA_MAIL_QUEUED: break;                     // visible to the receiver when this macrostep ends
 *	case TA_MAIL_FULL:   raise_execution_error(); break;
 *	case TA_MAIL_ROUTER: offer_to_router(); break;  // transitional
 *	}
 *	... later, when the input completes ...
 *	ta_mail_macrostep_ended(system);
 */

/* ta_mail_status is the answer of ta_mail_send. */
typedef enum ta_mail_status {
    TA_MAIL_QUEUED = 0, /* an envelope is on the running input's outgoing list */
    TA_MAIL_FULL,       /* no envelope: the claim is spent, the pool is at its bound, or the receiver has too many; nothing retained */
    TA_MAIL_ROUTER      /* not placed here (no input is running, or the receiver is not a resident session); nothing retained */
} ta_mail_status;

/*
 * ta_mail_params is the request of ta_mail_send. Lifetime: call. Everything
 * in it is borrowed: the destination (a named reference is retained by
 * nobody, because the envelope stores the resolved session), and the event,
 * whose data, origin and invoke ID the envelope retains for itself.
 */
typedef struct ta_mail_params {
    ta_actor_id sender;       /* the sending session, whom an undeliverable mail is reported to; 0 for none */
    ta_actor_ref destination; /* a runtime reference (session routing is done) or a named one */
    ta_event event;           /* name, type, send ID, origin, invoke ID and data, as the receiver is to see them */
    ta_trace_parent trace;    /* the sending step, which the receiver's macrostep runs under (ADR 0067) */
} ta_mail_params;

/*
 * ta_mail_send makes an envelope for `params->event` and appends it to the
 * running input's outgoing list, if a resident, unfinished session is the
 * destination. It is for use inside a macrostep: it answers TA_MAIL_ROUTER
 * when no input is running. The receiver's `queued` count includes the mail
 * from now on, so the session is busy for unload and export (actor.h).
 *
 * It never allocates. TA_MAIL_FULL says the claim is spent, or the pool has
 * no free envelope or no quota, or the receiver already has UINT16_MAX inputs
 * queued; the sender's macrostep reports it as error.execution (it is a
 * quota refusal, like maxPendingSends). O(1).
 */
ta_mail_status ta_mail_send(ta_system *system, const ta_mail_params *params);

/*
 * ta_mail_macrostep_ended makes the running input's mail visible: its outgoing
 * list joins the end of the run queue in one step, and the rest of its claim
 * goes back to the quota. Call it as the input completes, before the slot is
 * cleared. It never allocates and visits no envelope. O(1).
 */
void ta_mail_macrostep_ended(ta_system *system);

/*
 * ta_mail_macrostep_discarded frees the running input's mail unseen and gives
 * its claim back: the input was abandoned, ran away, or its session was
 * destroyed. Call it before the slot is cleared. Receivers' `queued` counts
 * fall by one per envelope freed. It never allocates. O(envelopes freed).
 */
void ta_mail_macrostep_discarded(ta_system *system);

/*
 * ta_recycle_reason says why an envelope could not be delivered; it picks the
 * words of nothing yet (an error.communication is data-free) but keeps the
 * rows of ADR 0099 section 6 apart for the callers and the tests.
 */
typedef enum ta_recycle_reason {
    TA_RECYCLE_DESTROYED, /* the receiver was destroyed with the mail waiting */
    TA_RECYCLE_DONE       /* the receiver finished while the mail waited */
} ta_recycle_reason;

/*
 * ta_mail_recycle reports the undeliverable mail envelope `index`, which is
 * on no list, to its sender (ADR 0099 section 6). The envelope becomes the
 * error: its event is released and replaced by a data-free
 * error.communication carrying the send's ID, its target is the sender, and it
 * joins the end of the run queue. When the sender has finished or is gone,
 * the envelope is freed instead. Never allocates, never fails. O(1).
 */
void ta_mail_recycle(ta_system *system, uint32_t index, ta_recycle_reason reason);

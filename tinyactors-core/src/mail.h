#pragma once

#include "types.h"
#include "event.h"
#include "routing.h"
#include "trace.h"

typedef struct ta_actor ta_actor;

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
 *      the data-free error.communication its sender takes (section 6). A
 *      receiver that is out (unloaded) makes the envelope wait for its
 *      address instead (step 4).
 *   4. Waiting for an address (section 4). An input whose target is a named
 *      address with no resident session joins that address's waiting list
 *      (ta_mail_post_waiting): host input from ta_actor_deliver with an
 *      address, and mail whose receiver was unloaded before it was taken. The
 *      first envelope of a list raises a cue, which ta_run writes as a
 *      LOAD_NEEDED notice. The list then leaves in one piece, in arrival
 *      order: to the run queue when a session becomes resident at the address
 *      (ta_mail_address_resident, called by spawn, publish and load), or
 *      recycled when the host answers ta_address_load_failed. A replacement
 *      that takes over an address brings the incumbent's queued inputs with it
 *      (ta_mail_retarget).
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
 *   - system: nothing of its own. The waiting lists, the table that finds
 *     them and the cue lists are package envelope's and package run's; this
 *     package decides when an envelope moves between them.
 *
 * When it allocates. Never. (ta_actor_deliver, the one caller that adds an
 * envelope to a waiting list from outside a macrostep, refills the pool first,
 * at its own safe point; a waiting list is found by a table that has a slot
 * for every envelope, so joining it cannot fail.) Every function here is allowed inside a
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
 *   M5: a WAITING envelope is on exactly one waiting list, holds a reference to
 *       its address, has target 0 and is counted by no session's `queued`;
 *       its list is on the cue list (`loads`) exactly while the LOAD_NEEDED
 *       notice for it has not been written.
 *   M6: a waiting list leaves whole and in order: every envelope goes to the
 *       run queue (retargeted, counted in `queued`) or is recycled, and no
 *       envelope of the list is left behind or reordered.
 *
 * Complexity. ta_mail_send is O(1) (a table lookup for a named destination).
 * ta_mail_macrostep_ended is O(1). ta_mail_macrostep_discarded is
 * O(outgoing envelopes), the cost of freeing them. ta_mail_recycle and
 * ta_mail_post_waiting are O(1). ta_mail_address_resident and
 * ta_address_load_failed are O(list moved) plus O(pending cues) to drop the
 * list's cue. ta_mail_retarget is O(pending envelopes), and is for the rare,
 * host-initiated replacement.
 *
 * Failure. ta_mail_send changes nothing unless it answers TA_MAIL_QUEUED.
 * No function here fails after it has begun to change something, and none of
 * the recycling and waiting functions can fail at all: they only move
 * envelopes the pool already holds.
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
 *	case TA_MAIL_QUEUED: break;                      // visible to the receiver when this macrostep ends
 *	case TA_MAIL_UNDELIVERABLE: raise_communication_error(); break;
 *	case TA_MAIL_FULL:   raise_execution_error(); break;
 *	case TA_MAIL_ROUTER: offer_to_router(); break;  // transitional
 *	}
 *	... later, when the input completes ...
 *	ta_mail_macrostep_ended(system);
 */

/* ta_mail_status is the answer of ta_mail_send. */
typedef enum ta_mail_status {
    TA_MAIL_QUEUED = 0,   /* an envelope is on the running input's outgoing list */
    TA_MAIL_UNDELIVERABLE,/* the receiver is a name that a finished session holds: the sender hears error.communication in this macrostep (SCXML 6.2.4); nothing retained */
    TA_MAIL_FULL,         /* no envelope: the claim is spent, the pool is at its bound, or the receiver has too many; nothing retained */
    TA_MAIL_ROUTER        /* not placed here (no input is running, or the receiver is not a resident session); nothing retained */
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
 * running input's outgoing list, if a resident session is the destination. It
 * is for use inside a macrostep: it answers TA_MAIL_ROUTER when no input is
 * running. The receiver's `queued` count includes the mail
 * from now on, so the session is busy for unload and export (actor.h).
 *
 * A named receiver that is a resident session which has finished is
 * undeliverable now, and known to be: the answer is TA_MAIL_UNDELIVERABLE, and
 * the caller
 * raises error.communication in the sender's own queue, in this macrostep, as
 * SCXML 6.2.4 asks. Mail that waits and finds its receiver gone or finished
 * when it is taken is recycled instead (ta_mail_recycle).
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
 * ta_mail_recycle reports the undeliverable envelope `index`, which is on no
 * list and not WAITING, as ADR 0099 section 6 says, for one of the
 * TA_REFUSED_ reasons (the receiver was destroyed or finished, an input time
 * is no longer valid, or the host's answer to a load was a failure).
 *
 *   - mail from a session (source MAIL or ERROR): the envelope becomes the
 *     error. Its event is released and replaced by a data-free
 *     error.communication carrying the send's ID, its target is the sender, and
 *     it joins the end of the run queue. The sender is told the same whatever
 *     the reason, because SCXML has one error for a message that did not
 *     arrive; the reason is for host input only. When the sender has finished
 *     or is gone, the envelope is freed instead.
 *   - host input (source HOST): the envelope goes on the runner's `refused`
 *     list with `reason` in its flags, and the next ta_run writes an
 *     INPUT_REFUSED notice for it, lends its event, and releases it.
 *
 * Never allocates, never fails. O(1).
 */
void ta_mail_recycle(ta_system *system, uint32_t index, ta_refusal_reason reason);

/*
 * ta_mail_resident_at answers the session that the host's directory says is
 * resident at `address`, or NULL when it says anything else (unloaded, no
 * entry) or names an ID that is not a session of this system. The session may
 * have finished. It never allocates and retains nothing. O(1) plus the
 * directory's own cost.
 */
ta_actor *ta_mail_resident_at(const ta_system *system, ta_address *address);

/*
 * ta_mail_post_waiting puts the made envelope `index`, which is on no list,
 * on the waiting list of `address`: it becomes WAITING with target 0 and its
 * own reference to the address, and, when it starts the list, its cue joins
 * the runner's `loads` list for the next ta_run to report. The caller has
 * counted nothing in any session's `queued`. It never allocates and cannot
 * fail after a pool refill (envelope.h I4); if the table refuses all the same
 * (a caller bug) the envelope is recycled as TA_REFUSED_LOAD_FAILED so nothing
 * is lost or leaked. O(1) expected.
 */
void ta_mail_post_waiting(ta_system *system, uint32_t index, ta_address *address);

/*
 * ta_mail_address_resident tells the package that a session became resident
 * at `address` (spawn with the address, ta_actor_publish, ta_actor_load): the
 * address's waiting list, if any, moves in arrival order to the end of the run
 * queue, retargeted to `actor` and counted in its `queued`, and the list's
 * pending cue, if it has not been reported yet, is dropped. An envelope the
 * session cannot take (it finished, or already has UINT16_MAX inputs queued)
 * is recycled with TA_REFUSED_DONE or TA_REFUSED_INVALID instead. Nothing
 * waits, so it does nothing, in O(1). Never allocates.
 */
void ta_mail_address_resident(ta_system *system, const ta_address *address, ta_actor_id actor);

/*
 * ta_mail_retarget hands over the inputs queued for a replaced session
 * (ta_actor_publish with an incumbent): every host input and mail envelope
 * addressed to `from`, on the run queue or on the running input's outgoing
 * list, now goes to `to`, in the same order, and the two sessions' `queued`
 * counts follow. Errors that `from` is to hear stay with it: they report on
 * what `from` sent. When `to` reaches UINT16_MAX inputs the rest stay with
 * `from` and are refused when they come to the head. Never allocates. O(pending
 * envelopes), because mail is addressed by session and not found by name.
 */
void ta_mail_retarget(ta_system *system, ta_actor_id from, ta_actor_id to);

/*
 * ta_mail_outgoing_to answers how many envelopes on the running input's
 * outgoing list are addressed to `actor`, when `actor` is the session running
 * that input; 0 otherwise. An unload of a session paused inside its macrostep
 * does not wait for the mail that macrostep sent to itself: the list is made
 * visible when the session leaves, and that mail finds its receiver out and
 * waits for its address (ta_mail_follow). It never allocates. O(outgoing envelopes), and only for the session in the slot.
 */
uint32_t ta_mail_outgoing_to(const ta_system *system, ta_actor_id actor);

/* ta_mail_follow_status is the answer of ta_mail_follow. */
typedef enum ta_mail_follow_status {
    TA_MAIL_FOLLOWED = 0, /* the session is resident under another runtime ID: *out is it, and counts the envelope */
    TA_MAIL_WAITS,        /* the session is out, with an address: the envelope now waits for it (consumed) */
    TA_MAIL_GONE          /* the session is gone, or cannot take it: the caller recycles the envelope */
} ta_mail_follow_status;

/*
 * ta_mail_follow finds where the envelope `index` goes now, for one whose
 * target runtime ID names no resident session. A session keeps its ID across
 * unload and load (ADR 0066), so mail that waited while its receiver was out
 * and back in follows it to its new runtime ID (TA_MAIL_FOLLOWED: the
 * envelope's target is the new ID and the receiver's `queued` count includes
 * it). Mail that finds its receiver out waits for the receiver's address
 * (TA_MAIL_WAITS, ta_mail_post_waiting: the first of a list raises LOAD_NEEDED).
 * A session that is gone for good, or whose queue is full, is TA_MAIL_GONE and
 * the envelope is untouched. It never allocates. O(1).
 */
ta_mail_follow_status ta_mail_follow(ta_system *system, uint32_t index, ta_actor **out);

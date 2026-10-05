#pragma once

#include "types.h"
#include "event.h"
#include "routing.h"
#include "trace.h"

typedef struct ta_actor ta_actor;

/*
 * Package mail turns a <send> into an envelope, and decides what happens to
 * that envelope until its receiver takes it (ADR 0099 sections 4 and 6). It is
 * policy over package envelope (the pool, the run queue, the waiting lists,
 * the quotas) and package run (the slot that holds the running input); it
 * adds no storage of its own.
 *
 * What it is for. Mail between sessions used to leave core through the host's
 * router, which could only accept, ask to be offered the event again, or fail
 * after the fact (ADR 0040). A send could not be refused in the macrostep
 * that made it, a full mailbox overflowed without bound, and a send to a
 * session that was not resident was the host's problem. Here the sender's own
 * input pays for the mail it sends, from a claim made before the input started
 * (the macrostep reserve), so a send inside a macrostep is either an envelope
 * or a clean refusal in that macrostep; and core itself keeps the mail of a
 * session that is not resident until the host has loaded it or says it cannot.
 *
 * What it does, in the order a mail lives.
 *
 *   1. ta_mail_send, in the sender's macrostep: find the receiver without
 *      allocating, take one envelope from the running input's claim, fill it
 *      (the event is retained, not copied: ADR 0050), and append it in send
 *      order to one of two lists of the input: `outgoing` for a resident
 *      receiver, `outgoing_waiting` for a destination that is not resident (a
 *      name nobody holds now, or a session that is gone or has finished and
 *      whose family may have a loader). Nothing else sees it yet.
 *   2. ta_mail_macrostep_ended, when the input completes: the whole `outgoing`
 *      list is spliced onto the end of the run queue in one step, so a
 *      receiver never takes mail from a macrostep that has not ended, and one
 *      macrostep's mail keeps its order. The `outgoing_waiting` list is then
 *      placed envelope by envelope, in send order: to the run queue if its
 *      address has become resident meanwhile, else onto the address's waiting
 *      list (step 4). A session that is gone is given its key address here,
 *      interned from the family and the session's text; if that fails for want
 *      of memory the mail is recycled, the one place this function allocates.
 *      ta_mail_macrostep_discarded instead, when the input is abandoned or
 *      reaches the microstep limit, or its session goes: both lists are freed
 *      unseen (ADR 0099 section 4; ADR 0082).
 *      Mail sent at a safe point (a due send, a publish) has no macrostep to
 *      wait for: it is placed at once.
 *   3. ta_run takes the envelope like any input. A receiver that is gone or
 *      has finished does not get it: ta_mail_recycle turns the envelope into
 *      the data-free error.communication its sender takes (section 6). A
 *      receiver that is out (unloaded) makes the envelope wait for its
 *      address instead (step 4).
 *   4. Waiting for an address (section 4). An input whose target is an address
 *      with no resident session joins that address's waiting list
 *      (ta_mail_post_waiting): host input from ta_actor_deliver with an
 *      address, mail placed in step 2, and mail whose receiver was unloaded
 *      before it was taken. The first envelope of a list raises a cue, which
 *      ta_run writes as a LOAD_NEEDED notice. The list then leaves in one
 *      piece, in arrival order: to the run queue when a session becomes
 *      resident at the address (ta_mail_address_resident, called by spawn,
 *      publish and load) or the host names the session that serves it
 *      (ta_address_loaded), or recycled when the host answers
 *      ta_address_load_failed. A replacement that takes over an address brings
 *      the incumbent's queued inputs with it (ta_mail_retarget).
 *
 * Lifetimes. Each thing here has exactly one.
 *
 *   - call: a ta_mail_params and the answer. The destination and the event
 *     are borrowed for the call; on every answer the caller still owns what it
 *     passed, because the envelope retains its own references.
 *   - input: the two outgoing lists and the claim they pay from, all in the
 *     run slot (run.h), from the input's take until its COMPLETED notice. An
 *     envelope on `outgoing_waiting` holds its own reference to its address
 *     (state WAITING), or, for a session, the session's number and family in
 *     `target` and `input` to be keyed at the end (flag
 *     TA_MAIL_FLAG_SESSION); the pool gives the reference back whichever way
 *     it ends.
 *   - envelope: a mail envelope, from ta_mail_send until the receiver's input
 *     completes (ta_envelope_release), or until it is recycled; a recycled
 *     envelope is the same envelope, now an error for the sender, and lives
 *     until the sender's input completes. Anything holding its index (an
 *     outgoing list, a waiting list, the run queue, the slot) drops it first.
 *   - system: nothing of its own. The waiting lists, the table that finds
 *     them and the cue lists are package envelope's and package run's; this
 *     package decides when an envelope moves between them.
 *
 * When it allocates. ta_mail_send never does: it takes an envelope that the
 * pool already holds, under the claim, and answers TA_MAIL_FULL when there is
 * none (the claim is spent, or the pool is at its bound). The pool grows only
 * at the safe points the envelope package names (admission, before an input
 * is taken, at a due dispatch, at a publish: ta_mail_prepare), so a fanout
 * fails at the same send whether the system is cold or warm. Joining a waiting
 * list never allocates either: the table has a slot for every envelope. The
 * one allocation outside a safe point is the key of a gone session
 * (ta_address_intern), at the end of the sender's macrostep, and its failure
 * is a recycled mail, not a failed call. Event names are interned trie IDs, so
 * a recycled error needs no name storage; the per-envelope reservation of
 * ADR 0099 section 6 arrives with the name area (ADR 0100).
 *
 * Invariants.
 *   M1: an envelope on `outgoing` is RUNNABLE, made from the claim, and on
 *       no other list; its receiver's `queued` counts it. An envelope on
 *       `outgoing_waiting` is WAITING (or flagged as a session's), made from
 *       the claim; nobody's `queued` counts it.
 *   M2: the quota stays balanced (envelope.h I1): every outgoing envelope is
 *       "used" from the moment it is made; the claim holds only the rest.
 *   M3: after an input completes, both its lists are empty and its claim is
 *       zero, whichever way it completed.
 *   M4: the order of mail from one sender to one receiver is the order of the
 *       sends: lists are appended in send order, spliced or placed in order,
 *       and waiting lists keep arrival order.
 *   M5: a WAITING envelope on a waiting list is on exactly one, holds a
 *       reference to its address, has target 0 and is counted by no session's
 *       `queued`; its list is on the cue list (`loads`) exactly while the
 *       LOAD_NEEDED notice for it has not been written.
 *   M6: a waiting list leaves whole and in order: every envelope goes to the
 *       run queue (retargeted, counted in `queued`) or is recycled, and no
 *       envelope of the list is left behind or reordered.
 *
 * Complexity. ta_mail_send is O(1) (a directory lookup for a named
 * destination). ta_mail_macrostep_ended is O(1) for the resident mail and
 * O(waiting mail sent) for the rest, each an expected O(1) table probe.
 * ta_mail_macrostep_discarded is O(outgoing envelopes), the cost of freeing
 * them. ta_mail_recycle and ta_mail_post_waiting are O(1).
 * ta_mail_address_resident and ta_address_load_failed are O(list moved) plus
 * O(pending cues) to drop the list's cue. ta_mail_retarget is O(pending
 * envelopes), and is for the rare, host-initiated replacement.
 *
 * Failure. ta_mail_send changes nothing unless it answers TA_MAIL_QUEUED or
 * TA_MAIL_WAITING. No function here fails after it has begun to change
 * something, and none of the recycling and waiting functions can fail at all:
 * they only move envelopes the pool already holds.
 *
 * Example (inside a macrostep):
 *
 *	ta_mail_params mail = { .sender = actor->record.id, .destination = destination,
 *	                        .event = event, .trace = trace, .in_macrostep = true };
 *	switch (ta_mail_send(system, &mail)) {
 *	case TA_MAIL_QUEUED: break;                      // visible to the receiver when this macrostep ends
 *	case TA_MAIL_WAITING: break;                     // waits for an address; the host hears LOAD_NEEDED
 *	case TA_MAIL_UNDELIVERABLE: raise_communication_error(); break;
 *	case TA_MAIL_FULL:   raise_execution_error(); break;
 *	}
 *	... later, when the input completes ...
 *	ta_mail_macrostep_ended(system);
 */

/*
 * TA_MAIL_FLAG_SESSION marks (in ta_envelope.flags) a mail envelope on
 * `outgoing_waiting` whose destination is a session that is gone: `target`
 * holds the session's number and `input` the family's atom, and the key
 * address is made when the mail is placed.
 */
#define TA_MAIL_FLAG_SESSION 0x1u

/*
 * TA_MAIL_FLAG_KEY marks (in ta_envelope.flags) a mail envelope that waits on
 * the key address of a gone session, so the LOAD_NEEDED notice of its list says
 * the address is a key (ta_run_notice.session).
 */
#define TA_MAIL_FLAG_KEY 0x2u

/* ta_mail_status is the answer of ta_mail_send. */
typedef enum ta_mail_status {
    TA_MAIL_QUEUED = 0,   /* an envelope is on the running input's outgoing list, or (at a safe point) on the run queue */
    TA_MAIL_WAITING,      /* the receiver is not resident: the envelope waits, or will at the end of the macrostep, for an address */
    TA_MAIL_UNDELIVERABLE,/* known undeliverable now (a name a finished session holds, a session no family can serve): the sender hears error.communication in this macrostep (SCXML 6.2.4); nothing retained */
    TA_MAIL_FULL          /* no envelope: the claim is spent, the pool is at its bound, or the receiver has too many; nothing retained */
} ta_mail_status;

/*
 * ta_mail_params is the request of ta_mail_send. Lifetime: call. Everything
 * in it is borrowed: the destination (a named reference is retained by the
 * envelope only if the mail has to wait for it), and the event, whose data,
 * origin and invoke ID the envelope retains for itself.
 */
typedef struct ta_mail_params {
    ta_actor_id sender;       /* the sending session, whom an undeliverable mail is reported to; 0 for none */
    ta_actor_ref destination; /* a runtime reference (session routing is done; its family says who may serve a gone session) or a named one */
    ta_event event;           /* name, type, send ID, origin, invoke ID and data, as the receiver is to see them */
    ta_trace_parent trace;    /* the sending step, which the receiver's macrostep runs under (ADR 0067) */
    /*
     * True when the mail is sent by the macrostep in the run slot: the
     * envelope is paid from that input's claim and waits on its outgoing list.
     * False at a safe point (a due send, a publish): the envelope is paid
     * from the pool's free quota and joins the run queue at once. The caller
     * says which; this package never infers it from the slot, because a
     * macrostep paused by a budget keeps the slot while safe points run.
     */
    bool in_macrostep;
} ta_mail_params;

/*
 * ta_mail_send makes an envelope for `params->event` and places it by where
 * the destination is.
 *
 *   - A resident session that has not finished: TA_MAIL_QUEUED. Inside a
 *     macrostep (`in_macrostep`) the envelope is appended to the running
 *     input's `outgoing` list and becomes visible when that input ends; at a
 *     safe point it is pushed on the run queue at once, behind everything
 *     already queued. The receiver's `queued` count includes the mail from
 *     now on, so the session is busy for unload and export (actor.h).
 *   - A name that no resident session holds (unloaded, or no entry):
 *     TA_MAIL_WAITING. The envelope keeps a reference to the address and,
 *     inside a macrostep, goes on `outgoing_waiting`; at a safe point it joins
 *     the address's waiting list at once.
 *   - A session ID whose session is gone or has finished, with a family that
 *     names who may serve it (core ADR 0088, napi ADR 0032 section 2):
 *     TA_MAIL_WAITING, on a key address made from the family and the
 *     session's text. The key is made when the mail is placed (the end of the
 *     macrostep, or at once at a safe point).
 *   - A name that a finished session holds, a session ID with no family, or
 *     session 0 (a session core cannot name): TA_MAIL_UNDELIVERABLE, and the
 *     caller raises error.communication in the sender's own queue, in this
 *     macrostep, as SCXML 6.2.4 asks. Mail that waits and finds its receiver
 *     gone or finished when it is taken is recycled instead (ta_mail_recycle).
 *
 * A safe-point send takes its envelope from the pool's free quota, not from a
 * claim, so the caller makes room first with ta_mail_prepare. Without room it
 * answers TA_MAIL_FULL and changes nothing; it never grows the pool itself.
 *
 * It never allocates in a macrostep. TA_MAIL_FULL says the claim is spent, or
 * the pool has no free envelope or no quota, or the receiver already has
 * UINT16_MAX inputs queued; the sender's macrostep reports it as
 * error.execution (it is a quota refusal, like maxPendingSends). O(1).
 */
ta_mail_status ta_mail_send(ta_system *system, const ta_mail_params *params);

/*
 * ta_mail_prepare is the safe-point step that precedes `count` safe-point
 * sends (ta_mail_send with in_macrostep false): it grows the pool, if need be,
 * so that `count` envelopes can be made without touching the items the running
 * input's claim promised. It is the only function of this package that
 * allocates, and a safe point is the only place to call it (a due dispatch, a
 * publish, an admission). TA_ERR_NOMEM: the allocator refused; nothing has
 * changed. TA_ERR_FULL: the pool's bound leaves no room for `count` more
 * envelopes; nothing has changed. O(1) when the pool already has room.
 */
ta_result ta_mail_prepare(ta_system *system, uint32_t count);

/*
 * ta_mail_macrostep_ended makes the running input's mail visible: its `outgoing`
 * list joins the end of the run queue in one step, its `outgoing_waiting` list
 * is placed in send order (to the run queue if the address is resident by now,
 * else onto its waiting list; a gone session gets its key address, or the mail
 * is recycled if that cannot be interned), and the rest of its claim goes back
 * to the quota. Call it as the input completes, before the slot is cleared. It
 * allocates only for the key of a gone session, and a refusal there is a
 * recycled mail. O(1) plus O(waiting mail sent).
 */
void ta_mail_macrostep_ended(ta_system *system);

/*
 * ta_mail_macrostep_discarded frees the running input's mail unseen, both
 * lists, and gives its claim back: the input was abandoned, ran away, or its
 * session was destroyed. Call it before the slot is cleared. Receivers'
 * `queued` counts fall by one per envelope freed. It never allocates.
 * O(envelopes freed).
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
 * ta_mail_address_served is ta_mail_address_resident for an address that is
 * only a key: the host loaded or spawned `actor`, which does not hold the
 * address (a session of a family, napi ADR 0032 section 2), to serve the mail
 * that waits for it. The list moves as there. Answers true when something
 * waited. Never allocates.
 */
bool ta_mail_address_served(ta_system *system, const ta_address *address, ta_actor_id actor);

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

/*
 * Implementation of package mail. The contract, the lifetimes and the
 * invariants are in mail.h; this comment records why the steps are in the
 * order they are.
 *
 * ta_mail_send checks everything that can say "not here" before it takes an
 * envelope, and takes the envelope before it touches any other state, so
 * every answer but TA_MAIL_QUEUED leaves the system as it found it: the
 * receiver is resolved first (a pure lookup), the receiver's counter is
 * checked next, then the one step that can be refused, taking an envelope
 * from the claim. Only then is the envelope filled, which cannot fail: the
 * event is retained with ta_event_retain, which only counts references.
 *
 * The outgoing list lives in the run slot and the envelopes on it are
 * RUNNABLE already, so splicing needs no pass over them: the receiver's
 * `queued` count went up when the envelope was made, and the envelope only
 * has to join the run queue. Discarding is the one place that walks the list,
 * because every envelope owns references and a count to give back.
 */

#include "internal.h"

/*
 * ta_mail_receiver answers the resident session `destination` names, or NULL.
 * A runtime reference names it by its ID; a named reference is resolved
 * through the host's directory, whose answer is the session that holds the
 * address now. Neither interns, retains nor allocates, so it is safe in a
 * macrostep. The session may have finished: ta_mail_send decides what that
 * means.
 */
static ta_actor *ta_mail_receiver(const ta_system *system, const ta_actor_ref *destination) {
    ta_actor_id id = 0;
    if (destination->kind == TA_ACTOR_REF_RUNTIME) {
        id = destination->as.runtime;
    } else if (destination->kind == TA_ACTOR_REF_NAMED && destination->as.named != NULL &&
               system->directory.v != NULL && system->directory.v->resolve != NULL) {
        ta_actor_directory_entry entry = { .kind = TA_ACTOR_DIRECTORY_NONE };
        ta_result result = system->directory.v->resolve(
            system->directory.self,
            &(ta_actor_directory_resolve_params){ .address = destination->as.named, .out = &entry });
        if (result != TA_OK || entry.kind != TA_ACTOR_DIRECTORY_RESIDENT) {
            return NULL;
        }
        id = entry.actor;
    }
    return id != 0 ? ta_actor_lookup(system, id) : NULL;
}

/* ta_mail_send: see mail.h. */
ta_mail_status ta_mail_send(ta_system *system, const ta_mail_params *params) {
    ta_run_slot *slot = &system->runner.slot;
    if (!slot->occupied) {
        return TA_MAIL_ROUTER;
    }
    ta_actor *receiver = ta_mail_receiver(system, &params->destination);
    if (receiver == NULL) {
        return TA_MAIL_ROUTER;
    }
    if (receiver->finished) {
        /*
         * A name that a finished session still holds is undeliverable now. A
         * session ID is not: its family may have a loader that starts a new
         * instance for the mail (napi ADR 0032 section 2), which only the
         * router knows until core owns waiting lists for sessions.
         */
        return params->destination.kind == TA_ACTOR_REF_NAMED ? TA_MAIL_UNDELIVERABLE : TA_MAIL_ROUTER;
    }
    if (receiver->queued == UINT16_MAX) {
        return TA_MAIL_FULL;
    }
    uint32_t index = ta_envelope_make(&system->envelopes, &slot->claim,
                                      TA_ENVELOPE_RUNNABLE);
    if (index == 0) {
        return TA_MAIL_FULL;
    }
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    envelope->source = TA_SOURCE_MAIL;
    envelope->target = receiver->record.id;
    envelope->from = params->sender;
    envelope->due = ta_system_time(system);
    envelope->trace = params->trace;
    ta_event_retain(&params->event, &envelope->event);
    receiver->queued += 1u;
    ta_ilist_push_back(&slot->outgoing, ta_envelopes_link(&system->envelopes), index);
    return TA_MAIL_QUEUED;
}

/* ta_mail_macrostep_ended: see mail.h. */
void ta_mail_macrostep_ended(ta_system *system) {
    ta_run_slot *slot = &system->runner.slot;
    ta_envelopes_run_splice(&system->envelopes, &slot->outgoing);
    (void)ta_envelopes_unreserve(&system->envelopes, &slot->claim);
}

/* ta_mail_macrostep_discarded: see mail.h. */
void ta_mail_macrostep_discarded(ta_system *system) {
    ta_run_slot *slot = &system->runner.slot;
    const ta_ilink *link = ta_envelopes_link(&system->envelopes);
    uint32_t index = 0;
    while ((index = ta_ilist_pop_front(&slot->outgoing, link)) != 0) {
        ta_actor *receiver = ta_actor_lookup(system, ta_envelope_at(&system->envelopes, index)->target);
        if (receiver != NULL && receiver->queued != 0) {
            receiver->queued -= 1u;
        }
        ta_envelope_release(&system->envelopes, index);
    }
    (void)ta_envelopes_unreserve(&system->envelopes, &slot->claim);
}

/* ta_mail_recycle: see mail.h. */
void ta_mail_recycle(ta_system *system, uint32_t index, ta_recycle_reason reason) {
    (void)reason; /* an error.communication is data-free: every row of section 6 reads the same to the sender */
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    ta_actor *sender = envelope->source == TA_SOURCE_MAIL ? ta_actor_lookup(system, envelope->from) : NULL;
    if (sender == NULL || sender->finished || sender->queued == UINT16_MAX) {
        ta_envelope_release(&system->envelopes, index); /* nobody to tell */
        return;
    }
    ta_send_id send_id = envelope->event.send_id;
    ta_event_release(&envelope->event);
    envelope->event = (ta_event){ .name = TA_EVENT_NAME_ERROR_COMMUNICATION,
                                  .type = TA_EVENT_PLATFORM,
                                  .send_id = send_id,
                                  .invoke_id = TA_INVOKE_ID_NONE };
    envelope->source = TA_SOURCE_ERROR;
    envelope->target = sender->record.id;
    envelope->from = 0;
    sender->queued += 1u;
    ta_interpreter_count_error(system, sender, TA_EVENT_NAME_ERROR_COMMUNICATION);
    ta_envelopes_run_push(&system->envelopes, index);
}

/* ta_mail_outgoing_to: see mail.h. */
uint32_t ta_mail_outgoing_to(const ta_system *system, ta_actor_id actor) {
    const ta_run_slot *slot = &system->runner.slot;
    uint32_t count = 0;
    if (!slot->occupied || slot->actor != actor) {
        return 0;
    }
    const ta_ilink *link = ta_envelopes_link(&system->envelopes);
    for (uint32_t index = slot->outgoing.head; index != 0; index = ta_ilist_next(link, index)) {
        count += ta_envelope_at(&system->envelopes, index)->target == actor ? 1u : 0u;
    }
    return count;
}

/* ta_mail_follow: see mail.h. */
ta_actor *ta_mail_follow(ta_system *system, uint32_t index) {
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    ta_actor_ref routed;
    ta_actor_ref session = { .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = envelope->target };
    if (ta_session_route(system, &session, &routed)) {
        ta_actor_ref_release(&routed); /* the session is out: a named address, which only a load answers */
        return NULL;
    }
    ta_actor *receiver = routed.kind == TA_ACTOR_REF_RUNTIME ? ta_actor_lookup(system, routed.as.runtime) : NULL;
    if (receiver == NULL || receiver->queued == UINT16_MAX) {
        return NULL;
    }
    envelope->target = receiver->record.id;
    receiver->queued += 1u;
    return receiver;
}

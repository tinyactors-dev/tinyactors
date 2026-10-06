/*
 * Implementation of package mail. The contract, the lifetimes and the
 * invariants are in mail.h; this comment records why the steps are in the
 * order they are.
 *
 * ta_mail_send checks everything that can say "not here" before it takes an
 * envelope, and takes the envelope before it touches any other state, so
 * every answer but TA_MAIL_QUEUED and TA_MAIL_WAITING leaves the system as it
 * found it: the destination is resolved first (a pure lookup), the receiver's
 * counter is checked next, then the one step that can be refused, taking an
 * envelope from the claim. Only then is the envelope filled, which cannot
 * fail: the event is retained with ta_event_retain, which only counts
 * references, and so is the address of a mail that has to wait.
 *
 * The outgoing lists live in the run slot. The envelopes on `outgoing` are
 * RUNNABLE already, so splicing needs no pass over them: the receiver's
 * `queued` count went up when the envelope was made, and the envelope only
 * has to join the run queue. The envelopes on `outgoing_waiting` cannot be
 * spliced, because each goes to a list of its own, so the end of the
 * macrostep places them one by one, in the order they were sent. Discarding
 * is the one place that walks `outgoing`, because every envelope owns
 * references and a count to give back.
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

/*
 * ta_mail_route says where a destination is now. Lifetime: call; `address` is
 * borrowed from the destination, and `receiver` from the actor table.
 */
typedef struct ta_mail_route {
    ta_actor *receiver;  /* a resident session that has not finished, or NULL */
    ta_address *address; /* the named address to wait for, or NULL */
    uint64_t session;    /* a gone session to wait for under a key, or 0 */
    ta_atom family;      /* the family of that session */
} ta_mail_route;

/*
 * ta_mail_resolve finds where `destination` is without allocating or retaining
 * anything, and answers the mail status the caller returns when the mail cannot
 * be placed: QUEUED (a resident receiver), WAITING (an address or a session
 * key to wait for) or UNDELIVERABLE. A name that a finished session holds is
 * undeliverable; a session ID is not, if its family may have a loader (napi
 * ADR 0032 section 2). That includes session 0 with a family, which is how a
 * session of another incarnation parses (scxml_uri.c); its key is
 * "<this incarnation>.0.<family>". A session ID with no family, which core
 * cannot name, is undeliverable.
 */
static ta_mail_status ta_mail_resolve(const ta_system *system, const ta_actor_ref *destination, ta_mail_route *route) {
    *route = (ta_mail_route){0};
    if (destination->kind == TA_ACTOR_REF_NAMED) {
        if (destination->as.named == NULL) {
            return TA_MAIL_UNDELIVERABLE;
        }
        ta_actor *holder = ta_mail_receiver(system, destination);
        if (holder == NULL) {
            route->address = destination->as.named;
            return TA_MAIL_WAITING;
        }
        if (holder->finished) {
            return TA_MAIL_UNDELIVERABLE;
        }
        route->receiver = holder;
        return TA_MAIL_QUEUED;
    }
    ta_actor *receiver = ta_mail_receiver(system, destination);
    if (receiver != NULL && !receiver->finished) {
        route->receiver = receiver;
        return TA_MAIL_QUEUED;
    }
    if (destination->kind != TA_ACTOR_REF_RUNTIME || destination->family == 0) {
        return TA_MAIL_UNDELIVERABLE;
    }
    route->session = destination->as.runtime;
    route->family = destination->family;
    return TA_MAIL_WAITING;
}

/*
 * ta_mail_key interns the key address of a gone session: the family and the
 * session's text as the name, `{ family, "<incarnation>.<session>.<family>" }`,
 * which is a key and never a directory entry (napi ADR 0032 section 2). The
 * address is owned by the caller (one reference). TA_ERR_NOMEM when the text
 * or the address cannot be made; nothing is retained then.
 */
static ta_result ta_mail_key(ta_system *system, uint64_t session, ta_atom family, ta_address **out) {
    ta_str family_text = {0};
    if (ta_system_atom_text(system, family, &family_text) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    char small[160];
    char *text = small;
    uint32_t needed = 0;
    /* Not ta_session_id_format: session 0, a session of another incarnation, has a key too. */
    ta_text_writer writer = { .buffer = small, .capacity = sizeof small };
    ta_session_text_write(system, &writer, session, family_text);
    ta_result result = ta_text_finish(&writer, &needed);
    if (result != TA_OK && needed > sizeof small) {
        text = ta_mem_alloc(system->allocator, needed, 1);
        if (text == NULL) {
            return TA_ERR_NOMEM;
        }
        writer = (ta_text_writer){ .buffer = text, .capacity = needed };
        ta_session_text_write(system, &writer, session, family_text);
        result = ta_text_finish(&writer, &needed);
    }
    if (result == TA_OK) {
        result = ta_address_intern(system,
                                   &(ta_actor_address){ .family = { .name = family_text },
                                                        .name = { .ptr = text, .len = needed - 1u } },
                                   out);
    }
    if (text != small) {
        ta_mem_free(system->allocator, text, &(ta_allocator_free_params){ .size = needed, .alignment = 1 });
    }
    return result == TA_OK ? TA_OK : TA_ERR_NOMEM;
}

/*
 * ta_mail_enter_resident makes the made envelope `index` an input of the
 * resident `receiver`: its target, its place in the receiver's `queued`, and
 * the run queue. The caller has checked the receiver can take it.
 */
static void ta_mail_enter_resident(ta_system *system, uint32_t index, ta_actor *receiver) {
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    envelope->target = receiver->record.id;
    receiver->queued += 1u;
    ta_envelopes_run_push(&system->envelopes, index);
}

/*
 * ta_mail_place_waiting places a made mail envelope that has to wait: the one
 * in `index` is RUNNABLE, on no list, and `route` says what for. A key for a
 * gone session is made first; if that cannot be made the mail is recycled
 * (the sender hears error.communication) and counted. Otherwise, if the address
 * is resident by now, the mail goes to its session; else it joins the
 * address's waiting list. Never fails; allocates only for the key.
 */
static void ta_mail_place_waiting(ta_system *system, uint32_t index, const ta_mail_route *route) {
    ta_address *address = route->address;
    bool keyed = false; /* the address is a made key: the flag is set only where the envelope waits */
    if (address != NULL) {
        ta_address_retain(address);
    } else if (ta_mail_key(system, route->session, route->family, &address) != TA_OK) {
        ta_mail_recycle(system, index, TA_REFUSED_LOAD_FAILED);
        return;
    } else {
        keyed = true;
    }
    ta_actor *holder = ta_mail_resident_at(system, address);
    if (holder != NULL && !holder->finished && holder->queued != UINT16_MAX) {
        ta_mail_enter_resident(system, index, holder);
    } else if (holder != NULL) {
        ta_mail_recycle(system, index, holder->finished ? TA_REFUSED_DONE : TA_REFUSED_INVALID);
    } else {
        if (keyed) {
            ta_envelope_at(&system->envelopes, index)->flags = TA_MAIL_FLAG_KEY;
        }
        ta_mail_post_waiting(system, index, address);
    }
    ta_address_release(address); /* the waiting list took its own */
}

/*
 * ta_mail_send sends one chart event as mail (lifetime of the envelope it
 * makes: from now until its input is taken, refused or discarded; it owns a
 * retained reference to the event data, so the caller's copy may go at once).
 * It answers where the mail went: TA_MAIL_QUEUED (a resident receiver's
 * queue, or, inside a macrostep, its outgoing list), TA_MAIL_WAITING (it
 * waits for an address or session to become resident, or for the macrostep to
 * end), TA_MAIL_FULL (no room: the receiver's queue is at its limit, the pool
 * is at its bound, or the claim of the running slot is spent) and
 * TA_MAIL_UNDELIVERABLE (the destination can never receive it).
 *
 * Order of work, so a refusal retains nothing: the slot is checked, the
 * destination is resolved, the receiver's queue limit is checked, and only
 * then is an envelope taken. UNDELIVERABLE and FULL therefore leave no
 * envelope, no reference and no counter changed. Inside a macrostep
 * (`in_macrostep`) the envelope is paid from the slot's claim and nothing is
 * allocated; outside one it comes from the pool, which a safe point has
 * already refilled (ta_mail_prepare), so a plain post does not allocate
 * either, except for the key of a gone session's address (ta_mail_place_waiting).
 */
ta_mail_status ta_mail_send(ta_system *system, const ta_mail_params *params) {
    ta_run_slot *slot = &system->runner.slot;
    if (params->in_macrostep && !slot->occupied) {
        return TA_MAIL_FULL; /* a caller that says it is in a macrostep, with none running, has nothing to pay from */
    }
    ta_mail_route route;
    ta_mail_status placed = ta_mail_resolve(system, &params->destination, &route);
    if (placed == TA_MAIL_UNDELIVERABLE) {
        return placed;
    }
    if (route.receiver != NULL && route.receiver->queued == UINT16_MAX) {
        return TA_MAIL_FULL;
    }
    uint32_t index = ta_envelope_make(&system->envelopes, params->in_macrostep ? &slot->claim : NULL,
                                      TA_ENVELOPE_RUNNABLE);
    if (index == 0) {
        return TA_MAIL_FULL;
    }
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    envelope->source = TA_SOURCE_MAIL;
    envelope->from = params->sender;
    envelope->due = ta_system_time(system);
    envelope->trace = params->trace;
    ta_event_retain(&params->event, &envelope->event);
    if (route.receiver != NULL) {
        envelope->target = route.receiver->record.id;
        route.receiver->queued += 1u;
        if (params->in_macrostep) {
            ta_ilist_push_back(&slot->outgoing, ta_envelopes_link(&system->envelopes), index);
        } else {
            ta_envelopes_run_push(&system->envelopes, index);
        }
        return TA_MAIL_QUEUED;
    }
    if (!params->in_macrostep) {
        ta_mail_place_waiting(system, index, &route);
        return TA_MAIL_WAITING;
    }
    /* It waits for the end of the macrostep, holding what it needs to be placed then. */
    if (route.address != NULL) {
        ta_address_retain(route.address);
        envelope->state = TA_ENVELOPE_WAITING;
        envelope->payload.address = route.address;
    } else {
        envelope->target = route.session;
        envelope->input = route.family;
        envelope->flags = TA_MAIL_FLAG_SESSION;
    }
    ta_ilist_push_back(&slot->outgoing_waiting, ta_envelopes_link(&system->envelopes), index);
    return TA_MAIL_WAITING;
}

/* ta_mail_prepare: see mail.h. */
ta_result ta_mail_prepare(ta_system *system, uint32_t count) {
    /* The items the running input's claim promised stay free: they are asked for on top of `count`. */
    uint32_t promised = (uint32_t)system->runner.slot.claim.held;
    if (count > UINT32_MAX - promised) {
        return TA_ERR_NOMEM;
    }
    if (ta_envelopes_refill(&system->envelopes, count + promised) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    return ta_quota_available(&system->envelopes.quota) >= count ? TA_OK : TA_ERR_FULL;
}

/* ta_mail_hold: see mail.h. */
void ta_mail_hold(ta_system *system, uint32_t index, ta_address *address) {
    ta_run_slot *slot = &system->runner.slot;
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    if (address == NULL) {
        ta_actor *target = ta_actor_lookup(system, envelope->target);
        if (target != NULL) {
            target->queued += 1u; /* the caller checked it could take one more */
        }
        ta_ilist_push_back(&slot->outgoing, ta_envelopes_link(&system->envelopes), index);
        return;
    }
    ta_address_retain(address);
    envelope->state = TA_ENVELOPE_WAITING;
    envelope->payload.address = address;
    ta_ilist_push_back(&slot->outgoing_waiting, ta_envelopes_link(&system->envelopes), index);
}

/* ta_mail_macrostep_ended: see mail.h. */
void ta_mail_macrostep_ended(ta_system *system) {
    ta_run_slot *slot = &system->runner.slot;
    const ta_ilink *link = ta_envelopes_link(&system->envelopes);
    ta_envelopes_run_splice(&system->envelopes, &slot->outgoing);
    uint32_t index = 0;
    while ((index = ta_ilist_pop_front(&slot->outgoing_waiting, link)) != 0) {
        ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
        ta_mail_route route = {0};
        if (envelope->source == TA_SOURCE_HOST && (envelope->flags & TA_ENVELOPE_WITHDRAWN) != 0) {
            /* Withdrawn while held: it holds no place in `queued` (run.h I7), so free it unseen. */
            ta_envelope_release(&system->envelopes, index);
            continue;
        }
        if ((envelope->flags & TA_MAIL_FLAG_SESSION) != 0) {
            route.session = envelope->target;
            route.family = (ta_atom)envelope->input;
            envelope->target = 0;
            envelope->input = 0;
            envelope->flags = 0;
        } else {
            /* The envelope's own reference is given back after the placement took a new one. */
            route.address = envelope->payload.address;
            envelope->payload.address = NULL;
            envelope->state = TA_ENVELOPE_RUNNABLE;
        }
        ta_mail_place_waiting(system, index, &route);
        if (route.address != NULL) {
            ta_address_release(route.address);
        }
    }
    (void)ta_envelopes_unreserve(&system->envelopes, &slot->claim);
}

/*
 * ta_mail_discard disposes of one envelope of an abandoned macrostep's
 * outgoing lists, already off the list: mail is freed unseen (a WAITING
 * envelope gives its address back), and a host input the macrostep admitted is
 * refused (TA_REFUSED_DISCARDED) so that the host, which holds its token, is
 * told. A withdrawn input is freed like mail.
 */
static void ta_mail_discard(ta_system *system, uint32_t index) {
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    if (envelope->source != TA_SOURCE_HOST || (envelope->flags & TA_ENVELOPE_WITHDRAWN) != 0) {
        ta_envelope_release(&system->envelopes, index);
        return;
    }
    if (envelope->state == TA_ENVELOPE_WAITING) {
        ta_address_release(envelope->payload.address);
        envelope->payload.address = NULL;
    }
    envelope->flags = (uint16_t)TA_REFUSED_DISCARDED;
    envelope->target = 0;
    envelope->state = TA_ENVELOPE_DECIDED;
    ta_run_input_decided(system, envelope);
    ta_ilist_push_back(&system->runner.refused, ta_envelopes_link(&system->envelopes), index);
}

/*
 * ta_mail_macrostep_discarded disposes of everything an abandoned macrostep
 * sent or admitted: the envelopes on the slot's `outgoing` and
 * `outgoing_waiting` lists (the macrostep's effects are undone, so its mail
 * never existed). Mail is freed unseen and a resident receiver's queued count
 * goes back down. A host input admitted inside the macrostep is different:
 * the host holds its token, so it is moved to the refused list with reason
 * TA_REFUSED_DISCARDED and its REFUSED notice is written by the next ta_run.
 * The host_inputs gauge goes down at that moment (ta_run_input_decided), not
 * when the notice is read. Finally the unused part of the slot's claim is
 * returned. Never allocates; O(envelopes on the two lists).
 */
void ta_mail_macrostep_discarded(ta_system *system) {
    ta_run_slot *slot = &system->runner.slot;
    const ta_ilink *link = ta_envelopes_link(&system->envelopes);
    uint32_t index = 0;
    while ((index = ta_ilist_pop_front(&slot->outgoing, link)) != 0) {
        ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
        /* A withdrawn input gave its place back when it was withdrawn. */
        ta_actor *receiver = (envelope->flags & TA_ENVELOPE_WITHDRAWN) != 0
                                 ? NULL
                                 : ta_actor_lookup(system, envelope->target);
        if (receiver != NULL && receiver->queued != 0) {
            receiver->queued -= 1u;
        }
        ta_mail_discard(system, index);
    }
    while ((index = ta_ilist_pop_front(&slot->outgoing_waiting, link)) != 0) {
        ta_mail_discard(system, index);
    }
    (void)ta_envelopes_unreserve(&system->envelopes, &slot->claim);
}

/* ta_mail_recycle: see mail.h. */
void ta_mail_recycle(ta_system *system, uint32_t index, ta_refusal_reason reason) {
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    if ((envelope->flags & TA_ENVELOPE_WITHDRAWN) != 0) {
        ta_envelope_release(&system->envelopes, index); /* the host took it back: nobody to tell */
        return;
    }
    if (envelope->source == TA_SOURCE_HOST) {
        /* The host asked to be told: the reason rides in the flags until ta_run writes the notice. */
        envelope->flags = (uint16_t)reason;
        envelope->target = 0;
        envelope->state = TA_ENVELOPE_DECIDED;
        ta_run_input_decided(system, envelope);
        ta_ilist_push_back(&system->runner.refused, ta_envelopes_link(&system->envelopes), index);
        return;
    }
    /* An error.communication is data-free: every row of section 6 reads the same to the sender. */
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

/* ta_mail_resident_at: see mail.h. */
ta_actor *ta_mail_resident_at(const ta_system *system, ta_address *address) {
    return ta_mail_receiver(system, &(ta_actor_ref){ .kind = TA_ACTOR_REF_NAMED, .as.named = address });
}

/*
 * ta_mail_unwait takes a WAITING envelope off its address: it is RUNNABLE
 * again, on no list, ready to be pushed on the run queue or recycled. The
 * list it was on has been taken by the caller. The reference to the address
 * goes back, except for a host input of a list that failed to load
 * (`refusing`): it keeps the reference, and the REFUSED notice that ta_run
 * writes for it names the address the host posted to. The reference is given
 * back with the envelope.
 */
static void ta_mail_unwait(ta_envelope *envelope, bool refusing) {
    if (!refusing || envelope->source != TA_SOURCE_HOST) {
        ta_address_release(envelope->payload.address);
        envelope->payload.address = NULL;
    }
    envelope->state = TA_ENVELOPE_RUNNABLE;
    envelope->flags = (uint16_t)(envelope->flags & (TA_ENVELOPE_WITHDRAWN | TA_ENVELOPE_REPORT | TA_ENVELOPE_REPORT_ENDS));
    envelope->next = 0;
}

/*
 * ta_mail_drop_cue removes the pending LOAD_NEEDED cue of the list whose first
 * envelope is `head`, if its notice has not been written yet. The cue list
 * holds only the lists that began since the last ta_run, so the walk is short.
 */
static void ta_mail_drop_cue(ta_system *system, uint32_t head) {
    (void)ta_ilist_unlink(&system->runner.loads, ta_envelopes_owner_link(&system->envelopes), head);
}

/*
 * ta_mail_post_waiting puts a RUNNABLE envelope that is on no list on the
 * waiting list of `address` (the envelope retains the address itself, via
 * ta_waiting_insert). If it is the first of its list the envelope is also
 * queued on the cue list, so the next ta_run writes one LOAD_NEEDED notice
 * for the address. The envelope's target is 0 while it waits: it counts for
 * no session.
 *
 * It never allocates. The waiting table entry for a new list was made room
 * for by ta_envelopes_refill when the envelope's slot in the pool was made
 * (envelope.h I4), which is why this can run inside a macrostep's end. The
 * recycle on a failed insert is therefore unreachable and only a guard.
 */
void ta_mail_post_waiting(ta_system *system, uint32_t index, ta_address *address) {
    bool first = false;
    ta_envelope_at(&system->envelopes, index)->target = 0;
    if (ta_waiting_insert(&system->envelopes, index, &(ta_waiting_put){ .address = address, .first = &first }) !=
        TA_OK) {
        ta_mail_recycle(system, index, TA_REFUSED_LOAD_FAILED); /* unreachable after a refill (envelope.h I4) */
        return;
    }
    if (first) {
        ta_ilist_push_back(&system->runner.loads, ta_envelopes_owner_link(&system->envelopes), index);
    }
}

/* ta_mail_address_resident: see mail.h. */
void ta_mail_address_resident(ta_system *system, const ta_address *address, ta_actor_id actor_id) {
    uint32_t index = ta_waiting_take_list(&system->envelopes, address);
    if (index == 0) {
        return;
    }
    ta_mail_drop_cue(system, index);
    ta_actor *actor = ta_actor_lookup(system, actor_id);
    const ta_ilink *link = ta_envelopes_link(&system->envelopes);
    while (index != 0) {
        uint32_t next = ta_ilist_next(link, index); /* read before the push rewrites the link */
        ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
        bool withdrawn = (envelope->flags & TA_ENVELOPE_WITHDRAWN) != 0;
        ta_mail_unwait(envelope, false);
        if (withdrawn) {
            ta_envelope_release(&system->envelopes, index);
        } else if (actor == NULL) {
            ta_mail_recycle(system, index, TA_REFUSED_DESTROYED);
        } else if (actor->finished) {
            ta_mail_recycle(system, index, TA_REFUSED_DONE);
        } else if (actor->queued == UINT16_MAX) {
            ta_mail_recycle(system, index, TA_REFUSED_INVALID);
        } else {
            envelope->target = actor_id;
            actor->queued += 1u;
            ta_envelopes_run_push(&system->envelopes, index);
        }
        index = next;
    }
}

/* ta_mail_address_served: see mail.h. */
bool ta_mail_address_served(ta_system *system, const ta_address *address, ta_actor_id actor) {
    if (ta_waiting_length(&system->envelopes, address) == 0) {
        return false;
    }
    ta_mail_address_resident(system, address, actor);
    return true;
}

/* ta_address_loaded: see tinyactors.h. */
ta_result ta_address_loaded(ta_system *system, const ta_address_loaded_params *params) {
    if (system == NULL || params == NULL || params->address == NULL) {
        return TA_ERR_INVALID;
    }
    if (ta_actor_lookup(system, params->actor) == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    return ta_mail_address_served(system, params->address, params->actor) ? TA_OK : TA_ERR_NOT_FOUND;
}

/* ta_address_load_failed: see tinyactors.h. */
ta_result ta_address_load_failed(ta_system *system, const ta_address_load_failed_params *params) {
    if (system == NULL || params == NULL || params->address == NULL ||
        (uint32_t)params->reason < (uint32_t)TA_REFUSED_LOAD_NOT_FOUND ||
        (uint32_t)params->reason > (uint32_t)TA_REFUSED_LOAD_UNLOADED) {
        return TA_ERR_INVALID;
    }
    uint32_t index = ta_waiting_take_list(&system->envelopes, params->address);
    if (index == 0) {
        return TA_ERR_NOT_FOUND;
    }
    ta_mail_drop_cue(system, index);
    const ta_ilink *link = ta_envelopes_link(&system->envelopes);
    while (index != 0) {
        uint32_t next = ta_ilist_next(link, index);
        ta_mail_unwait(ta_envelope_at(&system->envelopes, index), true);
        ta_mail_recycle(system, index, params->reason);
        index = next;
    }
    return TA_OK;
}

/* ta_address_waiting: see tinyactors.h. */
uint32_t ta_address_waiting(const ta_system *system, const ta_address *address) {
    return system == NULL ? 0 : ta_waiting_pending(&system->envelopes, address);
}

/* ta_mail_retarget_step is what one pass of ta_mail_retarget_list needs. Lifetime: call. */
typedef struct ta_mail_retarget_step {
    ta_actor_id from;
    ta_actor_id to;
    uint32_t room; /* how many more the receiving session may be given */
} ta_mail_retarget_step;

/*
 * ta_mail_retarget_list moves the envelopes of one list that go from `step->from`
 * to `step->to`: host input and mail, not errors. It answers how many it moved,
 * which is at most `step->room`, so the receiving session's `queued` cannot
 * overflow.
 */
static uint32_t ta_mail_retarget_list(ta_system *system, uint32_t head, const ta_mail_retarget_step *step) {
    const ta_ilink *link = ta_envelopes_link(&system->envelopes);
    uint32_t moved = 0;
    for (uint32_t index = head; index != 0 && moved < step->room; index = ta_ilist_next(link, index)) {
        ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
        if (envelope->target == step->from && (envelope->flags & TA_ENVELOPE_WITHDRAWN) == 0 &&
            (envelope->source == TA_SOURCE_HOST || envelope->source == TA_SOURCE_MAIL)) {
            envelope->target = step->to;
            moved += 1u;
        }
    }
    return moved;
}

/* ta_mail_retarget: see mail.h. */
void ta_mail_retarget(ta_system *system, ta_actor_id from, ta_actor_id to) {
    ta_actor *source = ta_actor_lookup(system, from);
    ta_actor *target = ta_actor_lookup(system, to);
    if (source == NULL || target == NULL || from == to) {
        return;
    }
    ta_mail_retarget_step step = { .from = from, .to = to, .room = (uint32_t)(UINT16_MAX - target->queued) };
    uint32_t moved = ta_mail_retarget_list(system, system->envelopes.run.head, &step);
    if (system->runner.slot.occupied) {
        step.room -= moved;
        moved += ta_mail_retarget_list(system, system->runner.slot.outgoing.head, &step);
    }
    target->queued = (uint16_t)(target->queued + moved);
    source->queued = (uint16_t)(source->queued >= moved ? source->queued - moved : 0u);
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
        const ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
        count += envelope->target == actor && (envelope->flags & TA_ENVELOPE_WITHDRAWN) == 0 ? 1u : 0u;
    }
    return count;
}

/* ta_mail_follow: see mail.h. */
ta_mail_follow_status ta_mail_follow(ta_system *system, uint32_t index, ta_actor **out) {
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    ta_actor_ref routed;
    ta_actor_ref session = { .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = envelope->target };
    if (ta_session_route(system, &session, &routed)) {
        /* The session is out: it has an address, which only a load answers. The envelope takes its own reference. */
        ta_mail_post_waiting(system, index, routed.as.named);
        ta_actor_ref_release(&routed);
        return TA_MAIL_WAITS;
    }
    ta_actor *receiver = routed.kind == TA_ACTOR_REF_RUNTIME ? ta_actor_lookup(system, routed.as.runtime) : NULL;
    if (receiver == NULL || receiver->queued == UINT16_MAX) {
        return TA_MAIL_GONE;
    }
    envelope->target = receiver->record.id;
    receiver->queued += 1u;
    *out = receiver;
    return TA_MAIL_FOLLOWED;
}

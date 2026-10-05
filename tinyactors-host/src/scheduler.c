#include "tinyactors_host/scheduler.h"

#include "memory.h"

#include "tinyactors_host/allocator.h"
#include "allocator_private.h"
#include "scheduler_private.h"

#define TAH_QUEUE_CHUNK 1024u

/* ------------------------------------------------------------------------
 * The envelope store
 */

static void tah_envelopes_init(tah_envelopes *store, ta_allocator allocator) {
    *store = (tah_envelopes){ .allocator = allocator, .issued = 1u }; /* index 0 means none */
    tah_vec_init(&store->pages, allocator, TAH_LAYOUT(tah_envelope *));
}

tah_envelope *tah_sched_envelope(const tah_sched *sched, uint32_t index) {
    const tah_envelopes *store = &sched->envelopes;
    uint32_t page = index >> TAH_ENVELOPE_PAGE_BITS;
    if (index == 0 || index >= store->issued || page >= store->pages.count) {
        return NULL;
    }
    tah_envelope **entries = tah_vec_at(&store->pages, page);
    return &(*entries)[index & (TAH_ENVELOPE_PAGE_ENTRIES - 1u)];
}

/* A fresh envelope index, or 0 when no memory. */
static uint32_t tah_envelope_take(tah_sched *sched) {
    tah_envelopes *store = &sched->envelopes;
    uint32_t index = store->free_head;
    if (index != 0) {
        tah_envelope *envelope = tah_sched_envelope(sched, index);
        store->free_head = envelope->next;
    } else {
        if ((store->issued >> TAH_ENVELOPE_PAGE_BITS) >= store->pages.count) {
            /* Every issued index is in use and the last page is full: add a page. */
            size_t bytes = TAH_ENVELOPE_PAGE_ENTRIES * sizeof(tah_envelope);
            tah_envelope *page = tah_alloc(store->allocator, bytes, _Alignof(tah_envelope));
            if (page == NULL) {
                return 0;
            }
            if (tah_vec_push(&store->pages, &page) != TA_OK) {
                tah_free(store->allocator,
                         &(ta_allocator_free_params){ .ptr = page, .size = bytes, .alignment = _Alignof(tah_envelope) });
                return 0;
            }
        }
        index = store->issued;
        store->issued += 1u;
    }
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    *envelope = (tah_envelope){ .live = 1u };
    store->live += 1u;
    return index;
}

static size_t tah_jump_size(uint32_t count) {
    return sizeof(tah_jump) + (size_t)count * sizeof(ta_state_id);
}

/* Frees the operator transition an envelope carries, if any. */
static void tah_jump_give(tah_sched *sched, tah_envelope *envelope) {
    if (!envelope->has_jump) {
        return;
    }
    tah_free(sched->envelopes.allocator, &(ta_allocator_free_params){
                                             .ptr = envelope->jump,
                                             .size = tah_jump_size(envelope->jump->count),
                                             .alignment = _Alignof(tah_jump),
                                         });
    envelope->jump = NULL;
    envelope->has_jump = 0;
}

/* Tells the recorder, when there is one (kernel ADR 0006 section 2). */
static void tah_sched_note_to(const tah_sched *sched, tah_sched_note note) {
    if (sched->observer.self == NULL) {
        return;
    }
    note.message = note.envelope != 0 ? tah_sched_envelope(sched, note.envelope) : NULL;
    sched->observer.note(sched->observer.self, &note);
}

/* Releases the envelope's references and returns it to the free list. */
static void tah_envelope_give(tah_sched *sched, uint32_t index) {
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    if (envelope->overflow) {
        sched->overflow -= 1u;
    } else {
        sched->retained -= envelope->bytes;
        sched->queued -= 1u;
    }
    ta_event_release(&envelope->event);
    if (envelope->has_jump) {
        tah_jump_give(sched, envelope);
    } else {
        ta_address_release(envelope->address);
    }
    if (envelope->trace != NULL) {
        tah_free_list_give(&sched->traces, envelope->trace);
    }
    *envelope = (tah_envelope){ .next = sched->envelopes.free_head };
    sched->envelopes.free_head = index;
    sched->envelopes.live -= 1u;
}

static void tah_envelopes_destroy(tah_envelopes *store) {
    for (uint32_t page = 0; page < store->pages.count; page += 1) {
        tah_envelope **entries = tah_vec_at(&store->pages, page);
        tah_free(store->allocator, &(ta_allocator_free_params){
                                       .ptr = *entries,
                                       .size = TAH_ENVELOPE_PAGE_ENTRIES * sizeof(tah_envelope),
                                       .alignment = _Alignof(tah_envelope),
                                   });
    }
    tah_vec_destroy(&store->pages);
    store->issued = 1u;
    store->free_head = 0;
    store->live = 0;
}

const char *tah_dead_reason_name(tah_dead_reason reason) {
    static const char *const names[TAH_DEAD_REASONS] = {
        [TAH_DEAD_NOT_FOUND] = "not-found",       [TAH_DEAD_DONE] = "done",
        [TAH_DEAD_DESTROYED] = "destroyed",       [TAH_DEAD_REFUSED] = "refused",
        [TAH_DEAD_UNLOADED] = "unloaded",         [TAH_DEAD_REJECTED] = "rejected",
        [TAH_DEAD_LOAD_FAILED] = "load-failed",   [TAH_DEAD_LOAD_TIMEOUT] = "load-timeout",
        [TAH_DEAD_WAITING_FULL] = "waiting-full",
    };
    return (uint32_t)reason < TAH_DEAD_REASONS ? names[reason] : "not-found";
}

/* ------------------------------------------------------------------------
 * The table
 */

void tah_sched_init(tah_sched *sched, ta_allocator allocator, ta_allocator envelopes) {
    *sched = (tah_sched){ .allocator = allocator };
    tah_vec_init(&sched->pages, allocator, TAH_LAYOUT(tah_sched_entry *));
    tah_ring_shape shape = { .layout = TAH_LAYOUT(ta_actor_id), .chunk_capacity = TAH_QUEUE_CHUNK };
    tah_ring_init(&sched->runnable, allocator, shape);
    tah_ring_init(&sched->ready, allocator, shape);    tah_ring_init(&sched->room, allocator, shape);
    tah_envelopes_init(&sched->envelopes, envelopes);
    tah_free_list_init(&sched->traces, envelopes, TAH_LAYOUT(ta_trace_parent));
    tah_vec_init(&sched->waiters, envelopes, TAH_LAYOUT(tah_waiter));
    tah_map_init(&sched->inflight, allocator);
    /* One chunk holds a default turn's settlements (messageBudget and
     * resumesPerTurn are 1024 by default), so steady state reuses it. */
    tah_ring_init(&sched->settled, allocator,
                  (tah_ring_shape){ .layout = TAH_LAYOUT(tah_settlement), .chunk_capacity = TAH_QUEUE_CHUNK });
}

void tah_sched_discard_mail(tah_sched *sched) {
    tah_envelopes *store = &sched->envelopes;
    for (uint32_t index = 1; index < store->issued; index += 1) {
        tah_envelope *envelope = tah_sched_envelope(sched, index);
        if (envelope->live) {
            ta_event_release(&envelope->event);
            envelope->event = (ta_event){0};
            if (envelope->has_jump) {
                tah_jump_give(sched, envelope);
            } else {
                ta_address_release(envelope->address);
                envelope->address = NULL;
            }
            if (envelope->trace != NULL) {
                tah_free_list_give(&sched->traces, envelope->trace);
                envelope->trace = NULL;
            }
        }
    }
}

void tah_sched_destroy(tah_sched *sched) {
    for (uint32_t page = 0; page < sched->pages.count; page += 1) {
        tah_sched_entry **entries = tah_vec_at(&sched->pages, page);
        if (*entries != NULL) {
            tah_free(sched->allocator, &(ta_allocator_free_params){
                                           .ptr = *entries,
                                           .size = TAH_SCHED_PAGE_ENTRIES * sizeof(tah_sched_entry),
                                           .alignment = _Alignof(tah_sched_entry),
                                       });
        }
    }
    tah_vec_destroy(&sched->pages);
    tah_ring_destroy(&sched->runnable);
    tah_ring_destroy(&sched->ready);
    tah_ring_destroy(&sched->room);
    tah_sched_discard_mail(sched); /* gives back the trace parents envelopes hold */
    tah_envelopes_destroy(&sched->envelopes);
    tah_free_list_destroy(&sched->traces);
    tah_vec_destroy(&sched->waiters);
    tah_map_destroy(&sched->inflight);
    tah_ring_destroy(&sched->settled);
    sched->waiter_free = 0;
    sched->waiters_live = 0;
    sched->retained = 0;
    sched->known = 0;
    sched->queued = 0;
    sched->overflow = 0;
    sched->dead_first = 0;
    sched->dead_last = 0;
    sched->dead_count = 0;
    sched->records = NULL;
    sched->last_record = NULL;
    sched->record_count = 0;
}

tah_sched_entry *tah_sched_entry_at(const tah_sched *sched, uint32_t slot) {
    uint32_t page = slot >> TAH_SCHED_PAGE_BITS;
    if (page >= sched->pages.count) {
        return NULL;
    }
    tah_sched_entry **entries = tah_vec_at(&sched->pages, page);
    return *entries == NULL ? NULL : &(*entries)[slot & (TAH_SCHED_PAGE_ENTRIES - 1u)];
}

tah_sched_entry *tah_sched_lookup(const tah_sched *sched, ta_actor_id actor) {
    tah_sched_entry *entry = tah_sched_entry_at(sched, ta_actor_id_slot(actor));
    if (entry == NULL || entry->state == TAH_SCHED_NONE || entry->generation != ta_actor_id_generation(actor)) {
        return NULL;
    }
    return entry;
}

/* The entry of `slot`, allocating its page (and the directory up to it). */
static tah_sched_entry *tah_sched_entry_make(tah_sched *sched, uint32_t slot) {
    uint32_t page = slot >> TAH_SCHED_PAGE_BITS;
    if (page >= sched->pages.count) {
        if (tah_vec_reserve(&sched->pages, page + 1u) != TA_OK) {
            return NULL;
        }
        tah_sched_entry *none = NULL;
        while (sched->pages.count <= page) {
            if (tah_vec_push(&sched->pages, &none) != TA_OK) {
                return NULL;
            }
        }
    }
    tah_sched_entry **entries = tah_vec_at(&sched->pages, page);
    if (*entries == NULL) {
        size_t bytes = TAH_SCHED_PAGE_ENTRIES * sizeof(tah_sched_entry);
        *entries = tah_alloc(sched->allocator, bytes, _Alignof(tah_sched_entry));
        if (*entries == NULL) {
            return NULL;
        }
        tah_mem_set(*entries, 0, bytes);
    }
    return &(*entries)[slot & (TAH_SCHED_PAGE_ENTRIES - 1u)];
}

/* Appends one taken envelope to the dead list. */
static void tah_sched_dead_append(tah_sched *sched, uint32_t index, tah_dead_reason reason) {
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    if (envelope->report != TAH_REPORT_NONE) {
        /* A report about an invocation whose parent is gone tells nobody
         * anything: it is not mail, so it is no dead letter either. */
        tah_envelope_give(sched, index);
        return;
    }
    envelope->reason = (uint8_t)reason;
    envelope->next = 0;
    if (sched->dead_last != 0) {
        tah_sched_envelope(sched, sched->dead_last)->next = index;
    } else {
        sched->dead_first = index;
    }
    sched->dead_last = index;
    sched->dead_count += 1u;
}

/* Moves the entry's whole mailbox to the dead list (ADR 0005 point 10). */
static void tah_sched_bury(tah_sched *sched, tah_sched_entry *entry, tah_dead_reason reason) {
    uint32_t index = entry->first;
    while (index != 0) {
        uint32_t next = tah_sched_envelope(sched, index)->next;
        tah_sched_dead_append(sched, index, reason);
        index = next;
    }
    entry->first = 0;
    entry->last = 0;
    entry->parked = 0;
}

/* ------------------------------------------------------------------------
 * send() waiters
 */

tah_waiter *tah_sched_waiter(const tah_sched *sched, uint32_t index) {
    if (index == 0 || index >= sched->waiters.count) {
        return NULL;
    }
    return tah_vec_at(&sched->waiters, index);
}

uint32_t tah_sched_waiter_take(tah_sched *sched) {
    if (sched->waiters.count == 0) {
        tah_waiter none = { .generation = 1u };
        if (tah_vec_push(&sched->waiters, &none) != TA_OK) { /* index 0 is never issued */
            return 0;
        }
    }
    uint32_t index = sched->waiter_free;
    if (index != 0) {
        sched->waiter_free = tah_sched_waiter(sched, index)->next_free;
    } else {
        tah_waiter fresh = { .generation = 1u };
        if (tah_vec_push(&sched->waiters, &fresh) != TA_OK) {
            return 0;
        }
        index = sched->waiters.count - 1u;
    }
    tah_waiter *waiter = tah_sched_waiter(sched, index);
    waiter->state = TAH_WAITER_SETTLING; /* the caller's until a post takes it */
    waiter->envelope = 0;
    waiter->actor = 0;
    waiter->next_free = 0;
    waiter->host = NULL;
    sched->waiters_live += 1u;
    return index;
}

void tah_sched_waiter_set_host(tah_sched *sched, uint32_t index, void *host) {
    tah_waiter *waiter = tah_sched_waiter(sched, index);
    if (waiter != NULL) {
        waiter->host = host;
    }
}

void *tah_sched_waiter_host(const tah_sched *sched, uint32_t index) {
    const tah_waiter *waiter = tah_sched_waiter(sched, index);
    return waiter == NULL ? NULL : waiter->host;
}

uint32_t tah_sched_waiter_generation(const tah_sched *sched, uint32_t index) {
    const tah_waiter *waiter = tah_sched_waiter(sched, index);
    return waiter == NULL ? 0 : waiter->generation;
}

uint32_t tah_sched_next_waiter(const tah_sched *sched, uint32_t after) {
    for (uint32_t index = after + 1u; index < sched->waiters.count; index += 1u) {
        const tah_waiter *waiter = tah_vec_at(&sched->waiters, index);
        if (waiter->state != TAH_WAITER_FREE) {
            return index;
        }
    }
    return 0;
}

void tah_sched_waiter_give(tah_sched *sched, uint32_t index) {
    tah_waiter *waiter = tah_sched_waiter(sched, index);
    if (waiter == NULL || waiter->state == TAH_WAITER_FREE) {
        return;
    }
    waiter->generation = waiter->generation == UINT32_MAX ? 1u : waiter->generation + 1u;
    waiter->state = TAH_WAITER_FREE;
    waiter->host = NULL;
    waiter->next_free = sched->waiter_free;
    sched->waiter_free = index;
    sched->waiters_live -= 1u;
}

bool tah_sched_settlement_pop(tah_sched *sched, tah_settlement *out) {
    return tah_ring_pop(&sched->settled, out);
}

/* Queues a settlement for a waiter and marks it SETTLING. */
static void tah_sched_settle_waiter(tah_sched *sched, uint32_t index, tah_settlement settlement) {
    tah_waiter *waiter = tah_sched_waiter(sched, index);
    if (waiter == NULL) {
        return;
    }
    waiter->state = TAH_WAITER_SETTLING;
    settlement.waiter = index;
    /* The ring keeps a spare chunk, so this allocates only as it grows. If
     * it cannot, the promise is not settled until close() rejects it. */
    (void)tah_ring_push(&sched->settled, &settlement);
}

/* The macrostep `actor` ran for a send() ended: settle its waiter. */
static void tah_sched_complete(tah_sched *sched, ta_actor_id actor, tah_settlement settlement) {
    uint64_t index = 0;
    if (tah_map_count(&sched->inflight) == 0 || !tah_map_get(&sched->inflight, actor, &index)) {
        return;
    }
    tah_map_remove(&sched->inflight, actor);
    settlement.actor = actor;
    tah_sched_settle_waiter(sched, (uint32_t)index, settlement);
}

/* Unlinks `index` from the entry's mailbox; false when it is not there. */
static bool tah_sched_unlink(tah_sched *sched, tah_sched_entry *entry, uint32_t index) {
    uint32_t previous = 0;
    for (uint32_t at = entry->first; at != 0; at = tah_sched_envelope(sched, at)->next) {
        if (at == index) {
            uint32_t next = tah_sched_envelope(sched, at)->next;
            if (previous == 0) {
                entry->first = next;
            } else {
                tah_sched_envelope(sched, previous)->next = next;
            }
            if (entry->last == index) {
                entry->last = previous;
            }
            return true;
        }
        previous = at;
    }
    return false;
}

bool tah_sched_cancel(tah_sched *sched, uint32_t index) {
    tah_waiter *waiter = tah_sched_waiter(sched, index);
    if (waiter == NULL) {
        return false;
    }
    if (waiter->state == TAH_WAITER_QUEUED) {
        /* A message waiting for a load is withdrawn by tah_system_cancel(). */
        tah_envelope *envelope = tah_sched_envelope(sched, waiter->envelope);
        tah_sched_entry *entry = tah_sched_lookup(sched, waiter->actor);
        if (envelope != NULL && entry != NULL && tah_sched_unlink(sched, entry, waiter->envelope)) {
            /* A ready actor whose mailbox is now empty becomes idle when popped. */
            tah_envelope_give(sched, waiter->envelope);
        } else if (envelope != NULL) {
            envelope->waiter = 0; /* on the dead list: still reported, without the waiter */
        }
        waiter->state = TAH_WAITER_SETTLING;
        return true;
    }
    if (waiter->state == TAH_WAITER_INFLIGHT) {
        tah_map_remove(&sched->inflight, waiter->actor);
        waiter->state = TAH_WAITER_SETTLING;
        return true;
    }
    return false;
}

ta_result tah_sched_add(tah_sched *sched, ta_actor_id actor) {
    tah_sched_entry *entry = tah_sched_entry_make(sched, ta_actor_id_slot(actor));
    if (entry == NULL) {
        return TA_ERR_NOMEM;
    }
    if (tah_ring_push(&sched->runnable, &actor) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    if (entry->state == TAH_SCHED_NONE) {
        sched->known += 1u;
    } else {
        /* A previous actor of this slot the addon never saw go: its mail is dead. */
        sched->quarantined -= entry->state == TAH_SCHED_QUARANTINED ? 1u : 0u;
        tah_sched_bury(sched, entry, TAH_DEAD_NOT_FOUND);
        tah_sched_complete(sched, ta_actor_id_make(ta_actor_id_slot(actor), entry->generation),
                           (tah_settlement){ .kind = TAH_SETTLE_DEAD, .reason = TAH_DEAD_NOT_FOUND });
    }
    *entry = (tah_sched_entry){ .generation = ta_actor_id_generation(actor), .state = TAH_SCHED_RUNNABLE };
    return TA_OK;
}

void tah_sched_remove(tah_sched *sched, ta_actor_id actor, tah_dead_reason reason) {
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry == NULL) {
        return;
    }
    tah_sched_bury(sched, entry, reason);
    tah_sched_complete(sched, actor, (tah_settlement){ .kind = TAH_SETTLE_DEAD, .reason = (uint8_t)reason });
    sched->quarantined -= entry->state == TAH_SCHED_QUARANTINED ? 1u : 0u;
    *entry = (tah_sched_entry){0};
    sched->known -= 1u;
}

/*
 * Moves the list `first`..`last`, in order, to the front of `to`'s mailbox,
 * with its send() waiters; an idle `to` becomes ready. TA_ERR_NOMEM, with
 * nothing moved, when `to` cannot join the ready queue.
 */
typedef struct {
    uint32_t first;
    uint32_t last;
    ta_actor_id to;
    tah_sched_entry *entry; /* `to`'s */
} tah_splice;

static ta_result tah_sched_splice_front(tah_sched *sched, const tah_splice *splice) {
    tah_sched_entry *to = splice->entry;
    if (splice->first == 0) {
        return TA_OK;
    }
    /* A parked `to` gets a new head, which may not wait for room: it goes
     * back on the ready queue, and its entry on the room queue is skipped. */
    bool unpark = to->parked && to->state == TAH_SCHED_READY;
    if ((to->state == TAH_SCHED_IDLE || unpark) && tah_ring_push(&sched->ready, &splice->to) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    to->parked = 0;
    for (uint32_t index = splice->first; index != 0;) {
        tah_envelope *envelope = tah_sched_envelope(sched, index);
        envelope->target = splice->to;
        tah_waiter *waiter = tah_sched_waiter(sched, envelope->waiter);
        if (waiter != NULL && waiter->state == TAH_WAITER_QUEUED) {
            waiter->actor = splice->to;
        }
        index = envelope->next;
    }
    tah_sched_envelope(sched, splice->last)->next = to->first;
    if (to->last == 0) {
        to->last = splice->last;
    }
    to->first = splice->first;
    if (to->state == TAH_SCHED_IDLE) {
        to->state = TAH_SCHED_READY;
    }
    return TA_OK;
}

ta_result tah_sched_transfer(tah_sched *sched, const tah_sched_transfer_params *params) {
    tah_sched_entry *from = tah_sched_lookup(sched, params->from);
    tah_sched_entry *to = tah_sched_lookup(sched, params->to);
    if (from == NULL || to == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (from == to || from->first == 0) {
        return TA_OK;
    }
    ta_result result = tah_sched_splice_front(
        sched, &(tah_splice){ .first = from->first, .last = from->last, .to = params->to, .entry = to });
    if (result != TA_OK) {
        return result;
    }
    from->first = 0;
    from->last = 0;
    from->parked = 0;
    if (from->state == TAH_SCHED_READY) {
        from->state = TAH_SCHED_IDLE; /* its ready-queue entry is skipped */
    }
    return TA_OK;
}

ta_result tah_sched_hand_over(tah_sched *sched, tah_mailbox *box, ta_actor_id to) {
    tah_sched_entry *entry = tah_sched_lookup(sched, to);
    if (entry == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_result result = tah_sched_splice_front(
        sched, &(tah_splice){ .first = box->first, .last = box->last, .to = to, .entry = entry });
    if (result == TA_OK) {
        *box = (tah_mailbox){ 0 };
    }
    return result;
}

void tah_sched_bury_waiting(tah_sched *sched, tah_mailbox *box, tah_dead_reason reason) {
    uint32_t index = box->first;
    while (index != 0) {
        uint32_t next = tah_sched_envelope(sched, index)->next;
        tah_sched_dead_append(sched, index, reason);
        index = next;
    }
    *box = (tah_mailbox){ 0 };
}

bool tah_sched_withdraw_waiting(tah_sched *sched, tah_mailbox *box, uint32_t index) {
    uint32_t previous = 0;
    for (uint32_t at = box->first; at != 0; at = tah_sched_envelope(sched, at)->next) {
        if (at == index) {
            uint32_t next = tah_sched_envelope(sched, at)->next;
            if (previous == 0) {
                box->first = next;
            } else {
                tah_sched_envelope(sched, previous)->next = next;
            }
            if (box->last == index) {
                box->last = previous;
            }
            box->count -= 1u;
            tah_envelope_give(sched, index);
            return true;
        }
        previous = at;
    }
    return false;
}

ta_result tah_sched_wake(tah_sched *sched, ta_actor_id actor) {
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (entry->state != TAH_SCHED_IDLE && entry->state != TAH_SCHED_READY) {
        return TA_OK;
    }
    /* A ready actor's entry in the ready queue, or the room queue, is skipped once it is runnable. */
    if (tah_ring_push(&sched->runnable, &actor) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    entry->state = TAH_SCHED_RUNNABLE;
    entry->parked = 0;
    return TA_OK;
}

ta_result tah_sched_release(tah_sched *sched, ta_actor_id actor) {
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (entry->state != TAH_SCHED_QUARANTINED) {
        return TA_OK;
    }
    if (tah_ring_push(&sched->runnable, &actor) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    entry->state = TAH_SCHED_RUNNABLE;
    entry->flags &= (uint8_t)~TAH_SCHED_BACKOFF;
    sched->quarantined -= 1u;
    return TA_OK;
}

bool tah_sched_quarantine(tah_sched *sched, ta_actor_id actor) {
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry == NULL || entry->state == TAH_SCHED_QUARANTINED) {
        return false;
    }
    entry->state = TAH_SCHED_QUARANTINED;
    entry->flags &= (uint8_t)~TAH_SCHED_BACKOFF;
    sched->quarantined += 1u;
    sched->faults[0] += 1u;
    return true;
}

ta_result tah_sched_abandoned(tah_sched *sched, ta_actor_id actor) {
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    tah_sched_complete(sched, actor, (tah_settlement){ .kind = TAH_SETTLE_ABANDONED });
    if (entry->state != TAH_SCHED_RUNNABLE) {
        return TA_OK; /* quarantined, or core abandoned an event the addon never delivered */
    }
    entry->flags &= (uint8_t)~TAH_SCHED_BACKOFF;
    if (entry->first == 0) {
        entry->state = TAH_SCHED_IDLE;
        return TA_OK;
    }
    entry->state = TAH_SCHED_READY;
    if (tah_ring_push(&sched->ready, &actor) != TA_OK) {
        /* No room in the ready queue: it stays runnable, and its entry in the
         * runnable queue resumes it, which finds it stable and settles it. */
        entry->state = TAH_SCHED_RUNNABLE;
    }
    return TA_OK;
}

/* The limits a message must fit before it is accepted (ADR 0005 point 9). */
static bool tah_sched_full(const tah_sched *sched, uint32_t bytes) {
    return (sched->max_queued != 0 && sched->queued >= sched->max_queued) ||
           (sched->max_retained != 0 && sched->retained + bytes > sched->max_retained);
}

bool tah_sched_quiet(const tah_sched *sched) {
    return tah_ring_count(&sched->runnable) == 0 && tah_ring_count(&sched->ready) == 0 && sched->dead_count == 0 &&
           tah_ring_count(&sched->settled) == 0 && sched->stranded == 0 &&
           (tah_ring_count(&sched->room) == 0 || tah_sched_full(sched, 0));
}

uint32_t tah_sched_mailbox_count(const tah_sched *sched, ta_actor_id actor) {
    const tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    uint32_t count = 0;
    for (uint32_t index = entry == NULL ? 0 : entry->first; index != 0;
         index = tah_sched_envelope(sched, index)->next) {
        count += 1u;
    }
    return count;
}

tah_sched_state tah_sched_state_of(const tah_sched *sched, ta_actor_id actor) {
    const tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    return entry == NULL ? TAH_SCHED_NONE : (tah_sched_state)entry->state;
}

uint32_t tah_sched_mailbox_first(const tah_sched *sched, ta_actor_id actor) {
    const tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    return entry == NULL ? 0 : entry->first;
}

tah_queued_message tah_sched_queued_message(const tah_sched *sched, uint32_t envelope) {
    const tah_envelope *message = tah_sched_envelope(sched, envelope);
    return (tah_queued_message){ .event = &message->event,
                                 .jump = message->has_jump ? message->jump : NULL,
                                 .waiter = message->waiter,
                                 .next = message->next,
                                 .overflow = message->overflow != 0 };
}

void tah_sched_read_stats(const tah_sched *sched, tah_sched_stats *out) {
    *out = (tah_sched_stats){ .runnable = tah_ring_count(&sched->runnable),
                              .ready = tah_ring_count(&sched->ready),
                              .quarantined = sched->quarantined,
                              .queued = sched->queued + sched->overflow,
                              .overflow = sched->overflow,
                              .delivered = sched->delivered,
                              .dead_letters = sched->dead_letters };
    for (uint32_t reason = 0; reason < TAH_DEAD_REASONS; reason += 1u) {
        out->dead_by_reason[reason] = sched->dead_by_reason[reason];
    }
    for (uint32_t code = 0; code < TAH_FAULT_CODES; code += 1u) {
        out->faults[code] = sched->faults[code];
    }
}

/* ------------------------------------------------------------------------
 * Posting
 */

/* A copy of the message's trace parent from the free list, or NULL for an
 * untraced message. False when there is no memory for one. */
static bool tah_sched_take_trace(tah_sched *sched, const tah_message *message, ta_trace_parent **out) {
    *out = NULL;
    const ta_trace_parent *trace = message->trace;
    if (trace == NULL || (trace->context.trace_id_high == 0 && trace->context.trace_id_low == 0 &&
                          trace->cause.session == 0 && trace->cause.macrostep == 0)) {
        return true;
    }
    *out = tah_free_list_take(&sched->traces);
    if (*out == NULL) {
        return false;
    }
    **out = *trace;
    return true;
}

/* A copy of the message's operator transition, or NULL for none. False when
 * there is no memory for one. */
static bool tah_sched_take_jump(tah_sched *sched, const tah_message *message, tah_jump **out) {
    *out = NULL;
    const tah_message_jump *jump = message->jump;
    if (jump == NULL) {
        return true;
    }
    uint32_t count = jump->transition.target_count;
    *out = tah_alloc(sched->envelopes.allocator, tah_jump_size(count), _Alignof(tah_jump));
    if (*out == NULL) {
        return false;
    }
    (*out)->definition = jump->definition;
    (*out)->source = jump->transition.source;
    (*out)->count = count;
    if (count != 0) {
        tah_mem_copy((*out)->targets, jump->transition.targets, (size_t)count * sizeof(ta_state_id));
    }
    return true;
}

/* Fills a taken envelope from the message and attaches its waiter. */
static void tah_sched_fill(tah_sched *sched, uint32_t index, const tah_message *message, ta_trace_parent *trace) {
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    envelope->trace = trace;
    envelope->event = *message->event;
    envelope->target = message->target;
    envelope->bytes = message->bytes;
    envelope->waiter = message->waiter;
    envelope->report = message->report;
    sched->queued += 1u;
    sched->retained += message->bytes;
    tah_waiter *waiter = tah_sched_waiter(sched, message->waiter);
    if (waiter != NULL) {
        waiter->state = TAH_WAITER_QUEUED;
        waiter->envelope = index;
        waiter->actor = message->target;
    }
}

/*
 * Whether `message` is accepted past the limits, as overflow (kernel ADR
 * 0003). TA_ERR_BUSY when it is full and the message may not overflow.
 */
static ta_result tah_sched_admit(const tah_sched *sched, const tah_message *message, bool *overflow) {
    *overflow = tah_sched_full(sched, message->bytes);
    return *overflow && !message->may_overflow ? TA_ERR_BUSY : TA_OK;
}

/* Counts a filled envelope as overflow instead of queued (kernel ADR 0003). */
static void tah_sched_mark_overflow(tah_sched *sched, uint32_t index) {
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    envelope->overflow = 1u;
    sched->queued -= 1u;
    sched->retained -= envelope->bytes;
    sched->overflow += 1u;
}

/* Moves an overflowed envelope into the mailbox: it counts against the limits from now on. */
static void tah_sched_move_in(tah_sched *sched, tah_envelope *envelope) {
    envelope->overflow = 0;
    sched->overflow -= 1u;
    sched->queued += 1u;
    sched->retained += envelope->bytes;
}

ta_result tah_sched_post(tah_sched *sched, const tah_message *message) {
    ta_actor_id target = message->target;
    bool overflow = false;
    if (tah_sched_admit(sched, message, &overflow) != TA_OK) {
        return TA_ERR_BUSY;
    }
    tah_sched_entry *entry = tah_sched_lookup(sched, target);
    bool dead = entry == NULL || (entry->flags & TAH_SCHED_FINISHED) != 0;
    /* An idle actor joins the ready queue; reserve that before taking mail. */
    if (!dead && entry->state == TAH_SCHED_IDLE && tah_ring_push(&sched->ready, &target) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    ta_trace_parent *trace = NULL;
    if (!tah_sched_take_trace(sched, message, &trace)) {
        return TA_ERR_NOMEM; /* a ready-queue entry pushed above names an idle actor; delivery skips it */
    }
    tah_jump *jump = NULL;
    uint32_t index = tah_sched_take_jump(sched, message, &jump) ? tah_envelope_take(sched) : 0;
    if (index == 0) {
        /* A ready-queue entry pushed above names an idle actor; delivery skips it. */
        if (trace != NULL) {
            tah_free_list_give(&sched->traces, trace);
        }
        if (jump != NULL) {
            tah_free(sched->envelopes.allocator, &(ta_allocator_free_params){
                                                     .ptr = jump,
                                                     .size = tah_jump_size(jump->count),
                                                     .alignment = _Alignof(tah_jump),
                                                 });
        }
        return TA_ERR_NOMEM;
    }
    tah_sched_fill(sched, index, message, trace);
    if (overflow) {
        tah_sched_mark_overflow(sched, index);
    }
    if (jump != NULL) {
        tah_envelope *envelope = tah_sched_envelope(sched, index);
        envelope->jump = jump;
        envelope->has_jump = 1u;
    }
    if (dead) {
        tah_sched_dead_append(sched, index, entry == NULL ? TAH_DEAD_NOT_FOUND : TAH_DEAD_DONE);
        return TA_OK;
    }
    if (entry->last != 0) {
        tah_sched_envelope(sched, entry->last)->next = index;
    } else {
        entry->first = index;
    }
    entry->last = index;
    if (entry->state == TAH_SCHED_IDLE) {
        entry->state = TAH_SCHED_READY;
    }
    return TA_OK;
}

ta_result tah_sched_post_waiting(tah_sched *sched, const tah_waiting_post *post) {
    bool overflow = false;
    if (tah_sched_admit(sched, post->message, &overflow) != TA_OK) {
        return TA_ERR_BUSY;
    }
    ta_trace_parent *trace = NULL;
    if (!tah_sched_take_trace(sched, post->message, &trace)) {
        return TA_ERR_NOMEM;
    }
    uint32_t index = tah_envelope_take(sched);
    if (index == 0) {
        if (trace != NULL) {
            tah_free_list_give(&sched->traces, trace);
        }
        return TA_ERR_NOMEM;
    }
    tah_message message = *post->message;
    message.target = 0; /* no actor yet: the hand-over names it */
    tah_sched_fill(sched, index, &message, trace);
    if (overflow) {
        tah_sched_mark_overflow(sched, index);
    }
    ta_address_retain(post->address);
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    envelope->address = post->address; /* a dead letter names it if the load fails */
    tah_mailbox *box = post->box;
    if (box->last != 0) {
        tah_sched_envelope(sched, box->last)->next = index;
    } else {
        box->first = index;
    }
    box->last = index;
    box->count += 1u;
    return TA_OK;
}

ta_result tah_sched_post_dead(tah_sched *sched, const tah_dead_post *post) {
    bool overflow = false;
    if (tah_sched_admit(sched, post->message, &overflow) != TA_OK) {
        return TA_ERR_BUSY;
    }
    ta_trace_parent *trace = NULL;
    if (!tah_sched_take_trace(sched, post->message, &trace)) {
        return TA_ERR_NOMEM;
    }
    uint32_t index = tah_envelope_take(sched);
    if (index == 0) {
        if (trace != NULL) {
            tah_free_list_give(&sched->traces, trace);
        }
        return TA_ERR_NOMEM;
    }
    tah_sched_fill(sched, index, post->message, trace);
    if (overflow) {
        tah_sched_mark_overflow(sched, index);
    }
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    envelope->target = 0;
    envelope->address = post->address;
    tah_sched_dead_append(sched, index, post->reason);
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * The turn
 */

static void tah_sched_record(tah_sched *sched, const tah_turn_params *params, tah_turn_record record) {
    tah_turn_record *copy = ta_arena_alloc(params->arena, sizeof *copy, _Alignof(tah_turn_record));
    if (copy == NULL) {
        /* The turn arena is out of memory: the hook is not called, and what
         * the record owned goes back now. */
        ta_value_release(&record.data);
        return;
    }
    *copy = record;
    copy->next = NULL;
    if (sched->last_record != NULL) {
        sched->last_record->next = copy;
    } else {
        sched->records = copy;
    }
    sched->last_record = copy;
    sched->record_count += 1u;
}

/* Quarantines the record's actor (ADR 0008) and records the fault. */
static void tah_sched_fault(tah_sched *sched, const tah_turn_params *params, tah_turn_record record) {
    tah_sched_entry *entry = tah_sched_lookup(sched, record.actor);
    uint32_t code = record.code < 0 ? (uint32_t)(-(int64_t)record.code) : 0u;
    sched->faults[code < TAH_FAULT_CODES ? code : 0u] += 1u;
    if (entry != NULL) {
        sched->quarantined += entry->state == TAH_SCHED_QUARANTINED ? 0u : 1u;
        entry->state = TAH_SCHED_QUARANTINED;
        entry->flags &= (uint8_t)~TAH_SCHED_BACKOFF;
    }
    record.kind = TAH_RECORD_FAULT;
    tah_sched_record(sched, params, record);
}

/*
 * Puts an actor back at the end of `queue`, the one its state names. Out of
 * memory, it is stranded: it keeps its state and its mail, and the next
 * turn tries again (tah_sched_unstrand), so a quiet scheduler never holds
 * an actor with work to do.
 */
static void tah_sched_requeue(tah_sched *sched, tah_ring *queue, ta_actor_id actor) {
    if (tah_ring_push(queue, &actor) == TA_OK) {
        return;
    }
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry != NULL && (entry->flags & TAH_SCHED_STRANDED) == 0) {
        entry->flags |= TAH_SCHED_STRANDED;
        sched->stranded += 1u;
    }
}

/*
 * Puts stranded actors back on their queues, and recounts the ones still
 * stranded. A walk of the table, only after a requeue ran out of memory.
 */
static void tah_sched_unstrand(tah_sched *sched) {
    if (sched->stranded == 0) {
        return;
    }
    uint32_t left = 0;
    for (uint32_t page = 0; page < sched->pages.count; page += 1u) {
        tah_sched_entry **entries = tah_vec_at(&sched->pages, page);
        for (uint32_t at = 0; *entries != NULL && at < TAH_SCHED_PAGE_ENTRIES; at += 1u) {
            tah_sched_entry *entry = &(*entries)[at];
            if ((entry->flags & TAH_SCHED_STRANDED) == 0) {
                continue;
            }
            tah_ring *queue = entry->state == TAH_SCHED_READY      ? &sched->ready
                              : entry->state == TAH_SCHED_RUNNABLE ? &sched->runnable
                                                                   : NULL;
            ta_actor_id actor = ta_actor_id_make((page << TAH_SCHED_PAGE_BITS) | at, entry->generation);
            if (queue != NULL && (left != 0 || tah_ring_push(queue, &actor) != TA_OK)) {
                left += 1u; /* still no memory: the next turn tries again */
                continue;
            }
            entry->flags &= (uint8_t)~TAH_SCHED_STRANDED;
        }
    }
    sched->stranded = left;
}

/* A stable actor: ready when it has mail, idle otherwise. */
static void tah_sched_settle(tah_sched *sched, tah_sched_entry *entry, ta_actor_id actor) {
    if (entry->first == 0) {
        entry->state = TAH_SCHED_IDLE;
        return;
    }
    entry->state = TAH_SCHED_READY;
    tah_sched_requeue(sched, &sched->ready, actor);
}

/* The turn left core: let the owner read what the call traced (tah_turn_params.left_core). */
static void tah_sched_left_core(const tah_turn_params *params) {
    if (params->left_core != NULL) {
        params->left_core(params->stop_context);
    }
}

/*
 * A session's input ended (a COMPLETED notice): settle the actor as the end of
 * its resume used to (kernel ADR 0005 point 11). An entry that is not
 * runnable is not waiting for it: the actor was removed, quarantined or
 * abandoned meanwhile, and its waiter was settled by whoever did that.
 */
static void tah_sched_completed(tah_sched *sched, const tah_turn_params *params, const ta_run_notice *notice) {
    ta_actor_id actor = notice->actor;
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry == NULL || entry->state != TAH_SCHED_RUNNABLE || notice->status == TA_INPUT_ABANDONED) {
        return;
    }
    tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_ENDED, .actor = actor });
    if (notice->status == TA_INPUT_DONE) {
        tah_sched_complete(sched, actor, (tah_settlement){ .kind = TAH_SETTLE_DONE, .ignored = notice->ignored });
        entry->state = TAH_SCHED_IDLE;
        tah_sched_bury(sched, entry, TAH_DEAD_DONE);
        if ((entry->flags & TAH_SCHED_FINISHED) == 0) {
            entry->flags |= TAH_SCHED_FINISHED;
            tah_sched_record(sched, params, (tah_turn_record){ .kind = TAH_RECORD_DONE, .actor = actor });
            tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_FINISHED, .actor = actor });
        }
        return;
    }
    /* Reaching the microstep limit abandoned the macrostep: the actor is stable, the fault hook hears of it, and
     * so does the send(). */
    bool runaway = notice->status == TA_INPUT_RUNAWAY;
    if (runaway) {
        sched->faults[TAH_FAULT_RUNAWAY] += 1u;
        tah_sched_record(sched, params, (tah_turn_record){ .kind = TAH_RECORD_FAULT, .actor = actor,
                                                           .operation = "run", .runaway = true });
    }
    tah_sched_complete(sched, actor, (tah_settlement){ .kind = runaway ? TAH_SETTLE_RUNAWAY : TAH_SETTLE_STABLE,
                                                       .ignored = notice->ignored });
    tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_SETTLED, .actor = actor });
    tah_sched_settle(sched, entry, actor);
}

/*
 * An admitted message core could not take (a REFUSED notice): the target went
 * or finished after core admitted it. It is a dead letter, as if
 * ta_actor_deliver() had said so, and the actor, if the table still knows it,
 * stands between macrosteps again. The event belongs to core until the next
 * ta_run(); the record keeps its own references.
 */
static void tah_sched_refused(tah_sched *sched, const tah_turn_params *params, const ta_run_notice *notice) {
    tah_dead_reason reason = notice->reason == TA_REFUSED_DONE      ? TAH_DEAD_DONE
                             : notice->reason == TA_REFUSED_INVALID ? TAH_DEAD_REFUSED
                                                                    : TAH_DEAD_NOT_FOUND;
    tah_turn_record record = {
        .kind = TAH_RECORD_DEAD_LETTER,
        .actor = notice->actor,
        .reason = reason,
        .event = notice->event != NULL ? notice->event->name : 0,
    };
    if (notice->event != NULL) {
        record.data = notice->event->data;
        ta_value_retain(&record.data);
    }
    sched->dead_letters += 1u;
    sched->dead_by_reason[reason] += 1u;
    tah_sched_record(sched, params, record);
    tah_sched_entry *entry = tah_sched_lookup(sched, notice->actor);
    tah_sched_complete(sched, notice->actor, (tah_settlement){ .kind = TAH_SETTLE_DEAD, .reason = (uint8_t)reason });
    if (entry != NULL && entry->state == TAH_SCHED_RUNNABLE) {
        tah_sched_settle(sched, entry, notice->actor);
    }
}

#define TAH_RUN_NOTICES 16u

ta_result tah_sched_run(tah_sched *sched, const tah_turn_params *params, tah_run_watch *watch, ta_run_status *status) {
    ta_result result = TA_OK;
    uint32_t budget = params->budgets.microsteps != 0 ? params->budgets.microsteps : 64u;
    do {
        if (sched->turn_bounded && sched->turn_left == 0) {
            *status = TA_RUN_MORE; /* the turn used its inputs: the rest waits for the next one */
            break;
        }
        if (sched->turn_bounded && sched->turn_left < budget) {
            budget = sched->turn_left; /* one microstep per input at least, so this ends the turn in time */
        }
        ta_run_notice notices[TAH_RUN_NOTICES];
        /* A host that polls `stop` (the trace staging bound) hears of each input as it ends: room for one input's
         * notices makes core stop after it, and the loop below asks. */
        uint32_t capacity = params->stop != NULL ? 3u : TAH_RUN_NOTICES;
        uint32_t count = 0;
        *params->inside_core = true;
        result = ta_run(params->core, &(ta_run_params){ .budget = budget,
                                                        .action_budget = params->budgets.actions,
                                                        .status = status,
                                                        .notices = notices,
                                                        .notice_capacity = capacity,
                                                        .notice_count = &count });
        *params->inside_core = false;
        tah_sched_left_core(params);
        for (uint32_t index = 0; index < count; index += 1u) {
            const ta_run_notice *notice = &notices[index];
            if (notice->kind == TA_NOTICE_INPUT_COMPLETED && sched->turn_left > 0) {
                sched->turn_left -= 1u;
            }
            tah_sched_entry *entry = tah_sched_lookup(sched, notice->actor);
            bool replayed = entry != NULL && (entry->flags & TAH_SCHED_REPLAYING) != 0;
            if (watch != NULL && notice->actor == watch->actor && notice->kind == TA_NOTICE_INPUT_COMPLETED) {
                watch->completed = true;
                watch->done = watch->done || notice->status == TA_INPUT_DONE;
                watch->ignored = watch->ignored || notice->ignored;
            } else if (replayed) {
                continue; /* the replay settles it itself */
            } else if (notice->kind == TA_NOTICE_INPUT_COMPLETED) {
                tah_sched_completed(sched, params, notice);
            } else if (notice->kind == TA_NOTICE_INPUT_REFUSED) {
                tah_sched_refused(sched, params, notice);
            }
        }
        if (*status == TA_RUN_NOTICES_FULL && params->stop != NULL && params->stop(params->stop_context)) {
            *status = TA_RUN_MORE; /* the host asked the turn to end: what core still holds waits for the next one */
            break;
        }
    } while (result == TA_OK && *status == TA_RUN_NOTICES_FULL);
    return result;
}

static void tah_sched_resume_one(tah_sched *sched, const tah_turn_params *params, ta_actor_id actor) {
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry == NULL || entry->state != TAH_SCHED_RUNNABLE) {
        return; /* destroyed, replaced, quarantined or settled by an earlier call since it was queued */
    }
    if ((entry->flags & TAH_SCHED_REPLAYING) != 0) {
        entry->state = TAH_SCHED_IDLE; /* its replay resumes it (napi ADR 0034) */
        return;
    }
    if ((entry->flags & TAH_SCHED_BACKOFF) != 0) {
        entry->flags &= (uint8_t)~TAH_SCHED_BACKOFF;
        tah_sched_requeue(sched, &sched->runnable, actor);
        return;
    }
    ta_run_status status = TA_RUN_IDLE;
    ta_result result = tah_sched_run(sched, params, NULL, &status);
    if (status == TA_RUN_PAUSED) {
        sched->turn_left = 0; /* a budget stopped an input inside its macrostep: the turn ends, the next continues it */
    }
    /* The table is the addon's: core calls never move its pages. */
    if (result == TA_ERR_NOMEM) {
        entry->flags |= TAH_SCHED_BACKOFF;
        tah_sched_requeue(sched, &sched->runnable, actor);
        return;
    }
    if (entry->state != TAH_SCHED_RUNNABLE) {
        return;
    }
    if (status == TA_RUN_IDLE) {
        /* Core has nothing for it: a released actor, or one whose macrostep ended with nobody to hear of it. It
         * stands stable, as a resume of an idle actor used to find it. */
        tah_sched_completed(sched, params, &(ta_run_notice){ .kind = TA_NOTICE_INPUT_COMPLETED, .actor = actor,
                                                              .status = TA_INPUT_STABLE });
        return;
    }
    /* Its input is paused by a budget, or waits behind others in core's queue: ask again next turn. */
    tah_sched_requeue(sched, &sched->runnable, actor);
}

/*
 * The operator transition of a jump envelope for ta_actor_deliver(), or NULL
 * when the actor no longer runs the definition its state IDs belong to (a
 * replacement took the mail over, ADR 0005 point 12). Only a jump pays for
 * the inspection.
 */
static const ta_operator_transition *tah_sched_jump(const tah_turn_params *params, ta_actor_id actor, const tah_jump *jump,
                                                    ta_operator_transition *out) {
    ta_actor_inspection view;
    *params->inside_core = true;
    ta_result result = ta_actor_inspect(params->core, actor, &view);
    *params->inside_core = false;
    tah_sched_left_core(params);
    if (result != TA_OK || view.definition_id != jump->definition) {
        return NULL;
    }
    *out = (ta_operator_transition){ .source = jump->source, .targets = jump->targets, .target_count = jump->count };
    return out;
}

/* The head envelope is a jump the actor does not accept: a dead letter with
 * reason `refused`, and the actor stands between macrosteps as before. */
static void tah_sched_refuse_jump(tah_sched *sched, tah_sched_entry *entry, ta_actor_id actor) {
    uint32_t head = entry->first;
    entry->first = tah_sched_envelope(sched, head)->next;
    if (entry->first == 0) {
        entry->last = 0;
    }
    tah_sched_dead_append(sched, head, TAH_DEAD_REFUSED);
    tah_sched_settle(sched, entry, actor);
}

/* The core reason token of a report envelope (core ADR 0081). */
static ta_str tah_report_reason(uint8_t kind) {
    switch (kind) {
    case TAH_REPORT_QUARANTINED: return (ta_str){ .ptr = "quarantined", .len = 11 };
    case TAH_REPORT_FAILED: return (ta_str){ .ptr = "failed", .len = 6 };
    default: return (ta_str){ .ptr = "unavailable", .len = 11 };
    }
}

/*
 * Offers the head envelope, an error report, to its idle parent (napi ADR
 * 0032 section 3). Taken, it starts a macrostep; a report core has no
 * invocation for any more (it ended, or the parent went) is dropped, and
 * the next mail follows; out of memory, or busy, it stays at the head.
 */

static void tah_sched_report_one(tah_sched *sched, const tah_turn_params *params, tah_sched_entry *entry,
                                 ta_actor_id actor, uint32_t head) {
    tah_envelope *envelope = tah_sched_envelope(sched, head);
    tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_REPORTING, .actor = actor, .envelope = head });
    ta_trace_parent trace = envelope->trace != NULL ? *envelope->trace : (ta_trace_parent){0};
    bool has_data = envelope->event.data.v != NULL;
    ta_invoke_error error = {
        .reason = tah_report_reason(envelope->report),
        .data = has_data ? &envelope->event.data : NULL,
        .ends = envelope->report != TAH_REPORT_QUARANTINED,
    };
    *params->inside_core = true;
    ta_result result = ta_actor_report_invoke(params->core, &(ta_invoke_report_params){ .time = ta_system_time(params->core),
                                                                .actor = actor,
                                                                .invoke_id = envelope->event.invoke_id,
                                                                .kind = TA_INVOKE_REPORT_ERROR,
                                                                .trace = trace,
                                                                .as.error = &error,
                                                            });
    *params->inside_core = false;
    tah_sched_left_core(params);
    if (result != TA_OK) {
        tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_UNDELIVERED, .actor = actor, .envelope = head });
    }
    if (result == TA_ERR_NOMEM || result == TA_ERR_BUSY) {
        if (result == TA_ERR_NOMEM) {
            entry->flags |= TAH_SCHED_BACKOFF; /* retryable (ADR 0008): try again next turn */
        }
        tah_sched_requeue(sched, &sched->ready, actor);
        return;
    }
    entry->first = envelope->next;
    if (entry->first == 0) {
        entry->last = 0;
    }
    if (result == TA_OK) {
        tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_REPORTED, .actor = actor, .envelope = head });
    }
    if (envelope->waiter != 0) {
        /* A replayed report (napi ADR 0032 section 7) follows the macrostep
         * it starts, as a delivered message's send() does. */
        tah_waiter *waiter = tah_sched_waiter(sched, envelope->waiter);
        if (result == TA_OK && tah_map_put(&sched->inflight, actor, envelope->waiter) == TA_OK) {
            waiter->state = TAH_WAITER_INFLIGHT;
            waiter->actor = actor;
            waiter->envelope = 0;
        } else {
            tah_sched_settle_waiter(sched, envelope->waiter,
                                    result == TA_OK
                                        ? (tah_settlement){ .kind = TAH_SETTLE_FAULT, .code = TA_ERR_NOMEM, .actor = actor }
                                        : (tah_settlement){ .kind = TAH_SETTLE_DEAD, .reason = TAH_DEAD_NOT_FOUND,
                                                            .actor = actor });
        }
        envelope->waiter = 0;
    }
    tah_envelope_give(sched, head);
    if (result == TA_OK) {
        entry->state = TAH_SCHED_RUNNABLE;
        tah_sched_requeue(sched, &sched->runnable, actor);
        return;
    }
    tah_sched_settle(sched, entry, actor);
}

/*
 * A ready actor whose head waits in the overflow (kernel ADR 0003) waits on
 * the room queue, off the ready queue, until a turn moves its head in.
 */
static void tah_sched_park(tah_sched *sched, tah_sched_entry *entry, ta_actor_id actor) {
    if (entry->parked) {
        return; /* on the room queue already */
    }
    if (tah_ring_push(&sched->room, &actor) != TA_OK) {
        tah_sched_requeue(sched, &sched->ready, actor); /* out of memory: it asks again next turn */
        return;
    }
    entry->parked = 1u;
}

/*
 * While there is room, moves the head of each actor on the room queue into
 * its mailbox, one message each, in the order they asked, and puts the
 * actor on the ready queue. Entries of actors that left (woken, moved on,
 * destroyed) are skipped.
 */
static void tah_sched_unpark(tah_sched *sched) {
    while (tah_ring_count(&sched->room) != 0 && !tah_sched_full(sched, 0)) {
        ta_actor_id actor = 0;
        (void)tah_ring_pop(&sched->room, &actor);
        tah_sched_entry *entry = tah_sched_lookup(sched, actor);
        if (entry == NULL || !entry->parked) {
            continue;
        }
        entry->parked = 0;
        if (entry->state != TAH_SCHED_READY || entry->first == 0) {
            continue;
        }
        tah_envelope *head = tah_sched_envelope(sched, entry->first);
        if (head->overflow) {
            if (tah_sched_full(sched, head->bytes)) {
                tah_sched_park(sched, entry, actor); /* its bytes do not fit yet */
                return;
            }
            tah_sched_move_in(sched, head);
        }
        tah_sched_requeue(sched, &sched->ready, actor);
    }
}

/* Delivers the head envelope of a ready actor (ADR 0005 point 6). */
static void tah_sched_deliver_one(tah_sched *sched, const tah_turn_params *params, ta_actor_id actor) {
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry == NULL || entry->state != TAH_SCHED_READY) {
        return; /* destroyed, replaced or quarantined since it was queued */
    }
    if ((entry->flags & TAH_SCHED_BACKOFF) != 0) {
        entry->flags &= (uint8_t)~TAH_SCHED_BACKOFF;
        tah_sched_requeue(sched, &sched->ready, actor);
        return;
    }
    if (entry->first == 0) {
        entry->state = TAH_SCHED_IDLE;
        return;
    }
    uint32_t head = entry->first;
    tah_envelope *envelope = tah_sched_envelope(sched, head);
    if (envelope->overflow) {
        /* Room goes to the actors that asked first (kernel ADR 0003). */
        if (entry->parked || tah_ring_count(&sched->room) != 0 || tah_sched_full(sched, envelope->bytes)) {
            tah_sched_park(sched, entry, actor);
            return;
        }
        tah_sched_move_in(sched, envelope);
    }
    if (envelope->report != TAH_REPORT_NONE) {
        tah_sched_report_one(sched, params, entry, actor, head);
        return;
    }
    ta_trace_parent trace = envelope->trace != NULL ? *envelope->trace : (ta_trace_parent){0};
    tah_trace_watch_mark(params->watch, params->core, actor, &trace);
    ta_operator_transition jump;
    const ta_operator_transition *transition = NULL;
    if (envelope->has_jump) {
        transition = tah_sched_jump(params, actor, envelope->jump, &jump);
        if (transition == NULL) {
            tah_sched_refuse_jump(sched, entry, actor);
            return;
        }
    }
    tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_DELIVERING, .actor = actor, .envelope = head });
    *params->inside_core = true;
    ta_result result =
        transition == NULL
            ? ta_actor_deliver(params->core, &(ta_actor_deliver_params){ .time = ta_system_time(params->core),
                                                                         .actor = actor,
                                                                         .event = &envelope->event,
                                                                         .trace = trace })
            : ta_actor_transition(params->core, &(ta_actor_transition_params){ .time = ta_system_time(params->core),
                                                                                .actor = actor,
                                                                                .event = &envelope->event,
                                                                                .trace = trace,
                                                                                .transition = transition });
    *params->inside_core = false;
    tah_sched_left_core(params);
    if (result != TA_OK) {
        tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_UNDELIVERED, .actor = actor, .envelope = head });
    }
    switch (result) {
    case TA_OK:
        /* Core retained what it keeps; the envelope's own references go. */
        entry->first = envelope->next;
        if (entry->first == 0) {
            entry->last = 0;
        }
        if (envelope->waiter != 0) {
            /* The send() follows the macrostep this message starts. */
            tah_waiter *waiter = tah_sched_waiter(sched, envelope->waiter);
            if (tah_map_put(&sched->inflight, actor, envelope->waiter) == TA_OK) {
                waiter->state = TAH_WAITER_INFLIGHT;
                waiter->actor = actor;
                waiter->envelope = 0;
            } else {
                tah_sched_settle_waiter(sched, envelope->waiter,
                                        (tah_settlement){ .kind = TAH_SETTLE_FAULT, .code = TA_ERR_NOMEM, .actor = actor });
            }
        }
        tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_DELIVERED, .actor = actor, .envelope = head });
        tah_envelope_give(sched, head);
        sched->delivered += 1u;
        entry->state = TAH_SCHED_RUNNABLE;
        tah_sched_requeue(sched, &sched->runnable, actor);
        return;
    case TA_ERR_NOMEM:
    case TA_ERR_FULL: /* core's pool is at its watermark: the turn's ta_run() makes room */
    case TA_ERR_BUSY: /* an operator transition while another input holds core's running slot: ask again */
        entry->flags |= TAH_SCHED_BACKOFF; /* retryable (ADR 0008): try again next turn */
        tah_sched_requeue(sched, &sched->ready, actor);
        return;
    case TA_ERR_DONE:
        entry->state = TAH_SCHED_IDLE;
        entry->flags |= TAH_SCHED_FINISHED;
        tah_sched_bury(sched, entry, TAH_DEAD_DONE);
        return;
    case TA_ERR_NOT_FOUND:
        tah_sched_remove(sched, actor, TAH_DEAD_NOT_FOUND);
        return;
    default:
        if (result == TA_ERR_INVALID && transition != NULL) {
            /* The actor does not accept the jump now (its source is no
             * longer active): the operator's input is at fault, not the
             * actor (ADR 0026). */
            tah_sched_refuse_jump(sched, entry, actor);
            return;
        }
        /*
         * Any other failure quarantines the actor and keeps its mailbox
         * (ADR 0008). The envelope core refused (for example a malformed
         * event) becomes a dead letter: delivering it again after release()
         * would only fault again.
         */
        entry->first = envelope->next;
        if (entry->first == 0) {
            entry->last = 0;
        }
        tah_sched_dead_append(sched, head, TAH_DEAD_REFUSED);
        tah_sched_fault(sched, params, (tah_turn_record){ .actor = actor, .operation = "deliver", .code = result });
        return;
    }
}

static bool tah_copy_text(ta_arena *arena, ta_str text, ta_str *out) {
    char *copy = ta_arena_alloc(arena, (size_t)text.len + 1u, 1);
    if (copy == NULL) {
        return false;
    }
    tah_mem_copy(copy, text.ptr, text.len);
    copy[text.len] = '\0';
    *out = (ta_str){ .ptr = copy, .len = text.len };
    return true;
}

/* Copies an address's text into the turn arena, so a record outlives the reference. */
static bool tah_copy_address(ta_arena *arena, const ta_address *address, ta_actor_address *out) {
    ta_actor_address view = ta_address_view(address);
    return tah_copy_text(arena, view.family.name, &out->family.name) && tah_copy_text(arena, view.name, &out->name);
}

/* Reports up to `limit` dead letters as records and frees their envelopes. */
static void tah_sched_report_dead(tah_sched *sched, const tah_turn_params *params, uint32_t limit) {
    for (uint32_t reported = 0; reported < limit && sched->dead_first != 0; reported += 1) {
        uint32_t index = sched->dead_first;
        tah_envelope *envelope = tah_sched_envelope(sched, index);
        sched->dead_first = envelope->next;
        if (sched->dead_first == 0) {
            sched->dead_last = 0;
        }
        sched->dead_count -= 1u;
        sched->dead_letters += 1u;
        sched->dead_by_reason[envelope->reason < TAH_DEAD_REASONS ? envelope->reason : 0u] += 1u;
        tah_turn_record record = {
            .kind = TAH_RECORD_DEAD_LETTER,
            .actor = envelope->target,
            .event = envelope->event.name,
            .reason = (tah_dead_reason)envelope->reason,
            /* The data moves to the record; the envelope gives back the rest. */
            .data = envelope->event.data,
            .has_trace = envelope->trace != NULL,
            .trace = envelope->trace != NULL ? *envelope->trace : (ta_trace_parent){0},
        };
        envelope->event.data = (ta_value){0};
        if (!envelope->has_jump && envelope->address != NULL) {
            /* The envelope's reference goes below; the record keeps the text. */
            record.has_address = tah_copy_address(params->arena, envelope->address, &record.address);
        }
        tah_sched_record(sched, params, record);
        if (envelope->waiter != 0) {
            tah_sched_settle_waiter(sched, envelope->waiter,
                                    (tah_settlement){ .kind = TAH_SETTLE_DEAD, .reason = envelope->reason, .actor = envelope->target });
        }
        tah_envelope_give(sched, index);
    }
}

const tah_turn_record *tah_sched_records(const tah_sched *sched) {
    return sched->records;
}

void tah_sched_release_records(tah_sched *sched) {
    for (tah_turn_record *record = sched->records; record != NULL; record = record->next) {
        ta_value_release(&record->data);
        record->data = (ta_value){0};
    }
    sched->records = NULL;
    sched->last_record = NULL;
    sched->record_count = 0;
}

void tah_sched_turn(tah_sched *sched, const tah_turn_params *params) {
    tah_sched_release_records(sched);
    tah_sched_unstrand(sched);
    /* Only actors queued before each phase began: a requeued actor waits for the next turn. */
    uint32_t pending = tah_ring_count(&sched->runnable);
    /* `resumes` bounds the inputs core completes in the turn, not the entries popped: one call of ta_run() serves
     * every actor whose input is next, and the entries it settled are dropped when they come up. */
    sched->turn_left = params->budgets.resumes;
    sched->turn_bounded = true;
    sched->turn_visited = 0;
    for (uint32_t index = 0; index < pending; index += 1) {
        if (sched->turn_left == 0) {
            /* The turn's inputs are used. Entries of actors a call already settled are dropped, which leaves the
             * ring free of duplicates (a ring that kept them would grow by a round of deliveries each turn and
             * allocate chunks); the first entry that still needs a call stays where it is, and so does everything
             * behind it, in order. Each entry is popped once per push, and the walk ends at the first live one,
             * so a turn costs its inputs plus the entries they settled, never the length of the queue. */
            const ta_actor_id *front = tah_ring_front(&sched->runnable);
            tah_sched_entry *waiting = front == NULL ? NULL : tah_sched_lookup(sched, *front);
            if (waiting != NULL && waiting->state == TAH_SCHED_RUNNABLE) {
                break;
            }
        }
        ta_actor_id actor = 0;
        if (!tah_ring_pop(&sched->runnable, &actor)) {
            break;
        }
        sched->turn_visited += 1u;
        if (sched->turn_left == 0) {
            continue; /* the stale entry the check above looked at */
        }
        tah_sched_resume_one(sched, params, actor);
        if (params->stop != NULL && params->stop(params->stop_context)) {
            sched->turn_bounded = false;
            return;
        }
    }
    sched->turn_bounded = false;
    uint32_t limit = 0;
    tah_sched_unpark(sched);
    pending = tah_ring_count(&sched->ready);
    limit = params->budgets.messages < pending ? params->budgets.messages : pending;
    for (uint32_t index = 0; index < limit; index += 1) {
        ta_actor_id actor = 0;
        if (!tah_ring_pop(&sched->ready, &actor)) {
            break;
        }
        tah_sched_deliver_one(sched, params, actor);
        if (params->stop != NULL && params->stop(params->stop_context)) {
            return;
        }
    }
    tah_sched_report_dead(sched, params, params->budgets.messages);
    tah_sched_unpark(sched); /* what the deliveries and reports made room for waits for the next turn */
}

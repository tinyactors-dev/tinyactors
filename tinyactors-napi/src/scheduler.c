#include "scheduler.h"

#include <string.h>

#include "allocator.h"

#define TNA_QUEUE_CHUNK 1024u

/* ------------------------------------------------------------------------
 * The envelope store
 */

static void tna_envelopes_init(tna_envelopes *store, ta_allocator allocator) {
    *store = (tna_envelopes){ .allocator = allocator, .issued = 1u }; /* index 0 means none */
    tna_vec_init(&store->pages, allocator, TNA_LAYOUT(tna_envelope *));
}

tna_envelope *tna_sched_envelope(const tna_sched *sched, uint32_t index) {
    const tna_envelopes *store = &sched->envelopes;
    uint32_t page = index >> TNA_ENVELOPE_PAGE_BITS;
    if (index == 0 || index >= store->issued || page >= store->pages.count) {
        return NULL;
    }
    tna_envelope **entries = tna_vec_at(&store->pages, page);
    return &(*entries)[index & (TNA_ENVELOPE_PAGE_ENTRIES - 1u)];
}

/* A fresh envelope index, or 0 when no memory. */
static uint32_t tna_envelope_take(tna_sched *sched) {
    tna_envelopes *store = &sched->envelopes;
    uint32_t index = store->free_head;
    if (index != 0) {
        tna_envelope *envelope = tna_sched_envelope(sched, index);
        store->free_head = envelope->next;
    } else {
        if ((store->issued >> TNA_ENVELOPE_PAGE_BITS) >= store->pages.count) {
            /* Every issued index is in use and the last page is full: add a page. */
            size_t bytes = TNA_ENVELOPE_PAGE_ENTRIES * sizeof(tna_envelope);
            tna_envelope *page = tna_alloc(store->allocator, bytes, _Alignof(tna_envelope));
            if (page == NULL) {
                return 0;
            }
            if (tna_vec_push(&store->pages, &page) != TA_OK) {
                tna_free(store->allocator,
                         &(ta_allocator_free_params){ .ptr = page, .size = bytes, .alignment = _Alignof(tna_envelope) });
                return 0;
            }
        }
        index = store->issued;
        store->issued += 1u;
    }
    tna_envelope *envelope = tna_sched_envelope(sched, index);
    *envelope = (tna_envelope){ .live = 1u };
    store->live += 1u;
    return index;
}

/* Releases the envelope's references and returns it to the free list. */
static void tna_envelope_give(tna_sched *sched, uint32_t index) {
    tna_envelope *envelope = tna_sched_envelope(sched, index);
    ta_event_release(&envelope->event);
    *envelope = (tna_envelope){ .next = sched->envelopes.free_head };
    sched->envelopes.free_head = index;
    sched->envelopes.live -= 1u;
    sched->queued -= 1u;
}

static void tna_envelopes_destroy(tna_envelopes *store) {
    for (uint32_t page = 0; page < store->pages.count; page += 1) {
        tna_envelope **entries = tna_vec_at(&store->pages, page);
        tna_free(store->allocator, &(ta_allocator_free_params){
                                       .ptr = *entries,
                                       .size = TNA_ENVELOPE_PAGE_ENTRIES * sizeof(tna_envelope),
                                       .alignment = _Alignof(tna_envelope),
                                   });
    }
    tna_vec_destroy(&store->pages);
    store->issued = 1u;
    store->free_head = 0;
    store->live = 0;
}

/* ------------------------------------------------------------------------
 * The table
 */

void tna_sched_init(tna_sched *sched, ta_allocator allocator, ta_allocator envelopes) {
    *sched = (tna_sched){ .allocator = allocator };
    tna_vec_init(&sched->pages, allocator, TNA_LAYOUT(tna_sched_entry *));
    tna_ring_shape shape = { .layout = TNA_LAYOUT(ta_actor_id), .chunk_capacity = TNA_QUEUE_CHUNK };
    tna_ring_init(&sched->runnable, allocator, shape);
    tna_ring_init(&sched->ready, allocator, shape);
    tna_envelopes_init(&sched->envelopes, envelopes);
}

void tna_sched_discard_mail(tna_sched *sched) {
    tna_envelopes *store = &sched->envelopes;
    for (uint32_t index = 1; index < store->issued; index += 1) {
        tna_envelope *envelope = tna_sched_envelope(sched, index);
        if (envelope->live) {
            ta_event_release(&envelope->event);
            envelope->event = (ta_event){0};
        }
    }
}

void tna_sched_destroy(tna_sched *sched) {
    for (uint32_t page = 0; page < sched->pages.count; page += 1) {
        tna_sched_entry **entries = tna_vec_at(&sched->pages, page);
        if (*entries != NULL) {
            tna_free(sched->allocator, &(ta_allocator_free_params){
                                           .ptr = *entries,
                                           .size = TNA_SCHED_PAGE_ENTRIES * sizeof(tna_sched_entry),
                                           .alignment = _Alignof(tna_sched_entry),
                                       });
        }
    }
    tna_vec_destroy(&sched->pages);
    tna_ring_destroy(&sched->runnable);
    tna_ring_destroy(&sched->ready);
    tna_envelopes_destroy(&sched->envelopes);
    sched->known = 0;
    sched->queued = 0;
    sched->dead_first = 0;
    sched->dead_last = 0;
    sched->dead_count = 0;
    sched->records = NULL;
    sched->last_record = NULL;
    sched->record_count = 0;
}

tna_sched_entry *tna_sched_entry_at(const tna_sched *sched, uint32_t slot) {
    uint32_t page = slot >> TNA_SCHED_PAGE_BITS;
    if (page >= sched->pages.count) {
        return NULL;
    }
    tna_sched_entry **entries = tna_vec_at(&sched->pages, page);
    return *entries == NULL ? NULL : &(*entries)[slot & (TNA_SCHED_PAGE_ENTRIES - 1u)];
}

tna_sched_entry *tna_sched_lookup(const tna_sched *sched, ta_actor_id actor) {
    tna_sched_entry *entry = tna_sched_entry_at(sched, ta_actor_id_slot(actor));
    if (entry == NULL || entry->state == TNA_SCHED_NONE || entry->generation != ta_actor_id_generation(actor)) {
        return NULL;
    }
    return entry;
}

/* The entry of `slot`, allocating its page (and the directory up to it). */
static tna_sched_entry *tna_sched_entry_make(tna_sched *sched, uint32_t slot) {
    uint32_t page = slot >> TNA_SCHED_PAGE_BITS;
    if (page >= sched->pages.count) {
        if (tna_vec_reserve(&sched->pages, page + 1u) != TA_OK) {
            return NULL;
        }
        tna_sched_entry *none = NULL;
        while (sched->pages.count <= page) {
            if (tna_vec_push(&sched->pages, &none) != TA_OK) {
                return NULL;
            }
        }
    }
    tna_sched_entry **entries = tna_vec_at(&sched->pages, page);
    if (*entries == NULL) {
        size_t bytes = TNA_SCHED_PAGE_ENTRIES * sizeof(tna_sched_entry);
        *entries = tna_alloc(sched->allocator, bytes, _Alignof(tna_sched_entry));
        if (*entries == NULL) {
            return NULL;
        }
        memset(*entries, 0, bytes);
    }
    return &(*entries)[slot & (TNA_SCHED_PAGE_ENTRIES - 1u)];
}

/* Appends one taken envelope to the dead list. */
static void tna_sched_dead_append(tna_sched *sched, uint32_t index, tna_dead_reason reason) {
    tna_envelope *envelope = tna_sched_envelope(sched, index);
    envelope->reason = (uint8_t)reason;
    envelope->next = 0;
    if (sched->dead_last != 0) {
        tna_sched_envelope(sched, sched->dead_last)->next = index;
    } else {
        sched->dead_first = index;
    }
    sched->dead_last = index;
    sched->dead_count += 1u;
}

/* Moves the entry's whole mailbox to the dead list (ADR 0005 point 10). */
static void tna_sched_bury(tna_sched *sched, tna_sched_entry *entry, tna_dead_reason reason) {
    uint32_t index = entry->first;
    while (index != 0) {
        uint32_t next = tna_sched_envelope(sched, index)->next;
        tna_sched_dead_append(sched, index, reason);
        index = next;
    }
    entry->first = 0;
    entry->last = 0;
}

ta_result tna_sched_add(tna_sched *sched, ta_actor_id actor) {
    tna_sched_entry *entry = tna_sched_entry_make(sched, ta_actor_id_slot(actor));
    if (entry == NULL) {
        return TA_ERR_NOMEM;
    }
    if (tna_ring_push(&sched->runnable, &actor) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    if (entry->state == TNA_SCHED_NONE) {
        sched->known += 1u;
    } else {
        /* A previous actor of this slot the addon never saw go: its mail is dead. */
        tna_sched_bury(sched, entry, TNA_DEAD_NOT_FOUND);
    }
    *entry = (tna_sched_entry){ .generation = ta_actor_id_generation(actor), .state = TNA_SCHED_RUNNABLE };
    return TA_OK;
}

void tna_sched_remove(tna_sched *sched, ta_actor_id actor, tna_dead_reason reason) {
    tna_sched_entry *entry = tna_sched_lookup(sched, actor);
    if (entry == NULL) {
        return;
    }
    tna_sched_bury(sched, entry, reason);
    *entry = (tna_sched_entry){0};
    sched->known -= 1u;
}

ta_result tna_sched_release(tna_sched *sched, ta_actor_id actor) {
    tna_sched_entry *entry = tna_sched_lookup(sched, actor);
    if (entry == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (entry->state != TNA_SCHED_QUARANTINED) {
        return TA_OK;
    }
    if (tna_ring_push(&sched->runnable, &actor) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    entry->state = TNA_SCHED_RUNNABLE;
    entry->flags &= (uint8_t)~TNA_SCHED_BACKOFF;
    return TA_OK;
}

bool tna_sched_quiet(const tna_sched *sched) {
    return tna_ring_count(&sched->runnable) == 0 && tna_ring_count(&sched->ready) == 0 && sched->dead_count == 0;
}

uint32_t tna_sched_mailbox_count(const tna_sched *sched, ta_actor_id actor) {
    const tna_sched_entry *entry = tna_sched_lookup(sched, actor);
    uint32_t count = 0;
    for (uint32_t index = entry == NULL ? 0 : entry->first; index != 0;
         index = tna_sched_envelope(sched, index)->next) {
        count += 1u;
    }
    return count;
}

/* ------------------------------------------------------------------------
 * Posting
 */

ta_result tna_sched_post(tna_sched *sched, ta_actor_id target, const ta_event *event) {
    if (sched->max_queued != 0 && sched->queued >= sched->max_queued) {
        return TA_ERR_BUSY;
    }
    tna_sched_entry *entry = tna_sched_lookup(sched, target);
    bool dead = entry == NULL || (entry->flags & TNA_SCHED_FINISHED) != 0;
    /* An idle actor joins the ready queue; reserve that before taking mail. */
    if (!dead && entry->state == TNA_SCHED_IDLE && tna_ring_push(&sched->ready, &target) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    uint32_t index = tna_envelope_take(sched);
    if (index == 0) {
        /* A ready-queue entry pushed above names an idle actor; delivery skips it. */
        return TA_ERR_NOMEM;
    }
    sched->queued += 1u;
    tna_envelope *envelope = tna_sched_envelope(sched, index);
    envelope->event = *event;
    envelope->target = target;
    if (dead) {
        tna_sched_dead_append(sched, index, entry == NULL ? TNA_DEAD_NOT_FOUND : TNA_DEAD_DONE);
        return TA_OK;
    }
    if (entry->last != 0) {
        tna_sched_envelope(sched, entry->last)->next = index;
    } else {
        entry->first = index;
    }
    entry->last = index;
    if (entry->state == TNA_SCHED_IDLE) {
        entry->state = TNA_SCHED_READY;
    }
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * The turn
 */

static void tna_sched_record(tna_sched *sched, const tna_turn_params *params, tna_turn_record record) {
    tna_turn_record *copy = ta_arena_alloc(params->arena, sizeof *copy, _Alignof(tna_turn_record));
    if (copy == NULL) {
        return; /* the turn arena is out of memory: the hook is not called */
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
static void tna_sched_fault(tna_sched *sched, const tna_turn_params *params, tna_turn_record record) {
    tna_sched_entry *entry = tna_sched_lookup(sched, record.actor);
    if (entry != NULL) {
        entry->state = TNA_SCHED_QUARANTINED;
        entry->flags &= (uint8_t)~TNA_SCHED_BACKOFF;
    }
    record.kind = TNA_RECORD_FAULT;
    tna_sched_record(sched, params, record);
}

/* Puts an actor back at the end of `queue`; a failure is a fault. */
static void tna_sched_requeue(tna_sched *sched, const tna_turn_params *params, tna_ring *queue, ta_actor_id actor) {
    if (tna_ring_push(queue, &actor) != TA_OK) {
        tna_sched_fault(sched, params, (tna_turn_record){ .actor = actor, .operation = "schedule", .code = TA_ERR_NOMEM });
    }
}

/* A stable actor: ready when it has mail, idle otherwise. */
static void tna_sched_settle(tna_sched *sched, const tna_turn_params *params, tna_sched_entry *entry, ta_actor_id actor) {
    if (entry->first == 0) {
        entry->state = TNA_SCHED_IDLE;
        return;
    }
    entry->state = TNA_SCHED_READY;
    tna_sched_requeue(sched, params, &sched->ready, actor);
}

static void tna_sched_resume_one(tna_sched *sched, const tna_turn_params *params, ta_actor_id actor) {
    tna_sched_entry *entry = tna_sched_lookup(sched, actor);
    if (entry == NULL || entry->state != TNA_SCHED_RUNNABLE) {
        return; /* destroyed, replaced or quarantined since it was queued */
    }
    if ((entry->flags & TNA_SCHED_BACKOFF) != 0) {
        entry->flags &= (uint8_t)~TNA_SCHED_BACKOFF;
        tna_sched_requeue(sched, params, &sched->runnable, actor);
        return;
    }
    ta_run_status status = TA_RUN_STABLE;
    ta_actor_resume_params resume = {
        .actor = actor,
        .budget = params->budgets.microsteps,
        .status = &status,
        .action_budget = params->budgets.actions,
    };
    *params->inside_core = true;
    ta_result result = ta_actor_resume(params->core, &resume);
    *params->inside_core = false;
    /* The table is the addon's: core calls never move its pages. */
    if (result == TA_ERR_NOMEM) {
        entry->flags |= TNA_SCHED_BACKOFF;
        tna_sched_requeue(sched, params, &sched->runnable, actor);
        return;
    }
    if (result == TA_ERR_NOT_FOUND) {
        tna_sched_remove(sched, actor, TNA_DEAD_NOT_FOUND);
        return;
    }
    if (result != TA_OK) {
        tna_sched_fault(sched, params, (tna_turn_record){ .actor = actor, .operation = "resume", .code = result });
        return;
    }
    switch (status) {
    case TA_RUN_PAUSED:
        tna_sched_requeue(sched, params, &sched->runnable, actor);
        return;
    case TA_RUN_DONE:
        entry->state = TNA_SCHED_IDLE;
        tna_sched_bury(sched, entry, TNA_DEAD_DONE);
        if ((entry->flags & TNA_SCHED_FINISHED) == 0) {
            entry->flags |= TNA_SCHED_FINISHED;
            tna_sched_record(sched, params, (tna_turn_record){ .kind = TNA_RECORD_DONE, .actor = actor });
        }
        return;
    case TA_RUN_STABLE:
    default:
        tna_sched_settle(sched, params, entry, actor);
        return;
    }
}

/* Delivers the head envelope of a ready actor (ADR 0005 point 6). */
static void tna_sched_deliver_one(tna_sched *sched, const tna_turn_params *params, ta_actor_id actor) {
    tna_sched_entry *entry = tna_sched_lookup(sched, actor);
    if (entry == NULL || entry->state != TNA_SCHED_READY) {
        return; /* destroyed, replaced or quarantined since it was queued */
    }
    if ((entry->flags & TNA_SCHED_BACKOFF) != 0) {
        entry->flags &= (uint8_t)~TNA_SCHED_BACKOFF;
        tna_sched_requeue(sched, params, &sched->ready, actor);
        return;
    }
    if (entry->first == 0) {
        entry->state = TNA_SCHED_IDLE;
        return;
    }
    uint32_t head = entry->first;
    tna_envelope *envelope = tna_sched_envelope(sched, head);
    *params->inside_core = true;
    ta_result result = ta_actor_deliver(params->core, actor, &envelope->event);
    *params->inside_core = false;
    switch (result) {
    case TA_OK:
        /* Core retained what it keeps; the envelope's own references go. */
        entry->first = envelope->next;
        if (entry->first == 0) {
            entry->last = 0;
        }
        tna_envelope_give(sched, head);
        sched->delivered += 1u;
        entry->state = TNA_SCHED_RUNNABLE;
        tna_sched_requeue(sched, params, &sched->runnable, actor);
        return;
    case TA_ERR_NOMEM:
        entry->flags |= TNA_SCHED_BACKOFF; /* retryable (ADR 0008): try again next turn */
        tna_sched_requeue(sched, params, &sched->ready, actor);
        return;
    case TA_ERR_DONE:
        entry->state = TNA_SCHED_IDLE;
        entry->flags |= TNA_SCHED_FINISHED;
        tna_sched_bury(sched, entry, TNA_DEAD_DONE);
        return;
    case TA_ERR_NOT_FOUND:
        tna_sched_remove(sched, actor, TNA_DEAD_NOT_FOUND);
        return;
    default:
        /*
         * TA_ERR_BUSY cannot happen while the table is right: a ready actor is
         * between macrosteps. It is a host bug, so it quarantines the actor,
         * as any other failure does, and keeps its mail.
         */
        tna_sched_fault(sched, params, (tna_turn_record){ .actor = actor, .operation = "deliver", .code = result });
        return;
    }
}

/* Reports up to `limit` dead letters as records and frees their envelopes. */
static void tna_sched_report_dead(tna_sched *sched, const tna_turn_params *params, uint32_t limit) {
    for (uint32_t reported = 0; reported < limit && sched->dead_first != 0; reported += 1) {
        uint32_t index = sched->dead_first;
        tna_envelope *envelope = tna_sched_envelope(sched, index);
        sched->dead_first = envelope->next;
        if (sched->dead_first == 0) {
            sched->dead_last = 0;
        }
        sched->dead_count -= 1u;
        sched->dead_letters += 1u;
        tna_sched_record(sched, params, (tna_turn_record){
                                            .kind = TNA_RECORD_DEAD_LETTER,
                                            .actor = envelope->target,
                                            .event = envelope->event.name,
                                            .reason = (tna_dead_reason)envelope->reason,
                                        });
        tna_envelope_give(sched, index);
    }
}

void tna_sched_turn(tna_sched *sched, const tna_turn_params *params) {
    sched->records = NULL;
    sched->last_record = NULL;
    sched->record_count = 0;
    /* Only actors queued before each phase began: a requeued actor waits for the next turn. */
    uint32_t pending = tna_ring_count(&sched->runnable);
    uint32_t limit = params->budgets.resumes < pending ? params->budgets.resumes : pending;
    for (uint32_t index = 0; index < limit; index += 1) {
        ta_actor_id actor = 0;
        if (!tna_ring_pop(&sched->runnable, &actor)) {
            break;
        }
        tna_sched_resume_one(sched, params, actor);
    }
    pending = tna_ring_count(&sched->ready);
    limit = params->budgets.messages < pending ? params->budgets.messages : pending;
    for (uint32_t index = 0; index < limit; index += 1) {
        ta_actor_id actor = 0;
        if (!tna_ring_pop(&sched->ready, &actor)) {
            break;
        }
        tna_sched_deliver_one(sched, params, actor);
    }
    tna_sched_report_dead(sched, params, params->budgets.messages);
}

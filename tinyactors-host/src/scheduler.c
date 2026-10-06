#include "tinyactors_host/scheduler.h"

#include "memory.h"

#include "tinyactors_host/allocator.h"
#include "allocator_private.h"
#include "scheduler_private.h"

#define TAH_QUEUE_CHUNK 1024u
/*
 * Outcomes one ta_run() reports at most: an input's TAKEN and its end. With
 * room for two, a call takes at most one input, so `resumes` ta_run() calls
 * a turn take at most `resumes` inputs, as many as actors it once resumed.
 * A macrostep its budget pauses gets one call a turn, one slice, and ends
 * the turn's run of core: the next turn continues it.
 */
#define TAH_SCHED_OUTCOMES 2u

/* A fault the kernel found outside a turn, recorded at the next one. */
typedef struct {
    ta_actor_id actor;
    ta_result code;
} tah_pending_fault;

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

/*
 * The name core knows the message in envelope `index` by: its index. Core
 * holds a message only while its envelope lives here, admitted or taken: the
 * kernel withdraws what it gives back first, and core reports nothing it
 * dropped itself (an abandoned or a destroyed session's input).
 */
static uint64_t tah_envelope_correlation(const tah_sched *sched, uint32_t index) {
    (void)sched;
    return index;
}

/* The envelope `correlation` names, or 0 when it is core's own input, or a
 * message the kernel does not hold in core. */
static uint32_t tah_envelope_of(const tah_sched *sched, uint64_t correlation) {
    if (correlation > UINT32_MAX) {
        return 0;
    }
    uint32_t index = (uint32_t)correlation;
    const tah_envelope *envelope = tah_sched_envelope(sched, index);
    if (envelope == NULL || !envelope->live ||
        (envelope->phase != TAH_ENVELOPE_ADMITTED && envelope->phase != TAH_ENVELOPE_TAKEN &&
         envelope->phase != TAH_ENVELOPE_ORPHANED)) {
        return 0;
    }
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
    if (envelope->phase == TAH_ENVELOPE_TAKEN) {
        /* uncounted when core took it */
    } else if (envelope->overflow) {
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
    sched->held_blocked = false; /* room under the limits */
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
    };
    return (uint32_t)reason < TAH_DEAD_REASONS ? names[reason] : "not-found";
}

/* ------------------------------------------------------------------------
 * The table
 */

void tah_sched_init(tah_sched *sched, ta_allocator allocator, ta_allocator envelopes) {
    *sched = (tah_sched){ .allocator = allocator };
    tah_names_init(&sched->names, allocator, TA_PARAM_DEFAULT_EVENT_NAME_DEPTH);
    tah_vec_init(&sched->taken_names, allocator, TAH_LAYOUT(char));
    tah_vec_init(&sched->pages, allocator, TAH_LAYOUT(tah_sched_entry *));
    tah_ring_shape shape = { .layout = TAH_LAYOUT(ta_actor_id), .chunk_capacity = TAH_QUEUE_CHUNK };
    tah_ring_init(&sched->held, allocator, shape);
    tah_ring_init(&sched->backoff, allocator, shape);
    tah_envelopes_init(&sched->envelopes, envelopes);
    tah_free_list_init(&sched->traces, envelopes, TAH_LAYOUT(ta_trace_parent));
    tah_vec_init(&sched->waiters, envelopes, TAH_LAYOUT(tah_waiter));
    tah_vec_init(&sched->faults_pending, allocator, TAH_LAYOUT(tah_pending_fault));
    tah_vec_init(&sched->outcomes_pending, allocator, TAH_LAYOUT(ta_input_outcome));
    tah_map_init(&sched->failed_loads, allocator);
    /* One chunk holds a default turn's settlements (messageBudget and
     * resumesPerTurn are 1024 by default), so steady state reuses it. */
    tah_ring_init(&sched->settled, allocator,
                  (tah_ring_shape){ .layout = TAH_LAYOUT(tah_settlement), .chunk_capacity = TAH_QUEUE_CHUNK });
}

void tah_sched_attach(tah_sched *sched, const tah_sched_attach_params *params) {
    sched->core = params->core;
    sched->inside_core = params->inside_core;
    sched->watch = params->watch;
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
    tah_names_destroy(&sched->names);
    tah_vec_destroy(&sched->taken_names);
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
    tah_map_destroy(&sched->failed_loads); /* emptied while core lived (tah_sched_forget_failed_loads) */
    tah_ring_destroy(&sched->held);
    tah_ring_destroy(&sched->backoff);
    tah_sched_discard_mail(sched); /* gives back the trace parents envelopes hold */
    tah_envelopes_destroy(&sched->envelopes);
    tah_free_list_destroy(&sched->traces);
    tah_vec_destroy(&sched->waiters);
    tah_vec_destroy(&sched->faults_pending);
    tah_vec_destroy(&sched->outcomes_pending);
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
    sched->ready_count = 0;
    sched->runnable_count = 0;
    sched->core_work = false;
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

/* Moves an entry to `state`, keeping the counts of each state. */
static void tah_sched_set_state(tah_sched *sched, tah_sched_entry *entry, tah_sched_state state) {
    switch ((tah_sched_state)entry->state) {
    case TAH_SCHED_READY: sched->ready_count -= 1u; break;
    case TAH_SCHED_RUNNABLE: sched->runnable_count -= 1u; break;
    case TAH_SCHED_QUARANTINED: sched->quarantined -= 1u; break;
    default: break;
    }
    switch (state) {
    case TAH_SCHED_READY: sched->ready_count += 1u; break;
    case TAH_SCHED_RUNNABLE: sched->runnable_count += 1u; break;
    case TAH_SCHED_QUARANTINED: sched->quarantined += 1u; break;
    default: break;
    }
    entry->state = (uint8_t)state;
}

static uint32_t tah_sched_waiting_first(const tah_sched *sched, const tah_sched_entry *entry);

/* An actor between macrosteps: ready with mail in its mailbox, idle without. */
static void tah_sched_between(tah_sched *sched, tah_sched_entry *entry) {
    tah_sched_set_state(sched, entry, tah_sched_waiting_first(sched, entry) != 0 ? TAH_SCHED_READY : TAH_SCHED_IDLE);
}

/* An actor that took mail into its mailbox: an idle one is ready now. */
static void tah_sched_has_mail(tah_sched *sched, tah_sched_entry *entry) {
    if (entry->state == TAH_SCHED_IDLE) {
        tah_sched_set_state(sched, entry, TAH_SCHED_READY);
    }
}

/* Appends one envelope to the dead list; a report is dropped instead. */
static void tah_sched_dead_append(tah_sched *sched, uint32_t index, tah_dead_reason reason) {
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    if (envelope->report != TAH_REPORT_NONE) {
        /* A report about an invocation whose parent is gone tells nobody
         * anything: it is not mail, so it is no dead letter either. */
        tah_envelope_give(sched, index);
        return;
    }
    envelope->phase = TAH_ENVELOPE_DEAD;
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

/* Appends envelope `index` to the entry's mailbox. */
static void tah_sched_mailbox_append(tah_sched *sched, tah_sched_entry *entry, uint32_t index) {
    tah_sched_envelope(sched, index)->next = 0;
    if (entry->last != 0) {
        tah_sched_envelope(sched, entry->last)->next = index;
    } else {
        entry->first = index;
    }
    entry->last = index;
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
            tah_sched_envelope(sched, index)->next = 0;
            return true;
        }
        previous = at;
    }
    return false;
}

/* The held mail of the entry's mailbox becomes dead letters with `reason`;
 * mail core admitted stays, and core refuses it. */
static void tah_sched_bury_held(tah_sched *sched, tah_sched_entry *entry, tah_dead_reason reason) {
    uint32_t index = entry->first;
    while (index != 0) {
        tah_envelope *envelope = tah_sched_envelope(sched, index);
        uint32_t next = envelope->next;
        if (envelope->phase == TAH_ENVELOPE_HELD) {
            (void)tah_sched_unlink(sched, entry, index);
            tah_sched_dead_append(sched, index, reason);
        }
        index = next;
    }
}

/* Puts an actor with held mail on the held queue, once; a quarantined one
 * joins it when it is released. Out of memory, it is stranded, and the next
 * turn tries again. */
static void tah_sched_hold(tah_sched *sched, tah_sched_entry *entry, ta_actor_id actor) {
    if (entry->held || entry->state == TAH_SCHED_QUARANTINED) {
        return;
    }
    entry->held = 1u;
    if (tah_ring_push(&sched->held, &actor) != TA_OK && (entry->flags & TAH_SCHED_STRANDED) == 0) {
        entry->flags |= TAH_SCHED_STRANDED;
        sched->stranded += 1u;
    }
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

/* Settles the waiter of envelope `index`, if it has one, and detaches it. */
static void tah_sched_settle_envelope(tah_sched *sched, uint32_t index, tah_settlement settlement) {
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    if (envelope->waiter != 0) {
        tah_sched_settle_waiter(sched, envelope->waiter, settlement);
        envelope->waiter = 0;
    }
}

/*
 * The input `actor`'s macrostep ran for, at the head of its mailbox, ended
 * without core saying so (it was abandoned): its send() settles and its
 * envelope goes.
 */
static void tah_sched_complete(tah_sched *sched, tah_sched_entry *entry, ta_actor_id actor,
                               tah_settlement settlement) {
    uint32_t index = entry->first;
    if (index == 0 || tah_sched_envelope(sched, index)->phase != TAH_ENVELOPE_TAKEN) {
        return;
    }
    (void)tah_sched_unlink(sched, entry, index);
    settlement.actor = actor;
    tah_sched_settle_envelope(sched, index, settlement);
    tah_envelope_give(sched, index);
}

/* The first message of the entry's mailbox core has not taken, or 0. */
static uint32_t tah_sched_waiting_first(const tah_sched *sched, const tah_sched_entry *entry) {
    uint32_t index = entry->first;
    if (index != 0 && tah_sched_envelope(sched, index)->phase == TAH_ENVELOPE_TAKEN) {
        index = tah_sched_envelope(sched, index)->next;
    }
    return index;
}

/* Withdraws envelope `index` from core's pool; true when core held it untaken. */
static bool tah_sched_withdraw_admitted(tah_sched *sched, uint32_t index) {
    if (sched->core == NULL) {
        return false;
    }
    bool inside = *sched->inside_core;
    *sched->inside_core = true;
    ta_result result = ta_system_withdraw_input(sched->core, tah_envelope_correlation(sched, index));
    *sched->inside_core = inside;
    return result == TA_OK;
}

bool tah_sched_cancel(tah_sched *sched, uint32_t index) {
    tah_waiter *waiter = tah_sched_waiter(sched, index);
    if (waiter == NULL) {
        return false;
    }
    if (waiter->state == TAH_WAITER_QUEUED) {
        uint32_t at = waiter->envelope;
        tah_envelope *envelope = tah_sched_envelope(sched, at);
        tah_sched_entry *entry = tah_sched_lookup(sched, waiter->actor);
        bool withdrawn = false;
        if (envelope != NULL && envelope->phase == TAH_ENVELOPE_HELD) {
            withdrawn = entry != NULL && tah_sched_unlink(sched, entry, at);
        } else if (envelope != NULL && envelope->phase == TAH_ENVELOPE_ADMITTED) {
            withdrawn = tah_sched_withdraw_admitted(sched, at);
            if (withdrawn && entry != NULL) {
                (void)tah_sched_unlink(sched, entry, at);
            }
        }
        if (withdrawn) {
            tah_envelope_give(sched, at);
            if (entry != NULL && entry->state == TAH_SCHED_READY && entry->first == 0) {
                tah_sched_set_state(sched, entry, TAH_SCHED_IDLE);
            }
        } else if (envelope != NULL) {
            envelope->waiter = 0; /* on the dead list: still reported, without the waiter */
        }
        waiter->state = TAH_WAITER_SETTLING;
        return true;
    }
    if (waiter->state == TAH_WAITER_INFLIGHT) {
        tah_envelope *envelope = tah_sched_envelope(sched, waiter->envelope);
        if (envelope != NULL && envelope->waiter == index) {
            envelope->waiter = 0;
        }
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
    if (entry->state == TAH_SCHED_NONE) {
        sched->known += 1u;
    } else {
        /* A previous actor of this slot the addon never saw go: its mail is dead. */
        tah_sched_remove(sched, ta_actor_id_make(ta_actor_id_slot(actor), entry->generation), TAH_DEAD_NOT_FOUND);
        sched->known += 1u;
    }
    *entry = (tah_sched_entry){ .generation = ta_actor_id_generation(actor) };
    /* Core admitted its bootstrap, or the macrostep it was restored in; an
     * actor restored between macrosteps has nothing to run. */
    ta_actor_inspection view;
    bool idle = sched->core != NULL && ta_actor_inspect(sched->core, actor, &view) == TA_OK &&
                !view.macrostep_in_progress;
    tah_sched_set_state(sched, entry, idle ? TAH_SCHED_IDLE : TAH_SCHED_RUNNABLE);
    sched->core_work = sched->core_work || !idle;
    return TA_OK;
}

void tah_sched_remove(tah_sched *sched, ta_actor_id actor, tah_dead_reason reason) {
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry == NULL) {
        return;
    }
    /* Its mail dies now, what core admitted withdrawn from its pool first.
     * Inside a ta_run() core may have taken a message whose outcome the turn
     * has not read yet: that one waits for it (orphaned). The message its
     * macrostep took settles as a dead letter: core dropped it with the
     * session. */
    uint32_t index = entry->first;
    while (index != 0) {
        tah_envelope *envelope = tah_sched_envelope(sched, index);
        uint32_t next = envelope->next;
        if (envelope->phase == TAH_ENVELOPE_TAKEN) {
            tah_sched_settle_envelope(sched, index, (tah_settlement){ .kind = TAH_SETTLE_DEAD, .reason = (uint8_t)reason,
                                                                      .actor = actor });
            tah_envelope_give(sched, index);
        } else if (envelope->phase == TAH_ENVELOPE_ADMITTED && !tah_sched_withdraw_admitted(sched, index)) {
            envelope->phase = TAH_ENVELOPE_ORPHANED;
            envelope->reason = (uint8_t)reason;
            envelope->next = 0;
        } else {
            tah_sched_dead_append(sched, index, reason);
        }
        index = next;
    }
    entry->first = 0;
    entry->last = 0;
    if ((entry->flags & TAH_SCHED_STRANDED) != 0) {
        sched->stranded -= 1u;
    }
    tah_sched_set_state(sched, entry, TAH_SCHED_NONE);
    *entry = (tah_sched_entry){0};
    sched->known -= 1u;
}

/* Withdraws what core admitted from the entry's mailbox and holds it again,
 * so it is admitted anew behind what goes in front of it. */
static void tah_sched_rehold(tah_sched *sched, tah_sched_entry *entry) {
    for (uint32_t index = entry->first; index != 0; index = tah_sched_envelope(sched, index)->next) {
        tah_envelope *envelope = tah_sched_envelope(sched, index);
        if (envelope->phase == TAH_ENVELOPE_ADMITTED && tah_sched_withdraw_admitted(sched, index)) {
            envelope->phase = TAH_ENVELOPE_HELD;
        }
    }
}

/*
 * Moves the list `first`..`last`, in order, to the front of `to`'s mailbox,
 * held, with its send() waiters; `to` joins the held queue, and its own
 * mail, withdrawn from core's pool, is held behind it, so the list is
 * admitted first.
 */
typedef struct {
    uint32_t first;
    uint32_t last;
    ta_actor_id to;
    tah_sched_entry *entry; /* `to`'s */
} tah_splice;

static void tah_sched_splice_front(tah_sched *sched, const tah_splice *splice) {
    tah_sched_entry *to = splice->entry;
    if (splice->first == 0) {
        return;
    }
    tah_sched_rehold(sched, to);
    for (uint32_t index = splice->first; index != 0;) {
        tah_envelope *envelope = tah_sched_envelope(sched, index);
        envelope->target = splice->to;
        envelope->phase = TAH_ENVELOPE_HELD;
        tah_waiter *waiter = tah_sched_waiter(sched, envelope->waiter);
        if (waiter != NULL && waiter->state == TAH_WAITER_QUEUED) {
            waiter->actor = splice->to;
        }
        index = envelope->next;
    }
    /* In front of the waiting mail, behind a message `to` is running. */
    uint32_t taken = to->first != 0 && tah_sched_envelope(sched, to->first)->phase == TAH_ENVELOPE_TAKEN ? to->first : 0;
    uint32_t *front = taken != 0 ? &tah_sched_envelope(sched, taken)->next : &to->first;
    tah_sched_envelope(sched, splice->last)->next = *front;
    if (*front == 0) {
        to->last = splice->last;
    }
    *front = splice->first;
    tah_sched_has_mail(sched, to);
    tah_sched_hold(sched, to, splice->to);
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
    /* What core admitted for `from` and has not taken is withdrawn and held
     * for `to`; what it took already stays `from`'s. */
    uint32_t first = 0;
    uint32_t last = 0;
    uint32_t index = from->first;
    while (index != 0) {
        tah_envelope *envelope = tah_sched_envelope(sched, index);
        uint32_t next = envelope->next;
        if (envelope->phase == TAH_ENVELOPE_HELD ||
            (envelope->phase == TAH_ENVELOPE_ADMITTED && tah_sched_withdraw_admitted(sched, index))) {
            (void)tah_sched_unlink(sched, from, index);
            if (last != 0) {
                tah_sched_envelope(sched, last)->next = index;
            } else {
                first = index;
            }
            last = index;
        }
        index = next;
    }
    tah_sched_splice_front(sched, &(tah_splice){ .first = first, .last = last, .to = params->to, .entry = to });
    if (from->state == TAH_SCHED_READY && from->first == 0) {
        tah_sched_set_state(sched, from, TAH_SCHED_IDLE);
    }
    return TA_OK;
}

ta_result tah_sched_wake(tah_sched *sched, ta_actor_id actor) {
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    sched->core_work = true;
    if (entry->state == TAH_SCHED_IDLE || entry->state == TAH_SCHED_READY) {
        tah_sched_set_state(sched, entry, TAH_SCHED_RUNNABLE);
    }
    return TA_OK;
}

void tah_sched_core_work(tah_sched *sched) {
    sched->core_work = true;
}

ta_result tah_sched_release(tah_sched *sched, ta_actor_id actor) {
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (entry->state != TAH_SCHED_QUARANTINED) {
        return TA_OK;
    }
    bool inside = *sched->inside_core;
    *sched->inside_core = true;
    ta_result result = ta_actor_release(sched->core, actor);
    *sched->inside_core = inside;
    if (result == TA_OK) {
        /* Core goes on with the macrostep a fault stopped. */
        tah_sched_set_state(sched, entry, TAH_SCHED_RUNNABLE);
        sched->core_work = true;
    } else {
        /* Core did not stop it: the kernel quarantined it, or its macrostep
         * was abandoned since. */
        tah_sched_between(sched, entry);
    }
    entry->flags &= (uint8_t)~TAH_SCHED_BACKOFF;
    /* Mail it collected while quarantined is admitted at the next turn,
     * though overflow waits for room: the limits do not hold it back. */
    if (tah_sched_waiting_first(sched, entry) != 0) {
        tah_sched_hold(sched, entry, actor);
        sched->held_blocked = false;
    }
    return TA_OK;
}

void tah_sched_ran(tah_sched *sched, ta_actor_id actor, bool finished) {
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry == NULL || entry->state == TAH_SCHED_QUARANTINED) {
        return;
    }
    if (finished) {
        entry->flags |= TAH_SCHED_FINISHED;
    }
    tah_sched_between(sched, entry);
}

ta_result tah_sched_abandoned(tah_sched *sched, ta_actor_id actor) {
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    if (entry == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    tah_sched_complete(sched, entry, actor, (tah_settlement){ .kind = TAH_SETTLE_ABANDONED });
    /* Core returned the mail it deferred for it to its run queue. */
    sched->core_work = true;
    if (entry->state == TAH_SCHED_QUARANTINED) {
        return TA_OK; /* release() lets it take its mail again */
    }
    entry->flags &= (uint8_t)~TAH_SCHED_BACKOFF;
    tah_sched_between(sched, entry);
    return TA_OK;
}

/* The limits mail between actors must fit before it is admitted (ADR 0005 point 9). */
static bool tah_sched_full(const tah_sched *sched, uint32_t bytes) {
    return (sched->max_queued != 0 && sched->queued >= sched->max_queued) ||
           (sched->max_retained != 0 && sched->retained + bytes > sched->max_retained);
}

bool tah_sched_quiet(const tah_sched *sched) {
    return !sched->core_work && sched->dead_count == 0 && tah_ring_count(&sched->settled) == 0 &&
           sched->stranded == 0 && tah_ring_count(&sched->backoff) == 0 && sched->faults_pending.count == 0 &&
           sched->outcomes_pending.count == 0 &&
           (tah_ring_count(&sched->held) == 0 || sched->held_blocked);
}

uint32_t tah_sched_mailbox_count(const tah_sched *sched, ta_actor_id actor) {
    const tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    uint32_t count = 0;
    for (uint32_t index = entry == NULL ? 0 : tah_sched_waiting_first(sched, entry); index != 0;
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
    return entry == NULL ? 0 : tah_sched_waiting_first(sched, entry);
}

tah_queued_message tah_sched_queued_message(const tah_sched *sched, uint32_t envelope) {
    const tah_envelope *message = tah_sched_envelope(sched, envelope);
    return (tah_queued_message){ .event = &message->event,
                                 .jump = message->has_jump ? message->jump : NULL,
                                 .waiter = message->waiter,
                                 .next = message->next,
                                 .overflow = message->overflow != 0,
                                 .held = message->phase == TAH_ENVELOPE_HELD };
}

void tah_sched_read_stats(const tah_sched *sched, tah_sched_stats *out) {
    *out = (tah_sched_stats){ .runnable = sched->runnable_count,
                              .ready = sched->ready_count,
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
 * Admission
 */

/* The core reason token of a report envelope (core ADR 0081). */
static ta_str tah_report_reason(uint8_t kind) {
    switch (kind) {
    case TAH_REPORT_QUARANTINED: return (ta_str){ .ptr = "quarantined", .len = 11 };
    case TAH_REPORT_FAILED: return (ta_str){ .ptr = "failed", .len = 6 };
    default: return (ta_str){ .ptr = "unavailable", .len = 11 };
    }
}

/* Offers envelope `index` to core's pool: ta_actor_deliver(), or for a
 * report ta_actor_report_invoke(), under the envelope's correlation. */
static ta_result tah_sched_offer(tah_sched *sched, uint32_t index) {
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    ta_trace_parent trace = envelope->trace != NULL ? *envelope->trace : (ta_trace_parent){0};
    uint64_t correlation = tah_envelope_correlation(sched, index);
    ta_time now = ta_system_time(sched->core);
    bool inside = *sched->inside_core;
    *sched->inside_core = true;
    ta_result result = TA_OK;
    if (envelope->report != TAH_REPORT_NONE) {
        ta_invoke_error error = {
            .reason = tah_report_reason(envelope->report),
            .data = envelope->event.data.v != NULL ? &envelope->event.data : NULL,
            .ends = envelope->report != TAH_REPORT_QUARANTINED,
        };
        result = ta_actor_report_invoke(sched->core, &(ta_invoke_report_params){ .time = now,
                                                                                 .actor = envelope->target,
                                                                                 .invoke_id = envelope->event.invoke_id,
                                                                                 .kind = TA_INVOKE_REPORT_ERROR,
                                                                                 .trace = trace,
                                                                                 .correlation = correlation,
                                                                                 .as.error = &error });
    } else {
        tah_trace_watch_mark(sched->watch, sched->core, envelope->target, &trace);
        /* A message posted by address names no actor: core resolves it
         * (core ADR 0099). */
        ta_actor_address view = { 0 };
        bool by_address = envelope->target == 0 && !envelope->has_jump && envelope->address != NULL;
        if (by_address) {
            view = ta_address_view(envelope->address);
        }
        ta_operator_transition jump = { 0 };
        if (envelope->has_jump) {
            jump = (ta_operator_transition){ .source = envelope->jump->source,
                                             .targets = envelope->jump->targets,
                                             .target_count = envelope->jump->count };
        }
        result = ta_actor_deliver(sched->core, &(ta_actor_deliver_params){ .time = now,
                                                                           .actor = envelope->target,
                                                                           .address = by_address ? &view : NULL,
                                                                           .name = tah_names_text(&sched->names,
                                                                                                  envelope->event.name),
                                                                           .event = &envelope->event,
                                                                           .trace = trace,
                                                                           .transition = envelope->has_jump ? &jump
                                                                                                            : NULL,
                                                                           .correlation = correlation });
    }
    *sched->inside_core = inside;
    return result;
}

/* Notes a fault the kernel found outside a turn, for the next turn's records. */
static void tah_sched_fault_later(tah_sched *sched, ta_actor_id actor, ta_result code) {
    tah_pending_fault fault = { .actor = actor, .code = code };
    (void)tah_vec_push(&sched->faults_pending, &fault); /* out of memory: no record, the quarantine stands */
}

/*
 * Admits envelope `index`, in its target's mailbox, into core's pool.
 * TA_OK when core admitted it, or it became a dead letter (the target
 * gone, finished, or refusing it); TA_ERR_FULL or TA_ERR_NOMEM when core
 * had no room, and the envelope is as it was.
 */
static ta_result tah_sched_admit(tah_sched *sched, tah_sched_entry *entry, uint32_t index) {
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    ta_actor_id actor = envelope->target;
    ta_result result = tah_sched_offer(sched, index);
    if (result == TA_ERR_FULL || result == TA_ERR_NOMEM) {
        return result;
    }
    if (result == TA_OK) {
        envelope->phase = TAH_ENVELOPE_ADMITTED;
        sched->core_work = true;
        return TA_OK;
    }
    (void)tah_sched_unlink(sched, entry, index);
    bool jump = envelope->has_jump;
    tah_dead_reason reason = result == TA_ERR_DONE ? TAH_DEAD_DONE
                             : result == TA_ERR_NOT_FOUND ? TAH_DEAD_NOT_FOUND
                                                          : TAH_DEAD_REFUSED;
    tah_sched_dead_append(sched, index, reason);
    if (result == TA_ERR_DONE) {
        entry->flags |= TAH_SCHED_FINISHED;
        tah_sched_bury_held(sched, entry, TAH_DEAD_DONE);
    } else if (result != TA_ERR_NOT_FOUND && !jump && entry->state != TAH_SCHED_QUARANTINED) {
        /* Core refused the message itself (a malformed event): the actor is
         * quarantined with its mailbox, as ADR 0008 says, and the fault is
         * recorded at the next turn. A jump it refuses is the operator's
         * fault, not the actor's (ADR 0026). */
        uint32_t code = (uint32_t)(-(int64_t)result);
        sched->faults[code < TAH_FAULT_CODES ? code : 0u] += 1u;
        tah_sched_set_state(sched, entry, TAH_SCHED_QUARANTINED);
        tah_sched_fault_later(sched, actor, result);
    }
    if (entry->state == TAH_SCHED_READY && entry->first == 0) {
        tah_sched_set_state(sched, entry, TAH_SCHED_IDLE);
    }
    return TA_OK;
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
    /* The kernel holds the name as text; tah_sched_post adopted it. */
    envelope->event.name = message->name;
    envelope->target = message->target;
    envelope->bytes = message->bytes;
    envelope->waiter = message->waiter;
    envelope->report = message->report;
    envelope->external = !message->may_overflow && message->report == TAH_REPORT_NONE;
    sched->queued += 1u;
    sched->retained += message->bytes;
    tah_waiter *waiter = tah_sched_waiter(sched, message->waiter);
    if (waiter != NULL) {
        waiter->state = TAH_WAITER_QUEUED;
        waiter->envelope = index;
        waiter->actor = message->target;
    }
}

/* Counts a filled envelope as overflow instead of queued (kernel ADR 0003). */
static void tah_sched_mark_overflow(tah_sched *sched, uint32_t index) {
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    envelope->overflow = 1u;
    sched->queued -= 1u;
    sched->retained -= envelope->bytes;
    sched->overflow += 1u;
}

/* Moves an overflowed envelope in: it counts against the limits from now on. */
static void tah_sched_move_in(tah_sched *sched, tah_envelope *envelope) {
    envelope->overflow = 0;
    sched->overflow -= 1u;
    sched->queued += 1u;
    sched->retained += envelope->bytes;
}

/* Whether the operator transition is for the definition `actor` runs. */
static bool tah_sched_jump_applies(tah_sched *sched, ta_actor_id actor, const tah_message *message) {
    if (message->jump == NULL) {
        return true;
    }
    ta_actor_inspection view;
    bool inside = *sched->inside_core;
    *sched->inside_core = true;
    ta_result result = ta_actor_inspect(sched->core, actor, &view);
    *sched->inside_core = inside;
    return result == TA_OK && view.definition_id == message->jump->definition;
}

/*
 * Posts a message by address (core ADR 0099): core resolves the address,
 * admitting it for the actor resident there, or keeping it until the
 * address's load is answered. It is in no actor's mailbox; core's outcomes
 * settle it. On a refusal at the post the caller keeps the event and the
 * waiter, as for the host's own message to an actor.
 */
static ta_result tah_sched_post_address(tah_sched *sched, const tah_message *message) {
    ta_trace_parent *trace = NULL;
    if (!tah_sched_take_trace(sched, message, &trace)) {
        return TA_ERR_NOMEM;
    }
    uint32_t index = tah_envelope_take(sched);
    if (index == 0) {
        if (trace != NULL) {
            tah_free_list_give(&sched->traces, trace);
        }
        return TA_ERR_NOMEM;
    }
    tah_message posted = *message;
    posted.target = 0;
    tah_sched_fill(sched, index, &posted, trace);
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    ta_address_retain(message->address);
    envelope->address = message->address; /* a dead letter names it */
    ta_result result = tah_sched_offer(sched, index);
    if (result == TA_OK) {
        envelope->phase = TAH_ENVELOPE_ADMITTED;
        sched->core_work = true;
        return TA_OK;
    }
    envelope->event = (ta_event){ 0 };
    tah_waiter *waiter = tah_sched_waiter(sched, message->waiter);
    if (waiter != NULL) {
        waiter->state = TAH_WAITER_SETTLING;
        waiter->envelope = 0;
        waiter->actor = 0;
    }
    tah_envelope_give(sched, index);
    return result;
}

ta_result tah_sched_post(tah_sched *sched, const tah_message *posted) {
    /* The kernel holds the name as text: a core name is formatted now,
     * while it is valid, and kept in the kernel's table (events.h). */
    tah_message adopted = *posted;
    adopted.name = posted->event->name;
    ta_result named = tah_names_adopt(&sched->names, sched->core, &adopted.name);
    if (named != TA_OK) {
        return named;
    }
    const tah_message *message = &adopted;
    if (message->address != NULL) {
        return tah_sched_post_address(sched, message);
    }
    ta_actor_id target = message->target;
    tah_sched_entry *entry = tah_sched_lookup(sched, target);
    bool dead = entry == NULL || (entry->flags & TAH_SCHED_FINISHED) != 0;
    bool refused = !dead && !tah_sched_jump_applies(sched, target, message);
    bool overflow = message->may_overflow && tah_sched_full(sched, message->bytes);
    ta_trace_parent *trace = NULL;
    if (!tah_sched_take_trace(sched, message, &trace)) {
        return TA_ERR_NOMEM;
    }
    tah_jump *jump = NULL;
    uint32_t index = tah_sched_take_jump(sched, message, &jump) ? tah_envelope_take(sched) : 0;
    if (index == 0) {
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
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    if (overflow) {
        tah_sched_mark_overflow(sched, index);
    }
    if (jump != NULL) {
        envelope->jump = jump;
        envelope->has_jump = 1u;
    }
    if (dead || refused) {
        tah_sched_dead_append(sched, index, dead ? (entry == NULL ? TAH_DEAD_NOT_FOUND : TAH_DEAD_DONE)
                                                 : TAH_DEAD_REFUSED);
        return TA_OK;
    }
    tah_sched_mailbox_append(sched, entry, index);
    tah_sched_has_mail(sched, entry);
    /* A quarantined actor collects mail until it is released (ADR 0008),
     * and mail between actors queues behind the target's held mail. */
    bool hold = overflow || entry->state == TAH_SCHED_QUARANTINED || (!envelope->external && entry->held);
    ta_result result = hold ? TA_ERR_FULL : tah_sched_admit(sched, entry, index);
    if (result == TA_OK) {
        return TA_OK;
    }
    if (envelope->external && !hold) {
        /* The host's own message: core's full pool refuses it at the post.
         * The caller keeps the event and the waiter: the envelope gives back
         * only what the scheduler took for it (its trace, its transition). */
        (void)tah_sched_unlink(sched, entry, index);
        envelope->event = (ta_event){ 0 };
        tah_waiter *waiter = tah_sched_waiter(sched, message->waiter);
        if (waiter != NULL) {
            waiter->state = TAH_WAITER_SETTLING;
            waiter->envelope = 0;
            waiter->actor = 0;
        }
        tah_envelope_give(sched, index);
        if (entry->state == TAH_SCHED_READY && entry->first == 0) {
            tah_sched_set_state(sched, entry, TAH_SCHED_IDLE);
        }
        return result;
    }
    envelope->phase = TAH_ENVELOPE_HELD;
    if (!hold) {
        sched->held_blocked = true;
    }
    tah_sched_hold(sched, entry, target);
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * The turn
 */

static void tah_sched_record(tah_sched *sched, const tah_turn_params *params, tah_turn_record record) {
    tah_turn_record *copy =
        params == NULL ? NULL : ta_arena_alloc(params->arena, sizeof *copy, _Alignof(tah_turn_record));
    if (copy == NULL) {
        /* The turn arena is out of memory, or there is no turn: the hook is
         * not called, and what the record owned goes back now. */
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
        tah_sched_set_state(sched, entry, TAH_SCHED_QUARANTINED);
        entry->flags &= (uint8_t)~TAH_SCHED_BACKOFF;
    }
    if (params == NULL) {
        tah_sched_fault_later(sched, record.actor, record.code); /* no turn: recorded at the next one */
        return;
    }
    record.kind = TAH_RECORD_FAULT;
    tah_sched_record(sched, params, record);
}

/* The faults found outside the turn become its first records. */
static void tah_sched_record_pending_faults(tah_sched *sched, const tah_turn_params *params) {
    for (uint32_t at = 0; at < sched->faults_pending.count; at += 1u) {
        const tah_pending_fault *fault = tah_vec_at(&sched->faults_pending, at);
        tah_sched_record(sched, params, (tah_turn_record){ .kind = TAH_RECORD_FAULT, .actor = fault->actor,
                                                           .operation = "deliver", .code = fault->code });
    }
    tah_vec_clear(&sched->faults_pending);
}

/*
 * Puts stranded actors back on the held queue, and recounts the ones still
 * stranded. A walk of the table, only after a push ran out of memory.
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
            ta_actor_id actor = ta_actor_id_make((page << TAH_SCHED_PAGE_BITS) | at, entry->generation);
            if (left != 0 || tah_ring_push(&sched->held, &actor) != TA_OK) {
                left += 1u; /* still no memory: the next turn tries again */
                continue;
            }
            entry->flags &= (uint8_t)~TAH_SCHED_STRANDED;
        }
    }
    sched->stranded = left;
}

/* What admitting an actor's held mail ran into. */
typedef struct {
    bool pool;   /* core's pool had no room: nothing more is admitted this turn */
    bool limits; /* overflow found no room under the limits */
} tah_held_blocked;

/*
 * Admits one actor's held mail, in order, while there is room: core's pool
 * below its watermark, and, for overflow, room under the limits, which
 * `blocked->limits` says an earlier actor's overflow already found none of.
 * Stops at the first message without room, and says which room was
 * missing; the actor's held mail from there on waits.
 */
static void tah_sched_admit_held(tah_sched *sched, tah_sched_entry *entry, tah_held_blocked *blocked) {
    uint32_t index = entry->first;
    while (index != 0) {
        tah_envelope *envelope = tah_sched_envelope(sched, index);
        uint32_t next = envelope->next;
        if (envelope->phase == TAH_ENVELOPE_HELD) {
            if (envelope->overflow) {
                if (blocked->limits || tah_sched_full(sched, envelope->bytes)) {
                    blocked->limits = true;
                    return;
                }
                tah_sched_move_in(sched, envelope);
            }
            ta_result result = tah_sched_admit(sched, entry, index);
            if (result != TA_OK) {
                blocked->pool = true;
                return;
            }
        }
        index = next;
    }
}

/*
 * Admits held mail, the actors in the order they first held it (kernel ADR
 * 0003), up to `limit` of them. When core's pool has no room the rest keep
 * their order for the next turn. When the limits have none, overflow waits
 * in the same order, but the mail that needs no room under them (the
 * application's, held while its actor was quarantined or the pool was
 * full) is still admitted, up to each actor's first overflow: it may be
 * what makes the room.
 */
static void tah_sched_admit_holders(tah_sched *sched, uint32_t limit) {
    uint32_t pending = tah_ring_count(&sched->held);
    tah_held_blocked blocked = { .pool = false, .limits = false };
    for (uint32_t at = 0; at < pending; at += 1u) {
        ta_actor_id actor = 0;
        (void)tah_ring_pop(&sched->held, &actor);
        tah_sched_entry *entry = tah_sched_lookup(sched, actor);
        if (entry == NULL || !entry->held) {
            continue; /* gone, or a stale entry of the queue */
        }
        if (entry->state == TAH_SCHED_QUARANTINED) {
            entry->held = 0; /* it collects mail until release() queues it again */
            continue;
        }
        if (!blocked.pool && at < limit) {
            tah_sched_admit_held(sched, entry, &blocked);
        }
        bool still = false;
        for (uint32_t index = entry->first; index != 0; index = tah_sched_envelope(sched, index)->next) {
            still = still || tah_sched_envelope(sched, index)->phase == TAH_ENVELOPE_HELD;
        }
        if (!still) {
            entry->held = 0;
            continue;
        }
        if (tah_ring_push(&sched->held, &actor) != TA_OK && (entry->flags & TAH_SCHED_STRANDED) == 0) {
            entry->flags |= TAH_SCHED_STRANDED;
            sched->stranded += 1u;
        }
    }
    sched->held_blocked = blocked.pool || blocked.limits;
}

/* The actors whose input ran out of memory last turn go on (ADR 0008's backoff). */
static void tah_sched_release_backoff(tah_sched *sched) {
    uint32_t pending = tah_ring_count(&sched->backoff);
    for (uint32_t at = 0; at < pending; at += 1u) {
        ta_actor_id actor = 0;
        (void)tah_ring_pop(&sched->backoff, &actor);
        tah_sched_entry *entry = tah_sched_lookup(sched, actor);
        if (entry == NULL || (entry->flags & TAH_SCHED_BACKOFF) == 0) {
            continue;
        }
        entry->flags &= (uint8_t)~TAH_SCHED_BACKOFF;
        bool inside = *sched->inside_core;
        *sched->inside_core = true;
        (void)ta_actor_release(sched->core, actor);
        *sched->inside_core = inside;
        sched->core_work = true;
    }
}

/*
 * Core took the message in `index` for `actor`: its macrostep runs it now.
 * It leaves the waiting mail, uncounted, and stays at the head of the
 * mailbox until it ends, where an abandon or a removal finds it.
 */
static void tah_sched_taken(tah_sched *sched, tah_sched_entry *entry, ta_actor_id actor, uint32_t index) {
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    if (entry != NULL) {
        /* A post by address was in no mailbox until now: it joins this one,
         * so a destroy or an abandon of its actor settles it. */
        (void)tah_sched_unlink(sched, entry, index);
        envelope->next = entry->first;
        entry->first = index;
        if (entry->last == 0) {
            entry->last = index;
        }
    }
    if (!envelope->overflow) {
        sched->queued -= 1u;
        sched->retained -= envelope->bytes;
    }
    envelope->phase = TAH_ENVELOPE_TAKEN;
    if (envelope->report == TAH_REPORT_NONE) {
        sched->delivered += 1u;
    }
    tah_waiter *waiter = tah_sched_waiter(sched, envelope->waiter);
    if (waiter != NULL) {
        /* The send() follows the macrostep this message starts. */
        waiter->state = TAH_WAITER_INFLIGHT;
        waiter->actor = actor;
    }
    tah_sched_note_to(sched, (tah_sched_note){
                                 .kind = envelope->report != TAH_REPORT_NONE ? TAH_NOTE_REPORTED : TAH_NOTE_DELIVERED,
                                 .actor = actor,
                                 .envelope = index });
}

/* The input in `index` ended (or 0 for core's own): its send() settles and its envelope goes. */
static void tah_sched_ended(tah_sched *sched, tah_sched_entry *entry, ta_actor_id actor, uint32_t index,
                            tah_settlement settlement) {
    if (index != 0 && entry != NULL) {
        (void)tah_sched_unlink(sched, entry, index);
    }
    if (index != 0) {
        settlement.actor = actor;
        tah_sched_settle_envelope(sched, index, settlement);
        tah_envelope_give(sched, index);
    }
}

void tah_sched_note_failed_load(tah_sched *sched, const ta_address *address, tah_dead_reason reason) {
    /* The map holds the address, so the key names this address until the
     * entry goes, never another one that reuses its memory. */
    uint64_t key = (uint64_t)(uintptr_t)address;
    uint64_t before = 0;
    bool had = tah_map_count(&sched->failed_loads) != 0 && tah_map_get(&sched->failed_loads, key, &before);
    if (tah_map_put(&sched->failed_loads, key, (uint64_t)reason) == TA_OK && !had) {
        ta_address_retain((ta_address *)(uintptr_t)address);
    }
}

void tah_sched_forget_failed_loads(tah_sched *sched) {
    uint32_t cursor = 0;
    uint64_t key = 0;
    uint64_t value = 0;
    while (tah_map_next(&sched->failed_loads, &cursor, &key, &value)) {
        ta_address_release((ta_address *)(uintptr_t)key);
    }
    tah_map_destroy(&sched->failed_loads);
    tah_map_init(&sched->failed_loads, sched->allocator);
}

void tah_sched_forget_failed_load(tah_sched *sched, const ta_address *address) {
    if (tah_map_count(&sched->failed_loads) != 0 && tah_map_remove(&sched->failed_loads, (uint64_t)(uintptr_t)address)) {
        ta_address_release((ta_address *)(uintptr_t)address);
    }
}

/* A message core refused: a dead letter. A message posted by address whose
 * load failed carries the failure's code (system.c), and its reason when
 * the loader gave one (tah_sched_note_failed_load). */
static void tah_sched_refused(tah_sched *sched, tah_sched_entry *entry, uint32_t index, ta_result code) {
    tah_dead_reason reason = code == TA_ERR_DONE       ? TAH_DEAD_DONE
                             : code == TA_ERR_INVALID  ? TAH_DEAD_REFUSED
                             : code == TA_ERR_UNLOADED ? TAH_DEAD_UNLOADED
                                                       : TAH_DEAD_NOT_FOUND;
    const tah_envelope *posted = tah_sched_envelope(sched, index);
    uint64_t failed = 0;
    if (posted != NULL && posted->target == 0 && !posted->has_jump && posted->address != NULL &&
        reason == TAH_DEAD_NOT_FOUND && tah_map_count(&sched->failed_loads) != 0 &&
        tah_map_get(&sched->failed_loads, (uint64_t)(uintptr_t)posted->address, &failed)) {
        reason = (tah_dead_reason)failed;
    }
    if (entry != NULL) {
        (void)tah_sched_unlink(sched, entry, index);
        if (code == TA_ERR_DONE) {
            entry->flags |= TAH_SCHED_FINISHED;
        }
    }
    tah_sched_dead_append(sched, index, reason);
}

/* Acts on one outcome ta_run() reported (core ADR 0103). */
static void tah_sched_outcome(tah_sched *sched, const tah_turn_params *params, const ta_input_outcome *outcome) {
    ta_actor_id actor = outcome->actor;
    tah_sched_entry *entry = tah_sched_lookup(sched, actor);
    uint32_t index = outcome->correlation == 0 ? 0 : tah_envelope_of(sched, outcome->correlation);
    tah_envelope *orphan = index == 0 ? NULL : tah_sched_envelope(sched, index);
    if (orphan != NULL && orphan->phase == TAH_ENVELOPE_ORPHANED) {
        /* Its actor was removed while core ran it, or before core's refusal
         * was read: it is a dead letter of that removal. */
        tah_sched_dead_append(sched, index, (tah_dead_reason)orphan->reason);
        return;
    }
    switch (outcome->kind) {
    case TA_INPUT_TAKEN:
        if (index != 0) {
            tah_sched_taken(sched, entry, actor, index);
        } else if (outcome->has_event) {
            /* Mail between actors, which core queued itself (core ADR 0099):
             * its name goes to the observer as text, and only an observer
             * that keeps the event interns it (core ADR 0100). */
            ta_event event = outcome->event;
            event.name = 0u;
            tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_DELIVERED, .actor = actor,
                                                       .event = &event, .name = outcome->name });
        }
        if (entry != NULL && entry->state != TAH_SCHED_QUARANTINED) {
            tah_sched_set_state(sched, entry, TAH_SCHED_RUNNABLE);
        }
        return;
    case TA_INPUT_COMPLETED:
    case TA_INPUT_IGNORED: {
        sched->held_blocked = false;
        tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_ENDED, .actor = actor });
        tah_sched_ended(sched, entry, actor, index,
                        (tah_settlement){ .kind = TAH_SETTLE_STABLE, .ignored = outcome->kind == TA_INPUT_IGNORED });
        if (entry != NULL) {
            tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_SETTLED, .actor = actor });
            if (entry->state != TAH_SCHED_QUARANTINED) {
                tah_sched_between(sched, entry);
            }
        }
        return;
    }
    case TA_INPUT_FINISHED:
        sched->held_blocked = false;
        tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_ENDED, .actor = actor });
        tah_sched_ended(sched, entry, actor, index, (tah_settlement){ .kind = TAH_SETTLE_DONE });
        if (entry != NULL) {
            tah_sched_bury_held(sched, entry, TAH_DEAD_DONE);
            if (entry->state != TAH_SCHED_QUARANTINED) {
                tah_sched_between(sched, entry);
            }
            if ((entry->flags & TAH_SCHED_FINISHED) == 0) {
                entry->flags |= TAH_SCHED_FINISHED;
                tah_sched_record(sched, params, (tah_turn_record){ .kind = TAH_RECORD_DONE, .actor = actor });
                tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_FINISHED, .actor = actor });
            }
        }
        return;
    case TA_INPUT_FAULTED:
        if (outcome->code == TA_ERR_NOMEM) {
            /* Retryable (ADR 0008): released at the next turn; its send()
             * follows the macrostep until it ends. */
            if (entry != NULL && (entry->flags & TAH_SCHED_BACKOFF) == 0) {
                entry->flags |= TAH_SCHED_BACKOFF;
                if (tah_ring_push(&sched->backoff, &actor) != TA_OK) {
                    entry->flags &= (uint8_t)~TAH_SCHED_BACKOFF;
                    tah_sched_fault(sched, params, (tah_turn_record){ .actor = actor, .operation = "resume",
                                                                      .code = outcome->code });
                }
            }
            return;
        }
        tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_ENDED, .actor = actor });
        if (index != 0) {
            tah_sched_settle_envelope(sched, index,
                                      (tah_settlement){ .kind = TAH_SETTLE_FAULT, .code = outcome->code, .actor = actor });
        }
        tah_sched_fault(sched, params, (tah_turn_record){ .actor = actor, .operation = "resume", .code = outcome->code });
        return;
    case TA_INPUT_REFUSED:
        sched->held_blocked = false;
        if (index != 0) {
            tah_sched_refused(sched, entry, index, outcome->code);
        }
        if (entry != NULL && entry->state == TAH_SCHED_READY && entry->first == 0) {
            tah_sched_set_state(sched, entry, TAH_SCHED_IDLE);
        }
        return;
    default:
        return;
    }
}

void tah_sched_outcome_outside(tah_sched *sched, const ta_input_outcome *outcome) {
    if (outcome->correlation == 0) {
        return; /* core's own input, or the caller's */
    }
    if (tah_vec_push(&sched->outcomes_pending, outcome) != TA_OK) {
        /* No memory to keep it for the turn: acted on now, so its send()
         * settles; a record it would write is lost, as when the turn arena
         * runs out. */
        tah_sched_outcome(sched, NULL, outcome);
    }
}

/* The outcomes reported outside the turn, acted on first, in order. */
static void tah_sched_take_pending_outcomes(tah_sched *sched, const tah_turn_params *params) {
    for (uint32_t at = 0; at < sched->outcomes_pending.count; at += 1u) {
        const ta_input_outcome *outcome = tah_vec_at(&sched->outcomes_pending, at);
        tah_sched_outcome(sched, params, outcome);
    }
    tah_vec_clear(&sched->outcomes_pending);
}

/*
 * Runs core until it is idle, up to `resumes` calls of ta_run(), acting on
 * each outcome. False when the turn is to stop (the staging bound).
 */
static bool tah_sched_run_core(tah_sched *sched, const tah_turn_params *params) {
    ta_input_outcome outcomes[TAH_SCHED_OUTCOMES];
    if (tah_vec_reserve(&sched->taken_names, TA_RUN_NAMES_MIN_BYTES) != TA_OK) {
        /* No room for the names of the inputs core takes: the next turn. */
        return true;
    }
    for (uint32_t call = 0; call < params->budgets.resumes && sched->core_work; call += 1u) {
        uint32_t count = 0;
        ta_run_status status = TA_RUN_IDLE;
        *sched->inside_core = true;
        ta_result result = ta_run(sched->core, &(ta_run_params){ .budget = params->budgets.microsteps,
                                                                 .action_budget = params->budgets.actions,
                                                                 .report_taken = true,
                                                                 .outcomes = outcomes,
                                                                 .outcome_capacity = TAH_SCHED_OUTCOMES,
                                                                 .outcome_count = &count,
                                                                 .status = &status,
                                                                 .names = sched->taken_names.items,
                                                                 .name_capacity = TA_RUN_NAMES_MIN_BYTES });
        *sched->inside_core = false;
        /* The table is the addon's: core calls never move its pages. */
        for (uint32_t at = 0; at < count; at += 1u) {
            tah_sched_outcome(sched, params, &outcomes[at]);
            ta_event_release(&outcomes[at].event); /* the event of mail core took */
        }
        if (result != TA_OK) {
            /* No memory to take the next input: it waits for the next turn. */
            return true;
        }
        if (status == TA_RUN_IDLE) {
            sched->core_work = false;
        }
        if (params->stop != NULL && params->stop(params->stop_context)) {
            return false;
        }
        if (status == TA_RUN_PAUSED) {
            /* A macrostep its budget paused has had its slice of the turn:
             * the next turn continues it (ADR 0009). */
            return true;
        }
    }
    return true;
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
    tah_sched_record_pending_faults(sched, params);
    tah_sched_take_pending_outcomes(sched, params);
    tah_sched_unstrand(sched);
    tah_sched_admit_holders(sched, params->budgets.messages);
    tah_sched_release_backoff(sched);
    if (!tah_sched_run_core(sched, params)) {
        return;
    }
    tah_sched_report_dead(sched, params, params->budgets.messages);
}

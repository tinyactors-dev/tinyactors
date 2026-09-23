#include "scheduler.h"

#include <string.h>

#include "allocator.h"

#define TNA_RUNNABLE_CHUNK 1024u

void tna_sched_init(tna_sched *sched, ta_allocator allocator) {
    *sched = (tna_sched){ .allocator = allocator };
    tna_vec_init(&sched->pages, allocator, TNA_LAYOUT(tna_sched_entry *));
    tna_ring_init(&sched->runnable, allocator,
                  (tna_ring_shape){ .layout = TNA_LAYOUT(ta_actor_id), .chunk_capacity = TNA_RUNNABLE_CHUNK });
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
    sched->known = 0;
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
    }
    *entry = (tna_sched_entry){ .generation = ta_actor_id_generation(actor), .state = TNA_SCHED_RUNNABLE };
    return TA_OK;
}

void tna_sched_remove(tna_sched *sched, ta_actor_id actor) {
    tna_sched_entry *entry = tna_sched_lookup(sched, actor);
    if (entry == NULL) {
        return;
    }
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
    return tna_ring_count(&sched->runnable) == 0;
}

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

static void tna_sched_fault(tna_sched *sched, const tna_turn_params *params, ta_actor_id actor, ta_result code) {
    tna_sched_entry *entry = tna_sched_lookup(sched, actor);
    if (entry != NULL) {
        entry->state = TNA_SCHED_QUARANTINED;
        entry->flags &= (uint8_t)~TNA_SCHED_BACKOFF;
    }
    tna_sched_record(sched, params, (tna_turn_record){
                                        .kind = TNA_RECORD_FAULT,
                                        .actor = actor,
                                        .operation = "resume",
                                        .code = code,
                                    });
}

/* Puts an actor back at the end of the runnable queue; a failure is a fault. */
static void tna_sched_requeue(tna_sched *sched, const tna_turn_params *params, ta_actor_id actor) {
    if (tna_ring_push(&sched->runnable, &actor) != TA_OK) {
        tna_sched_fault(sched, params, actor, TA_ERR_NOMEM);
    }
}

static void tna_sched_resume_one(tna_sched *sched, const tna_turn_params *params, ta_actor_id actor) {
    tna_sched_entry *entry = tna_sched_lookup(sched, actor);
    if (entry == NULL || entry->state != TNA_SCHED_RUNNABLE) {
        return; /* destroyed, replaced or quarantined since it was queued */
    }
    if ((entry->flags & TNA_SCHED_BACKOFF) != 0) {
        entry->flags &= (uint8_t)~TNA_SCHED_BACKOFF;
        tna_sched_requeue(sched, params, actor);
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
        tna_sched_requeue(sched, params, actor);
        return;
    }
    if (result == TA_ERR_NOT_FOUND) {
        tna_sched_remove(sched, actor);
        return;
    }
    if (result != TA_OK) {
        tna_sched_fault(sched, params, actor, result);
        return;
    }
    switch (status) {
    case TA_RUN_PAUSED:
        tna_sched_requeue(sched, params, actor);
        return;
    case TA_RUN_DONE:
        entry->state = TNA_SCHED_IDLE;
        if ((entry->flags & TNA_SCHED_FINISHED) == 0) {
            entry->flags |= TNA_SCHED_FINISHED;
            tna_sched_record(sched, params, (tna_turn_record){ .kind = TNA_RECORD_DONE, .actor = actor });
        }
        return;
    case TA_RUN_STABLE:
    default:
        entry->state = TNA_SCHED_IDLE; /* step 6: ready when its mailbox has mail */
        return;
    }
}

void tna_sched_turn(tna_sched *sched, const tna_turn_params *params) {
    sched->records = NULL;
    sched->last_record = NULL;
    sched->record_count = 0;
    /* Only actors queued before the turn began: a requeued actor waits for the next one. */
    uint32_t pending = tna_ring_count(&sched->runnable);
    uint32_t limit = params->budgets.resumes < pending ? params->budgets.resumes : pending;
    for (uint32_t index = 0; index < limit; index += 1) {
        ta_actor_id actor = 0;
        if (!tna_ring_pop(&sched->runnable, &actor)) {
            break;
        }
        tna_sched_resume_one(sched, params, actor);
    }
}

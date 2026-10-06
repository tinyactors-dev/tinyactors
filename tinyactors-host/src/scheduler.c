/*
 * Implementation of the scheduler. The contract (what a post is, the
 * lifetimes, the limits, the turn) is in tinyactors_host/scheduler.h; this
 * comment records the order of the steps and what the code relies on.
 *
 * A post is one call into core and some bookkeeping that cannot fail after
 * it. The order is the point: everything that can be refused for the
 * application's reasons is asked before core is touched (the limits, an
 * address's waiting count), then core is asked to admit the input, and only a
 * successful admission is followed by steps that can fail for want of memory
 * (the token's map entries). A failure there withdraws the input again with
 * ta_input_withdraw(), which cannot fail and allocates nothing, so a post
 * that answers TA_ERR_NOMEM left no token behind. The one input that cannot
 * be withdrawn is an operator's transition, which core starts in its running
 * slot at once; it is abandoned instead (ta_actor_abandon()), which leaves
 * the actor where it was because its macrostep had not run a step.
 *
 * A notice is the only thing that ends an input. TAKEN, COMPLETED and
 * REFUSED name a token or an actor; the scheduler finds the waiter by token
 * and settles it, tells the recorder, and makes the records the application
 * hears about. Every map entry of a token is removed by the first notice that
 * ends it, so a token that is reported twice (it cannot be) would find
 * nothing the second time. A session the replay driver runs is settled by the
 * replay: the token bookkeeping still happens for its notices, because core
 * issues a token for every input, but nothing else is made of them.
 *
 * Sizes are computed with sizeof, never from input.
 *
 * Complexity. Cancel and a notice are O(1). A post is O(1), except a post to
 * an address when the family limits its waiting messages: it asks core for
 * the address's waiting count, which walks the waiting list (O(list), at most
 * the limit plus the withdrawn ones). A post made inside a macrostep (a
 * data-model callback) may also allocate in the kernel (the follow table, the
 * verdict ring), outside core's safe points; it answers a clean TA_ERR_NOMEM
 * and withdraws the input when it fails. A turn costs the inputs it
 * finishes plus the verdicts it reports.
 */

#include "tinyactors_host/scheduler.h"

#include "memory.h"

#include "tinyactors_host/allocator.h"
#include "allocator_private.h"
#include "scheduler_private.h"

#define TAH_VERDICT_CHUNK 32u
#define TAH_SETTLED_CHUNK 1024u

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
 * The scheduler's lifetime
 */

void tah_sched_init(tah_sched *sched, ta_allocator allocator, ta_allocator envelopes) {
    *sched = (tah_sched){ .allocator = allocator, .envelopes = envelopes };
    tah_vec_init(&sched->waiters, envelopes, TAH_LAYOUT(tah_waiter));
    tah_vec_init(&sched->follows, allocator, TAH_LAYOUT(tah_follow));
    tah_map_init(&sched->replaying, allocator);
    tah_ring_init(&sched->verdicts, envelopes,
                  (tah_ring_shape){ .layout = TAH_LAYOUT(tah_verdict), .chunk_capacity = TAH_VERDICT_CHUNK });
    /* One chunk holds a default turn's settlements (messageBudget is 1024 by default), so steady state reuses it. */
    tah_ring_init(&sched->settled, allocator,
                  (tah_ring_shape){ .layout = TAH_LAYOUT(tah_settlement), .chunk_capacity = TAH_SETTLED_CHUNK });
}

void tah_sched_set_limits(tah_sched *sched, uint32_t max_queued, uint64_t max_retained) {
    sched->max_queued = max_queued;
    sched->max_retained = max_retained;
}

/* Gives back what a verdict owns. */
static void tah_verdict_release(tah_verdict *verdict) {
    ta_event_release(&verdict->event);
    ta_address_release(verdict->address);
    *verdict = (tah_verdict){0};
}

void tah_sched_discard(tah_sched *sched) {
    tah_verdict verdict;
    while (tah_ring_pop(&sched->verdicts, &verdict)) {
        tah_verdict_release(&verdict);
    }
}

void tah_sched_destroy(tah_sched *sched) {
    tah_sched_discard(sched); /* the references a verdict holds are core's: they go before core does */
    tah_vec_destroy(&sched->waiters);
    tah_vec_destroy(&sched->follows);
    tah_map_destroy(&sched->replaying);
    tah_ring_destroy(&sched->verdicts);
    tah_ring_destroy(&sched->settled);
    sched->waiter_free = 0;
    sched->waiters_live = 0;
    sched->retained_total = 0;
    sched->records = NULL;
    sched->last_record = NULL;
    sched->record_count = 0;
}

/* Tells the recorder, when there is one (kernel ADR 0006 section 2). */
static void tah_sched_note_to(const tah_sched *sched, tah_sched_note note) {
    if (sched->observer.self == NULL) {
        return;
    }
    sched->observer.note(sched->observer.self, &note);
}

/* ------------------------------------------------------------------------
 * The inputs the kernel follows
 */

/* The entry that follows `token`, or NULL when none does (it ended, or was never followed). */
static tah_follow *tah_sched_follow_of(const tah_sched *sched, uint64_t token) {
    uint32_t slot = ta_input_slot(token);
    if (token == 0 || slot >= sched->follows.count) {
        return NULL;
    }
    tah_follow *follow = tah_vec_at(&sched->follows, slot);
    return follow->token == token ? follow : NULL;
}

/*
 * Makes the entry for `token`, growing the table to its slot. TA_ERR_NOMEM
 * leaves the table as it was apart from entries it added that follow nothing.
 */
static ta_result tah_sched_follow_make(tah_sched *sched, uint64_t token, tah_follow **out) {
    uint32_t slot = ta_input_slot(token);
    if (slot >= sched->follows.count && tah_vec_reserve(&sched->follows, slot + 1u) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    tah_follow none = {0};
    while (sched->follows.count <= slot) {
        if (tah_vec_push(&sched->follows, &none) != TA_OK) {
            return TA_ERR_NOMEM;
        }
    }
    *out = tah_vec_at(&sched->follows, slot);
    return TA_OK;
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
    waiter->token = 0;
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

tah_waiter_state tah_sched_waiter_state(const tah_sched *sched, uint32_t index) {
    const tah_waiter *waiter = tah_sched_waiter(sched, index);
    return waiter == NULL ? TAH_WAITER_FREE : (tah_waiter_state)waiter->state;
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
    if (waiter->token != 0) {
        tah_follow *follow = tah_sched_follow_of(sched, waiter->token);
        if (follow != NULL) {
            follow->waiter = 0; /* a waiter given back at close still names its token */
        }
    }
    waiter->generation = waiter->generation == UINT32_MAX ? 1u : waiter->generation + 1u;
    waiter->state = TAH_WAITER_FREE;
    waiter->token = 0;
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
    waiter->token = 0;
    settlement.waiter = index;
    /* The ring keeps spare chunks, so this allocates only as it grows. If
     * it cannot, the promise is not settled until close() rejects it. */
    (void)tah_ring_push(&sched->settled, &settlement);
}

/* ------------------------------------------------------------------------
 * Tokens
 */

bool tah_sched_awaited(const tah_sched *sched, uint64_t token) {
    const tah_follow *follow = sched->followed == 0 ? NULL : tah_sched_follow_of(sched, token);
    return follow != NULL && follow->waiter != 0;
}

/* Ends the following of `follow`: its bytes leave the count and the slot is free again. */
static void tah_sched_unfollow(tah_sched *sched, tah_follow *follow) {
    sched->retained_total -= follow->bytes;
    *follow = (tah_follow){0};
    sched->followed -= 1u;
}

/*
 * The input `token` ended: its waiter, if it has one, settles with
 * `settlement`, and its retained bytes go. A token the scheduler never
 * followed (a plain post, a replay's input, a report) finds nothing.
 */
static void tah_sched_finish_token(tah_sched *sched, uint64_t token, tah_settlement settlement) {
    tah_follow *follow = sched->followed == 0 ? NULL : tah_sched_follow_of(sched, token);
    if (follow == NULL) {
        return;
    }
    uint32_t waiter = follow->waiter;
    tah_sched_unfollow(sched, follow);
    if (waiter != 0) {
        tah_sched_settle_waiter(sched, waiter, settlement);
    }
}

bool tah_sched_cancel(tah_sched *sched, ta_system *core, uint32_t index) {
    tah_waiter *waiter = tah_sched_waiter(sched, index);
    if (waiter == NULL || waiter->state != TAH_WAITER_PENDING) {
        return false;
    }
    if (waiter->token != 0) {
        uint64_t token = waiter->token;
        tah_follow *follow = tah_sched_follow_of(sched, token);
        /* An input core has not taken never runs. One it has taken keeps its macrostep and its retained bytes until
         * the notice that ends it; the promise is detached either way. */
        if (ta_input_withdraw(core, token) == TA_OK) {
            if (follow != NULL) {
                tah_sched_unfollow(sched, follow);
            }
        } else if (follow != NULL) {
            follow->waiter = 0;
            if (follow->bytes == 0) {
                tah_sched_unfollow(sched, follow);
            }
        }
    }
    waiter->token = 0;
    waiter->state = TAH_WAITER_SETTLING;
    return true;
}

/* ------------------------------------------------------------------------
 * Replaying actors
 */

ta_result tah_sched_replaying_add(tah_sched *sched, ta_actor_id actor) {
    return tah_map_put(&sched->replaying, actor, 1u);
}

void tah_sched_replaying_remove(tah_sched *sched, ta_actor_id actor) {
    if (tah_map_count(&sched->replaying) != 0) {
        tah_map_remove(&sched->replaying, actor);
    }
}

bool tah_sched_replaying(const tah_sched *sched, ta_actor_id actor) {
    uint64_t found = 0;
    return tah_map_count(&sched->replaying) != 0 && tah_map_get(&sched->replaying, actor, &found);
}

/* ------------------------------------------------------------------------
 * Posting
 */

bool tah_sched_full(const tah_sched *sched, const ta_system *core, uint32_t bytes) {
    if (sched->max_queued != 0) {
        ta_counters counters;
        ta_system_counters(core, &counters);
        if (counters.host_inputs + tah_ring_count(&sched->verdicts) >= sched->max_queued) {
            return true;
        }
    }
    return sched->max_retained != 0 && sched->retained_total + bytes > sched->max_retained;
}

/*
 * Makes the verdict for a post core refused for the application's reasons,
 * or that the scheduler refused itself (an address's waiting limit): a dead
 * letter the next turn reports. TA_ERR_NOMEM, with nothing retained, when
 * the ring cannot grow.
 */
static ta_result tah_sched_verdict(tah_sched *sched, const tah_message *message, tah_dead_reason reason) {
    tah_verdict verdict = { .target = message->target, .address = message->address, .waiter = message->waiter,
                            .reason = (uint8_t)reason };
    if (message->trace != NULL && !(message->trace->context.trace_id_high == 0 &&
                                    message->trace->context.trace_id_low == 0 &&
                                    message->trace->cause.session == 0 && message->trace->cause.macrostep == 0)) {
        verdict.trace = *message->trace;
        verdict.has_trace = 1u;
    }
    const tah_waiter *waiter = tah_sched_waiter(sched, message->waiter);
    verdict.waiter_generation = waiter != NULL ? waiter->generation : 0u;
    ta_event_retain(message->event, &verdict.event);
    ta_address_retain(verdict.address);
    if (tah_ring_push(&sched->verdicts, &verdict) != TA_OK) {
        tah_verdict_release(&verdict);
        return TA_ERR_NOMEM;
    }
    tah_waiter *follower = tah_sched_waiter(sched, message->waiter);
    if (follower != NULL) {
        follower->state = TAH_WAITER_PENDING;
        follower->token = 0;
    }
    return TA_OK;
}

/*
 * Asks core to start an operator's transition. The actor must run the
 * definition the state IDs belong to (a replacement may have taken the
 * address over, ADR 0005 point 12); TA_ERR_INVALID otherwise, as core answers
 * a transition it does not accept. TA_ERR_NOT_FOUND for an actor core does
 * not know.
 */
static ta_result tah_sched_transition(const tah_post *post, uint64_t *token) {
    const tah_message *message = post->message;
    ta_actor_inspection view;
    ta_result result = ta_actor_inspect(post->core, message->target, &view);
    if (result != TA_OK) {
        return result;
    }
    if (view.definition_id != message->jump->definition) {
        return TA_ERR_INVALID;
    }
    ta_trace_parent none = {0};
    return ta_actor_transition(post->core, &(ta_actor_transition_params){
                                               .actor = message->target,
                                               .event = message->event,
                                               .trace = message->trace != NULL ? *message->trace : none,
                                               .transition = &message->jump->transition,
                                               .time = post->time,
                                               .input = token });
}

/*
 * Follows the admitted input `token` for the post: its waiter becomes PENDING
 * on it, and its bytes are counted when maxRetainedBytes is set. TA_ERR_NOMEM
 * leaves no entry behind; the caller then takes the input back.
 */
static ta_result tah_sched_follow(tah_sched *sched, const tah_message *message, uint64_t token) {
    bool counted = sched->max_retained != 0 && message->bytes != 0;
    tah_waiter *waiter = tah_sched_waiter(sched, message->waiter);
    if (waiter == NULL && !counted) {
        return TA_OK;
    }
    tah_follow *follow = NULL;
    if (tah_sched_follow_make(sched, token, &follow) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    *follow = (tah_follow){ .token = token, .waiter = waiter != NULL ? message->waiter : 0u,
                            .bytes = counted ? message->bytes : 0u };
    sched->followed += 1u;
    sched->retained_total += follow->bytes;
    if (waiter != NULL) {
        waiter->state = TAH_WAITER_PENDING;
        waiter->token = token;
    }
    return TA_OK;
}

ta_result tah_sched_post(tah_sched *sched, const tah_post *post) {
    const tah_message *message = post->message;
    ta_system *core = post->core;
    if (tah_sched_full(sched, core, message->bytes)) {
        return TA_ERR_FULL;
    }
    if (message->address != NULL && post->max_waiting != 0 &&
        ta_address_waiting(core, message->address) >= post->max_waiting) {
        /* The address already has all the waiting messages its family allows (napi ADR 0020). */
        ta_result made = tah_sched_verdict(sched, message, TAH_DEAD_WAITING_FULL);
        if (made == TA_OK) {
            sched->delivered += 1u;
        }
        return made;
    }
    uint64_t token = 0;
    ta_result result;
    if (message->jump != NULL) {
        result = tah_sched_transition(post, &token);
    } else {
        ta_trace_parent none = {0};
        result = ta_actor_deliver(core, &(ta_actor_deliver_params){ .actor = message->target,
                                                                    .address = message->address,
                                                                    .event = message->event,
                                                                    .trace = message->trace != NULL ? *message->trace : none,
                                                                    .time = post->time,
                                                                    .input = &token });
    }
    if (result == TA_ERR_NOT_FOUND || result == TA_ERR_DONE) {
        /* A target that is not here, or has finished, is the message's fate, not a failure of the post. */
        ta_result made = tah_sched_verdict(sched, message,
                                           result == TA_ERR_DONE ? TAH_DEAD_DONE : TAH_DEAD_NOT_FOUND);
        if (made == TA_OK) {
            sched->delivered += 1u;
        }
        return made;
    }
    if (result != TA_OK) {
        return result;
    }
    result = tah_sched_follow(sched, message, token);
    if (result != TA_OK) {
        if (message->jump != NULL) {
            (void)ta_actor_abandon(core, message->target); /* the transition has not run a step: the actor is as it was */
        } else {
            (void)ta_input_withdraw(core, token);
        }
        return result;
    }
    sched->delivered += 1u;
    return TA_OK;
}

void tah_sched_read_stats(const tah_sched *sched, const ta_system *core, tah_sched_stats *out) {
    ta_counters counters;
    ta_system_counters(core, &counters);
    *out = (tah_sched_stats){ .queued = (uint32_t)counters.host_inputs + tah_ring_count(&sched->verdicts),
                              .delivered = sched->delivered,
                              .dead_letters = sched->dead_letters };
    for (uint32_t reason = 0; reason < TAH_DEAD_REASONS; reason += 1u) {
        out->dead_by_reason[reason] = sched->dead_by_reason[reason];
    }
    for (uint32_t code = 0; code < TAH_FAULT_CODES; code += 1u) {
        out->faults[code] = sched->faults[code];
    }
}

bool tah_sched_quiet(const tah_sched *sched) {
    return tah_ring_count(&sched->verdicts) == 0 && tah_ring_count(&sched->settled) == 0;
}

/* ------------------------------------------------------------------------
 * Records
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

/* The left-over turn budget one finished input or reported verdict uses up. */
static void tah_sched_spend(tah_sched *sched) {
    if (sched->turn_left > 0) {
        sched->turn_left -= 1u;
    }
}

/*
 * Reports up to the turn's budget of verdicts as dead-letter records, and
 * settles their send() waiters (those still waiting for them). It reports
 * what was made between turns, in the order it was made.
 */
static void tah_sched_report_verdicts(tah_sched *sched, const tah_turn_params *params) {
    tah_verdict verdict;
    while (sched->turn_left > 0 && tah_ring_pop(&sched->verdicts, &verdict)) {
        tah_sched_spend(sched);
        sched->dead_letters += 1u;
        sched->dead_by_reason[verdict.reason < TAH_DEAD_REASONS ? verdict.reason : 0u] += 1u;
        tah_turn_record record = {
            .kind = TAH_RECORD_DEAD_LETTER,
            .actor = verdict.target,
            .event = verdict.event.name,
            .reason = (tah_dead_reason)verdict.reason,
            /* The data moves to the record; the verdict gives back the rest. */
            .data = verdict.event.data,
            .has_trace = verdict.has_trace != 0,
            .trace = verdict.trace,
        };
        verdict.event.data = (ta_value){0};
        if (verdict.address != NULL) {
            record.has_address = tah_copy_address(params->arena, verdict.address, &record.address);
        }
        tah_sched_record(sched, params, record);
        const tah_waiter *waiter = tah_sched_waiter(sched, verdict.waiter);
        if (waiter != NULL && waiter->state == TAH_WAITER_PENDING && waiter->generation == verdict.waiter_generation) {
            tah_sched_settle_waiter(sched, verdict.waiter,
                                    (tah_settlement){ .kind = TAH_SETTLE_DEAD, .reason = verdict.reason,
                                                      .actor = verdict.target });
        }
        tah_verdict_release(&verdict);
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

/* ------------------------------------------------------------------------
 * Notices
 */

/* The turn left core: let the owner read what the call traced (tah_turn_params.left_core). */
static void tah_sched_left_core(const tah_turn_params *params) {
    if (params->left_core != NULL) {
        params->left_core(params->stop_context);
    }
}

/* The dead-letter reason a refusal reads as. */
static tah_dead_reason tah_dead_of(ta_refusal_reason reason) {
    switch (reason) {
    case TA_REFUSED_DESTROYED: return TAH_DEAD_DESTROYED;
    case TA_REFUSED_DONE: return TAH_DEAD_DONE;
    case TA_REFUSED_INVALID:
    case TA_REFUSED_DISCARDED: return TAH_DEAD_REFUSED;
    case TA_REFUSED_LOAD_REJECTED: return TAH_DEAD_REJECTED;
    case TA_REFUSED_LOAD_FAILED: return TAH_DEAD_LOAD_FAILED;
    case TA_REFUSED_LOAD_TIMED_OUT: return TAH_DEAD_LOAD_TIMEOUT;
    case TA_REFUSED_LOAD_UNLOADED: return TAH_DEAD_UNLOADED;
    case TA_REFUSED_LOAD_NOT_FOUND:
    default: return TAH_DEAD_NOT_FOUND;
    }
}

/*
 * An admitted message core could not take (a REFUSED notice): its target went
 * or finished after core admitted it, a load for its address failed, or the
 * macrostep that posted it was abandoned. It is a dead letter, as if
 * ta_actor_deliver() had said so. The event belongs to core until the next
 * ta_run(); the record keeps its own references.
 */
static void tah_sched_refused(tah_sched *sched, const tah_turn_params *params, const ta_run_notice *notice) {
    tah_dead_reason reason = tah_dead_of(notice->reason);
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
    if (notice->address != NULL) {
        record.has_address = tah_copy_address(params->arena, notice->address, &record.address);
    }
    record.has_trace = notice->trace.context.trace_id_high != 0 || notice->trace.context.trace_id_low != 0;
    if (record.has_trace) {
        record.trace = notice->trace;
    }
    sched->dead_letters += 1u;
    sched->dead_by_reason[reason] += 1u;
    tah_sched_record(sched, params, record);
    tah_sched_finish_token(sched, notice->input,
                           (tah_settlement){ .kind = TAH_SETTLE_DEAD, .reason = (uint8_t)reason,
                                             .actor = notice->actor });
}

/*
 * An input ended (a COMPLETED notice). A send() that follows it settles by
 * how it ended; the recorder hears that the macrostep ended, and that the
 * actor is stable (or finished: the FINISHED notice that follows says so
 * once). A runaway is a record: the macrostep reached the microstep limit and
 * core abandoned it, and the actor stands as it was after its last microstep.
 */
static void tah_sched_completed(tah_sched *sched, const tah_turn_params *params, const ta_run_notice *notice) {
    ta_actor_id actor = notice->actor;
    bool abandoned = notice->status == TA_INPUT_ABANDONED;
    bool runaway = notice->status == TA_INPUT_RUNAWAY;
    tah_settle_kind kind = abandoned ? TAH_SETTLE_ABANDONED
                           : notice->status == TA_INPUT_DONE ? TAH_SETTLE_DONE
                           : runaway ? TAH_SETTLE_RUNAWAY
                                     : TAH_SETTLE_STABLE;
    tah_sched_finish_token(sched, notice->input,
                           (tah_settlement){ .kind = (uint8_t)kind, .ignored = notice->ignored, .actor = actor });
    if (abandoned || tah_sched_replaying(sched, actor)) {
        return; /* an abandoned input has no end to note, and a replay settles its own actor */
    }
    if (notice->source == TA_SOURCE_MAIL) {
        sched->delivered += 1u; /* mail counts as delivered once its receiver ran it, as a posted message does */
    }
    tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_ENDED, .actor = actor });
    if (runaway) {
        sched->faults[TAH_FAULT_RUNAWAY] += 1u;
        tah_sched_record(sched, params, (tah_turn_record){ .kind = TAH_RECORD_FAULT, .actor = actor,
                                                           .operation = "run" });
    }
    if (notice->status != TA_INPUT_DONE) {
        tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_SETTLED, .actor = actor });
    }
}

#define TAH_RUN_NOTICES 16u

ta_result tah_sched_run(tah_sched *sched, const tah_turn_params *params, tah_run_watch *watch, ta_run_status *status) {
    ta_result result = TA_OK;
    uint32_t budget = params->budgets.microsteps != 0 ? params->budgets.microsteps : 64u;
    bool lent = false; /* a notice lent its event, which core keeps until the next call (REFUSED, TAKEN) */
    do {
        if (sched->turn_bounded && sched->turn_left == 0) {
            *status = TA_RUN_MORE; /* the turn used its inputs: the rest waits for the next one */
            break;
        }
        ta_run_notice notices[TAH_RUN_NOTICES];
        /* A host that polls `stop` (the trace staging bound) hears of each input as it ends: room for one input's
         * notices makes core stop after it, and the loop below asks. */
        uint32_t capacity = params->stop != NULL ? 3u : TAH_RUN_NOTICES;
        uint32_t count = 0;
        *params->inside_core = true;
        sched->turn_run_calls += 1u;
        sched->run_calls_total += 1u;
        result = ta_run(params->core, &(ta_run_params){ .budget = budget,
                                                        .action_budget = params->budgets.actions,
                                                        .report_taken = sched->observer.self != NULL,
                                                        .status = status,
                                                        .notices = notices,
                                                        .notice_capacity = capacity,
                                                        .notice_count = &count });
        *params->inside_core = false;
        tah_sched_left_core(params);
        for (uint32_t index = 0; index < count; index += 1u) {
            const ta_run_notice *notice = &notices[index];
            bool replayed = tah_sched_replaying(sched, notice->actor);
            switch (notice->kind) {
            case TA_NOTICE_LOAD_NEEDED:
                if (params->load_needed != NULL) {
                    params->load_needed(params->stop_context, notice);
                }
                break;
            case TA_NOTICE_INPUT_TAKEN:
                lent = true;
                if (!replayed) {
                    tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_TAKEN, .actor = notice->actor,
                                                               .event = notice->event, .time = notice->time });
                }
                break;
            case TA_NOTICE_FINISHED:
                if (!replayed) {
                    tah_sched_record(sched, params, (tah_turn_record){ .kind = TAH_RECORD_DONE, .actor = notice->actor });
                    tah_sched_note_to(sched, (tah_sched_note){ .kind = TAH_NOTE_FINISHED, .actor = notice->actor });
                }
                break;
            case TA_NOTICE_INPUT_COMPLETED:
                tah_sched_spend(sched);
                if (watch != NULL && notice->actor == watch->actor) {
                    tah_sched_finish_token(sched, notice->input, (tah_settlement){ .kind = TAH_SETTLE_STABLE });
                    watch->completed = true;
                    watch->done = watch->done || notice->status == TA_INPUT_DONE;
                    watch->ignored = watch->ignored || notice->ignored;
                } else {
                    tah_sched_completed(sched, params, notice);
                }
                break;
            case TA_NOTICE_INPUT_REFUSED:
                lent = true;
                tah_sched_spend(sched);
                tah_sched_refused(sched, params, notice);
                break;
            default:
                break;
            }
        }
        if (*status == TA_RUN_NOTICES_FULL && params->stop != NULL && params->stop(params->stop_context)) {
            *status = TA_RUN_MORE; /* the host asked the turn to end: what core still holds waits for the next one */
            break;
        }
    } while (result == TA_OK && *status == TA_RUN_NOTICES_FULL);
    if (lent && result == TA_OK && *status == TA_RUN_IDLE) {
        /* Nothing is left to run, and the last call's lent events would keep their data until the next turn: an
         * idle call gives them back now (a refused post's data is released when its dead letter is reported). */
        ta_run_notice none[3];
        uint32_t count = 0;
        ta_run_status idle = TA_RUN_IDLE;
        *params->inside_core = true;
        (void)ta_run(params->core, &(ta_run_params){ .budget = budget, .status = &idle, .notices = none,
                                                     .notice_capacity = 3u, .notice_count = &count });
        *params->inside_core = false;
        tah_sched_left_core(params);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * The turn
 */

void tah_sched_turn(tah_sched *sched, const tah_turn_params *params) {
    /* `inputs` bounds the inputs core finishes in the turn and the verdicts it reports, not the calls: one call of
     * ta_run() serves every input that is next, whoever it is for. */
    sched->turn_left = params->budgets.inputs;
    sched->turn_bounded = true;
    sched->turn_run_calls = 0;
    tah_sched_report_verdicts(sched, params);
    while (sched->turn_left > 0 && ta_run_pending(params->core)) {
        ta_run_status status = TA_RUN_IDLE;
        ta_result result = tah_sched_run(sched, params, NULL, &status);
        if (status == TA_RUN_PAUSED) {
            sched->turn_left = 0; /* a budget stopped an input inside its macrostep: the next turn continues it */
        }
        if (result != TA_OK || status == TA_RUN_IDLE || (params->stop != NULL && params->stop(params->stop_context))) {
            break;
        }
    }
    sched->turn_bounded = false;
}

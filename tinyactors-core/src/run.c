/*
 * Implementation of package run. The contract, the lifetimes and the
 * invariants are in run.h; this comment records the order of the steps and
 * why they are in it.
 *
 * Admission (ta_actor_deliver) is the old delivery test turned into a queue
 * push. The checks come in the order that makes the answer the host's whole
 * decision (invalid, done, full), then the one step that can fail for lack
 * of memory (the pool refill), then steps that cannot: filling the envelope
 * and queueing it. Nothing is retained on any other result.
 *
 * Taking an input is the old delivery's second half. The event moves from its
 * envelope into the session's event block by *retaining* a copy, not by
 * moving it: the envelope keeps its reference until the input ends, so a TAKEN
 * notice can lend the event to the host, the block's `_event` binding is
 * untouched, and a run that stops for lack of memory can take the input
 * again. The envelope is the input's record (ADR 0099 section 8); the block
 * is the interpreter's working storage until a later step of unit 4.2c retires it.
 *
 * ta_run is a loop over four decisions, in this order each time round: report
 * a slot that was abandoned; take the next input if the slot is free (the
 * started sessions first, then the run queue); stop if a budget is spent; run
 * the slot's input until its macrostep ends, a budget stops it, or the
 * microstep limit abandons it. It checks the room for the notices it will
 * write before it takes or runs anything, so it never has to un-take an input
 * because the caller's array was too small.
 */

#include "internal.h"

/* ta_run_notices is the caller's array and how much of it ta_run has used. Lifetime: call. */
typedef struct ta_run_notices {
    ta_run_notice *notices;
    uint32_t capacity;
    uint32_t count;
} ta_run_notices;

/*
 * ta_run_room answers how many more notices fit in the caller's array. It
 * never allocates and never fails.
 */
static uint32_t ta_run_room(const ta_run_notices *out) {
    return out->capacity - out->count;
}

/*
 * ta_run_notice_add copies `notice` into the next free place of the array.
 * It needs ta_run_room(out) > 0 (callers reserve room before they act) and
 * never allocates; the notice's borrowed pointers are copied, not retained.
 */
static void ta_run_notice_add(ta_run_notices *out, const ta_run_notice *notice) {
    out->notices[out->count] = *notice;
    out->count += 1u;
}

/* ta_runner_init: see run.h. */
ta_result ta_runner_init(ta_runner *runner, ta_allocator allocator) {
    *runner = (ta_runner){0};
    ta_result result = ta_pool_init(&runner->cells, allocator,
                                    &(ta_pool_params){ .item_size = sizeof(ta_run_cell),
                                                       .item_alignment = _Alignof(ta_run_cell),
                                                       .link_offset = offsetof(ta_run_cell, next),
                                                       .page_items = TA_RUN_CELL_PAGE });
    if (result != TA_OK) {
        *runner = (ta_runner){0};
        return result;
    }
    ta_ilist_init(&runner->lent);
    ta_ilist_init(&runner->abandoned);
    return TA_OK;
}

/* ta_runner_destroy: see run.h. */
void ta_runner_destroy(ta_runner *runner) {
    ta_pool_destroy(&runner->cells);
    *runner = (ta_runner){0};
}

/* ta_run_reserve_start: see run.h. */
ta_result ta_run_reserve_start(ta_system *system) {
    return ta_pool_refill(&system->runner.cells, 1u);
}

/* ta_run_start: see run.h. */
void ta_run_start(ta_system *system, ta_actor *actor) {
    if (actor->started) {
        return;
    }
    ta_runner *runner = &system->runner;
    uint32_t index = ta_pool_take(&runner->cells);
    if (index == 0) {
        return; /* only if the caller did not reserve */
    }
    ta_run_cell *cell = ta_pool_at(&runner->cells, index);
    cell->id = actor->record.id;
    if (runner->started_last == 0) {
        runner->started_first = index;
    } else {
        ((ta_run_cell *)ta_pool_at(&runner->cells, runner->started_last))->next = index;
    }
    runner->started_last = index;
    actor->started = true;
}

/*
 * ta_run_end_slot frees the slot of a session that no longer runs its input:
 * the input's envelope, if it has a token to report, waits on `abandoned`
 * for the next ta_run. An input with no envelope (a started macrostep) has
 * nobody to tell.
 */
static void ta_run_end_slot(ta_system *system) {
    ta_run_slot *slot = &system->runner.slot;
    ta_mail_macrostep_discarded(system); /* the mail it sent so far is never seen (ADR 0099 section 4) */
    if (slot->envelope != 0) {
        /* The event is not needed to say how the input ended: let go of it now, not at the next call. */
        ta_envelope *envelope = ta_envelope_at(&system->envelopes, slot->envelope);
        ta_event_release(&envelope->event);
        envelope->event = (ta_event){0};
        ta_ilist_push_back(&system->runner.abandoned, ta_envelopes_link(&system->envelopes), slot->envelope);
    }
    *slot = (ta_run_slot){0};
}

/*
 * ta_run_slot_open gives the input that has just filled the slot its mail
 * state: an empty outgoing list and the macrostep reserve (ADR 0099 section
 * 3), as many envelopes as the quota allows up to macrostepReserve. It never
 * allocates and cannot fail; the pool was refilled to cover the reserve before
 * the input was chosen (ta_run_cover).
 */
static void ta_run_slot_open(ta_system *system) {
    ta_run_slot *slot = &system->runner.slot;
    ta_ilist_init(&slot->outgoing);
    slot->claim = (ta_quota_claim){0};
    (void)ta_envelopes_reserve(&system->envelopes, &slot->claim);
}

/*
 * ta_run_cover makes the pool hold at least macrostepReserve free envelopes
 * (fewer when the bound does not allow), so that the claim ta_run_slot_open
 * makes is backed by envelopes that exist and a send in the macrostep never
 * fails because the pool had not grown yet. It is a safe point: the only
 * allocation in the path of taking an input. Answers TA_ERR_NOMEM with
 * nothing changed (envelope.h: a refill is all or nothing).
 */
static ta_result ta_run_cover(ta_system *system) {
    return ta_envelopes_refill(&system->envelopes, system->envelopes.macrostep_reserve);
}

/* ta_run_actor_gone: see run.h. */
void ta_run_actor_gone(ta_system *system, const ta_actor *actor) {
    const ta_run_slot *slot = &system->runner.slot;
    if (slot->occupied && slot->actor == actor->record.id) {
        ta_run_end_slot(system);
    }
}

/* ta_run_unloaded: see run.h. */
void ta_run_unloaded(ta_system *system, const ta_actor *actor) {
    const ta_run_slot *slot = &system->runner.slot;
    if (slot->occupied && slot->actor == actor->record.id) {
        ta_mail_macrostep_ended(system);
    }
    ta_run_actor_gone(system, actor);
}

/* ta_run_abandoned: see run.h. */
void ta_run_abandoned(ta_system *system, const ta_actor *actor) {
    ta_run_actor_gone(system, actor);
}

/* ------------------------------------------------------------------------
 * Admission
 */

/* ta_run_token issues the next token: never 0, never repeated. */
static uint64_t ta_run_token(ta_system *system) {
    system->runner.tokens += 1u;
    return system->runner.tokens;
}

/*
 * ta_run_event_valid is the part of the admission test that depends on the
 * event and the target alone: a name this system issued, not the root, and a
 * time the target accepts. It is the same for an admitted event and for an
 * operator's.
 */
static bool ta_run_event_valid(const ta_system *system, const ta_actor *actor, const ta_event *event, ta_time time) {
    return ta_trie_valid(&system->event_names.trie, event->name) && event->name != TA_EVENT_NAME_ROOT &&
           ta_actor_input_time_valid(system, actor, time);
}

typedef struct ta_run_input {
    const ta_event *event;
    const ta_trace_parent *trace;
    ta_time time;
    ta_input_source source;
    uint32_t room; /* free envelopes the pool must hold before this one is made, at least 1 */
} ta_run_input;

/*
 * ta_run_fill makes the envelope for an input and fills it from `event`
 * (retained: the envelope owns its references from here). It consumes one
 * unit of the pool's quota that nothing reserved. The envelope is not on a
 * list. Answers 0 when the pool cannot make one: the refill failed, or the
 * pool is at its bound.
 *
 * The refill leaves room for the envelope and for what the running input has
 * yet to claim: envelopes are interchangeable pool items, so one taken for
 * admission could otherwise be the item a send in a paused macrostep was
 * promised (envelope.h: a claim is backed by items that exist).
 */
static uint32_t ta_run_fill(ta_system *system, ta_actor *actor, const ta_run_input *input) {
    uint32_t promised = (uint32_t)system->runner.slot.claim.held;
    if (ta_envelopes_refill(&system->envelopes, input->room + promised) != TA_OK) {
        return 0;
    }
    uint32_t index = ta_envelope_make(&system->envelopes, NULL, TA_ENVELOPE_RUNNABLE);
    if (index == 0) {
        return 0;
    }
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    envelope->source = (uint8_t)input->source;
    envelope->target = actor->record.id;
    envelope->due = input->time;
    envelope->trace = *input->trace;
    ta_event_retain(input->event, &envelope->event);
    return index;
}

/*
 * ta_actor_deliver admits an event: see tinyactors.h. The steps that can
 * fail precede the ones that cannot, and the event's references are taken
 * last, so every refusal leaves the caller's event untouched.
 */
ta_result ta_actor_deliver(ta_system *system, const ta_actor_deliver_params *params) {
    if (system == NULL || params == NULL || params->event == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (!ta_run_event_valid(system, actor, params->event, params->time)) {
        return TA_ERR_INVALID;
    }
    /* Before full: a finished actor never becomes free again (ADR 0055). */
    if (actor->finished) {
        return TA_ERR_DONE;
    }
    if (!ta_envelopes_admissible(&system->envelopes) || actor->queued == UINT16_MAX) {
        return TA_ERR_FULL;
    }
    ta_event accepted = *params->event;
    ta_result result = ta_actor_accept_origin(system, actor, &accepted);
    if (result != TA_OK) {
        return result;
    }
    uint32_t index = ta_run_fill(system, actor, &(ta_run_input){ .event = &accepted,
                                                                 .trace = &params->trace,
                                                                 .time = params->time,
                                                                 .source = TA_SOURCE_HOST,
                                                                 .room = 1u });
    if (index == 0) {
        return TA_ERR_NOMEM;
    }
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    envelope->input = ta_run_token(system);
    if (params->input != NULL) {
        *params->input = envelope->input;
    }
    actor->queued += 1u;
    ta_envelopes_run_push(&system->envelopes, index);
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Taking an input
 */

/*
 * ta_run_install makes the actor's macrostep begin with the event in
 * `envelope`: the session takes a copy into its event block (which the caller
 * has already made), moves its clock to the input's time, and runs under the
 * input's trace parent. The operator transition `jump`, if any, passes to
 * the block, which frees it when the macrostep takes the event. Nothing here
 * allocates or fails.
 */
static void ta_run_install(ta_actor *actor, const ta_envelope *envelope, ta_operator_jump *jump) {
    ta_actor_move_clock(actor, envelope->due);
    ta_event_slots *slots = &actor->event_block->slots;
    ta_event_retain(&envelope->event, &slots->external_event);
    slots->external = TA_EXTERNAL_WAITING;
    slots->jump = jump;
    slots->trace = envelope->trace;
    actor->macrostep = true;
    ta_actor_touch(actor);
    actor->record.residency = TA_ACTOR_RUNNING;
}

/*
 * ta_run_refuse discards an admitted input that cannot be taken: a host input
 * becomes a REFUSED notice that lends its event until the next call; any other
 * input is released unseen (nobody asked to be told). The caller has checked
 * that the notice array has room.
 */
static void ta_run_refuse(ta_system *system, uint32_t index, ta_refusal_reason reason, ta_run_notices *out) {
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    if (envelope->source != TA_SOURCE_HOST) {
        /* Mail comes back to its sender as an error; anything else nobody asked to hear about is freed. */
        ta_mail_recycle(system, index, reason == TA_REFUSED_DONE ? TA_RECYCLE_DONE : TA_RECYCLE_DESTROYED);
        return;
    }
    ta_run_notice_add(out, &(ta_run_notice){ .kind = TA_NOTICE_INPUT_REFUSED,
                                             .actor = envelope->target,
                                             .input = envelope->input,
                                             .source = (ta_input_source)envelope->source,
                                             .reason = reason,
                                             .event = &envelope->event });
    ta_ilist_push_back(&system->runner.lent, ta_envelopes_link(&system->envelopes), index);
}

/*
 * ta_run_take takes the envelope `index`, already off the run queue, into the
 * slot, or discards it. TA_OK says it was dealt with: `*occupied` tells which.
 * TA_ERR_NOMEM says the session could not get an event block; nothing changed
 * and the caller puts the envelope back at the head of the queue.
 */
static ta_result ta_run_take(ta_system *system, uint32_t index, ta_run_notices *out, bool *occupied) {
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    ta_actor *actor = ta_actor_lookup(system, envelope->target);
    *occupied = false;
    if (actor == NULL && (envelope->source == TA_SOURCE_MAIL || envelope->source == TA_SOURCE_ERROR)) {
        actor = ta_mail_follow(system, index); /* its session may have been unloaded and loaded again meanwhile */
    }
    if (actor == NULL) {
        ta_run_refuse(system, index, TA_REFUSED_DESTROYED, out);
        return TA_OK;
    }
    if (actor->finished) {
        actor->queued -= 1u;
        ta_run_refuse(system, index, TA_REFUSED_DONE, out);
        return TA_OK;
    }
    /* A replaying actor's clock may have moved past the time it was admitted for; every other actor ignores it. */
    if (ta_actor_replaying(actor) && !ta_actor_input_time_valid(system, actor, envelope->due)) {
        actor->queued -= 1u;
        ta_run_refuse(system, index, TA_REFUSED_INVALID, out);
        return TA_OK;
    }
    ta_result result = ta_interpreter_hold_block(system, actor);
    if (result != TA_OK) {
        return result;
    }
    actor->queued -= 1u;
    if ((envelope->source == TA_SOURCE_MAIL || envelope->source == TA_SOURCE_ERROR) && system->input_watch.v != NULL) {
        /* The host never saw this input, so it marks its trace parent here (tinyactors.h, ta_input_watch). */
        system->input_watch.v->mark(system->input_watch.self, actor->record.id, &envelope->trace);
    }
    ta_run_install(actor, envelope, NULL);
    envelope->state = TA_ENVELOPE_TAKEN;
    system->runner.slot = (ta_run_slot){ .actor = actor->record.id, .envelope = index, .occupied = true };
    ta_run_slot_open(system);
    *occupied = true;
    return TA_OK;
}

/*
 * ta_actor_transition starts an operator's event and transition in the slot:
 * see tinyactors.h. It is the admission test of ta_actor_deliver with the
 * checks of ADR 0082 added, in the order that makes the answer the host's
 * whole decision. The steps that can fail (the event's origin, the jump's
 * allocation, the envelope, the event block) come last and are undone in
 * reverse, so a refusal changes nothing.
 */
ta_result ta_actor_transition(ta_system *system, const ta_actor_transition_params *params) {
    if (system == NULL || params == NULL || params->event == NULL || params->transition == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_operator_transition *transition = params->transition;
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (!ta_run_event_valid(system, actor, params->event, params->time) ||
        !ta_interpreter_jump_valid(actor->definition, transition)) {
        return TA_ERR_INVALID;
    }
    if (actor->finished) {
        return TA_ERR_DONE;
    }
    if (system->runner.slot.occupied || actor->macrostep || actor->queued != 0 ||
        ta_actor_external_event(actor) != NULL) {
        return TA_ERR_BUSY;
    }
    /* An idle actor's configuration is the one the operator transition runs
     * from: it is the first step its macrostep takes (ADR 0082). */
    if (transition->source != TA_STATE_NONE && !ta_actor_state_active(actor, transition->source)) {
        return TA_ERR_INVALID;
    }
    ta_event accepted = *params->event;
    ta_result result = ta_actor_accept_origin(system, actor, &accepted);
    if (result != TA_OK) {
        return result;
    }
    uint32_t jump_bytes = 0;
    if (ta_operator_jump_bytes(transition->target_count, &jump_bytes) != TA_OK) {
        return TA_ERR_NOMEM; /* the count does not fit; nothing was allocated */
    }
    ta_operator_jump *jump = ta_mem_alloc(system->allocator, jump_bytes, _Alignof(ta_operator_jump));
    if (jump == NULL) {
        return TA_ERR_NOMEM;
    }
    jump->bytes = jump_bytes;
    jump->source = transition->source;
    jump->target_count = transition->target_count;
    ta_mem_copy(jump->targets, transition->targets, (size_t)transition->target_count * sizeof(ta_state_id));
    uint32_t index = ta_run_fill(system, actor, &(ta_run_input){ .event = &accepted,
                                                                 .trace = &params->trace,
                                                                 .time = params->time,
                                                                 .source = TA_SOURCE_TRANSITION,
                                                                 .room = 1u + system->envelopes.macrostep_reserve });
    if (index == 0) {
        /* A pool at its bound is a capacity condition, not an allocation failure. */
        result = system->envelopes.pool.used >= system->envelopes.bound ? TA_ERR_FULL : TA_ERR_NOMEM;
    } else {
        result = ta_interpreter_hold_block(system, actor);
    }
    if (result != TA_OK) {
        ta_envelope_release(&system->envelopes, index);
        ta_mem_free(system->allocator, jump,
                    &(ta_allocator_free_params){ .size = jump->bytes, .alignment = _Alignof(ta_operator_jump) });
        return result;
    }
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    envelope->input = ta_run_token(system);
    envelope->state = TA_ENVELOPE_TAKEN;
    if (params->input != NULL) {
        *params->input = envelope->input;
    }
    ta_run_install(actor, envelope, jump);
    system->runner.slot = (ta_run_slot){ .actor = actor->record.id, .envelope = index, .occupied = true };
    ta_run_slot_open(system);
    return TA_OK;
}

/*
 * ta_run_started_drained trims the cell pool when the queue has emptied and
 * the pool grew past TA_RUN_STARTED_KEEP cells, so a burst of spawns does not
 * keep its pages for ever. It never allocates.
 */
static void ta_run_started_drained(ta_runner *runner) {
    ta_pool_counts counts;
    ta_pool_counts_get(&runner->cells, &counts);
    if (counts.capacity > TA_RUN_STARTED_KEEP) {
        (void)ta_pool_trim(&runner->cells);
    }
}

/*
 * ta_run_take_started fills the slot from the oldest started session that
 * still has a macrostep to run. Entries for sessions that are gone, or whose
 * macrostep ended meanwhile (abandoned, or run by an earlier slot), are
 * skipped. Answers false when none is left. When the queue drains its pool is
 * trimmed if it grew past TA_RUN_STARTED_KEEP cells.
 */
static bool ta_run_take_started(ta_system *system) {
    ta_runner *runner = &system->runner;
    while (runner->started_first != 0) {
        uint32_t index = runner->started_first;
        ta_run_cell *cell = ta_pool_at(&runner->cells, index);
        ta_actor_id id = cell->id;
        runner->started_first = cell->next;
        if (runner->started_first == 0) {
            runner->started_last = 0;
        }
        cell->next = 0;
        ta_pool_give(&runner->cells, index);
        ta_actor *actor = ta_actor_lookup(system, id);
        if (actor == NULL || !actor->started) {
            continue;
        }
        actor->started = false;
        if (!actor->macrostep) {
            continue;
        }
        runner->slot = (ta_run_slot){ .actor = id, .occupied = true };
        ta_run_slot_open(system);
        if (runner->started_first == 0) {
            ta_run_started_drained(runner);
        }
        return true;
    }
    ta_run_started_drained(runner);
    return false;
}

/* ------------------------------------------------------------------------
 * Ending an input
 */

/*
 * ta_run_report_abandoned writes the COMPLETED notice of every input that
 * ended without ta_run (ta_actor_abandon, a session destroyed in its macrostep)
 * and releases its envelope. It stops, with the rest still listed, when the
 * array is full, and answers whether any are left.
 */
static bool ta_run_report_abandoned(ta_system *system, ta_run_notices *out) {
    ta_runner *runner = &system->runner;
    const ta_ilink *link = ta_envelopes_link(&system->envelopes);
    while (!ta_ilist_is_empty(&runner->abandoned)) {
        if (ta_run_room(out) < 1u) {
            return true;
        }
        uint32_t index = ta_ilist_pop_front(&runner->abandoned, link);
        const ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
        ta_run_notice_add(out, &(ta_run_notice){ .kind = TA_NOTICE_INPUT_COMPLETED,
                                                 .actor = envelope->target,
                                                 .input = envelope->input,
                                                 .source = (ta_input_source)envelope->source,
                                                 .status = TA_INPUT_ABANDONED });
        ta_envelope_release(&system->envelopes, index);
    }
    return false;
}

/*
 * ta_run_complete writes the input's COMPLETED notice (and FINISHED when the
 * session finished), frees the slot and disposes of the envelope: it is lent
 * to the notices when `lend` (a TAKEN notice borrowed its event), else
 * released now. The caller has checked there is room for two notices.
 */
static void ta_run_complete(ta_system *system, ta_input_status status, bool ignored, ta_run_notices *out,
                            bool lend) {
    ta_run_slot *slot = &system->runner.slot;
    uint32_t index = slot->envelope;
    uint64_t token = 0;
    ta_input_source source = TA_SOURCE_START;
    if (index != 0) {
        token = ta_envelope_at(&system->envelopes, index)->input;
        source = (ta_input_source)ta_envelope_at(&system->envelopes, index)->source;
    }
    ta_run_notice_add(out, &(ta_run_notice){ .kind = TA_NOTICE_INPUT_COMPLETED,
                                             .actor = slot->actor,
                                             .input = token,
                                             .source = source,
                                             .status = status,
                                             .ignored = ignored });
    if (status == TA_INPUT_DONE) {
        ta_run_notice_add(out, &(ta_run_notice){ .kind = TA_NOTICE_FINISHED, .actor = slot->actor, .input = token,
                                                 .source = source });
    }
    /* The macrostep's mail becomes visible now, all at once; an abandoned macrostep's never does. */
    if (status == TA_INPUT_RUNAWAY || status == TA_INPUT_ABANDONED) {
        ta_mail_macrostep_discarded(system);
    } else {
        ta_mail_macrostep_ended(system);
    }
    if (index != 0) {
        if (lend) {
            ta_ilist_push_back(&system->runner.lent, ta_envelopes_link(&system->envelopes), index);
        } else {
            ta_envelope_release(&system->envelopes, index);
        }
    }
    *slot = (ta_run_slot){0};
}

/* ta_run_free_lent releases the envelopes the last call's notices lent. */
static void ta_run_free_lent(ta_system *system) {
    uint32_t index = 0;
    while ((index = ta_ilist_pop_front(&system->runner.lent, ta_envelopes_link(&system->envelopes))) != 0) {
        ta_envelope_release(&system->envelopes, index);
    }
}

/*
 * ta_run_end_status reads how the slot's input ended from the interpreter's
 * answer. A macrostep that ran its microstep limit is abandoned here, as
 * ta_actor_abandon would (ADR 0082, ADR 0099 section 7): the session keeps the
 * configuration after its last completed microstep and the next input runs.
 *
 * The abandon allocates (it builds an interpreter on scratch memory), so it can
 * fail with TA_ERR_NOMEM. Then nothing has changed: the session is still in its
 * macrostep, stopped between two microsteps, and the input stays in the slot.
 * The result is the abandon's, *ended is false, and the caller stops ta_run;
 * the next call runs the session again, reaches the limit at once and retries
 * the abandon. (A bootstrap cannot be here: the limit is only checked once
 * the session is bootstrapped.)
 *
 * In practice the abandon rebuilds the interpreter in the scratch arena that
 * ta_interpreter_run_input has just sized for the same session, so it takes no
 * new memory and a test allocator cannot refuse it; the check is here so that
 * a future change to the arena cannot turn a refusal into a stuck session.
 */
static ta_result ta_run_end_status(ta_system *system, ta_actor *actor, ta_step_status step, ta_input_status *status) {
    switch (step) {
    case TA_STEP_DONE:
        *status = TA_INPUT_DONE;
        break;
    case TA_STEP_RUNAWAY: {
        ta_result result = ta_interpreter_abandon(system, actor);
        if (result != TA_OK) {
            return result;
        }
        *status = TA_INPUT_RUNAWAY;
        break;
    }
    case TA_STEP_PAUSED:
    case TA_STEP_STABLE:
    default:
        *status = TA_INPUT_STABLE;
        break;
    }
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * ta_run
 */

/* ta_run_check says whether `params` can be used at all. */
static bool ta_run_check(const ta_system *system, const ta_run_params *params) {
    return system != NULL && params != NULL && params->budget != 0 && params->status != NULL &&
           params->notices != NULL && params->notice_count != NULL && params->notice_capacity >= 3u;
}

/* ta_run_pending: see tinyactors.h. */
bool ta_run_pending(const ta_system *system) {
    const ta_runner *runner = &system->runner;
    return runner->slot.occupied || runner->started_first != 0 || !ta_ilist_is_empty(&system->envelopes.run) ||
           !ta_ilist_is_empty(&runner->abandoned);
}

ta_result ta_run(ta_system *system, const ta_run_params *params) {
    if (!ta_run_check(system, params)) {
        if (params != NULL && params->status != NULL) {
            *params->status = TA_RUN_IDLE;
        }
        if (params != NULL && params->notice_count != NULL) {
            *params->notice_count = 0;
        }
        return TA_ERR_INVALID;
    }
    ta_run_notices out = { .notices = params->notices, .capacity = params->notice_capacity };
    ta_runner *runner = &system->runner;
    ta_ilist *queue = &system->envelopes.run;
    uint32_t budget = params->budget;
    ta_result result = TA_OK;
    ta_run_status status = TA_RUN_IDLE;
    ta_run_free_lent(system);
    for (;;) {
        if (ta_run_report_abandoned(system, &out)) {
            status = TA_RUN_NOTICES_FULL;
            break;
        }
        if (!runner->slot.occupied) {
            if (ta_run_room(&out) < 3u) {
                status = TA_RUN_NOTICES_FULL;
                break;
            }
            if (runner->started_first == 0 && ta_ilist_is_empty(queue)) {
                status = TA_RUN_IDLE;
                break;
            }
            if (budget == 0) {
                status = TA_RUN_MORE;
                break;
            }
            /* Safe point: the pool grows to back the reserve before any input is chosen, so a failure changes nothing. */
            result = ta_run_cover(system);
            if (result != TA_OK) {
                status = TA_RUN_MORE;
                break;
            }
            if (ta_run_take_started(system)) {
                continue;
            }
            uint32_t index = ta_envelopes_run_pop(&system->envelopes);
            if (index == 0) {
                status = TA_RUN_IDLE; /* only stale started entries were left */
                break;
            }
            bool occupied = false;
            result = ta_run_take(system, index, &out, &occupied);
            if (result != TA_OK) {
                ta_ilist_push_front(queue, ta_envelopes_link(&system->envelopes), index);
                status = TA_RUN_MORE;
                break;
            }
            if (occupied && params->report_taken) {
                ta_run_notice_add(&out, &(ta_run_notice){
                                            .kind = TA_NOTICE_INPUT_TAKEN,
                                            .actor = runner->slot.actor,
                                            .input = ta_envelope_at(&system->envelopes, runner->slot.envelope)->input,
                                            .source = (ta_input_source)ta_envelope_at(&system->envelopes,
                                                                                      runner->slot.envelope)->source,
                                            .event = &ta_envelope_at(&system->envelopes, runner->slot.envelope)->event });
            }
            continue;
        }
        if (ta_run_room(&out) < 2u) {
            status = TA_RUN_NOTICES_FULL;
            break;
        }
        ta_actor *actor = ta_actor_lookup(system, runner->slot.actor);
        if (actor == NULL) {
            ta_run_end_slot(system); /* it went without telling ta_run_actor_gone */
            continue;
        }
        ta_input_result stepped;
        result = ta_interpreter_run_input(system, actor, &(ta_input_run){ .budget = budget,
                                                                        .action_budget = params->action_budget,
                                                                        .pause_at_start = params->pause_at_microstep_start },
                                          &stepped);
        if (result != TA_OK) {
            status = TA_RUN_PAUSED;
            break;
        }
        budget = stepped.steps >= budget ? 0 : budget - stepped.steps;
        if (stepped.status == TA_STEP_PAUSED) {
            status = TA_RUN_PAUSED;
            break;
        }
        bool lend = params->report_taken && runner->slot.envelope != 0;
        ta_input_status ended = TA_INPUT_STABLE;
        result = ta_run_end_status(system, actor, stepped.status, &ended);
        if (result != TA_OK) {
            status = TA_RUN_PAUSED; /* the input stays in the slot; the next call retries the abandon */
            break;
        }
        ta_run_complete(system, ended, stepped.ignored, &out, lend);
    }
    *params->status = status;
    *params->notice_count = out.count;
    return result;
}

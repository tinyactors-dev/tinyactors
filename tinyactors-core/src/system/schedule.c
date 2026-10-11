/* schedule.c — cooperative scheduler, mailbox delivery, inbound commands.
 *
 *   run(budget):  fire due timers; round-robin RUNNABLE actors with a per-actor
 *                 quantum until the budget is used, nothing is runnable, the
 *                 outbox is full, or a pool is exhausted.
 * An actor that exhausts its quantum stays RUNNABLE and goes to the back of
 * the run queue; no event is needed to wake it. */
#include "base/memory.h"
#include "system/system_internal.h"
#include "trace/record.h"

static void run_queue_push_front(ta_system *system, uint32_t index) {
    ta_actor *actor = ta_actor_at(system, index);
    actor->next = system->run_queue.head;
    system->run_queue.head = index;
    if (system->run_queue.tail == TA_NIL) system->run_queue.tail = index;
    actor->state = TA_ACTOR_RUNNABLE;
    system->runnable_count++;
}

/* Wake (FIFO) as many workspace waiters as there are free workspaces. */
static void wake_workspace_waiters(ta_system *system) {
    uint32_t n = ta_pool_free_count(&system->workspaces);
    while (n-- && !ta_index_queue_empty(&system->workspace_queue)) {
        uint32_t w = ta_index_queue_pop(&system->workspace_queue, &system->actors, 0);
        ta_actor_at(system, w)->state = TA_ACTOR_IDLE;
        ta_system_make_runnable(system, w);
    }
}

static uint32_t mailbox_pop(ta_system *system, ta_actor *actor) {
    uint32_t event = actor->mailbox_head;
    if (event == TA_NIL) return TA_NIL;
    actor->mailbox_head = ta_event_at(&system->core, event)->next;
    if (actor->mailbox_head == TA_NIL) actor->mailbox_tail = TA_NIL;
    actor->mailbox_length--;
    return event;
}

/* Take the next external event. Events of cancelled invocations were purged
 * when the invocation was cancelled; a completed invocation's events (incl.
 * done.invoke) are delivered even if the invoking state was exited since. */
static uint32_t next_external(ta_system *system, uint32_t index) {
    ta_core *core = &system->core;
    ta_actor *actor = ta_actor_at(system, index);
    ta_instance *instance = ta_actor_instance(system, actor);
    uint32_t event_index = mailbox_pop(system, actor);
    if (event_index == TA_NIL) return TA_NIL;
    ta_event *event = ta_event_at(core, event_index);
    if ((event->flags & TA_EVENT_FLAG_DONE) && (event->flags & TA_EVENT_FLAG_FROM_CHILD) &&
        ta_core_invoke_child(core, instance, event->invoke_slot) == event->origin) {
        uint32_t child = event->origin;
        if (child & TA_EXTERNAL_INVOKE_BIT) {
            uint32_t ri = ta_pool_resolve(&system->requests, child & ~TA_EXTERNAL_INVOKE_BIT);
            if (ri != TA_NIL) {
                ta_shared_string_release(
                    core, ((ta_request *)ta_pool_at(&system->requests, ri))->invokeid);
                ta_pool_free(&system->requests, ri);
            }
        }
        /* the invocation is complete (the event holds its own id ref) */
        ta_core_clear_invoke_child(core, instance, event->invoke_slot);
    }
    return event_index;
}

static uint32_t exhausted_pool(const ta_system *system) {
    if (ta_pool_free_count(&system->timers) == 0) return TA_POOL_TIMERS;
    return ta_pool_free_count(&system->events) <= system->core.reserve ? TA_POOL_EVENTS
                                                                       : TA_POOL_SLAB;
}

/* The LOG effect and record of a <log> (core->out.label, .text). */
static void step_log(ta_system *system, uint32_t index, ta_instance *instance) {
    ta_core *core = &system->core;
    ta_effect_record *effect = ta_system_effect_push(system, TA_EFFECT_LOG, index);
    uint32_t text = core->out.text.length
                        ? ta_shared_string_new(core, core->out.text.pointer, core->out.text.length)
                        : TA_NIL;
    if (effect) {
        effect->auxiliary = core->out.label;
        ta_shared_string_retain(core, effect->auxiliary);
        effect->event_name = text;
        ta_shared_string_retain(core, text);
        effect->trace = ta_core_step_trace(core, instance);
    }
    ta_trace_record *record = ta_core_trace_step(core, instance, TA_TRACE_LOG,
                                                 (uint32_t)(sizeof *record + sizeof(ta_trace_log)),
                                                 instance->microstep, (const uint32_t *)0);
    if (record) {
        ta_trace_log *log = (ta_trace_log *)ta_trace_record_body(record);
        log->label = core->out.label;
        ta_shared_string_retain(core, log->label);
        log->text = text;
        ta_shared_string_retain(core, text);
        ta_trace_record_commit(core->journal, record);
    }
    ta_shared_string_release(core, text);
}

/* Run one actor for up to `quantum` fuel. Returns a TA_RUN_* status. The fuel
 * clock at the start of its turn is `clock`; what the kernel and the system
 * write is stamped with it plus the fuel used so far. */
static uint32_t run_actor(ta_system *system, uint32_t index, uint32_t quantum, uint64_t clock,
                          uint32_t *used, uint32_t *detail) {
    ta_core *core = &system->core;
    uint32_t fuel = quantum, debt = 0;
    uint32_t status = TA_RUN_IDLE;
    ta_actor_at(system, index)->flags |= ACTOR_FLAG_TOUCHED;
    /* the chart's counts (tinyactors/stats.h) move once per turn: the
     * macrosteps begun are the instance's own counter's difference */
    const ta_instance *first = ta_actor_instance(system, ta_actor_at(system, index));
    ta_chart_stats *stats = &core->chart_stats[first->chart];
    uint64_t macrostep_first = first->macrostep, macrostep_last = macrostep_first;
    for (;;) {
        ta_actor *actor = ta_actor_at(system, index);
        ta_instance *instance = ta_actor_instance(system, actor);
        if (ta_system_effect_full(system)) {
            run_queue_push_front(system, index);
            status = TA_RUN_EFFECTS_FULL;
            break;
        }
        core->trace_clock = clock + (quantum - fuel) + debt;
        uint32_t rc = ta_core_step(core, instance, &fuel);
        macrostep_last = instance->macrostep;
        debt += core->overrun;
        core->trace_clock = clock + (quantum - fuel) + debt;
        int stop = 0;
        switch (rc) {
        case TA_STEP_SEND: {
            int sync = core->out.route == TA_ROUTE_EXTERNAL &&
                       core->out.arg < system->config.io_processor_count &&
                       (system->config.io_processors[core->out.arg].flags & TA_IO_PROCESSOR_SYNC) &&
                       !core->out.delay_ns;
            if (ta_system_route_send(system, index, &core->out) == TA_POOL_TIMERS)
                ta_core_raise_platform(core, instance, "error.execution", TA_NIL);
            if (sync) { /* let the host answer before the sender continues */
                run_queue_push_front(system, index);
                status = TA_RUN_EFFECTS;
                stop = 1;
            }
            break;
        }
        case TA_STEP_CANCEL:
            ta_system_cancel(system, index, core->out.sendid);
            break;
        case TA_STEP_INVOKE: {
            uint32_t pool = ta_system_invoke(system, index, &core->out);
            core->out.payload = 0;
            if (pool == TA_POOL_ACTORS || pool == TA_POOL_SLAB) {
                /* spawn parked; let the host grow the system, then retry */
                run_queue_push_front(system, index);
                *detail = pool;
                status = TA_RUN_EXHAUSTED;
                stop = 1;
            }
            break;
        }
        case TA_STEP_UNINVOKE:
            ta_system_uninvoke(system, index, &core->out);
            break;
        case TA_STEP_AUTOFORWARD:
            ta_system_autoforward(system, index, &core->out);
            core->out.event = TA_NIL;
            break;
        case TA_STEP_LOG:
            step_log(system, index, instance);
            break;
        case TA_STEP_STABLE: {
            uint32_t event = next_external(system, index);
            if (event == TA_NIL) {
                actor->state = TA_ACTOR_IDLE;
                stop = 1;
            } else {
                if (system->rule_points & TA_TRACE_AT_TAKE) /* the take rules decide again */
                    ta_system_trace_take(system, index, event);
                if (ta_core_deliver(core, instance, event) != TA_OK) ta_event_release(core, event);
            }
            break;
        }
        case TA_STEP_YIELD:
            actor->state = TA_ACTOR_IDLE;
            ta_system_make_runnable(system, index); /* back of the queue */
            stop = 1;
            break;
        case TA_STEP_NEED_WORKSPACE:
            actor->state = TA_ACTOR_WAIT_WORKSPACE;
            ta_index_queue_push(&system->workspace_queue, &system->actors, 0, index);
            stop = 1;
            break;
        case TA_STEP_DONE: {
            ta_payload payload = core->out.payload;
            core->out.payload = 0;
            actor->state = TA_ACTOR_DONE;
            ta_system_done(system, index, payload, core->out.final_state);
            stop = 1;
            break;
        }
        case TA_STEP_EXHAUSTED:
            run_queue_push_front(system, index);
            *detail = exhausted_pool(system);
            status = TA_RUN_EXHAUSTED;
            stop = 1;
            break;
        default: { /* FAULT or unknown: report and reclaim */
            ta_effect_record *effect = ta_system_effect_push(system, TA_EFFECT_ERROR, index);
            if (effect) effect->flags = core->out.fault;
            stats->faults++;
            actor->state = TA_ACTOR_DONE;
            ta_system_reclaim(system, index, RECLAIM_ENDED);
            stop = 1;
            break;
        }
        }
        wake_workspace_waiters(system);
        if (stop) break;
        if (fuel == 0) {
            actor = ta_actor_at(system, index);
            actor->state = TA_ACTOR_IDLE;
            ta_system_make_runnable(system, index);
            break;
        }
    }
    *used = quantum - fuel + debt;
    stats->turns++;
    stats->fuel += *used;
    stats->macrosteps += macrostep_last - macrostep_first;
    return status;
}

/* Outside a run, records are stamped with the system's fuel clock. */
static void core_trace_clock_sync(ta_system *system) {
    system->core.trace_clock = system->fuel_clock;
    system->core.trace_now = system->now;
}

ta_run ta_system_run(ta_system *system, uint32_t budget) {
    ta_run result;
    ta_memset(&result, 0, sizeof result);
    result.next_deadline = UINT64_MAX;
    if (system->in_call) {
        result.status = TA_RUN_FAULT;
        result.detail = (uint32_t)TA_ERROR_STATE;
        return result;
    }
    system->in_call = 1;
    /* what this run writes is stamped after the clock its caller noted
     * before it (docs/adr/0006, section 9) */
    system->fuel_clock++;
    system->core.trace_clock = system->fuel_clock;
    ta_system_fire_timers(system);
    uint32_t used = 0, status = TA_RUN_IDLE;
    uint32_t pending = ta_system_retry_pending(system);
    if (pending != TA_POOL_NONE) {
        system->in_call = 0;
        result.status = TA_RUN_EXHAUSTED;
        result.detail = pending;
        result.effect_count = system->effects_length - system->effects_head;
        result.next_deadline = ta_system_next_deadline(system);
        return result;
    }
    while (used < budget) {
        if (ta_system_effect_full(system)) {
            status = TA_RUN_EFFECTS_FULL;
            break;
        }
        uint32_t index = ta_index_queue_pop(&system->run_queue, &system->actors, 0);
        if (index == TA_NIL) break;
        system->runnable_count--;
        uint32_t q =
            system->config.quantum < budget - used ? system->config.quantum : budget - used;
        uint32_t u = 0;
        status = run_actor(system, index, q, system->fuel_clock + used, &u, &result.detail);
        used += u ? u : 1;
        system->stats_turns++;
        if (status != TA_RUN_IDLE) break;
    }
    wake_workspace_waiters(system);
    ta_system_emit_wakes(system);
    if (status == TA_RUN_IDLE && (!ta_index_queue_empty(&system->run_queue)))
        status = TA_RUN_YIELDED;
    if (status == TA_RUN_IDLE && !ta_index_queue_empty(&system->wake_queue))
        status = TA_RUN_EFFECTS_FULL;
    system->in_call = 0;
    system->stats_fuel += used;
    system->fuel_clock += used;
    core_trace_clock_sync(system);
    result.status = status;
    result.used = used;
    result.effect_count = system->effects_length - system->effects_head;
    result.next_deadline = ta_system_next_deadline(system);
    return result;
}

void ta_system_advance_time(ta_system *system, ta_time now) {
    if (system->in_call) return;
    if (now > system->now) system->now = now;
    core_trace_clock_sync(system);
    ta_system_fire_timers(system);
}

ta_time ta_system_next_deadline(const ta_system *system) {
    const ta_heap_node *top = ta_heap_top(&system->heap);
    return top ? top->due : UINT64_MAX;
}

/* ---- inbound events -------------------------------------------------------------------- */

static void release_payload(ta_system *system, uint32_t chart, ta_payload payload) {
    const ta_datamodel_binding *binding = ta_core_datamodel(&system->core, chart);
    if (payload && binding->vtable->release) binding->vtable->release(binding->datamodel, payload);
}

static uint32_t opt_str_plain(ta_core *core, ta_string_view v) {
    return v.length ? ta_shared_string_new(core, v.pointer, v.length) : TA_NIL;
}

static uint32_t opt_str(ta_core *core, ta_string_view v, int *oom) {
    if (!v.length) return TA_NIL;
    uint32_t r = ta_shared_string_new(core, v.pointer, v.length);
    if (r == TA_NIL) *oom = 1;
    return r;
}

/* Also during a run, from a datamodel (system.h): the event takes the path
 * of an internal <send>, the target's mailbox, read at its next stable point. */
ta_status ta_system_send(ta_system *system, const ta_event_in *event_in) {
    ta_core *core = &system->core;
    uint32_t index = ta_system_resolve_actor(system, event_in->target);
    if (index == TA_NIL) return TA_ERROR_STALE;
    ta_actor *actor = ta_actor_at(system, index);
    if (actor->state == TA_ACTOR_DONE) return TA_ERROR_STALE;
    /* the macrostep the event starts runs under its traceparent, or in a new trace */
    ta_string_view traceparent = {(const char *)0, 0};
    if (event_in->size >= offsetof(ta_event_in, traceparent) + sizeof event_in->traceparent)
        traceparent = event_in->traceparent;
    uint32_t trace_flags =
        event_in->size >= offsetof(ta_event_in, trace_flags) + sizeof event_in->trace_flags
            ? event_in->trace_flags
            : 0;
    ta_trace_ask ask = {TA_TRACE_AT_START, TA_TRACE_FROM_HOST, index, ta_actor_chart(system, actor),
                        TA_NIL};
    if ((event_in->flags & TA_EVENT_IN_ERROR_COMMUNICATION) &&
        actor->state == TA_ACTOR_HIBERNATED) {
        /* parked in the mailbox; moved to the internal queue when woken */
        uint32_t event_index = ta_event_new(core, 1);
        if (event_index == TA_NIL) return TA_ERROR_NO_MEMORY; /* the payload stays the caller's */
        ta_event *event = ta_event_at(core, event_index);
        event->type = TA_EVENT_TYPE_PLATFORM;
        event->flags = TA_EVENT_FLAG_RAISE_ERROR_COMMUNICATION;
        event->sendid = opt_str_plain(core, event_in->sendid);
        ta_trace_context value;
        ask.event = event_index;
        event->trace = ta_system_start_trace(system, traceparent, trace_flags, &ask, &value);
        release_payload(system, ta_actor_chart(system, actor), event_in->data);
        ta_system_mailbox_push(system, index, event_index);
        return TA_OK;
    }
    if (event_in->flags & TA_EVENT_IN_ERROR_COMMUNICATION) { /* SCXML: error.communication goes to
                                                          the internal queue */
        uint32_t sid = opt_str_plain(core, event_in->sendid);
        uint32_t raised = ta_core_raise_platform(core, ta_actor_instance(system, actor),
                                                 "error.communication", sid);
        if (raised != TA_NIL && ta_event_at(core, raised)->trace == TA_NIL) {
            ta_trace_context value;
            ask.event = raised;
            ta_event_at(core, raised)->trace =
                ta_system_start_trace(system, traceparent, trace_flags, &ask, &value);
        }
        ta_shared_string_release(core, sid);
        release_payload(system, ta_actor_chart(system, actor), event_in->data);
        ta_system_make_runnable(system, index);
        return TA_OK;
    }
    uint32_t event_index = ta_event_new(core, 0);
    if (event_index == TA_NIL)
        return TA_ERROR_NO_MEMORY; /* the payload stays the caller's: grow, retry */
    ta_event *event = ta_event_at(core, event_index);
    event->type = TA_EVENT_TYPE_EXTERNAL;
    event->payload = event_in->data;
    event->datamodel = core->chart_datamodel[ta_actor_chart(system, actor)];
    int oom = 0;
    ta_request *request = (ta_request *)0;
    if (event_in->flags & (TA_EVENT_IN_FROM_INVOKE | TA_EVENT_IN_INVOKE_DONE)) {
        uint32_t ri = ta_pool_resolve(&system->requests, event_in->token);
        request = ri == TA_NIL ? (ta_request *)0 : (ta_request *)ta_pool_at(&system->requests, ri);
        if (!request || request->kind != REQUEST_EXTERNAL_INVOKE ||
            request->actor != event_in->target) {
            event->payload = 0; /* the caller's */
            ta_event_release(core, event_index);
            return TA_ERROR_STALE;
        }
        event->invokeid = request->invokeid;
        ta_shared_string_retain(core, event->invokeid);
        event->invoke_slot = (uint16_t)request->slot;
        event->origin = event_in->token | TA_EXTERNAL_INVOKE_BIT;
        event->flags = TA_EVENT_FLAG_FROM_CHILD | TA_EVENT_FLAG_FROM_EXTERNAL_INVOKE;
    }
    if (event_in->flags & TA_EVENT_IN_INVOKE_DONE) {
        ta_string_view id = ta_shared_string_get(core, request->invokeid);
        char *buffer = core->scratch;
        uint32_t n = 0;
        const char *pre = "done.invoke.";
        while (pre[n]) {
            buffer[n] = pre[n];
            n++;
        }
        for (uint32_t i = 0; i < id.length && n < sizeof core->scratch; i++)
            buffer[n++] = id.pointer[i];
        if (ta_event_set_name(core, event, buffer, n) != TA_OK) oom = 1;
        event->flags |= TA_EVENT_FLAG_DONE;
    } else if (ta_event_set_name(core, event, event_in->name.pointer ? event_in->name.pointer : "",
                                 event_in->name.length) != TA_OK) {
        oom = 1;
    }
    event->sendid = opt_str(core, event_in->sendid, &oom);
    event->origin_string = opt_str(core, event_in->origin, &oom);
    event->origintype = opt_str(core, event_in->origintype, &oom);
    if (event_in->size >= sizeof *event_in) event->raw = opt_str(core, event_in->raw, &oom);
    if (oom) {
        event->payload = 0; /* the caller's: grow, retry */
        ta_event_release(core, event_index);
        return TA_ERROR_NO_MEMORY;
    }
    ta_trace_context value;
    ask.event = event_index;
    event->trace = ta_system_start_trace(system, traceparent, trace_flags, &ask, &value);
    ta_system_mailbox_push(system, index, event_index);
    return TA_OK;
}

uint32_t ta_system_send_many(ta_system *system, const ta_event_in *events, uint32_t n) {
    uint32_t i = 0;
    while (i < n && ta_system_send(system, &events[i]) == TA_OK) /* an accepted prefix */
        i++;
    return i;
}

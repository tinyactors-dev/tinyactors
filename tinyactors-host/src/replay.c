#include "tinyactors_host/replay.h"

#include "memory.h"

#include "tinyactors_host/adapter_io.h"
#include "tinyactors_host/allocator.h"
#include "tinyactors_host/events.h"
#include "tinyactors_host/hooks.h"
#include "tinyactors_host/inputs.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"
#include "tinyactors_host/wal.h"
#include "allocator_private.h"
#include "inputs_private.h"
#include "scheduler_private.h"
#include "system_private.h"

/* A message the log shows accepted and not yet delivered, owned. */
typedef struct {
    uint64_t id;
    ta_event event;
    uint8_t report; /* tah_report_kind */
    bool ends;
} tah_replay_message;

struct tah_replay {
    tah_system *system;
    ta_allocator allocator;
    tah_vec bytes;      /* uint8_t: pushed and not yet read, from `start` */
    uint32_t start;
    tah_vec owed;       /* tah_replay_message */
    uint64_t covered;   /* records up to this seq are in the checkpoint the replay started from */
    uint64_t last_seq;
    bool has_time;
    ta_time last_time;
    bool closed;
};

tah_replay *tah_replay_open(tah_system *system) {
    ta_allocator allocator = tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER);
    tah_replay *replay = tah_alloc(allocator, sizeof *replay, _Alignof(tah_replay));
    if (replay == NULL) {
        return NULL;
    }
    *replay = (tah_replay){ .system = system, .allocator = allocator };
    tah_vec_init(&replay->bytes, allocator, TAH_LAYOUT(uint8_t));
    tah_vec_init(&replay->owed, allocator, TAH_LAYOUT(tah_replay_message));
    return replay;
}

static void tah_replay_release_owed(tah_replay *replay) {
    bool alive = !tah_system_closed(replay->system);
    for (uint32_t index = 0; index < replay->owed.count; index += 1u) {
        tah_replay_message *message = tah_vec_at(&replay->owed, index);
        if (alive) {
            ta_event_release(&message->event);
        }
    }
    tah_vec_clear(&replay->owed);
}

void tah_replay_close(tah_replay *replay) {
    if (replay == NULL || replay->closed) {
        return;
    }
    replay->closed = true;
    tah_replay_release_owed(replay);
    tah_vec_destroy(&replay->owed);
    tah_vec_destroy(&replay->bytes);
}

void tah_replay_free(tah_replay *replay) {
    if (replay == NULL) {
        return;
    }
    tah_replay_close(replay);
    tah_free(replay->allocator, &(ta_allocator_free_params){ .ptr = replay, .size = sizeof *replay,
                                                             .alignment = _Alignof(tah_replay) });
}

bool tah_replay_closed(const tah_replay *replay) {
    return replay->closed;
}

tah_system *tah_replay_system(const tah_replay *replay) {
    return replay->system;
}

ta_result tah_replay_push(tah_replay *replay, const uint8_t *bytes, size_t length) {
    if (replay->closed) {
        return TA_ERR_INVALID;
    }
    /* What was read goes first: the replay holds one record and a piece. */
    if (replay->start != 0) {
        uint8_t *items = replay->bytes.items;
        uint32_t left = replay->bytes.count - replay->start;
        for (uint32_t index = 0; index < left; index += 1u) {
            items[index] = items[replay->start + index];
        }
        replay->bytes.count = left;
        replay->start = 0;
    }
    if (length > (size_t)(UINT32_MAX - replay->bytes.count)) {
        return TA_ERR_NOMEM;
    }
    ta_result result = tah_vec_reserve(&replay->bytes, replay->bytes.count + (uint32_t)length);
    if (result != TA_OK) {
        return result;
    }
    tah_mem_copy((uint8_t *)replay->bytes.items + replay->bytes.count, bytes, length);
    replay->bytes.count += (uint32_t)length;
    return TA_OK;
}

size_t tah_replay_buffered(const tah_replay *replay) {
    return replay->closed ? 0 : (size_t)(replay->bytes.count - replay->start);
}

bool tah_replay_last_time(const tah_replay *replay, ta_time *out) {
    if (replay->has_time) {
        *out = replay->last_time;
    }
    return replay->has_time;
}

/* The next whole record (or checkpoint unit), or 0 bytes when it is not all there. */
static size_t tah_replay_next_span(const tah_replay *replay, bool unit) {
    const uint8_t *at = (const uint8_t *)replay->bytes.items + replay->start;
    return tah_wal_span(at, replay->bytes.count - replay->start, unit);
}

static const uint8_t *tah_replay_at(const tah_replay *replay) {
    return (const uint8_t *)replay->bytes.items + replay->start;
}

/* ------------------------------------------------------------------------
 * Messages
 */

/*
 * The origin an ACCEPTED record names: an actor, an I/O processor's message
 * origin, or none. A message from an invocation gets its origin from core
 * when it is delivered (core ADR 0068).
 */
static ta_result tah_replay_origin(tah_system *system, const tah_wal_view *view, ta_origin *out) {
    *out = (ta_origin){ .kind = TA_ORIGIN_NONE };
    ta_str origin = { 0 };
    ta_str type = { 0 };
    ta_str invoke = { 0 };
    if (tah_wal_text(view, 6, &invoke) || !tah_wal_text(view, 4, &origin) || !tah_wal_text(view, 5, &type)) {
        return TA_OK;
    }
    ta_str scxml = { 0 };
    (void)ta_system_atom_text(system->core, ta_system_scxml_io_type(system->core), &scxml);
    if (type.len == scxml.len && tah_mem_equal(type.ptr, scxml.ptr, type.len)) {
        ta_scxml_target target = { 0 };
        ta_result result = ta_scxml_target_parse(system->core, origin, &target);
        if (result == TA_ERR_NOMEM) {
            return result;
        }
        if (result == TA_OK && target.kind == TA_SCXML_TARGET_ACTOR) {
            *out = (ta_origin){ .kind = TA_ORIGIN_ACTOR, .as.actor = target.as.actor };
            return TA_OK;
        }
        if (result == TA_OK) {
            ta_scxml_target_release(&target);
        }
        return TA_OK;
    }
    ta_atom atom = 0;
    if (ta_system_intern_atom(system->core, type, &atom) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    uint32_t processor = tah_io_index_of(system, atom);
    if (processor == 0) {
        return TA_OK; /* no such processor here: the message arrives without an origin */
    }
    return ta_io_origin_make(system->core, &(ta_io_origin_make_params){
                                               .processor = processor,
                                               .origin_class = TA_IO_ORIGIN_MESSAGE,
                                               .uri = origin,
                                               .out = out,
                                           });
}

/* The message an ACCEPTED record holds, owned by the caller. */
static ta_result tah_replay_message_of(tah_system *system, const tah_wal_view *view, tah_replay_message *out) {
    *out = (tah_replay_message){ .id = tah_wal_number(view, 0),
                                 .event = { .invoke_id = TA_INVOKE_ID_NONE },
                                 .report = (uint8_t)tah_wal_number(view, 7),
                                 .ends = tah_wal_number(view, 9) != 0 };
    uint64_t type = tah_wal_number(view, 1);
    ta_str name = { 0 };
    ta_str text = { 0 };
    /* A message has a name; an invocation's report is named by core when it is reported. */
    bool named = tah_wal_text(view, 2, &name) && name.len != 0;
    if (type > (uint64_t)TA_EVENT_PLATFORM || out->report > TAH_REPORT_FAILED || named == (out->report != TAH_REPORT_NONE)) {
        return TA_ERR_CORRUPT;
    }
    out->event.type = (ta_event_type)type;
    ta_result result = named ? tah_event_name_intern(system->core, name, &out->event.name) : TA_OK;
    if (result == TA_ERR_INVALID) {
        result = TA_ERR_CORRUPT;
    }
    if (result == TA_OK && tah_wal_text(view, 3, &text)) {
        result = ta_send_id_intern(system->core, text, &out->event.send_id);
    }
    if (result == TA_OK && tah_wal_text(view, 6, &text)) {
        result = ta_system_intern_invoke_id(system->core, text, &out->event.invoke_id);
    }
    if (result == TA_OK) {
        result = tah_replay_origin(system, view, &out->event.origin);
    }
    if (result == TA_OK && view->value_count != 0) {
        result = tah_wal_read_value(view, 0, system->inputs.datamodel, &out->event.data);
    }
    if (result != TA_OK) {
        ta_event_release(&out->event);
        out->event = (ta_event){ .invoke_id = TA_INVOKE_ID_NONE };
    }
    return result;
}

static int64_t tah_replay_owed_index(const tah_replay *replay, uint64_t id) {
    for (uint32_t index = 0; index < replay->owed.count; index += 1u) {
        if (((const tah_replay_message *)tah_vec_at(&replay->owed, index))->id == id) {
            return index;
        }
    }
    return -1;
}

/* Takes message `index` out of the owed list, into `out`. */
static void tah_replay_take(tah_replay *replay, uint32_t index, tah_replay_message *out) {
    tah_replay_message *items = replay->owed.items;
    *out = items[index];
    for (uint32_t at = index + 1u; at < replay->owed.count; at += 1u) {
        items[at - 1u] = items[at];
    }
    replay->owed.count -= 1u;
}

/* Holds the message an ACCEPTED record names, once (a checkpoint may repeat it). */
static ta_result tah_replay_hold(tah_replay *replay, const tah_wal_view *view) {
    if (tah_replay_owed_index(replay, tah_wal_number(view, 0)) >= 0) {
        return TA_OK;
    }
    tah_replay_message message;
    ta_result result = tah_replay_message_of(replay->system, view, &message);
    if (result != TA_OK) {
        return result;
    }
    result = tah_vec_push(&replay->owed, &message);
    if (result != TA_OK) {
        ta_event_release(&message.event);
    }
    return result;
}

static void tah_replay_forget(tah_replay *replay, uint64_t id) {
    int64_t index = tah_replay_owed_index(replay, id);
    if (index >= 0) {
        tah_replay_message message;
        tah_replay_take(replay, (uint32_t)index, &message);
        ta_event_release(&message.event);
    }
}

/* ------------------------------------------------------------------------
 * Applying records
 */

/* Resumes a replaying actor until it is between macrosteps; `done` when it finished. */
static ta_result tah_replay_resume(tah_system *system, ta_actor_id actor, bool unlimited, bool *ignored, bool *done) {
    uint32_t budget = system->budgets.microsteps != 0 ? system->budgets.microsteps : 64u;
    for (;;) {
        ta_run_status status = TA_RUN_STABLE;
        bool took_nothing = false;
        system->inside_core = true;
        ta_result result = ta_actor_resume(system->core, &(ta_actor_resume_params){
                                                             .actor = actor,
                                                             .budget = budget,
                                                             .status = &status,
                                                             .action_budget = system->budgets.actions,
                                                             .no_microstep_limit = unlimited,
                                                             .ignored = &took_nothing,
                                                         });
        system->inside_core = false;
        if (result != TA_OK) {
            return result;
        }
        if (status == TA_RUN_PAUSED) {
            continue;
        }
        *ignored = took_nothing;
        *done = status == TA_RUN_DONE;
        return TA_OK;
    }
}

/* Whether `invoke` is one of the actor's active invocations. */
static bool tah_replay_invoke_active(tah_system *system, ta_actor_id actor, ta_invoke_id invoke) {
    ta_actor_inspection view;
    if (invoke == TA_INVOKE_ID_NONE || ta_actor_inspect(system->core, actor, &view) != TA_OK) {
        return false;
    }
    for (uint32_t index = 0; index < view.invoke_count; index += 1u) {
        if (view.invokes[index].id == invoke) {
            return true;
        }
    }
    return false;
}

/* A JUMP's operator transition, by its state IDs; `targets` has room for its count. */
static ta_result tah_replay_jump(tah_system *system, ta_actor_id actor, const tah_wal_view *view,
                                 ta_operator_transition *out, ta_state_id *targets) {
    ta_actor_inspection actor_view;
    if (ta_actor_inspect(system->core, actor, &actor_view) != TA_OK) {
        return TA_ERR_NOT_FOUND;
    }
    ta_str text = { 0 };
    *out = (ta_operator_transition){ .source = TA_STATE_NONE, .targets = targets,
                                     .target_count = (uint32_t)tah_wal_number(view, 2) };
    if (tah_wal_text(view, 1, &text) &&
        ta_definition_find_state(system->core, &(ta_definition_find_state_params){
                                                   .definition = actor_view.definition_id, .id = text,
                                                   .out = &out->source }) != TA_OK) {
        return TA_ERR_NOT_FOUND;
    }
    for (uint32_t index = 0; index < out->target_count; index += 1u) {
        if (!tah_wal_text(view, 3u + index, &text) ||
            ta_definition_find_state(system->core, &(ta_definition_find_state_params){
                                                       .definition = actor_view.definition_id, .id = text,
                                                       .out = &targets[index] }) != TA_OK) {
            return TA_ERR_NOT_FOUND;
        }
    }
    return TA_OK;
}

/* DELIVERED or JUMP: the held message, delivered (or reported) at the logged time. */
static ta_result tah_replay_deliver(tah_replay *replay, ta_actor_id actor, const tah_wal_view *view,
                                    tah_replay_message *message) {
    tah_system *system = replay->system;
    if (message->report != TAH_REPORT_NONE) {
        if (!tah_replay_invoke_active(system, actor, message->event.invoke_id)) {
            return TA_ERR_NOT_FOUND; /* the invocation it reports on is not active in the replay */
        }
        ta_str reason = message->report == TAH_REPORT_QUARANTINED ? (ta_str){ .ptr = "quarantined", .len = 11 }
                        : message->report == TAH_REPORT_FAILED  ? (ta_str){ .ptr = "failed", .len = 6 }
                                                                : (ta_str){ .ptr = "unavailable", .len = 11 };
        ta_invoke_error error = { .reason = reason,
                                  .data = message->event.data.v != NULL ? &message->event.data : NULL,
                                  .ends = message->ends };
        system->inside_core = true;
        ta_result result = ta_actor_report_invoke(system->core, &(ta_invoke_report_params){
                                                                    .actor = actor,
                                                                    .invoke_id = message->event.invoke_id,
                                                                    .kind = TA_INVOKE_REPORT_ERROR,
                                                                    .time = view->time,
                                                                    .as.error = &error,
                                                                });
        system->inside_core = false;
        return result;
    }
    if (message->event.invoke_id != TA_INVOKE_ID_NONE &&
        !tah_replay_invoke_active(system, actor, message->event.invoke_id)) {
        /* The invocation it came from is not active in the replay (it was
         * never started, or it ended): the event arrives without it. */
        message->event.invoke_id = TA_INVOKE_ID_NONE;
    }
    ta_operator_transition transition;
    ta_state_id *targets = NULL;
    size_t target_bytes = 0;
    ta_result result = TA_OK;
    if (view->kind == TAH_WAL_JUMP) {
        target_bytes = (size_t)tah_wal_number(view, 2) * sizeof(ta_state_id) + 1u;
        targets = tah_alloc(replay->allocator, target_bytes, _Alignof(ta_state_id));
        result = targets == NULL ? TA_ERR_NOMEM : tah_replay_jump(system, actor, view, &transition, targets);
    }
    if (result == TA_OK) {
        system->inside_core = true;
        result = ta_actor_deliver(system->core, &(ta_actor_deliver_params){
                                                    .actor = actor,
                                                    .event = &message->event,
                                                    .transition = view->kind == TAH_WAL_JUMP ? &transition : NULL,
                                                    .time = view->time,
                                                });
        system->inside_core = false;
    }
    if (targets != NULL) {
        tah_free(replay->allocator, &(ta_allocator_free_params){ .ptr = targets, .size = target_bytes,
                                                                 .alignment = _Alignof(ta_state_id) });
    }
    return result;
}

/* An event's dotted name, into `buffer`; empty when it does not fit. */
static ta_str tah_replay_name(tah_system *system, ta_event_name_id name, char *buffer, uint32_t capacity) {
    uint32_t needed = 0;
    if (ta_event_name_format(system->core, &(ta_event_name_format_params){ .name = name, .buffer = buffer,
                                                                           .capacity = capacity,
                                                                           .needed = &needed }) != TA_OK) {
        return (ta_str){ 0 };
    }
    return (ta_str){ .ptr = buffer, .len = needed - 1u };
}

typedef struct {
    tah_replay *replay;
    const tah_replay_run_params *params;
    const tah_wal_view *view;
} tah_replay_step;

/* Applies one record; `stop` is set to the state that ends the window. */
static ta_result tah_replay_apply(const tah_replay_step *step, bool *stop, tah_replay_state *state) {
    tah_replay *replay = step->replay;
    tah_system *system = replay->system;
    const tah_wal_view *view = step->view;
    ta_actor_id actor = step->params->actor;
    *stop = false;
    switch (view->kind) {
    case TAH_WAL_ACCEPTED:
        return tah_replay_hold(replay, view);
    case TAH_WAL_DROPPED:
        tah_replay_forget(replay, tah_wal_number(view, 0));
        return TA_OK;
    case TAH_WAL_FIRED:
    case TAH_WAL_EVICTED:
        return TA_OK; /* time only: the next input's, or the publish's, moves the clock */
    case TAH_WAL_FINISHED:
        *stop = true;
        *state = TAH_REPLAY_FINISHED;
        return TA_OK;
    case TAH_WAL_DESTROYED:
        *stop = true;
        *state = TAH_REPLAY_ENDED;
        return TA_OK;
    case TAH_WAL_ABANDONED: {
        system->inside_core = true;
        ta_result result = ta_actor_abandon(system->core, actor);
        system->inside_core = false;
        return result;
    }
    case TAH_WAL_WITHDRAWN: {
        ta_str text = { 0 };
        ta_send_id send = 0;
        if (!tah_wal_text(view, 0, &text)) {
            return TA_ERR_CORRUPT;
        }
        ta_result result = ta_send_id_intern(system->core, text, &send);
        if (result == TA_OK) {
            system->inside_core = true;
            result = ta_actor_withdraw_send(system->core, actor, send);
            system->inside_core = false;
        }
        /* Gone already (its due time passed with an input) is what withdrawing asked for. */
        return result == TA_ERR_NOT_FOUND || result == TA_ERR_DONE ? TA_OK : result;
    }
    case TAH_WAL_RELEASED:
    case TAH_WAL_DELIVERED:
    case TAH_WAL_JUMP:
        break;
    default:
        return TA_ERR_CORRUPT; /* a life's first record, or a kind this driver does not apply */
    }
    bool unlimited = view->kind == TAH_WAL_RELEASED;
    tah_replay_message message = { .event = { .invoke_id = TA_INVOKE_ID_NONE } };
    bool delivered = false;
    if (!unlimited) {
        int64_t index = tah_replay_owed_index(replay, tah_wal_number(view, 0));
        if (index < 0) {
            return TA_ERR_CORRUPT; /* a delivery of a message the log never accepted */
        }
        tah_replay_take(replay, (uint32_t)index, &message);
        ta_result result = tah_replay_deliver(replay, actor, view, &message);
        if (result != TA_OK) {
            ta_event_release(&message.event);
            return result;
        }
        delivered = true;
    }
    bool ignored = false;
    bool done = false;
    ta_result result = tah_replay_resume(system, actor, unlimited, &ignored, &done);
    if (result == TA_ERR_RUNAWAY) {
        result = TA_OK; /* stopped as it was live: the next record abandons or releases it */
    } else if (result == TA_OK && ignored && delivered && message.report == TAH_REPORT_NONE &&
               step->params->ignored != NULL) {
        char buffer[256];
        ta_str name = tah_replay_name(system, message.event.name, buffer, sizeof buffer);
        step->params->ignored(step->params->context, name, view->time);
    }
    ta_event_release(&message.event);
    if (result == TA_OK && done) {
        *stop = true;
        *state = TAH_REPLAY_FINISHED;
    }
    return result;
}

static bool tah_replay_between_kind(tah_wal_kind kind) {
    return kind == TAH_WAL_ACCEPTED || kind == TAH_WAL_DROPPED;
}

ta_result tah_replay_run(tah_replay *replay, const tah_replay_run_params *params) {
    if (replay->closed) {
        return TA_ERR_INVALID;
    }
    *params->state = TAH_REPLAY_MORE;
    *params->records = 0;
    while (*params->records < params->most) {
        size_t span = tah_replay_next_span(replay, false);
        if (span == 0) {
            *params->state = TAH_REPLAY_NEED;
            return TA_OK;
        }
        const uint8_t *at = tah_replay_at(replay);
        tah_wal_view view;
        ta_result result = tah_wal_read(at, replay->bytes.count - replay->start, &view);
        if (result != TA_OK) {
            return result;
        }
        if (params->between && !tah_replay_between_kind(view.kind)) {
            *params->state = TAH_REPLAY_LIFE;
            return TA_OK;
        }
        if (view.kind == TAH_WAL_CHECKPOINT) {
            /* A checkpoint inside the log: the replay has that state
             * already, and the messages it owes (kernel ADR 0002 section 7). */
            size_t unit = tah_replay_next_span(replay, true);
            if (unit == 0) {
                *params->state = TAH_REPLAY_NEED;
                return TA_OK;
            }
            replay->start += (uint32_t)unit;
            *params->records += 1u;
            continue;
        }
        bool covered = view.seq != 0 && view.seq <= replay->covered;
        bool stop = false;
        tah_replay_state state = TAH_REPLAY_MORE;
        if (!covered) {
            result = tah_replay_apply(&(tah_replay_step){ .replay = replay, .params = params, .view = &view }, &stop,
                                      &state);
            if (result != TA_OK) {
                return result;
            }
        }
        replay->start += (uint32_t)view.span;
        /* The latest time, not the last record's: a delivery's record carries
         * the time it was delivered, and is written when the macrostep it
         * started ends, after records of later times (a firing meanwhile;
         * kernel ADR 0004). */
        if (!replay->has_time || view.time > replay->last_time) {
            replay->last_time = view.time;
        }
        replay->has_time = true;
        if (view.seq > replay->last_seq) {
            replay->last_seq = view.seq;
        }
        *params->records += 1u;
        if (stop) {
            *params->state = state;
            return TA_OK;
        }
    }
    return TA_OK;
}

void tah_replay_finish(tah_replay *replay, ta_actor_id actor) {
    tah_sched_entry *entry = tah_sched_lookup(&replay->system->sched, actor);
    if (entry != NULL) {
        entry->flags &= (uint8_t)~TAH_SCHED_REPLAYING;
    }
    tah_inputs_set_actor_seq(&replay->system->inputs, actor, replay->last_seq);
}

ta_result tah_replay_redeliver(tah_replay *replay, ta_actor_id actor, uint32_t *count) {
    tah_system *system = replay->system;
    *count = 0;
    ta_result failure = TA_OK;
    for (uint32_t index = 0; index < replay->owed.count; index += 1u) {
        tah_replay_message *owed = tah_vec_at(&replay->owed, index);
        /* A message this system accepted is still in memory (waiting for
         * this very load, say) and reaches the actor from there. */
        if (failure != TA_OK || tah_inputs_seq_is_ours(&system->inputs, owed->id)) {
            ta_event_release(&owed->event);
            continue;
        }
        ta_trace_parent trace = { 0 };
        tah_inputs_redeliver(&system->inputs, owed->id, ta_system_time(system->core));
        /* The log accepted it already: past the limits it overflows (kernel ADR 0003). */
        ta_result result = tah_system_post(system, &(tah_message){ .target = actor, .event = &owed->event,
                                                                   .trace = &trace, .report = owed->report,
                                                                   .may_overflow = true });
        tah_inputs_redelivered(&system->inputs);
        if (result != TA_OK) {
            ta_event_release(&owed->event);
            failure = result;
            continue;
        }
        *count += 1u;
    }
    tah_vec_clear(&replay->owed);
    return failure;
}

/* ------------------------------------------------------------------------
 * Preparing: `spawned`, or a checkpoint
 */

/* Everything a checkpoint's restore owns while ta_actor_prepare() borrows it. */
typedef struct {
    ta_actor_state state;
    ta_state_id *states;      /* active, then late */
    ta_history_entry *history;
    ta_delayed_send_info *sends;
    ta_invoke_state *invokes;
    uint32_t send_count;
    size_t state_bytes;
    size_t history_bytes;
    size_t send_bytes;
    size_t invoke_bytes;
} tah_replay_restore;

static void *tah_replay_array(tah_replay *replay, size_t count, size_t size, size_t align, size_t *bytes) {
    *bytes = count * size + 1u;
    void *array = tah_alloc(replay->allocator, *bytes, align);
    if (array != NULL) {
        tah_mem_set(array, 0, *bytes);
    }
    return array;
}

static void tah_replay_restore_free(tah_replay *replay, tah_replay_restore *restore) {
    for (uint32_t index = 0; restore->sends != NULL && index < restore->send_count; index += 1u) {
        ta_delayed_send_info *send = &restore->sends[index];
        ta_value_release(&send->data);
        ta_value_release(&send->target);
        if (send->destination.kind == TA_ACTOR_REF_NAMED) {
            ta_address_release(send->destination.as.named);
        }
    }
    ta_value_release(&restore->state.root);
    struct {
        void *ptr;
        size_t bytes;
        size_t align;
    } arrays[] = {
        { restore->states, restore->state_bytes, _Alignof(ta_state_id) },
        { restore->history, restore->history_bytes, _Alignof(ta_history_entry) },
        { restore->sends, restore->send_bytes, _Alignof(ta_delayed_send_info) },
        { restore->invokes, restore->invoke_bytes, _Alignof(ta_invoke_state) },
    };
    for (uint32_t index = 0; index < 4u; index += 1u) {
        if (arrays[index].ptr != NULL) {
            tah_free(replay->allocator, &(ta_allocator_free_params){ .ptr = arrays[index].ptr, .size = arrays[index].bytes,
                                                                     .alignment = arrays[index].align });
        }
    }
}

/* A CHECKPOINT_SEND part, as the send ta_actor_prepare() restores held. */
static ta_result tah_replay_send_of(tah_replay *replay, const tah_wal_view *view, ta_delayed_send_info *out) {
    tah_system *system = replay->system;
    uint64_t flags = tah_wal_number(view, 5);
    ta_str text = { 0 };
    ta_result result = TA_OK;
    *out = (ta_delayed_send_info){ .due = (ta_time)tah_wal_number(view, 4),
                                   .submitted = (flags & TAH_WAL_SEND_SUBMITTED) != 0,
                                   .has_target_text = (flags & TAH_WAL_SEND_TARGET_TEXT) != 0,
                                   .target_text = (ta_content_id)tah_wal_number(view, 6),
                                   .has_content = (flags & TAH_WAL_SEND_CONTENT) != 0,
                                   .content = (ta_content_id)tah_wal_number(view, 7) };
    if (tah_wal_text(view, 0, &text)) {
        result = ta_send_id_intern(system->core, text, &out->send_id);
    }
    if (result == TA_OK) {
        result = tah_wal_text(view, 1, &text) ? tah_event_name_intern(system->core, text, &out->event) : TA_ERR_CORRUPT;
    }
    if (result == TA_OK) {
        result = tah_wal_text(view, 2, &text) ? ta_system_intern_atom(system->core, text, &out->io_type) : TA_ERR_CORRUPT;
    }
    if (result == TA_OK && (flags & TAH_WAL_SEND_SELF) != 0) {
        out->destination = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = 0 };
    } else if (result == TA_OK && tah_wal_text(view, 3, &text)) {
        ta_scxml_target target = { 0 };
        result = ta_scxml_target_parse(system->core, text, &target);
        if (result == TA_OK && target.kind == TA_SCXML_TARGET_ACTOR) {
            out->destination = target.as.actor;
        } else if (result == TA_OK) {
            ta_scxml_target_release(&target);
            result = TA_ERR_CORRUPT;
        }
    }
    uint32_t stream = 0;
    if (result == TA_OK && (flags & TAH_WAL_SEND_DATA) != 0) {
        result = tah_wal_read_value(view, stream++, system->inputs.datamodel, &out->data);
    }
    if (result == TA_OK && (flags & TAH_WAL_SEND_TARGET) != 0) {
        result = tah_wal_read_value(view, stream, system->inputs.datamodel, &out->target);
    }
    return result;
}

/*
 * Reads a checkpoint unit into `restore`, and holds the messages it owes.
 * The unit is all there (tah_wal_span with `unit`).
 */
static ta_result tah_replay_read_checkpoint(tah_replay *replay, const tah_wal_view *header, tah_replay_restore *restore) {
    tah_system *system = replay->system;
    ta_str family = { 0 };
    ta_str revision = { 0 };
    if (!tah_wal_text(header, 0, &family) || !tah_wal_text(header, 2, &revision)) {
        return TA_ERR_CORRUPT;
    }
    uint32_t active = (uint32_t)tah_wal_number(header, 9);
    uint32_t late = (uint32_t)tah_wal_number(header, 10);
    uint32_t history = (uint32_t)tah_wal_number(header, 11);
    uint64_t parts = tah_wal_number(header, 7);
    uint64_t owed = tah_wal_number(header, 8);
    restore->state = (ta_actor_state){
        .definition = { .family = { .name = family }, .revision = { .ptr = (const uint8_t *)revision.ptr, .len = revision.len } },
        .generated_ids = tah_wal_number(header, 3),
        .clock = header->time,
        .macrostep = tah_wal_number(header, 4),
        .microstep = (uint32_t)tah_wal_number(header, 5),
        .lifecycle = (uint32_t)tah_wal_number(header, 6),
        .active_count = active,
        .late_count = late,
        .history_count = history,
    };
    restore->states = tah_replay_array(replay, (size_t)active + late, sizeof(ta_state_id), _Alignof(ta_state_id),
                                       &restore->state_bytes);
    restore->history = tah_replay_array(replay, history, sizeof(ta_history_entry), _Alignof(ta_history_entry),
                                        &restore->history_bytes);
    restore->sends = tah_replay_array(replay, (size_t)parts, sizeof(ta_delayed_send_info), _Alignof(ta_delayed_send_info),
                                      &restore->send_bytes);
    restore->invokes = tah_replay_array(replay, (size_t)parts, sizeof(ta_invoke_state), _Alignof(ta_invoke_state),
                                        &restore->invoke_bytes);
    if (restore->states == NULL || restore->history == NULL || restore->sends == NULL || restore->invokes == NULL) {
        return TA_ERR_NOMEM;
    }
    uint32_t list = TAH_WAL_CHECKPOINT_FIELDS;
    for (uint32_t index = 0; index < active + late; index += 1u) {
        restore->states[index] = (ta_state_id)tah_wal_number(header, list++);
    }
    for (uint32_t index = 0; index < history; index += 1u) {
        restore->history[index].history = (ta_state_id)tah_wal_number(header, list++);
        restore->history[index].state = (ta_state_id)tah_wal_number(header, list++);
    }
    restore->state.active = restore->states;
    restore->state.late = restore->states + active;
    restore->state.history = restore->history;
    ta_result result = TA_OK;
    if (header->value_count != 0) {
        result = tah_wal_read_value(header, 0, system->inputs.datamodel, &restore->state.root);
    }
    size_t at = header->span;
    const uint8_t *unit = tah_replay_at(replay);
    size_t length = replay->bytes.count - replay->start;
    uint32_t invokes = 0;
    for (uint64_t part = 0; result == TA_OK && part < parts + owed; part += 1u) {
        tah_wal_view view;
        result = tah_wal_read(unit + at, length - at, &view);
        if (result != TA_OK) {
            break;
        }
        at += view.span;
        if (view.kind == TAH_WAL_CHECKPOINT_SEND) {
            result = tah_replay_send_of(replay, &view, &restore->sends[restore->send_count]);
            restore->send_count += 1u;
        } else if (view.kind == TAH_WAL_CHECKPOINT_INVOKE) {
            ta_str text = { 0 };
            ta_invoke_state *invoke = &restore->invokes[invokes++];
            invoke->decl = (ta_invoke_decl_id)tah_wal_number(&view, 2);
            result = tah_wal_text(&view, 0, &text) ? ta_system_intern_invoke_id(system->core, text, &invoke->id)
                                                   : TA_ERR_CORRUPT;
            if (result == TA_OK) {
                result = tah_wal_text(&view, 1, &text) ? ta_system_intern_atom(system->core, text, &invoke->type)
                                                       : TA_ERR_CORRUPT;
            }
        } else if (view.kind == TAH_WAL_ACCEPTED) {
            result = tah_replay_hold(replay, &view);
        } else {
            result = TA_ERR_CORRUPT;
        }
    }
    restore->state.sends = restore->sends;
    restore->state.send_count = restore->send_count;
    restore->state.invokes = restore->invokes;
    restore->state.invoke_count = invokes;
    return result;
}

/* The replaying actor starts under the scheduler's eye but left to the replay. */
static ta_result tah_replay_adopt(tah_system *system, ta_actor_id actor) {
    ta_result result = tah_hook_actor_created(system, actor);
    if (result != TA_OK) {
        return result;
    }
    tah_sched_entry *entry = tah_sched_lookup(&system->sched, actor);
    if (entry != NULL) {
        entry->flags |= TAH_SCHED_REPLAYING;
    }
    return TA_OK;
}

ta_result tah_replay_prepare(tah_replay *replay, const tah_replay_prepare_params *params) {
    tah_system *system = replay->system;
    *params->found = false;
    if (replay->closed) {
        return TA_ERR_INVALID;
    }
    /* Mail that waited for a load of an address no actor had yet comes
     * before the life's first record: it is held as owed. */
    for (;;) {
        size_t span = tah_replay_next_span(replay, true);
        if (span == 0) {
            return TA_OK;
        }
        tah_wal_view view;
        ta_result result = tah_wal_read(tah_replay_at(replay), replay->bytes.count - replay->start, &view);
        if (result != TA_OK) {
            return result;
        }
        /* The latest time (kernel ADR 0004): mail accepted for a new actor
         * before its bootstrap ended comes before its later-timed SPAWNED. */
        if (!replay->has_time || view.time > replay->last_time) {
            replay->last_time = view.time;
        }
        replay->has_time = true;
        if (view.kind == TAH_WAL_ACCEPTED || view.kind == TAH_WAL_DROPPED || view.kind == TAH_WAL_EVICTED ||
            view.kind == TAH_WAL_FIRED) {
            if (view.kind == TAH_WAL_ACCEPTED) {
                result = tah_replay_hold(replay, &view);
            } else if (view.kind == TAH_WAL_DROPPED) {
                tah_replay_forget(replay, tah_wal_number(&view, 0));
            }
            if (result != TA_OK) {
                return result;
            }
            replay->start += (uint32_t)span;
            continue;
        }
        ta_actor_id actor = 0;
        replay->covered = 0;
        if (view.kind == TAH_WAL_SPAWNED) {
            ta_value binding = { 0 };
            if (view.value_count != 0) {
                result = tah_wal_read_value(&view, 0, system->inputs.datamodel, &binding);
            }
            ta_time at = view.time;
            if (result == TA_OK) {
                system->inside_core = true;
                result = ta_actor_prepare(system->core, &(ta_actor_prepare_params){
                                                            .incumbent = params->incumbent,
                                                            .address = params->address,
                                                            .definition = params->definition,
                                                            .datamodel_binding = binding.v != NULL ? &binding : NULL,
                                                            .at = &at,
                                                            .out = &actor,
                                                        });
                system->inside_core = false;
            }
            ta_value_release(&binding);
        } else if (view.kind == TAH_WAL_CHECKPOINT) {
            tah_replay_restore restore = { 0 };
            result = tah_replay_read_checkpoint(replay, &view, &restore);
            if (result == TA_OK) {
                system->inside_core = true;
                result = ta_actor_prepare(system->core, &(ta_actor_prepare_params){
                                                            .incumbent = params->incumbent,
                                                            .address = params->address,
                                                            .definition = params->definition,
                                                            .state = &restore.state,
                                                            .out = &actor,
                                                        });
                system->inside_core = false;
            }
            tah_replay_restore_free(replay, &restore);
            replay->covered = view.seq;
        } else {
            return TA_ERR_CORRUPT; /* a life starts with `spawned` or a checkpoint */
        }
        if (result != TA_OK) {
            return result;
        }
        replay->last_seq = view.seq;
        replay->start += (uint32_t)span;
        result = tah_replay_adopt(system, actor);
        if (result != TA_OK) {
            return result;
        }
        /* Its bootstrap, from `spawned`; a checkpoint's actor is idle already. */
        if (view.kind == TAH_WAL_SPAWNED) {
            bool ignored = false;
            bool done = false;
            result = tah_replay_resume(system, actor, false, &ignored, &done);
            if (result != TA_OK && result != TA_ERR_RUNAWAY) {
                /* The caller is never given this actor: it must not stay. */
                system->inside_core = true;
                (void)ta_actor_destroy(system->core, actor);
                system->inside_core = false;
                (void)tah_hook_actor_removed(system, actor);
                return result;
            }
        }
        *params->out = actor;
        *params->found = true;
        return TA_OK;
    }
}

/* ------------------------------------------------------------------------
 * Writing a checkpoint
 */

typedef struct {
    tah_system *system;
    ta_byte_sink sink;
    tah_vec buffer; /* uint8_t: one record at a time */
    ta_result failure;
} tah_checkpoint_out;

/* Writes `record` and hands its bytes to the sink. */
static void tah_checkpoint_emit(tah_checkpoint_out *out, tah_wal_record *record) {
    if (out->failure != TA_OK) {
        return;
    }
    out->buffer.count = 0;
    ta_result result = tah_wal_write(record, &(tah_wal_write_params){ .out = &out->buffer,
                                                                      .datamodel = out->system->inputs.datamodel });
    if (result == TA_OK &&
        out->sink.v->write(out->sink.self, (ta_bytes){ .ptr = out->buffer.items, .len = out->buffer.count }) != TA_OP_OK) {
        result = TA_ERR_NOMEM;
    }
    out->failure = result;
}

/* The text of a send's destination, into `buffer`; false for none. */
static bool tah_checkpoint_destination(tah_system *system, const ta_delayed_send_info *send, char *buffer,
                                       uint32_t capacity, ta_str *out) {
    if (send->destination.kind == TA_ACTOR_REF_RUNTIME && send->destination.as.runtime == 0) {
        return false;
    }
    uint32_t needed = 0;
    ta_scxml_target target = { .kind = TA_SCXML_TARGET_ACTOR, .as.actor = send->destination };
    if (ta_scxml_target_format(system->core, &(ta_scxml_target_format_params){ .target = target, .buffer = buffer,
                                                                               .capacity = capacity,
                                                                               .needed = &needed }) != TA_OK) {
        return false;
    }
    *out = (ta_str){ .ptr = buffer, .len = needed - 1u };
    return true;
}

typedef struct {
    ta_actor_id actor;
    ta_actor_address address;
    uint64_t seq;
    ta_actor_state state;
    uint32_t owed;
} tah_checkpoint_header;

static void tah_checkpoint_parts(tah_checkpoint_out *out, const tah_checkpoint_header *header) {
    tah_system *system = out->system;
    const ta_actor_state *state = &header->state;
    ta_atom scxml = ta_system_scxml_io_type(system->core);
    /* A delayed send to the actor itself names its session (core ADR 0066):
     * it is written as "self", so a restore points it at the new session. */
    ta_actor_inspection inspection = { 0 };
    uint64_t session = ta_actor_inspect(system->core, header->actor, &inspection) == TA_OK ? inspection.session_id : 0;
    for (uint32_t index = 0; index < state->send_count && out->failure == TA_OK; index += 1u) {
        const ta_delayed_send_info *send = &state->sends[index];
        char send_text[128];
        char name_text[512];
        char destination_text[512];
        uint32_t needed = 0;
        ta_str send_id = { 0 };
        ta_str name = { 0 };
        ta_str type = { 0 };
        ta_str destination = { 0 };
        bool has_send = send->send_id != 0 &&
                        ta_send_id_format(system->core, &(ta_send_id_format_params){ .send_id = send->send_id,
                                                                                     .buffer = send_text,
                                                                                     .capacity = sizeof send_text,
                                                                                     .needed = &needed }) == TA_OK;
        if (has_send) {
            send_id = (ta_str){ .ptr = send_text, .len = needed - 1u };
        }
        name = tah_replay_name(system, send->event, name_text, sizeof name_text);
        (void)ta_system_atom_text(system->core, send->io_type, &type);
        bool self = send->io_type == scxml && send->destination.kind == TA_ACTOR_REF_RUNTIME &&
                    (send->destination.as.runtime == 0 || (session != 0 && send->destination.as.runtime == session));
        bool has_destination = send->io_type == scxml && !self &&
                               tah_checkpoint_destination(system, send, destination_text, sizeof destination_text,
                                                          &destination);
        uint64_t flags = (send->data.v != NULL ? TAH_WAL_SEND_DATA : 0u) | (send->target.v != NULL ? TAH_WAL_SEND_TARGET : 0u) |
                         (send->has_target_text ? TAH_WAL_SEND_TARGET_TEXT : 0u) |
                         (send->has_content ? TAH_WAL_SEND_CONTENT : 0u) | (self ? TAH_WAL_SEND_SELF : 0u) |
                         (send->submitted ? TAH_WAL_SEND_SUBMITTED : 0u);
        if (name.len == 0) {
            out->failure = TA_ERR_NOMEM;
            return;
        }
        tah_wal_record record = {
            .kind = TAH_WAL_CHECKPOINT_SEND,
            .seq = header->seq,
            .time = state->clock,
            .field_count = 8,
            .fields = { tah_wal_text_field(has_send, send_id), tah_wal_text_field(true, name),
                        tah_wal_text_field(true, type), tah_wal_text_field(has_destination, destination),
                        tah_wal_number_field(send->due), tah_wal_number_field(flags),
                        tah_wal_number_field(send->target_text), tah_wal_number_field(send->content) },
            .values = { &send->data, &send->target },
        };
        tah_checkpoint_emit(out, &record);
    }
    for (uint32_t index = 0; index < state->invoke_count && out->failure == TA_OK; index += 1u) {
        const ta_invoke_state *invoke = &state->invokes[index];
        ta_str id = { 0 };
        ta_str type = { 0 };
        (void)ta_system_invoke_id_text(system->core, invoke->id, &id);
        (void)ta_system_atom_text(system->core, invoke->type, &type);
        tah_wal_record record = {
            .kind = TAH_WAL_CHECKPOINT_INVOKE,
            .seq = header->seq,
            .time = state->clock,
            .field_count = 3,
            .fields = { tah_wal_text_field(true, id), tah_wal_text_field(true, type), tah_wal_number_field(invoke->decl) },
        };
        tah_checkpoint_emit(out, &record);
    }
}

/* The messages the actor's mailbox holds that the log numbered, as ACCEPTED records. */
static void tah_checkpoint_owed(tah_checkpoint_out *out, const tah_checkpoint_header *header, bool count_only,
                                uint32_t *count) {
    tah_system *system = out->system;
    tah_sched_entry *entry = tah_sched_lookup(&system->sched, header->actor);
    ta_actor_inspection view;
    uint64_t session = ta_actor_inspect(system->core, header->actor, &view) == TA_OK ? view.session_id : 0;
    *count = 0;
    for (uint32_t index = entry != NULL ? entry->first : 0; index != 0 && out->failure == TA_OK;
         index = tah_sched_envelope(&system->sched, index)->next) {
        uint64_t id = 0;
        if (!tah_map_get(&system->inputs.seqs, index, &id) || id == 0) {
            continue;
        }
        *count += 1u;
        if (count_only) {
            continue;
        }
        tah_wal_record record;
        ta_result result = tah_inputs_accepted_record(system, tah_sched_envelope(&system->sched, index), id, session, &record);
        if (result == TA_OK) {
            record.seq = header->seq;
            tah_checkpoint_emit(out, &record);
        } else {
            out->failure = result;
        }
        if (system->inputs.text_ready) {
            ta_arena_reset(&system->inputs.text);
        }
    }
}

ta_result tah_checkpoint_write(tah_system *system, ta_actor_id actor, ta_byte_sink sink) {
    ta_actor_address address = { 0 };
    if (!tah_inputs_logged(system, actor, &address)) {
        return TA_ERR_INVALID;
    }
    /* The checkpoint's record number covers its last macrostep's first record (kernel ADR 0004). */
    ta_result first = tah_inputs_write_first(system, actor);
    if (first != TA_OK) {
        return first;
    }
    ta_allocator allocator = tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER);
    uint32_t state_count = 0;
    uint32_t history_count = 0;
    uint32_t invoke_count = 0;
    ta_actor_state state = { 0 };
    system->inside_core = true;
    ta_result result = ta_actor_export_state(system->core, &(ta_actor_export_params){
                                                               .actor = actor,
                                                               .state_count = &state_count,
                                                               .history_count = &history_count,
                                                               .invoke_count = &invoke_count,
                                                               .out = &state,
                                                           });
    system->inside_core = false;
    if (result != TA_OK && result != TA_ERR_NOMEM) {
        return result;
    }
    ta_value_release(&state.root);
    size_t state_bytes = (size_t)state_count * sizeof(ta_state_id) + 1u;
    size_t history_bytes = (size_t)history_count * sizeof(ta_history_entry) + 1u;
    size_t invoke_bytes = (size_t)invoke_count * sizeof(ta_invoke_state) + 1u;
    ta_state_id *states = tah_alloc(allocator, state_bytes, _Alignof(ta_state_id));
    ta_history_entry *history = tah_alloc(allocator, history_bytes, _Alignof(ta_history_entry));
    ta_invoke_state *invokes = tah_alloc(allocator, invoke_bytes, _Alignof(ta_invoke_state));
    result = states == NULL || history == NULL || invokes == NULL ? TA_ERR_NOMEM : TA_OK;
    if (result == TA_OK) {
        state = (ta_actor_state){ 0 };
        system->inside_core = true;
        result = ta_actor_export_state(system->core, &(ta_actor_export_params){
                                                         .actor = actor,
                                                         .states = states,
                                                         .state_capacity = state_count,
                                                         .state_count = &state_count,
                                                         .history = history,
                                                         .history_capacity = history_count,
                                                         .history_count = &history_count,
                                                         .invokes = invokes,
                                                         .invoke_capacity = invoke_count,
                                                         .invoke_count = &invoke_count,
                                                         .out = &state,
                                                     });
        system->inside_core = false;
    }
    tah_checkpoint_out out = { .system = system, .sink = sink, .failure = result };
    tah_vec_init(&out.buffer, allocator, TAH_LAYOUT(uint8_t));
    tah_checkpoint_header header = { .actor = actor, .address = address, .state = state,
                                     .seq = tah_inputs_actor_seq(&system->inputs, actor) };
    uint32_t owed = 0;
    if (out.failure == TA_OK) {
        tah_checkpoint_owed(&out, &header, true, &owed);
    }
    if (out.failure == TA_OK && sink.v->begin(sink.self) != TA_OP_OK) {
        out.failure = TA_ERR_NOMEM;
    }
    bool begun = out.failure == TA_OK;
    if (out.failure == TA_OK) {
        uint32_t lists = state.active_count + state.late_count + 2u * state.history_count;
        uint64_t *numbers = tah_alloc(allocator, (size_t)lists * sizeof(uint64_t) + 1u, _Alignof(uint64_t));
        if (numbers == NULL) {
            out.failure = TA_ERR_NOMEM;
        } else {
            uint32_t at = 0;
            for (uint32_t index = 0; index < state.active_count; index += 1u) {
                numbers[at++] = state.active[index];
            }
            for (uint32_t index = 0; index < state.late_count; index += 1u) {
                numbers[at++] = state.late[index];
            }
            for (uint32_t index = 0; index < state.history_count; index += 1u) {
                numbers[at++] = state.history[index].history;
                numbers[at++] = state.history[index].state;
            }
            tah_wal_record record = {
                .kind = TAH_WAL_CHECKPOINT,
                .seq = header.seq,
                .time = state.clock,
                .field_count = TAH_WAL_CHECKPOINT_FIELDS,
                .fields = { tah_wal_text_field(true, address.family.name), tah_wal_text_field(true, address.name),
                            tah_wal_text_field(true, (ta_str){ .ptr = (const char *)state.definition.revision.ptr,
                                                               .len = state.definition.revision.len }),
                            tah_wal_number_field(state.generated_ids), tah_wal_number_field(state.macrostep),
                            tah_wal_number_field(state.microstep), tah_wal_number_field(state.lifecycle),
                            tah_wal_number_field((uint64_t)state.send_count + state.invoke_count),
                            tah_wal_number_field(owed), tah_wal_number_field(state.active_count),
                            tah_wal_number_field(state.late_count), tah_wal_number_field(state.history_count) },
                .numbers = numbers,
                .number_count = lists,
                .values = { &state.root },
            };
            tah_checkpoint_emit(&out, &record);
            tah_free(allocator, &(ta_allocator_free_params){ .ptr = numbers, .size = (size_t)lists * sizeof(uint64_t) + 1u,
                                                             .alignment = _Alignof(uint64_t) });
        }
    }
    tah_checkpoint_parts(&out, &header);
    uint32_t written = 0;
    if (out.failure == TA_OK) {
        tah_checkpoint_owed(&out, &header, false, &written);
    }
    if (begun) {
        sink.v->end(sink.self, out.failure == TA_OK);
    }
    ta_value_release(&state.root);
    tah_vec_destroy(&out.buffer);
    if (states != NULL) {
        tah_free(allocator, &(ta_allocator_free_params){ .ptr = states, .size = state_bytes, .alignment = _Alignof(ta_state_id) });
    }
    if (history != NULL) {
        tah_free(allocator, &(ta_allocator_free_params){ .ptr = history, .size = history_bytes,
                                                         .alignment = _Alignof(ta_history_entry) });
    }
    if (invokes != NULL) {
        tah_free(allocator, &(ta_allocator_free_params){ .ptr = invokes, .size = invoke_bytes,
                                                         .alignment = _Alignof(ta_invoke_state) });
    }
    return out.failure;
}

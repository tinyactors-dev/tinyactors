#include "run.h"
#include "../include/tinyactors.h"
#include "actor.h"
#include "chunks.h"
#include "envelope.h"
#include "event.h"
#include "generated/ring_records.h"
#include "interpreter.h"
#include "invoke.h"
#include "list.h"
#include "memory.h"
#include "name_area.h"
#include "quota.h"
#include "system.h"
#include "trace.h"

/* ------------------------------------------------------------------------
 * An input's text: one binary field (see the file comment)
 */

/* The field holds a trace parent. */
#define TA_RUN_RECORD_TRACED 0x01u
/* The field holds an operator transition and its definition. */
#define TA_RUN_RECORD_JUMP 0x02u
/* A report that retires its invocation. */
#define TA_RUN_RECORD_ENDS 0x04u

/* What an input keeps in its text, unpacked. */
typedef struct {
    ta_event_name_id name;
    uint8_t flags;
    ta_origin origin;
    ta_time time;
    ta_trace_parent trace;
    ta_operator_jump *jump;
    ta_definition_id jump_definition;
} ta_run_record;

/* The most bytes a packed record takes. */
#define TA_RUN_RECORD_BYTES                                                                                     \
    (sizeof(ta_event_name_id) + 1u + sizeof(ta_origin) + sizeof(ta_time) + sizeof(ta_trace_parent) +          \
     sizeof(ta_operator_jump *) + sizeof(ta_definition_id))

typedef struct {
    uint8_t *bytes;
    uint32_t length;
} ta_run_packer;

static void ta_run_pack(ta_run_packer *packer, const void *value, uint32_t size) {
    ta_mem_copy(packer->bytes + packer->length, value, size);
    packer->length += size;
}

/* Packs `record` into `bytes`, field by field, with no padding; answers its length. */
static uint32_t ta_run_record_pack(const ta_run_record *record, uint8_t *bytes) {
    ta_run_packer packer = { .bytes = bytes, .length = 0u };
    ta_run_pack(&packer, &record->name, sizeof record->name);
    ta_run_pack(&packer, &record->flags, sizeof record->flags);
    ta_run_pack(&packer, &record->origin, sizeof record->origin);
    ta_run_pack(&packer, &record->time, sizeof record->time);
    if ((record->flags & TA_RUN_RECORD_TRACED) != 0u) {
        ta_run_pack(&packer, &record->trace, sizeof record->trace);
    }
    if ((record->flags & TA_RUN_RECORD_JUMP) != 0u) {
        ta_run_pack(&packer, &record->jump, sizeof record->jump);
        ta_run_pack(&packer, &record->jump_definition, sizeof record->jump_definition);
    }
    return packer.length;
}

static void ta_run_unpack(ta_run_packer *packer, void *value, uint32_t size) {
    ta_mem_copy(value, packer->bytes + packer->length, size);
    packer->length += size;
}

/*
 * The record in envelope `index`'s text. An envelope without text (a wake)
 * reads as all zero. The text was written by ta_run_admit, so it holds one
 * field of a packed record.
 */
static ta_run_record ta_run_record_of(const ta_system *system, uint32_t index) {
    ta_run_record record = { .origin = { .kind = TA_ORIGIN_NONE } };
    const ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    if (envelope->text.first == 0u) {
        return record;
    }
    uint8_t bytes[TA_RUN_RECORD_BYTES];
    ta_name_area_reader reader = ta_name_area_reader_of(envelope->text);
    uint32_t length = 0u;
    if (ta_name_area_next(&system->run.names, &reader, &length) != TA_OK || length > sizeof bytes) {
        return record;
    }
    uint32_t got = 0u;
    while (got < length) {
        ta_bytes span = ta_name_area_read(&system->run.names, &reader, length - got);
        ta_mem_copy(bytes + got, span.ptr, span.len);
        got += span.len;
    }
    ta_run_packer packer = { .bytes = bytes, .length = 0u };
    ta_run_unpack(&packer, &record.name, sizeof record.name);
    ta_run_unpack(&packer, &record.flags, sizeof record.flags);
    ta_run_unpack(&packer, &record.origin, sizeof record.origin);
    ta_run_unpack(&packer, &record.time, sizeof record.time);
    if ((record.flags & TA_RUN_RECORD_TRACED) != 0u) {
        ta_run_unpack(&packer, &record.trace, sizeof record.trace);
    }
    if ((record.flags & TA_RUN_RECORD_JUMP) != 0u) {
        ta_run_unpack(&packer, &record.jump, sizeof record.jump);
        ta_run_unpack(&packer, &record.jump_definition, sizeof record.jump_definition);
    }
    return record;
}

/* Frees an operator transition an input carried. */
static void ta_run_jump_free(ta_system *system, ta_operator_jump *jump) {
    if (jump == NULL) {
        return;
    }
    size_t bytes = 0;
    (void)ta_operator_jump_bytes(jump->target_count, &bytes); /* sized when it was allocated */
    ta_mem_free(system->allocator, jump,
                &(ta_allocator_free_params){ .size = bytes, .alignment = _Alignof(ta_operator_jump) });
}

/*
 * Releases everything envelope `index` holds (its data and content, an
 * event's origin and operator transition, its text) and leaves it bare,
 * where it stands: a bare envelope reads as a wake without an event. O(1)
 * in the envelopes; frees the text's chunks.
 */
static void ta_run_strip(ta_system *system, uint32_t index) {
    if (system->run.event_input == index) {
        system->run.event_input = 0u;
        system->run.event = (ta_event){ 0 };
    }
    ta_run_record record = ta_run_record_of(system, index);
    ta_origin_release(&record.origin);
    ta_run_jump_free(system, record.jump);
    ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    ta_value_release(&envelope->data);
    ta_value_release(&envelope->content);
    envelope->data = (ta_value){ 0 };
    envelope->content = (ta_value){ 0 };
    ta_name_area_free(&system->run.names, &envelope->text);
}

/* Releases everything envelope `index` holds and gives it back. The
 * envelope is on no list. */
static void ta_run_discard(ta_system *system, uint32_t index) {
    ta_run_strip(system, index);
    ta_envelope_pool_give(&system->run.pool, index);
}

/* ------------------------------------------------------------------------
 * The state
 */

ta_result ta_run_init(ta_system *system) {
    uint32_t bound = system->params[TA_PARAM_POOL_ENVELOPES];
    uint32_t watermark = system->params[TA_PARAM_ADMISSION_WATERMARK];
    uint32_t chunks = system->params[TA_PARAM_NAME_AREA_BYTES] / TA_NAME_AREA_CHUNK_BYTES;
    ta_result result = ta_envelope_pool_init(&system->run.pool, &(ta_envelope_pool_params){
                                                                    .allocator = system->allocator,
                                                                    .bound = bound,
                                                                    .watermark_percent = watermark,
                                                                    .reserve = bound < TA_RUN_RESERVE ? bound
                                                                                                      : TA_RUN_RESERVE,
                                                                });
    if (result != TA_OK) {
        return result;
    }
    return ta_name_area_init(&system->run.names, &(ta_name_area_params){
                                                     .allocator = system->allocator,
                                                     .bytes = system->params[TA_PARAM_NAME_AREA_BYTES],
                                                     .watermark_percent = watermark,
                                                     .reserve = chunks < TA_RUN_RESERVE ? chunks : TA_RUN_RESERVE,
                                                 });
}

void ta_run_destroy(ta_system *system) {
    ta_envelope_pool *pool = &system->run.pool;
    for (uint32_t index = 1u; index <= pool->items.high_water; index += 1u) {
        ta_envelope *envelope = ta_envelope_pool_at(pool, index);
        /* Every session is gone, and gave its taken input back; a runnable
         * envelope, or a taken one no session names, still holds its own. */
        if (envelope != NULL &&
            (envelope->state == TA_ENVELOPE_RUNNABLE || envelope->state == TA_ENVELOPE_TAKEN)) {
            ta_run_record record = ta_run_record_of(system, index);
            ta_origin_release(&record.origin);
            ta_run_jump_free(system, record.jump);
            ta_value_release(&envelope->data);
            ta_value_release(&envelope->content);
        }
    }
    ta_name_area_destroy(&system->run.names);
    ta_envelope_pool_destroy(pool);
    system->run.slot = 0u;
    system->run.event_input = 0u;
}

/* What a refill answered, for the pool and for the name area. */
typedef struct {
    ta_result pool;
    ta_result names;
} ta_run_refilled;

/* Keeps the reserve of free envelopes and chunks, unless a macrostep runs:
 * a safe point. A refill that fails leaves what is free, and says so. */
static ta_run_refilled ta_run_refill(ta_system *system) {
    if (system->run.running) {
        return (ta_run_refilled){ .pool = TA_OK, .names = TA_OK };
    }
    return (ta_run_refilled){ .pool = ta_envelope_pool_refill(&system->run.pool),
                              .names = ta_name_area_refill(&system->run.names) };
}

/* What the pool or the name area refused with, and what the refill before
 * it answered for the same store. */
typedef struct {
    ta_result refill;
    ta_result result;
} ta_run_refusal;

/*
 * The answer for an envelope `pool` refused with `refusal.result`.
 * TA_ERR_FULL becomes TA_ERR_NOMEM when the refill before it failed for want
 * of memory and the pool's limits would have let it in (`admission` adds the
 * watermark): no envelope was free only because none could be allocated
 * (ADR 0102).
 */
static ta_result ta_run_envelope_short(const ta_envelope_pool *pool, ta_run_refusal refusal, bool admission) {
    if (refusal.result != TA_ERR_FULL || refusal.refill != TA_ERR_NOMEM) {
        return refusal.result;
    }
    /* The refill allocates only below the bound, so the bound refused nothing. */
    return !admission || ta_envelope_pool_admitting(pool) ? TA_ERR_NOMEM : refusal.result;
}

/* As ta_run_envelope_short, for `length` bytes of admitted text `names`
 * refused. */
static ta_result ta_run_names_short(const ta_name_area *names, ta_run_refusal refusal, uint32_t length) {
    if (refusal.result != TA_ERR_FULL || refusal.refill != TA_ERR_NOMEM) {
        return refusal.result;
    }
    ta_quota_request request = { .amount = ta_name_area_field_need(names, 0u, length),
                                 .keep = names->quota.capacity - names->watermark };
    return ta_quota_fits(&names->quota, request) ? TA_ERR_NOMEM : refusal.result;
}

/* ------------------------------------------------------------------------
 * Admission
 */

/* An input to admit: the envelope's fields and its record. */
typedef struct {
    uint8_t source;
    uint8_t type;
    ta_actor_id target;
    ta_send_id send_id;
    ta_invoke_id invoke;
    uint64_t correlation;
    const ta_run_record *record;
} ta_run_admission;

/* Writes `record` as the one field of envelope `index`'s text, which has
 * none. TA_ERR_FULL or TA_ERR_NOMEM as the name area answers, changing
 * nothing; `admission` holds it to the watermark. */
static ta_result ta_run_write_record(ta_system *system, uint32_t index, const ta_run_record *record,
                                     ta_run_refilled refilled, bool admission) {
    uint8_t bytes[TA_RUN_RECORD_BYTES];
    uint32_t length = ta_run_record_pack(record, bytes);
    ta_chunk_writer writer = ta_chunk_writer_new();
    ta_result result = ta_name_area_write(&system->run.names, &(ta_name_area_write_params){
                                                                   .writer = &writer,
                                                                   .text = { .ptr = bytes, .len = length },
                                                                   .admission = admission,
                                                               });
    if (result != TA_OK) {
        return admission ? ta_run_names_short(&system->run.names,
                                              (ta_run_refusal){ .refill = refilled.names, .result = result }, length)
                         : result == TA_ERR_FULL && refilled.names == TA_ERR_NOMEM ? TA_ERR_NOMEM : result;
    }
    ta_envelope_pool_at(&system->run.pool, index)->text = writer.chain;
    return TA_OK;
}

/*
 * Takes an envelope below the watermark, writes the record into its text,
 * fills it and appends it to the run queue. Whoever calls it retains what
 * the envelope holds once it answers TA_OK. TA_ERR_FULL changes nothing.
 */
static ta_result ta_run_admit(ta_system *system, const ta_run_admission *admission, uint32_t *out) {
    ta_run_refilled refilled = ta_run_refill(system);
    uint32_t index = 0u;
    ta_result result = ta_envelope_pool_admit(&system->run.pool, &index);
    if (result != TA_OK) {
        return ta_run_envelope_short(&system->run.pool, (ta_run_refusal){ .refill = refilled.pool, .result = result },
                                     true);
    }
    result = ta_run_write_record(system, index, admission->record, refilled, true);
    if (result != TA_OK) {
        ta_envelope_pool_give(&system->run.pool, index);
        return result;
    }
    ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    envelope->source = admission->source;
    envelope->type = admission->type;
    envelope->target = admission->target;
    envelope->send_id = admission->send_id;
    envelope->invoke_instance = admission->invoke;
    envelope->source_seq = admission->correlation;
    ta_envelope_pool_enqueue(&system->run.pool, index);
    *out = index;
    return TA_OK;
}

ta_result ta_run_admit_event(ta_system *system, const ta_run_event *input) {
    const ta_event *event = input->event;
    ta_run_record record = { .name = event->name, .origin = event->origin, .time = input->time };
    if (input->trace != NULL && !ta_trace_parent_empty(input->trace)) {
        record.flags |= TA_RUN_RECORD_TRACED;
        record.trace = *input->trace;
    }
    if (input->jump != NULL) {
        record.flags |= TA_RUN_RECORD_JUMP;
        record.jump = input->jump;
        record.jump_definition = input->actor->record.definition;
    }
    uint32_t index = 0u;
    ta_result result = ta_run_admit(system, &(ta_run_admission){ .source = TA_RECORD_INPUT_SOURCE_H2C,
                                                                 .type = (uint8_t)event->type,
                                                                 .target = input->actor->record.id,
                                                                 .send_id = event->send_id,
                                                                 .invoke = event->invoke_id,
                                                                 .correlation = input->correlation,
                                                                 .record = &record },
                                    &index);
    if (result != TA_OK) {
        return result;
    }
    /* The envelope holds the event's references from here on: the text
     * holds the origin, `data` the data. */
    ta_event held;
    ta_event_retain(event, &held);
    ta_envelope_pool_at(&system->run.pool, index)->data = held.data;
    ta_actor_note_admitted(input->actor, input->time);
    return TA_OK;
}

ta_result ta_run_admit_report(ta_system *system, const ta_run_report *report) {
    ta_run_record record = { .name = report->name, .origin = { .kind = TA_ORIGIN_NONE }, .time = report->time,
                             .flags = report->ends ? TA_RUN_RECORD_ENDS : 0u };
    if (report->trace != NULL && !ta_trace_parent_empty(report->trace)) {
        record.flags |= TA_RUN_RECORD_TRACED;
        record.trace = *report->trace;
    }
    uint32_t index = 0u;
    ta_result result = ta_run_admit(system, &(ta_run_admission){ .source = TA_RECORD_INPUT_SOURCE_INVOKE_REPORT,
                                                                 .type = (uint8_t)TA_EVENT_PLATFORM,
                                                                 .target = report->actor->record.id,
                                                                 .invoke = report->invoke,
                                                                 .correlation = report->correlation,
                                                                 .record = &record },
                                    &index);
    if (result != TA_OK) {
        return result;
    }
    if (report->data != NULL) {
        ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
        envelope->data = *report->data;
        ta_value_retain(&envelope->data);
    }
    ta_actor_note_admitted(report->actor, report->time);
    return TA_OK;
}

ta_result ta_run_wake_reserve(ta_system *system, const ta_run_wake *wake, uint32_t *out) {
    ta_run_refilled refilled = ta_run_refill(system);
    uint32_t index = 0u;
    ta_result result = wake->admission ? ta_envelope_pool_admit(&system->run.pool, &index)
                                       : ta_envelope_pool_take(&system->run.pool, &index);
    if (result != TA_OK) {
        return ta_run_envelope_short(&system->run.pool, (ta_run_refusal){ .refill = refilled.pool, .result = result },
                                     wake->admission);
    }
    bool traced = wake->trace != NULL && !ta_trace_parent_empty(wake->trace);
    if (traced || wake->event != NULL) {
        ta_run_record record = { .origin = { .kind = TA_ORIGIN_NONE } };
        if (traced) {
            record.flags |= TA_RUN_RECORD_TRACED;
            record.trace = *wake->trace;
        }
        if (wake->event != NULL) {
            record.name = wake->event->name;
            record.origin = wake->event->origin;
        }
        if (wake->jump != NULL) {
            record.flags |= TA_RUN_RECORD_JUMP;
            record.jump = wake->jump;
        }
        result = ta_run_write_record(system, index, &record, refilled, wake->admission);
        if (result != TA_OK) {
            ta_envelope_pool_give(&system->run.pool, index);
            return result;
        }
    }
    if (wake->event != NULL) {
        ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
        envelope->type = (uint8_t)wake->event->type;
        envelope->send_id = wake->event->send_id;
        envelope->invoke_instance = wake->event->invoke_id;
        envelope->data = wake->event->data;
    }
    *out = index;
    return TA_OK;
}

void ta_run_wake_place(ta_system *system, ta_actor *actor, uint32_t wake) {
    if (!actor->macrostep) {
        ta_run_discard(system, wake);
        return;
    }
    ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, wake);
    envelope->source = TA_RECORD_INPUT_SOURCE_PLATFORM;
    envelope->target = actor->record.id;
    if (actor->input == 0u) {
        /* The wake that starts its macrostep: what the macrostep runs under
         * is read from it until it is taken. */
        actor->input = wake;
        actor->input_flags = TA_ACTOR_INPUT_WAITING;
    }
    if (system->run.running && system->run.current != 0u) {
        ta_envelope_pool_enqueue_beside(&system->run.pool,
                                        (ta_envelope_beside){ .beside = system->run.current, .index = wake });
        return;
    }
    ta_envelope_pool_enqueue(&system->run.pool, wake);
}

void ta_run_wake_cancel(ta_system *system, uint32_t wake) {
    ta_run_discard(system, wake);
}

/* ------------------------------------------------------------------------
 * A taken input
 */

ta_event ta_run_input_event_of(const ta_system *system, uint32_t input) {
    if (input == 0u) {
        return (ta_event){ .origin = { .kind = TA_ORIGIN_NONE } };
    }
    ta_run_record record = ta_run_record_of(system, input);
    const ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, input);
    return (ta_event){ .name = record.name,
                       .type = (ta_event_type)envelope->type,
                       .send_id = envelope->send_id,
                       .origin = record.origin,
                       .invoke_id = envelope->invoke_instance,
                       .data = envelope->data };
}

const ta_event *ta_run_input_event(ta_system *system, uint32_t input) {
    if (input == 0u) {
        return NULL;
    }
    if (system->run.event_input != input) {
        system->run.event = ta_run_input_event_of(system, input);
        system->run.event_input = input;
    }
    return &system->run.event;
}

const struct ta_operator_jump *ta_run_input_jump(const ta_system *system, uint32_t input) {
    return input == 0u ? NULL : ta_run_record_of(system, input).jump;
}

/* The trace parent envelope `index`'s text holds, all zero for none. */
static ta_trace_parent ta_run_trace_of(const ta_system *system, uint32_t index) {
    ta_run_record record = ta_run_record_of(system, index);
    return (record.flags & TA_RUN_RECORD_TRACED) != 0u ? record.trace : (ta_trace_parent){ 0 };
}

ta_trace_parent ta_run_session_trace(const ta_system *system, const ta_actor *actor) {
    return actor->input == 0u ? (ta_trace_parent){ 0 } : ta_run_trace_of(system, actor->input);
}

ta_time ta_run_macrostep_time(const ta_system *system, const ta_actor *actor) {
    if (actor->macrostep && actor->input != 0u && (actor->input_flags & TA_ACTOR_INPUT_WAITING) == 0u) {
        return ta_envelope_pool_at(&system->run.pool, actor->input)->due;
    }
    return ta_actor_now(system, actor);
}

/* ------------------------------------------------------------------------
 * Sessions
 */

/* Whether envelope `index` is the taken input of its session, queued again. */
static bool ta_run_is_taken_input(const ta_actor *actor, uint32_t index) {
    return actor != NULL && actor->input == index && (actor->input_flags & TA_ACTOR_INPUT_WAITING) == 0u;
}

/* `actor` names no input any more. */
static void ta_run_forget_input(ta_actor *actor) {
    actor->input = 0u;
    actor->input_flags = 0u;
}

/* Returns the first of `actor`'s deferred envelopes to the run queue, or all of them. */
static void ta_run_return_deferred(ta_system *system, ta_actor *actor, bool all) {
    ta_list_links links = ta_envelope_pool_links(&system->run.pool);
    for (;;) {
        uint32_t index = ta_list_pop_front(&actor->deferred, &links);
        if (index == 0u) {
            return;
        }
        ta_envelope_pool_return(&system->run.pool, index);
        if (!all) {
            return;
        }
    }
}

/* Releases and gives back `actor`'s taken input, wherever it is, and
 * forgets it; nothing when it has none. A wake not taken yet stays queued,
 * so this never walks the run queue for it: what it holds (an event's
 * data, origin and operator transition, its text) is released at once,
 * and the bare wake is dropped when it is reached. */
static void ta_run_drop_input(ta_system *system, ta_actor *actor) {
    uint32_t index = actor->input;
    if ((actor->input_flags & TA_ACTOR_INPUT_WAITING) != 0u) {
        ta_run_strip(system, index);
        ta_run_forget_input(actor);
        return;
    }
    if (index == 0u) {
        return;
    }
    if (system->run.slot == index) {
        system->run.slot = 0u;
    } else {
        /* Released, or paused by a filtered call, it waits on the run
         * queue; stopped, it is on no list. */
        (void)ta_envelope_pool_unqueue(&system->run.pool, index);
    }
    ta_run_discard(system, index);
    ta_run_forget_input(actor);
}

void ta_run_session_gone(ta_system *system, ta_actor *actor) {
    ta_run_drop_input(system, actor);
    ta_run_return_deferred(system, actor, true);
}

void ta_run_input_dropped(ta_system *system, ta_actor *actor) {
    ta_run_drop_input(system, actor);
    ta_run_return_deferred(system, actor, false);
}

/* ------------------------------------------------------------------------
 * ta_run
 */

/* One ta_run() call: its parameters, and what is left of its budgets. */
typedef struct {
    ta_system *system;
    const ta_run_params *params;
    uint32_t budget;
    uint32_t actions;
    uint32_t count;
    bool paused;
} ta_run_call;

static void ta_run_outcome(ta_run_call *call, ta_input_outcome outcome) {
    call->params->outcomes[call->count] = outcome;
    call->count += 1u;
}

/* Reports `kind` for envelope `index`, as long as it is the host's input or a fault. */
static void ta_run_report_input(ta_run_call *call, uint32_t index, ta_input_outcome kind) {
    const ta_envelope *envelope = ta_envelope_pool_at(&call->system->run.pool, index);
    kind.actor = envelope->target;
    kind.correlation = envelope->source_seq;
    ta_run_outcome(call, kind);
}

/* Refuses envelope `index` with `code`: reported unless it is a wake, and discarded. */
static void ta_run_refuse(ta_run_call *call, uint32_t index, ta_result code) {
    const ta_envelope *envelope = ta_envelope_pool_at(&call->system->run.pool, index);
    if (envelope->source != TA_RECORD_INPUT_SOURCE_PLATFORM) {
        ta_run_report_input(call, index, (ta_input_outcome){ .kind = TA_INPUT_REFUSED, .code = code });
    }
    ta_run_discard(call->system, index);
}

/* The next envelope the call takes off the run queue: the head, or with a
 * session filter that session's first; 0 when there is none. */
static uint32_t ta_run_next(ta_run_call *call) {
    ta_envelope_pool *pool = &call->system->run.pool;
    ta_actor_id session = call->params->session;
    if (session == 0u) {
        return ta_envelope_pool_dequeue(pool);
    }
    ta_list_links links = ta_envelope_pool_links(pool);
    uint32_t previous = 0u;
    for (uint32_t index = ta_list_head(&pool->run_queue); index != 0u; index = ta_list_next(&links, index)) {
        if (ta_envelope_pool_at(pool, index)->target == session) {
            return ta_envelope_pool_dequeue_after(pool, previous);
        }
        previous = index;
    }
    return 0u;
}

/* Whether the run queue holds an envelope the call could take. */
static bool ta_run_waiting(const ta_run_call *call) {
    const ta_envelope_pool *pool = &call->system->run.pool;
    ta_actor_id session = call->params->session;
    if (session == 0u) {
        return !ta_list_empty(&pool->run_queue);
    }
    ta_list_links links = ta_envelope_pool_links((ta_envelope_pool *)pool);
    for (uint32_t index = ta_list_head(&pool->run_queue); index != 0u; index = ta_list_next(&links, index)) {
        if (ta_envelope_pool_at(pool, index)->target == session) {
            return true;
        }
    }
    return false;
}

/*
 * Binds an external event to `actor`, idle: it becomes the waiting external
 * event (ADR 0069), read from its envelope, with its operator transition,
 * trace parent and time. TA_ERR_INVALID when the operator transition no
 * longer applies; TA_ERR_NOMEM, changing nothing, when an invoked session
 * cannot have the storage its kept event needs.
 */
static ta_result ta_run_bind_event(ta_system *system, ta_actor *actor, uint32_t index) {
    ta_run_record record = ta_run_record_of(system, index);
    if (record.jump != NULL &&
        (record.jump_definition != actor->record.definition ||
         (record.jump->source != TA_STATE_NONE && !ta_actor_state_active(actor, record.jump->source)))) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_interpreter_keep_room(system, actor);
    if (result != TA_OK) {
        return result;
    }
    /* Admission checked the time against every input admitted before it. */
    ta_actor_move_clock(actor, record.time);
    actor->external = TA_EXTERNAL_WAITING;
    actor->macrostep = true;
    ta_actor_touch(actor);
    actor->record.residency = TA_ACTOR_RUNNING;
    return TA_OK;
}

/*
 * Binds an error report to `actor`, idle: its event joins the internal
 * queue, the invocation retires when the report ends it, and a macrostep
 * starts under its trace parent (ADR 0081), which stays in its envelope.
 * TA_ERR_NOT_FOUND when the invocation ended; TA_ERR_NOMEM, changing
 * nothing, when there is no room.
 */
static ta_result ta_run_bind_report(ta_system *system, ta_actor *actor, uint32_t index) {
    ta_run_record record = ta_run_record_of(system, index);
    ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    ta_trace_parent trace = (record.flags & TA_RUN_RECORD_TRACED) != 0u ? record.trace : (ta_trace_parent){ 0 };
    ta_result result = ta_invoke_take_report(system, actor, &(ta_invoke_report_take){
                                                                .invoke = envelope->invoke_instance,
                                                                .name = record.name,
                                                                .data = &envelope->data,
                                                                .ends = (record.flags & TA_RUN_RECORD_ENDS) != 0u,
                                                                .trace = &trace,
                                                            });
    if (result != TA_OK) {
        return result;
    }
    ta_actor_move_clock(actor, record.time);
    return TA_OK;
}

/* Takes envelope `index` as `actor`'s input: it becomes the input the
 * session's macrostep runs, until it completes, and its `due` the
 * macrostep's logical time (ADR 0089 section 9). */
static void ta_run_hold_input(ta_system *system, ta_actor *actor, uint32_t index) {
    ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    envelope->state = TA_ENVELOPE_TAKEN;
    envelope->due = ta_actor_now(system, actor);
    actor->input = index;
    actor->input_flags = 0u;
}

/* Puts the input that could not be bound back at the head: it is taken first next time. */
static ta_result ta_run_put_back(ta_run_call *call, uint32_t index, ta_result result) {
    ta_envelope_pool_push_front(&call->system->run.pool, index);
    return result;
}

/* An input that ended: its envelope, and how it ended. */
typedef struct {
    uint32_t index;
    ta_input_outcome outcome;
} ta_run_ended;

/*
 * Ends `actor`'s input `ended.index`: its outcome, the envelope back, and
 * the session's deferred mail, the first of it, or all of it once the
 * session finished, back in the run queue.
 */
static void ta_run_complete(ta_run_call *call, ta_actor *actor, ta_run_ended ended) {
    ta_system *system = call->system;
    uint32_t index = ended.index;
    ta_input_outcome outcome = ended.outcome;
    ta_run_report_input(call, index, outcome);
    if (system->run.slot == index) {
        system->run.slot = 0u;
    }
    ta_run_discard(system, index);
    if (actor->input == index) {
        ta_run_forget_input(actor);
    }
    ta_run_return_deferred(system, actor, actor->finished);
}

/*
 * Runs `actor`'s macrostep for its input `index` with what is left of the
 * call's budgets, and settles what it came to: ended, paused in the slot,
 * or stopped by a fault.
 */
static void ta_run_input(ta_run_call *call, ta_actor *actor, uint32_t index) {
    ta_system *system = call->system;
    bool limited = call->params->action_budget != 0u;
    ta_interpreter_run_params run = {
        .budget = call->budget,
        .action_budget = limited ? call->actions : 0u,
        .pause_at_microstep_start = call->params->pause_at_microstep_start,
        .unlimited = (actor->input_flags & TA_ACTOR_INPUT_UNLIMITED) != 0u,
    };
    system->run.running = true;
    system->run.current = index;
    ta_result result = ta_interpreter_run(system, actor, &run);
    system->run.running = false;
    system->run.current = 0u;
    call->budget -= run.steps < call->budget ? run.steps : call->budget;
    if (limited) {
        call->actions -= run.actions < call->actions ? run.actions : call->actions;
    }
    if (result != TA_OK && actor->macrostep) {
        /* Stopped where it stands: it keeps its input and holds no slot. */
        ta_run_report_input(call, index, (ta_input_outcome){ .kind = TA_INPUT_FAULTED, .code = result });
        if (system->run.slot == index) {
            system->run.slot = 0u;
        }
        actor->input_flags = (uint8_t)((actor->input_flags & TA_ACTOR_INPUT_IGNORED) | TA_ACTOR_INPUT_STOPPED |
                                       (result == TA_ERR_RUNAWAY ? TA_ACTOR_INPUT_RUNAWAY : 0u));
        return;
    }
    if (result != TA_OK) {
        /* A fault that came as the macrostep ended leaves nothing to
         * continue: the input ended, and its outcome says how, with the
         * fault's code. FAULTED always means the session keeps its input. */
        ta_run_complete(call, actor,
                        (ta_run_ended){ .index = index,
                                        .outcome = { .kind = actor->finished ? TA_INPUT_FINISHED : TA_INPUT_COMPLETED,
                                                     .code = result } });
        return;
    }
    if (run.status == TA_MACROSTEP_PAUSED) {
        call->paused = true;
        if (system->run.slot == 0u || system->run.slot == index) {
            system->run.slot = index;
        } else {
            /* The slot is another session's: a filtered call paused this one,
             * which waits at the head of the run queue. */
            ta_envelope_pool_push_front(&system->run.pool, index);
        }
        return;
    }
    ta_input_outcome_kind kind = run.status == TA_MACROSTEP_FINISHED ? TA_INPUT_FINISHED
                                 : run.ignored                       ? TA_INPUT_IGNORED
                                                                     : TA_INPUT_COMPLETED;
    ta_run_complete(call, actor, (ta_run_ended){ .index = index, .outcome = { .kind = kind } });
}

/*
 * Takes envelope `index`, just off the run queue, and runs it when it is an
 * input to take: refused, dropped or deferred otherwise. TA_ERR_NOMEM when
 * it could not be bound; it is back at the head.
 */
static ta_result ta_run_take(ta_run_call *call, uint32_t index) {
    ta_system *system = call->system;
    ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    ta_actor *actor = ta_actor_lookup(system, envelope->target);
    if (ta_run_is_taken_input(actor, index)) {
        /* Released, or paused by a filtered call: it goes on. */
        ta_run_input(call, actor, index);
        return TA_OK;
    }
    bool wake = envelope->source == TA_RECORD_INPUT_SOURCE_PLATFORM;
    if (actor == NULL) {
        ta_run_refuse(call, index, TA_ERR_NOT_FOUND);
        return TA_OK;
    }
    if (wake) {
        if (!actor->macrostep || actor->input != index) {
            /* Not the wake that starts its macrostep, which began or ended
             * already, or waits for another wake. */
            ta_run_discard(system, index);
            return TA_OK;
        }
    } else if (actor->finished && !actor->macrostep) {
        ta_run_refuse(call, index, TA_ERR_DONE);
        return TA_OK;
    } else if (actor->macrostep) {
        /* Its session is in a macrostep: the input waits for it to end. */
        ta_envelope_pool_defer(&system->run.pool, (ta_envelope_defer){ .deferred = &actor->deferred,
                                                                      .index = index });
        return TA_OK;
    } else {
        ta_result result = envelope->source == TA_RECORD_INPUT_SOURCE_INVOKE_REPORT
                               ? ta_run_bind_report(system, actor, index)
                               : ta_run_bind_event(system, actor, index);
        if (result == TA_ERR_NOMEM) {
            return ta_run_put_back(call, index, result);
        }
        if (result != TA_OK) {
            ta_run_refuse(call, index, result);
            /* It may have been the deferred envelope that returned: the next one takes its place. */
            ta_run_return_deferred(system, actor, false);
            return TA_OK;
        }
    }
    ta_run_hold_input(system, actor, index);
    if (call->params->report_taken) {
        ta_run_report_input(call, index, (ta_input_outcome){ .kind = TA_INPUT_TAKEN });
    }
    ta_run_input(call, actor, index);
    return TA_OK;
}

/* How the call ends when it stops between inputs. */
static ta_run_status ta_run_between(const ta_run_call *call) {
    return call->paused ? TA_RUN_PAUSED : ta_run_waiting(call) ? TA_RUN_MORE : TA_RUN_IDLE;
}

/* What one input can report: TAKEN when asked, and its end. */
static uint32_t ta_run_room(const ta_run_params *params) {
    return params->report_taken ? 2u : 1u;
}

ta_result ta_run(ta_system *system, const ta_run_params *params) {
    if (system == NULL || params == NULL || params->status == NULL || params->outcome_count == NULL ||
        params->budget == 0u || params->outcomes == NULL || params->outcome_capacity < ta_run_room(params)) {
        return TA_ERR_INVALID;
    }
    *params->outcome_count = 0u;
    if (system->run.running) {
        return TA_ERR_BUSY;
    }
    ta_run_call call = { .system = system, .params = params, .budget = params->budget,
                         .actions = params->action_budget };
    uint32_t room = ta_run_room(params);
    ta_result result = TA_OK;
    ta_run_status status = TA_RUN_IDLE;
    for (;;) {
        if (call.paused || call.budget == 0u || (params->action_budget != 0u && call.actions == 0u) ||
            params->outcome_capacity - call.count < room) {
            status = ta_run_between(&call);
            break;
        }
        /* A safe point, before the call takes or continues an input, never
         * after its last: a failed refill leaves what is free. */
        (void)ta_run_refill(system);
        uint32_t slot = system->run.slot;
        ta_actor *paused = slot == 0u ? NULL
                                      : ta_actor_lookup(system, ta_envelope_pool_at(&system->run.pool, slot)->target);
        if (paused != NULL && (params->session == 0u || params->session == paused->record.id)) {
            ta_run_input(&call, paused, slot);
            continue;
        }
        uint32_t index = ta_run_next(&call);
        if (index == 0u) {
            status = call.paused ? TA_RUN_PAUSED : TA_RUN_IDLE;
            break;
        }
        result = ta_run_take(&call, index);
        if (result != TA_OK) {
            status = TA_RUN_MORE;
            break;
        }
    }
    *params->outcome_count = call.count;
    *params->status = status;
    return result;
}

bool ta_run_has_mail(const ta_system *system, const ta_actor *actor) {
    if (!ta_list_empty(&actor->deferred)) {
        return true;
    }
    const ta_envelope_pool *pool = &system->run.pool;
    ta_list_links links = ta_envelope_pool_links((ta_envelope_pool *)pool);
    for (uint32_t index = ta_list_head(&pool->run_queue); index != 0u; index = ta_list_next(&links, index)) {
        const ta_envelope *envelope = ta_envelope_pool_at(pool, index);
        if (envelope->target == actor->record.id && envelope->source != TA_RECORD_INPUT_SOURCE_PLATFORM &&
            !ta_run_is_taken_input(actor, index)) {
            return true;
        }
    }
    return false;
}

ta_result ta_actor_release(ta_system *system, ta_actor_id id) {
    if (system == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if ((actor->input_flags & TA_ACTOR_INPUT_STOPPED) == 0u) {
        return TA_ERR_INVALID;
    }
    bool runaway = (actor->input_flags & TA_ACTOR_INPUT_RUNAWAY) != 0u;
    actor->input_flags = (uint8_t)((actor->input_flags & TA_ACTOR_INPUT_IGNORED) |
                                   (runaway ? TA_ACTOR_INPUT_UNLIMITED : 0u));
    ta_envelope_pool_enqueue(&system->run.pool, actor->input);
    return TA_OK;
}

ta_result ta_system_withdraw_input(ta_system *system, uint64_t correlation) {
    if (system == NULL || correlation == 0u) {
        return TA_ERR_INVALID;
    }
    ta_envelope_pool *pool = &system->run.pool;
    for (uint32_t index = 1u; index <= pool->items.high_water; index += 1u) {
        ta_envelope *envelope = ta_envelope_pool_at(pool, index);
        if (envelope == NULL || envelope->state != TA_ENVELOPE_RUNNABLE || envelope->source_seq != correlation ||
            envelope->source == TA_RECORD_INPUT_SOURCE_PLATFORM) {
            continue;
        }
        ta_actor *actor = ta_actor_lookup(system, envelope->target);
        if (ta_run_is_taken_input(actor, index)) {
            continue; /* taken already: its session runs it */
        }
        bool queued = ta_envelope_pool_unqueue(pool, index) == TA_OK;
        if (!queued && actor != NULL) {
            ta_list_links links = ta_envelope_pool_links(pool);
            (void)ta_list_unlink(&actor->deferred, &links, index);
        }
        ta_run_discard(system, index);
        if (queued && actor != NULL && !actor->macrostep) {
            /* It may have been the deferred envelope that returned: the next one takes its place. */
            ta_run_return_deferred(system, actor, false);
        }
        return TA_OK;
    }
    return TA_ERR_NOT_FOUND;
}

#include "run.h"
#include "../include/tinyactors.h"
#include "actor.h"
#include "chunks.h"
#include "clock.h"
#include "deadline.h"
#include "envelope.h"
#include "event.h"
#include "generated/ring_records.h"
#include "interpreter.h"
#include "invoke.h"
#include "list.h"
#include "memory.h"
#include "name_area.h"
#include "hash.h"
#include "quota.h"
#include "routing.h"
#include "session.h"
#include "system.h"
#include "trace.h"
#include "waiting.h"

/* ------------------------------------------------------------------------
 * An input's text: one binary field, the name as text when it is not
 * interned, and the trace parent behind them when the input is traced
 * (see the file comment)
 */

/* The field holds an operator transition and its definition. */
#define TA_RUN_RECORD_JUMP 0x02u
/* A report that retires its invocation. */
#define TA_RUN_RECORD_ENDS 0x04u
/* The bytes of name the run's name buffer holds from system creation: the
 * longest name a name-area field holds, so the buffer never grows, and a
 * macrostep that holds a name never allocates (ADR 0102). */
#define TA_RUN_NAME_ROOM TA_NAME_AREA_FIELD_MAX
/* The event's name is not interned: it is the text field after the record. */
#define TA_RUN_RECORD_NAME_TEXT 0x08u

/* What an input keeps in its first field, unpacked. */
typedef struct {
    ta_event_name_id name;
    uint8_t flags;
    ta_origin origin;
    ta_time time;
    /* The family of the trace parent's cause, which the trace parent field
     * leaves out (name_area.h); 0 when untraced. */
    ta_atom cause_family;
    ta_operator_jump *jump;
    ta_definition_id jump_definition;
} ta_run_record;

/* The most bytes a packed record takes. */
#define TA_RUN_RECORD_BYTES                                                                                     \
    (sizeof(ta_event_name_id) + 1u + sizeof(ta_origin) + sizeof(ta_time) + sizeof(ta_atom) +                  \
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
    ta_run_pack(&packer, &record->cause_family, sizeof record->cause_family);
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
 * reads as all zero. The text was written by ta_run_write_text, so its
 * first field is a packed record.
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
    ta_run_unpack(&packer, &record.cause_family, sizeof record.cause_family);
    if ((record.flags & TA_RUN_RECORD_JUMP) != 0u) {
        ta_run_unpack(&packer, &record.jump, sizeof record.jump);
        ta_run_unpack(&packer, &record.jump_definition, sizeof record.jump_definition);
    }
    return record;
}

/* The trace parent envelope `index` carries at the end of its text, all
 * zero for none (name_area.h), with its cause's family from the record. */
static ta_trace_parent ta_run_trace_of(const ta_system *system, uint32_t index) {
    const ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    ta_trace_parent parent = { 0 };
    if ((envelope->flags & TA_ENVELOPE_FLAG_TRACED) != 0u &&
        ta_name_area_trace_parent(&system->run.names, envelope->text, &parent) == TA_OK) {
        parent.cause.family = ta_run_record_of(system, index).cause_family;
    }
    return parent;
}

/* The length of the name envelope `index` keeps as text, 0 when its name is
 * interned. Never allocates. */
static uint32_t ta_run_name_length(const ta_system *system, uint32_t index) {
    const ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    if (envelope->text.first == 0u ||
        (ta_run_record_of(system, index).flags & TA_RUN_RECORD_NAME_TEXT) == 0u) {
        return 0u;
    }
    ta_name_area_reader reader = ta_name_area_reader_of(envelope->text);
    uint32_t length = 0u;
    if (ta_name_area_next(&system->run.names, &reader, &length) != TA_OK ||
        ta_name_area_next(&system->run.names, &reader, &length) != TA_OK) {
        return 0u;
    }
    return length;
}

/* The name envelope `index` keeps as text, read into the run's name buffer,
 * valid until the buffer is read into again or grows; empty when its name
 * is interned or does not fit the buffer (ta_run_take checks it fits). */
static ta_str ta_run_name_text(const ta_system *system, uint32_t index) {
    uint32_t length = ta_run_name_length(system, index);
    if (length == 0u || length > system->run.name_capacity) {
        return (ta_str){ 0 };
    }
    const ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    ta_name_area_reader reader = ta_name_area_reader_of(envelope->text);
    uint32_t skip = 0u;
    (void)ta_name_area_next(&system->run.names, &reader, &skip);
    (void)ta_name_area_next(&system->run.names, &reader, &length);
    uint32_t got = 0u;
    while (got < length) {
        ta_bytes span = ta_name_area_read(&system->run.names, &reader, length - got);
        ta_mem_copy(system->run.name_buffer + got, span.ptr, span.len);
        got += span.len;
    }
    return (ta_str){ .ptr = system->run.name_buffer, .len = length };
}

ta_str ta_run_input_name(const ta_system *system, ta_event_name_id name) {
    if ((name & TA_RUN_NAME_KEPT) != 0u && ta_run_name_is_input(name)) {
        /* A kept event's name, in its session's own queue (ADR 0100). */
        const ta_actor_slot *slot = ta_actor_slot_at(system, name & ~(TA_RUN_NAME_INPUT | TA_RUN_NAME_KEPT));
        const ta_internal_queue *queue = slot == NULL || slot->actor == NULL ? NULL : slot->actor->internal;
        if (queue == NULL || !queue->has_kept || queue->kept_text == NULL) {
            return (ta_str){ 0 };
        }
        return (ta_str){ .ptr = queue->kept_text, .len = queue->kept_len };
    }
    uint32_t index = name & ~TA_RUN_NAME_INPUT;
    if (!ta_run_name_is_input(name) || index == 0u || index > system->run.pool.items.high_water) {
        return (ta_str){ 0 };
    }
    return ta_run_name_text(system, index);
}

ta_result ta_run_name_room(ta_system *system) {
    ta_run_state *run = &system->run;
    if (run->name_capacity >= run->name_longest) {
        return TA_OK;
    }
    char *grown = ta_mem_alloc(system->allocator, run->name_longest, 1u);
    if (grown == NULL) {
        return TA_ERR_NOMEM;
    }
    if (run->name_buffer != NULL) {
        ta_mem_free(system->allocator, run->name_buffer,
                    &(ta_allocator_free_params){ .size = run->name_capacity, .alignment = 1u });
    }
    run->name_buffer = grown;
    run->name_capacity = run->name_longest;
    return TA_OK;
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
/* Releases the references envelope `index` holds (its data and content,
 * an event's origin and operator transition) and leaves its text. A kept
 * envelope holds none: its record's were released when it was kept. */
static void ta_run_release_references(ta_system *system, uint32_t index) {
    if (system->run.event_input == index) {
        system->run.event_input = 0u;
        system->run.event = (ta_event){ 0 };
    }
    ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    if (envelope->state != TA_ENVELOPE_KEPT) {
        ta_run_record record = ta_run_record_of(system, index);
        ta_origin_release(&record.origin);
        ta_run_jump_free(system, record.jump);
    }
    ta_value_release(&envelope->data);
    ta_value_release(&envelope->content);
    envelope->data = (ta_value){ 0 };
    envelope->content = (ta_value){ 0 };
}

static void ta_run_strip(ta_system *system, uint32_t index) {
    ta_run_release_references(system, index);
    ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    envelope->flags &= (uint8_t)~TA_ENVELOPE_FLAG_TRACED;
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
    result = ta_name_area_init(&system->run.names, &(ta_name_area_params){
                                                       .allocator = system->allocator,
                                                       .bytes = system->params[TA_PARAM_NAME_AREA_BYTES],
                                                       .watermark_percent = watermark,
                                                       .reserve = chunks < TA_RUN_RESERVE ? chunks : TA_RUN_RESERVE,
                                                   });
    if (result != TA_OK) {
        return result;
    }
    result = ta_waiting_init(&system->run.waiting,
                             &(ta_waiting_params){ .allocator = system->allocator,
                                                   .bound = bound,
                                                   .reserve = bound < TA_RUN_RESERVE ? bound : TA_RUN_RESERVE });
    if (result == TA_OK) {
        /* Room for every name a field holds, so taking or holding one,
         * or admitting it when nothing more can be allocated, needs no
         * growth. */
        system->run.name_longest = TA_RUN_NAME_ROOM;
        result = ta_run_name_room(system);
    }
    return result;
}

void ta_run_destroy(ta_system *system) {
    ta_envelope_pool *pool = &system->run.pool;
    for (uint32_t index = 1u; index <= pool->items.high_water; index += 1u) {
        ta_envelope *envelope = ta_envelope_pool_at(pool, index);
        /* Every session is gone, and gave its taken input back with the
         * mail its macrostep sent; a runnable envelope, a taken one no
         * session names, or mail of a macrostep running at teardown, still
         * holds its own. */
        if (envelope != NULL && (envelope->state == TA_ENVELOPE_RUNNABLE || envelope->state == TA_ENVELOPE_TAKEN ||
                                 envelope->state == TA_ENVELOPE_OUTGOING || envelope->state == TA_ENVELOPE_WAITING)) {
            ta_run_record record = ta_run_record_of(system, index);
            ta_origin_release(&record.origin);
            ta_run_jump_free(system, record.jump);
            ta_value_release(&envelope->data);
            ta_value_release(&envelope->content);
        }
    }
    ta_waiting_destroy(&system->run.waiting);
    ta_name_area_destroy(&system->run.names);
    ta_envelope_pool_destroy(pool);
    if (system->run.name_buffer != NULL) {
        ta_mem_free(system->allocator, system->run.name_buffer,
                    &(ta_allocator_free_params){ .size = system->run.name_capacity, .alignment = 1u });
        system->run.name_buffer = NULL;
        system->run.name_capacity = 0u;
    }
    system->run.slot = 0u;
    system->run.event_input = 0u;
    system->run.outgoing = TA_LIST_EMPTY;
}

/* What a refill answered, for the pool and for the name area. */
typedef struct {
    ta_result pool;
    ta_result names;
} ta_run_refilled;

/* Keeps the reserve of free envelopes, chunks and waiting entries, and room
 * in the waiting entries' table, unless a macrostep runs: a safe point. A
 * refill that fails leaves what is free, and says so for the envelopes and
 * the chunks; one that finds no room for an entry answers the mail's take. */
static ta_run_refilled ta_run_refill(ta_system *system) {
    if (system->run.running) {
        return (ta_run_refilled){ .pool = TA_OK, .names = TA_OK };
    }
    ta_run_refilled refilled = { .pool = ta_envelope_pool_refill(&system->run.pool),
                                 .names = ta_name_area_refill(&system->run.names, 0u) };
    /* The deadline index keeps room for every envelope, so a delayed send
     * never allocates (ADR 0101). */
    if (refilled.pool == TA_OK) {
        refilled.pool = ta_deadline_grow(system);
    }
    (void)ta_waiting_refill(&system->run.waiting);
    (void)ta_run_name_room(system);
    return refilled;
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

/* As ta_run_envelope_short, for admitted text of `need` chunks that `names`
 * refused. */
static ta_result ta_run_names_short(const ta_name_area *names, ta_run_refusal refusal, uint32_t need) {
    if (refusal.result != TA_ERR_FULL || refusal.refill != TA_ERR_NOMEM) {
        return refusal.result;
    }
    ta_quota_request request = { .amount = need, .keep = names->quota.capacity - names->watermark };
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
    /* The name as text, written after the record; empty for none. */
    ta_str name_text;
    /* What its macrostep runs under, or null. */
    const ta_trace_parent *trace;
    /* With `target` 0: the destination it waits for, or null. */
    const ta_waiting_key *wait;
} ta_run_admission;

/* The text ta_run_write_text writes into an envelope. */
typedef struct {
    /* The envelope, which has no text yet. */
    uint32_t index;
    /* Its first field. */
    const ta_run_record *record;
    /* The trace parent its macrostep runs under, written behind the record
     * as the text's last field when it has a trace ID; null for none. */
    const ta_trace_parent *trace;
    /* What the refill before it answered. */
    ta_run_refilled refilled;
    /* Admitted input, held to the name area's watermark. */
    bool admission;
    /* The event's name when it is not interned, written as the field after
     * the record; empty for an interned name. */
    ta_str name_text;
} ta_run_text;

/*
 * Writes the record, and the trace parent when there is one, as the text of
 * envelope `text->index`, and marks it traced. All or nothing: TA_ERR_FULL,
 * or TA_ERR_NOMEM when only a failed refill left too few chunks free,
 * changing nothing. Never allocates.
 */
static ta_result ta_run_write_text(ta_system *system, const ta_run_text *text) {
    ta_name_area *names = &system->run.names;
    bool traced = text->trace != NULL && !ta_trace_context_empty(&text->trace->context);
    ta_run_record record = *text->record;
    record.cause_family = traced ? text->trace->cause.family : 0u;
    bool named = text->name_text.len != 0u;
    if (named) {
        record.flags |= TA_RUN_RECORD_NAME_TEXT;
    }
    uint8_t bytes[TA_RUN_RECORD_BYTES];
    uint32_t length = ta_run_record_pack(&record, bytes);
    ta_chunk_writer writer = ta_chunk_writer_new();
    ta_result result = ta_name_area_write(names, &(ta_name_area_write_params){
                                                     .writer = &writer,
                                                     .text = { .ptr = bytes, .len = length },
                                                     .admission = text->admission,
                                                 });
    if (result == TA_OK && named) {
        result = ta_name_area_write(names, &(ta_name_area_write_params){
                                               .writer = &writer,
                                               .text = { .ptr = (const uint8_t *)text->name_text.ptr,
                                                         .len = text->name_text.len },
                                               .admission = text->admission,
                                           });
        if (result != TA_OK) {
            ta_name_area_free(names, &writer.chain);
        }
    }
    if (result == TA_OK && traced) {
        result = ta_name_area_write_trace_parent(names, &(ta_name_area_trace_params){
                                                            .writer = &writer,
                                                            .parent = text->trace,
                                                            .admission = text->admission,
                                                        });
        if (result != TA_OK) {
            ta_name_area_free(names, &writer.chain);
        }
    }
    if (result != TA_OK) {
        uint32_t need = ta_name_area_field_need(names, 0u, length);
        uint32_t written = TA_NAME_AREA_PREFIX_BYTES + length;
        if (named) {
            need += ta_name_area_field_need(names, written, text->name_text.len);
            written += TA_NAME_AREA_PREFIX_BYTES + text->name_text.len;
        }
        if (traced) {
            need += ta_name_area_field_need(names, written, TA_NAME_AREA_TRACE_PARENT_BYTES);
        }
        ta_run_refusal refusal = { .refill = text->refilled.names, .result = result };
        return text->admission ? ta_run_names_short(names, refusal, need)
               : result == TA_ERR_FULL && text->refilled.names == TA_ERR_NOMEM ? TA_ERR_NOMEM
                                                                                : result;
    }
    ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, text->index);
    envelope->text = writer.chain;
    if (named && text->name_text.len > system->run.name_longest) {
        system->run.name_longest = text->name_text.len;
    }
    if (traced) {
        envelope->flags |= TA_ENVELOPE_FLAG_TRACED;
    }
    return TA_OK;
}

static void ta_run_waiting_unref(ta_system *system, uint32_t entry);
static void ta_run_arrive(ta_system *system, uint32_t index);

/*
 * Takes an envelope below the watermark, writes the record into its text,
 * fills it and appends it to the run queue. Whoever calls it retains what
 * the envelope holds once it answers TA_OK. TA_ERR_FULL changes nothing.
 */
static ta_result ta_run_admit(ta_system *system, const ta_run_admission *admission, uint32_t *out) {
    ta_run_refilled refilled = ta_run_refill(system);
    uint32_t entry = 0u;
    ta_result result = TA_OK;
    /* Admission is a safe point: the name area makes room for a long name,
     * and the name buffer grows to take it, so taking the input never
     * needs to. */
    if (admission->name_text.len != 0u && !system->run.running) {
        uint32_t need = ta_name_area_field_need(&system->run.names, TA_NAME_AREA_PREFIX_BYTES + TA_RUN_RECORD_BYTES,
                                                admission->name_text.len);
        refilled.names = ta_name_area_refill(&system->run.names, need);
    }
    if (admission->name_text.len > system->run.name_capacity && !system->run.running) {
        uint32_t longest = system->run.name_longest;
        system->run.name_longest = admission->name_text.len;
        result = ta_run_name_room(system);
        system->run.name_longest = longest > admission->name_text.len ? longest : admission->name_text.len;
        if (result != TA_OK) {
            system->run.name_longest = longest;
            return result;
        }
    }
    if (admission->wait != NULL) {
        result = ta_waiting_take(&system->run.waiting, admission->wait, &entry);
        if (result != TA_OK) {
            return result;
        }
        /* Counted now, so a failure below frees a new entry again. */
        ta_waiting_at(&system->run.waiting, entry)->count += 1u;
    }
    uint32_t index = 0u;
    result = ta_envelope_pool_admit(&system->run.pool, &index);
    if (result != TA_OK) {
        if (entry != 0u) {
            ta_run_waiting_unref(system, entry);
        }
        return ta_run_envelope_short(&system->run.pool, (ta_run_refusal){ .refill = refilled.pool, .result = result },
                                     true);
    }
    result = ta_run_write_text(system, &(ta_run_text){ .index = index,
                                                       .record = admission->record,
                                                       .trace = admission->trace,
                                                       .refilled = refilled,
                                                       .admission = true,
                                                       .name_text = admission->name_text });
    if (result != TA_OK) {
        ta_envelope_pool_give(&system->run.pool, index);
        if (entry != 0u) {
            ta_run_waiting_unref(system, entry);
        }
        return result;
    }
    ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    envelope->source = admission->source;
    envelope->type = admission->type;
    envelope->target = entry != 0u ? entry : admission->target;
    envelope->send_id = admission->send_id;
    envelope->invoke_instance = admission->invoke;
    envelope->source_seq = admission->correlation;
    *out = index;
    if (entry != 0u) {
        envelope->flags |= TA_ENVELOPE_FLAG_WAITING_TARGET;
        ta_run_arrive(system, index);
        return TA_OK;
    }
    ta_envelope_pool_enqueue(&system->run.pool, index);
    return TA_OK;
}

ta_result ta_run_admit_event(ta_system *system, const ta_run_event *input) {
    const ta_event *event = input->event;
    ta_run_record record = { .name = input->name_text.len != 0u ? 0u : event->name,
                             .origin = event->origin,
                             .time = input->time };
    if (input->jump != NULL) {
        record.flags |= TA_RUN_RECORD_JUMP;
        record.jump = input->jump;
        record.jump_definition = input->actor->record.definition;
    }
    uint32_t index = 0u;
    ta_result result = ta_run_admit(system, &(ta_run_admission){ .source = TA_RECORD_INPUT_SOURCE_H2C,
                                                                 .type = (uint8_t)event->type,
                                                                 .target = input->actor != NULL ? input->actor->record.id
                                                                                                : 0u,
                                                                 .wait = input->actor != NULL ? NULL : input->wait,
                                                                 .send_id = event->send_id,
                                                                 .invoke = event->invoke_id,
                                                                 .correlation = input->correlation,
                                                                 .record = &record,
                                                                 .name_text = input->name_text,
                                                                 .trace = input->trace },
                                    &index);
    if (result != TA_OK) {
        return result;
    }
    /* The envelope holds the event's references from here on: the text
     * holds the origin, `data` the data. */
    ta_event held;
    ta_event_retain(event, &held);
    ta_envelope_pool_at(&system->run.pool, index)->data = held.data;
    if (input->actor != NULL) {
        ta_actor_note_admitted(input->actor, input->time);
    }
    return TA_OK;
}

ta_result ta_run_admit_report(ta_system *system, const ta_run_report *report) {
    ta_run_record record = { .origin = { .kind = TA_ORIGIN_NONE }, .time = report->time,
                             .flags = report->ends ? TA_RUN_RECORD_ENDS : 0u };
    uint32_t index = 0u;
    ta_result result = ta_run_admit(system, &(ta_run_admission){ .source = TA_RECORD_INPUT_SOURCE_INVOKE_REPORT,
                                                                 .type = (uint8_t)TA_EVENT_PLATFORM,
                                                                 .target = report->actor->record.id,
                                                                 .invoke = report->invoke,
                                                                 .correlation = report->correlation,
                                                                 .record = &record,
                                                                 .name_text = report->name_text,
                                                                 .trace = report->trace },
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
    bool traced = wake->trace != NULL && !ta_trace_context_empty(&wake->trace->context);
    if (traced || wake->event != NULL || wake->name_text.len != 0u) {
        ta_run_record record = { .origin = { .kind = TA_ORIGIN_NONE } };
        if (wake->event != NULL) {
            record.name = wake->name_text.len != 0u ? 0u : wake->event->name;
            record.origin = wake->event->origin;
        }
        if (wake->jump != NULL) {
            record.flags |= TA_RUN_RECORD_JUMP;
            record.jump = wake->jump;
        }
        result = ta_run_write_text(system, &(ta_run_text){ .index = index,
                                                           .record = &record,
                                                           .trace = wake->trace,
                                                           .refilled = refilled,
                                                           .admission = wake->admission,
                                                           .name_text = wake->name_text });
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
 * Names a kept event holds (ADR 0100)
 */

ta_result ta_run_hold_name(ta_system *system, ta_str name, ta_event_name_id *out) {
    uint32_t index = 0u;
    ta_result result = ta_run_wake_reserve(system, &(ta_run_wake){ .name_text = name }, &index);
    if (result != TA_OK) {
        return result;
    }
    /* The event may be read in the macrostep that holds it, before any
     * refill: the name buffer grows to the name now. */
    result = ta_run_name_room(system);
    if (result != TA_OK) {
        ta_run_discard(system, index);
        return result;
    }
    ta_envelope_pool_at(&system->run.pool, index)->state = TA_ENVELOPE_KEPT;
    *out = TA_RUN_NAME_INPUT | index;
    return TA_OK;
}

ta_result ta_run_hold_copy(ta_system *system, ta_event_name_id name, ta_event_name_id *out) {
    if (!ta_run_name_is_input(name)) {
        *out = name;
        return TA_OK;
    }
    if (!system->run.running) {
        /* Refilled now, the name buffer the text is read into does not move
         * while the text is written (ta_run_name_room). */
        (void)ta_run_refill(system);
    }
    return ta_run_hold_name(system, ta_run_input_name(system, name), out);
}

void ta_run_release_name(ta_system *system, ta_event_name_id name) {
    if (ta_run_name_is_input(name) && (name & TA_RUN_NAME_KEPT) == 0u) {
        ta_run_discard(system, name & ~TA_RUN_NAME_INPUT);
    }
}

/* ------------------------------------------------------------------------
 * Mail between sessions, and mail that waits for its target's load
 */

/* One fewer envelope names waiting entry `entry`: it is freed once none
 * does, unless the host was asked about it and has not answered. */
static void ta_run_waiting_unref(ta_system *system, uint32_t entry) {
    ta_waiting *waiting = &system->run.waiting;
    ta_waiting_entry *held = ta_waiting_at(waiting, entry);
    held->count -= 1u;
    bool asked = (held->flags & (TA_WAITING_OWED | TA_WAITING_REPORTED)) != 0u &&
                 (held->flags & TA_WAITING_RETIRED) == 0u;
    if (held->count == 0u && !asked) {
        ta_waiting_free(waiting, entry);
    }
}

/* The fault core gives mail whose target it knows is gone or finished. */
static ta_fault ta_run_lost_fault(ta_result code) {
    static const char done[] = "the target session has finished";
    static const char gone[] = "no session has that ID or name";
    return code == TA_ERR_DONE ? (ta_fault){ .code = TA_ERR_DONE, .message = { .ptr = done, .len = sizeof done - 1u } }
                               : (ta_fault){ .code = TA_ERR_NOT_FOUND,
                                             .message = { .ptr = gone, .len = sizeof gone - 1u } };
}

/*
 * Mail envelope `index`, on no list, cannot be delivered: it is released
 * and given back, and its sender, when it is still resident, gets
 * error.communication with the send's ID and `fault`, whose data it
 * retains (ADR 0099, recycling). Outside a macrostep only.
 */
static void ta_run_recycle(ta_system *system, uint32_t index, const ta_fault *fault) {
    ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    uint64_t from = envelope->from;
    ta_send_id send_id = envelope->send_id;
    ta_trace_parent trace = ta_run_trace_of(system, index);
    /* Given back first, so raising the error finds an envelope free. */
    ta_run_discard(system, index);
    /* A session's ID is the runtime ID it was spawned with, until it is
     * paged and loaded again (ADR 0066). */
    ta_actor *sender = from == 0u ? NULL : ta_actor_lookup(system, from);
    if (sender == NULL || sender->session_id != from) {
        const ta_session_node *node = from == 0u ? NULL : ta_session_find(&system->sessions, from);
        sender = node != NULL ? node->actor : NULL;
    }
    if (sender == NULL) {
        return;
    }
    ta_fault error = *fault;
    ta_value_retain(&error.data);
    ta_interpreter_send_failed(system, sender,
                               &(ta_send_failure){ .send_id = send_id, .fault = &error, .trace = &trace });
}

/*
 * Mail envelope `index`, which names its waiting entry, arrives there:
 * outside a macrostep, at once; inside one, when the macrostep ends. It
 * joins the entry's list and the entry is owed a load request; or, when
 * the host answered the entry meanwhile, it follows the answer.
 */
static void ta_run_arrive(ta_system *system, uint32_t index) {
    ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    uint32_t entry = (uint32_t)envelope->target;
    ta_waiting *waiting = &system->run.waiting;
    ta_waiting_entry *held = ta_waiting_at(waiting, entry);
    if ((held->flags & TA_WAITING_RETIRED) == 0u) {
        envelope->state = TA_ENVELOPE_WAITING;
        ta_list_links links = ta_envelope_pool_links(&system->run.pool);
        ta_list_push_back(&held->list, &links, index);
        ta_waiting_owe(waiting, entry);
        return;
    }
    ta_actor *actor = held->settled_to == 0u ? NULL : ta_actor_lookup(system, held->settled_to);
    ta_result failed = held->failed_code;
    ta_run_waiting_unref(system, entry);
    if (actor != NULL) {
        envelope->target = actor->record.id;
        envelope->flags &= (uint8_t)~TA_ENVELOPE_FLAG_WAITING_TARGET;
        ta_envelope_pool_enqueue(&system->run.pool, index);
        return;
    }
    ta_fault fault = ta_run_lost_fault(failed == TA_OK ? TA_ERR_NOT_FOUND : failed);
    ta_run_recycle(system, index, &fault);
}

ta_result ta_run_post_mail(ta_system *system, const ta_run_mail *mail, ta_record_capacity_resource *short_of) {
    ta_run_refilled refilled = ta_run_refill(system);
    uint32_t entry = 0u;
    ta_result result = TA_OK;
    if (mail->target == NULL) {
        result = ta_waiting_take(&system->run.waiting, mail->wait, &entry);
        if (result != TA_OK) {
            *short_of = TA_RECORD_CAPACITY_RESOURCE_POOL;
            return result;
        }
        /* Counted now, so a failure below frees a new entry again. */
        ta_waiting_at(&system->run.waiting, entry)->count += 1u;
    }
    uint32_t index = 0u;
    result = ta_envelope_pool_take(&system->run.pool, &index);
    if (result != TA_OK) {
        if (entry != 0u) {
            ta_run_waiting_unref(system, entry);
        }
        *short_of = TA_RECORD_CAPACITY_RESOURCE_POOL;
        return ta_run_envelope_short(&system->run.pool, (ta_run_refusal){ .refill = refilled.pool, .result = result },
                                     false);
    }
    const ta_event *event = mail->event;
    /* A name kept as text is copied into the new envelope's text. */
    ta_str name_text = ta_run_name_is_input(event->name) ? ta_run_input_name(system, event->name) : mail->name_text;
    ta_run_record record = { .name = name_text.len != 0u ? 0u : event->name,
                             .origin = event->origin,
                             .time = ta_clock_now(&system->clock) };
    result = ta_run_write_text(system, &(ta_run_text){
                                           .index = index, .record = &record, .trace = mail->trace,
                                           .refilled = refilled, .name_text = name_text });
    if (result != TA_OK) {
        ta_envelope_pool_give(&system->run.pool, index);
        if (entry != 0u) {
            ta_run_waiting_unref(system, entry);
        }
        *short_of = TA_RECORD_CAPACITY_RESOURCE_NAMES;
        return result;
    }
    /* The envelope holds the event's references from here on: the text
     * holds the origin, `data` the data. */
    ta_event held;
    ta_event_retain(event, &held);
    ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
    envelope->source = mail->source;
    envelope->type = (uint8_t)event->type;
    envelope->target = entry != 0u ? entry : mail->target->record.id;
    if (entry != 0u) {
        envelope->flags |= TA_ENVELOPE_FLAG_WAITING_TARGET;
    }
    envelope->from = mail->from;
    envelope->send_id = event->send_id;
    envelope->invoke_instance = event->invoke_id;
    envelope->data = held.data;
    if (system->run.running) {
        ta_envelope_pool_post(&system->run.pool,
                              (ta_envelope_post){ .outgoing = &system->run.outgoing, .index = index });
    } else if (entry != 0u) {
        ta_run_arrive(system, index);
    } else {
        ta_envelope_pool_enqueue(&system->run.pool, index);
    }
    return TA_OK;
}

/* The macrostep that sent the outgoing list `outgoing` ended: mail for a
 * target that is not resident arrives at its waiting entry, and the rest
 * joins the run queue, in send order. */
static void ta_run_deliver(ta_system *system, ta_list *outgoing) {
    ta_list_links links = ta_envelope_pool_links(&system->run.pool);
    uint32_t previous = 0u;
    uint32_t index = ta_list_head(outgoing);
    while (index != 0u) {
        uint32_t next = ta_list_next(&links, index);
        if ((ta_envelope_pool_at(&system->run.pool, index)->flags & TA_ENVELOPE_FLAG_WAITING_TARGET) != 0u) {
            (void)ta_list_remove_after(outgoing, &links, previous);
            ta_run_arrive(system, index);
        } else {
            previous = index;
        }
        index = next;
    }
    ta_envelope_pool_deliver(&system->run.pool, outgoing);
}

/* The outgoing list parked in taken input `input`, which no longer holds
 * it: the head is the input's `owner_next`, the tail the head's. */
static ta_list ta_run_unpark(ta_system *system, uint32_t input) {
    ta_envelope *taken = ta_envelope_pool_at(&system->run.pool, input);
    uint32_t head = taken->owner_next;
    taken->owner_next = 0u;
    if (head == 0u) {
        return TA_LIST_EMPTY;
    }
    ta_envelope *first = ta_envelope_pool_at(&system->run.pool, head);
    ta_list list = { .head = head, .tail = first->owner_next };
    first->owner_next = 0u;
    return list;
}

/* Parks the running macrostep's outgoing list in its taken input `input`,
 * while the macrostep waits to go on, and leaves the run's list empty. */
static void ta_run_park(ta_system *system, uint32_t input) {
    ta_list *outgoing = &system->run.outgoing;
    uint32_t head = ta_list_head(outgoing);
    ta_envelope_pool_at(&system->run.pool, input)->owner_next = head;
    if (head != 0u) {
        ta_envelope_pool_at(&system->run.pool, head)->owner_next = ta_list_tail(outgoing);
    }
    *outgoing = TA_LIST_EMPTY;
}

/* Gives back the mail parked in taken input `input`, unseen: its macrostep
 * will not end. */
static void ta_run_drop_outgoing(ta_system *system, uint32_t input) {
    ta_list list = ta_run_unpark(system, input);
    ta_list_links links = ta_envelope_pool_links(&system->run.pool);
    for (uint32_t index = ta_list_pop_front(&list, &links); index != 0u; index = ta_list_pop_front(&list, &links)) {
        ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
        uint32_t entry = (envelope->flags & TA_ENVELOPE_FLAG_WAITING_TARGET) != 0u ? (uint32_t)envelope->target : 0u;
        ta_run_discard(system, index);
        if (entry != 0u) {
            ta_run_waiting_unref(system, entry);
        }
    }
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
    return (ta_event){ .name = (record.flags & TA_RUN_RECORD_NAME_TEXT) != 0u ? TA_RUN_NAME_INPUT | input : record.name,
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
    ta_run_drop_outgoing(system, index);
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
    /* Bytes of `params->names` the TAKEN outcomes' names fill. */
    uint32_t names_used;
    /* The names buffer had no room for the next input's name. */
    bool names_full;
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
    ta_result result = ta_interpreter_keep_room(system, actor, index);
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
    ta_trace_parent trace = ta_run_trace_of(system, index);
    ta_result result = ta_invoke_take_report(system, actor, &(ta_invoke_report_take){
                                                                .invoke = envelope->invoke_instance,
                                                                .name = (record.flags & TA_RUN_RECORD_NAME_TEXT) != 0u
                                                                            ? TA_RUN_NAME_INPUT | index
                                                                            : record.name,
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
    system->run.outgoing = ta_run_unpark(system, index);
    system->run.running = true;
    system->run.current = index;
    ta_result result = ta_interpreter_run(system, actor, &run);
    system->run.running = false;
    system->run.current = 0u;
    call->budget -= run.steps < call->budget ? run.steps : call->budget;
    if (limited) {
        call->actions -= run.actions < call->actions ? run.actions : call->actions;
    }
    if ((result != TA_OK && actor->macrostep) || (result == TA_OK && run.status == TA_MACROSTEP_PAUSED)) {
        /* It goes on later: its mail waits with its input, unseen. */
        ta_run_park(system, index);
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
        ta_run_deliver(system, &system->run.outgoing);
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
    /* The macrostep ended, and the sender forgot its input first: its mail
     * joins the run queue, in send order, or waits for its target's load;
     * mail that comes back to the sender at once as error.communication
     * starts a macrostep of its own. */
    ta_run_deliver(system, &system->run.outgoing);
}

/* Whether envelope `index`'s TAKEN outcome carries its event: core made
 * it (mail, a delayed send that fell due), so no host call admitted it. */
static bool ta_run_takes_event(const ta_envelope *envelope) {
    return envelope->source == TA_RECORD_INPUT_SOURCE_MAIL || envelope->source == TA_RECORD_INPUT_SOURCE_TIMER;
}

/* The bytes, its NUL included, the name `name` takes in the names buffer:
 * 0 for none, or for an interned name longer than the buffer can ever
 * hold, which the outcome gives by ID only. Never allocates. */
static uint32_t ta_run_name_bytes(const ta_system *system, ta_event_name_id name) {
    uint32_t needed = 0u;
    (void)ta_event_name_format(system, &(ta_event_name_format_params){ .name = name, .needed = &needed });
    return needed > TA_RUN_NAMES_MIN_BYTES ? 0u : needed;
}

/* Whether the call's names buffer has room for the name envelope
 * `index`'s TAKEN outcome gives. */
static bool ta_run_names_room(const ta_run_call *call, uint32_t index) {
    const ta_envelope *envelope = ta_envelope_pool_at(&call->system->run.pool, index);
    if (!call->params->report_taken || !ta_run_takes_event(envelope)) {
        return true;
    }
    ta_run_record record = ta_run_record_of(call->system, index);
    ta_event_name_id name = (record.flags & TA_RUN_RECORD_NAME_TEXT) != 0u ? TA_RUN_NAME_INPUT | index : record.name;
    return ta_run_name_bytes(call->system, name) <= call->params->name_capacity - call->names_used;
}

/* Writes `name`'s text into the call's names buffer, which
 * ta_run_names_room found room in, and answers it; empty for none. */
static ta_str ta_run_taken_name(ta_run_call *call, ta_event_name_id name) {
    uint32_t bytes = ta_run_name_bytes(call->system, name);
    uint32_t needed = 0u;
    char *at = call->params->names + call->names_used;
    if (bytes == 0u ||
        ta_event_name_format(call->system, &(ta_event_name_format_params){ .name = name,
                                                                           .buffer = at,
                                                                           .capacity = bytes,
                                                                           .needed = &needed }) != TA_OK) {
        return (ta_str){ 0 };
    }
    call->names_used += bytes;
    return (ta_str){ .ptr = at, .len = bytes - 1u };
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
    if ((envelope->flags & TA_ENVELOPE_FLAG_REFUSED) != 0u) {
        /* Posted to an address whose load failed (ta_system_load_failed()). */
        ta_run_refuse(call, index, (ta_result)(int64_t)envelope->due);
        return TA_OK;
    }
    bool wake = envelope->source == TA_RECORD_INPUT_SOURCE_PLATFORM;
    bool mail = envelope->source == TA_RECORD_INPUT_SOURCE_MAIL || envelope->source == TA_RECORD_INPUT_SOURCE_TIMER;
    if (mail && (actor == NULL || (actor->finished && !actor->macrostep))) {
        /* Its target went, or finished, since it was sent: the sender is
         * told, as for a target that cannot be loaded. */
        ta_fault fault = ta_run_lost_fault(actor == NULL ? TA_ERR_NOT_FOUND : TA_ERR_DONE);
        ta_run_recycle(system, index, &fault);
        return TA_OK;
    }
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
        if (ta_run_name_length(system, index) > system->run.name_capacity) {
            /* A restored external event's name, kept as text, does not fit
             * the name buffer the refill before it could not grow. */
            return ta_run_put_back(call, index, TA_ERR_NOMEM);
        }
        if (ta_interpreter_reserve(system, actor) != TA_OK) {
            /* The macrostep must be able to pause without allocating. */
            return ta_run_put_back(call, index, TA_ERR_NOMEM);
        }
    } else if (actor->finished && !actor->macrostep) {
        ta_run_refuse(call, index, TA_ERR_DONE);
        return TA_OK;
    } else if (actor->macrostep || (mail && !actor->published)) {
        /* Its session is in a macrostep: the input waits for it to end. Mail
         * for a session that is not published (prepared, or replaying)
         * waits until it is, so a replay sees only its own inputs. */
        ta_envelope_pool_defer(&system->run.pool, (ta_envelope_defer){ .deferred = &actor->deferred,
                                                                      .index = index });
        return TA_OK;
    } else if (ta_run_name_length(system, index) > system->run.name_capacity) {
        /* Its name, kept as text, does not fit the name buffer the refill
         * before it could not grow. */
        return ta_run_put_back(call, index, TA_ERR_NOMEM);
    } else if (!ta_run_names_room(call, index)) {
        /* Its TAKEN outcome's name does not fit what is left of the names
         * buffer: the next call takes it. */
        call->names_full = true;
        return ta_run_put_back(call, index, TA_OK);
    } else if (ta_interpreter_reserve(system, actor) != TA_OK) {
        /* What a pause needs is allocated here, at a safe point, so the
         * macrostep never allocates (ADR 0102). */
        return ta_run_put_back(call, index, TA_ERR_NOMEM);
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
        ta_input_outcome taken = { .kind = TA_INPUT_TAKEN };
        if (ta_run_takes_event(envelope)) {
            /* Core made it: the host learns its event here, and releases it,
             * and its name as text in the call's names buffer. */
            ta_event event = ta_run_input_event_of(system, index);
            ta_event_retain(&event, &taken.event);
            taken.name = ta_run_taken_name(call, taken.event.name);
            if (ta_run_name_is_input(taken.event.name)) {
                taken.event.name = 0u;
            }
            taken.has_event = true;
        }
        ta_run_report_input(call, index, taken);
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
        params->budget == 0u || params->outcomes == NULL || params->outcome_capacity < ta_run_room(params) ||
        (params->report_taken && (params->names == NULL || params->name_capacity < TA_RUN_NAMES_MIN_BYTES))) {
        return TA_ERR_INVALID;
    }
    *params->outcome_count = 0u;
    if (params->name_used != NULL) {
        *params->name_used = 0u;
    }
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
            params->outcome_capacity - call.count < room || call.names_full) {
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
    if (params->name_used != NULL) {
        *params->name_used = call.names_used;
    }
    ta_time wake = 0;
    bool pending = ta_deadline_next_due(system, &wake);
    if (params->has_wake != NULL) {
        *params->has_wake = pending;
    }
    if (params->wake != NULL) {
        *params->wake = pending ? wake : 0;
    }
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
    /* Mail a paused or stopped macrostep sent it, parked with that
     * macrostep's input and on no queue yet: it names this runtime ID. */
    for (uint32_t index = 1u; index <= pool->items.high_water; index += 1u) {
        const ta_envelope *envelope = ta_envelope_pool_at(pool, index);
        if (envelope->state == TA_ENVELOPE_OUTGOING && envelope->target == actor->record.id &&
            (envelope->flags & TA_ENVELOPE_FLAG_WAITING_TARGET) == 0u) {
            return true;
        }
    }
    return false;
}

bool ta_run_has_unsent_mail(const ta_system *system, const ta_actor *actor) {
    return actor->input != 0u && (actor->input_flags & TA_ACTOR_INPUT_WAITING) == 0u &&
           ta_envelope_pool_at(&system->run.pool, actor->input)->owner_next != 0u;
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

/*
 * The first input admitted with `correlation` and not taken yet: runnable,
 * on the run queue or a deferred list, by arrival; or else waiting for its
 * target's load, the one in the pool's lowest slot, since waiting inputs
 * carry no arrival number. 0 when there is none. Scans the pool (ADR 0099 decision
 * 11), bounded by poolEnvelopes; allocates nothing.
 */
static uint32_t ta_run_find_input(ta_system *system, uint64_t correlation) {
    ta_envelope_pool *pool = &system->run.pool;
    uint32_t found = 0u;
    for (uint32_t index = 1u; index <= pool->items.high_water; index += 1u) {
        const ta_envelope *envelope = ta_envelope_pool_at(pool, index);
        if (envelope == NULL || envelope->source_seq != correlation ||
            envelope->source == TA_RECORD_INPUT_SOURCE_PLATFORM ||
            (envelope->state != TA_ENVELOPE_RUNNABLE && envelope->state != TA_ENVELOPE_WAITING)) {
            continue;
        }
        if (envelope->state == TA_ENVELOPE_RUNNABLE &&
            ta_run_is_taken_input(ta_actor_lookup(system, envelope->target), index)) {
            continue; /* taken already: its session runs it */
        }
        const ta_envelope *best = found == 0u ? NULL : ta_envelope_pool_at(pool, found);
        bool earlier = best == NULL ||
                       (envelope->state == TA_ENVELOPE_RUNNABLE &&
                        (best->state != TA_ENVELOPE_RUNNABLE ||
                         ta_envelope_arrived_before(envelope->heap_index, best->heap_index)));
        if (earlier) {
            found = index;
        }
    }
    return found;
}

ta_result ta_system_withdraw_input(ta_system *system, uint64_t correlation) {
    if (system == NULL || correlation == 0u) {
        return TA_ERR_INVALID;
    }
    ta_envelope_pool *pool = &system->run.pool;
    uint32_t index = ta_run_find_input(system, correlation);
    if (index == 0u) {
        return TA_ERR_NOT_FOUND;
    }
    ta_envelope *envelope = ta_envelope_pool_at(pool, index);
    ta_list_links links = ta_envelope_pool_links(pool);
    if (envelope->state == TA_ENVELOPE_WAITING) {
        /* Its entry, and the load request it is owed or was asked, stay
         * until the host answers (ADR 0099). */
        uint32_t entry = (uint32_t)envelope->target;
        ta_waiting_entry *held = ta_waiting_at(&system->run.waiting, entry);
        (void)ta_list_unlink(&held->list, &links, index);
        ta_run_discard(system, index);
        ta_run_waiting_unref(system, entry);
        return TA_OK;
    }
    ta_actor *actor = ta_actor_lookup(system, envelope->target);
    bool queued = ta_envelope_pool_unqueue(pool, index) == TA_OK;
    if (!queued && actor != NULL) {
        (void)ta_list_unlink(&actor->deferred, &links, index);
    }
    ta_run_discard(system, index);
    if (queued && actor != NULL && !actor->macrostep) {
        /* It may have been the deferred envelope that returned: the next one takes its place. */
        ta_run_return_deferred(system, actor, false);
    }
    return TA_OK;
}

void ta_run_session_published(ta_system *system, ta_actor *actor) {
    ta_run_return_deferred(system, actor, true);
}

/* ------------------------------------------------------------------------
 * The host's answers for targets that are not resident
 */

ta_result ta_system_load_needed(ta_system *system, ta_load_request *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_waiting *waiting = &system->run.waiting;
    uint32_t entry = ta_waiting_next_owed(waiting);
    if (entry == 0u) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_waiting_entry *held = ta_waiting_at(waiting, entry);
    uint32_t first = ta_list_head(&held->list);
    uint32_t count = 0u;
    ta_list_links links = ta_envelope_pool_links(&system->run.pool);
    for (uint32_t index = first; index != 0u; index = ta_list_next(&links, index)) {
        count += 1u;
    }
    *out = (ta_load_request){ .request = ta_waiting_token(waiting, entry),
                              .destination = held->destination,
                              .hint = held->hint,
                              .waiting = count,
                              .trace = first == 0u ? (ta_trace_parent){ 0 } : ta_run_trace_of(system, first) };
    return TA_OK;
}

/* The entry a host's answer names, or 0 when it is stale. */
static uint32_t ta_run_answered(ta_system *system, uint64_t request) {
    return ta_waiting_find_request(&system->run.waiting, request);
}

ta_result ta_system_load_settled(ta_system *system, const ta_load_settled_params *params) {
    if (system == NULL || params == NULL) {
        return TA_ERR_INVALID;
    }
    if (system->run.running) {
        return TA_ERR_BUSY;
    }
    uint32_t entry = ta_run_answered(system, params->request);
    if (entry == 0u) {
        return TA_ERR_STALE;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_waiting *waiting = &system->run.waiting;
    ta_waiting_retire(waiting, entry, actor->record.id, TA_OK);
    ta_list_links links = ta_envelope_pool_links(&system->run.pool);
    ta_waiting_entry *held = ta_waiting_at(waiting, entry);
    for (uint32_t index = ta_list_pop_front(&held->list, &links); index != 0u;
         index = ta_list_pop_front(&held->list, &links)) {
        ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
        envelope->target = actor->record.id;
        envelope->flags &= (uint8_t)~TA_ENVELOPE_FLAG_WAITING_TARGET;
        ta_envelope_pool_enqueue(&system->run.pool, index);
        held->count -= 1u;
    }
    if (held->count == 0u) {
        ta_waiting_free(waiting, entry);
    }
    return TA_OK;
}

ta_result ta_system_load_failed(ta_system *system, const ta_load_failed_params *params) {
    if (system == NULL || params == NULL || params->fault == NULL) {
        return TA_ERR_INVALID;
    }
    if (system->run.running) {
        return TA_ERR_BUSY;
    }
    uint32_t entry = ta_run_answered(system, params->request);
    if (entry == 0u) {
        return TA_ERR_STALE;
    }
    /* A failure the host gave no code is NOT_FOUND, for senders too. */
    ta_fault fault = *params->fault;
    if (fault.code == TA_OK) {
        fault.code = TA_ERR_NOT_FOUND;
    }
    ta_waiting *waiting = &system->run.waiting;
    ta_waiting_retire(waiting, entry, 0u, fault.code);
    ta_list_links links = ta_envelope_pool_links(&system->run.pool);
    ta_list list = ta_waiting_at(waiting, entry)->list;
    ta_waiting_at(waiting, entry)->list = TA_LIST_EMPTY;
    for (uint32_t index = ta_list_pop_front(&list, &links); index != 0u; index = ta_list_pop_front(&list, &links)) {
        ta_waiting_at(waiting, entry)->count -= 1u;
        ta_envelope *envelope = ta_envelope_pool_at(&system->run.pool, index);
        if (envelope->source == TA_RECORD_INPUT_SOURCE_H2C) {
            /* The host's own input is refused, under its correlation, by
             * the next ta_run(): it carries the fault's code in `due`. */
            envelope->flags = (uint8_t)((envelope->flags & ~TA_ENVELOPE_FLAG_WAITING_TARGET) |
                                        TA_ENVELOPE_FLAG_REFUSED);
            envelope->target = 0u;
            envelope->due = (ta_time)(int64_t)fault.code;
            ta_envelope_pool_enqueue(&system->run.pool, index);
            continue;
        }
        ta_run_recycle(system, index, &fault);
    }
    if (ta_waiting_at(waiting, entry)->count == 0u) {
        ta_waiting_free(waiting, entry);
    }
    return TA_OK;
}

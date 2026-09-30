#include "tinyactors_host/inputs.h"

#include "memory.h"


#include "tinyactors_host/allocator.h"
#include "tinyactors_host/hooks.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"
#include "tinyactors_host/wal.h"
#include "allocator_private.h"
#include "inputs_private.h"
#include "scheduler_private.h"
#include "system_private.h"

/* The arenas' block: a turn's wake addresses, or one record's text, usually fit in one. */
#define TAH_INPUTS_BLOCK_BYTES 4096u

size_t tah_record_span(const uint8_t *bytes, size_t length, bool unit) {
    return tah_wal_span(bytes, length, unit);
}

void tah_inputs_init(tah_inputs *inputs, ta_allocator allocator) {
    *inputs = (tah_inputs){ .allocator = allocator };
    tah_vec_init(&inputs->families, allocator, TAH_LAYOUT(ta_str));
    tah_vec_init(&inputs->entries, allocator, TAH_LAYOUT(tah_input_buffer));
    tah_map_init(&inputs->entry_index, allocator);
    tah_vec_init(&inputs->spare, allocator, TAH_LAYOUT(tah_vec));
    tah_map_init(&inputs->actor_seq, allocator);
    tah_map_init(&inputs->seqs, allocator);
    tah_map_init(&inputs->queued_address, allocator);
    tah_map_init(&inputs->mail_count, allocator);
    tah_map_init(&inputs->mail_since, allocator);
    tah_vec_init(&inputs->touched, allocator, TAH_LAYOUT(tah_input_touch));
    tah_map_init(&inputs->touched_index, allocator);
    tah_vec_init(&inputs->held, allocator, TAH_LAYOUT(tah_input_held));
    tah_map_init(&inputs->held_index, allocator);
}

void tah_inputs_set_datamodel(tah_inputs *inputs, ta_datamodel datamodel) {
    inputs->datamodel = datamodel;
}

/* The held first record of `actor`'s open macrostep, or NULL; `index` is its place. */
static tah_input_held *tah_inputs_held_of(const tah_inputs *inputs, ta_actor_id actor, uint32_t *index) {
    uint64_t at = 0;
    if (actor == 0 || inputs->held.count == 0 || !tah_map_get(&inputs->held_index, actor, &at)) {
        return NULL;
    }
    *index = (uint32_t)at;
    return tah_vec_at(&inputs->held, (uint32_t)at);
}

/* Whether `actor`'s macrostep is open: its first record is held and it has not ended. */
static bool tah_inputs_open(const tah_inputs *inputs, ta_actor_id actor) {
    uint32_t index = 0;
    const tah_input_held *held = tah_inputs_held_of(inputs, actor, &index);
    return held != NULL && !held->ended;
}

/* Gives a record's byte buffer back for reuse. */
static void tah_inputs_spare(tah_inputs *inputs, tah_vec *bytes) {
    bytes->count = 0;
    if (tah_vec_push(&inputs->spare, bytes) != TA_OK) {
        tah_vec_destroy(bytes);
    }
}

/* An emptied byte buffer: a spare one, or a new one. */
static tah_vec tah_inputs_bytes(tah_inputs *inputs) {
    tah_vec bytes;
    if (inputs->spare.count != 0) {
        inputs->spare.count -= 1u;
        return *(tah_vec *)tah_vec_at(&inputs->spare, inputs->spare.count);
    }
    tah_vec_init(&bytes, inputs->allocator, TAH_LAYOUT(uint8_t));
    return bytes;
}

/* Lets go of held record `index`: the last one takes its place. */
static void tah_inputs_unhold(tah_inputs *inputs, uint32_t index) {
    tah_input_held *held = tah_vec_at(&inputs->held, index);
    (void)tah_map_remove(&inputs->held_index, held->actor);
    inputs->ended -= held->ended ? 1u : 0u;
    ta_address_release(held->address);
    tah_inputs_spare(inputs, &held->bytes);
    uint32_t last = inputs->held.count - 1u;
    if (index != last) {
        *held = *(tah_input_held *)tah_vec_at(&inputs->held, last);
        (void)tah_map_put(&inputs->held_index, held->actor, index); /* the key is there: no allocation */
    }
    inputs->held.count = last;
}

static void tah_inputs_forget_held(tah_inputs *inputs) {
    while (inputs->held.count != 0) {
        tah_inputs_unhold(inputs, inputs->held.count - 1u);
    }
}

/* Forgets every queued message and waiting count: the log was turned off or
 * on again, and a message queued before now has no ACCEPTED record. */
static void tah_inputs_forget_mail(tah_inputs *inputs) {
    uint32_t cursor = 0;
    uint64_t key = 0;
    uint64_t value = 0;
    while (tah_map_next(&inputs->queued_address, &cursor, &key, &value)) {
        ta_address_release((ta_address *)(uintptr_t)value);
    }
    tah_map_destroy(&inputs->queued_address);
    tah_map_init(&inputs->queued_address, inputs->allocator);
    tah_map_destroy(&inputs->seqs);
    tah_map_init(&inputs->seqs, inputs->allocator);
    tah_map_destroy(&inputs->mail_count);
    tah_map_init(&inputs->mail_count, inputs->allocator);
    tah_map_destroy(&inputs->mail_since);
    tah_map_init(&inputs->mail_since, inputs->allocator);
    tah_inputs_forget_held(inputs);
}

/* Forgets this turn's touched addresses. */
static void tah_inputs_forget_touched(tah_inputs *inputs) {
    for (uint32_t at = 0; at < inputs->touched.count; at += 1u) {
        ta_address_release(((tah_input_touch *)tah_vec_at(&inputs->touched, at))->address);
    }
    tah_vec_clear(&inputs->touched);
    tah_map_destroy(&inputs->touched_index);
    tah_map_init(&inputs->touched_index, inputs->allocator);
}

static void tah_inputs_free_families(tah_inputs *inputs) {
    for (uint32_t at = 0; at < inputs->families.count; at += 1u) {
        ta_str *family = tah_vec_at(&inputs->families, at);
        tah_free(inputs->allocator, &(ta_allocator_free_params){
                                        .ptr = (void *)family->ptr, .size = (size_t)family->len + 1u, .alignment = 1 });
    }
    tah_vec_clear(&inputs->families);
}

void tah_inputs_release(tah_inputs *inputs) {
    for (uint32_t at = 0; at < inputs->entries.count; at += 1u) {
        tah_input_buffer *entry = tah_vec_at(&inputs->entries, at);
        ta_address_release(entry->address);
        entry->bytes.count = 0;
        if (tah_vec_push(&inputs->spare, &entry->bytes) != TA_OK) {
            tah_vec_destroy(&entry->bytes);
        }
    }
    tah_vec_clear(&inputs->entries);
    tah_map_destroy(&inputs->entry_index);
    tah_map_init(&inputs->entry_index, inputs->allocator);
    inputs->count = 0;
    inputs->offering = 0;
    if (inputs->arena_ready) {
        ta_arena_reset(&inputs->arena);
    }
}

bool tah_inputs_enabled(const tah_inputs *inputs) {
    return inputs->enabled;
}

uint32_t tah_inputs_touched_count(const tah_inputs *inputs) {
    return inputs->touched.count;
}

uint32_t tah_inputs_entry_count(const tah_inputs *inputs) {
    return inputs->entries.count;
}

tah_input_entry tah_inputs_entry_at(const tah_inputs *inputs, uint32_t index) {
    const tah_input_buffer *buffer = tah_vec_at(&inputs->entries, index);
    return (tah_input_entry){ .address = buffer->address, .bytes = buffer->bytes.items, .length = buffer->bytes.count };
}

void tah_inputs_destroy(tah_system *system) {
    tah_inputs *inputs = &system->inputs;
    tah_inputs_forget_held(inputs);
    tah_vec_destroy(&inputs->held);
    tah_map_destroy(&inputs->held_index);
    tah_inputs_release(inputs);
    for (uint32_t at = 0; at < inputs->spare.count; at += 1u) {
        tah_vec_destroy(tah_vec_at(&inputs->spare, at));
    }
    tah_vec_destroy(&inputs->spare);
    tah_vec_destroy(&inputs->entries);
    tah_map_destroy(&inputs->entry_index);
    tah_inputs_free_families(inputs);
    tah_vec_destroy(&inputs->families);
    tah_inputs_forget_mail(inputs);
    tah_map_destroy(&inputs->seqs);
    tah_map_destroy(&inputs->queued_address);
    tah_map_destroy(&inputs->mail_count);
    tah_map_destroy(&inputs->mail_since);
    tah_map_destroy(&inputs->actor_seq);
    tah_inputs_forget_touched(inputs);
    tah_vec_destroy(&inputs->touched);
    tah_map_destroy(&inputs->touched_index);
    if (inputs->arena_ready) {
        ta_arena_destroy(&inputs->arena);
        inputs->arena_ready = false;
    }
    if (inputs->text_ready) {
        ta_arena_destroy(&inputs->text);
        inputs->text_ready = false;
    }
    inputs->enabled = false;
}

static bool tah_inputs_family_listed(const tah_inputs *inputs, ta_str family) {
    for (uint32_t at = 0; at < inputs->families.count; at += 1u) {
        const ta_str *listed = tah_vec_at(&inputs->families, at);
        if (listed->len == family.len && tah_mem_equal(listed->ptr, family.ptr, family.len)) {
            return true;
        }
    }
    return false;
}

/* Marks `address` touched this turn; `fixed` gives its wake outright. */
static void tah_inputs_touch(tah_inputs *inputs, ta_address *address, bool fixed, bool has_at, ta_time at) {
    if (address == NULL) {
        return;
    }
    uint64_t index = 0;
    tah_input_touch *touch = NULL;
    if (tah_map_get(&inputs->touched_index, (uint64_t)(uintptr_t)address, &index)) {
        touch = tah_vec_at(&inputs->touched, (uint32_t)index);
    } else {
        tah_input_touch fresh = { .address = address };
        if (tah_vec_push(&inputs->touched, &fresh) != TA_OK) {
            inputs->lost += 1u; /* its wake is not reported this turn */
            return;
        }
        if (tah_map_put(&inputs->touched_index, (uint64_t)(uintptr_t)address, inputs->touched.count - 1u) != TA_OK) {
            inputs->touched.count -= 1u;
            inputs->lost += 1u;
            return;
        }
        ta_address_retain(address);
        touch = tah_vec_at(&inputs->touched, inputs->touched.count - 1u);
    }
    if (fixed) {
        touch->fixed = true;
        touch->has_at = has_at;
        touch->at = at;
    }
}

/* ------------------------------------------------------------------------
 * Writing records
 */

/* The entry for `address` this turn, made when it has none; NULL without memory. */
static tah_input_buffer *tah_inputs_entry(tah_inputs *inputs, ta_address *address) {
    uint64_t index = 0;
    if (tah_map_get(&inputs->entry_index, (uint64_t)(uintptr_t)address, &index)) {
        return tah_vec_at(&inputs->entries, (uint32_t)index);
    }
    tah_input_buffer entry = { .address = address };
    if (inputs->spare.count != 0) {
        inputs->spare.count -= 1u;
        entry.bytes = *(tah_vec *)tah_vec_at(&inputs->spare, inputs->spare.count);
    } else {
        tah_vec_init(&entry.bytes, inputs->allocator, TAH_LAYOUT(uint8_t));
    }
    if (tah_vec_push(&inputs->entries, &entry) != TA_OK) {
        tah_vec_destroy(&entry.bytes);
        return NULL;
    }
    if (tah_map_put(&inputs->entry_index, (uint64_t)(uintptr_t)address, inputs->entries.count - 1u) != TA_OK) {
        inputs->entries.count -= 1u;
        tah_vec_destroy(&entry.bytes);
        return NULL;
    }
    ta_address_retain(address);
    return tah_vec_at(&inputs->entries, inputs->entries.count - 1u);
}

uint64_t tah_inputs_actor_seq(const tah_inputs *inputs, ta_actor_id actor) {
    uint64_t seq = 0;
    return actor != 0 && tah_map_get(&inputs->actor_seq, actor, &seq) ? seq : 0;
}

void tah_inputs_set_actor_seq(tah_inputs *inputs, ta_actor_id actor, uint64_t seq) {
    if (actor != 0 && tah_map_put(&inputs->actor_seq, actor, seq) != TA_OK) {
        inputs->lost += 1u;
    }
}

/* Where to write: the address's entry, and the record number to take. */
typedef struct {
    ta_address *address;
    ta_actor_id actor; /* the resident actor whose record it is, or 0 */
} tah_inputs_target;

static void tah_inputs_mail_done(tah_inputs *inputs, ta_address *address);

/*
 * Writes `actor`'s held first record into this turn's entry for its
 * address, with the actor's next record number: its macrostep has ended,
 * or a record that must follow it is about to be written (kernel ADR
 * 0004). Out of memory the record stays held, marked ended, and the next
 * turn tries again (tah_inputs_retry); TA_ERR_NOMEM says so.
 */
static ta_result tah_inputs_flush(tah_system *system, ta_actor_id actor) {
    tah_inputs *inputs = &system->inputs;
    uint32_t index = 0;
    tah_input_held *held = tah_inputs_held_of(inputs, actor, &index);
    if (held == NULL) {
        return TA_OK;
    }
    tah_input_buffer *entry = tah_inputs_entry(inputs, held->address);
    uint32_t start = entry != NULL ? entry->bytes.count : 0;
    uint32_t length = held->bytes.count;
    uint64_t seq = tah_inputs_actor_seq(inputs, actor) + 1u;
    if (entry == NULL || tah_vec_reserve(&entry->bytes, start + length) != TA_OK ||
        tah_map_put(&inputs->actor_seq, actor, seq) != TA_OK) {
        if (!held->ended) {
            held->ended = true;
            inputs->ended += 1u;
        }
        return TA_ERR_NOMEM;
    }
    uint8_t *bytes = held->bytes.items;
    for (uint32_t at = 0; at < 8u; at += 1u) {
        bytes[8u + at] = (uint8_t)(seq >> (8u * at)); /* the fixed part's u64 seq (wal.h) */
    }
    tah_mem_copy((uint8_t *)entry->bytes.items + start, bytes, length);
    entry->bytes.count = start + length;
    inputs->count += 1u;
    tah_inputs_touch(inputs, held->address, false, false, 0);
    if (held->owed) {
        tah_inputs_mail_done(inputs, held->address);
    }
    tah_inputs_unhold(inputs, index);
    tah_system_want_turn(system); /* the batch goes out after the next turn */
    return TA_OK;
}

ta_result tah_inputs_write_first(tah_system *system, ta_actor_id actor) {
    return tah_inputs_open(&system->inputs, actor) ? TA_ERR_BUSY : tah_inputs_flush(system, actor);
}

/* Writes the held first records whose macrosteps ended while memory ran out. */
void tah_inputs_retry(tah_system *system) {
    tah_inputs *inputs = &system->inputs;
    for (uint32_t index = inputs->held.count; inputs->ended != 0 && index > 0; index -= 1u) {
        tah_input_held *held = tah_vec_at(&inputs->held, index - 1u);
        if (held->ended) {
            (void)tah_inputs_flush(system, held->actor); /* moves only the ones after it */
        }
    }
}

/*
 * Holds `record`, the first record of `target.actor`'s macrostep, until the
 * macrostep ends (tah_inputs_flush). A first record still held (its
 * macrostep ended while memory ran out) is written first.
 */
static ta_result tah_inputs_hold(tah_system *system, tah_inputs_target target, tah_wal_record *record) {
    tah_inputs *inputs = &system->inputs;
    ta_result result = tah_inputs_flush(system, target.actor);
    if (result != TA_OK) {
        if (inputs->text_ready) {
            ta_arena_reset(&inputs->text);
        }
        return result;
    }
    tah_input_held held = { .actor = target.actor,
                            .address = target.address,
                            .bytes = tah_inputs_bytes(inputs),
                            .life = record->kind == TAH_WAL_SPAWNED };
    record->seq = 0; /* filled in when it is written */
    result = tah_wal_write(record, &(tah_wal_write_params){ .out = &held.bytes, .datamodel = inputs->datamodel });
    if (inputs->text_ready) {
        ta_arena_reset(&inputs->text);
    }
    if (result == TA_OK && tah_vec_push(&inputs->held, &held) != TA_OK) {
        result = TA_ERR_NOMEM;
    } else if (result == TA_OK && tah_map_put(&inputs->held_index, target.actor, inputs->held.count - 1u) != TA_OK) {
        inputs->held.count -= 1u;
        result = TA_ERR_NOMEM;
    }
    if (result != TA_OK) {
        tah_inputs_spare(inputs, &held.bytes);
        return result;
    }
    ta_address_retain(target.address);
    return TA_OK;
}

/*
 * Whether a record of `kind` must follow the held first record of the
 * actor's macrostep (kernel ADR 0004, "The ordering rule"). Mail accepted
 * for it, mail dropped and its timers' firings do not change what a replay
 * of the macrostep does (a firing is time only), so they may come first;
 * anything else is ordered after it.
 */
static bool tah_inputs_follows_first(tah_wal_kind kind) {
    return kind != TAH_WAL_ACCEPTED && kind != TAH_WAL_DROPPED && kind != TAH_WAL_FIRED;
}

/*
 * Appends `record` (its seq and time filled in here) to `target`'s entry.
 * `mark`, when given, lets the caller undo it. The record's rendered text
 * is dropped afterwards.
 */
static ta_result tah_inputs_write(tah_system *system, tah_inputs_target target, tah_wal_record *record,
                                  tah_input_mark *mark) {
    tah_inputs *inputs = &system->inputs;
    uint32_t held_at = 0;
    tah_input_held *held = tah_inputs_held_of(inputs, target.actor, &held_at);
    if (held != NULL && (held->ended || tah_inputs_follows_first(record->kind)) &&
        tah_inputs_flush(system, target.actor) != TA_OK && tah_inputs_follows_first(record->kind)) {
        if (inputs->text_ready) {
            ta_arena_reset(&inputs->text);
        }
        return TA_ERR_NOMEM;
    }
    tah_input_buffer *entry = tah_inputs_entry(inputs, target.address);
    if (entry == NULL) {
        if (inputs->text_ready) {
            ta_arena_reset(&inputs->text);
        }
        return TA_ERR_NOMEM;
    }
    /* Before its held SPAWNED, an actor's records are outside its life: seq 0 (kernel ADR 0004). */
    held = tah_inputs_held_of(inputs, target.actor, &held_at);
    if (held != NULL && held->life) {
        target.actor = 0;
    }
    uint64_t before = tah_inputs_actor_seq(inputs, target.actor);
    record->seq = target.actor != 0 ? before + 1u : 0;
    uint32_t start = entry->bytes.count;
    ta_result result = tah_wal_write(record, &(tah_wal_write_params){ .out = &entry->bytes, .datamodel = inputs->datamodel });
    if (inputs->text_ready) {
        ta_arena_reset(&inputs->text);
    }
    if (result != TA_OK) {
        return result;
    }
    if (target.actor != 0 && tah_map_put(&inputs->actor_seq, target.actor, record->seq) != TA_OK) {
        entry->bytes.count = start;
        return TA_ERR_NOMEM;
    }
    inputs->count += 1u;
    tah_inputs_touch(inputs, target.address, false, false, 0);
    tah_system_want_turn(system); /* the batch goes out after the next turn */
    if (mark != NULL) {
        *mark = (tah_input_mark){ .open = true,
                                  .entry = (uint32_t)(entry - (tah_input_buffer *)inputs->entries.items),
                                  .start = start,
                                  .actor = target.actor,
                                  .seq_before = before };
    }
    return TA_OK;
}

void tah_inputs_undo(tah_system *system, tah_input_mark *mark) {
    tah_inputs *inputs = &system->inputs;
    if (!mark->open) {
        return;
    }
    mark->open = false;
    if (mark->entry < inputs->entries.count) {
        tah_input_buffer *entry = tah_vec_at(&inputs->entries, mark->entry);
        if (mark->start <= entry->bytes.count) {
            entry->bytes.count = mark->start;
            inputs->count -= inputs->count != 0 ? 1u : 0u;
        }
    }
    if (mark->actor != 0) {
        tah_inputs_set_actor_seq(inputs, mark->actor, mark->seq_before);
    }
}

/* The arena one record's texts are rendered into. */
static ta_arena *tah_inputs_text_arena(tah_inputs *inputs) {
    if (!inputs->text_ready) {
        if (ta_arena_init(&inputs->text, inputs->allocator, TAH_INPUTS_BLOCK_BYTES) != TA_OK) {
            return NULL;
        }
        inputs->text_ready = true;
    }
    return &inputs->text;
}

/* An event name's dotted text, in the text arena; false without memory. */
static bool tah_inputs_event_name(tah_system *system, ta_event_name_id name, ta_str *out) {
    ta_arena *arena = tah_inputs_text_arena(&system->inputs);
    uint32_t needed = 0;
    if (arena == NULL) {
        return false;
    }
    (void)ta_event_name_format(system->core, &(ta_event_name_format_params){ .name = name, .needed = &needed });
    char *buffer = needed == 0 ? NULL : ta_arena_alloc(arena, needed, 1);
    if (buffer == NULL ||
        ta_event_name_format(system->core, &(ta_event_name_format_params){ .name = name, .buffer = buffer,
                                                                           .capacity = needed, .needed = &needed }) != TA_OK) {
        return false;
    }
    *out = (ta_str){ .ptr = buffer, .len = needed - 1u };
    return true;
}

/* A send ID's text, in the text arena; false for none or without memory. */
static bool tah_inputs_send_text(tah_system *system, ta_send_id send, ta_str *out) {
    ta_arena *arena = tah_inputs_text_arena(&system->inputs);
    uint32_t needed = 0;
    if (send == 0 || arena == NULL) {
        return false;
    }
    (void)ta_send_id_format(system->core, &(ta_send_id_format_params){ .send_id = send, .needed = &needed });
    char *buffer = needed == 0 ? NULL : ta_arena_alloc(arena, needed, 1);
    if (buffer == NULL ||
        ta_send_id_format(system->core, &(ta_send_id_format_params){ .send_id = send, .buffer = buffer,
                                                                     .capacity = needed, .needed = &needed }) != TA_OK) {
        return false;
    }
    *out = (ta_str){ .ptr = buffer, .len = needed - 1u };
    return true;
}

/* A state's ID text, borrowed from its definition; false for none. */
static bool tah_inputs_state_text(tah_system *system, ta_definition_id definition, ta_state_id state, ta_str *out) {
    ta_state_info info;
    if (state == TA_STATE_NONE ||
        ta_definition_state(system->core, &(ta_definition_state_params){ .definition = definition, .state = state,
                                                                         .out = &info }) != TA_OK) {
        return false;
    }
    *out = info.id;
    return true;
}

/* The core reason token of a report (core ADR 0081). */
static ta_str tah_inputs_report_reason(uint8_t kind) {
    switch (kind) {
    case TAH_REPORT_QUARANTINED: return (ta_str){ .ptr = "quarantined", .len = 11 };
    case TAH_REPORT_FAILED: return (ta_str){ .ptr = "failed", .len = 6 };
    default: return (ta_str){ .ptr = "unavailable", .len = 11 };
    }
}

/*
 * ACCEPTED for a message (kernel ADR 0002 section 2): its ID, the event's
 * texts and data, and for an error report its kind, reason and whether it
 * ends the invocation. `session` renders a computed origin (0 when none).
 * The texts live in the log's text arena until the next record is written.
 */
ta_result tah_inputs_accepted_record(tah_system *system, const tah_envelope *envelope, uint64_t id, uint64_t session,
                                     tah_wal_record *out) {
    const ta_event *event = &envelope->event;
    ta_str name = { 0 };
    ta_str send = { 0 };
    ta_str origin = { 0 };
    ta_str type = { 0 };
    ta_str invoke = { 0 };
    if (!tah_inputs_event_name(system, event->name, &name)) {
        return TA_ERR_NOMEM;
    }
    bool has_send = tah_inputs_send_text(system, event->send_id, &send);
    bool has_origin = false;
    if (event->origin.kind != TA_ORIGIN_NONE) {
        ta_arena *arena = tah_inputs_text_arena(&system->inputs);
        if (arena == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_result result = ta_origin_text(system->core, &(ta_origin_text_params){ .origin = &event->origin,
                                                                                  .session_id = session,
                                                                                  .arena = ta_arena_allocator(arena),
                                                                                  .type = &type,
                                                                                  .uri = &origin });
        if (result == TA_ERR_NOMEM) {
            return result;
        }
        has_origin = result == TA_OK;
    }
    bool has_invoke = event->invoke_id != TA_INVOKE_ID_NONE &&
                      ta_system_invoke_id_text(system->core, event->invoke_id, &invoke) == TA_OK;
    bool report = envelope->report != TAH_REPORT_NONE;
    *out = (tah_wal_record){
        .kind = TAH_WAL_ACCEPTED,
        .time = ta_system_time(system->core),
        .field_count = 10,
        .fields = {
            tah_wal_number_field(id),
            tah_wal_number_field((uint64_t)event->type),
            tah_wal_text_field(true, name),
            tah_wal_text_field(has_send, send),
            tah_wal_text_field(has_origin, origin),
            tah_wal_text_field(has_origin, type),
            tah_wal_text_field(has_invoke, invoke),
            tah_wal_number_field(envelope->report),
            tah_wal_text_field(report, report ? tah_inputs_report_reason(envelope->report) : (ta_str){ 0 }),
            tah_wal_number_field(report && envelope->report != TAH_REPORT_QUARANTINED ? 1u : 0u),
        },
        .values = { &event->data },
    };
    return TA_OK;
}

static ta_result tah_inputs_accepted(tah_system *system, tah_inputs_target target, uint64_t id,
                                     const tah_envelope *envelope) {
    uint64_t session = 0;
    if (target.actor != 0) {
        ta_actor_inspection view;
        if (ta_actor_inspect(system->core, target.actor, &view) == TA_OK) {
            session = view.session_id;
        }
    }
    tah_wal_record record;
    ta_result result = tah_inputs_accepted_record(system, envelope, id, session, &record);
    if (result != TA_OK) {
        if (system->inputs.text_ready) {
            ta_arena_reset(&system->inputs.text);
        }
        return result;
    }
    return tah_inputs_write(system, target, &record, NULL);
}

/* ------------------------------------------------------------------------
 * Configuration
 */

/* A 64-bit mix of the incarnation (SplitMix64's finalizer). */
static uint64_t tah_mix(uint64_t value) {
    value += 0x9e3779b97f4a7c15u;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9u;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebu;
    return value ^ (value >> 31);
}

ta_result tah_inputs_configure(tah_system *system, const ta_str *families, uint32_t count) {
    tah_inputs *inputs = &system->inputs;
    if (count > 0 && !inputs->arena_ready) {
        if (ta_arena_init(&inputs->arena, inputs->allocator, TAH_INPUTS_BLOCK_BYTES) != TA_OK) {
            return TA_ERR_NOMEM;
        }
        inputs->arena_ready = true;
    }
    tah_vec copies;
    tah_vec_init(&copies, inputs->allocator, TAH_LAYOUT(ta_str));
    ta_result result = tah_vec_reserve(&copies, count);
    for (uint32_t at = 0; result == TA_OK && at < count; at += 1u) {
        char *text = tah_alloc(inputs->allocator, (size_t)families[at].len + 1u, 1);
        if (text == NULL) {
            result = TA_ERR_NOMEM;
            break;
        }
        tah_mem_copy(text, families[at].ptr, families[at].len);
        text[families[at].len] = '\0';
        ta_str copy = { .ptr = text, .len = families[at].len };
        (void)tah_vec_push(&copies, &copy);
    }
    if (result != TA_OK) {
        tah_vec old = inputs->families;
        inputs->families = copies;
        tah_inputs_free_families(inputs);
        tah_vec_destroy(&inputs->families);
        inputs->families = old;
        return result;
    }
    tah_inputs_free_families(inputs);
    tah_vec_destroy(&inputs->families);
    inputs->families = copies;
    inputs->enabled = count > 0;
    if (inputs->seq_base == 0) {
        inputs->seq_base = tah_mix(ta_system_incarnation(system->core)) & 0xffffffff00000000u;
        inputs->seq_base = inputs->seq_base == 0 ? (uint64_t)1 << 32 : inputs->seq_base;
        inputs->next_seq = inputs->seq_base;
    }
    /* A message queued before now has no ACCEPTED record. */
    tah_inputs_forget_mail(inputs);
    tah_sched_forget_logging(&system->sched);
    system->sched.observer = inputs->enabled
                                 ? (tah_sched_observer){ .self = system, .note = tah_inputs_note }
                                 : (tah_sched_observer){ 0 };
    return TA_OK;
}

bool tah_inputs_seq_is_ours(const tah_inputs *inputs, uint64_t seq) {
    return inputs->seq_base != 0 && (seq & 0xffffffff00000000u) == inputs->seq_base;
}

/* tah_inputs_logged(), also answering the address's handle (borrowed). */
static bool tah_inputs_logged_handle(tah_system *system, ta_actor_id actor, ta_actor_address *address,
                                     ta_address **handle) {
    tah_sched_entry *entry = tah_sched_lookup(&system->sched, actor);
    if (!system->inputs.enabled || (entry != NULL && (entry->flags & TAH_SCHED_LOG_KNOWN) != 0 &&
                                    (entry->flags & TAH_SCHED_LOGGED) == 0)) {
        return false;
    }
    /* Read-only, so it may run inside a router callback (the target of mail). */
    ta_actor_inspection view;
    if (ta_actor_inspect(system->core, actor, &view) != TA_OK) {
        return false;
    }
    /* An unpublished actor, a replay's above all, is logged by nothing it
     * does (napi ADR 0032 section 7). Not cached: once published it is. */
    if (!view.published) {
        return false;
    }
    bool logged = view.self.kind == TA_ACTOR_REF_NAMED &&
                  tah_inputs_family_listed(&system->inputs, view.definition.family.name);
    if (entry != NULL) {
        entry->flags |= TAH_SCHED_LOG_KNOWN;
        entry->flags = logged ? (uint8_t)(entry->flags | TAH_SCHED_LOGGED) : (uint8_t)(entry->flags & ~TAH_SCHED_LOGGED);
    }
    if (logged && address != NULL) {
        *address = ta_address_view(view.self.as.named);
    }
    if (logged && handle != NULL) {
        *handle = view.self.as.named;
    }
    return logged;
}

bool tah_inputs_logged(tah_system *system, ta_actor_id actor, ta_actor_address *address) {
    return tah_inputs_logged_handle(system, actor, address, NULL);
}

/* ------------------------------------------------------------------------
 * The mailbox's records
 */

/* One more queued message waiting for `address`, accepted at `since`. */
static void tah_inputs_mail_add(tah_inputs *inputs, ta_address *address, ta_time since) {
    uint64_t key = (uint64_t)(uintptr_t)address;
    uint64_t count = 0;
    (void)tah_map_get(&inputs->mail_count, key, &count);
    if (count == 0 && tah_map_put(&inputs->mail_since, key, since) != TA_OK) {
        inputs->lost += 1u;
    }
    if (tah_map_put(&inputs->mail_count, key, count + 1u) != TA_OK) {
        inputs->lost += 1u;
    }
}

/* One queued message for `address` delivered or dropped. */
static void tah_inputs_mail_done(tah_inputs *inputs, ta_address *address) {
    uint64_t key = (uint64_t)(uintptr_t)address;
    uint64_t count = 0;
    if (!tah_map_get(&inputs->mail_count, key, &count)) {
        return;
    }
    if (count <= 1u) {
        (void)tah_map_remove(&inputs->mail_count, key);
        (void)tah_map_remove(&inputs->mail_since, key);
    } else {
        (void)tah_map_put(&inputs->mail_count, key, count - 1u);
    }
}

/* The queued message behind `envelope`, if the log numbered it: its ID and
 * address (retained, the caller releases it), out of the maps. */
static bool tah_inputs_unqueue(tah_inputs *inputs, uint32_t envelope, uint64_t *seq, ta_address **address) {
    if (!tah_map_get(&inputs->seqs, envelope, seq)) {
        return false;
    }
    (void)tah_map_remove(&inputs->seqs, envelope);
    uint64_t value = 0;
    *address = NULL;
    if (tah_map_get(&inputs->queued_address, envelope, &value)) {
        (void)tah_map_remove(&inputs->queued_address, envelope);
        *address = (ta_address *)(uintptr_t)value;
        tah_inputs_mail_done(inputs, *address);
    }
    return true;
}

/* A delivered message out of the maps: its address (retained, the caller
 * releases it) keeps counting it as waiting until its macrostep's first
 * record is written (tah_inputs_flush), so a batch before that one wakes
 * the address for it. */
static bool tah_inputs_unqueue_held(tah_inputs *inputs, uint32_t envelope, ta_address **address) {
    uint64_t value = 0;
    *address = NULL;
    if (!tah_map_remove(&inputs->seqs, envelope) || !tah_map_get(&inputs->queued_address, envelope, &value)) {
        return false;
    }
    (void)tah_map_remove(&inputs->queued_address, envelope);
    *address = (ta_address *)(uintptr_t)value;
    return true;
}

/* The resident, published actor at `address`, or 0. */
static ta_actor_id tah_inputs_resident(tah_system *system, ta_address *address) {
    ta_actor_directory_entry entry = tah_directory_find(&system->directory, address);
    if (entry.kind != TA_ACTOR_DIRECTORY_RESIDENT) {
        return 0;
    }
    ta_actor_inspection view;
    return ta_actor_inspect(system->core, entry.actor, &view) == TA_OK && view.published ? entry.actor : 0;
}

/*
 * Numbers `envelope` as message `seq` for `handle`, which it waits for.
 * TA_ERR_NOMEM numbers nothing: a message is never queued without its ID,
 * which a checkpoint and a redelivery need.
 */
static ta_result tah_inputs_number(tah_inputs *inputs, uint32_t envelope, uint64_t seq, ta_address *handle,
                                   ta_time since) {
    if (tah_map_put(&inputs->seqs, envelope, seq) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    if (tah_map_put(&inputs->queued_address, envelope, (uint64_t)(uintptr_t)handle) != TA_OK) {
        (void)tah_map_remove(&inputs->seqs, envelope);
        return TA_ERR_NOMEM;
    }
    ta_address_retain(handle);
    tah_inputs_touch(inputs, handle, false, false, 0);
    tah_inputs_mail_add(inputs, handle, since);
    return TA_OK;
}

/* Takes back what tah_inputs_number() did for `envelope`. */
static void tah_inputs_unnumber(tah_inputs *inputs, uint32_t envelope) {
    uint64_t seq = 0;
    ta_address *address = NULL;
    if (tah_inputs_unqueue(inputs, envelope, &seq, &address)) {
        ta_address_release(address);
    }
}

static ta_result tah_inputs_queued(tah_system *system, const tah_sched_note *note) {
    tah_inputs *inputs = &system->inputs;
    const tah_envelope *envelope = note->message;
    if (envelope == NULL) {
        return TA_OK;
    }
    ta_address *handle = NULL;
    ta_actor_id actor = 0;
    if (note->address != NULL) {
        handle = note->address;
        if (!tah_inputs_family_listed(inputs, ta_address_view(handle).family.name)) {
            return TA_OK;
        }
    } else if (tah_inputs_logged_handle(system, note->actor, NULL, &handle)) {
        actor = note->actor;
    } else {
        return TA_OK;
    }
    bool owed = inputs->redelivering;
    ta_time since = owed ? inputs->redeliver_since : ta_system_time(system->core);
    uint64_t seq = owed ? inputs->redeliver_seq : inputs->next_seq + 1u;
    inputs->redelivering = false;
    /* Its ID first: a message the log cannot number is not accepted. */
    ta_result result = tah_inputs_number(inputs, note->envelope, seq, handle, since);
    if (result != TA_OK) {
        return result;
    }
    if (!owed) {
        result = tah_inputs_accepted(system, (tah_inputs_target){ .address = handle, .actor = actor }, seq, envelope);
        if (result != TA_OK) {
            tah_inputs_unnumber(inputs, note->envelope);
            return result; /* not accepted: the log could not hold it */
        }
        inputs->next_seq = seq;
    }
    return TA_OK;
}

/*
 * DELIVERED, or JUMP for a message carrying an operator transition, made
 * ahead of core's ta_actor_deliver() (or ta_actor_report_invoke() for a
 * report), with the delivery's time, and held until the macrostep it
 * starts ends (tah_inputs_hold). A message the log has no ACCEPTED record
 * of (it was queued before the log was on) gets one first.
 */
static ta_result tah_inputs_delivering(tah_system *system, const tah_sched_note *note) {
    tah_inputs *inputs = &system->inputs;
    inputs->offering = 0;
    ta_address *handle = NULL;
    if (note->message == NULL || !tah_inputs_logged_handle(system, note->actor, NULL, &handle)) {
        return TA_OK;
    }
    tah_inputs_target target = { .address = handle, .actor = note->actor };
    uint64_t seq = 0;
    if (!tah_map_get(&inputs->seqs, note->envelope, &seq)) {
        seq = inputs->next_seq + 1u;
        ta_result result = tah_inputs_number(inputs, note->envelope, seq, handle, ta_system_time(system->core));
        if (result != TA_OK) {
            return result; /* the delivery waits a turn */
        }
        result = tah_inputs_accepted(system, target, seq, note->message);
        if (result != TA_OK) {
            tah_inputs_unnumber(inputs, note->envelope);
            return result;
        }
        inputs->next_seq = seq;
    }
    const tah_envelope *envelope = note->message;
    tah_wal_record record = { .kind = TAH_WAL_DELIVERED,
                              .time = ta_system_time(system->core),
                              .field_count = 1,
                              .fields = { tah_wal_number_field(seq) } };
    ta_str *targets = NULL;
    if (envelope->has_jump) {
        const tah_jump *jump = envelope->jump;
        ta_str source = { 0 };
        bool has_source = tah_inputs_state_text(system, jump->definition, jump->source, &source);
        ta_arena *arena = tah_inputs_text_arena(inputs);
        targets = arena == NULL ? NULL : ta_arena_alloc(arena, (size_t)jump->count * sizeof *targets + 1u, _Alignof(ta_str));
        if (targets == NULL) {
            return TA_ERR_NOMEM;
        }
        for (uint32_t index = 0; index < jump->count; index += 1u) {
            if (!tah_inputs_state_text(system, jump->definition, jump->targets[index], &targets[index])) {
                return TA_ERR_INVALID;
            }
        }
        record.kind = TAH_WAL_JUMP;
        record.field_count = 3;
        record.fields[1] = tah_wal_text_field(has_source, source);
        record.fields[2] = tah_wal_number_field(jump->count);
        record.texts = targets;
        record.text_count = jump->count;
    }
    ta_result result = tah_inputs_hold(system, target, &record);
    if (result == TA_OK) {
        inputs->offering = note->actor;
    }
    return result;
}

/* FINISHED, with the top-level final state the actor reached. */
static void tah_inputs_finished(tah_system *system, ta_actor_id actor, ta_address *handle) {
    ta_actor_inspection view;
    ta_str state = { 0 };
    bool has_state = ta_actor_inspect(system->core, actor, &view) == TA_OK &&
                     tah_inputs_state_text(system, view.definition_id, view.final_state, &state);
    tah_wal_record record = { .kind = TAH_WAL_FINISHED,
                              .time = ta_system_time(system->core),
                              .field_count = 1,
                              .fields = { tah_wal_text_field(has_state, state) } };
    if (tah_inputs_write(system, (tah_inputs_target){ .address = handle, .actor = actor }, &record, NULL) != TA_OK) {
        system->inputs.lost += 1u; /* a replay reaches the final state anyway */
    }
    /* Nothing will need it back, though an earlier batch's wake counted the
     * message of the macrostep that finished it, and the actor may be
     * reclaimed before this batch's wakes are worked out. */
    tah_inputs_touch(&system->inputs, handle, true, false, 0);
}

ta_result tah_inputs_note(void *self, const tah_sched_note *note) {
    tah_system *system = self;
    tah_inputs *inputs = &system->inputs;
    if (!inputs->enabled) {
        return TA_OK;
    }
    ta_address *handle = NULL;
    switch (note->kind) {
    case TAH_NOTE_QUEUED:
        return tah_inputs_queued(system, note);
    case TAH_NOTE_DELIVERING:
    case TAH_NOTE_REPORTING:
        return tah_inputs_delivering(system, note);
    case TAH_NOTE_UNDELIVERED: {
        uint32_t index = 0;
        if (tah_inputs_held_of(inputs, inputs->offering, &index) != NULL) {
            tah_inputs_unhold(inputs, index); /* core refused it: no macrostep started */
        }
        inputs->offering = 0;
        return TA_OK;
    }
    case TAH_NOTE_DELIVERED:
    case TAH_NOTE_REPORTED: {
        uint32_t index = 0;
        tah_input_held *held = tah_inputs_held_of(inputs, inputs->offering, &index);
        inputs->offering = 0;
        uint64_t seq = 0;
        ta_address *queued_address = NULL;
        bool numbered = held != NULL ? tah_inputs_unqueue_held(inputs, note->envelope, &queued_address)
                                     : tah_inputs_unqueue(inputs, note->envelope, &seq, &queued_address);
        if (numbered) {
            if (held != NULL) {
                held->owed = queued_address != NULL;
            }
            tah_inputs_touch(inputs, queued_address, false, false, 0);
            ta_address_release(queued_address);
        }
        return TA_OK;
    }
    case TAH_NOTE_ENDED:
        /* Its first record goes into this turn's batch, beside what the macrostep sent. */
        (void)tah_inputs_flush(system, note->actor);
        return TA_OK;
    case TAH_NOTE_DROPPED: {
        uint64_t seq = 0;
        ta_address *queued_address = NULL;
        if (!tah_inputs_unqueue(inputs, note->envelope, &seq, &queued_address)) {
            return TA_OK;
        }
        if (queued_address != NULL) {
            tah_wal_record record = { .kind = TAH_WAL_DROPPED,
                                      .time = ta_system_time(system->core),
                                      .field_count = 1,
                                      .fields = { tah_wal_number_field(seq) } };
            tah_inputs_target target = { .address = queued_address,
                                         .actor = tah_inputs_resident(system, queued_address) };
            if (tah_inputs_write(system, target, &record, NULL) != TA_OK) {
                inputs->lost += 1u;
            }
            ta_address_release(queued_address);
        }
        return TA_OK;
    }
    case TAH_NOTE_RELEASING:
        if (tah_inputs_logged_handle(system, note->actor, NULL, &handle)) {
            /* It starts the rest of a macrostep, held as a delivery's record is. */
            tah_wal_record record = { .kind = TAH_WAL_RELEASED, .time = ta_system_time(system->core) };
            if (tah_inputs_hold(system, (tah_inputs_target){ .address = handle, .actor = note->actor }, &record) !=
                TA_OK) {
                inputs->lost += 1u;
            }
        }
        return TA_OK;
    case TAH_NOTE_FINISHED:
        if (tah_inputs_logged_handle(system, note->actor, NULL, &handle)) {
            tah_inputs_finished(system, note->actor, handle);
            tah_system_want_turn(system);
        }
        return TA_OK;
    case TAH_NOTE_SETTLED:
        /* Its macrostep may have armed or cancelled sends: work its wake out. */
        if (tah_inputs_logged_handle(system, note->actor, NULL, &handle)) {
            tah_inputs_touch(inputs, handle, false, false, 0);
            tah_system_want_turn(system);
        }
        return TA_OK;
    default:
        return TA_OK;
    }
}

/* ------------------------------------------------------------------------
 * Lifecycle records
 */

ta_result tah_inputs_spawned(tah_system *system, ta_actor_id actor) {
    ta_address *handle = NULL;
    if (!tah_inputs_logged_handle(system, actor, NULL, &handle)) {
        return TA_OK;
    }
    ta_actor_inspection view;
    if (ta_actor_inspect(system->core, actor, &view) != TA_OK) {
        return TA_OK;
    }
    ta_value binding = { 0 };
    ta_result root = ta_actor_datamodel_root(system->core, actor, &binding);
    if (root != TA_OK && root != TA_ERR_UNSUPPORTED) {
        return root;
    }
    ta_actor_address address = ta_address_view(handle);
    tah_wal_record record = {
        .kind = TAH_WAL_SPAWNED,
        .time = ta_system_time(system->core),
        .field_count = 3,
        .fields = { tah_wal_text_field(true, address.family.name), tah_wal_text_field(true, address.name),
                    tah_wal_text_field(true, (ta_str){ .ptr = (const char *)view.definition.revision.ptr,
                                                       .len = view.definition.revision.len }) },
        .values = { &binding },
    };
    tah_inputs_set_actor_seq(&system->inputs, actor, 0); /* a new life: its records count from 1 */
    /* Held until its bootstrap macrostep ends (kernel ADR 0004). */
    ta_result result = tah_inputs_hold(system, (tah_inputs_target){ .address = handle, .actor = actor }, &record);
    ta_value_release(&binding);
    return result;
}

ta_result tah_inputs_evicted(tah_system *system, ta_actor_id actor, bool has_wake, ta_time wake) {
    ta_address *handle = NULL;
    if (!tah_inputs_logged_handle(system, actor, NULL, &handle)) {
        return TA_OK;
    }
    if (tah_inputs_open(&system->inputs, actor)) {
        return TA_ERR_BUSY; /* inside a macrostep: a replay could not stop where it stopped */
    }
    tah_wal_record record = { .kind = TAH_WAL_EVICTED, .time = ta_system_time(system->core) };
    ta_result result = tah_inputs_write(system, (tah_inputs_target){ .address = handle, .actor = actor }, &record, NULL);
    if (result == TA_OK) {
        tah_inputs_touch(&system->inputs, handle, true, has_wake, wake);
    }
    return result;
}

ta_result tah_inputs_destroying(tah_system *system, ta_actor_id actor) {
    ta_address *handle = NULL;
    if (!tah_inputs_logged_handle(system, actor, NULL, &handle)) {
        return TA_OK;
    }
    ta_actor_inspection view;
    if (ta_actor_inspect(system->core, actor, &view) != TA_OK || view.has_completion) {
        return TA_OK; /* finished: FINISHED says so already */
    }
    /* Only the actor that holds its address ends its life here: an
     * incumbent a replacement took the address from does not. */
    ta_actor_directory_entry entry = tah_directory_find(&system->directory, handle);
    if (entry.kind != TA_ACTOR_DIRECTORY_RESIDENT || entry.actor != actor) {
        return TA_OK;
    }
    tah_wal_record record = { .kind = TAH_WAL_DESTROYED, .time = ta_system_time(system->core) };
    ta_result result = tah_inputs_write(system, (tah_inputs_target){ .address = handle, .actor = actor }, &record, NULL);
    if (result == TA_OK) {
        tah_inputs_touch(&system->inputs, handle, true, false, 0); /* nothing will need it back */
    }
    return result;
}

void tah_inputs_removed(tah_system *system, ta_actor_id actor) {
    tah_inputs *inputs = &system->inputs;
    uint32_t index = 0;
    tah_input_held *held = tah_inputs_held_of(inputs, actor, &index);
    if (held != NULL) {
        /* Gone inside a macrostep, with no record of its end (a
         * replacement took its address, say): its message stays owed in
         * the log (kernel ADR 0004, "Removal"). */
        if (held->owed) {
            tah_inputs_mail_done(inputs, held->address);
        }
        tah_inputs_unhold(inputs, index);
    }
    (void)tah_map_remove(&system->inputs.actor_seq, actor);
}

/* The logged actor a reference names: a named address's resident actor, or a runtime ID. */
static bool tah_inputs_logged_ref(tah_system *system, const ta_actor_ref *ref, ta_actor_id *actor, ta_address **handle) {
    *actor = ref->kind == TA_ACTOR_REF_RUNTIME ? ref->as.runtime : 0;
    if (ref->kind == TA_ACTOR_REF_NAMED) {
        ta_actor_directory_entry entry = tah_directory_find(&system->directory, ref->as.named);
        *actor = entry.kind == TA_ACTOR_DIRECTORY_RESIDENT ? entry.actor : 0;
    }
    return *actor != 0 && tah_inputs_logged_handle(system, *actor, NULL, handle);
}

ta_result tah_inputs_fired(tah_system *system, const ta_actor_ref *sender, ta_send_id send, tah_input_mark *mark) {
    *mark = (tah_input_mark){ 0 };
    ta_actor_id actor = 0;
    ta_address *handle = NULL;
    if (!system->inputs.enabled || !tah_inputs_logged_ref(system, sender, &actor, &handle)) {
        return TA_OK;
    }
    ta_str text = { 0 };
    bool has_text = tah_inputs_send_text(system, send, &text);
    tah_wal_record record = { .kind = TAH_WAL_FIRED,
                              .time = ta_system_time(system->core),
                              .field_count = 1,
                              .fields = { tah_wal_text_field(has_text, text) } };
    ta_result result = tah_inputs_write(system, (tah_inputs_target){ .address = handle, .actor = actor }, &record, mark);
    return result == TA_OK ? TA_OK : TA_ERR_NOMEM;
}

/* ------------------------------------------------------------------------
 * An operator's actions
 */

ta_result tah_system_withdraw_send(tah_system *system, ta_actor_id actor, ta_send_id send) {
    tah_input_mark mark = { 0 };
    ta_address *handle = NULL;
    if (tah_inputs_open(&system->inputs, actor)) {
        return TA_ERR_BUSY; /* a replay cannot withdraw in the middle of a macrostep (kernel ADR 0004) */
    }
    if (tah_inputs_logged_handle(system, actor, NULL, &handle)) {
        ta_str text = { 0 };
        bool has_text = tah_inputs_send_text(system, send, &text);
        tah_wal_record record = { .kind = TAH_WAL_WITHDRAWN,
                                  .time = ta_system_time(system->core),
                                  .field_count = 1,
                                  .fields = { tah_wal_text_field(has_text, text) } };
        ta_result result = tah_inputs_write(system, (tah_inputs_target){ .address = handle, .actor = actor }, &record,
                                            &mark);
        if (result != TA_OK) {
            return result;
        }
    }
    system->inside_core = true;
    ta_result result = ta_actor_withdraw_send(system->core, actor, send);
    system->inside_core = false;
    if (result != TA_OK) {
        tah_inputs_undo(system, &mark);
        return result;
    }
    if (handle != NULL) {
        tah_inputs_touch(&system->inputs, handle, false, false, 0); /* its wake may move */
    }
    return TA_OK;
}

ta_result tah_system_abandon(tah_system *system, ta_actor_id actor) {
    tah_input_mark mark = { 0 };
    ta_address *handle = NULL;
    if (tah_inputs_logged_handle(system, actor, NULL, &handle)) {
        tah_wal_record record = { .kind = TAH_WAL_ABANDONED, .time = ta_system_time(system->core) };
        ta_result result = tah_inputs_write(system, (tah_inputs_target){ .address = handle, .actor = actor }, &record,
                                            &mark);
        if (result != TA_OK) {
            return result;
        }
    }
    system->inside_core = true;
    ta_result result = ta_actor_abandon(system->core, actor);
    system->inside_core = false;
    if (result != TA_OK) {
        tah_inputs_undo(system, &mark);
        return result;
    }
    return tah_hook_actor_abandoned(system, actor);
}

void tah_inputs_redeliver(tah_inputs *inputs, uint64_t seq, ta_time since) {
    inputs->redelivering = inputs->enabled && seq != 0;
    inputs->redeliver_seq = seq;
    inputs->redeliver_since = since;
}

void tah_inputs_redelivered(tah_inputs *inputs) {
    inputs->redelivering = false;
    inputs->redeliver_seq = 0;
}

/* ------------------------------------------------------------------------
 * Wakes
 */

static bool tah_inputs_copy(tah_inputs *inputs, ta_str text, ta_str *out) {
    char *copy = ta_arena_alloc(&inputs->arena, (size_t)text.len + 1u, 1);
    if (copy == NULL) {
        return false;
    }
    tah_mem_copy(copy, text.ptr, text.len);
    copy[text.len] = '\0';
    *out = (ta_str){ .ptr = copy, .len = text.len };
    return true;
}

/* A touched address's wake; false when it has none to report (no resident
 * actor and no queued mail: it keeps the wake it had). */
static bool tah_inputs_wake_of(tah_system *system, const tah_input_touch *touch, bool *has_at, ta_time *at) {
    if (touch->fixed) {
        *has_at = touch->has_at;
        *at = touch->at;
        return true;
    }
    tah_inputs *inputs = &system->inputs;
    uint64_t key = (uint64_t)(uintptr_t)touch->address;
    uint64_t since = 0;
    bool mail = tah_map_get(&inputs->mail_since, key, &since);
    *has_at = mail;
    *at = mail ? (ta_time)since : 0;
    ta_actor_directory_entry entry = tah_directory_find(&system->directory, touch->address);
    if (entry.kind != TA_ACTOR_DIRECTORY_RESIDENT) {
        return mail;
    }
    ta_actor_inspection view;
    if (ta_actor_inspect(system->core, entry.actor, &view) != TA_OK) {
        return mail;
    }
    if (view.has_completion) {
        *has_at = false; /* finished: nothing will need it again */
        return true;
    }
    for (uint32_t index = 0; index < view.delayed_send_count; index += 1u) {
        ta_time due = view.delayed_sends[index].due;
        if (!*has_at || due < *at) {
            *has_at = true;
            *at = due;
        }
    }
    return true;
}

tah_input_wake *tah_inputs_wakes(tah_system *system, uint32_t *count) {
    tah_inputs *inputs = &system->inputs;
    *count = 0;
    uint32_t touched = inputs->touched.count;
    tah_input_wake *wakes = touched == 0 || !inputs->arena_ready
                                ? NULL
                                : ta_arena_alloc(&inputs->arena, (size_t)touched * sizeof *wakes, _Alignof(tah_input_wake));
    for (uint32_t at = 0; wakes != NULL && at < touched; at += 1u) {
        const tah_input_touch *touch = tah_vec_at(&inputs->touched, at);
        tah_input_wake wake = { 0 };
        if (!tah_inputs_wake_of(system, touch, &wake.has_at, &wake.at)) {
            continue;
        }
        ta_actor_address address = ta_address_view(touch->address);
        if (!tah_inputs_copy(inputs, address.family.name, &wake.address.family.name) ||
            !tah_inputs_copy(inputs, address.name, &wake.address.name)) {
            inputs->lost += 1u;
            continue;
        }
        wakes[(*count)++] = wake;
    }
    if (touched != 0 && wakes == NULL) {
        inputs->lost += 1u;
    }
    tah_inputs_forget_touched(inputs);
    return wakes;
}

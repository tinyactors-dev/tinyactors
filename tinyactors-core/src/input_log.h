#pragma once

#include "system.h"

/*
 * The input log (ADR 0090): core writes each input a logged named actor
 * accepts as a record in the portable layout the public header describes,
 * through the system's input sink, and replays a log one record at a time.
 *
 * Writing is part of accepting. A caller that changes the actor after the
 * record is written opens the record's unit with ta_input_open(), makes the
 * change, and closes it with ta_input_close(keep): a record is kept only
 * when its input was taken. Nothing is allocated to write a record: the
 * header, the fields, the chunk frames and windows of rendered text are
 * staged through the system's fixed scratch buffer, and values stream
 * through the data model into the sink.
 */

/* The family is logged (ta_system_log_family). */
bool ta_input_family_logged(const ta_system *system, ta_atom family);
/* The actor writes its inputs: a sink is installed, and it is published,
 * named, and of a logged family. */
bool ta_input_logged(const ta_system *system, const ta_actor *actor);

/* A text field: `text` itself, or what `write` renders. */
typedef struct {
    bool present;
    ta_str text;
    ta_text_write_fn write;
    const void *context;
} ta_record_text;

typedef struct {
    bool is_text;
    uint64_t number;
    ta_record_text text;
} ta_record_field;

#define TA_RECORD_FIELDS_MAX 12u

static inline ta_record_field ta_record_number(uint64_t number) {
    return (ta_record_field){ .number = number };
}

/* A text field; absent when `present` is false. */
static inline ta_record_field ta_record_text_field(bool present, ta_str text) {
    return (ta_record_field){ .is_text = true, .text = { .present = present, .text = text } };
}

/* What an event's text fields render from; lives while its record is written. */
typedef struct {
    const ta_system *system;
    ta_event_name_id name;
    ta_send_id send_id;
    ta_scxml_target origin;
} ta_record_event_texts;

typedef struct {
    ta_record_kind kind;
    ta_time time;
    uint32_t field_count;
    ta_record_field fields[TA_RECORD_FIELDS_MAX];
    /* JUMP: the target states, of `definition`; or, when `target_at` is
     * set, the states it gives for 0 .. target_count - 1, asked in order
     * (a checkpoint's lists, walked without an array). */
    const ta_state_id *targets;
    uint32_t target_count;
    ta_state_id (*target_at)(void *context, uint32_t index);
    void *target_context;
    /* The record's values, written by `definition`'s data model, `value`
     * then `value2`; each is absent when null or when its `v` is null. */
    const ta_value *value;
    const ta_value *value2;
    const ta_definition *definition;
} ta_record_desc;

/* The event fields of EVENT, JUMP and QUEUED records, in their order. */
typedef struct {
    ta_system *system;
    const ta_actor *actor; /* whose session renders a computed origin; may be null */
    const ta_event *event;
    uint64_t host_seq;
} ta_record_event_params;
void ta_input_event_fields(const ta_record_event_params *params, ta_record_event_texts *texts, ta_record_desc *record);
/* Adds a send ID field, rendered from `texts`, which `system` and `send` fill. */
void ta_input_send_field(ta_record_event_texts *texts, ta_send_id send, ta_record_desc *record);

/* Opens a unit and writes a whole record into it; the record takes
 * `actor`'s next number (0 for no actor). The unit stays open until
 * ta_input_close(). */
ta_result ta_input_open(ta_system *system, ta_actor *actor, const ta_record_desc *record);
void ta_input_close(ta_system *system, bool keep);
/* Opens and keeps. */
ta_result ta_input_write(ta_system *system, ta_actor *actor, const ta_record_desc *record);

/* An actor's first record makes its sparse state, which holds its record
 * number; TA_ERR_NOMEM when it cannot. */
ta_result ta_input_prepare_actor(ta_system *system, ta_actor *actor);

/* FINISHED, once, for a logged actor that reached a top-level final state. */
ta_result ta_input_finished(ta_system *system, ta_actor *actor);
/* RELEASED, once per macrostep, for a logged actor resumed without its
 * microstep limit while it is stopped as a runaway. */
ta_result ta_input_released(ta_system *system, ta_actor *actor, bool no_microstep_limit);
/* The actor is past its microstep limit, stopped inside a macrostep (ADR 0075). */
bool ta_input_runaway_stopped(const ta_system *system, const ta_actor *actor);

/* One record's fixed part and text part, in one allocation. */
typedef struct {
    ta_record_kind kind;
    uint64_t seq;
    ta_time time;
    uint32_t fields; /* 8-byte fields after the header */
    uint32_t text_bytes;
    uint16_t value_count;
    uint8_t *block;
    size_t block_bytes;
    const uint8_t *text;
} ta_input_record;

/* A log's first record, SPAWNED or CHECKPOINT, read for ta_actor_prepare().
 * A CHECKPOINT keeps its record, whose state lists ta_input_restore()
 * applies; release it with ta_input_spawn_release(). */
typedef struct {
    ta_record_kind kind;
    ta_time time;
    uint64_t seq;
    bool has_binding;
    ta_value binding; /* owned */
    ta_input_record checkpoint;
} ta_input_spawn;
void ta_input_spawn_release(ta_system *system, ta_input_spawn *spawn);
typedef struct {
    const ta_byte_source *log;
    const ta_definition *definition;
    ta_input_spawn *out;
} ta_input_spawn_params;
ta_result ta_input_read_spawn(ta_system *system, const ta_input_spawn_params *params);

/* Gives a replaying actor, just prepared and not bootstrapped, the state a
 * checkpoint holds: its lists, then its parts, read from `log`. */
typedef struct {
    ta_input_spawn *spawn;
    const ta_byte_source *log;
} ta_input_restore_params;
ta_result ta_input_restore(ta_system *system, ta_actor *actor, const ta_input_restore_params *params);

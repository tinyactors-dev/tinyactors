#pragma once
/*
 * The write-ahead log's records (kernel ADR 0002 section 4): one named
 * actor's inputs, in order, in a portable byte layout. The layout is core
 * ADR 0090 section 3's, owned by the kernel since core ADR 0094, at version
 * 2: little-endian and 8-byte aligned, in three parts.
 *
 *   fixed part   u16 version (TAH_WAL_VERSION), u16 kind (tah_wal_kind),
 *                u32 fixed_bytes (a multiple of 8, header included),
 *                u64 seq (the actor's record number in its current life,
 *                from 1; 0 for a record written while it had no resident
 *                actor), u64 time (logical milliseconds), u32 text_bytes,
 *                u16 value_count, u16 flags (0), then the kind's fields, 8
 *                bytes each: a u64 number, or a text reference (u32 offset
 *                from the start of the text part, u32 length; offset
 *                UINT32_MAX is absent)
 *   text part    text_bytes of UTF-8, padded with zeros to 8
 *   values       value_count value streams, each chunks of (u32 length,
 *                bytes, zeros to 8), ended by a chunk of length 0
 *
 * The kinds' fields, in order (`text` a reference, `u64` a number):
 *   SPAWNED     family, name, revision                     value: binding
 *   ACCEPTED    u64 id, u64 event type, name, send ID, origin, origin type,
 *               invoke ID, u64 report (tah_report_kind, 0 for a message),
 *               reason, u64 ends                           value: data
 *   DELIVERED   u64 id
 *   JUMP        u64 id, source state ID (absent: the root), u64 target
 *               count, then one text per target state ID
 *   DROPPED     u64 id
 *   FIRED       send ID          WITHDRAWN   send ID
 *   FINISHED    final state ID
 *   ABANDONED, RELEASED, DESTROYED, EVICTED: none
 * A message has an identity from the moment the kernel accepts it (section
 * 2): ACCEPTED carries its content once, DELIVERED or JUMP when core took
 * it, DROPPED when it will never be delivered.
 *
 * A checkpoint (section 7) is one unit of records, all with the
 * checkpoint's seq: CHECKPOINT, then its parts, then an ACCEPTED record for
 * each message the actor's mailbox still owed:
 *   CHECKPOINT  family, name, revision, u64 generated-ID counter, u64
 *               macrostep, u64 microstep, u64 lifecycle count, u64 part
 *               count, u64 owed count, u64 active count, u64 late count,
 *               u64 history count, then one u64 per active atomic state, one
 *               per state whose late-bound data is initialized, and two per
 *               recorded history entry (history state, state); the state IDs
 *               are the definition's own numbers, which its revision fixes
 *                                                          value: data root
 *   CHECKPOINT_SEND  send ID, event name, I/O type, destination (a target
 *               URI; absent for a send to the actor itself or to a host
 *               processor), u64 due, u64 flags (TAH_WAL_SEND_*), u64 literal
 *               target's content ID, u64 content ID
 *                                                          values: data, target
 *   CHECKPOINT_INVOKE  invoke ID, invoker type, u64 <invoke> index
 * The header's time is the actor's clock.
 *
 * A value stream is present only when the value is (value_count; a
 * CHECKPOINT_SEND's flags say which). Readers skip fields past the ones
 * they know (fixed_bytes), and refuse a version or a kind they do not know.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "containers.h"
#include "tinyactors.h"

#define TAH_WAL_VERSION ((uint16_t)2)
#define TAH_WAL_HEADER 32u

typedef enum {
    TAH_WAL_SPAWNED = 1,
    TAH_WAL_ACCEPTED = 2,
    TAH_WAL_DELIVERED = 3,
    TAH_WAL_JUMP = 4,
    TAH_WAL_DROPPED = 5,
    TAH_WAL_FIRED = 6,
    TAH_WAL_WITHDRAWN = 7,
    TAH_WAL_ABANDONED = 8,
    TAH_WAL_RELEASED = 9,
    TAH_WAL_FINISHED = 10,
    TAH_WAL_DESTROYED = 11,
    TAH_WAL_EVICTED = 12,
    TAH_WAL_CHECKPOINT = 13,
    TAH_WAL_CHECKPOINT_SEND = 14,
    TAH_WAL_CHECKPOINT_INVOKE = 15
} tah_wal_kind;

/* A CHECKPOINT_SEND's flags. */
enum {
    TAH_WAL_SEND_DATA = 1u,         /* a data stream follows */
    TAH_WAL_SEND_TARGET = 2u,       /* a target value stream follows */
    TAH_WAL_SEND_TARGET_TEXT = 4u,  /* a literal target, by content ID */
    TAH_WAL_SEND_CONTENT = 8u,      /* a literal <content>, by content ID */
    TAH_WAL_SEND_SELF = 16u,        /* to the actor itself */
    TAH_WAL_SEND_SUBMITTED = 32u    /* refused once, waiting to be offered again */
};

/* The fields a CHECKPOINT has before its lists of state numbers. */
#define TAH_WAL_CHECKPOINT_FIELDS 12u

/* One field: a number, or a text (absent when `present` is false). */
typedef struct {
    bool is_text;
    bool present;
    uint64_t number;
    ta_str text;
} tah_wal_field;

static inline tah_wal_field tah_wal_number_field(uint64_t number) {
    return (tah_wal_field){ .number = number };
}
static inline tah_wal_field tah_wal_text_field(bool present, ta_str text) {
    return (tah_wal_field){ .is_text = true, .present = present, .text = text };
}

#define TAH_WAL_FIELDS_MAX 12u

/*
 * A record to write. `fields` come first; then `texts` (a JUMP's targets),
 * one text field each; then `numbers` (a CHECKPOINT's lists), one number
 * field each. Values are written in order, those whose `v` is null
 * skipped, by `datamodel`'s codec with durable = true.
 */
typedef struct {
    tah_wal_kind kind;
    uint64_t seq;
    ta_time time;
    uint32_t field_count;
    tah_wal_field fields[TAH_WAL_FIELDS_MAX];
    const ta_str *texts;
    uint32_t text_count;
    const uint64_t *numbers;
    uint32_t number_count;
    const ta_value *values[2];
} tah_wal_record;

typedef struct {
    tah_vec *out;          /* uint8_t: the record is appended */
    ta_datamodel datamodel; /* whose codec writes the values */
    TA_OUT ta_fault *fault; /* why a value was refused; may be null */
} tah_wal_write_params;

/*
 * Appends one record to `out`. TA_ERR_UNSUPPORTED when a value has no
 * durable form, TA_ERR_NOMEM when `out` cannot grow; either way `out` is
 * as it was.
 */
ta_result tah_wal_write(const tah_wal_record *record, const tah_wal_write_params *params);

/*
 * The bytes of the first whole record at `bytes`, or, when `unit` and it is
 * a CHECKPOINT, of the whole unit it opens; 0 when they are not all there
 * yet. Bytes that are no record at all answer `length`, for the reader to
 * refuse.
 */
size_t tah_wal_span(const uint8_t *bytes, size_t length, bool unit);

/* One whole record, read in place. */
typedef struct {
    uint16_t version;
    tah_wal_kind kind;
    uint64_t seq;
    ta_time time;
    uint32_t field_count;
    const uint8_t *fields;
    uint32_t text_bytes;
    const uint8_t *text;
    uint16_t value_count;
    const uint8_t *values; /* the first value stream */
    size_t span;           /* the record's bytes */
} tah_wal_view;

/*
 * Reads the record at `bytes`, whose span tah_wal_span() found. TA_ERR_CORRUPT
 * for a version or kind this build does not read, fewer fields than the kind
 * has, or a text reference outside the text part.
 */
ta_result tah_wal_read(const uint8_t *bytes, size_t length, tah_wal_view *out);
/* Field `index` as a number (0 when it is past the record's fields). */
uint64_t tah_wal_number(const tah_wal_view *view, uint32_t index);
/* Field `index` as a text; false when it is absent. */
bool tah_wal_text(const tah_wal_view *view, uint32_t index, ta_str *out);

/* A value stream of a record, read as a byte source up to its end. */
typedef struct {
    const uint8_t *at;
    const uint8_t *end;
    const uint8_t *next; /* where the current chunk's padding ends */
    uint32_t left;       /* bytes of the current chunk not yet read */
    bool done;
} tah_wal_value_reader;
/* Positions `reader` at the record's value stream `index`; false when it has none. */
bool tah_wal_value_at(const tah_wal_view *view, uint32_t index, tah_wal_value_reader *reader);
/* The reader as a source, for a data model's value_read. */
ta_byte_source tah_wal_value_source(tah_wal_value_reader *reader);

/*
 * Reads value stream `index` with `datamodel`'s codec (durable = true) into
 * `out`, owned by the caller. TA_ERR_NOT_FOUND when the record has no such
 * stream; TA_ERR_CORRUPT when the data model refuses the bytes.
 */
ta_result tah_wal_read_value(const tah_wal_view *view, uint32_t index, ta_datamodel datamodel, ta_value *out);

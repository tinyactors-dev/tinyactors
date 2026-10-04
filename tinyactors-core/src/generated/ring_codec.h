/**
 * @file ring_codec.h
 *
 * # The record codec
 *
 * Generated from `protocol/catalog.rb` by `mise run protocol` in
 * tinyactors-core. Do not edit: change the catalog, or the renderer
 * (`protocol/render.rb`), and render again.
 *
 * Writes the records of `ring_records.h` into bytes a producer reserved,
 * and checks a record a consumer read before anything trusts it. h2c is
 * where a host's bytes enter core, so the reader is where hostile or
 * corrupt input meets core.
 *
 * ## Writing
 *
 * For every record `<ring>.<name>`:
 *
 * - `ta_<ring>_<name>_write(out, fixed, texts)` fills the record in place
 *   in `out`, for example the bytes and size of a `ta_ring_reserve`
 *   reservation. It copies the fixed part from `fixed`, every byte of it,
 *   its pads included (build it with `{0}`); sets `kind` to the record's
 *   code and `size` to `out.size`; keeps the `seq` already in `out`,
 *   which the reservation stamped; lays the texts end to end in the
 *   tail, in the order the catalog declares them, starting right after
 *   the fixed part; points each span at its text, an empty one at where
 *   it would start; and zeroes every byte after the last text up to
 *   `out.size`. The `kind`, `size`, `seq` and span fields of `fixed` are
 *   ignored. `fixed` may be `out.bytes` itself, so a producer can build
 *   the fixed part in the reservation; the texts must not lie in `out`.
 * - A record with text or bytes has `ta_<ring>_<name>_texts`: a `ta_str`
 *   per text field and a `ta_bytes` per bytes field, named after it. Its
 *   `ta_<ring>_<name>_size(texts, &size)` answers the bytes to reserve:
 *   the fixed part plus the texts' lengths, rounded up to 8. A record
 *   without them has no `texts` parameter, and its size is `sizeof` its
 *   struct.
 *
 * ## Reading
 *
 * `ta_record_read` checks a record's bytes, for example those of a
 * `ta_ring_peek` record, as a record of the ring they were read from:
 *
 * - the bytes hold at least the 32-byte header, and the kind is not the
 *   pad;
 * - the kind is one the catalog defines, and a kind of that ring;
 * - its `size` is a multiple of 8, at least the kind's fixed part and at
 *   most the bytes given;
 * - every span lies in the tail: it starts at or after the fixed part
 *   and ends at or before `size`, an empty one included.
 *
 * One generated descriptor per kind (its ring, its fixed size and the
 * offsets of its spans) and one loop do these checks for every kind;
 * `ta_record_describe` answers a kind's descriptor. The reader does not
 * check `seq`: `ta_ring_peek` does, gap-free on a lossless ring and only
 * growing on a lossy one. Nor does it check flag bits or enum values:
 * whoever uses a field checks its value.
 *
 * After `TA_OK`, `ta_<ring>_<name>_from_view` answers the typed record,
 * and `ta_record_view_text` and `ta_record_view_bytes` a span's bytes.
 *
 * ## Memory
 *
 * None. Every function borrows the caller's bytes for one call: a
 * writer writes only `out.bytes[0, out.size)` and reads `fixed` and the
 * texts; the reader reads only `bytes[0, size)` of its parameters. A
 * view, a typed record and a text it answers point into the caller's
 * bytes and stay valid as long as those do, for a ring record until it
 * is released. A description points into constant tables. Nothing
 * allocates, so every function may run inside a macrostep.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_<ring>_<name>_write` | O(record size) |
 * | `ta_<ring>_<name>_size` | O(spans of the kind) |
 * | `ta_record_read` | O(1) to find the kind, plus O(spans of the kind) |
 * | `ta_record_describe`, `ta_<ring>_<name>_from_view`, `ta_record_view_text`, `ta_record_view_bytes` | O(1) |
 *
 * ## Failure
 *
 * A function that can fail answers a `ta_result`, and when it fails it
 * changes nothing: no byte of `out`, no `*out`.
 *
 * - A writer answers `TA_ERR_INVALID` when `out.bytes` is NULL or not
 *   8-byte aligned, `out.size` is not a multiple of 8, `fixed` or
 *   `texts` is NULL, a text has a length but no bytes, or the record
 *   would exceed the largest size a `u32` holds; and `TA_ERR_FULL` when
 *   `out.size` is smaller than the record.
 * - A size function answers `TA_ERR_INVALID` for the same texts.
 * - `ta_record_read` answers `TA_ERR_INVALID` when its bytes are NULL or
 *   not 8-byte aligned, or its ring is not a `ta_record_ring`;
 *   `TA_ERR_UNSUPPORTED` for a kind the catalog does not define; and
 *   `TA_ERR_CORRUPT` for any other check above that fails.
 * - `ta_record_describe` answers `TA_ERR_NOT_FOUND` for a kind the
 *   catalog does not define, the pad included.
 *
 * ## Users
 *
 * Core reads h2c records and writes c2h and tel records; a host does
 * the reverse; a test drives core by writing h2c records and checking
 * the c2h records it reads back. Each side reserves and peeks through
 * `ta_ring`.
 */
#pragma once

#include "../../include/tinyactors.h"
#include "ring_records.h"
#include "../span.h"

/** Where a writer puts a record: for example a ring reservation's bytes and size. */
typedef struct {
    /** The record's first byte, 8-byte aligned. */
    unsigned char *bytes;
    /** The bytes the record takes, a multiple of 8; its `size` becomes this. */
    uint32_t size;
} ta_record_buffer;

/** What `ta_record_read` checks. */
typedef struct {
    /** The record's first byte, 8-byte aligned; borrowed for the call. */
    const unsigned char *bytes;
    /** The bytes readable from `bytes`; the record's `size` may not pass it. */
    uint32_t size;
    /** The ring the record was read from. */
    ta_record_ring ring;
} ta_record_read_params;

/** A record `ta_record_read` checked. */
typedef struct {
    /** The record's first byte; borrowed from the caller. */
    const unsigned char *bytes;
    /** The record's `size`. */
    uint32_t size;
    /** The record's `kind`, a `ta_record_kind`. */
    uint16_t kind;
} ta_record_view;

/** What the codec knows of a kind. */
typedef struct {
    /** The ring the kind crosses. */
    ta_record_ring ring;
    /** Its fixed part in bytes: `sizeof` its struct. */
    uint32_t fixed_size;
    /** How many span fields it has. */
    uint32_t span_count;
    /** The offset of each span field, in the order the catalog declares them; constant. */
    const uint32_t *span_offsets;
} ta_record_description;

/**
 * Checks the record at `params->bytes` as a record of `params->ring`
 * (see the file comment) and answers it in `*out`. Borrows the bytes
 * for the call; the view points into them. Never allocates; may run
 * inside a macrostep. O(1) plus one step per span of the kind.
 *
 * Answers `TA_ERR_INVALID` for NULL or unaligned bytes or an unknown
 * ring, `TA_ERR_UNSUPPORTED` for a kind the catalog does not define,
 * and `TA_ERR_CORRUPT` for a record too short for the header, a pad, a
 * kind of another ring, a wrong size or a span outside the tail;
 * `*out` is then not written.
 */
ta_result ta_record_read(const ta_record_read_params *params, ta_record_view *out);

/**
 * Answers in `*out` the ring, fixed size and span offsets of `kind`.
 * Never allocates; may run inside a macrostep. O(1).
 *
 * Answers `TA_ERR_NOT_FOUND`, and does not write `*out`, for a kind
 * the catalog does not define, the pad included.
 */
ta_result ta_record_describe(uint16_t kind, ta_record_description *out);

/**
 * The text `span` names in the record `view` holds. `view` comes from
 * `ta_record_read`, which checked the record's own spans; a span that
 * does not lie inside the record answers empty text with a NULL
 * pointer. Borrows the record's bytes. Never allocates. O(1).
 */
static inline ta_str ta_record_view_text(const ta_record_view *view, ta_span32 span) {
    ta_str text = { NULL, 0u };
    if (span.offset <= view->size && span.length <= view->size - span.offset) {
        text.ptr = (const char *)view->bytes + span.offset;
        text.len = span.length;
    }
    return text;
}

/**
 * The bytes `span` names in the record `view` holds, as
 * `ta_record_view_text` answers text. Never allocates. O(1).
 */
static inline ta_bytes ta_record_view_bytes(const ta_record_view *view, ta_span32 span) {
    ta_bytes bytes = { NULL, 0u };
    if (span.offset <= view->size && span.length <= view->size - span.offset) {
        bytes.ptr = (const uint8_t *)view->bytes + span.offset;
        bytes.len = span.length;
    }
    return bytes;
}

/* ---- h2c.time ---- */

/**
 * Writes an `h2c.time` record into `out` (see the file comment): the fixed part
 * from `fixed`, kind 0x0101, size `out.size`, the seq already in `out`. Borrows
 * `fixed` for the call. Never allocates; may run inside a macrostep. Answers
 * `TA_ERR_INVALID` for a bad buffer or argument and `TA_ERR_FULL` when
 * `out.size` is too small, and then writes nothing.
 */
ta_result ta_h2c_time_write(ta_record_buffer out, const ta_h2c_time *fixed);

/**
 * The `h2c.time` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_h2c_time *ta_h2c_time_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_TIME ? (const ta_h2c_time *)(const void *)view->bytes : NULL;
}

/* ---- h2c.event ---- */

/**
 * The text and bytes of an `h2c.event` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    /** the address (family/name), when session is 0 */
    ta_str target;
    /** the dotted event name, as bytes */
    ta_str name;
    /** _event.sendid, or empty */
    ta_str sendid;
    /** _event.origin, or empty */
    ta_str origin;
    /** _event.origintype, or empty */
    ta_str origin_type;
    /** _event.invokeid for an invoked service, or empty */
    ta_str invokeid;
} ta_h2c_event_texts;

/**
 * Answers in `*out` the bytes an `h2c.event` record with `texts` takes: its
 * fixed part, 128 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_h2c_event_size(const ta_h2c_event_texts *texts, uint32_t *out);

/**
 * Writes an `h2c.event` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x0102, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_h2c_event_write(ta_record_buffer out, const ta_h2c_event *fixed, const ta_h2c_event_texts *texts);

/**
 * The `h2c.event` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_h2c_event *ta_h2c_event_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_EVENT ? (const ta_h2c_event *)(const void *)view->bytes : NULL;
}

/* ---- h2c.spawn ---- */

/**
 * The text and bytes of an `h2c.spawn` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    /** family/name */
    ta_str address;
    /** the definition's revision hash */
    ta_bytes revision;
} ta_h2c_spawn_texts;

/**
 * Answers in `*out` the bytes an `h2c.spawn` record with `texts` takes: its
 * fixed part, 112 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_h2c_spawn_size(const ta_h2c_spawn_texts *texts, uint32_t *out);

/**
 * Writes an `h2c.spawn` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x0103, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_h2c_spawn_write(ta_record_buffer out, const ta_h2c_spawn *fixed, const ta_h2c_spawn_texts *texts);

/**
 * The `h2c.spawn` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_h2c_spawn *ta_h2c_spawn_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_SPAWN ? (const ta_h2c_spawn *)(const void *)view->bytes : NULL;
}

/* ---- h2c.publish ---- */

/**
 * The text and bytes of an `h2c.publish` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    /** family/name, or empty for every replaying session */
    ta_str address;
} ta_h2c_publish_texts;

/**
 * Answers in `*out` the bytes an `h2c.publish` record with `texts` takes: its
 * fixed part, 48 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_h2c_publish_size(const ta_h2c_publish_texts *texts, uint32_t *out);

/**
 * Writes an `h2c.publish` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x0104, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_h2c_publish_write(ta_record_buffer out, const ta_h2c_publish *fixed, const ta_h2c_publish_texts *texts);

/**
 * The `h2c.publish` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_h2c_publish *ta_h2c_publish_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_PUBLISH ? (const ta_h2c_publish *)(const void *)view->bytes : NULL;
}

/* ---- h2c.load ---- */

/**
 * The text and bytes of an `h2c.load` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str address;
} ta_h2c_load_texts;

/**
 * Answers in `*out` the bytes an `h2c.load` record with `texts` takes: its
 * fixed part, 56 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_h2c_load_size(const ta_h2c_load_texts *texts, uint32_t *out);

/**
 * Writes an `h2c.load` record into `out` (see the file comment): the fixed part
 * from `fixed`, kind 0x0105, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_h2c_load_write(ta_record_buffer out, const ta_h2c_load *fixed, const ta_h2c_load_texts *texts);

/**
 * The `h2c.load` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_h2c_load *ta_h2c_load_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_LOAD ? (const ta_h2c_load *)(const void *)view->bytes : NULL;
}

/* ---- h2c.load_failed ---- */

/**
 * The text and bytes of an `h2c.load_failed` record, in the order they lie in
 * its tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str address;
    ta_str reason;
} ta_h2c_load_failed_texts;

/**
 * Answers in `*out` the bytes an `h2c.load_failed` record with `texts` takes:
 * its fixed part, 56 bytes, plus their lengths, rounded up to 8. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not
 * write `*out`, when `texts` is NULL, a text has a length but no bytes, or the
 * record would exceed the largest size a `u32` holds.
 */
ta_result ta_h2c_load_failed_size(const ta_h2c_load_failed_texts *texts, uint32_t *out);

/**
 * Writes an `h2c.load_failed` record into `out` (see the file comment): the
 * fixed part from `fixed`, kind 0x0106, size `out.size`, the seq already in
 * `out` and `texts` in the tail. Borrows `fixed` and `texts` for the call.
 * Never allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a
 * bad buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and
 * then writes nothing.
 */
ta_result ta_h2c_load_failed_write(ta_record_buffer out, const ta_h2c_load_failed *fixed, const ta_h2c_load_failed_texts *texts);

/**
 * The `h2c.load_failed` record `view` holds, or NULL when it holds another
 * kind. `view` comes from `ta_record_read`; the record is borrowed from its
 * bytes. Never allocates. O(1).
 */
static inline const ta_h2c_load_failed *ta_h2c_load_failed_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_LOAD_FAILED ? (const ta_h2c_load_failed *)(const void *)view->bytes : NULL;
}

/* ---- h2c.unload ---- */

/**
 * The text and bytes of an `h2c.unload` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str address;
} ta_h2c_unload_texts;

/**
 * Answers in `*out` the bytes an `h2c.unload` record with `texts` takes: its
 * fixed part, 48 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_h2c_unload_size(const ta_h2c_unload_texts *texts, uint32_t *out);

/**
 * Writes an `h2c.unload` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x0107, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_h2c_unload_write(ta_record_buffer out, const ta_h2c_unload *fixed, const ta_h2c_unload_texts *texts);

/**
 * The `h2c.unload` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_h2c_unload *ta_h2c_unload_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_UNLOAD ? (const ta_h2c_unload *)(const void *)view->bytes : NULL;
}

/* ---- h2c.checkpoint ---- */

/**
 * The text and bytes of an `h2c.checkpoint` record, in the order they lie in
 * its tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str address;
} ta_h2c_checkpoint_texts;

/**
 * Answers in `*out` the bytes an `h2c.checkpoint` record with `texts` takes:
 * its fixed part, 48 bytes, plus their lengths, rounded up to 8. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not
 * write `*out`, when `texts` is NULL, a text has a length but no bytes, or the
 * record would exceed the largest size a `u32` holds.
 */
ta_result ta_h2c_checkpoint_size(const ta_h2c_checkpoint_texts *texts, uint32_t *out);

/**
 * Writes an `h2c.checkpoint` record into `out` (see the file comment): the
 * fixed part from `fixed`, kind 0x0108, size `out.size`, the seq already in
 * `out` and `texts` in the tail. Borrows `fixed` and `texts` for the call.
 * Never allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a
 * bad buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and
 * then writes nothing.
 */
ta_result ta_h2c_checkpoint_write(ta_record_buffer out, const ta_h2c_checkpoint *fixed, const ta_h2c_checkpoint_texts *texts);

/**
 * The `h2c.checkpoint` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_h2c_checkpoint *ta_h2c_checkpoint_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_CHECKPOINT ? (const ta_h2c_checkpoint *)(const void *)view->bytes : NULL;
}

/* ---- h2c.invoke_report ---- */

/**
 * The text and bytes of an `h2c.invoke_report` record, in the order they lie in
 * its tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str invokeid;
    /** for error: the reason segment */
    ta_str reason;
} ta_h2c_invoke_report_texts;

/**
 * Answers in `*out` the bytes an `h2c.invoke_report` record with `texts` takes:
 * its fixed part, 96 bytes, plus their lengths, rounded up to 8. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not
 * write `*out`, when `texts` is NULL, a text has a length but no bytes, or the
 * record would exceed the largest size a `u32` holds.
 */
ta_result ta_h2c_invoke_report_size(const ta_h2c_invoke_report_texts *texts, uint32_t *out);

/**
 * Writes an `h2c.invoke_report` record into `out` (see the file comment): the
 * fixed part from `fixed`, kind 0x0109, size `out.size`, the seq already in
 * `out` and `texts` in the tail. Borrows `fixed` and `texts` for the call.
 * Never allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a
 * bad buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and
 * then writes nothing.
 */
ta_result ta_h2c_invoke_report_write(ta_record_buffer out, const ta_h2c_invoke_report *fixed, const ta_h2c_invoke_report_texts *texts);

/**
 * The `h2c.invoke_report` record `view` holds, or NULL when it holds another
 * kind. `view` comes from `ta_record_read`; the record is borrowed from its
 * bytes. Never allocates. O(1).
 */
static inline const ta_h2c_invoke_report *ta_h2c_invoke_report_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_INVOKE_REPORT ? (const ta_h2c_invoke_report *)(const void *)view->bytes : NULL;
}

/* ---- h2c.destroy ---- */

/**
 * The text and bytes of an `h2c.destroy` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str address;
} ta_h2c_destroy_texts;

/**
 * Answers in `*out` the bytes an `h2c.destroy` record with `texts` takes: its
 * fixed part, 40 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_h2c_destroy_size(const ta_h2c_destroy_texts *texts, uint32_t *out);

/**
 * Writes an `h2c.destroy` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x010A, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_h2c_destroy_write(ta_record_buffer out, const ta_h2c_destroy *fixed, const ta_h2c_destroy_texts *texts);

/**
 * The `h2c.destroy` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_h2c_destroy *ta_h2c_destroy_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_DESTROY ? (const ta_h2c_destroy *)(const void *)view->bytes : NULL;
}

/* ---- h2c.definition_register ---- */

/**
 * Writes an `h2c.definition_register` record into `out` (see the file comment):
 * the fixed part from `fixed`, kind 0x010B, size `out.size`, the seq already in
 * `out`. Borrows `fixed` for the call. Never allocates; may run inside a
 * macrostep. Answers `TA_ERR_INVALID` for a bad buffer or argument and
 * `TA_ERR_FULL` when `out.size` is too small, and then writes nothing.
 */
ta_result ta_h2c_definition_register_write(ta_record_buffer out, const ta_h2c_definition_register *fixed);

/**
 * The `h2c.definition_register` record `view` holds, or NULL when it holds
 * another kind. `view` comes from `ta_record_read`; the record is borrowed from
 * its bytes. Never allocates. O(1).
 */
static inline const ta_h2c_definition_register *ta_h2c_definition_register_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_DEFINITION_REGISTER ? (const ta_h2c_definition_register *)(const void *)view->bytes : NULL;
}

/* ---- h2c.definition_release ---- */

/**
 * Writes an `h2c.definition_release` record into `out` (see the file comment):
 * the fixed part from `fixed`, kind 0x010C, size `out.size`, the seq already in
 * `out`. Borrows `fixed` for the call. Never allocates; may run inside a
 * macrostep. Answers `TA_ERR_INVALID` for a bad buffer or argument and
 * `TA_ERR_FULL` when `out.size` is too small, and then writes nothing.
 */
ta_result ta_h2c_definition_release_write(ta_record_buffer out, const ta_h2c_definition_release *fixed);

/**
 * The `h2c.definition_release` record `view` holds, or NULL when it holds
 * another kind. `view` comes from `ta_record_read`; the record is borrowed from
 * its bytes. Never allocates. O(1).
 */
static inline const ta_h2c_definition_release *ta_h2c_definition_release_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_DEFINITION_RELEASE ? (const ta_h2c_definition_release *)(const void *)view->bytes : NULL;
}

/* ---- h2c.definition_retire ---- */

/**
 * Writes an `h2c.definition_retire` record into `out` (see the file comment):
 * the fixed part from `fixed`, kind 0x010D, size `out.size`, the seq already in
 * `out`. Borrows `fixed` for the call. Never allocates; may run inside a
 * macrostep. Answers `TA_ERR_INVALID` for a bad buffer or argument and
 * `TA_ERR_FULL` when `out.size` is too small, and then writes nothing.
 */
ta_result ta_h2c_definition_retire_write(ta_record_buffer out, const ta_h2c_definition_retire *fixed);

/**
 * The `h2c.definition_retire` record `view` holds, or NULL when it holds
 * another kind. `view` comes from `ta_record_read`; the record is borrowed from
 * its bytes. Never allocates. O(1).
 */
static inline const ta_h2c_definition_retire *ta_h2c_definition_retire_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_DEFINITION_RETIRE ? (const ta_h2c_definition_retire *)(const void *)view->bytes : NULL;
}

/* ---- h2c.withdraw ---- */

/**
 * Writes an `h2c.withdraw` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x010E, size `out.size`, the seq already in `out`.
 * Borrows `fixed` for the call. Never allocates; may run inside a macrostep.
 * Answers `TA_ERR_INVALID` for a bad buffer or argument and `TA_ERR_FULL` when
 * `out.size` is too small, and then writes nothing.
 */
ta_result ta_h2c_withdraw_write(ta_record_buffer out, const ta_h2c_withdraw *fixed);

/**
 * The `h2c.withdraw` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_h2c_withdraw *ta_h2c_withdraw_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_WITHDRAW ? (const ta_h2c_withdraw *)(const void *)view->bytes : NULL;
}

/* ---- h2c.type_declare ---- */

/**
 * The text and bytes of an `h2c.type_declare` record, in the order they lie in
 * its tail; a writer borrows them for one call.
 */
typedef struct {
    /** the type name as bytes; interned as an atom when declared */
    ta_str type;
} ta_h2c_type_declare_texts;

/**
 * Answers in `*out` the bytes an `h2c.type_declare` record with `texts` takes:
 * its fixed part, 48 bytes, plus their lengths, rounded up to 8. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not
 * write `*out`, when `texts` is NULL, a text has a length but no bytes, or the
 * record would exceed the largest size a `u32` holds.
 */
ta_result ta_h2c_type_declare_size(const ta_h2c_type_declare_texts *texts, uint32_t *out);

/**
 * Writes an `h2c.type_declare` record into `out` (see the file comment): the
 * fixed part from `fixed`, kind 0x010F, size `out.size`, the seq already in
 * `out` and `texts` in the tail. Borrows `fixed` and `texts` for the call.
 * Never allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a
 * bad buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and
 * then writes nothing.
 */
ta_result ta_h2c_type_declare_write(ta_record_buffer out, const ta_h2c_type_declare *fixed, const ta_h2c_type_declare_texts *texts);

/**
 * The `h2c.type_declare` record `view` holds, or NULL when it holds another
 * kind. `view` comes from `ta_record_read`; the record is borrowed from its
 * bytes. Never allocates. O(1).
 */
static inline const ta_h2c_type_declare *ta_h2c_type_declare_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_H2C_TYPE_DECLARE ? (const ta_h2c_type_declare *)(const void *)view->bytes : NULL;
}

/* ---- c2h.spawned ---- */

/**
 * The text and bytes of a `c2h.spawned` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str address;
    ta_bytes revision;
} ta_c2h_spawned_texts;

/**
 * Answers in `*out` the bytes a `c2h.spawned` record with `texts` takes: its
 * fixed part, 64 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_c2h_spawned_size(const ta_c2h_spawned_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.spawned` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x0201, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_c2h_spawned_write(ta_record_buffer out, const ta_c2h_spawned *fixed, const ta_c2h_spawned_texts *texts);

/**
 * The `c2h.spawned` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_c2h_spawned *ta_c2h_spawned_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_SPAWNED ? (const ta_c2h_spawned *)(const void *)view->bytes : NULL;
}

/* ---- c2h.input ---- */

/**
 * The text and bytes of a `c2h.input` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str name;
    ta_str sendid;
    ta_str origin;
    ta_str origin_type;
    ta_str invokeid;
    /**
     * source invoke_report: original report invocation ID, separate from event
     * invokeid
     */
    ta_str report_invokeid;
    /** source invoke_report/error: original reason token */
    ta_str report_reason;
} ta_c2h_input_texts;

/**
 * Answers in `*out` the bytes a `c2h.input` record with `texts` takes: its
 * fixed part, 152 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_c2h_input_size(const ta_c2h_input_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.input` record into `out` (see the file comment): the fixed part
 * from `fixed`, kind 0x0202, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_c2h_input_write(ta_record_buffer out, const ta_c2h_input *fixed, const ta_c2h_input_texts *texts);

/**
 * The `c2h.input` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_c2h_input *ta_c2h_input_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_INPUT ? (const ta_c2h_input *)(const void *)view->bytes : NULL;
}

/* ---- c2h.send ---- */

/**
 * The text and bytes of a `c2h.send` record, in the order they lie in its tail;
 * a writer borrows them for one call.
 */
typedef struct {
    ta_str sendid;
    ta_str io_type;
    ta_str target;
    ta_str name;
} ta_c2h_send_texts;

/**
 * Answers in `*out` the bytes a `c2h.send` record with `texts` takes: its fixed
 * part, 120 bytes, plus their lengths, rounded up to 8. Never allocates; may
 * run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write `*out`,
 * when `texts` is NULL, a text has a length but no bytes, or the record would
 * exceed the largest size a `u32` holds.
 */
ta_result ta_c2h_send_size(const ta_c2h_send_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.send` record into `out` (see the file comment): the fixed part
 * from `fixed`, kind 0x0203, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_c2h_send_write(ta_record_buffer out, const ta_c2h_send *fixed, const ta_c2h_send_texts *texts);

/**
 * The `c2h.send` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_c2h_send *ta_c2h_send_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_SEND ? (const ta_c2h_send *)(const void *)view->bytes : NULL;
}

/* ---- c2h.invoke ---- */

/**
 * The text and bytes of a `c2h.invoke` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str invokeid;
    ta_str type;
    ta_str src;
} ta_c2h_invoke_texts;

/**
 * Answers in `*out` the bytes a `c2h.invoke` record with `texts` takes: its
 * fixed part, 112 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_c2h_invoke_size(const ta_c2h_invoke_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.invoke` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x0204, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_c2h_invoke_write(ta_record_buffer out, const ta_c2h_invoke *fixed, const ta_c2h_invoke_texts *texts);

/**
 * The `c2h.invoke` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_c2h_invoke *ta_c2h_invoke_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_INVOKE ? (const ta_c2h_invoke *)(const void *)view->bytes : NULL;
}

/* ---- c2h.invoke_cancel ---- */

/**
 * The text and bytes of a `c2h.invoke_cancel` record, in the order they lie in
 * its tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str invokeid;
} ta_c2h_invoke_cancel_texts;

/**
 * Answers in `*out` the bytes a `c2h.invoke_cancel` record with `texts` takes:
 * its fixed part, 56 bytes, plus their lengths, rounded up to 8. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not
 * write `*out`, when `texts` is NULL, a text has a length but no bytes, or the
 * record would exceed the largest size a `u32` holds.
 */
ta_result ta_c2h_invoke_cancel_size(const ta_c2h_invoke_cancel_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.invoke_cancel` record into `out` (see the file comment): the
 * fixed part from `fixed`, kind 0x0205, size `out.size`, the seq already in
 * `out` and `texts` in the tail. Borrows `fixed` and `texts` for the call.
 * Never allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a
 * bad buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and
 * then writes nothing.
 */
ta_result ta_c2h_invoke_cancel_write(ta_record_buffer out, const ta_c2h_invoke_cancel *fixed, const ta_c2h_invoke_cancel_texts *texts);

/**
 * The `c2h.invoke_cancel` record `view` holds, or NULL when it holds another
 * kind. `view` comes from `ta_record_read`; the record is borrowed from its
 * bytes. Never allocates. O(1).
 */
static inline const ta_c2h_invoke_cancel *ta_c2h_invoke_cancel_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_INVOKE_CANCEL ? (const ta_c2h_invoke_cancel *)(const void *)view->bytes : NULL;
}

/* ---- c2h.invoke_forward ---- */

/**
 * The text and bytes of a `c2h.invoke_forward` record, in the order they lie in
 * its tail; a writer borrows them for one call.
 */
typedef struct {
    /** destination invocation, not the forwarded event invokeid */
    ta_str invokeid;
    ta_str name;
    ta_str sendid;
    ta_str origin;
    /** the original event origintype */
    ta_str origin_type;
    /** the original event invokeid */
    ta_str event_invokeid;
} ta_c2h_invoke_forward_texts;

/**
 * Answers in `*out` the bytes a `c2h.invoke_forward` record with `texts` takes:
 * its fixed part, 136 bytes, plus their lengths, rounded up to 8. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not
 * write `*out`, when `texts` is NULL, a text has a length but no bytes, or the
 * record would exceed the largest size a `u32` holds.
 */
ta_result ta_c2h_invoke_forward_size(const ta_c2h_invoke_forward_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.invoke_forward` record into `out` (see the file comment): the
 * fixed part from `fixed`, kind 0x0206, size `out.size`, the seq already in
 * `out` and `texts` in the tail. Borrows `fixed` and `texts` for the call.
 * Never allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a
 * bad buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and
 * then writes nothing.
 */
ta_result ta_c2h_invoke_forward_write(ta_record_buffer out, const ta_c2h_invoke_forward *fixed, const ta_c2h_invoke_forward_texts *texts);

/**
 * The `c2h.invoke_forward` record `view` holds, or NULL when it holds another
 * kind. `view` comes from `ta_record_read`; the record is borrowed from its
 * bytes. Never allocates. O(1).
 */
static inline const ta_c2h_invoke_forward *ta_c2h_invoke_forward_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_INVOKE_FORWARD ? (const ta_c2h_invoke_forward *)(const void *)view->bytes : NULL;
}

/* ---- c2h.log ---- */

/**
 * The text and bytes of a `c2h.log` record, in the order they lie in its tail;
 * a writer borrows them for one call.
 */
typedef struct {
    ta_str label;
} ta_c2h_log_texts;

/**
 * Answers in `*out` the bytes a `c2h.log` record with `texts` takes: its fixed
 * part, 88 bytes, plus their lengths, rounded up to 8. Never allocates; may run
 * inside a macrostep. Answers `TA_ERR_INVALID`, and does not write `*out`, when
 * `texts` is NULL, a text has a length but no bytes, or the record would exceed
 * the largest size a `u32` holds.
 */
ta_result ta_c2h_log_size(const ta_c2h_log_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.log` record into `out` (see the file comment): the fixed part
 * from `fixed`, kind 0x0207, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_c2h_log_write(ta_record_buffer out, const ta_c2h_log *fixed, const ta_c2h_log_texts *texts);

/**
 * The `c2h.log` record `view` holds, or NULL when it holds another kind. `view`
 * comes from `ta_record_read`; the record is borrowed from its bytes. Never
 * allocates. O(1).
 */
static inline const ta_c2h_log *ta_c2h_log_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_LOG ? (const ta_c2h_log *)(const void *)view->bytes : NULL;
}

/* ---- c2h.macrostep_ended ---- */

/**
 * Writes a `c2h.macrostep_ended` record into `out` (see the file comment): the
 * fixed part from `fixed`, kind 0x0208, size `out.size`, the seq already in
 * `out`. Borrows `fixed` for the call. Never allocates; may run inside a
 * macrostep. Answers `TA_ERR_INVALID` for a bad buffer or argument and
 * `TA_ERR_FULL` when `out.size` is too small, and then writes nothing.
 */
ta_result ta_c2h_macrostep_ended_write(ta_record_buffer out, const ta_c2h_macrostep_ended *fixed);

/**
 * The `c2h.macrostep_ended` record `view` holds, or NULL when it holds another
 * kind. `view` comes from `ta_record_read`; the record is borrowed from its
 * bytes. Never allocates. O(1).
 */
static inline const ta_c2h_macrostep_ended *ta_c2h_macrostep_ended_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_MACROSTEP_ENDED ? (const ta_c2h_macrostep_ended *)(const void *)view->bytes : NULL;
}

/* ---- c2h.wake ---- */

/**
 * Writes a `c2h.wake` record into `out` (see the file comment): the fixed part
 * from `fixed`, kind 0x0209, size `out.size`, the seq already in `out`. Borrows
 * `fixed` for the call. Never allocates; may run inside a macrostep. Answers
 * `TA_ERR_INVALID` for a bad buffer or argument and `TA_ERR_FULL` when
 * `out.size` is too small, and then writes nothing.
 */
ta_result ta_c2h_wake_write(ta_record_buffer out, const ta_c2h_wake *fixed);

/**
 * The `c2h.wake` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_c2h_wake *ta_c2h_wake_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_WAKE ? (const ta_c2h_wake *)(const void *)view->bytes : NULL;
}

/* ---- c2h.bounced ---- */

/**
 * The text and bytes of a `c2h.bounced` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str detail;
} ta_c2h_bounced_texts;

/**
 * Answers in `*out` the bytes a `c2h.bounced` record with `texts` takes: its
 * fixed part, 56 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_c2h_bounced_size(const ta_c2h_bounced_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.bounced` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x020A, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_c2h_bounced_write(ta_record_buffer out, const ta_c2h_bounced *fixed, const ta_c2h_bounced_texts *texts);

/**
 * The `c2h.bounced` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_c2h_bounced *ta_c2h_bounced_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_BOUNCED ? (const ta_c2h_bounced *)(const void *)view->bytes : NULL;
}

/* ---- c2h.load_needed ---- */

/**
 * The text and bytes of a `c2h.load_needed` record, in the order they lie in
 * its tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str address;
} ta_c2h_load_needed_texts;

/**
 * Answers in `*out` the bytes a `c2h.load_needed` record with `texts` takes:
 * its fixed part, 48 bytes, plus their lengths, rounded up to 8. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not
 * write `*out`, when `texts` is NULL, a text has a length but no bytes, or the
 * record would exceed the largest size a `u32` holds.
 */
ta_result ta_c2h_load_needed_size(const ta_c2h_load_needed_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.load_needed` record into `out` (see the file comment): the
 * fixed part from `fixed`, kind 0x020B, size `out.size`, the seq already in
 * `out` and `texts` in the tail. Borrows `fixed` and `texts` for the call.
 * Never allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a
 * bad buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and
 * then writes nothing.
 */
ta_result ta_c2h_load_needed_write(ta_record_buffer out, const ta_c2h_load_needed *fixed, const ta_c2h_load_needed_texts *texts);

/**
 * The `c2h.load_needed` record `view` holds, or NULL when it holds another
 * kind. `view` comes from `ta_record_read`; the record is borrowed from its
 * bytes. Never allocates. O(1).
 */
static inline const ta_c2h_load_needed *ta_c2h_load_needed_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_LOAD_NEEDED ? (const ta_c2h_load_needed *)(const void *)view->bytes : NULL;
}

/* ---- c2h.loaded ---- */

/**
 * The text and bytes of a `c2h.loaded` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str address;
} ta_c2h_loaded_texts;

/**
 * Answers in `*out` the bytes a `c2h.loaded` record with `texts` takes: its
 * fixed part, 48 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_c2h_loaded_size(const ta_c2h_loaded_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.loaded` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x020C, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_c2h_loaded_write(ta_record_buffer out, const ta_c2h_loaded *fixed, const ta_c2h_loaded_texts *texts);

/**
 * The `c2h.loaded` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_c2h_loaded *ta_c2h_loaded_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_LOADED ? (const ta_c2h_loaded *)(const void *)view->bytes : NULL;
}

/* ---- c2h.unloaded ---- */

/**
 * The text and bytes of a `c2h.unloaded` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str address;
} ta_c2h_unloaded_texts;

/**
 * Answers in `*out` the bytes a `c2h.unloaded` record with `texts` takes: its
 * fixed part, 72 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_c2h_unloaded_size(const ta_c2h_unloaded_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.unloaded` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x020D, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_c2h_unloaded_write(ta_record_buffer out, const ta_c2h_unloaded *fixed, const ta_c2h_unloaded_texts *texts);

/**
 * The `c2h.unloaded` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_c2h_unloaded *ta_c2h_unloaded_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_UNLOADED ? (const ta_c2h_unloaded *)(const void *)view->bytes : NULL;
}

/* ---- c2h.state ---- */

/**
 * The text and bytes of a `c2h.state` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str address;
} ta_c2h_state_texts;

/**
 * Answers in `*out` the bytes a `c2h.state` record with `texts` takes: its
 * fixed part, 72 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_c2h_state_size(const ta_c2h_state_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.state` record into `out` (see the file comment): the fixed part
 * from `fixed`, kind 0x020E, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_c2h_state_write(ta_record_buffer out, const ta_c2h_state *fixed, const ta_c2h_state_texts *texts);

/**
 * The `c2h.state` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_c2h_state *ta_c2h_state_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_STATE ? (const ta_c2h_state *)(const void *)view->bytes : NULL;
}

/* ---- c2h.published ---- */

/**
 * The text and bytes of a `c2h.published` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str address;
} ta_c2h_published_texts;

/**
 * Answers in `*out` the bytes a `c2h.published` record with `texts` takes: its
 * fixed part, 72 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_c2h_published_size(const ta_c2h_published_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.published` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x020F, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_c2h_published_write(ta_record_buffer out, const ta_c2h_published *fixed, const ta_c2h_published_texts *texts);

/**
 * The `c2h.published` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_c2h_published *ta_c2h_published_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_PUBLISHED ? (const ta_c2h_published *)(const void *)view->bytes : NULL;
}

/* ---- c2h.finished ---- */

/**
 * Writes a `c2h.finished` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x0210, size `out.size`, the seq already in `out`.
 * Borrows `fixed` for the call. Never allocates; may run inside a macrostep.
 * Answers `TA_ERR_INVALID` for a bad buffer or argument and `TA_ERR_FULL` when
 * `out.size` is too small, and then writes nothing.
 */
ta_result ta_c2h_finished_write(ta_record_buffer out, const ta_c2h_finished *fixed);

/**
 * The `c2h.finished` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_c2h_finished *ta_c2h_finished_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_FINISHED ? (const ta_c2h_finished *)(const void *)view->bytes : NULL;
}

/* ---- c2h.destroyed ---- */

/**
 * The text and bytes of a `c2h.destroyed` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    ta_str address;
} ta_c2h_destroyed_texts;

/**
 * Answers in `*out` the bytes a `c2h.destroyed` record with `texts` takes: its
 * fixed part, 56 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_c2h_destroyed_size(const ta_c2h_destroyed_texts *texts, uint32_t *out);

/**
 * Writes a `c2h.destroyed` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x0211, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_c2h_destroyed_write(ta_record_buffer out, const ta_c2h_destroyed *fixed, const ta_c2h_destroyed_texts *texts);

/**
 * The `c2h.destroyed` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_c2h_destroyed *ta_c2h_destroyed_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_DESTROYED ? (const ta_c2h_destroyed *)(const void *)view->bytes : NULL;
}

/* ---- c2h.definition_result ---- */

/**
 * Writes a `c2h.definition_result` record into `out` (see the file comment):
 * the fixed part from `fixed`, kind 0x0212, size `out.size`, the seq already in
 * `out`. Borrows `fixed` for the call. Never allocates; may run inside a
 * macrostep. Answers `TA_ERR_INVALID` for a bad buffer or argument and
 * `TA_ERR_FULL` when `out.size` is too small, and then writes nothing.
 */
ta_result ta_c2h_definition_result_write(ta_record_buffer out, const ta_c2h_definition_result *fixed);

/**
 * The `c2h.definition_result` record `view` holds, or NULL when it holds
 * another kind. `view` comes from `ta_record_read`; the record is borrowed from
 * its bytes. Never allocates. O(1).
 */
static inline const ta_c2h_definition_result *ta_c2h_definition_result_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_DEFINITION_RESULT ? (const ta_c2h_definition_result *)(const void *)view->bytes : NULL;
}

/* ---- c2h.input_refused ---- */

/**
 * Writes a `c2h.input_refused` record into `out` (see the file comment): the
 * fixed part from `fixed`, kind 0x0213, size `out.size`, the seq already in
 * `out`. Borrows `fixed` for the call. Never allocates; may run inside a
 * macrostep. Answers `TA_ERR_INVALID` for a bad buffer or argument and
 * `TA_ERR_FULL` when `out.size` is too small, and then writes nothing.
 */
ta_result ta_c2h_input_refused_write(ta_record_buffer out, const ta_c2h_input_refused *fixed);

/**
 * The `c2h.input_refused` record `view` holds, or NULL when it holds another
 * kind. `view` comes from `ta_record_read`; the record is borrowed from its
 * bytes. Never allocates. O(1).
 */
static inline const ta_c2h_input_refused *ta_c2h_input_refused_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_INPUT_REFUSED ? (const ta_c2h_input_refused *)(const void *)view->bytes : NULL;
}

/* ---- c2h.input_completed ---- */

/**
 * Writes a `c2h.input_completed` record into `out` (see the file comment): the
 * fixed part from `fixed`, kind 0x0214, size `out.size`, the seq already in
 * `out`. Borrows `fixed` for the call. Never allocates; may run inside a
 * macrostep. Answers `TA_ERR_INVALID` for a bad buffer or argument and
 * `TA_ERR_FULL` when `out.size` is too small, and then writes nothing.
 */
ta_result ta_c2h_input_completed_write(ta_record_buffer out, const ta_c2h_input_completed *fixed);

/**
 * The `c2h.input_completed` record `view` holds, or NULL when it holds another
 * kind. `view` comes from `ta_record_read`; the record is borrowed from its
 * bytes. Never allocates. O(1).
 */
static inline const ta_c2h_input_completed *ta_c2h_input_completed_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_INPUT_COMPLETED ? (const ta_c2h_input_completed *)(const void *)view->bytes : NULL;
}

/* ---- c2h.load_result ---- */

/**
 * Writes a `c2h.load_result` record into `out` (see the file comment): the
 * fixed part from `fixed`, kind 0x0215, size `out.size`, the seq already in
 * `out`. Borrows `fixed` for the call. Never allocates; may run inside a
 * macrostep. Answers `TA_ERR_INVALID` for a bad buffer or argument and
 * `TA_ERR_FULL` when `out.size` is too small, and then writes nothing.
 */
ta_result ta_c2h_load_result_write(ta_record_buffer out, const ta_c2h_load_result *fixed);

/**
 * The `c2h.load_result` record `view` holds, or NULL when it holds another
 * kind. `view` comes from `ta_record_read`; the record is borrowed from its
 * bytes. Never allocates. O(1).
 */
static inline const ta_c2h_load_result *ta_c2h_load_result_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_C2H_LOAD_RESULT ? (const ta_c2h_load_result *)(const void *)view->bytes : NULL;
}

/* ---- tel.trace ---- */

/**
 * The text and bytes of a `tel.trace` record, in the order they lie in its
 * tail; a writer borrows them for one call.
 */
typedef struct {
    /** the session’s family/name */
    ta_str address;
    /**
     * the definition’s document name and source, on the kinds that open a run
     */
    ta_str definition;
    /** the name of `state` */
    ta_str state_name;
    /** the name of `transition` */
    ta_str transition_name;
    /** the name of `action` */
    ta_str action_name;
    /** the event name, or the name the kind carries */
    ta_str name;
    /** the send ID, for SENT and its kin */
    ta_str send_id;
    /** the invoke ID, for INVOKED and its kin */
    ta_str invoke_id;
    ta_str value_text;
    /** the fault’s error event name */
    ta_str fault_detail;
    ta_str fault_message;
    /**
     * the microstep’s transitions: a u32 count, then per transition its u32 ID,
     * then its name as a u32 length and UTF-8 bytes
     */
    ta_bytes transitions;
    /**
     * exited, entered, history, states: per list a u32 count, then per state
     * its u32 ID, then its name as a u32 length and UTF-8 bytes
     */
    ta_bytes states;
} ta_tel_trace_texts;

/**
 * Answers in `*out` the bytes a `tel.trace` record with `texts` takes: its
 * fixed part, 224 bytes, plus their lengths, rounded up to 8. Never allocates;
 * may run inside a macrostep. Answers `TA_ERR_INVALID`, and does not write
 * `*out`, when `texts` is NULL, a text has a length but no bytes, or the record
 * would exceed the largest size a `u32` holds.
 */
ta_result ta_tel_trace_size(const ta_tel_trace_texts *texts, uint32_t *out);

/**
 * Writes a `tel.trace` record into `out` (see the file comment): the fixed part
 * from `fixed`, kind 0x0301, size `out.size`, the seq already in `out` and
 * `texts` in the tail. Borrows `fixed` and `texts` for the call. Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_INVALID` for a bad
 * buffer or argument and `TA_ERR_FULL` when `out.size` is too small, and then
 * writes nothing.
 */
ta_result ta_tel_trace_write(ta_record_buffer out, const ta_tel_trace *fixed, const ta_tel_trace_texts *texts);

/**
 * The `tel.trace` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_tel_trace *ta_tel_trace_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_TEL_TRACE ? (const ta_tel_trace *)(const void *)view->bytes : NULL;
}

/* ---- tel.dropped ---- */

/**
 * Writes a `tel.dropped` record into `out` (see the file comment): the fixed
 * part from `fixed`, kind 0x0302, size `out.size`, the seq already in `out`.
 * Borrows `fixed` for the call. Never allocates; may run inside a macrostep.
 * Answers `TA_ERR_INVALID` for a bad buffer or argument and `TA_ERR_FULL` when
 * `out.size` is too small, and then writes nothing.
 */
ta_result ta_tel_dropped_write(ta_record_buffer out, const ta_tel_dropped *fixed);

/**
 * The `tel.dropped` record `view` holds, or NULL when it holds another kind.
 * `view` comes from `ta_record_read`; the record is borrowed from its bytes.
 * Never allocates. O(1).
 */
static inline const ta_tel_dropped *ta_tel_dropped_from_view(const ta_record_view *view) {
    return view->kind == TA_RECORD_KIND_TEL_DROPPED ? (const ta_tel_dropped *)(const void *)view->bytes : NULL;
}

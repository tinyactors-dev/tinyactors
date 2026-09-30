#include "tinyactors_host/wal.h"

#include "memory.h"

/* Header offsets. */
#define TAH_WAL_KIND_AT 2u
#define TAH_WAL_FIXED_AT 4u
#define TAH_WAL_SEQ_AT 8u
#define TAH_WAL_TIME_AT 16u
#define TAH_WAL_TEXT_AT 24u
#define TAH_WAL_VALUES_AT 28u
#define TAH_WAL_ABSENT UINT32_MAX

static uint64_t tah_wal_le(const uint8_t *bytes, uint32_t count) {
    uint64_t value = 0;
    for (uint32_t index = count; index > 0; index -= 1u) {
        value = value << 8 | bytes[index - 1u];
    }
    return value;
}

static void tah_wal_put_le(uint8_t *bytes, uint64_t value, uint32_t count) {
    for (uint32_t index = 0; index < count; index += 1u) {
        bytes[index] = (uint8_t)(value >> (8u * index));
    }
}

static size_t tah_wal_pad8(size_t length) {
    return (length + 7u) & ~(size_t)7u;
}

/* ------------------------------------------------------------------------
 * Writing
 */

/* Appends `length` bytes, or zeros when `bytes` is null. */
static ta_result tah_wal_append(tah_vec *out, const void *bytes, size_t length) {
    if (length > (size_t)(UINT32_MAX - out->count)) {
        return TA_ERR_NOMEM;
    }
    ta_result result = tah_vec_reserve(out, out->count + (uint32_t)length);
    if (result != TA_OK) {
        return result;
    }
    uint8_t *at = (uint8_t *)out->items + out->count;
    if (bytes != NULL) {
        tah_mem_copy(at, bytes, length);
    } else {
        tah_mem_set(at, 0, length);
    }
    out->count += (uint32_t)length;
    return TA_OK;
}

/* A value's pieces, each framed as a chunk as it arrives (ADR 0090 section 3). */
typedef struct {
    tah_vec *out;
    bool failed;
} tah_wal_frames;

static ta_op_status tah_wal_frame_begin(void *self) {
    (void)self;
    return TA_OP_OK;
}

static ta_op_status tah_wal_frame_write(void *self, ta_bytes bytes) {
    tah_wal_frames *frames = self;
    if (bytes.len == 0) {
        return TA_OP_OK; /* an empty chunk would end the stream */
    }
    uint8_t length[4];
    tah_wal_put_le(length, bytes.len, 4);
    if (tah_wal_append(frames->out, length, 4) != TA_OK || tah_wal_append(frames->out, bytes.ptr, bytes.len) != TA_OK ||
        tah_wal_append(frames->out, NULL, tah_wal_pad8(4u + (size_t)bytes.len) - 4u - bytes.len) != TA_OK) {
        frames->failed = true;
        return TA_OP_NOMEM;
    }
    return TA_OP_OK;
}

static void tah_wal_frame_end(void *self, bool keep) {
    (void)self;
    (void)keep;
}

static const ta_byte_sink_vtable tah_wal_frame_vtable = {
    .begin = tah_wal_frame_begin,
    .write = tah_wal_frame_write,
    .end = tah_wal_frame_end,
};

static bool tah_wal_value_present(const ta_value *value) {
    return value != NULL && value->v != NULL;
}

/* The record's text fields in order, `index` counting fields then `texts`. */
static bool tah_wal_text_of(const tah_wal_record *record, uint32_t index, ta_str *out) {
    if (index < record->field_count) {
        const tah_wal_field *field = &record->fields[index];
        if (!field->is_text || !field->present) {
            return false;
        }
        *out = field->text;
        return true;
    }
    index -= record->field_count;
    if (index < record->text_count) {
        *out = record->texts[index];
        return true;
    }
    return false;
}

ta_result tah_wal_write(const tah_wal_record *record, const tah_wal_write_params *params) {
    tah_vec *out = params->out;
    uint32_t start = out->count;
    uint32_t fields = record->field_count + record->text_count + record->number_count;
    size_t fixed = (size_t)TAH_WAL_HEADER + 8u * (size_t)fields;
    size_t text_bytes = 0;
    for (uint32_t index = 0; index < record->field_count + record->text_count; index += 1u) {
        ta_str text = { 0 };
        if (tah_wal_text_of(record, index, &text)) {
            text_bytes += text.len;
        }
    }
    uint16_t value_count = 0;
    for (uint32_t index = 0; index < 2u; index += 1u) {
        value_count = (uint16_t)(value_count + (tah_wal_value_present(record->values[index]) ? 1u : 0u));
    }
    if (fixed > UINT32_MAX || text_bytes > UINT32_MAX) {
        return TA_ERR_NOMEM;
    }
    ta_result result = tah_wal_append(out, NULL, fixed + tah_wal_pad8(text_bytes));
    if (result != TA_OK) {
        return result;
    }
    uint8_t *base = (uint8_t *)out->items + start;
    tah_wal_put_le(base, TAH_WAL_VERSION, 2);
    tah_wal_put_le(base + TAH_WAL_KIND_AT, (uint64_t)record->kind, 2);
    tah_wal_put_le(base + TAH_WAL_FIXED_AT, fixed, 4);
    tah_wal_put_le(base + TAH_WAL_SEQ_AT, record->seq, 8);
    tah_wal_put_le(base + TAH_WAL_TIME_AT, record->time, 8);
    tah_wal_put_le(base + TAH_WAL_TEXT_AT, text_bytes, 4);
    tah_wal_put_le(base + TAH_WAL_VALUES_AT, value_count, 2);
    uint32_t offset = 0;
    uint8_t *text_part = base + fixed;
    for (uint32_t index = 0; index < fields; index += 1u) {
        uint8_t *field = base + TAH_WAL_HEADER + 8u * index;
        bool is_text = index < record->field_count ? record->fields[index].is_text
                                                   : index < record->field_count + record->text_count;
        if (!is_text) {
            uint64_t number = index < record->field_count
                                  ? record->fields[index].number
                                  : record->numbers[index - record->field_count - record->text_count];
            tah_wal_put_le(field, number, 8);
            continue;
        }
        ta_str text = { 0 };
        if (!tah_wal_text_of(record, index, &text)) {
            tah_wal_put_le(field, TAH_WAL_ABSENT, 4);
            tah_wal_put_le(field + 4, 0, 4);
            continue;
        }
        tah_wal_put_le(field, offset, 4);
        tah_wal_put_le(field + 4, text.len, 4);
        tah_mem_copy(text_part + offset, text.ptr, text.len);
        offset += text.len;
    }
    for (uint32_t index = 0; index < 2u; index += 1u) {
        const ta_value *value = record->values[index];
        if (!tah_wal_value_present(value)) {
            continue;
        }
        if (params->datamodel.v == NULL || params->datamodel.v->value_write == NULL) {
            out->count = start;
            return TA_ERR_UNSUPPORTED;
        }
        tah_wal_frames frames = { .out = out };
        ta_fault fault = { 0 };
        ta_op_status status = params->datamodel.v->value_write(
            params->datamodel.self, &(ta_datamodel_value_write_params){
                                        .value = value,
                                        .sink = { .self = &frames, .v = &tah_wal_frame_vtable },
                                        .durable = true,
                                        .fault = &fault,
                                    });
        uint8_t end[8] = { 0 };
        if (status == TA_OP_OK && !frames.failed && tah_wal_append(out, end, sizeof end) != TA_OK) {
            status = TA_OP_NOMEM;
        }
        if (status != TA_OP_OK || frames.failed) {
            out->count = start;
            if (params->fault != NULL) {
                *params->fault = fault;
            } else {
                ta_value_release(&fault.data);
            }
            return status == TA_OP_ERROR ? TA_ERR_UNSUPPORTED : TA_ERR_NOMEM;
        }
        ta_value_release(&fault.data);
    }
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Reading
 */

/* One record's span, or 0 when it is not all there. */
static size_t tah_wal_one_span(const uint8_t *bytes, size_t length) {
    if (length < TAH_WAL_HEADER) {
        return 0;
    }
    size_t at = (size_t)tah_wal_le(bytes + TAH_WAL_FIXED_AT, 4);
    uint64_t text = tah_wal_le(bytes + TAH_WAL_TEXT_AT, 4);
    uint32_t values = (uint32_t)tah_wal_le(bytes + TAH_WAL_VALUES_AT, 2);
    if (at < TAH_WAL_HEADER || (at & 7u) != 0u) {
        return length; /* not a record: the reader says so */
    }
    at += tah_wal_pad8((size_t)text);
    for (uint32_t value = 0; value < values; value += 1u) {
        for (;;) {
            if (at + 4u > length) {
                return 0;
            }
            uint64_t chunk = tah_wal_le(bytes + at, 4);
            at += chunk == 0 ? 8u : tah_wal_pad8(4u + (size_t)chunk);
            if (chunk == 0) {
                break;
            }
        }
    }
    return at <= length ? at : 0;
}

size_t tah_wal_span(const uint8_t *bytes, size_t length, bool unit) {
    size_t first = tah_wal_one_span(bytes, length);
    if (first == 0 || !unit || tah_wal_le(bytes + TAH_WAL_KIND_AT, 2) != TAH_WAL_CHECKPOINT) {
        return first;
    }
    /* Bytes that are no record (their whole length: the reader says so)
     * are not a unit. A checkpoint record that ends exactly where the
     * bytes do is one, still waiting for its parts. */
    size_t fixed = (size_t)tah_wal_le(bytes + TAH_WAL_FIXED_AT, 4);
    if (fixed < TAH_WAL_HEADER + 8u * TAH_WAL_CHECKPOINT_FIELDS || (fixed & 7u) != 0u) {
        return first;
    }
    uint64_t more = tah_wal_le(bytes + TAH_WAL_HEADER + 8u * 7u, 8) + tah_wal_le(bytes + TAH_WAL_HEADER + 8u * 8u, 8);
    size_t at = first;
    for (uint64_t record = 0; record < more; record += 1u) {
        size_t span = tah_wal_one_span(bytes + at, length - at);
        if (span == 0) {
            return 0;
        }
        at += span;
    }
    return at;
}

/* The fields a kind has at least, and which of them are texts (a bit each). */
static bool tah_wal_shape(tah_wal_kind kind, uint32_t *fields, uint32_t *texts) {
    switch (kind) {
    case TAH_WAL_SPAWNED: *fields = 3; *texts = 0x7u; return true;
    case TAH_WAL_ACCEPTED: *fields = 10; *texts = 0x7cu | 0x100u; return true;
    case TAH_WAL_DELIVERED:
    case TAH_WAL_DROPPED: *fields = 1; *texts = 0; return true;
    case TAH_WAL_JUMP: *fields = 3; *texts = 0x2u; return true;
    case TAH_WAL_FIRED:
    case TAH_WAL_WITHDRAWN:
    case TAH_WAL_FINISHED: *fields = 1; *texts = 0x1u; return true;
    case TAH_WAL_ABANDONED:
    case TAH_WAL_RELEASED:
    case TAH_WAL_DESTROYED:
    case TAH_WAL_EVICTED: *fields = 0; *texts = 0; return true;
    case TAH_WAL_CHECKPOINT: *fields = TAH_WAL_CHECKPOINT_FIELDS; *texts = 0x7u; return true;
    case TAH_WAL_CHECKPOINT_SEND: *fields = 8; *texts = 0xfu; return true;
    case TAH_WAL_CHECKPOINT_INVOKE: *fields = 3; *texts = 0x3u; return true;
    default: return false;
    }
}

static bool tah_wal_reference_ok(const tah_wal_view *view, uint32_t index) {
    const uint8_t *field = view->fields + 8u * index;
    uint64_t offset = tah_wal_le(field, 4);
    uint64_t length = tah_wal_le(field + 4, 4);
    return offset == TAH_WAL_ABSENT || offset + length <= view->text_bytes;
}

ta_result tah_wal_read(const uint8_t *bytes, size_t length, tah_wal_view *out) {
    size_t span = tah_wal_one_span(bytes, length);
    if (span == 0 || span > length || length < TAH_WAL_HEADER) {
        return TA_ERR_CORRUPT;
    }
    uint32_t fixed = (uint32_t)tah_wal_le(bytes + TAH_WAL_FIXED_AT, 4);
    *out = (tah_wal_view){
        .version = (uint16_t)tah_wal_le(bytes, 2),
        .kind = (tah_wal_kind)tah_wal_le(bytes + TAH_WAL_KIND_AT, 2),
        .seq = tah_wal_le(bytes + TAH_WAL_SEQ_AT, 8),
        .time = (ta_time)tah_wal_le(bytes + TAH_WAL_TIME_AT, 8),
        .field_count = (fixed - TAH_WAL_HEADER) / 8u,
        .fields = bytes + TAH_WAL_HEADER,
        .text_bytes = (uint32_t)tah_wal_le(bytes + TAH_WAL_TEXT_AT, 4),
        .text = bytes + fixed,
        .value_count = (uint16_t)tah_wal_le(bytes + TAH_WAL_VALUES_AT, 2),
        .span = span,
    };
    out->values = out->text + tah_wal_pad8(out->text_bytes);
    uint32_t known = 0;
    uint32_t texts = 0;
    if (out->version != TAH_WAL_VERSION || !tah_wal_shape(out->kind, &known, &texts) || out->field_count < known ||
        (size_t)fixed + tah_wal_pad8(out->text_bytes) > span) {
        return TA_ERR_CORRUPT;
    }
    for (uint32_t index = 0; index < known; index += 1u) {
        if ((texts & (1u << index)) != 0u && !tah_wal_reference_ok(out, index)) {
            return TA_ERR_CORRUPT;
        }
    }
    /* A JUMP's targets, texts past its known fields. */
    if (out->kind == TAH_WAL_JUMP) {
        uint64_t count = tah_wal_number(out, 2);
        if (count > out->field_count - 3u) {
            return TA_ERR_CORRUPT;
        }
        for (uint32_t index = 0; index < (uint32_t)count; index += 1u) {
            if (!tah_wal_reference_ok(out, 3u + index)) {
                return TA_ERR_CORRUPT;
            }
        }
    }
    if (out->kind == TAH_WAL_CHECKPOINT) {
        uint64_t lists = tah_wal_number(out, 9) + tah_wal_number(out, 10) + 2u * tah_wal_number(out, 11);
        if (lists > out->field_count - TAH_WAL_CHECKPOINT_FIELDS) {
            return TA_ERR_CORRUPT;
        }
    }
    return TA_OK;
}

uint64_t tah_wal_number(const tah_wal_view *view, uint32_t index) {
    return index < view->field_count ? tah_wal_le(view->fields + 8u * index, 8) : 0;
}

bool tah_wal_text(const tah_wal_view *view, uint32_t index, ta_str *out) {
    if (index >= view->field_count) {
        return false;
    }
    const uint8_t *field = view->fields + 8u * index;
    uint64_t offset = tah_wal_le(field, 4);
    uint64_t length = tah_wal_le(field + 4, 4);
    if (offset == TAH_WAL_ABSENT || offset + length > view->text_bytes) {
        return false;
    }
    *out = (ta_str){ .ptr = (const char *)view->text + offset, .len = (uint32_t)length };
    return true;
}

bool tah_wal_value_at(const tah_wal_view *view, uint32_t index, tah_wal_value_reader *reader) {
    if (index >= view->value_count) {
        return false;
    }
    const uint8_t *at = view->values;
    const uint8_t *end = view->fields - TAH_WAL_HEADER + view->span; /* the record's start, plus its span */
    for (uint32_t skip = 0; skip < index; skip += 1u) {
        for (;;) {
            uint64_t chunk = tah_wal_le(at, 4);
            at += chunk == 0 ? 8u : tah_wal_pad8(4u + (size_t)chunk);
            if (chunk == 0) {
                break;
            }
        }
    }
    *reader = (tah_wal_value_reader){ .at = at, .end = end };
    return true;
}

static ta_op_status tah_wal_value_read(void *self, const ta_byte_read_params *params) {
    tah_wal_value_reader *reader = self;
    uint32_t got = 0;
    while (got < params->capacity && !reader->done) {
        if (reader->left == 0) {
            if (reader->at + 4u > reader->end) {
                return TA_OP_ERROR;
            }
            uint32_t chunk = (uint32_t)tah_wal_le(reader->at, 4);
            if (chunk == 0) {
                reader->at += 8u;
                reader->done = true;
                break;
            }
            if (reader->at + 4u + chunk > reader->end) {
                return TA_OP_ERROR;
            }
            reader->left = chunk;
            reader->next = reader->at + tah_wal_pad8(4u + (size_t)chunk);
            reader->at += 4u;
        }
        uint32_t count = params->capacity - got < reader->left ? params->capacity - got : reader->left;
        tah_mem_copy(params->buffer + got, reader->at, count);
        got += count;
        reader->at += count;
        reader->left -= count;
        if (reader->left == 0) {
            reader->at = reader->next; /* past the chunk's padding */
        }
    }
    *params->got = got;
    return TA_OP_OK;
}

static const ta_byte_source_vtable tah_wal_value_vtable = { .read = tah_wal_value_read };

ta_byte_source tah_wal_value_source(tah_wal_value_reader *reader) {
    return (ta_byte_source){ .self = reader, .v = &tah_wal_value_vtable };
}

ta_result tah_wal_read_value(const tah_wal_view *view, uint32_t index, ta_datamodel datamodel, ta_value *out) {
    *out = (ta_value){ 0 };
    tah_wal_value_reader reader;
    if (!tah_wal_value_at(view, index, &reader)) {
        return TA_ERR_NOT_FOUND;
    }
    if (datamodel.v == NULL || datamodel.v->value_read == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_fault fault = { 0 };
    ta_op_status status = datamodel.v->value_read(datamodel.self, &(ta_datamodel_value_read_params){
                                                                      .source = tah_wal_value_source(&reader),
                                                                      .durable = true,
                                                                      .out = out,
                                                                      .fault = &fault,
                                                                  });
    ta_value_release(&fault.data);
    return status == TA_OP_OK ? TA_OK : status == TA_OP_NOMEM ? TA_ERR_NOMEM : TA_ERR_CORRUPT;
}

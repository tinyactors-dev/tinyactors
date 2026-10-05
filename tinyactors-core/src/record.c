#include "record.h"

/*
 * The implementation of package record (see record.h for the contract).
 * Nothing here allocates. Every load from input bytes goes through
 * ta_mem_copy into a local, never through a cast of the pointer: the bytes
 * may be unaligned, and a cast would also assume the host does not change
 * them under the load (the same rule as ring.c's get_u32).
 */

static uint16_t load_u16(const unsigned char *at) {
    uint16_t value;
    ta_mem_copy(&value, at, sizeof value);
    return value;
}

static uint32_t load_u32(const unsigned char *at) {
    uint32_t value;
    ta_mem_copy(&value, at, sizeof value);
    return value;
}

static uint64_t load_u64(const unsigned char *at) {
    uint64_t value;
    ta_mem_copy(&value, at, sizeof value);
    return value;
}

/*
 * descriptor_valid says whether one descriptor can be trusted by
 * ta_record_check: everything the checker will index with is inside the fixed
 * part, after the header. It is the reason init is O(table).
 */
static bool descriptor_valid(const ta_record_descriptor *descriptor) {
    if (descriptor->kind == 0 || descriptor->ring >= TA_RING_COUNT) {
        return false;
    }
    if (descriptor->fixed_size < TA_RECORD_HEADER_BYTES || descriptor->fixed_size > TA_RECORD_FIXED_MAX ||
        descriptor->fixed_size % TA_RING_ALIGN != 0) {
        return false;
    }
    if ((descriptor->span_count > 0 && descriptor->spans == NULL) || (descriptor->domain_count > 0 && descriptor->domains == NULL)) {
        return false;
    }
    for (uint32_t i = 0; i < descriptor->span_count; i += 1) {
        /* Subtraction, not addition: fixed_size is at most 128, so this cannot wrap either way, but the rule is the same everywhere. */
        if (descriptor->spans[i].at < TA_RECORD_HEADER_BYTES || descriptor->spans[i].at > descriptor->fixed_size - 8u) {
            return false;
        }
    }
    for (uint32_t i = 0; i < descriptor->domain_count; i += 1) {
        if (descriptor->domains[i].at < TA_RECORD_HEADER_BYTES || descriptor->domains[i].at >= descriptor->fixed_size) {
            return false;
        }
    }
    return true;
}

/* ta_record_reader_init: see record.h. Validates everything first and writes *reader last, so a refusal leaves it untouched. */
ta_result ta_record_reader_init(ta_record_reader *reader, const ta_record_reader_params *params) {
    if (params->table == NULL || (params->table->count > 0 && params->table->descriptor == NULL)) {
        return TA_ERR_INVALID;
    }
    if ((unsigned)params->ring >= (unsigned)TA_RING_COUNT || (unsigned)params->mode > (unsigned)TA_RING_LOSSY) {
        return TA_ERR_INVALID;
    }
    if (params->max_record_bytes < TA_RECORD_HEADER_BYTES || params->max_record_bytes % TA_RING_ALIGN != 0) {
        return TA_ERR_INVALID;
    }
    for (uint32_t i = 0; i < params->table->count; i += 1) {
        const ta_record_descriptor *descriptor = &params->table->descriptor[i];
        if (!descriptor_valid(descriptor) || (i > 0 && params->table->descriptor[i - 1].kind >= descriptor->kind)) {
            return TA_ERR_INVALID;
        }
    }
    *reader = (ta_record_reader){ .table = params->table,
                                  .ring = params->ring,
                                  .mode = params->mode,
                                  .max_record_bytes = params->max_record_bytes,
                                  .next_seq = 1 };
    return TA_OK;
}

/* find_descriptor binary-searches the table, sorted by kind, for kind; NULL when there is none. */
static const ta_record_descriptor *find_descriptor(const ta_record_table *table, uint16_t kind) {
    uint32_t low = 0;
    uint32_t high = table->count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (table->descriptor[middle].kind == kind) {
            return &table->descriptor[middle];
        }
        if (table->descriptor[middle].kind < kind) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return NULL;
}

/* span_ok is the whole span rule of the package comment, on values already copied out of the record. */
static bool span_ok(uint32_t offset, uint32_t length, uint32_t fixed_size, uint32_t size) {
    if (length == 0) {
        return offset == 0;
    }
    /* offset <= size first, so that size - offset cannot wrap; the length test then needs no addition. */
    return offset >= fixed_size && offset <= size && length <= size - offset;
}

bool ta_record_in_domain(uint64_t allowed, uint32_t value) {
    return value < TA_RECORD_DOMAIN_BITS && ((allowed >> value) & 1u) != 0;
}

/* seq_check applies the reader's sequence rule to seq and reports the gap; false is TA_RECORD_SEQ. */
static bool seq_check(const ta_record_reader *reader, uint64_t seq, uint64_t *gap) {
    if (seq == 0 || seq == UINT64_MAX) {
        return false;
    }
    if (reader->mode == TA_RING_LOSSLESS) {
        *gap = 0;
        return seq == reader->next_seq;
    }
    if (seq < reader->next_seq) {
        return false;
    }
    *gap = seq - reader->next_seq;
    return true;
}

/* ta_record_check: see record.h for the contract and its order. Every decision after the copy is made on out->fixed. */
ta_record_fault ta_record_check(ta_record_reader *reader, const ta_record_input *input, ta_record *out) {
    const unsigned char *bytes = input->bytes;
    if (bytes == NULL || input->length < TA_RECORD_HEADER_BYTES) {
        return TA_RECORD_TRUNCATED;
    }
    /* The size field is a u32, so a longer input cannot be one record; this is also the only place a size_t is narrowed. */
    if ((uint64_t)input->length > UINT32_MAX) {
        return TA_RECORD_SIZE;
    }
    uint32_t length = (uint32_t)input->length;
    uint32_t size = load_u32(bytes + TA_RECORD_AT_SIZE);
    if (size != length || size % TA_RING_ALIGN != 0 || size > reader->max_record_bytes) {
        return TA_RECORD_SIZE;
    }
    uint16_t kind = load_u16(bytes + TA_RECORD_AT_KIND);
    const ta_record_descriptor *descriptor = find_descriptor(reader->table, kind);
    if (descriptor == NULL) {
        return TA_RECORD_KIND;
    }
    if (descriptor->ring != (uint8_t)reader->ring) {
        return TA_RECORD_WRONG_RING;
    }
    if (length < descriptor->fixed_size) {
        return TA_RECORD_TRUNCATED;
    }

    /* The one read of the fixed part. From here on, only the snapshot is looked at. */
    ta_mem_copy(out->fixed, bytes, descriptor->fixed_size);
    /* The host may have rewritten the header between the loads above and the copy; the snapshot must say what was judged. */
    if (load_u32(out->fixed + TA_RECORD_AT_SIZE) != size) {
        return TA_RECORD_SIZE;
    }
    if (load_u16(out->fixed + TA_RECORD_AT_KIND) != kind) {
        return TA_RECORD_KIND;
    }

    uint64_t seq = load_u64(out->fixed + TA_RECORD_AT_SEQ);
    uint64_t gap = 0;
    if (!seq_check(reader, seq, &gap)) {
        return TA_RECORD_SEQ;
    }
    if ((load_u16(out->fixed + TA_RECORD_AT_FLAGS) & (uint16_t)~descriptor->flags_allowed) != 0) {
        return TA_RECORD_VALUE;
    }
    for (uint32_t i = 0; i < descriptor->span_count; i += 1) {
        uint32_t at = descriptor->spans[i].at;
        uint32_t span_offset = load_u32(out->fixed + at);
        uint32_t span_length = load_u32(out->fixed + at + 4u);
        if (!span_ok(span_offset, span_length, descriptor->fixed_size, size)) {
            return TA_RECORD_SPAN;
        }
    }
    for (uint32_t i = 0; i < descriptor->domain_count; i += 1) {
        if (!ta_record_in_domain(descriptor->domains[i].allowed, out->fixed[descriptor->domains[i].at])) {
            return TA_RECORD_VALUE;
        }
    }

    out->bytes = bytes;
    out->size = size;
    out->kind = kind;
    out->seq = seq;
    out->gap = gap;
    out->descriptor = descriptor;
    reader->next_seq = seq + 1u; /* seq < UINT64_MAX was checked */
    return TA_RECORD_OK;
}

const char *ta_record_fault_name(ta_record_fault fault) {
    switch (fault) {
    case TA_RECORD_OK:
        return "ok";
    case TA_RECORD_TRUNCATED:
        return "truncated";
    case TA_RECORD_SIZE:
        return "size";
    case TA_RECORD_KIND:
        return "kind";
    case TA_RECORD_WRONG_RING:
        return "wrong ring";
    case TA_RECORD_SEQ:
        return "seq";
    case TA_RECORD_SPAN:
        return "span";
    case TA_RECORD_VALUE:
        return "value";
    }
    return "unknown";
}

/* fixed_scalar copies `size` bytes of the snapshot at `at` into dst, or zeroes dst when they do not lie inside the fixed part. */
static void fixed_scalar(const ta_record *record, uint32_t at, void *dst, uint32_t size) {
    uint32_t fixed = record->descriptor == NULL ? 0u : record->descriptor->fixed_size;
    if (at > fixed || size > fixed - at) {
        ta_mem_zero(dst, size);
        return;
    }
    ta_mem_copy(dst, record->fixed + at, size);
}

uint8_t ta_record_u8(const ta_record *record, uint32_t at) {
    uint8_t value;
    fixed_scalar(record, at, &value, sizeof value);
    return value;
}

uint16_t ta_record_u16(const ta_record *record, uint32_t at) {
    uint16_t value;
    fixed_scalar(record, at, &value, sizeof value);
    return value;
}

uint32_t ta_record_u32(const ta_record *record, uint32_t at) {
    uint32_t value;
    fixed_scalar(record, at, &value, sizeof value);
    return value;
}

uint64_t ta_record_u64(const ta_record *record, uint32_t at) {
    uint64_t value;
    fixed_scalar(record, at, &value, sizeof value);
    return value;
}

ta_bytes ta_record_span(const ta_record *record, uint32_t at) {
    ta_bytes none = { .ptr = NULL, .len = 0 };
    uint32_t fixed = record->descriptor == NULL ? 0u : record->descriptor->fixed_size;
    if (fixed < 8u || at > fixed - 8u) {
        return none;
    }
    uint32_t offset = load_u32(record->fixed + at);
    uint32_t length = load_u32(record->fixed + at + 4u);
    if (length == 0 || offset > record->size || length > record->size - offset) {
        return none;
    }
    return (ta_bytes){ .ptr = record->bytes + offset, .len = length };
}

bool ta_record_bytes_ok(ta_bytes bytes) {
    return bytes.len == 0 || bytes.ptr != NULL;
}

uint32_t ta_record_put_span(ta_span32 *span, unsigned char *record, uint32_t tail, ta_bytes bytes) {
    if (bytes.len == 0) {
        *span = (ta_span32){ .offset = 0, .length = 0 };
        return tail;
    }
    ta_mem_copy(record + tail, bytes.ptr, bytes.len);
    *span = (ta_span32){ .offset = tail, .length = bytes.len };
    return tail + bytes.len;
}

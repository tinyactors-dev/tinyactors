#include "name_area.h"
#include "../include/tinyactors.h"
#include "chunks.h"
#include "index_pool.h"
#include "memory.h"
#include "quota.h"

ta_result ta_name_area_init(ta_name_area *area, const ta_name_area_params *params) {
    if (params->bytes < TA_NAME_AREA_MIN_BYTES || params->bytes > TA_NAME_AREA_MAX_BYTES ||
        params->watermark_percent < 1u || params->watermark_percent > 99u) {
        return TA_ERR_INVALID;
    }
    uint32_t count = params->bytes / TA_NAME_AREA_CHUNK_BYTES;
    if (params->reserve < 1u || params->reserve > count) {
        return TA_ERR_INVALID;
    }
    ta_chunks chunks;
    ta_result result = ta_chunks_init(&chunks, &(ta_chunks_params){ .allocator = params->allocator,
                                                                    .chunk_size = TA_NAME_AREA_CHUNK_BYTES,
                                                                    .page_shift = TA_NAME_AREA_PAGE_SHIFT,
                                                                    .ceiling = count });
    if (result != TA_OK) {
        return result;
    }
    uint32_t watermark = (uint32_t)((uint64_t)count * params->watermark_percent / 100u);
    *area = (ta_name_area){
        .chunks = chunks,
        .quota = ta_quota_make(count),
        .watermark = watermark == 0u ? 1u : watermark,
        .reserve = params->reserve,
    };
    return TA_OK;
}

void ta_name_area_destroy(ta_name_area *area) {
    ta_chunks_destroy(&area->chunks);
    area->quota = ta_quota_make(area->quota.capacity);
}

ta_result ta_name_area_refill(ta_name_area *area) {
    uint32_t available = ta_quota_available(&area->quota);
    uint32_t min_free = area->reserve < available ? area->reserve : available;
    /* The ceiling is the quota's capacity, so `used + min_free` never passes it: only NOMEM remains. */
    return ta_index_pool_refill(&area->chunks.pool, min_free);
}

void ta_name_area_trim(ta_name_area *area) {
    ta_index_pool_trim(&area->chunks.pool, area->reserve);
}

/* The quota request of `need` chunks, kept to the watermark for admission. */
static ta_quota_request ta_name_area_request(const ta_name_area *area, uint32_t need, bool admission) {
    return (ta_quota_request){ .amount = need, .keep = admission ? area->quota.capacity - area->watermark : 0u };
}

bool ta_name_area_admitting(const ta_name_area *area) {
    return ta_quota_fits(&area->quota, ta_name_area_request(area, 1u, true));
}

uint32_t ta_name_area_field_need(const ta_name_area *area, uint32_t chain_length, uint32_t text_length) {
    if (text_length > TA_NAME_AREA_FIELD_MAX) {
        return UINT32_MAX;
    }
    return ta_chunks_need(&area->chunks, chain_length, TA_NAME_AREA_PREFIX_BYTES + text_length);
}

/* Bytes of the field being appended: its prefix, and its text. */
typedef struct {
    uint8_t prefix[TA_NAME_AREA_PREFIX_BYTES];
    ta_bytes text;
} ta_name_area_field;

/* Appends `field` to the writer's chain, all or nothing; see ta_name_area_write. */
static ta_result ta_name_area_append(ta_name_area *area, const ta_name_area_write_params *params,
                                     const ta_name_area_field *field) {
    uint32_t need = ta_name_area_field_need(area, params->writer->chain.length, field->text.len);
    ta_quota_request request = ta_name_area_request(area, need, params->admission);
    if (need > area->chunks.pool.free || !ta_quota_fits(&area->quota, request)) {
        return TA_ERR_FULL;
    }
    /* Neither write can fail: the whole field's chunks are free, and the needs of its two parts add up. */
    (void)ta_chunks_write(&area->chunks, params->writer,
                          (ta_bytes){ .ptr = field->prefix, .len = TA_NAME_AREA_PREFIX_BYTES });
    (void)ta_chunks_write(&area->chunks, params->writer, field->text);
    (void)ta_quota_take(&area->quota, request);
    return TA_OK;
}

ta_result ta_name_area_write(ta_name_area *area, const ta_name_area_write_params *params) {
    if (params->text.len > TA_NAME_AREA_FIELD_MAX) {
        return TA_ERR_INVALID;
    }
    ta_name_area_field field = { .prefix = { (uint8_t)(params->text.len & 0xFFu), (uint8_t)(params->text.len >> 8) },
                                 .text = params->text };
    return ta_name_area_append(area, params, &field);
}

/* Writes `value` as `count` bytes, little-endian, at `out`; answers the byte after them. */
static uint8_t *ta_name_area_put(uint8_t *out, uint64_t value, uint32_t count) {
    for (uint32_t byte = 0; byte < count; byte += 1u) {
        out[byte] = (uint8_t)(value >> (8u * byte));
    }
    return out + count;
}

/* Reads `count` bytes, little-endian, at `*in`, and moves `*in` past them. */
static uint64_t ta_name_area_get(const uint8_t **in, uint32_t count) {
    uint64_t value = 0u;
    for (uint32_t byte = 0; byte < count; byte += 1u) {
        value |= (uint64_t)(*in)[byte] << (8u * byte);
    }
    *in += count;
    return value;
}

ta_result ta_name_area_write_trace_parent(ta_name_area *area, const ta_name_area_trace_params *params) {
    const ta_trace_parent *parent = params->parent;
    if (parent->context.trace_id_high == 0u && parent->context.trace_id_low == 0u) {
        return TA_ERR_INVALID;
    }
    uint8_t bytes[TA_NAME_AREA_TRACE_PARENT_BYTES];
    uint8_t *at = bytes;
    at = ta_name_area_put(at, parent->context.trace_id_high, 8u);
    at = ta_name_area_put(at, parent->context.trace_id_low, 8u);
    at = ta_name_area_put(at, parent->context.flags, 1u);
    at = ta_name_area_put(at, parent->context.detail, 1u);
    at = ta_name_area_put(at, parent->cause.session, 8u);
    at = ta_name_area_put(at, parent->cause.macrostep, 8u);
    (void)ta_name_area_put(at, parent->cause.microstep, 4u);
    ta_name_area_field field = { .prefix = { TA_NAME_AREA_TRACE_PARENT_BYTES, 0u },
                                 .text = { .ptr = bytes, .len = TA_NAME_AREA_TRACE_PARENT_BYTES } };
    ta_name_area_write_params write = { .writer = params->writer, .text = field.text, .admission = params->admission };
    return ta_name_area_append(area, &write, &field);
}

void ta_name_area_free(ta_name_area *area, ta_chunk_chain *chain) {
    uint32_t held = ta_chunks_need(&area->chunks, 0u, chain->length);
    ta_chunks_free(&area->chunks, chain);
    (void)ta_quota_release(&area->quota, held); /* cannot fail: the chain's chunks are in use */
}

/* Moves `reader` past `count` bytes; answers how many of them the chain did not have. */
static uint32_t ta_name_area_skip(const ta_name_area *area, ta_chunk_reader *reader, uint32_t count) {
    while (count != 0u) {
        ta_bytes span = ta_chunks_read(&area->chunks, reader, count);
        if (span.len == 0u) {
            break;
        }
        count -= span.len;
    }
    return count;
}

/* Where ta_name_area_copy puts bytes, and how many. */
typedef struct {
    uint8_t *ptr;
    uint32_t len;
} ta_name_area_buffer;

/* Copies the next `out.len` bytes into `out.ptr`; answers false when the chain ends first. */
static bool ta_name_area_copy(const ta_name_area *area, ta_chunk_reader *reader, ta_name_area_buffer out) {
    uint32_t copied = 0u;
    while (copied < out.len) {
        ta_bytes span = ta_chunks_read(&area->chunks, reader, out.len - copied);
        if (span.len == 0u) {
            return false;
        }
        ta_mem_copy(out.ptr + copied, span.ptr, span.len);
        copied += span.len;
    }
    return true;
}

ta_result ta_name_area_next(const ta_name_area *area, ta_name_area_reader *reader, uint32_t *length) {
    ta_chunk_reader at = reader->chunks;
    if (ta_name_area_skip(area, &at, reader->field) != 0u) {
        return TA_ERR_CORRUPT;
    }
    if (at.remaining == 0u) {
        return TA_ERR_NOT_FOUND;
    }
    uint8_t prefix[TA_NAME_AREA_PREFIX_BYTES];
    if (!ta_name_area_copy(area, &at, (ta_name_area_buffer){ .ptr = prefix, .len = TA_NAME_AREA_PREFIX_BYTES })) {
        return TA_ERR_CORRUPT;
    }
    uint32_t field = (uint32_t)prefix[0] | (uint32_t)prefix[1] << 8;
    if (field > at.remaining) {
        return TA_ERR_CORRUPT;
    }
    *reader = (ta_name_area_reader){ .chunks = at, .field = field };
    *length = field;
    return TA_OK;
}

ta_bytes ta_name_area_read(const ta_name_area *area, ta_name_area_reader *reader, uint32_t max) {
    ta_bytes span = ta_chunks_read(&area->chunks, &reader->chunks, max < reader->field ? max : reader->field);
    reader->field -= span.len;
    return span;
}

ta_result ta_name_area_trace_parent(const ta_name_area *area, ta_chunk_chain chain, ta_trace_parent *out) {
    if (chain.length < TA_NAME_AREA_TRACE_FIELD_BYTES) {
        return TA_ERR_NOT_FOUND;
    }
    ta_chunk_reader reader = ta_chunk_reader_of(chain);
    (void)ta_name_area_skip(area, &reader, chain.length - TA_NAME_AREA_TRACE_FIELD_BYTES);
    uint8_t bytes[TA_NAME_AREA_TRACE_FIELD_BYTES];
    (void)ta_name_area_copy(area, &reader, (ta_name_area_buffer){ .ptr = bytes, .len = TA_NAME_AREA_TRACE_FIELD_BYTES });
    const uint8_t *at = bytes;
    if (ta_name_area_get(&at, TA_NAME_AREA_PREFIX_BYTES) != TA_NAME_AREA_TRACE_PARENT_BYTES) {
        return TA_ERR_NOT_FOUND;
    }
    ta_trace_parent parent = { 0 };
    parent.context.trace_id_high = ta_name_area_get(&at, 8u);
    parent.context.trace_id_low = ta_name_area_get(&at, 8u);
    parent.context.flags = (uint8_t)ta_name_area_get(&at, 1u);
    parent.context.detail = (uint8_t)ta_name_area_get(&at, 1u);
    parent.cause.session = ta_name_area_get(&at, 8u);
    parent.cause.macrostep = ta_name_area_get(&at, 8u);
    parent.cause.microstep = (uint32_t)ta_name_area_get(&at, 4u);
    *out = parent;
    return TA_OK;
}

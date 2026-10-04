#include "internal.h"

ta_result ta_chain_pool_init(ta_pool *pool, ta_allocator allocator, const ta_chain_pool_params *params) {
    if (pool == NULL || params == NULL) {
        return TA_ERR_INVALID;
    }
    if (params->chunk_size < 8u || params->chunk_size % 4u != 0u) {
        ta_mem_zero(pool, sizeof(*pool));
        return TA_ERR_INVALID;
    }
    return ta_pool_init(pool, allocator,
                        &(ta_pool_params){ .item_size = params->chunk_size,
                                           .item_alignment = 4u,
                                           .link_offset = 0u,
                                           .page_items = params->page_chunks,
                                           .max_items = params->max_chunks,
                                           .fresh = NULL });
}

uint32_t ta_chain_payload(const ta_pool *pool) {
    return (uint32_t)pool->item_size - 4u;
}

uint32_t ta_chain_need(const ta_pool *pool, const ta_chain *chain, uint32_t bytes) {
    uint32_t payload = ta_chain_payload(pool);
    uint32_t slack = 0;
    if (chain != NULL && chain->length != 0) {
        /* Every chunk but the tail is full, so the tail's fill is length % payload (full when 0). */
        slack = (payload - chain->length % payload) % payload;
    }
    if (bytes <= slack) {
        return 0;
    }
    /* In 64 bits: bytes - slack + payload - 1 can pass UINT32_MAX. */
    return (uint32_t)(((uint64_t)(bytes - slack) + payload - 1u) / payload);
}

/* next_get and next_set access the chunk's link word. A chunk is 4-aligned; the copy keeps it type-safe. */
static uint32_t next_get(const unsigned char *chunk) {
    uint32_t next;
    ta_mem_copy(&next, chunk, sizeof(next));
    return next;
}

static void next_set(unsigned char *chunk, uint32_t next) {
    ta_mem_copy(chunk, &next, sizeof(next));
}

ta_result ta_chain_write(ta_pool *pool, ta_chain *chain, const void *data, uint32_t bytes) {
    if (pool == NULL || chain == NULL || (data == NULL && bytes != 0)) {
        return TA_ERR_INVALID;
    }
    if (bytes == 0) {
        return TA_OK;
    }
    /* Preflight: everything that can fail is decided before any state changes. */
    if (bytes > UINT32_MAX - chain->length) {
        return TA_ERR_NOMEM;
    }
    ta_pool_counts counts;
    ta_pool_counts_get(pool, &counts);
    if (counts.free < ta_chain_need(pool, chain, bytes)) {
        return TA_ERR_NOMEM;
    }

    uint32_t payload = ta_chain_payload(pool);
    const unsigned char *source = data;
    uint32_t left = bytes;
    uint32_t fill = chain->length % payload; /* 0 for an empty chain or a full tail */
    if (fill != 0) {
        unsigned char *tail = ta_pool_at(pool, chain->tail);
        uint32_t room = payload - fill;
        uint32_t count = left < room ? left : room;
        ta_mem_copy(tail + 4u + fill, source, count);
        source += count;
        left -= count;
        chain->length += count;
    }
    while (left != 0) {
        /* Cannot be 0: the preflight counted these chunks free. */
        uint32_t index = ta_pool_take(pool);
        unsigned char *chunk = ta_pool_at(pool, index);
        uint32_t count = left < payload ? left : payload;
        ta_mem_copy(chunk + 4u, source, count);
        if (chain->tail != 0) {
            next_set(ta_pool_at(pool, chain->tail), index);
        } else {
            chain->head = index;
        }
        chain->tail = index; /* its next is already 0: a taken item's link bytes are zero */
        source += count;
        left -= count;
        chain->length += count;
    }
    return TA_OK;
}

void ta_chain_reader_init(ta_chain_reader *reader, const ta_pool *pool, const ta_chain *chain) {
    reader->pool = pool;
    reader->chunk = chain->head;
    reader->offset = 0;
    reader->remaining = chain->length;
}

uint32_t ta_chain_reader_remaining(const ta_chain_reader *reader) {
    return reader->remaining;
}

/* reader_take yields the next span of at most `limit` bytes (limit > 0) and advances. */
static bool reader_take(ta_chain_reader *reader, uint32_t limit, ta_chain_span *out) {
    if (reader->remaining == 0) {
        return false;
    }
    uint32_t payload = ta_chain_payload(reader->pool);
    const unsigned char *chunk = ta_pool_at(reader->pool, reader->chunk);
    uint32_t count = payload - reader->offset; /* what is left of this chunk's payload */
    if (count > reader->remaining) {
        count = reader->remaining;
    }
    if (count > limit) {
        count = limit;
    }
    out->bytes = chunk + 4u + reader->offset;
    out->length = count;
    reader->offset += count;
    reader->remaining -= count;
    if (reader->remaining == 0) {
        reader->chunk = 0;
    } else if (reader->offset == payload) {
        reader->chunk = next_get(chunk);
        reader->offset = 0;
    }
    return true;
}

bool ta_chain_reader_span(ta_chain_reader *reader, ta_chain_span *out) {
    return reader_take(reader, UINT32_MAX, out);
}

bool ta_chain_reader_copy(ta_chain_reader *reader, void *out, uint32_t bytes) {
    if (bytes > reader->remaining) {
        return false;
    }
    unsigned char *destination = out;
    while (bytes != 0) {
        ta_chain_span span;
        if (!reader_take(reader, bytes, &span)) {
            return false; /* not reached: bytes <= remaining */
        }
        ta_mem_copy(destination, span.bytes, span.length);
        destination += span.length;
        bytes -= span.length;
    }
    return true;
}

uint32_t ta_chain_free(ta_pool *pool, ta_chain *chain) {
    uint32_t freed = 0;
    uint32_t index = chain->head;
    while (index != 0) {
        uint32_t next = next_get(ta_pool_at(pool, index)); /* before the give overwrites it */
        ta_pool_give(pool, index);
        freed += 1;
        index = next;
    }
    ta_mem_zero(chain, sizeof(*chain));
    return freed;
}

#include "chunks.h"
#include "../include/tinyactors.h"
#include "index_pool.h"
#include "memory.h"

/* The chunk at `index`, which the pool holds: its link, then its payload. */
static uint8_t *ta_chunks_at(const ta_chunks *chunks, uint32_t index) {
    return ta_index_pool_at(&chunks->pool, index);
}

static uint32_t ta_chunks_link(const ta_chunks *chunks, uint32_t index) {
    uint32_t link;
    ta_mem_copy(&link, ta_chunks_at(chunks, index), sizeof(link));
    return link;
}

static void ta_chunks_set_link(ta_chunks *chunks, uint32_t index, uint32_t link) {
    ta_mem_copy(ta_chunks_at(chunks, index), &link, sizeof(link));
}

static uint8_t *ta_chunks_payload(const ta_chunks *chunks, uint32_t index) {
    return ta_chunks_at(chunks, index) + TA_CHUNKS_LINK_BYTES;
}

/* Chunks a chain of `length` bytes holds. */
static uint64_t ta_chunks_count(const ta_chunks *chunks, uint64_t length) {
    return (length + chunks->payload - 1u) / chunks->payload;
}

ta_result ta_chunks_init(ta_chunks *chunks, const ta_chunks_params *params) {
    if (params->chunk_size % TA_CHUNKS_LINK_BYTES != 0 || params->chunk_size < 2u * TA_CHUNKS_LINK_BYTES ||
        params->ceiling == UINT32_MAX) {
        return TA_ERR_INVALID;
    }
    ta_index_pool pool;
    ta_result result = ta_index_pool_init(&pool, &(ta_index_pool_params){
                                                     .allocator = params->allocator,
                                                     .item_size = params->chunk_size,
                                                     .item_alignment = _Alignof(uint32_t),
                                                     .link_offset = 0,
                                                     .page_shift = params->page_shift,
                                                     .ceiling = params->ceiling,
                                                 });
    if (result != TA_OK) {
        return result;
    }
    *chunks = (ta_chunks){ .pool = pool, .payload = params->chunk_size - TA_CHUNKS_LINK_BYTES };
    return TA_OK;
}

void ta_chunks_destroy(ta_chunks *chunks) {
    ta_index_pool_destroy(&chunks->pool);
}

uint32_t ta_chunks_need(const ta_chunks *chunks, uint32_t length, uint32_t n) {
    uint64_t total = (uint64_t)length + n;
    if (total > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)(ta_chunks_count(chunks, total) - ta_chunks_count(chunks, length));
}

ta_chunk_writer ta_chunk_writer_open(const ta_chunks *chunks, ta_chunk_chain chain) {
    ta_chunk_writer writer = { .chain = chain, .last = 0u };
    if (chain.length == 0u) {
        writer.chain.first = 0u;
        return writer;
    }
    uint32_t last = chain.first;
    for (uint64_t left = ta_chunks_count(chunks, chain.length); left > 1u; left -= 1u) {
        last = ta_chunks_link(chunks, last);
    }
    writer.last = last;
    return writer;
}

ta_result ta_chunks_write(ta_chunks *chunks, ta_chunk_writer *writer, ta_bytes bytes) {
    if (bytes.len == 0u) {
        return TA_OK;
    }
    uint32_t length = writer->chain.length;
    uint32_t need = ta_chunks_need(chunks, length, bytes.len);
    if (need > chunks->pool.free) {
        return TA_ERR_FULL;
    }
    const uint8_t *from = bytes.ptr;
    uint32_t left = bytes.len;
    /* The space left in the last chunk: none when the chain is empty or its last chunk is full. */
    uint32_t used = length % chunks->payload;
    if (length != 0u && used != 0u) {
        uint32_t room = chunks->payload - used;
        uint32_t count = left < room ? left : room;
        ta_mem_copy(ta_chunks_payload(chunks, writer->last) + used, from, count);
        from += count;
        left -= count;
    }
    for (uint32_t taken = 0; taken < need; taken += 1u) {
        uint32_t chunk = 0;
        (void)ta_index_pool_take(&chunks->pool, &chunk); /* cannot fail: `need` chunks are free */
        if (writer->last == 0u) {
            writer->chain.first = chunk;
        } else {
            ta_chunks_set_link(chunks, writer->last, chunk);
        }
        writer->last = chunk;
        uint32_t count = left < chunks->payload ? left : chunks->payload;
        ta_mem_copy(ta_chunks_payload(chunks, chunk), from, count);
        from += count;
        left -= count;
    }
    writer->chain.length = length + bytes.len;
    return TA_OK;
}

ta_bytes ta_chunks_read(const ta_chunks *chunks, ta_chunk_reader *reader, uint32_t max) {
    if (reader->remaining == 0u || max == 0u) {
        return (ta_bytes){ .ptr = NULL, .len = 0u };
    }
    uint32_t in_chunk = chunks->payload - reader->offset;
    uint32_t count = in_chunk < reader->remaining ? in_chunk : reader->remaining;
    count = count < max ? count : max;
    ta_bytes span = { .ptr = ta_chunks_payload(chunks, reader->chunk) + reader->offset, .len = count };
    reader->remaining -= count;
    reader->offset += count;
    if (reader->offset == chunks->payload) {
        reader->chunk = reader->remaining == 0u ? 0u : ta_chunks_link(chunks, reader->chunk);
        reader->offset = 0u;
    }
    return span;
}

void ta_chunks_free(ta_chunks *chunks, ta_chunk_chain *chain) {
    uint32_t chunk = chain->length == 0u ? 0u : chain->first;
    for (uint64_t left = ta_chunks_count(chunks, chain->length); left > 0u; left -= 1u) {
        uint32_t next = ta_chunks_link(chunks, chunk);
        ta_index_pool_give(&chunks->pool, chunk);
        chunk = next;
    }
    *chain = (ta_chunk_chain){ 0u, 0u };
}

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

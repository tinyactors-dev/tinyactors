#include "byte_io.h"

/*
 * Implementation of package byte_io. The package comment in byte_io.h is the
 * reference: what each adapter is for, who owns the storage it works on, its
 * lifetime (call: a stack struct for one write or read of one unit), when it
 * allocates (only ta_arena_sink_write, through the arena it was given; the
 * memory source and both chain adapters never do), failure behaviour (a
 * failed write changes nothing for that piece, and end(keep=false) discards
 * the unit) and complexity. It is not repeated here so the two cannot drift.
 *
 * Notes for the code below that the header does not need:
 *   - The arena sink's size sum goes through ta_checked_add, and an
 *     overflow answers TA_OP_NOMEM, never a short or wrapped write (the
 *     chain's own limit is checked in ta_chain_write).
 *   - Nothing here frees with a size it did not allocate with: the arena
 *     sink never frees (the arena owns every run it handed out), and the
 *     chain sink returns chunks only through ta_chain_free, which knows the
 *     pool's chunk size.
 *   - The vtables are static const, so an adapter's "as" is two pointers and
 *     no allocation.
 */

static ta_op_status ta_arena_sink_begin(void *self) {
    ((ta_arena_sink *)self)->len = 0;
    return TA_OP_OK;
}

static ta_op_status ta_arena_sink_write(void *self, ta_bytes bytes) {
    ta_arena_sink *sink = self;
    if (bytes.len == 0) {
        return TA_OP_OK;
    }
    if ((size_t)bytes.len > sink->cap - sink->len) {
        size_t need;
        if (!ta_checked_add(sink->len, (size_t)bytes.len, &need)) {
            return TA_OP_NOMEM;
        }
        size_t cap = sink->cap < 64u ? 64u : sink->cap;
        while (cap < need) {
            if (cap > SIZE_MAX / 2u) {
                return TA_OP_NOMEM;
            }
            cap *= 2u;
        }
        uint8_t *grown = ta_arena_alloc(sink->arena, cap, _Alignof(uint64_t));
        if (grown == NULL) {
            return TA_OP_NOMEM;
        }
        if (sink->len != 0) {
            ta_mem_copy(grown, sink->ptr, sink->len);
        }
        sink->ptr = grown;
        sink->cap = cap;
    }
    ta_mem_copy(sink->ptr + sink->len, bytes.ptr, bytes.len);
    sink->len += bytes.len;
    return TA_OP_OK;
}

static void ta_arena_sink_end(void *self, bool keep) {
    if (!keep) {
        ((ta_arena_sink *)self)->len = 0;
    }
}

static const ta_byte_sink_vtable ta_arena_sink_vtable = {
    .begin = ta_arena_sink_begin,
    .write = ta_arena_sink_write,
    .end = ta_arena_sink_end,
};

void ta_arena_sink_init(ta_arena_sink *sink, ta_arena *arena) {
    *sink = (ta_arena_sink){ .arena = arena };
}

ta_byte_sink ta_arena_sink_as(ta_arena_sink *sink) {
    return (ta_byte_sink){ .self = sink, .v = &ta_arena_sink_vtable };
}

static ta_op_status ta_memory_source_read(void *self, const ta_byte_read_params *params) {
    ta_memory_source *source = self;
    uint32_t left = source->bytes.len - source->at;
    uint32_t count = left < params->capacity ? left : params->capacity;
    if (count != 0) {
        ta_mem_copy(params->buffer, source->bytes.ptr + source->at, count);
    }
    source->at += count;
    *params->got = count;
    return TA_OP_OK;
}

static const ta_byte_source_vtable ta_memory_source_vtable = { .read = ta_memory_source_read };

void ta_memory_source_init(ta_memory_source *source, ta_bytes bytes) {
    *source = (ta_memory_source){ .bytes = bytes };
}

ta_byte_source ta_memory_source_as(ta_memory_source *source) {
    return (ta_byte_source){ .self = source, .v = &ta_memory_source_vtable };
}

static ta_op_status ta_chain_sink_begin(void *self) {
    ta_chain_sink *sink = self;
    const ta_chain *chain = sink->params.chain;
    if (sink->open || chain->length != 0 || chain->head != 0) {
        return TA_OP_ERROR;
    }
    if (sink->params.reserve != 0) {
        ta_pool_counts counts;
        ta_pool_counts_get(sink->params.pool, &counts);
        if (counts.free < ta_chain_need(sink->params.pool, chain, sink->params.reserve)) {
            return TA_OP_NOMEM;
        }
    }
    sink->freed = 0;
    sink->open = true;
    return TA_OP_OK;
}

static ta_op_status ta_chain_sink_write(void *self, ta_bytes bytes) {
    ta_chain_sink *sink = self;
    if (!sink->open) {
        return TA_OP_ERROR;
    }
    ta_result result = ta_chain_write(sink->params.pool, sink->params.chain,
                                      (ta_chain_span){ .bytes = bytes.ptr, .length = bytes.len });
    if (result == TA_OK) {
        return TA_OP_OK;
    }
    return result == TA_ERR_NOMEM ? TA_OP_NOMEM : TA_OP_ERROR;
}

static void ta_chain_sink_end(void *self, bool keep) {
    ta_chain_sink *sink = self;
    if (!sink->open) {
        return;
    }
    sink->open = false;
    if (!keep) {
        sink->freed = ta_chain_free(sink->params.pool, sink->params.chain);
    }
}

static const ta_byte_sink_vtable ta_chain_sink_vtable = {
    .begin = ta_chain_sink_begin,
    .write = ta_chain_sink_write,
    .end = ta_chain_sink_end,
};

void ta_chain_sink_init(ta_chain_sink *sink, const ta_chain_sink_params *params) {
    *sink = (ta_chain_sink){ .params = *params };
}

ta_byte_sink ta_chain_sink_as(ta_chain_sink *sink) {
    return (ta_byte_sink){ .self = sink, .v = &ta_chain_sink_vtable };
}

static ta_op_status ta_chain_source_read(void *self, const ta_byte_read_params *params) {
    ta_chain_source *source = self;
    uint32_t left = ta_chain_reader_remaining(&source->reader);
    uint32_t count = left < params->capacity ? left : params->capacity;
    if (count != 0) {
        /* Cannot fail: count <= remaining. */
        (void)ta_chain_reader_copy(&source->reader, params->buffer, count);
    }
    *params->got = count;
    return TA_OP_OK;
}

static const ta_byte_source_vtable ta_chain_source_vtable = { .read = ta_chain_source_read };

void ta_chain_source_init(ta_chain_source *source, const ta_pool *pool, const ta_chain *chain) {
    ta_chain_reader_init(&source->reader, pool, chain);
}

ta_byte_source ta_chain_source_as(ta_chain_source *source) {
    return (ta_byte_source){ .self = source, .v = &ta_chain_source_vtable };
}

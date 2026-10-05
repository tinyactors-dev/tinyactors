#include "bytes.h"
#include "chunks.h"
#include "memory.h"
#include "../include/tinyactors.h"

/* The smallest run an arena sink takes, in bytes. */
#define TA_ARENA_SINK_MIN_RUN 64u

static ta_op_status ta_arena_sink_begin(void *self) {
    ((ta_arena_sink *)self)->len = 0;
    return TA_OP_OK;
}

/* The capacity of the next run: doubled until `need` fits, at least the minimum. */
static size_t ta_arena_sink_next_cap(size_t cap, size_t need) {
    cap = cap < TA_ARENA_SINK_MIN_RUN ? TA_ARENA_SINK_MIN_RUN : cap;
    while (cap < need) {
        if (cap > SIZE_MAX / 2u) {
            return need;
        }
        cap *= 2u;
    }
    return cap;
}

static ta_op_status ta_arena_sink_write(void *self, ta_bytes bytes) {
    ta_arena_sink *sink = self;
    if (bytes.len == 0) {
        return TA_OP_OK;
    }
    if ((size_t)bytes.len > sink->cap - sink->len) {
        if ((size_t)bytes.len > SIZE_MAX - sink->len) {
            return TA_OP_NOMEM;
        }
        size_t cap = ta_arena_sink_next_cap(sink->cap, sink->len + (size_t)bytes.len);
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

ta_byte_sink ta_arena_sink_as_byte_sink(ta_arena_sink *sink) {
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

ta_byte_source ta_memory_source_as_byte_source(ta_memory_source *source) {
    return (ta_byte_source){ .self = source, .v = &ta_memory_source_vtable };
}

static ta_op_status ta_chunk_sink_begin(void *self) {
    ((ta_chunk_sink *)self)->writer = ta_chunk_writer_new();
    return TA_OP_OK;
}

static ta_op_status ta_chunk_sink_write(void *self, ta_bytes bytes) {
    ta_chunk_sink *sink = self;
    return ta_chunks_write(sink->chunks, &sink->writer, bytes) == TA_OK ? TA_OP_OK : TA_OP_NOMEM;
}

static void ta_chunk_sink_end(void *self, bool keep) {
    ta_chunk_sink *sink = self;
    if (!keep) {
        ta_chunks_free(sink->chunks, &sink->writer.chain);
        sink->writer = ta_chunk_writer_new();
    }
}

static const ta_byte_sink_vtable ta_chunk_sink_vtable = {
    .begin = ta_chunk_sink_begin,
    .write = ta_chunk_sink_write,
    .end = ta_chunk_sink_end,
};

void ta_chunk_sink_init(ta_chunk_sink *sink, ta_chunks *chunks) {
    *sink = (ta_chunk_sink){ .chunks = chunks, .writer = ta_chunk_writer_new() };
}

ta_byte_sink ta_chunk_sink_as_byte_sink(ta_chunk_sink *sink) {
    return (ta_byte_sink){ .self = sink, .v = &ta_chunk_sink_vtable };
}

static ta_op_status ta_chunk_source_read(void *self, const ta_byte_read_params *params) {
    ta_chunk_source *source = self;
    uint32_t got = 0;
    while (got < params->capacity) {
        ta_bytes span = ta_chunks_read(source->chunks, &source->reader, params->capacity - got);
        if (span.len == 0u) {
            break;
        }
        ta_mem_copy(params->buffer + got, span.ptr, span.len);
        got += span.len;
    }
    *params->got = got;
    return TA_OP_OK;
}

static const ta_byte_source_vtable ta_chunk_source_vtable = { .read = ta_chunk_source_read };

void ta_chunk_source_init(ta_chunk_source *source, const ta_chunks *chunks, ta_chunk_chain chain) {
    *source = (ta_chunk_source){ .chunks = chunks, .reader = ta_chunk_reader_of(chain) };
}

ta_byte_source ta_chunk_source_as_byte_source(ta_chunk_source *source) {
    return (ta_byte_source){ .self = source, .v = &ta_chunk_source_vtable };
}

#include "byte_io.h"

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

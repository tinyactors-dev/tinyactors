/*
 * A memory sink and a memory source for data-model codec tests (core ADR
 * 0090): the sink gathers what a value_write writes, and the source hands
 * bytes back in pieces of `piece` bytes, so a value_read sees every split.
 */
#ifndef TAT_TEST_BYTE_BUFFERS_H
#define TAT_TEST_BYTE_BUFFERS_H

#include <stdlib.h>
#include <string.h>

#include "tinyactors.h"

typedef struct {
    uint8_t *bytes;
    size_t len;
    size_t cap;
} test_sink;

static ta_op_status test_sink_begin(void *self) {
    ((test_sink *)self)->len = 0;
    return TA_OP_OK;
}

static ta_op_status test_sink_write(void *self, ta_bytes bytes) {
    test_sink *sink = self;
    if (sink->len + bytes.len > sink->cap) {
        size_t cap = sink->cap == 0 ? 64 : sink->cap;
        while (cap < sink->len + bytes.len) {
            cap *= 2;
        }
        uint8_t *grown = realloc(sink->bytes, cap);
        if (grown == NULL) {
            return TA_OP_NOMEM;
        }
        sink->bytes = grown;
        sink->cap = cap;
    }
    if (bytes.len != 0) {
        memcpy(sink->bytes + sink->len, bytes.ptr, bytes.len);
    }
    sink->len += bytes.len;
    return TA_OP_OK;
}

static void test_sink_end(void *self, bool keep) {
    if (!keep) {
        ((test_sink *)self)->len = 0;
    }
}

static const ta_byte_sink_vtable TEST_SINK = {
    .begin = test_sink_begin, .write = test_sink_write, .end = test_sink_end,
};

typedef struct {
    const uint8_t *bytes;
    size_t len;
    size_t at;
    uint32_t piece;
} test_source;

static ta_op_status test_source_read(void *self, const ta_byte_read_params *params) {
    test_source *source = self;
    size_t count = source->len - source->at;
    if (count > params->capacity) {
        count = params->capacity;
    }
    if (source->piece != 0 && count > source->piece) {
        count = source->piece;
    }
    if (count != 0) {
        memcpy(params->buffer, source->bytes + source->at, count);
    }
    source->at += count;
    *params->got = (uint32_t)count;
    return TA_OP_OK;
}

static const ta_byte_source_vtable TEST_SOURCE = { .read = test_source_read };

/* A sink that keeps nothing: it counts the bytes and the largest write, to
 * show a writer streams. */
typedef struct {
    size_t total;
    size_t largest;
} test_counting_sink;

static ta_op_status test_counting_begin(void *self) {
    (void)self;
    return TA_OP_OK;
}

static ta_op_status test_counting_write(void *self, ta_bytes bytes) {
    test_counting_sink *sink = self;
    sink->total += bytes.len;
    if (bytes.len > sink->largest) {
        sink->largest = bytes.len;
    }
    return TA_OP_OK;
}

static void test_counting_end(void *self, bool keep) {
    (void)self;
    (void)keep;
}

static const ta_byte_sink_vtable TEST_COUNTING_SINK = {
    .begin = test_counting_begin, .write = test_counting_write, .end = test_counting_end,
};

#endif

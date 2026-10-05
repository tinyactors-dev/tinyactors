/* Allocation through the embedder, growable buffers, the context, errors. */
#include "internal.h"

uint32_t sjs_version(void) {
    return SJS_VERSION;
}

void *sjs_mem_alloc(const sjs_allocator *allocator, size_t size) {
    if (size == 0) size = 1;
    return allocator->v->alloc(allocator->self, size, 16);
}

void sjs_mem_free(const sjs_allocator *allocator, void *ptr, size_t size) {
    if (ptr == NULL) return;
    if (size == 0) size = 1;
    sjs_allocator_free_params params = { ptr, size, 16 };
    allocator->v->free(allocator->self, &params);
}

void sjs_copy(void *to, const void *from, size_t length) {
    uint8_t *t = to;
    const uint8_t *f = from;
    for (size_t i = 0; i < length; i++) t[i] = f[i];
}

void sjs_zero(void *to, size_t length) {
    uint8_t *t = to;
    for (size_t i = 0; i < length; i++) t[i] = 0;
}

sjs_str sjs_cstr(const char *text) {
    uint32_t length = 0;
    while (text[length] != '\0') length++;
    return (sjs_str){ text, length };
}

sjs_result sjs_buf_reserve(const sjs_allocator *allocator, sjs_buf *buf, uint32_t extra) {
    if (extra > UINT32_MAX - buf->length) return SJS_ERR_LIMIT;
    uint32_t need = buf->length + extra;
    if (need <= buf->capacity) return SJS_OK;
    uint32_t capacity = buf->capacity < 64u ? 64u : buf->capacity;
    while (capacity < need) {
        capacity = capacity > UINT32_MAX / 2u ? need : capacity * 2u;
    }
    uint8_t *grown = sjs_mem_alloc(allocator, capacity);
    if (grown == NULL) return SJS_ERR_NOMEM;
    sjs_copy(grown, buf->ptr, buf->length);
    sjs_mem_free(allocator, buf->ptr, buf->capacity);
    buf->ptr = grown;
    buf->capacity = capacity;
    return SJS_OK;
}

sjs_result sjs_buf_append(const sjs_allocator *allocator, sjs_buf *buf, sjs_bytes bytes) {
    sjs_result r = sjs_buf_reserve(allocator, buf, bytes.length);
    if (r != SJS_OK) return r;
    sjs_copy(buf->ptr + buf->length, bytes.ptr, bytes.length);
    buf->length += bytes.length;
    return SJS_OK;
}

void sjs_buf_free(const sjs_allocator *allocator, sjs_buf *buf) {
    sjs_mem_free(allocator, buf->ptr, buf->capacity);
    buf->ptr = NULL;
    buf->length = 0;
    buf->capacity = 0;
}

/* The context counts what passes through it. */
static void *counting_alloc(void *self, size_t size, size_t alignment) {
    sjs_context *context = self;
    void *ptr = context->backing.v->alloc(context->backing.self, size, alignment);
    if (ptr != NULL) context->live += size;
    return ptr;
}

static void counting_free(void *self, const sjs_allocator_free_params *params) {
    sjs_context *context = self;
    context->live -= params->size;
    context->backing.v->free(context->backing.self, params);
}

static const sjs_allocator_vtable counting_vtable = { counting_alloc, counting_free };

sjs_result sjs_context_init(sjs_context **out, const sjs_context_config *config) {
    if (out == NULL || config == NULL || config->allocator.v == NULL) return SJS_ERR_INVALID;
    sjs_context *context = sjs_mem_alloc(&config->allocator, sizeof *context);
    if (context == NULL) return SJS_ERR_NOMEM;
    sjs_zero(context, sizeof *context);
    context->backing = config->allocator;
    context->counting = (sjs_allocator){ context, &counting_vtable };
    context->limits = config->limits;
    *out = context;
    return SJS_OK;
}

void sjs_context_destroy(sjs_context *context) {
    if (context == NULL) return;
    sjs_allocator backing = context->backing;
    sjs_mem_free(&backing, context, sizeof *context);
}

size_t sjs_context_live_bytes(const sjs_context *context) {
    return context == NULL ? 0 : context->live;
}

void sjs_error_set(sjs_error_slot *slot, sjs_position at, sjs_problem problem) {
    slot->error.code = problem.code;
    slot->error.at = at;
    slot->error.message = sjs_cstr(problem.message);
    slot->error.subject = (sjs_str){ slot->subject, 0 };
}

void sjs_error_subject(sjs_error_slot *slot, sjs_str subject) {
    uint32_t length = subject.length < SJS_SUBJECT_CAPACITY ? subject.length : SJS_SUBJECT_CAPACITY;
    /* Never cut a UTF-8 sequence in two. */
    while (length > 0 && length < subject.length && ((uint8_t)subject.ptr[length] & 0xC0u) == 0x80u) length--;
    sjs_copy(slot->subject, subject.ptr, length);
    slot->error.subject = (sjs_str){ slot->subject, length };
}

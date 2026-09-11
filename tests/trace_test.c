#include "../src/internal.h"
#include "host.h"
#include <assert.h>

static size_t starts, ends, errors;
static ta_status_t start_result;
static ta_span_t saved;
static ta_text_t saved_event;

static ta_status_t begin_cb(void *user, const ta_span_start_t *start, ta_span_t *out)
{
    (void)user;
    ++starts;
    assert(start != NULL && start->arena != NULL);
    saved_event = start->event_name;
    out->handle = 42u;
    return start_result;
}

static void end_cb(void *user, const ta_span_end_t *end)
{
    (void)user;
    ++ends;
    assert(end != NULL && end->span != NULL);
    saved = *end->span;
}

static void error_cb(void *user, uintptr_t handle, const ta_diagnostic_t *error)
{
    (void)user; (void)handle; (void)error; ++errors;
}

int main(void)
{
    ta_test_allocator_t allocator;
    ta_chunk_allocator_t callbacks;
    ta_arena_t *root;
    ta_system_t system = {0};
    ta_system_config_t config = {0};
    ta_span_start_t start = {0};
    tai_span_t *span;
    size_t acquires;

    ta_test_allocator_init(&allocator, 256u, 0u);
    callbacks = ta_test_allocator_callbacks(&allocator);
    assert(ta_arena_create(&callbacks, &(ta_arena_options_t){256u, 8192u, 8192u, 4096u}, &root) == TA_OK);
    system.arena = root;
    config.arenas[TA_LIFETIME_TRACE] = (ta_arena_options_t){256u, 4096u, 4096u, 4096u};
    config.trace.start = begin_cb; config.trace.end = end_cb; config.trace.error = error_cb;
    system.config = config;
    start.event_name = (ta_text_t){"event", 5u};
    start.operation = TA_SPAN_PROCESS;
    start.arena = NULL;
    tai_span_begin(&system, &start, &span);
    assert(span != NULL && starts == 1u && saved_event.data[0] == 'e');
    tai_span_error(span, &(ta_diagnostic_t){.status = TA_EXECUTION_ERROR});
    assert(errors == 1u);
    tai_span_finish(span, TA_OK);
    assert(ends == 1u && saved.handle == 42u);
    tai_span_finish(span, TA_FAULTED);
    assert(ends == 1u);

    config.trace.start = NULL; system.config = config;
    acquires = allocator.acquire_count;
    tai_span_begin(&system, &start, &span);
    assert(span == NULL && allocator.acquire_count == acquires);
    config.trace.start = begin_cb; start_result = TA_FAULTED; system.config = config;
    tai_span_begin(&system, &start, &span);
    assert(span == NULL && starts == 2u && ends == 1u);
    start_result = TA_OK;

    config.trace.start = begin_cb; system.config = config;
    tai_span_begin(&system, &start, &span);
    assert(span != NULL);
    ta_arena_close(root);
    assert(ends == 2u);
    assert(ta_test_allocator_releases_ok(&allocator));
    ta_test_allocator_reset(&allocator);
    return 0;
}

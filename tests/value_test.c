#include "../src/internal.h"
#include "host.h"
#include <assert.h>
#include <string.h>

static size_t snapshots;
static size_t releases;
static uintptr_t fail_source;

static void keep(void *user, ta_value_t value)
{
    (void)user; (void)value;
}

static ta_status_t snap(void *user, ta_value_t source, ta_value_t *out)
{
    (void)user;
    ++snapshots;
    if (source.handle == fail_source) return TA_NO_MEMORY;
    out->handle = source.handle + 100u;
    return TA_OK;
}

static void drop(void *user, ta_value_t value)
{
    (void)user; (void)value; ++releases;
}

static ta_arena_t *root_open(ta_test_allocator_t *allocator)
{
    ta_arena_options_t options = {256u, 8192u, 8192u, 4096u};
    ta_arena_t *root = NULL;
    ta_chunk_allocator_t callbacks = ta_test_allocator_callbacks(allocator);
    assert(ta_arena_create(&callbacks, &options, &root) == TA_OK);
    return root;
}

int main(void)
{
    ta_test_allocator_t allocator;
    ta_arena_t *root;
    ta_arena_t *arena;
    ta_system_t system = {0};
    tai_value_scope_t scope;
    ta_payload_t payload, copied;
    ta_body_t body, body_copy;
    ta_parameter_t params[2];
    const ta_trace_context_t *trace_copy;
    ta_trace_context_t trace;
    char bytes[] = "abc";
    char names[] = "xx";
    char metadata[300];
    size_t fail_index;
    ta_system_config_t config = {0};

    ta_test_allocator_init(&allocator, 256u, 0u);
    root = root_open(&allocator);
    assert(ta_arena_open(root, &(ta_arena_options_t){256u, 4096u, 4096u, 4096u}, &arena) == TA_OK);
    config.limits.trace_metadata_bytes = 5u;
    config.values.snapshot = snap;
    config.values.release = drop;
    config.values.retain = keep;
    system.arena = root;
    system.config = config;
    scope.system = &system; scope.arena = arena;

    payload.kind = TA_PAYLOAD_BYTES;
    payload.as.bytes.data = bytes; payload.as.bytes.size = 3u;
    assert(tai_payload_snapshot(&scope, &payload, &copied) == TA_OK);
    bytes[0] = 'z';
    assert(((const char *)copied.as.bytes.data)[0] == 'a');

    payload.kind = TA_PAYLOAD_NATIVE; payload.as.native.handle = 7u;
    assert(tai_payload_snapshot(&scope, &payload, &copied) == TA_OK);
    assert(copied.as.native.handle == 107u && snapshots == 1u);

    params[0].name = (ta_text_t){names, 1u}; params[0].value.handle = 1u;
    params[1].name = (ta_text_t){names + 1, 1u}; params[1].value.handle = 2u;
    body = (ta_body_t){0}; body.kind = TA_BODY_PARAMETERS;
    body.parameters = params; body.parameter_count = 2u;
    assert(tai_body_snapshot(&scope, &body, &body_copy) == TA_OK);
    assert(body_copy.parameter_count == 2u && body_copy.parameters[0].value.handle == 101u &&
           body_copy.parameters[1].value.handle == 102u);
    assert(body_copy.parameters[0].name.data[0] == 'x' && body_copy.parameters[1].name.data[0] == 'x');

    fail_source = 2u;
    assert(tai_body_snapshot(&scope, &body, &body_copy) == TA_NO_MEMORY);
    fail_source = 0u;

    memset(&trace, 0, sizeof(trace));
    trace.tracestate = (ta_text_t){"ab", 2u}; trace.baggage = (ta_text_t){"cde", 3u};
    assert(tai_trace_copy(&scope, &trace, &trace_copy) == TA_OK);
    assert(trace_copy != NULL && trace_copy->tracestate.data != trace.tracestate.data);
    trace.baggage.size = 4u;
    assert(tai_trace_copy(&scope, &trace, &trace_copy) == TA_LIMIT);
    assert(tai_trace_copy(&scope, NULL, &trace_copy) == TA_OK && trace_copy == NULL);

    memset(metadata, 'm', sizeof(metadata));
    system.config.limits.trace_metadata_bytes = sizeof(metadata);
    for (fail_index = 0u; fail_index < 3u; ++fail_index) {
        ta_arena_t *failed_arena = NULL;
        ta_memory_stats_t before, after;
        ta_arena_stats(root, &before);
        assert(ta_arena_open(root, &(ta_arena_options_t){256u, 4096u, 4096u, 4096u}, &failed_arena) == TA_OK);
        allocator.fail_acquire = allocator.acquire_count + 1u;
        scope.arena = failed_arena;
        trace.tracestate = (ta_text_t){metadata, sizeof(metadata)};
        trace.baggage = (ta_text_t){NULL, 0u};
        assert(tai_trace_copy(&scope, &trace, &trace_copy) == TA_NO_MEMORY);
        ta_arena_close(failed_arena);
        ta_arena_stats(root, &after);
        assert(before.reserved_bytes == after.reserved_bytes && before.used_bytes == after.used_bytes &&
               before.descendant_count == after.descendant_count);
        allocator.fail_acquire = 0u;
    }
    scope.arena = arena;

    assert(ta_arena_set_limit(arena, 1u) != TA_OK);
    ta_arena_close(arena);
    assert(releases == 4u);
    ta_arena_close(root);
    assert(ta_test_allocator_releases_ok(&allocator));
    ta_test_allocator_reset(&allocator);
    return 0;
}

#include "internal.h"
#include "host.h"
#include <assert.h>

typedef struct trace_entry {
    ta_span_operation_t operation;
    ta_span_kind_t kind;
    ta_span_t span;
    ta_trace_context_t parent;
    ta_trace_context_t link;
    size_t links;
    uint8_t ended;
} trace_entry_t;

typedef struct trace_fixture {
    ta_test_allocator_t allocator;
    ta_arena_t *arena;
    ta_system_t *system;
    ta_definition_t *definition;
    ta_actor_ref_t target;
    trace_entry_t entries[32];
    size_t count;
    size_t ends;
} trace_fixture_t;

static ta_status_t trace_start(void *user, const ta_span_start_t *start, ta_span_t *out)
{
    trace_fixture_t *f = (trace_fixture_t *)user;
    trace_entry_t *e;
    assert(f->count < 32 && start != NULL);
    e = &f->entries[f->count++];
    e->operation = start->operation; e->kind = start->kind;
    e->links = start->link_count;
    if(start->link_count!=0)e->link=start->links[0];
    if (start->parent != NULL) e->parent = *start->parent;
    out->handle = (uintptr_t)f->count;
    out->context = (ta_trace_context_t){0};
    out->context.trace_id[0] = 0xdd;
    out->context.span_id[7] = (uint8_t)f->count;
    e->span=*out;
    return TA_OK;
}

static void trace_end(void *user, const ta_span_end_t *end)
{
    trace_fixture_t *f = (trace_fixture_t *)user;
    size_t i;
    assert(end != NULL && end->span != NULL);
    ++f->ends;
    for (i = 0; i < f->count; ++i)
        if (f->entries[i].span.handle == end->span->handle) {
            assert(!f->entries[i].ended); f->entries[i].ended = 1; return;
        }
    assert(0); /* No end without a matching successful start. */
}

static ta_status_t action(ta_context_t *context, ta_action_kind_t kind, ta_code_t code)
{
    trace_fixture_t *f = ta_context_host_user(context);
    ta_send_t send = {0};
    uint64_t id;
    (void)kind;
    if (code != 1) return TA_OK;
    send.event = ta_text_from_cstr("local");
    send.target.kind = TA_TARGET_INSTANCE; send.target.instance = f->target;
    if (ta_context_send(context, &send, &id) != TA_OK) return TA_EXECUTION_ERROR;
    send = (ta_send_t){0};
    send.event = ta_text_from_cstr("external"); send.type = ta_text_from_cstr("trace-io");
    send.target.kind = TA_TARGET_URI; send.target.text = ta_text_from_cstr("/sink");
    return ta_context_send(context, &send, &id);
}

static void fixture_open(trace_fixture_t *f)
{
    const ta_ioprocessor_desc_t processors[] = {
        {.type = "trace-io", .aliases = (const char *const[]) {"trace", NULL}}, {0}};
    const ta_action_t fire = {.kind = TA_ACTION_CUSTOM, .as.custom.code = 1};
    const ta_chart_t chart = {
        .name = "trace-message", .data_model = "bytes",
        .data_layout = {.storage = TA_DATA_BYTES, .bytes_layout = {1, 1}},
        .states = (const ta_chart_state_t[]) {{
            .id = "idle", .transitions = (const ta_chart_transition_t[]) {
                {.event = "go", .content = {.count = 1, .actions = &fire}}, {0}}}, {0}}};
    ta_chunk_allocator_t allocator;
    ta_system_config_t config;
    ta_definition_result_t result;
    *f = (trace_fixture_t){0};
    ta_test_allocator_init(&f->allocator, 0, 0); allocator = ta_test_allocator_callbacks(&f->allocator);
    assert(ta_arena_create(&allocator, &(ta_arena_options_t){256,65536,TA_UNLIMITED,65536}, &f->arena) == TA_OK);
    ta_system_config_init(&config); config.session_prefix = ta_text_from_cstr("trace");
    config.data_model = ta_text_from_cstr("bytes"); config.host_user = f; config.data.run = action;
    config.ioprocessors = processors; config.trace.user = f; config.trace.start = trace_start; config.trace.end = trace_end;
    assert(ta_system_create(f->arena, &config, &f->system) == TA_OK);
    assert(ta_definition_build(f->system, &chart, &result) == TA_OK); f->definition = result.definition;
}

static ta_actor_ref_t spawn(trace_fixture_t *f)
{
    ta_actor_ref_t actor;
    assert(ta_actor_spawn(f->system, &(ta_spawn_options_t){.definition = f->definition}, &actor) == TA_OK);
    return actor;
}

static void run(trace_fixture_t *f, size_t units)
{
    ta_run_result_t result;
    assert(ta_system_run(f->system, &(ta_run_options_t){1, {units, 1}}, &result) == TA_OK);
}

static void cleanup(trace_fixture_t *f, ta_actor_ref_t first, ta_actor_ref_t second)
{
    assert(ta_actor_stop(f->system, first) == TA_OK); assert(ta_actor_stop(f->system, second) == TA_OK);
    run(f, 100); while (f->system->requests != NULL) { const ta_io_request_t *r; assert(ta_io_next(f->system, &r) == TA_OK); assert(ta_io_complete(f->system, r->request, TA_CANCELLED) == TA_OK); }
    assert(ta_actor_release(f->system, first) == TA_OK); assert(ta_actor_release(f->system, second) == TA_OK);
    ta_definition_release(f->definition); assert(ta_system_destroy(f->system) == TA_OK); ta_arena_close(f->arena);
    assert(ta_test_allocator_releases_ok(&f->allocator)); ta_test_allocator_reset(&f->allocator);
}

int main(void)
{
    trace_fixture_t f; ta_actor_ref_t first, second; ta_trace_context_t incoming = {{1}, {2}, 1, 0, {0}, {0}};
    size_t i;
    const ta_io_request_t *request;
    fixture_open(&f); first = spawn(&f); second = spawn(&f); f.target = second; run(&f, 100);
    assert(ta_post(f.system, &(ta_post_options_t){.target = {TA_TARGET_INSTANCE, first, {0}}, .message = {.name = ta_text_from_cstr("go"), .trace = &incoming}}, NULL) == TA_OK);
    run(&f,1);
    assert(f.count==3 && f.ends==1);
    assert(f.entries[0].operation==TA_SPAN_PROCESS && f.entries[0].kind==TA_SPAN_CONSUMER);
    assert(f.entries[0].links==1 && f.entries[0].link.trace_id[0]==1 && f.entries[0].link.span_id[0]==2);
    assert(!f.entries[0].ended && !f.entries[2].ended && f.entries[1].ended);
    for(i=1;i<3;++i) {
        assert(f.entries[i].operation==TA_SPAN_SEND && f.entries[i].kind==TA_SPAN_PRODUCER);
        assert(f.entries[i].parent.span_id[7]==1 && f.entries[i].parent.trace_id[0]==0xdd);
    }
    for(i=0;i<20;++i)run(&f,1);
    assert(f.count==4 && f.ends==3);
    assert(f.entries[3].kind==TA_SPAN_CONSUMER && f.entries[3].links==1 && f.entries[3].link.span_id[7]==2);
    assert(ta_io_next(f.system,&request)==TA_OK && request->trace->span_id[7]==3);
    assert(!f.entries[2].ended);
    assert(ta_io_complete(f.system,request->request,TA_OK)==TA_OK && f.entries[2].ended);
    cleanup(&f, first, second);
    assert(f.count==f.ends);
    for(i=0;i<f.count;++i)assert(f.entries[i].ended);
    return 0;
}

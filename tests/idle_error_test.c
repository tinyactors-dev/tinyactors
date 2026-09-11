#include "host.h"
#include <assert.h>

static ta_status_t count_error(ta_context_t *context, ta_action_kind_t kind,
                               ta_code_t code)
{
    ta_mut_bytes_t bytes;
    (void)kind;
    (void)code;
    assert(ta_context_bytes(context, &bytes) == TA_OK);
    ++*(uint32_t *)bytes.data;
    return TA_OK;
}

int main(void)
{
    ta_test_allocator_t allocator;
    ta_chunk_allocator_t chunks;
    ta_arena_options_t arena_options = {512, 65536, (size_t)-1, 65536};
    ta_arena_t *root;
    ta_system_t *system;
    ta_system_config_t config;
    ta_definition_result_t definition;
    ta_actor_ref_t actor;
    ta_actor_info_t info;
    ta_spawn_options_t spawn = {0};
    ta_run_options_t run = {0, {100, 10}};
    ta_run_result_t result;
    ta_io_route_t route;
    const ta_ioprocessor_desc_t processors[] = {{.type = "urn:test"}, {0}};
    const ta_chart_t chart = {
        .name = "idle-error", .data_model = "c",
        .data_layout = {.storage = TA_DATA_BYTES,
                        .bytes_layout = {sizeof(uint32_t), sizeof(uint32_t)}},
        .states = (const ta_chart_state_t[]){
            {.id = "idle", .transitions = (const ta_chart_transition_t[]){
                {.event = "error.communication", .content = {
                    .actions = (const ta_action_t[]){
                        {.kind = TA_ACTION_CUSTOM, .as.custom = {.code = 1}}
                    }, .count = 1}}, {0}}}, {0}}
    };
    ta_test_allocator_init(&allocator, 0, 0);
    chunks = ta_test_allocator_callbacks(&allocator);
    assert(ta_arena_create(&chunks, &arena_options, &root) == TA_OK);
    ta_system_config_init(&config);
    config.session_prefix = ta_text_from_cstr("idle");
    config.data_model = ta_text_from_cstr("c");
    config.data.run = count_error;
    config.ioprocessors = processors;
    assert(ta_system_create(root, &config, &system) == TA_OK);
    assert(ta_definition_build(system, &chart, &definition) == TA_OK);
    spawn.definition = definition.definition;
    assert(ta_actor_spawn(system, &spawn, &actor) == TA_OK);
    assert(ta_system_run(system, &run, &result) == TA_OK);
    assert(ta_actor_inspect(system, actor, &info) == TA_OK && info.stable);
    route.processor = ta_text_from_cstr("urn:test");
    route.destination = actor;
    assert(ta_io_receive_error(system, &route, ta_text_from_cstr("send-1")) == TA_OK);
    assert(ta_system_run(system, &run, &result) == TA_OK);
    assert(ta_actor_inspect(system, actor, &info) == TA_OK);
    assert(info.stable && info.internal_events == 0);
    assert(*(const uint32_t *)info.state.as.bytes.data == 1);
    assert(ta_actor_stop(system, actor) == TA_OK);
    assert(ta_system_run(system, &run, &result) == TA_OK);
    assert(ta_actor_release(system, actor) == TA_OK);
    ta_definition_release(definition.definition);
    assert(ta_system_destroy(system) == TA_OK);
    ta_arena_close(root);
    assert(allocator.live_bytes == 0 && ta_test_allocator_releases_ok(&allocator));
    ta_test_allocator_reset(&allocator);
    return 0;
}

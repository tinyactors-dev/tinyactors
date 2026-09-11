#include "../src/internal.h"
#include "host.h"

static int make_root(ta_test_allocator_t *pool, ta_arena_t **out)
{
    ta_arena_options_t options = { 256u, 4096u, TA_UNLIMITED, 4096u };
    ta_chunk_allocator_t allocator;
    ta_test_allocator_init(pool, 0u, 0u);
    allocator = ta_test_allocator_callbacks(pool);
    return ta_arena_create(&allocator, &options, out) == TA_OK;
}

static ta_status_t test_eval(ta_context_t *c, ta_code_t code, ta_value_t *out) { (void)c; (void)code; out->handle=1u; return TA_OK; }
static ta_status_t test_test(ta_context_t *c, ta_code_t code, uint8_t *out) { (void)c; (void)code; *out=1u; return TA_OK; }
static ta_status_t test_iter(ta_context_t *c, ta_code_t code, ta_iteration_t *out) { (void)c; (void)code; out->snapshot.handle=1u; out->count=1u; return TA_OK; }
static ta_status_t test_bind(ta_context_t *c, const ta_iteration_step_t *step) { (void)c; (void)step; return TA_OK; }
static ta_status_t test_run(ta_context_t *c, ta_action_kind_t k, ta_code_t code) { (void)c; (void)k; (void)code; return TA_OK; }
static unsigned test_retains, test_releases, test_storage_releases;
static void test_value_retain(void *u, ta_value_t v) { (void)u; (void)v; ++test_retains; }
static void test_value_release(void *u, ta_value_t v) { (void)u; (void)v; ++test_releases; }
static ta_status_t test_value_snapshot(void *u, ta_value_t v, ta_value_t *out) { (void)u; *out=v; return TA_OK; }
static void test_storage_release(void *u) { (void)u; ++test_storage_releases; }

int main(void)
{
    ta_test_allocator_t pool;
    ta_arena_t *root;
    ta_system_t *system;
    ta_system_config_t config;
    ta_definition_result_t result;
    ta_state_desc_t states[2] = {0};
    ta_definition_desc_t description = {0};
    ta_definition_input_t input;
    char name[] = "copy-me";
    char event_name[] = "nested.event";
    ta_action_t send_action = { .kind = TA_ACTION_SEND, .as.send = { .event = { { event_name, 12u }, 0u }, .body = { TA_BODY_CONTENT, 1u, 0u, NULL, 0u } } };
    ta_action_t foreach_action = { .kind = TA_ACTION_FOREACH, .as.foreach_loop = { 1u, {"item",4u}, {"index",5u}, { &send_action, 1u } } };
    ta_branch_t branch = { 1u, { &foreach_action, 1u } };
    ta_action_t if_action = { .kind = TA_ACTION_IF, .as.conditional = { &branch, 1u } };
    if (!make_root(&pool, &root)) return 1;
    ta_system_config_init(&config);
    config.session_prefix=(ta_text_t){"test",4u}; config.data_model=(ta_text_t){"bytes",5u};
    config.values.retain=test_value_retain; config.values.release=test_value_release; config.values.snapshot=test_value_snapshot;
    config.data.evaluate=test_eval; config.data.test=test_test; config.data.iteration_open=test_iter;
    config.data.iteration_bind=test_bind; config.data.run=test_run;
    if (ta_system_create(root, &config, &system) != TA_OK) return 2;
    states[0].name = (ta_text_t){"$root", 5u}; states[0].parent = TA_INDEX_NONE; states[0].kind = TA_STATE_ROOT;
    states[1].name = (ta_text_t){name, 7u}; states[1].parent = 0u; states[1].kind = TA_STATE_ATOMIC;
    states[1].onentry.blocks = &(ta_block_t){ &if_action, 1u }; states[1].onentry.count = 1u;
    description.name = (ta_text_t){"definition", 10u}; description.code_owner.handle=11u;
    description.data_model = (ta_text_t){"bytes",5u};
    description.data_layout.storage = TA_DATA_BYTES;
    description.data_layout.bytes_layout.alignment = 1u;
    description.states = states; description.state_count = 2u;
    input.description = &description; input.options.storage = TA_DEFINITION_COPY;
    if (ta_definition_prepare(system, &input, &result) != TA_OK) return 7;
    name[0] = 'X';
    if (ta_definition_description(result.definition)->states[1].name.data[0] != 'c') return 3;
    event_name[0] = 'X';
    if (ta_definition_description(result.definition)->states[1].onentry.blocks[0].actions[0].as.conditional.branches[0].body.actions[0].as.foreach_loop.body.actions[0].as.send.event.literal.data[0] != 'n') return 6;
    if (ta_definition_revision(result.definition) == 0u) return 4;
    ta_definition_release(result.definition);
    if (system->definitions != NULL || system->stats.definitions != 0u) return 5;
    if (ta_system_destroy(system) != TA_OK) return 8;
    if (test_retains != 1u || test_releases != 1u) return 9;
    ta_arena_close(root);
    if (!ta_test_allocator_releases_ok(&pool)) return 16;
    ta_test_allocator_reset(&pool);
    /* A BORROW lease is released exactly once, after the final reference. */
    ta_test_allocator_t pool2; ta_arena_t *root2; ta_system_t *system2; ta_system_config_t config2;
    ta_definition_result_t borrowed; ta_definition_input_t borrowed_input=input;
    if (!make_root(&pool2,&root2)) return 10; ta_system_config_init(&config2); config2.session_prefix=(ta_text_t){"borrow",6u}; config2.data_model=(ta_text_t){"bytes",5u}; config2.data.evaluate=test_eval; config2.data.test=test_test; config2.data.iteration_open=test_iter; config2.data.iteration_bind=test_bind; config2.data.run=test_run; config2.values.retain=test_value_retain; config2.values.release=test_value_release; config2.values.snapshot=test_value_snapshot;
    if (ta_system_create(root2,&config2,&system2)!=TA_OK) return 11; borrowed_input.options.storage=TA_DEFINITION_BORROW; borrowed_input.options.storage_release=test_storage_release; test_storage_releases=0u;
    if (ta_definition_prepare(system2,&borrowed_input,&borrowed)!=TA_OK) return 12; ta_definition_retain(borrowed.definition); ta_definition_release(borrowed.definition); if(test_storage_releases!=0u)return 13; ta_definition_release(borrowed.definition); if(test_storage_releases!=1u)return 14; if(ta_system_destroy(system2)!=TA_OK)return 15; ta_arena_close(root2); ta_test_allocator_reset(&pool2);
    return 0;
}

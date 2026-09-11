#include "host.h"
#include "../src/internal.h"
#include <assert.h>

typedef struct fixture { ta_test_allocator_t host; ta_arena_t *root; ta_system_t *system; } fixture_t;

static void fixture_open(fixture_t *f)
{
    ta_chunk_allocator_t allocator;
    ta_arena_options_t options = {512, 65536, (size_t)-1, 65536};
    ta_system_config_t config;
    ta_test_allocator_init(&f->host, 0, 0);
    allocator = ta_test_allocator_callbacks(&f->host);
    assert(ta_arena_create(&allocator, &options, &f->root) == TA_OK);
    ta_system_config_init(&config);
    config.session_prefix = ta_text_from_cstr("validation");
    config.data_model = ta_text_from_cstr("bytes");
    assert(ta_system_create(f->root, &config, &f->system) == TA_OK);
}

static void fixture_close(fixture_t *f)
{
    assert(ta_system_destroy(f->system) == TA_OK);
    ta_arena_close(f->root);
    assert(ta_test_allocator_releases_ok(&f->host));
    ta_test_allocator_reset(&f->host);
}

static void expect(fixture_t *f, const ta_definition_desc_t *description, ta_status_t status)
{
    ta_definition_input_t input = { .description = description };
    ta_definition_result_t result;
    ta_memory_stats_t before, after;
    ta_arena_stats(f->root, &before);
    assert(ta_definition_prepare(f->system, &input, &result) == status);
    if (status == TA_OK) {
        assert(result.definition != NULL);
        ta_definition_release(result.definition);
    } else {
        assert(result.definition == NULL);
        assert(result.error.status == status);
    }
    ta_arena_stats(f->root, &after);
    assert(before.reserved_bytes == after.reserved_bytes);
    assert(before.used_bytes == after.used_bytes);
    assert(before.descendant_count == after.descendant_count);
}

static ta_definition_desc_t description(ta_state_desc_t *states, uint32_t count)
{
    ta_definition_desc_t d = {
        .name = {"validation", 10}, .data_model = {"bytes", 5},
        .data_layout = {.storage = TA_DATA_BYTES, .bytes_layout = {0, 1}},
        .states = states, .state_count = count
    };
    states[0] = (ta_state_desc_t){ .name = {"$root", 5}, .parent = UINT32_MAX, .kind = TA_STATE_ROOT };
    return d;
}

static void graph_cases(fixture_t *f)
{
    ta_state_desc_t states[6] = {
        {0}, { .name = {"A", 1}, .parent = 0, .kind = TA_STATE_COMPOUND },
        { .name = {"B", 1}, .parent = 1, .kind = TA_STATE_ATOMIC },
        { .name = {"C", 1}, .parent = 0, .kind = TA_STATE_ATOMIC },
        { .name = {"D", 1}, .parent = 1, .kind = TA_STATE_ATOMIC }
    };
    ta_definition_desc_t d = description(states, 4);
    ta_text_t event = {"go", 2};
    uint32_t target[] = {3};
    ta_transition_desc_t transition = {
        .source = 2, .events = &event, .event_count = 1, .targets = {target, 1}
    };
    states[2].transition_count = 1;
    d.transitions = &transition;
    d.transition_count = 1;
    expect(f, &d, TA_OK);
    d.state_count = 5;
    expect(f, &d, TA_INVALID); /* Closed A subtree cannot resume after C. */
    d.state_count = 4;
    transition.source = 3;
    expect(f, &d, TA_INVALID);
    transition.source = 2;
    target[0] = 6;
    expect(f, &d, TA_INVALID);
    target[0] = 3;
    states[3].name = states[2].name;
    expect(f, &d, TA_INVALID);
    states[3].name = ta_text_from_cstr("C");
    d.data_model = ta_text_from_cstr("other");
    expect(f, &d, TA_INVALID);
}

static void history_cases(fixture_t *f)
{
    uint32_t saved[] = {2}, restore[] = {3};
    ta_text_t event = {"restore", 7};
    ta_state_desc_t states[] = {
        {0}, { .name = {"A", 1}, .parent = 0, .kind = TA_STATE_COMPOUND },
        { .name = {"B", 1}, .parent = 1, .kind = TA_STATE_ATOMIC, .transition_count = 1 },
        { .name = {"H", 1}, .parent = 1, .kind = TA_STATE_HISTORY_DEEP,
          .initial = {.targets = {saved, 1}} }
    };
    ta_transition_desc_t transition = {
        .source = 2, .events = &event, .event_count = 1, .targets = {restore, 1}
    };
    ta_definition_desc_t d = description(states, 4);
    d.transitions = &transition;
    d.transition_count = 1;
    expect(f, &d, TA_OK);
    saved[0] = 3;
    expect(f, &d, TA_INVALID);
    saved[0] = 2;
    states[3].kind = TA_STATE_HISTORY_SHALLOW;
    expect(f, &d, TA_OK);
    saved[0] = 1;
    expect(f, &d, TA_INVALID);
}

static void target_cases(fixture_t *f)
{
    uint32_t targets[] = {2, 3};
    ta_text_t event = {"go", 2};
    ta_state_desc_t states[] = {
        {0}, {.name = {"P", 1}, .parent = 0, .kind = TA_STATE_PARALLEL},
        {.name = {"A", 1}, .parent = 1, .kind = TA_STATE_ATOMIC, .transition_count = 1},
        {.name = {"B", 1}, .parent = 1, .kind = TA_STATE_ATOMIC}
    };
    ta_transition_desc_t transition = {
        .source = 2, .events = &event, .event_count = 1, .targets = {targets, 2}
    };
    ta_definition_desc_t d = description(states, 4);
    d.transitions = &transition;
    d.transition_count = 1;
    expect(f, &d, TA_OK);
    states[1].kind = TA_STATE_COMPOUND;
    expect(f, &d, TA_INVALID);
    states[1].kind = TA_STATE_PARALLEL;
    targets[1] = 2;
    expect(f, &d, TA_INVALID);
    targets[1] = 1;
    expect(f, &d, TA_INVALID);
}

static void action_cases(fixture_t *f)
{
    ta_action_t action = {.kind = TA_ACTION_CUSTOM, .as.custom.code = 1};
    ta_block_t block = { &action, 1 };
    ta_branch_t branch = { .body = { &action, 1 } };
    ta_state_desc_t states[] = {
        {0}, {.name = {"A", 1}, .parent = 0, .kind = TA_STATE_ATOMIC, .onentry = {&block, 1}}
    };
    ta_definition_desc_t d = description(states, 2);
    expect(f, &d, TA_UNSUPPORTED);
    action = (ta_action_t){ .kind = TA_ACTION_IF, .as.conditional = { &branch, 1 } };
    expect(f, &d, TA_INVALID);
    action = (ta_action_t){ .kind = TA_ACTION_RAISE, .as.raise.event = {"raised", 6} };
    expect(f, &d, TA_OK);
    f->system->config.limits.definition_actions = 0;
    expect(f, &d, TA_LIMIT);
    f->system->config.limits.definition_actions = (size_t)-1;
}

int main(void)
{
    fixture_t f;
    fixture_open(&f);
    graph_cases(&f);
    history_cases(&f);
    target_cases(&f);
    action_cases(&f);
    fixture_close(&f);
    return 0;
}

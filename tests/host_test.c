#include "host.h"
#include <stdint.h>

static int check_allocator(void)
{
    ta_test_allocator_t state;
    ta_chunk_allocator_t allocator;
    ta_chunk_t first;
    ta_chunk_t second;
    ta_layout_t layout = { 33u, 64u };
    ta_layout_t other = { 1u, 1u };
    ta_test_allocator_init(&state, 128u, 0u);
    allocator = ta_test_allocator_callbacks(&state);
    if (allocator.acquire(allocator.user, &layout, &first) != TA_OK) return 1;
    if (first.layout.size != 128u || ((uintptr_t)first.data % 64u) != 0u) return 2;
    if (state.live_bytes != 128u || state.peak_bytes != 128u || state.live_blocks != 1u) return 3;
    second = first; second.layout = other;
    allocator.release(allocator.user, &second);
    if (state.bad_releases != 1u || state.live_blocks != 1u) return 4;
    allocator.release(allocator.user, &first);
    if (ta_test_allocator_releases_ok(&state) || state.release_count != 1u) return 5;
    ta_test_allocator_reset(&state);
    ta_test_allocator_init(&state, 128u, 0u);
    allocator = ta_test_allocator_callbacks(&state);
    layout.size = 128u;
    if (allocator.acquire(allocator.user, &layout, &first) != TA_OK || first.layout.size != 128u) return 6;
    allocator.release(allocator.user, &first);
    ta_test_allocator_reset(&state);
    ta_test_allocator_init(&state, SIZE_MAX, 0u);
    allocator = ta_test_allocator_callbacks(&state);
    layout.size = SIZE_MAX - 1u;
    if (allocator.acquire(allocator.user, &layout, &first) != TA_NO_MEMORY) return 7;
    ta_test_allocator_reset(&state);
    ta_test_allocator_init(&state, 0u, 2u);
    allocator = ta_test_allocator_callbacks(&state);
    layout.size = 33u;
    if (allocator.acquire(allocator.user, &layout, &first) != TA_OK) return 8;
    if (allocator.acquire(allocator.user, &layout, &second) != TA_NO_MEMORY) return 9;
    if (state.failed_acquires != 1u) return 10;
    allocator.release(allocator.user, &first);
    if (!ta_test_allocator_releases_ok(&state)) return 11;
    ta_test_allocator_reset(&state);
    return 0;
}

static int check_trace(void)
{
    ta_test_trace_t trace;
    ta_event_t event = {0};
    ta_actor_termination_t termination = { { 7u }, TA_ACTOR_DONE, TA_OK };
    ta_actor_ref_t actor = { 7u };
    ta_test_action_t action = { actor, 4u, 9u, TA_ACTION_CUSTOM, (ta_code_t)55u };
    char mutable_name[] = "tick";
    event.name.data = mutable_name; event.name.size = 4u;
    ta_test_trace_init(&trace);
    if (!ta_test_trace_event(&trace, actor, &event)) return 10;
    mutable_name[0] = 'T';
    if (!ta_test_trace_action(&trace, &action)) return 11;
    if (!ta_test_trace_terminal(&trace, &termination)) return 12;
    if (trace.count != 3u || trace.entries[0].text.size != 4u ||
        trace.entries[0].text.data[0] != 't' || trace.entries[1].code != (ta_code_t)55u ||
        trace.entries[1].state != 4u || trace.entries[2].status != TA_OK) return 13;
    ta_test_trace_reset(&trace);
    return 0;
}

int main(void)
{
    int result = check_allocator();
    if (result != 0) return result;
    return check_trace();
}

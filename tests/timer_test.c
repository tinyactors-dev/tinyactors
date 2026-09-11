#include "../src/internal.h"
#include "host.h"
#include <assert.h>
#include <string.h>

static ta_send_t send_value(uint64_t delay, const char *id)
{
    ta_send_t send = {0};
    send.event = (ta_text_t){"tick", 4u};
    send.sendid = id == NULL ? (ta_text_t){NULL, 0u} : ta_text_from_cstr(id);
    send.body.kind = TA_BODY_NONE;
    send.target.kind = TA_TARGET_SELF;
    send.delay_ns = delay;
    return send;
}

int main(void)
{
    ta_test_allocator_t allocator;
    ta_chunk_allocator_t callbacks;
    ta_arena_t *root;
    ta_system_t system = {0};
    ta_system_config_t config = {0};
    tai_actor_t actor1 = {0}, actor2 = {0};
    tai_actor_extra_t extra1 = {0}, extra2 = {0};
    ta_actor_limits_t limits = {0};
    tai_timer_t *a, *b, *c, *taken;
    ta_send_t send;

    ta_test_allocator_init(&allocator, 256u, 0u);
    callbacks = ta_test_allocator_callbacks(&allocator);
    assert(ta_arena_create(&callbacks, &(ta_arena_options_t){256u, 16384u, 16384u, 4096u}, &root) == TA_OK);
    config.limits.delayed_sends = 100u;
    config.arenas[TA_LIFETIME_MESSAGE] = (ta_arena_options_t){256u, 4096u, 4096u, 4096u};
    system.arena = root; system.config = config; system.time_set = 1u; system.now_ns = 100u; system.next_message = 1u;
    limits.delayed_sends = 100u;
    actor1.system = &system; actor1.extra = &extra1; actor1.limits = &limits;
    actor2.system = &system; actor2.extra = &extra2; actor2.limits = &limits;
    actor1.arena = actor2.arena = root;

    send = send_value(10u, "same"); assert(tai_timer_schedule(&actor1, &send, &a) == TA_OK);
    send = send_value(10u, "same"); assert(tai_timer_schedule(&actor1, &send, &b) == TA_OK);
    send = send_value(5u, "early"); assert(tai_timer_schedule(&actor2, &send, &c) == TA_OK);
    assert(system.stats.delayed_sends == 3u && ta_system_next_deadline(&system) == 105u);
    taken = tai_timer_take(&system); assert(taken == c); tai_timer_dispose(taken, TA_OK);
    taken = tai_timer_take(&system); assert(taken == a); tai_timer_dispose(taken, TA_OK);
    taken = tai_timer_take(&system); assert(taken == b); tai_timer_dispose(taken, TA_OK);
    assert(system.stats.delayed_sends == 0u && ta_system_next_deadline(&system) == TA_TIME_NEVER);

    send = send_value(10u, "cancel"); assert(tai_timer_schedule(&actor1, &send, &a) == TA_OK);
    send = send_value(20u, "cancel"); assert(tai_timer_schedule(&actor1, &send, &b) == TA_OK);
    send = send_value(30u, "cancel"); assert(tai_timer_schedule(&actor2, &send, &c) == TA_OK);
    tai_cancel_timers(&actor1); assert(extra1.delayed_count == 0u && extra2.delayed_count == 1u);
    taken = tai_timer_take(&system); assert(taken == c); tai_timer_dispose(taken, TA_CANCELLED);

    limits.delayed_sends = 0u; send = send_value(1u, NULL);
    assert(tai_timer_schedule(&actor1, &send, &a) == TA_LIMIT);
    limits.delayed_sends = 100u; system.now_ns = UINT64_MAX - 1u; send = send_value(2u, NULL);
    assert(tai_timer_schedule(&actor1, &send, &a) == TA_INVALID);
    ta_arena_close(root);
    assert(ta_test_allocator_releases_ok(&allocator));
    ta_test_allocator_reset(&allocator);
    return 0;
}

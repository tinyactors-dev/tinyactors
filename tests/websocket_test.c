#include "host.h"
#include "internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../examples/websocket_connection.c"

typedef struct ws_fixture {
    ta_test_allocator_t allocator;
    ta_arena_t *arena;
    ta_system_t *system;
    ta_definition_t *definition;
    size_t writes;
    size_t closes;
    uint64_t socket;
    ta_request_ref_t retained;
    ta_status_t queue_result;
    ws_host_ops_t ops;
} ws_fixture_t;

static ta_status_t queue_write(void *user, uint64_t socket,
                                const ta_io_request_t *request)
{
    ws_fixture_t *f = (ws_fixture_t *)user;
    assert(request != NULL);
    f->socket = socket;
    f->retained = request->request;
    ++f->writes;
    return f->queue_result;
}

static void close_once(void *user, ta_actor_ref_t actor)
{
    ws_fixture_t *f = (ws_fixture_t *)user;
    assert(actor.id != 0);
    ++f->closes;
}

static void fixture_open(ws_fixture_t *f)
{
    ta_chunk_allocator_t allocator;
    ta_system_config_t config;
    ta_definition_result_t result;
    *f = (ws_fixture_t){0};
    ta_test_allocator_init(&f->allocator, 0, 0);
    allocator = ta_test_allocator_callbacks(&f->allocator);
    assert(ta_arena_create(&allocator, &(ta_arena_options_t){256, 65536, TA_UNLIMITED, 65536}, &f->arena) == TA_OK);
    f->ops = (ws_host_ops_t){f, queue_write, close_once};
    make_connection_config(&config, &f->ops);
    assert(ta_system_create(f->arena, &config, &f->system) == TA_OK);
    assert(ta_definition_build(f->system, &connection_chart, &result) == TA_OK);
    f->definition = result.definition;
}

static ta_run_result_t drive(ws_fixture_t *f, uint64_t now)
{
    ta_run_result_t result;
    assert(drive_once(f->system, now, &result) == TA_OK);
    return result;
}

static void drain_io(ws_fixture_t *f)
{
    const ta_io_request_t *request;
    while (ta_io_next(f->system, &request) == TA_OK)
        assert(ta_io_complete(f->system, request->request, TA_CANCELLED) == TA_OK);
    if (f->retained.id != 0) {
        assert(ta_io_complete(f->system, f->retained, TA_CANCELLED) == TA_OK);
        f->retained = (ta_request_ref_t){0};
    }
}

static ta_actor_ref_t spawn(ws_fixture_t *f, uint64_t socket, uint64_t generation)
{
    connection_state_t state = {socket, generation, 0};
    ws_spawn_input_t input = {f->definition, {&state, sizeof(state)}};
    ta_actor_ref_t actor;
    assert(spawn_connection(f->system, &input, &actor) == TA_OK);
    return actor;
}

static void fixture_close(ws_fixture_t *f, ta_actor_ref_t actor)
{
    tai_actor_t *a = tai_actor_find(f->system, actor);
    if (a != NULL && a->status != TA_ACTOR_DONE && a->status != TA_ACTOR_STOPPED &&
        a->status != TA_ACTOR_FAILED) {
        assert(ta_actor_stop(f->system, actor) == TA_OK);
        (void)drive(f, 40000000000ull);
    }
    drain_io(f);
    a = tai_actor_find(f->system, actor);
    if (a != NULL) assert(ta_actor_release(f->system, actor) == TA_OK);
    ta_definition_release(f->definition);
    { ta_status_t destroyed = ta_system_destroy(f->system);
      if (destroyed != TA_OK) fprintf(stderr, "destroy status=%d actors=%zu io=%zu\n", destroyed, f->system->stats.actors, f->system->stats.io_requests);
      assert(destroyed == TA_OK); }
    ta_arena_close(f->arena);
    assert(ta_test_allocator_releases_ok(&f->allocator));
    ta_test_allocator_reset(&f->allocator);
}

static void connection_lifecycle(void)
{
    ws_fixture_t f;
    ta_actor_ref_t actor;
    ta_actor_info_t info;
    ta_payload_t payload = {0};
    fixture_open(&f);
    f.queue_result = TA_OK;
    actor = spawn(&f, 77, 3);
    (void)drive(&f, 0);
    assert(ta_post(f.system, &(ta_post_options_t){
        .target = {TA_TARGET_INSTANCE, actor, {0}},
        .message = {.name = ta_text_from_cstr("ws.open"), .data = payload}}, NULL) == TA_OK);
    (void)drive(&f, 1);
    assert(ta_actor_inspect(f.system, actor, &info) == TA_OK && info.stable);
    assert(post_update(f.system, actor, payload) == TA_OK);
    (void)drive(&f, 2);
    { ta_status_t dispatched = ws_dispatch_one(f.system, &f.ops);
      if (dispatched != TA_OK) fprintf(stderr, "heartbeat dispatch=%d writes=%zu\n", dispatched, f.writes);
      assert(dispatched == TA_OK); }
    assert(f.writes == 1 && f.socket == 77);
    fixture_close(&f, actor);
}

static void blocked_and_timeout(void)
{
    ws_fixture_t f;
    ta_actor_ref_t actor;
    fixture_open(&f);
    f.queue_result = TA_AGAIN;
    actor = spawn(&f, 88, 4);
    (void)drive(&f, 0);
    assert(ta_post(f.system, &(ta_post_options_t){
        .target = {TA_TARGET_INSTANCE, actor, {0}},
        .message = {.name = ta_text_from_cstr("ws.open")}}, NULL) == TA_OK);
    (void)drive(&f, 1);
    assert(post_update(f.system, actor, (ta_payload_t){0}) == TA_OK);
    (void)drive(&f, 2);
    assert(ws_dispatch_one(f.system, &f.ops) == TA_AGAIN);
    assert(ta_io_complete(f.system, f.retained, TA_OK) == TA_OK);
    f.retained = (ta_request_ref_t){0};
    (void)drive(&f, 3);
    assert(ta_post(f.system, &(ta_post_options_t){
        .target = {TA_TARGET_INSTANCE, actor, {0}},
        .message = {.name = ta_text_from_cstr("ws.drain")}}, NULL) == TA_OK);
    (void)drive(&f, 4);
    f.queue_result = TA_OK;
    (void)drive(&f, 16000000000ull);
    assert(ws_dispatch_one(f.system, &f.ops) == TA_OK);
    assert(f.writes >= 2);
    (void)drive(&f, 40000000000ull);
    assert(f.closes == 1);
    fixture_close(&f, actor);
}

int main(void)
{
    connection_lifecycle();
    blocked_and_timeout();
    return 0;
}

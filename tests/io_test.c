#include "host.h"
#include "../src/internal.h"
#include <assert.h>

int main(void)
{
    ta_test_allocator_t allocator;
    ta_chunk_allocator_t callbacks;
    ta_arena_options_t options = {512, 65536, (size_t)-1, 65536};
    ta_arena_t *root, *arena;
    ta_system_t *system;
    ta_system_config_t config;
    tai_actor_t actor = {0};
    tai_io_t *io;
    const ta_io_request_t *leased;
    ta_io_request_t request = {0};
    ta_status_t status;
    ta_request_ref_t request_ref;
    ta_test_allocator_init(&allocator, 0, 0);
    callbacks = ta_test_allocator_callbacks(&allocator);
    assert(ta_arena_create(&callbacks, &options, &root) == TA_OK);
    ta_system_config_init(&config); config.session_prefix = ta_text_from_cstr("test"); config.data_model = ta_text_from_cstr("bytes");
    assert(ta_system_create(root, &config, &system) == TA_OK);
    actor.system = system; actor.arena = system->arena; actor.id = 7; actor.status = TA_ACTOR_READY; actor.limits = &system->config.actor_defaults;
    tai_actor_insert(system,&actor);
    request.kind = TA_IO_SEND; request.type = ta_text_from_cstr("urn:test"); request.target = ta_text_from_cstr("dest");
    request.body.kind = TA_BODY_CONTENT; request.body.content = (ta_payload_t){0};
    status = tai_io_open(&actor, &request, &io); assert(status == TA_OK && io != NULL);
    tai_io_expose(io); assert(ta_io_next(system, &leased) == TA_OK && leased == &io->request);
    request_ref=leased->request;
    assert(ta_io_complete(system, request_ref, TA_OK) == TA_OK);
    assert(ta_io_complete(system, request_ref, TA_OK) == TA_STALE);
    assert(actor.extra->request_count==0 && actor.extra->request_bytes==0);
    assert(ta_io_next(system, &leased) == TA_EMPTY && leased == NULL);
    (void)arena;
    tai_actor_remove(system,&actor);
    assert(ta_system_destroy(system) == TA_OK);
    ta_arena_close(root);
    assert(ta_test_allocator_releases_ok(&allocator)); ta_test_allocator_reset(&allocator);
    return 0;
}

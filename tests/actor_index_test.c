#include "internal.h"
#include <assert.h>
#include <stdlib.h>

int main(void)
{
    ta_system_t system = {0};
    tai_actor_t *actors = (tai_actor_t *)calloc(10000, sizeof(*actors));
    ta_target_t target;
    ta_actor_ref_t ref;
    size_t i;
    system.config.session_prefix = (ta_text_t){"test", 4};
    assert(actors != NULL);
    for (i = 0; i < 10000; ++i) {
        actors[i] = (tai_actor_t){0}; actors[i].system = &system; actors[i].id = (uint64_t)(i + 1);
        actors[i].status = TA_ACTOR_READY; tai_actor_insert(&system, &actors[i]);
    }
    for (i = 0; i < 10000; ++i) assert(tai_actor_find(&system, (ta_actor_ref_t){i + 1}) == &actors[i]);
    { size_t count = 0; tai_actor_t *at = system.actors, *prev = NULL;
      while (at != NULL) { assert(at->previous == prev); prev = at; at = at->next; ++count; }
      assert(count == 10000); }
    tai_actor_remove(&system, &actors[0]);
    tai_actor_remove(&system, &actors[5000]);
    tai_actor_remove(&system, &actors[9999]);
    { size_t count = 0; tai_actor_t *at = system.actors;
      while (at != NULL) { ++count; at = at->next; }
      assert(count == 9997); }
    tai_ready(&actors[10]); tai_ready(&actors[20]); tai_ready(&actors[10]);
    assert(system.ready_head == &actors[10] && system.ready_tail == &actors[20]);
    assert(system.stats.runnable_actors == 2); tai_unready(&actors[10]); tai_unready(&actors[10]);
    assert(system.ready_head == &actors[20] && system.stats.runnable_actors == 1);
    assert(ta_target_parse((ta_text_t){"#_internal", 10}, &target) == TA_OK && target.kind == TA_TARGET_INTERNAL);
    assert(ta_target_parse(ta_text_from_cstr("#_scxml_test.1"), &target) == TA_OK && target.kind == TA_TARGET_URI);
    assert(ta_target_parse(ta_text_from_cstr("#__invoke"), &target) == TA_OK && target.kind == TA_TARGET_INVOKE);
    assert(ta_target_parse((ta_text_t){"#_invoke", 8}, &target) == TA_OK && target.kind == TA_TARGET_INVOKE);
    assert(ta_target_parse((ta_text_t){NULL, 0}, &target) == TA_OK && target.kind == TA_TARGET_SELF);
    ref = (ta_actor_ref_t){0}; assert(ta_session_lookup(&system, (ta_text_t){"test.1", 6}, &ref) == TA_STALE);
    while (system.actor_index != NULL) tai_actor_remove(&system, system.actor_index);
    assert(system.actor_index == NULL && system.actors == NULL);
    free(actors);
    return 0;
}

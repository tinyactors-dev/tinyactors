#include "../src/internal.h"
#include <assert.h>
#include "host.h"

static unsigned ran;
static unsigned roots;
static ta_status_t run_action(ta_context_t *c, ta_action_kind_t k, ta_code_t code)
{ (void)c; (void)k; ran += (unsigned)code; return code == 99 ? TA_EXECUTION_ERROR : TA_OK; }
static ta_status_t test_true(ta_context_t *c, ta_code_t code, uint8_t *out)
{ (void)c; *out = (uint8_t)(code != 0u); return TA_OK; }
static ta_status_t open_iter(ta_context_t *c, ta_code_t code, ta_iteration_t *out)
{ (void)c; out->snapshot.handle = 1u; ++roots; out->count = (size_t)code; return TA_OK; }
static void drop(void *user, ta_value_t value) { (void)user; assert(value.handle==1); assert(roots!=0); --roots; }
static ta_status_t bind_iter(ta_context_t *c, const ta_iteration_step_t *step)
{ (void)c; ran += (unsigned)(step->ordinal + 1u); return TA_OK; }

static int blocks_and_foreach(void)
{
    ta_system_t sys = {0}; tai_actor_t actor = {0}; ta_context_t context = {0};
    ta_test_allocator_t backing; ta_chunk_allocator_t callbacks; ta_arena_t *arena;
    ta_action_t custom = {.kind = TA_ACTION_CUSTOM, .as.custom.code = 3u};
    ta_branch_t branch = {.condition = 1u, .body = {&custom, 1u}};
    ta_action_t conditional = {.kind = TA_ACTION_IF, .as.conditional = {&branch, 1u}};
    ta_action_t loop = {.kind = TA_ACTION_FOREACH, .as.foreach_loop = {2u, {"x",1u}, {0}, {&custom,1u}}};
    ta_action_t actions[] = {conditional, loop, custom}; ta_block_t block = {actions, 3u};
    ta_test_allocator_init(&backing,0,0); callbacks=ta_test_allocator_callbacks(&backing);
    assert(ta_arena_create(&callbacks,&(ta_arena_options_t){256,65536,TA_UNLIMITED,65536},&arena)==TA_OK);
    ta_system_config_init(&sys.config); sys.arena=arena;
    sys.config.limits.action_depth = 8u; sys.config.data.run = run_action;
    sys.config.data.test = test_true; sys.config.data.iteration_open = open_iter;
    sys.config.data.iteration_bind = bind_iter; sys.config.values.release=drop;
    actor.system = &sys; actor.limits = &sys.config.actor_defaults; actor.arena=arena;
    context.actor = &actor; ran = 0u;
    assert(tai_execute(&context, &block) == TA_OK); assert(ran == 15u && roots==0);
    actor.actions=0; sys.config.actor_defaults.actions_per_macrostep=4;
    assert(tai_execute(&context,&block)==TA_FAULTED && roots==0);
    actor.actions=0; sys.config.actor_defaults.actions_per_macrostep=100;
    custom.as.custom.code=99;
    assert(tai_execute(&context,&(ta_block_t){&loop,1})==TA_EXECUTION_ERROR && roots==0);
    ta_arena_close(arena); assert(ta_test_allocator_releases_ok(&backing)); ta_test_allocator_reset(&backing);
    return 0;
}

int main(void) { return blocks_and_foreach(); }

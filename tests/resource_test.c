#include "internal.h"
#include "host.h"
#include <assert.h>

typedef struct fixture {
    ta_test_allocator_t host;
    ta_arena_t *root;
    ta_system_t *system;
    ta_definition_t *definition;
    ta_actor_ref_t actor;
    size_t callbacks;
} fixture_t;

static ta_status_t action(ta_context_t *context, ta_action_kind_t kind, ta_code_t code)
{
    fixture_t *f=ta_context_host_user(context);
    (void)kind;++f->callbacks;return code==1?TA_NO_MEMORY:TA_OK;
}

static void setup(fixture_t *f, const ta_chart_state_t *states)
{
    ta_system_config_t config;
    ta_chunk_allocator_t allocator;
    ta_definition_result_t result;
    ta_chart_t chart={.name="resources",.data_model="bytes",.states=states,
        .data_layout={.storage=TA_DATA_BYTES,.bytes_layout={0,1}}};
    *f=(fixture_t){0};ta_test_allocator_init(&f->host,0,0);allocator=ta_test_allocator_callbacks(&f->host);
    assert(ta_arena_create(&allocator,&(ta_arena_options_t){256,65536,TA_UNLIMITED,65536},&f->root)==TA_OK);
    ta_system_config_init(&config);config.session_prefix=ta_text_from_cstr("limits");config.data_model=ta_text_from_cstr("bytes");
    config.host_user=f;config.data.run=action;config.actor_defaults.internal_events=0;config.actor_defaults.microsteps_per_macrostep=3;
    assert(ta_system_create(f->root,&config,&f->system)==TA_OK);
    assert(ta_definition_build(f->system,&chart,&result)==TA_OK);f->definition=result.definition;
    assert(ta_actor_spawn(f->system,&(ta_spawn_options_t){.definition=f->definition},&f->actor)==TA_OK);
}

static void run(fixture_t *f, size_t units)
{
    ta_run_result_t result;
    assert(ta_system_run(f->system,&(ta_run_options_t){0,{units,1}},&result)==TA_OK && result.work_units<=units);
}

static void cleanup(fixture_t *f)
{
    assert(ta_actor_stop(f->system,f->actor)==TA_OK);run(f,20);
    assert(ta_actor_release(f->system,f->actor)==TA_OK);ta_definition_release(f->definition);
    assert(ta_system_destroy(f->system)==TA_OK);ta_arena_close(f->root);
    assert(ta_test_allocator_releases_ok(&f->host));ta_test_allocator_reset(&f->host);
}

static void reserved_slot(uint8_t twice)
{
    fixture_t f;
    ta_action_t error={.kind=TA_ACTION_CUSTOM,.as.custom={1}};
    ta_block_t blocks[]={{&error,1},{&error,1}};
    ta_chart_state_t states[]={
        {.id="a",.onentry={blocks,twice?2u:1u},.transitions=(ta_chart_transition_t[]){
            {.event="error.platform.memory",.target="handled"},{0}}},
        {.id="handled"},{0}};
    tai_actor_t *actor;
    setup(&f,states);run(&f,1);actor=tai_actor_find(f.system,f.actor);
    if(twice)assert(actor->status==TA_ACTOR_FAILED && f.callbacks==2);
    else {
        ta_actor_limits_t limits=*actor->limits;
        assert(actor->internal_count==1 && actor->error_slot==1 && actor->limits->internal_events==0);
        assert(ta_actor_set_limits(f.system,f.actor,&limits)==TA_OK);
        run(&f,20);assert(actor->status==TA_ACTOR_READY && actor->stable && tai_active(actor,2));
    }
    cleanup(&f);
}

static void livelock_ceiling(void)
{
    fixture_t f;
    ta_action_t count={.kind=TA_ACTION_CUSTOM,.as.custom={2}};
    ta_chart_state_t states[]={{.id="loop",.transitions=(ta_chart_transition_t[]){
        {.target="loop",.content={&count,1}},{0}}},{0}};
    tai_actor_t *actor;
    setup(&f,states);run(&f,1);actor=tai_actor_find(f.system,f.actor);
    run(&f,1);run(&f,1);assert(actor->status==TA_ACTOR_SUSPENDED && f.callbacks==2);
    run(&f,10);assert(actor->status==TA_ACTOR_FAILED && actor->reason==TA_LIMIT && f.callbacks==3);
    cleanup(&f);
}

int main(void)
{
    reserved_slot(0);reserved_slot(1);livelock_ceiling();return 0;
}

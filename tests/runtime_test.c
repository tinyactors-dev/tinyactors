#include "internal.h"
#include "host.h"
#include <assert.h>
#include <stdio.h>

typedef struct fixture {
    ta_test_allocator_t allocator;
    ta_arena_t *arena;
    ta_system_t *system;
    ta_definition_t *definition;
    unsigned log[64];
    size_t count;
} fixture_t;

static ta_status_t action(ta_context_t *context, ta_action_kind_t kind, ta_code_t code)
{
    fixture_t *f = ta_context_host_user(context);
    ta_mut_bytes_t bytes;
    unsigned value;
    (void)kind;
    assert(f->count < 64);
    f->log[f->count++] = (unsigned)code;
    assert(ta_context_bytes(context, &bytes) == TA_OK);
    tai_copy(&value, bytes.data, sizeof(value));
    ++value;
    tai_copy(bytes.data, &value, sizeof(value));
    return code == 99 ? TA_EXECUTION_ERROR : TA_OK;
}

static void open_fixture(fixture_t *f, const ta_chart_t *chart)
{
    ta_chunk_allocator_t allocator;
    ta_system_config_t config;
    ta_definition_result_t result;
    *f = (fixture_t){0};
    ta_test_allocator_init(&f->allocator, 0, 0);
    allocator = ta_test_allocator_callbacks(&f->allocator);
    assert(ta_arena_create(&allocator, &(ta_arena_options_t){256,65536,TA_UNLIMITED,65536}, &f->arena) == TA_OK);
    ta_system_config_init(&config);
    config.session_prefix = ta_text_from_cstr("runtime");
    config.data_model = ta_text_from_cstr("bytes");
    config.host_user = f;
    config.data.run = action;
    assert(ta_system_create(f->arena, &config, &f->system) == TA_OK);
    { ta_status_t status=ta_definition_build(f->system,chart,&result);
      if(status!=TA_OK)fprintf(stderr,"build: %d %.*s\n",status,(int)result.error.detail.size,result.error.detail.data);
      assert(status==TA_OK); }
    f->definition = result.definition;
}

static ta_run_result_t run(fixture_t *f, size_t units)
{
    ta_run_result_t result;
    assert(ta_system_run(f->system, &(ta_run_options_t){1,{units,1}}, &result) == TA_OK);
    return result;
}

static ta_actor_ref_t spawn(fixture_t *f)
{
    ta_actor_ref_t actor;
    assert(ta_actor_spawn(f->system, &(ta_spawn_options_t){.definition=f->definition}, &actor) == TA_OK);
    return actor;
}

static void post(fixture_t *f, ta_actor_ref_t actor, const char *name)
{
    ta_post_options_t input = {.target={.kind=TA_TARGET_INSTANCE,.instance=actor},
        .message={.name=ta_text_from_cstr(name)}};
    assert(ta_post(f->system, &input, NULL) == TA_OK);
}

static void close_fixture(fixture_t *f, ta_actor_ref_t actor)
{
    assert(ta_actor_stop(f->system, actor) == TA_OK);
    (void)run(f, 100);
    assert(ta_actor_release(f->system, actor) == TA_OK);
    ta_definition_release(f->definition);
    assert(ta_system_destroy(f->system) == TA_OK);
    ta_arena_close(f->arena);
    assert(ta_test_allocator_releases_ok(&f->allocator));
    ta_test_allocator_reset(&f->allocator);
}

static void run_to_completion(void)
{
    ta_action_t exit_a={.kind=TA_ACTION_CUSTOM,.as.custom={1}};
    ta_action_t to_b[]={ {.kind=TA_ACTION_CUSTOM,.as.custom={2}},
        {.kind=TA_ACTION_RAISE,.as.raise={{"inside",6}}} };
    ta_action_t enter_b={.kind=TA_ACTION_CUSTOM,.as.custom={3}};
    ta_action_t to_c={.kind=TA_ACTION_CUSTOM,.as.custom={4}};
    ta_action_t enter_c={.kind=TA_ACTION_CUSTOM,.as.custom={5}};
    ta_action_t to_final={.kind=TA_ACTION_CUSTOM,.as.custom={6}};
    ta_action_t enter_final={.kind=TA_ACTION_CUSTOM,.as.custom={7}};
    ta_action_t exit_final={.kind=TA_ACTION_CUSTOM,.as.custom={8}};
    ta_chart_t chart={.name="rtc",.data_model="bytes",
        .data_layout={.storage=TA_DATA_BYTES,.bytes_layout={sizeof(unsigned),1}},
        .states=(ta_chart_state_t[]){
            {.id="a",.onexit={(ta_block_t[]){{&exit_a,1}},1},
             .transitions=(ta_chart_transition_t[]){{.event="tick",.target="b",.content={to_b,2}},{0}}},
            {.id="b",.onentry={(ta_block_t[]){{&enter_b,1}},1},
             .transitions=(ta_chart_transition_t[]){{.target="c",.content={&to_c,1}},{0}}},
            {.id="c",.onentry={(ta_block_t[]){{&enter_c,1}},1},
             .transitions=(ta_chart_transition_t[]){{.event="inside",.target="finished",.content={&to_final,1}},{0}}},
            {.id="finished",.kind=TA_CHART_FINAL,.onentry={(ta_block_t[]){{&enter_final,1}},1},.onexit={(ta_block_t[]){{&exit_final,1}},1}},
            {0}}};
    fixture_t f;
    ta_actor_ref_t ref;
    tai_actor_t *actor;
    size_t i;
    open_fixture(&f,&chart);
    ref=spawn(&f); actor=tai_actor_find(f.system,ref);
    assert(run(&f,2).runnable==0 && actor->stable && tai_active(actor,1));
    assert(actor->extra==NULL); /* Empty feature sets must stay lightweight. */
    post(&f,ref,"tick"); post(&f,ref,"later");
    (void)run(&f,1);
    assert(tai_active(actor,2) && !actor->stable && actor->internal_count==1 && actor->external_count==1);
    (void)run(&f,1);
    assert(tai_active(actor,3) && actor->internal_count==1);
    assert(tai_text_compare(actor->current->event.name,ta_text_from_cstr("tick"))==0);
    (void)run(&f,1);
    assert(actor->status==TA_ACTOR_DONE && actor->external_count==1 && f.count==8);
    for(i=0;i<8;++i) assert(f.log[i]==i+1);
    close_fixture(&f,ref);
}

static void error_boundaries(void)
{
    ta_action_t first[]={{.kind=TA_ACTION_CUSTOM,.as.custom={99}}, {.kind=TA_ACTION_CUSTOM,.as.custom={100}}};
    ta_action_t second={.kind=TA_ACTION_CUSTOM,.as.custom={101}};
    ta_chart_t chart={.name="errors",.data_model="bytes",
        .data_layout={.storage=TA_DATA_BYTES,.bytes_layout={sizeof(unsigned),1}},
        .states=(ta_chart_state_t[]){
          {.id="a",.onentry={(ta_block_t[]){{first,2},{&second,1}},2},
           .transitions=(ta_chart_transition_t[]){{.event="error.execution",.target="handled"},{0}}},
          {.id="handled"},{0}}};
    fixture_t f;
    ta_actor_ref_t ref;
    open_fixture(&f,&chart); ref=spawn(&f);
    assert(run(&f,30).runnable==0);
    assert(f.count==2 && f.log[0]==99 && f.log[1]==101);
    assert(tai_active(tai_actor_find(f.system,ref),2));
    close_fixture(&f,ref);
}

int main(void)
{
    run_to_completion();
    error_boundaries();
    return 0;
}

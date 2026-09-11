#include "../src/internal.h"
#include "host.h"
#include <assert.h>
#include <stdio.h>

typedef struct sem_fixture { ta_test_allocator_t host; ta_arena_t *root; ta_system_t *system; ta_definition_t *definition; unsigned log[32]; size_t count; } sem_fixture_t;

static ta_status_t sem_action(ta_context_t *c, ta_action_kind_t k, ta_code_t code)
{
    sem_fixture_t *f=(sem_fixture_t *)ta_context_host_user(c); (void)k;
    assert(f->count<32); f->log[f->count++]=(unsigned)code; return TA_OK;
}
static void sem_open(sem_fixture_t *f, const ta_chart_t *chart)
{
    ta_chunk_allocator_t cb; ta_system_config_t cfg; ta_definition_result_t r;
    *f=(sem_fixture_t){0}; ta_test_allocator_init(&f->host,0,0); cb=ta_test_allocator_callbacks(&f->host);
    assert(ta_arena_create(&cb,&(ta_arena_options_t){256,65536,TA_UNLIMITED,65536},&f->root)==TA_OK);
    ta_system_config_init(&cfg); cfg.session_prefix=ta_text_from_cstr("sem"); cfg.data_model=ta_text_from_cstr("bytes"); cfg.host_user=f; cfg.data.run=sem_action;
    assert(ta_system_create(f->root,&cfg,&f->system)==TA_OK); { ta_status_t s=ta_definition_build(f->system,chart,&r); if(s!=TA_OK) fprintf(stderr,"semantics definition status=%d detail=%.*s\n",s,(int)r.error.detail.size,r.error.detail.data); assert(s==TA_OK); } f->definition=r.definition;
}
static ta_actor_ref_t sem_spawn(sem_fixture_t *f)
{ ta_actor_ref_t r; assert(ta_actor_spawn(f->system,&(ta_spawn_options_t){.definition=f->definition},&r)==TA_OK); return r; }
static void sem_run(sem_fixture_t *f, size_t units)
{ ta_run_result_t r; assert(ta_system_run(f->system,&(ta_run_options_t){1,{units,1}},&r)==TA_OK); }
static void sem_post(sem_fixture_t *f, ta_actor_ref_t a, const char *name)
{ assert(ta_post(f->system,&(ta_post_options_t){.target={.kind=TA_TARGET_INSTANCE,.instance=a},.message={.name=ta_text_from_cstr(name)}},NULL)==TA_OK); }
static void sem_close(sem_fixture_t *f, ta_actor_ref_t a)
{
    assert(ta_actor_stop(f->system,a)==TA_OK); sem_run(f,100); assert(ta_actor_release(f->system,a)==TA_OK); ta_definition_release(f->definition);
    assert(ta_system_destroy(f->system)==TA_OK); ta_arena_close(f->root); assert(ta_test_allocator_releases_ok(&f->host)); ta_test_allocator_reset(&f->host);
}

static void hierarchical(void)
{
    ta_chart_state_t kids[]={{.id="left"},{.id="right"},{0}};
    ta_chart_transition_t parent_tr[]={{.event="leave",.target="outside",.content={(ta_action_t[]){{.kind=TA_ACTION_CUSTOM,.as.custom={7}}},1}},{0}};
    ta_chart_state_t states[]={{.id="parent",.states=kids,.transitions=parent_tr},{.id="outside"},{0}};
    ta_chart_t chart={.name="hier",.data_model="bytes",.data_layout={.storage=TA_DATA_BYTES,.bytes_layout={1,1}},.states=states,.initial={.target="parent"}}; sem_fixture_t f; ta_actor_ref_t a; tai_actor_t *x;
    sem_open(&f,&chart); a=sem_spawn(&f); sem_run(&f,20); x=tai_actor_find(f.system,a); assert(tai_active(x,1)&&tai_active(x,2));
    sem_post(&f,a,"leave"); sem_run(&f,20); assert(tai_active(x,4)&&!tai_active(x,1)); assert(f.count==1&&f.log[0]==7); sem_close(&f,a);
}

static void parallel(void)
{
    ta_chart_transition_t r1t[]={{.event="one",.target="r1b"},{.event="tick",.target="r1b"},{0}}, r2t[]={{.event="two",.target="r2b"},{.event="tick",.target="r2b"},{0}};
    ta_chart_state_t r1[]={{.id="r1a",.transitions=r1t},{.id="r1b"},{0}}, r2[]={{.id="r2a",.transitions=r2t},{.id="r2b"},{0}};
    ta_chart_state_t states[]={{.id="p",.kind=TA_CHART_PARALLEL,.states=(ta_chart_state_t[]){{.id="r1",.states=r1},{.id="r2",.states=r2},{0}}},{0}};
    ta_chart_t chart={.name="par",.data_model="bytes",.data_layout={.storage=TA_DATA_BYTES,.bytes_layout={1,1}},.states=states}; sem_fixture_t f; ta_actor_ref_t a; tai_actor_t *x;
    sem_open(&f,&chart); a=sem_spawn(&f); sem_run(&f,20); x=tai_actor_find(f.system,a); assert(tai_active(x,3)&&tai_active(x,6));
    sem_post(&f,a,"tick"); sem_run(&f,40); assert(tai_active(x,4)&&tai_active(x,7)); sem_close(&f,a);
}

static void internal_external(void)
{
    ta_action_t enter={.kind=TA_ACTION_CUSTOM,.as.custom={1}}, exit={.kind=TA_ACTION_CUSTOM,.as.custom={2}};
    ta_chart_transition_t tr[]={{.kind=TA_TRANSITION_INTERNAL,.event="internal",.target="right"},{.kind=TA_TRANSITION_EXTERNAL,.event="external",.target="left"},{0}};
    ta_chart_state_t kids[]={{.id="left"},{.id="right"},{0}};
    ta_chart_state_t states[]={{.id="parent",.states=kids,.onentry={(ta_block_t[]){{&enter,1}},1},.onexit={(ta_block_t[]){{&exit,1}},1},.transitions=tr},{.id="outside"},{0}};
    ta_chart_t chart={.name="int-ext",.data_model="bytes",.data_layout={.storage=TA_DATA_BYTES,.bytes_layout={1,1}},.states=states,.initial={.target="parent"}};
    sem_fixture_t f; ta_actor_ref_t a; tai_actor_t *x; sem_open(&f,&chart); a=sem_spawn(&f); sem_run(&f,20); x=tai_actor_find(f.system,a);
    assert(f.count==1&&f.log[0]==1&&tai_active(x,1)&&tai_active(x,2)); sem_post(&f,a,"internal"); sem_run(&f,20); assert(f.count==1&&tai_active(x,3));
    sem_post(&f,a,"external"); sem_run(&f,20); assert(f.count==3&&f.log[1]==2&&f.log[2]==1&&tai_active(x,2)); sem_close(&f,a);
}

static void history(void)
{
    ta_chart_state_t hist={.id="memory",.kind=TA_CHART_HISTORY_SHALLOW,.initial={.target="x"}};
    ta_chart_transition_t xtr[]={{.event="advance",.target="y"},{0}}, outtr[]={{.event="out",.target="away"},{0}}, backtr[]={{.event="back",.target="memory"},{0}};
    ta_chart_state_t boxkids[]={{.id="x",.transitions=xtr},{.id="y"},hist,{0}};
    ta_chart_state_t states[]={{.id="box",.states=boxkids,.initial={.target="x"},.transitions=outtr},{.id="away",.transitions=backtr},{0}};
    ta_chart_t chart={.name="hist",.data_model="bytes",.data_layout={.storage=TA_DATA_BYTES,.bytes_layout={1,1}},.states=states,.initial={.target="box"}}; sem_fixture_t f; ta_actor_ref_t a; tai_actor_t *x;
    sem_open(&f,&chart); a=sem_spawn(&f); sem_run(&f,20); x=tai_actor_find(f.system,a); assert(tai_active(x,2)); sem_post(&f,a,"advance"); sem_run(&f,20); assert(tai_active(x,3)); sem_post(&f,a,"out"); sem_run(&f,20); assert(tai_active(x,5)); sem_post(&f,a,"back"); sem_run(&f,30); assert(tai_active(x,3)); sem_close(&f,a);
}

static void nested_history(uint8_t deep)
{
    ta_chart_state_t children[]={
        {.id="inner",.states=(ta_chart_state_t[]){
            {.id="one",.transitions=(ta_chart_transition_t[]){{.event="advance",.target="two"},{0}}},
            {.id="two"},{0}}},
        {.id="h",.kind=deep?TA_CHART_HISTORY_DEEP:TA_CHART_HISTORY_SHALLOW,.initial={.target="inner"}},{0}};
    ta_chart_state_t states[]={
        {.id="box",.states=children,.transitions=(ta_chart_transition_t[]){{.event="out",.target="away"},{0}}},
        {.id="away",.transitions=(ta_chart_transition_t[]){{.event="back",.target="h"},{0}}},{0}};
    ta_chart_t chart={.name="nested-history",.data_model="bytes",.states=states,
        .data_layout={.storage=TA_DATA_BYTES,.bytes_layout={0,1}}};
    sem_fixture_t f;ta_actor_ref_t actor;tai_actor_t *a;
    sem_open(&f,&chart);actor=sem_spawn(&f);sem_run(&f,50);a=tai_actor_find(f.system,actor);
    sem_post(&f,actor,"advance");sem_run(&f,50);assert(tai_active(a,4));
    sem_post(&f,actor,"out");sem_run(&f,50);assert(tai_active(a,6));
    sem_post(&f,actor,"back");sem_run(&f,50);assert(tai_active(a,deep?4:3));
    sem_close(&f,actor);
}

static void nested_completion(void)
{
    ta_chart_t chart={.name="completion",.data_model="bytes",
        .data_layout={.storage=TA_DATA_BYTES,.bytes_layout={0,1}},
        .states=(ta_chart_state_t[]){
            {.id="p",.kind=TA_CHART_PARALLEL,.transitions=(ta_chart_transition_t[]){{.event="done.state.p",.target="outside"},{0}},
             .states=(ta_chart_state_t[]){
                {.id="r1",.transitions=(ta_chart_transition_t[]){{.event="advance",.target="r1done"},{0}},
                 .states=(ta_chart_state_t[]){
                    {.id="inner",.states=(ta_chart_state_t[]){{.id="innerdone",.kind=TA_CHART_FINAL},{0}}},
                    {.id="r1done",.kind=TA_CHART_FINAL},{0}}},
                {.id="r2",.states=(ta_chart_state_t[]){{.id="r2done",.kind=TA_CHART_FINAL},{0}}},{0}}},
            {.id="outside"},{0}}};
    sem_fixture_t f;ta_actor_ref_t actor;tai_actor_t *a;
    sem_open(&f,&chart);actor=sem_spawn(&f);sem_run(&f,100);a=tai_actor_find(f.system,actor);
    assert(tai_active(a,1) && tai_active(a,4) && !tai_active(a,8));
    sem_post(&f,actor,"advance");sem_run(&f,100);assert(tai_active(a,8));
    sem_close(&f,actor);
}

int main(void)
{
    hierarchical();parallel();internal_external();history();nested_history(0);nested_history(1);nested_completion();return 0;
}

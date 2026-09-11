#include "internal.h"
#include "host.h"
#include <assert.h>

typedef struct fixture {
    ta_test_allocator_t host;
    ta_arena_t *arena;
    ta_system_t *system;
    ta_definition_t *old, *next;
    ta_actor_ref_t ref;
    uint8_t fail_binding;
    size_t binds;
} fixture_t;

static ta_status_t bind(ta_context_t *context, const ta_system_values_t *values)
{
    fixture_t *f=ta_context_host_user(context);
    ta_context_view_t view;
    ta_context_inspect(context,&view);
    assert(tai_text_compare(values->name,ta_text_from_cstr("original"))==0);
    assert(values->ioprocessor_count==1);
    if(view.phase==TA_PHASE_MIGRATE) {
        ++f->binds;
        assert(ta_actor_stop(f->system,f->ref)==TA_BUSY);
        assert(ta_context_in(context,ta_text_from_cstr("away")));
        if(f->fail_binding)return TA_EXECUTION_ERROR;
    }
    return TA_OK;
}

static void run(fixture_t *f)
{
    ta_run_result_t result;
    assert(ta_system_run(f->system,&(ta_run_options_t){1,{100,1}},&result)==TA_OK);
    assert(result.runnable==0);
}

static void post(fixture_t *f, const char *event)
{
    assert(ta_post(f->system,&(ta_post_options_t){.target={.kind=TA_TARGET_INSTANCE,.instance=f->ref},
        .message={.name=ta_text_from_cstr(event)}},NULL)==TA_OK);
}

static ta_definition_t *build(fixture_t *f, uint8_t replacement)
{
    ta_chart_state_t children[]={
        {.id="a",.transitions=(ta_chart_transition_t[]){{.event="advance",.target="b"},{0}}},
        {.id="b"},{.id="h",.kind=TA_CHART_HISTORY_DEEP,.initial={.target="a"}},{0}};
    ta_chart_state_t states[]={
        {.id="box",.states=children,.transitions=(ta_chart_transition_t[]){{.event="out",.target="away"},{0}}},
        {.id="away",.transitions=(ta_chart_transition_t[]){{.event="back",.target="h"},{0}}},
        {.id="unused"},{0}};
    ta_chart_t chart={.name=replacement?"replacement":"original",.data_model="bytes",.binding=TA_BINDING_LATE,
        .data_layout={.storage=TA_DATA_BYTES,.bytes_layout={sizeof(unsigned),sizeof(unsigned)},.schema={"v1",2}},
        .initial={.target="box"},.states=states};
    ta_definition_result_t result;
    if(replacement)states[2].states=(ta_chart_state_t[]){{.id="new-inactive"},{0}};
    assert(ta_definition_build(f->system,&chart,&result)==TA_OK);
    return result.definition;
}

static void open_fixture(fixture_t *f)
{
    ta_chunk_allocator_t allocator;
    ta_system_config_t config;
    ta_actor_limits_t limits;
    unsigned initial=42;
    *f=(fixture_t){0};ta_test_allocator_init(&f->host,0,0);allocator=ta_test_allocator_callbacks(&f->host);
    assert(ta_arena_create(&allocator,&(ta_arena_options_t){256,65536,TA_UNLIMITED,65536},&f->arena)==TA_OK);
    ta_system_config_init(&config);config.session_prefix=ta_text_from_cstr("up");config.data_model=ta_text_from_cstr("bytes");
    config.host_user=f;config.data.bind_system=bind;
    assert(ta_system_create(f->arena,&config,&f->system)==TA_OK);
    f->old=build(f,0);f->next=build(f,1);
    assert(ta_actor_spawn(f->system,&(ta_spawn_options_t){.definition=f->old,.initial_bytes={&initial,sizeof(initial)}},&f->ref)==TA_OK);
    run(f);post(f,"advance");run(f);post(f,"out");run(f);
    limits=*tai_actor_find(f->system,f->ref)->limits;limits.external_events=16;
    assert(ta_actor_set_limits(f->system,f->ref,&limits)==TA_OK);
    assert(ta_name_bind(f->system,&(ta_name_binding_t){{"entity",6},{0}},f->ref)==TA_OK);
}

static void close_fixture(fixture_t *f)
{
    assert(ta_actor_stop(f->system,f->ref)==TA_OK);run(f);assert(ta_actor_release(f->system,f->ref)==TA_OK);
    ta_definition_release(f->old);ta_definition_release(f->next);assert(ta_system_destroy(f->system)==TA_OK);
    ta_arena_close(f->arena);assert(ta_test_allocator_releases_ok(&f->host));ta_test_allocator_reset(&f->host);
}

static void assert_old(fixture_t *f, size_t bytes, const void *state)
{
    tai_actor_t *a=tai_actor_find(f->system,f->ref);
    assert(a->definition==f->old && a->stable && a->data.as.bytes.data==state);
    assert(f->host.live_bytes==bytes && a->external_count==1);
    assert(a->extra->history!=NULL && a->extra->history->targets[0]==3);
    assert(tai_text_compare(a->current->event.name,ta_text_from_cstr("out"))==0);
    assert(a->limits->external_events==16);
}

static void rollback_and_repetition(void)
{
    fixture_t f;
    tai_actor_t *a;
    ta_upgrade_options_t options;
    size_t baseline, offset, plateau[2]={0}, i;
    const void *state;
    ta_status_t status=TA_OK;
    ta_actor_ref_t named;
    open_fixture(&f);a=tai_actor_find(f.system,f.ref);post(&f,"queued");
    baseline=f.host.live_bytes;state=a->data.as.bytes.data;
    options=(ta_upgrade_options_t){.actor=f.ref,.replacement=f.next,.expected_revision=TA_REVISION_ANY};
    f.fail_binding=1;
    assert(ta_actor_upgrade(f.system,&options)==TA_EXECUTION_ERROR);assert_old(&f,baseline,state);
    f.fail_binding=0;
    for(offset=1;offset<50;++offset) {
        f.host.fail_acquire=f.host.acquire_count+offset;
        status=ta_actor_upgrade(f.system,&options);
        f.host.fail_acquire=0;
        if(status==TA_OK)break;
        assert(status==TA_NO_MEMORY);assert_old(&f,baseline,state);
    }
    assert(status==TA_OK && offset>3 && offset<50);
    assert(a->definition==f.next && a->external_count==1 && !a->stable);
    assert(*(const unsigned *)a->data.as.bytes.data==42 && a->limits->external_events==16);
    assert(a->identity_definition==f.old);
    assert(ta_name_lookup(f.system,ta_text_from_cstr("entity"),&named)==TA_OK && named.id==f.ref.id);
    run(&f);
    for(i=0;i<24;++i) {
        options.replacement=(i%2==0)?f.old:f.next;
        assert(ta_actor_upgrade(f.system,&options)==TA_OK);run(&f);
        if(i<2)plateau[i]=f.host.live_bytes;else assert(f.host.live_bytes==plateau[i%2]);
        assert(a->limits->external_events==16 && *(const unsigned *)a->data.as.bytes.data==42);
    }
    post(&f,"back");run(&f);assert(tai_active(a,3));
    post(&f,"out");run(&f); /* Replacing a mapped history must not close all candidate tables. */
    assert(a->extra->history->targets[0]==3);
    close_fixture(&f);
}

static void incompatible_active_topology(void)
{
    fixture_t f;
    ta_definition_result_t result;
    ta_chart_t chart={.name="invalid-topology",.data_model="bytes",.initial={.target="away"},
        .data_layout={.storage=TA_DATA_BYTES,.bytes_layout={sizeof(unsigned),sizeof(unsigned)},.schema={"v1",2}},
        .states=(ta_chart_state_t[]){{.id="away",.states=(ta_chart_state_t[]){{.id="newchild"},{0}}},{0}}};
    size_t baseline;
    open_fixture(&f);assert(ta_definition_build(f.system,&chart,&result)==TA_OK);baseline=f.host.live_bytes;
    assert(ta_actor_upgrade(f.system,&(ta_upgrade_options_t){.actor=f.ref,.replacement=result.definition,
        .expected_revision=TA_REVISION_ANY})==TA_INVALID);
    assert(f.host.live_bytes==baseline && f.binds==0);
    ta_definition_release(result.definition);close_fixture(&f);
}

int main(void)
{
    assert(ta_actor_upgrade(NULL,NULL)==TA_INVALID);
    rollback_and_repetition();incompatible_active_topology();return 0;
}

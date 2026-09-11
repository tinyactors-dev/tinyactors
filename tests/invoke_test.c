#include "host.h"
#include "internal.h"
#include <assert.h>

typedef struct fixture {
    ta_test_allocator_t host;
    ta_arena_t *root;
    ta_system_t *system;
    ta_definition_t *definition;
    unsigned char content[2];
} fixture_t;

static ta_status_t bytes_eval(ta_context_t *context, ta_code_t code, ta_bytes_t *out)
{
    fixture_t *f=ta_context_host_user(context);
    (void)code;*out=(ta_bytes_t){f->content,2};return TA_OK;
}

static void setup(fixture_t *f)
{
    ta_chunk_allocator_t callbacks;
    ta_system_config_t config;
    *f=(fixture_t){0};f->content[0]='x';f->content[1]='y';
    ta_test_allocator_init(&f->host,0,0);callbacks=ta_test_allocator_callbacks(&f->host);
    assert(ta_arena_create(&callbacks,&(ta_arena_options_t){512,65536,TA_UNLIMITED,65536},&f->root)==TA_OK);
    ta_system_config_init(&config);config.session_prefix=ta_text_from_cstr("invoke");config.data_model=ta_text_from_cstr("bytes");
    config.host_user=f;config.data.evaluate_bytes=bytes_eval;
    config.ioprocessors=(ta_ioprocessor_desc_t[]){{.type="urn:events"},{0}};
    config.services=(ta_service_desc_t[]){{.type="urn:service",.event_processor="urn:events"},{0}};
    assert(ta_system_create(f->root,&config,&f->system)==TA_OK);
}

static ta_definition_t *build(fixture_t *f, const ta_chart_state_t *states)
{
    ta_definition_result_t result;
    ta_chart_t chart={.name="invocations",.data_model="bytes",.states=states,
        .data_layout={.storage=TA_DATA_BYTES,.bytes_layout={1,1}}};
    assert(ta_definition_build(f->system,&chart,&result)==TA_OK);return result.definition;
}

static ta_actor_ref_t spawn(fixture_t *f, ta_definition_t *inline_child)
{
    ta_invoke_desc_t invoke={.id={"svc",3}};
    ta_actor_ref_t actor;
    if(inline_child!=NULL)invoke.inline_definition=inline_child;
    else {
        invoke.type.literal=ta_text_from_cstr("urn:service");invoke.source.literal=ta_text_from_cstr("endpoint");
        invoke.body=(ta_body_expr_t){.kind=TA_BODY_CONTENT,.bytes_content=1};
    }
    f->definition=build(f,(ta_chart_state_t[]){
        {.id="work",.invokes={&invoke,1},.transitions=(ta_chart_transition_t[]){
            {.event="ping"},{.event="done.invoke.svc",.target="complete"},{0}}},
        {.id="complete"},{0}});
    assert(ta_actor_spawn(f->system,&(ta_spawn_options_t){.definition=f->definition},&actor)==TA_OK);
    return actor;
}

static void run(fixture_t *f, size_t units)
{
    ta_run_result_t result;
    assert(ta_system_run(f->system,&(ta_run_options_t){1,{units,1}},&result)==TA_OK);
}

static void teardown(fixture_t *f, ta_actor_ref_t actor)
{
    assert(ta_actor_stop(f->system,actor)==TA_OK);run(f,100);
    assert(ta_actor_release(f->system,actor)==TA_OK);ta_definition_release(f->definition);
    assert(ta_system_destroy(f->system)==TA_OK);ta_arena_close(f->root);
    assert(ta_test_allocator_releases_ok(&f->host));ta_test_allocator_reset(&f->host);
}

static void external_lifecycle(void)
{
    fixture_t f;
    ta_actor_ref_t actor;
    tai_actor_t *a;
    const ta_io_request_t *request;
    ta_invocation_ref_t invocation;
    unsigned char bytes[2]={'a','b'};
    ta_message_t message={.name={"ping",4},.data={.kind=TA_PAYLOAD_BYTES,.as.bytes={bytes,2}}};
    uint8_t cancelled;
    setup(&f);actor=spawn(&f,NULL);run(&f,20);a=tai_actor_find(f.system,actor);
    f.content[0]='z';
    assert(ta_io_next(f.system,&request)==TA_OK && request->kind==TA_IO_INVOKE_START);
    assert(request->body.kind==TA_BODY_CONTENT && request->body.content.kind==TA_PAYLOAD_BYTES);
    assert(((const char *)request->body.content.as.bytes.data)[0]=='x');
    invocation=request->invocation;
    assert(ta_io_complete(f.system,request->request,TA_OK)==TA_OK);
    assert(ta_invocation_emit(f.system,invocation,&message)==TA_OK);bytes[0]='z';run(&f,20);
    assert(a->current->event.data.kind==TA_PAYLOAD_BYTES && ((const char *)a->current->event.data.as.bytes.data)[0]=='a');
    assert(tai_text_compare(a->current->event.invokeid,ta_text_from_cstr("svc"))==0);
    assert(ta_invocation_done(f.system,invocation,&(ta_body_t){.kind=TA_BODY_CONTENT,.content=message.data})==TA_OK);
    run(&f,30);assert(tai_active(a,2));
    assert(tai_text_compare(a->current->event.name,ta_text_from_cstr("done.invoke.svc"))==0);
    assert(ta_invocation_is_cancelled(f.system,invocation,&cancelled)==TA_STALE);
    assert(f.system->stats.invocations==0 && f.system->stats.io_requests==0);
    teardown(&f,actor);
}

static void cancellation(uint8_t take)
{
    fixture_t f;
    ta_actor_ref_t actor;
    const ta_io_request_t *request;
    ta_request_ref_t start={0};
    ta_invocation_ref_t invocation;
    uint8_t cancelled;
    setup(&f);actor=spawn(&f,NULL);run(&f,20);
    invocation=f.system->invocations->ref;
    if(take) {assert(ta_io_next(f.system,&request)==TA_OK);start=request->request;}
    assert(ta_actor_stop(f.system,actor)==TA_OK);run(&f,20);
    if(take) {
        assert(ta_invocation_is_cancelled(f.system,invocation,&cancelled)==TA_OK && cancelled);
        assert(ta_io_next(f.system,&request)==TA_EMPTY);
        assert(ta_io_complete(f.system,start,TA_OK)==TA_OK);
        assert(ta_io_next(f.system,&request)==TA_OK && request->kind==TA_IO_INVOKE_CANCEL);
        assert(ta_io_complete(f.system,request->request,TA_OK)==TA_OK);
    } else assert(ta_io_next(f.system,&request)==TA_EMPTY);
    assert(f.system->stats.invocations==0 && f.system->stats.io_requests==0);
    teardown(&f,actor);
}

static void inline_child(void)
{
    fixture_t f;
    ta_definition_t *child;
    ta_actor_ref_t actor;
    tai_actor_t *a;
    setup(&f);
    child=build(&f,(ta_chart_state_t[]){{.id="done",.kind=TA_CHART_FINAL,
        .done_data={.kind=TA_BODY_CONTENT,.bytes_content=1}},{0}});
    actor=spawn(&f,child);ta_definition_release(child);run(&f,100);
    a=tai_actor_find(f.system,actor);
    assert(a!=NULL && tai_active(a,2));
    assert(a->current->event.data.kind==TA_PAYLOAD_BYTES && ((const char *)a->current->event.data.as.bytes.data)[0]=='x');
    assert(f.system->stats.actors==1 && f.system->stats.invocations==0);
    teardown(&f,actor);
}

int main(void)
{
    external_lifecycle();cancellation(0);cancellation(1);inline_child();return 0;
}

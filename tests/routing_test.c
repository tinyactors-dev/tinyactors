#include "host.h"
#include "internal.h"
#include <assert.h>

typedef struct fixture {
    ta_test_allocator_t host;
    ta_arena_t *root;
    ta_system_t *simulation,*renderer;
    ta_definition_t *old,*next,*render_definition;
    ta_actor_ref_t render_actor;
    size_t copied;
} fixture_t;

static ta_status_t action(ta_context_t *context, ta_action_kind_t kind, ta_code_t code)
{
    ta_mut_bytes_t bytes;
    ta_context_view_t view;
    ta_send_t send={.event={"tick",4},.target={.kind=TA_TARGET_NAME,.text={"player",6}},.delay_ns=10};
    (void)kind;
    assert(ta_context_bytes(context,&bytes)==TA_OK);
    if(code==3)return ta_context_send(context,&send,NULL);
    if(code==4) {
        ta_context_inspect(context,&view);
        assert(view.system->event->data.kind==TA_PAYLOAD_BYTES);
        tai_copy(bytes.data,view.system->event->data.as.bytes.data,sizeof(unsigned));
    } else *(unsigned *)bytes.data+=(unsigned)code;
    return TA_OK;
}

static ta_definition_t *definition(ta_system_t *system, unsigned code)
{
    ta_action_t update={.kind=TA_ACTION_CUSTOM,.as.custom={code}},delay={.kind=TA_ACTION_CUSTOM,.as.custom={3}};
    ta_chart_t chart={.name="entities",.data_model="bytes",
        .data_layout={.storage=TA_DATA_BYTES,.bytes_layout={sizeof(unsigned),sizeof(unsigned)}},
        .states=(ta_chart_state_t[]){{.id="idle",.transitions=(ta_chart_transition_t[]){
            {.event="tick",.content={&update,1}},{.event="schedule",.content={&delay,1}},{0}}},{0}}};
    ta_definition_result_t result;
    assert(ta_definition_build(system,&chart,&result)==TA_OK);return result.definition;
}

static void run(ta_system_t *system, uint64_t now)
{
    ta_run_result_t result;
    assert(ta_system_run(system,&(ta_run_options_t){now,{100,1}},&result)==TA_OK && !result.runnable);
}

static void setup(fixture_t *f)
{
    ta_chunk_allocator_t allocator;
    ta_system_config_t config;
    *f=(fixture_t){0};ta_test_allocator_init(&f->host,0,0);allocator=ta_test_allocator_callbacks(&f->host);
    assert(ta_arena_create(&allocator,&(ta_arena_options_t){256,65536,TA_UNLIMITED,65536},&f->root)==TA_OK);
    ta_system_config_init(&config);config.session_prefix=ta_text_from_cstr("simulation");
    config.data_model=ta_text_from_cstr("bytes");config.data.run=action;
    assert(ta_system_create(f->root,&config,&f->simulation)==TA_OK);
    config.session_prefix=ta_text_from_cstr("renderer");
    assert(ta_system_create(f->root,&config,&f->renderer)==TA_OK);
    f->old=definition(f->simulation,1);f->next=definition(f->simulation,2);f->render_definition=definition(f->renderer,4);
    assert(ta_actor_spawn(f->renderer,&(ta_spawn_options_t){.definition=f->render_definition},&f->render_actor)==TA_OK);
    run(f->renderer,0);
}

static ta_visit_t bridge(void *user, const ta_inspect_record_t *record)
{
    fixture_t *f=user;
    assert(record->kind==TA_INSPECT_ACTOR && record->as.info.state.storage==TA_DATA_BYTES);
    assert(ta_post(f->renderer,&(ta_post_options_t){.target={.kind=TA_TARGET_INSTANCE,.instance=f->render_actor},
        .message={.name={"tick",4},.data={.kind=TA_PAYLOAD_BYTES,.as.bytes=record->as.info.state.as.bytes}}},NULL)==TA_OK);
    ++f->copied;return TA_VISIT_CONTINUE;
}

static void stop(ta_system_t *system, ta_actor_ref_t actor)
{
    assert(ta_actor_stop(system,actor)==TA_OK);run(system,20);assert(ta_actor_release(system,actor)==TA_OK);
}

int main(void)
{
    fixture_t f;
    ta_definition_binding_t binding={{"game",4},0};
    ta_actor_ref_t before,after,found;
    ta_definition_t *looked;
    ta_actor_info_t info;
    ta_inspect_cursor_t cursor={{0}};
    ta_inspect_options_t inspect_options={.stable_only=1,.details=TA_INSPECT_DATA,.work_units=20,.sink={&f,bridge}};
    setup(&f);
    assert(ta_definition_publish(f.simulation,&binding,f.old)==TA_OK);
    assert(ta_actor_spawn(f.simulation,&(ta_spawn_options_t){.definition_key={"game",4},.name={"player",6}},&before)==TA_OK);
    run(f.simulation,0);
    assert(ta_post(f.simulation,&(ta_post_options_t){.target={.kind=TA_TARGET_INSTANCE,.instance=before},.message={.name={"schedule",8}}},NULL)==TA_OK);
    run(f.simulation,0);assert(ta_system_next_deadline(f.simulation)==10);
    assert(ta_definition_publish(f.simulation,&binding,f.next)==TA_CONFLICT);
    binding.expected_revision=ta_definition_revision(f.old);
    assert(ta_definition_publish(f.simulation,&binding,f.next)==TA_OK);
    assert(ta_definition_lookup(f.simulation,binding.key,&looked)==TA_OK && looked==f.next);ta_definition_release(looked);
    assert(ta_actor_spawn(f.simulation,&(ta_spawn_options_t){.definition_key={"game",4}},&after)==TA_OK);
    assert(ta_name_bind(f.simulation,&(ta_name_binding_t){{"player",6},before},after)==TA_OK);
    assert(ta_name_bind(f.simulation,&(ta_name_binding_t){{"player",6},before},before)==TA_CONFLICT);
    run(f.simulation,10); /* Delayed NAME resolves the replacement, not original incarnation. */
    assert(ta_actor_inspect(f.simulation,after,&info)==TA_OK && *(const unsigned *)info.state.as.bytes.data==2);
    assert(ta_actor_inspect(f.simulation,before,&info)==TA_OK && *(const unsigned *)info.state.as.bytes.data==0);
    assert(info.revision==ta_definition_revision(f.old));
    assert(ta_session_lookup(f.simulation,info.sessionid,&found)==TA_OK && found.id==before.id);
    stop(f.simulation,before);
    assert(ta_name_lookup(f.simulation,ta_text_from_cstr("player"),&found)==TA_OK && found.id==after.id);
    assert(ta_post(f.simulation,&(ta_post_options_t){.target={.kind=TA_TARGET_INSTANCE,.instance=before},.message={.name={"tick",4}}},NULL)==TA_STALE);
    assert(ta_system_inspect(f.simulation,&inspect_options,&cursor)==TA_OK && f.copied==1);
    run(f.renderer,10);
    assert(ta_actor_inspect(f.renderer,f.render_actor,&info)==TA_OK && *(const unsigned *)info.state.as.bytes.data==2);
    stop(f.simulation,after);stop(f.renderer,f.render_actor);
    binding.expected_revision=ta_definition_revision(f.next);assert(ta_definition_unpublish(f.simulation,&binding)==TA_OK);
    ta_definition_release(f.old);ta_definition_release(f.next);ta_definition_release(f.render_definition);
    assert(ta_system_destroy(f.simulation)==TA_OK && ta_system_destroy(f.renderer)==TA_OK);
    ta_arena_close(f.root);assert(ta_test_allocator_releases_ok(&f.host));ta_test_allocator_reset(&f.host);return 0;
}

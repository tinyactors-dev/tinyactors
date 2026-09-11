#include "../src/tinyactors.h"
#include "host.h"
#include <assert.h>

typedef struct native_fixture { ta_test_allocator_t host; ta_arena_t *root; ta_system_t *system; ta_definition_t *old_def; ta_definition_t *new_def; unsigned next; unsigned releases[32]; unsigned release_count; unsigned fail_bind; } native_fixture_t;

static void native_release(void *u, ta_value_t v)
{ native_fixture_t *f=(native_fixture_t *)u; assert(f->release_count<32); f->releases[f->release_count++]=(unsigned)v.handle; }
static void native_retain(void *u, ta_value_t v) { (void)u; (void)v; }
static ta_status_t native_snapshot(void *u, ta_value_t v, ta_value_t *out) { (void)u; *out=v; return TA_OK; }
static ta_status_t native_create(ta_context_t *c, ta_value_t seed, ta_value_t *out) { native_fixture_t *f=(native_fixture_t *)ta_context_host_user(c); (void)seed; *out=(ta_value_t){++f->next}; return TA_OK; }
static ta_status_t native_bind_system(ta_context_t *c, const ta_system_values_t *v)
{ native_fixture_t *f=(native_fixture_t *)ta_context_host_user(c); ta_context_view_t view; (void)v; ta_context_inspect(c,&view); return f->fail_bind && view.phase==TA_PHASE_MIGRATE ? TA_EXECUTION_ERROR : TA_OK; }
static ta_status_t native_bind_event(ta_context_t *c, const ta_event_t *e) { (void)c; (void)e; return TA_OK; }
static ta_status_t native_migrate(void *u, const ta_migration_t *m, ta_value_t *out)
{ native_fixture_t *f=(native_fixture_t *)u; (void)m; *out=(ta_value_t){++f->next}; return TA_OK; }

static void native_open(native_fixture_t *f)
{
    ta_chunk_allocator_t cb; ta_system_config_t cfg; ta_definition_result_t r; ta_chart_state_t state[]={{.id="idle"},{0}};
    ta_chart_t chart={.name="native",.data_model="native",.data_layout={.storage=TA_DATA_NATIVE},.states=state,.initial={.target="idle"}};
    *f=(native_fixture_t){0}; ta_test_allocator_init(&f->host,0,0); cb=ta_test_allocator_callbacks(&f->host); assert(ta_arena_create(&cb,&(ta_arena_options_t){256,65536,TA_UNLIMITED,65536},&f->root)==TA_OK);
    ta_system_config_init(&cfg); cfg.session_prefix=ta_text_from_cstr("native"); cfg.data_model=ta_text_from_cstr("native"); cfg.host_user=f;
    cfg.values=(ta_value_ops_t){native_retain,native_release,native_snapshot}; cfg.data.create=native_create; cfg.data.bind_system=native_bind_system; cfg.data.bind_event=native_bind_event;
    assert(ta_system_create(f->root,&cfg,&f->system)==TA_OK); assert(ta_definition_build(f->system,&chart,&r)==TA_OK); f->old_def=r.definition; assert(ta_definition_build(f->system,&chart,&r)==TA_OK); f->new_def=r.definition;
}
static ta_actor_ref_t native_spawn(native_fixture_t *f)
{ ta_actor_ref_t a; assert(ta_actor_spawn(f->system,&(ta_spawn_options_t){.definition=f->old_def},&a)==TA_OK); { ta_run_result_t r; assert(ta_system_run(f->system,&(ta_run_options_t){1,{20,1}},&r)==TA_OK); } return a; }
static void native_close(native_fixture_t *f, ta_actor_ref_t a)
{ assert(ta_actor_stop(f->system,a)==TA_OK); { ta_run_result_t r; assert(ta_system_run(f->system,&(ta_run_options_t){1,{20,1}},&r)==TA_OK); } assert(ta_actor_release(f->system,a)==TA_OK); ta_definition_release(f->old_def); ta_definition_release(f->new_def); assert(ta_system_destroy(f->system)==TA_OK); ta_arena_close(f->root); assert(ta_test_allocator_releases_ok(&f->host)); ta_test_allocator_reset(&f->host); }

static void commit_ownership(void)
{
    native_fixture_t f; ta_actor_ref_t a; ta_actor_info_t info; native_open(&f); a=native_spawn(&f); assert(ta_actor_upgrade(f.system,&(ta_upgrade_options_t){a,f.new_def,TA_REVISION_ANY,&f,native_migrate})==TA_OK);
    assert(ta_actor_inspect(f.system,a,&info)==TA_OK); assert(info.state.storage==TA_DATA_NATIVE && info.state.as.native.handle==2u); assert(f.release_count==1u&&f.releases[0]==1u); native_close(&f,a); assert(f.release_count==2u&&f.releases[1]==2u);
}
static void rollback_ownership(void)
{
    native_fixture_t f; ta_actor_ref_t a; ta_actor_info_t info; native_open(&f); f.fail_bind=1; a=native_spawn(&f);
    assert(ta_actor_upgrade(f.system,&(ta_upgrade_options_t){a,f.new_def,TA_REVISION_ANY,&f,native_migrate})==TA_EXECUTION_ERROR); assert(ta_actor_inspect(f.system,a,&info)==TA_OK); assert(info.state.as.native.handle==1u); assert(f.release_count==1u&&f.releases[0]==2u); native_close(&f,a); assert(f.release_count==2u&&f.releases[1]==1u);
}
int main(void) { commit_ownership(); rollback_ownership(); return 0; }

#include "../src/internal.h"
#include "host.h"

static void cleanup(void *p) { ++*(unsigned *)p; }

typedef struct context_fixture { ta_test_allocator_t pool; ta_arena_t *root; ta_system_t *sys; ta_definition_t *def; } context_fixture_t;
static int setup(context_fixture_t *f, ta_state_desc_t *states, uint32_t count)
{
    ta_test_allocator_t *pool=&f->pool; ta_arena_t **root=&f->root; ta_system_t **sys=&f->sys; ta_definition_t **def=&f->def;
    ta_chunk_allocator_t cb; ta_system_config_t cfg; ta_definition_desc_t d = {0};
    ta_definition_input_t in={0}; ta_definition_result_t r;
    ta_test_allocator_init(pool, 0u, 0u);
    cb = ta_test_allocator_callbacks(pool);
    if (ta_arena_create(&cb, &(ta_arena_options_t){256u,8192u,TA_UNLIMITED,8192u}, root) != TA_OK) return 0;
    ta_system_config_init(&cfg); cfg.session_prefix=(ta_text_t){"ctx",3u}; cfg.data_model=(ta_text_t){"bytes",5u};
    if (ta_system_create(*root, &cfg, sys) != TA_OK) return 0;
    d.name=(ta_text_t){"d", 1u}; d.data_model=(ta_text_t){"bytes",5u}; d.data_layout.storage=TA_DATA_BYTES;
    d.data_layout.bytes_layout=(ta_layout_t){4u,1u}; d.states=states; d.state_count=count;
    in.description=&d; in.options.storage=TA_DEFINITION_BORROW;
    if (ta_definition_prepare(*sys,&in,&r) != TA_OK) return 0;
    *def=r.definition; return 1;
}

static int one_chart(void)
{
    context_fixture_t f={0}; ta_system_t *sys;
    ta_state_desc_t s[3] = {0}; ta_spawn_options_t so={0}; ta_actor_ref_t ref; tai_actor_t *a;
    ta_context_t c; ta_mut_bytes_t bytes; ta_arena_t *ma,*aa,*child; unsigned cleaned=0;
    s[0].name=(ta_text_t){"r",1u}; s[0].parent=TA_INDEX_NONE; s[0].kind=TA_STATE_ROOT;
    s[1].name=(ta_text_t){"p",1u}; s[1].parent=0u; s[1].kind=TA_STATE_COMPOUND;
    s[2].name=(ta_text_t){"x",1u}; s[2].parent=1u; s[2].kind=TA_STATE_ATOMIC;
    if (!setup(&f,s,3u)) return 1; sys=f.sys;
    so.definition=f.def; if (ta_actor_spawn(sys,&so,&ref)!=TA_OK) return 2; a=tai_actor_find(sys,ref);
    a->leaf=TA_INDEX_NONE; if (tai_set_active(a,0u,1u)!=TA_OK || tai_set_active(a,1u,1u)!=TA_OK || tai_set_active(a,2u,1u)!=TA_OK) return 3;
    tai_context(a,TA_PHASE_ONENTRY,&c); c.view.state_id=2u;
    if(tai_text_compare(c.values.sessionid,ta_text_from_cstr("ctx.1"))!=0 || c.values.ioprocessor_count!=1) return 12;
    if(tai_text_compare(c.values.ioprocessors[0].location,ta_text_from_cstr("#_scxml_ctx.1"))!=0) return 13;
    if (!ta_context_in(&c,(ta_text_t){"p",1u}) || !ta_context_in(&c,(ta_text_t){"x",1u})) return 4;
    if (ta_context_bytes(&c,&bytes)!=TA_OK || bytes.size!=4u) return 5;
    c.view.phase=TA_PHASE_GUARD; if (ta_context_bytes(&c,&bytes)!=TA_INVALID) return 6;
    c.view.phase=TA_PHASE_ONENTRY; if (ta_context_arena(&c,TA_LIFETIME_MACROSTEP,&ma)!=TA_OK) return 7;
    if(ta_context_arena(&c,TA_LIFETIME_MACROSTEP,&child)!=TA_OK || child!=ma) return 14;
    if(ta_context_arena(&c,TA_LIFETIME_ACTOR,&child)!=TA_OK || child!=a->arena) return 15;
    if (ta_context_arena(&c,TA_LIFETIME_ACTIVATION,&aa)!=TA_OK) return 8;
    if (ta_arena_defer(aa,cleanup,&cleaned)!=TA_OK) return 9;
    c.view.state_id=1u; if (ta_context_arena(&c,TA_LIFETIME_ACTIVATION,&child)!=TA_OK || child==aa) return 10;
    tai_activation_close(a,1u); if (cleaned!=1u) return 11;
    a->status=TA_ACTOR_STOPPED; a->leaf=TA_INDEX_NONE;
    if(ta_actor_release(sys,ref)!=TA_OK) return 16;
    ta_definition_release(f.def);
    if(ta_system_destroy(sys)!=TA_OK) return 17;
    ta_arena_close(f.root); if(!ta_test_allocator_releases_ok(&f.pool))return 18; ta_test_allocator_reset(&f.pool); return 0;
}

int main(void) { return one_chart(); }
